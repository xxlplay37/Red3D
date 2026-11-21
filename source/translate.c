#include "translate.h"
#include "json.h"
#include "net.h"
#include "settings.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Service principal : point d'accès public de Google Traduction (sans clé), en POST,
 * plusieurs textes par requête. Secours : MyMemory (un texte à la fois, 500 caractères max).
 */
#define GTX_URL       "https://translate.googleapis.com/translate_a/t?client=gtx&sl=auto&tl=%s"
#define BATCH_ITEMS   25
#define BATCH_CHARS   4500

static bool blank(const char *s)
{
    if (!s) return true;
    for (; *s; s++)
        if (!isspace((unsigned char)*s)) return false;
    return true;
}

static char *dup(const char *s)
{
    size_t n = strlen(s);
    char *d = malloc(n + 1);
    if (d) memcpy(d, s, n + 1);
    return d;
}

/* Ajoute "&q=<texte encodé>" au corps de requête */
static bool body_add(char **body, size_t *len, size_t *cap, const char *text)
{
    size_t need = strlen(text) * 3 + 8;
    if (!*body || *len + need + 1 > *cap) {
        size_t nc = (*cap ? *cap : 4096);
        while (nc < *len + need + 1) nc *= 2;
        char *b = realloc(*body, nc);
        if (!b) return false;
        *body = b;
        *cap = nc;
    }
    if (*len) (*body)[(*len)++] = '&';
    memcpy(*body + *len, "q=", 2);
    *len += 2;
    url_encode(text, *body + *len, *cap - *len);
    *len += strlen(*body + *len);
    return true;
}

/* Google : traduit les éléments idx[0..k) en une requête */
static int gtx_batch(struct NetConn *c, char *const *in, const int *idx, int k, const char *tl,
                     char **out, char *err, size_t errlen)
{
    char *body = NULL;
    size_t len = 0, cap = 0;
    for (int i = 0; i < k; i++)
        if (!body_add(&body, &len, &cap, in[idx[i]])) { free(body); snprintf(err, errlen, "mémoire"); return -1; }

    char url[160];
    snprintf(url, sizeof(url), GTX_URL, tl);
    Buf b = {0};
    long code = 0;
    int r = http_request(c, url, NULL, NULL, body ? body : "", ACCEPT_JSON, 0, &b, &code, err, errlen);
    free(body);
    if (r) { buf_free(&b); return -1; }
    if (code != 200 || !b.data) {
        snprintf(err, errlen, "%s %ld", TR("Traduction : erreur HTTP", "Translation: HTTP error"), code);
        buf_free(&b);
        return -1;
    }
    JVal *j = json_parse(b.data);
    buf_free(&b);
    if (!j || j->type != J_ARR) {
        json_free(j);
        snprintf(err, errlen, "%s", TR("Traduction : réponse invalide", "Translation: invalid response"));
        return -1;
    }
    /* [["texte traduit","langue source"], ...] (ou chaînes simples) */
    int i = 0;
    for (JVal *e = j->child; e && i < k; e = e->next, i++) {
        const JVal *t = e->type == J_ARR ? json_idx(e, 0) : e;
        if (t && t->type == J_STR) out[idx[i]] = dup(t->str);
    }
    json_free(j);
    return i == k ? 0 : -1;
}

/* MyMemory : un texte, tronqué à ~480 octets (limite du service anonyme) */
static void mymemory_one(struct NetConn *c, const char *text, const char *tl, char **out)
{
    char part[488], enc[1600], url[1800], err[96];
    size_t n = strlen(text);
    if (n > sizeof(part) - 1) {
        n = sizeof(part) - 1;
        while (n > 0 && ((unsigned char)text[n] & 0xC0) == 0x80) n--; /* coupe sur un caractère entier */
    }
    memcpy(part, text, n);
    part[n] = '\0';
    url_encode(part, enc, sizeof(enc));
    snprintf(url, sizeof(url), "https://api.mymemory.translated.net/get?q=%s&langpair=autodetect%%7C%s", enc, tl);
    Buf b = {0};
    long code = 0;
    if (http_request(c, url, NULL, NULL, NULL, ACCEPT_JSON, 0, &b, &code, err, sizeof(err)) == 0 && code == 200) {
        JVal *j = json_parse(b.data);
        const char *t = json_str(json_get(j, "responseData"), "translatedText", NULL);
        if (t && *t && !json_bool(j, "quotaFinished", false)) *out = dup(t);
        json_free(j);
    }
    buf_free(&b);
}

int translate_texts(struct NetConn *c, char *const *in, int n, const char *tl, char **out,
                    volatile int *progress, char *err, size_t errlen)
{
    int idx[BATCH_ITEMS];
    int pos = 0, failures = 0;
    bool use_fallback = false;

    while (pos < n) {
        /* lot suivant : textes non vides, dans l'ordre */
        int k = 0, end = pos;
        size_t chars = 0;
        while (end < n && k < BATCH_ITEMS) {
            if (!blank(in[end])) {
                size_t l = strlen(in[end]);
                if (k > 0 && chars + l > BATCH_CHARS) break;
                idx[k++] = end;
                chars += l;
            }
            end++;
        }

        if (k > 0) {
            int r = -1;
            if (!use_fallback) {
                r = gtx_batch(c, in, idx, k, tl, out, err, errlen);
                if (r && !strcmp(err, "Annulé")) return -1;
                if (r) use_fallback = true; /* Google indisponible : on bascule pour la suite */
            }
            if (r) {
                for (int i = 0; i < k; i++)
                    if (!out[idx[i]]) mymemory_one(c, in[idx[i]], tl, &out[idx[i]]);
                for (int i = 0; i < k; i++)
                    if (!out[idx[i]]) failures++;
            }
        }
        pos = end;
        __sync_synchronize(); /* résultats écrits avant d'annoncer la progression */
        *progress = pos;
    }
    if (failures) {
        snprintf(err, errlen, TR("%d texte(s) n'ont pas pu être traduits", "%d text(s) couldn't be translated"),
                 failures);
        return -1;
    }
    return 0;
}
