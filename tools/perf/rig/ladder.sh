#!/usr/bin/env bash
# Toggle-ladder validation: 4 legs, each a fresh boot of the merged-tiers
# binary into the probe scene. Counters are the boot-robust signal;
# cross-boot mspf is noisy (documented) and read with that caveat.
BIN="$HOME/xemu_vr-research/build/qemu-system-i386"
ROM="$HOME/roms/xbox/Steel Battalion (USA).xiso.iso"
g=$(pgrep -u 1000 -x gnome-shell | head -1)
export DISPLAY=$(tr '\0' '\n' < /proc/$g/environ | sed -n 's/^DISPLAY=//p' | head -1)
export XAUTHORITY=$(tr '\0' '\n' < /proc/$g/environ | sed -n 's/^XAUTHORITY=//p' | head -1)
export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus
export XDG_RUNTIME_DIR=/run/user/1000 SDL_VIDEODRIVER=x11
export __NV_PRIME_RENDER_OFFLOAD=1 __GLX_VENDOR_LIBRARY_NAME=nvidia
touch /tmp/predl-off   # keep predl out of the picture
rm -f /tmp/narrowfence-on

SNAP="vm-20260725222138"
leg() { # $1 label, rest: env assignments
    local label=$1; shift
    pkill -f "research/build/[q]emu" 2>/dev/null; pkill -f "nc -U" 2>/dev/null; sleep 3
    rm -f /tmp/xemu-mon.sock
    env "$@" setsid "$BIN" -config_path "$HOME/xemu-assets/sb-floor-s1.toml" \
        -dvd_path "$ROM" -monitor unix:/tmp/xemu-mon.sock,server,nowait \
        > "$HOME/ladder-$label.log" 2>&1 < /dev/null &
    sleep 45
    echo "loadvm $SNAP" | timeout 15 nc -q 8 -U /tmp/xemu-mon.sock >/dev/null 2>&1
    sleep 30
    echo "== $label =="
    pgrep -f "research/build/[q]emu" >/dev/null && echo "alive" || echo "DIED"
    grep -icE "VUID|VALIDATION|VK_ERROR|Assertion|SIGSEGV" "$HOME/ladder-$label.log" | sed 's/^/vk_errors=/'
    tail -6 "$HOME/ladder-$label.log" | grep -oE "mspf=[0-9]+|TEX_HASH=[0-9]+|TEX_HASH_KB=[0-9]+|TEX_HASH_SKIPPED=[0-9]+|FINISH_STALLED=[0-9]+|QUERY_ASYNC_RESOLVE=[0-9]+|SURF_CPU_READ=[0-9]+|FINISH_SURFACE_DOWN=[0-9]+" | paste - - - - - - - - 2>/dev/null | tail -3
}

leg L0-stock       XEMU_NO_EXACT_DIRTY=1
leg L1-exactdirty  NOOP=1
leg L2-queries     XEMU_ASYNC_QUERIES=1
leg L3-all         XEMU_ASYNC_QUERIES=1 XEMU_TLB_FASTPATH=1
pkill -f "research/build/[q]emu" 2>/dev/null
rm -f /tmp/predl-off
echo "LADDER COMPLETE"
