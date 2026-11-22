/*
 * Lecteur vidéo Reddit.
 *
 *   v.redd.it/<id>/DASHPlaylist.mpd  -> choix de la qualité (≤ max_p) et de la piste son
 *   2 téléchargements en flux (vidéo, son) -> tampons circulaires -> FFmpeg (démuxeur MP4)
 *   thread vidéo  : H.264 -> YUV -> RGB565 (texture tuilée) -> file de 3 images
 *   thread son    : AAC -> PCM 16 bits stéréo -> tampons NDSP
 *   thread principal : affiche l'image dont l'horodatage correspond à l'horloge du son
 */
#include "video.h"
#include "net.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>

#define VQ          3              /* images décodées d'avance */
#define NBUF        8              /* tampons son NDSP */
#define BUF_FRAMES  2048           /* échantillons stéréo par tampon (~43 ms à 48 kHz) */
#define CH          0              /* canal NDSP */
#define VBUF_CAP    (1536 * 1024)  /* tampon de téléchargement vidéo */
#define ABUF_CAP    (512 * 1024)   /* tampon de téléchargement son */
#define MS          1000000LL      /* nanosecondes par milliseconde */

/* ------------------------------------------------------------------ */
/* Journal : sdmc:/3ds/reddit3ds/video.log (la dernière ligne indique où ça a planté) */

#define VLOG_PATH "sdmc:/3ds/reddit3ds/video.log"

static LightLock s_log_lock;
static bool s_log_ready;
static u64 s_t0;
static LightLock s_ff_lock; /* FFmpeg est compilé sans pthreads : on sérialise ses initialisations */

static void vlog(const char *fmt, ...)
{
    if (!s_log_ready) return;
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    LightLock_Lock(&s_log_lock);
    FILE *f = fopen(VLOG_PATH, "a");
    if (f) {
        fprintf(f, "[%6lu ms] %s\n", (unsigned long)(osGetTime() - s_t0), line);
        fclose(f);
    }
    LightLock_Unlock(&s_log_lock);
}

static void vlog_start(void)
{
    LightLock_Init(&s_log_lock);
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/reddit3ds", 0777);
    s_t0 = osGetTime();
    FILE *f = fopen(VLOG_PATH, "w");
    s_log_ready = f != NULL;
    if (f) fclose(f);
}

/* Crée un thread : cœur demandé, puis cœur par défaut, puis priorité un cran plus basse */
static Thread make_thread(ThreadFunc fn, void *arg, size_t stack, s32 prio, int core)
{
    Thread t = NULL;
    if (core >= 0) t = threadCreate(fn, arg, stack, prio, core, false);
    if (!t) t = threadCreate(fn, arg, stack, prio, -2, false);
    if (!t) t = threadCreate(fn, arg, stack, prio + 1, -2, false);
    vlog("thread (pile %luK, prio %d, coeur voulu %d) : %s", (unsigned long)(stack / 1024), (int)prio, core,
         t ? "ok" : "ECHEC");
    return t;
}

/* ------------------------------------------------------------------ */
/* Tampon de téléchargement en flux                                   */

typedef struct {
    char url[300];
    NetConn *conn;
    Thread thread;
    LightLock lock;
    CondVar cv;
    u8 *buf;
    size_t cap, head, size;
    long long got;
    FILE *wf, *rf;       /* préchargement : écriture puis relecture d'un fichier temporaire */
    char path[96];
    volatile bool eof, failed;
} Source;

typedef struct {
    AVFormatContext *fmt;
    AVIOContext *avio;
    AVCodecContext *dec;
    AVPacket *pkt;
    AVFrame *frm;
    int idx;
    double tb;
} Track;

static volatile bool s_stop;
static volatile VideoStatus s_status = VS_IDLE;
static char s_error[160];
static char s_id[32];
static int  s_maxp;
static int  s_quality;
static double s_duration;

static Source s_vsrc, s_asrc;
static Thread s_vthread, s_athread;
static NetConn *s_mconn;
static bool s_new3ds;
static bool s_preload;

/* file d'images */
static u16 *s_vq[VQ];
static double s_vq_pts[VQ];
static int s_vq_r, s_vq_n;
static LightLock s_vq_lock;
static CondVar s_vq_cv;
static int s_dw, s_dh, s_tw, s_th;
static volatile bool s_dims_ready, s_vdone;
static volatile bool s_logged_frame;
static u64 s_open_ms;

/* texture */
static C3D_Tex s_tex;
static Tex3DS_SubTexture s_sub;
static C2D_Image s_img;
static bool s_tex_ok, s_has_frame, s_tex_free_pending;

/* son */
static bool s_ndsp_tried, s_ndsp_ok;
static volatile int s_audio_state; /* 0 en attente, 1 actif, 2 aucun son */
static volatile bool s_adone;      /* décodage du son terminé */
static volatile int s_aqueued;
static ndspWaveBuf s_wb[NBUF];
static s16 *s_wdata[NBUF];
static double s_wpts[NBUF];
static int s_rate = 48000;
static float s_volume = 0.8f;

/* horloge */
static volatile float s_clock_pub; /* lu par le thread vidéo (32 bits : lecture atomique) */
static double s_clock_last;
static u64 s_wall_start, s_pause_start, s_pause_acc;

/* ------------------------------------------------------------------ */
/* Source : téléchargement -> tampon circulaire -> lecture par FFmpeg */

