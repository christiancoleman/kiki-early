#!/usr/bin/env bash
# build.sh — compile spi_shim.so from all translation units.
#
# USAGE:
#   ./build.sh                # optimized build (-O2)
#   DEBUG=1 ./build.sh        # debug build (-O0 -g)
#   CC=clang-aarch64 ./build.sh   # alternative cross-compiler
#
# PREREQ: gcc-aarch64-linux-gnu  (sudo apt install gcc-aarch64-linux-gnu)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

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

# Source list — update as modules are carved out of spi_shim.c.
SOURCES=(
    spi_shim.c
    shim_bridges.c
    shim_diagnostics.c
    shim_nodebus.c
    shim_fs.c
)

echo "[build] $CC $CFLAGS"
echo "[build] sources: ${SOURCES[*]}"
$CC $CFLAGS -o spi_shim.so "${SOURCES[@]}" -ldl -lpthread
printf '[build] OK — spi_shim.so: %s bytes\n' "$(stat -c%s spi_shim.so)"
