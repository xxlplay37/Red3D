#include "html.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

const char *html_find(const char *s, const char *end, const char *needle)
{
    size_t n = strlen(needle);
    if (!s || !end || n == 0) return NULL;
    for (const char *p = s; p + n <= end; p++) {
        p = memchr(p, needle[0], (size_t)(end - p));
        if (!p || p + n > end) return NULL;
        if (!memcmp(p, needle, n)) return p;
    }
    return NULL;
}

char *html_copy_after(const char *s, const char *end, const char *marker, char stop)
{
    const char *p = html_find(s, end, marker);
    if (!p) return NULL;
    p += strlen(marker);
    const char *q = memchr(p, stop, (size_t)(end - p));
    if (!q) return NULL;
    char *r = malloc((size_t)(q - p) + 1);
    if (!r) return NULL;
    memcpy(r, p, (size_t)(q - p));
    r[q - p] = '\0';
    return r;
}

/* ------------------------------------------------------------------ */

typedef struct {
    char *buf;
    size_t len, cap;
    bool line_start; /* on vient d'émettre un saut de ligne (ou début) */
} Out;

static void out_putc(Out *o, char c)
{
    if (o->len + 1 >= o->cap) return; /* capacité calculée large à l'avance */
    o->buf[o->len++] = c;
}

static void out_puts(Out *o, const char *s)
{
    while (*s) out_putc(o, *s++);
    o->line_start = false;
}

static void out_newlines(Out *o, int want)
{
    while (o->len > 0 && o->buf[o->len - 1] == ' ') o->len--;
    if (o->len == 0) { o->line_start = true; return; }
    int have = 0;
    for (size_t i = o->len; i > 0 && o->buf[i - 1] == '\n'; i--) have++;
    while (have++ < want) out_putc(o, '\n');
    o->line_start = true;
}

