#pragma once
#include <3ds.h>
#include <citro2d.h>

/* Palette du thème actif (ui_set_theme). COL_WHITE = texte fort du thème (blanc sur thème sombre,
 * presque noir sur thème clair) ; COL_ONACC = vrai blanc, pour ce qui est posé sur une couleur
 * (bouton principal, pastille, voile sur une vidéo). */
/* Un thème change les couleurs ET l'aspect : forme des coins, contours, ombre, façon de montrer la
 * sélection, barres, boutons et motif du fond. */
enum { SEL_RING, SEL_GLOW, SEL_BOX, SEL_AERO };  /* sélection : anneau (standard), halo (3DS), cadre clignotant (rétro), dégradé bleu (Aero) */
enum { BAR_FLAT, BAR_GLOSS, BAR_RETRO, BAR_GLASS }; /* barres : unie, brillante (3DS), double trait (rétro), verre (Aero) */
enum { BG_PLAIN, BG_STRIPES, BG_DOTS, BG_SCAN, BG_AERO }; /* fond : uni, rayures, points, lignes CRT, dégradé + reflets (Aero) */
typedef struct {
    u32 bg, bar, card, card_hi, line, text, strong, dim, accent, accent2, link, green, red, yellow, toast;
    u32 pattern;      /* couleur du motif de fond */
    u32 onacc;        /* texte posé sur la couleur d'accent */
    u32 border;       /* contour des cartes et des boutons */
    u32 bar2;         /* bas du dégradé des barres (BAR_GLOSS) */
    u32 bg2;          /* bas du dégradé du fond (BG_AERO) */
    float radius;     /* facteur des arrondis (0 = coins carrés) */
    u8 border_w;      /* épaisseur du contour des cartes (0 = aucun) */
    u8 shadow;        /* ombre portée des cartes (px) */
    u8 sel, bar_style, bg_style;
} Theme;
extern Theme g_th;

enum { TH_DARK, TH_LIGHT, TH_BLACK, TH_NAVY,
       TH_3DS, TH_3DS_RED, TH_3DS_AQUA, TH_3DS_PINK, TH_3DS_PURPLE, TH_3DS_GREEN, TH_3DS_WHITE, TH_3DS_BLACK,
       TH_3DS_SET, TH_AERO, TH_AERO_DARK, TH_GB, TH_TERM, THEMES };
/* Clé du thème dans config.ini */
const char *ui_theme_key(int t);
void ui_set_theme(int t);
/* Nom du thème (fr = 1 : français) */
const char *ui_theme_name(int t, int fr);

#define COL_BG      (g_th.bg)
#define COL_BAR     (g_th.bar)
#define COL_CARD    (g_th.card)
#define COL_CARD_HI (g_th.card_hi)
#define COL_SEL     COL_CARD_HI
#define COL_LINE    (g_th.line)
#define COL_TEXT    (g_th.text)
#define COL_WHITE   (g_th.strong)
#define COL_DIM     (g_th.dim)
#define COL_ACCENT  (g_th.accent)
#define COL_ACCENT2 (g_th.accent2)
#define COL_LINK    (g_th.link)
#define COL_GREEN   (g_th.green)
#define COL_RED     (g_th.red)
#define COL_YELLOW  (g_th.yellow)
#define COL_ONACC   (g_th.onacc)
#define COL_OVERLAY C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF) /* sur un voile noir ou une vidéo, quel que soit le thème */
#define COL_SCRIM   C2D_Color32(0x00, 0x00, 0x00, 0xA0)

/* Glyphes boutons de la police système (UTF-8) */
#define BTN_A "\xEE\x80\x80"
#define BTN_B "\xEE\x80\x81"
#define BTN_X "\xEE\x80\x82"
#define BTN_Y "\xEE\x80\x83"
#define BTN_L "\xEE\x80\x84"
#define BTN_R "\xEE\x80\x85"
#define BTN_DPAD "\xEE\x80\x86"

