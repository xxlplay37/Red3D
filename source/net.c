#include "net.h"

#include <3ds.h>
#include <curl/curl.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SOC_BUFSIZE 0x100000

struct NetConn {
    CURL *curl;
    volatile bool *cancel;
    volatile long long dl_total;
    long long resume_base;
    int last_res; /* dernier code CURLcode, pour le repli sur le bundle complet */
};

typedef struct {
    Buf *buf;
    size_t max;
} WriteCtx;

static u32 *s_soc_buf;

void buf_free(Buf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

static void fullca_init(void);

int net_init(void)
{
    fullca_init();
    s_soc_buf = memalign(0x1000, SOC_BUFSIZE);
    if (!s_soc_buf) return -1;
    if (R_FAILED(socInit(s_soc_buf, SOC_BUFSIZE))) {
        free(s_soc_buf);
        s_soc_buf = NULL;
        return -2;
    }
    if (curl_global_init(CURL_GLOBAL_ALL) != CURLE_OK) return -3;
    return 0;
}

void net_exit(void)
{
    curl_global_cleanup();
    if (s_soc_buf) {
        socExit();
        free(s_soc_buf);
        s_soc_buf = NULL;
    }
}

NetConn *net_conn_new(volatile bool *cancel)
{
    NetConn *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->curl = curl_easy_init();
    c->cancel = cancel;
    if (!c->curl) { free(c); return NULL; }
    return c;
}

void net_conn_set_cancel(NetConn *c, volatile bool *cancel)
{
    if (c) c->cancel = cancel;
}

long long net_conn_total(const NetConn *c)
{
    return c ? c->dl_total : 0;
}

void net_conn_free(NetConn *c)
{
    if (!c) return;
    curl_easy_cleanup(c->curl);
    free(c);
}

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    WriteCtx *w = userdata;
    Buf *b = w->buf;
    size_t n = size * nmemb;
    if (b->len + n + 1 > w->max) return 0;
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 32 * 1024;
        while (nc < b->len + n + 1) nc *= 2;
        char *d = realloc(b->data, nc);
        if (!d) return 0;
        b->data = d;
        b->cap = nc;
    }
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = '\0';
    return n;
}

static int progress_cb(void *userdata, curl_off_t dltotal, curl_off_t dlnow,
                       curl_off_t ultotal, curl_off_t ulnow)
{
    (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
    NetConn *c = userdata;
    if (dltotal > 0) c->dl_total = (long long)dltotal + c->resume_base;
    return (c->cancel && *c->cancel) ? 1 : 0;
}

static int http_request_once(NetConn *c, const char *url, const char *bearer, const char *basic_user,
                             const char *post, const char *accept, size_t max_size,
                             Buf *out, long *status, char *err, size_t errlen, bool full_ca)
{
    *status = 0;
    if (!c) {
        snprintf(err, errlen, "Réseau non initialisé");
        return -1;
    }
    CURL *h = c->curl;
    curl_easy_reset(h);
    c->dl_total = 0;
    c->resume_base = 0;

    char errbuf[CURL_ERROR_SIZE];
    errbuf[0] = '\0';
    char hdr_accept[128], auth[600];
    struct curl_slist *headers = NULL;

    snprintf(hdr_accept, sizeof(hdr_accept), "Accept: %s", accept ? accept : ACCEPT_JSON);
    headers = curl_slist_append(headers, hdr_accept);
    if (bearer) {
        snprintf(auth, sizeof(auth), "Authorization: bearer %s", bearer);
        headers = curl_slist_append(headers, auth);
    }

    WriteCtx wctx = { out, max_size ? max_size : 8 * 1024 * 1024 };

    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_USERAGENT, NET_USER_AGENT);
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(h, CURLOPT_CAINFO, full_ca ? NET_CA_PATH : NET_CA_SMALL);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 12L);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, 45L);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(h, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &wctx);
    curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, progress_cb);
    curl_easy_setopt(h, CURLOPT_XFERINFODATA, c);
    curl_easy_setopt(h, CURLOPT_BUFFERSIZE, 64L * 1024);

    if (basic_user) {
        curl_easy_setopt(h, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);
        curl_easy_setopt(h, CURLOPT_USERNAME, basic_user);
        curl_easy_setopt(h, CURLOPT_PASSWORD, "");
    }
    if (post) curl_easy_setopt(h, CURLOPT_POSTFIELDS, post);

    CURLcode res = curl_easy_perform(h);
    c->last_res = (int)res;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, status);
    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        if (res == CURLE_ABORTED_BY_CALLBACK) snprintf(err, errlen, "Annulé");
        else snprintf(err, errlen, "Réseau : %s", errbuf[0] ? errbuf : curl_easy_strerror(res));
        return -1;
    }
    return 0;
}

