#!/usr/bin/env bash
# setup_bind_mounts.sh — establish kernel-level bind mounts so the game can
# resolve its native paths (/games, /connectivity, /data) without any
# LD_PRELOAD remapping.
#
# Game-agnostic: pass the sysroot path of any Spike-3 game and it'll bind
# /games, /connectivity, /data to the corresponding sysroot subdirs.
#
# Usage:
#   sudo ./setup_bind_mounts.sh                                    # default = pokemon_le
#   sudo ./setup_bind_mounts.sh /mnt/c/Workspace/starwars-le-0.85.0 # explicit sysroot
#   ./setup_bind_mounts.sh --check [sysroot]                        # verify only
#   sudo ./setup_bind_mounts.sh --umount                            # tear down
#   sudo ./setup_bind_mounts.sh --switch <sysroot>                  # umount + remount to <sysroot>
#
# Tracking: the only host-level state this creates outside the project is the
# mount-point directories (/games, /connectivity, /data) at root. They are
# zero-content directories that exist only as bind-mount targets. If you ever
# need to remove them, sudo rmdir /games /connectivity /data after --umount.

set -euo pipefail

PLE_DEFAULT=/mnt/c/Workspace/pokemon-le-0.81.0

declare -A MOUNT_SUFFIX=(
    [/games]="/games"
    [/connectivity]="/connectivity"
    [/data]="/data"
)

usage() {
    sed -n '2,18p' "$0"
    exit 2
}

is_mounted() {
    findmnt -n "$1" >/dev/null 2>&1
}

current_mount_source() {
    findmnt -n -o SOURCE "$1" 2>/dev/null
}

setup() {
    local sysroot="$1"
    [[ -d "$sysroot" ]] || { echo "ERROR: sysroot does not exist: $sysroot" >&2; exit 3; }
    case "$sysroot" in /mnt/c/Workspace/*) : ;;
        *) echo "ERROR: sysroot must be under /mnt/c/Workspace/: $sysroot" >&2; exit 3 ;;
    esac
    [[ "$EUID" -eq 0 ]] || { echo "ERROR: setup requires root (run with sudo)" >&2; exit 2; }

    for target in "${!MOUNT_SUFFIX[@]}"; do
        local src="${sysroot}${MOUNT_SUFFIX[$target]}"
        [[ -d "$src" ]] || { echo "ERROR: missing source: $src" >&2; exit 3; }
        mkdir -p "$target"
        if is_mounted "$target"; then
            echo "ALREADY MOUNTED  $target  (source: $(current_mount_source "$target"))"
        else
            mount --bind "$src" "$target"
            echo "MOUNTED          $target  <- $src"
        fi
    done
}

teardown() {
    [[ "$EUID" -eq 0 ]] || { echo "ERROR: --umount requires root" >&2; exit 2; }
    for target in "${!MOUNT_SUFFIX[@]}"; do
        if is_mounted "$target"; then
            umount "$target" && echo "umounted $target"
        else
            echo "(not mounted) $target"
        fi
    done
}

check() {
    local expected_sysroot="${1:-}"
    local all_ok=1
    for target in "${!MOUNT_SUFFIX[@]}"; do
        if is_mounted "$target"; then
            local src
            src="$(current_mount_source "$target")"
            local expected="${expected_sysroot}${MOUNT_SUFFIX[$target]}"
            local expected_drvfs=""
            if [[ "$expected" == /mnt/c/* ]]; then
                expected_drvfs="C:\\[${expected#/mnt/c}]"
            fi
            if [[ -n "$expected_sysroot" && "$src" != "$expected" && "$src" != "$expected_drvfs" ]]; then
                echo "WRONG $target  <- $src  (expected: $expected)"
                all_ok=0
            else
                echo "OK    $target  <- $src"
            fi
        else
            echo "MISS  $target"
            all_ok=0
        fi
    done
    [[ "$all_ok" -eq 1 ]] || exit 1
}

action="${1:-setup}"
case "$action" in
    --help|-h) usage ;;
    --check) check "${2:-}" ;;
    --umount) teardown ;;
    --switch)
        new_sysroot="${2:?--switch requires a sysroot path}"
        teardown 2>/dev/null || true
        setup "$new_sysroot"
        ;;
    /*)
        # First arg is a sysroot path
        setup "$1"
        ;;
    setup|"")
        setup "$PLE_DEFAULT"
        ;;
    *)
        echo "Unknown option: $action" >&2
        usage
        ;;
esac
