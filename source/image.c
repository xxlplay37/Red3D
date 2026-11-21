#include "image.h"
#include "net.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jpeglib.h>
#include <png.h>

#define URL_MAX 600

/* Deux files : les miniatures ne sont jamais bloquées par une grosse image, et inversement. */
#define NWORKERS 2
static int worker_of(ImgKind k) { return k == IMG_THUMB ? 0 : 1; }

#define STALE_FRAMES  20  /* demande non renouvelée => abandon de la file */
#define ABORT_FRAMES  45  /* image plus affichée => téléchargement interrompu */
#define RETRY_FRAMES 240  /* nouvel essai après un échec (≈ 4 s) */
#define MAX_TRIES      3

typedef enum { E_EMPTY, E_QUEUED, E_LOADING, E_DECODED, E_READY, E_FAILED } EState;

typedef struct {
    char url[URL_MAX];
    ImgKind kind;
    EState state;
    u32 last_used; /* dernière frame où l'image a été demandée */
    u32 req_frame;
    u32 fail_frame;
    int tries;
    u32 gen;
    u8 *pixels;    /* texture tuilée (RGBA8) en attente d'envoi au GPU */
    int w, h, tw, th;
    C3D_Tex tex;
    Tex3DS_SubTexture sub;
    C2D_Image img;
} Entry;

static const int POOL[IMG_KINDS]  = { 48, 6, 1 };
static const int MAXW[IMG_KINDS]  = { 128, 400, 1024 };
static const int MAXH[IMG_KINDS]  = { 128, 240, 1024 };
static const size_t MAXDL[IMG_KINDS] = { 12u << 20, 12u << 20, 12u << 20 };

static volatile int s_quality; /* 0 normale, 1 haute, 2 très haute */

/* Taille max du décodage : l'aperçu grandit avec la qualité (512x256 = même texture GPU que 400x240) */
static int max_w(ImgKind k) { return (k == IMG_PREVIEW && s_quality >= 1) ? 512 : MAXW[k]; }
static int max_h(ImgKind k)
{
    if (k == IMG_PREVIEW && s_quality >= 2) return 512;
    if (k == IMG_PREVIEW && s_quality >= 1) return 256;
    return MAXH[k];
}

void img_set_quality(int q)
{
    s_quality = q < 0 ? 0 : (q > 2 ? 2 : q);
}

static Entry *s_entries[IMG_KINDS];
static LightLock s_lock;
static volatile bool s_quit;
static volatile u32 s_frame = 1;

typedef struct {
    Thread thread;
    LightEvent wake;
    NetConn *conn;
    volatile bool abort;
    Entry *volatile current; /* entrée en cours de chargement */
} Worker;
static Worker s_workers[NWORKERS];

/* ------------------------------------------------------------------ */
/* Conversion vers une texture tuilée                                 */

static int next_pow2(int v)
{
    int p = 8;
    while (p < v) p <<= 1;
    return p;
}

static inline u32 tile_index(int x, int y, int tw)
{
    return ((((y >> 3) * (tw >> 3) + (x >> 3)) << 6) +
            ((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3)));
}

/* Réduction par moyenne de zone, ligne par ligne (peu de mémoire). */
typedef struct {
    int sw, sh, dw, dh, tw, th;
    u32 *acc, *cnt;
    int cur_dy;
    u32 *out;
} Scaler;

static bool scaler_init(Scaler *s, int sw, int sh, int maxw, int maxh)
{
    float k = 1.0f;
    if (sw > maxw) k = (float)maxw / sw;
    if (sh * k > maxh) k = (float)maxh / sh;
    s->sw = sw; s->sh = sh;
    s->dw = (int)(sw * k); if (s->dw < 1) s->dw = 1;
    s->dh = (int)(sh * k); if (s->dh < 1) s->dh = 1;
    s->tw = next_pow2(s->dw);
    s->th = next_pow2(s->dh);
    s->acc = calloc((size_t)s->dw * 4, sizeof(u32));
    s->cnt = calloc((size_t)s->dw, sizeof(u32));
    s->out = calloc((size_t)s->tw * s->th, sizeof(u32));
    s->cur_dy = 0;
    if (!s->acc || !s->cnt || !s->out) {
        free(s->acc); free(s->cnt); free(s->out);
        s->acc = s->cnt = s->out = NULL;
        return false;
    }
    return true;
}

static void scaler_free(Scaler *s)
{
    free(s->acc); free(s->cnt); free(s->out);
    s->acc = s->cnt = s->out = NULL;
}