void url_encode(const char *in, char *out, size_t outlen)
{
    static const char *hex = "0123456789ABCDEF";
    size_t o = 0;
    for (; *in && o + 4 < outlen; in++) {
        unsigned char c = (unsigned char)*in;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }
    out[o] = '\0';
}

typedef struct {
    NetDataFn fn;
    void *ud;
} StreamCtx;

static size_t stream_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    StreamCtx *s = userdata;
    return s->fn(ptr, size * nmemb, s->ud);
}

static int http_stream_once(NetConn *c, const char *url, long long resume_from, NetDataFn on_data, void *userdata,
                            long *status, char *err, size_t errlen, bool full_ca)
{
    *status = 0;
    if (!c) {
        snprintf(err, errlen, "Réseau non initialisé");
        return -1;
    }
    CURL *h = c->curl;
    curl_easy_reset(h);
    c->dl_total = 0;
    c->resume_base = resume_from > 0 ? resume_from : 0;
    char errbuf[CURL_ERROR_SIZE];
    errbuf[0] = '\0';
    StreamCtx sc = { on_data, userdata };

    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_USERAGENT, NET_USER_AGENT);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(h, CURLOPT_CAINFO, full_ca ? NET_CA_PATH : NET_CA_SMALL);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, stream_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &sc);
    curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, progress_cb);
    curl_easy_setopt(h, CURLOPT_XFERINFODATA, c);
    curl_easy_setopt(h, CURLOPT_BUFFERSIZE, 64L * 1024);
    if (resume_from > 0) curl_easy_setopt(h, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)resume_from);

    CURLcode res = curl_easy_perform(h);
    c->last_res = (int)res;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, status);
    if (res != CURLE_OK) {
        if (res == CURLE_ABORTED_BY_CALLBACK || res == CURLE_WRITE_ERROR) snprintf(err, errlen, "Annulé");
        else snprintf(err, errlen, "Réseau : %s", errbuf[0] ? errbuf : curl_easy_strerror(res));
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Certificats : jeu réduit d'abord (chargement TLS plus rapide), bundle complet si l'hôte n'y est pas */

#define FULLCA_HOSTS 16
static char s_fullca_host[FULLCA_HOSTS][64];
static int s_fullca_n;
static LightLock s_fullca_lock;
static bool s_fullca_init;

static void url_host(const char *url, char *host, size_t n)
{
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    size_t i = 0;
    while (*p && *p != '/' && *p != ':' && *p != '?' && i + 1 < n) host[i++] = *p++;
    host[i] = '\0';
}

static bool host_needs_full(const char *url)
{
    char h[64];
    url_host(url, h, sizeof(h));
    bool yes = false;
    if (!s_fullca_init) return false;
    LightLock_Lock(&s_fullca_lock);
    for (int i = 0; i < s_fullca_n; i++)
        if (!strcmp(s_fullca_host[i], h)) { yes = true; break; }
    LightLock_Unlock(&s_fullca_lock);
    return yes;
}

static void host_mark_full(const char *url)
{
    char h[64];
    url_host(url, h, sizeof(h));
    if (!s_fullca_init || !h[0]) return;
    LightLock_Lock(&s_fullca_lock);
    if (s_fullca_n < FULLCA_HOSTS) snprintf(s_fullca_host[s_fullca_n++], sizeof(s_fullca_host[0]), "%s", h);
    LightLock_Unlock(&s_fullca_lock);
}

/* Le certificat n'a pas pu être vérifié (ou le fichier manque) : on retente avec le bundle complet */
static bool ca_retry_wanted(const NetConn *c, bool was_full)
{
    return !was_full && c && (c->last_res == CURLE_PEER_FAILED_VERIFICATION || c->last_res == CURLE_SSL_CACERT_BADFILE);
}

int http_request(NetConn *c, const char *url, const char *bearer, const char *basic_user,
                 const char *post, const char *accept, size_t max_size,
                 Buf *out, long *status, char *err, size_t errlen)
{
    bool full = host_needs_full(url);
    int r = http_request_once(c, url, bearer, basic_user, post, accept, max_size, out, status, err, errlen, full);
    if (r != 0 && ca_retry_wanted(c, full)) {
        host_mark_full(url);
        r = http_request_once(c, url, bearer, basic_user, post, accept, max_size, out, status, err, errlen, true);
    }
    return r;
}

int http_stream(NetConn *c, const char *url, long long resume_from, NetDataFn on_data, void *userdata,
                long *status, char *err, size_t errlen)
{
    bool full = host_needs_full(url);
    int r = http_stream_once(c, url, resume_from, on_data, userdata, status, err, errlen, full);
    if (r != 0 && ca_retry_wanted(c, full)) {
        host_mark_full(url);
        r = http_stream_once(c, url, resume_from, on_data, userdata, status, err, errlen, true);
    }
    return r;
}

static void fullca_init(void)
{
    LightLock_Init(&s_fullca_lock);
    s_fullca_init = true;
}
