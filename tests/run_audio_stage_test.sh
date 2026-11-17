#!/usr/bin/env bash
# Compile and run tests/audio_stage_test.c against engine/runner/audio.c (needs SDL2 headers).
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${TMPDIR:-/tmp}/audio_stage_test"
cc -O1 -std=gnu11 -I"$HERE/engine/runner" -I"$HERE/engine/runner/include" \
   $(pkg-config --cflags sdl2) -I"$(dirname "$(pkg-config --variable=includedir sdl2)/SDL2")" \
   "$HERE/tests/audio_stage_test.c" "$HERE/engine/runner/audio.c" \
   $(pkg-config --libs sdl2) -lm -o "$OUT" 2>&1 | grep -v "warning" || true
"$OUT"