static void scaler_flush(Scaler *s)
{
    if (s->cur_dy >= s->dh) return;
    for (int x = 0; x < s->dw; x++) {
        u32 n = s->cnt[x] ? s->cnt[x] : 1;
        u32 r = s->acc[x * 4] / n, g = s->acc[x * 4 + 1] / n;
        u32 b = s->acc[x * 4 + 2] / n, a = s->acc[x * 4 + 3] / n;
        s->out[tile_index(x, s->cur_dy, s->tw)] = (r << 24) | (g << 16) | (b << 8) | a;
    }
    memset(s->acc, 0, (size_t)s->dw * 4 * sizeof(u32));
    memset(s->cnt, 0, (size_t)s->dw * sizeof(u32));
}

static void scaler_row(Scaler *s, int sy, const u8 *row, int comps)
{
    int dy = (int)((long long)sy * s->dh / s->sh);
    if (dy >= s->dh) dy = s->dh - 1;
    while (dy > s->cur_dy) { scaler_flush(s); s->cur_dy++; }
    for (int sx = 0; sx < s->sw; sx++) {
        int dx = (int)((long long)sx * s->dw / s->sw);
        if (dx >= s->dw) dx = s->dw - 1;
        const u8 *p = row + sx * comps;
        u32 *a = &s->acc[dx * 4];
        a[0] += p[0]; a[1] += p[1]; a[2] += p[2];
        a[3] += comps == 4 ? p[3] : 255;
        s->cnt[dx]++;
    }
}

static void scaler_finish(Scaler *s)
{
    scaler_flush(s);
    free(s->acc);
    free(s->cnt);
    s->acc = s->cnt = NULL;
}

/* ------------------------------------------------------------------ */
/* JPEG                                                               */

typedef struct {
    struct jpeg_error_mgr pub;
    jmp_buf jmp;
} JpegErr;

static void jpeg_error_exit(j_common_ptr cinfo)
{
    longjmp(((JpegErr *)cinfo->err)->jmp, 1);
}

static bool decode_jpeg(const u8 *data, size_t len, int maxw, int maxh, Scaler *s, volatile bool *abort)
{
    struct jpeg_decompress_struct cinfo;
    JpegErr jerr;
    u8 *volatile row = NULL; /* modifiées après setjmp */

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_error_exit;
    if (setjmp(jerr.jmp)) {
        free((void *)row);
        scaler_free(s);
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, (unsigned char *)data, (unsigned long)len);
    jpeg_read_header(&cinfo, TRUE);

    /* Les JPEG progressifs gardent toute l'image en mémoire : on limite la taille. */
    if (cinfo.progressive_mode && (long)cinfo.image_width * cinfo.image_height > 6000000L) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }

    /* Réduction gratuite par le décodeur (1/2, 1/4, 1/8) tant qu'on reste ≥ la cible */
    float k = 1.0f;
    if ((int)cinfo.image_width > maxw) k = (float)maxw / cinfo.image_width;
    if (cinfo.image_height * k > maxh) k = (float)maxh / cinfo.image_height;
    int denom = 1;
    while (denom < 8 && (float)(denom * 2) <= 1.0f / k) denom *= 2;
    cinfo.scale_num = 1;
    cinfo.scale_denom = denom;
    cinfo.out_color_space = JCS_RGB;
    if (s_quality >= 1) { /* plus précis (couleurs lissées, moins de blocs), un peu plus lent */
        cinfo.dct_method = JDCT_ISLOW;
        cinfo.do_fancy_upsampling = TRUE;
    } else {
        cinfo.dct_method = JDCT_IFAST;
        cinfo.do_fancy_upsampling = FALSE;
    }

    jpeg_start_decompress(&cinfo);
    if (!scaler_init(s, cinfo.output_width, cinfo.output_height, maxw, maxh)) longjmp(jerr.jmp, 1);
    row = malloc((size_t)cinfo.output_width * 3);
    if (!row) longjmp(jerr.jmp, 1);

    while (cinfo.output_scanline < cinfo.output_height) {
        JSAMPROW rp = (JSAMPROW)row;
        int y = cinfo.output_scanline;
        jpeg_read_scanlines(&cinfo, &rp, 1);
        scaler_row(s, y, (const u8 *)row, 3);
        if (s_quit || *abort) longjmp(jerr.jmp, 1);
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    free((void *)row);
    scaler_finish(s);
    return true;
}

/* ------------------------------------------------------------------ */
/* PNG (ligne par ligne : pas de limite de taille, sauf PNG entrelacés) */

