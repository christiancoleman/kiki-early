#!/usr/bin/env python3
"""
marker_button.py — TIMER GUI for manual switch-press triggers.

Button press/release events append down/up commands to
runs/latest/press_request.txt; the shim's shim_manual_press_thread polls this
file every 200 ms and updates its held-switch overlay. Legacy one-line numeric
requests still work as taps.

Usage:
    python3 marker_button.py
    (or via run_game.sh's TIMER=ON path — needs DISPLAY/WAYLAND_DISPLAY)

A live "T+M:SS.x" elapsed timer updates every 100 ms at the top of the window.
"""

import datetime
import fcntl
import os
import pathlib
import subprocess
import sys
import time
import tkinter as tk
from tkinter import ttk

from switch_inventory import MANUAL_SWITCHES, SWITCH_BY_MANUAL_ID

# run_game.sh passes explicit paths so a symlinked _emulation directory can
# drive multiple sysroots without pathlib resolving back to the canonical tree.
_HERE = pathlib.Path(os.environ.get("SPIKE3_EMULATION_DIR", pathlib.Path(__file__).parent)).absolute()
_SYSROOT = pathlib.Path(os.environ.get("SPIKE3_SYSROOT", _HERE.parent.parent.parent)).absolute()

GAME_LOG_PATH = os.environ.get("SPIKE3_GAME_LOG_PATH", str(_HERE / "runs" / "latest" / "game.log"))
PRESS_REQUEST_PATH = os.environ.get("SHIM_PRESS_REQUEST_PATH", str(_HERE / "runs" / "latest" / "press_request.txt"))
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


def write_switch_command(action: str, sw_id: int, name: str,
                         elapsed_var, status_var, count_ref):
    """Append a switch command to press_request.txt.

    The shim accepts explicit "down N" / "up N" / "tap N" commands. O_APPEND
    keeps each short command atomic when future external clients join in.
    """
    try:
        line_out = f"{action} {sw_id}\n"
        fd = os.open(PRESS_REQUEST_PATH, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o644)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX)
            os.write(fd, line_out.encode("ascii"))
        finally:
            try:
                fcntl.flock(fd, fcntl.LOCK_UN)
            except OSError:
                pass
            os.close(fd)
        count_ref[0] += 1
        elapsed = elapsed_var.get()
        log_action = action.upper()
        line = f"{now_timestamp()} - [marker] {elapsed} event #{count_ref[0]} ({log_action} {name} sw_id={sw_id})\n"
        append_to_game_log(line)
        log_error(f"{log_action} request written: sw_id={sw_id} ({name})")
        try: status_var.set(f"#{count_ref[0]} {elapsed} -> {log_action} {name} (sw_id={sw_id})")
        except Exception: pass
    except OSError as e:
        log_error(f"{action.upper()} request write failed: {e}")
        try: status_var.set(f"{action.upper()} {name} FAILED: {e}")
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


_SERVICE_BY_ACTION = {
    "Back": "C12",
    "< -": "C11",
    "> -": "C10",
    "Select": "C9",
}
_SERVICE_BUTTONS = [
    (label, *SWITCH_BY_MANUAL_ID[manual_id], bg, fg)
    for label, manual_id, bg, fg in [
        ("Back", "C12", "#2e7d32", "white"),
        ("< -", "C11", "#c62828", "white"),
        ("> -", "C10", "#c62828", "white"),
        ("Select", "C9", "#111111", "white"),
    ]
]
_SERVICE_MANUAL_IDS = set(_SERVICE_BY_ACTION.values())
_GRID_SWITCHES = [row for row in MANUAL_SWITCHES
                  if row[0] not in _SERVICE_MANUAL_IDS]
_TAB_ORDER = ("Coin Door", "Cabinet", "Playfield", "CPU Node")
_TAB_SHORT_NAME = {
    "Coin Door": "Coin",
    "Cabinet": "Cab",
    "Playfield": "PF",
    "CPU Node": "CPU",
}


def _button_label(manual_id: str, address: str, name: str) -> str:
    short_name = name
    if len(short_name) > 18:
        short_name = short_name[:17] + "."
    return f"{manual_id}\n{address}\n{short_name}"


def _button_detail(manual_id: str, address: str, runtime_sw_id, name: str,
                   conn: str, location: str, switch_type: str) -> str:
    runtime = "not mapped" if runtime_sw_id is None else f"runtime sw_id {runtime_sw_id}"
    return (f"{manual_id}  {address}\n"
            f"{name}\n"
            f"{location}  {conn}  {switch_type}\n"
            f"{runtime}")


def _bind_hold_button(button, sw_id: int, name: str, held_set: set[int],
                      elapsed_var, status_var, count_ref):
    def on_down(_event, s=sw_id, n=name):
        if s in held_set:
            return "break"
        held_set.add(s)
        write_switch_command("down", s, n, elapsed_var, status_var, count_ref)
        return "break"

    def on_up(_event, s=sw_id, n=name):
        if s not in held_set:
            return "break"
        held_set.discard(s)
        write_switch_command("up", s, n, elapsed_var, status_var, count_ref)
        return "break"

    button.bind("<ButtonPress-1>", on_down)
    button.bind("<ButtonRelease-1>", on_up)
    button.bind("<KeyPress-space>", on_down)
    button.bind("<KeyRelease-space>", on_up)


