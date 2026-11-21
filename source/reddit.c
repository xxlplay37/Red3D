/*
 * Accès aux données Reddit, sans compte ni client_id :
 *
 *   1. Redlib  : front-end Reddit libre (instances publiques), données en direct,
 *                tris hot/new/top/rising. On lit son HTML.
 *   2. Arctic Shift : archive JSON de Reddit, quasi temps réel, très fiable,
 *                mais uniquement « récents » et scores figés à l'archivage.
 *   3. (optionnel) API officielle si un client_id est configuré.
 *
 * L'ordre est automatique : client_id > Redlib (instances essayées une par une) > archive.
 */
#include "reddit.h"
#include "html.h"
#include "json.h"
#include "net.h"
#include "settings.h"

#include <3ds.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define MAX_COMMENTS 400
#define MAX_INSTANCES 12
#define ARCTIC_BASE  "https://arctic-shift.photon-reddit.com/api"

typedef enum { MODE_AUTO, MODE_REDLIB, MODE_ARCHIVE } Mode;

static char s_client_id[64];
static char s_token[512];
static u64  s_token_expiry;

static char s_instances[MAX_INSTANCES][64];
static int  s_ninstances;
static int  s_good_instance; /* dernière instance qui a répondu */
static Mode s_mode = MODE_AUTO;
static char s_source[96] = "";

/* ------------------------------------------------------------------ */
/* Utilitaires                                                        */

static char *xstrdup(const char *s)
{
    if (!s) s = "";
    size_t n = strlen(s);
    char *d = malloc(n + 1);
    if (d) memcpy(d, s, n + 1);
    return d;
}

static void set_instances(const char *list)
{
    s_ninstances = 0;
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", list);
    for (char *tok = strtok(tmp, ", \t"); tok && s_ninstances < MAX_INSTANCES; tok = strtok(NULL, ", \t")) {
        if (!strncmp(tok, "https://", 8)) tok += 8;
        else if (!strncmp(tok, "http://", 7)) tok += 7;
        char *slash = strchr(tok, '/');
        if (slash) *slash = '\0';
        if (*tok) snprintf(s_instances[s_ninstances++], sizeof(s_instances[0]), "%s", tok);
    }
    s_good_instance = 0;
}

const char *reddit_source(void)
{
    return s_source;
}

void reddit_configure(const char *client_id, const char *instances, int mode)
{
    if (strcmp(s_client_id, client_id ? client_id : "")) s_token[0] = '\0';
    snprintf(s_client_id, sizeof(s_client_id), "%s", client_id ? client_id : "");
    set_instances(instances && *instances ? instances : REDDIT_DEFAULT_INSTANCES);
    s_mode = mode == 1 ? MODE_REDLIB : mode == 2 ? MODE_ARCHIVE : MODE_AUTO;
}

bool reddit_using_oauth(void)
{
    return s_client_id[0] != '\0';
}

/* ------------------------------------------------------------------ */
/* Modèle                                                             */

static void post_free(Post *p)
{
    free(p->title); free(p->author); free(p->sub); free(p->selftext);
    free(p->url); free(p->domain); free(p->flair); free(p->thumb); free(p->image);
}

void feed_clear(Feed *f)
{
    for (int i = 0; i < f->n; i++) post_free(&f->items[i]);
    free(f->items);
    memset(f, 0, sizeof(*f));
}

void feed_append(Feed *dst, Feed *src)
{
    if (src->n > 0) {
        if (dst->n + src->n > dst->cap) {
            int nc = dst->n + src->n + 16;
            Post *p = realloc(dst->items, nc * sizeof(Post));
            if (!p) { feed_clear(src); return; }
            dst->items = p;
            dst->cap = nc;
        }
        memcpy(dst->items + dst->n, src->items, src->n * sizeof(Post));
        dst->n += src->n;
    }
    snprintf(dst->after, sizeof(dst->after), "%s", src->after);
    free(src->items);
    memset(src, 0, sizeof(*src));
}

void feed_remove(Feed *f, int idx)
{
    if (idx < 0 || idx >= f->n) return;
    post_free(&f->items[idx]);
    memmove(&f->items[idx], &f->items[idx + 1], (f->n - idx - 1) * sizeof(Post));
    f->n--;
}

void sublist_clear(SubList *l)
{
    for (int i = 0; i < l->n; i++) {
        free(l->items[i].name);
        free(l->items[i].desc);
        free(l->items[i].icon);
    }
    free(l->items);
    memset(l, 0, sizeof(*l));
}

static SubInfo *sublist_push(SubList *l)
{
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 32;
        SubInfo *p = realloc(l->items, nc * sizeof(SubInfo));
        if (!p) return NULL;
        l->items = p;
        l->cap = nc;
    }
    SubInfo *s = &l->items[l->n++];
    memset(s, 0, sizeof(*s));
    return s;
}

/* "a&amp;b" -> "a&b" (les URL de l'archive sont échappées HTML) */
static char *unescape_amp(const char *s)
{
    char *r = xstrdup(s);
    if (!r) return NULL;
    char *o = r;
    for (const char *p = r; *p;) {
        if (!strncmp(p, "&amp;", 5)) { *o++ = '&'; p += 5; }
        else *o++ = *p++;
    }
    *o = '\0';
    return r;
}

