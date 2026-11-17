#!/usr/bin/env bash
# Build the macOS app: out/Arcus Odyssey DX.app
#
# The full-screen game (the --tall ROM, as on the Switch) as a normal Mac app, built natively
# for this Mac's architecture:
#   0. extras (the manual, magazine reviews) fetched from their public URLs
#      (tools/fetch-extras.py); after the first build, the game guide is made by running the
#      new app itself (tools/make-guide.py) and embedded by a second, incremental build;
#   1. CMake build in build-mac/ (ARCUS_MAC_APP: app bundle, saves in Application Support,
#      no TCP debug server);
#   2. the icon (out/icon.jpg, tools/make_icon.py) becomes an .icns;
#   3. SDL2 is copied into the bundle so the app runs without Homebrew, and the bundle is
#      signed ad hoc (built locally, so macOS does not quarantine it). Homebrew's sdl2 is now
#      sdl2-compat, which loads SDL3 at run time from beside itself: SDL3 is bundled too then.
# Needs Homebrew sdl2, cmake and ninja.
#
# usage: tools/build-mac.sh            (ARCUS_ROM=... to use another full-screen input ROM)
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
# ARCUS_WORK: where generated files go (build.py: build/; else this folder). ARCUS_OUT: the result.
WORK="${ARCUS_WORK:-$HERE}"
# rom/arcus-odyssey-usa-dx.bin: your ROM with our patch, made by build.py; ../build/: the development tree.
DEFAULT_ROM="$WORK/rom/arcus-odyssey-usa-dx.bin"; [ -f "$DEFAULT_ROM" ] || DEFAULT_ROM="$HERE/../build/arcus-tall.bin"
ROM="${ARCUS_ROM:-$DEFAULT_ROM}"
APP_NAME="Arcus Odyssey DX"
BUILD="$WORK/build-mac"
OUT="${ARCUS_OUT:-$HERE/out}"
[ -f "$ROM" ] || { echo "ROM not found: $ROM" >&2; exit 1; }
CRC="0x$(python3 -c "import zlib,sys;print('%08X'%(zlib.crc32(open(sys.argv[1],'rb').read())&0xffffffff))" "$ROM")"
mkdir -p "$OUT"

# Extras (the manual, reviews) from their public URLs; the build goes on without them if unavailable.
# ARCUS_EXTRAS=cached: build.py already downloaded them.
[ "${ARCUS_EXTRAS:-}" = cached ] || python3 "$HERE/tools/fetch-extras.py"

cmake -S "$HERE" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DARCUS_TALL=ON -DARCUS_ROM="$ROM" -DARCUS_ROM_CRC32="$CRC" \
      -DARCUS_MAC_APP=ON -DGEN_ENABLE_TRACE=OFF -DARCUS_WORK_DIR="$WORK" >/dev/null
ninja -C "$BUILD" ArcusOdyssey
# The game guide: pictures captured by running the app just built (cached per ROM), then
# embedded (CMake re-globs extras-cache/books; nothing to do when the guide was current).
python3 "$HERE/tools/make-guide.py" --exe "$BUILD/$APP_NAME.app/Contents/MacOS/$APP_NAME"
cmake "$BUILD" >/dev/null
ninja -C "$BUILD" ArcusOdyssey

# Icon: tools/make_icon.py, every build (the box artwork with the logo, both downloaded and
# pinned like the extras; else a title capture). From out/icon-1024.png the iconset goes up to
# 512@2x; from the 256 px out/icon.jpg (capture fallback) only up to 256.
# Python with Pillow: $PYTHON (build.py sets it), else the repo's .venv, else the development
# tree's ../.venv, else python3.
if [ -z "${PYTHON:-}" ]; then
    for PYTHON in "$WORK/venv/bin/python" "$HERE/../.venv/bin/python" python3; do
        command -v "$PYTHON" >/dev/null 2>&1 && break
    done