static void out_utf8(Out *o, unsigned cp)
{
    if (cp == 0 || cp > 0x10FFFF) cp = '?';
    if (cp < 0x80) { out_putc(o, (char)cp); }
    else if (cp < 0x800) {
        out_putc(o, (char)(0xC0 | (cp >> 6)));
        out_putc(o, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out_putc(o, (char)(0xE0 | (cp >> 12)));
        out_putc(o, (char)(0x80 | ((cp >> 6) & 0x3F)));
        out_putc(o, (char)(0x80 | (cp & 0x3F)));
    } else {
        out_putc(o, (char)(0xF0 | (cp >> 18)));
        out_putc(o, (char)(0x80 | ((cp >> 12) & 0x3F)));
        out_putc(o, (char)(0x80 | ((cp >> 6) & 0x3F)));
        out_putc(o, (char)(0x80 | (cp & 0x3F)));
    }
    o->line_start = false;
}

static const struct { const char *name; unsigned cp; } ENTITIES[] = {
    { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' },
    { "nbsp", ' ' }, { "bull", 0x2022 }, { "rarr", 0x2192 }, { "larr", 0x2190 },
    { "hellip", 0x2026 }, { "mdash", 0x2014 }, { "ndash", 0x2013 }, { "rsquo", 0x2019 },
    { "lsquo", 0x2018 }, { "ldquo", 0x201C }, { "rdquo", 0x201D }, { "middot", 0xB7 },
    { "copy", 0xA9 }, { "reg", 0xAE }, { "trade", 0x2122 }, { "euro", 0x20AC },
    { "times", 0xD7 }, { "deg", 0xB0 },
};

/* p pointe sur '&'. Retourne le nombre d'octets consommés. */
static size_t decode_entity(Out *o, const char *p, const char *end)
{
    const char *semi = memchr(p, ';', (size_t)(end - p) < 12 ? (size_t)(end - p) : 12);
    if (!semi) { out_putc(o, '&'); o->line_start = false; return 1; }
    const char *name = p + 1;
    size_t n = (size_t)(semi - name);
    if (n >= 2 && name[0] == '#') {
        unsigned cp = (name[1] == 'x' || name[1] == 'X')
                      ? (unsigned)strtoul(name + 2, NULL, 16)
                      : (unsigned)strtoul(name + 1, NULL, 10);
        out_utf8(o, cp);
        return (size_t)(semi - p) + 1;
    }
    for (size_t i = 0; i < sizeof(ENTITIES) / sizeof(ENTITIES[0]); i++) {
        if (strlen(ENTITIES[i].name) == n && !memcmp(ENTITIES[i].name, name, n)) {
            if (ENTITIES[i].cp == ' ') {
                if (!o->line_start) out_putc(o, ' ');
            } else {
                out_utf8(o, ENTITIES[i].cp);
            }
            return (size_t)(semi - p) + 1;
        }
    }
    out_putc(o, '&');
    o->line_start = false;
    return 1;
}

static bool tag_is(const char *t, size_t tl, const char *name)
{
    return strlen(name) == tl && !strncasecmp(t, name, tl);
}

char *html_to_text(const char *s, size_t n)
{
    Out o = { malloc(n * 2 + 16), 0, n * 2 + 16, true };
    if (!o.buf) return NULL;
    const char *p = s, *end = s + n;

    while (p < end) {
        char c = *p;
        if (c == '<') {
            const char *q = memchr(p, '>', (size_t)(end - p));
            if (!q) break;
            const char *t = p + 1;
            bool closing = (*t == '/');
            if (closing) t++;
            size_t tl = 0;
            while (t + tl < q && isalnum((unsigned char)t[tl])) tl++;

            if (tag_is(t, tl, "br")) {
                while (o.len > 0 && o.buf[o.len - 1] == ' ') o.len--;
                if (o.len > 0) out_putc(&o, '\n');
                o.line_start = true;
            } else if (tag_is(t, tl, "p") || tag_is(t, tl, "blockquote") || tag_is(t, tl, "ul") ||
                       tag_is(t, tl, "ol") || tag_is(t, tl, "pre") || tag_is(t, tl, "table") ||
                       tag_is(t, tl, "h1") || tag_is(t, tl, "h2") || tag_is(t, tl, "h3") ||
                       tag_is(t, tl, "h4") || tag_is(t, tl, "h5") || tag_is(t, tl, "h6") ||
                       tag_is(t, tl, "hr")) {
                out_newlines(&o, 2);
            } else if (tag_is(t, tl, "li")) {
                out_newlines(&o, 1);
                if (!closing) { out_puts(&o, "\xE2\x80\xA2 "); o.line_start = true; }
            } else if (tag_is(t, tl, "div") || tag_is(t, tl, "tr")) {
                out_newlines(&o, 1);
            }
            p = q + 1;
            continue;
        }
        if (c == '&') {
            p += decode_entity(&o, p, end);
            continue;
        }
        p++;
        if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
            if (!o.line_start && o.len > 0 && o.buf[o.len - 1] != ' ') out_putc(&o, ' ');
            continue;
        }
        out_putc(&o, c);
        o.line_start = false;
    }

    while (o.len > 0 && (o.buf[o.len - 1] == ' ' || o.buf[o.len - 1] == '\n')) o.len--;
    o.buf[o.len] = '\0';
    return o.buf;
}

/* ------------------------------------------------------------------ */

static long days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

double html_parse_date(const char *s)
{
    static const char *MONTHS = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4] = "";
    int d, y, h, mi, se;
    if (!s || sscanf(s, "%3s %d %d, %d:%d:%d", mon, &d, &y, &h, &mi, &se) != 6) return 0;
    const char *f = strstr(MONTHS, mon);
    if (!f || (f - MONTHS) % 3) return 0;
    int m = (int)(f - MONTHS) / 3 + 1;
    return (double)days_from_civil(y, m, d) * 86400.0 + h * 3600 + mi * 60 + se;
}
