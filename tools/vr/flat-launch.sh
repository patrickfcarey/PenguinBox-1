#!/usr/bin/env bash
# Generic flat-verify launcher (canonical copy — deployed as ~/flat-launch.sh
# on the rig; keep in sync). Generalizes sb-launch.sh: any toml, any ROM.
#
#   flat-launch.sh <toml> <label> [rom]
#     toml   config path (e.g. ~/xemu-assets/flat-check.toml)
#     label  log tag -> ~/flat-<label>.log
#     rom    optional ISO path, passed as -dvd_path (overrides the toml's)
#
#   Env overrides (measurement workflows, runbooks/save-state-testing.md):
#     XEMU_BIN     alternate binary (e.g. the sb-graphics-research worktree
#                  build ~/xemu_vr-research/build/qemu-system-i386)
#     XEMU_LOADVM  snapshot name -> passed as -loadvm '<name>' (snapshots
#                  live INSIDE the qcow2 at the toml's hdd_path)
#
# - PRIME env forced: the VULKAN display path needs VK *and* GL on the
#   NVIDIA device even for flat runs (H-6 / AGENTS §5 — a GL-on-iGPU
#   launch segfaults at VK init, field-hit 2026-07-23).
# - SDL x11: the window is XWayland-backed so xwd screenshots and key
#   injection work.
# - VR must be off in the toml (vr.enable=false or section absent).
set -u
TOML="${1:?usage: flat-launch.sh <toml> <label> [rom]}"
RUN="${2:?usage: flat-launch.sh <toml> <label> [rom]}"
ROM="${3:-}"
LOG="$HOME/flat-$RUN.log"

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

BIN="${XEMU_BIN:-$HOME/xemu_vr/build/qemu-system-i386}"

args=(-config_path "$TOML")
if [ -n "$ROM" ]; then
    args+=(-dvd_path "$ROM")
fi
if [ -n "${XEMU_LOADVM:-}" ]; then
    args+=(-loadvm "$XEMU_LOADVM")
fi

nohup "$BIN" "${args[@]}" \
    > "$LOG" 2>&1 < /dev/null &
echo "launched pid $! log $LOG"
