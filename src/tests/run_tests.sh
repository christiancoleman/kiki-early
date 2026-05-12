#!/usr/bin/env bash
# run_tests.sh — build + run the host-side unit tests (_emulation/src/tests/).
#
# These run on the HOST compiler (gcc/clang for the machine you're on), not the
# aarch64 cross-compiler that ../build.sh uses — the shim TUs under test are
# portable C and the tests #include them directly to reach their static helpers.
# -I.. points the compiler at _emulation/src/ so a test in tests/<module>/ can
# `#include "spi_shim.h"` / `#include "<module>/<file>.c"`.
#
# Test sources mirror src/'s module layout: tests/nodebus/ tests src/nodebus/.
# Add new module test dirs to the list below as they appear.
#
#   ./run_tests.sh           # build + run everything; exit non-zero on any failure
#   CC=clang ./run_tests.sh  # pick a different host compiler
#
# Build artifacts land in tests/.build/ (git-ignored).

set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$DIR"

CC="${CC:-cc}"
BUILD="$DIR/.build"
mkdir -p "$BUILD"

command -v "$CC" >/dev/null || { echo "ERROR: host compiler '$CC' not found." >&2; exit 1; }
command -v python3 >/dev/null || { echo "ERROR: python3 not found." >&2; exit 1; }

# ---- nodebus module ----
echo "[tests] compiling nodebus/test_nodebus (host: $CC) ..."
"$CC" -std=gnu11 -O0 -g -Wall -Wextra -Wno-unused-function \
      -I.. -o "$BUILD/test_nodebus" nodebus/test_nodebus.c -ldl

# shim_nodebus.c chats to stderr (bridge response hex dumps etc.) — useful when
# a case fails, noise otherwise. Capture it; surface it only on failure.
echo "[tests] === nodebus/test_nodebus (C: src/nodebus/shim_nodebus.c) ==="
if ! "$BUILD/test_nodebus" 2> "$BUILD/test_nodebus.stderr.log"; then
    echo "[tests] --- test_nodebus stderr (shim log) ---" >&2
    cat "$BUILD/test_nodebus.stderr.log" >&2
    exit 1
fi

echo "[tests] === nodebus/test_nodebus_model.py (Python golden model) ==="
python3 nodebus/test_nodebus_model.py

echo "[tests] all green."
