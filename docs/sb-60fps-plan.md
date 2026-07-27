# The 30→60 plan — attacking the guest-CPU wall

> **SUPERSEDED (same day, 2026-07-26).** This charter's premise — that the
> guest-CPU thread's ~31 ms is the wall — was REFUTED by its own Phase 0:
> the guest profiler showed ~77% of guest time is SPIN waiting on the
> emulated GPU. SB is pfifo/GPU-emulation-throughput-bound; spin
> elimination was measured a non-lever (see the graveyard in
> `road-to-60.md`, the authoritative synthesis). Kept for the record —
> the radar skip-N idea (P2) remains parked-but-live there. Do not build
> from this doc.

**2026-07-26. Status: charter.** Prereq reading: sb-graphics-findings.md
(the campaign), sb-rendering-anatomy.md (the game). Current shipped state:
30-31 FPS mission, guest vCPU ~94% busy (~31 ms/frame), pfifo ~45%, GPU
~60%. Target: guest-thread ≤16.7 ms. Every phase below carries its own
measurement gate (M-rules apply: replication, armed detectors, same-scene
arms).

## The budget model (what the ~31 ms is made of — to be MEASURED in P0)
| Component | Est. today | Removable? |
|---|---|---|
| Genuine game logic under TCG | unknown (bulk) | only via HLE/game surgery |
| Guest BUSY-SPINS (report polls 2-4x/frame, DL round-trip, vblank?) | **3-9 ms** | YES — idle-loop skipping (P1) |
| Radar scan loops (scales w/ combat) | 1-5 ms | YES — skip-N landed (P2) |
| TCG translation overhead multiplier | (multiplies all of the above) | partially (P3/P4) |
| Residual emu-side traps/callbacks | ~1 ms | mostly done (fast path, TLB) |

## Phase 0 — GUEST PROFILER (the keystone; do first, everything keys off it)
We have never actually profiled inside the guest. Build the EIP sampler:
generalize docs/perf/scan-pc-capture.patch from "log watched-surface
readers" to timer-based guest-PC sampling (e.g. sample env->eip from the
vCPU thread every N TB exits or via cpu_exec hooks; dedupe into a
histogram; symbolize against the XBE map we already parsed).
**Deliverable:** top-30 guest hot PCs per scene (idle cockpit / radar-live
/ combat), each classified: game-logic / spin-loop / scan / XDK-runtime
(memcpy-class). One rig session. **Gate:** the histogram is the plan's
ground truth — no phase proceeds against guesses.

## Phase 1 — SPIN ELIMINATION (idle-loop skipping) — biggest expected win
For each spin loop P0 confirms (report-poll first: we WRITE the value the
guest polls, so the wake condition is fully known):
hook the loop head with the radar-skip breakpoint machinery → instead of
executing the poll, halt the vCPU (cpu halt/wfi-style or a host futex)
→ wake exactly when the emulator writes the polled address (we own that
store — pgraph_write_zpass_pixel_cnt_report and the report/semaphore
writers). Guest observes identical values, identical order; it just stops
burning cycles between request and delivery.
**Est:** 3-9 ms/frame. **Risk:** low-medium (wake-condition completeness;
title-gated like radar-skip; per-loop toggle files). **Gate:** guest
busy% drops with mspf, same-scene A/B, zero behavior change.

## Phase 2 — SCAN REMOVAL, FULL VALUE (radar-skip → combat validation → v2)
(a) Owner live test validates skip-N visually (pending). (b) Measure in
COMBAT (scan scales with contacts — idle cockpit showed 0 FPS delta; the
claim lives in busy scenes). (c)