void  ui_init(void);
void  ui_exit(void);
void  ui_set_text_scale(float m);
void  ui_frame_begin(void); /* à appeler après C3D_FrameBegin */
/* Efface l'écran (fond du thème, rayures éventuelles) et commence à y dessiner */
void  ui_screen_begin(C3D_RenderTarget *t, float w);
/* Inversion des glyphes de boutons affichés (A<->B, X<->Y) quand les commandes sont inversées */
void  ui_set_glyph_swap(bool ab, bool xy);
float ui_line_height(float scale);
float ui_text_width(const char *s, float scale);
void  ui_text(float x, float y, float scale, u32 color, const char *s);
void  ui_bar(float x, float y, float w, float h, const char *left, const char *right);

extern u32 g_ui_frame; /* compteur de frames (animations) */

/* Formes */
void  ui_rrect(float x, float y, float w, float h, float r, u32 color);
void  ui_rrect_outline(float x, float y, float w, float h, float r, float t, u32 color, u32 inner);
/* Carte du thème (contour, ombre, sélection selon le style) */
void  ui_card(float x, float y, float w, float h, float r, bool sel);
/* Fond de barre (en-tête : edge_bottom = vrai ; barre d'outils du bas : faux) */
void  ui_bar_bg(float x, float y, float w, float h, bool edge_bottom);
/* Pastille de texte, renvoie sa largeur */
float ui_pill(float x, float y, const char *text, float scale, u32 bg, u32 fg);
float ui_pill_width(const char *text, float scale);
void  ui_spinner(float cx, float cy, float r, u32 color);
void  ui_icon_up(float x, float y, float s, u32 color);
void  ui_icon_down(float x, float y, float s, u32 color);
void  ui_icon_comment(float x, float y, float s, u32 color);
void  ui_logo(float cx, float cy, float r);
void  ui_text_center(float cx, float y, float scale, u32 color, const char *s);

/* Bouton tactile : dessine et renvoie vrai si touché ce frame-ci */
bool  ui_button(float x, float y, float w, float h, const char *label, bool primary, bool enabled);
/* Notification temporaire */
void  ui_toast(const char *msg);
void  ui_draw_toast(float screen_w);
/* Entrées tactiles du frame courant */
void  ui_touch_update(void);
bool  ui_touch_down(void);  /* doigt posé ce frame */
bool  ui_touch_held(void);
bool  ui_touch_tap(void);   /* relâché sans glisser */
float ui_touch_x(void);
float ui_touch_y(void);
float ui_touch_dy(void);    /* déplacement vertical depuis le frame précédent */
float ui_touch_dx(void);

/* ------------------------------------------------------------------ */
/* Document : lignes pré-coupées + "items" sélectionnables            */

typedef struct {
    char *text;
    float x, y, scale;
    u32 color;
    u32 bar; /* couleur d'un trait vertical à gauche (0 = aucun) */
} DocLine;

typedef struct {
    float top, bot;
} DocItem;

typedef struct {
    DocLine *lines;
    int nlines, capl;
    DocItem *items;
    int nitems, capi;
    float h; /* hauteur totale / curseur vertical */
} Doc;

void doc_free(Doc *d);
void doc_text(Doc *d, const char *s, float scale, float x, float maxw, u32 color, u32 bar);
void doc_space(Doc *d, float px);
void doc_item_begin(Doc *d);
void doc_item_end(Doc *d);
void doc_draw(const Doc *d, float vx, float vy, float vw, float vh, float scroll, int sel);
void doc_clamp(const Doc *d, float vh, float *scroll);
void doc_ensure_visible(const Doc *d, int item, float vh, float *scroll);
/* Déplace la sélection, ou fait défiler si l'item courant dépasse l'écran. */
void doc_step(const Doc *d, int dir, float vh, int *sel, float *scroll);
/* Garde au plus max_lines lignes (la dernière se termine par "…"). */
void doc_truncate(Doc *d, int max_lines);
