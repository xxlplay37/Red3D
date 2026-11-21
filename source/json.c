#include "json.h"
#include <stdlib.h>
#include <string.h>

#define JSON_MAX_DEPTH 256

typedef struct {
    const char *p;
    int depth;
} Parser;

static void skip_ws(Parser *s)
{
    while (*s->p == ' ' || *s->p == '\t' || *s->p == '\n' || *s->p == '\r')
        s->p++;
}

static JVal *new_val(JType t)
{
    JVal *v = calloc(1, sizeof(*v));
    if (v) v->type = t;
    return v;
}

static int hex4(const char *p)
{
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return -1; /* s'arrête aussi sur '\0' : pas de débordement */
    }
    return v;
}

static int utf8_encode(char *o, unsigned cp)
{
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        o[0] = (char)(0xC0 | (cp >> 6));
        o[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | (cp >> 12));
        o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        o[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    o[0] = (char)(0xF0 | (cp >> 18));
    o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* s->p pointe sur le '"' ouvrant. */
static char *parse_string(Parser *s)
{
    const char *start = s->p + 1;
    const char *q = start;
    while (*q && *q != '"') {
        if (*q == '\\') {
            q++;
            if (!*q) return NULL;
        }
        q++;
    }
    if (*q != '"') return NULL;

    /* La version décodée n'est jamais plus longue que la source. */
    char *out = malloc((size_t)(q - start) + 1);
    if (!out) return NULL;

    char *o = out;
    const char *p = start;
    while (p < q) {
        char c = *p++;
        if (c != '\\') { *o++ = c; continue; }
        c = *p++;
        switch (c) {
        case 'b': *o++ = '\b'; break;
        case 'f': *o++ = '\f'; break;
        case 'n': *o++ = '\n'; break;
        case 'r': *o++ = '\r'; break;
        case 't': *o++ = '\t'; break;
        case 'u': {
            int cp = (p + 4 <= q) ? hex4(p) : -1;
            if (cp < 0) { *o++ = '?'; break; }
            p += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                int lo = (p + 6 <= q && p[0] == '\\' && p[1] == 'u') ? hex4(p + 2) : -1;
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    p += 6;
                } else {
                    cp = 0xFFFD;
                }
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                cp = 0xFFFD;
            }
            o += utf8_encode(o, (unsigned)cp);
            break;
        }
        default: *o++ = c; break; /* \" \\ \/ */
        }
    }
    *o = '\0';
    s->p = q + 1;
    return out;
}

static JVal *parse_value(Parser *s);

static JVal *parse_object(Parser *s)
{
    JVal *v = new_val(J_OBJ);
    if (!v) return NULL;
    JVal **tail = &v->child;
    s->p++; /* '{' */
    skip_ws(s);
    if (*s->p == '}') { s->p++; return v; }
    for (;;) {
        skip_ws(s);
        if (*s->p != '"') goto fail;
        char *key = parse_string(s);
        if (!key) goto fail;
        skip_ws(s);
        if (*s->p != ':') { free(key); goto fail; }
        s->p++;
        JVal *c = parse_value(s);
        if (!c) { free(key); goto fail; }
        c->key = key;
        *tail = c;
        tail = &c->next;
        skip_ws(s);
        if (*s->p == ',') { s->p++; continue; }
        if (*s->p == '}') { s->p++; return v; }
        goto fail;
    }
fail:
    json_free(v);
    return NULL;
}

static JVal *parse_array(Parser *s)
{
    JVal *v = new_val(J_ARR);
    if (!v) return NULL;
    JVal **tail = &v->child;
    s->p++; /* '[' */
    skip_ws(s);
    if (*s->p == ']') { s->p++; return v; }
    for (;;) {
        JVal *c = parse_value(s);
        if (!c) goto fail;
        *tail = c;
        tail = &c->next;
        skip_ws(s);
        if (*s->p == ',') { s->p++; continue; }
        if (*s->p == ']') { s->p++; return v; }
        goto fail;
    }
fail:
    json_free(v);
    return NULL;
}

static JVal *parse_value(Parser *s)
{
    JVal *v = NULL;
    skip_ws(s);
    if (++s->depth > JSON_MAX_DEPTH) return NULL;

    switch (*s->p) {
    case '{': v = parse_object(s); break;
    case '[': v = parse_array(s); break;
    case '"': {
        char *str = parse_string(s);
        if (!str) break;
        v = new_val(J_STR);
        if (v) v->str = str; else free(str);
        break;
    }
    case 't':
        if (!strncmp(s->p, "true", 4)) { s->p += 4; v = new_val(J_BOOL); if (v) v->b = true; }
        break;
    case 'f':
        if (!strncmp(s->p, "false", 5)) { s->p += 5; v = new_val(J_BOOL); }
        break;
    case 'n':
        if (!strncmp(s->p, "null", 4)) { s->p += 4; v = new_val(J_NULL); }
        break;
    default: {
        char *end;
        double d = strtod(s->p, &end);
        if (end != s->p) {
            s->p = end;
            v = new_val(J_NUM);
            if (v) v->num = d;
        }
        break;
    }
    }
    s->depth--;
    return v;
}

JVal *json_parse(const char *text)
{
    if (!text) return NULL;
    Parser s = { text, 0 };
    return parse_value(&s);
}

void json_free(JVal *v)
{
    while (v) {
        JVal *next = v->next;
        json_free(v->child);
        free(v->key);
        free(v->str);
        free(v);
        v = next;
    }
}

JVal *json_get(const JVal *obj, const char *key)
{
    if (!obj || obj->type != J_OBJ) return NULL;
    for (JVal *c = obj->child; c; c = c->next)
        if (c->key && !strcmp(c->key, key)) return c;
    return NULL;
}

JVal *json_idx(const JVal *arr, int i)
{
    if (!arr || arr->type != J_ARR) return NULL;
    JVal *c = arr->child;
    while (c && i-- > 0) c = c->next;
    return c;
}

const char *json_str(const JVal *obj, const char *key, const char *def)
{
    JVal *v = json_get(obj, key);
    return (v && v->type == J_STR) ? v->str : def;
}

double json_num(const JVal *obj, const char *key, double def)
{
    JVal *v = json_get(obj, key);
    return (v && v->type == J_NUM) ? v->num : def;
}

bool json_bool(const JVal *obj, const char *key, bool def)
{
    JVal *v = json_get(obj, key);
    return (v && v->type == J_BOOL) ? v->b : def;
}
