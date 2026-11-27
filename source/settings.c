#include "settings.h"
#include "reddit.h"
#include "ui.h"

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
static const char *HOME_KEYS[] = { "suggestions", "subs", "both" };
static const char *SUG_KEYS[]  = { "lang", "gaming", "nintendo", "tech", "science", "news", "fun", "off" };
static const char *DENS_KEYS[]  = { "normal", "compact", "large" };
static const char *THUMB_KEYS[] = { "right", "left" };
static const char *BTN_KEYS[]   = { "standard", "swap_ab", "swap_abxy" };
static const char *LR_KEYS[]    = { "sort", "page" };
static const char *S3D_KEYS[]   = { "off", "zoom", "volume", "text" };

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
    g_cfg.sfx = 1;
    snprintf(g_cfg.music, sizeof(g_cfg.music), "off");
    g_cfg.music_vol = 1;
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
    char line[1600];
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
        else if (!strcmp(k, "subs")) snprintf(g_cfg.subs, sizeof(g_cfg.subs), "%s", v);
        else if (!strcmp(k, "theme")) {
            for (int t = 0; t < THEMES; t++)
                if (!strcmp(v, ui_theme_key(t))) g_cfg.theme = t;
        }
        else if (!strcmp(k, "density")) g_cfg.density = find_key(v, DENS_KEYS, DENSITIES, DENS_NORMAL);
        else if (!strcmp(k, "thumbnails")) g_cfg.thumb_left = find_key(v, THUMB_KEYS, 2, 0);
        else if (!strcmp(k, "buttons")) g_cfg.buttons = find_key(v, BTN_KEYS, BTN_LAYOUTS, BTN_STD);
        else if (!strcmp(k, "lr")) g_cfg.lr_mode = find_key(v, LR_KEYS, LR_MODES, LR_SORT);
        else if (!strcmp(k, "slider_3d")) g_cfg.slider = find_key(v, S3D_KEYS, S3D_MODES, S3D_OFF);
        else if (!strcmp(k, "sounds")) g_cfg.sfx = strcmp(v, "off") != 0;
        else if (!strcmp(k, "sound_pack")) snprintf(g_cfg.sfx_pack, sizeof(g_cfg.sfx_pack), "%s", strcmp(v, "default") ? v : "");
        else if (!strcmp(k, "music"))
            snprintf(g_cfg.music, sizeof(g_cfg.music), "%s", !strcmp(v, "default") ? "" : !strcmp(v, "random") ? "*" : v);
        else if (!strcmp(k, "music_volume")) {
            int p = atoi(v);
            g_cfg.music_vol = p <= 25 ? 0 : p <= 50 ? 1 : p <= 75 ? 2 : 3;
        }
        else if (!strcmp(k, "home_feed")) g_cfg.home_mode = find_key(v, HOME_KEYS, HOME_MODES, HOME_SUGGEST);
        else if (!strcmp(k, "suggestions")) g_cfg.sug_theme = find_key(v, SUG_KEYS, SUG_THEMES, SUG_LANG);
        else if (!strcmp(k, "redlib") && *v) snprintf(g_cfg.redlib, sizeof(g_cfg.redlib), "%s", v);
        else if (!strcmp(k, "redlib_server")) snprintf(g_cfg.redlib_server, sizeof(g_cfg.redlib_server), "%s", v);
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
    /* ancienne liste par défaut (6 serveurs sur 8 morts ou bloqués) : remplacée par la nouvelle */
    if (!strcmp(g_cfg.redlib, REDDIT_OLD_DEFAULT_INSTANCES)) {
        snprintf(g_cfg.redlib, sizeof(g_cfg.redlib), "%s", REDDIT_DEFAULT_INSTANCES);
        settings_save();
    }
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
            "# Thème : dark, light, black, navy, 3ds, 3ds_red, 3ds_aqua, 3ds_pink, 3ds_purple, 3ds_green, 3ds_white,\n"
            "#         3ds_black, 3ds_settings, aero, aero_dark, retro_gb, retro_term\n"
            "theme=%s\n"
            "# Disposition du fil : normal, compact, large ; miniatures : right, left\n"
            "density=%s\n"
            "thumbnails=%s\n"
            "# Boutons : standard, swap_ab (A/B inversés), swap_abxy (A/B et X/Y inversés)\n"
            "buttons=%s\n"
            "# L / R dans le fil : sort (changer le tri), page (défiler)\n"
            "lr=%s\n"
            "# Curseur 3D : off, zoom (images et vidéos), volume (vidéos), text (taille du texte)\n"
            "slider_3d=%s\n"
            "# Sons de l'interface : on, off ; pack : default ou un dossier de sdmc:/3ds/reddit3ds/sons\n"
            "sounds=%s\n"
            "sound_pack=%s\n"
            "# Musique : off, default, random (dossier musique) ou un fichier du dossier musique ; volume : 25 à 100\n"
            "music=%s\n"
            "music_volume=%d\n"
            "# Fil d'accueil : suggestions, subs (abonnements), both (les deux)\n"
            "home_feed=%s\n"
            "# Suggestions (accueil et onglet Subs) : lang, gaming, nintendo, tech, science, news, fun, off\n"
            "suggestions=%s\n"
            "# Abonnements locaux (subreddits séparés par des virgules)\n"
            "subs=%s\n"
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
            "# Serveur Redlib imposé (vide = automatique : la liste ci-dessus, dans l'ordre)\n"
            "redlib_server=%s\n"
            "# Subreddits récents (onglet Subs)\n"
            "recent=%s\n"
            "# Facultatif : client_id d'une app Reddit \"installed app\" (API officielle)\n"
            "client_id=%s\n",
            LANG_KEYS[g_cfg.ui_lang], CL_KEYS[g_cfg.content_lang],
            ui_theme_key(g_cfg.theme), DENS_KEYS[g_cfg.density], THUMB_KEYS[g_cfg.thumb_left], BTN_KEYS[g_cfg.buttons],
            LR_KEYS[g_cfg.lr_mode], S3D_KEYS[g_cfg.slider],
            g_cfg.sfx ? "on" : "off", g_cfg.sfx_pack[0] ? g_cfg.sfx_pack : "default",
            !strcmp(g_cfg.music, "*") ? "random" : g_cfg.music[0] ? g_cfg.music : "default", (g_cfg.music_vol + 1) * 25,
            HOME_KEYS[g_cfg.home_mode], SUG_KEYS[g_cfg.sug_theme], g_cfg.subs, g_cfg.start_sub,
            SORT_KEYS[g_cfg.default_sort], NSFW_KEYS[g_cfg.nsfw], IMG_KEYS[g_cfg.images],
            TEXT_KEYS[g_cfg.text_size], TL_KEYS[g_cfg.tr_lang], VQ_KEYS[g_cfg.video_q], VM_KEYS[g_cfg.video_mode], IQ_KEYS[g_cfg.img_quality], CB_KEYS[g_cfg.com_batch], SRC_KEYS[g_cfg.source], g_cfg.redlib, g_cfg.redlib_server, g_cfg.recent, g_cfg.client_id);
    fclose(f);
    return true;
}

