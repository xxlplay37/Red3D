/*
 * Sons de l'interface et musique de fond.
 *
 *   Sons    : décodés une fois (FFmpeg) en PCM 16 bits, joués sur 4 voix NDSP (canaux 2..5).
 *   Musique : thread dédié, décodage en flux vers 6 tampons NDSP (canal 1), en boucle.
 */
#include "audio.h"
#include "settings.h"

#include <3ds.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>

#define MS           1000000LL
#define CH_MUSIC     1
#define CH_SFX0      2
#define SFX_VOICES   4
#define SFX_VARIANTS 4            /* fichiers différents par son (choisi au hasard) */
#define SFX_MAX_SEC  4            /* les sons plus longs sont coupés */
#define MUS_NBUF     6
#define MUS_FRAMES   4096         /* échantillons stéréo par tampon (~93 ms à 44,1 kHz) */

static const char *SFX_NAMES[SFX_COUNT] = {
    "navigation", "valider", "retour", "ouvrir", "changer", "onglet", "erreur", "notification", "touche",
};
static const float SFX_GAIN = 0.55f;
static const float MUSIC_VOL[] = { 0.25f, 0.5f, 0.75f, 1.0f };

static bool s_ok, s_inited;
static LightLock s_ff;

typedef struct {
    s16 *pcm;       /* mémoire linéaire (lue par le DSP) */
    u32 frames;
    int channels;   /* 1 ou 2 */
    int rate;
} Clip;

static Clip s_clips[SFX_COUNT][SFX_VARIANTS];
static int s_nclips[SFX_COUNT];
static ndspWaveBuf s_voice_wb[SFX_VOICES];
static int s_next_voice;
static bool s_sfx_on;

void audio_ff_lock(void) { if (s_inited) LightLock_Lock(&s_ff); }
void audio_ff_unlock(void) { if (s_inited) LightLock_Unlock(&s_ff); }
bool audio_ok(void) { return s_ok; }
const char *sfx_name(int id) { return (id >= 0 && id < SFX_COUNT) ? SFX_NAMES[id] : ""; }

/* ------------------------------------------------------------------ */
/* Lecture d'un fichier audio par FFmpeg (entrée personnalisée sur FILE*) */

typedef struct {
    FILE *f;
    AVFormatContext *fmt;
    AVIOContext *avio;
    AVCodecContext *dec;
    AVPacket *pkt;
    AVFrame *frm;
    int idx;
} Dec;

static int file_read(void *op, uint8_t *buf, int n)
{
    size_t r = fread(buf, 1, (size_t)n, (FILE *)op);
    return r > 0 ? (int)r : AVERROR_EOF;
}

static int64_t file_seek(void *op, int64_t off, int whence)
{
    FILE *f = op;
    if (whence & AVSEEK_SIZE) {
        long cur = ftell(f);
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, cur, SEEK_SET);
        return sz;
    }
    whence &= ~AVSEEK_FORCE;
    if (fseek(f, (long)off, whence) != 0) return -1;
    return ftell(f);
}

static const char *ext_of(const char *path)
{
    const char *d = strrchr(path, '.');
    return d ? d + 1 : "";
}

static bool is_audio_file(const char *name)
{
    const char *e = ext_of(name);
    return name[0] != '.' && (!strcasecmp(e, "ogg") || !strcasecmp(e, "mp3") || !strcasecmp(e, "wav") ||
                              !strcasecmp(e, "m4a") || !strcasecmp(e, "mp4"));
}

static void dec_close(Dec *d)
{
    av_frame_free(&d->frm);
    av_packet_free(&d->pkt);
    avcodec_free_context(&d->dec);
    if (d->fmt) avformat_close_input(&d->fmt);
    if (d->avio) {
        av_freep(&d->avio->buffer);
        avio_context_free(&d->avio);
    }
    if (d->f) fclose(d->f);
    memset(d, 0, sizeof(*d));
}

