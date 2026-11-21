#pragma once
#include <3ds.h>
#include <citro2d.h>

/*
 * Images chargées en arrière-plan (thread dédié) et mises en cache sous forme de
 * textures GPU. On demande une image à chaque frame où elle est visible : la
 * première demande lance le téléchargement, les suivantes renvoient la texture.
 */

typedef enum {
    IMG_THUMB,   /* miniatures (≤ 128x128) */
    IMG_PREVIEW, /* aperçu écran (≤ 400x240) */
    IMG_FULL,    /* plein écran zoomable (≤ 1024x1024) */
    IMG_KINDS
} ImgKind;

typedef enum { IMGS_LOADING = 0, IMGS_READY = 1, IMGS_FAILED = -1 } ImgState;

void img_init(void);
void img_exit(void);

/* À appeler une fois par frame, juste après C3D_FrameBegin. */
void img_frame(void);

/* Renvoie l'image si prête (sinon NULL), et l'état dans *state (facultatif). */
const C2D_Image *img_get(const char *url, ImgKind kind, ImgState *state);

/* Taille réelle (en pixels) de l'image décodée. */
void img_size(const C2D_Image *img, float *w, float *h);

/* Dessine l'image centrée dans le rectangle (x,y,w,h), en conservant le ratio. */
void img_draw_fit(const C2D_Image *img, float x, float y, float w, float h, float zoom,
                  float pan_x, float pan_y);

/* Libère toutes les images d'un type (ex. IMG_FULL en sortant du visualiseur). */
void img_drop_kind(ImgKind kind);
/* Vide tout le cache d'images */
void img_drop_all(void);
