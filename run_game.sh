#!/usr/bin/env bash
#
# The repo layout is the Spike 3 SD-card filesystem used as a sysroot. This
# script auto-detects everything from its own location — no env setup needed
# in the normal case. Clone the repo anywhere, run ./run_game.sh.
#
#
# TUNABLE ENV OVERRIDES (defaults in []):
#   TIMEOUT=<sec>           [0]     cap run duration; 0 = no limit (default play)
#   VIEWER=<0|1>            [1]     auto-launch python3 frame_viewer.py
#   VIEWER_FULLSCREEN=<0|1> [0]     open the viewer fullscreen rather than
#                                   in a 1920×1080 window
#   AUDIO_PLAYER=<0|1>      [1]     auto-launch audio_subscriber.py | ffplay
#   BOOT_DISPLAY=<0|1>      [1]     run spike3/bin/boot_display for ~1.5 s
#                                   pre-game, then kill it (mimics
#                                   etc/init.d/game_monitor). Lets boot_display
#                                   write /games/data/boot_display_cmd which the
#                                   game reads at startup.
#   BOOT_DISPLAY_DURATION=<s> [1.5]  how long to leave boot_display alive.
#   STALE_KILL=<0|1>        [1]     reap leftover qemu/helpers from prior runs
#                                   before starting.
#   REAL_GL=<0|1>           [1]     forward EGL/GLES to Mesa vs return stubs
#   FRAME_BRIDGE=<0|1>      [1]     emit RGBA frames on /tmp/spike3_frame.sock
#   AUDIO_BRIDGE=<0|1>      [1]     emit S16_LE PCM on /tmp/spike3_audio.sock
#   PATCH_SPI_VIDEO=<0|1>   [1]     vtable-patch SpiDecoder::Play (Step 62)
#   SW_HEVC=<0|1>           [1]     NOP-patch get_format + accept SW pix_fmt
#   PATCH_AUDIO_ABRT=<0|1>  [1]     patch AudioDriver vtable slot 7 to no-op
#                                   (Step 65) — fixes pure-virtual abort during
#                                   teardown race in audio mixer thread
#   STATE_BRIDGE=<0|1>      [0]     emit cmode JSON on /tmp/spike3_bridge.sock
#   LOGFILE=<path>          []      redirect game stdout+stderr to a file
#                                   (separate from --debug; only affects game)
#   QEMU=<bin>              [qemu-aarch64]     override qemu binary
#
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

# ---- Parse script-level flags (everything else falls through to qemu) ----
DEBUG=0
POSITIONAL=()
for a in "$@"; do
    case "$a" in
        --debug) DEBUG=1 ;;
        *)       POSITIONAL+=("$a") ;;
    esac
done
set -- "${POSITIONAL[@]+"${POSITIONAL[@]}"}"

# ---- Auto-detect paths ----
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# _emulation → pokemon_le → games → sysroot
SYSROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
GAME_DIR="$SYSROOT/games/pokemon_le"
LATEST_DIR="$SCRIPT_DIR/runs/latest"

# ---- Defaults (override via env) ----
TIMEOUT="${TIMEOUT:-0}"
REAL_GL="${REAL_GL:-1}"
FRAME_BRIDGE="${FRAME_BRIDGE:-1}"
AUDIO_BRIDGE="${AUDIO_BRIDGE:-1}"
PATCH_SPI_VIDEO="${PATCH_SPI_VIDEO:-1}"
SW_HEVC="${SW_HEVC:-1}"
PATCH_AUDIO_ABRT="${PATCH_AUDIO_ABRT:-1}"
STATE_BRIDGE="${STATE_BRIDGE:-0}"
OFFLINE_MODE="${OFFLINE_MODE:-1}"
VIEWER="${VIEWER:-1}"
VIEWER_FULLSCREEN="${VIEWER_FULLSCREEN:-0}"
AUDIO_PLAYER="${AUDIO_PLAYER:-1}"
BOOT_DISPLAY="${BOOT_DISPLAY:-1}"
BOOT_DISPLAY_DURATION="${BOOT_DISPLAY_DURATION:-3}"
STALE_KILL="${STALE_KILL:-1}"
LOGFILE="${LOGFILE:-}"
QEMU="${QEMU:-qemu-aarch64}"