static size_t src_data(const void *data, size_t len, void *ud)
{
    Source *s = ud;
    const u8 *p = data;
    size_t left = len;
    if (s->wf) { /* préchargement : tout va sur la carte SD */
        if (s_stop) return 0;
        if (fwrite(data, 1, len, s->wf) != len) { s->failed = true; return 0; }
        LightLock_Lock(&s->lock);
        s->got += (long long)len;
        LightLock_Unlock(&s->lock);
        return len;
    }
    LightLock_Lock(&s->lock);
    while (left > 0) {
        while (s->size == s->cap && !s_stop) CondVar_WaitTimeout(&s->cv, &s->lock, 50 * MS);
        if (s_stop) { LightLock_Unlock(&s->lock); return 0; }
        size_t tail = (s->head + s->size) % s->cap;
        size_t chunk = s->cap - s->size;
        if (chunk > left) chunk = left;
        if (chunk > s->cap - tail) chunk = s->cap - tail;
        memcpy(s->buf + tail, p, chunk);
        s->size += chunk;
        s->got += chunk;
        p += chunk;
        left -= chunk;
        CondVar_Broadcast(&s->cv);
    }
    LightLock_Unlock(&s->lock);
    return len;
}

static void src_thread(void *arg)
{
    Source *s = arg;
    char err[128];
    for (int tries = 0; !s_stop; tries++) {
        long code = 0;
        int r = http_stream(s->conn, s->url, s->got, src_data, s, &code, err, sizeof(err));
        if (s_stop || s->failed) break;
        if (r == 0) {
            if (code != 200 && code != 206) s->failed = true;
            break; /* terminé */
        }
        if (tries >= 4) { s->failed = true; break; }
        svcSleepThread(600 * MS); /* coupure : on reprend là où on en était */
    }
    if (s->wf) { fclose(s->wf); s->wf = NULL; }
    LightLock_Lock(&s->lock);
    s->eof = true;
    CondVar_Broadcast(&s->cv);
    LightLock_Unlock(&s->lock);
}

static int src_read(void *opaque, uint8_t *out, int want)
{
    Source *s = opaque;
    if (s->rf) { /* fichier préchargé */
        if (s_stop) return AVERROR_EOF;
        size_t got = fread(out, 1, (size_t)want, s->rf);
        return got > 0 ? (int)got : AVERROR_EOF;
    }
    LightLock_Lock(&s->lock);
    while (s->size == 0 && !s->eof && !s_stop) CondVar_WaitTimeout(&s->cv, &s->lock, 50 * MS);
    if (s->size == 0 || s_stop) {
        bool failed = s->failed;
        LightLock_Unlock(&s->lock);
        return failed ? AVERROR(EIO) : AVERROR_EOF;
    }
    size_t n = (size_t)want < s->size ? (size_t)want : s->size;
    size_t first = s->cap - s->head;
    if (first > n) first = n;
    memcpy(out, s->buf + s->head, first);
    if (n > first) memcpy(out + first, s->buf, n - first);
    s->head = (s->head + n) % s->cap;
    s->size -= n;
    CondVar_Broadcast(&s->cv);
    LightLock_Unlock(&s->lock);
    return (int)n;
}

/* file != NULL : télécharge tout dans ce fichier (préchargement) au lieu du tampon circulaire */
/* adopt : connexion déjà ouverte (TLS établi) à reprendre au lieu d'en créer une nouvelle */
static bool src_start(Source *s, const char *url, size_t cap, int core, const char *file, NetConn *adopt)
{
    snprintf(s->url, sizeof(s->url), "%s", url);
    LightLock_Init(&s->lock);
    CondVar_Init(&s->cv);
    s->cap = cap;
    if (file) {
        snprintf(s->path, sizeof(s->path), "%s", file);
        s->wf = fopen(file, "wb");
    } else {
        s->buf = malloc(cap);
    }
    s->conn = adopt ? adopt : net_conn_new(&s_stop);
    if ((!file && !s->buf) || (file && !s->wf) || !s->conn) return false;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    s->thread = make_thread(src_thread, s, 128 * 1024, prio + 1, core); /* TLS : pile confortable */
    return s->thread != NULL;
}

static void src_stop(Source *s)
{
    if (s->thread) {
        threadJoin(s->thread, U64_MAX);
        threadFree(s->thread);
    }
    net_conn_free(s->conn);
    free(s->buf);
    if (s->wf) fclose(s->wf);
    if (s->rf) fclose(s->rf);
    if (s->path[0]) remove(s->path); /* fichier temporaire du préchargement */
    memset(s, 0, sizeof(*s));
}

static bool src_open_read(Source *s)
{
    s->rf = fopen(s->path, "rb");
    return s->rf != NULL;
}

/* ------------------------------------------------------------------ */
/* Playlist DASH : choix des flux                                     */

static const char *find_in(const char *s, const char *e, const char *needle)
{
    size_t n = strlen(needle);
    for (const char *p = s; p && p + n <= e; p++) {
        p = memchr(p, needle[0], (size_t)(e - p));
        if (!p || p + n > e) return NULL;
        if (!memcmp(p, needle, n)) return p;
    }
    return NULL;
}

static long attr_int(const char *s, const char *e, const char *name)
{
    char key[32];
    snprintf(key, sizeof(key), " %s=\"", name);
    const char *p = find_in(s, e, key);
    return p ? strtol(p + strlen(key), NULL, 10) : 0;
}

