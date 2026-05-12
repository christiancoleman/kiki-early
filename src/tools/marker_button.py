#!/usr/bin/env python3
"""
marker_button.py — TIMER GUI for manual switch triggers.

Clicking a service-menu switch box toggles the requested switch state and
appends down/up commands to runs/latest/press_request.txt; the shim's
shim_manual_press_thread polls this file every 200 ms and updates its
held-switch overlay. Legacy one-line numeric requests still work as taps.

Usage:
    python3 marker_button.py
    (or via run_game.sh's TIMER=ON path — needs DISPLAY/WAYLAND_DISPLAY)
"""

import datetime
import os
import pathlib
import subprocess
import time
import tkinter as tk

from switch_command import append_locked
from switch_inventory import (
    GAME_TITLE,
    GAME_VERSION,
    SWITCH_BY_MANUAL_ID,
    SWITCH_BY_RUNTIME_ID,
    SWITCH_LAYOUT,
)

# run_game.sh passes explicit paths so a symlinked _emulation directory can
# drive multiple sysroots without pathlib resolving back to the canonical tree.
_HERE = pathlib.Path(os.environ.get("SPIKE3_EMULATION_DIR", pathlib.Path(__file__).resolve().parents[2])).absolute()
_SYSROOT = pathlib.Path(os.environ.get("SPIKE3_SYSROOT", _HERE.parent.parent.parent)).absolute()

GAME_LOG_PATH = os.environ.get("SPIKE3_GAME_LOG_PATH", str(_HERE / "runs" / "latest" / "game.log"))
PRESS_REQUEST_PATH = os.environ.get("SHIM_PRESS_REQUEST_PATH", str(_HERE / "runs" / "latest" / "press_request.txt"))
ERROR_LOG_PATH = str(_SYSROOT / "tmp" / "marker_button_errors.log")


def now_timestamp():
    n = datetime.datetime.now()
    return f"{n:%Y/%m/%d - %H:%M:%S}.{n.microsecond // 1000:03d}"


def append_to_game_log(line: str) -> tuple[bool, str]:
    try:
        append_locked(pathlib.Path(GAME_LOG_PATH), line)
    except OSError as e:
        return False, f"write: {e}"
    return True, "ok"


def log_error(detail: str):
    try:
        with open(ERROR_LOG_PATH, "a") as f:
            f.write(f"{now_timestamp()} - {detail}\n")
    except OSError:
        pass


def _switch_metadata(sw_id: int, fallback_name: str):
    row = SWITCH_BY_RUNTIME_ID.get(sw_id)
    if row is None:
        return "-", "-", fallback_name
    manual_id, address, _runtime_sw_id, name, _conn, _location, _switch_type = row
    return manual_id, address, name


def write_switch_command(action: str, sw_id: int, name: str,
                         elapsed_var, status_var, count_ref,
                         switch_test_var=None, observed_var=None):
    """Append a switch command to press_request.txt.

    The shim accepts explicit "down N" / "up N" / "tap N" commands. O_APPEND
    keeps each short command atomic when future external clients join in.
    """
    try:
        line_out = f"{action} {sw_id}\n"
        append_locked(pathlib.Path(PRESS_REQUEST_PATH), line_out)
        count_ref[0] += 1
        elapsed = elapsed_var.get()
        log_action = action.upper()
        manual_id, address, resolved_name = _switch_metadata(sw_id, name)
        line = f"{now_timestamp()} - [marker] {elapsed} event #{count_ref[0]} ({log_action} {name} sw_id={sw_id})\n"
        append_to_game_log(line)
        append_to_game_log(
            f"{now_timestamp()} - [external_switch] "
            f"{log_action} {manual_id} {address} {resolved_name} runtime_sw_id={sw_id}\n"
        )
        if switch_test_var is not None and switch_test_var.get():
            observed = ""
            if observed_var is not None:
                observed = observed_var.get().strip()
            test_line = (f"{now_timestamp()} - [switch_test] {elapsed} "
                         f"event #{count_ref[0]} command={log_action} "
                         f"sw_id={sw_id} name=\"{name}\" observed=\"{observed}\"\n")
            append_to_game_log(test_line)
        log_error(f"{log_action} request written: sw_id={sw_id} ({name})")
        try: status_var.set(f"#{count_ref[0]} {elapsed} -> {log_action} {name} (sw_id={sw_id})")
        except Exception: pass
    except OSError as e:
        log_error(f"{action.upper()} request write failed: {e}")
        try: status_var.set(f"{action.upper()} {name} FAILED: {e}")
        except Exception: pass