# ---- Derived ----
SHIM="$SCRIPT_DIR/spi_shim.so"
OVERLAY="$SCRIPT_DIR/mesa_sw_overlay/pkg/usr/lib/aarch64-linux-gnu"
DRIVERS="$OVERLAY/dri"

declare -a INIT_SHARED_ENV=()
if [[ -f "$SYSROOT/etc/init.d/shared" ]]; then
    while IFS= read -r line; do
        [[ -z "$line" ]] && continue
        INIT_SHARED_ENV+=(-E "$line")
    done < <(
        source "$SYSROOT/etc/init.d/shared" 2>/dev/null
        # The shared script does:
        #   BOOT_DISPLAY=$GAMES_PATH/spike3/bin/boot_display
        #   if [ ! -x "$BOOT_DISPLAY" ]; then BOOT_DISPLAY=/usr/local/spike/...; fi
        # The [ -x ] check runs on the HOST filesystem (where /games/... doesn't
        # exist), so the fallback fires and we'd export the wrong path.
        # Override to the sysroot-absolute path real hardware would use.
        BOOT_DISPLAY="$GAMES_PATH/spike3/bin/boot_display"
        DPRINT="$GAMES_PATH/spike3/bin/dprint"
        for v in GAMES_PATH SPIKE_MENU_PATH OS_UPDATE_PATH \
                 DOWNLOADED_SPK_PATH MACHINE_REBOOTS_PATH \
                 CORE_FILE_PATH GAME_RESTARTS_PATH \
                 BOOT_DISPLAY DPRINT  GAME \
                 DOWNLOADED_SPK_AVAILABLE \
                 CONNECTIVITY_CACHE_CLEARED; do
            # Only emit if defined and non-empty
            if [[ -n "${!v:-}" ]]; then
                printf '%s=%s\n' "$v" "${!v}"
            fi
        done
    )
fi

# Debug-mode per-subsystem log paths. Computed here so variables exist even
# when --debug is off (so the rest of the script can reference them
# unconditionally — redirections only activate if $DEBUG==1).
GAME_DBG_LOG="$LATEST_DIR/game.log"
VIEWER_DBG_LOG="$LATEST_DIR/viewer.log"
AUDIO_DBG_LOG="$LATEST_DIR/audio.log"
BOOT_DISPLAY_DBG_LOG="$LATEST_DIR/boot_display.log"
BOOT_DISPLAY_BIN="$GAME_DIR/spike3/bin/boot_display"

# ---- Preflight ----
die() { echo "ERROR: $*" >&2; exit 1; }

[[ -f "$GAME_DIR/game" ]] || die "game binary not found at $GAME_DIR/game"
[[ -f "$SHIM" ]]          || die "shim not built. Run ./build.sh first."
command -v "$QEMU" >/dev/null || die "$QEMU not in PATH. Install qemu-user."
if [[ "$REAL_GL" == "1" && ! -d "$OVERLAY" ]]; then
    die "Mesa overlay not built at $OVERLAY. Run: (cd mesa_sw_overlay && ./build_overlay.sh)"
fi
if [[ "$BOOT_DISPLAY" == "1" && ! -x "$BOOT_DISPLAY_BIN" ]]; then
    printf '[run_game] BOOT_DISPLAY=1 but %s not found/executable; disabling\n' "$BOOT_DISPLAY_BIN" >&2
    BOOT_DISPLAY=0
fi
# boot_display writes /games/data/boot_display_cmd; ensure the directory exists
# in the sysroot so the write succeeds (then later the game reads from it).
if [[ "$BOOT_DISPLAY" == "1" ]]; then
    mkdir -p "$SYSROOT/games/data" 2>/dev/null || true
fi


# Debug-mode: ensure log dir exists.
if [[ "$DEBUG" == "1" ]]; then
    mkdir -p "$LATEST_DIR"
    : > "$GAME_DBG_LOG"
    : > "$VIEWER_DBG_LOG"
    : > "$AUDIO_DBG_LOG"
    printf '[run_game] --debug on: logs in %s\n' "$LATEST_DIR" >&2
fi

