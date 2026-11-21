#include "settings.h"
#include "reddit.h"

#include <3ds.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define CONFIG_DIR  "sdmc:/3ds/reddit3ds"
#define CONFIG_PATH CONFIG_DIR "/config.ini"

Settings g_cfg;

/* Fils d'accueil par langue : grands subreddits actifs dans cette langue */
static const struct { const char *name, *code, *feed; } CONTENT[CONTENT_LANGS] = {
    [CL_ALL] = { "Tout (r/popular)", "",   "popular" },
    [CL_FR]  = { "Français",         "FR", "france+AskFrance+Quebec+belgium+jeuxvideo+programmation+Paris+Lyon" },
    [CL_EN]  = { "English",          "EN", "worldnews+AskReddit+todayilearned+technology+gaming+movies+science+pics" },
    [CL_ES]  = { "Español",          "ES", "es+spain+mexico+argentina+chile+colombia+uruguay+Republica_Argentina" },
    [CL_DE]  = { "Deutsch",          "DE", "de+germany+Austria+Switzerland+de_IAmA+Finanzen+fragreddit" },
    [CL_IT]  = { "Italiano",         "IT", "italy+Italia+ItaliaPersonalFinance+italygames+ItalyInformatica" },
    [CL_PT]  = { "Português",        "PT", "brasil+portugal+desabafos+futebol+investimentos" },
};

static const char *SORT_KEYS[] = { "hot", "new", "top", "rising" };
static const char *NSFW_KEYS[] = { "hide", "blur", "show" };
static const char *IMG_KEYS[]  = { "all", "light", "none" };
static const char *TEXT_KEYS[] = { "small", "normal", "large" };
static const char *SRC_KEYS[]  = { "auto", "redlib", "archive" };
static const char *LANG_KEYS[] = { "fr", "en" };
static const char *CL_KEYS[]   = { "all", "fr", "en", "es", "de", "it", "pt" };
static const char *TL_KEYS[]   = { "fr", "en", "es", "de", "it", "pt" };
static const char *TL_NAMES[]  = { "Français", "English", "Español", "Deutsch", "Italiano", "Português" };
static const char *VQ_KEYS[]   = { "auto", "220", "270", "360" };
static const char *VM_KEYS[]   = { "direct", "preload" };
static const char *IQ_KEYS[]   = { "normal", "high", "max" };
static const char *CB_KEYS[]   = { "4", "8", "12", "20", "all" };
static const int   CB_VALUES[] = { 4, 8, 12, 20, 0 };

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    return s;
}

static int find_key(const char *v, const char **keys, int n, int def)
{
    for (int i = 0; i < n; i++)
        if (!strcasecmp(v, keys[i])) return i;
    return def;
}

static void defaults(void)
{
    memset(&g_cfg, 0, sizeof(g_cfg));
    /* langue de la console par défaut */
    u8 lang = CFG_LANGUAGE_FR;
    if (R_SUCCEEDED(cfguInit())) {
        CFGU_GetSystemLanguage(&lang);
        cfguExit();
    }
    g_cfg.ui_lang = (lang == CFG_LANGUAGE_FR) ? LANG_FR : LANG_EN;
    switch (lang) {
    case CFG_LANGUAGE_FR: g_cfg.tr_lang = TL_FR; break;
    case CFG_LANGUAGE_ES: g_cfg.tr_lang = TL_ES; break;
    case CFG_LANGUAGE_DE: g_cfg.tr_lang = TL_DE; break;
    case CFG_LANGUAGE_IT: g_cfg.tr_lang = TL_IT; break;
    case CFG_LANGUAGE_PT: g_cfg.tr_lang = TL_PT; break;
    default:              g_cfg.tr_lang = TL_EN; break;
    }
    switch (lang) {
    case CFG_LANGUAGE_FR: g_cfg.content_lang = CL_FR; break;
    case CFG_LANGUAGE_ES: g_cfg.content_lang = CL_ES; break;
    case CFG_LANGUAGE_DE: g_cfg.content_lang = CL_DE; break;
    case CFG_LANGUAGE_IT: g_cfg.content_lang = CL_IT; break;
    case CFG_LANGUAGE_PT: g_cfg.content_lang = CL_PT; break;
    default:              g_cfg.content_lang = CL_ALL; break;
    }
    g_cfg.default_sort = 0;
    g_cfg.nsfw = NSFW_BLUR;
    g_cfg.images = IMAGES_ALL;
    g_cfg.text_size = TEXT_NORMAL;
    g_cfg.source = SRC_AUTO;
    snprintf(g_cfg.redlib, sizeof(g_cfg.redlib), "%s", REDDIT_DEFAULT_INSTANCES);
}