typedef struct {
    const u8 *data;
    size_t len, pos;
} MemSrc;

static void png_read_mem(png_structp png, png_bytep out, png_size_t n)
{
    MemSrc *m = png_get_io_ptr(png);
    if (m->pos + n > m->len) png_error(png, "eof");
    memcpy(out, m->data + m->pos, n);
    m->pos += n;
}

static bool decode_png(const u8 *data, size_t len, int maxw, int maxh, Scaler *s, volatile bool *abort)
{
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) return false;
    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_read_struct(&png, NULL, NULL); return false; }

    MemSrc src = { data, len, 0 };
    u8 *volatile buf = NULL;
    png_bytep *volatile rows = NULL;

    if (setjmp(png_jmpbuf(png))) {
        free((void *)buf);
        free((void *)rows);
        scaler_free(s);
        png_destroy_read_struct(&png, &info, NULL);
        return false;
    }
    png_set_read_fn(png, &src, png_read_mem);
    png_read_info(png, info);

    png_uint_32 w = png_get_image_width(png, info), h = png_get_image_height(png, info);
    int interlace = png_get_interlace_type(png, info);
    png_set_expand(png);        /* palette / gris < 8 bits / tRNS -> RGB(A) 8 bits */
    png_set_strip_16(png);
    png_set_gray_to_rgb(png);
    int passes = png_set_interlace_handling(png);
    png_read_update_info(png, info);
    int comps = png_get_channels(png, info);
    if (comps != 3 && comps != 4) png_error(png, "format");
    size_t stride = png_get_rowbytes(png, info);

    if (!scaler_init(s, w, h, maxw, maxh)) png_error(png, "mem");

    if (interlace == PNG_INTERLACE_NONE && passes == 1) {
        buf = malloc(stride);
        if (!buf) png_error(png, "mem");
        for (png_uint_32 y = 0; y < h; y++) {
            png_read_row(png, (png_bytep)buf, NULL);
            scaler_row(s, y, (const u8 *)buf, comps);
            if (s_quit || *abort) png_error(png, "abort");
        }
    } else {
        /* entrelacé : l'image entière doit tenir en mémoire */
        if ((unsigned long long)stride * h > 16ull * 1024 * 1024) png_error(png, "too big");
        buf = malloc(stride * h);
        rows = malloc(sizeof(png_bytep) * h);
        if (!buf || !rows) png_error(png, "mem");
        for (png_uint_32 y = 0; y < h; y++) rows[y] = (png_bytep)buf + y * stride;
        png_read_image(png, (png_bytepp)rows);
        for (png_uint_32 y = 0; y < h; y++) scaler_row(s, y, rows[y], comps);
    }
    png_destroy_read_struct(&png, &info, NULL);
    free((void *)buf);
    free((void *)rows);
    scaler_finish(s);
    return true;
}

/* ------------------------------------------------------------------ */
/* Threads de chargement                                              */

/* Les aperçus redimensionnés par Reddit sont légers : on les sert en premier. */
static int url_weight(const char *url)
{
    return (strstr(url, "width=") || strstr(url, "thumbs.redditmedia")) ? 1 : 0;
}

static Entry *pick_job(int wi)
{
    Entry *best = NULL;
    u32 now = s_frame;
    for (int k = 0; k < IMG_KINDS; k++) {
        if (worker_of((ImgKind)k) != wi) continue;
        for (int i = 0; i < POOL[k]; i++) {
            Entry *e = &s_entries[k][i];
            if (e->state != E_QUEUED) continue;
            if (now - e->req_frame > STALE_FRAMES) { e->state = E_EMPTY; continue; }
            if (!best) { best = e; continue; }
            /* priorité : grande image plein écran > aperçu > miniature légère > récent */
            if (e->kind != best->kind) { if (e->kind > best->kind) best = e; continue; }
            int we = url_weight(e->url), wb = url_weight(best->url);
            if (we != wb) { if (we > wb) best = e; continue; }
            if (e->req_frame > best->req_frame) best = e;
        }
    }
    return best;
}

