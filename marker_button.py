#!/usr/bin/env python3
"""
marker_button.py — observation-tagging GUI plus manual switch-press triggers.

Two button groups:
  - OBSERVATION: drops a timestamped marker into game.log so user observations
    interleave with shim/Godot output under the same timeline.
  - PRESS: writes runs/latest/press_request.txt; the shim's
    shim_manual_press_thread polls this file every 200 ms, parses it as a
    runtime sw_id, and drives a single press/release through the same
    translate() / SPI-rx-extra path the auto-press thread uses.

Usage:
    python3 marker_button.py
    (or via run_game.sh's TIMER=ON path — needs DISPLAY/WAYLAND_DISPLAY)

A live "T+M:SS.x" elapsed timer updates every 100 ms at the top of the window
so observations land with at-a-glance "this happened ~30 s in" framing
without having to subtract timestamps from the log later.
"""

import datetime
import os
import pathlib
import subprocess
import sys
import time
import tkinter as tk

# Derive paths from the script's location so the marker button works no matter
# where the repo is cloned. Layout: <SYSROOT>/games/pokemon_le/_emulation/marker_button.py
_HERE = pathlib.Path(__file__).resolve().parent  # _emulation/
_SYSROOT = _HERE.parent.parent.parent  # 3 up from _emulation = project sysroot

GAME_LOG_PATH = str(_HERE / "runs" / "latest" / "game.log")
PRESS_REQUEST_PATH = str(_HERE / "runs" / "latest" / "press_request.txt")
ERROR_LOG_PATH = str(_SYSROOT / "tmp" / "marker_button_errors.log")


def now_timestamp():
    n = datetime.datetime.now()
    return f"{n:%Y/%m/%d - %H:%M:%S}.{n.microsecond // 1000:03d}"


def append_to_game_log(line: str) -> tuple[bool, str]:
    """Raw O_APPEND write — atomic per line on Linux for sub-PIPE_BUF writes."""
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
        pass


def write_marker(label: str, elapsed_var, status_var, count_ref):
    try:
        count_ref[0] += 1
        elapsed = elapsed_var.get()
        line = f"{now_timestamp()} - [marker] {elapsed} click #{count_ref[0]} ({label})\n"
    except Exception as e:
        log_error(f"CLICK SETUP RAISED: {type(e).__name__}: {e}")
        return
    log_error(f"CLICK #{count_ref[0]} fired (label={label!r})")
    ok, detail = append_to_game_log(line)
    if ok:
        try: status_var.set(f"#{count_ref[0]} {elapsed} → {label}")
        except Exception: pass
        return
    log_error(f"CLICK #{count_ref[0]} APPEND FAILED: {detail} — line: {line.rstrip()}")
    print(f"[marker] click #{count_ref[0]} FAILED: {detail}", file=sys.stderr, flush=True)
    try: status_var.set(f"#{count_ref[0]} FAILED ({detail}). See {ERROR_LOG_PATH}.")
    except Exception: pass


def write_press_request(sw_id: int, name: str, elapsed_var, status_var, count_ref):
    """Write the requested sw_id to press_request.txt; the shim's
    shim_manual_press_thread reads + deletes the file + triggers a press."""
    try:
        with open(PRESS_REQUEST_PATH, "w") as f:
            f.write(f"{sw_id}\n")
        count_ref[0] += 1
        elapsed = elapsed_var.get()
        line = f"{now_timestamp()} - [marker] {elapsed} click #{count_ref[0]} (PRESS {name} sw_id={sw_id})\n"
        append_to_game_log(line)
        log_error(f"PRESS request written: sw_id={sw_id} ({name})")
        try: status_var.set(f"#{count_ref[0]} {elapsed} → PRESS {name} (sw_id={sw_id})")
        except Exception: pass
    except OSError as e:
        log_error(f"PRESS request write failed: {e}")
        try: status_var.set(f"PRESS {name} FAILED: {e}")
        except Exception: pass


_HELPER_PATTERNS = (
    r"qemu-aarch64.*LD_PRELOAD",
    r"python3.*frame_viewer\.py",
    r"python3.*audio_subscriber\.py",
    r"python3.*devbus_subscriber\.py",
    r"python3.*decode_daemon\.py",
    r"python3.*gl_daemon\.py",
    r"ffplay.*spike3_audio",
)


def kill_game(root, status_var):
    try:
        subprocess.run(
            ["pkill", "-TERM", "-f", r"run_game\.sh"],
            capture_output=True, text=True, timeout=2,
        )
        status_var.set("SIGTERM → run_game.sh. SIGKILL fallback in 2 s.")
        append_to_game_log(f"{now_timestamp()} - [marker] KILL: SIGTERM run_game.sh\n")
        root.after(2000, lambda: _kill_followup(root, status_var))
    except Exception as e:
        status_var.set(f"KILL TERM failed: {e}")


