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


# Press buttons — runtime sw_id per PROTOCOL §11 + Step 93f. Each click writes
# the sw_id to PRESS_REQUEST_PATH; the shim's shim_manual_press_thread does
# the actual press through the same path the auto-press thread used.
_MANUAL_SWITCHES = [
    # manual_id, address, runtime_sw_id, name, connector/input, location, type
    ("1", "8-SW-17", 17, "Left Outlane", "CN11/4", "Playfield", "Rollover"),
    ("2", "8-SW-18", 18, "Left Return Lane", "CN11/5", "Playfield", "Rollover"),
    ("5", "8-SW-19", 19, "Right Return Lane", "CN11/6", "Playfield", "Rollover"),
    ("6", "8-SW-20", 20, "Right Outlane", "CN11/7", "Playfield", "Rollover"),
    ("7", "8-SW-30", 21, "Left Slingshot", "CN10/4", "Playfield", "Leaf"),
    ("8", "8-SW-29", 22, "Right Slingshot", "CN10/3", "Playfield", "Leaf"),
    ("9", "8-SW-25", 23, "Left Flipper Button", "CN9/4", "Cabinet", "Leaf"),
    ("10", "8-SW-24", 24, "Right Flipper Button", "CN9/3", "Cabinet", "Leaf"),
    ("11", "8-SW-16", 25, "Left Flipper EOS", "CN10/6", "Playfield", "Leaf"),
    ("12", "8-SW-31", 26, "Right Flipper EOS", "CN10/5", "Playfield", "Leaf"),
    ("15", "8-SW-32", 27, "Trough 6", "8a/CN14", "Playfield", "Opto"),
    ("16", "8-SW-33", 28, "Trough 5", "8a/CN14", "Playfield", "Opto"),
    ("17", "8-SW-34", 29, "Trough 4", "8a/CN14", "Playfield", "Opto"),
    ("18", "8-SW-35", 30, "Trough 3", "8a/CN14", "Playfield", "Opto"),
    ("19", "8-SW-36", 31, "Trough 2", "8a/CN14", "Playfield", "Opto"),
    ("20", "8-SW-37", 32, "Trough 1", "8a/CN14", "Playfield", "Opto"),
    ("21", "8-SW-38", 33, "Trough Jam", "8a/CN14", "Playfield", "Opto"),
    ("22", "8-SW-28", 34, "Shooter Lane", "CN10/2", "Playfield", "Micro"),
    ("25", "8-SW-27", 35, "Bowl Opto", "CN11/3", "Playfield", "Opto"),
    ("27", "8-SW-9", 36, "Berry Target Left", "CN12/3", "Playfield", "Leaf target"),
    ("28", "8-SW-10", 37, "Berry Target Center", "CN12/4", "Playfield", "Leaf target"),
    ("29", "8-SW-11", 38, "Left Ramp Enter Opto", "CN12/5", "Playfield", "Opto"),
    ("30", "8-SW-12", 39, "Left Ramp Made 2 Gate", "CN12/6", "Playfield", "Micro"),
    ("31", "8-SW-8", 40, "Subway Enter", "CN12/2", "Playfield", "Opto"),
    ("33", "8-SW-0", 41, "Lower Left Target-Top", "CN13/2", "Playfield", "Leaf target"),
    ("34", "8-SW-1", 42, "Lower Left Target-Mid", "CN13/3", "Playfield", "Leaf target"),
    ("35", "8-SW-2", 43, "Lower Left Target-Bot", "CN13/4", "Playfield", "Leaf target"),
    ("36", "8-SW-3", 44, "Lower Right Target-Top", "CN13/5", "Playfield", "Leaf target"),
    ("37", "8-SW-4", 45, "Lower Right Target-Bot", "CN13/6", "Playfield", "Leaf target"),
    ("40", "8-SW-5", 46, "Left Eject Opto", "CN13/7", "Playfield", "Opto"),
    ("41", "9-SW-1", 47, "Right Eject", "CN13/3", "Playfield", "Micro"),
    ("44", "9-SW-8", 48, "Left Orbit", "CN12/2", "Playfield", "Rollover"),
    ("45", "9-SW-9", 49, "Left Ramp Made 1 Opto", "CN12/3", "Playfield", "Opto"),
    ("46", "9-SW-29", 50, "Rear Ramp Made", "CN10/3", "Playfield", "Micro"),
    ("49", "9-SW-13", 51, "Arena Left 10 Pt Sw", "CN12/7", "Playfield", "Leaf"),
    ("50", "9-SW-14", 52, "Arena Right 10 Pt Switch", "CN12/8", "Playfield", "Leaf"),
    ("51", "9-SW-26", 53, "(B)ATTLE Target", "CN11/2", "Playfield", "Leaf target"),
    ("52", "9-SW-27", 54, "B(A)TTLE Target", "CN11/3", "Playfield", "Leaf target"),
    ("53", "9-SW-17", 55, "BA(T)TLE Target", "CN11/4", "Playfield", "Leaf target"),
    ("54", "9-SW-18", 56, "BAT(T)LE Target", "CN11/5", "Playfield", "Leaf target"),
    ("55", "9-SW-19", 57, "BATT(L)E Target", "CN11/6", "Playfield", "Leaf target"),
    ("56", "9-SW-20", 58, "BATTL(E) Target", "CN11/7", "Playfield", "Leaf target"),
    ("57", "9-SW-21", 59, "Team Rocket Target Left", "CN11/8", "Playfield", "Leaf target"),
    ("58", "9-SW-22", 60, "Team Rocket Target Right", "CN11/9", "Playfield", "Leaf target"),
    ("59", "9-SW-23", 61, "Meowth Hit Opto", "CN11/10", "Playfield", "Opto"),
    ("60", "9-SW-24", 62, "Meowth Up", "CN9/3", "Playfield", "Magnet switch"),
    ("61", "9-SW-25", 63, "Meowth Down", "CN9/4", "Playfield", "Magnet switch"),
    ("63", "9-SW-28", 64, "Top Pop Bumper", "CN10/2", "Playfield", "Leaf"),
    ("64", "9-SW-30", 65, "Bot Pop Bumper", "CN10/4", "Playfield", "Leaf"),
    ("66", "9-SW-10", 66, "Top Lane Left", "CN12/4", "Playfield", "Rollover"),
    ("67", "9-SW-11", 67, "Top Lane Right", "CN12/5", "Playfield", "Rollover"),
    ("70", "9-SW-0", 68, "Berry Target Right", "CN13/2", "Playfield", "Leaf target"),
    ("71", "9-SW-4", 69, "Captive Ball-Lower", "CN13/6", "Playfield", "Rollover"),
    ("72", "9-SW-5", 70, "Captive Ball-Upper", "CN13/7", "Playfield", "Rollover"),
    ("73", "9-SW-6", 71, "Captive Ball-Target", "CN13/8", "Playfield", "Leaf target"),
    ("74", "9-SW-15", 72, "Right Ramp Hold", "CN12/9", "Playfield", "Micro"),
    ("76", "9-SW-31", 73, "Right Ramp Made Opto", "CN10/5", "Playfield", "Opto"),
    ("78", "9-SW-7", 74, "Spinner Opto", "CN13/9", "Playfield", "Opto"),
    ("79", "9-SW-2", 75, "Team Rocket Enter", "CN13/4", "Playfield", "Rollover"),
    ("80", "9-SW-3", 76, "Right Orbit", "CN13/5", "Playfield", "Rollover"),
    ("90", "1-SW-2", 77, "Action Button", "CN7/10", "Cabinet", "Leaf"),
    ("93", "1-SW-11", 78, "Start Button", "CN6/11", "Cabinet", "Micro"),
    ("94", "1-SW-12", 79, "Tournament Start Button", "CN6/9", "Cabinet", "Micro"),
    ("96", "1-SW-16", 80, "Left Coin", "CN5/9", "Coin Door", "Micro"),
    ("97", "1-SW-18", 81, "Right Coin", "CN5/7", "Coin Door", "Micro"),
    ("98", "1-SW-17", 82, "Center Coin", "CN5/8", "Coin Door", "Micro"),
    ("99", "1-SW-19", 83, "Fourth Coin", "CN5/6", "Coin Door", "-"),
    ("100", "1-SW-20", 84, "Fifth Coin", "CN5/5", "Coin Door", "-"),
    ("101", "1-SW-14", 85, "Tilt Pendulum", "CN6/7", "Cabinet", "Plumb bob"),
    ("102", "1-SW-21", 86, "Sixth Coin", "CN9/5", "Cabinet", "-"),
    ("103", "1-SW-8", 87, "Ticket Notch", "CN11/5", "Cabinet", "-"),
    ("104", "1-SW-22", 88, "Slam Tilt", "CN5/4", "Cabinet", "-"),
    ("105", "1-SW-7", 89, "Ticket Low", "CN2/2", "Cabinet", "-"),
    ("C9", "0-SW-8", 90, "Service Select", "CN25/1", "Coin Door", "Switch"),
    ("C10", "0-SW-9", 91, "Service Plus", "CN25/2", "Coin Door", "Switch"),
    ("C11", "0-SW-10", 92, "Service Minus", "CN25/3", "Coin Door", "Switch"),
    ("C12", "0-SW-11", 93, "Service Back", "CN25/4", "Coin Door", "Switch"),
    ("C17", "0-SW-16", 94, "Headphone Detect", "-", "Coin Door", "Detect"),
    ("C18", "0-SW-17", 95, "Headphone Kit Cable Detect", "CN3/5", "Coin Door", "Detect"),
    ("C19", "0-SW-18", 96, "Volume Encoder 1", "CN3/6", "Coin Door", "Encoder"),
    ("C20", "0-SW-19", 97, "Volume Encoder 2", "CN3/7", "Coin Door", "Encoder"),
    ("C21", "0-SW-20", None, "Jumper LVDS Resolution 1", "-", "CPU Node", "Jumper"),
    ("C22", "0-SW-21", None, "Jumper LVDS Resolution 2", "-", "CPU Node", "Jumper"),
    ("C23", "0-SW-22", None, "Jumper Unused", "-", "CPU Node", "Jumper"),
    ("C24", "0-SW-23", 98, "Coin Door Power Interlock", "CN4/1", "Cabinet", "Interlock"),
]

_SERVICE_BY_ACTION = {
    "Back": "C12",
    "< -": "C11",
    "> -": "C10",
    "Select": "C9",
}
_SWITCH_BY_MANUAL_ID = {row[0]: row for row in _MANUAL_SWITCHES}
_SERVICE_BUTTONS = [
    (label, *_SWITCH_BY_MANUAL_ID[manual_id], bg, fg)
    for label, manual_id, bg, fg in [
        ("Back", "C12", "#2e7d32", "white"),
        ("< -", "C11", "#c62828", "white"),
        ("> -", "C10", "#c62828", "white"),
        ("Select", "C9", "#111111", "white"),
    ]
]
_SERVICE_MANUAL_IDS = set(_SERVICE_BY_ACTION.values())
_GRID_SWITCHES = [row for row in _MANUAL_SWITCHES
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
