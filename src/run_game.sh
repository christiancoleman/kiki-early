#!/usr/bin/env bash
# run_game.sh — launch Pokémon LE under qemu-aarch64 with the shim preloaded,
# plus the live frame viewer by default. Optional helpers are env-gated below.
#
# The repo layout is the Spike 3 SD-card filesystem used as a sysroot. This
# script auto-detects everything from its own location — no env setup needed
# in the normal case. Clone the repo anywhere, run ./run_game.sh.
#
# USAGE:
#   ./run_game.sh                 # play: game + viewer.
#                                   No timeout. Press Ctrl+C to stop.
#   ./run_game.sh --debug         # route every subsystem to its own log file
#                                   in a fresh runs/<TS>/ folder
#   ./run_game.sh --note "trying X"  # add a slug to the run folder name +
#                                     a note line to run_info.txt
#   TIMEOUT=185 ./run_game.sh     # cap at 185 s (useful for smoke runs)
#   VIEWER=0 ./run_game.sh        # game only, no Tk viewer window
#   AUDIO_PLAYER=0 ./run_game.sh  # game only, no ffplay audio
#   TIMER=ON ./run_game.sh        # spawn marker_button.py (Tk switch panel)
#   TIMER=EXTERNAL ./run_game.sh  # enable external switch client, no WSL Tk
#   LOGFILE=myrun.log ./run_game.sh  # capture only game stdout+stderr
#   REAL_GL=0 ./run_game.sh       # use stub EGL (no Mesa forwarding)
#   ./run_game.sh --help          # pass --help through to qemu-aarch64
#   ./run_game.sh -strace 2>trace.txt   # pass extra flags straight to qemu
#
# TUNABLE ENV OVERRIDES (defaults in []):
#   TIMEOUT=<sec>           [0]     cap run duration; 0 = no limit (default play)
#   VIEWER=<0|1>            [1]     auto-launch python3 frame_viewer.py
#   VIEWER_MAXIMIZED=<0|1>  [1]     open the viewer maximized; frames fit the
#                                   usable window area without clipping
#   VIEWER_FULLSCREEN=<0|1> [0]     open the viewer fullscreen rather than
#                                   maximized/in a 1920×1080 window
#   AUDIO_PLAYER=<0|1>      [0]     auto-launch audio_subscriber.py | ffplay
#   TIMER=<OFF|ON|EXTERNAL> [OFF]   ON auto-launches marker_button.py — a Tk
#                                   switch panel that writes switch commands to
#                                   runs/latest/press_request.txt. EXTERNAL
#                                   keeps the same shim command path active
#                                   without launching the WSL Tk panel.
#   SHIM_AUTO_PRESS=<0|1>   [0]     schedule one host-side switch press request
#                                   without the TIMER UI. Defaults to Service
#                                   Back at T+60s when enabled; override with
#                                   SHIM_AUTO_PRESS_DELAY_S / _SW_ID / _LABEL.
#   SHIM_EXPECTED_SWITCH_IDLE=<0|1> [0] apply the manual expected idle bits to
#                                   synthesized switch channel buffers.
#   SHIM_DISCOVERY_TRAPS=<0|1> [0] install the historical BRK trap suite for
#                                   Start/multiball/scene/nodebus tracing.
#   SPIKE3_COIN_DOOR_INTERLOCK=<closed|open|1|0>
#                             [closed] synthetic coin-door interlock state.
#                                   unset/closed/restored/1 = restored/closed;
#                                   open/0 = open/interlock active.
#   DEVBUS=<0|1>           [1]     enable switch-observability IPC bridge.
#   DEVBUS_TAIL=<auto|0|1> [auto]  auto=1 for --debug when DEVBUS=1, else 0.
#                                   When enabled, writes devbus.log in debug runs.
#   BOOT_DISPLAY=<0|1>      [0]     run spike3/bin/boot_display for ~1.5 s
#                                   pre-game, then kill it (mimics
#                                   etc/init.d/game_monitor). Lets boot_display
#                                   write /games/data/boot_display_cmd which the
#                                   game reads at startup.
#   BOOT_DISPLAY_DURATION=<s> [3]    how long to leave boot_display alive.
#   STALE_KILL=<0|1>        [1]     reap leftover qemu/helpers from prior runs
#                                   before starting.
#   REAL_GL=<0|1>           [1]     forward EGL/GLES to Mesa vs return stubs
#   FRAME_BRIDGE=<0|1>      [1]     emit RGBA frames on abstract AF_UNIX socket
#   AUDIO_BRIDGE=<0|1>      [1]     emit S16_LE PCM on abstract AF_UNIX socket
#   FORWARD_DECODE=<0|1>    [1]     launch decode_daemon.py and route
#                                   avcodec_send_packet/receive_frame to the
#                                   host-side NVDEC bridge. No in-VM SW HEVC
#                                   fallback — unreachable bridge returns
#                                   -EIO and the game asserts.
#   SPIKE3_DECODER_PATCH=<0|1> [1]  apply two HEVC-path compatibility patches:
#                                   (1) NOP b.ne at game VA 0x920cf0 so
#                                   get_format accepts NV12 instead of
#                                   AV_PIX_FMT_DRM_PRIME; (2) name-swap
#                                   hevc_v4l2request/hevc_v4l2m2m → hevc.
#                                   Set =0 only to debug.
#   STATE_BRIDGE=<0|1>      [0]     emit cmode JSON on abstract AF_UNIX socket
#   LOGFILE=<path>          []      redirect game stdout+stderr to a file
#                                   (separate from --debug; only affects game)
#   SWITCH_NOISE_TEST=<auto|0|1> [0]
#                                   after the run, analyze devbus.log for
#                                   same-timestamp watched switch clusters.
#                                   auto enables only when DEVBUS_TAIL=1.
#   SWITCH_NOISE_WATCH=<ids> [77-98]
#                                   comma/range list passed to
#                                   switch_noise_test.py --watch.
#   SWITCH_NOISE_TEST_STRICT=<0|1> [0]
#                                   if 1, a failed switch-noise test becomes
#                                   the script exit status when the game itself
#                                   exited cleanly.
#   AUTO_BIND_MOUNTS=<0|1>  [1]     if /games, /connectivity, /data are
#                                   missing or mounted to a different sysroot,
#                                   run setup_bind_mounts.sh --switch "$SYSROOT"
#                                   via sudo before launching.
#   SKIP_BIND_MOUNT_CHECK=<0|1> [0]  skip bind-mount validation entirely.
#                                   Debug-only: absolute /games paths may resolve
#                                   to the wrong tree if mounts are stale.
#   QEMU=<bin>              [qemu-aarch64]     override qemu binary
#
# RUN ARCHIVING:
#   Every run gets its own timestamped folder at runs/<TS>[_<note_slug>]/
#   containing all log files for that run. runs/latest/ is a symlink to the
#   most recent one — backward-compatible with everything that referenced
#   runs/latest/<file>. A run_info.txt at the top of each folder records
#   timestamp, note, the env-toggle table, and the qemu invocation.
#   Pass --note "your description" (or -note) to add a slug to the folder
#   name and a note line to run_info.txt.
#
#
# PREREQS (see README.md for details):
#   - qemu-user (provides qemu-aarch64 + binfmt_misc)
#   - gcc-aarch64-linux-gnu (only if you modify the shim — run ./build.sh)
#   - Mesa overlay built: cd src/mesa_sw_overlay && ./build_overlay.sh
#   - Optional: python3 + numpy + Pillow + tk   (live viewer)
#   - Optional: ffplay                           (live audio)