void settings_load(void)
{
    defaults();
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) {
        settings_save();
        return;
    }
    bool has_start = false;
    char line[1200];
    while (fgets(line, sizeof(line), f)) {
        char *l = trim(line);
        if (!*l || *l == '#' || *l == ';') continue;
        char *eq = strchr(l, '=');
        if (!eq) continue;
        *eq = '\0';
        char *hash = strchr(eq + 1, '#'); /* commentaire en fin de ligne */
        if (hash && (hash == eq + 1 || isspace((unsigned char)hash[-1]))) *hash = '\0';
        char *k = trim(l), *v = trim(eq + 1);

        if (!strcmp(k, "client_id")) snprintf(g_cfg.client_id, sizeof(g_cfg.client_id), "%s", v);
        else if (!strcmp(k, "video_quality")) g_cfg.video_q = find_key(v, VQ_KEYS, VIDEO_QUALITIES, VQ_AUTO);
        else if (!strcmp(k, "video_mode")) g_cfg.video_mode = find_key(v, VM_KEYS, VIDEO_MODES, VM_DIRECT);
        else if (!strcmp(k, "image_quality")) g_cfg.img_quality = find_key(v, IQ_KEYS, IMG_QUALITIES, IQ_NORMAL);
        else if (!strcmp(k, "comments_per_page")) g_cfg.com_batch = find_key(v, CB_KEYS, COM_BATCHES, 0);
        else if (!strcmp(k, "translate_to")) g_cfg.tr_lang = find_key(v, TL_KEYS, TL_LANGS, g_cfg.tr_lang);
        else if (!strcmp(k, "recent")) snprintf(g_cfg.recent, sizeof(g_cfg.recent), "%s", v);
        else if (!strcmp(k, "redlib") && *v) snprintf(g_cfg.redlib, sizeof(g_cfg.redlib), "%s", v);
        else if (!strcmp(k, "source")) g_cfg.source = find_key(v, SRC_KEYS, SRC_MODES, SRC_AUTO);
        else if (!strcmp(k, "ui_lang")) g_cfg.ui_lang = find_key(v, LANG_KEYS, UI_LANGS, g_cfg.ui_lang);
        else if (!strcmp(k, "content_lang")) g_cfg.content_lang = find_key(v, CL_KEYS, CONTENT_LANGS, g_cfg.content_lang);
        else if (!strcmp(k, "sort")) g_cfg.default_sort = find_key(v, SORT_KEYS, 4, 0);
        else if (!strcmp(k, "nsfw")) g_cfg.nsfw = find_key(v, NSFW_KEYS, NSFW_MODES, NSFW_BLUR);
        else if (!strcmp(k, "images")) g_cfg.images = find_key(v, IMG_KEYS, IMAGES_MODES, IMAGES_ALL);
        else if (!strcmp(k, "text_size")) g_cfg.text_size = find_key(v, TEXT_KEYS, TEXT_SIZES, TEXT_NORMAL);
        else if (!strcmp(k, "start_sub") || !strcmp(k, "default_sub")) {
            /* ancienne clé default_sub=popular => fil d'accueil */
            if (!strcmp(k, "start_sub") || !has_start) {
                snprintf(g_cfg.start_sub, sizeof(g_cfg.start_sub), "%s",
                         !strcasecmp(v, "popular") ? "" : v);
                has_start = !strcmp(k, "start_sub");
            }
        }
    }
    fclose(f);
}

