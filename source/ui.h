#pragma once
#include <3ds.h>
#include <citro2d.h>

/* Palette : thème sombre moderne */
#define COL_BG      C2D_Color32(0x0F, 0x10, 0x12, 0xFF)
#define COL_BAR     C2D_Color32(0x17, 0x18, 0x1B, 0xFF)
#define COL_CARD    C2D_Color32(0x1C, 0x1D, 0x21, 0xFF)
#define COL_CARD_HI C2D_Color32(0x26, 0x28, 0x2E, 0xFF)
#define COL_SEL     COL_CARD_HI
#define COL_LINE    C2D_Color32(0x2C, 0x2E, 0x34, 0xFF)
#define COL_TEXT    C2D_Color32(0xE6, 0xE8, 0xEB, 0xFF)
#define COL_WHITE   C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF)
#define COL_DIM     C2D_Color32(0x8E, 0x93, 0x9A, 0xFF)
#define COL_ACCENT  C2D_Color32(0xFF, 0x45, 0x00, 0xFF)
#define COL_ACCENT2 C2D_Color32(0xFF, 0x7A, 0x3D, 0xFF)
#define COL_LINK    C2D_Color32(0x4F, 0xBC, 0xFF, 0xFF)
#define COL_GREEN   C2D_Color32(0x46, 0xD1, 0x60, 0xFF)
#define COL_RED     C2D_Color32(0xFF, 0x58, 0x5B, 0xFF)
#define COL_YELLOW  C2D_Color32(0xFF, 0xD6, 0x35, 0xFF)
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
float ui_line_height(float scale);
float ui_text_width(const char *s, float scale);
void  ui_text(float x, float y, float scale, u32 color, const char *s);
void  ui_bar(float x, float y, float w, float h, const char *left, const char *right);

extern u32 g_ui_frame; /* compteur de frames (animations) */

/* Formes */
void  ui_rrect(float x, float y, float w, float h, float r, u32 color);
void  ui_rrect_outline(float x, float y, float w, float h, float r, float t, u32 color, u32 inner);
/* Pastille de texte, renvoie sa largeur */
float ui_pill(float x, float y, const char *text, float scale, u32 bg, u32 fg);
float ui_pill_width(const char *text, float scale);
void  ui_spinner(float cx, float cy, float r, u32 color);
void  ui_icon_up(float x, float y, float s, u32 color);
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