# -e intentionally omitted: background helpers exit non-zero on kill, which
# is normal and shouldn't tear down the script.
set -uo pipefail

# ---- Parse script-level flags (everything else falls through to qemu) ----
DEBUG=0
POSITIONAL=()
NOTE=""
expecting_note=0
for a in "$@"; do
    if [[ "$expecting_note" == "1" ]]; then
        NOTE="$a"; expecting_note=0; continue
    fi
    case "$a" in
        --debug) DEBUG=1 ;;
        --note|-note) expecting_note=1 ;;
        *)       POSITIONAL+=("$a") ;;
    esac
done
set -- "${POSITIONAL[@]+"${POSITIONAL[@]}"}"

# Sanitize note → directory-safe slug (alnum + underscore, lowercase, ≤40 chars).
NOTE_SLUG=""
if [[ -n "$NOTE" ]]; then
    NOTE_SLUG=$(printf '%s' "$NOTE" | tr -c 'A-Za-z0-9' '_' | tr 'A-Z' 'a-z' | sed -E 's/_+/_/g; s/^_+//; s/_+$//')
    NOTE_SLUG="${NOTE_SLUG:0:40}"
fi

# ---- Auto-detect paths ----
# This script lives in _emulation/src/.  SRC_DIR holds the shim source + the
# helper daemons/tools; EMU_ROOT is the _emulation/ project dir (runs/, docs/,
# captures/ live there).  EMU_ROOT auto-detects as the parent of src/; a
# collaborator with a non-standard checkout can pin it via SPIKE3_EMULATION_DIR.
# Then EMU_ROOT → <game_name> → games → sysroot — auto-detected (not hardcoded
# to "pokemon_le") so the same run_game.sh drives any Spike 3 game; each game
# keeps its own _emulation/ dir (symlinked from the canonical PLE one).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$SCRIPT_DIR"
EMU_ROOT="${SPIKE3_EMULATION_DIR:-$(cd "$SCRIPT_DIR/.." && pwd)}"
GAME_DIR="$(cd "$EMU_ROOT/.." && pwd)"
SYSROOT="$(cd "$GAME_DIR/../.." && pwd)"
AUTO_BIND_MOUNTS="${AUTO_BIND_MOUNTS:-1}"
SKIP_BIND_MOUNT_CHECK="${SKIP_BIND_MOUNT_CHECK:-0}"

