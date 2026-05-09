#!/usr/bin/env bash
# reset_run_state.sh — clear per-run persistent state for "first boot" testing.
#
# Per the 2026-05-09 finding (JOURNAL Step 94k followup): a fresh-state run
# (no NVRAM, no warmed shader caches) was empirically on par with the
# best-ever run. Use this script to reproduce that first-boot condition
# before each test launch when you want a clean baseline.
#
# Usage:
#   ./reset_run_state.sh          # delete current state, no backup (default)
#   ./reset_run_state.sh --keep   # move state aside to *.backup_<TS> dirs
#                                 # (reversible; restore by mv-back)
#   ./reset_run_state.sh --help
#
# What gets cleared:
#   /data/nv/pokemon_le/          — NVRAM tiers (NVM, PIN_NVRAM, SYS_NVRAM, etc.)
#   /tmp/cache/mesa/              — Mesa shader cache (warmed across runs)
#   /tmp/cache/shader_cache/      — Godot/other shader cache
#
# What is NOT cleared:
#   runs/                         — per-run logs (you keep those)
#   tmp/godot_logs* / tmp/blob_*  — output-only, game doesn't read them
#   /connectivity/*               — already empty
#
# Safe to run when no game is currently launched. The script does not start
# the game itself — chain it: ./scripts/reset_run_state.sh && ./run_game.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SYSROOT="$(cd "$SCRIPT_DIR/../../../.." && pwd)"

NVRAM_DIR="$SYSROOT/data/nv/pokemon_le"
MESA_CACHE="$SYSROOT/tmp/cache/mesa"
SHADER_CACHE="$SYSROOT/tmp/cache/shader_cache"

MODE="delete"
case "${1:-}" in
    -h|--help)
        sed -n '/^# /{s/^# //;p}' "$0" | head -n 20
        exit 0
        ;;
    --keep|-k)
        MODE="keep"
        ;;
    "")
        ;;
    *)
        echo "[reset_run_state] unknown arg: $1 — see --help" >&2
        exit 1
        ;;
esac

clear_dir() {
    local target="$1"
    if [[ ! -d "$target" ]]; then
        printf '[reset_run_state] %-50s — not present (already clean)\n' "$target"
        return
    fi
    if [[ "$MODE" == "keep" ]]; then
        local backup="${target}.backup_$(date +%Y%m%d-%H%M%S)"
        mv "$target" "$backup"
        printf '[reset_run_state] %-50s → %s\n' "$target" "$backup"
    else
        rm -rf "$target"
        printf '[reset_run_state] %-50s — deleted\n' "$target"
    fi
}

printf '[reset_run_state] mode=%s\n' "$MODE"
clear_dir "$NVRAM_DIR"
clear_dir "$MESA_CACHE"
clear_dir "$SHADER_CACHE"
printf '[reset_run_state] done — next run will see fresh state.\n'
