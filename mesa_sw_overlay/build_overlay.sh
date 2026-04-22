#!/bin/bash
# Rebuilds the Mesa software-raster overlay from scratch.
# Idempotent — re-run safely. No sudo, no foreign-arch dpkg. Needs: curl, python3, dpkg-deb,
# aarch64-linux-gnu-gcc (for the probes; optional if just rebuilding the overlay).
#
# End state: pkg/usr/lib/aarch64-linux-gnu/{libgallium-25.0.7-2.so, dri/swrast_dri.so, ...}
# plus all transitive deps, with driver version strings patched to "24.0.9" so the
# sysroot's Mesa 24.0.9 libEGL accepts them.
#
# JOURNAL Step 55 has the full story.
set -euo pipefail
cd "$(dirname "$0")"

mkdir -p debs debs_2408 pkg

# ---- Debian trixie 25.0.7 (mesa core + libgallium) -------------------------
# The 25.0.7 DRI drivers aren't used (sysroot libEGL rejects them), but we keep
# libgallium-25.0.7-2.so and the full transitive closure installed — the 24.0.8
# driver pulls in the same deps (libLLVM-17 instead of -19) and any missing one
# from the 25.0.7 closure would break loading.
BASE=https://deb.debian.org/debian/pool/main/m
TRIXIE_25_0_7=(
    "$BASE/mesa/libgl1-mesa-dri_25.0.7-2_arm64.deb"
    "$BASE/mesa/libegl-mesa0_25.0.7-2_arm64.deb"
    "$BASE/mesa/libgbm1_25.0.7-2_arm64.deb"
    "$BASE/mesa/mesa-libgallium_25.0.7-2_arm64.deb"
)

# ---- Transitive deps (Debian trixie stable) --------------------------------
# Minor version drift here is safe — these are leaf libs with ultra-stable ABIs.
TRIXIE_DEPS=(
    "$BASE/llvm-toolchain-19/libllvm19_19.1.7-3+b1_arm64.deb"
    "$BASE/llvm-toolchain-17/libllvm17t64_17.0.6-22+b2_arm64.deb"
    "$BASE/libdrm/libdrm-amdgpu1_2.4.124-2_arm64.deb"
    "$BASE/libdrm/libdrm-intel1_2.4.124-2_arm64.deb"
    "$BASE/libdrm/libdrm-nouveau2_2.4.124-2_arm64.deb"
    "$BASE/libdrm/libdrm-radeon1_2.4.124-2_arm64.deb"
    "$BASE/elfutils/libelf1t64_0.193-3~bpo13+1_arm64.deb"
    "$BASE/lm-sensors/libsensors5_3.6.2-2_arm64.deb"
    "$BASE/wayland/libwayland-client0_1.23.1-3_arm64.deb"
    "$BASE/wayland/libwayland-server0_1.23.1-3_arm64.deb"
    "$BASE/libx11/libx11-xcb1_1.8.12-1_arm64.deb"
    "$BASE/libxau/libxau6_1.0.11-1_arm64.deb"
    "$BASE/libxcb/libxcb1_1.17.0-2+b1_arm64.deb"
    "$BASE/libxcb/libxcb-dri3-0_1.17.0-2+b1_arm64.deb"
    "$BASE/libxcb/libxcb-present0_1.17.0-2+b1_arm64.deb"
    "$BASE/libxcb/libxcb-randr0_1.17.0-2+b1_arm64.deb"
    "$BASE/libxcb/libxcb-shm0_1.17.0-2+b1_arm64.deb"
    "$BASE/libxcb/libxcb-sync1_1.17.0-2+b1_arm64.deb"
    "$BASE/libxcb/libxcb-xfixes0_1.17.0-2+b1_arm64.deb"
    "$BASE/libxdmcp/libxdmcp6_1.1.5-1_arm64.deb"
    "$BASE/libxshmfence/libxshmfence1_1.3.3-1_arm64.deb"
    "$BASE/libzstd/libzstd1_1.5.7+dfsg-1_arm64.deb"
    "$BASE/libedit/libedit2_3.1-20250104-1_arm64.deb"
    "$BASE/ncurses/libtinfo6_6.5+20250216-2_arm64.deb"
    "$BASE/libxml2/libxml2_2.12.7+dfsg+really2.9.14-2.1+deb13u2_arm64.deb"
    "$BASE/libbsd/libbsd0_0.12.2-2_arm64.deb"
    "$BASE/libmd/libmd0_1.1.0-2+b1_arm64.deb"
    "$BASE/icu/libicu76_76.1-4_arm64.deb"
    "$BASE/z3/libz3-4_4.13.3-1_arm64.deb"
    "$BASE/gcc-14/libstdc++6_14.2.0-19_arm64.deb"
)