if [[ "$SKIP_BIND_MOUNT_CHECK" == "1" ]]; then
    printf '[run_game] SKIP_BIND_MOUNT_CHECK=1; not validating /games, /connectivity, /data\n' >&2
elif ! "$SCRIPT_DIR/scripts/setup_bind_mounts.sh" --check "$SYSROOT" >/dev/null 2>&1; then
    if [[ "$AUTO_BIND_MOUNTS" == "1" ]]; then
        printf '[run_game] bind mounts do not match SYSROOT=%s; switching via sudo\n' "$SYSROOT" >&2
        if sudo "$SCRIPT_DIR/scripts/setup_bind_mounts.sh" --switch "$SYSROOT"; then
            printf '[run_game] bind mounts switched to %s\n' "$SYSROOT" >&2
        else
            cat <<EOF >&2
[run_game] ERROR: failed to switch bind mounts for this sysroot.

  Manual repair:
    sudo $SCRIPT_DIR/scripts/setup_bind_mounts.sh --switch "$SYSROOT"

  Disable automatic switching:
    AUTO_BIND_MOUNTS=0 ./run_game.sh
EOF
            exit 2
        fi
    fi
fi

if [[ "$SKIP_BIND_MOUNT_CHECK" != "1" ]] &&
   ! "$SCRIPT_DIR/scripts/setup_bind_mounts.sh" --check "$SYSROOT" >/dev/null 2>&1; then
    cat <<EOF >&2
[run_game] ERROR: bind mounts are not active for this sysroot.

  Restore (one-time per WSL session, requires sudo):
    sudo $SCRIPT_DIR/scripts/setup_bind_mounts.sh --switch "$SYSROOT"

  Persist across WSL reboots — append to /etc/fstab:
    $SYSROOT/games        /games        none bind 0 0
    $SYSROOT/connectivity /connectivity none bind 0 0
    $SYSROOT/data         /data         none bind 0 0
EOF
    exit 2
fi
RUN_TS="$(date +%Y-%m-%dT%H.%M.%S)"
RUN_DIRNAME="$RUN_TS${NOTE_SLUG:+_$NOTE_SLUG}"
RUN_DIR="$EMU_ROOT/runs/$RUN_DIRNAME"
LATEST_DIR="$RUN_DIR"
mkdir -p "$RUN_DIR"
ln -sfn "$RUN_DIRNAME" "$EMU_ROOT/runs/latest"
printf '%s\n' "$RUN_DIRNAME" > "$EMU_ROOT/runs/latest_path.txt"

export SPIKE3_SYSROOT="$SYSROOT"
export SPIKE3_GAME_DIR="$GAME_DIR"
export SPIKE3_EMULATION_DIR="$EMU_ROOT"
export SPIKE3_RUN_DIR="$RUN_DIR"
export SPIKE3_GAME_LOG_PATH="$LATEST_DIR/game.log"
export SHIM_PRESS_REQUEST_PATH="$RUN_DIR/press_request.txt"

# ---- Defaults (override via env) ----
TIMEOUT="${TIMEOUT:-0}"
REAL_GL="${REAL_GL:-1}"
FRAME_BRIDGE="${FRAME_BRIDGE:-1}"
AUDIO_BRIDGE="${AUDIO_BRIDGE:-1}"
# FORWARD_DECODE=1: launch decode_daemon.py and route the game's
# avcodec_send_packet/receive_frame to host NVDEC via AF_UNIX abstract socket
# @spike3_decode. When the daemon can't be reached or PyAV is missing, the
# shim falls back gracefully to local libavcodec passthrough — no errors
# propagated to the game.
FORWARD_DECODE="${FORWARD_DECODE:-1}"
# GL_DAEMON=1: launch gl_daemon.py and route shim GL/EGL calls to host-native
# Mesa via abstract AF_UNIX @spike3_gl. Default on; when enabled, default the
# daemon's zero/zero blend handling to the verified alpha-preserving mode unless
# the caller explicitly supplied a zero-blend env override.
GL_DAEMON="${GL_DAEMON:-1}"
if [[ "$GL_DAEMON" == "1" \
      && -z "${SPIKE3_ZERO_BLEND_DISABLE+x}" \
      && -z "${SPIKE3_ZERO_BLEND_IGNORE+x}" \
      && -z "${SPIKE3_ZERO_BLEND_MODE+x}" ]]; then
    export SPIKE3_ZERO_BLEND_DISABLE=1
