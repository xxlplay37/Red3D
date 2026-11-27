#!/usr/bin/env bash
# Compile une version minimale de FFmpeg pour la 3DS : vidéo Reddit (MP4, H.264, AAC) et sons / musique
# de l'interface (OGG Vorbis, MP3, WAV).
# FFMPEG_CONF : à changer quand la configuration change (build.sh recompile alors FFmpeg).
# Usage : tools/build_ffmpeg.sh <dossier de travail> <préfixe d'installation>
set -euo pipefail
WORK="$1"
PREFIX="$2"
VER=6.1.2
FFMPEG_CONF="video+sons-1"
: "${DEVKITPRO:?DEVKITPRO non défini}"
: "${DEVKITARM:?DEVKITARM non défini}"

mkdir -p "$WORK"
cd "$WORK"
if [ ! -d "ffmpeg-$VER" ]; then
    echo "  téléchargement de FFmpeg $VER..."
    curl -fsSL -o ffmpeg.tar.gz "https://ffmpeg.org/releases/ffmpeg-$VER.tar.gz"
    tar -xzf ffmpeg.tar.gz
fi
cd "ffmpeg-$VER"

ARCHF="-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft"
echo "  configuration..."
./configure --prefix="$PREFIX" --enable-cross-compile --cross-prefix=arm-none-eabi- \
    --arch=arm --cpu=armv6k --target-os=none --pkg-config=false --host-cc=arm-none-eabi-gcc \
    --disable-everything --disable-programs --disable-doc --disable-network --disable-avdevice \
    --disable-avfilter --disable-swscale --disable-swresample --disable-postproc \
    --disable-pthreads --disable-w32threads --disable-os2threads --disable-iconv --disable-zlib \
    --disable-bzlib --disable-lzma --disable-debug --disable-runtime-cpudetect \
    --disable-neon --disable-vfpv3 --disable-thumb \
    --enable-demuxer=mov --enable-decoder=h264 --enable-decoder=aac \
    --enable-parser=h264 --enable-parser=aac \
    --enable-demuxer=ogg --enable-demuxer=mp3 --enable-demuxer=wav \
    --enable-decoder=vorbis --enable-decoder=mp3float --enable-decoder=pcm_s16le \
    --enable-parser=vorbis --enable-parser=mpegaudio \
    --extra-cflags="$ARCHF -D__3DS__ -O2 -ffunction-sections -fdata-sections -Wno-incompatible-pointer-types -Wno-int-conversion -I$DEVKITPRO/libctru/include" \
    --extra-ldflags="$ARCHF -specs=3dsx.specs -L$DEVKITPRO/libctru/lib" \
    --extra-libs="-lctru -lm" | grep -E 'install prefix|License'
echo "  compilation (quelques minutes)..."
make -j"$(nproc 2>/dev/null || echo 4)" >/dev/null
make install >/dev/null
echo "$FFMPEG_CONF" > "$PREFIX/.conf"
echo "  FFmpeg installé dans $PREFIX"