static bool is_http(const char *s)
{
    return s && !strncmp(s, "http", 4);
}

/* Copie l'identifiant qui suit `marker` (jusqu'à / ? " ou &) */
static void copy_id_after(const char *s, const char *marker, char *out, size_t n)
{
    const char *p = s ? strstr(s, marker) : NULL;
    if (!p) return;
    p += strlen(marker);
    size_t i = 0;
    while (p[i] && p[i] != '/' && p[i] != '?' && p[i] != '"' && p[i] != '&' && i + 1 < n) {
        out[i] = p[i];
        i++;
    }
    out[i] = '\0';
}

static bool is_direct_image(const char *url)
{
    if (!url) return false;
    const char *q = strchr(url, '?');
    size_t n = q ? (size_t)(q - url) : strlen(url);
    return (n > 4 && (!strncasecmp(url + n - 4, ".jpg", 4) || !strncasecmp(url + n - 4, ".png", 4))) ||
           (n > 5 && !strncasecmp(url + n - 5, ".jpeg", 5));
}

void comments_clear(Comments *c)
{
    for (int i = 0; i < c->n; i++) {
        free(c->items[i].author);
        free(c->items[i].body);
    }
    free(c->items);
    memset(c, 0, sizeof(*c));
}

static Post *feed_push(Feed *f)
{
    if (f->n == f->cap) {
        int nc = f->cap ? f->cap * 2 : 32;
        Post *p = realloc(f->items, nc * sizeof(Post));
        if (!p) return NULL;
        f->items = p;
        f->cap = nc;
    }
    Post *p = &f->items[f->n++];
    memset(p, 0, sizeof(*p));
    return p;
}

static void feed_pop(Feed *f)
{
    if (f->n > 0) post_free(&f->items[--f->n]);
}

static Comment *comments_push(Comments *c)
{
    if (c->n == c->cap) {
        int nc = c->cap ? c->cap * 2 : 64;
        Comment *p = realloc(c->items, nc * sizeof(Comment));
        if (!p) return NULL;
        c->items = p;
        c->cap = nc;
    }
    Comment *p = &c->items[c->n++];
    memset(p, 0, sizeof(*p));
    return p;
}

static void add_more(Comments *out, int depth, int count)
{
    Comment *cm = comments_push(out);
    if (!cm) return;
    char txt[64];
    if (count > 0)
        snprintf(txt, sizeof(txt), TR("+ %d réponse%s non chargée%s", "+ %d more repl%s not loaded%s"),
                 count, count > 1 ? TR("s", "ies") : TR("", "y"), count > 1 ? TR("s", "") : "");
    else
        snprintf(txt, sizeof(txt), TR("+ autres réponses non chargées", "+ more replies not loaded"));
    cm->author = xstrdup("");
    cm->body   = xstrdup(txt);
    cm->depth  = depth;
    cm->more   = true;
}

/* Remplit un Post depuis un objet JSON au format Reddit (API officielle / Arctic Shift). */
static void fill_post_json(Post *p, const JVal *d)
{
    snprintf(p->id, sizeof(p->id), "%s", json_str(d, "id", ""));
    p->title     = xstrdup(json_str(d, "title", ""));
    p->author    = xstrdup(json_str(d, "author", "[supprimé]"));
    p->sub       = xstrdup(json_str(d, "subreddit", ""));
    p->selftext  = xstrdup(json_str(d, "selftext", ""));
    p->url       = xstrdup(json_str(d, "url", ""));
    p->domain    = xstrdup(json_str(d, "domain", ""));
    p->flair     = xstrdup(json_str(d, "link_flair_text", ""));
    p->score     = (int)json_num(d, "score", 0);
    p->ncomments = (int)json_num(d, "num_comments", 0);
    p->created   = json_num(d, "created_utc", 0);
    p->is_self   = json_bool(d, "is_self", false);
    p->nsfw      = json_bool(d, "over_18", false);
    p->stickied  = json_bool(d, "stickied", false);
    p->is_video  = json_bool(d, "is_video", false);
    p->is_gallery = json_bool(d, "is_gallery", false);
    /* vidéo hébergée par Reddit */
    static const char *VKEYS[] = { "fallback_url", "dash_url", "hls_url" };
    for (int m = 0; m < 2 && !p->video_id[0]; m++) {
        JVal *rv = json_get(json_get(d, m ? "secure_media" : "media"), "reddit_video");
        for (int k = 0; k < 3 && !p->video_id[0]; k++)
            copy_id_after(json_str(rv, VKEYS[k], NULL), "v.redd.it/", p->video_id, sizeof(p->video_id));
    }
    if (!p->video_id[0]) copy_id_after(p->url, "v.redd.it/", p->video_id, sizeof(p->video_id));
    if (p->video_id[0]) p->is_video = true;
    if (!strcmp(p->selftext, "[removed]") || !strcmp(p->selftext, "[deleted]")) {
        free(p->selftext);
        p->selftext = xstrdup(TR("[supprimé]", "[deleted]"));
    }

    /* Images : on préfère les aperçus redimensionnés par Reddit (JPEG légers). */
    const char *thumb = NULL, *image = NULL;
    JVal *img0 = json_idx(json_get(json_get(d, "preview"), "images"), 0);
    JVal *res = json_get(img0, "resolutions");
    if (res && res->type == J_ARR) {
        int best_t = 0, best_i = 0;
        for (JVal *r = res->child; r; r = r->next) {
            int w = (int)json_num(r, "width", 0);
            const char *u = json_str(r, "url", NULL);
            if (!u) continue;
            if (w <= 320 && w > best_t) { best_t = w; thumb = u; }
            if (w <= 1080 && w > best_i) { best_i = w; image = u; }
        }
    }
    const char *tn = json_str(d, "thumbnail", NULL);
    if (!thumb && is_http(tn)) thumb = tn;
    if (is_direct_image(p->url) && strstr(p->url, "i.redd.it")) image = p->url;
    if (!image && thumb) image = thumb;
    p->thumb = unescape_amp(thumb ? thumb : "");
    p->image = unescape_amp(image ? image : "");
}

