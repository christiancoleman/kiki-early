#!/usr/bin/env python3
"""
marker_button.py — single-button GUI to drop timestamped markers into the
running game.log so user observations interleave with shim/Godot output
under the same timeline.

Usage:
    python3 marker_button.py
    (or via run_game.sh's TIMER=ON path — needs DISPLAY/WAYLAND_DISPLAY)

Output:
    Each click writes ONE line in the EXACT same format as SHIM_LOG, directly
    to runs/latest/game.log via raw O_APPEND for atomic line append (safe
    even with the qemu/shim writing concurrently):
        2026/04/28 - 14:05:48.178 - [marker] click #N (label)

    On write failure (file missing, permission, etc.), the failure is recorded
    in $SYSROOT/tmp/marker_button_errors.log so we can debug after the fact.

Edit the label field before clicking to tag what you're marking. Spacebar
also fires a click.
"""

import datetime
import os
import pathlib
import subprocess
import sys
import tkinter as tk

# Derive paths from the script's location so the marker button works no matter
# where the repo is cloned. Layout: <SYSROOT>/games/pokemon_le/_emulation/marker_button.py
_HERE = pathlib.Path(__file__).resolve().parent  # _emulation/
_SYSROOT = _HERE.parent.parent.parent  # 3 up from _emulation = project sysroot

GAME_LOG_PATH = str(_HERE / "runs" / "latest" / "game.log")
# Stay inside the project sysroot — never write into WSL's /tmp
ERROR_LOG_PATH = str(_SYSROOT / "tmp" / "marker_button_errors.log")


def now_timestamp():
    n = datetime.datetime.now()
    return f"{n:%Y/%m/%d - %H:%M:%S}.{n.microsecond // 1000:03d}"


def append_to_game_log(line: str) -> tuple[bool, str]:
    """Raw O_APPEND write — atomic per line on Linux for sub-PIPE_BUF writes.
    Returns (success, detail)."""
    try:
        fd = os.open(GAME_LOG_PATH, os.O_WRONLY | os.O_APPEND)
    except OSError as e:
        return False, f"open: {e}"
    try:
        os.write(fd, line.encode("utf-8"))
        return True, "ok"
    except OSError as e:
        return False, f"write: {e}"
    finally:
        os.close(fd)


def log_error(detail: str):
    try:
        with open(ERROR_LOG_PATH, "a") as f:
            f.write(f"{now_timestamp()} - {detail}\n")
    except OSError:
        pass  # last-resort silent


def write_marker(label_var, status_var, count_ref):
    """Broad try/except + always-log diagnostic so ANY click activity is
    visible in the error log. Earlier narrow OSError-only catch + stderr-
    to-/dev/null was silently eating click failures."""
    try:
        count_ref[0] += 1
        label = label_var.get().strip() or "event"
        line = f"{now_timestamp()} - [marker] click #{count_ref[0]} ({label})\n"
    except Exception as e:
        log_error(f"CLICK #{count_ref[0]} SETUP RAISED: {type(e).__name__}: {e}")
        try: status_var.set(f"SETUP EXCEPTION: {type(e).__name__}: {e}")
        except Exception: pass
        return

    # Always log every click attempt so we know the handler fired.
    log_error(f"CLICK #{count_ref[0]} fired (label={label!r})")

    ok, detail = append_to_game_log(line)
    if ok:
        log_error(f"CLICK #{count_ref[0]} wrote OK to game.log")
        try:
            status_var.set(f"#{count_ref[0]} @ {line.split(' - ')[1]} → game.log")
        except Exception:
            pass  # status display failure should never lose the marker write
        return

    # Write failed — log every detail.
    log_error(f"CLICK #{count_ref[0]} APPEND FAILED: {detail} — line: {line.rstrip()}")
    log_error(f"  GAME_LOG_PATH={GAME_LOG_PATH}")
    log_error(f"  exists={os.path.exists(GAME_LOG_PATH)} writable={os.access(GAME_LOG_PATH, os.W_OK)}")
    print(f"[marker] click #{count_ref[0]} FAILED: {detail}", file=sys.stderr, flush=True)
    try:
        status_var.set(f"#{count_ref[0]} FAILED ({detail}). See {ERROR_LOG_PATH}.")
    except Exception:
        pass


_HELPER_PATTERNS = (
    r"qemu-aarch64.*LD_PRELOAD",
    r"python3.*frame_viewer\.py",
    r"python3.*audio_subscriber\.py",
    r"ffplay.*spike3_audio",
)


