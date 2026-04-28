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


def kill_game(root, status_var):
    """Two-stage kill of qemu: SIGTERM, then SIGKILL after 1.5s if still alive.
    Uses pkill -f to match the qemu-aarch64 invocation that the shim is
    LD_PRELOAD'd into. Helpers (frame viewer, audio) tear down via the
    run_game.sh EXIT trap when qemu dies. Marker GUI stays up so the user
    can see the status update — close it manually when done."""
    try:
        r = subprocess.run(
            ["pkill", "-TERM", "-f", r"qemu-aarch64.*LD_PRELOAD"],
            capture_output=True, text=True, timeout=2,
        )
        if r.returncode == 0:
            status_var.set("SIGTERM sent to qemu. SIGKILL in 1.5s if still alive...")
            line = f"{now_timestamp()} - [marker] KILL: SIGTERM to qemu\n"
            append_to_game_log(line)
            root.after(1500, lambda: _kill_followup(status_var))
        elif r.returncode == 1:
            status_var.set("KILL: no matching qemu process found.")
        else:
            status_var.set(f"KILL TERM rc={r.returncode}: {r.stderr.strip()}")
    except Exception as e:
        status_var.set(f"KILL TERM failed: {e}")


def _kill_followup(status_var):
    try:
        r = subprocess.run(
            ["pkill", "-KILL", "-f", r"qemu-aarch64.*LD_PRELOAD"],
            capture_output=True, text=True, timeout=2,
        )
        if r.returncode == 0:
            status_var.set("SIGKILL'd qemu (was still alive after TERM).")
            append_to_game_log(f"{now_timestamp()} - [marker] KILL: SIGKILL fallback fired\n")
        else:
            status_var.set("Game shut down cleanly via SIGTERM.")
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
