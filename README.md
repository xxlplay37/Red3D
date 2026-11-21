# Reddit3DS

Client Reddit en lecture seule pour Nintendo 3DS (homebrew, CFW requis), distribué en `.cia`
(ou `.3dsx`). Aucun compte ni client_id nécessaire.

- Interface moderne : cartes arrondies, miniatures, défilement fluide, animations de chargement,
  notifications, barre d'actions tactile
- **Images** : miniatures dans le fil, aperçu sur l'écran du bas, image dans le post et
  **visionneuse plein écran zoomable** (JPEG et PNG ; aperçu pour les vidéos et galeries)
- **Recherche de subreddits** avec icônes, nombre de membres et description
- **Vidéos Reddit avec le son** (v.redd.it), jusqu'à 360p, lecture en flux ou après préchargement complet, pause, volume, pourcentage de chargement
- **Traduction** des posts et commentaires en un bouton (affichage au fur et à mesure)
- Commentaires en arborescence colorée, défilement au doigt avec inertie
- Tris : Populaires / Nouveaux / Top (24 h) / En hausse, défilement infini
- Chargements en arrière-plan (l'interface ne se fige jamais, B annule)
- HTTPS vérifié (libcurl + mbedTLS + certificats Mozilla dans le RomFS)

## Commandes

| Écran      | Bouton                 | Action                                        |
|------------|------------------------|-----------------------------------------------|
| Fil        | Croix (←/→ : ×4)       | Choisir un post                               |
| Fil        | A / toucher l'aperçu   | Ouvrir le post                                |
| Fil        | Y / bouton Subs        | Onglet Subs : barre de recherche + clavier tactile |
| Fil        | L / R                  | Changer le tri                                |
| Fil        | X                      | Actualiser                                    |
| Post       | Croix / glisser le doigt | Parcourir les commentaires                  |
| Post       | Stick, L / R           | Faire défiler le texte du post               |
| Post       | A                      | Image en plein écran                          |
| Post       | A (post vidéo)         | Lire la vidéo avec le son                     |
| Vidéo      | A / toucher, B         | Pause / lecture, retour                       |
| Vidéo      | Croix haut/bas, L / R  | Volume                                        |
| Post       | Y / bouton Traduire    | Traduire le post et les commentaires (appuyer à nouveau : original) |
| Image      | L / R, boutons + −     | Zoom                                          |
| Image      | Stick, croix, glisser  | Se déplacer dans l'image                     |
| Subs       | Clavier tactile        | Taper (recherche automatique), récents et suggestions quand la barre est vide |
| Subs       | X / Y                  | Effacer une lettre / lancer la recherche     |
| Subs       | Croix, A               | Choisir et ouvrir un subreddit                |
| Partout    | SELECT / bouton Réglages | Paramètres                                  |
| Partout    | B                      | Retour / annuler le chargement                |
| Partout    | START                  | Quitter                                       |

## Compilation

**Le plus simple : double-cliquer sur `COMPILER.bat`** (Windows + WSL).
Sous Linux/WSL : `./build.sh`.

Le script n'installe rien sur le système (pas de sudo, pas de Docker) : il télécharge
devkitARM depuis l'image Docker officielle `devkitpro/devkitarm` et `makerom` depuis
GitHub dans `~/.cache/reddit3ds`, puis produit `Reddit3DS.cia` et `Reddit3DS.3dsx` ici.
Le premier lancement prend quelques minutes, les suivants quelques secondes.

Le premier lancement compile aussi une version minimale de **FFmpeg** (H.264, AAC, MP4 ;
licence LGPL) via `tools/build_ffmpeg.sh`, mise en cache dans `~/.cache/reddit3ds/ffmpeg`.

La bannière (`assets/banner.bnr`) est pré-générée et l'icône est convertie par `smdhtool`
(fourni avec devkitPro) : bannertool n'est pas nécessaire.

Avec un devkitPro déjà installé : `make cia` (makerom dans le `PATH`).

## Installation

1. Copier `Reddit3DS.cia` sur la carte SD.
2. L'installer avec **FBI** (ou Universal-Updater / GodMode9).
3. Le lancer depuis le menu HOME (Wi-Fi activé).

Version `.3dsx` : la copier dans `sdmc:/3ds/` et la lancer depuis le Homebrew Launcher.

## Sources des données (aucun compte ni client_id requis)

Reddit bloque désormais l'accès anonyme direct et ne délivre plus facilement de `client_id`.
L'appli passe donc par :

1. **Redlib** : interface Reddit libre hébergée par des bénévoles. Données en direct, tris
   Populaires / Nouveaux / Top / En hausse. Plusieurs instances sont essayées l'une après
   l'autre et la dernière qui a marché est réutilisée.
2. **Arctic Shift** (archive JSON, en dernier recours) : très fiable, mais seulement les posts
   les plus récents, scores figés au moment de l'archivage, et pas de r/popular.

La source utilisée s'affiche en bas de l'écran du bas.

## Paramètres (SELECT)

| Réglage                | Valeurs                                                                 |
|------------------------|-------------------------------------------------------------------------|
| Langue de l'interface  | Français, English (par défaut : langue de la console)                   |
| Langue du contenu      | Tout (r/popular), Français, English, Español, Deutsch, Italiano, Português : le fil d'accueil regroupe les grands subreddits de cette langue |
| Tri par défaut         | Populaires, Nouveaux, Top 24 h, En hausse                               |
| Au démarrage           | Fil d'accueil ou un subreddit au choix                                 |
| Contenu NSFW           | Masquer (retiré du fil), Flouter (image cachée jusqu'à A), Afficher    |
| Images                 | Toutes, Légères (petits aperçus seulement), Aucune (économie de données) |
| Taille du texte        | Petit, Normal, Grand                                                    |
| Traduire vers          | Français, English, Español, Deutsch, Italiano, Português (Google Traduction, MyMemory en secours) |
| Qualité vidéo max      | Auto (360p New 3DS, 270p Old 3DS), 220p, 270p, 360p                     |
| Lecture vidéo          | Direct (en flux, démarre vite) ou Préchargement (la vidéo et le son sont d'abord entièrement téléchargés sur la carte SD, puis lus sans coupure) |
| Source des données     | Auto, Redlib, Archive                                                   |
| Vider le cache d'images |                                                                        |

Reddit ne traduit pas les posts : « langue du contenu » choisit des subreddits écrits dans
cette langue. Les réglages sont enregistrés dans `sdmc:/3ds/reddit3ds/config.ini`, où l'on
peut aussi modifier la liste des instances Redlib (`redlib=`) et ajouter un `client_id`.

## Architecture

```
source/
  main.c    écrans, mise en page, contrôles
  ui.c/h    rendu citro2d : texte, cartes, pastilles, boutons tactiles, animations
  image.c/h téléchargement + décodage JPEG/PNG en arrière-plan, cache de textures
  job.c/h   requêtes Reddit dans un thread de travail (annulables)
  settings.c/h paramètres (config.ini) et traductions FR/EN
  reddit.c/h sources (Redlib, Arctic Shift, OAuth optionnel), modèle Post/Comment
  html.c/h  extraction de texte depuis le HTML de Redlib
  net.c/h   HTTP(S) via libcurl (handle réutilisé = keep-alive TLS)
  json.c/h  parseur JSON minimal
assets/     icône 48×48, bannière 256×128, son de bannière
romfs/      cacert.pem
app.rsf     description du titre pour makerom
```

## Limites connues

- Vidéos : le son nécessite `sdmc:/3ds/dspfirm.cdc` (à créer une fois avec l'outil DSP1).
  Sur Old 3DS, le décodage est logiciel : 270p par défaut, filtre anti-blocs désactivé.
  Pas d'avance rapide ; les vidéos YouTube ou externes ne sont pas lisibles.
- Lecture seule (pas de vote, pas de commentaire, pas de connexion à un compte).
- Le markdown est affiché brut.
- Les réponses « + N non chargées » ne sont pas dépliables (jusqu'à 400 commentaires par post).
- GIF animés : seul l'aperçu fixe est affiché ; galeries : première image seulement.
- Pas de WebP (les images sont demandées en JPEG/PNG au CDN de Reddit).

## Dépannage vidéo

À chaque lecture, l'appli écrit `sdmc:/3ds/reddit3ds/video.log` (remis à zéro à chaque vidéo).
Le son passe par le DSP : `app.rsf` doit mapper la mémoire DSP (`IORegisterMapping`), sinon le `.cia`
plante au lancement d'une vidéo (le `.3dsx` lancé depuis Homebrew Launcher n'a pas ce problème).
En cas de plantage, la **dernière ligne** du fichier indique l'étape atteinte (playlist, ouverture
des pistes FFmpeg, création des threads, son, première image...).