fetch() {
    local url="$1"  dest="$2"  fname
    fname="$(basename "$url")"
    if [ -f "$dest/$fname" ]; then return 0; fi
    echo "PULL $fname"
    curl -fsSL -o "$dest/$fname" "$url"
}

echo "=== [1/4] Fetching Debian trixie 25.0.7 + deps (arm64) ==="
for u in "${TRIXIE_25_0_7[@]}" "${TRIXIE_DEPS[@]}"; do fetch "$u" debs; done

# ---- Debian snapshot 24.0.8-1 (Mesa core for ABI match with sysroot 24.0.9) -
# Fetched by SHA-1 hash from snapshot.debian.org/mr API. Hashes are permanent.
echo "=== [2/4] Fetching snapshot.debian.org Mesa 24.0.8-1 ==="
SNAPSHOT=https://snapshot.debian.org/file
declare -A SNAP_FILES=(
    ["libgl1-mesa-dri_24.0.8-1_arm64.deb"]="9ede7478cba671aad592a768a84ad6eed4f7c5dd"
    ["libegl-mesa0_24.0.8-1_arm64.deb"]=""
    ["libgbm1_24.0.8-1_arm64.deb"]=""
)
# Re-resolve hashes if missing (snapshot.debian.org mr API).
for pkg in "${!SNAP_FILES[@]}"; do
    if [ -f "debs_2408/$pkg" ]; then continue; fi
    hash="${SNAP_FILES[$pkg]}"
    if [ -z "$hash" ]; then
        # parse pkg name to ask API
        basename_pkg="${pkg%%_*}"
        api_url="https://snapshot.debian.org/mr/binary/${basename_pkg}/24.0.8-1/binfiles"
        hash=$(curl -sSL "$api_url" | python3 -c 'import sys,json; d=json.load(sys.stdin); [print(r["hash"]) for r in d.get("result",[]) if r.get("architecture")=="arm64"]' 2>/dev/null || true)
    fi
    if [ -z "$hash" ]; then echo "FAIL: no arm64 hash for $pkg"; continue; fi
    echo "PULL $pkg (hash=$hash)"
    curl -fsSL -o "debs_2408/$pkg" "$SNAPSHOT/$hash"
done

echo "=== [3/4] Extract everything into pkg/ (24.0.8 overlays 25.0.7 for drivers) ==="
for d in debs/*.deb; do dpkg-deb -x "$d" pkg/; done
for d in debs_2408/*.deb; do dpkg-deb -x "$d" pkg/; done

echo "=== [4/4] Patch driver version strings 24.0.8-1 → 24.0.9 ==="
python3 <<'PY'
import glob, os
patched_total = 0
for path in sorted(glob.glob("pkg/usr/lib/aarch64-linux-gnu/dri/*_dri.so")):
    # skip symlinks/hardlinks if any — resolve to real file
    if os.path.islink(path): continue
    with open(path, "rb") as f: data = bytearray(f.read())
    n = data.count(b"24.0.8-1")
    if n == 0: continue
    data = bytearray(data.replace(b"24.0.8-1", b"24.0.9\x00\x00"))
    with open(path, "wb") as f: f.write(data)
    print(f"  patched {n:>2}x in {os.path.basename(path)}")
    patched_total += n
print(f"total patches: {patched_total}")
PY

echo ""
echo "=== Done. Overlay ready at pkg/ ==="
du -sh pkg/
echo ""
echo "Run the probes:"
echo "  aarch64-linux-gnu-gcc -o gles_probe2 gles_probe2.c -ldl"
echo "  SYSROOT=/mnt/c/Workspace/pokemon-le-0.81.0"
echo "  OVERLAY=\$PWD/pkg/usr/lib/aarch64-linux-gnu"
echo "  qemu-aarch64 -L \$SYSROOT \\"
echo "      -E LD_LIBRARY_PATH=\$OVERLAY:\$SYSROOT/usr/lib64:\$SYSROOT/usr/lib:\$SYSROOT/lib64:\$SYSROOT/lib \\"
echo "      -E LIBGL_DRIVERS_PATH=\$OVERLAY/dri \\"
echo "      -E MESA_LOADER_DRIVER_OVERRIDE=swrast \\"
echo "      -E LIBGL_ALWAYS_SOFTWARE=1 \\"
echo "      -E EGL_PLATFORM=surfaceless \\"
echo "      ./gles_probe2"