static bool dec_open(Dec *d, const char *path)
{
    memset(d, 0, sizeof(*d));
    d->f = fopen(path, "rb");
    if (!d->f) return false;
    audio_ff_lock();
    bool ok = false;
    unsigned char *iobuf = av_malloc(16 * 1024);
    if (!iobuf) goto out;
    d->avio = avio_alloc_context(iobuf, 16 * 1024, 0, d->f, file_read, NULL, file_seek);
    if (!d->avio) { av_free(iobuf); goto out; }
    d->fmt = avformat_alloc_context();
    if (!d->fmt) goto out;
    d->fmt->pb = d->avio;
    d->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    const char *e = ext_of(path);
    const AVInputFormat *ifmt = !strcasecmp(e, "ogg") ? av_find_input_format("ogg")
                              : !strcasecmp(e, "mp3") ? av_find_input_format("mp3")
                              : !strcasecmp(e, "wav") ? av_find_input_format("wav")
                                                      : av_find_input_format("mov");
    if (avformat_open_input(&d->fmt, NULL, ifmt, NULL) < 0) { d->fmt = NULL; goto out; }
    if (avformat_find_stream_info(d->fmt, NULL) < 0) goto out;
    d->idx = av_find_best_stream(d->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (d->idx < 0) goto out;
    const AVCodec *c = avcodec_find_decoder(d->fmt->streams[d->idx]->codecpar->codec_id);
    if (!c) goto out;
    d->dec = avcodec_alloc_context3(c);
    if (!d->dec || avcodec_parameters_to_context(d->dec, d->fmt->streams[d->idx]->codecpar) < 0) goto out;
    d->dec->thread_count = 1;
    if (avcodec_open2(d->dec, c, NULL) < 0) goto out;
    d->pkt = av_packet_alloc();
    d->frm = av_frame_alloc();
    ok = d->pkt && d->frm && d->dec->sample_rate > 0 && d->dec->ch_layout.nb_channels > 0;
out:
    audio_ff_unlock();
    if (!ok) dec_close(d);
    return ok;
}

static inline s16 f2s(float v)
{
    int s = (int)(v * 32767.0f);
    return (s16)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
}

/* Échantillon i, canal c (0 ou 1) d'une trame décodée, en 16 bits */
static inline s16 sample_at(const AVFrame *f, int i, int c, int chans)
{
    if (c >= chans) c = chans - 1;
    switch (f->format) {
    case AV_SAMPLE_FMT_FLTP: return f2s(((const float *)f->data[c])[i]);
    case AV_SAMPLE_FMT_FLT:  return f2s(((const float *)f->data[0])[i * chans + c]);
    case AV_SAMPLE_FMT_S16P: return ((const s16 *)f->data[c])[i];
    case AV_SAMPLE_FMT_S16:  return ((const s16 *)f->data[0])[i * chans + c];
    case AV_SAMPLE_FMT_S32:  return (s16)(((const int32_t *)f->data[0])[i * chans + c] >> 16);
    case AV_SAMPLE_FMT_S32P: return (s16)(((const int32_t *)f->data[c])[i] >> 16);
    default:                 return 0;
    }
}

/* Trame suivante (0 si une trame est prête dans d->frm, <0 en fin de fichier ou erreur) */
static int dec_next(Dec *d, bool *eof)
{
    for (;;) {
        int r = avcodec_receive_frame(d->dec, d->frm);
        if (r == 0) return 0;
        if (r == AVERROR_EOF) return -1;
        if (*eof) return -1;
        r = av_read_frame(d->fmt, d->pkt);
        if (r < 0) {
            *eof = true;
            avcodec_send_packet(d->dec, NULL);
            continue;
        }
        if (d->pkt->stream_index == d->idx) avcodec_send_packet(d->dec, d->pkt);
        av_packet_unref(d->pkt);
    }
}

/* Décode entièrement un son court en mémoire linéaire */
static bool load_clip(const char *path, Clip *out)
{
    Dec d;
    if (!dec_open(&d, path)) return false;
    int chans = d.dec->ch_layout.nb_channels > 1 ? 2 : 1;
    int rate = d.dec->sample_rate;
    u32 cap = (u32)rate * SFX_MAX_SEC, n = 0;
    s16 *tmp = malloc((size_t)cap * chans * sizeof(s16));
    bool eof = false;
    while (tmp && n < cap && dec_next(&d, &eof) == 0) {
        const AVFrame *f = d.frm;
        int fc = d.dec->ch_layout.nb_channels;
        for (int i = 0; i < f->nb_samples && n < cap; i++, n++)
            for (int c = 0; c < chans; c++) tmp[n * chans + c] = sample_at(f, i, c, fc);
    }
    dec_close(&d);
    if (!tmp || n == 0) { free(tmp); return false; }
    out->pcm = linearAlloc((size_t)n * chans * sizeof(s16));
    if (!out->pcm) { free(tmp); return false; }
    memcpy(out->pcm, tmp, (size_t)n * chans * sizeof(s16));
    free(tmp);
    DSP_FlushDataCache(out->pcm, n * chans * sizeof(s16));
    out->frames = n;
    out->channels = chans;
    out->rate = rate;
    return true;
}

static void clips_free(void)
{
    for (int i = 0; i < SFX_VOICES; i++) ndspChnWaveBufClear(CH_SFX0 + i);
    svcSleepThread(10 * MS); /* le DSP lâche les tampons */
    for (int s = 0; s < SFX_COUNT; s++) {
        for (int v = 0; v < s_nclips[s]; v++) linearFree(s_clips[s][v].pcm);
        s_nclips[s] = 0;
    }
    memset(s_clips, 0, sizeof(s_clips));
}

/* Charge les sons du pack (dossier par son, ou fichier <son>.<ext>), ceux par défaut sinon */
static void clips_load(const char *pack)
{
    clips_free();
    for (int s = 0; s < SFX_COUNT; s++) {
        if (pack && *pack) {
            char dir[192];
            snprintf(dir, sizeof(dir), SOUNDS_DIR "/%s/%s", pack, SFX_NAMES[s]);
            DIR *dp = opendir(dir);
            if (dp) {
                struct dirent *e;
                while ((e = readdir(dp)) && s_nclips[s] < SFX_VARIANTS) {
                    if (!is_audio_file(e->d_name) || strlen(e->d_name) > 120) continue;
                    char path[456];
                    snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
                    if (load_clip(path, &s_clips[s][s_nclips[s]])) s_nclips[s]++;
                }
                closedir(dp);
            }
            static const char *EXTS[] = { "ogg", "mp3", "wav", "m4a" };
            for (int k = 0; k < 4 && s_nclips[s] == 0; k++) {
                char path[200];
                snprintf(path, sizeof(path), SOUNDS_DIR "/%s/%s.%s", pack, SFX_NAMES[s], EXTS[k]);
                if (load_clip(path, &s_clips[s][0])) s_nclips[s] = 1;
            }
        }
        if (s_nclips[s] == 0) {
            char path[96];
            snprintf(path, sizeof(path), "romfs:/sons/defaut/%s.ogg", SFX_NAMES[s]);
            if (load_clip(path, &s_clips[s][0])) s_nclips[s] = 1;
        }
    }
}

void sfx_play(int id)
{
    if (!s_ok || !s_sfx_on || id < 0 || id >= SFX_COUNT || s_nclips[id] == 0) return;
    const Clip *c = &s_clips[id][s_nclips[id] > 1 ? rand() % s_nclips[id] : 0];
    int v = s_next_voice;
    s_next_voice = (s_next_voice + 1) % SFX_VOICES;
    int ch = CH_SFX0 + v;
    ndspChnWaveBufClear(ch);
    ndspChnReset(ch);
    ndspChnSetInterp(ch, NDSP_INTERP_LINEAR);
    ndspChnSetRate(ch, (float)c->rate);
    ndspChnSetFormat(ch, c->channels == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
    float mix[12] = { 0 };
    mix[0] = mix[1] = SFX_GAIN;
    ndspChnSetMix(ch, mix);
    ndspWaveBuf *wb = &s_voice_wb[v];
    memset(wb, 0, sizeof(*wb));
    wb->data_pcm16 = c->pcm;
    wb->nsamples = c->frames;
    ndspChnWaveBufAdd(ch, wb);
}

/* ------------------------------------------------------------------ */
/* Musique de fond                                                    */

static Thread s_mus_thread;
static volatile bool s_mus_stop, s_mus_paused;
static char s_mus_choice[96];   /* "off", "" (par défaut), "*" (au hasard) ou un fichier de MUSIC_DIR */
static char s_mus_now[96];
static float s_mus_vol = 0.5f;
static s16 *s_mus_data[MUS_NBUF];
static ndspWaveBuf s_mus_wb[MUS_NBUF];

const char *audio_music_now(void) { return s_mus_now; }

static void mus_apply_volume(void)
{
    float mix[12] = { 0 };
    mix[0] = mix[1] = s_mus_vol;
    ndspChnSetMix(CH_MUSIC, mix);
}

/* Chemin du prochain morceau selon le choix */
static bool mus_pick(char *path, size_t n)
{
    if (!strcmp(s_mus_choice, "*")) {
        char list[32][96];
        int k = audio_tracks(list, 32);
        if (k > 0) {
            int r = rand() % k;
            snprintf(path, n, MUSIC_DIR "/%s", list[r]);
            snprintf(s_mus_now, sizeof(s_mus_now), "%s", list[r]);
            return true;
        }
    } else if (s_mus_choice[0]) {
        snprintf(path, n, MUSIC_DIR "/%s", s_mus_choice);
        snprintf(s_mus_now, sizeof(s_mus_now), "%s", s_mus_choice);
        return true;
    }
    snprintf(path, n, "romfs:/musique/heavenly_loop.ogg");
    snprintf(s_mus_now, sizeof(s_mus_now), "Heavenly Loop (isaiah658)");
    return true;
}

static void music_thread(void *arg)
{
    (void)arg;
    int cur = 0, fails = 0;
    ndspChnReset(CH_MUSIC);
    ndspChnSetInterp(CH_MUSIC, NDSP_INTERP_LINEAR);
    ndspChnSetFormat(CH_MUSIC, NDSP_FORMAT_STEREO_PCM16);
    mus_apply_volume();
    while (!s_mus_stop) {
        char path[192];
        mus_pick(path, sizeof(path));
        Dec d;
        if (!dec_open(&d, path)) {
            /* fichier illisible : on revient à la musique par défaut */
            if (++fails >= 3 || !strcmp(path, "romfs:/musique/heavenly_loop.ogg")) break;
            s_mus_choice[0] = '\0';
            continue;
        }
        fails = 0;
        ndspChnSetRate(CH_MUSIC, (float)d.dec->sample_rate);
        int fc = d.dec->ch_layout.nb_channels;
        bool eof = false;
        int fill = 0;
        while (!s_mus_stop) {
            while (s_mus_paused && !s_mus_stop) svcSleepThread(50 * MS);
            if (dec_next(&d, &eof) != 0) break; /* fin du morceau : suivant (ou le même, en boucle) */
            const AVFrame *f = d.frm;
            for (int i = 0; i < f->nb_samples && !s_mus_stop; i++) {
                if (fill == 0) { /* attend que le tampon ait été rejoué */
                    while (!s_mus_stop && (s_mus_wb[cur].status == NDSP_WBUF_QUEUED ||
                                           s_mus_wb[cur].status == NDSP_WBUF_PLAYING))
                        svcSleepThread(5 * MS);
                }
                s_mus_data[cur][fill * 2] = sample_at(f, i, 0, fc);
                s_mus_data[cur][fill * 2 + 1] = sample_at(f, i, 1, fc);
                if (++fill == MUS_FRAMES) {
                    ndspWaveBuf *wb = &s_mus_wb[cur];
                    memset(wb, 0, sizeof(*wb));
                    wb->data_pcm16 = s_mus_data[cur];
                    wb->nsamples = MUS_FRAMES;
                    DSP_FlushDataCache(s_mus_data[cur], MUS_FRAMES * 4);
                    ndspChnWaveBufAdd(CH_MUSIC, wb);
                    cur = (cur + 1) % MUS_NBUF;
                    fill = 0;
                }
            }
        }
        dec_close(&d);
    }
    s_mus_now[0] = '\0';
}

static void music_stop(void)
{
    if (!s_mus_thread) return;
    s_mus_stop = true;
    threadJoin(s_mus_thread, U64_MAX);
    threadFree(s_mus_thread);
    s_mus_thread = NULL;
    ndspChnWaveBufClear(CH_MUSIC);
    svcSleepThread(10 * MS);
    for (int i = 0; i < MUS_NBUF; i++) {
        linearFree(s_mus_data[i]);
        s_mus_data[i] = NULL;
    }
    memset(s_mus_wb, 0, sizeof(s_mus_wb));
    s_mus_now[0] = '\0';
}

static void music_start(void)
{
    music_stop();
    if (!s_ok || !strcmp(s_mus_choice, "off")) return;
    for (int i = 0; i < MUS_NBUF; i++) {
        s_mus_data[i] = linearAlloc(MUS_FRAMES * 4);
        if (!s_mus_data[i]) { music_stop(); return; }
    }
    memset(s_mus_wb, 0, sizeof(s_mus_wb));
    s_mus_stop = false;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    s_mus_thread = threadCreate(music_thread, NULL, 96 * 1024, prio + 2, -2, false);
}

void music_pause(bool p)
{
    s_mus_paused = p;
    if (s_ok) ndspChnSetPaused(CH_MUSIC, p);
}

/* ------------------------------------------------------------------ */
/* Dossiers de la carte SD                                            */

int audio_packs(char out[][48], int max)
{
    int n = 0;
    DIR *dp = opendir(SOUNDS_DIR);
    if (!dp) return 0;
    struct dirent *e;
    while ((e = readdir(dp)) && n < max) {
        if (e->d_name[0] == '.' || e->d_name[0] == '_' || strlen(e->d_name) >= 48) continue;
        char p[160];
        snprintf(p, sizeof(p), SOUNDS_DIR "/%s", e->d_name);
        struct stat st;
        if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) snprintf(out[n++], 48, "%s", e->d_name);
    }
    closedir(dp);
    return n;
}

int audio_tracks(char out[][96], int max)
{
    int n = 0;
    DIR *dp = opendir(MUSIC_DIR);
    if (!dp) return 0;
    struct dirent *e;
    while ((e = readdir(dp)) && n < max)
        if (is_audio_file(e->d_name) && strlen(e->d_name) < 96) snprintf(out[n++], 96, "%s", e->d_name);
    closedir(dp);
    return n;
}

static void write_if_missing(const char *path, const char *text)
{
    FILE *f = fopen(path, "r");
    if (f) { fclose(f); return; }
    f = fopen(path, "w");
    if (!f) return;
    fputs(text, f);
    fclose(f);
}

static const char SOUNDS_README[] =
    "Red3D - packs de sons de l'interface\n"
    "====================================\n"
    "\n"
    "Chaque dossier ici est un pack, à choisir dans Paramètres > Sons > Pack de sons.\n"
    "Le dossier _modele (ignoré par l'appli) montre la structure : copie-le, renomme-le\n"
    "(ex. : mon_pack) et mets tes sons dedans.\n"
    "\n"
    "Structure d'un pack : un dossier par son\n"
    "  sons/mon_pack/navigation/    déplacement dans une liste (croix)\n"
    "  sons/mon_pack/valider/       A : valider, activer un réglage\n"
    "  sons/mon_pack/retour/        B : revenir en arrière\n"
    "  sons/mon_pack/ouvrir/        ouvrir un post, une image, une vidéo, un subreddit\n"
    "  sons/mon_pack/changer/       changer une valeur (gauche / droite), le tri\n"
    "  sons/mon_pack/onglet/        changer d'onglet, ouvrir les Subs ou les Paramètres\n"
    "  sons/mon_pack/erreur/        erreur de chargement\n"
    "  sons/mon_pack/notification/  abonnement, test des serveurs terminé...\n"
    "  sons/mon_pack/touche/        touche du clavier tactile\n"
    "\n"
    "Dans chaque dossier : un fichier audio (n'importe quel nom). S'il y en a plusieurs (4 au plus),\n"
    "l'appli en joue un au hasard. On peut aussi mettre directement sons/mon_pack/valider.ogg.\n"
    "Formats : .ogg, .mp3, .wav, .m4a. Sons courts conseillés (coupés après 4 secondes).\n"
    "Un son absent du pack est remplacé par celui par défaut.\n"
    "\n"
    "Il faut sdmc:/3ds/dspfirm.cdc (créé une fois avec l'outil DSP1) pour avoir du son.\n"
    "Sons par défaut : « Interface Sounds » de Kenney (www.kenney.nl), domaine public (CC0).\n";

static const char MUSIC_README[] =
    "Red3D - musique de fond\n"
    "=======================\n"
    "\n"
    "Mets ici tes musiques (.ogg, .mp3, .wav, .m4a), puis choisis-les dans\n"
    "Paramètres > Sons > Musique : un morceau précis (joué en boucle), « Au hasard » (tous les\n"
    "morceaux de ce dossier) ou « Par défaut ».\n"
    "La musique se met en pause pendant les vidéos.\n"
    "\n"
    "Conseil : .ogg en 44,1 kHz stéréo, 96 à 128 kbit/s (léger et bien décodé, même sur Old 3DS).\n"
    "Musique par défaut : « Heavenly Loop » par isaiah658 (opengameart.org), domaine public (CC0).\n";

static void make_dirs(void)
{
    mkdir("sdmc:/3ds", 0777);
    mkdir(AUDIO_DIR, 0777);
    mkdir(SOUNDS_DIR, 0777);
    mkdir(MUSIC_DIR, 0777);
    write_if_missing(SOUNDS_DIR "/LISEZMOI.txt", SOUNDS_README);
    write_if_missing(MUSIC_DIR "/LISEZMOI.txt", MUSIC_README);
    char p[160];
    snprintf(p, sizeof(p), SOUNDS_DIR "/_modele");
    struct stat st;
    if (stat(p, &st) != 0) { /* modèle de pack : un dossier vide par son */
        mkdir(p, 0777);
        for (int s = 0; s < SFX_COUNT; s++) {
            snprintf(p, sizeof(p), SOUNDS_DIR "/_modele/%s", SFX_NAMES[s]);
            mkdir(p, 0777);
        }
    }
}

/* ------------------------------------------------------------------ */

void audio_apply(void)
{
    if (!s_ok) return;
    static char loaded_pack[48] = "\x01"; /* rien de chargé */
    s_sfx_on = g_cfg.sfx;
    if (strcmp(loaded_pack, g_cfg.sfx_pack)) {
        clips_load(g_cfg.sfx_pack);
        snprintf(loaded_pack, sizeof(loaded_pack), "%s", g_cfg.sfx_pack);
    }
    int vi = g_cfg.music_vol;
    s_mus_vol = MUSIC_VOL[vi >= 0 && vi < 4 ? vi : 1];
    if (s_mus_thread) mus_apply_volume();
    if (strcmp(s_mus_choice, g_cfg.music) || (!s_mus_thread && strcmp(g_cfg.music, "off"))) {
        snprintf(s_mus_choice, sizeof(s_mus_choice), "%s", g_cfg.music);
        music_start();
    }
}

void audio_init(void)
{
    LightLock_Init(&s_ff);
    s_inited = true;
    make_dirs();
    s_ok = R_SUCCEEDED(ndspInit()); /* nécessite sdmc:/3ds/dspfirm.cdc */
    if (!s_ok) return;
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    av_log_set_level(AV_LOG_QUIET);
    s_mus_choice[0] = '\x01'; /* force le démarrage selon g_cfg */
    audio_apply();
}

void audio_exit(void)
{
    if (!s_ok) return;
    music_stop();
    clips_free();
    ndspExit();
    s_ok = false;
}