fi
export SPIKE3_ZERO_BLEND_DISABLE="${SPIKE3_ZERO_BLEND_DISABLE:-}"
export SPIKE3_ZERO_BLEND_IGNORE="${SPIKE3_ZERO_BLEND_IGNORE:-}"
export SPIKE3_ZERO_BLEND_MODE="${SPIKE3_ZERO_BLEND_MODE:-}"
# SPI_QUIET=1: stop replying byte-2=0x04 ("ack/data-available") to every SPI
# transfer; instead reply all-zeros. Per-cmd overrides via SPIKE3_SPI_RX_<HEX>=
# still take precedence.
SPI_QUIET="${SPI_QUIET:-0}"
# SPI_DELAY_US: sleep this many microseconds inside each SPI ioctl handler.
# Real hardware's SPI poll loop runs at ~10kHz; qemu+shim handle each call in
# microseconds, so the game polls millions/sec. Setting 100 caps at ~10kHz
# (closer to realistic) and makes the log readable. Clamped 0..100000.
SPI_DELAY_US="${SPI_DELAY_US:-0}"
STATE_BRIDGE="${STATE_BRIDGE:-0}"
OFFLINE_MODE="${OFFLINE_MODE:-1}"
VIEWER="${VIEWER:-1}"
VIEWER_MAXIMIZED="${VIEWER_MAXIMIZED:-1}"
VIEWER_FULLSCREEN="${VIEWER_FULLSCREEN:-0}"
AUDIO_PLAYER="${AUDIO_PLAYER:-0}"
TIMER="${TIMER:-OFF}"
case "$TIMER" in
    ON)
        TIMER_ACTIVE=1
        TIMER_GUI=1
        ;;
    EXTERNAL|REMOTE)
        TIMER=EXTERNAL
        TIMER_ACTIVE=1
        TIMER_GUI=0
        ;;
    OFF|0)
        TIMER=OFF
        TIMER_ACTIVE=0
        TIMER_GUI=0
        ;;
    *)
        printf '[run_game] unknown TIMER=%s; expected OFF, ON, or EXTERNAL\n' "$TIMER" >&2
        exit 2
        ;;
esac
DEVBUS="${DEVBUS:-1}"
DEVBUS_TAIL="${DEVBUS_TAIL:-auto}"
if [[ "$DEVBUS_TAIL" == "auto" ]]; then
    if [[ "$DEBUG" == "1" && "$DEVBUS" == "1" ]]; then
        DEVBUS_TAIL=1
    else
        DEVBUS_TAIL=0
    fi
fi
SWITCH_NOISE_TEST="${SWITCH_NOISE_TEST:-0}"
SWITCH_NOISE_WATCH="${SWITCH_NOISE_WATCH:-77-98}"
SWITCH_NOISE_TEST_STRICT="${SWITCH_NOISE_TEST_STRICT:-0}"
if [[ "$SWITCH_NOISE_TEST" == "auto" ]]; then
    if [[ "$DEVBUS_TAIL" == "1" && "$DEVBUS" == "1" ]]; then
        SWITCH_NOISE_TEST=1
    else
        SWITCH_NOISE_TEST=0
    fi
fi
BOOT_DISPLAY="${BOOT_DISPLAY:-0}"
SPIKE3_COIN_DOOR_INTERLOCK="${SPIKE3_COIN_DOOR_INTERLOCK:-closed}"
BOOT_DISPLAY_DURATION="${BOOT_DISPLAY_DURATION:-3}"
STALE_KILL="${STALE_KILL:-1}"
LOGFILE="${LOGFILE:-}"
QEMU="${QEMU:-qemu-aarch64}"