bool settings_save(void)
{
    mkdir("sdmc:/3ds", 0777);
    mkdir(CONFIG_DIR, 0777);
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) return false;
    fprintf(f,
            "# Configuration de Reddit3DS (modifiable aussi depuis l'appli : SELECT)\n"
            "\n"
            "# Langue de l'interface : fr, en\n"
            "ui_lang=%s\n"
            "# Langue du fil d'accueil : all, fr, en, es, de, it, pt\n"
            "content_lang=%s\n"
            "# Subreddit au démarrage (vide = fil d'accueil)\n"
            "start_sub=%s\n"
            "# Tri par défaut : hot, new, top, rising\n"
            "sort=%s\n"
            "# Contenu NSFW : hide, blur, show\n"
            "nsfw=%s\n"
            "# Images : all, light, none\n"
            "images=%s\n"
            "# Taille du texte : small, normal, large\n"
            "text_size=%s\n"
            "# Traduire les posts et commentaires vers : fr, en, es, de, it, pt\n"
            "translate_to=%s\n"
            "# Qualité vidéo maximale : auto (360p New 3DS, 270p Old 3DS), 220, 270, 360\n"
            "video_quality=%s\n"
            "# Lecture vidéo : direct (en flux), preload (démarre quand la vidéo est entièrement chargée)\n"
            "video_mode=%s\n"
            "# Qualité des images : normal, high, max (décodage plus précis, aperçus plus grands, un peu plus lent)\n"
            "image_quality=%s\n"
            "# Commentaires affichés à la fois : 4, 8, 12, 20, all\n"
            "comments_per_page=%s\n"
            "\n"
            "# Source des données : auto (Redlib puis archive), redlib, archive\n"
            "source=%s\n"
            "# Instances Redlib essayées dans l'ordre (https://github.com/redlib-org/redlib-instances)\n"
            "redlib=%s\n"
            "# Subreddits récents (onglet Subs)\n"
            "recent=%s\n"
            "# Facultatif : client_id d'une app Reddit \"installed app\" (API officielle)\n"
            "client_id=%s\n",
            LANG_KEYS[g_cfg.ui_lang], CL_KEYS[g_cfg.content_lang], g_cfg.start_sub,
            SORT_KEYS[g_cfg.default_sort], NSFW_KEYS[g_cfg.nsfw], IMG_KEYS[g_cfg.images],
            TEXT_KEYS[g_cfg.text_size], TL_KEYS[g_cfg.tr_lang], VQ_KEYS[g_cfg.video_q], VM_KEYS[g_cfg.video_mode], IQ_KEYS[g_cfg.img_quality], CB_KEYS[g_cfg.com_batch], SRC_KEYS[g_cfg.source], g_cfg.redlib, g_cfg.recent, g_cfg.client_id);
    fclose(f);
    return true;
}

void settings_apply_source(void)
{
    reddit_configure(g_cfg.client_id, g_cfg.redlib, g_cfg.source);
}

const char *settings_home_feed(void)
{
    return CONTENT[g_cfg.content_lang].feed;
}

const char *settings_content_lang_name(int cl)
{
    if (cl == CL_ALL) return TR("Tout (r/popular)", "Everything (r/popular)");
    return CONTENT[cl].name;
}

const char *settings_content_lang_code(int cl)
{
    return CONTENT[cl].code;
}

int settings_recent(char out[][64], int max)
{
    int n = 0;
    char tmp[sizeof(g_cfg.recent)];
    snprintf(tmp, sizeof(tmp), "%s", g_cfg.recent);
    for (char *t = strtok(tmp, ","); t && n < max; t = strtok(NULL, ",")) {
        t = trim(t);
        if (*t) snprintf(out[n++], 64, "%s", t);
    }
    return n;
}

bool settings_add_recent(const char *sub)
{
    char list[10][64];
    int n = settings_recent(list, 10);
    if (n > 0 && !strcasecmp(list[0], sub)) return false; /* déjà en tête */
    char out[sizeof(g_cfg.recent)];
    snprintf(out, sizeof(out), "%s", sub);
    int kept = 1;
    for (int i = 0; i < n && kept < 8; i++) {
        if (!strcasecmp(list[i], sub)) continue;
        size_t l = strlen(out);
        snprintf(out + l, sizeof(out) - l, ",%s", list[i]);
        kept++;
    }
    snprintf(g_cfg.recent, sizeof(g_cfg.recent), "%s", out);
    return true;
}

const char *settings_tr_code(int tl)
{
    return TL_KEYS[tl];
}

const char *settings_tr_name(int tl)
{
    return TL_NAMES[tl];
}

int settings_comment_batch(void)
{
    return CB_VALUES[g_cfg.com_batch >= 0 && g_cfg.com_batch < COM_BATCHES ? g_cfg.com_batch : 0];
}

int settings_video_maxp(void)
{
    switch (g_cfg.video_q) {
    case VQ_220: return 220;
    case VQ_270: return 270;
    case VQ_360: return 360;
    default: {
        bool n3ds = false;
        APT_CheckNew3DS(&n3ds);
        return n3ds ? 360 : 270;
    }
    }
}
