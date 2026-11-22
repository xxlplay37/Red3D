# Reddit3DS

A read-only Reddit client for the Nintendo 3DS (homebrew, CFW required), distributed as a `.cia`
(or `.3dsx`). No account or `client_id` required.

- Modern interface: rounded cards, thumbnails, smooth scrolling, loading animations,
  notifications, and a touch action bar
- **Images**: thumbnails in the feed, preview on the bottom screen, images in posts, and a
  **zoomable full-screen viewer** (JPEG and PNG; previews for videos and galleries)
- **Subreddit search** with icons, member counts, and descriptions
- **Reddit videos with sound** (`v.redd.it`), up to 360p, streamed or played after full preloading,
  with pause, volume control, and loading progress
- **One-tap translation** of posts and comments (displayed progressively)
- Color-coded threaded comments, with inertial touch scrolling
- Sorting: Popular / New / Top (24 h) / Rising, with infinite scrolling
- Background loading (the interface never freezes; B cancels the operation)
- Verified HTTPS (libcurl + mbedTLS + Mozilla certificates in RomFS)

## Controls

| Screen | Button | Action |
|---|---|---|
| Feed | D-pad (←/→: ×4) | Select a post |
| Feed | A / tap the preview | Open the post |
| Feed | Y / Subs button | Open the Subs tab: search bar + touch keyboard |
| Feed | L / R | Change sorting |
| Feed | X | Refresh |
| Post | D-pad / swipe | Browse comments |
| Post | Circle Pad, L / R | Scroll through the post text |
| Post | A | Open image full-screen |
| Post | A (video post) | Play the video with sound |
| Video | A / tap, B | Pause / play, exit |
| Video | D-pad up/down, L / R | Adjust volume |
| Post | Y / Translate button | Translate the post and comments (press again to show the original) |
| Image | L / R, + / − buttons | Zoom |
| Image | Circle Pad, D-pad, swipe | Move around the image |
| Subs | Touch keyboard | Type (automatic search); recent searches and suggestions appear when the search bar is empty |
| Subs | X / Y | Delete a character / start the search |
| Subs | D-pad, A | Select and open a subreddit |
| Anywhere | SELECT / Settings button | Open settings |
| Anywhere | B | Go back / cancel loading |
| Anywhere | START | Exit |

## Building

**The easiest option is to double-click `COMPILER.bat`** (Windows + WSL).
On Linux/WSL, run `./build.sh`.

The script does not install anything system-wide (no `sudo`, no Docker): it downloads
devkitARM from the official `devkitpro/devkitarm` Docker image and `makerom` from
GitHub into `~/.cache/reddit3ds`, then generates `Reddit3DS.cia` and `Reddit3DS.3dsx` here.
The first run takes a few minutes; subsequent runs take only a few seconds.

The first run also builds a minimal version of **FFmpeg** (H.264, AAC, MP4;
LGPL license) using `tools/build_ffmpeg.sh`. It is cached in `~/.cache/reddit3ds/ffmpeg`.

The banner (`assets/banner.bnr`) is pre-generated, and the icon is converted with
`smdhtool` (included with devkitPro): `bannertool` is not required.

If devkitPro is already installed, run `make cia` (`makerom` must be in your `PATH`).

## Installation

1. Copy `Reddit3DS.cia` to your SD card.
2. Install it using **FBI** (or Universal-Updater / GodMode9).
3. Launch it from the HOME Menu (with Wi-Fi enabled).

For the `.3dsx` version, copy it to `sdmc:/3ds/` and launch it from the Homebrew Launcher.

## Data sources (no account or `client_id` required)

Reddit now blocks direct anonymous access and no longer makes it easy to obtain a `client_id`.
The app therefore uses:

1. **Redlib**: a free Reddit frontend hosted by volunteers. It provides live data and the
   Popular / New / Top / Rising sorting options. Several instances are tried in sequence,
   and the last working one is reused.
2. **Arctic Shift** (JSON archive, as a last resort): very reliable, but only includes the
   most recent posts, has scores frozen at the time of archiving, and does not support `r/popular`.

The active data source is displayed at the bottom of the lower screen.

## Settings (SELECT)

| Setting | Options |
|---|---|
| Interface language | French, English (default: console language) |
| Content language | All (`r/popular`), French, English, Spanish, German, Italian, Portuguese: the home feed groups major subreddits in the selected language |
| Default sorting | Popular, New, Top 24 h, Rising |
| On startup | Home feed or a selected subreddit |
| NSFW content | Hide (removed from the feed), Blur (image hidden until A is pressed), Show |
| Images | All, Lightweight (small previews only), None (save data) |
| Text size | Small, Normal, Large |
| Translate to | French, English, Spanish, German, Italian, Portuguese (Google Translate, with MyMemory as a fallback) |
| Maximum video quality | Auto (360p on New 3DS, 270p on Old 3DS), 220p, 270p, 360p |
| Video playback | Direct (streaming, starts quickly) or Preload (the video and audio are fully downloaded to the SD card first, then played without interruptions) |
| Data source | Auto, Redlib, Archive |
| Clear image cache | |

Reddit does not translate posts: **Content language** selects subreddits written in that language.
Settings are saved to `sdmc:/3ds/reddit3ds/config.ini`. You can also edit the Redlib instance
list (`redlib=`) there and add a `client_id`.

## Architecture

```text
source/
  main.c    screens, layout, controls
  ui.c/h    citro2d rendering: text, cards, badges, touch buttons, animations
  image.c/h background JPEG/PNG downloading and decoding, texture cache
  job.c/h   Reddit requests in a worker thread (cancellable)
  settings.c/h settings (`config.ini`) and FR/EN translations
  reddit.c/h data sources (Redlib, Arctic Shift, optional OAuth), Post/Comment model
  html.c/h  text extraction from Redlib HTML
  net.c/h   HTTP(S) via libcurl (reused handle = TLS keep-alive)
  json.c/h  minimal JSON parser
assets/     48×48 icon, 256×128 banner, banner sound
romfs/      cacert.pem
app.rsf     title description for makerom
```

## Known limitations

- Videos: sound requires `sdmc:/3ds/dspfirm.cdc` (create it once using the DSP1 tool).
  On Old 3DS, decoding is done in software: 270p by default, with the deblocking filter disabled.
  No seeking; YouTube and other external videos cannot be played.
- Read-only (no voting, commenting, or account sign-in).
- Markdown is displayed as raw text.
- Replies shown as “+ N more not loaded” cannot be expanded (up to 400 comments per post).
- Animated GIFs: only a static preview is displayed; galleries: only the first image is shown.
- No WebP support (images are requested from Reddit's CDN as JPEG/PNG).

## Video troubleshooting

Each playback session writes `sdmc:/3ds/reddit3ds/video.log` (the file is reset for every video).
Audio is handled by the DSP: `app.rsf` must map DSP memory (`IORegisterMapping`), otherwise the
`.cia` crashes when a video is started (the `.3dsx` launched from the Homebrew Launcher does not
have this issue).

If the app crashes, the **last line** of the log file indicates the stage it reached (playlist,
FFmpeg track opening, thread creation, audio, first frame, etc.).




## Source used

FourthTube : https://github.com/erievs/FourthTube

Logo: https://www.flaticon.com/free-icon/reddit_2504934

Logo convertion :DavidLaurent007(https://github.com/DavidLaurent007)

Creating the .gitignore file : KaliLugu
