# The Present-Wall Plan — how we get over it

> **BUILT AND MEASURED (2026-07-26, same day).** All six stages were implemented
> by a five-agent parallel build (branches overlap-s1-foundation / s2-present /
> s34-queries / s5-uploads / perf-element-batch, merged at `8a50cbcd98`),
> compiled clean first try, and validated in a 7-leg cumulative matrix on the
> mission save. **THE WALL FELL: pfifo_work 28.8 → 18.6 ms** — present 6.6→0.4,
> queries 2.9→0.1, renderpasses 205→33, AUX drains 30→4, ELEMENT_BATCH engaged
> (2418/frame), 0 VK errors, every armed detector clean, S2+S4 interlock cost
> 0.4 ms. **AND: net FPS did not move — frame time pins at exactly 33.3 ms
> (2×16.67): Steel Battalion vsync-locks itself to 30 via vblank pacing.** The
> pfifo thread now idles ~14.6 ms/frame waiting for the guest. **That guest wall
> is now RESOLVED (see [`../sb-frame-limiter.md`](../sb-frame-limiter.md)): SB is
> a FIXED 30 Hz-timestep engine — the "60fps game all along" hypothesis is
> REFUTED; a present-unlock byte-patch runs the game at 2× speed. True 60 needs
> sim/render decoupling (frame-gen), not a patch. Live combat = ~23 fps.** Every
> other title without a 30-lock inherits the 10 ms immediately. Still pending: owner-eyes +
> flicker-detector + radar byte-diff correctness gates (all stages remain
> default-OFF until they pass). Follow-ups: uniform-arena peak 7.07/8 MB
> (headroom shrinks if drains reduce further); S2+S4 region-window rework only
> if the 0.4 ms interlock ever matters.

**2026-07-26. The synthesis of the four-lane investigation.** Depth lives in the
lane docs; this is the decision surface and build order.

| Lane | Doc | One-line finding |
|---|---|---|
| 1 present path | `overlap-present-path.md` | The 6.6 ms is one wait (`display.c:917` → wait-all); no VK↔GL semaphore exists — move the wait to the parked display thread; the VR copy ring already proves submit-no-wait in-repo |
| 2 queries | `overlap-query-attacks.md` | ~90% of renderpass churn is query-reset placement (fixable, legal); async report delivery = same bytes, same wall-clock, zero pfifo blocking |
| 3 resources | `overlap-resource-audit.md` | The 4-slot ring already exists; the recycle phase is the real surgery; single-time fences transitively drain everything (the hidden wall) |
| 4 prior art | `overlap-prior-art.md` | RPCS3/DXVK/PCSX2/Dolphin all converge on the same design; RPCS3 independently proved stale query values = flicker (Brutal Legend) and walked it back |

## The answer in one paragraph

Steel Battalion's frame is serial: the pfifo thread records ~19-20 ms of Vulkan
work, then stands still ~7-9 ms waiting for the GPU, and every millisecond we
free anywhere else re-absorbs into that wait (measured twice). The way over is
the design every mature emulator converged on: **submit early and often, wait
never — except scoped, and at the consumer.** Reports are written when the GPU
finishes (the guest's own spin absorbs the latency — identical bytes at
identical wall-clock time, the opposite of the refuted stale query-ring);
the display waits its own fence on its own thread (which sits parked with a
16.7 ms budget); the radar readback keeps byte-exact full-drain semantics
until last. End state: frame time → max(CPU record, GPU exec) instead of
their sum — **~20 ms ≈ 50 FPS** from overlap alone, with the renderpass-churn
kill and the already-built pipe/const/ELEMENT levers then actually *paying*
(they are absorbed today) toward 55-60.

## The convergent design (what gets built)

**D1 — Overlap-ready resources** (Lane 3 §6). Decompose the recycle phase:
per-slot descriptor pools, per-slot framebuffer destroy-lists, staging-arena
watermarks with reclaim-on-retire, `uploaded_bitmap` tied to true full-drains.
The CB/fence/semaphore ring (4 slots) already exists. ~Zero new memory.

**D2 — Async present** (Lane 1 §5.1). PresentSlot ring N=2 (per-slot composite
CB + fence + descriptor set + ping-pong exported display image/GL texture);
`render_display` and `flip_stall` submit-without-wait; the display thread
fence-gates before sampling (preserving the exact CPU-side interop contract);
destruction paths (`invalidate_surface`, resize, loadvm/flush, finalize) wait
the present ring. Modeled on the in-repo VR copy ring.

**D3 — Async report delivery** (Lane 2 Attack A + prior-art triggers). At the
stall site: submit + pending-marker, fence-POLL at the top of the pfifo loop,
prefix-apply retired reports, timed park (0.5-1 ms) instead of indefinite wait
while a marker is pending. Same bytes, same wall-clock. Add the prior-art
submit-early triggers as counters justify: submit at GET_REPORT after observed
stalls (DXVK), age-out at 300 µs (RPCS3), ring-half-full background submit
(Dolphin). Force-complete at finish/DMA-remap/savevm/renderer-switch.