if [[ "$STALE_KILL" == "1" ]]; then
    stale_qemu=$(pgrep -f "qemu-aarch64.*LD_PRELOAD=$SHIM" 2>/dev/null || true)
    stale_help=$(pgrep -f "$SCRIPT_DIR/(frame_viewer|audio_subscriber)\.py" 2>/dev/null || true)
    stale_fplay=$(pgrep -f "ffplay.*s16le.*44100" 2>/dev/null || true)
    stale_all="$stale_qemu $stale_help $stale_fplay"
    if [[ -n "${stale_all// }" ]]; then
        printf '[run_game] reaping stale processes from prior runs: %s\n' "$stale_all" >&2
        for pid in $stale_all; do kill -TERM "$pid" 2>/dev/null || true; done
        sleep 0.5
        for pid in $stale_all; do kill -KILL "$pid" 2>/dev/null || true; done
    fi
    rm -f /tmp/spike3_frame.sock /tmp/spike3_audio.sock /tmp/spike3_bridge.sock
fi

QEMU_PID=""
declare -a HELPER_PIDS=()
declare -a HELPER_LABELS=()

kill_with_grace() {
    # $1 = pid, $2 = label, $3 = grace_ticks (each tick = 0.5s)
    local pid="$1" lbl="$2" ticks="$3"
    kill -0 "$pid" 2>/dev/null || return 0
    printf '[run_game] stopping %s (pid %d)\n' "$lbl" "$pid" >&2
    kill -TERM "$pid" 2>/dev/null || true
    pkill -TERM -P "$pid" 2>/dev/null || true
    local _i
    for ((_i=0; _i<ticks; _i++)); do
        kill -0 "$pid" 2>/dev/null || return 0
        sleep 0.5
    done
    if kill -0 "$pid" 2>/dev/null; then
        printf '[run_game] %s unresponsive to SIGTERM, sending SIGKILL\n' "$lbl" >&2
        pkill -KILL -P "$pid" 2>/dev/null || true
        kill -KILL "$pid" 2>/dev/null || true
    fi
}