def _kill_followup(root, status_var):
    try:
        for pat in _HELPER_PATTERNS:
            subprocess.run(["pkill", "-KILL", "-f", pat],
                           capture_output=True, timeout=1)
        status_var.set("SIGKILL'd game/helpers. Parent script will finish diagnostics.")
        append_to_game_log(f"{now_timestamp()} - [marker] KILL: SIGKILL helpers fallback fired\n")
        root.after(1000, lambda: os._exit(0))
    except Exception as e:
        status_var.set(f"KILL KILL failed: {e}")


# Observation presets — labels describe what the user is SEEING on the screen
# at the moment of click, so log lines align with visual state.
_PRESETS = [
    ("Pokemon Logo (Startup In Progress...)", "#1565c0"),
    ("Service Menu (Main)",                   "#2e7d32"),
    ("Service Menu (#?)",                     "#558b2f"),
    ("Attract Mode (#1)",                     "#6a1b9a"),
    ("Attract Mode (#2)",                     "#8e24aa"),
    ("Sound Starts",                          "#ef6c00"),
    ("Sound Ends",                            "#bf360c"),
]

# Press buttons — runtime sw_id per PROTOCOL §11 + Step 93f. Each click writes
# the sw_id to PRESS_REQUEST_PATH; the shim's shim_manual_press_thread does
# the actual press through the same path the auto-press thread used.
_PRESSES = [
    ("PRESS Service Select (sw_id 90)", 90, "#0277bd"),
    ("PRESS Service Plus  (sw_id 91)",  91, "#0288d1"),
    ("PRESS Service Minus (sw_id 92)",  92, "#039be5"),
    ("PRESS Service Back  (sw_id 93)",  93, "#0097a7"),
    ("PRESS Left Coin     (sw_id 80)",  80, "#2e7d32"),
    ("PRESS Start Button  (sw_id 78)",  78, "#00838f"),
    ("PRESS Action Button (sw_id 77)",  77, "#006064"),
]


def _fmt_elapsed(secs: float) -> str:
    m = int(secs) // 60
    s = secs - m * 60
    return f"T+{m:d}:{s:04.1f}"


def _tick_timer(root, start_time, elapsed_var):
    elapsed = time.monotonic() - start_time
    elapsed_var.set(_fmt_elapsed(elapsed))
    root.after(100, lambda: _tick_timer(root, start_time, elapsed_var))


def main():
    log_error(f"STARTED pid={os.getpid()} GAME_LOG_PATH={GAME_LOG_PATH} "
              f"PRESS_REQUEST_PATH={PRESS_REQUEST_PATH} "
              f"exists={os.path.exists(GAME_LOG_PATH)} "
              f"writable={os.access(GAME_LOG_PATH, os.W_OK)}")

    root = tk.Tk()
    root.title("Spike 3 Marker + Press")
    root.geometry("520x780")

    elapsed_var = tk.StringVar(value="T+0:00.0")
    status_var = tk.StringVar(value="Click an observation OR press button.")
    count_ref = [0]

    # Top: live elapsed timer (updates every 100ms via root.after).
    tk.Label(
        root,
        textvariable=elapsed_var,
        font=("monospace", 24, "bold"),
        fg="#000",
        bg="#e3f2fd",
        height=2,
    ).pack(padx=8, pady=(8, 4), fill=tk.X)
    start_time = time.monotonic()
    _tick_timer(root, start_time, elapsed_var)

    tk.Label(root, text="Observation (mark what you see):", font=("monospace", 9)).pack(pady=(4, 0))
    for label, color in _PRESETS:
        tk.Button(
            root, text=label,
            command=lambda l=label: write_marker(l, elapsed_var, status_var, count_ref),
            bg=color, fg="white",
            font=("monospace", 9, "bold"),
            height=1,
        ).pack(padx=12, pady=1, fill=tk.X)

    tk.Label(root, text="Press (drive a switch):", font=("monospace", 9)).pack(pady=(8, 0))
    for label, sw_id, color in _PRESSES:
        tk.Button(
            root, text=label,
            command=lambda l=label, s=sw_id: write_press_request(
                s, l.split(" (")[0].replace("PRESS ", ""), elapsed_var, status_var, count_ref),
            bg=color, fg="white",
            font=("monospace", 9, "bold"),
            height=1,
        ).pack(padx=12, pady=1, fill=tk.X)

    tk.Button(
        root, text="KILL GAME (SIGTERM → SIGKILL)",
        command=lambda: kill_game(root, status_var),
        bg="#b00020", fg="white",
        font=("monospace", 10, "bold"),
        height=1,
    ).pack(padx=12, pady=(8, 2), fill=tk.X)

    tk.Label(root, textvariable=status_var, wraplength=480, fg="#444",
             font=("monospace", 9)).pack(pady=4, padx=10)

    root.mainloop()


if __name__ == "__main__":
    main()
