#!/usr/bin/env bash
# Build the Linux package: arcus-odyssey-dx-<version>-linux-<arch>.tar.gz
#
# The full-screen game (the --tall ROM, as on the Switch and the Mac), like build-mac.sh:
#   0. extras (the manual, magazine reviews) fetched from their public URLs
#      (tools/fetch-extras.py); after the first build, the game guide is made by running the
#      new program itself (tools/make-guide.py) and embedded by a second, incremental build.
#      ARCUS_EXTRAS=cached skips both and embeds what extras-cache/ already holds (the Docker
#      build from the Mac does this: the Mac build made them);
#   1. CMake build in build-linux-<arch>/ (ARCUS_LINUX_APP: saves in ~/.local/share, no TCP
#      debug server);
#   2. the package: a folder with the program, an icon, a .desktop entry and a README, as a
#      tarball. The program needs only glibc 2.35+ when SDL2 is the static one from
#      platform/linux/Dockerfile; with a distribution's SDL2 it needs that SDL2 installed.
# Runs on Linux (x86_64 or arm64). On a Mac: tools/build-linux-docker.sh.
# Needs cmake, ninja, a C/C++ compiler, SDL2 (development files) and python3 with Pillow.
#
# usage: tools/build-linux.sh            (ARCUS_ROM=... to use another full-screen input ROM)
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
# ARCUS_WORK: where generated files go (build.py: build/; else this folder). ARCUS_OUT: the result.
WORK="${ARCUS_WORK:-$HERE}"
# rom/arcus-odyssey-usa-dx.bin: your ROM with our patch, made by build.py; ../build/: the development tree.
DEFAULT_ROM="$WORK/rom/arcus-odyssey-usa-dx.bin"; [ -f "$DEFAULT_ROM" ] || DEFAULT_ROM="$HERE/../build/arcus-tall.bin"
ROM="${ARCUS_ROM:-$DEFAULT_ROM}"
ARCH="$(uname -m)"
BUILD="${ARCUS_BUILD_DIR:-$WORK/build-linux-$ARCH}"
OUT="${ARCUS_OUT:-$HERE/out}"
EXE="$BUILD/arcus-odyssey-dx"
[ "$(uname -s)" = Linux ] || { echo "build-linux.sh runs on Linux (on a Mac: tools/build-linux-docker.sh)" >&2; exit 1; }
[ -f "$ROM" ] || { echo "ROM not found: $ROM" >&2; exit 1; }
CRC="0x$(python3 -c "import zlib,sys;print('%08X'%(zlib.crc32(open(sys.argv[1],'rb').read())&0xffffffff))" "$ROM")"
VERSION="$(sed -n 's/^project(ArcusOdysseyRecomp VERSION \([0-9.]*\).*/\1/p' "$HERE/CMakeLists.txt")"
mkdir -p "$OUT"

# Extras (the manual, reviews) from their public URLs; the build goes on without them if unavailable.
case "${ARCUS_EXTRAS:-}" in cached|guide) ;; *) python3 "$HERE/tools/fetch-extras.py" ;; esac

cmake -S "$HERE" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DARCUS_TALL=ON -DARCUS_ROM="$ROM" -DARCUS_ROM_CRC32="$CRC" \
      -DARCUS_LINUX_APP=ON -DGEN_ENABLE_TRACE=OFF -DARCUS_WORK_DIR="$WORK" >/dev/null
ninja -C "$BUILD" ArcusOdyssey
if [ "${ARCUS_EXTRAS:-}" != cached ]; then
    # The game guide: pictures captured by running the program just built (cached per ROM),
    # then embedded (CMake re-globs extras-cache/books; nothing to do when it was current).
    python3 "$HERE/tools/make-guide.py" --exe "$EXE"
    cmake "$BUILD" >/dev/null
    ninja -C "$BUILD" ArcusOdyssey
fi

# What the program needs from the system: glibc only (a static SDL2), or SDL2 too.
NEEDED="$(objdump -p "$EXE" | awk '/NEEDED/ {print $2}' | tr '\n' ' ')"
GLIBC="$(objdump -T "$EXE" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)"
if echo "$NEEDED" | tr ' ' '\n' | grep -vE '^(libc|libm|libdl|libpthread|librt|ld-linux.*)\.so' | grep -q .; then
    echo "note: needs $(echo "$NEEDED" | tr ' ' '\n' | grep -vE '^(libc|libm|libdl|libpthread|librt|ld-linux.*)\.so' | tr '\n' ' ')from the system"
fi

# The package folder, staged in a work dir and swapped into out/ (the previous tarball is
# moved aside to the work dir, not deleted).
NAME="arcus-odyssey-dx-$VERSION-linux-$ARCH"
TMPW="$(mktemp -d)"; trap 'rm -rf "$TMPW"' EXIT     # scratch: the iconset, the staging, the previous result
PKG="$TMPW/arcus-odyssey-dx"
mkdir -p "$PKG"
cp "$EXE" "$PKG/arcus-odyssey-dx"
strip "$PKG/arcus-odyssey-dx"
# Icon: the Mac/Switch icon (tools/make_icon.py output in out/), 256 px.
ICON_SRC=""
[ -f "$OUT/icon-1024.png" ] && ICON_SRC="$OUT/icon-1024.png"
[ -z "$ICON_SRC" ] && [ -f "$OUT/icon.jpg" ] && ICON_SRC="$OUT/icon.jpg"
if [ -n "$ICON_SRC" ]; then
    python3 -c "import sys;from PIL import Image;Image.open(sys.argv[1]).convert('RGB').resize((256,256),Image.LANCZOS).save(sys.argv[2])" \
        "$ICON_SRC" "$PKG/arcus-odyssey-dx.png" || echo "warning: no icon (Pillow?)" >&2
fi
cp "$HERE/platform/linux/arcus-odyssey-dx.desktop" "$PKG/"
cp "$HERE/platform/linux/install.sh" "$PKG/"
chmod +x "$PKG/install.sh" "$PKG/arcus-odyssey-dx"
sed -e "s/@VERSION@/$VERSION/" -e "s/@ARCH@/$ARCH/" -e "s/@GLIBC@/${GLIBC#GLIBC_}/" \
    "$HERE/platform/linux/README.txt" > "$PKG/README.txt"

TAR="$OUT/$NAME.tar.gz"
tar -C "$TMPW" -czf "$TMPW/$NAME.tar.gz" arcus-odyssey-dx
if [ -e "$TAR" ]; then mv "$TAR" "$TMPW/previous.tar.gz"; fi
mv "$TMPW/$NAME.tar.gz" "$TAR"
echo "built $TAR ($(du -h "$TAR" | cut -f1), $ARCH, needs $GLIBC; links: $NEEDED)"
