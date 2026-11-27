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
    float ratio;  /* proportion de votes positifs (0..1), 0 si inconnue */
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
    float ratio; /* ratio de votes positifs du post (lu sur sa page), 0 si inconnu */
} Comments;

/* Votes estimés à partir du score et du ratio (Reddit ne publie plus le détail) :
 * ups - downs = score, ups / (ups + downs) = ratio. Faux si le ratio est inconnu ou vaut 50 %. */
bool reddit_estimate_votes(int score, float ratio, int *ups, int *downs);

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

/* Instances testées le 26/11/2026 (renvoient des posts sans anti-robot), de la plus rapide à la plus lente.
 * safereddit.com : SFW seulement, et Reddit lui refuse parfois les pages de post. */
#define REDDIT_DEFAULT_INSTANCES \
    "redlib.tiekoetter.com,redlib.reallyaweso.me,redlib.utilibre.org,redlib.hbubli.cc," \
    "redlib.zanity.net,safereddit.com"
/* Ancienne liste par défaut : remplacée automatiquement si elle est encore dans config.ini */
#define REDDIT_OLD_DEFAULT_INSTANCES \
    "redlib.tiekoetter.com,safereddit.com,redlib.r4fo.com,redlib.ducks.party," \
    "redlib.4o1x5.dev,l.opnxng.com,redlib.catsarch.com,redlib.perennialte.ch"

#define REDDIT_MAX_INSTANCES 12

/* mode : 0 auto, 1 Redlib seulement, 2 archive seulement.
 * server : instance Redlib imposée ("" = automatique, on essaie la liste dans l'ordre). */
void reddit_configure(const char *client_id, const char *instances, int mode, const char *server);

/* Liste des instances Redlib configurées */
int reddit_instance_count(void);
const char *reddit_instance(int i);

/* Statut d'une instance Redlib (test : chargement de r/popular) */
enum { INST_UNTESTED = 0, INST_OK = 1, INST_BLOCKED = 2, INST_DOWN = 3 };
typedef struct {
    char host[64];
    int state;     /* INST_* */
    int ms;        /* temps de réponse */
    int posts;     /* posts trouvés sur la page */
    char err[96];
} InstStatus;
/* Teste chaque instance ; out[0..*progress) est prêt au fur et à mesure. Renvoie le nombre d'instances. */
int reddit_check_instances(InstStatus *out, int max, volatile int *progress);
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