**D4 — Renderpass-churn kill** (Lane 2 Attack B). Frame-start batched
`vkCmdResetQueryPool` (whole pool, outside any pass) + monotonic slot
allocation + ping-pong pools (2×1024); `vkCmdBeginQuery/EndQuery` move inside
render passes; passes stop breaking at report boundaries: ~205 → ~20-30/frame.
Directly attacks the combat-scene collapse (500 polls → 500+ passes today).
DXVK precedent: host-reset + per-CB query segments, zero pass churn.

**D5 — In-batch uploads** (Lane 3 §0/§3 — the hidden wall). Move texture and
surface uploads from the blocking single-time path into the frame's command
stream with staging rings; narrow the `surface.c:1331` upload full-finish
(the drain no lane owned — now owned here). Until D5, every single-time fence
transitively drains all in-flight slots and silently caps D1-D4's win.

**Kept synchronous (correctness anchors):** the radar/surface download path —
byte-exact, full-drain semantics in v1, narrowed only LAST (PCSX2-shape:
submit-if-current → wait one fence → memcpy) after everything else is stable;
loadvm/savevm/reset/scale-change full drains; the guest flip pacing contract
(READ_3D/vblank) untouched.

## Build order (each stage ships default-OFF, gated, reversible)

| Stage | Contents | Gate to pass | Expected effect (honest) |
|---|---|---|---|
| **S0 counters** | Arena high-water, descriptor max, TEX_UPLOAD bytes, vertex-RAM dirty-sync count, FINISH_PRESENTING/frame, churn sub-timer in method_pipe | One rig session; no behavior change | Sizes the unknowns that decide S1 details; settles uniform-arena headroom (7-8 MB vs 8 MB risk) and the vertex-rewrite frequency (the #1 overlap-killer if high) |
| **S1 overlap-ready** | D1 behind `XEMU_FRAME_OVERLAP` (OFF) | ON == OFF golden-image + mspf-identical; sync-val armed | None (scaffolding); proven methodology from the ring build |
| **S2 async present** | D2 (+S1 ON) | No tearing, no VUIDs, radar PPM byte-diff clean, savevm/loadvm cycle, resize storm, flicker_detect, owner eyes | composite 6.6 → ~0.4; net ≈2-4 ms alone (re-absorption expected — pre-registered, not failure) |
| **S3 async reports** | D3 | STALLED→0 with correct values; per-present dump A/B if available; flicker_detect + owner eyes | queries 2.8 → ~0.1 *and stays freed* (S2 removed the absorber); guest sees identical values |
| **S4 churn kill** | D4 | Pass count ~205→~30 counter-verified; sync-val armed; flicker_detect + owner eyes (ISS-B16 gate) | method_pipe −0.6-1.5 ms CPU; GPU frame −0.5-2 ms (bubbles); combat-scene scaling flattens |
| **S5 in-batch uploads** | D5 | QUEUE_SUBMIT_AUX drains → ~0 mid-frame; upload paths golden-image | Removes the transitive-drain cap; the overlap becomes real for the whole frame |
| **S6 claim the banked levers** | pipe-opt ON (+ ELEMENT-batch sibling if built) | Standard lever gate | Now they pay instead of being absorbed |

Every stage: `XEMU_*` env + watch-file + stderr state line + self-counter
(M-5); two independent runs before "cleared" (M-1); armed detectors (M-3);
owner eyes before any default-ON (M-4). ABABA same-boot A/B via the interleave
harness for every perf claim (M-8).

## The math (pre-registered expectations)

Today: pfifo_work 28.8 ≈ frame. Composition target:
- After S2+S3+S5: pfifo stops blocking mid-frame → frame ≈ CPU record ≈
  method 15-16 + texhash 1 + download 1.8 + submit ~1 + present ~0.5 ≈
  **~19-20 ms ≈ 50 FPS** (GPU's 9-10 ms fully hidden). 45+ = high confidence;
  50 = the model's center.
- S4 + S6 then remove ~2-3 ms of real CPU work → **~17 ms ≈ 57-60 FPS** at the
  optimistic edge; **55 realistic**. True locked 60 may additionally need
  method_resid/const attention (the remaining honest gap).
- Failure shape to watch: a stage that "works" but shows zero mspf change and
  no bucket relocation = a hidden serializer remains (QUEUE_SUBMIT_AUX is the
  telltale — Lane 3's transitive-drain trap).

## Kill conditions (any → stage OFF, investigate before proceeding)

New-class VUID or sync-val hazard; flicker-detector separation vs baseline;
see-through geometry or any culling artifact (the query-ring signature);
radar PPM byte-diff nonzero; visible tearing in flat mode; ABABA mspf
regression >1 ms; savevm/loadvm breakage.

## Not in this plan

Threading the pfifo decoder (DO-NOT, reconfirmed by RPCS3+Dolphin defaults);
any stale/approximate query tier (refuted here AND at RPCS3); KVM (orthogonal,
wrong tool for SB); guest-code surgery (radar skip-N stays parked separately);
the GL renderer (untouched).
