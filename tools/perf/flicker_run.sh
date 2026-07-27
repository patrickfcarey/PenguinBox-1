#!/usr/bin/env bash
# flicker_run.sh — LOCAL orchestrator for the rig flicker detector.
#
# Deploys the tool trio to the rig, drives a capture, pulls results back, and
# runs the analysis locally. Robust to the rig's intermittent ssh drops: every
# rig call is short and retried; long work runs detached on the rig and is
# polled for a DONE marker.
#
# Modes:
#   deploy                     scp the 3 tool files to the rig (~/flicker-tools)
#   live   <label> [nframes]   read-only single burst from the CURRENT xemu
#                              window (does not launch/kill anything); analyze.
#   ab     [nframes]           full A/B: baseline vs XEMU_ASYNC_QUERIES=1. Runs
#                              detached on the rig; REFUSES if any instance is
#                              already up. Fetches both legs; runs `ab`.
#   fetch  <rig_run_root>      scp a finished rig run root back and analyze it.
#
# Env: RIG (default pacarey@192.168.68.85), SSH_KEY (~/.ssh/id_ed25519),
#      OUTDIR (local results dir, default ./flicker-results).
set -euo pipefail

RIG="${RIG:-pacarey@192.168.68.85}"
SSH_KEY="${SSH_KEY:-$HOME/.ssh/id_ed25519}"
OUTDIR="${OUTDIR:-$PWD/flicker-results}"
HERE="$(cd "$(dirname "$0")" && pwd)"
SSH_OPTS=(-o IdentitiesOnly=yes -i "$SSH_KEY" -o BatchMode=yes -o ConnectTimeout=10)
RTOOLS="flicker-tools"

rssh() {  # retried short ssh
    local n=0
    until ssh "${SSH_OPTS[@]}" "$RIG" "$@"; do
        n=$((n+1)); [ "$n" -ge 5 ] && { echo "ssh failed 5x: $*" >&2; return 255; }
        echo "(ssh retry $n) ..." >&2; sleep 3
    done
}
rscp() { scp "${SSH_OPTS[@]}" "$@"; }

deploy() {
    rssh "mkdir -p ~/$RTOOLS/runs"
    rscp "$HERE/flicker_detect.py" "$HERE/rig_flicker_capture.sh" \
         "$HERE/rig_flicker_ab.sh" "$RIG:$RTOOLS/"
    rssh "chmod +x ~/$RTOOLS/rig_flicker_capture.sh ~/$RTOOLS/rig_flicker_ab.sh"
    echo "deployed to $RIG:~/$RTOOLS/"
}

live() {
    local label="${1:-live}" nframes="${2:-40}"
    local stamp; stamp="$(date +%Y%m%d-%H%M%S)"
    local rdir="$RTOOLS/runs/live-$stamp"
    deploy
    echo ">> read-only capture from current xemu window ($nframes frames)"
    rssh "FDPY=~/$RTOOLS/flicker_detect.py bash ~/$RTOOLS/rig_flicker_capture.sh ~/$rdir $nframes 0 '$label'"
    local out="$OUTDIR/live-$stamp"; mkdir -p "$out"
    rscp -q "$RIG:$rdir/*" "$out/" 2>/dev/null || rscp -r "$RIG:$rdir" "$out"
    python3 "$HERE/flicker_detect.py" analyze "$out" --label "$label"
}

ab() {
    local nframes="${1:-60}"
    local stamp; stamp="$(date +%Y%m%d-%H%M%S)"
    local root="$RTOOLS/runs/ab-$stamp"
    deploy
    echo ">> launching detached A/B on rig ($root, $nframes frames/leg)"
    rssh "setsid bash ~/$RTOOLS/rig_flicker_ab.sh ~/$root $nframes >/dev/null 2>&1 < /dev/null & echo started"
    echo ">> polling for completion (each leg ~ boot 45s + settle + capture)"
    local waited=0
    until rssh "test -f ~/$root/DONE" 2>/dev/null; do
        sleep 15; waited=$((waited+15))
        printf '   ... %ds\n' "$waited"
        [ "$waited" -ge 900 ] && { echo "timeout waiting for DONE" >&2; break; }
    done
    local status; status="$(rssh "cat ~/$root/DONE" 2>/dev/null || echo UNKNOWN)"
    echo ">> rig DONE: $status"
    if echo "$status" | grep -q ABORTED; then
        echo "A/B aborted on rig (a session was live). Nothing to analyze." >&2
        rssh "cat ~/$root/runner.log" 2>/dev/null || true
        return 3
    fi
    local out="$OUTDIR/ab-$stamp"; mkdir -p "$out"
    rscp -r "$RIG:$root" "$out/"
    python3 "$HERE/flicker_detect.py" ab "$out"/ab-*/baseline "$out"/ab-*/async-queries \
        --label-a baseline --label-b async-queries \
        --out "$out/flicker_ab_report.json"
}

fetch() {
    local root="$1"; local base; base="$(basename "$root")"
    local out="$OUTDIR/$base"; mkdir -p "$out"
    rscp -r "$RIG:$root" "$out/"
    echo "fetched to $out"
}

cmd="${1:-}"; shift || true
case "$cmd" in
    deploy) deploy ;;
    live)   live "$@" ;;
    ab)     ab "$@" ;;
    fetch)  fetch "$@" ;;
    *) echo "usage: flicker_run.sh {deploy|live <label> [n]|ab [n]|fetch <root>}" >&2; exit 1 ;;
esac
