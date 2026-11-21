#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static C2D_TextBuf s_buf;
static float s_line_feed = 30.0f;
static float s_ascii_adv[128];
static float s_mul = 1.0f; /* taille du texte (paramètres) */

void ui_set_text_scale(float m)
{
    s_mul = m;
}

static int utf8_next(const char *p, u32 *cp)
{
    const unsigned char *s = (const unsigned char *)p;
    if (s[0] < 0x80) { *cp = s[0]; return 1; }
    if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        *cp = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F);
        return 2;
    }
    if ((s[0] & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        *cp = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        return 3;
    }
    if ((s[0] & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 &&
        (s[3] & 0xC0) == 0x80) {
        *cp = ((s[0] & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        return 4;
    }
    *cp = '?';
    return 1;
}

static float glyph_adv_raw(u32 cp)
{
    int gi = C2D_FontGlyphIndexFromCodePoint(NULL, cp);
    charWidthInfo_s *w = C2D_FontGetCharWidthInfo(NULL, gi);
    return w ? (float)w->charWidth : 0.0f;
}

static float glyph_adv(u32 cp)
{
    if (cp < 128) return s_ascii_adv[cp];
    return glyph_adv_raw(cp);
}

void ui_init(void)
{
    fontEnsureMapped();
    s_buf = C2D_TextBufNew(16384);
    FINF_s *info = C2D_FontGetInfo(NULL);
    if (info && info->lineFeed) s_line_feed = info->lineFeed;
    for (u32 c = 0; c < 128; c++)
        s_ascii_adv[c] = (c < 32) ? 0.0f : glyph_adv_raw(c);
}

void ui_exit(void)
{
    C2D_TextBufDelete(s_buf);
}

u32 g_ui_frame;

void ui_frame_begin(void)
{
    C2D_TextBufClear(s_buf);
    g_ui_frame++;
}

float ui_line_height(float scale)
{
    return s_line_feed * scale * s_mul;
}

float ui_text_width(const char *s, float scale)
{
    scale *= s_mul;
    float w = 0;
    while (*s) {
        u32 cp;
        s += utf8_next(s, &cp);
        w += glyph_adv(cp);
    }
    return w * scale;
}

void ui_text(float x, float y, float scale, u32 color, const char *s)
{
    if (!s || !*s) return;
    scale *= s_mul;
    C2D_Text t;
    C2D_TextParse(&t, s_buf, s);
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor, x, y, 0.5f, scale, scale, color);
}

void ui_bar(float x, float y, float w, float h, const char *left, const char *right)
{
    const float sc = 0.5f;
    float ty = y + (h - ui_line_height(sc)) / 2.0f;
    C2D_DrawRectSolid(x, y, 0.6f, w, h, COL_BAR);
    if (left) ui_text(x + 6, ty, sc, COL_WHITE, left);
    if (right) ui_text(x + w - 6 - ui_text_width(right, sc), ty, sc, COL_DIM, right);
}

/* ------------------------------------------------------------------ */

void doc_free(Doc *d)
{
    for (int i = 0; i < d->nlines; i++) free(d->lines[i].text);
    free(d->lines);
    free(d->items);
    memset(d, 0, sizeof(*d));
}

static void doc_push_line(Doc *d, const char *s, size_t n, float scale, float x, u32 color, u32 bar)
{
    if (d->nlines == d->capl) {
        int nc = d->capl ? d->capl * 2 : 128;
        DocLine *l = realloc(d->lines, nc * sizeof(DocLine));
        if (!l) return;
        d->lines = l;
        d->capl = nc;
    }
    char *t = malloc(n + 1);
    if (!t) return;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\r') continue;
        t[o++] = (c == '\t') ? ' ' : c;
    }
    t[o] = '\0';

    DocLine *l = &d->lines[d->nlines++];
    l->text = t;
    l->x = x;
    l->y = d->h;
    l->scale = scale;
    l->color = color;
    l->bar = bar;
    d->h += ui_line_height(scale);
}

void doc_text(Doc *d, const char *s, float scale, float x, float maxw, u32 color, u32 bar)
{
    if (!s) s = "";
    const char *p = s, *ls = s, *sp = NULL;
    float w = 0, wsp = 0;

    while (*p) {
        u32 cp;
        int n = utf8_next(p, &cp);
        if (cp == '\n') {
            doc_push_line(d, ls, p - ls, scale, x, color, bar);
            p += n;
            ls = p;
            sp = NULL;
            w = 0;
            continue;
        }
        if (cp == '\t') cp = ' ';
        float a = glyph_adv(cp) * scale * s_mul;
        if (w + a > maxw && p > ls) {
            if (sp) {
                doc_push_line(d, ls, sp - ls, scale, x, color, bar);
                ls = sp + 1;
                w -= wsp;
            } else {
                doc_push_line(d, ls, p - ls, scale, x, color, bar);
                ls = p;
                w = 0;
            }
            sp = NULL;
        }
        if (cp == ' ') {
            sp = p;
            wsp = w + a;
        }
        w += a;
        p += n;
    }
    if (p > ls || p == s)
        doc_push_line(d, ls, p - ls, scale, x, color, bar);
}

void doc_space(Doc *d, float px)
{
    d->h += px;
}

void doc_item_begin(Doc *d)
{
    if (d->nitems == d->capi) {
        int nc = d->capi ? d->capi * 2 : 64;
        DocItem *it = realloc(d->items, nc * sizeof(DocItem));
        if (!it) return;
        d->items = it;
        d->capi = nc;
    }
    d->items[d->nitems].top = d->h;
    d->items[d->nitems].bot = d->h;
    d->nitems++;
    d->h += 3; /* marge haute */
}

void doc_item_end(Doc *d)
{
    d->h += 3; /* marge basse */
    if (d->nitems > 0) d->items[d->nitems - 1].bot = d->h;
}

void doc_draw(const Doc *d, float vx, float vy, float vw, float vh, float scroll, int sel)
{
    if (sel >= 0 && sel < d->nitems) {
        float t = d->items[sel].top - scroll + vy;
        float b = d->items[sel].bot - scroll + vy;
        C2D_DrawRectSolid(vx, t, 0.4f, vw, b - t, COL_SEL);
        C2D_DrawRectSolid(vx, t, 0.45f, 3, b - t, COL_ACCENT);
    }
    for (int i = 0; i < d->nlines; i++) {
        const DocLine *l = &d->lines[i];
        float lh = ui_line_height(l->scale);
        float y = l->y - scroll + vy;
        if (y + lh < vy) continue;
        if (y > vy + vh) break;
        if (l->bar) C2D_DrawRectSolid(vx + l->x - 5, y, 0.45f, 2, lh, l->bar);
        ui_text(vx + l->x, y, l->scale, l->color, l->text);
    }
}

void doc_clamp(const Doc *d, float vh, float *scroll)
{
    float max = d->h - vh;
    if (max < 0) max = 0;
    if (*scroll > max) *scroll = max;
    if (*scroll < 0) *scroll = 0;
}

void doc_ensure_visible(const Doc *d, int item, float vh, float *scroll)
{
    if (item < 0 || item >= d->nitems) return;
    const DocItem *it = &d->items[item];
    if (it->top < *scroll) {
        *scroll = it->top;
    } else if (it->bot > *scroll + vh) {
        float s = it->bot - vh;
        *scroll = (s > it->top) ? it->top : s; /* item plus grand que l'écran : on montre le haut */
    }
    doc_clamp(d, vh, scroll);
}

void doc_step(const Doc *d, int dir, float vh, int *sel, float *scroll)
{
    if (d->nitems == 0) {
        *scroll += dir * vh * 0.5f;
        doc_clamp(d, vh, scroll);
        return;
    }
    if (*sel < 0 || *sel >= d->nitems) *sel = 0;
    const DocItem *it = &d->items[*sel];
    const float step = vh * 0.6f;

    if (dir > 0) {
        float hidden = it->bot - (*scroll + vh);
        if (hidden > 1) *scroll += hidden < step ? hidden : step;
        else if (*sel < d->nitems - 1) { (*sel)++; doc_ensure_visible(d, *sel, vh, scroll); }
    } else {
        float hidden = *scroll - it->top;
        if (hidden > 1) *scroll -= hidden < step ? hidden : step;
        else if (*sel > 0) { (*sel)--; doc_ensure_visible(d, *sel, vh, scroll); }
    }
    doc_clamp(d, vh, scroll);
}

/* ------------------------------------------------------------------ */
/* Formes et widgets                                                  */

void ui_rrect(float x, float y, float w, float h, float r, u32 c)
{
    if (w <= 0 || h <= 0) return;
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    if (r < 1) { C2D_DrawRectSolid(x, y, 0.5f, w, h, c); return; }
    C2D_DrawRectSolid(x + r, y, 0.5f, w - 2 * r, h, c);
    C2D_DrawRectSolid(x, y + r, 0.5f, r, h - 2 * r, c);
    C2D_DrawRectSolid(x + w - r, y + r, 0.5f, r, h - 2 * r, c);
    C2D_DrawCircleSolid(x + r, y + r, 0.5f, r, c);
    C2D_DrawCircleSolid(x + w - r, y + r, 0.5f, r, c);
    C2D_DrawCircleSolid(x + r, y + h - r, 0.5f, r, c);
    C2D_DrawCircleSolid(x + w - r, y + h - r, 0.5f, r, c);
}

void ui_rrect_outline(float x, float y, float w, float h, float r, float t, u32 color, u32 inner)
{
    ui_rrect(x, y, w, h, r, color);
    ui_rrect(x + t, y + t, w - 2 * t, h - 2 * t, r - t > 1 ? r - t : 1, inner);
}

float ui_pill_width(const char *text, float scale)
{
    return ui_text_width(text, scale) + 12;
}

float ui_pill(float x, float y, const char *text, float scale, u32 bg, u32 fg)
{
    float w = ui_pill_width(text, scale);
    float h = ui_line_height(scale) + 2;
    ui_rrect(x, y, w, h, h / 2, bg);
    ui_text(x + 6, y + 1, scale, fg, text);
    return w;
}

void ui_spinner(float cx, float cy, float r, u32 color)
{
    const int N = 8;
    int head = (g_ui_frame / 4) % N;
    for (int i = 0; i < N; i++) {
        float a = (float)i / N * 6.2831853f;
        int age = (head - i + N) % N;
        u32 alpha = 255 - age * 28;
        u32 c = (color & 0x00FFFFFF) | (alpha << 24);
        float dr = r * 0.22f * (1.0f - age * 0.07f);
        C2D_DrawCircleSolid(cx + cosf(a) * r, cy + sinf(a) * r, 0.5f, dr, c);
    }
}

void ui_icon_up(float x, float y, float s, u32 c)
{
    /* flèche : triangle + tige */
    C2D_DrawTriangle(x + s / 2, y, c, x, y + s * 0.55f, c, x + s, y + s * 0.55f, c, 0.5f);
    C2D_DrawRectSolid(x + s * 0.32f, y + s * 0.5f, 0.5f, s * 0.36f, s * 0.5f, c);
}

void ui_icon_comment(float x, float y, float s, u32 c)
{
    ui_rrect(x, y, s, s * 0.72f, s * 0.2f, c);
    C2D_DrawTriangle(x + s * 0.2f, y + s * 0.6f, c, x + s * 0.5f, y + s * 0.6f, c,
                     x + s * 0.18f, y + s * 0.95f, c, 0.5f);
}

void ui_logo(float cx, float cy, float r)
{
    C2D_DrawCircleSolid(cx, cy, 0.5f, r, COL_ACCENT);
    /* visage */
    C2D_DrawEllipseSolid(cx - r * 0.62f, cy - r * 0.30f, 0.5f, r * 1.24f, r * 0.84f, COL_WHITE);
    C2D_DrawCircleSolid(cx - r * 0.25f, cy + r * 0.05f, 0.5f, r * 0.13f, COL_ACCENT);
    C2D_DrawCircleSolid(cx + r * 0.25f, cy + r * 0.05f, 0.5f, r * 0.13f, COL_ACCENT);
}

void ui_text_center(float cx, float y, float scale, u32 color, const char *s)
{
    ui_text(cx - ui_text_width(s, scale) / 2, y, scale, color, s);
}

/* ------------------------------------------------------------------ */
/* Tactile                                                            */

static bool s_t_held, s_t_prev, s_t_moved, s_t_tap;
static float s_t_x, s_t_y, s_t_px, s_t_py, s_t_sx, s_t_sy;

void ui_touch_update(void)
{
    touchPosition tp;
    s_t_prev = s_t_held;
    s_t_held = (hidKeysHeld() & KEY_TOUCH) != 0;
    s_t_tap = false;
    s_t_px = s_t_x;
    s_t_py = s_t_y;
    if (s_t_held) {
        hidTouchRead(&tp);
        s_t_x = tp.px;
        s_t_y = tp.py;
        if (!s_t_prev) {
            s_t_sx = s_t_px = s_t_x;
            s_t_sy = s_t_py = s_t_y;
            s_t_moved = false;
        }
        if (fabsf(s_t_x - s_t_sx) > 8 || fabsf(s_t_y - s_t_sy) > 8) s_t_moved = true;
    } else if (s_t_prev && !s_t_moved) {
        s_t_tap = true; /* position = dernière position connue */
    }
}

bool  ui_touch_down(void) { return s_t_held && !s_t_prev; }
bool  ui_touch_held(void) { return s_t_held; }
bool  ui_touch_tap(void)  { return s_t_tap; }
float ui_touch_x(void)    { return s_t_x; }
float ui_touch_y(void)    { return s_t_y; }
float ui_touch_dy(void)   { return (s_t_held && s_t_prev) ? s_t_y - s_t_py : 0; }
float ui_touch_dx(void)   { return (s_t_held && s_t_prev) ? s_t_x - s_t_px : 0; }

bool ui_button(float x, float y, float w, float h, const char *label, bool primary, bool enabled)
{
    bool inside = s_t_x >= x && s_t_x < x + w && s_t_y >= y && s_t_y < y + h;
    bool pressed = enabled && s_t_held && inside && !s_t_moved;
    u32 bg = !enabled ? COL_CARD : primary ? (pressed ? COL_ACCENT2 : COL_ACCENT)
                                           : (pressed ? COL_LINE : COL_CARD_HI);
    ui_rrect(x, y, w, h, h / 2, bg);
    float sc = 0.45f;
    float tw = ui_text_width(label, sc);
    if (tw > w - 8) { sc = 0.4f; tw = ui_text_width(label, sc); }
    ui_text(x + (w - tw) / 2, y + (h - ui_line_height(sc)) / 2, sc,
            enabled ? COL_WHITE : COL_DIM, label);
    return enabled && s_t_tap && inside;
}

/* ------------------------------------------------------------------ */
/* Notification                                                       */

static char s_toast[160];
static int s_toast_t;

void ui_toast(const char *msg)
{
    snprintf(s_toast, sizeof(s_toast), "%s", msg);
    s_toast_t = 150;
}

void ui_draw_toast(float sw)
{
    if (s_toast_t <= 0) return;
    s_toast_t--;
    const float sc = 0.45f;
    float tw = ui_text_width(s_toast, sc);
    if (tw > sw - 40) tw = sw - 40;
    float w = tw + 24, h = ui_line_height(sc) + 10;
    float x = (sw - w) / 2, y = 240 - h - 10;
    if (s_toast_t < 15) y += (15 - s_toast_t) * 3; /* glisse vers le bas en disparaissant */
    ui_rrect(x, y, w, h, h / 2, C2D_Color32(0x33, 0x35, 0x3B, 0xFF));
    ui_text(x + 12, y + 5, sc, COL_WHITE, s_toast);
}

/* ------------------------------------------------------------------ */

void doc_truncate(Doc *d, int max_lines)
{
    if (d->nlines <= max_lines || max_lines <= 0) return;
    for (int i = max_lines; i < d->nlines; i++) free(d->lines[i].text);
    d->nlines = max_lines;
    DocLine *l = &d->lines[max_lines - 1];
    size_t n = strlen(l->text);
    /* retire quelques caractères pour faire de la place au "…" */
    int cut = 0;
    while (n > 0 && cut < 3) {
        n--;
        while (n > 0 && ((unsigned char)l->text[n] & 0xC0) == 0x80) n--;
        cut++;
    }
    char *t = malloc(n + 4);
    if (!t) return;
    memcpy(t, l->text, n);
    memcpy(t + n, "\xE2\x80\xA6", 4);
    free(l->text);
    l->text = t;
    d->h = l->y + ui_line_height(l->scale);
}
