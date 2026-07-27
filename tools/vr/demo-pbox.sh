#!/usr/bin/env bash
# demo-pbox.sh — PenguinBox (xemu-VR) demo launcher.
#
# Canonical copy: tools/vr/demo-pbox.sh — deploy to the rig HOME dir as
# ~/demo-pbox.sh (run it from ~ on the rig, like the sibling repos' demo
# launchers) and keep the two in sync (rig-diff discipline; deploy-to-rig.sh
# syncs it automatically).
#
# Same base as boot-test.sh (GPU forcing, WiVRn runtime, [vr] management) plus
# demo instrumentation:
#   * MONADO_DEBUG=1        — verbose XR runtime logging (teed to a logfile).
#   * MangoHud GL overlay   — GPU utilisation + the GPU's own name, on the
#                             desktop mirror the audience watches.
#
# The HUD rides xemu's OpenGL mirror window (xemu never vkQueuePresents to a
# window, so MangoHud's *Vulkan* layer has nothing to hook — we LD_PRELOAD the
# OpenGL hook instead). It does NOT appear in the headset: the headset image is
# copied from the offscreen display VkImage before any window present.
#
# Usage:  demo-pbox.sh [rom]              (VR on by default — it's a demo)
#         XEMU_VR=0 demo-pbox.sh [rom]    (flat + HUD; used for HUD smoke test)
#         XEMU_VR_RUNTIME=system ...      (system runtime instead of WiVRn)
set -euo pipefail

# ── GPU forcing (mandatory on the hybrid RTX 4050 + AMD 680M rig) ──────────
# XR pins Vulkan to the NVIDIA dGPU; the VK→GL mirror interop needs GL on the
# SAME device, and MangoHud must read stats from the GPU actually rendering.
export __NV_PRIME_RENDER_OFFLOAD=1 __GLX_VENDOR_LIBRARY_NAME=nvidia

A="$HOME/xemu-assets"
TOML="$A/xemu-test.toml"
BIN="$HOME/xemu_vr/build/qemu-system-i386"
for f in "$A/mcpx_1.0.bin" "$A/xbox-4627_debug.bin" "$TOML" "$BIN"; do
    [ -e "$f" ] || { echo "MISSING: $f" >&2; exit 1; }
done

# ── GPU name, pulled from the machine (never hard-coded) ───────────────────
# MangoHud's `gpu_name` auto-detects and shows it in the HUD; we also echo it
# here for terminal confirmation.
GPU_NAME="$(nvidia-smi --query-gpu=name --format=csv,noheader 2>/dev/null | head -1)"
[ -n "${GPU_NAME:-}" ] || GPU_NAME="$(lspci 2>/dev/null | grep -iE 'vga|3d controller' | grep -i nvidia | sed 's/.*: //' | head -1)"
echo "demo-pbox: rendering GPU detected as: ${GPU_NAME:-unknown}"

# NOTE: MangoHud's `pci_dev` filter does NOT match the NVIDIA card here (it
# reads NVIDIA via NVML, not the sysfs PCI path pci_dev keys on) — setting it
# drops the GPU stats entirely. So we show BOTH GPUs; the `gpu_name` line (the
# live GL renderer string) makes it unambiguous that the 4050 is doing the
# rendering. To isolate one GPU on a future MangoHud, try `gpu_list=<index>`.

# ── MangoHud (OpenGL overlay via LD_PRELOAD) ───────────────────────────────
MH_DIR="$HOME/.local/lib/mangohud"
[ -d "$MH_DIR" ] || MH_DIR="/usr/\$LIB/mangohud"   # fallback if system-installed
if [ -e "$MH_DIR/libMangoHud_shim.so" ]; then
    export MANGOHUD=1
    # Preload the SHIM (not the hooks directly): xemu/SDL resolve GL via
    # dlopen/dlsym, which plain symbol-interposition misses — the shim
    # intercepts dlsym and pulls in libMangoHud.so + libMangoHud_opengl.so
    # (its siblings in the same dir) itself. This is what the `mangohud`
    # wrapper does.
    export LD_PRELOAD="${MH_DIR}/libMangoHud_shim.so${LD_PRELOAD:+:$LD_PRELOAD}"
    # HUD content: GPU name + utilisation are the stars; comma-separated, no
    # commas inside values. Tune here.
    export MANGOHUD_CONFIG="\
gpu_name,\
gpu_stats,\
gpu_load_change,\
gpu_temp,\
gpu_power,\
gpu_core_clock,\
gpu_mem_clock,\
vram,\
fps,\
frame_timing=1,\
cpu_stats,\
cpu_temp,\
ram,\
position=top-left,\
font_size=26,\
background_alpha=0.4,\
round_corners=8,\
gpu_color=76B900,\
engine_short_names,\
custom_text=PenguinBox VR - xemu on ${GPU_NAME:-GPU}"
    echo "demo-pbox: MangoHud overlay ENABLED (GPU name + utilisation)."
else
    echo "demo-pbox: WARNING — MangoHud not found at $MH_DIR; running without HUD." >&2
fi

# ── VR on by default (XEMU_VR=0 to force flat, e.g. HUD smoke test) ─────────
want_vr="${XEMU_VR:-1}"
vr_bool=false; [ "$want_vr" = "1" ] && vr_bool=true
# Robust: ensure the [vr] section AND the enable key exist. xemu strips
# default-valued keys (enable=false) on exit, so a plain sed-replace would
# no-op on a stripped key and silently leave VR off — insert if missing.
grep -q '^\[vr\]' "$TOML" || printf '\n[vr]\n' >> "$TOML"
if grep -q '^enable = ' "$TOML"; then
    sed -i "s/^enable = .*/enable = $vr_bool/" "$TOML"
else
    sed -i "/^\[vr\]/a enable = $vr_bool" "$TOML"
fi
echo "demo-pbox: VR $( [ "$vr_bool" = true ] && echo ENABLED || echo disabled )."

# ── OpenXR runtime selection + Monado debug ────────────────────────────────
export MONADO_DEBUG=1
if [ "$vr_bool" = true ] && [ "${XEMU_VR_RUNTIME:-wivrn}" = "wivrn" ]; then
    json=$(ls "$HOME"/.local/share/flatpak/app/io.github.wivrn.wivrn/*/*/*/files/share/openxr/1/openxr_wivrn.json 2>/dev/null | head -1)
    if [ -n "$json" ]; then
        export XR_RUNTIME_JSON="$json"
        rtdir="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
        if [ ! -S "$rtdir/wivrn/comp_ipc" ] && [ ! -S "$rtdir/monado_comp_ipc" ]; then
            echo "WARNING: no WiVRn compositor socket under $rtdir — start the" >&2
            echo "         WiVRn server and connect the headset, or it runs flat." >&2
        fi
    else
        echo "WARNING: WiVRn manifest not found; using system runtime." >&2
    fi
fi

rom="${1:-$HOME/roms/xbox/Operation Flashpoint - Elite (USA, Europe) (En,Fr,De,It).xiso.iso}"
LOG="$HOME/xemu-demo.log"
echo "demo-pbox: launching $(basename "$rom")"
echo "demo-pbox: MONADO_DEBUG output + emulator log tee'd to $LOG"

# tee (not exec) so MONADO_DEBUG log is captured for post-demo review.
"$BIN" -config_path "$TOML" -dvd_path "$rom" 2>&1 | tee "$LOG"
