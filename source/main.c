/*
 * Reddit3DS — client Reddit en lecture seule pour Nintendo 3DS.
 *
 * Écrans :
 *   Fil        haut : cartes des posts (miniatures)     bas : aperçu + barre d'actions
 *   Post       haut : titre, image, texte                bas : commentaires (tactile)
 *   Image      haut : visionneuse zoomable               bas : commandes
 *   Recherche  haut : subreddits trouvés                 bas : détails
 *   Paramètres haut : liste des réglages                 bas : description + boutons
 *
 * Le réseau tourne dans des threads (job.c, image.c) : l'interface reste animée.
 */
#include <3ds.h>
#include <citro2d.h>

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "image.h"
#include "job.h"
#include "net.h"
#include "reddit.h"
#include "settings.h"
#include "ui.h"
#include "video.h"

u32 __stacksize__ = 0x40000;

#define APP_VERSION "2.1"

#define TOP_W   400.0f
#define BOT_W   320.0f
#define SCR_H   240.0f
#define HEAD_H  26.0f
#define TOOL_H  36.0f
#define TOOL_Y  (SCR_H - TOOL_H)

#define CARD_X  6.0f
#define CARD_W  388.0f
#define PAD     8.0f
#define THUMB   64.0f
#define BODY_IMG_H 168.0f

typedef enum { SCR_FEED, SCR_POST, SCR_IMAGE, SCR_SEARCH, SCR_SETTINGS, SCR_VIDEO } Screen;

typedef enum {
    ACT_NONE, ACT_SEARCH, ACT_SORT_NEXT, ACT_SORT_PREV, ACT_REFRESH, ACT_OPEN, ACT_BACK,
    ACT_VIEW, ACT_ZOOM_IN, ACT_ZOOM_OUT, ACT_ZOOM_RESET, ACT_SETTINGS,
    ACT_SET_PREV, ACT_SET_NEXT, ACT_SET_ACTIVATE, ACT_TRANSLATE, ACT_VIDEO_PLAY, ACT_VOL_UP, ACT_VOL_DOWN,
} Action;

static const char *SORTS[] = { "hot", "new", "top", "rising" };
#define NSORTS 4

static const char *sort_label(int s)
{
    switch (s) {
    case 0:  return TR("Populaires", "Hot");
    case 1:  return TR("Nouveaux", "New");
    case 2:  return TR("Top 24 h", "Top 24 h");
    default: return TR("En hausse", "Rising");
    }
}

static C3D_RenderTarget *g_top, *g_bot;
static Screen g_screen = SCR_FEED;
static Action g_act = ACT_NONE;
static bool   g_online;

static char g_sub[64]; /* "" = fil d'accueil (selon la langue du contenu) */
static int  g_sort;
static char g_source[96];

/* Requêtes */
static Job  g_pending, g_running;
static bool g_has_pending;

/* Fil */
typedef struct { Doc title; float y, h; } Card;
static Feed  g_feed;
static Card *g_cards;
static int   g_ncards;
static float g_feed_h, g_scroll, g_scroll_to;
static int   g_sel, g_sel_stable;
static Doc   g_info, g_status_doc;
static int   g_info_for = -2;
static char  g_status[256];
static u32   g_more_retry; /* frame avant laquelle on ne relance pas « charger plus » */

/* Post */
static int   g_post;
static char  g_post_id[16];
static char  g_reveal_id[16]; /* post NSFW dont l'image a été dévoilée */
static Comments g_comments;
static char  g_com_err[256];
static Doc   g_body, g_com;
static float g_body_img_y = -1, g_body_scroll, g_body_to;
static float g_com_scroll, g_com_to, g_com_vel;
static int   g_com_sel;
static int  *g_com_depth;

/* Image */
static char  g_view_url[600];
static float g_zoom = 1, g_pan_x, g_pan_y;
static Screen g_view_back = SCR_POST;
static Screen g_video_back = SCR_POST;
/* Recherche (onglet Subs) */
enum { SC_GOTO, SC_RESULT, SC_RECENT, SC_SUGGEST };
typedef struct { Doc desc; float y, h; int kind, idx; const char *section; } SubCard;
static char   g_query[96];
static char   g_last_q[96];   /* dernière requête envoyée */
static bool   g_q_dirty;      /* saisie modifiée, recherche automatique en attente */
static u32    g_q_changed;
static SubList g_subs;
static SubCard *g_scards;
static int    g_nscards;
static float  g_s_scroll, g_s_to, g_s_h;
static int    g_s_sel;
static char   g_s_err[256];
static char   g_recent[10][64];
static int    g_nrecent;

/* Paramètres */
enum { SET_UI_LANG, SET_CONTENT, SET_SORT, SET_START, SET_NSFW, SET_IMAGES, SET_TEXT, SET_TRANSLATE,
       SET_VIDEO, SET_VMODE, SET_SOURCE, SET_CLEAR, SET_COUNT };
#define SET_ROW_H 36.0f
static int    g_set_sel;
static float  g_set_scroll, g_set_to;
static Screen g_set_back = SCR_FEED;
static bool   g_set_dirty, g_set_reload;
static Doc    g_set_help;
static int    g_set_help_for = -2;

/* ------------------------------------------------------------------ */
/* Formatage                                                          */

static void fmt_num(char *o, size_t n, int v)
{
    int a = abs(v);
    if (a >= 1000000)    snprintf(o, n, "%.1fM", v / 1e6);
    else if (a >= 10000) snprintf(o, n, "%dk", v / 1000);
    else if (a >= 1000)  snprintf(o, n, "%.1fk", v / 1000.0);
    else                 snprintf(o, n, "%d", v);
}

static void fmt_age(char *o, size_t n, double created)
{
    long d = (long)((double)time(NULL) - created);
    if (created <= 0) { snprintf(o, n, "?"); return; }
    if (d < 0) d = 0;
    bool en = g_cfg.ui_lang == LANG_EN;
    if (d < 3600)              snprintf(o, n, "%ld min", d / 60);
    else if (d < 86400)        snprintf(o, n, "%ld h", d / 3600);
    else if (d < 86400L * 30)  snprintf(o, n, en ? "%ld d" : "%ld j", d / 86400);
    else if (d < 86400L * 365) snprintf(o, n, en ? "%ld mo" : "%ld mois", d / (86400L * 30));
    else                       snprintf(o, n, en ? "%ld y" : "%ld an%s", d / (86400L * 365),
                                        d >= 86400L * 730 ? "s" : "");
}

static void fmt_members(char *o, size_t n, int v)
{
    const char *m = TR("membres", "members");
    if (v >= 1000000)   snprintf(o, n, "%.1f M %s", v / 1e6, m);
    else if (v >= 1000) snprintf(o, n, "%.1f k %s", v / 1e3, m);
    else                snprintf(o, n, "%d %s", v, m);
}

static u32 depth_color(int depth)
{
    switch ((depth - 1) % 6) {
    case 0:  return COL_ACCENT;
    case 1:  return COL_LINK;
    case 2:  return COL_GREEN;
    case 3:  return COL_YELLOW;
    case 4:  return C2D_Color32(0xC5, 0x7B, 0xFF, 0xFF);
    default: return COL_RED;
    }
}

static void sanitize_sub(char *s)
{
    char *src = s;
    while (*src == ' ') src++;
    if (!strncasecmp(src, "/r/", 3)) src += 3;
    else if (!strncasecmp(src, "r/", 2)) src += 2;
    char *o = s;
    for (; *src; src++)
        if (isalnum((unsigned char)*src) || *src == '_' || *src == '+') *o++ = *src;
    *o = '\0';
}

static void smooth(float *v, float to)
{
    float d = to - *v;
    *v = fabsf(d) < 0.5f ? to : *v + d * 0.35f;
}

static void clamp_scroll(float *s, float content_h, float view_h)
{
    float max = content_h - view_h;
    if (max < 0) max = 0;
    if (*s > max) *s = max;
    if (*s < 0) *s = 0;
}

static void draw_scrollbar(float x, float top, float vh, float total, float scroll)
{
    if (total <= vh + 1) return;
    float h = vh * vh / total;
    if (h < 14) h = 14;
    float y = top + (vh - h) * (scroll / (total - vh));
    ui_rrect(x, y, 3, h, 1.5f, COL_LINE);
}

/* Fil réellement demandé : subreddit choisi, ou fil d'accueil de la langue du contenu */
static const char *feed_target(void)
{
    return g_sub[0] ? g_sub : settings_home_feed();
}

static void feed_title(char *o, size_t n)
{
    if (g_sub[0]) {
        snprintf(o, n, "r/%s", g_sub);
    } else {
        const char *code = settings_content_lang_code(g_cfg.content_lang);
        snprintf(o, n, "%s%s%s", TR("Accueil", "Home"), code[0] ? " · " : "", code);
    }
}

/* ------------------------------------------------------------------ */
/* Posts et images                                                    */

static Post *cur_post(void)
{
    return (g_post >= 0 && g_post < g_feed.n) ? &g_feed.items[g_post] : NULL;
}

static bool has_media(const Post *p)
{
    return p->thumb[0] || p->image[0];
}

static const char *post_media(const Post *p)
{
    return p->image[0] ? p->image : p->thumb;
}

/* Image masquée (NSFW flouté et pas encore dévoilé) ? */
static bool media_hidden(const Post *p)
{
    return p->nsfw && g_cfg.nsfw == NSFW_BLUR && strcmp(g_reveal_id, p->id);
}

