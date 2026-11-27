#include "ui.h"
#include "emoji_data.h"

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

/* ------------------------------------------------------------------ */
/* Emoji : images pré-rendues (romfs:/emoji.bin), affichées dans le texte */

static C3D_Tex s_emoji_tex;
static bool s_emoji_ok, s_emoji_failed;

static int emoji_index(u32 cp)
{
    int lo = 0, hi = EMOJI_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (EMOJI_CP[mid] == cp) return mid;
        if (EMOJI_CP[mid] < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

/* Caractères invisibles qui accompagnent les emoji : sélecteurs, jonction, teintes de peau, étiquettes */
static bool emoji_ignored(u32 cp)
{
    return cp == 0xFE0F || cp == 0xFE0E || cp == 0x200D || cp == 0x20E3 ||
           (cp >= 0x1F3FB && cp <= 0x1F3FF) || (cp >= 0xE0020 && cp <= 0xE007F);
}

static bool is_regional(u32 cp)
{
    return cp >= 0x1F1E6 && cp <= 0x1F1FF; /* drapeaux : affichés en lettres (FR, US...) */
}

/* Faut-il un traitement particulier pour ce caractère ? */
static bool cp_special(u32 cp)
{
    if (cp < 0x200D) return false;
    return emoji_ignored(cp) || is_regional(cp) || emoji_index(cp) >= 0;
}

static bool text_has_special(const char *s)
{
    for (const char *p = s; *p;) {
        if ((unsigned char)*p >= 0xE2) {
            u32 cp;
            int n = utf8_next(p, &cp);
            if (cp_special(cp)) return true;
            p += n;
        } else {
            p++;
        }
    }
    return false;
}

static float emoji_adv(void)
{
    return s_line_feed * 0.92f;
}

static float glyph_adv(u32 cp)
{
    if (cp < 128) return s_ascii_adv[cp];
    if (cp >= 0x200D) {
        if (emoji_ignored(cp)) return 0.0f;
        if (is_regional(cp)) return s_ascii_adv['A' + (cp - 0x1F1E6)];
        if (emoji_index(cp) >= 0) return emoji_adv();
    }
    return glyph_adv_raw(cp);
}

static void emoji_load(void)
{
    if (s_emoji_ok || s_emoji_failed) return;
    s_emoji_failed = true;
    FILE *f = fopen("romfs:/emoji.bin", "rb");
    if (!f) return;
    size_t size = (size_t)EMOJI_TW * EMOJI_TH * 4;
    if (C3D_TexInit(&s_emoji_tex, EMOJI_TW, EMOJI_TH, GPU_RGBA8)) {
        if (fread(s_emoji_tex.data, 1, size, f) == size) {
            C3D_TexFlush(&s_emoji_tex);
            C3D_TexSetFilter(&s_emoji_tex, GPU_LINEAR, GPU_LINEAR);
            C3D_TexSetWrap(&s_emoji_tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
            s_emoji_ok = true;
            s_emoji_failed = false;
        } else {
            C3D_TexDelete(&s_emoji_tex);
        }
    }
    fclose(f);
}

/* Dessine l'emoji idx dans la ligne de texte dont le coin haut-gauche est (x, y) */
static void emoji_draw(int idx, float x, float y, float scale)
{
    emoji_load();
    if (!s_emoji_ok) return; /* pas de texture : on laisse un espace */
    const float lh = s_line_feed * scale;
    const float g = emoji_adv() * scale;                          /* taille visible du dessin */
    const float d = g * (float)EMOJI_CELL / (EMOJI_CELL - 2);     /* case entière (marge de 1 px) */
    int col = idx % EMOJI_COLS, row = idx / EMOJI_COLS;
    Tex3DS_SubTexture st;
    st.width = EMOJI_CELL;
    st.height = EMOJI_CELL;
    st.left = (float)(col * EMOJI_CELL) / EMOJI_TW;
    st.right = (float)((col + 1) * EMOJI_CELL) / EMOJI_TW;
    st.top = 1.0f - (float)(row * EMOJI_CELL) / EMOJI_TH;
    st.bottom = 1.0f - (float)((row + 1) * EMOJI_CELL) / EMOJI_TH;
    C2D_Image img = { &s_emoji_tex, &st };
    float k = d / EMOJI_CELL;
    C2D_DrawImageAt(img, x - (d - g) / 2, y + (lh - d) / 2 + lh * 0.04f, 0.5f, NULL, k, k);
}

void ui_init(void)
{
    ui_set_theme(TH_DARK);
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
    if (s_emoji_ok) C3D_TexDelete(&s_emoji_tex);
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

static void draw_run(const char *run, float x, float y, float scale, u32 color)
{
    C2D_Text t;
    C2D_TextParse(&t, s_buf, run);
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor, x, y, 0.5f, scale, scale, color);
}

static const char *swap_glyphs(const char *s, char *tmp, size_t n);

void ui_text(float x, float y, float scale, u32 color, const char *s)
{
    if (!s || !*s) return;
    char swapped[512];
    s = swap_glyphs(s, swapped, sizeof(swapped));
    scale *= s_mul;
    if (!text_has_special(s)) {
        draw_run(s, x, y, scale, color);
        return;
    }
    /* Texte avec emoji : on dessine les morceaux de texte et les images l'un après l'autre */
    char run[200];
    size_t rn = 0;
    float rw = 0, cx = x;
    for (const char *p = s; *p;) {
        u32 cp;
        int n = utf8_next(p, &cp);
        const char *src = p;
        p += n;
        if (emoji_ignored(cp)) continue;
        int ei = (cp >= 0x200D && !is_regional(cp)) ? emoji_index(cp) : -1;
        if (ei >= 0) {
            if (rn) { run[rn] = '\0'; draw_run(run, cx, y, scale, color); cx += rw; rn = 0; rw = 0; }
            emoji_draw(ei, cx, y, scale);
            cx += emoji_adv() * scale;
            continue;
        }
        char one[1];
        if (is_regional(cp)) { one[0] = (char)('A' + (cp - 0x1F1E6)); src = one; n = 1; }
        if (rn + (size_t)n >= sizeof(run) - 1) {
            run[rn] = '\0';
            draw_run(run, cx, y, scale, color);
            cx += rw;
            rn = 0;
            rw = 0;
        }
        memcpy(run + rn, src, (size_t)n);
        rn += (size_t)n;
        rw += glyph_adv(cp) * scale;
    }
    if (rn) { run[rn] = '\0'; draw_run(run, cx, y, scale, color); }
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

static inline u32 with_alpha(u32 c, u8 a)
{
    return (c & 0x00FFFFFFu) | ((u32)a << 24);
}

static void rrect_raw(float x, float y, float w, float h, float r, u32 c)
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

/* Rectangle arrondi selon le thème ; les panneaux (couleur des cartes) reçoivent le contour du thème */
void ui_rrect(float x, float y, float w, float h, float r, u32 c)
{
    r *= g_th.radius;
    if (g_th.border_w && c == g_th.card && w > 30 && h > 20) {
        float b = g_th.border_w;
        rrect_raw(x - b, y - b, w + 2 * b, h + 2 * b, r > 0 ? r + b : 0, g_th.border);
    }
    rrect_raw(x, y, w, h, r, c);
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

void ui_icon_down(float x, float y, float s, u32 c)
{
    /* flèche vers le bas : tige + triangle */
    C2D_DrawRectSolid(x + s * 0.32f, y, 0.5f, s * 0.36f, s * 0.5f, c);
    C2D_DrawTriangle(x + s / 2, y + s, c, x, y + s * 0.45f, c, x + s, y + s * 0.45f, c, 0.5f);
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
    C2D_DrawEllipseSolid(cx - r * 0.62f, cy - r * 0.30f, 0.5f, r * 1.24f, r * 0.84f, COL_ONACC);
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
    switch (g_th.sel) {
    case SEL_GLOW: { /* 3DS : bouton en relief (contour, reflet en haut, ombre en bas) */
        float r = 7 * g_th.radius;
        rrect_raw(x, y + 1, w, h, r, C2D_Color32(0, 0, 0, 0x30));
        rrect_raw(x, y, w, h, r, primary && enabled ? COL_ACCENT2 : g_th.border);
        rrect_raw(x + 1, y + 1, w - 2, h - 2, r > 1 ? r - 1 : 0, bg == COL_CARD_HI ? COL_CARD : bg);
        if (!pressed) C2D_DrawRectSolid(x + 3, y + 2, 0.5f, w - 6, h / 2 - 2, C2D_Color32(0xFF, 0xFF, 0xFF, 0x38));
        break;
    }
    case SEL_AERO: { /* Aero : bouton vitré (contour sombre, moitié haute claire, reflet) */
        float r = 4;
        u32 top = primary && enabled ? (pressed ? COL_ACCENT2 : COL_ACCENT) : (pressed ? COL_CARD_HI : COL_CARD);
        rrect_raw(x, y, w, h, r, enabled ? (primary ? COL_ACCENT2 : g_th.border) : COL_LINE);
        rrect_raw(x + 1, y + 1, w - 2, h - 2, r - 1, top);
        C2D_DrawRectangle(x + 2, y + h / 2, 0.5f, w - 4, h / 2 - 2, with_alpha(0, 0x00), with_alpha(0, 0x00),
                          with_alpha(0, 0x22), with_alpha(0, 0x22));
        if (!pressed) C2D_DrawRectSolid(x + 2, y + 2, 0.5f, w - 4, h / 2 - 2, with_alpha(0xFFFFFFFFu, 0x60));
        break;
    }
    case SEL_BOX: { /* rétro : case carrée à contour épais, couleurs inversées quand on appuie */
        float b = g_th.border_w ? g_th.border_w : 1;
        C2D_DrawRectSolid(x, y, 0.5f, w, h, enabled ? (primary ? COL_ACCENT : g_th.border) : COL_LINE);
        C2D_DrawRectSolid(x + b, y + b, 0.5f, w - 2 * b, h - 2 * b,
                          pressed ? COL_ACCENT : primary && enabled ? COL_ACCENT : COL_CARD);
        break;
    }
    default:
        rrect_raw(x, y, w, h, h / 2, bg);
        break;
    }
    float sc = 0.45f;
    float tw = ui_text_width(label, sc);
    if (tw > w - 8) { sc = 0.4f; tw = ui_text_width(label, sc); }
    ui_text(x + (w - tw) / 2, y + (h - ui_line_height(sc)) / 2, sc,
            !enabled ? COL_DIM : (primary || (pressed && g_th.sel == SEL_BOX)) ? COL_ONACC : COL_WHITE, label);
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
    ui_rrect(x, y, w, h, h / 2, g_th.toast);
    ui_text(x + 12, y + 5, sc, COL_OVERLAY, s_toast);
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

/* ------------------------------------------------------------------ */
/* Thèmes : couleurs + aspect                                         */

#define RGB(r, g, b) ((u32)(r) | ((u32)(g) << 8) | ((u32)(b) << 16) | 0xFF000000u) /* = C2D_Color32, mais constant */
#define WHITE RGB(0xFF, 0xFF, 0xFF)

/* Aspect « standard » : coins arrondis, pas de contour, sélection par anneau, barres unies */
#define STD_LOOK .radius = 1.0f, .border_w = 0, .shadow = 0, .sel = SEL_RING, .bar_style = BAR_FLAT, .bg_style = BG_PLAIN

static const Theme THEMES_TAB[THEMES] = {
    [TH_DARK] = { .bg = RGB(0x0F, 0x10, 0x12), .bar = RGB(0x17, 0x18, 0x1B), .card = RGB(0x1C, 0x1D, 0x21),
                  .card_hi = RGB(0x26, 0x28, 0x2E), .line = RGB(0x2C, 0x2E, 0x34), .text = RGB(0xE6, 0xE8, 0xEB),
                  .strong = WHITE, .dim = RGB(0x8E, 0x93, 0x9A), .accent = RGB(0xFF, 0x45, 0x00),
                  .accent2 = RGB(0xFF, 0x7A, 0x3D), .link = RGB(0x4F, 0xBC, 0xFF), .green = RGB(0x46, 0xD1, 0x60),
                  .red = RGB(0xFF, 0x58, 0x5B), .yellow = RGB(0xFF, 0xD6, 0x35), .toast = RGB(0x33, 0x35, 0x3B),
                  .onacc = WHITE, .border = RGB(0x2C, 0x2E, 0x34), .bar2 = RGB(0x17, 0x18, 0x1B), STD_LOOK },
    [TH_LIGHT] = { .bg = RGB(0xEE, 0xF0, 0xF3), .bar = WHITE, .card = WHITE, .card_hi = RGB(0xE4, 0xEA, 0xF2),
                   .line = RGB(0xD3, 0xD7, 0xDD), .text = RGB(0x24, 0x28, 0x2E), .strong = RGB(0x0B, 0x0C, 0x0E),
                   .dim = RGB(0x6A, 0x71, 0x7A), .accent = RGB(0xFF, 0x45, 0x00), .accent2 = RGB(0xD9, 0x48, 0x0F),
                   .link = RGB(0x0A, 0x66, 0xC2), .green = RGB(0x1F, 0x9D, 0x3A), .red = RGB(0xD9, 0x30, 0x35),
                   .yellow = RGB(0x9A, 0x6E, 0x00), .toast = RGB(0x30, 0x33, 0x38), .onacc = WHITE,
                   .border = RGB(0xD3, 0xD7, 0xDD), .bar2 = WHITE, STD_LOOK },
    [TH_BLACK] = { .bg = RGB(0x00, 0x00, 0x00), .bar = RGB(0x0A, 0x0A, 0x0A), .card = RGB(0x11, 0x11, 0x11),
                   .card_hi = RGB(0x1E, 0x1E, 0x1E), .line = RGB(0x26, 0x26, 0x26), .text = RGB(0xE6, 0xE6, 0xE6),
                   .strong = WHITE, .dim = RGB(0x8A, 0x8A, 0x8A), .accent = RGB(0xFF, 0x45, 0x00),
                   .accent2 = RGB(0xFF, 0x7A, 0x3D), .link = RGB(0x4F, 0xBC, 0xFF), .green = RGB(0x46, 0xD1, 0x60),
                   .red = RGB(0xFF, 0x58, 0x5B), .yellow = RGB(0xFF, 0xD6, 0x35), .toast = RGB(0x2A, 0x2A, 0x2A),
                   .onacc = WHITE, .border = RGB(0x26, 0x26, 0x26), .bar2 = RGB(0x0A, 0x0A, 0x0A), STD_LOOK },
    [TH_NAVY] = { .bg = RGB(0x0D, 0x14, 0x21), .bar = RGB(0x13, 0x1C, 0x2E), .card = RGB(0x17, 0x22, 0x38),
                  .card_hi = RGB(0x21, 0x30, 0x50), .line = RGB(0x2A, 0x3A, 0x5C), .text = RGB(0xDC, 0xE6, 0xF5),
                  .strong = WHITE, .dim = RGB(0x87, 0x96, 0xB0), .accent = RGB(0x4F, 0x8C, 0xFF),
                  .accent2 = RGB(0x7F, 0xB0, 0xFF), .link = RGB(0x6C, 0xC7, 0xFF), .green = RGB(0x46, 0xD1, 0x60),
                  .red = RGB(0xFF, 0x58, 0x5B), .yellow = RGB(0xFF, 0xD6, 0x35), .toast = RGB(0x2A, 0x36, 0x52),
                  .onacc = WHITE, .border = RGB(0x2A, 0x3A, 0x5C), .bar2 = RGB(0x13, 0x1C, 0x2E), STD_LOOK },
    /* Menu HOME de la 3DS : fond gris rayé, panneaux blancs bordés avec ombre, halo autour de la
     * sélection, barres brillantes en dégradé, boutons en relief. Une variante par couleur de console :
     * accent, accent foncé, fond de la sélection. */
#define HOME_LOOK(acc, acc2, sel_bg)                                                                           \
    { .bg = RGB(0xE2, 0xE2, 0xE2), .pattern = RGB(0xD7, 0xD7, 0xD7), .bar = WHITE, .bar2 = RGB(0xDC, 0xDC, 0xDC), \
      .card = WHITE, .card_hi = sel_bg, .line = RGB(0xC4, 0xC4, 0xC4), .text = RGB(0x3C, 0x3C, 0x3C),            \
      .strong = RGB(0x1E, 0x1E, 0x1E), .dim = RGB(0x7C, 0x7C, 0x7C), .accent = acc, .accent2 = acc2,             \
      .link = RGB(0x16, 0x78, 0xC8), .green = RGB(0x36, 0xA8, 0x32), .red = RGB(0xC8, 0x2A, 0x24),               \
      .yellow = RGB(0x9A, 0x6E, 0x00), .toast = RGB(0x46, 0x46, 0x46), .onacc = WHITE,                           \
      .border = RGB(0xC0, 0xC0, 0xC0), .radius = 0.75f, .border_w = 1, .shadow = 2,                             \
      .sel = SEL_GLOW, .bar_style = BAR_GLOSS, .bg_style = BG_STRIPES }
    [TH_3DS]        = HOME_LOOK(RGB(0x12, 0x8A, 0xD0), RGB(0x0E, 0x70, 0xB4), RGB(0xDD, 0xF3, 0xFE)), /* bleu cobalt */
    [TH_3DS_RED]    = HOME_LOOK(RGB(0xD0, 0x1F, 0x2E), RGB(0xA8, 0x14, 0x22), RGB(0xFC, 0xE4, 0xE4)), /* rouge flamme */
    [TH_3DS_AQUA]   = HOME_LOOK(RGB(0x00, 0x8C, 0xA8), RGB(0x00, 0x70, 0x88), RGB(0xD6, 0xF4, 0xF8)), /* bleu aqua */
    [TH_3DS_PINK]   = HOME_LOOK(RGB(0xD0, 0x3C, 0x7E), RGB(0xA8, 0x2C, 0x64), RGB(0xFC, 0xE4, 0xEF)), /* rose corail */
    [TH_3DS_PURPLE] = HOME_LOOK(RGB(0x6E, 0x40, 0xAA), RGB(0x56, 0x30, 0x88), RGB(0xEC, 0xE2, 0xF8)), /* violet minuit */
    [TH_3DS_GREEN]  = HOME_LOOK(RGB(0x2E, 0x8A, 0x2A), RGB(0x22, 0x6E, 0x20), RGB(0xE0, 0xF4, 0xDC)), /* vert lime */
    [TH_3DS_WHITE]  = HOME_LOOK(RGB(0x5A, 0x6E, 0x84), RGB(0x46, 0x58, 0x6C), RGB(0xE8, 0xEE, 0xF4)), /* blanc glacier */
    /* Noir cosmos : même aspect, version sombre */
    [TH_3DS_BLACK] = { .bg = RGB(0x2A, 0x2B, 0x2E), .pattern = RGB(0x24, 0x25, 0x28), .bar = RGB(0x4A, 0x4C, 0x52),
                       .bar2 = RGB(0x2E, 0x30, 0x34), .card = RGB(0x3A, 0x3C, 0x41), .card_hi = RGB(0x24, 0x3E, 0x52),
                       .line = RGB(0x55, 0x58, 0x5F), .text = RGB(0xE4, 0xE6, 0xEA), .strong = WHITE,
                       .dim = RGB(0xA0, 0xA4, 0xAC), .accent = RGB(0x3C, 0xB4, 0xF0), .accent2 = RGB(0x6C, 0xC8, 0xF6),
                       .link = RGB(0x6C, 0xC8, 0xF6), .green = RGB(0x5C, 0xD0, 0x5A), .red = RGB(0xFF, 0x6A, 0x5E),
                       .yellow = RGB(0xFF, 0xD0, 0x40), .toast = RGB(0x1C, 0x1D, 0x20), .onacc = RGB(0x08, 0x1E, 0x2C),
                       .border = RGB(0x1C, 0x1D, 0x20), .radius = 0.75f, .border_w = 1, .shadow = 2,
                       .sel = SEL_GLOW, .bar_style = BAR_GLOSS, .bg_style = BG_STRIPES },
    /* Aero (Windows Vista / 7) : fond dégradé ciel avec reflets, barres en verre, cartes lumineuses à
     * contour bleuté, sélection en dégradé bleu, boutons vitrés */
    [TH_AERO] = { .bg = RGB(0xB9, 0xD8, 0xF2), .bg2 = RGB(0xEC, 0xF5, 0xFC), .pattern = 0, .bar = RGB(0xE8, 0xF2, 0xFB),
                  .bar2 = RGB(0xA9, 0xCB, 0xEA), .card = RGB(0xFB, 0xFD, 0xFF), .card_hi = RGB(0xD9, 0xEB, 0xFB),
                  .line = RGB(0xA8, 0xC4, 0xE0), .text = RGB(0x1E, 0x2E, 0x42), .strong = RGB(0x0A, 0x16, 0x26),
                  .dim = RGB(0x4E, 0x62, 0x7A), .accent = RGB(0x1C, 0x6E, 0xC4), .accent2 = RGB(0x15, 0x5A, 0xA4),
                  .link = RGB(0x0A, 0x5C, 0xB8), .green = RGB(0x1E, 0x8A, 0x32), .red = RGB(0xC8, 0x24, 0x24),
                  .yellow = RGB(0x8A, 0x64, 0x00), .toast = RGB(0x1E, 0x3A, 0x5C), .onacc = WHITE,
                  .border = RGB(0x7D, 0xA2, 0xCE), .radius = 0.45f, .border_w = 1, .shadow = 2,
                  .sel = SEL_AERO, .bar_style = BAR_GLASS, .bg_style = BG_AERO },
    [TH_AERO_DARK] = { .bg = RGB(0x0A, 0x1C, 0x34), .bg2 = RGB(0x1E, 0x44, 0x6E), .pattern = 0, .bar = RGB(0x4A, 0x6E, 0x96),
                       .bar2 = RGB(0x14, 0x2C, 0x4C), .card = RGB(0x17, 0x2E, 0x4C), .card_hi = RGB(0x22, 0x4C, 0x7C),
                       .line = RGB(0x34, 0x58, 0x84), .text = RGB(0xE0, 0xEC, 0xF8), .strong = WHITE,
                       .dim = RGB(0x9C, 0xB4, 0xCE), .accent = RGB(0x4C, 0xA8, 0xF0), .accent2 = RGB(0x80, 0xC4, 0xF6),
                       .link = RGB(0x80, 0xCC, 0xFF), .green = RGB(0x5C, 0xD0, 0x6A), .red = RGB(0xFF, 0x6E, 0x6E),
                       .yellow = RGB(0xFF, 0xD6, 0x50), .toast = RGB(0x0A, 0x16, 0x28), .onacc = RGB(0x06, 0x14, 0x26),
                       .border = RGB(0x4E, 0x7A, 0xAE), .radius = 0.45f, .border_w = 1, .shadow = 2,
                       .sel = SEL_AERO, .bar_style = BAR_GLASS, .bg_style = BG_AERO },
    /* Menu Paramètres de la 3DS : fond gris bleuté pointillé, listes en cases blanches bien bordées,
     * coins peu arrondis, barres grises brillantes, sélection verte */
    [TH_3DS_SET] = { .bg = RGB(0xD5, 0xDB, 0xE3), .pattern = RGB(0xC3, 0xCB, 0xD6), .bar = RGB(0xFA, 0xFB, 0xFC),
                     .bar2 = RGB(0xC9, 0xD0, 0xDA), .card = RGB(0xFB, 0xFC, 0xFD), .card_hi = RGB(0xDF, 0xF5, 0xEA),
                     .line = RGB(0xB4, 0xBD, 0xC9), .text = RGB(0x33, 0x3A, 0x44), .strong = RGB(0x15, 0x1A, 0x20),
                     .dim = RGB(0x6E, 0x77, 0x84), .accent = RGB(0x1B, 0x8A, 0x5A), .accent2 = RGB(0x14, 0x6E, 0x48),
                     .link = RGB(0x16, 0x6E, 0xC0), .green = RGB(0x2E, 0x9A, 0x3C), .red = RGB(0xCC, 0x33, 0x33),
                     .yellow = RGB(0x8A, 0x64, 0x00), .toast = RGB(0x3C, 0x44, 0x50), .onacc = WHITE,
                     .border = RGB(0xA3, 0xAE, 0xBC), .radius = 0.4f, .border_w = 1, .shadow = 1,
                     .sel = SEL_GLOW, .bar_style = BAR_GLOSS, .bg_style = BG_DOTS },
    /* Rétro Game Boy : 4 teintes de vert, coins carrés, cases à double contour, sélection par cadre
     * clignotant, lignes d'écran */
    [TH_GB] = { .bg = RGB(0x9B, 0xBC, 0x0F), .pattern = RGB(0x93, 0xB3, 0x0E), .bar = RGB(0x8B, 0xAC, 0x0F),
                .bar2 = RGB(0x8B, 0xAC, 0x0F), .card = RGB(0x9B, 0xBC, 0x0F), .card_hi = RGB(0x8B, 0xAC, 0x0F),
                .line = RGB(0x30, 0x62, 0x30), .text = RGB(0x0F, 0x38, 0x0F), .strong = RGB(0x0F, 0x38, 0x0F),
                .dim = RGB(0x30, 0x62, 0x30), .accent = RGB(0x0F, 0x38, 0x0F), .accent2 = RGB(0x30, 0x62, 0x30),
                .link = RGB(0x30, 0x62, 0x30), .green = RGB(0x30, 0x62, 0x30), .red = RGB(0x0F, 0x38, 0x0F),
                .yellow = RGB(0x30, 0x62, 0x30), .toast = RGB(0x0F, 0x38, 0x0F), .onacc = RGB(0x9B, 0xBC, 0x0F),
                .border = RGB(0x30, 0x62, 0x30), .radius = 0, .border_w = 2, .shadow = 0,
                .sel = SEL_BOX, .bar_style = BAR_RETRO, .bg_style = BG_SCAN },
    /* Rétro terminal : vert phosphore sur noir, lignes CRT, cadres fins */
    [TH_TERM] = { .bg = RGB(0x00, 0x00, 0x00), .pattern = RGB(0x00, 0x14, 0x06), .bar = RGB(0x00, 0x12, 0x06),
                  .bar2 = RGB(0x00, 0x12, 0x06), .card = RGB(0x00, 0x06, 0x02), .card_hi = RGB(0x00, 0x2A, 0x10),
                  .line = RGB(0x0F, 0x5F, 0x2A), .text = RGB(0x33, 0xFF, 0x66), .strong = RGB(0x8C, 0xFF, 0xAA),
                  .dim = RGB(0x1E, 0x9E, 0x46), .accent = RGB(0x33, 0xFF, 0x66), .accent2 = RGB(0x22, 0xCC, 0x55),
                  .link = RGB(0x66, 0xFF, 0xCC), .green = RGB(0x33, 0xFF, 0x66), .red = RGB(0xFF, 0x55, 0x44),
                  .yellow = RGB(0xFF, 0xDD, 0x33), .toast = RGB(0x0A, 0x3A, 0x18), .onacc = RGB(0x00, 0x12, 0x06),
                  .border = RGB(0x1E, 0x9E, 0x46), .radius = 0, .border_w = 1, .shadow = 0,
                  .sel = SEL_BOX, .bar_style = BAR_RETRO, .bg_style = BG_SCAN },
};

Theme g_th;

void ui_set_theme(int t)
{
    if (t < 0 || t >= THEMES) t = TH_DARK;
    g_th = THEMES_TAB[t];
}

static const struct { const char *key, *fr, *en; } THEME_INFO[THEMES] = {
    [TH_DARK]       = { "dark",         "Standard sombre",            "Standard dark" },
    [TH_LIGHT]      = { "light",        "Standard clair",             "Standard light" },
    [TH_BLACK]      = { "black",        "Standard noir",              "Standard black" },
    [TH_NAVY]       = { "navy",         "Standard nuit bleue",        "Standard midnight" },
    [TH_3DS]        = { "3ds",          "3DS HOME bleu cobalt",       "3DS HOME cobalt blue" },
    [TH_3DS_RED]    = { "3ds_red",      "3DS HOME rouge flamme",      "3DS HOME flame red" },
    [TH_3DS_AQUA]   = { "3ds_aqua",     "3DS HOME bleu aqua",         "3DS HOME aqua blue" },
    [TH_3DS_PINK]   = { "3ds_pink",     "3DS HOME rose corail",       "3DS HOME coral pink" },
    [TH_3DS_PURPLE] = { "3ds_purple",   "3DS HOME violet minuit",     "3DS HOME midnight purple" },
    [TH_3DS_GREEN]  = { "3ds_green",    "3DS HOME vert lime",         "3DS HOME lime green" },
    [TH_3DS_WHITE]  = { "3ds_white",    "3DS HOME blanc glacier",     "3DS HOME ice white" },
    [TH_3DS_BLACK]  = { "3ds_black",    "3DS HOME noir cosmos",       "3DS HOME cosmos black" },
    [TH_3DS_SET]    = { "3ds_settings", "3DS Paramètres",             "3DS System Settings" },
    [TH_AERO]       = { "aero",         "Aero",                       "Aero" },
    [TH_AERO_DARK]  = { "aero_dark",    "Aero nuit",                  "Aero night" },
    [TH_GB]         = { "retro_gb",     "Rétro Game Boy",             "Retro Game Boy" },
    [TH_TERM]       = { "retro_term",   "Rétro terminal",             "Retro terminal" },
};

const char *ui_theme_name(int t, int fr)
{
    if (t < 0 || t >= THEMES) t = TH_DARK;
    return fr ? THEME_INFO[t].fr : THEME_INFO[t].en;
}

const char *ui_theme_key(int t)
{
    return (t >= 0 && t < THEMES) ? THEME_INFO[t].key : "dark";
}

void ui_screen_begin(C3D_RenderTarget *t, float w)
{
    C2D_TargetClear(t, COL_BG);
    C2D_SceneBegin(t);
    u32 c = g_th.pattern;
    switch (g_th.bg_style) {
    case BG_STRIPES: /* rayures horizontales du menu HOME */
        for (float y = 0; y < 240; y += 8) C2D_DrawRectSolid(0, y, 0.1f, w, 4, c);
        break;
    case BG_DOTS: /* trame de points */
        for (float y = 6; y < 240; y += 14)
            for (float x = ((int)y / 14 % 2) ? 13 : 6; x < w; x += 14) C2D_DrawRectSolid(x, y, 0.1f, 2, 2, c);
        break;
    case BG_SCAN: /* lignes d'écran cathodique */
        for (float y = 0; y < 240; y += 3) C2D_DrawRectSolid(0, y, 0.1f, w, 1, c);
        break;
    case BG_AERO: { /* dégradé vertical + deux reflets lumineux en diagonale */
        C2D_DrawRectangle(0, 0, 0.1f, w, 240, COL_BG, COL_BG, g_th.bg2, g_th.bg2);
        u32 l1 = C2D_Color32(0xFF, 0xFF, 0xFF, 0x22), l0 = C2D_Color32(0xFF, 0xFF, 0xFF, 0x00);
        C2D_DrawTriangle(w * 0.15f, 0, l1, w * 0.55f, 0, l1, -w * 0.15f, 240, l0, 0.1f);
        C2D_DrawTriangle(w * 0.62f, 0, l1, w * 0.78f, 0, l1, w * 0.40f, 240, l0, 0.1f);
        break;
    }
    default:
        break;
    }
}

void ui_card(float x, float y, float w, float h, float r, bool sel)
{
    switch (g_th.sel) {
    case SEL_GLOW: /* 3DS : ombre douce, contour fin, halo autour de la sélection */
        if (g_th.shadow) rrect_raw(x + 1, y + g_th.shadow, w, h, r * g_th.radius, C2D_Color32(0, 0, 0, 0x28));
        if (sel) {
            rrect_raw(x - 4, y - 4, w + 8, h + 8, (r + 4) * g_th.radius, with_alpha(COL_ACCENT, 0x50));
            rrect_raw(x - 2, y - 2, w + 4, h + 4, (r + 2) * g_th.radius, COL_ACCENT);
        } else if (g_th.border_w) {
            rrect_raw(x - 1, y - 1, w + 2, h + 2, (r + 1) * g_th.radius, g_th.border);
        }
        rrect_raw(x, y, w, h, r * g_th.radius, sel ? COL_CARD_HI : COL_CARD);
        /* reflet en haut de la carte */
        C2D_DrawRectSolid(x + r * g_th.radius, y + 1, 0.5f, w - 2 * r * g_th.radius, 1, with_alpha(0xFFFFFFFFu, 0xA0));
        break;
    case SEL_BOX: { /* rétro : cadre carré, double contour, coins clignotants sur la sélection */
        float b = g_th.border_w ? g_th.border_w : 1;
        C2D_DrawRectSolid(x, y, 0.5f, w, h, sel ? COL_ACCENT : g_th.border);
        C2D_DrawRectSolid(x + b, y + b, 0.5f, w - 2 * b, h - 2 * b, sel ? COL_CARD_HI : COL_CARD);
        if (sel && (g_ui_frame / 20) % 2 == 0) {
            const float L = 8, T = 2;
            float xs[2] = { x - 3, x + w + 3 - T }, ys[2] = { y - 3, y + h + 3 - T };
            for (int i = 0; i < 2; i++)
                for (int j = 0; j < 2; j++) {
                    float cx = xs[i], cy = ys[j];
                    C2D_DrawRectSolid(i ? cx - L + T : cx, cy, 0.5f, L, T, COL_ACCENT);
                    C2D_DrawRectSolid(cx, j ? cy - L + T : cy, 0.5f, T, L, COL_ACCENT);
                }
        }
        break;
    }
    case SEL_AERO: { /* Aero : carte claire à contour bleuté, sélection en dégradé bleu avec reflet */
        float rr = r * g_th.radius;
        if (g_th.shadow) rrect_raw(x + 1, y + g_th.shadow, w, h, rr, C2D_Color32(0, 0x20, 0x50, 0x28));
        rrect_raw(x - 1, y - 1, w + 2, h + 2, rr + 1, sel ? COL_ACCENT : g_th.border);
        rrect_raw(x, y, w, h, rr, sel ? COL_CARD_HI : COL_CARD);
        if (sel) /* dégradé vertical sous le reflet (sans recouvrir les coins arrondis) */
            C2D_DrawRectangle(x + rr, y + 1, 0.5f, w - 2 * rr, h - 2, with_alpha(0xFFFFFFFFu, 0x00), with_alpha(0xFFFFFFFFu, 0x00),
                              with_alpha(COL_ACCENT, 0x30), with_alpha(COL_ACCENT, 0x30));
        C2D_DrawRectSolid(x + rr, y + 1, 0.5f, w - 2 * rr, h / 2 - 1, with_alpha(0xFFFFFFFFu, sel ? 0x70 : 0x48));
        break;
    }
    default: /* standard : anneau de couleur autour de la sélection */
        if (sel) rrect_raw(x - 2, y - 2, w + 4, h + 4, (r + 2) * g_th.radius, COL_ACCENT);
        rrect_raw(x, y, w, h, r * g_th.radius, sel ? COL_CARD_HI : COL_CARD);
        break;
    }
}

void ui_bar_bg(float x, float y, float w, float h, bool edge_bottom)
{
    float ey = edge_bottom ? y + h - 1 : y; /* trait du côté du contenu */
    switch (g_th.bar_style) {
    case BAR_GLOSS:
        C2D_DrawRectangle(x, y, 0.5f, w, h, g_th.bar, g_th.bar, g_th.bar2, g_th.bar2);
        C2D_DrawRectSolid(x, y + (edge_bottom ? 0 : 1), 0.5f, w, 1, with_alpha(0xFFFFFFFFu, 0xC0));
        C2D_DrawRectSolid(x, ey, 0.5f, w, 1, g_th.border);
        if (edge_bottom) C2D_DrawRectSolid(x, y + h, 0.5f, w, 2, C2D_Color32(0, 0, 0, 0x22)); /* ombre */
        else C2D_DrawRectSolid(x, y - 2, 0.5f, w, 2, C2D_Color32(0, 0, 0, 0x22));
        break;
    case BAR_GLASS: /* verre : dégradé, moitié haute brillante, liseré clair et contour */
        C2D_DrawRectangle(x, y, 0.5f, w, h, g_th.bar, g_th.bar, g_th.bar2, g_th.bar2);
        C2D_DrawRectSolid(x, y, 0.5f, w, h / 2, with_alpha(0xFFFFFFFFu, 0x40));
        C2D_DrawRectSolid(x, edge_bottom ? y : y + 1, 0.5f, w, 1, with_alpha(0xFFFFFFFFu, 0xD0));
        C2D_DrawRectSolid(x, ey, 0.5f, w, 1, g_th.border);
        if (edge_bottom) C2D_DrawRectSolid(x, y + h, 0.5f, w, 3, C2D_Color32(0, 0x20, 0x50, 0x20));
        else C2D_DrawRectSolid(x, y - 3, 0.5f, w, 3, C2D_Color32(0, 0x20, 0x50, 0x20));
        break;
    case BAR_RETRO:
        C2D_DrawRectSolid(x, y, 0.5f, w, h, g_th.bar);
        C2D_DrawRectSolid(x, ey, 0.5f, w, 1, COL_ACCENT);
        C2D_DrawRectSolid(x, edge_bottom ? ey - 3 : ey + 3, 0.5f, w, 1, COL_ACCENT);
        break;
    default:
        C2D_DrawRectSolid(x, y, 0.5f, w, h, g_th.bar);
        C2D_DrawRectSolid(x, ey, 0.5f, w, 1, COL_LINE);
        break;
    }
}

static bool s_swap_ab, s_swap_xy;

void ui_set_glyph_swap(bool ab, bool xy)
{
    s_swap_ab = ab;
    s_swap_xy = xy;
}

/* Glyphes de boutons de la police système : EE 80 80 (A), 81 (B), 82 (X), 83 (Y) */
static const char *swap_glyphs(const char *s, char *tmp, size_t n)
{
    if ((!s_swap_ab && !s_swap_xy) || !strstr(s, "\xEE\x80")) return s;
    size_t l = strlen(s);
    if (l >= n) return s;
    memcpy(tmp, s, l + 1);
    for (size_t i = 0; i + 2 < l; i++) {
        if ((unsigned char)tmp[i] != 0xEE || (unsigned char)tmp[i + 1] != 0x80) continue;
        unsigned char c = (unsigned char)tmp[i + 2];
        if (s_swap_ab && (c == 0x80 || c == 0x81)) tmp[i + 2] = (char)(c ^ 1);
        else if (s_swap_xy && (c == 0x82 || c == 0x83)) tmp[i + 2] = (char)(c ^ 1);
        i += 2;
    }
    return tmp;
}
