#!/usr/bin/env bash
# build.sh — compile spi_shim.so from all translation units.
#
# Sources live in this directory (= _emulation/src/). Carved-out modules get
# their own subdir — currently nodebus/. Add new subdir paths to SOURCES below
# as more modules are split out; -I"$SRC_DIR" lets every TU keep
# `#include "spi_shim.h"` regardless of which subdir it lives in.
#
# USAGE:
#   ./build.sh                # optimized build (-O2)
#   DEBUG=1 ./build.sh        # debug build (-O0 -g)
#   CC=clang-aarch64 ./build.sh   # alternative cross-compiler
#
# PREREQ: gcc-aarch64-linux-gnu  (sudo apt install gcc-aarch64-linux-gnu)

set -euo pipefail

# SRC_DIR is _emulation/src/ — auto-located from this script. (Collaborators
# with a non-standard checkout can run from anywhere; the script self-locates.)
SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SRC_DIR"

CC="${CC:-aarch64-linux-gnu-gcc}"
if [[ "${DEBUG:-0}" == "1" ]]; then
    CFLAGS="${CFLAGS:--Wall -Wextra -shared -fPIC -O0 -g}"
else
    CFLAGS="${CFLAGS:--Wall -Wextra -shared -fPIC -O2}"
fi

command -v "$CC" >/dev/null || {
    echo "ERROR: $CC not in PATH. Install gcc-aarch64-linux-gnu." >&2
    exit 1
}

# Source list — paths relative to SRC_DIR. Carved-out modules live in subdirs.
SOURCES=(
    spi_shim.c
    shim_bridges.c
    shim_diagnostics.c
    nodebus/shim_nodebus.c
    shim_discovery.c
    shim_fs.c
    shim_gl.c
    shim_io.c
    shim_ioctl.c
    shim_patches.c
    shim_avforward.c
    shim_gl_marshal.c
    shim_switches.c
    shim_inject.c
    shim_net.c
    shim_devbus.c
    games/pokemon_le_machine.c
)

VERSION_SCRIPT="$SRC_DIR/spi_shim.ver"

echo "[build] $CC $CFLAGS -I$SRC_DIR"
echo "[build] sources: ${SOURCES[*]}"
$CC $CFLAGS -I"$SRC_DIR" -o spi_shim.so "${SOURCES[@]}" \
    -Wl,--version-script="$VERSION_SCRIPT" \
    -ldl -lpthread
printf '[build] OK — spi_shim.so: %s bytes\n' "$(stat -c%s spi_shim.so)"