def write_raw_command(command: str, label: str, elapsed_var, status_var, count_ref):
    """Append a non-switch command understood by shim_manual_press_thread."""
    try:
        append_locked(pathlib.Path(PRESS_REQUEST_PATH), f"{command}\n")
        count_ref[0] += 1
        elapsed = elapsed_var.get()
        append_to_game_log(
            f"{now_timestamp()} - [marker] {elapsed} event #{count_ref[0]} ({label})\n"
        )
        log_error(f"{label} request written")
        try: status_var.set(f"#{count_ref[0]} {elapsed} -> {label}")
        except Exception: pass
    except OSError as e:
        log_error(f"{label} request write failed: {e}")
        try: status_var.set(f"{label} FAILED: {e}")
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
_QUICK_BUTTONS = [
    ("L FLIP", "9", "#263238", "#f5f5f5"),
    ("START", "93", "#0d47a1", "#f5f5f5"),
    ("ACTION", "90", "#1b5e20", "#f5f5f5"),
    ("R FLIP", "10", "#263238", "#f5f5f5"),
]
_ENTER_COIN_ID = "96"


def _bind_hold_button(button, sw_id: int, name: str, held_set: set[int],
                      elapsed_var, status_var, count_ref,
                      switch_test_var, observed_var):
    def on_down(_event, s=sw_id, n=name):
        if s in held_set:
            return "break"
        held_set.add(s)
        write_switch_command("down", s, n, elapsed_var, status_var, count_ref,
                             switch_test_var, observed_var)
        return "break"

    def on_up(_event, s=sw_id, n=name):
        if s not in held_set:
            return "break"
        held_set.discard(s)
        write_switch_command("up", s, n, elapsed_var, status_var, count_ref,
                             switch_test_var, observed_var)
        return "break"

    button.bind("<ButtonPress-1>", on_down)
    button.bind("<ButtonRelease-1>", on_up)
    button.bind("<KeyPress-space>", on_down)
    button.bind("<KeyRelease-space>", on_up)


def _write_tap_command(sw_id: int, name: str, elapsed_var, status_var,
                       count_ref, switch_test_var=None, observed_var=None,
                       hold_ms=120):
    try:
        append_locked(pathlib.Path(PRESS_REQUEST_PATH), f"tap {sw_id} {hold_ms}\n")
        count_ref[0] += 1
        elapsed = elapsed_var.get()
        manual_id, address, resolved_name = _switch_metadata(sw_id, name)
        append_to_game_log(
            f"{now_timestamp()} - [marker] {elapsed} event #{count_ref[0]} "
            f"(TAP {name} sw_id={sw_id})\n"
        )
        append_to_game_log(
            f"{now_timestamp()} - [external_switch] "
            f"TAP {manual_id} {address} {resolved_name} runtime_sw_id={sw_id}\n"
        )
        if switch_test_var is not None and switch_test_var.get():
            observed = observed_var.get().strip() if observed_var is not None else ""
            append_to_game_log(
                f"{now_timestamp()} - [switch_test] {elapsed} event #{count_ref[0]} "
                f"command=TAP sw_id={sw_id} name=\"{name}\" observed=\"{observed}\"\n"
            )
        status_var.set(f"#{count_ref[0]} {elapsed} -> TAP {name} (sw_id={sw_id})")
    except OSError as e:
        log_error(f"TAP request write failed: {e}")
        status_var.set(f"TAP {name} FAILED: {e}")


