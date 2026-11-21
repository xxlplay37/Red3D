#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char id[16];
    char video_id[32]; /* identifiant v.redd.it ("" si pas de vidéo Reddit) */
    char *title, *author, *sub, *selftext, *url, *domain, *flair;
    char *thumb;  /* petite image (miniature), "" si aucune */
    char *image;  /* image principale (aperçu / plein écran), "" si aucune */
    int score, ncomments;
    double created;
    bool is_self, nsfw, stickied, is_video, is_gallery;
} Post;

typedef struct {
    Post *items;
    int n, cap;
    char after[32]; /* curseur de pagination, "" si fin */
} Feed;

typedef struct {
    char *author, *body;
    int score, depth;
    bool more; /* "N réponses de plus" (non chargées) */
    bool op;   /* auteur du post */
} Comment;

typedef struct {
    Comment *items;
    int n, cap;
} Comments;

typedef struct {
    char *name, *desc, *icon;
    int subscribers;
    bool nsfw;
} SubInfo;

typedef struct {
    SubInfo *items;
    int n, cap;
} SubList;

struct NetConn;

#define REDDIT_DEFAULT_INSTANCES \
    "redlib.tiekoetter.com,safereddit.com,redlib.r4fo.com,redlib.ducks.party," \
    "redlib.4o1x5.dev,l.opnxng.com,redlib.catsarch.com,redlib.perennialte.ch"

/* mode : 0 auto, 1 Redlib seulement, 2 archive seulement */
void reddit_configure(const char *client_id, const char *instances, int mode);
/* Connexion utilisée par les requêtes (thread de travail). */
void reddit_attach(struct NetConn *conn);
bool reddit_using_oauth(void);
/* Source utilisée pour la dernière requête réussie (ex. "Redlib (xxx)") */
const char *reddit_source(void);

/* last_created : date du dernier post déjà affiché (pagination de l'archive), 0 sinon */
int reddit_fetch_feed(const char *sub, const char *sort, const char *after, double last_created,
                      Feed *feed, char *err, size_t errlen);
int reddit_search_subs(const char *query, SubList *out, char *err, size_t errlen);
int reddit_fetch_comments(const Post *post, Comments *out, char *err, size_t errlen);

void feed_clear(Feed *f);
void comments_clear(Comments *c);
void sublist_clear(SubList *l);
/* Déplace les posts de src à la fin de dst (src est vidé). */
void feed_append(Feed *dst, Feed *src);
/* Retire le post idx */
void feed_remove(Feed *f, int idx);