fi
# Offline, the icon is a title capture taken with the app just built.
"$PYTHON" "$HERE/tools/make_icon.py" --exe "$BUILD/$APP_NAME.app/Contents/MacOS/$APP_NAME" --out "$OUT"
ICON_SRC="$OUT/icon.jpg"; ICON_MAX=256
[ -f "$OUT/icon-1024.png" ] && { ICON_SRC="$OUT/icon-1024.png"; ICON_MAX=1024; }
TMPW="$(mktemp -d)"; trap 'rm -rf "$TMPW"' EXIT     # scratch: the iconset, the staging, the previous result
ICONSET="$TMPW/arcus-odyssey-dx.iconset"
mkdir -p "$ICONSET"
for s in 16 32 128 256 512; do
    [ $s -le $ICON_MAX ] && sips -s format png -z $s $s "$ICON_SRC" --out "$ICONSET/icon_${s}x${s}.png" >/dev/null
    d=$((s * 2))
    [ $d -le $ICON_MAX ] && sips -s format png -z $d $d "$ICON_SRC" --out "$ICONSET/icon_${s}x${s}@2x.png" >/dev/null
done
iconutil -c icns "$ICONSET" -o "$TMPW/arcus-odyssey-dx.icns"

# Stage the bundle in the work dir, then swap it into out/ (the previous app is moved aside
# to the work dir, not deleted).
STAGE="$TMPW/$APP_NAME.app"
ditto "$BUILD/$APP_NAME.app" "$STAGE"
mkdir -p "$STAGE/Contents/Resources" "$STAGE/Contents/Frameworks"
cp "$TMPW/arcus-odyssey-dx.icns" "$STAGE/Contents/Resources/"

BIN="$STAGE/Contents/MacOS/$APP_NAME"
SDL_REF="$(otool -L "$BIN" | awk '/libSDL2/ {print $1; exit}')"
[ -n "$SDL_REF" ] || { echo "SDL2 not linked?" >&2; exit 1; }
SDL_LIB="$(cd "$(dirname "$SDL_REF")" && pwd -P)/$(basename "$SDL_REF")"
SDL_NAME="$(basename "$SDL_REF")"
cp -L "$SDL_LIB" "$STAGE/Contents/Frameworks/$SDL_NAME"
chmod u+w "$STAGE/Contents/Frameworks/$SDL_NAME"
install_name_tool -id "@executable_path/../Frameworks/$SDL_NAME" "$STAGE/Contents/Frameworks/$SDL_NAME"
install_name_tool -change "$SDL_REF" "@executable_path/../Frameworks/$SDL_NAME" "$BIN"
BUNDLED=("$STAGE/Contents/Frameworks/$SDL_NAME")

# sdl2-compat (Homebrew's sdl2 since 2025) is SDL2's interface over SDL3, which it dlopens as
# @loader_path/libSDL3.dylib among others. Without it in the bundle, SDL shows an error dialog
# before main() and the app never starts.
if strings "$SDL_LIB" | grep -q '@loader_path/libSDL3.dylib'; then
    SDL3_LIB=""
    for d in "$(dirname "$SDL_LIB")" "$(brew --prefix sdl3 2>/dev/null)/lib"; do
        [ -f "$d/libSDL3.0.dylib" ] && { SDL3_LIB="$d/libSDL3.0.dylib"; break; }
    done
    [ -n "$SDL3_LIB" ] || { echo "SDL2 here is sdl2-compat, but SDL3 wasn't found (brew install sdl3)" >&2; exit 1; }
    cp -L "$SDL3_LIB" "$STAGE/Contents/Frameworks/libSDL3.dylib"
    chmod u+w "$STAGE/Contents/Frameworks/libSDL3.dylib"
    install_name_tool -id "@executable_path/../Frameworks/libSDL3.dylib" "$STAGE/Contents/Frameworks/libSDL3.dylib"
    BUNDLED+=("$STAGE/Contents/Frameworks/libSDL3.dylib")
    echo "bundled SDL3 for sdl2-compat"
fi

# Nothing may still point outside the bundle or the system.
if otool -L "$BIN" "${BUNDLED[@]}" | tail -n +2 | grep -vE "^[^[:space:]]|@executable_path|/usr/lib/|/System/Library/" ; then
    echo "unbundled library dependency (above)" >&2
    exit 1
fi

for lib in "${BUNDLED[@]}"; do codesign --force --sign - "$lib"; done
codesign --force --sign - "$STAGE"
codesign --verify --strict "$STAGE"

if [ -e "$OUT/$APP_NAME.app" ]; then mv "$OUT/$APP_NAME.app" "$TMPW/previous.app"; fi
mv "$STAGE" "$OUT/$APP_NAME.app"
echo "built $OUT/$APP_NAME.app ($(du -sh "$OUT/$APP_NAME.app" | cut -f1), $(uname -m))"