def kill_game(root, status_var):
    """Layered kill: SIGTERM run_game.sh first so bash's `cleanup_all` trap
    runs the well-tested graceful teardown (qemu → helpers → exit). After
    a 2-second grace we SIGKILL the parent + every helper pattern by name,
    in case the parent's `wait` got stuck (e.g. WSL2 SIGCHLD-mask deadlock).
    Marker GUI kills itself last so the status line is visible."""
    try:
        subprocess.run(
            ["pkill", "-TERM", "-f", r"run_game\.sh"],
            capture_output=True, text=True, timeout=2,
        )
        status_var.set("SIGTERM → run_game.sh (its trap kills qemu + helpers). "
                       "SIGKILL fallback in 2 s.")
        append_to_game_log(f"{now_timestamp()} - [marker] KILL: SIGTERM run_game.sh\n")
        root.after(2000, lambda: _kill_followup(root, status_var))
    except Exception as e:
        status_var.set(f"KILL TERM failed: {e}")


def _kill_followup(root, status_var):
    """SIGKILL anything still alive — parent script first, then every
    helper. Marker self-kill scheduled separately so the user sees the
    final status line before the window disappears."""
    try:
        # Force-kill the parent script first (in case its `wait` was stuck
        # — see WSL2 SIGCHLD-mask issue documented 2026-05-05).
        subprocess.run(["pkill", "-KILL", "-f", r"run_game\.sh"],
                       capture_output=True, timeout=1)
        # Then every helper that might be a stale WSLg window or detached
        # process. Each pattern's pkill is independent — a missing match
        # (rc=1) is fine.
        for pat in _HELPER_PATTERNS:
            subprocess.run(["pkill", "-KILL", "-f", pat],
                           capture_output=True, timeout=1)

        status_var.set("SIGKILL'd parent + helpers. Marker closing in 1 s.")
        append_to_game_log(f"{now_timestamp()} - [marker] KILL: SIGKILL fallback fired\n")
        # Self-kill last, after the user has had a chance to see the
        # status line. os._exit (vs sys.exit) avoids running atexit
        # handlers that might block on Tk.
        root.after(1000, lambda: os._exit(0))
    except Exception as e:
        status_var.set(f"KILL KILL failed: {e}")


def main():
    # Startup diagnostic — proves the script ran. If user reports "no markers"
    # and this line is absent, marker_button.py never spawned (env / TIMER=ON
    # / DISPLAY issue). If it's present and click logs are absent, the GUI
    # isn't receiving click events (focus / Tk issue).
    log_error(f"STARTED pid={os.getpid()} GAME_LOG_PATH={GAME_LOG_PATH} "
              f"exists={os.path.exists(GAME_LOG_PATH)} "
              f"writable={os.access(GAME_LOG_PATH, os.W_OK)}")

    root = tk.Tk()
    root.title("Spike 3 Marker")
    root.geometry("420x240")

    label_var = tk.StringVar(value="event")
    status_var = tk.StringVar(value="Edit label, then press MARK to log a timestamp.")
    count_ref = [0]

    tk.Label(root, text="Label:").pack(pady=(10, 0))
    tk.Entry(root, textvariable=label_var, font=("monospace", 12)).pack(
        padx=20, pady=(2, 8), fill=tk.X
    )
    tk.Button(
        root,
        text="MARK",
        command=lambda: write_marker(label_var, status_var, count_ref),
        bg="#ff8c00",
        fg="white",
        font=("monospace", 16, "bold"),
        height=2,
    ).pack(padx=20, pady=4, fill=tk.X)
    tk.Button(
        root,
        text="KILL GAME (SIGTERM → SIGKILL)",
        command=lambda: kill_game(root, status_var),
        bg="#b00020",
        fg="white",
        font=("monospace", 11, "bold"),
        height=1,
    ).pack(padx=20, pady=2, fill=tk.X)
    tk.Label(root, textvariable=status_var, wraplength=380, fg="#444").pack(
        pady=8, padx=10
    )

    # NOTE: no global keyboard shortcut. Earlier <space> binding fired even
    # when the user was typing labels containing spaces ("FROZEN SCREEN" etc.)
    # which generated phantom markers. Use the buttons.

    root.mainloop()


if __name__ == "__main__":
    sys.exit(main() or 0)
