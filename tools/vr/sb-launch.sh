#!/usr/bin/env bash
# Steel Battalion flat verify launcher (PenguinBox steel-battalion branch)
# - VK on NVIDIA + GL forced to NVIDIA via PRIME (H-6 / AGENTS §5: the VULKAN
#   renderer's display path needs VK and GL on the same device)
# - XWayland (SDL x11) so xwd screenshots + future key injection work
# - vr.enable=false in the toml: flat run, no XR runtime involved
set -u
RUN="${1:-run}"
LOG="$HOME/sb-verify-$RUN.log"

# Auto-detect the live session's display env from gnome-shell — works for
# both Wayland+XWayland (DISPLAY=:0, mutter auth) and native Xorg sessions
# (DISPLAY=:1, gdm Xauthority; seen after the 2026-07-24 driver update).
g=$(pgrep -u "$(id -u)" -x gnome-shell | head -1)
if [ -n "$g" ]; then
    d=$(tr '\0' '\n' < /proc/$g/environ | sed -n 's/^DISPLAY=//p' | head -1)
    a=$(tr '\0' '\n' < /proc/$g/environ | sed -n 's/^XAUTHORITY=//p' | head -1)
    [ -n "$d" ] && export DISPLAY="$d"
    [ -n "$a" ] && export XAUTHORITY="$a"
fi
export DISPLAY="${DISPLAY:-:0}"
if [ -z "${XAUTHORITY:-}" ]; then
    export XAUTHORITY=$(ls /run/user/1000/.mutter-Xwaylandauth.* 2>/dev/null | head -1)
fi
export XDG_RUNTIME_DIR=/run/user/1000
export SDL_VIDEODRIVER=x11
export __NV_PRIME_RENDER_OFFLOAD=1
export __GLX_VENDOR_LIBRARY_NAME=nvidia

nohup "$HOME/xemu_vr/build/qemu-system-i386" \
    -config_path "$HOME/xemu-assets/sb-verify.toml" \
    > "$LOG" 2>&1 < /dev/null &
echo "launched pid $! log $LOG"
