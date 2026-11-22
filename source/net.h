#pragma once
#include <stdbool.h>
#include <stddef.h>

#define NET_USER_AGENT "3ds:reddit3ds:v2.0 (homebrew Nintendo 3DS client)"
#define NET_CA_PATH    "romfs:/cacert.pem"        /* bundle complet (~118 autorités) */
#define NET_CA_SMALL   "romfs:/cacert_small.pem"  /* jeu réduit : TLS démarre bien plus vite ; repli sur le complet */

#define ACCEPT_JSON  "application/json"
#define ACCEPT_HTML  "text/html,application/xhtml+xml"
#define ACCEPT_IMAGE "image/jpeg,image/png;q=0.9,*/*;q=0.1" /* surtout pas de webp */

typedef struct {
    char *data;
    size_t len, cap;
} Buf;

void buf_free(Buf *b);

int  net_init(void);
void net_exit(void);

/* Une connexion par thread (handle curl réutilisé => keep-alive TLS). */
typedef struct NetConn NetConn;
NetConn *net_conn_new(volatile bool *cancel); /* cancel : si *cancel devient vrai, la requête s'interrompt */
void     net_conn_free(NetConn *c);
/* Taille totale (octets) du téléchargement en cours, 0 si inconnue (en-tête Content-Length reçu) */
long long net_conn_total(const NetConn *c);

/*
 * Requête HTTP(S) bloquante.
 *  bearer     : jeton OAuth (ou NULL)
 *  basic_user : utilisateur pour l'auth Basic, mot de passe vide (ou NULL)
 *  post       : corps x-www-form-urlencoded => POST (ou NULL => GET)
 *  accept     : en-tête Accept (ou NULL => JSON)
 * Retourne 0 si la requête a abouti (quel que soit le code HTTP, cf. *status).
 */
int http_request(NetConn *c, const char *url, const char *bearer, const char *basic_user,
                 const char *post, const char *accept, size_t max_size,
                 Buf *out, long *status, char *err, size_t errlen);

/* Encode une chaîne pour une URL (résultat dans out). */
void url_encode(const char *in, char *out, size_t outlen);

/*
 * Téléchargement en flux : chaque morceau reçu est passé à on_data (qui peut bloquer).
 * on_data renvoie le nombre d'octets acceptés (moins => interruption).
 * resume_from : reprise à partir de cet octet (requête Range).
 */
typedef size_t (*NetDataFn)(const void *data, size_t len, void *userdata);
int http_stream(NetConn *c, const char *url, long long resume_from, NetDataFn on_data, void *userdata,
                long *status, char *err, size_t errlen);
