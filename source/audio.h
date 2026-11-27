#pragma once
#include <stdbool.h>

/*
 * Sons de l'interface et musique de fond (NDSP).
 *   Sons : romfs:/sons/defaut/<son>.ogg, ou un pack sur la carte SD :
 *          sdmc:/3ds/reddit3ds/sons/<pack>/<son>/<fichier>  (plusieurs fichiers = un au hasard)
 *          ou sdmc:/3ds/reddit3ds/sons/<pack>/<son>.<ext>
 *   Musique : romfs:/musique/heavenly_loop.ogg, ou sdmc:/3ds/reddit3ds/musique/<fichier>
 *   Formats : .ogg (Vorbis), .mp3, .wav, .m4a. Les sons manquants d'un pack reprennent ceux par défaut.
 * Canaux NDSP : 0 = vidéo (video.c), 1 = musique, 2..5 = sons.
 */

enum { SFX_NAV, SFX_OK, SFX_BACK, SFX_OPEN, SFX_CHANGE, SFX_TAB, SFX_ERROR, SFX_NOTIFY, SFX_KEY, SFX_COUNT };

#define AUDIO_DIR   "sdmc:/3ds/reddit3ds"
#define SOUNDS_DIR  AUDIO_DIR "/sons"
#define MUSIC_DIR   AUDIO_DIR "/musique"

/* Initialise le DSP (nécessite sdmc:/3ds/dspfirm.cdc), crée les dossiers et leurs README, applique g_cfg */
void audio_init(void);
void audio_exit(void);
/* Le DSP est disponible (sinon : ni son, ni musique, ni son des vidéos) */
bool audio_ok(void);
/* Relit les réglages (sons activés, pack, musique, volume) */
void audio_apply(void);

void sfx_play(int id);
/* Musique en pause (pendant une vidéo) */
void music_pause(bool p);
/* Morceau en cours ("" si aucun) */
const char *audio_music_now(void);

/* Packs de sons et musiques trouvés sur la carte SD */
int audio_packs(char out[][48], int max);
int audio_tracks(char out[][96], int max);
/* Nom du dossier d'un son (navigation, valider...) */
const char *sfx_name(int id);

/* FFmpeg est compilé sans pthreads : les ouvertures de fichiers / décodeurs sont sérialisées
 * entre la musique et la vidéo */
void audio_ff_lock(void);
void audio_ff_unlock(void);
