#!/usr/bin/env bash
# rig_flicker_capture.sh — capture one flicker burst from the xemu window.
#
# RUNS ON THE RIG. Pure, read-only screen capture: it never launches, kills,
# focuses, or talks to any emulator. Safe to point at a live user session
# (it only reads root-window pixels via xwd). The A/B runner
# (rig_flicker_ab.sh) is what owns launching/killing its own instances.
#
# Pipeline: derive the live session's X display from gnome-shell (matches
# ~/ring-launch.sh), find the on-screen xemu client window geometry via
# xwininfo, rapid-fire xwd -root dumps into tmpfs (/dev/shm, ~47ms/frame on
# this rig => ~21fps), then convert+crop to PNG with flicker_detect.py.
#
# Usage: rig_flicker_capture.sh <run_dir> [nframes] [interval_ms] [label]
#   run_dir      output dir for PNGs + meta.json (created)
#   nframes      default 40
#   interval_ms  extra sleep between grabs, default 0 (grab as fast as xwd allows)
#   label        metadata tag (e.g. baseline / async-queries)
set -euo pipefail

RUN_DIR="${1:?usage: rig_flicker_capture.sh <run_dir> [nframes] [interval_ms] [label]}"
NFRAMES="${2:-40}"
INTERVAL_MS="${3:-0}"
LABEL="${4:-capture}"
FDPY="${FDPY:-$HOME/flicker-tools/flicker_detect.py}"

# --- derive the live X session env (same pattern as ring-launch.sh) ---
g=$(pgrep -u "$(id -u)" -x gnome-shell | head -1)
if [ -z "$g" ]; then echo "ERR: no gnome-shell (no live session?)" >&2; exit 2; fi
export DISPLAY=$(tr '\0' '\n' < /proc/$g/environ | sed -n 's/^DISPLAY=//p' | head -1)
export XAUTHORITY=$(tr '\0' '\n' < /proc/$g/environ | sed -n 's/^XAUTHORITY=//p' | head -1)
export XDG_RUNTIME_DIR=/run/user/$(id -u)
[ -z "${DISPLAY:-}" ] && export DISPLAY=:0

# --- find the on-screen xemu client window (largest, not the SDL offscreen) ---
if [ -n "${GEOM:-}" ]; then
    read GX GY GW GH <<<"$GEOM"          # manual override: GEOM="x y w h"
else
    WID=$(xwininfo -root -tree 2>/dev/null \
          | grep 'app.xemu.xemu' | grep -vi 'Offscreen' \
          | awk '{ for(i=1;i<=NF;i++) if($i ~ /^[0-9]+x[0-9]+\+/){
                     split($i,a,"x"); ar=a[1]*a[2];
                     if(ar>best){best=ar; id=$1} } } END{print id}')
    if [ -z "$WID" ]; then echo "ERR: no on-screen xemu window found" >&2; exit 2; fi
    eval "$(xwininfo -id "$WID" 2>/dev/null | awk '
        /Absolute upper-left X/ {x=$NF}
        /Absolute upper-left Y/ {y=$NF}
        /^ *Width:/  {w=$NF}
        /^ *Height:/ {h=$NF}
        END{printf "GX=%d; GY=%d; GW=%d; GH=%d", x,y,w,h}')"
fi
echo "capture: window ${GW}x${GH}+${GX}+${GY} on $DISPLAY, $NFRAMES frames"

SHM="/dev/shm/flicker.$$"; mkdir -p "$SHM"
trap 'rm -rf "$SHM"' EXIT
mkdir -p "$RUN_DIR"
LOG="$RUN_DIR/capture.log"; : > "$LOG"

start=$(date +%s.%N)
i=0
while [ "$i" -lt "$NFRAMES" ]; do
    printf -v f "%s/f%04d.xwd" "$SHM" "$i"
    ts=$(date +%s.%N)
    xwd -root -silent -out "$f" 2>>"$LOG" || { echo "xwd failed at $i" >&2; break; }
    echo "$i $ts" >> "$LOG"
    i=$((i+1))
    if [ "$INTERVAL_MS" -gt 0 ]; then sleep "$(awk "BEGIN{print $INTERVAL_MS/1000}")"; fi
done
end=$(date +%s.%N)
got=$(ls "$SHM"/*.xwd 2>/dev/null | wc -l)
span=$(awk "BEGIN{printf \"%.2f\", $end-$start}")
echo "capture: grabbed $got frames in ${span}s (~$(awk "BEGIN{printf \"%.1f\", $got/($end-$start)}") fps)"

# --- convert + crop to PNG ---
python3 "$FDPY" xwd2png --src "$SHM" --dst "$RUN_DIR" \
    --x "$GX" --y "$GY" --w "$GW" --h "$GH"

# --- run metadata ---
cat > "$RUN_DIR/meta.json" <<EOF
{
  "label": "$LABEL",
  "captured": "$(date -Iseconds)",
  "nframes_requested": $NFRAMES,
  "nframes_captured": $got,
  "interval_ms": $INTERVAL_MS,
  "geometry": {"x": $GX, "y": $GY, "w": $GW, "h": $GH},
  "display": "$DISPLAY",
  "config": {"env": "${LEG_ENV:-}"}
}
EOF
echo "capture: wrote $RUN_DIR (label=$LABEL)"