static void base_url(const char *s, const char *e, const char *id, char *out, size_t n)
{
    out[0] = '\0';
    const char *b = find_in(s, e, "<BaseURL>");
    const char *be = b ? find_in(b, e, "</BaseURL>") : NULL;
    if (!b || !be) return;
    b += 9;
    char file[200];
    size_t l = (size_t)(be - b) < sizeof(file) - 1 ? (size_t)(be - b) : sizeof(file) - 1;
    memcpy(file, b, l);
    file[l] = '\0';
    if (!strncmp(file, "http", 4)) snprintf(out, n, "%s", file);
    else snprintf(out, n, "https://v.redd.it/%s/%s", id, file);
}

static double parse_iso_duration(const char *p) /* "PT1H2M3.5S" */
{
    if (!p || strncmp(p, "PT", 2)) return 0;
    p += 2;
    double total = 0;
    while (*p && *p != '"') {
        char *end;
        double v = strtod(p, &end);
        if (end == p) break;
        if (*end == 'H') total += v * 3600;
        else if (*end == 'M') total += v * 60;
        else if (*end == 'S') total += v;
        else break;
        p = end + 1;
    }
    return total;
}

static bool choose_streams(char *vurl, char *aurl, size_t n)
{
    char url[160], err[128];
    snprintf(url, sizeof(url), "https://v.redd.it/%s/DASHPlaylist.mpd", s_id);
    Buf b = {0};
    long code = 0;
    if (http_request(s_mconn, url, NULL, NULL, NULL, "*/*", 512 * 1024, &b, &code, err, sizeof(err)) || code != 200 || !b.data) {
        snprintf(s_error, sizeof(s_error), "Playlist vidéo : %s", code ? "HTTP" : err);
        if (code) snprintf(s_error + strlen(s_error), sizeof(s_error) - strlen(s_error), " %ld", code);
        buf_free(&b);
        return false;
    }
    const char *mpd = b.data, *end = b.data + b.len;
    const char *d = find_in(mpd, end, "mediaPresentationDuration=\"");
    s_duration = d ? parse_iso_duration(d + 27) : 0;

    int best_v = -1, best_small = 1 << 30;
    long best_a = -1;
    char small_url[300] = "";
    vurl[0] = aurl[0] = '\0';

    for (const char *as = find_in(mpd, end, "<AdaptationSet"); as;) {
        const char *ae = find_in(as, end, "</AdaptationSet>");
        if (!ae) ae = end;
        const char *as_tag_end = memchr(as, '>', (size_t)(ae - as));
        bool audio = (as_tag_end && find_in(as, as_tag_end, "contentType=\"audio\"")) ||
                     find_in(as, ae, "mimeType=\"audio/");
        for (const char *r = find_in(as, ae, "<Representation"); r;) {
            const char *re = find_in(r + 1, ae, "</Representation>");
            if (!re) re = ae;
            const char *tag_end = memchr(r, '>', (size_t)(re - r));
            if (!tag_end) break;
            char u[300];
            base_url(r, re, s_id, u, sizeof(u));
            if (u[0]) {
                if (audio) {
                    /* son : le meilleur jusqu'à ~128 kb/s */
                    long bw = attr_int(r, tag_end, "bandwidth");
                    long score = bw <= 140000 ? bw : 140000 - bw / 1000;
                    if (score > best_a) { best_a = score; snprintf(aurl, n, "%s", u); }
                } else {
                    int w = (int)attr_int(r, tag_end, "width"), h = (int)attr_int(r, tag_end, "height");
                    int m = w < h ? w : h;
                    if (m <= s_maxp && m > best_v) { best_v = m; snprintf(vurl, n, "%s", u); }
                    if (m < best_small) { best_small = m; snprintf(small_url, sizeof(small_url), "%s", u); }
                }
            }
            r = find_in(re, ae, "<Representation");
        }
        as = find_in(ae, end, "<AdaptationSet");
    }
    buf_free(&b);
    if (!vurl[0] && small_url[0]) { /* rien sous la limite : la plus petite qualité */
        snprintf(vurl, n, "%s", small_url);
        best_v = best_small;
    }
    if (!vurl[0]) {
        snprintf(s_error, sizeof(s_error), "Aucun flux vidéo trouvé");
        return false;
    }
    s_quality = best_v;
    return true;
}

/* ------------------------------------------------------------------ */
/* FFmpeg                                                             */

static void track_close(Track *t)
{
    av_frame_free(&t->frm);
    av_packet_free(&t->pkt);
    avcodec_free_context(&t->dec);
    if (t->fmt) avformat_close_input(&t->fmt);
    if (t->avio) {
        av_freep(&t->avio->buffer);
        avio_context_free(&t->avio);
    }
}

