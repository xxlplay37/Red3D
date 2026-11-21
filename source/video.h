#pragma once
#include <3ds.h>
#include <citro2d.h>
#include <stdbool.h>

/*
 * Lecteur vidéo Reddit (v.redd.it) : H.264 + AAC en flux, décodés par FFmpeg dans des
 * threads dédiés, son via NDSP, image synchronisée sur le son.
 */

typedef enum { VS_IDLE, VS_LOADING, VS_PLAYING, VS_PAUSED, VS_ENDED, VS_ERROR } VideoStatus;

/* max_p : qualité maximale (plus petit côté en pixels, ex. 360) */
/* preload : la lecture ne démarre qu'une fois la vidéo (et le son) entièrement téléchargés */
bool video_open(const char *video_id, int max_p, bool preload);
void video_close(void);
/* À appeler à chaque frame, juste après C3D_FrameBegin (envoie la dernière image au GPU). */
void video_frame(void);
/* Image courante (NULL tant qu'aucune image n'est prête) */
const C2D_Image *video_image(void);

VideoStatus video_status(void);
const char *video_error(void);
bool video_buffering(void);
int  video_load_percent(void);  /* progression du chargement 0..100, -1 si inconnue */
float video_position(void);
float video_duration(void);
int  video_quality(void);       /* plus petit côté de la vidéo choisie (ex. 360) */
bool video_has_audio(void);     /* la vidéo a une piste son */
bool video_sound_ok(void);      /* le son est réellement joué (NDSP disponible) */
void video_toggle_pause(void);
void video_restart(void);
void video_set_volume(float v); /* 0..1 */
float video_volume(void);

/* À la fermeture de l'appli */
void video_exit(void);