cleanup_all() {
    # 1) the game's qemu
    [[ -n "$QEMU_PID" ]] && kill_with_grace "$QEMU_PID" "qemu" 3
    if (( ${#HELPER_PIDS[@]} > 0 )); then
        for i in "${!HELPER_PIDS[@]}"; do
            kill_with_grace "${HELPER_PIDS[$i]}" "${HELPER_LABELS[$i]}" 2
        done
    fi
    pkill -KILL -f "qemu-aarch64.*LD_PRELOAD=$SHIM" 2>/dev/null || true
    wait 2>/dev/null || true
}
trap cleanup_all EXIT INT TERM

# ---- Run boot_display briefly before everything else ----
# Mirrors etc/init.d/game_monitor: start the splash, wait, kill it. We give
# it $BOOT_DISPLAY_DURATION seconds (default 3 — qemu-user is slow on the
# render-init path, so 1 s isn't always enough). boot_display is a pure
# OpenGL splash program; it goes through the same shim hooks the game
# does, so its EGL/GBM/DRM init exercises every code path the game will
# also exercise.
#
# We do NOT pass SPIKE3_FRAME_BRIDGE / SPIKE3_AUDIO_BRIDGE to boot_display
# — those bridges belong to the game process; if boot_display also bound
# them it would race against the game.
if [[ "$BOOT_DISPLAY" == "1" ]]; then
    boot_display_log_target=/dev/null
    [[ "$DEBUG" == "1" ]] && boot_display_log_target="$BOOT_DISPLAY_DBG_LOG"
    # Mirror etc/init.d/S12lvds_panel "start" — that script just echoes this
    # message and execs $BOOT_DISPLAY in background.
    printf '[run_game] Starting LVDS panel boot_display\n' >&2
    printf '[run_game] (boot_display gets up to %s s before kill)\n' "$BOOT_DISPLAY_DURATION" >&2
    (
        cd "$GAME_DIR"
        "$QEMU" -L "$SYSROOT" \
            -E "LD_PRELOAD=$SHIM" \
            -E "LD_LIBRARY_PATH=$OVERLAY:$SYSROOT/usr/lib64:$SYSROOT/usr/lib:$SYSROOT/lib64:$SYSROOT/lib" \
            -E "SPIKE3_SYSROOT=$SYSROOT" \
            -E "SPIKE3_REAL_GL=$REAL_GL" \
            -E "LIBGL_DRIVERS_PATH=$DRIVERS" \
            -E "MESA_LOADER_DRIVER_OVERRIDE=swrast" \
            -E "LIBGL_ALWAYS_SOFTWARE=1" \
            "${INIT_SHARED_ENV[@]+"${INIT_SHARED_ENV[@]}"}" \
            "$BOOT_DISPLAY_BIN" \
                -background_stretch \
                -background_image spike3/bin/SternLogo_1920x1080.png
    ) >"$boot_display_log_target" 2>&1 &
    BOOT_DISPLAY_PID=$!
    # Wait up to BOOT_DISPLAY_DURATION seconds; if it exits early, proceed.
    # Slept-then-checked rather than a `wait -t` to keep bash-portable.
    end_ts=$(awk -v d="$BOOT_DISPLAY_DURATION" 'BEGIN{print systime()+d}')
    while [ "$(date +%s)" -lt "$end_ts" ] && kill -0 "$BOOT_DISPLAY_PID" 2>/dev/null; do
        sleep 0.2
    done
    if kill -0 "$BOOT_DISPLAY_PID" 2>/dev/null; then
        # Still running after timeout — kill it like init.d/game_monitor does.
        printf '[run_game] killing boot_display (pid %d) before game launch\n' "$BOOT_DISPLAY_PID" >&2
        pkill -P "$BOOT_DISPLAY_PID" 2>/dev/null || true
        kill -TERM "$BOOT_DISPLAY_PID" 2>/dev/null || true
        sleep 0.3
        kill -KILL "$BOOT_DISPLAY_PID" 2>/dev/null || true
        # Plus belt-and-suspenders: nuke any qemu-aarch64 still running boot_display
        pkill -KILL -f "qemu-aarch64.*$BOOT_DISPLAY_BIN" 2>/dev/null || true
    else
        printf '[run_game] boot_display exited on its own before timeout\n' >&2
    fi
    wait "$BOOT_DISPLAY_PID" 2>/dev/null || true
fi


# ---- Launch helpers BEFORE the game ----
# Both subscribers have internal retry loops so it doesn't matter that the
# shim hasn't opened its sockets yet.
if [[ "$VIEWER" == "1" && "$FRAME_BRIDGE" == "1" ]]; then
    if command -v python3 >/dev/null; then
        viewer_log_target=/dev/null
        [[ "$DEBUG" == "1" ]] && viewer_log_target="$VIEWER_DBG_LOG"
        viewer_args=()
        [[ "$VIEWER_FULLSCREEN" == "1" ]] && viewer_args+=("--fullscreen")
        python3 "$SCRIPT_DIR/frame_viewer.py" "${viewer_args[@]+"${viewer_args[@]}"}" \
            >"$viewer_log_target" 2>&1 &
        HELPER_PIDS+=("$!"); HELPER_LABELS+=("frame viewer")
        printf '[run_game] spawned frame viewer (pid %d)\n' "$!" >&2
    else
        printf '[run_game] VIEWER=1 but python3 not found; skipping viewer\n' >&2
    fi
fi

if [[ "$AUDIO_PLAYER" == "1" && "$AUDIO_BRIDGE" == "1" ]]; then
    if command -v python3 >/dev/null && command -v ffplay >/dev/null; then
        audio_log_target=/dev/null
        [[ "$DEBUG" == "1" ]] && audio_log_target="$AUDIO_DBG_LOG"
        # Subshell groups the python | ffplay pipe so cleanup_all's pkill -P
        # reaps both halves. Stderr of both goes to the same audio log.
        (
            python3 "$SCRIPT_DIR/audio_subscriber.py" 2>>"$audio_log_target" | \
            ffplay -f s16le -ar 44100 -ch_layout stereo -nodisp -autoexit \
                   -loglevel error -i - 2>>"$audio_log_target"
        ) &
        HELPER_PIDS+=("$!"); HELPER_LABELS+=("audio player (subscriber | ffplay)")
        printf '[run_game] spawned audio player (subshell pid %d)\n' "$!" >&2
    else
        printf '[run_game] AUDIO_PLAYER=1 but python3 or ffplay not found; skipping\n' >&2
    fi
fi


# ---- Compose game command ----
args=(
    "$QEMU" -L "$SYSROOT"
    -E "LD_PRELOAD=$SHIM"
    -E "LD_LIBRARY_PATH=$OVERLAY:$SYSROOT/usr/lib64:$SYSROOT/usr/lib:$SYSROOT/lib64:$SYSROOT/lib"
    -E "SPIKE3_SYSROOT=$SYSROOT"
)
# Inherit /etc/init.d/shared env (GAMES_PATH, BOOT_DISPLAY, GAME, etc.)
if (( ${#INIT_SHARED_ENV[@]} > 0 )); then
    args+=("${INIT_SHARED_ENV[@]}")
fi

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
[[ "$PATCH_SPI_VIDEO"  == "1" ]] && args+=(-E "SPIKE3_PATCH_SPI_VIDEO=1")
[[ "$SW_HEVC"          == "1" ]] && args+=(-E "SPIKE3_SW_HEVC=1")
[[ "$PATCH_AUDIO_ABRT" == "1" ]] && args+=(-E "SPIKE3_PATCH_AUDIO_ABRT=1")
[[ "$STATE_BRIDGE"    == "1" ]] && args+=(-E "SPIKE3_BRIDGE=1")
[[ "$OFFLINE_MODE"    == "1" ]] && args+=(-E "SPIKE3_OFFLINE=1")

# Append any caller-supplied flags (post --debug-strip) and the game binary last.
args+=("${POSITIONAL[@]+"${POSITIONAL[@]}"}" ./game)

# Wrap in `timeout` unless TIMEOUT=0
if [[ "$TIMEOUT" != "0" ]]; then
    args=(timeout "$TIMEOUT" "${args[@]}")
fi

# Resolve the destination for the game's stdout+stderr. Precedence:
#   LOGFILE env (explicit) > --debug ($GAME_DBG_LOG) > terminal inherit
game_redirect=""
if [[ -n "$LOGFILE" ]]; then
    game_redirect="$LOGFILE"
elif [[ "$DEBUG" == "1" ]]; then
    game_redirect="$GAME_DBG_LOG"
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
if [[ -n "$game_redirect" ]]; then
    "${args[@]}" >"$game_redirect" 2>&1 &
else
    "${args[@]}" &
fi
QEMU_PID=$!
# `wait` returns when qemu exits OR when a trapped signal arrives (SIGINT
# from the terminal). Either way, the trap runs cleanup_all before we leave.
wait "$QEMU_PID" 2>/dev/null
GAME_EXIT=$?

# Translate the exit code per etc/init.d/game_monitor's GAME_EXIT_REASON map.
# Hardware game_monitor decides whether to restart, reboot, or hang based on
# this; for us it's the most useful single line in the run output. Anything
# >= 128 is "uncaught signal SIGNUM = code - 128" (e.g. 134 → SIGABRT,
# 139 → SIGSEGV, 124 → timeout's SIGTERM via the `timeout` wrapper).
exit_reason=""
case "$GAME_EXIT" in
    0)   exit_reason="RESTART REQUESTED (Service Menu reset)" ;;
    1)   exit_reason="REBOOT REQUESTED (post-update)" ;;
    2)   exit_reason="EXIT REQUESTED (dev Ctrl+C / debugger)" ;;
    3)   exit_reason="POWER LOSS (power switch off)" ;;
    4)   exit_reason="FATAL ERROR (sys_debug_log_fatal_error)" ;;
    5)   exit_reason="DISPATCH TIMEOUT (proc system deadlock)" ;;
    6)   exit_reason="DEEP SLEEP" ;;
    124) exit_reason="timeout(1) killed it (SIGTERM via run_game.sh's TIMEOUT)" ;;
    129) exit_reason="uncaught SIGHUP" ;;
    130) exit_reason="uncaught SIGINT (Ctrl+C reached the game)" ;;
    134) exit_reason="uncaught SIGABRT (assertion / pure-virtual / abort())" ;;
    137) exit_reason="uncaught SIGKILL (force-killed)" ;;
    139) exit_reason="uncaught SIGSEGV (NULL deref / bad address)" ;;
    143) exit_reason="uncaught SIGTERM" ;;
    *)
        if [[ "$GAME_EXIT" -ge 128 ]]; then
            exit_reason="uncaught signal $((GAME_EXIT - 128))"
        else
            exit_reason="unknown"
        fi
        ;;
esac
printf '[run_game] game exited (status %d — %s)\n' "$GAME_EXIT" "$exit_reason" >&2
# cleanup_all runs via the EXIT trap; exit with the game's status so
# callers can still detect timeout (124) or crash.
exit "$GAME_EXIT"
