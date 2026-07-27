#!/usr/bin/env bash
# rig_flicker_ab.sh — A/B flicker capture runner (RUNS ON THE RIG, detached).
#
# Two legs against the SAME save state so animated content is statistically
# identical and any metric delta is the flicker a config adds:
#   leg 1  baseline        (no async queries)
#   leg 2  async-queries   (XEMU_ASYNC_QUERIES=1)   <- known-flickery
# Each leg: launch OUR OWN instance, restore the mission snapshot, settle,
# capture a burst (via rig_flicker_capture.sh), kill OUR instance.
#
# SAFETY (hard): this script manages only instances IT launches. If any xemu
# instance — or pcsx2 / mupen64plus — is already running when it starts, it
# ABORTS without touching it. Launch it only when the rig is free.
#
# Self-detaching: launch via `setsid bash rig_flicker_ab.sh ...` (the local
# orchestrator flicker_run.sh does this) so it survives ssh drops. It writes
# a DONE marker at the end for the poller.
#
# Usage: rig_flicker_ab.sh <run_root> [nframes] [settle_s] [snapshot]
set -euo pipefail

RUN_ROOT="${1:-$HOME/flicker-tools/runs/$(date +%Y%m%d-%H%M%S)}"
NFRAMES="${2:-60}"
SETTLE="${3:-5}"
SNAP="${4:-vm-20260725222138}"

BIN="$HOME/xemu_vr-research/build/qemu-system-i386"
ROM="$HOME/roms/xbox/Steel Battalion (USA).xiso.iso"
CFG="$HOME/xemu-assets/sb-floor-s1.toml"
CAP="$HOME/flicker-tools/rig_flicker_capture.sh"
MON=/tmp/xemu-mon.sock

mkdir -p "$RUN_ROOT"
exec > >(tee -a "$RUN_ROOT/runner.log") 2>&1
echo "=== rig_flicker_ab $(date -Iseconds)  root=$RUN_ROOT nframes=$NFRAMES ==="

# --- HARD GUARD: never disturb an existing session -------------------------
if pgrep -f "research/build/[q]emu" >/dev/null 2>&1; then
    echo "ABORT: an xemu instance is already running — refusing to launch/kill."
    echo "ABORTED" > "$RUN_ROOT/DONE"; exit 3
fi
if pgrep -x pcsx2-qt >/dev/null 2>&1 || pgrep -x mupen64plus >/dev/null 2>&1; then
    echo "ABORT: another emulator (pcsx2/mupen) is live."
    echo "ABORTED" > "$RUN_ROOT/DONE"; exit 3
fi

# --- live X session env (same pattern as ring-launch.sh) -------------------
g=$(pgrep -u "$(id -u)" -x gnome-shell | head -1)
export DISPLAY=$(tr '\0' '\n' < /proc/$g/environ | sed -n 's/^DISPLAY=//p' | head -1)
export XAUTHORITY=$(tr '\0' '\n' < /proc/$g/environ | sed -n 's/^XAUTHORITY=//p' | head -1)
export XDG_RUNTIME_DIR=/run/user/$(id -u)
export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/$(id -u)/bus
export SDL_VIDEODRIVER=x11 __NV_PRIME_RENDER_OFFLOAD=1 __GLX_VENDOR_LIBRARY_NAME=nvidia

leg() {  # $1 label ; rest: env assignments for this leg
    local label="$1"; shift
    local dir="$RUN_ROOT/$label"
    echo "--- leg $label : env[$*] ---"
    pkill -f "research/build/[q]emu" 2>/dev/null || true
    pkill -f "nc -U $MON" 2>/dev/null || true
    sleep 3; rm -f "$MON"
    env "$@" setsid "$BIN" -config_path "$CFG" -dvd_path "$ROM" \
        -monitor unix:"$MON",server,nowait \
        > "$RUN_ROOT/$label.emu.log" 2>&1 < /dev/null &
    sleep 45
    echo "loadvm $SNAP" | timeout 15 nc -q 8 -U "$MON" >/dev/null 2>&1 || echo "WARN: loadvm failed"
    sleep "$SETTLE"
    if ! pgrep -f "research/build/[q]emu" >/dev/null; then
        echo "WARN: $label instance died before capture"; return 1
    fi
    LEG_ENV="$*" FDPY="$HOME/flicker-tools/flicker_detect.py" \
        bash "$CAP" "$dir" "$NFRAMES" 0 "$label" || echo "WARN: capture failed for $label"
    pkill -f "research/build/[q]emu" 2>/dev/null || true
    sleep 3; rm -f "$MON"
}

leg baseline                              # no async queries
leg async-queries XEMU_ASYNC_QUERIES=1    # known-flickery positive control

pkill -f "research/build/[q]emu" 2>/dev/null || true
echo "OK $(date -Iseconds)" > "$RUN_ROOT/DONE"
echo "=== A/B COMPLETE : $RUN_ROOT (baseline, async-queries) ==="
