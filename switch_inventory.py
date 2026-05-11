#!/usr/bin/env python3
"""Shared Pokemon LE switch inventory for TIMER and external switch commands."""

from __future__ import annotations


# manual_id, address, runtime_sw_id, name, connector/input, location, type
MANUAL_SWITCHES = [
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

SWITCH_BY_MANUAL_ID = {row[0].upper(): row for row in MANUAL_SWITCHES}
SWITCH_BY_ADDRESS = {row[1].upper(): row for row in MANUAL_SWITCHES}
SWITCH_BY_RUNTIME_ID = {
    row[2]: row
    for row in MANUAL_SWITCHES
    if row[2] is not None
}


def resolve_switch(token: str, runtime: bool = False):
    if runtime:
        sw_id = int(token, 10)
        row = SWITCH_BY_RUNTIME_ID.get(sw_id)
        if row is not None:
            return row
        return (str(sw_id), "runtime", sw_id, f"Switch {sw_id}", "-", "-", "-")

    key = token.upper()
    if key in SWITCH_BY_MANUAL_ID:
        return SWITCH_BY_MANUAL_ID[key]
    if key in SWITCH_BY_ADDRESS:
        return SWITCH_BY_ADDRESS[key]
    if token.isdigit() and token in SWITCH_BY_MANUAL_ID:
        return SWITCH_BY_MANUAL_ID[token]
    if token.isdigit():
        raise SystemExit(
            f"unknown manual switch ID {token!r}; use --runtime for runtime sw_id {token}"
        )
    raise SystemExit(f"unknown switch {token!r}; use manual ID, N-SW-K address, or --runtime")
