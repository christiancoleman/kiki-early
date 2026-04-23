#!/usr/bin/env bash
# run_game.sh — launch Pokémon LE under qemu-aarch64 with the shim preloaded.
#
# The repo layout is the Spike 3 SD-card filesystem used as a sysroot. This
# script auto-detects everything from its own location — no env setup needed
# in the normal case. Clone the repo anywhere, run ./run_game.sh.
#
# USAGE:
#   ./run_game.sh                         # 185 s real-GL run, defaults on
#   TIMEOUT=600 ./run_game.sh             # 10-minute run
#   LOGFILE=myrun.log ./run_game.sh       # capture stdout+stderr to file
#   REAL_GL=0 ./run_game.sh               # use stub EGL (old Step 54 mode)
#   ./run_game.sh --help                  # qemu-aarch64 help
#   ./run_game.sh -strace 2>trace.txt     # pass extra flags straight to qemu
#
# TUNABLE ENV OVERRIDES (defaults in []):
#   TIMEOUT=<sec>           [185]   run duration before SIGTERM; 0 = no timeout
#   REAL_GL=<0|1>           [1]     forward EGL/GLES to Mesa vs return stubs
#   FRAME_BRIDGE=<0|1>      [1]     emit RGBA frames on /tmp/spike3_frame.sock
#   PATCH_SPI_VIDEO=<0|1>   [1]     vtable-patch SpiDecoder::Play (Step 62)
#   SW_HEVC=<0|1>           [1]     NOP-patch get_format + accept SW pix_fmt
#   STATE_BRIDGE=<0|1>      [0]     emit cmode JSON on /tmp/spike3_bridge.sock
#   LOGFILE=<path>          []      redirect stdout+stderr; empty = inherit
#   QEMU=<bin>              [qemu-aarch64]     override qemu binary
#
# PREREQS (see SETUP.md for details):
#   - qemu-user (provides qemu-aarch64 + binfmt_misc)
#   - gcc-aarch64-linux-gnu (only if you modify spi_shim.c — run ./build.sh)
#   - Mesa overlay built: cd mesa_sw_overlay && ./build_overlay.sh

set -euo pipefail

# ---- Auto-detect paths ----
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# _emulation → pokemon_le → games → sysroot
SYSROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
GAME_DIR="$SYSROOT/games/pokemon_le"

# ---- Defaults (override via env) ----
TIMEOUT="${TIMEOUT:-185}"
REAL_GL="${REAL_GL:-1}"
FRAME_BRIDGE="${FRAME_BRIDGE:-1}"
PATCH_SPI_VIDEO="${PATCH_SPI_VIDEO:-1}"
SW_HEVC="${SW_HEVC:-1}"
STATE_BRIDGE="${STATE_BRIDGE:-0}"
LOGFILE="${LOGFILE:-}"
QEMU="${QEMU:-qemu-aarch64}"

# ---- Derived ----
SHIM="$SCRIPT_DIR/spi_shim.so"
OVERLAY="$SCRIPT_DIR/mesa_sw_overlay/pkg/usr/lib/aarch64-linux-gnu"
DRIVERS="$OVERLAY/dri"

# ---- Preflight ----
die() { echo "ERROR: $*" >&2; exit 1; }

[[ -f "$GAME_DIR/game" ]] || die "game binary not found at $GAME_DIR/game"
[[ -f "$SHIM" ]]          || die "shim not built. Run ./build.sh first."
command -v "$QEMU" >/dev/null || die "$QEMU not in PATH. Install qemu-user."
if [[ "$REAL_GL" == "1" && ! -d "$OVERLAY" ]]; then
    die "Mesa overlay not built at $OVERLAY. Run: (cd mesa_sw_overlay && ./build_overlay.sh)"
fi

# ---- Compose command ----
args=(
    "$QEMU" -L "$SYSROOT"
    -E "LD_PRELOAD=$SHIM"
    -E "LD_LIBRARY_PATH=$OVERLAY:$SYSROOT/usr/lib64:$SYSROOT/usr/lib:$SYSROOT/lib64:$SYSROOT/lib"
    -E "SPIKE3_SYSROOT=$SYSROOT"
)

if [[ "$REAL_GL" == "1" ]]; then
    args+=(
        -E "SPIKE3_REAL_GL=1"
        -E "LIBGL_DRIVERS_PATH=$DRIVERS"
        -E "MESA_LOADER_DRIVER_OVERRIDE=swrast"
        -E "LIBGL_ALWAYS_SOFTWARE=1"
    )
fi
[[ "$FRAME_BRIDGE"    == "1" ]] && args+=(-E "SPIKE3_FRAME_BRIDGE=1")
[[ "$PATCH_SPI_VIDEO" == "1" ]] && args+=(-E "SPIKE3_PATCH_SPI_VIDEO=1")
[[ "$SW_HEVC"         == "1" ]] && args+=(-E "SPIKE3_SW_HEVC=1")
[[ "$STATE_BRIDGE"    == "1" ]] && args+=(-E "SPIKE3_BRIDGE=1")

# Append any caller-supplied flags and the game binary last.
args+=("$@" ./game)

# Wrap in `timeout` unless TIMEOUT=0
if [[ "$TIMEOUT" != "0" ]]; then
    args=(timeout "$TIMEOUT" "${args[@]}")
fi

# Echo the command so the user can see what's about to run.
printf '[run_game] SYSROOT=%s\n' "$SYSROOT" >&2
printf '[run_game] %s\n'        "${args[*]}" >&2

cd "$GAME_DIR"
if [[ -n "$LOGFILE" ]]; then
    exec "${args[@]}" >"$LOGFILE" 2>&1
else
    exec "${args[@]}"
fi