static void worker(void *arg)
{
    Worker *w = arg;
    int wi = (int)(w - s_workers);
    char url[URL_MAX], err[128];

    while (!s_quit) {
        LightEvent_Wait(&w->wake);
        for (;;) {
            if (s_quit) return;
            LightLock_Lock(&s_lock);
            Entry *e = pick_job(wi);
            if (!e) { LightLock_Unlock(&s_lock); break; }
            e->state = E_LOADING;
            u32 gen = e->gen;
            ImgKind kind = e->kind;
            memcpy(url, e->url, sizeof(url));
            w->abort = false;
            w->current = e;
            LightLock_Unlock(&s_lock);

            Buf b = {0};
            long code = 0;
            bool ok = false;
            Scaler sc = {0};
            if (http_request(w->conn, url, NULL, NULL, NULL, ACCEPT_IMAGE, MAXDL[kind],
                             &b, &code, err, sizeof(err)) == 0 && code == 200 && b.len > 8) {
                const u8 *d = (const u8 *)b.data;
                if (d[0] == 0xFF && d[1] == 0xD8)
                    ok = decode_jpeg(d, b.len, max_w(kind), max_h(kind), &sc, &w->abort);
                else if (!memcmp(d, "\x89PNG", 4))
                    ok = decode_png(d, b.len, max_w(kind), max_h(kind), &sc, &w->abort);
            }
            buf_free(&b);

            LightLock_Lock(&s_lock);
            w->current = NULL;
            if (e->gen == gen && e->state == E_LOADING) {
                if (ok) {
                    e->pixels = (u8 *)sc.out;
                    e->w = sc.dw; e->h = sc.dh; e->tw = sc.tw; e->th = sc.th;
                    e->state = E_DECODED;
                    sc.out = NULL;
                } else if (w->abort) {
                    e->state = E_EMPTY; /* interrompu : sera redemandé s'il réapparaît */
                } else {
                    e->state = E_FAILED;
                    e->fail_frame = s_frame;
                    e->tries++;
                }
            }
            LightLock_Unlock(&s_lock);
            free(sc.out);
        }
    }
}

/* ------------------------------------------------------------------ */

void img_init(void)
{
    LightLock_Init(&s_lock);
    for (int k = 0; k < IMG_KINDS; k++) {
        s_entries[k] = calloc(POOL[k], sizeof(Entry));
        if (!s_entries[k]) svcBreak(USERBREAK_PANIC); /* plus de mémoire au démarrage : rien à faire */
        for (int i = 0; i < POOL[k]; i++) s_entries[k][i].kind = (ImgKind)k;
    }
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    for (int i = 0; i < NWORKERS; i++) {
        Worker *w = &s_workers[i];
        LightEvent_Init(&w->wake, RESET_ONESHOT);
        w->conn = net_conn_new(&w->abort);
        w->thread = threadCreate(worker, w, 128 * 1024, prio + 2, -2, false);
    }
}

static void entry_reset(Entry *e)
{
    if (e->state == E_READY) C3D_TexDelete(&e->tex);
    free(e->pixels);
    e->pixels = NULL;
    e->state = E_EMPTY;
    e->url[0] = '\0';
    e->tries = 0;
    e->gen++;
}

void img_exit(void)
{
    s_quit = true;
    for (int i = 0; i < NWORKERS; i++) {
        s_workers[i].abort = true;
        LightEvent_Signal(&s_workers[i].wake);
    }
    for (int i = 0; i < NWORKERS; i++) {
        if (s_workers[i].thread) {
            threadJoin(s_workers[i].thread, U64_MAX);
            threadFree(s_workers[i].thread);
        }
        net_conn_free(s_workers[i].conn);
    }
    for (int k = 0; k < IMG_KINDS; k++) {
        for (int i = 0; i < POOL[k]; i++) entry_reset(&s_entries[k][i]);
        free(s_entries[k]);
    }
}