/* Miniature à afficher dans la carte du fil (NULL = icône) */
static const char *card_thumb(const Post *p)
{
    if (g_cfg.images == IMAGES_NONE) return NULL;
    if (p->thumb[0]) return p->thumb;
    if (g_cfg.images == IMAGES_ALL) return p->image[0] ? p->image : NULL;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Requêtes asynchrones                                               */

static void submit(const Job *j)
{
    if (g_has_pending) job_free_inputs(&g_pending); /* demande remplacée avant d'avoir démarré */
    if (job_busy()) job_cancel(); /* la nouvelle demande remplace l'ancienne */
    g_pending = *j;
    g_has_pending = true;
}

/* Abandonne la demande en attente (sans la lancer) */
static void drop_pending(void)
{
    if (g_has_pending) job_free_inputs(&g_pending);
    g_has_pending = false;
}

static bool loading(JobType t, int more)
{
    if (g_has_pending && g_pending.type == t && (more < 0 || g_pending.more == more)) return true;
    return job_busy() && g_running.type == t && (more < 0 || g_running.more == more);
}

static void request_feed(bool more)
{
    Job j;
    memset(&j, 0, sizeof(j));
    j.type = JOB_FEED;
    snprintf(j.sub, sizeof(j.sub), "%s", feed_target());
    snprintf(j.sort, sizeof(j.sort), "%s", SORTS[g_sort]);
    j.more = more;
    if (more) {
        snprintf(j.after, sizeof(j.after), "%s", g_feed.after);
        j.last_created = g_feed.n ? g_feed.items[g_feed.n - 1].created : 0;
    }
    submit(&j);
}

static void request_comments(void)
{
    Post *p = cur_post();
    if (!p) return;
    Job j;
    memset(&j, 0, sizeof(j));
    j.type = JOB_COMMENTS;
    snprintf(j.post_id, sizeof(j.post_id), "%s", p->id);
    snprintf(j.post_sub, sizeof(j.post_sub), "%s", p->sub);
    snprintf(j.post_author, sizeof(j.post_author), "%s", p->author);
    submit(&j);
}

static void request_search(void)
{
    Job j;
    memset(&j, 0, sizeof(j));
    j.type = JOB_SEARCH;
    snprintf(j.query, sizeof(j.query), "%s", g_query);
    submit(&j);
}

/* ------------------------------------------------------------------ */
/* Fil : mise en page                                                 */

static void free_cards(void)
{
    for (int i = 0; i < g_ncards; i++) doc_free(&g_cards[i].title);
    free(g_cards);
    g_cards = NULL;
    g_ncards = 0;
}

static void build_cards(void)
{
    free_cards();
    g_cards = calloc(g_feed.n ? g_feed.n : 1, sizeof(Card));
    if (!g_cards) return;
    g_ncards = g_feed.n;
    float y = HEAD_H + 6;
    for (int i = 0; i < g_feed.n; i++) {
        const Post *p = &g_feed.items[i];
        Card *c = &g_cards[i];
        bool media = has_media(p);
        float tw = CARD_W - 2 * PAD - (media ? THUMB + PAD : 0);
        doc_text(&c->title, p->title, 0.5f, 0, tw, p->stickied ? COL_GREEN : COL_TEXT, 0);
        doc_truncate(&c->title, 4);
        float h = PAD + ui_line_height(0.4f) + 3 + c->title.h + 5 + ui_line_height(0.42f) + PAD;
        if (media && h < THUMB + 2 * PAD) h = THUMB + 2 * PAD;
        c->y = y;
        c->h = h;
        y += h + 6;
    }
    g_feed_h = y + (g_feed.after[0] ? 34 : 0) + 4;

    doc_free(&g_status_doc);
    if (g_status[0]) doc_text(&g_status_doc, g_status, 0.48f, 0, CARD_W - 2 * PAD, COL_TEXT, 0);
    g_info_for = -2;
}

static int feed_items(void)
{
    return g_feed.n + (g_feed.after[0] ? 1 : 0);
}

static void feed_ensure_visible(void)
{
    float top, bot;
    if (g_sel < g_ncards) {
        top = g_cards[g_sel].y;
        bot = top + g_cards[g_sel].h;
    } else {
        top = g_feed_h - 38;
        bot = g_feed_h - 4;
    }
    if (top - 6 < g_scroll_to + HEAD_H) g_scroll_to = top - 6 - HEAD_H;
    if (bot + 6 > g_scroll_to + SCR_H) {
        float s = bot + 6 - SCR_H;
        g_scroll_to = (top - 6 - HEAD_H < s) ? top - 6 - HEAD_H : s; /* carte trop haute : on montre le haut */
    }
    clamp_scroll(&g_scroll_to, g_feed_h, SCR_H);
}

static void feed_move(int d)
{
    int n = feed_items();
    if (n == 0) return;
    int s = g_sel + d;
    if (s < 0) s = 0;
    if (s >= n) s = n - 1;
    if (s != g_sel) {
        g_sel = s;
        g_sel_stable = 0;
    }
    feed_ensure_visible();
}

static void build_info_doc(void)
{
    if (g_info_for == g_sel) return;
    g_info_for = g_sel;
    doc_free(&g_info);
    if (g_sel < 0 || g_sel >= g_feed.n) return;

    const Post *p = &g_feed.items[g_sel];
    const float x = 0, w = BOT_W - 32;
    char line[200], age[24], score[16];
    fmt_age(age, sizeof(age), p->created);
    fmt_num(score, sizeof(score), p->score);

    doc_text(&g_info, p->title, 0.52f, x, w, COL_WHITE, 0);
    doc_truncate(&g_info, has_media(p) ? 2 : 4);
    doc_space(&g_info, 3);
    snprintf(line, sizeof(line), "u/%s · r/%s · %s", p->author, p->sub, age);
    doc_text(&g_info, line, 0.4f, x, w, COL_DIM, 0);
    snprintf(line, sizeof(line), TR("%s points · %d commentaires", "%s points · %d comments"),
             score, p->ncomments);
    doc_text(&g_info, line, 0.4f, x, w, COL_ACCENT2, 0);
    if (!has_media(p)) {
        if (!p->is_self && p->url[0]) {
            doc_space(&g_info, 4);
            doc_text(&g_info, p->url, 0.4f, x, w, COL_LINK, 0);
        }
        if (p->selftext[0]) {
            doc_space(&g_info, 5);
            doc_text(&g_info, p->selftext, 0.44f, x, w, COL_TEXT, 0);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Traduction                                                         */

/* Textes traduits du post affiché : [0] titre, [1] texte, [2+i] commentaire i */
static char **g_tr;
static int    g_tr_n, g_tr_got;   /* taille, nombre de résultats déjà récupérés */
static bool   g_tr_show;          /* afficher la traduction (sinon l'original) */
static char   g_tr_post[16];

static void clear_translation(void)
{
    for (int i = 0; i < g_tr_n; i++) free(g_tr[i]);
    free(g_tr);
    g_tr = NULL;
    g_tr_n = g_tr_got = 0;
    g_tr_show = false;
    g_tr_post[0] = '\0';
}

/* Texte à afficher : traduction si disponible et activée */
static const char *tr_text(int i, const char *orig)
{
    return (g_tr_show && i < g_tr_n && g_tr[i]) ? g_tr[i] : orig;
}

/* ------------------------------------------------------------------ */
/* Post : mise en page                                                */

static void build_body(bool keep)
{
    const Post *p = cur_post();
    doc_free(&g_body);
    if (!keep) g_body_scroll = g_body_to = 0;
    g_body_img_y = -1;
    if (!p) return;

    const float x = 10, w = TOP_W - 24;
    char line[200], age[24], score[16];
    fmt_age(age, sizeof(age), p->created);
    fmt_num(score, sizeof(score), p->score);

    doc_space(&g_body, 8);
    snprintf(line, sizeof(line), TR("u/%s · il y a %s", "u/%s · %s ago"), p->author, age);
    doc_text(&g_body, line, 0.42f, x, w, COL_DIM, 0);
    doc_space(&g_body, 2);
    doc_text(&g_body, tr_text(0, p->title), 0.62f, x, w, COL_WHITE, 0);
    doc_space(&g_body, 3);
    snprintf(line, sizeof(line), TR("%s points · %d commentaires%s%s%s", "%s points · %d comments%s%s%s"),
             score, p->ncomments, p->flair[0] ? " · " : "", p->flair, p->nsfw ? " · NSFW" : "");
    doc_text(&g_body, line, 0.42f, x, w, COL_ACCENT2, 0);
    doc_space(&g_body, 8);

    if (has_media(p) || p->video_id[0]) {
        g_body_img_y = g_body.h;
        doc_space(&g_body, BODY_IMG_H + 8);
    }
    if (p->is_video) {
        doc_text(&g_body, p->video_id[0] ? TR("Vidéo : " BTN_A " pour la lire avec le son.", "Video: press " BTN_A " to play it with sound.")
                                         : TR("Vidéo externe : lecture impossible sur 3DS.", "External video: can't be played on 3DS."),
                 0.42f, x, w, COL_DIM, 0);
        doc_space(&g_body, 6);
    } else if (p->is_gallery) {
        doc_text(&g_body, TR("Galerie : seule la première image est affichée.",
                             "Gallery: only the first image is shown."), 0.42f, x, w, COL_DIM, 0);
        doc_space(&g_body, 6);
    }
    if (!p->is_self && p->url[0] && !(has_media(p) && !strcmp(p->url, p->image))) {
        doc_text(&g_body, p->url, 0.42f, x, w, COL_LINK, 0);
        doc_space(&g_body, 8);
    }
    if (p->selftext[0]) doc_text(&g_body, tr_text(1, p->selftext), 0.5f, x, w, COL_TEXT, 0);
    doc_space(&g_body, 14);
    if (keep) clamp_scroll(&g_body_to, g_body.h, SCR_H - HEAD_H);
}

static void build_comments(bool keep)
{
    doc_free(&g_com);
    free(g_com_depth);
    g_com_depth = calloc(g_comments.n + 1, sizeof(int));
    if (!keep) {
        g_com_sel = 0;
        g_com_scroll = g_com_to = g_com_vel = 0;
    }

    doc_space(&g_com, 6);
    if (g_com_err[0]) {
        doc_text(&g_com, g_com_err, 0.45f, 14, BOT_W - 28, COL_RED, 0);
        return;
    }
    if (g_comments.n == 0 && !loading(JOB_COMMENTS, -1)) {
        doc_text(&g_com, TR("Aucun commentaire pour l'instant.", "No comments yet."), 0.45f, 14,
                 BOT_W - 28, COL_DIM, 0);
        return;
    }
    float head = ui_line_height(0.4f) + 6;
    for (int i = 0; i < g_comments.n; i++) {
        const Comment *c = &g_comments.items[i];
        int d = c->depth > 6 ? 6 : c->depth;
        float cx = 6 + d * 7;
        float tx = cx + 9;
        float w = BOT_W - 6 - tx - 8;
        if (g_com_depth) g_com_depth[i] = d;

        doc_item_begin(&g_com);
        if (c->more) {
            doc_space(&g_com, 2);
            doc_text(&g_com, c->body, 0.4f, tx, w, COL_DIM, 0);
            doc_space(&g_com, 2);
        } else {
            doc_space(&g_com, head);
            doc_text(&g_com, tr_text(2 + i, c->body), 0.45f, tx, w, COL_TEXT, 0);
            doc_space(&g_com, 3);
        }
        doc_item_end(&g_com);
        doc_space(&g_com, 4);
    }
    if (keep) {
        if (g_com_sel >= g_com.nitems) g_com_sel = g_com.nitems ? g_com.nitems - 1 : 0;
        clamp_scroll(&g_com_to, g_com.h, TOOL_Y);
    }
}

/* ------------------------------------------------------------------ */
/* Recherche : mise en page                                           */

#define SBAR_Y  (HEAD_H + 6)
#define SBAR_H  30.0f
#define LIST_Y  (SBAR_Y + SBAR_H + 8)
#define NSUGGEST 8

/* Suggestions quand la barre est vide, selon la langue du contenu */
static const char *SUGGEST[CONTENT_LANGS][NSUGGEST] = {
    [CL_ALL] = { "AskReddit", "worldnews", "gaming", "pics", "movies", "todayilearned", "3DS", "Nintendo" },
    [CL_FR]  = { "france", "AskFrance", "jeuxvideo", "Quebec", "belgium", "programmation", "3DS", "Nintendo" },
    [CL_EN]  = { "AskReddit", "worldnews", "gaming", "todayilearned", "technology", "movies", "3DS", "Nintendo" },
    [CL_ES]  = { "es", "spain", "mexico", "argentina", "chile", "colombia", "3DS", "Nintendo" },
    [CL_DE]  = { "de", "germany", "Austria", "Switzerland", "Finanzen", "FragReddit", "3DS", "Nintendo" },
    [CL_IT]  = { "italy", "Italia", "italygames", "ItaliaPersonalFinance", "ItalyInformatica", "3DS", "Nintendo", "gaming" },
    [CL_PT]  = { "brasil", "portugal", "desabafos", "futebol", "investimentos", "3DS", "Nintendo", "gaming" },
};

static void free_scards(void)
{
    for (int i = 0; i < g_nscards; i++) doc_free(&g_scards[i].desc);
    free(g_scards);
    g_scards = NULL;
    g_nscards = 0;
}

static const char *query_as_sub(void)
{
    static char q[96];
    snprintf(q, sizeof(q), "%s", g_query);
    sanitize_sub(q);
    return q;
}

static const char *scard_name(const SubCard *c)
{
    switch (c->kind) {
    case SC_GOTO:   return query_as_sub();
    case SC_RESULT: return g_subs.items[c->idx].name;
    case SC_RECENT: return g_recent[c->idx];
    default:        return SUGGEST[g_cfg.content_lang][c->idx];
    }
}

static void add_scard(float *y, int kind, int idx, const char *section)
{
    SubCard *c = &g_scards[g_nscards++];
    c->kind = kind;
    c->idx = idx;
    c->section = section;
    if (section) *y += 18;
    c->y = *y;
    if (kind == SC_RESULT) {
        doc_text(&c->desc, g_subs.items[idx].desc, 0.42f, 0, CARD_W - 2 * PAD - 48 - PAD, COL_DIM, 0);
        doc_truncate(&c->desc, 2);
        float h = PAD + ui_line_height(0.55f) + ui_line_height(0.4f) + 2 + c->desc.h + PAD;
        c->h = h < 48 + 2 * PAD ? 48 + 2 * PAD : h;
    } else {
        c->h = 34;
    }
    *y += c->h + 6;
}

static void build_search_cards(void)
{
    free_scards();
    g_nrecent = settings_recent(g_recent, 10);
    g_scards = calloc(g_nrecent + NSUGGEST + g_subs.n + 2, sizeof(SubCard));
    if (!g_scards) return;
    float y = LIST_Y;
    if (!g_query[0]) {
        for (int i = 0; i < g_nrecent; i++)
            add_scard(&y, SC_RECENT, i, i == 0 ? TR("Récents", "Recent") : NULL);
        for (int i = 0; i < NSUGGEST; i++)
            add_scard(&y, SC_SUGGEST, i, i == 0 ? TR("Suggestions", "Suggestions") : NULL);
    } else {
        if (query_as_sub()[0]) add_scard(&y, SC_GOTO, 0, NULL);
        for (int i = 0; i < g_subs.n; i++)
            add_scard(&y, SC_RESULT, i, i == 0 ? TR("Résultats", "Results") : NULL);
    }
    g_s_h = y + 30;
    if (g_s_sel >= g_nscards) g_s_sel = g_nscards ? g_nscards - 1 : 0;
}

static void search_ensure_visible(void)
{
    if (g_s_sel < 0 || g_s_sel >= g_nscards) return;
    const SubCard *c = &g_scards[g_s_sel];
    float top = c->y - (c->section ? 18 : 0) - 4, bot = c->y + c->h + 6;
    if (top - g_s_to < LIST_Y) g_s_to = top - LIST_Y;
    if (bot - g_s_to > SCR_H) g_s_to = bot - SCR_H;
    clamp_scroll(&g_s_to, g_s_h, SCR_H);
}

/* Lance la recherche pour le texte saisi */
static void search_now(void)
{
    g_q_dirty = false;
    if (strlen(g_query) < 2 || !strcmp(g_query, g_last_q)) return;
    snprintf(g_last_q, sizeof(g_last_q), "%s", g_query);
    sublist_clear(&g_subs);
    g_s_err[0] = '\0';
    g_s_sel = 0;
    g_s_scroll = g_s_to = 0;
    build_search_cards();
    request_search();
}

/* Le texte a changé (clavier) */
static void query_changed(void)
{
    g_q_dirty = true;
    g_q_changed = g_ui_frame;
    if (!g_query[0]) {
        if (loading(JOB_SEARCH, -1)) { job_cancel(); drop_pending(); }
        sublist_clear(&g_subs);
        g_last_q[0] = '\0';
        g_s_err[0] = '\0';
        g_q_dirty = false;
    }
    g_s_sel = 0;
    g_s_scroll = g_s_to = 0;
    build_search_cards();
}

/* ------------------------------------------------------------------ */
/* Clavier tactile (écran du bas)                                     */

#define KB_Y     58.0f
#define KB_ROW   36.0f
#define KB_KEYH  32.0f
#define KB_PITCH ((BOT_W - 8) / 10.0f)
enum { K_BS = 1, K_OK = 2 };

typedef struct { float x, y, w, h; int code; char label[16]; } Key;

static int kb_layout(Key *keys)
{
    const char *rows_fr[] = { "1234567890", "azertyuiop", "qsdfghjklm", "wxcvbn_" };
    const char *rows_en[] = { "1234567890", "qwertyuiop", "asdfghjkl_", "zxcvbnm" };
    const char **rows = g_cfg.ui_lang == LANG_EN ? rows_en : rows_fr;
    int n = 0;
    for (int r = 0; r < 4; r++) {
        int len = (int)strlen(rows[r]);
        float units = len + (r == 3 ? 2 : 0);
        float x = 4 + (10 - units) * KB_PITCH / 2;
        float y = KB_Y + r * KB_ROW;
        for (int i = 0; i < len; i++) {
            Key *k = &keys[n++];
            k->x = x + i * KB_PITCH; k->y = y; k->w = KB_PITCH - 3; k->h = KB_KEYH;
            k->code = rows[r][i];
            k->label[0] = rows[r][i];
            k->label[1] = '\0';
        }
        if (r == 3) {
            Key *k = &keys[n++];
            k->x = x + len * KB_PITCH; k->y = y; k->w = 2 * KB_PITCH - 3; k->h = KB_KEYH;
            k->code = K_BS;
            snprintf(k->label, sizeof(k->label), "%s", TR("Effacer", "Delete"));
        }
    }
    float y = KB_Y + 4 * KB_ROW;
    Key *sp = &keys[n++];
    sp->x = 4; sp->y = y; sp->w = 6 * KB_PITCH - 3; sp->h = KB_KEYH;
    sp->code = ' ';
    snprintf(sp->label, sizeof(sp->label), "%s", TR("espace", "space"));
    Key *ok = &keys[n++];
    ok->x = 4 + 6 * KB_PITCH; ok->y = y; ok->w = 4 * KB_PITCH - 3; ok->h = KB_KEYH;
    ok->code = K_OK;
    snprintf(ok->label, sizeof(ok->label), "%s", TR(BTN_Y " Chercher", BTN_Y " Search"));
    return n;
}

static void kb_type(int code)
{
    size_t len = strlen(g_query);
    if (code == K_BS) {
        if (len > 0) g_query[len - 1] = '\0';
        query_changed();
    } else if (code == K_OK) {
        g_last_q[0] = '\0'; /* force une nouvelle recherche */
        search_now();
    } else if (len < 48 && !(code == ' ' && (len == 0 || g_query[len - 1] == ' '))) {
        g_query[len] = (char)code;
        g_query[len + 1] = '\0';
        query_changed();
    }
}

/* Recalcule toutes les mises en page (langue ou taille du texte changée) */
static void rebuild_all(void)
{
    int sel = g_sel;
    float sc = g_scroll, st = g_scroll_to;
    build_cards();
    g_sel = sel < feed_items() ? sel : 0;
    g_scroll = sc;
    g_scroll_to = st;
    feed_ensure_visible();
    if (cur_post()) {
        build_body(true);
        build_comments(true);
    }
    if (g_nscards) build_search_cards();
    g_info_for = g_set_help_for = -2;
}

/* ------------------------------------------------------------------ */
/* Paramètres                                                         */

static const float TEXT_SCALES[TEXT_SIZES] = { 0.9f, 1.0f, 1.12f };

static const char *set_label(int i)
{
    switch (i) {
    case SET_UI_LANG: return TR("Langue de l'interface", "Interface language");
    case SET_CONTENT: return TR("Langue du contenu", "Content language");
    case SET_SORT:    return TR("Tri par défaut", "Default sort");
    case SET_START:   return TR("Au démarrage", "On startup");
    case SET_NSFW:    return TR("Contenu NSFW", "NSFW content");
    case SET_IMAGES:  return TR("Images", "Images");
    case SET_TEXT:    return TR("Taille du texte", "Text size");
    case SET_TRANSLATE: return TR("Traduire vers", "Translate to");
    case SET_VIDEO: return TR("Qualité vidéo max", "Max video quality");
    case SET_VMODE: return TR("Lecture vidéo", "Video playback");
    case SET_SOURCE:  return TR("Source des données", "Data source");
    case SET_CLEAR:   return TR("Vider le cache d'images", "Clear image cache");
    }
    return "";
}

static void set_value(int i, char *o, size_t n)
{
    switch (i) {
    case SET_UI_LANG: snprintf(o, n, "%s", g_cfg.ui_lang == LANG_EN ? "English" : "Français"); break;
    case SET_CONTENT: snprintf(o, n, "%s", settings_content_lang_name(g_cfg.content_lang)); break;
    case SET_SORT:    snprintf(o, n, "%s", sort_label(g_cfg.default_sort)); break;
    case SET_START:
        if (g_cfg.start_sub[0]) snprintf(o, n, "r/%s", g_cfg.start_sub);
        else snprintf(o, n, "%s", TR("Fil d'accueil", "Home feed"));
        break;
    case SET_NSFW: {
        const char *v[] = { TR("Masquer", "Hide"), TR("Flouter", "Blur"), TR("Afficher", "Show") };
        snprintf(o, n, "%s", v[g_cfg.nsfw]);
        break;
    }
    case SET_IMAGES: {
        const char *v[] = { TR("Toutes", "All"), TR("Légères", "Light"), TR("Aucune", "None") };
        snprintf(o, n, "%s", v[g_cfg.images]);
        break;
    }
    case SET_TEXT: {
        const char *v[] = { TR("Petit", "Small"), TR("Normal", "Normal"), TR("Grand", "Large") };
        snprintf(o, n, "%s", v[g_cfg.text_size]);
        break;
    }
    case SET_TRANSLATE: snprintf(o, n, "%s", settings_tr_name(g_cfg.tr_lang)); break;
    case SET_VIDEO:
        if (g_cfg.video_q == VQ_AUTO) snprintf(o, n, TR("Auto (%dp)", "Auto (%dp)"), settings_video_maxp());
        else snprintf(o, n, "%dp", settings_video_maxp());
        break;
    case SET_VMODE:
        snprintf(o, n, "%s", g_cfg.video_mode == VM_PRELOAD ? TR("Préchargement", "Preload") : TR("Direct", "Direct"));
        break;
    case SET_SOURCE: {
        const char *v[] = { "Auto", "Redlib", TR("Archive", "Archive") };
        snprintf(o, n, "%s", v[g_cfg.source]);
        break;
    }
    case SET_CLEAR: snprintf(o, n, "%s", BTN_A); break;
    default: o[0] = '\0';
    }
}

static const char *set_help(int i)
{
    switch (i) {
    case SET_UI_LANG:
        return TR("Langue des menus et des messages de l'application.",
                  "Language of the menus and messages.");
    case SET_CONTENT:
        return TR("Le fil d'accueil rassemble les principaux subreddits de cette langue. "
                  "« Tout » affiche r/popular (surtout en anglais). Reddit ne propose pas de "
                  "traduction : les posts restent dans leur langue d'origine.",
                  "The home feed gathers the main subreddits in this language. "
                  "\"Everything\" shows r/popular (mostly English). Reddit offers no "
                  "translation: posts stay in their original language.");
    case SET_SORT:
        return TR("Tri utilisé au lancement et à l'ouverture d'un subreddit. "
                  "Change-le à tout moment avec L / R.",
                  "Sort used at launch and when opening a subreddit. Change it any time with L / R.");
    case SET_START:
        return TR("Ce qui s'affiche au lancement : le fil d'accueil, ou le subreddit ouvert en ce moment. "
                  "Ouvre d'abord le subreddit voulu, puis reviens ici et appuie sur " BTN_A ".",
                  "What opens at launch: the home feed, or the subreddit currently open. "
                  "Open the subreddit you want first, then come back here and press " BTN_A ".");
    case SET_NSFW:
        return TR("Masquer : les posts NSFW sont retirés du fil. Flouter : leurs images sont "
                  "cachées jusqu'à ce que tu appuies sur " BTN_A ". Afficher : aucun filtre.",
                  "Hide: NSFW posts are removed from the feed. Blur: their images stay hidden "
                  "until you press " BTN_A ". Show: no filter.");
    case SET_IMAGES:
        return TR("Toutes : miniatures partout (plus de données). Légères : seulement les petits "
                  "aperçus fournis par Reddit. Aucune : rien n'est téléchargé, sauf en plein écran.",
                  "All: thumbnails everywhere (more data). Light: only the small previews provided "
                  "by Reddit. None: nothing is downloaded except in full screen.");
    case SET_TEXT:
        return TR("Taille des textes dans toute l'application.", "Size of text throughout the app.");
    case SET_TRANSLATE:
        return TR("Langue utilisée par le bouton " BTN_Y " Traduire d'un post (titre, texte et commentaires). "
                  "La traduction passe par Google Traduction (sans compte), ou MyMemory en secours.",
                  "Language used by a post's " BTN_Y " Translate button (title, text and comments). "
                  "Translation goes through Google Translate (no account), with MyMemory as a fallback.");
    case SET_VIDEO:
        return TR("Qualité maximale des vidéos Reddit (avec le son). Auto : 360p sur New 3DS, 270p sur "
                  "Old 3DS (décodage plus lent). Le son nécessite sdmc:/3ds/dspfirm.cdc (outil DSP1).",
                  "Maximum quality for Reddit videos (with sound). Auto: 360p on New 3DS, 270p on Old 3DS "
                  "(slower decoding). Sound needs sdmc:/3ds/dspfirm.cdc (DSP1 tool).");
    case SET_VMODE:
        return TR("Direct : la vidéo démarre dès que possible et se charge pendant la lecture. "
                  "Préchargement : la vidéo (et le son) est d'abord entièrement téléchargée sur la carte SD, "
                  "puis lue sans aucune coupure. Le chargement prend plus de temps avant de démarrer.",
                  "Direct: the video starts as soon as possible and loads while playing. "
                  "Preload: the video (and sound) is first fully downloaded to the SD card, "
                  "then played without any interruption. It takes longer before it starts.");
    case SET_SOURCE:
        return TR("Auto : Redlib (en direct), puis l'archive Arctic Shift si Redlib ne répond pas. "
                  "Les instances Redlib se règlent dans config.ini.",
                  "Auto: Redlib (live), then the Arctic Shift archive if Redlib doesn't respond. "
                  "Redlib instances are set in config.ini.");
    case SET_CLEAR:
        return TR("Libère la mémoire utilisée par les images déjà chargées.",
                  "Frees the memory used by images already loaded.");
    }
    return "";
}

static int cycle(int v, int d, int n)
{
    return (v + d + n) % n;
}

static void set_change(int i, int d)
{
    switch (i) {
    case SET_UI_LANG:
        g_cfg.ui_lang = cycle(g_cfg.ui_lang, d, UI_LANGS);
        rebuild_all();
        break;
    case SET_CONTENT:
        g_cfg.content_lang = cycle(g_cfg.content_lang, d, CONTENT_LANGS);
        if (!g_sub[0]) g_set_reload = true;
        break;
    case SET_SORT:
        g_cfg.default_sort = cycle(g_cfg.default_sort, d, NSORTS);
        g_sort = g_cfg.default_sort;
        g_set_reload = true;
        break;
    case SET_START:
        if (d == 0 || !g_cfg.start_sub[0]) snprintf(g_cfg.start_sub, sizeof(g_cfg.start_sub), "%s", g_sub);
        else g_cfg.start_sub[0] = '\0';
        break;
    case SET_NSFW:
        g_cfg.nsfw = cycle(g_cfg.nsfw, d, NSFW_MODES);
        g_set_reload = true; /* « Masquer » filtre au chargement */
        break;
    case SET_IMAGES:
        g_cfg.images = cycle(g_cfg.images, d, IMAGES_MODES);
        break;
    case SET_TEXT:
        g_cfg.text_size = cycle(g_cfg.text_size, d, TEXT_SIZES);
        ui_set_text_scale(TEXT_SCALES[g_cfg.text_size]);
        rebuild_all();
        break;
    case SET_TRANSLATE:
        g_cfg.tr_lang = cycle(g_cfg.tr_lang, d, TL_LANGS);
        break;
    case SET_VIDEO:
        g_cfg.video_q = cycle(g_cfg.video_q, d, VIDEO_QUALITIES);
        break;
    case SET_VMODE:
        g_cfg.video_mode = cycle(g_cfg.video_mode, d, VIDEO_MODES);
        break;
    case SET_SOURCE:
        g_cfg.source = cycle(g_cfg.source, d, SRC_MODES);
        settings_apply_source();
        g_set_reload = true;
        break;
    case SET_CLEAR:
        if (g_online) img_drop_all();
        ui_toast(TR("Cache d'images vidé", "Image cache cleared"));
        return;
    }
    g_set_dirty = true;
    g_set_help_for = -2;
}

static void set_ensure_visible(void)
{
    float top = HEAD_H + 6 + g_set_sel * (SET_ROW_H + 4);
    float total = HEAD_H + 6 + SET_COUNT * (SET_ROW_H + 4) + 40;
    if (top - 6 < g_set_to + HEAD_H) g_set_to = top - 6 - HEAD_H;
    if (top + SET_ROW_H + 6 > g_set_to + SCR_H) g_set_to = top + SET_ROW_H + 6 - SCR_H;
    clamp_scroll(&g_set_to, total, SCR_H);
}

static void open_settings(void)
{
    g_set_back = g_screen == SCR_SETTINGS ? SCR_FEED : g_screen;
    g_set_dirty = g_set_reload = false;
    g_set_help_for = -2;
    g_screen = SCR_SETTINGS;
}

/* ------------------------------------------------------------------ */
/* Actions                                                            */

static void open_post(int idx)
{
    if (idx < 0 || idx >= g_feed.n) return;
    g_post = idx;
    snprintf(g_post_id, sizeof(g_post_id), "%s", g_feed.items[idx].id);
    comments_clear(&g_comments);
    g_com_err[0] = '\0';
    clear_translation();
    build_body(false);
    request_comments();
    build_comments(false);
    g_screen = SCR_POST;
}

/* Récupère les traductions déjà prêtes (pendant ou après le travail) */
static void tr_collect(Job *j)
{
    if (!g_tr || !j->out || strcmp(j->post_id, g_tr_post) || strcmp(g_tr_post, g_post_id)) return;
    int pg = j->progress;
    __sync_synchronize();
    if (pg > g_tr_n) pg = g_tr_n;
    if (pg <= g_tr_got) return;
    for (int i = g_tr_got; i < pg; i++)
        if (j->out[i] && !g_tr[i]) g_tr[i] = strdup(j->out[i]);
    g_tr_got = pg;
    if (g_tr_show) {
        build_body(true);
        build_comments(true);
    }
}

static void toggle_translation(void)
{
    Post *p = cur_post();
    if (!p) return;
    if (g_tr_n && !strcmp(g_tr_post, p->id)) {
        if (loading(JOB_TRANSLATE, -1)) {
            ui_toast(TR("Traduction en cours...", "Translating..."));
            return;
        }
        g_tr_show = !g_tr_show; /* bascule traduction / original */
        build_body(true);
        build_comments(true);
        ui_toast(g_tr_show ? TR("Traduction affichée", "Showing translation") : TR("Texte original", "Original text"));
        return;
    }
    if (loading(JOB_COMMENTS, -1)) {
        ui_toast(TR("Attends la fin du chargement des commentaires", "Wait for the comments to load"));
        return;
    }

    clear_translation();
    int n = 2 + g_comments.n;
    Job j;
    memset(&j, 0, sizeof(j));
    j.type = JOB_TRANSLATE;
    snprintf(j.post_id, sizeof(j.post_id), "%s", p->id);
    snprintf(j.tl, sizeof(j.tl), "%s", settings_tr_code(g_cfg.tr_lang));
    j.texts = calloc(n, sizeof(char *));
    j.out = calloc(n, sizeof(char *));
    g_tr = calloc(n, sizeof(char *));
    if (!j.texts || !j.out || !g_tr) {
        free(j.texts);
        free(j.out);
        free(g_tr);
        g_tr = NULL;
        return;
    }
    j.ntexts = n;
    j.texts[0] = strdup(p->title);
    j.texts[1] = strdup(p->selftext);
    for (int i = 0; i < g_comments.n; i++)
        j.texts[2 + i] = strdup(g_comments.items[i].more ? "" : g_comments.items[i].body);
    g_tr_n = n;
    g_tr_got = 0;
    g_tr_show = true;
    snprintf(g_tr_post, sizeof(g_tr_post), "%s", p->id);
    submit(&j);

    char msg[96];
    snprintf(msg, sizeof(msg), TR("Traduction en %s...", "Translating to %s..."), settings_tr_name(g_cfg.tr_lang));
    ui_toast(msg);
}

static void open_viewer(void)
{
    Post *p = cur_post();
    if (!p || !has_media(p)) return;
    if (media_hidden(p)) { /* premier appui : on dévoile l'image NSFW */
        snprintf(g_reveal_id, sizeof(g_reveal_id), "%s", p->id);
        return;
    }
    snprintf(g_view_url, sizeof(g_view_url), "%s", post_media(p));
    g_zoom = 1;
    g_pan_x = g_pan_y = 0;
    g_view_back = g_screen;
    g_screen = SCR_IMAGE;
}

static void close_viewer(void)
{
    g_screen = g_view_back;
    img_drop_kind(IMG_FULL);
}

static void open_video(void)
{
    Post *p = cur_post();
    if (!p || !p->video_id[0]) return;
    g_video_back = g_screen;
    g_screen = SCR_VIDEO;
    if (!video_open(p->video_id, settings_video_maxp(), g_cfg.video_mode == VM_PRELOAD))
        ui_toast(video_error());
}

static void close_video(void)
{
    video_close();
    g_screen = g_video_back;
}

static void open_search_tab(void)
{
    g_screen = SCR_SEARCH;
    build_search_cards();
    search_ensure_visible();
}

static void open_feed(const char *name) /* "" = fil d'accueil */
{
    char s[64];
    snprintf(s, sizeof(s), "%s", name);
    sanitize_sub(s);
    if (s[0] && settings_add_recent(s)) settings_save();
    snprintf(g_sub, sizeof(g_sub), "%s", s);
    feed_clear(&g_feed);
    g_status[0] = '\0';
    g_sel = 0;
    g_scroll = g_scroll_to = 0;
    build_cards();
    request_feed(false);
    g_screen = SCR_FEED;
}

static void change_sort(int d)
{
    g_sort = cycle(g_sort, d, NSORTS);
    char msg[64];
    snprintf(msg, sizeof(msg), TR("Tri : %s", "Sort: %s"), sort_label(g_sort));
    ui_toast(msg);
    request_feed(false);
}

static void viewer_zoom(float k)
{
    g_zoom *= k;
    if (g_zoom < 1) g_zoom = 1;
    if (g_zoom > 8) g_zoom = 8;
    g_pan_x *= k;
    g_pan_y *= k;
}

static void leave_settings(void)
{
    if (g_set_dirty && !settings_save())
        ui_toast(TR("Impossible d'enregistrer config.ini", "Couldn't save config.ini"));
    g_screen = g_set_back;
    if (g_set_reload) {
        g_set_reload = false;
        if (g_screen != SCR_FEED) g_screen = SCR_FEED;
        open_feed(g_sub);
    }
}

static void do_action(Action a)
{
    switch (a) {
    case ACT_SEARCH: open_search_tab(); break;
    case ACT_SORT_NEXT: change_sort(+1); break;
    case ACT_SORT_PREV: change_sort(-1); break;
    case ACT_SETTINGS: open_settings(); break;
    case ACT_REFRESH:
        if (g_screen == SCR_POST) {
            comments_clear(&g_comments);
            clear_translation();
            build_body(true);
            g_com_err[0] = '\0';
            request_comments();
            build_comments(false);
        } else {
            request_feed(false);
        }
        break;
    case ACT_OPEN:
        if (g_screen == SCR_FEED) {
            if (g_sel < g_feed.n) open_post(g_sel);
            else if (g_feed.after[0] && !loading(JOB_FEED, -1)) request_feed(true);
        } else if (g_screen == SCR_SEARCH) {
            if (g_s_sel >= 0 && g_s_sel < g_nscards) {
                char name[96];
                snprintf(name, sizeof(name), "%s", scard_name(&g_scards[g_s_sel]));
                if (name[0]) open_feed(name);
            }
        }
        break;
    case ACT_BACK:
        if (g_screen == SCR_VIDEO) {
            close_video();
        } else if (g_screen == SCR_IMAGE) {
            close_viewer();
        } else if (g_screen == SCR_SETTINGS) {
            leave_settings();
        } else if (g_screen == SCR_POST || g_screen == SCR_SEARCH) {
            if (loading(JOB_COMMENTS, -1) || loading(JOB_SEARCH, -1)) {
                job_cancel();
                drop_pending();
            }
            g_screen = SCR_FEED;
        } else if (job_busy() || g_has_pending) {
            job_cancel();
            drop_pending();
            ui_toast(TR("Chargement annulé", "Loading cancelled"));
        } else if (g_sub[0]) {
            open_feed(""); /* B sur un subreddit : retour au fil d'accueil */
        }
        break;
    case ACT_VIEW: {
        Post *p = cur_post();
        if (p && media_hidden(p)) snprintf(g_reveal_id, sizeof(g_reveal_id), "%s", p->id); /* NSFW : d'abord dévoiler */
        else if (p && p->video_id[0]) open_video();
        else open_viewer();
        break;
    }
    case ACT_VIDEO_PLAY:
        if (video_status() == VS_ERROR) video_restart();
        else video_toggle_pause();
        break;
    case ACT_VOL_UP: video_set_volume(video_volume() + 0.1f); break;
    case ACT_VOL_DOWN: video_set_volume(video_volume() - 0.1f); break;
    case ACT_TRANSLATE: toggle_translation(); break;
    case ACT_ZOOM_IN: viewer_zoom(1.5f); break;
    case ACT_ZOOM_OUT: viewer_zoom(1 / 1.5f); break;
    case ACT_ZOOM_RESET: g_zoom = 1; g_pan_x = g_pan_y = 0; break;
    case ACT_SET_PREV: set_change(g_set_sel, -1); break;
    case ACT_SET_NEXT: set_change(g_set_sel, +1); break;
    case ACT_SET_ACTIVATE: set_change(g_set_sel, g_set_sel == SET_START ? 0 : +1); break;
    default: break;
    }
}

/* ------------------------------------------------------------------ */
/* Résultats des requêtes                                             */

static void handle_job(Job *j)
{
    if (j->rc == 0) snprintf(g_source, sizeof(g_source), "%s", reddit_source());

    switch (j->type) {
    case JOB_FEED:
        if (strcmp(j->sub, feed_target())) break; /* on a changé de fil entre-temps */
        if (j->rc == 0) {
            if (g_cfg.nsfw == NSFW_HIDE)
                for (int i = j->feed.n - 1; i >= 0; i--)
                    if (j->feed.items[i].nsfw) feed_remove(&j->feed, i);
            g_status[0] = '\0';
            if (j->more) {
                feed_append(&g_feed, &j->feed);
            } else {
                feed_clear(&g_feed);
                g_feed = j->feed;
                memset(&j->feed, 0, sizeof(j->feed));
                g_sel = 0;
                g_sel_stable = 0;
                g_scroll = g_scroll_to = 0;
                img_drop_kind(IMG_PREVIEW);
            }
            build_cards();
        } else if (j->rc != -2) {
            if (j->more) g_more_retry = g_ui_frame + 300;
            if (g_feed.n == 0) {
                snprintf(g_status, sizeof(g_status), "%s", j->err);
                build_cards();
            } else {
                ui_toast(j->err);
            }
        }
        break;
    case JOB_COMMENTS:
        if (g_screen != SCR_POST && g_screen != SCR_IMAGE) break;
        if (strcmp(j->post_id, g_post_id)) break;
        comments_clear(&g_comments);
        if (j->rc == 0) {
            g_comments = j->comments;
            memset(&j->comments, 0, sizeof(j->comments));
            g_com_err[0] = '\0';
        } else {
            snprintf(g_com_err, sizeof(g_com_err), "%s", j->err);
        }
        if (g_tr_n) { clear_translation(); build_body(true); }
        build_comments(false);
        break;
    case JOB_TRANSLATE:
        tr_collect(j);
        if (j->rc != 0 && j->rc != -2 && !strcmp(j->post_id, g_post_id)) ui_toast(j->err);
        break;
    case JOB_SEARCH:
        if (strcmp(j->query, g_query)) break;
        sublist_clear(&g_subs);
        if (j->rc == 0) {
            g_subs = j->subs;
            memset(&j->subs, 0, sizeof(j->subs));
            if (g_cfg.nsfw == NSFW_HIDE) {
                int o = 0;
                for (int i = 0; i < g_subs.n; i++) {
                    if (g_subs.items[i].nsfw) {
                        free(g_subs.items[i].name);
                        free(g_subs.items[i].desc);
                        free(g_subs.items[i].icon);
                    } else {
                        g_subs.items[o++] = g_subs.items[i];
                    }
                }
                g_subs.n = o;
            }
            g_s_err[0] = '\0';
        } else {
            snprintf(g_s_err, sizeof(g_s_err), "%s", j->err);
        }
        build_search_cards();
        if (g_s_sel >= g_nscards) g_s_sel = 0;
        break;
    default:
        break;
    }
}

static void pump_jobs(void)
{
    Job *cur = job_current();
    if (cur && cur->type == JOB_TRANSLATE) tr_collect(cur); /* affichage progressif */
    Job *j = job_poll();
    if (j) {
        /* résultat remplacé par une demande plus récente du même type : on l'ignore */
        if (!g_has_pending || g_pending.type != j->type) handle_job(j);
        job_release();
    }
    if (g_has_pending && !job_busy() && !job_poll()) {
        if (job_start(&g_pending)) {
            g_running = g_pending;
            g_has_pending = false;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Rendu : éléments communs                                           */

static void draw_header(float w, const char *title, const char *right, bool busy)
{
    C2D_DrawRectSolid(0, 0, 0.5f, w, HEAD_H, COL_BAR);
    C2D_DrawRectSolid(0, HEAD_H - 1, 0.5f, w, 1, COL_LINE);
    ui_logo(14, HEAD_H / 2, 8);
    ui_text(28, (HEAD_H - ui_line_height(0.55f)) / 2, 0.55f, COL_WHITE, title);
    float rx = w - 8;
    if (right && *right) {
        float pw = ui_pill_width(right, 0.4f);
        rx -= pw;
        ui_pill(rx, (HEAD_H - ui_line_height(0.4f) - 2) / 2, right, 0.4f, COL_CARD_HI, COL_TEXT);
        rx -= 6;
    }
    if (busy) ui_spinner(rx - 9, HEAD_H / 2, 6, COL_ACCENT);
}

static void draw_toolbar_bg(void)
{
    C2D_DrawRectSolid(0, TOOL_Y, 0.5f, BOT_W, TOOL_H, COL_BAR);
    C2D_DrawRectSolid(0, TOOL_Y, 0.5f, BOT_W, 1, COL_LINE);
}

/*
 * Zone d'image. load : on peut lancer le téléchargement ; hidden : NSFW flouté ;
 * placeholder_hint : texte affiché quand l'image n'est pas chargée (mode « Aucune »).
 */
static void draw_media_box(const char *url, float x, float y, float w, float h, ImgKind kind,
                           bool load, bool video, bool hidden, const char *placeholder_hint)
{
    ui_rrect(x, y, w, h, 8, COL_BG);
    bool small = w < 80;
    if (hidden) {
        ui_rrect(x + 2, y + 2, w - 4, h - 4, 7, C2D_Color32(0x3A, 0x16, 0x18, 0xFF));
        ui_text_center(x + w / 2, y + h / 2 - (small ? 7 : 14), small ? 0.42f : 0.55f, COL_RED, "NSFW");
        if (!small) ui_text_center(x + w / 2, y + h / 2 + 4, 0.4f, COL_DIM,
                                   TR(BTN_A " pour afficher", BTN_A " to show"));
        return;
    }
    ImgState st = IMGS_LOADING;
    const C2D_Image *im = (load && url && *url) ? img_get(url, kind, &st) : NULL;
    if (im) {
        img_draw_fit(im, x + 2, y + 2, w - 4, h - 4, 1, 0, 0);
    } else if (load && st == IMGS_FAILED) {
        ui_text_center(x + w / 2, y + h / 2 - 6, 0.38f, COL_DIM, TR("image indisponible", "image unavailable"));
    } else if (load) {
        ui_spinner(x + w / 2, y + h / 2, small ? 8 : 12, COL_DIM);
    } else {
        /* icône « image » */
        float cx = x + w / 2, cy = y + h / 2, s = small ? 1.0f : 1.4f;
        C2D_DrawTriangle(cx - 18 * s, cy + 12 * s, COL_DIM, cx - 2 * s, cy - 10 * s, COL_DIM,
                         cx + 12 * s, cy + 12 * s, COL_DIM, 0.5f);
        C2D_DrawTriangle(cx + 2 * s, cy + 12 * s, COL_LINE, cx + 12 * s, cy - 2 * s, COL_LINE,
                         cx + 22 * s, cy + 12 * s, COL_LINE, 0.5f);
        C2D_DrawCircleSolid(cx + 14 * s, cy - 12 * s, 0.5f, 4 * s, COL_DIM);
        if (placeholder_hint && !small) ui_text_center(cx, y + h - 22, 0.4f, COL_DIM, placeholder_hint);
    }
    if (video && !hidden) {
        float r = small ? 10 : 18;
        C2D_DrawCircleSolid(x + w / 2, y + h / 2, 0.5f, r, COL_SCRIM);
        C2D_DrawTriangle(x + w / 2 - r * 0.35f, y + h / 2 - r * 0.5f, COL_WHITE,
                         x + w / 2 - r * 0.35f, y + h / 2 + r * 0.5f, COL_WHITE,
                         x + w / 2 + r * 0.55f, y + h / 2, COL_WHITE, 0.5f);
    }
}

/* ------------------------------------------------------------------ */
/* Rendu : fil                                                        */

static void draw_card(int i, float y)
{
    const Post *p = &g_feed.items[i];
    const Card *c = &g_cards[i];
    bool sel = (i == g_sel);
    if (sel) ui_rrect(CARD_X - 2, y - 2, CARD_W + 4, c->h + 4, 11, COL_ACCENT);
    ui_rrect(CARD_X, y, CARD_W, c->h, 9, sel ? COL_CARD_HI : COL_CARD);

    float tx = CARD_X + PAD, ty = y + PAD;
    char buf[160], age[24];
    snprintf(buf, sizeof(buf), "r/%s", p->sub);
    ui_text(tx, ty, 0.4f, COL_ACCENT2, buf);
    float sw = ui_text_width(buf, 0.4f);
    fmt_age(age, sizeof(age), p->created);
    snprintf(buf, sizeof(buf), " · u/%s · %s", p->author, age);
    ui_text(tx + sw, ty, 0.4f, COL_DIM, buf);
    ty += ui_line_height(0.4f) + 3;

    doc_draw(&c->title, tx, ty, CARD_W, SCR_H, 0, -1);
    ty += c->title.h + 5;

    char score[16], ncom[16];
    fmt_num(score, sizeof(score), p->score);
    fmt_num(ncom, sizeof(ncom), p->ncomments);
    float x = tx;
    ui_icon_up(x, ty + 2, 9, COL_ACCENT);
    ui_text(x + 12, ty, 0.42f, COL_TEXT, score);
    x += 12 + ui_text_width(score, 0.42f) + 10;
    ui_icon_comment(x, ty + 2, 10, COL_DIM);
    ui_text(x + 13, ty, 0.42f, COL_DIM, ncom);
    x += 13 + ui_text_width(ncom, 0.42f) + 10;

    float limit = CARD_X + CARD_W - PAD - (has_media(p) ? THUMB + PAD : 0);
    const char *vid = TR("Vidéo", "Video"), *gal = TR("Galerie", "Gallery");
    if (p->nsfw && x + ui_pill_width("NSFW", 0.38f) < limit)
        x += ui_pill(x, ty, "NSFW", 0.38f, COL_RED, COL_WHITE) + 4;
    if (p->is_video && x + ui_pill_width(vid, 0.38f) < limit)
        x += ui_pill(x, ty, vid, 0.38f, C2D_Color32(0x1E, 0x4E, 0x78, 0xFF), COL_WHITE) + 4;
    if (p->is_gallery && x + ui_pill_width(gal, 0.38f) < limit)
        x += ui_pill(x, ty, gal, 0.38f, C2D_Color32(0x2E, 0x5E, 0x3A, 0xFF), COL_WHITE) + 4;
    if (p->flair[0] && x + ui_pill_width(p->flair, 0.38f) < limit)
        x += ui_pill(x, ty, p->flair, 0.38f, COL_LINE, COL_TEXT) + 4;

    if (has_media(p)) {
        const char *t = card_thumb(p);
        draw_media_box(t, CARD_X + CARD_W - PAD - THUMB, y + PAD, THUMB, THUMB, IMG_THUMB,
                       t != NULL, p->is_video, media_hidden(p), NULL);
    }
}

static void draw_feed_top(void)
{
    C2D_TargetClear(g_top, COL_BG);
    C2D_SceneBegin(g_top);
    float off = -g_scroll;

    for (int i = 0; i < g_ncards; i++) {
        float y = g_cards[i].y + off;
        if (y > SCR_H || y + g_cards[i].h < HEAD_H) continue;
        draw_card(i, y);
    }

    if (g_feed.after[0]) {
        float y = g_feed_h - 38 + off;
        bool sel = (g_sel == g_feed.n);
        if (sel) ui_rrect(CARD_X - 2, y - 2, CARD_W + 4, 32, 11, COL_ACCENT);
        ui_rrect(CARD_X, y, CARD_W, 28, 9, sel ? COL_CARD_HI : COL_CARD);
        if (loading(JOB_FEED, 1)) {
            ui_spinner(TOP_W / 2 - 50, y + 14, 6, COL_ACCENT);
            ui_text(TOP_W / 2 - 38, y + 7, 0.45f, COL_DIM, TR("Chargement...", "Loading..."));
        } else {
            ui_text_center(TOP_W / 2, y + 7, 0.45f, COL_LINK, TR(BTN_A " Charger plus", BTN_A " Load more"));
        }
    }

    char title[96];
    feed_title(title, sizeof(title));
    if (g_feed.n == 0) {
        if (loading(JOB_FEED, 0)) {
            ui_spinner(TOP_W / 2, 118, 16, COL_ACCENT);
            char msg[128];
            snprintf(msg, sizeof(msg), TR("Chargement de %s", "Loading %s"), title);
            ui_text_center(TOP_W / 2, 146, 0.5f, COL_DIM, msg);
            ui_text_center(TOP_W / 2, 166, 0.4f, COL_DIM, TR(BTN_B " annuler", BTN_B " cancel"));
        } else if (g_status[0]) {
            float h = g_status_doc.h + 2 * PAD + 22;
            float y = 60;
            ui_rrect(CARD_X, y, CARD_W, h, 10, COL_CARD);
            ui_text(CARD_X + PAD, y + PAD, 0.5f, COL_RED, TR("Impossible de charger", "Couldn't load"));
            doc_draw(&g_status_doc, CARD_X + PAD, y + PAD + 20, CARD_W, SCR_H, 0, -1);
            ui_text_center(TOP_W / 2, y + h + 10, 0.42f, COL_DIM,
                           TR(BTN_X " réessayer   " BTN_Y " autre subreddit",
                              BTN_X " retry   " BTN_Y " another subreddit"));
        } else if (!job_busy() && !g_has_pending) {
            ui_text_center(TOP_W / 2, 110, 0.5f, COL_DIM, TR("Aucun post.", "No posts."));
        }
    }

    draw_scrollbar(TOP_W - 4, HEAD_H + 2, SCR_H - HEAD_H - 4, g_feed_h, g_scroll);

    char right[48];
    snprintf(right, sizeof(right), "%s",
             !strncmp(g_source, "Archive", 7) ? TR("Récents (archive)", "Newest (archive)") : sort_label(g_sort));
    draw_header(TOP_W, title, right, job_busy() || g_has_pending);
    ui_draw_toast(TOP_W);
}

static void draw_feed_bottom(void)
{
    C2D_TargetClear(g_bot, COL_BG);
    C2D_SceneBegin(g_bot);

    if (g_sel >= 0 && g_sel < g_feed.n) {
        const Post *p = &g_feed.items[g_sel];
        build_info_doc();
        float y = 8;
        if (has_media(p)) {
            /* on attend que la sélection se stabilise avant de télécharger l'aperçu */
            bool load = g_cfg.images != IMAGES_NONE && g_sel_stable > 12;
            draw_media_box(post_media(p), 8, y, BOT_W - 16, 116, IMG_PREVIEW, load, p->is_video,
                           media_hidden(p), TR(BTN_A " ouvrir le post", BTN_A " open post"));
            y += 116 + 8;
        }
        ui_rrect(8, y, BOT_W - 16, TOOL_Y - y - 6, 9, COL_CARD);
        doc_draw(&g_info, 16, y + 6, BOT_W - 32, TOOL_Y - y - 14, 0, -1);
        C2D_DrawRectSolid(8, TOOL_Y - 6, 0.5f, BOT_W - 16, 6, COL_BG); /* masque le débordement */

        if (ui_touch_tap() && ui_touch_y() < TOOL_Y) g_act = ACT_OPEN;
    } else {
        ui_logo(BOT_W / 2, 52, 26);
        ui_text_center(BOT_W / 2, 86, 0.65f, COL_WHITE, "Reddit3DS");
        ui_text_center(BOT_W / 2, 110, 0.42f, COL_DIM,
                       TR(BTN_DPAD " naviguer   " BTN_A " ouvrir   " BTN_Y " subreddits",
                          BTN_DPAD " browse   " BTN_A " open   " BTN_Y " subreddits"));
        ui_text_center(BOT_W / 2, 126, 0.42f, COL_DIM,
                       TR(BTN_L BTN_R " tri   " BTN_X " actualiser   SELECT réglages",
                          BTN_L BTN_R " sort   " BTN_X " refresh   SELECT settings"));
        if (g_source[0]) {
            char src[128];
            snprintf(src, sizeof(src), TR("Source : %s", "Source: %s"), g_source);
            ui_text_center(BOT_W / 2, 154, 0.38f, COL_DIM, src);
        }
    }

    draw_toolbar_bg();
    const float by = TOOL_Y + 5, bh = 26, bw = 59;
    if (ui_button(5, by, bw, bh, BTN_Y " Subs", false, true)) g_act = ACT_SEARCH;
    if (ui_button(68, by, bw, bh, TR(BTN_R " Tri", BTN_R " Sort"), false, true)) g_act = ACT_SORT_NEXT;
    if (ui_button(131, by, bw, bh, TR(BTN_X " Actu.", BTN_X " Reload"), false, true)) g_act = ACT_REFRESH;
    if (ui_button(194, by, bw, bh, TR("Réglages", "Settings"), false, true)) g_act = ACT_SETTINGS;
    if (ui_button(257, by, bw - 1, bh, TR(BTN_A " Ouvrir", BTN_A " Open"), true, g_sel < feed_items()))
        g_act = ACT_OPEN;
}

/* ------------------------------------------------------------------ */
/* Rendu : post                                                       */

static void draw_post_top(void)
{
    const Post *p = cur_post();
    C2D_TargetClear(g_top, COL_BG);
    C2D_SceneBegin(g_top);
    if (!p) return;

    if (g_body_img_y >= 0) {
        float iy = HEAD_H + g_body_img_y - g_body_scroll;
        if (iy < SCR_H && iy + BODY_IMG_H > HEAD_H) {
            bool hidden = media_hidden(p);
            bool load = (g_cfg.images != IMAGES_NONE || !strcmp(g_reveal_id, p->id)) && post_media(p)[0];
            draw_media_box(post_media(p), 10, iy, TOP_W - 20, BODY_IMG_H, IMG_PREVIEW, load, p->is_video,
                           hidden, TR(BTN_A " afficher l'image", BTN_A " show image"));
            if (!hidden && (load || p->video_id[0])) {
                const char *hint = p->video_id[0] ? TR(BTN_A " lire la vidéo", BTN_A " play video")
                                                  : TR(BTN_A " plein écran", BTN_A " full screen");
                float pw = ui_pill_width(hint, 0.38f);
                ui_pill(TOP_W - 16 - pw, iy + BODY_IMG_H - 20, hint, 0.38f, COL_BAR, COL_WHITE);
            }
        }
    }
    doc_draw(&g_body, 0, HEAD_H, TOP_W, SCR_H - HEAD_H, g_body_scroll, -1);
    draw_scrollbar(TOP_W - 4, HEAD_H + 2, SCR_H - HEAD_H - 4, g_body.h, g_body_scroll);

    char title[96], score[16], right[32];
    snprintf(title, sizeof(title), "r/%s", p->sub);
    fmt_num(score, sizeof(score), p->score);
    snprintf(right, sizeof(right), "%s pts", score);
    Job *cur = job_current();
    if (cur && cur->type == JOB_TRANSLATE && job_busy() && cur->ntexts > 0 && !strcmp(cur->post_id, p->id))
        snprintf(right, sizeof(right), TR("Traduction %d %%", "Translating %d %%"), cur->progress * 100 / cur->ntexts);
    else if (g_tr_n && g_tr_show && !strcmp(g_tr_post, p->id))
        snprintf(right, sizeof(right), TR("Traduit · %s", "Translated · %s"), settings_tr_code(g_cfg.tr_lang));
    draw_header(TOP_W, title, right, loading(JOB_COMMENTS, -1) || loading(JOB_TRANSLATE, -1));
    ui_draw_toast(TOP_W);
}

static void draw_post_bottom(void)
{
    C2D_TargetClear(g_bot, COL_BG);
    C2D_SceneBegin(g_bot);

    if (loading(JOB_COMMENTS, -1) && g_comments.n == 0) {
        ui_spinner(BOT_W / 2, 90, 14, COL_ACCENT);
        ui_text_center(BOT_W / 2, 114, 0.45f, COL_DIM,
                       TR("Chargement des commentaires...", "Loading comments..."));
    }

    for (int i = 0; i < g_com.nitems && i < g_comments.n; i++) {
        float top = g_com.items[i].top - g_com_scroll;
        float bot = g_com.items[i].bot - g_com_scroll;
        if (bot < 0) continue;
        if (top > TOOL_Y) break;
        const Comment *c = &g_comments.items[i];
        int d = g_com_depth ? g_com_depth[i] : 0;
        float cx = 6 + d * 7;
        bool sel = (i == g_com_sel);
        ui_rrect(cx, top, BOT_W - 6 - cx, bot - top, 7, sel ? COL_CARD_HI : COL_CARD);
        if (d > 0) ui_rrect(cx + 3, top + 4, 2.5f, bot - top - 8, 1, depth_color(c->depth));
        if (sel) ui_rrect(BOT_W - 9, top + 4, 2.5f, bot - top - 8, 1, COL_ACCENT);
        if (!c->more) {
            float tx = cx + 9;
            char author[96], score[16];
            snprintf(author, sizeof(author), "u/%s", c->author);
            ui_text(tx, top + 5, 0.4f, c->op ? COL_LINK : COL_ACCENT2, author);
            if (c->op) ui_pill(tx + ui_text_width(author, 0.4f) + 4, top + 4, "OP", 0.34f, COL_LINK, COL_BG);
            fmt_num(score, sizeof(score), c->score);
            float sw = ui_text_width(score, 0.4f);
            float sx = BOT_W - 16 - sw;
            ui_icon_up(sx - 11, top + 7, 8, COL_DIM);
            ui_text(sx, top + 5, 0.4f, COL_DIM, score);
        }
    }
    doc_draw(&g_com, 0, 0, BOT_W, TOOL_Y, g_com_scroll, -1);
    draw_scrollbar(BOT_W - 4, 2, TOOL_Y - 4, g_com.h, g_com_scroll);

    draw_toolbar_bg();
    const Post *p = cur_post();
    const float by = TOOL_Y + 5, bh = 26, bw = 74;
    bool tr_on = g_tr_n && g_tr_show && p && !strcmp(g_tr_post, p->id);
    if (ui_button(6, by, bw, bh, TR(BTN_B " Retour", BTN_B " Back"), false, true)) g_act = ACT_BACK;
    bool vid = p && p->video_id[0];
    if (ui_button(84, by, bw, bh, vid ? TR(BTN_A " Vidéo", BTN_A " Video") : BTN_A " Image", false,
                  p && (has_media(p) || vid)))
        g_act = ACT_VIEW;
    if (ui_button(162, by, bw, bh, tr_on ? BTN_Y " Original" : TR(BTN_Y " Traduire", BTN_Y " Translate"),
                  true, p != NULL))
        g_act = ACT_TRANSLATE;
    if (ui_button(240, by, bw, bh, TR(BTN_X " Actu.", BTN_X " Reload"), false, true)) g_act = ACT_REFRESH;
}

/* ------------------------------------------------------------------ */
/* Rendu : visionneuse                                                */

static const C2D_Image *viewer_image(ImgState *st)
{
    const C2D_Image *im = img_get(g_view_url, IMG_FULL, st);
    if (!im) im = img_get(g_view_url, IMG_PREVIEW, NULL); /* aperçu en attendant */
    return im;
}

static void viewer_clamp(void)
{
    ImgState st;
    const C2D_Image *im = viewer_image(&st);
    if (!im) { g_pan_x = g_pan_y = 0; return; }
    float iw, ih;
    img_size(im, &iw, &ih);
    float k = TOP_W / iw;
    if (ih * k > SCR_H) k = SCR_H / ih;
    float mx = (iw * k * g_zoom - TOP_W) / 2, my = (ih * k * g_zoom - SCR_H) / 2;
    if (mx < 0) mx = 0;
    if (my < 0) my = 0;
    if (g_pan_x > mx) g_pan_x = mx;
    if (g_pan_x < -mx) g_pan_x = -mx;
    if (g_pan_y > my) g_pan_y = my;
    if (g_pan_y < -my) g_pan_y = -my;
}

static void draw_viewer_top(void)
{
    C2D_TargetClear(g_top, C2D_Color32(0, 0, 0, 0xFF));
    C2D_SceneBegin(g_top);
    ImgState st = IMGS_LOADING;
    const C2D_Image *im = viewer_image(&st);
    if (im) img_draw_fit(im, 0, 0, TOP_W, SCR_H, g_zoom, g_pan_x, g_pan_y);
    if (st == IMGS_LOADING) ui_spinner(TOP_W - 18, 18, 8, COL_ACCENT);
    if (!im && st == IMGS_FAILED)
        ui_text_center(TOP_W / 2, 112, 0.5f, COL_DIM, TR("Image indisponible", "Image unavailable"));
    ui_draw_toast(TOP_W);
}

static void draw_viewer_bottom(void)
{
    C2D_TargetClear(g_bot, COL_BG);
    C2D_SceneBegin(g_bot);
    const Post *p = cur_post();
    if (p) {
        Doc d;
        memset(&d, 0, sizeof(d));
        doc_text(&d, p->title, 0.5f, 0, BOT_W - 32, COL_WHITE, 0);
        doc_truncate(&d, 3);
        ui_rrect(8, 8, BOT_W - 16, d.h + 16, 9, COL_CARD);
        doc_draw(&d, 16, 16, BOT_W, SCR_H, 0, -1);
        doc_free(&d);
    }
    char z[32];
    snprintf(z, sizeof(z), "Zoom %d %%", (int)(g_zoom * 100 + 0.5f));
    ui_text_center(BOT_W / 2, 112, 0.55f, COL_TEXT, z);
    ui_text_center(BOT_W / 2, 136, 0.4f, COL_DIM,
                   TR(BTN_L BTN_R " zoom   stick / " BTN_DPAD " / glisser : déplacer",
                      BTN_L BTN_R " zoom   stick / " BTN_DPAD " / drag: pan"));

    draw_toolbar_bg();
    const float by = TOOL_Y + 5, bh = 26;
    if (ui_button(6, by, 70, bh, "-", false, g_zoom > 1)) g_act = ACT_ZOOM_OUT;
    if (ui_button(80, by, 70, bh, "+", false, g_zoom < 8)) g_act = ACT_ZOOM_IN;
    if (ui_button(154, by, 74, bh, TR(BTN_A " Ajuster", BTN_A " Fit"), false, true)) g_act = ACT_ZOOM_RESET;
    if (ui_button(232, by, 82, bh, TR(BTN_B " Retour", BTN_B " Back"), true, true)) g_act = ACT_BACK;
}

/* ------------------------------------------------------------------ */
/* Rendu : onglet Subs (barre de recherche + clavier)                  */

static void draw_search_bar(void)
{
    ui_rrect(CARD_X, SBAR_Y, CARD_W, SBAR_H, SBAR_H / 2, COL_CARD_HI);
    /* loupe */
    float cx = CARD_X + 17, cy = SBAR_Y + SBAR_H / 2 - 1;
    C2D_DrawCircleSolid(cx, cy, 0.5f, 6.5f, COL_DIM);
    C2D_DrawCircleSolid(cx, cy, 0.5f, 4.5f, COL_CARD_HI);
    for (int i = 0; i < 4; i++) C2D_DrawCircleSolid(cx + 5 + i * 1.3f, cy + 5 + i * 1.3f, 0.5f, 1.4f, COL_DIM);

    float tx = CARD_X + 32, ty = SBAR_Y + (SBAR_H - ui_line_height(0.5f)) / 2;
    if (g_query[0]) {
        ui_text(tx, ty, 0.5f, COL_WHITE, g_query);
        tx += ui_text_width(g_query, 0.5f) + 1;
    } else {
        ui_text(tx + 3, ty, 0.5f, COL_DIM, TR("Rechercher un subreddit...", "Search subreddits..."));
    }
    if ((g_ui_frame / 30) % 2 == 0) /* curseur clignotant */
        C2D_DrawRectSolid(tx, SBAR_Y + 7, 0.5f, 1.5f, SBAR_H - 14, COL_ACCENT);
    if (loading(JOB_SEARCH, -1)) ui_spinner(CARD_X + CARD_W - 18, SBAR_Y + SBAR_H / 2, 6, COL_ACCENT);
}

static void draw_search_top(void)
{
    C2D_TargetClear(g_top, COL_BG);
    C2D_SceneBegin(g_top);
    float off = -g_s_scroll;
    char buf[160];
    bool icons = g_cfg.images != IMAGES_NONE;

    for (int i = 0; i < g_nscards; i++) {
        const SubCard *c = &g_scards[i];
        float y = c->y + off;
        if (y > SCR_H || y + c->h < LIST_Y - 20) continue;
        if (c->section) ui_text(CARD_X + 4, y - 17, 0.42f, COL_DIM, c->section);
        bool sel = (i == g_s_sel);
        if (sel) ui_rrect(CARD_X - 2, y - 2, CARD_W + 4, c->h + 4, 11, COL_ACCENT);
        ui_rrect(CARD_X, y, CARD_W, c->h, 9, sel ? COL_CARD_HI : COL_CARD);
        const char *name = scard_name(c);

        if (c->kind != SC_RESULT) {
            ui_logo(CARD_X + 18, y + c->h / 2, 9);
            if (c->kind == SC_GOTO) snprintf(buf, sizeof(buf), TR("Aller à r/%s", "Go to r/%s"), name);
            else snprintf(buf, sizeof(buf), "r/%s", name);
            ui_text(CARD_X + 34, y + (c->h - ui_line_height(0.5f)) / 2, 0.5f, COL_TEXT, buf);
            continue;
        }
        const SubInfo *s = &g_subs.items[c->idx];
        float ix = CARD_X + PAD, iy = y + PAD;
        if (s->icon[0] && icons) draw_media_box(s->icon, ix, iy, 48, 48, IMG_THUMB, true, false, false, NULL);
        else ui_logo(ix + 24, iy + 24, 20);
        float tx = ix + 48 + PAD, ty = iy;
        snprintf(buf, sizeof(buf), "r/%s", s->name);
        ui_text(tx, ty, 0.55f, COL_WHITE, buf);
        if (s->nsfw) ui_pill(tx + ui_text_width(buf, 0.55f) + 6, ty + 2, "NSFW", 0.36f, COL_RED, COL_WHITE);
        ty += ui_line_height(0.55f);
        fmt_members(buf, sizeof(buf), s->subscribers);
        ui_text(tx, ty, 0.4f, COL_ACCENT2, buf);
        ty += ui_line_height(0.4f) + 2;
        doc_draw(&c->desc, tx, ty, CARD_W, SCR_H, 0, -1);
    }

    /* état de la recherche sous la liste */
    float ly = (g_nscards > 0 ? g_scards[g_nscards - 1].y + g_scards[g_nscards - 1].h + 10 : LIST_Y + 10) + off;
    if (g_query[0]) {
        if (loading(JOB_SEARCH, -1)) {
            ui_spinner(TOP_W / 2 - 46, ly + 8, 7, COL_ACCENT);
            ui_text(TOP_W / 2 - 32, ly + 1, 0.45f, COL_DIM, TR("Recherche...", "Searching..."));
        } else if (g_s_err[0]) {
            ui_text_center(TOP_W / 2, ly, 0.42f, COL_RED, g_s_err);
        } else if (g_q_dirty || strlen(g_query) < 2) {
            ui_text_center(TOP_W / 2, ly, 0.42f, COL_DIM, TR("Continue d'écrire...", "Keep typing..."));
        } else if (g_subs.n == 0 && !strcmp(g_query, g_last_q)) {
            ui_text_center(TOP_W / 2, ly, 0.45f, COL_DIM, TR("Aucun subreddit trouvé.", "No subreddit found."));
        }
    }

    draw_scrollbar(TOP_W - 4, LIST_Y, SCR_H - LIST_Y - 4, g_s_h - LIST_Y + 30, g_s_scroll);
    /* barre de recherche fixe au-dessus de la liste */
    C2D_DrawRectSolid(0, HEAD_H, 0.5f, TOP_W, LIST_Y - HEAD_H - 2, COL_BG);
    draw_search_bar();
    draw_header(TOP_W, "Subreddits", TR(BTN_B " Retour", BTN_B " Back"), false);
    ui_draw_toast(TOP_W);
}

static void draw_search_bottom(void)
{
    C2D_TargetClear(g_bot, COL_BG);
    C2D_SceneBegin(g_bot);
    char buf[128];

    /* bandeau : élément sélectionné (toucher = ouvrir) */
    ui_rrect(4, 4, BOT_W - 8, 48, 9, COL_CARD);
    if (g_s_sel >= 0 && g_s_sel < g_nscards) {
        const SubCard *c = &g_scards[g_s_sel];
        const SubInfo *s = c->kind == SC_RESULT ? &g_subs.items[c->idx] : NULL;
        if (s && s->icon[0] && g_cfg.images != IMAGES_NONE)
            draw_media_box(s->icon, 8, 8, 40, 40, IMG_THUMB, true, false, false, NULL);
        else
            ui_logo(28, 28, 16);
        snprintf(buf, sizeof(buf), "r/%s", scard_name(c));
        ui_text(54, 10, 0.5f, COL_WHITE, buf);
        if (s) fmt_members(buf, sizeof(buf), s->subscribers);
        else snprintf(buf, sizeof(buf), "%s", TR(BTN_A " ou toucher ici pour ouvrir", BTN_A " or tap here to open"));
        ui_text(54, 30, 0.4f, s ? COL_ACCENT2 : COL_DIM, buf);
        if (ui_touch_tap() && ui_touch_y() < 54) g_act = ACT_OPEN;
    } else {
        ui_text_center(BOT_W / 2, 20, 0.45f, COL_DIM, TR("Tape le nom d'un subreddit", "Type a subreddit name"));
    }

    /* clavier */
    Key keys[64];
    int n = kb_layout(keys);
    float txp = ui_touch_x(), typ = ui_touch_y();
    for (int i = 0; i < n; i++) {
        const Key *k = &keys[i];
        bool pressed = ui_touch_held() && txp >= k->x && txp < k->x + k->w && typ >= k->y && typ < k->y + k->h;
        u32 bg = k->code == K_OK ? (pressed ? COL_ACCENT2 : COL_ACCENT)
                                 : (pressed ? COL_LINE : (k->code == K_BS ? COL_CARD : COL_CARD_HI));
        ui_rrect(k->x, k->y, k->w, k->h, 7, bg);
        float sc = k->label[1] ? 0.42f : 0.55f;
        ui_text_center(k->x + k->w / 2, k->y + (k->h - ui_line_height(sc)) / 2, sc, COL_WHITE, k->label);
    }
}

/* ------------------------------------------------------------------ */
/* Rendu : lecteur vidéo                                              */

static void fmt_time(char *o, size_t n, float s)
{
    if (s < 0) s = 0;
    int t = (int)s;
    if (t >= 3600) snprintf(o, n, "%d:%02d:%02d", t / 3600, (t / 60) % 60, t % 60);
    else snprintf(o, n, "%d:%02d", t / 60, t % 60);
}

static void draw_video_top(void)
{
    C2D_TargetClear(g_top, C2D_Color32(0, 0, 0, 0xFF));
    C2D_SceneBegin(g_top);
    const C2D_Image *im = video_image();
    if (im) img_draw_fit(im, 0, 0, TOP_W, SCR_H, 1, 0, 0);

    VideoStatus st = video_status();
    if (st == VS_ERROR) {
        ui_rrect(40, 90, TOP_W - 80, 60, 10, COL_CARD);
        ui_text_center(TOP_W / 2, 100, 0.5f, COL_RED, TR("Lecture impossible", "Can't play video"));
        ui_text_center(TOP_W / 2, 122, 0.42f, COL_DIM, video_error());
    } else if (st == VS_PAUSED || st == VS_ENDED) { /* le chargement s'affiche en bas (pourcentage) */
        float cx = TOP_W / 2, cy = SCR_H / 2;
        C2D_DrawCircleSolid(cx, cy, 0.5f, 26, COL_SCRIM);
        if (st == VS_PAUSED) {
            C2D_DrawTriangle(cx - 8, cy - 13, COL_WHITE, cx - 8, cy + 13, COL_WHITE, cx + 14, cy, COL_WHITE, 0.5f);
        } else {
            ui_text_center(cx, cy - 8, 0.45f, COL_WHITE, TR("Fin", "End"));
        }
    }
    ui_draw_toast(TOP_W);
}

static void draw_video_bottom(void)
{
    C2D_TargetClear(g_bot, COL_BG);
    C2D_SceneBegin(g_bot);
    const Post *p = cur_post();
    float y = 8;
    if (p) {
        Doc d;
        memset(&d, 0, sizeof(d));
        doc_text(&d, p->title, 0.48f, 0, BOT_W - 32, COL_WHITE, 0);
        doc_truncate(&d, 3);
        ui_rrect(8, y, BOT_W - 16, d.h + 16, 9, COL_CARD);
        doc_draw(&d, 16, y + 8, BOT_W, SCR_H, 0, -1);
        y += d.h + 24;
        doc_free(&d);
    }

    /* barre de progression */
    float pos = video_position(), dur = video_duration();
    char a[16], b[16], line[96];
    fmt_time(a, sizeof(a), pos);
    fmt_time(b, sizeof(b), dur);
    float bx = 16, bw = BOT_W - 32, by = y + 4;
    ui_rrect(bx, by, bw, 6, 3, COL_CARD_HI);
    if (video_status() == VS_LOADING) {
        /* chargement : juste un pourcentage (barre = progression du téléchargement) */
        int pc = video_load_percent();
        if (pc > 0) ui_rrect(bx, by, bw * pc / 100.0f, 6, 3, COL_ACCENT);
        if (pc >= 0) snprintf(line, sizeof(line), TR("Chargement : %d %%", "Loading: %d %%"), pc);
        else snprintf(line, sizeof(line), "%s", TR("Chargement...", "Loading..."));
    } else {
        if (dur > 0) {
            float k = pos / dur;
            if (k > 1) k = 1;
            if (k > 0.01f) ui_rrect(bx, by, bw * k, 6, 3, COL_ACCENT);
        }
        if (dur > 0) snprintf(line, sizeof(line), "%s / %s", a, b);
        else snprintf(line, sizeof(line), "%s", a);
        int pc = video_buffering() ? video_load_percent() : -1; /* coupure en cours de lecture */
        if (pc >= 0 && pc < 100) {
            size_t l = strlen(line);
            snprintf(line + l, sizeof(line) - l, " · %d %%", pc);
        }
    }
    ui_text(bx, by + 12, 0.42f, COL_TEXT, line);

    /* qualité et son */
    if (video_quality() > 0) {
        if (video_has_audio())
            snprintf(line, sizeof(line), TR("%dp · son", "%dp · sound"), video_quality());
        else if (!video_sound_ok())
            snprintf(line, sizeof(line), "%dp", video_quality());
        else
            snprintf(line, sizeof(line), TR("%dp · sans son", "%dp · no sound"), video_quality());
        ui_text(bx + bw - ui_text_width(line, 0.42f), by + 12, 0.42f, COL_DIM, line);
    }
    y = by + 34;
    if (!video_sound_ok()) {
        ui_text(bx, y, 0.38f, COL_YELLOW,
                TR("Son désactivé : sdmc:/3ds/dspfirm.cdc manquant (lance DSP1 une fois).",
                   "Sound off: sdmc:/3ds/dspfirm.cdc missing (run DSP1 once)."));
    } else if (video_has_audio()) {
        /* volume */
        snprintf(line, sizeof(line), "Volume %d %%", (int)(video_volume() * 100 + 0.5f));
        ui_text(bx, y, 0.42f, COL_DIM, line);
        float vx = bx + 90, vw = bw - 90;
        ui_rrect(vx, y + 5, vw, 5, 2.5f, COL_CARD_HI);
        ui_rrect(vx, y + 5, vw * video_volume(), 5, 2.5f, COL_LINK);
        ui_text(bx, y + 18, 0.38f, COL_DIM, TR(BTN_DPAD " haut/bas ou " BTN_L BTN_R " : volume",
                                               BTN_DPAD " up/down or " BTN_L BTN_R ": volume"));
    }

    draw_toolbar_bg();
    const float tby = TOOL_Y + 5, bh = 26;
    VideoStatus st = video_status();
    const char *play = st == VS_PLAYING ? TR(BTN_A " Pause", BTN_A " Pause")
                     : st == VS_ENDED   ? TR(BTN_A " Revoir", BTN_A " Replay")
                     : st == VS_ERROR   ? TR(BTN_A " Réessayer", BTN_A " Retry")
                                        : TR(BTN_A " Lecture", BTN_A " Play");
    if (ui_button(6, tby, 82, bh, TR(BTN_B " Retour", BTN_B " Back"), false, true)) g_act = ACT_BACK;
    if (ui_button(92, tby, 112, bh, play, true, st != VS_LOADING)) g_act = ACT_VIDEO_PLAY;
    if (ui_button(208, tby, 51, bh, "-", false, video_has_audio())) g_act = ACT_VOL_DOWN;
    if (ui_button(263, tby, 51, bh, "+", false, video_has_audio())) g_act = ACT_VOL_UP;
}

/* ------------------------------------------------------------------ */
/* Rendu : paramètres                                                 */

static void draw_settings_top(void)
{
    C2D_TargetClear(g_top, COL_BG);
    C2D_SceneBegin(g_top);
    float off = -g_set_scroll;
    char val[96];

    for (int i = 0; i < SET_COUNT; i++) {
        float y = HEAD_H + 6 + i * (SET_ROW_H + 4) + off;
        if (y > SCR_H || y + SET_ROW_H < HEAD_H) continue;
        bool sel = (i == g_set_sel);
        if (sel) ui_rrect(CARD_X - 2, y - 2, CARD_W + 4, SET_ROW_H + 4, 11, COL_ACCENT);
        ui_rrect(CARD_X, y, CARD_W, SET_ROW_H, 9, sel ? COL_CARD_HI : COL_CARD);
        float ty = y + (SET_ROW_H - ui_line_height(0.5f)) / 2;
        ui_text(CARD_X + 12, ty, 0.5f, COL_TEXT, set_label(i));

        set_value(i, val, sizeof(val));
        float vw = ui_text_width(val, 0.46f);
        float vx = CARD_X + CARD_W - 14 - vw;
        float vy = y + (SET_ROW_H - ui_line_height(0.46f)) / 2;
        if (i == SET_CLEAR) {
            ui_text(vx, vy, 0.46f, sel ? COL_ACCENT2 : COL_DIM, val);
            continue;
        }
        if (sel) {
            /* flèches gauche / droite */
            float cy = y + SET_ROW_H / 2;
            C2D_DrawTriangle(vx - 14, cy, COL_ACCENT, vx - 7, cy - 5, COL_ACCENT, vx - 7, cy + 5, COL_ACCENT, 0.5f);
            C2D_DrawTriangle(CARD_X + CARD_W - 6, cy, COL_ACCENT, CARD_X + CARD_W - 12, cy - 5, COL_ACCENT,
                             CARD_X + CARD_W - 12, cy + 5, COL_ACCENT, 0.5f);
            vx -= 4;
        }
        ui_text(vx, vy, 0.46f, sel ? COL_WHITE : COL_ACCENT2, val);
    }

    float total = HEAD_H + 6 + SET_COUNT * (SET_ROW_H + 4) + 40;
    float fy = HEAD_H + 6 + SET_COUNT * (SET_ROW_H + 4) + 6 + off;
    ui_text_center(TOP_W / 2, fy, 0.38f, COL_DIM, "Reddit3DS v" APP_VERSION "  ·  sdmc:/3ds/reddit3ds/config.ini");
    draw_scrollbar(TOP_W - 4, HEAD_H + 2, SCR_H - HEAD_H - 4, total, g_set_scroll);
    draw_header(TOP_W, TR("Paramètres", "Settings"), NULL, false);
    ui_draw_toast(TOP_W);
}

static void draw_settings_bottom(void)
{
    C2D_TargetClear(g_bot, COL_BG);
    C2D_SceneBegin(g_bot);

    if (g_set_help_for != g_set_sel) {
        g_set_help_for = g_set_sel;
        doc_free(&g_set_help);
        doc_text(&g_set_help, set_help(g_set_sel), 0.45f, 0, BOT_W - 32, COL_TEXT, 0);
    }
    char val[96];
    set_value(g_set_sel, val, sizeof(val));
    ui_text(14, 12, 0.55f, COL_WHITE, set_label(g_set_sel));
    if (g_set_sel != SET_CLEAR) ui_text(14, 34, 0.48f, COL_ACCENT2, val);
    ui_rrect(8, 58, BOT_W - 16, TOOL_Y - 64, 9, COL_CARD);
    doc_draw(&g_set_help, 16, 66, BOT_W - 32, TOOL_Y - 76, 0, -1);
    C2D_DrawRectSolid(8, TOOL_Y - 6, 0.5f, BOT_W - 16, 6, COL_BG);

    draw_toolbar_bg();
    const float by = TOOL_Y + 5, bh = 26;
    if (ui_button(6, by, 92, bh, TR(BTN_B " Retour", BTN_B " Back"), false, true)) g_act = ACT_BACK;
    if (g_set_sel == SET_CLEAR || g_set_sel == SET_START) {
        if (ui_button(102, by, 212, bh, g_set_sel == SET_CLEAR ? TR(BTN_A " Vider", BTN_A " Clear")
                                                                : TR(BTN_A " Prendre le sub actuel", BTN_A " Use current sub"),
                      true, true))
            g_act = ACT_SET_ACTIVATE;
    } else {
        if (ui_button(102, by, 104, bh, "<", false, true)) g_act = ACT_SET_PREV;
        if (ui_button(210, by, 104, bh, ">", true, true)) g_act = ACT_SET_NEXT;
    }
}

/* ------------------------------------------------------------------ */
/* Entrées                                                            */

static void input_feed(u32 down, u32 rep)
{
    if (rep & KEY_DOWN) feed_move(+1);
    if (rep & KEY_UP) feed_move(-1);
    if (rep & KEY_RIGHT) feed_move(+4);
    if (rep & KEY_LEFT) feed_move(-4);
    if (down & KEY_A) g_act = ACT_OPEN;
    if (down & KEY_B) g_act = ACT_BACK;
    if (down & KEY_X) g_act = ACT_REFRESH;
    if (down & KEY_Y) g_act = ACT_SEARCH;
    if (down & KEY_SELECT) g_act = ACT_SETTINGS;
    if (down & (KEY_R | KEY_ZR)) g_act = ACT_SORT_NEXT;
    if (down & (KEY_L | KEY_ZL)) g_act = ACT_SORT_PREV;

    /* défilement infini */
    if (g_feed.after[0] && g_feed.n > 0 && g_sel >= g_feed.n - 3 && !job_busy() && !g_has_pending &&
        g_ui_frame >= g_more_retry)
        request_feed(true);
}

static void input_post(u32 down, u32 rep, u32 held, const circlePosition *cp)
{
    if (rep & KEY_DOWN) doc_step(&g_com, +1, TOOL_Y, &g_com_sel, &g_com_to);
    if (rep & KEY_UP) doc_step(&g_com, -1, TOOL_Y, &g_com_sel, &g_com_to);
    if (rep & KEY_RIGHT) for (int i = 0; i < 4; i++) doc_step(&g_com, +1, TOOL_Y, &g_com_sel, &g_com_to);
    if (rep & KEY_LEFT) for (int i = 0; i < 4; i++) doc_step(&g_com, -1, TOOL_Y, &g_com_sel, &g_com_to);

    float dy = 0;
    if (abs(cp->dy) > 20) dy -= cp->dy / 10.0f;
    if (held & (KEY_R | KEY_ZR)) dy += 7;
    if (held & (KEY_L | KEY_ZL)) dy -= 7;
    if (dy != 0) {
        g_body_to += dy;
        clamp_scroll(&g_body_to, g_body.h, SCR_H - HEAD_H);
    }

    /* glisser du doigt sur les commentaires, avec inertie */
    if (ui_touch_held() && ui_touch_y() < TOOL_Y) {
        float d = ui_touch_dy();
        g_com_to -= d;
        g_com_vel = -d;
        g_com_scroll = g_com_to;
    } else if (fabsf(g_com_vel) > 0.3f) {
        g_com_to += g_com_vel;
        g_com_vel *= 0.92f;
    }
    clamp_scroll(&g_com_to, g_com.h, TOOL_Y);
    if (ui_touch_held() || fabsf(g_com_vel) > 0.3f) {
        for (int i = 0; i < g_com.nitems; i++) /* la sélection suit le défilement */
            if (g_com.items[i].bot > g_com_to + 4) { g_com_sel = i; break; }
    }

    if (down & KEY_A) g_act = ACT_VIEW;
    if (down & KEY_B) g_act = ACT_BACK;
    if (down & KEY_X) g_act = ACT_REFRESH;
    if (down & KEY_Y) g_act = ACT_TRANSLATE;
    if (down & KEY_SELECT) g_act = ACT_SETTINGS;
}

static void input_viewer(u32 down, u32 held, const circlePosition *cp)
{
    if (held & (KEY_R | KEY_ZR)) viewer_zoom(1.03f);
    if (held & (KEY_L | KEY_ZL)) viewer_zoom(1 / 1.03f);
    if (abs(cp->dx) > 20) g_pan_x -= cp->dx / 12.0f;
    if (abs(cp->dy) > 20) g_pan_y += cp->dy / 12.0f;
    if (held & KEY_DLEFT) g_pan_x += 5;
    if (held & KEY_DRIGHT) g_pan_x -= 5;
    if (held & KEY_DUP) g_pan_y += 5;
    if (held & KEY_DDOWN) g_pan_y -= 5;
    if (ui_touch_held() && ui_touch_y() < TOOL_Y) {
        g_pan_x += ui_touch_dx() * 1.5f;
        g_pan_y += ui_touch_dy() * 1.5f;
    }
    viewer_clamp();
    if (down & KEY_A) g_act = ACT_ZOOM_RESET;
    if (down & KEY_B) g_act = ACT_BACK;
}

static void input_search(u32 down, u32 rep)
{
    if (g_nscards > 0 && (rep & (KEY_DOWN | KEY_UP))) {
        g_s_sel += (rep & KEY_DOWN) ? 1 : -1;
        if (g_s_sel < 0) g_s_sel = 0;
        if (g_s_sel >= g_nscards) g_s_sel = g_nscards - 1;
        search_ensure_visible();
    }
    /* clavier tactile : la touche réagit dès que le doigt se pose */
    if (ui_touch_down()) {
        Key keys[64];
        int n = kb_layout(keys);
        float x = ui_touch_x(), y = ui_touch_y();
        for (int i = 0; i < n; i++)
            if (x >= keys[i].x && x < keys[i].x + keys[i].w && y >= keys[i].y && y < keys[i].y + keys[i].h) {
                kb_type(keys[i].code);
                break;
            }
    }
    if (rep & KEY_X) kb_type(K_BS);
    if (down & KEY_Y) kb_type(K_OK);
    /* recherche automatique après une courte pause dans la saisie */
    if (g_q_dirty && g_ui_frame - g_q_changed > 40) search_now();

    if (down & KEY_A) g_act = ACT_OPEN;
    if (down & KEY_B) g_act = ACT_BACK;
    if (down & KEY_SELECT) g_act = ACT_SETTINGS;
}

static void input_video(u32 down, u32 rep)
{
    if (down & KEY_A) g_act = ACT_VIDEO_PLAY;
    if (down & KEY_B) g_act = ACT_BACK;
    if (rep & (KEY_UP | KEY_R | KEY_ZR)) g_act = ACT_VOL_UP;
    if (rep & (KEY_DOWN | KEY_L | KEY_ZL)) g_act = ACT_VOL_DOWN;
    if (ui_touch_tap() && ui_touch_y() < TOOL_Y) g_act = ACT_VIDEO_PLAY;
}

static void input_settings(u32 down, u32 rep)
{
    if (rep & KEY_DOWN) { g_set_sel = (g_set_sel + 1) % SET_COUNT; set_ensure_visible(); }
    if (rep & KEY_UP) { g_set_sel = (g_set_sel + SET_COUNT - 1) % SET_COUNT; set_ensure_visible(); }
    if (rep & KEY_LEFT) g_act = ACT_SET_PREV;
    if (rep & KEY_RIGHT) g_act = ACT_SET_NEXT;
    if (down & KEY_A) g_act = ACT_SET_ACTIVATE;
    if (down & (KEY_B | KEY_SELECT)) g_act = ACT_BACK;
}

/* ------------------------------------------------------------------ */

int main(void)
{
    gfxInitDefault();
    romfsInit();
    psInit();                 /* entropie matérielle pour mbedTLS */
    osSetSpeedupEnable(true); /* New 3DS : 804 MHz + cache L2 */

    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(16384);
    C2D_Prepare();
    g_top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    g_bot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    ui_init();
    hidSetRepeatParameters(16, 4);

    settings_load();
    ui_set_text_scale(TEXT_SCALES[g_cfg.text_size]);
    g_sort = g_cfg.default_sort;
    snprintf(g_sub, sizeof(g_sub), "%s", g_cfg.start_sub);
    sanitize_sub(g_sub);

    FILE *ca = fopen(NET_CA_PATH, "r");
    int net = net_init();
    settings_apply_source();

    if (!ca) {
        snprintf(g_status, sizeof(g_status), TR("Certificats introuvables (%s).", "Certificates not found (%s)."),
                 NET_CA_PATH);
    } else if (net != 0) {
        snprintf(g_status, sizeof(g_status), TR("Impossible d'initialiser le réseau (%d).",
                                                "Couldn't initialise the network (%d)."), net);
    } else {
        job_init();
        img_init();
        g_online = true;
        request_feed(false);
    }
    if (ca) fclose(ca);
    build_cards();

    while (aptMainLoop()) {
        hidScanInput();
        u32 down = hidKeysDown(), held = hidKeysHeld(), rep = hidKeysDownRepeat();
        circlePosition cp;
        hidCircleRead(&cp);
        ui_touch_update();
        if (down & KEY_START) break;

        if (g_online) pump_jobs();

        switch (g_screen) {
        case SCR_FEED:     input_feed(down, rep); break;
        case SCR_POST:     input_post(down, rep, held, &cp); break;
        case SCR_IMAGE:    input_viewer(down, held, &cp); break;
        case SCR_SEARCH:   input_search(down, rep); break;
        case SCR_SETTINGS: input_settings(down, rep); break;
        case SCR_VIDEO:    input_video(down, rep); break;
        }
        if (g_sel_stable < 1000) g_sel_stable++;

        smooth(&g_scroll, g_scroll_to);
        smooth(&g_body_scroll, g_body_to);
        smooth(&g_com_scroll, g_com_to);
        smooth(&g_s_scroll, g_s_to);
        smooth(&g_set_scroll, g_set_to);

        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        ui_frame_begin();
        if (g_online) {
            img_frame();
            video_frame();
        }
        switch (g_screen) {
        case SCR_FEED:     draw_feed_top();     draw_feed_bottom();     break;
        case SCR_POST:     draw_post_top();     draw_post_bottom();     break;
        case SCR_IMAGE:    draw_viewer_top();   draw_viewer_bottom();   break;
        case SCR_SEARCH:   draw_search_top();   draw_search_bottom();   break;
        case SCR_SETTINGS: draw_settings_top(); draw_settings_bottom(); break;
        case SCR_VIDEO:    draw_video_top();    draw_video_bottom();    break;
        }
        C3D_FrameEnd(0);

        /* actions hors du rendu (le clavier système ne doit pas s'ouvrir pendant un frame) */
        Action a = g_act;
        g_act = ACT_NONE;
        if (a != ACT_NONE && (g_online || a == ACT_BACK || a == ACT_SETTINGS || a >= ACT_SET_PREV))
            do_action(a);
    }

    if (g_screen == SCR_SETTINGS && g_set_dirty) settings_save();
    if (g_online) {
        video_exit();
        job_exit();
        img_exit();
    }
    free_cards();
    free_scards();
    doc_free(&g_info);
    doc_free(&g_status_doc);
    doc_free(&g_body);
    doc_free(&g_com);
    doc_free(&g_set_help);
    free(g_com_depth);
    feed_clear(&g_feed);
    comments_clear(&g_comments);
    sublist_clear(&g_subs);

    net_exit();
    ui_exit();
    C2D_Fini();
    C3D_Fini();
    psExit();
    romfsExit();
    gfxExit();
    return 0;
}