def _switch_layout_rows():
    columns = int(SWITCH_LAYOUT.get("columns", 30))
    row_count = int(SWITCH_LAYOUT.get("rows", 5))
    numeric_start = int(SWITCH_LAYOUT.get("numeric_start", 1))
    numeric_through = int(
        SWITCH_LAYOUT.get("numeric_through", numeric_start + columns * row_count - 1)
    )
    extended_prefix = str(SWITCH_LAYOUT.get("extended_prefix", "C"))
    extended_start = int(SWITCH_LAYOUT.get("extended_start", 1))

    rows = []
    slot = 0
    for row_number in range(row_count):
        row = []
        for _col in range(columns):
            numeric_id = numeric_start + slot
            if numeric_id <= numeric_through:
                manual_id = str(numeric_id)
            else:
                extended_id = extended_start + numeric_id - numeric_through - 1
                manual_id = f"{extended_prefix}{extended_id}"
            row.append((manual_id, SWITCH_BY_MANUAL_ID.get(manual_id.upper())))
            slot += 1
        rows.append((f"row {row_number + 1}", row))
    return rows


def _compact_detail(row, requested_on=False, actual_on=None) -> str:
    manual_id, address, sw_id, name, conn, location, switch_type = row
    runtime = "not mapped" if sw_id is None else f"runtime sw_id {sw_id}"
    requested = "ON" if requested_on else "OFF"
    actual = "not wired yet" if actual_on is None else ("ON" if actual_on else "OFF")
    return (f"{manual_id}  {address}  {runtime}\n"
            f"{name}\n"
            f"{location}  {conn}  {switch_type}\n"
            f"Requested: {requested}   Game state: {actual}")


def _render_switch_canvas(canvas, layout_rows, cell_items, requested_on):
    canvas.delete("all")
    cell_items.clear()

    cell = 28
    gap = 8
    row_gap = 18
    left = 12
    top = 12
    bg = "#7b0000"
    outer = "#8f8f8f"
    middle = "#cfcfcf"
    off = "#780000"
    on = "#f8f8f8"
    disabled = "#4d0000"

    max_cols = max((len(row) for _title, row in layout_rows), default=0)
    width = left * 2 + max_cols * cell + max(0, max_cols - 1) * gap
    height = top * 2 + len(layout_rows) * cell + max(0, len(layout_rows) - 1) * row_gap
    canvas.configure(width=width, height=height, bg=bg, scrollregion=(0, 0, width, height))

    for r, (title, row) in enumerate(layout_rows):
        y = top + r * (cell + row_gap)
        for c, (manual_id, switch_row) in enumerate(row):
            if switch_row is None:
                continue
            x = left + c * (cell + gap)
            requested = manual_id.upper() in requested_on
            fill = on if requested else off
            if switch_row[2] is None:
                fill = disabled
            outer_id = canvas.create_rectangle(x, y, x + cell, y + cell,
                                               outline=outer, width=2, fill=bg)
            mid_id = canvas.create_rectangle(x + 4, y + 4, x + cell - 4, y + cell - 4,
                                             outline=middle, width=2, fill=fill)
            inner_id = canvas.create_rectangle(x + 9, y + 9, x + cell - 9, y + cell - 9,
                                               outline="#3f3f3f", width=1, fill=fill)
            if switch_row[2] is None:
                canvas.create_line(x + 9, y + cell - 9, x + cell - 9, y + 9,
                                   fill="#9a9a9a", width=2)
            cell_items[manual_id.upper()] = (outer_id, mid_id, inner_id)


def _set_canvas_switch_state(canvas, cell_items, manual_id, is_on, disabled=False):
    items = cell_items.get(manual_id.upper())
    if not items:
        return
    fill = "#4d0000" if disabled else ("#f8f8f8" if is_on else "#780000")
    for item in items[1:]:
        canvas.itemconfigure(item, fill=fill)


def _fmt_elapsed(secs: float) -> str:
    m = int(secs) // 60
    s = secs - m * 60
    return f"T+{m:d}:{s:04.1f}"