static bool track_open(Track *t, Source *src, enum AVMediaType type)
{
    memset(t, 0, sizeof(*t));
    unsigned char *iobuf = av_malloc(32 * 1024);
    if (!iobuf) return false;
    t->avio = avio_alloc_context(iobuf, 32 * 1024, 0, src, src_read, NULL, NULL);
    if (!t->avio) { av_free(iobuf); return false; }
    t->avio->seekable = 0;

    t->fmt = avformat_alloc_context();
    if (!t->fmt) goto fail;
    t->fmt->pb = t->avio;
    t->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    const AVInputFormat *ifmt = av_find_input_format("mov");
    if (!ifmt) vlog("demuxer mov absent de FFmpeg");
    int rc = avformat_open_input(&t->fmt, NULL, ifmt, NULL);
    if (rc < 0) {
        vlog("avformat_open_input (%s) : erreur %d", type == AVMEDIA_TYPE_VIDEO ? "video" : "son", rc);
        t->fmt = NULL; /* libéré par FFmpeg en cas d'échec */
        goto fail;
    }
    t->idx = av_find_best_stream(t->fmt, type, -1, -1, NULL, 0);
    if (t->idx < 0) { vlog("aucun flux %s dans le MP4 (%d)", type == AVMEDIA_TYPE_VIDEO ? "video" : "son", t->idx); goto fail; }
    AVStream *st = t->fmt->streams[t->idx];
    t->tb = av_q2d(st->time_base);

    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) { vlog("decodeur absent (codec_id %d)", (int)st->codecpar->codec_id); goto fail; }
    t->dec = avcodec_alloc_context3(codec);
    if (!t->dec || avcodec_parameters_to_context(t->dec, st->codecpar) < 0) goto fail;
    t->dec->thread_count = 1;
    if (type == AVMEDIA_TYPE_VIDEO && !s_new3ds) {
        /* Old 3DS : on sacrifie le filtre anti-blocs (≈ 30 % du temps de décodage) */
        t->dec->skip_loop_filter = AVDISCARD_ALL;
        t->dec->flags2 |= AV_CODEC_FLAG2_FAST;
    }
    rc = avcodec_open2(t->dec, codec, NULL);
    if (rc < 0) { vlog("avcodec_open2 (%s) : erreur %d", codec->name, rc); goto fail; }
    vlog("piste %s ouverte : %s", type == AVMEDIA_TYPE_VIDEO ? "video" : "son", codec->name);
    t->pkt = av_packet_alloc();
    t->frm = av_frame_alloc();
    if (!t->pkt || !t->frm) goto fail;
    return true;
fail:
    track_close(t);
    return false;
}

