#!/usr/bin/env bash
# Build the Nintendo Switch homebrew app: out/arcus-odyssey-dx.nro
#
# Runs in the devkitPro Docker image (devkitpro/devkita64, native arm64 on Apple
# silicon). Inside the container:
#   1. the recompiler (GenesisRecomp) is built for the container itself with the host
#      gcc, because the generated-C step runs it during the build;
#   2. the game is cross-compiled with devkitA64 (Switch.cmake), pointing the
#      generated-sources step at that host recompiler (GENESISRECOMP_EXECUTABLE);
#   3. elf2nro packs the NACP ("Arcus Odyssey DX", sixtyeightk, project version) and the icon.
# The icon (out/icon.jpg) is made on the Mac first (tools/make_icon.py) from public art
# downloaded at build time (or a title capture); it is never committed.
#
# usage: tools/build-switch.sh            (ARCUS_ROM=... to use another full-screen input ROM)
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
# ARCUS_WORK: where generated files go (build.py: build/; else this folder). ARCUS_OUT: the result.
WORK="${ARCUS_WORK:-$HERE}"
OUT="${ARCUS_OUT:-$HERE/out}"
# The container sees this folder as /src: the same places under it (both must be inside it).
CWORK="/src${WORK#"$HERE"}"; COUT="/src${OUT#"$HERE"}"
# The Switch build is the full-screen one: build/arcus-tall.bin (tools/build.py --tall).
# rom/arcus-odyssey-usa-dx.bin: your ROM with our patch, made by build.py; ../build/: the development tree.
DEFAULT_ROM="$WORK/rom/arcus-odyssey-usa-dx.bin"; [ -f "$DEFAULT_ROM" ] || DEFAULT_ROM="$HERE/../build/arcus-tall.bin"
ROM="${ARCUS_ROM:-$DEFAULT_ROM}"
NX=nx; OUTNAME=arcus-odyssey-dx; NACP_NAME="Arcus Odyssey DX"
[ -f "$ROM" ] || { echo "ROM not found: $ROM" >&2; exit 1; }
EXTRA="-DARCUS_TALL=ON -DARCUS_ROM_CRC32=0x$(python3 -c "import zlib,sys;print('%08X'%(zlib.crc32(open(sys.argv[1],'rb').read())&0xffffffff))" "$ROM")"
# Pinned: the image every release build was made and tested with (2026-02-19).
IMAGE="${DEVKITPRO_IMAGE:-devkitpro/devkita64@sha256:1fc388c3a0d34bd2045a6dadcb1020e069d5f876a187fd705de14b4440c00282}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
mkdir -p "$OUT"

# The icon is regenerated every build by tools/make_icon.py (below): the box artwork with the
# logo, downloaded and pinned like the extras; offline, a title capture, which needs a cached
# out/title.png or the build/ executable.
# Extras (the manual's pages) from their public URLs, rendered on the Mac (the container sees
# extras-cache/ through the /src mount); the build goes on without them if unavailable.
# ARCUS_EXTRAS=cached: build.py already downloaded them.
[ "${ARCUS_EXTRAS:-}" = cached ] || python3 "$HERE/tools/fetch-extras.py"
# The game guide needs a host build of the game to capture its pictures (tools/make-guide.py):
# the Mac app if built (tools/build-mac.sh), else the test build; without one, no guide.
# ARCUS_HOST_GAME overrides (build.py); a guide already made for this ROM needs none.
HOST_GAME="${ARCUS_HOST_GAME:-$WORK/build-mac/Arcus Odyssey DX.app/Contents/MacOS/Arcus Odyssey DX}"
[ -x "$HOST_GAME" ] || HOST_GAME="$HERE/build-tall/ArcusOdyssey"
if [ -x "$HOST_GAME" ] || [ -f "$WORK/extras-cache/books/02-guide/stamp.txt" ]; then
    python3 "$HERE/tools/make-guide.py" --exe "$HOST_GAME"
else
    echo "[guide] warning: no host build to capture with (run tools/build-mac.sh first); building without the guide" >&2
fi
# Python with Pillow: $PYTHON (build.py sets it), else the repo's .venv, else the development
# tree's ../.venv, else python3.
if [ -z "${PYTHON:-}" ]; then
    for PYTHON in "$WORK/venv/bin/python" "$HERE/../.venv/bin/python" python3; do
        command -v "$PYTHON" >/dev/null 2>&1 && break
    done
fi
# Offline, the icon is a title capture taken with the host build (the guide's, if any).
ICON_EXE="$HOST_GAME"; [ -x "$ICON_EXE" ] || ICON_EXE="$HERE/build/ArcusOdyssey"
# Offline with no capture either: no icon (the .nro is made without one) rather than no build.
"$PYTHON" "$HERE/tools/make_icon.py" --exe "$ICON_EXE" --out "$OUT" ||
    echo "[icon] warning: no icon this time (offline, and no game build to capture the title with)" >&2
# On Linux, run as you, so the files the container writes into build/ stay yours (Docker
# Desktop on a Mac maps ownership itself).
DUSER=(); [ "$(uname -s)" = Linux ] && DUSER=(--user "$(id -u):$(id -g)" -e HOME=/tmp)
docker run --rm ${DUSER[@]+"${DUSER[@]}"} \
    -v "$HERE":/src \
    -v "$ROM":/rom/arcus.bin:ro \
    -e BUILD_TYPE="$BUILD_TYPE" -e NX="$NX" -e EXTRA="$EXTRA" -e CWORK="$CWORK" -e COUT="$COUT" \
    "$IMAGE" bash -euo pipefail -c '
        git config --global --add safe.directory "*" || true
        cmake -S "$CWORK/engine/recompiler" -B "$CWORK/build-switch/host-recomp" -G Ninja \
              -DCMAKE_BUILD_TYPE=Release
        ninja -C "$CWORK/build-switch/host-recomp"
        cmake -S /src -B "$CWORK/build-switch/$NX" -G Ninja $EXTRA -DARCUS_WORK_DIR="$CWORK" \
              -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake \
              -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
              -DARCUS_ROM=/rom/arcus.bin \
              -DARCUS_NRO_ICON="$COUT/icon.jpg" \
              -DGENESISRECOMP_EXECUTABLE="$CWORK/build-switch/host-recomp/GenesisRecomp"
        ninja -C "$CWORK/build-switch/$NX"
    '

cp "$WORK/build-switch/$NX/arcus-odyssey-dx.nro" "$OUT/$OUTNAME.nro"
python3 "$HERE/tools/check_nro.py" --name "$NACP_NAME" "$OUT/$OUTNAME.nro"
echo "built $OUT/$OUTNAME.nro"