def _tick_timer(root, start_time, elapsed_var):
    elapsed = time.monotonic() - start_time
    elapsed_var.set(_fmt_elapsed(elapsed))
    root.after(100, lambda: _tick_timer(root, start_time, elapsed_var))


def _log_switch_test_mode(enabled: bool, elapsed_var, status_var, observed_var):
    elapsed = elapsed_var.get()
    state = "START" if enabled else "STOP"
    observed = observed_var.get().strip()
    line = f"{now_timestamp()} - [switch_test] {elapsed} {state} observed=\"{observed}\"\n"
    append_to_game_log(line)
    try:
        status_var.set(f"Switch test {state.lower()} at {elapsed}")
    except Exception:
        pass


def _log_observed_switches(elapsed_var, status_var, observed_var):
    elapsed = elapsed_var.get()
    observed = observed_var.get().strip()
    if not observed:
        observed = "-"
    line = f"{now_timestamp()} - [switch_test] {elapsed} OBSERVED toggles=\"{observed}\"\n"
    append_to_game_log(line)
    try:
        status_var.set(f"{elapsed} observed toggles: {observed}")
    except Exception:
        pass


def _make_round_hold_button(parent, row, label, fill, fg, held_set,
                            elapsed_var, status_var, count_ref,
                            switch_test_var, observed_var):
    manual_id, address, sw_id, name, _conn, _loc, _typ = row
    canvas = tk.Canvas(parent, width=74, height=74, bg=parent["bg"],
                       highlightthickness=0)
    oval = canvas.create_oval(7, 7, 67, 67, fill=fill, outline="#eceff1", width=2)
    text = canvas.create_text(37, 37, text=label, fill=fg,
                              font=("monospace", 9, "bold"), justify=tk.CENTER)

    def on_down(_event, s=sw_id, n=name):
        if s is None or s in held_set:
            return "break"
        held_set.add(s)
        canvas.itemconfigure(oval, fill="#ffffff")
        canvas.itemconfigure(text, fill="#111111")
        write_switch_command("down", s, n, elapsed_var, status_var, count_ref,
                             switch_test_var, observed_var)
        return "break"

    def on_up(_event, s=sw_id, n=name):
        if s is None or s not in held_set:
            return "break"
        held_set.discard(s)
        canvas.itemconfigure(oval, fill=fill)
        canvas.itemconfigure(text, fill=fg)
        write_switch_command("up", s, n, elapsed_var, status_var, count_ref,
                             switch_test_var, observed_var)
        return "break"

    for item in (oval, text):
        canvas.tag_bind(item, "<ButtonPress-1>", on_down)
        canvas.tag_bind(item, "<ButtonRelease-1>", on_up)
    canvas.bind("<ButtonPress-1>", on_down)
    canvas.bind("<ButtonRelease-1>", on_up)
    return canvas