/* Un seul thread à la fois dans les initialisations FFmpeg (pas de pthreads dans notre build) */
static bool track_open_safe(Track *t, Source *src, enum AVMediaType type)
{
    LightLock_Lock(&s_ff_lock);
    bool ok = track_open(t, src, type);
    LightLock_Unlock(&s_ff_lock);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Vidéo                                                              */

static inline u32 tile_index(int x, int y, int tw)
{
    return ((((y >> 3) * (tw >> 3) + (x >> 3)) << 6) +
            ((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3)));
}

static inline int clamp255(int v)
{
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

/* YUV 4:2:0 -> RGB565 tuilé, redimensionné (plus proche voisin) à dw x dh */
static void convert_frame(const AVFrame *f, u16 *dst)
{
    static int xmap[512];
    int sw = f->width, sh = f->height;
    for (int x = 0; x < s_dw; x++) xmap[x] = x * sw / s_dw;
    for (int y = 0; y < s_dh; y++) {
        int sy = y * sh / s_dh;
        const u8 *Y = f->data[0] + sy * f->linesize[0];
        const u8 *U = f->data[1] + (sy >> 1) * f->linesize[1];
        const u8 *V = f->data[2] + (sy >> 1) * f->linesize[2];
        for (int x = 0; x < s_dw; x++) {
            int sx = xmap[x];
            int yy = (Y[sx] - 16) * 298;
            int u = U[sx >> 1] - 128, v = V[sx >> 1] - 128;
            int r = clamp255((yy + 409 * v + 128) >> 8);
            int g = clamp255((yy - 100 * u - 208 * v + 128) >> 8);
            int b = clamp255((yy + 516 * u + 128) >> 8);
            dst[tile_index(x, y, s_tw)] = (u16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
    }
}

static int next_pow2(int v)
{
    int p = 8;
    while (p < v) p <<= 1;
    return p;
}

static bool setup_dims(int w, int h)
{
    if (w <= 0 || h <= 0) return false;
    float k = 400.0f / w;
    if (h * k > 240.0f) k = 240.0f / h;
    if (k > 1.0f) k = 1.0f; /* le GPU agrandit lui-même les petites vidéos */
    s_dw = (int)(w * k); if (s_dw < 2) s_dw = 2;
    s_dh = (int)(h * k); if (s_dh < 2) s_dh = 2;
    s_tw = next_pow2(s_dw);
    s_th = next_pow2(s_dh);
    for (int i = 0; i < VQ; i++) {
        s_vq[i] = calloc((size_t)s_tw * s_th, sizeof(u16));
        if (!s_vq[i]) return false;
    }
    __sync_synchronize();
    s_dims_ready = true;
    return true;
}

/* Met une image décodée dans la file (attend une place libre) */
static void push_frame(Track *t)
{
    AVFrame *f = t->frm;
    int64_t ts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
    double pts = ts != AV_NOPTS_VALUE ? ts * t->tb : 0;

    /* En retard sur le son : on jette l'image et on allège le décodage */
    if (s_status == VS_PLAYING && pts < s_clock_pub - 0.12) {
        t->dec->skip_frame = AVDISCARD_NONREF;
        return;
    }
    t->dec->skip_frame = AVDISCARD_DEFAULT;
    if ((f->format != AV_PIX_FMT_YUV420P && f->format != AV_PIX_FMT_YUVJ420P) ||
        !f->data[0] || !f->data[1] || !f->data[2] || f->width <= 0 || f->height <= 0) {
        if (s_status != VS_ERROR) {
            snprintf(s_error, sizeof(s_error), "Format vidéo non géré (pix_fmt %d)", f->format);
            vlog("%s", s_error);
            s_status = VS_ERROR;
        }
        return;
    }

    LightLock_Lock(&s_vq_lock);
    while (s_vq_n == VQ && !s_stop) CondVar_WaitTimeout(&s_vq_cv, &s_vq_lock, 20 * MS);
    if (s_stop) { LightLock_Unlock(&s_vq_lock); return; }
    int slot = (s_vq_r + s_vq_n) % VQ;
    LightLock_Unlock(&s_vq_lock);

    convert_frame(f, s_vq[slot]); /* emplacement libre : personne d'autre n'y touche */
    if (!s_logged_frame) {
        s_logged_frame = true;
        vlog("premiere image decodee %dx%d -> %dx%d", f->width, f->height, s_dw, s_dh);
    }

    LightLock_Lock(&s_vq_lock);
    s_vq_pts[slot] = pts;
    s_vq_n++;
    LightLock_Unlock(&s_vq_lock);
}

static void audio_thread(void *arg);

static void video_thread(void *arg)
{
    (void)arg;
    char vurl[300], aurl[300];
    Track t;
    memset(&t, 0, sizeof(t));
    const bool pre = s_preload;
    bool a_started = false;

    vlog("video_thread demarre (%s)", pre ? "prechargement" : "direct");
    if (!choose_streams(vurl, aurl, sizeof(vurl))) goto fail;
    vlog("playlist ok : qualite %dp, son %s, duree %.1f s", s_quality, aurl[0] ? "oui" : "non", s_duration);

    /* La connexion qui a servi à lire la playlist (même hôte v.redd.it) sert au flux vidéo :
     * pas de nouvelle poignée de main TLS ni de nouveau chargement des certificats. */
    NetConn *mc = s_mconn;
    s_mconn = NULL;
    bool vok = src_start(&s_vsrc, vurl, VBUF_CAP, -2, pre ? "sdmc:/3ds/reddit3ds/pre_v.mp4" : NULL, mc);
    if (!vok) {
        snprintf(s_error, sizeof(s_error), pre ? "Carte SD inaccessible (fichier temporaire)" : "Mémoire insuffisante");
        goto fail;
    }
    if (aurl[0] && s_ndsp_ok)
        a_started = src_start(&s_asrc, aurl, ABUF_CAP, -2, pre ? "sdmc:/3ds/reddit3ds/pre_a.mp4" : NULL, NULL);

    if (pre) {
        /* Préchargement : on attend la fin des téléchargements avant de lire quoi que ce soit */
        vlog("prechargement en cours...");
        u64 last_change = osGetTime();
        long long last_got = -1;
        while (!s_stop) {
            if (s_vsrc.eof && (!a_started || s_asrc.eof)) break;
            long long g = s_vsrc.got + (a_started ? s_asrc.got : 0);
            if (g != last_got) {
                last_got = g;
                last_change = osGetTime();
            } else if (osGetTime() - last_change > 30000) {
                snprintf(s_error, sizeof(s_error), "Téléchargement bloqué (réseau)");
                goto fail;
            }
            svcSleepThread(50 * MS);
        }
        if (s_stop) goto fail;
        if (s_vsrc.failed || s_vsrc.got <= 0) {
            snprintf(s_error, sizeof(s_error), "Téléchargement impossible (réseau ou carte SD)");
            goto fail;
        }
        if (a_started && (s_asrc.failed || s_asrc.got <= 0)) a_started = false; /* lecture sans son */
        if (!src_open_read(&s_vsrc)) {
            snprintf(s_error, sizeof(s_error), "Lecture du fichier temporaire impossible");
            goto fail;
        }
        if (a_started && !src_open_read(&s_asrc)) a_started = false;
        vlog("prechargement termine : video %lld o, son %lld o", s_vsrc.got, a_started ? s_asrc.got : 0LL);
    }

    if (a_started) s_athread = make_thread(audio_thread, NULL, 128 * 1024, 0x2F, -2);
    if (!s_athread) s_audio_state = 2; /* pas de piste son, NDSP indisponible ou échec */

    if (!track_open_safe(&t, &s_vsrc, AVMEDIA_TYPE_VIDEO)) {
        if (!s_stop) snprintf(s_error, sizeof(s_error), "Vidéo illisible (%s)", s_vsrc.failed ? "réseau" : "format");
        goto fail;
    }
    if (!setup_dims(t.dec->width, t.dec->height)) {
        snprintf(s_error, sizeof(s_error), "Mémoire insuffisante");
        goto fail;
    }
    vlog("video %dx%d, texture %dx%d", t.dec->width, t.dec->height, s_tw, s_th);
    if (s_duration <= 0 && t.fmt->duration > 0) s_duration = t.fmt->duration / (double)AV_TIME_BASE;

    while (!s_stop) {
        int r = av_read_frame(t.fmt, t.pkt);
        if (r < 0) {
            avcodec_send_packet(t.dec, NULL); /* vide le décodeur */
            while (!s_stop && avcodec_receive_frame(t.dec, t.frm) == 0) push_frame(&t);
            break;
        }
        if (t.pkt->stream_index == t.idx) {
            if (avcodec_send_packet(t.dec, t.pkt) >= 0)
                while (!s_stop && avcodec_receive_frame(t.dec, t.frm) == 0) push_frame(&t);
        }
        av_packet_unref(t.pkt);
    }
    vlog("video_thread : fin du flux");
    track_close(&t);
    s_vdone = true;
    return;

fail:
    vlog("video_thread : echec (%s)", s_error[0] ? s_error : "?");
    track_close(&t);
    if (!s_stop) s_status = VS_ERROR;
    s_vdone = true;
}

/* ------------------------------------------------------------------ */
/* Son                                                                */

static inline s16 f2s(float v)
{
    int s = (int)lrintf(v * 32767.0f);
    return (s16)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
}

static void apply_volume(void)
{
    float mix[12] = { 0 };
    mix[0] = mix[1] = s_volume;
    ndspChnSetMix(CH, mix);
}

static void submit_buf(int i, int frames, double pts)
{
    s_wb[i].data_pcm16 = s_wdata[i];
    s_wb[i].nsamples = frames;
    s_wb[i].looping = false;
    s_wpts[i] = pts;
    DSP_FlushDataCache(s_wdata[i], frames * 4);
    ndspChnWaveBufAdd(CH, &s_wb[i]);
    s_aqueued++;
}

static void audio_thread(void *arg)
{
    (void)arg;
    Track t;
    if (!track_open_safe(&t, &s_asrc, AVMEDIA_TYPE_AUDIO)) {
        vlog("son : piste illisible, lecture sans son");
        s_audio_state = 2;
        s_adone = true;
        return;
    }
    AVCodecContext *dc = t.dec;
    int chans = dc->ch_layout.nb_channels;
    enum AVSampleFormat sf = dc->sample_fmt;
    if (dc->sample_rate <= 0 || chans <= 0 ||
        (sf != AV_SAMPLE_FMT_FLTP && sf != AV_SAMPLE_FMT_FLT && sf != AV_SAMPLE_FMT_S16P && sf != AV_SAMPLE_FMT_S16)) {
        track_close(&t);
        s_audio_state = 2;
        s_adone = true;
        return;
    }
    s_rate = dc->sample_rate;
    vlog("son : %d Hz, %d canaux, format %d", s_rate, chans, (int)sf);
    for (int i = 0; i < NBUF; i++) {
        s_wdata[i] = linearAlloc(BUF_FRAMES * 4);
        if (!s_wdata[i]) {
            track_close(&t);
            s_audio_state = 2;
            s_adone = true;
            return;
        }
    }
    memset(s_wb, 0, sizeof(s_wb));
    ndspChnReset(CH);
    ndspChnSetInterp(CH, NDSP_INTERP_LINEAR);
    ndspChnSetRate(CH, (float)s_rate);
    ndspChnSetFormat(CH, NDSP_FORMAT_STEREO_PCM16);
    apply_volume();
    ndspChnSetPaused(CH, true); /* démarré par le thread principal quand l'image est prête */
    s_audio_state = 1;

    int cur = 0, fill = 0;
    double cur_pts = 0;
    bool eof = false;
    while (!s_stop && !eof) {
        int r = av_read_frame(t.fmt, t.pkt);
        if (r < 0) {
            eof = true;
            avcodec_send_packet(dc, NULL);
        } else if (t.pkt->stream_index != t.idx || avcodec_send_packet(dc, t.pkt) < 0) {
            av_packet_unref(t.pkt);
            continue;
        }
        if (!eof) av_packet_unref(t.pkt);

        while (!s_stop && avcodec_receive_frame(dc, t.frm) == 0) {
            AVFrame *f = t.frm;
            double fpts = f->pts != AV_NOPTS_VALUE ? f->pts * t.tb : cur_pts;
            for (int i = 0; i < f->nb_samples && !s_stop; i++) {
                if (fill == 0) {
                    /* attend que le tampon soit rejoué */
                    while (!s_stop && (s_wb[cur].status == NDSP_WBUF_QUEUED || s_wb[cur].status == NDSP_WBUF_PLAYING))
                        svcSleepThread(3 * MS);
                    cur_pts = fpts + (double)i / s_rate;
                }
                s16 l, rr;
                switch (sf) {
                case AV_SAMPLE_FMT_FLTP:
                    l = f2s(((float *)f->data[0])[i]);
                    rr = chans > 1 ? f2s(((float *)f->data[1])[i]) : l;
                    break;
                case AV_SAMPLE_FMT_FLT:
                    l = f2s(((float *)f->data[0])[i * chans]);
                    rr = chans > 1 ? f2s(((float *)f->data[0])[i * chans + 1]) : l;
                    break;
                case AV_SAMPLE_FMT_S16P:
                    l = ((s16 *)f->data[0])[i];
                    rr = chans > 1 ? ((s16 *)f->data[1])[i] : l;
                    break;
                default:
                    l = ((s16 *)f->data[0])[i * chans];
                    rr = chans > 1 ? ((s16 *)f->data[0])[i * chans + 1] : l;
                    break;
                }
                s_wdata[cur][fill * 2] = l;
                s_wdata[cur][fill * 2 + 1] = rr;
                if (++fill == BUF_FRAMES) {
                    submit_buf(cur, fill, cur_pts);
                    cur = (cur + 1) % NBUF;
                    fill = 0;
                }
            }
        }
    }
    if (!s_stop && fill > 0) submit_buf(cur, fill, cur_pts);
    track_close(&t);
    s_adone = true;
}

/* ------------------------------------------------------------------ */
/* Horloge                                                            */

static double clock_now(void)
{
    if (s_audio_state == 1) {
        for (int i = 0; i < NBUF; i++) {
            if (s_wb[i].status == NDSP_WBUF_PLAYING) {
                s_clock_last = s_wpts[i] + ndspChnGetSamplePos(CH) / (double)s_rate;
                break;
            }
        }
        return s_clock_last; /* entre deux tampons ou son en attente : l'horloge s'arrête */
    }
    if (!s_wall_start) return 0;
    u64 now = osGetTime();
    u64 paused = s_pause_acc + (s_status == VS_PAUSED && s_pause_start ? now - s_pause_start : 0);
    return (now - s_wall_start - paused) / 1000.0;
}

static bool audio_playing_or_queued(void)
{
    for (int i = 0; i < NBUF; i++)
        if (s_wb[i].status == NDSP_WBUF_QUEUED || s_wb[i].status == NDSP_WBUF_PLAYING) return true;
    return false;
}

/* ------------------------------------------------------------------ */
/* API                                                                */

bool video_open(const char *video_id, int max_p, bool preload)
{
    video_close();
    if (!video_id || !*video_id) return false;
    vlog_start();
    snprintf(s_id, sizeof(s_id), "%s", video_id);
    s_maxp = max_p > 0 ? max_p : 360;
    s_preload = preload;
    s_error[0] = '\0';
    s_quality = 0;
    s_duration = 0;
    s_stop = false;
    s_vdone = s_adone = s_dims_ready = false;
    s_audio_state = 0;
    s_aqueued = 0;
    s_vq_r = s_vq_n = 0;
    s_clock_pub = s_clock_last = 0;
    s_wall_start = s_pause_start = s_pause_acc = 0;
    s_has_frame = false;
    LightLock_Init(&s_vq_lock);
    LightLock_Init(&s_ff_lock);
    CondVar_Init(&s_vq_cv);
    s_logged_frame = false;
    s_open_ms = osGetTime();
    av_log_set_level(AV_LOG_QUIET);

    u32 lin = linearSpaceFree();
    vlog("ouverture id=%s, max %dp, memoire lineaire libre : %lu Ko", s_id, s_maxp, (unsigned long)(lin / 1024));
    if (lin < 1024 * 1024) { /* texture + tampons son : mieux vaut refuser que planter */
        snprintf(s_error, sizeof(s_error), "Mémoire insuffisante (%lu Ko libres)", (unsigned long)(lin / 1024));
        s_status = VS_ERROR;
        return false;
    }

    if (!s_ndsp_tried) {
        s_ndsp_tried = true;
        Result nrc = ndspInit(); /* nécessite sdmc:/3ds/dspfirm.cdc */
        s_ndsp_ok = R_SUCCEEDED(nrc);
        vlog("ndspInit : 0x%08lX (%s)", (unsigned long)nrc, s_ndsp_ok ? "ok" : "pas de son");
        if (s_ndsp_ok) ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    }

    s_new3ds = false;
    APT_CheckNew3DS(&s_new3ds);
    int core = 2;
    if (!s_new3ds) {
        APT_SetAppCpuTimeLimit(80); /* Old 3DS : le décodage tourne sur le cœur 1 */
        core = 1;
    }
    s_mconn = net_conn_new(&s_stop);
    s_status = VS_LOADING;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    s_vthread = make_thread(video_thread, NULL, 256 * 1024, prio, core);
    if (!s_vthread) {
        snprintf(s_error, sizeof(s_error), "Impossible de lancer le lecteur");
        s_status = VS_ERROR;
        return false;
    }
    return true;
}

void video_close(void)
{
    if (s_status == VS_IDLE && !s_vthread) return;
    vlog("video_close");
    s_stop = true;
    if (s_ndsp_ok) ndspChnWaveBufClear(CH);
    LightLock_Lock(&s_vq_lock);
    CondVar_Broadcast(&s_vq_cv);
    LightLock_Unlock(&s_vq_lock);

    if (s_vthread) { threadJoin(s_vthread, U64_MAX); threadFree(s_vthread); s_vthread = NULL; }
    if (s_athread) { threadJoin(s_athread, U64_MAX); threadFree(s_athread); s_athread = NULL; }
    src_stop(&s_vsrc);
    src_stop(&s_asrc);
    net_conn_free(s_mconn);
    s_mconn = NULL;

    if (s_ndsp_ok) {
        ndspChnWaveBufClear(CH);
        svcSleepThread(20 * MS); /* le DSP lâche les tampons */
    }
    for (int i = 0; i < NBUF; i++) {
        if (s_wdata[i]) linearFree(s_wdata[i]);
        s_wdata[i] = NULL;
    }
    memset(s_wb, 0, sizeof(s_wb));
    for (int i = 0; i < VQ; i++) { free(s_vq[i]); s_vq[i] = NULL; }
    if (s_tex_ok) s_tex_free_pending = true; /* le GPU peut encore lire la texture : libérée au prochain frame */
    s_has_frame = false;
    s_dims_ready = false;
    s_status = VS_IDLE;
}

void video_frame(void)
{
    if (s_tex_free_pending) {
        C3D_TexDelete(&s_tex);
        s_tex_ok = false;
        s_tex_free_pending = false;
    }
    if (s_status == VS_IDLE || s_status == VS_ERROR) return;

    if (s_dims_ready && !s_tex_ok) {
        if (!C3D_TexInit(&s_tex, s_tw, s_th, GPU_RGB565)) {
            snprintf(s_error, sizeof(s_error), "Mémoire vidéo insuffisante");
            s_status = VS_ERROR;
            return;
        }
        memset(s_tex.data, 0, (size_t)s_tw * s_th * 2);
        C3D_TexFlush(&s_tex);
        C3D_TexSetFilter(&s_tex, GPU_LINEAR, GPU_LINEAR);
        C3D_TexSetWrap(&s_tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        s_sub.width = (u16)s_dw;
        s_sub.height = (u16)s_dh;
        s_sub.left = 0.0f;
        s_sub.top = 1.0f;
        s_sub.right = (float)s_dw / s_tw;
        s_sub.bottom = 1.0f - (float)s_dh / s_th;
        s_img.tex = &s_tex;
        s_img.subtex = &s_sub;
        s_tex_ok = true;
    }

    if (!s_preload && s_status == VS_LOADING && osGetTime() - s_open_ms > 45000) {
        snprintf(s_error, sizeof(s_error), "Délai dépassé (réseau lent ou flux illisible)");
        vlog("%s", s_error);
        s_status = VS_ERROR;
        return;
    }

    /* Démarrage : première image prête et son en avance de quelques tampons */
    if (s_status == VS_LOADING) {
        bool vready = s_vq_n > 0 || s_vdone;
        bool aready = s_audio_state == 2 || (s_audio_state == 1 && (s_aqueued >= 3 || s_adone));
        if (vready && aready && s_tex_ok) {
            if (s_audio_state == 1) ndspChnSetPaused(CH, false);
            s_wall_start = osGetTime();
            s_status = VS_PLAYING;
            vlog("lecture demarree");
        }
    }

    double clock = clock_now();
    s_clock_pub = (float)clock;

    if (s_status == VS_PLAYING || (s_status == VS_LOADING && !s_has_frame)) {
        /* image la plus récente dont l'heure est venue (les plus anciennes sont sautées) */
        LightLock_Lock(&s_vq_lock);
        int chosen = -1;
        while (s_vq_n > 0 && (s_vq_pts[s_vq_r] <= clock + 0.015 || !s_has_frame)) {
            chosen = s_vq_r;
            s_vq_r = (s_vq_r + 1) % VQ;
            s_vq_n--;
            if (!s_has_frame) break; /* toute première image : affichée tout de suite */
        }
        if (chosen >= 0 && s_tex_ok) {
            memcpy(s_tex.data, s_vq[chosen], (size_t)s_tw * s_th * 2);
            C3D_TexFlush(&s_tex);
            s_has_frame = true;
        }
        if (chosen >= 0) CondVar_Broadcast(&s_vq_cv);
        LightLock_Unlock(&s_vq_lock);
    }

    /* Fin : plus d'images et plus de son */
    if (s_status == VS_PLAYING && s_vdone && s_vq_n == 0 &&
        (s_audio_state != 1 || (s_adone && !audio_playing_or_queued())))
        s_status = VS_ENDED;
}

const C2D_Image *video_image(void)
{
    return (s_tex_ok && s_has_frame && !s_tex_free_pending) ? &s_img : NULL;
}

VideoStatus video_status(void) { return s_status; }
const char *video_error(void) { return s_error; }
float video_position(void) { return (float)s_clock_pub; }
float video_duration(void) { return (float)s_duration; }
int  video_quality(void) { return s_quality; }
bool video_has_audio(void) { return s_audio_state == 1; }
bool video_sound_ok(void) { return s_ndsp_ok; }
float video_volume(void) { return s_volume; }

int video_load_percent(void)
{
    Source *ss[2] = { &s_vsrc, &s_asrc };
    long long got = 0, tot = 0;
    for (int i = 0; i < 2; i++) {
        Source *s = ss[i];
        if (!s->conn || s->failed) continue;
        long long t = net_conn_total(s->conn);
        if (t <= 0) return -1; /* taille pas encore connue */
        LightLock_Lock(&s->lock);
        long long g = s->got;
        LightLock_Unlock(&s->lock);
        got += g;
        tot += t;
    }
    if (tot <= 0) return -1;
    long long pc = got * 100 / tot;
    return pc < 0 ? 0 : pc > 100 ? 100 : (int)pc;
}

bool video_buffering(void)
{
    if (s_status == VS_LOADING) return true;
    if (s_status != VS_PLAYING) return false;
    if (!s_vdone && s_vq_n == 0) return true;
    return s_audio_state == 1 && !s_adone && !audio_playing_or_queued();
}

void video_toggle_pause(void)
{
    if (s_status == VS_PLAYING) {
        if (s_audio_state == 1) ndspChnSetPaused(CH, true);
        s_pause_start = osGetTime();
        s_status = VS_PAUSED;
    } else if (s_status == VS_PAUSED) {
        if (s_audio_state == 1) ndspChnSetPaused(CH, false);
        if (s_pause_start) s_pause_acc += osGetTime() - s_pause_start;
        s_pause_start = 0;
        s_status = VS_PLAYING;
    } else if (s_status == VS_ENDED) {
        video_restart();
    }
}

void video_restart(void)
{
    char id[32];
    snprintf(id, sizeof(id), "%s", s_id);
    video_open(id, s_maxp, s_preload);
}

void video_set_volume(float v)
{
    s_volume = v < 0 ? 0 : v > 1 ? 1 : v;
    if (s_ndsp_ok && s_audio_state == 1) apply_volume();
}

void video_exit(void)
{
    video_close();
    if (s_tex_free_pending) {
        C3D_TexDelete(&s_tex);
        s_tex_free_pending = false;
        s_tex_ok = false;
    }
    if (s_ndsp_ok) ndspExit();
    s_ndsp_ok = false;
}
