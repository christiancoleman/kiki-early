#!/usr/bin/env bash
# run_game.sh — launch Pokémon LE under qemu-aarch64 with the shim preloaded,
# plus the live frame viewer and audio player by default.
#
# The repo layout is the Spike 3 SD-card filesystem used as a sysroot. This
# script auto-detects everything from its own location — no env setup needed
# in the normal case. Clone the repo anywhere, run ./run_game.sh.
#
# USAGE:
#   ./run_game.sh                         # play: shim + frame viewer + audio,
#                                           no timeout. Press Ctrl+C to stop.
#   TIMEOUT=185 ./run_game.sh             # cap at 185 s (useful for smoke runs)
#   VIEWER=0 ./run_game.sh                # game only, no Tk viewer window
#   AUDIO_PLAYER=0 ./run_game.sh          # game only, no ffplay audio
#   LOGFILE=myrun.log ./run_game.sh       # capture game stdout+stderr to file
#   REAL_GL=0 ./run_game.sh               # use stub EGL (old Step 54 mode)
#   ./run_game.sh --help                  # pass --help through to qemu-aarch64
#   ./run_game.sh -strace 2>trace.txt     # pass extra flags straight to qemu
#
# TUNABLE ENV OVERRIDES (defaults in []):
#   TIMEOUT=<sec>           [0]     cap run duration; 0 = no limit (default play)
#   VIEWER=<0|1>            [1]     auto-launch python3 frame_viewer.py
#   AUDIO_PLAYER=<0|1>      [1]     auto-launch audio_subscriber.py | ffplay
#   STALE_KILL=<0|1>        [1]     reap leftover qemu/helpers from prior runs
#                                   before starting; disable if you want to
#                                   attach to an existing qemu intentionally
#   REAL_GL=<0|1>           [1]     forward EGL/GLES to Mesa vs return stubs
#   FRAME_BRIDGE=<0|1>      [1]     emit RGBA frames on /tmp/spike3_frame.sock
#   AUDIO_BRIDGE=<0|1>      [1]     emit S16_LE PCM on /tmp/spike3_audio.sock
#   PATCH_SPI_VIDEO=<0|1>   [1]     vtable-patch SpiDecoder::Play (Step 62)
#   SW_HEVC=<0|1>           [1]     NOP-patch get_format + accept SW pix_fmt
#   STATE_BRIDGE=<0|1>      [0]     emit cmode JSON on /tmp/spike3_bridge.sock
#   LOGFILE=<path>          []      redirect game stdout+stderr; empty = inherit
#   QEMU=<bin>              [qemu-aarch64]     override qemu binary
#
# PREREQS (see docs/SETUP.md for details):
#   - qemu-user (provides qemu-aarch64 + binfmt_misc)
#   - gcc-aarch64-linux-gnu (only if you modify the shim — run ./build.sh)
#   - Mesa overlay built: cd mesa_sw_overlay && ./build_overlay.sh
#   - Optional: python3 + numpy + Pillow + tk   (live viewer)
#   - Optional: ffplay                           (live audio)

# -e intentionally omitted: background helpers exit non-zero on kill, which
# is normal and shouldn't tear down the script.
set -uo pipefail

# ---- Auto-detect paths ----
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# _emulation → pokemon_le → games → sysroot
SYSROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
GAME_DIR="$SYSROOT/games/pokemon_le"

# ---- Defaults (override via env) ----
TIMEOUT="${TIMEOUT:-0}"
REAL_GL="${REAL_GL:-1}"
FRAME_BRIDGE="${FRAME_BRIDGE:-1}"
AUDIO_BRIDGE="${AUDIO_BRIDGE:-1}"
PATCH_SPI_VIDEO="${PATCH_SPI_VIDEO:-1}"
SW_HEVC="${SW_HEVC:-1}"
STATE_BRIDGE="${STATE_BRIDGE:-0}"
VIEWER="${VIEWER:-1}"
AUDIO_PLAYER="${AUDIO_PLAYER:-1}"
STALE_KILL="${STALE_KILL:-1}"
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

# ---- Reap leftover processes from prior runs ----
# The shim's SIGTERM handler does a state dump but doesn't always cleanly
# exit — so a previous Ctrl+C can leave qemu + helpers behind, holding
# socket endpoints and the audio file handle. Purge them before starting.
if [[ "$STALE_KILL" == "1" ]]; then
    stale_qemu=$(pgrep -f "qemu-aarch64.*LD_PRELOAD=$SHIM" 2>/dev/null || true)
    stale_help=$(pgrep -f "$SCRIPT_DIR/(frame_viewer|audio_subscriber)\.py" 2>/dev/null || true)
    stale_fplay=$(pgrep -f "ffplay.*s16le.*44100" 2>/dev/null || true)
    stale_all="$stale_qemu $stale_help $stale_fplay"
    if [[ -n "${stale_all// }" ]]; then
        printf '[run_game] reaping stale processes from prior runs: %s\n' "$stale_all" >&2
        # SIGTERM first (lets shim dump cleanly if it wants), then SIGKILL.
        for pid in $stale_all; do kill -TERM "$pid" 2>/dev/null || true; done
        sleep 0.5
        for pid in $stale_all; do kill -KILL "$pid" 2>/dev/null || true; done
    fi
    # Clean stale socket files (shim does this itself on fresh bind, but
    # belt-and-suspenders for the case where the stale qemu is still holding
    # them at the kernel level).
    rm -f /tmp/spike3_frame.sock /tmp/spike3_audio.sock /tmp/spike3_bridge.sock