void settings_apply_source(void)
{
    reddit_configure(g_cfg.client_id, g_cfg.redlib, g_cfg.source, g_cfg.redlib_server);
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

/* ------------------------------------------------------------------ */
/* Suggestions, abonnements et fil d'accueil                          */

/* Suggestions « langue du contenu » */
static const char *SUG_LANG_LIST[CONTENT_LANGS][NSUGGEST] = {
    [CL_ALL] = { "AskReddit", "worldnews", "gaming", "pics", "movies", "todayilearned", "3DS", "Nintendo" },
    [CL_FR]  = { "france", "AskFrance", "jeuxvideo", "Quebec", "belgium", "programmation", "3DS", "Nintendo" },
    [CL_EN]  = { "AskReddit", "worldnews", "gaming", "todayilearned", "technology", "movies", "3DS", "Nintendo" },
    [CL_ES]  = { "es", "spain", "mexico", "argentina", "chile", "colombia", "3DS", "Nintendo" },
    [CL_DE]  = { "de", "germany", "Austria", "Switzerland", "Finanzen", "FragReddit", "3DS", "Nintendo" },
    [CL_IT]  = { "italy", "Italia", "italygames", "ItaliaPersonalFinance", "ItalyInformatica", "3DS", "Nintendo", "gaming" },
    [CL_PT]  = { "brasil", "portugal", "desabafos", "futebol", "investimentos", "3DS", "Nintendo", "gaming" },
};

/* Suggestions par thème (index SUG_GAMING..SUG_FUN) */
static const char *SUG_THEME_LIST[SUG_OFF][NSUGGEST] = {
    [SUG_GAMING]   = { "gaming", "Games", "pcgaming", "NintendoSwitch", "PS5", "XboxSeriesX", "Steam", "retrogaming" },
    [SUG_NINTENDO] = { "Nintendo", "3DS", "NintendoSwitch", "pokemon", "zelda", "Mario", "3dshacks", "SmashBrosUltimate" },
    [SUG_TECH]     = { "technology", "programming", "linux", "Android", "apple", "pcmasterrace", "hardware", "homelab" },
    [SUG_SCIENCE]  = { "science", "space", "askscience", "Physics", "Futurology", "dataisbeautiful", "biology", "chemistry" },
    [SUG_NEWS]     = { "worldnews", "news", "europe", "UpliftingNews", "geopolitics", "economics", "environment", "technews" },
    [SUG_FUN]      = { "funny", "memes", "ProgrammerHumor", "me_irl", "wholesomememes", "Jokes", "comics", "mildlyinteresting" },
};

const char *settings_sug_name(int theme)
{
    switch (theme) {
    case SUG_LANG:     return TR("Selon la langue", "By language");
    case SUG_GAMING:   return TR("Jeux vidéo", "Video games");
    case SUG_NINTENDO: return "Nintendo / 3DS";
    case SUG_TECH:     return TR("Tech / info", "Tech");
    case SUG_SCIENCE:  return "Science";
    case SUG_NEWS:     return TR("Actualités", "News");
    case SUG_FUN:      return TR("Humour", "Humour");
    default:           return TR("Désactivées", "Off");
    }
}

const char *settings_home_mode_name(int m)
{
    switch (m) {
    case HOME_SUBS: return TR("Abonnements", "Subscriptions");
    case HOME_BOTH: return TR("Abonnements + suggestions", "Subscriptions + suggestions");
    default:        return TR("Suggestions", "Suggestions");
    }
}

int settings_suggestions(const char *out[], int max)
{
    int t = g_cfg.sug_theme;
    if (t < 0 || t >= SUG_OFF) return 0;
    const char *const *list = t == SUG_LANG ? SUG_LANG_LIST[g_cfg.content_lang] : SUG_THEME_LIST[t];
    int n = 0;
    for (int i = 0; i < NSUGGEST && n < max; i++)
        if (list[i]) out[n++] = list[i];
    return n;
}

int settings_subs(char out[][64], int max)
{
    int n = 0;
    char tmp[sizeof(g_cfg.subs)];
    snprintf(tmp, sizeof(tmp), "%s", g_cfg.subs);
    for (char *t = strtok(tmp, ","); t && n < max; t = strtok(NULL, ",")) {
        t = trim(t);
        if (*t) snprintf(out[n++], 64, "%s", t);
    }
    return n;
}

bool settings_is_sub(const char *sub)
{
    if (!sub || !*sub) return false;
    char list[MAX_SUBS][64];
    int n = settings_subs(list, MAX_SUBS);
    for (int i = 0; i < n; i++)
        if (!strcasecmp(list[i], sub)) return true;
    return false;
}

bool settings_toggle_sub(const char *sub)
{
    if (!sub || !*sub || strchr(sub, '+')) return false;
    char list[MAX_SUBS][64];
    int n = settings_subs(list, MAX_SUBS);
    bool found = false;
    char out[sizeof(g_cfg.subs)] = "";
    for (int i = 0; i < n; i++) {
        if (!strcasecmp(list[i], sub)) { found = true; continue; } /* désabonnement */
        size_t l = strlen(out);
        snprintf(out + l, sizeof(out) - l, "%s%.63s", l ? "," : "", list[i]);
    }
    if (!found) {
        if (n >= MAX_SUBS || strlen(out) + strlen(sub) + 2 > sizeof(out)) return false; /* liste pleine */
        size_t l = strlen(out);
        snprintf(out + l, sizeof(out) - l, "%s%s", l ? "," : "", sub);
    }
    snprintf(g_cfg.subs, sizeof(g_cfg.subs), "%s", out);
    return !found;
}

/* Ajoute "a+b+c" à buf tant que ça tient (la requête est limitée à ~250 caractères) */
static void feed_add(char *buf, size_t n, const char *sub)
{
    size_t l = strlen(buf), sl = strlen(sub);
    if (!sl || l + sl + 2 > n) return;
    /* pas de doublon */
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", buf);
    char *sv = NULL;
    for (char *t = strtok_r(tmp, "+", &sv); t; t = strtok_r(NULL, "+", &sv))
        if (!strcasecmp(t, sub)) return;
    snprintf(buf + l, n - l, "%s%s", l ? "+" : "", sub);
}

const char *settings_home_feed(void)
{
    static char buf[250];
    buf[0] = '\0';
    if (g_cfg.home_mode == HOME_SUBS || g_cfg.home_mode == HOME_BOTH) {
        char list[MAX_SUBS][64];
        int n = settings_subs(list, MAX_SUBS);
        for (int i = 0; i < n; i++) feed_add(buf, sizeof(buf), list[i]);
        if (g_cfg.home_mode == HOME_SUBS && buf[0]) return buf;
    }
    /* suggestions : thème choisi, ou les grands subreddits de la langue du contenu */
    if (g_cfg.sug_theme > SUG_LANG && g_cfg.sug_theme < SUG_OFF) {
        const char *s[NSUGGEST];
        int n = settings_suggestions(s, NSUGGEST);
        for (int i = 0; i < n; i++) feed_add(buf, sizeof(buf), s[i]);
        return buf;
    }
    if (!buf[0]) return CONTENT[g_cfg.content_lang].feed;
    /* abonnements + fil de la langue */
    char lang[200];
    snprintf(lang, sizeof(lang), "%s", CONTENT[g_cfg.content_lang].feed);
    if (!strcmp(lang, "popular")) return buf; /* r/popular ne se combine pas */
    char *sv = NULL;
    for (char *t = strtok_r(lang, "+", &sv); t; t = strtok_r(NULL, "+", &sv)) feed_add(buf, sizeof(buf), t);
    return buf;
}