void img_frame(void)
{
    s_frame++;
    u32 now = s_frame;
    LightLock_Lock(&s_lock);

    /* téléchargements d'images qui ne sont plus à l'écran : on coupe */
    for (int i = 0; i < NWORKERS; i++) {
        Entry *e = s_workers[i].current;
        if (e && now - e->last_used > ABORT_FRAMES) s_workers[i].abort = true;
    }

    bool wake[NWORKERS] = { false };
    for (int k = 0; k < IMG_KINDS; k++) {
        for (int i = 0; i < POOL[k]; i++) {
            Entry *e = &s_entries[k][i];
            if (e->state == E_QUEUED) wake[worker_of((ImgKind)k)] = true;
            if (e->state != E_DECODED) continue;
            if (!C3D_TexInit(&e->tex, e->tw, e->th, GPU_RGBA8)) {
                free(e->pixels);
                e->pixels = NULL;
                e->state = E_FAILED;
                e->fail_frame = now;
                e->tries++;
                continue;
            }
            memcpy(e->tex.data, e->pixels, (size_t)e->tw * e->th * 4);
            C3D_TexFlush(&e->tex);
            C3D_TexSetFilter(&e->tex, GPU_LINEAR, GPU_LINEAR);
            C3D_TexSetWrap(&e->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
            free(e->pixels);
            e->pixels = NULL;
            e->sub.width = (u16)e->w;
            e->sub.height = (u16)e->h;
            e->sub.left = 0.0f;
            e->sub.top = 1.0f;
            e->sub.right = (float)e->w / e->tw;
            e->sub.bottom = 1.0f - (float)e->h / e->th;
            e->img.tex = &e->tex;
            e->img.subtex = &e->sub;
            e->state = E_READY;
        }
    }
    LightLock_Unlock(&s_lock);
    for (int i = 0; i < NWORKERS; i++)
        if (wake[i]) LightEvent_Signal(&s_workers[i].wake);
}

const C2D_Image *img_get(const char *url, ImgKind kind, ImgState *state)
{
    if (state) *state = IMGS_FAILED;
    if (!url || !*url || strlen(url) >= URL_MAX) return NULL;

    const C2D_Image *res = NULL;
    u32 now = s_frame;
    bool signal = false;
    LightLock_Lock(&s_lock);

    Entry *pool = s_entries[kind];
    Entry *found = NULL;
    for (int i = 0; i < POOL[kind]; i++)
        if (pool[i].state != E_EMPTY && !strcmp(pool[i].url, url)) { found = &pool[i]; break; }

    if (!found) {
        /* Emplacement libre, sinon on recycle la moins récemment utilisée
         * (jamais une texture dessinée pendant cette frame, ni un chargement en cours). */
        Entry *victim = NULL;
        for (int i = 0; i < POOL[kind]; i++) {
            Entry *e = &pool[i];
            if (e->state == E_EMPTY) { victim = e; break; }
            if (e->state == E_LOADING || e->state == E_DECODED) continue;
            if (e->last_used >= now) continue;
            if (!victim || e->last_used < victim->last_used) victim = e;
        }
        if (victim) {
            entry_reset(victim);
            snprintf(victim->url, URL_MAX, "%s", url);
            victim->state = E_QUEUED;
            victim->req_frame = now;
            found = victim;
            signal = true;
        }
    }

    if (found) {
        found->last_used = now;
        /* échec : nouvel essai au bout de quelques secondes */
        if (found->state == E_FAILED && found->tries < MAX_TRIES && now - found->fail_frame > RETRY_FRAMES) {
            found->state = E_QUEUED;
            found->gen++;
            signal = true;
        }
        switch (found->state) {
        case E_READY:
            res = &found->img;
            if (state) *state = IMGS_READY;
            break;
        case E_FAILED:
            if (state) *state = IMGS_FAILED;
            break;
        case E_QUEUED:
            found->req_frame = now;
            /* fallthrough */
        default:
            if (state) *state = IMGS_LOADING;
            break;
        }
    } else if (state) {
        *state = IMGS_LOADING; /* toutes les places sont prises : on réessaiera */
    }
    LightLock_Unlock(&s_lock);
    if (signal) LightEvent_Signal(&s_workers[worker_of(kind)].wake);
    return res;
}

void img_size(const C2D_Image *img, float *w, float *h)
{
    *w = img->subtex->width;
    *h = img->subtex->height;
}

void img_draw_fit(const C2D_Image *img, float x, float y, float w, float h, float zoom,
                  float pan_x, float pan_y)
{
    float iw = img->subtex->width, ih = img->subtex->height;
    float k = w / iw;
    if (ih * k > h) k = h / ih;
    k *= zoom;
    float dw = iw * k, dh = ih * k;
    C2D_DrawImageAt(*img, x + (w - dw) / 2 + pan_x, y + (h - dh) / 2 + pan_y, 0.5f, NULL, k, k);
}

void img_drop_kind(ImgKind kind)
{
    u32 now = s_frame;
    LightLock_Lock(&s_lock);
    for (int i = 0; i < POOL[kind]; i++) {
        Entry *e = &s_entries[kind][i];
        if (e->state == E_LOADING) { e->gen++; e->state = E_EMPTY; continue; } /* résultat ignoré */
        if (e->state == E_READY && e->last_used >= now) continue;
        entry_reset(e);
    }
    for (int i = 0; i < NWORKERS; i++) {
        Entry *e = s_workers[i].current;
        if (e && e->kind == kind) s_workers[i].abort = true;
    }
    LightLock_Unlock(&s_lock);
}

void img_drop_all(void)
{
    for (int k = 0; k < IMG_KINDS; k++) img_drop_kind((ImgKind)k);
}