fi

# ---- Background-process bookkeeping ----
# The game (qemu) plus the helpers are all backgrounded so the trap can tear
# them down on Ctrl+C. Trap priorities: kill qemu first (it holds the file
# handles + sockets), SIGTERM with a grace period, then SIGKILL if unresponsive.
QEMU_PID=""
declare -a HELPER_PIDS=()
declare -a HELPER_LABELS=()

cleanup_all() {
    # 1) qemu
    if [[ -n "$QEMU_PID" ]] && kill -0 "$QEMU_PID" 2>/dev/null; then
        printf '[run_game] stopping qemu (pid %d)\n' "$QEMU_PID" >&2
        kill -TERM "$QEMU_PID" 2>/dev/null || true
        # Short grace period so the shim's SIGTERM handler can run its dump.
        for _ in 1 2 3; do
            kill -0 "$QEMU_PID" 2>/dev/null || break
            sleep 0.5
        done
        if kill -0 "$QEMU_PID" 2>/dev/null; then
            printf '[run_game] qemu unresponsive to SIGTERM, sending SIGKILL\n' >&2
            kill -KILL "$QEMU_PID" 2>/dev/null || true
        fi
    fi

    # 2) helpers (if still alive after qemu dropped their sockets)
    if (( ${#HELPER_PIDS[@]} > 0 )); then
        for i in "${!HELPER_PIDS[@]}"; do
            local pid="${HELPER_PIDS[$i]}"
            local lbl="${HELPER_LABELS[$i]}"
            if kill -0 "$pid" 2>/dev/null; then
                printf '[run_game] stopping %s (pid %d)\n' "$lbl" "$pid" >&2
                pkill -P "$pid" 2>/dev/null || true
                kill "$pid" 2>/dev/null || true
            fi
        done
    fi
    wait 2>/dev/null || true
}
trap cleanup_all EXIT INT TERM

# ---- Launch helpers BEFORE the game ----
# Both subscribers have internal retry loops so it doesn't matter that the
# shim hasn't opened its sockets yet.
if [[ "$VIEWER" == "1" && "$FRAME_BRIDGE" == "1" ]]; then
    if command -v python3 >/dev/null; then
        python3 "$SCRIPT_DIR/frame_viewer.py" >/dev/null 2>&1 &
        HELPER_PIDS+=("$!"); HELPER_LABELS+=("frame viewer")
        printf '[run_game] spawned frame viewer (pid %d)\n' "$!" >&2
    else
        printf '[run_game] VIEWER=1 but python3 not found; skipping viewer\n' >&2
    fi
fi

if [[ "$AUDIO_PLAYER" == "1" && "$AUDIO_BRIDGE" == "1" ]]; then
    if command -v python3 >/dev/null && command -v ffplay >/dev/null; then
        # Subshell groups the python | ffplay pipe so cleanup_helpers' pkill -P
        # reaps both halves.
        ( python3 "$SCRIPT_DIR/audio_subscriber.py" 2>/dev/null | \
          ffplay -f s16le -ar 44100 -ac 2 -nodisp -autoexit -loglevel error -i - 2>/dev/null ) &
        HELPER_PIDS+=("$!"); HELPER_LABELS+=("audio player (subscriber | ffplay)")
        printf '[run_game] spawned audio player (subshell pid %d)\n' "$!" >&2
    else
        printf '[run_game] AUDIO_PLAYER=1 but python3 or ffplay not found; skipping\n' >&2
    fi
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
[[ "$AUDIO_BRIDGE"    == "1" ]] && args+=(-E "SPIKE3_AUDIO_BRIDGE=1")
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
if [[ "$TIMEOUT" == "0" ]]; then
    printf '[run_game] no timeout — press Ctrl+C to stop the game\n' >&2
fi

cd "$GAME_DIR"
# Background qemu + wait so the trap can kill it directly on Ctrl+C (bash
# doesn't forward signals to foreground children reliably, and the shim's
# SIGINT handler doesn't always terminate — see cleanup_all's SIGKILL path).
if [[ -n "$LOGFILE" ]]; then
    "${args[@]}" >"$LOGFILE" 2>&1 &
else
    "${args[@]}" &
fi
QEMU_PID=$!
# `wait` returns when qemu exits OR when a trapped signal arrives (SIGINT
# from the terminal). Either way, the trap runs cleanup_all before we leave.
wait "$QEMU_PID" 2>/dev/null
GAME_EXIT=$?

printf '[run_game] game exited (status %d)\n' "$GAME_EXIT" >&2
# cleanup_all runs via the EXIT trap; exit with the game's status so
# callers can still detect timeout (124) or crash.
exit "$GAME_EXIT"
