#!/usr/bin/env bash
# Build the Linux package on a Mac (or any Docker host): runs tools/build-linux.sh in the
# platform/linux/Dockerfile image (Ubuntu 22.04 + a static SDL2), with the project mounted.
#
# The extras are taken as they are in extras-cache/ (ARCUS_EXTRAS=cached, the default): run
# tools/build-mac.sh first so the manual, the reviews and the game guide are there.
# ARCUS_EXTRAS=guide makes the game guide in the container instead (build.py does).
#
# usage: tools/build-linux-docker.sh [x86_64|aarch64|both]     (default: x86_64, the usual PC
#        and the Steam Deck; on Apple silicon it runs emulated, so it is slower)
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
# The public repo (rom/ made by build.py) is mounted alone; the development tree mounts its
# parent, where the ROM is (../build/arcus-tall.bin).
# build.py sets ARCUS_WORK and ARCUS_OUT (build/, inside this folder): the same places under /src.
DENV=()
if [ -n "${ARCUS_WORK:-}" ]; then
    ROOT="$HERE"; WORKDIR=/src
    OUTDIR="${ARCUS_OUT:-$HERE/out}"
    DENV=(-e "ARCUS_WORK=/src${ARCUS_WORK#"$HERE"}" -e "ARCUS_OUT=/src${OUTDIR#"$HERE"}")
elif [ -f "$HERE/rom/arcus-tall.bin" ]; then ROOT="$HERE"; WORKDIR=/src; else ROOT="$(cd "$HERE/.." && pwd)"; WORKDIR=/src/native; fi
ARCHES="${1:-x86_64}"
[ "$ARCHES" = both ] && ARCHES="x86_64 aarch64"

# build trees go in ARCUS_WORK (container path) when build.py set it, else the mounted folder
WORKDIR_BUILD="$WORKDIR"; [ -n "${ARCUS_WORK:-}" ] && WORKDIR_BUILD="/src${ARCUS_WORK#"$HERE"}"
for arch in $ARCHES; do
    case "$arch" in
        x86_64|amd64) plat=linux/amd64; tag=amd64 ;;
        aarch64|arm64) plat=linux/arm64; tag=arm64 ;;
        *) echo "unknown architecture: $arch" >&2; exit 1 ;;
    esac
    docker build --platform "$plat" -t "arcus-linux-build:$tag" "$HERE/platform/linux"
    # Its own build folder (the native build may use build-linux-<arch>), and on Linux it runs
    # as you, so what it writes into build/ stays yours.
    DUSER=(); [ "$(uname -s)" = Linux ] && DUSER=(--user "$(id -u):$(id -g)" -e HOME=/tmp)
    docker run --rm --platform "$plat" ${DUSER[@]+"${DUSER[@]}"} \
        -v "$ROOT:/src" -w "$WORKDIR" \
        -e ARCUS_EXTRAS="${ARCUS_EXTRAS:-cached}" -e "ARCUS_BUILD_DIR=$WORKDIR_BUILD/build-linux-docker-$arch" ${DENV[@]+"${DENV[@]}"} ${ARCUS_ROM:+-e ARCUS_ROM="$ARCUS_ROM"} \
        "arcus-linux-build:$tag" tools/build-linux.sh
done