def main():
    log_error(f"STARTED pid={os.getpid()} GAME_LOG_PATH={GAME_LOG_PATH} "
              f"PRESS_REQUEST_PATH={PRESS_REQUEST_PATH} "
              f"exists={os.path.exists(GAME_LOG_PATH)} "
              f"writable={os.access(GAME_LOG_PATH, os.W_OK)}")

    root = tk.Tk()
    root.title(f"Service Switch Panel - {GAME_TITLE} {GAME_VERSION}")
    root.geometry("1180x560")

    elapsed_var = tk.StringVar(value="T+0:00.0")
    status_var = tk.StringVar(value="Click a switch box to toggle requested ON/OFF.")
    detail_var = tk.StringVar(
        value="Select a switch. White = requested ON. Dark red = requested OFF. "
              "GRAB STATE dumps the game's actual switch states to game.log ([switch_dump]); "
              "an on-canvas overlay of that is a future devbus feed."
    )
    switch_test_var = tk.BooleanVar(value=False)
    observed_var = tk.StringVar(value="")
    count_ref = [0]
    held_switches: set[int] = set()
    requested_on: set[str] = set()
    cell_items = {}

    title_frame = tk.Frame(root)
    title_frame.pack(padx=10, pady=(6, 2), fill=tk.X)
    tk.Label(
        title_frame,
        text=f"Service Switch Panel  {GAME_TITLE} {GAME_VERSION}",
        font=("monospace", 14, "bold"),
        fg="#111",
    ).pack(side=tk.LEFT)
    start_time = time.monotonic()
    _tick_timer(root, start_time, elapsed_var)

    quick_frame = tk.Frame(root, bg="#111111")
    quick_frame.pack(padx=10, pady=(0, 3))
    for col, (label, manual_id, fill, fg) in enumerate(_QUICK_BUTTONS):
        row = SWITCH_BY_MANUAL_ID[manual_id]
        quick = _make_round_hold_button(quick_frame, row, label, fill, fg,
                                        held_switches, elapsed_var, status_var,
                                        count_ref, switch_test_var, observed_var)
        quick.grid(row=0, column=col, padx=16, pady=3)

    service_frame = tk.Frame(root)
    service_frame.pack(padx=10, pady=(0, 4), fill=tk.X)
    for col, (label, manual_id, address, sw_id, name, conn, loc, typ, bg, fg) in enumerate(_SERVICE_BUTTONS):
        service_frame.columnconfigure(col, weight=1, uniform="service")
        button = tk.Button(
            service_frame,
            text=label,
            bg=bg, fg=fg,
            activebackground=bg, activeforeground=fg,
            font=("monospace", 15, "bold"),
            height=1,
        )
        if sw_id is not None:
            _bind_hold_button(button, sw_id, name, held_switches,
                              elapsed_var, status_var, count_ref,
                              switch_test_var, observed_var)
        button.grid(row=0, column=col, padx=3, sticky="nsew")

    layout_rows = _switch_layout_rows()
    canvas_frame = tk.Frame(root, bg="#7b0000")
    canvas_frame.pack(padx=10, pady=(2, 5), fill=tk.BOTH, expand=True)
    switch_canvas = tk.Canvas(canvas_frame, bg="#7b0000", highlightthickness=0)
    hscroll = tk.Scrollbar(canvas_frame, orient=tk.HORIZONTAL,
                           command=switch_canvas.xview)
    switch_canvas.configure(xscrollcommand=hscroll.set)
    switch_canvas.pack(side=tk.TOP, fill=tk.BOTH, expand=True)
    hscroll.pack(side=tk.BOTTOM, fill=tk.X)
    _render_switch_canvas(switch_canvas, layout_rows, cell_items, requested_on)

    def select_switch(row, toggle=False):
        manual_id, address, sw_id, name, conn, loc, typ = row
        key = manual_id.upper()
        if toggle and sw_id is not None:
            if key in requested_on:
                requested_on.discard(key)
                held_switches.discard(sw_id)
                write_switch_command("up", sw_id, name, elapsed_var, status_var,
                                     count_ref, switch_test_var, observed_var)
            else:
                requested_on.add(key)
                held_switches.add(sw_id)
                write_switch_command("down", sw_id, name, elapsed_var, status_var,
                                     count_ref, switch_test_var, observed_var)
            _set_canvas_switch_state(switch_canvas, cell_items, key, key in requested_on)
        elif toggle:
            status_var.set(f"{manual_id} {address} is not mapped to a runtime sw_id.")
        detail_var.set(_compact_detail(row, requested_on=(key in requested_on)))

    for _title, row in layout_rows:
        for manual_id, switch_row in row:
            if switch_row is None:
                continue
            key = manual_id.upper()
            for item in cell_items.get(key, ()):
                switch_canvas.tag_bind(item, "<Button-1>",
                                       lambda _event, r=switch_row: select_switch(r, True))
                switch_canvas.tag_bind(item, "<Enter>",
                                       lambda _event: switch_canvas.configure(cursor="hand2"),
                                       add="+")
                switch_canvas.tag_bind(item, "<Leave>",
                                       lambda _event: switch_canvas.configure(cursor=""),
                                       add="+")

    detail_label = tk.Label(root, textvariable=detail_var, justify=tk.LEFT,
                            anchor="w", bg="#1f1f1f", fg="#f5f5f5",
                            font=("monospace", 9), padx=8, pady=4)
    detail_label.pack(padx=10, pady=(0, 3), fill=tk.X)

    bottom = tk.Frame(root)
    bottom.pack(padx=10, pady=(0, 6), fill=tk.X)
    bottom.columnconfigure(0, weight=1, uniform="bottom")
    bottom.columnconfigure(1, weight=1, uniform="bottom")
    bottom.columnconfigure(2, weight=1, uniform="bottom")

    door_frame = tk.Frame(bottom)
    door_frame.grid(row=0, column=0, sticky="ew", padx=(0, 5))
    door_frame.columnconfigure(0, weight=1)
    door_frame.columnconfigure(1, weight=1)
    tk.Button(
        door_frame,
        text="DOOR OPEN",
        command=lambda: write_raw_command(
            "coin_door open", "COIN DOOR OPEN", elapsed_var, status_var, count_ref
        ),
        bg="#f9a825", fg="#111",
        font=("monospace", 10, "bold"),
        height=1,
    ).grid(row=0, column=0, padx=(0, 3), sticky="ew")
    tk.Button(
        door_frame,
        text="DOOR CLOSED",
        command=lambda: write_raw_command(
            "coin_door closed", "COIN DOOR CLOSED", elapsed_var, status_var, count_ref
        ),
        bg="#2e7d32", fg="white",
        font=("monospace", 10, "bold"),
        height=1,
    ).grid(row=0, column=1, padx=(3, 0), sticky="ew")

    coin_row = SWITCH_BY_MANUAL_ID[_ENTER_COIN_ID]
    tk.Button(
        bottom,
        text="ENTER COIN",
        command=lambda r=coin_row: _write_tap_command(
            r[2], r[3], elapsed_var, status_var, count_ref,
            switch_test_var, observed_var
        ),
        bg="#004d40", fg="white",
        font=("monospace", 12, "bold"),
        height=1,
    ).grid(row=0, column=1, sticky="ew", padx=5)

    tk.Label(
        bottom,
        textvariable=elapsed_var,
        font=("monospace", 15, "bold"),
        fg="#000",
        bg="#e3f2fd",
        height=1,
    ).grid(row=0, column=2, sticky="ew", padx=(5, 0))

    footer = tk.Frame(root)
    footer.pack(padx=10, pady=(0, 5), fill=tk.X)
    footer.columnconfigure(0, weight=1)
    tk.Label(footer, textvariable=status_var, anchor="w", fg="#444",
             font=("monospace", 9)).grid(row=0, column=0, sticky="ew", padx=(0, 6))
    tk.Button(
        footer, text="GRAB STATE",
        command=lambda: write_raw_command(
            "dump_switches", "GRAB SWITCH STATE", elapsed_var, status_var, count_ref
        ),
        bg="#00695c", fg="white",
        font=("monospace", 9, "bold"),
        height=1,
    ).grid(row=0, column=1, sticky="e", padx=(0, 6))
    tk.Button(
        footer, text="KILL GAME",
        command=lambda: kill_game(root, status_var),
        bg="#b00020", fg="white",
        font=("monospace", 9, "bold"),
        height=1,
    ).grid(row=0, column=2, sticky="e")

    def on_close():
        if switch_test_var.get():
            _log_switch_test_mode(False, elapsed_var, status_var, observed_var)
            switch_test_var.set(False)
        for sw_id in list(held_switches):
            held_switches.discard(sw_id)
            write_switch_command("up", sw_id, f"Switch {sw_id}",
                                 elapsed_var, status_var, count_ref,
                                 switch_test_var, observed_var)
        root.destroy()

    root.protocol("WM_DELETE_WINDOW", on_close)
    root.mainloop()


if __name__ == "__main__":
    main()