# Manual press control uses runs/latest/press_request.txt. TIMER=ON spawns the
# marker GUI; TIMER=EXTERNAL leaves GUI ownership to an external process such
# as switch_command.py or a future Windows/COM client. The shim watcher is
# enabled only when an input producer is expected, unless SHIM_MANUAL_PRESS is
# explicitly set by the caller.
#
export SHIM_AUTO_PRESS="${SHIM_AUTO_PRESS:-0}"
export SHIM_AUTO_PRESS_DELAY_S="${SHIM_AUTO_PRESS_DELAY_S:-60}"
export SHIM_AUTO_PRESS_SW_ID="${SHIM_AUTO_PRESS_SW_ID:-93}"
export SHIM_AUTO_PRESS_LABEL="${SHIM_AUTO_PRESS_LABEL:-Service Back}"
if [[ -z "${SHIM_MANUAL_PRESS+x}" ]]; then
    if [[ "$TIMER_ACTIVE" == "1" || "$SHIM_AUTO_PRESS" == "1" ]]; then
        export SHIM_MANUAL_PRESS=1
    else
        export SHIM_MANUAL_PRESS=0
    fi
else
    export SHIM_MANUAL_PRESS
fi
export SHIM_MANUAL_PRESS_DELAY_S="${SHIM_MANUAL_PRESS_DELAY_S:-0}"
export SHIM_MANUAL_PRESS_MS="${SHIM_MANUAL_PRESS_MS:-120}"
export SHIM_DISCOVERY_TRAPS="${SHIM_DISCOVERY_TRAPS:-0}"
export SHIM_SWITCH_TABLE_PROBE="${SHIM_SWITCH_TABLE_PROBE:-0}"
export SHIM_SWITCH_TABLE_PROBE_DELAY_S="${SHIM_SWITCH_TABLE_PROBE_DELAY_S:-65}"

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
    die "Mesa overlay not built at $OVERLAY. Run: (cd \"$SRC_DIR/mesa_sw_overlay\" && ./build_overlay.sh)"
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
    stale_help=$(pgrep -f "$SCRIPT_DIR/(display/frame_viewer|audio_subscriber|tools/marker_button|nodebus/devbus_subscriber)\.py" 2>/dev/null || true)
    stale_fplay=$(pgrep -f "ffplay.*s16le.*44100" 2>/dev/null || true)
    stale_all="$stale_qemu $stale_help $stale_fplay"
    if [[ -n "${stale_all// }" ]]; then
        printf '[run_game] reaping stale processes from prior runs: %s\n' "$stale_all" >&2
        for pid in $stale_all; do kill -TERM "$pid" 2>/dev/null || true; done
        sleep 0.5
        for pid in $stale_all; do kill -KILL "$pid" 2>/dev/null || true; done
    fi
    # Sockets are AF_UNIX abstract namespace now (no filesystem files exist).
    # Reap any legacy filesystem socket files in case an older shim is still
    # in use somewhere — harmless no-op when our current shim is loaded.
    rm -f "$SYSROOT/tmp/spike3_frame.sock" "$SYSROOT/tmp/spike3_audio.sock" "$SYSROOT/tmp/spike3_bridge.sock" 2>/dev/null || true
    rm -f /tmp/spike3_frame.sock /tmp/spike3_audio.sock /tmp/spike3_bridge.sock 2>/dev/null || true
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
            -E "SPIKE3_GAME_DIR=$GAME_DIR" \
            -E "SPIKE3_EMULATION_DIR=$EMU_ROOT" \
            -E "SPIKE3_RUN_DIR=$RUN_DIR" \
            -E "SPIKE3_GAME_LOG_PATH=$GAME_DBG_LOG" \
            -E "SHIM_PRESS_REQUEST_PATH=$SHIM_PRESS_REQUEST_PATH" \
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
        # Helpers self-resolve their socket path from __file__ (== $SYSROOT/tmp/spike3_*.sock).
        # No --socket arg needed.
        viewer_args=()
        if [[ "$VIEWER_FULLSCREEN" == "1" ]]; then
            viewer_args+=("--fullscreen")
        elif [[ "$VIEWER_MAXIMIZED" == "1" ]]; then
            viewer_args+=("--maximized")
        fi
        python3 "$SCRIPT_DIR/display/frame_viewer.py" "${viewer_args[@]+"${viewer_args[@]}"}" \
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
        # audio_subscriber self-resolves its socket via __file__ (no --socket).
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

if [[ "$FORWARD_DECODE" == "1" ]]; then
    if command -v python3 >/dev/null; then
        decode_log_target="$LATEST_DIR/decode_daemon.log"
        # Daemon binds an abstract AF_UNIX socket (@spike3_decode); no file
        # cleanup needed. Stays alive until cleanup_all kills it.
        python3 -u "$SCRIPT_DIR/display/decode_daemon.py" \
            >"$decode_log_target" 2>&1 &
        HELPER_PIDS+=("$!"); HELPER_LABELS+=("decode daemon")
        printf '[run_game] spawned decode daemon (pid %d) — log %s\n' \
            "$!" "$decode_log_target" >&2
        # Give it a beat to bind before the game's first send_packet.
        sleep 0.3
    else
        printf '[run_game] FORWARD_DECODE=1 but python3 not found; skipping\n' >&2
    fi
