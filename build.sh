#!/usr/bin/env bash
# Compile Red3DS.cia sans rien installer sur le système (ni sudo, ni Docker).
# La toolchain devkitARM est extraite de l'image Docker officielle devkitpro/devkitarm,
# makerom est téléchargé depuis sa release GitHub.
# Tout est rangé dans ~/.cache/reddit3ds (réutilisé aux lancements suivants).
set -euo pipefail

cd "$(dirname "$0")"
ROOT="$PWD"
TC="${XDG_CACHE_HOME:-$HOME/.cache}/reddit3ds"
RT="$TC/rootfs"
BIN="$TC/bin"
mkdir -p "$TC" "$RT" "$BIN"

log() { printf '\n\033[1;33m==> %s\033[0m\n' "$*"; }
need() { command -v "$1" >/dev/null || { echo "Commande manquante : $1"; exit 1; }; }
need curl; need tar; need gzip; need perl

# --------------------------------------------------------------------------
# 1. devkitARM (image Docker devkitpro/devkitarm, téléchargée via l'API du registre)
# --------------------------------------------------------------------------
if [ ! -x "$RT/opt/devkitpro/devkitARM/bin/arm-none-eabi-gcc" ]; then
    log "Téléchargement de devkitARM (quelques centaines de Mo, une seule fois)..."
    REPO="devkitpro/devkitarm"
    TOKEN=$(curl -fsSL "https://auth.docker.io/token?service=registry.docker.io&scope=repository:$REPO:pull" \
            | sed -E 's/.*"token":"([^"]+)".*/\1/')
    ACCEPT="application/vnd.docker.distribution.manifest.list.v2+json,application/vnd.oci.image.index.v1+json,application/vnd.docker.distribution.manifest.v2+json,application/vnd.oci.image.manifest.v1+json"
    reg() { curl -fsSL -H "Authorization: Bearer $TOKEN" -H "Accept: $ACCEPT" "https://registry-1.docker.io/v2/$REPO/$1"; }

    MANIFEST=$(reg manifests/latest)
    if echo "$MANIFEST" | grep -q '"manifests"'; then
        # Index multi-architecture : on prend linux/amd64
        DIGEST=$(echo "$MANIFEST" | tr -d '\n ' | grep -oE '\{[^{}]*"digest":"sha256:[0-9a-f]+"[^{}]*\{[^{}]*"architecture":"amd64"[^{}]*\}' \
                 | grep -oE 'sha256:[0-9a-f]{64}' | head -1)
        MANIFEST=$(reg "manifests/$DIGEST")
    fi
    LAYERS=$(echo "$MANIFEST" | tr -d '\n ' | grep -oE '"layers":\[.*\]' | grep -oE 'sha256:[0-9a-f]{64}')
    [ -n "$LAYERS" ] || { echo "Impossible de lire le manifeste Docker"; exit 1; }

    for L in $LAYERS; do
        echo "  couche ${L:7:12}..."
        curl -fsSL -H "Authorization: Bearer $TOKEN" "https://registry-1.docker.io/v2/$REPO/blobs/$L" \
            | tar -xz -C "$RT" --no-same-owner --wildcards "opt/devkitpro/*" "usr/bin/make" 2>/dev/null || true
    done
    [ -x "$RT/opt/devkitpro/devkitARM/bin/arm-none-eabi-gcc" ] || { echo "Échec de l'extraction de devkitARM"; exit 1; }
fi

export DEVKITPRO="$RT/opt/devkitpro"
export DEVKITARM="$DEVKITPRO/devkitARM"
export PATH="$BIN:$DEVKITPRO/tools/bin:$DEVKITARM/bin:$PATH"
[ -x "$RT/usr/bin/make" ] && ! command -v make >/dev/null && ln -sf "$RT/usr/bin/make" "$BIN/make"
need make

for lib in libcurl.a libmbedtls.a libz.a; do
    [ -f "$DEVKITPRO/portlibs/3ds/lib/$lib" ] || { echo "Bibliothèque manquante dans l'image : $lib"; exit 1; }
done

# --------------------------------------------------------------------------
# 2. makerom
# --------------------------------------------------------------------------
unzip_to() { perl -MIO::Uncompress::Unzip=unzip,\$UnzipError -e '
    use IO::Uncompress::Unzip qw($UnzipError);
    my ($zip, $dir) = @ARGV;
    my $u = IO::Uncompress::Unzip->new($zip) or die $UnzipError;
    do { my $n = $u->getHeaderInfo->{Name}; next if $n =~ m{/$};
         (my $b = $n) =~ s{.*/}{}; open my $o, ">:raw", "$dir/$b" or die;
         my $buf; print $o $buf while $u->read($buf) > 0; close $o;
    } while ($u->nextStream > 0);' "$1" "$2"; }

makerom_ok() { [ -x "$BIN/makerom" ] || return 1; local o; o=$("$BIN/makerom" -version 2>&1 || true); ! printf "%s" "$o" | grep -q GLIBC; }

if ! makerom_ok; then
    log "Téléchargement de makerom..."
    # Les versions récentes exigent une glibc récente : on prend la plus récente qui tourne ici.
    URLS=$(curl -fsSL "https://api.github.com/repos/3DSGuy/Project_CTR/releases?per_page=40" \
           | grep -oE 'https://[^"]*makerom-[^"]*(ubuntu|linux)_x86_64[^"]*\.zip')
    for URL in $URLS; do
        rm -f "$BIN/makerom"
        curl -fsSL -o "$TC/makerom.zip" "$URL" || continue
        unzip_to "$TC/makerom.zip" "$BIN"
        chmod +x "$BIN/makerom" 2>/dev/null || true
        if makerom_ok; then echo "  $(basename "$URL")"; break; fi
    done
    makerom_ok || { echo "Aucune version de makerom ne fonctionne sur ce système"; exit 1; }
fi

# --------------------------------------------------------------------------
# 3. FFmpeg (lecture vidéo) : compilé une seule fois
# --------------------------------------------------------------------------
if [ ! -f "$TC/ffmpeg/lib/libavcodec.a" ]; then
    log "Compilation de FFmpeg (une seule fois, quelques minutes)..."
    bash "$ROOT/tools/build_ffmpeg.sh" "$TC/ffsrc" "$TC/ffmpeg"
fi

# --------------------------------------------------------------------------
# 4. Compilation
# --------------------------------------------------------------------------
# Compilation dans un dossier Linux : gcc gère mal les chemins /mnt/c de WSL
log "Compilation..."
# Anciens fichiers retirés d'abord : impossible d'installer par erreur une version périmée
rm -f "$ROOT/Red3DS.cia" "$ROOT/Red3DS.3dsx" "$ROOT/Reddit3DS.cia" "$ROOT/Reddit3DS.3dsx"
STAMP=$(date "+%d/%m %H:%M")
SRC="$TC/src"
rm -rf "$SRC" && mkdir -p "$SRC"
cp -r Makefile app.rsf source assets romfs "$SRC/"
make -C "$SRC" cia FFMPEG="$TC/ffmpeg" BUILD_STAMP="$STAMP"
cp "$SRC/Red3DS.cia" "$SRC/Red3DS.3dsx" "$ROOT/"

log "Terminé : $ROOT/Red3DS.cia (et Red3DS.3dsx), build du $STAMP"
echo "Dans l'appli : SELECT (Paramètres), tout en bas, la ligne doit afficher « build $STAMP »."
