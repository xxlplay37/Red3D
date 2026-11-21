#pragma once
#include <stdbool.h>

/* Paramètres de l'appli, enregistrés dans sdmc:/3ds/reddit3ds/config.ini */

enum { LANG_FR, LANG_EN, UI_LANGS };
enum { CL_ALL, CL_FR, CL_EN, CL_ES, CL_DE, CL_IT, CL_PT, CONTENT_LANGS };
enum { NSFW_HIDE, NSFW_BLUR, NSFW_SHOW, NSFW_MODES };
enum { IMAGES_ALL, IMAGES_LIGHT, IMAGES_NONE, IMAGES_MODES };
enum { TEXT_SMALL, TEXT_NORMAL, TEXT_LARGE, TEXT_SIZES };
enum { SRC_AUTO, SRC_REDLIB, SRC_ARCHIVE, SRC_MODES };
enum { TL_FR, TL_EN, TL_ES, TL_DE, TL_IT, TL_PT, TL_LANGS };
enum { VQ_AUTO, VQ_220, VQ_270, VQ_360, VIDEO_QUALITIES };
enum { VM_DIRECT, VM_PRELOAD, VIDEO_MODES };
enum { IQ_NORMAL, IQ_HIGH, IQ_MAX, IMG_QUALITIES };   /* qualité des images */
#define COM_BATCHES 5                                  /* 4, 8, 12, 20, tous */   /* lecture en flux / après chargement complet */

typedef struct {
    int ui_lang;
    int content_lang;
    int default_sort;   /* 0..3 : hot, new, top, rising */
    int nsfw;
    int images;
    int text_size;
    int source;
    int tr_lang;        /* langue cible de la traduction */
    int video_q;        /* qualité vidéo maximale */
    int img_quality;    /* IQ_* : décodage JPEG plus précis + aperçus plus grands */
    int com_batch;      /* commentaires affichés à la fois (index : 4, 8, 12, 20, tous) */
    int video_mode;     /* VM_DIRECT : en flux, VM_PRELOAD : démarre une fois tout chargé */
    char start_sub[64]; /* "" = fil d'accueil */
    char recent[700];   /* subreddits récents, séparés par des virgules */
    /* conservés tels quels (édition manuelle du fichier) */
    char client_id[64];
    char redlib[1024];
} Settings;

extern Settings g_cfg;

void settings_load(void);
bool settings_save(void);
/* Ajoute un subreddit en tête des récents ; vrai si la liste a changé */
bool settings_add_recent(const char *sub);
int  settings_recent(char out[][64], int max);
/* Applique les réglages réseau à reddit.c */
void settings_apply_source(void);

/* Multi-subreddit du fil d'accueil selon la langue du contenu */
const char *settings_home_feed(void);
const char *settings_content_lang_name(int cl);
const char *settings_content_lang_code(int cl);
const char *settings_tr_code(int tl);
const char *settings_tr_name(int tl);
/* Qualité vidéo maximale en pixels (plus petit côté), selon le réglage et le modèle */
int settings_video_maxp(void);
/* Nombre de commentaires affichés à la fois (0 = tous) */
int settings_comment_batch(void);

/* Traduction : TR("français", "english") */
#define TR(fr, en) (g_cfg.ui_lang == LANG_EN ? (en) : (fr))