fi

if [[ "$GL_DAEMON" == "1" ]]; then
    if command -v python3 >/dev/null; then
        gl_daemon_log_target="$LATEST_DIR/gl_daemon.log"
        # Daemon binds abstract AF_UNIX @spike3_gl. Same lifecycle as decode_daemon.
        python3 -u "$SCRIPT_DIR/display/gl_daemon.py" \
            >"$gl_daemon_log_target" 2>&1 &
        HELPER_PIDS+=("$!"); HELPER_LABELS+=("gl daemon")
        #printf '[run_game] spawned gl daemon (pid %d) — log %s\n' \
            #"$!" "$gl_daemon_log_target" >&2
        #sleep 0.3
    else
        printf '[run_game] GL_DAEMON=1 but python3 not found; skipping\n' >&2
    fi
fi

if [[ "$TIMER_GUI" == "1" ]]; then
    if command -v python3 >/dev/null; then
        # Marker writes go directly to runs/latest/game.log (raw O_APPEND in
        # marker_button.py) — no separate timer.log needed. BUT: route
        # stderr to a sticky log file so any Python exception from the GUI
        # callback path is captured (was silently lost when /dev/null'd).
        python3 -u "$SCRIPT_DIR/tools/marker_button.py" \
            >>"$SYSROOT/tmp/marker_button_errors.log" 2>&1 &
        HELPER_PIDS+=("$!"); HELPER_LABELS+=("marker button")
        printf '[run_game] spawned marker button (pid %d) — TIMER=ON\n' "$!" >&2
    else
        printf '[run_game] TIMER=ON but python3 not found; skipping marker button\n' >&2
    fi
elif [[ "$TIMER_ACTIVE" == "1" ]]; then
    printf '[run_game] TIMER=EXTERNAL — switch commands accepted at %s\n' "$SHIM_PRESS_REQUEST_PATH" >&2
fi

if [[ "$DEVBUS_TAIL" == "1" ]]; then
    if [[ "$DEVBUS" != "1" ]]; then
        printf '[run_game] DEVBUS_TAIL=1 ignored — DEVBUS=0\n' >&2
    elif command -v python3 >/dev/null; then
        devbus_log_target=/dev/null
        [[ "$DEBUG" == "1" ]] && devbus_log_target="$LATEST_DIR/devbus.log"
        python3 -u "$SCRIPT_DIR/nodebus/devbus_subscriber.py" --retry \
            >"$devbus_log_target" 2>&1 &
        HELPER_PIDS+=("$!"); HELPER_LABELS+=("devbus tail")
        printf '[run_game] spawned devbus tail (pid %d) — log %s\n' \
            "$!" "$devbus_log_target" >&2
    else
        printf '[run_game] DEVBUS_TAIL=1 but python3 not found; skipping\n' >&2
    fi
fi

if [[ "$SHIM_AUTO_PRESS" == "1" ]]; then
    (
        sleep "$SHIM_AUTO_PRESS_DELAY_S"
        request_path="$EMU_ROOT/runs/latest/press_request.txt"
        printf '%s\n' "$SHIM_AUTO_PRESS_SW_ID" > "$request_path"
        if [[ -f "$GAME_DBG_LOG" ]]; then
            printf '%s - [auto_press] T+%ss PRESS %s sw_id=%s -> %s\n' \
                "$(date '+%Y/%m/%d - %H:%M:%S.%3N')" \
                "$SHIM_AUTO_PRESS_DELAY_S" \
                "$SHIM_AUTO_PRESS_LABEL" \
                "$SHIM_AUTO_PRESS_SW_ID" \
                "$request_path" >> "$GAME_DBG_LOG"
        fi
    ) &
    HELPER_PIDS+=("$!"); HELPER_LABELS+=("auto press")
    printf '[run_game] scheduled auto press: %s sw_id=%s at T+%ss\n' \
        "$SHIM_AUTO_PRESS_LABEL" "$SHIM_AUTO_PRESS_SW_ID" "$SHIM_AUTO_PRESS_DELAY_S" >&2
fi


