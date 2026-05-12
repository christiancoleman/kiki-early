#!/usr/bin/env bash
# nuke_emulation.sh — Aggressively kills all processes related to the Spike 3 emulation environment.
# Use this when run_game.sh leaves orphaned background processes or sockets behind.

echo "Nuking all emulation processes..."

# 1. Kill bash wrapper scripts
pkill -f "run_game.sh" 2>/dev/null || true

# 2. Kill Python subscribers and viewers
pkill -f "frame_viewer.py" 2>/dev/null || true
pkill -f "audio_subscriber.py" 2>/dev/null || true

# 3. Kill ffplay audio pipelines
pkill -f "ffplay.*s16le" 2>/dev/null || true

# 4. Kill any qemu-aarch64 processes running our binaries or using our shim
pkill -f "qemu-aarch64.*LD_PRELOAD.*spi_shim.so" 2>/dev/null || true
pkill -f "qemu-aarch64.*boot_display" 2>/dev/null || true
pkill -f "qemu-aarch64.*game" 2>/dev/null || true

# 5. Just to be sure, kill the raw binary names in case they somehow ran natively
pkill -x "game" 2>/dev/null || true
pkill -x "boot_display" 2>/dev/null || true

# Give processes a moment to cleanly exit
sleep 0.5

echo "Sending SIGKILL to any stubborn processes..."

# Follow up with SIGKILL (-9) for anything that refused to die
pkill -9 -f "run_game.sh" 2>/dev/null || true
pkill -9 -f "frame_viewer.py" 2>/dev/null || true
pkill -9 -f "audio_subscriber.py" 2>/dev/null || true
pkill -9 -f "ffplay.*s16le" 2>/dev/null || true
pkill -9 -f "qemu-aarch64.*spi_shim.so" 2>/dev/null || true
pkill -9 -f "qemu-aarch64.*boot_display" 2>/dev/null || true
pkill -9 -f "qemu-aarch64.*game" 2>/dev/null || true
pkill -9 -x "game" 2>/dev/null || true

# 6. Clean up lingering IPC sockets
echo "Cleaning up lingering sockets in /tmp..."
rm -f /tmp/spike3_frame.sock
rm -f /tmp/spike3_audio.sock
rm -f /tmp/spike3_bridge.sock

echo "Emulation environment nuked."
