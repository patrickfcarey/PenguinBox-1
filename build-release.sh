#!/usr/bin/env bash
#
# build-release.sh — one entry point to build a distributable PenguinBox for a
# chosen OS, reproducibly, with every hard-won fix baked in.
#
#   ./build-release.sh windows      # -> dist/PenguinBox-win64.exe   (self-contained .exe)
#   ./build-release.sh linux        # -> dist/PenguinBox-x86_64.flatpak
#
# Options:
#   --rebuild-image   (windows) force-rebuild the cross toolchain image
#   --no-strip        (windows) keep debug symbols (208 MB instead of ~32 MB)
#   -h | --help
#
# Backends differ by target, on purpose:
#   windows -> podman/docker + the MXE mingw-w64 cross image (ubuntu-win64-cross/)
#              running xemu's own ./build.sh -p win64-cross.
#   linux   -> flatpak-builder + flatpak/app.xemu.xemu.yml. flatpak-builder
#              compiles xemu inside the freedesktop SDK sandbox (which supplies
#              all deps, incl. the X11/Wayland libs SDL3 needs), so it does NOT
#              use the Docker image. If flatpak-builder is absent it is installed
#              as the org.flatpak.Builder flatpak (flatpak --user, no sudo).
#
set -euo pipefail
IFS=$'\n\t'

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
cd "$REPO"

TARGET=""
REBUILD_IMAGE=0
STRIP=1
for a in "$@"; do
  case "$a" in
    windows|win|win64) TARGET=windows ;;
    linux)             TARGET=linux ;;
    --rebuild-image)   REBUILD_IMAGE=1 ;;
    --no-strip)        STRIP=0 ;;
    -h|--help)         grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown arg: $a" >&2; exit 2 ;;
  esac
done
if [ -z "$TARGET" ]; then
  echo "usage: $0 <windows|linux> [--rebuild-image] [--no-strip]" >&2
  exit 2
fi

log() { printf '\n=== %s ===\n' "$*"; }

# ---------------------------------------------------------------------------
build_windows() {
  local IMAGE=xemu-win64-cross:latest
  local DOCKERFILE=ubuntu-win64-cross/gcc.Dockerfile
  local CONTEXT=ubuntu-win64-cross

  # Container engine (podman preferred; docker fallback).
  local ENGINE
  ENGINE="$(command -v podman || command -v docker || true)"
  [ -n "$ENGINE" ] || { echo "need podman or docker for the windows build" >&2; exit 1; }

  image_exists() {
    if [ "$(basename "$ENGINE")" = podman ]; then "$ENGINE" image exists "$IMAGE"
    else "$ENGINE" image inspect "$IMAGE" >/dev/null 2>&1; fi
  }

  if [ "$REBUILD_IMAGE" = 1 ] || ! image_exists; then
    log "building cross toolchain image $IMAGE (one-time, ~tens of minutes)"
    "$ENGINE" build -t "$IMAGE" -f "$DOCKERFILE" "$CONTEXT"
  else
    log "cross toolchain image $IMAGE present (use --rebuild-image to refresh)"
  fi

  # dsp56300 (Xbox APU DSP) fetches a prebuilt blob at configure time via the
  # `curl` PROGRAM, which the image lacks. Pre-seed it on the host so meson's
  # fs.exists() check skips the download entirely.
  local DSP_VER=0.1.3 TRIP=x86_64-pc-windows-gnu
  local BLOB="subprojects/dsp56300/dsp56300-${DSP_VER}-${TRIP}.tar.gz"
  if [ ! -f "$BLOB" ]; then
    log "pre-seeding dsp56300 blob ($TRIP)"
    curl -fsSL "https://github.com/mborgerson/dsp56300/releases/download/v${DSP_VER}/dsp56300-${DSP_VER}-${TRIP}.tar.gz" -o "$BLOB"
  fi

  rm -rf build dist
  # TAR_OPTIONS=--no-same-owner: the dsp56300 blob stores uid 1001; under
  # rootless podman tar cannot chown to it and aborts. This suppresses the chown.
  log "cross-compiling (./build.sh -p win64-cross)"
  "$ENGINE" run --rm --security-opt label=disable \
    -e TAR_OPTIONS=--no-same-owner \
    -v "$REPO":/xemu -w /xemu "$IMAGE" \
    ./build.sh -p win64-cross

  [ -f dist/xemu.exe ] || { echo "build produced no dist/xemu.exe" >&2; exit 1; }

  if [ "$STRIP" = 1 ]; then
    log "stripping -> dist/PenguinBox-win64.exe"
    "$ENGINE" run --rm --security-opt label=disable -v "$REPO":/xemu -w /xemu "$IMAGE" \
      x86_64-w64-mingw32.static-strip -o dist/PenguinBox-win64.exe dist/xemu.exe
  else
    cp dist/xemu.exe dist/PenguinBox-win64.exe
  fi

  log "done"
  file dist/PenguinBox-win64.exe
  ( command -v sha256sum >/dev/null && sha256sum dist/PenguinBox-win64.exe ) || true
}

# ---------------------------------------------------------------------------
build_linux() {
  local MANIFEST=flatpak/app.xemu.xemu.yml
  local APPID=app.xemu.xemu
  local STATE=.flatpak-builder
  local FLATREPO="$STATE/repo"
  local OUT=dist/PenguinBox-x86_64.flatpak
  local FLATHUB=https://flathub.org/repo/flathub.flatpakrepo

  command -v flatpak >/dev/null || { echo "need flatpak on the host for the linux build" >&2; exit 1; }

  # Pick a flatpak-builder: host binary, else the org.flatpak.Builder flatpak,
  # else install that flatpak (--user, no sudo).
  local FB
  if command -v flatpak-builder >/dev/null; then
    FB=(flatpak-builder)
  elif flatpak info org.flatpak.Builder >/dev/null 2>&1; then
    FB=(flatpak run org.flatpak.Builder)
  else
    log "installing flatpak-builder (org.flatpak.Builder, flatpak --user, no sudo)"
    flatpak --user remote-add --if-not-exists flathub "$FLATHUB"
    flatpak --user install -y flathub org.flatpak.Builder
    FB=(flatpak run org.flatpak.Builder)
  fi

  mkdir -p dist
  log "flatpak-builder (pulls freedesktop runtime/SDK 25.08 on first run, ~1.5-2 GB)"
  "${FB[@]}" --user --force-clean --install-deps-from=flathub \
    --repo="$FLATREPO" "$STATE/build" "$MANIFEST"

  log "exporting single-file bundle -> $OUT"
  flatpak build-bundle --runtime-repo="$FLATHUB" "$FLATREPO" "$OUT" "$APPID"

  log "done"
  ls -la "$OUT"
  ( command -v sha256sum >/dev/null && sha256sum "$OUT" ) || true
  echo "install with:  flatpak install --user $OUT"
}

case "$TARGET" in
  windows) build_windows ;;
  linux)   build_linux ;;
esac
