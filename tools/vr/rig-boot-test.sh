#!/usr/bin/env bash
# xemu-VR rig launcher — canonical copy (deploy to the rig as
# ~/xemu-assets/boot-test.sh and keep the two in sync; pcsx2-VR's rig-diff
# discipline exists because deployed-only edits WILL drift).
#
# Usage: boot-test.sh [rom]        default ROM: Operation Flashpoint: Elite
#   XEMU_VR=1 boot-test.sh         enable VR (sibling convention: MUPEN_VR=1)
#   XEMU_VR_RUNTIME=system         use the system OpenXR runtime (Monado)
#                                  instead of auto-selecting WiVRn.
#
# VR on/off is managed HERE, in the toml's [vr] section, because xemu's
# auto-save rewrites the config with defaults omitted — a hand-added
# "enable = true" would survive, but there is no [vr] section to edit until
# something writes one. This launcher ensures the section exists and sets
# it to match XEMU_VR every run.
set -euo pipefail

# GPU forcing — MANDATORY on the hybrid (NVIDIA dGPU + AMD iGPU) rig, and
# doubly so with VR on: the XR runtime pins Vulkan to the NVIDIA GPU, and
# xemu's VK->GL mirror interop requires GL on the SAME device.
export __NV_PRIME_RENDER_OFFLOAD=1 __GLX_VENDOR_LIBRARY_NAME=nvidia

A="$HOME/xemu-assets"
# XEMU_TOML overrides the config (e.g. a per-title VR profile); the [vr]
# enable management below applies to whichever toml is active.
TOML="${XEMU_TOML:-$A/xemu-test.toml}"
for f in "$A/mcpx_1.0.bin" "$A/xbox-4627_debug.bin" "$TOML"; do
    [ -f "$f" ] || { echo "MISSING: $f" >&2; exit 1; }
done

# --- [vr] section management -------------------------------------------
want_vr="${XEMU_VR:-0}"
vr_bool=false; [ "$want_vr" = "1" ] && vr_bool=true
# Ensure BOTH the [vr] section AND the enable key exist. xemu strips
# default-valued keys (enable=false) on exit, so if the section survives but
# the key was dropped, a bare sed-replace no-ops and silently leaves VR off —
# which kills a hunt (no VR session -> no compositor frame -> F9 never dumps).
# Insert the key if it's missing rather than only replacing it.
grep -q '^\[vr\]' "$TOML" || printf '\n[vr]\n' >> "$TOML"
if grep -q '^enable = ' "$TOML"; then
    sed -i "s/^enable = .*/enable = $vr_bool/" "$TOML"
else
    sed -i "/^\[vr\]/a enable = $vr_bool" "$TOML"
fi
echo "xemu-vr launcher: VR $( [ "$vr_bool" = true ] && echo ENABLED || echo disabled ) (XEMU_VR=$want_vr)"

# --- OpenXR runtime selection (review F3) ------------------------------
# System default is Monado (no HMD on this rig): an enabled-VR run would
# silently fall back to flat. For the Quest 3, point the loader at WiVRn.
if [ "$vr_bool" = true ] && [ "${XEMU_VR_RUNTIME:-wivrn}" = "wivrn" ]; then
    json=$(ls "$HOME"/.local/share/flatpak/app/io.github.wivrn.wivrn/*/*/*/files/share/openxr/1/openxr_wivrn.json 2>/dev/null | head -1)
    if [ -n "$json" ]; then
        export XR_RUNTIME_JSON="$json"
        rtdir="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
        if [ ! -S "$rtdir/wivrn/comp_ipc" ] && [ ! -S "$rtdir/monado_comp_ipc" ]; then
            echo "WARNING: no WiVRn compositor socket found under $rtdir —" >&2
            echo "         start the WiVRn server and connect the headset" >&2
            echo "         first, or xemu will run flat." >&2
        fi
    else
        echo "WARNING: WiVRn manifest not found; using system runtime." >&2
    fi
fi

rom="${1:-$HOME/roms/xbox/Operation Flashpoint - Elite (USA, Europe) (En,Fr,De,It).xiso.iso}"
exec "$HOME/xemu_vr/build/qemu-system-i386" \
    -config_path "$TOML" -dvd_path "$rom"
