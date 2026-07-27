# Runbook — radar-skip (temporal downsampling) live test & operation

**What it is:** Steel Battalion's four radar scan-and-draw guest routines are
skipped on N-1 of every N frames via host-side breakpoints (title-gated
0x43430002, savestate-clean). Blip DATA refreshes at 30/N Hz; the GPU-rendered
sweep + the lock-on/targeting logic run every frame regardless.
Design + verification: docs/sb-graphics-findings.md §8.5, ISS-B16 file,
and the 2026-07-26 six-agent pre-flight (all green; machinery fired live:
arm lines, counter, disarm, loadvm re-arm; zero crashes).

## Procedure (verified end-to-end 2026-07-26)
```
rm -f /tmp/predl-off /tmp/narrowfence-on /tmp/radar-skip-n   # stale-file hazard
export XEMU_TLB_FASTPATH=1
bash ~/ring-launch.sh          # prints toggle states + any stale watch-files
# wait ~45 s, then restore mission:
echo "loadvm vm-20260725222138" | nc -q 8 -U /tmp/xemu-mon.sock
# pilot the mech (unattended the save self-fails in ~35 s), then:
echo 3 > /tmp/radar-skip-n     # ON  — expect BOTH lines:
                               #   radar-skip: N=3 -> ON (scan 1 frame in N)
                               #   radar-skip: Steel Battalion detected; ... armed
rm /tmp/radar-skip-n           # OFF — counter clears within a frame
```

## Reading the signals
- **Arm state = the `radar-skip: N=x -> ON/OFF` log lines**, not the counter
  VALUE. `RADAR_SCAN_SKIP` reads K (scanners actually skipped per skip frame),
  NOT the cadence average: K=2 in scenes where guest flag `[0x356ae0]==0`
  (confirmed three ways: disassembly, counter, live monitor peek `x /1wx
  0x356ae0` → 0). The game itself gates scanners 3/4 on that flag — skip-style
  gating is native to the game's own design.
- After any `loadvm`, re-arm is **silent** (BP_CPU wiped by vmstate load;
  re-arms in ≤N frames) — trust the counter reappearing, not a log line.
- **Set N only while SB is running.** The title probe latches once: a preset N
  during dashboard boot would latch the feature off until process restart.

## What the eyes are for (the gating call)
Blips: smooth-stale (persist between scans) vs blink (drawn-to-backbuffer).
Machine pre-read at ~16-21 fps saw NO blink (blips persist, brightness steady
±0.4), but cannot resolve a per-frame hold — real-time eyes decide.
Named cosmetic residual (proven bounded by disassembly): a texture-filter
field inherited by two small HUD icon overlays — nothing can be lost,
misplaced, or garbled; gameplay state untouched. If even that offends, v2 =
hook past the scan loop instead of the entry (keeps the state writes).

## Expectations
- Idle cockpit: NO FPS change (guest-CPU-bound; light scan load) — render
  thread drops ~7pp. The real win, if any, appears in busy-radar combat where
  scan work scales (~23K reads/frame).
- Close/move the Files (Nautilus) window — it overlaps the game window edge.