# ---- Compose game command ----
args=(
    "$QEMU" -L "$SYSROOT"
    -E "LD_PRELOAD=$SHIM"
    -E "LD_LIBRARY_PATH=$OVERLAY:$SYSROOT/usr/lib64:$SYSROOT/usr/lib:$SYSROOT/lib64:$SYSROOT/lib"
    -E "SPIKE3_SYSROOT=$SYSROOT"
    -E "SPIKE3_GAME_DIR=$GAME_DIR"
    -E "SPIKE3_EMULATION_DIR=$EMU_ROOT"
    -E "SPIKE3_RUN_DIR=$RUN_DIR"
    -E "SPIKE3_GAME_LOG_PATH=$GAME_DBG_LOG"
    -E "SHIM_PRESS_REQUEST_PATH=$SHIM_PRESS_REQUEST_PATH"
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
        # Keep Mesa's per-shader cache inside the project sysroot rather than
        # WSL's /tmp/cache/mesa/. Mesa creates the dir if missing. Reinforced
        # by the shim_fs.c remap_path /tmp/cache carve-out for any libc-level
        # writes that still go through that path.
        -E "MESA_SHADER_CACHE_DIR=$SYSROOT/tmp/cache/mesa"
    )
fi
[[ "$FRAME_BRIDGE"    == "1" ]] && args+=(-E "SPIKE3_FRAME_BRIDGE=1")
[[ "$AUDIO_BRIDGE"    == "1" ]] && args+=(-E "SPIKE3_AUDIO_BRIDGE=1")
[[ "$FORWARD_DECODE"   == "1" ]] && args+=(-E "SPIKE3_FORWARD_DECODE=1")
[[ "$GL_DAEMON"        == "1" ]] && args+=(-E "SPIKE3_GL_DAEMON=1")
[[ "$DEVBUS"           == "1" ]] && args+=(-E "SPIKE3_DEVBUS=1")
[[ "$DEVBUS"           == "0" ]] && args+=(-E "SPIKE3_DEVBUS=0")
[[ -n "${SPIKE3_COIN_DOOR_INTERLOCK:-}" ]] && args+=(-E "SPIKE3_COIN_DOOR_INTERLOCK=$SPIKE3_COIN_DOOR_INTERLOCK")
[[ "$SPI_QUIET"        == "1" ]] && args+=(-E "SPIKE3_SPI_QUIET=1")
[[ "$SPI_DELAY_US"     != "0" ]] && args+=(-E "SPIKE3_SPI_DELAY_US=$SPI_DELAY_US")
[[ "$STATE_BRIDGE"    == "1" ]] && args+=(-E "SPIKE3_BRIDGE=1")
[[ "$OFFLINE_MODE"    == "1" ]] && args+=(-E "SPIKE3_OFFLINE=1")

# Forward every exported SHIM_* env var to qemu (the shim's getenv() runs
# inside the qemu-aarch64 process and only sees vars passed via -E).
while IFS='=' read -r _name _val; do
    [[ -n "$_name" ]] && args+=(-E "$_name=$_val")
done < <(env | grep -E '^SHIM_' || true)


# If GDB_PORT is set, gate qemu on a gdbstub listener — game blocks until gdb
# attaches at localhost:$GDB_PORT. Use to drop breakpoints into the dispatcher
# and capture a stack trace at proc-system stall (status 5).
if [[ -n "${GDB_PORT:-}" ]]; then
    args+=(-g "$GDB_PORT")
    printf '[run_game] qemu gdbstub waiting on tcp:%s — connect with: gdb-multiarch -ex "target remote :%s"\n' \
        "$GDB_PORT" "$GDB_PORT" >&2
fi

# Run the game via its canonical /games/game path (a relative symlink to
# pokemon_le/game). Matches /etc/init.d/shared's GAME=/games/game env that
# real cabinets see — the binary's argv[0], /proc/self/exe, and any
# path-aware game-side checks all see the canonical path. cwd stays at
# $GAME_DIR (= /games/pokemon_le/) so the binary's relative ./assets/
# lookups still resolve through the per-game subdir.
args+=("${POSITIONAL[@]+"${POSITIONAL[@]}"}" /games/game)

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

