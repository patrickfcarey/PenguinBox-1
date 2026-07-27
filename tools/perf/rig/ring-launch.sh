#!/usr/bin/env bash
# Robust detached launcher for the ring binary — survives ssh disconnect.
pkill -f "research/build/[q]emu" 2>/dev/null
sleep 2
g=$(pgrep -u 1000 -x gnome-shell | head -1)
DISP=$(tr '\0' '\n' < /proc/$g/environ | sed -n 's/^DISPLAY=//p' | head -1)
XAUTH=$(tr '\0' '\n' < /proc/$g/environ | sed -n 's/^XAUTHORITY=//p' | head -1)
ROM="$HOME/roms/xbox/Steel Battalion (USA).xiso.iso"
export DISPLAY=$DISP XAUTHORITY=$XAUTH XDG_RUNTIME_DIR=/run/user/1000
export SDL_VIDEODRIVER=x11 __NV_PRIME_RENDER_OFFLOAD=1 __GLX_VENDOR_LIBRARY_NAME=nvidia
# RenderDoc runtime (portable, no sudo): lets the in-app capture API dlopen it.
export LD_LIBRARY_PATH="$HOME/renderdoc/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# Watch-file state report (M-5/gate-audit hazard: stale files silently
# diverge sessions from the verified baseline — make state visible).
for wf in /tmp/predl-off /tmp/narrowfence-on /tmp/radar-skip-n; do
    if [ -e "$wf" ]; then echo "watch-file PRESENT: $wf ($(cat "$wf" 2>/dev/null))"; fi
done
rm -f /tmp/xemu-mon.sock
setsid "$HOME/xemu_vr-research/build/qemu-system-i386" \
    -config_path "$HOME/xemu-assets/sb-floor-s1.toml" \
    -dvd_path "$ROM" \
    -monitor unix:/tmp/xemu-mon.sock,server,nowait \
    > "$HOME/ring-smoke.log" 2>&1 < /dev/null &
echo "launched pid $!"