static void walk_comments(const JVal *children, int depth, const char *op, Comments *out)
{
    if (!children || children->type != J_ARR) return;
    for (JVal *c = children->child; c && out->n < MAX_COMMENTS; c = c->next) {
        const char *kind = json_str(c, "kind", "");
        JVal *d = json_get(c, "data");
        if (!strcmp(kind, "t1")) {
            Comment *cm = comments_push(out);
            if (!cm) return;
            const char *author = json_str(d, "author", "[supprimé]");
            cm->author = xstrdup(author);
            cm->body   = xstrdup(json_str(d, "body", ""));
            cm->score  = (int)json_num(d, "score", 0);
            cm->depth  = depth;
            cm->op     = op && *op && !strcmp(author, op);

            JVal *replies = json_get(d, "replies");
            if (replies && replies->type == J_OBJ)
                walk_comments(json_get(json_get(replies, "data"), "children"), depth + 1, op, out);
        } else if (!strcmp(kind, "more")) {
            int count = (int)json_num(d, "count", 0);
            if (count > 0) add_more(out, depth, count);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Requête générique                                                  */

static NetConn *s_conn;

void reddit_attach(NetConn *conn)
{
    s_conn = conn;
}

static int fetch(const char *url, const char *bearer, const char *accept, Buf *b, long *code,
                 char *err, size_t errlen)
{
    memset(b, 0, sizeof(*b));
    if (http_request(s_conn, url, bearer, NULL, NULL, accept, 0, b, code, err, errlen)) {
        buf_free(b);
        return -1;
    }
    if (*code != 200) {
        switch (*code) {
        case 403: snprintf(err, errlen, TR("Accès refusé (403)", "Access denied (403)")); break;
        case 404: snprintf(err, errlen, TR("Introuvable (404)", "Not found (404)")); break;
        case 429: snprintf(err, errlen, TR("Trop de requêtes (429)", "Too many requests (429)")); break;
        default:  snprintf(err, errlen, TR("Erreur HTTP %ld", "HTTP error %ld"), *code); break;
        }
        buf_free(b);
        return -1;
    }
    if (!b->data) {
        snprintf(err, errlen, TR("Réponse vide", "Empty response"));
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Source 1 (optionnelle) : API officielle avec client_id             */

static int ensure_token(char *err, size_t errlen)
{
    if (s_token[0] && osGetTime() < s_token_expiry) return 0;

    Buf b = {0};
    long code = 0;
    int r = http_request(s_conn, "https://www.reddit.com/api/v1/access_token", NULL, s_client_id,
                         "grant_type=https%3A%2F%2Foauth.reddit.com%2Fgrants%2Finstalled_client"
                         "&device_id=DO_NOT_TRACK_THIS_DEVICE",
                         ACCEPT_JSON, 0, &b, &code, err, errlen);
    if (r) { buf_free(&b); return -1; }
    if (code != 200) {
        snprintf(err, errlen, TR("OAuth : HTTP %ld (client_id invalide ?)", "OAuth: HTTP %ld (invalid client_id?)"), code);
        buf_free(&b);
        return -1;
    }
    JVal *j = json_parse(b.data);
    buf_free(&b);
    const char *tok = json_str(j, "access_token", NULL);
    if (!tok || strlen(tok) >= sizeof(s_token)) {
        snprintf(err, errlen, TR("OAuth : réponse invalide", "OAuth: invalid response"));
        json_free(j);
        return -1;
    }
    snprintf(s_token, sizeof(s_token), "%s", tok);
    double exp = json_num(j, "expires_in", 3600);
    if (exp < 120) exp = 120;
    s_token_expiry = osGetTime() + (u64)((exp - 60) * 1000.0);
    json_free(j);
    return 0;
}

static JVal *oauth_get(const char *path, const char *query, char *err, size_t errlen)
{
    for (int attempt = 0; attempt < 2; attempt++) {
        if (ensure_token(err, errlen)) return NULL;
        char url[512];
        snprintf(url, sizeof(url), "https://oauth.reddit.com%s?%s", path, query);
        Buf b;
        long code = 0;
        if (fetch(url, s_token, ACCEPT_JSON, &b, &code, err, errlen)) {
            if (code == 401 && attempt == 0) { s_token[0] = '\0'; continue; }
            return NULL;
        }
        JVal *j = json_parse(b.data);
        buf_free(&b);
        if (!j) snprintf(err, errlen, TR("Réponse JSON invalide", "Invalid JSON response"));
        return j;
    }
    return NULL;
}

static int oauth_feed(const char *sub, const char *sort, const char *after, Feed *feed,
                      char *err, size_t errlen)
{
    char path[128], query[192];
    snprintf(path, sizeof(path), "/r/%s/%s", *sub ? sub : "popular", sort);
    snprintf(query, sizeof(query), "limit=25&raw_json=1%s%s%s",
             !strcmp(sort, "top") ? "&t=day" : "", after[0] ? "&after=" : "", after);
    JVal *j = oauth_get(path, query, err, errlen);
    if (!j) return -1;
    JVal *data = json_get(j, "data");
    JVal *children = json_get(data, "children");
    if (!children || children->type != J_ARR) {
        snprintf(err, errlen, TR("Réponse inattendue", "Unexpected response"));
        json_free(j);
        return -1;
    }
    for (JVal *c = children->child; c; c = c->next) {
        if (strcmp(json_str(c, "kind", ""), "t3")) continue;
        Post *p = feed_push(feed);
        if (!p) break;
        fill_post_json(p, json_get(c, "data"));
    }
    snprintf(feed->after, sizeof(feed->after), "%s", json_str(data, "after", ""));
    json_free(j);
    snprintf(s_source, sizeof(s_source), "API Reddit");
    return 0;
}

static int oauth_comments(const Post *post, Comments *out, char *err, size_t errlen)
{
    char path[64];
    snprintf(path, sizeof(path), "/comments/%s", post->id);
    JVal *j = oauth_get(path, "raw_json=1&limit=150&depth=8", err, errlen);
    if (!j) return -1;
    walk_comments(json_get(json_get(json_idx(j, 1), "data"), "children"), 0, post->author, out);
    json_free(j);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Source 2 : Redlib (HTML)                                           */

/* Chemins Redlib -> CDN Reddit (pris en charge : /img, /preview, /style). Libère path. */
static char *map_media_url(const char *host, char *path)
{
    if (!path) return NULL;
    char *dec = html_to_text(path, strlen(path)); /* &#38; -> & */
    free(path);
    if (!dec) return NULL;
    if (!*dec || is_http(dec)) return dec;
    static const struct { const char *prefix, *base; } MAP[] = {
        { "/img/", "https://i.redd.it/" },
        { "/preview/pre/", "https://preview.redd.it/" },
        { "/preview/external-pre/", "https://external-preview.redd.it/" },
        { "/style/", "https://styles.redditmedia.com/" },
    };
    const char *base = NULL, *rest = dec;
    for (size_t i = 0; i < sizeof(MAP) / sizeof(MAP[0]); i++) {
        size_t n = strlen(MAP[i].prefix);
        if (!strncmp(dec, MAP[i].prefix, n)) { base = MAP[i].base; rest = dec + n; break; }
    }
    char *out = malloc(strlen(dec) + strlen(host) + 48);
    if (out) {
        if (base) sprintf(out, "%s%s", base, rest);
        else sprintf(out, "https://%s%s", host, dec);
    }
    free(dec);
    return out;
}

/* Prochain "<div class="post" ou "<div class="post ..." (pas post_xxx) */
static const char *next_post_div(const char *p, const char *end)
{
    while ((p = html_find(p, end, "<div class=\"post")) != NULL) {
        char c = p[16];
        if (c == ' ' || c == '"') return p;
        p += 16;
    }
    return NULL;
}

static char *text_between(const char *s, const char *e)
{
    return html_to_text(s, (size_t)(e - s));
}

static bool redlib_parse_post(const char *b, const char *e, const char *host, Post *p)
{
    char *id = html_copy_after(b, e, "id=\"", '"');
    if (!id) return false;
    snprintf(p->id, sizeof(p->id), "%s", id);
    free(id);

    const char *tag_end = memchr(b, '>', (size_t)(e - b));
    p->stickied = tag_end && html_find(b, tag_end, "stickied") != NULL;
    p->nsfw = html_find(b, e, "class=\"nsfw\"") != NULL;

    p->sub = html_copy_after(b, e, "class=\"post_subreddit\" href=\"/r/", '"');
    const char *a = html_find(b, e, "class=\"post_author");
    p->author = a ? html_copy_after(a, e, "href=\"/u/", '"') : NULL;

    char *date = html_copy_after(b, e, "class=\"created\" title=\"", '"');
    p->created = html_parse_date(date);
    free(date);

    /* Titre : le lien vers /comments/ dans le <h2 class="post_title"> */
    const char *h2 = html_find(b, e, "class=\"post_title\"");
    const char *h2e = h2 ? html_find(h2, e, "</h2>") : NULL;
    if (h2 && h2e) {
        const char *l = html_find(h2, h2e, "/comments/");
        const char *gt = l ? memchr(l, '>', (size_t)(h2e - l)) : NULL;
        const char *ae = gt ? html_find(gt, h2e, "</a>") : NULL;
        if (gt && ae) p->title = text_between(gt + 1, ae);

        const char *fl = html_find(h2, h2e, "class=\"post_flair\"");
        const char *sp = fl ? html_find(fl, h2e, "<span>") : NULL;
        const char *spe = sp ? html_find(sp, h2e, "</span>") : NULL;
        if (sp && spe) p->flair = text_between(sp + 6, spe);
    }

    char *score = html_copy_after(b, e, "class=\"post_score\" title=\"", '"');
    p->score = score ? atoi(score) : 0;
    free(score);
    char *ncom = html_copy_after(b, e, "class=\"post_comments\" title=\"", '"');
    p->ncomments = ncom ? atoi(ncom) : 0;
    free(ncom);

    /* Lien externe, miniature et média */
    char *url = NULL, *thumb = NULL, *image = NULL;
    const char *th = html_find(b, e, "class=\"post_thumbnail\"");
    if (th) {
        const char *the = html_find(th, e, "</a>");
        if (!the) the = e;
        url = html_copy_after(th, the, "href=\"", '"');
        if (url && !strncmp(url, "/r/", 3)) { /* lien interne : galerie */
            p->is_gallery = html_find(th, the, "<span>gallery</span>") != NULL;
            free(url);
            url = NULL;
        }
        thumb = html_copy_after(th, the, "src=\"", '"');
    }
    const char *md = html_find(b, e, "class=\"post_media_content\"");
    if (md) {
        const char *vid = html_find(md, e, "<video");
        if (vid) {
            p->is_video = true;
            image = html_copy_after(vid, e, "poster=\"", '"');
            const char *ve = html_find(vid, e, "</video>");
            if (ve) {
                size_t vn = (size_t)(ve - vid);
                char *vb = malloc(vn + 1);
                if (vb) {
                    memcpy(vb, vid, vn);
                    vb[vn] = '\0';
                    copy_id_after(vb, "/vid/", p->video_id, sizeof(p->video_id));
                    if (!p->video_id[0]) copy_id_after(vb, "/hls/", p->video_id, sizeof(p->video_id));
                    free(vb);
                }
            }
        } else {
            image = html_copy_after(md, e, "href=\"", '"');
        }
    }
    url = map_media_url(host, url);
    p->thumb = map_media_url(host, thumb);
    p->image = map_media_url(host, image);
    if (!p->thumb) p->thumb = xstrdup("");
    if (!p->image) p->image = xstrdup("");
    if (!p->image[0] && p->thumb[0]) {
        free(p->image);
        p->image = xstrdup(p->thumb);
    }
    if (!url && !p->is_video && is_direct_image(p->image)) url = xstrdup(p->image);

    /* Texte du post */
    const char *body = html_find(b, e, "<div class=\"post_body");
    const char *mds = body ? html_find(body, e, "<div class=\"md\">") : NULL;
    const char *mde = mds ? html_find(mds, e, "<!-- SC_ON -->") : NULL;
    p->selftext = (mds && mde) ? text_between(mds, mde) : NULL;

    p->is_self = (url == NULL && !p->image[0] && !p->is_gallery);
    p->url = url;
    if (!p->title) p->title = xstrdup("(sans titre)");
    if (!p->sub) p->sub = xstrdup("");
    if (!p->author) p->author = xstrdup(TR("[supprimé]", "[deleted]"));
    if (!p->flair) p->flair = xstrdup("");
    if (!p->selftext) p->selftext = xstrdup("");
    if (!p->url) p->url = xstrdup("");
    p->domain = xstrdup("");
    return true;
}

/* Message d'erreur lisible à partir du <title> d'une page Redlib */
static void redlib_page_error(const char *html, char *err, size_t errlen)
{
    const char *end = html + strlen(html);
    const char *t = html_find(html, end, "<title>");
    const char *te = t ? html_find(t, end, "</title>") : NULL;
    if (t && te && te - t < 200) {
        char *s = text_between(t + 7, te);
        snprintf(err, errlen, "%s", s ? s : TR("page invalide", "invalid page"));
        free(s);
    } else {
        snprintf(err, errlen, TR("page invalide (protection anti-robot ?)", "invalid page (anti-bot protection?)"));
    }
}

static int redlib_feed_from(const char *host, const char *sub, const char *sort, const char *after,
                            Feed *feed, char *err, size_t errlen)
{
    char url[384];
    snprintf(url, sizeof(url), "https://%s/r/%s/%s?t=%s%s%s", host, *sub ? sub : "popular", sort,
             !strcmp(sort, "top") ? "day" : "", after[0] ? "&after=" : "", after);

    Buf b;
    long code = 0;
    if (fetch(url, NULL, ACCEPT_HTML, &b, &code, err, errlen)) return -1;

    const char *html = b.data, *end = b.data + b.len;
    const char *posts = html_find(html, end, "<div id=\"posts\"");
    if (!posts) {
        redlib_page_error(html, err, errlen);
        buf_free(&b);
        return -1;
    }

    int start_n = feed->n;
    const char *p = next_post_div(posts, end);
    while (p) {
        const char *next = next_post_div(p + 16, end);
        const char *pe = next ? next : end;
        Post *post = feed_push(feed);
        if (!post) break;
        if (!redlib_parse_post(p, pe, host, post)) feed_pop(feed);
        p = next;
    }

    char *aft = html_copy_after(posts, end, "after=t3_", '"');
    if (aft) {
        char *amp = strchr(aft, '&');
        if (amp) *amp = '\0';
    }
    snprintf(feed->after, sizeof(feed->after), "%s%s", aft ? "t3_" : "", aft ? aft : "");
    free(aft);
    buf_free(&b);

    if (feed->n == start_n && !after[0]) {
        /* page valide mais vide : sub vide ou inexistant, inutile d'essayer ailleurs */
        feed->after[0] = '\0';
    }
    snprintf(s_source, sizeof(s_source), "Redlib (%s)", host);
    return 0;
}

static int redlib_comments_from(const char *host, const Post *post, Comments *out,
                                char *err, size_t errlen)
{
    char url[384];
    snprintf(url, sizeof(url), "https://%s/r/%s/comments/%s/", host, post->sub, post->id);
    Buf b;
    long code = 0;
    if (fetch(url, NULL, ACCEPT_HTML, &b, &code, err, errlen)) return -1;

    const char *html = b.data, *end = b.data + b.len;
    if (!html_find(html, end, "class=\"post_title\"")) {
        redlib_page_error(html, err, errlen);
        buf_free(&b);
        return -1;
    }

    static const char *T_COMMENT = "class=\"comment\" data-comment-id=";
    static const char *T_REPLIES = "<blockquote class=\"replies\">";
    static const char *T_CLOSE   = "</blockquote>";
    static const char *T_MORE    = "class=\"deeper_replies\"";

    const char *p = html_find(html, end, "<div class=\"thread\"");
    int depth = 0;
    /* Positions des prochains jetons, mises en cache : on ne cherche à nouveau que lorsqu'on a
     * dépassé la position mémorisée (avant : 4 recherches jusqu'à la fin de la page à chaque tour). */
    const char *c = NULL, *r = NULL, *x = NULL, *m = NULL;
    if (p) {
        c = html_find(p, end, T_COMMENT);
        r = html_find(p, end, T_REPLIES);
        x = html_find(p, end, T_CLOSE);
        m = html_find(p, end, T_MORE);
    }
    while (p && out->n < MAX_COMMENTS) {
        if (c && c < p) c = html_find(p, end, T_COMMENT);
        if (r && r < p) r = html_find(p, end, T_REPLIES);
        if (x && x < p) x = html_find(p, end, T_CLOSE);
        if (m && m < p) m = html_find(p, end, T_MORE);
        const char *first = NULL;
        const char *cand[4] = { c, r, x, m };
        for (int i = 0; i < 4; i++)
            if (cand[i] && (!first || cand[i] < first)) first = cand[i];
        if (!first) break;

        if (first == r) {
            depth++;
            p = r + strlen(T_REPLIES);
        } else if (first == x) {
            if (depth > 0) depth--;
            p = x + strlen(T_CLOSE);
        } else if (first == m) {
            const char *gt = memchr(m, '>', (size_t)(end - m));
            const char *ae = gt ? html_find(gt, end, "</a>") : NULL;
            int count = 0;
            if (gt && ae) {
                const char *par = html_find(gt, ae, "(");
                if (par) count = atoi(par + 1);
            }
            add_more(out, depth, count);
            p = ae ? ae + 4 : m + strlen(T_MORE);
        } else {
            /* Un commentaire : on lit ses champs puis on saute son corps (qui peut
             * contenir des <blockquote> de citation) jusqu'au bloc de réponses. */
            const char *next_c = html_find(c + 10, end, T_COMMENT);
            const char *limit = next_c ? next_c : end;
            const char *rep = html_find(c, limit, T_REPLIES);
            const char *det = html_find(c, limit, "</details>");
            const char *ce = rep ? rep : (det ? det : limit);

            Comment *cm = comments_push(out);
            if (!cm) break;
            char *score = html_copy_after(c, ce, "class=\"comment_score\" title=\"", '"');
            cm->score = score ? atoi(score) : 0;
            free(score);

            const char *au = html_find(c, ce, "class=\"comment_author");
            const char *aq = au ? memchr(au + 7, '"', (size_t)(ce - au - 7)) : NULL; /* fin de class="..." */
            cm->op = au && aq && html_find(au, aq + 1, " op\"") != NULL;
            cm->author = au ? html_copy_after(au, ce, "href=\"/user/", '"') : NULL;
            if (!cm->author) cm->author = xstrdup(TR("[supprimé]", "[deleted]"));

            const char *bd = html_find(c, ce, "<div class=\"comment_body");
            const char *mds = bd ? html_find(bd, ce, "<div class=\"md\">") : NULL;
            cm->body = mds ? text_between(mds, ce) : xstrdup(TR("[supprimé]", "[deleted]"));
            if (!cm->body) cm->body = xstrdup("");
            cm->depth = depth;

            p = ce;
        }
    }
    buf_free(&b);
    snprintf(s_source, sizeof(s_source), "Redlib (%s)", host);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Source 3 : archive Arctic Shift (JSON)                             */

static int archive_feed(const char *sub, const char *after, double last_created, Feed *feed,
                        char *err, size_t errlen)
{
    if (!*sub || !strcasecmp(sub, "popular") || !strcasecmp(sub, "all") || strchr(sub, '+')) {
        snprintf(err, errlen, "%s", TR("L'archive ne gère ni r/popular ni le fil d'accueil : choisis un subreddit (Y).",
                                       "The archive can't serve r/popular or the home feed: pick a subreddit (Y)."));
        return -1;
    }
    char url[384], before[48] = "";
    if (!strncmp(after, "as:", 3)) snprintf(before, sizeof(before), "&before=%s", after + 3);
    else if (after[0] && last_created > 0) snprintf(before, sizeof(before), "&before=%.0f", last_created);
    snprintf(url, sizeof(url), ARCTIC_BASE "/posts/search?subreddit=%s&limit=25&sort=desc%s", sub, before);

    Buf b;
    long code = 0;
    if (fetch(url, NULL, ACCEPT_JSON, &b, &code, err, errlen)) return -1;
    JVal *j = json_parse(b.data);
    buf_free(&b);
    JVal *data = json_get(j, "data");
    if (!data || data->type != J_ARR) {
        snprintf(err, errlen, TR("Archive : réponse inattendue", "Archive: unexpected response"));
        json_free(j);
        return -1;
    }
    double last = 0;
    int added = 0;
    for (JVal *c = data->child; c; c = c->next) {
        Post *p = feed_push(feed);
        if (!p) break;
        fill_post_json(p, c);
        last = p->created;
        added++;
    }
    json_free(j);
    if (added >= 25 && last > 0) snprintf(feed->after, sizeof(feed->after), "as:%.0f", last);
    else feed->after[0] = '\0';
    snprintf(s_source, sizeof(s_source), TR("Archive Arctic Shift (récents)", "Archive Arctic Shift (newest)"));
    return 0;
}

static int archive_comments(const Post *post, Comments *out, char *err, size_t errlen)
{
    char url[256];
    snprintf(url, sizeof(url), ARCTIC_BASE "/comments/tree?link_id=%s&limit=500", post->id);
    Buf b;
    long code = 0;
    if (fetch(url, NULL, ACCEPT_JSON, &b, &code, err, errlen)) return -1;
    JVal *j = json_parse(b.data);
    buf_free(&b);
    JVal *data = json_get(j, "data");
    if (!data || data->type != J_ARR) {
        snprintf(err, errlen, TR("Archive : réponse inattendue", "Archive: unexpected response"));
        json_free(j);
        return -1;
    }
    walk_comments(data, 0, post->author, out);
    json_free(j);
    snprintf(s_source, sizeof(s_source), "Archive Arctic Shift");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Points d'entrée                                                    */

int reddit_fetch_feed(const char *sub, const char *sort, const char *after, double last_created,
                      Feed *feed, char *err, size_t errlen)
{
    char aft[32] = "";
    if (after) snprintf(aft, sizeof(aft), "%s", after);
    if (!sub) sub = "";

    if (s_client_id[0]) return oauth_feed(sub, sort, aft, feed, err, errlen);

    char last_err[160] = "";
    bool archive_cursor = !strncmp(aft, "as:", 3);

    if (s_mode != MODE_ARCHIVE && !archive_cursor) {
        for (int k = 0; k < s_ninstances; k++) {
            int i = (s_good_instance + k) % s_ninstances;
            if (redlib_feed_from(s_instances[i], sub, sort, aft, feed, last_err, sizeof(last_err)) == 0) {
                s_good_instance = i;
                return 0;
            }
        }
    }
    if (s_mode == MODE_REDLIB) {
        snprintf(err, errlen, TR("Aucune instance Redlib ne répond (%s).", "No Redlib instance is responding (%s)."), last_err);
        return -1;
    }
    if (archive_feed(sub, aft, last_created, feed, err, errlen) == 0) return 0;
    if (last_err[0]) {
        size_t l = strlen(err);
        snprintf(err + l, errlen - l, " / Redlib : %s", last_err);
    }
    return -1;
}

int reddit_fetch_comments(const Post *post, Comments *out, char *err, size_t errlen)
{
    if (s_client_id[0]) return oauth_comments(post, out, err, errlen);

    char last_err[160] = "";
    if (s_mode != MODE_ARCHIVE) {
        for (int k = 0; k < s_ninstances; k++) {
            int i = (s_good_instance + k) % s_ninstances;
            comments_clear(out);
            if (redlib_comments_from(s_instances[i], post, out, last_err, sizeof(last_err)) == 0) {
                s_good_instance = i;
                return 0;
            }
        }
    }
    if (s_mode == MODE_REDLIB) {
        snprintf(err, errlen, TR("Aucune instance Redlib ne répond (%s).", "No Redlib instance is responding (%s)."), last_err);
        return -1;
    }
    comments_clear(out);
    return archive_comments(post, out, err, errlen);
}

/* ------------------------------------------------------------------ */
/* Recherche de subreddits                                            */

static int redlib_search_from(const char *host, const char *q, SubList *out, char *err, size_t errlen)
{
    char enc[192], url[384];
    url_encode(q, enc, sizeof(enc));
    snprintf(url, sizeof(url), "https://%s/search?q=%s&type=sr_user", host, enc);
    Buf b;
    long code = 0;
    if (fetch(url, NULL, ACCEPT_HTML, &b, &code, err, errlen)) return -1;

    const char *html = b.data, *end = b.data + b.len;
    if (!html_find(html, end, "id=\"search_sort\"") && !html_find(html, end, "class=\"search_subreddit\"") &&
        !html_find(html, end, "id=\"posts\"")) {
        redlib_page_error(html, err, errlen);
        buf_free(&b);
        return -1;
    }
    static const char *MARK = "class=\"search_subreddit\">";
    const char *p = html_find(html, end, MARK);
    while (p) {
        const char *next = html_find(p + 10, end, MARK);
        const char *be = next ? next : end;
        /* le href est juste avant, dans la même balise <a> */
        const char *a = p;
        while (a > html && p - a < 300 && strncmp(a, "<a href=\"/r/", 12)) a--;
        if (!strncmp(a, "<a href=\"/r/", 12)) {
            SubInfo *s = sublist_push(out);
            if (!s) break;
            s->name = html_copy_after(a, p, "<a href=\"/r/", '"');
            if (s->name) {
                size_t n = strlen(s->name);
                if (n && s->name[n - 1] == '/') s->name[n - 1] = '\0';
            }
            char *icon = html_copy_after(p, be, "src=\"", '"');
            s->icon = map_media_url(host, icon);
            char *mem = html_copy_after(p, be, "class=\"search_subreddit_members\" title=\"", '"');
            s->subscribers = mem ? atoi(mem) : 0;
            free(mem);
            static const char *DESC = "class=\"search_subreddit_description\">";
            const char *d = html_find(p, be, DESC);
            const char *de = d ? html_find(d, be, "</p>") : NULL;
            s->desc = (d && de) ? text_between(d + strlen(DESC), de) : NULL;
            if (!s->name) s->name = xstrdup("?");
            if (!s->icon) s->icon = xstrdup("");
            if (!s->desc) s->desc = xstrdup("");
        }
        p = next;
    }
    buf_free(&b);
    snprintf(s_source, sizeof(s_source), "Redlib (%s)", host);
    return 0;
}

static void fill_sub_json(SubInfo *s, const JVal *d)
{
    s->name = xstrdup(json_str(d, "display_name", "?"));
    s->desc = xstrdup(json_str(d, "public_description", ""));
    const char *icon = json_str(d, "community_icon", NULL);
    if (!is_http(icon)) icon = json_str(d, "icon_img", NULL);
    s->icon = unescape_amp(is_http(icon) ? icon : "");
    s->subscribers = (int)json_num(d, "subscribers", 0);
    s->nsfw = json_bool(d, "over18", false);
}

static int archive_search(const char *q, SubList *out, char *err, size_t errlen)
{
    char enc[192], url[384];
    url_encode(q, enc, sizeof(enc));
    snprintf(url, sizeof(url), ARCTIC_BASE "/subreddits/search?subreddit_prefix=%s&limit=25", enc);
    Buf b;
    long code = 0;
    if (fetch(url, NULL, ACCEPT_JSON, &b, &code, err, errlen)) return -1;
    JVal *j = json_parse(b.data);
    buf_free(&b);
    JVal *data = json_get(j, "data");
    if (!data || data->type != J_ARR) {
        snprintf(err, errlen, TR("Archive : réponse inattendue", "Archive: unexpected response"));
        json_free(j);
        return -1;
    }
    for (JVal *c = data->child; c; c = c->next) {
        SubInfo *s = sublist_push(out);
        if (!s) break;
        fill_sub_json(s, c);
    }
    json_free(j);
    snprintf(s_source, sizeof(s_source), "Archive Arctic Shift");
    return 0;
}

static int oauth_search(const char *q, SubList *out, char *err, size_t errlen)
{
    char enc[192], query[256];
    url_encode(q, enc, sizeof(enc));
    snprintf(query, sizeof(query), "q=%s&limit=25&raw_json=1", enc);
    JVal *j = oauth_get("/subreddits/search", query, err, errlen);
    if (!j) return -1;
    JVal *children = json_get(json_get(j, "data"), "children");
    for (JVal *c = children ? children->child : NULL; c; c = c->next) {
        SubInfo *s = sublist_push(out);
        if (!s) break;
        fill_sub_json(s, json_get(c, "data"));
    }
    json_free(j);
    snprintf(s_source, sizeof(s_source), "API Reddit");
    return 0;
}

int reddit_search_subs(const char *query, SubList *out, char *err, size_t errlen)
{
    if (s_client_id[0]) return oauth_search(query, out, err, errlen);

    char last_err[160] = "";
    if (s_mode != MODE_ARCHIVE) {
        for (int k = 0; k < s_ninstances; k++) {
            int i = (s_good_instance + k) % s_ninstances;
            sublist_clear(out);
            if (redlib_search_from(s_instances[i], query, out, last_err, sizeof(last_err)) == 0) {
                s_good_instance = i;
                return 0;
            }
        }
    }
    sublist_clear(out);
    if (s_mode == MODE_REDLIB) {
        snprintf(err, errlen, TR("Aucune instance Redlib ne répond (%s).", "No Redlib instance is responding (%s)."), last_err);
        return -1;
    }
    return archive_search(query, out, err, errlen);
}