# Write run metadata to the top of the run dir (before launch — captured even
# if launch fails). Includes timestamp, note, the env-toggle table, the full
# qemu command line, and the originating shell invocation.
{
    printf '# run metadata — Pokémon LE / Star Wars LE emulation\n'
    printf 'timestamp:   %s\n' "$RUN_TS"
    printf 'run_dir:     %s\n' "$RUN_DIR"
    printf 'sysroot:     %s\n' "$SYSROOT"
    printf 'game_dir:    %s\n' "$GAME_DIR"
    [[ -n "$NOTE" ]] && printf 'note:        %s\n' "$NOTE"
    printf '\n# invocation (raw shell)\n'
    printf '$ %s %s\n' "$0" "$*"
    printf '\n# env-toggle settings (final values)\n'
    for v in TIMEOUT REAL_GL FRAME_BRIDGE AUDIO_BRIDGE FORWARD_DECODE GL_DAEMON \
             SPIKE3_ZERO_BLEND_DISABLE SPIKE3_ZERO_BLEND_IGNORE SPIKE3_ZERO_BLEND_MODE \
             SPIKE3_COIN_DOOR_INTERLOCK \
             DEVBUS DEVBUS_TAIL \
             SPI_QUIET SPI_DELAY_US STATE_BRIDGE OFFLINE_MODE \
             SWITCH_NOISE_TEST SWITCH_NOISE_WATCH SWITCH_NOISE_TEST_STRICT \
             VIEWER VIEWER_MAXIMIZED VIEWER_FULLSCREEN AUDIO_PLAYER TIMER  BOOT_DISPLAY \
             BOOT_DISPLAY_DURATION STALE_KILL SHIM_MANUAL_PRESS SHIM_MANUAL_PRESS_MS \
             SHIM_EXPECTED_SWITCH_IDLE SHIM_DISCOVERY_TRAPS \
             SHIM_SWITCH_TABLE_PROBE SHIM_AUTO_PRESS \
             SHIM_AUTO_PRESS_DELAY_S SHIM_AUTO_PRESS_SW_ID \
             SHIM_AUTO_PRESS_LABEL DEBUG LOGFILE; do
        printf '  %-22s = %s\n' "$v" "${!v:-}"
    done
    printf '\n# qemu invocation\n'
    printf '%s\n' "${args[*]}"
} > "$RUN_DIR/run_info.txt"

# cwd = /games/ (same dir as the /games/game symlink) so the binary's
# relative path lookups resolve from the canonical real-cabinet location.
# If asset opens fail (binary uses relative ./assets/) we'll see
# ENOENT in the log and revert to GAME_DIR cwd.
cd /games
# Background qemu + wait so the trap can kill it directly on Ctrl+C (bash
# doesn't forward signals to foreground children reliably, and the shim's
# SIGINT handler doesn't always terminate — see cleanup_all's SIGKILL path).
#
# IMPORTANT: use `>>` (O_APPEND) for the game.log redirect, with explicit
# pre-truncation. Reason: TIMER=ON marker_button.py and external clients can
# also append to game.log via O_APPEND. With `>` (no O_APPEND), qemu's fd has a
# fixed position counter; helper appends advance EOF past qemu's position, then
# qemu's next write OVERWRITES the marker line. With both writers in
# O_APPEND mode, every write goes atomically to current EOF — no
# trampling. Pre-truncate so each run starts fresh.
if [[ -n "$game_redirect" ]]; then
    : > "$game_redirect"
    "${args[@]}" >>"$game_redirect" 2>&1 &
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

# Stop helper processes now so post-run analyzers read stable log files.
cleanup_all
trap - EXIT INT TERM

SWITCH_NOISE_EXIT=0
if [[ "$SWITCH_NOISE_TEST" == "1" ]]; then
    noise_log="$RUN_DIR/switch_noise_test.log"
    if [[ "$DEVBUS" != "1" || "$DEVBUS_TAIL" != "1" ]]; then
        printf '[run_game] switch-noise test skipped — requires DEVBUS=1 and DEVBUS_TAIL=1\n' >&2
        printf '[switch_noise_test] skipped: requires DEVBUS=1 and DEVBUS_TAIL=1\n' >"$noise_log"
    elif ! command -v python3 >/dev/null; then
        printf '[run_game] switch-noise test skipped — python3 not found\n' >&2
        printf '[switch_noise_test] skipped: python3 not found\n' >"$noise_log"
    else
        printf '[run_game] running switch-noise test — log %s\n' "$noise_log" >&2
        if python3 "$SCRIPT_DIR/tools/switch_noise_test.py" \
                --run-dir "$RUN_DIR" \
                --watch "$SWITCH_NOISE_WATCH" \
                >"$noise_log" 2>&1; then
            printf '[run_game] switch-noise test PASS\n' >&2
        else
            SWITCH_NOISE_EXIT=$?
            printf '[run_game] switch-noise test FAIL — see %s\n' "$noise_log" >&2
        fi
    fi
fi

# Preserve the game's status by default so callers can still detect timeout
# (124) or crash. Strict mode promotes diagnostic failures on clean game exits.
if [[ "$SWITCH_NOISE_TEST_STRICT" == "1" && "$GAME_EXIT" == "0" && "$SWITCH_NOISE_EXIT" != "0" ]]; then
    exit "$SWITCH_NOISE_EXIT"
fi
exit "$GAME_EXIT"