def _switch_tabs():
    grouped = {name: [] for name in _TAB_ORDER}
    for row in _GRID_SWITCHES:
        loc = row[5]
        grouped.setdefault(loc, []).append(row)

    tabs = []
    for name, rows in grouped.items():
        short = _TAB_SHORT_NAME.get(name, name)
        for idx in range(0, len(rows), 4):
            tabs.append((f"{short} {idx // 4 + 1}", rows[idx:idx + 4]))
    return tabs


class _ToolTip:
    def __init__(self, widget, text: str):
        self.widget = widget
        self.text = text
        self.tip = None
        widget.bind("<Enter>", self._show)
        widget.bind("<Leave>", self._hide)

    def _show(self, _event=None):
        if self.tip or not self.text:
            return
        x = self.widget.winfo_rootx() + 16
        y = self.widget.winfo_rooty() + self.widget.winfo_height() + 6
        self.tip = tk.Toplevel(self.widget)
        self.tip.wm_overrideredirect(True)
        self.tip.wm_geometry(f"+{x}+{y}")
        tk.Label(
            self.tip,
            text=self.text,
            justify=tk.LEFT,
            bg="#fffde7",
            fg="#111",
            relief=tk.SOLID,
            borderwidth=1,
            font=("monospace", 9),
            padx=6,
            pady=4,
        ).pack()

    def _hide(self, _event=None):
        if self.tip:
            self.tip.destroy()
            self.tip = None


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
    root.title("Spike 3 TIMER Switches")
    root.geometry("820x720")

    elapsed_var = tk.StringVar(value="T+0:00.0")
    status_var = tk.StringVar(value="Hold a switch button.")
    count_ref = [0]
    held_switches: set[int] = set()

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

    service_frame = tk.Frame(root)
    service_frame.pack(padx=12, pady=(8, 6), fill=tk.X)
    for col, (label, manual_id, address, sw_id, name, conn, loc, typ, bg, fg) in enumerate(_SERVICE_BUTTONS):
        service_frame.columnconfigure(col, weight=1, uniform="service")
        button = tk.Button(
            service_frame,
            text=f"{label}\n{manual_id}\n{address}",
            bg=bg, fg=fg,
            activebackground=bg, activeforeground=fg,
            font=("monospace", 18, "bold"),
            height=3,
        )
        if sw_id is not None:
            _bind_hold_button(button, sw_id, name, held_switches,
                              elapsed_var, status_var, count_ref)
        _ToolTip(button, _button_detail(manual_id, address, sw_id, name,
                                        conn, loc, typ))
        button.grid(row=0, column=col, padx=5, pady=2, sticky="nsew")

    notebook = ttk.Notebook(root)
    notebook.pack(padx=12, pady=(4, 8), fill=tk.BOTH, expand=True)
    for tab_name, rows in _switch_tabs():
        tab = tk.Frame(notebook)
        notebook.add(tab, text=tab_name)
        for col in range(2):
            tab.columnconfigure(col, weight=1, uniform=f"{tab_name}-switch")
        for idx, (manual_id, address, sw_id, name, conn, loc, typ) in enumerate(rows):
            row, col = divmod(idx, 2)
            tab.rowconfigure(row, weight=1, uniform=f"{tab_name}-row")
            disabled = sw_id is None
            button = tk.Button(
                tab,
                text=_button_label(manual_id, address, name),
                bg="#eceff1" if not disabled else "#e0e0e0",
                fg="#111" if not disabled else "#777",
                activebackground="#cfd8dc", activeforeground="#111",
                font=("monospace", 16, "bold"),
                height=5,
                state=(tk.DISABLED if disabled else tk.NORMAL),
            )
            if not disabled:
                _bind_hold_button(button, sw_id, name, held_switches,
                                  elapsed_var, status_var, count_ref)
            _ToolTip(button, _button_detail(manual_id, address, sw_id, name,
                                            conn, loc, typ))
            button.grid(row=row, column=col, padx=5, pady=5, sticky="nsew")

    tk.Button(
        root, text="KILL GAME (SIGTERM → SIGKILL)",
        command=lambda: kill_game(root, status_var),
        bg="#b00020", fg="white",
        font=("monospace", 10, "bold"),
        height=1,
    ).pack(padx=12, pady=(8, 2), fill=tk.X)

    tk.Label(root, textvariable=status_var, wraplength=480, fg="#444",
             font=("monospace", 9)).pack(pady=4, padx=10)

    def on_close():
        for sw_id in list(held_switches):
            held_switches.discard(sw_id)
            write_switch_command("up", sw_id, f"Switch {sw_id}",
                                 elapsed_var, status_var, count_ref)
        root.destroy()

    root.protocol("WM_DELETE_WINDOW", on_close)
    root.mainloop()


if __name__ == "__main__":
    main()
