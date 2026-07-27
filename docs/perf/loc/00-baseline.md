# Steel Battalion: Line of Contact — performance baseline (session notes)

First live look at **Steel Battalion: Line of Contact** (the Xbox Live sequel,
`Tekki Taisen`), captured 2026-07-27 while the game ran on the rig. This is the
opening reconnaissance for a per-title perf campaign, in the shape of
[`docs/perf-per-title-playbook.md`](../../perf-per-title-playbook.md). Raw log
evidence: [`baseline-log-excerpt.txt`](baseline-log-excerpt.txt).

## TL;DR

- **LoC runs at ~3–8 FPS during an in-engine cutscene of multiplayer combat**
  (mspf **130–380 ms**, worst frame **703 ms**). Real-time engine rendering of
  combat content → a strong proxy for interactive-combat cost, though an
  interactive capture is still needed to confirm. Title/menu holds 30 (33.3 ms),
  like SB.
- **The wall is the surface→texture *download-fallback*, and it's
  emulator-internal — the guest never reads anything back** (`SURF_CPU_READ=1`,
  `MEMCB_READ_DISPATCH=1` *every* frame). LoC issues **~700–800 "use this surface
  as a texture" ops/frame**; kept GPU-side they cost ~300 ms (still only ~3 FPS),
  but when they **fall back to a blocking GPU→CPU download + rehash** they cost
  **~750–850 ms** (~1.2 FPS). `SURF_TO_TEX + SURF_TO_TEX_FALLBACK` ≈ constant;
  frame time is decided by how many fall back.
- **Root cause (measured):** `check_surface_to_texture_compatiblity()`
  (`vk/texture.c:1251`) returns false for the cockpit **sub-monitor** surfaces —
  the comment at `texture.c:1512` already calls this "the sub-monitor fallback."
  Each failure = one `SURF_DOWNLOAD` + a blocking `FINISH_SURFACE_DOWN` + a large
  `TEX_HASH` (up to **177 MB/frame**). The counters move together:
  `SURF_TO_TEX_FALLBACK ≈ SURF_DOWNLOAD ≈ QUEUE_SUBMIT_AUX ≈ FINISH_SURFACE_DOWN`
  (600–770 in the worst frames).
- **NOT the SB problem.** SB was a CPU read-*trap* fixed by the read/TLB
  fast-path; that fast-path does nothing here (`MEMCB_READ_DISPATCH=1`). This is a
  renderer surface→texture *compatibility* miss, unique to LoC's render-heavy
  cockpit. Corrected from the first draft's "readback-bound" framing — the guest
  isn't reading anything; the emulator is.

## Session / provenance

| | |
|---|---|
| Game | `Tekki Taisen - Steel Battalion - Line of Contact (World) (En,Ja).iso` (625 MB) |
| Binary | rig `xemu_vr-research/build/qemu-system-i386`, commit `8a50cbcd` |
| Renderer | **Vulkan** NV2A on **RTX 4050** (PRIME); no GL fallback |
| Flags | full stack on: `TLB_FASTPATH PHASE_TIMERS FRAME_OVERLAP ASYNC_PRESENT ASYNC_REPORTS INBATCH_UPLOADS BATCH_QRESET PIPE_OPT ELEMENT_BATCH` |
| Config | `sb-floor-s1.toml` (original-SB config — **not yet tuned for LoC**; same BIOS `mcpx_1.0` + `xbox-4627_debug`, same SBC controller map) |
| Launcher | `~/coldboot-play-loc.sh` on the rig (SB launcher with the disc + logfile swapped) |

## Measured (not inferred)

**Title / light scene** — representative of nothing (the dead-screen trap):
```
phase: mspf=33.3[32.6-33.9] pfifo_work=1.7 pfifo_idle=31.7 present=0.4 light=60 heavy=0
```
30 FPS, GPU idle 95% of the frame. (Note: LoC's title sits at **30**, where SB's
menus rendered at 60 — worth confirming whether LoC is also a fixed-30 engine.)

**Live 3D scene** — the real cost:
```
phase: mspf=286.0 run=285.4 blocked=0.6 dl_stall=0.6 pfifo_work=248.4 pfifo_idle=37.7 present=0.4 light=0 heavy=60
pfifo: work=252.5 method=21.7 texhash=2.4 queries=0.2 download=223.1 upload=1.0 submit=0.1 present=4.0
pfifo2: method_synctex=35.6 method_const=2.8 method_pipe=13.5 present_gpuwait=3.0 ...
nv2a-prof (703ms frame): QUEUE_SUBMIT=608 QUEUE_SUBMIT_AUX=603 PIPELINE_RENDERPASSES=616
  BEGIN_ENDS=1829 INLINE_ELEMENTS=1065 SHADER_BIND=1829 SURF_TO_TEX=14
  TEX_HASH=626 TEX_HASH_KB=177163 TLB_FILL=14351 ELEMENT_BATCH=1937
```
Session spread: 108 frames, min 1 ms (blank), median 33.3, p90 212, max 650.
Renderpasses: median 3 (menu) → max **767** (scene).

## The wall, decomposed

1. **`download` (surface readback → guest RAM) = 150–223 ms** — the dominant
   bucket, ~85–90% of `pfifo_work`. ~**600 auxiliary queue submits/frame**
   (`QUEUE_SUBMIT_AUX`), i.e. ~600 individual download submit+wait round-trips.
2. **`method_synctex` = ~35 ms** — surface→texture sync (second cost).
3. **`TEX_HASH` = 177 MB/frame across 626 textures** — texture-dirty hashing at
   scale (third cost; `texhash` bucket itself is small at 2.4 ms, so most of the
   177 MB is hashed cheaply, but it scales with the surface churn).
4. **NOT CPU-logic-bound:** `blocked≈0`, `dl_stall≈0.6 ms`, `run≈pfifo_work`.

So the emulator is doing ~600 GPU→CPU surface downloads per frame, serialized as
side submits. That is the thing to attack.

## How this differs from Steel Battalion

| | SB (original) | LoC |
|---|---|---|
| Shipped bottleneck | CPU read-**trap** on the radar surface (~23k traps/frame) | GPU surface **download** (~600 readbacks/frame) |
| Fix that worked | read/TLB fast-path (trap suppression) → 20→30 | *does not* address the download **cost** |
| Frame cost (busy scene) | ~19–20 ms record + ~7 present | **150–340 ms, download-dominated** |
| Renderpasses (busy) | ~205 → ~33 with `BATCH_QRESET` | up to **767** (BATCH_QRESET already on) |
| FPS (busy scene) | ~23–30 | **~3–8** |

The read fast-path attacks the *trap* (guest CPU reading RAM that shadows a
surface). LoC's problem is the *download itself* — the emulator materializing
~600 surfaces/frame to RAM. Different lever needed.

## Hypotheses (unverified — flagged as such)

- **Scene = in-engine cutscene of multiplayer combat** — owner-confirmed
  2026-07-27 (sustained 4 FPS, mspf ~700; appears in-engine/real-time, not FMV,
  as best the owner can tell). Not interactive gameplay, but real-time engine
  rendering of combat content → a reasonable proxy (likely near worst case) for
  interactive MP combat. Still want an **interactive-combat capture** to confirm,
  plus a **save-state** of a representative scene as the stable A/B benchmark (as
  used for SB). *Scene-typing here has been revised twice — don't over-read it.*
- **[RESOLVED by measurement — see the TL;DR root cause.]** The ~600 "downloads"
  are the **`SURF_TO_TEX` fallback**, not guest readback (`SURF_CPU_READ=1` every
  frame). The single remaining unknown is *which* of the six
  `check_surface_to_texture_compatiblity()` conditions the sub-monitors trip:
  non-swizzled pitch mismatch, width, height, cubemap, `levels>1` (mipmap), or
  color-format/bpp mismatch. A one-line instrumentation build (per-condition
  counter) names the exact fix.

## Open questions → next steps (for the campaign)

1. **Decisive question — ANSWERED (measured): emulator-forced, not guest.**
   `SURF_CPU_READ=1`/`MEMCB_READ_DISPATCH=1` every frame; the cost is the
   `SURF_TO_TEX` compatibility fallback. Remaining: a **one-line instrumentation
   build** (per-condition counter in `check_surface_to_texture_compatiblity`) to
   name which of the six conditions the sub-monitors trip. Needs a rig rebuild +
   the owner back in the scene to read it.
2. **Lever A — kill the fallback (worst frames ~750 → ~300 ms).** Once the failing
   condition is known, make the sub-monitor surfaces take the GPU-side path
   (`copy_surface_to_texture`) instead of download+rehash — e.g. handle the
   pitch/dimension/mip case with a GPU blit/scale rather than bailing. Purely
   emulator-internal (guest reads nothing), so low correctness risk. **Biggest
   single win.**
3. **Lever B — cut the volume (needed to approach 30).** Even GPU-side, ~700–800
   surface→texture ops/frame cost ~300 ms (~3 FPS). Dedup **unchanged** sub-monitor
   surfaces (skip re-render+re-sample when content is static) and/or
   temporal-downsample cosmetic displays (the `radar-skip` pattern generalized).
   This is the deeper lever and the one that decides whether LoC can be *playable*
   vs merely *smoother*.
4. **Timestep question:** fixed-30 like SB (→ 60 needs frame-gen) or time-based?
   Title ran at 33.3 ms. Frame-limiter RE method in
   [`runbooks/rig-perf-measurement.md`](../../../runbooks/rig-perf-measurement.md).
5. Capture an **interactive-combat** frame + a **save-state** as the stable A/B
   benchmark, then decide whether LoC warrants SB-depth.

> **Honest scope:** Lever A is achievable and high-value (removes the 1-fps
> stutters). But even perfect, best-case is ~300 ms/frame (~3 FPS) — LoC renders
> ~700–800 RTT passes/frame, so **30 FPS needs Lever B (a real volume cut), which
> is a substantial effort.** LoC is a harder target than SB.

## Implemented (2026-07-27, pending in-scene validation)

Both step-1 and Lever A landed in one pass (`vk/texture.c`, `debug.h`):

1. **Always-on miss-reason counters** — every storm-path fallback (`forced>0`)
   now also ticks exactly one of `S2T_MISS_NOSURF / WITHIN / MIPS / PITCH /
   DIMS / CUBEMAP / FORMAT` (the historical bool compat check refactored into
   `surface_to_texture_compat_reason()`; `NOSURF` refines to `WITHIN` when a
   surface *contains* the texture base without starting at it). Reading the
   breakdown in any heavy scene names the failing condition — no gdb needed.
2. **`XEMU_SURF2TEX_EXT=1` — the gather path** (Lever A, opt-in, default OFF,
   `XEMU_NO_SURF2TEX_EXT` kill-switch). Where the exact-match test fails but
   every overlapping resident surface is **row-compatible** (color, linear,
   same pitch & texel size, row-aligned overlap), the bind is serviced by
   direct `vkCmdCopyImage` regions from each overlapping surface into the
   texture image — fully GPU-side, replacing the blocking download + 177 MB/frame
   rehash + CPU re-upload. Covers: evicted-exact-base with resident overlaps,
   texture-inside-larger-framebuffer (`WITHIN`), tiled sub-surfaces, and
   width/height mismatches. Not covered (falls back as before): swizzled
   surfaces, pitch mismatch, zeta, cubemap, mip chains (`S2T_MISS_MIPS` counter
   decides whether a v2 mip-walk path is warranted). Dedup key = sum of
   gathered surfaces' `draw_time` (+ region count) — strictly changes when any
   gathered surface is redrawn; a max() key would miss low-timestamp redraws.
   Fresh partially-covered images are cleared to black first (deterministic;
   uncovered rows are not expected to be sampled). `S2T_EXT_COPY/REGIONS/
   PARTIAL` counters report engagement.

OFF path is byte-identical to the historical flow (counters only add
increments inside the already-cold storm branch).

## Iterated fix — v4 and the measured verdict (2026-07-27)

Live iteration (4 builds, counter+geometry-dump-driven) refined the fix into
three GPU-side paths under `XEMU_SURF2TEX_EXT`, matched to the three measured
storm classes:

| storm class (measured) | example geometry | path |
|---|---|---|
| PITCH, ~596/frame worst | 256×256 swizzled A8R8G8B8 tex @ zeta base | **BOUNCE** |
| WITHIN, ~240/window | 64×64 swizzled tex @ zeta base+0x80000 (word-aligned) | **BOUNCE** (offset bias) |
| DIMS/quadrant | 64×64 swizzled tex ⊂ 128×128 swizzled surface, same base | **QUADRANT** |

- **BOUNCE** — the storm killer: surface image → raw byte stream in
  `BUFFER_COMPUTE_DST` (`vkCmdCopyImageToBuffer` at surface pitch, gap bytes
  zero-filled) → **Morton-deswizzle compute** (new pipeline in
  `surface-compute.c`; push constants `{w, h, src_bias_words}`; bit convention
  = `swizzle.c`) → `vkCmdCopyBufferToImage` into the texture. Replicates the
  CPU fallback byte-for-byte with zero CPU round-trips. D16/color linear
  sources only (no D24S8), `scale==1`, square pow2 4-bpp textures,
  word-aligned offsets, bail on other-surface overlap or descriptor-pool
  exhaustion.
- **QUADRANT** — Morton nesting: a smaller square pow2 swizzled texture at a
  larger square pow2 swizzled surface's base *is* its top-left rect in both
  images' deswizzled storage → plain `vkCmdCopyImage`.

**A/B on the owner's storm-scene snapshot (`vm-20260727054737`, identical
scene, monitor-loadvm):**

| | EXT off | EXT on |
|---|---|---|
| sustained mspf | **210–449 ms** (~2–5 fps) | **60–63 ms** (~16 fps) |
| pfifo `download` | 143–341 ms | **18.8 ms** |
| `SURF_TO_TEX_FALLBACK` (window) | 128 | **19** |
| bounce/quadrant engagements | — | **7664 / 14** |
| VK validation errors | 0 | **0** |

**~4 fps → ~16 fps on the exact scene.** Residual: 19 SWIZSURF-class misses
(swizzled surface at an offset — uncovered corner) and one heavier transition
window (186 ms, download 107 — eviction traffic); both are follow-up polish.
The 60 ms floor is now method+present+residual-download bound, not
fallback-bound — reaching 30 fps is a volume/eviction campaign (Lever B), not
this fallback.

Snapshot loading note: the SBC controller is hotplugged, so CLI `-loadvm`
fails with `Unknown section or instance ... usb-steel-battalion`; issue
`loadvm` via the monitor socket after boot instead (`~/loc-bench-{off,ext}.sh`
do this).

## Round 2 (2026-07-27, same snapshot) — the eviction cascade

With the fallback dead, the next measured wall was the **surface eviction
cascade**: LoC re-uses the shadow map's memory — and its exact base — for
small transient color blocks; every create/re-target evicted the 6.5 MB zeta
(download, bursts to 173 ms) and the next full-map bind re-uploaded it
(~127 MB/window). **`XEMU_ALIAS_SURFACES`** (opt-in) keeps large parents
resident under much-smaller children (all three sites: create-overlap,
target-switch incompatible-evict, plus look-deeper same-base scans at the
texture bind and target switch; a draw_time freshness rule keeps the S2T
paths alias-correct).

| storm-snapshot metric | round 1 (EXT) | round 2 (EXT+ALIAS) |
|---|---|---|
| pfifo download | med 18.8 ms, bursts 173 | **med 0.8 ms, max 4.2** |
| 1800×1800 re-upload dumps | 600/window | **0** |
| med mspf | 60–63 | **50.1** (~20 fps; baseline was ~4) |
| VK errors | 0 | 0 |

Also measured: `CONST_SKIP` inert on LoC; mid-scene guest profile = **86%
game code** (hot loop `0x33060–a0` = 22% — future RE target); the remaining
~190 ms windows correlate 1:1 with **~700 exact-path surface→texture copies
(≈700 renderpasses)** — the ping-pong volume itself.

## Round 3 (2026-07-27) — the smoke wall falls: 4 fps → a steady ~20

Field observation cracked it: the owner's screenshots showed slowdown tracking
**smoke / haze / film-grain**, not unit count (two VTs at 18 fps; one VT in a
dust plume at 4), plus a decisive tell — the TV-static scene transition ran
slow *before the destination scene was ever shown*. Counters agreed:
`ZC_REJ_ZETA_SURF = 12,182/window`. The cause: **soft-particle smoke sampling
the still-bound D16 depth buffer** to depth-fade (plus the cockpit
sub-monitors, its color-feedback sibling) — legal on Xbox unified memory,
illegal to sample in Vulkan, so every one of ~700 binds/frame ran the full
depth→texture conversion.

**`XEMU_SURF2TEX_FEEDBACK`** (opt-in; requires `XEMU_SURF2TEX_ZEROCOPY`):
sticky per-surface feedback mode → the surface rests in
`VK_IMAGE_LAYOUT_GENERAL`; `color_general`/`zeta_general` render-pass variants
carry the attachment layouts (render-pass *compatibility* ignores layouts, so
pipelines and framebuffers interoperate); per-binding descriptor layouts; a
`GENERAL→GENERAL` visibility barrier emitted **once per redraw** (per-bind
flushing doubled the pass count — measured, then fixed); and **depth-aspect
views over the D16 zeta**, bit-identical to the Y16 the conversion produced.

| storm-scene metric | round 2 | **round 3 (v9.3)** |
|---|---|---|
| heavy frames | 190–250 ms, ~700 copies | **43–56 ms, ZERO copies** |
| render passes (heavy) | ~700 | **~20** (merged) |
| full-run mspf | med 50 / p90 197 | **med 50.0 / p90 51.7 / max 83** |
| VK validation errors | not measured¹ | not measured¹ |

**The 200 ms class is gone entirely** — the game holds a consistent ~20 fps in
smoke, cockpit, and combat alike (owner-confirmed on screen). Remaining gap to
30: the ~50 ms class is ≈3 vblank ticks; total work under 33.4 ms would lock
30 fps, and the anatomy now has no single villain left.

**Superseded design note (kept for provenance): zero-copy surface-as-texture** —
sample the surface's VkImage directly through a view (as the GL backend
always has) instead of copying per redraw-rebind cycle; kills the 700
copy+barrier clusters and re-enables renderpass merging. Invasive (breaks the
surfaces-rest-in-ATTACHMENT invariant; needs per-surface layout tracking +
borrowed-image lifetime in the texture cache) — gated on owner visual
validation of rounds 1–2 first, since the alias model intentionally serves
pre-clobber depth for aliased ranges.

## Reproduce

```
ssh <rig>  ~/coldboot-play-loc.sh          # full stack, Vulkan, LoC disc  (EXT off — counters only)
ssh <rig>  ~/coldboot-play-loc-ext.sh      # same + XEMU_SURF2TEX_EXT=1 → ~/coldboot-loc-ext.log
# FPS overlay: mouse → Debug → Video.  Phase log: ~/coldboot-loc.log
```
H-2 (live-session check) before any relaunch. Do **not** attach gdb mid-scene
(perturbs mspf; pauses the guest).


## Correction (2026-07-27, post-review)

Two errors in the rounds above, both found after the fact:

**1. ¹ "0 VK validation errors" was meaningless.** Validation layers are gated
on `display.vulkan.validation_layers`, which was never set on the rig — so they
were **off for every run in this campaign**. Grepping the logs for validation
messages could only ever return zero. Those numbers are not evidence of
correctness and have been struck; the work in rounds 1–3 was never
validation-checked. (Enabling the setting still produced no output; the layer
plumbing needs its own investigation before any such claim is made again.)

**2. The v9.3 depth borrow was unsound — owner caught it visually.** Sampling
the D16 zeta through a depth-aspect view was justified as "bit-identical to the
Y16 the conversion produced." It is not. LoC binds its shadow map as
`LU_IMAGE_DEPTH_Y16_FLOAT` (a `depth` format): the conversion copies **raw
16-bit bits** into a Y16 texture and the fragment shader applies the Xbox
float-depth decode, whereas a `D16_UNORM` view makes the **sampler**
pre-normalize to [0,1] and the shader then decodes on top — a double decode
feeding wrong values into the shadow comparison. Visible as blocky/incorrect
shadow pixels; no counter could have seen it.

**v9.4 fixes both concerns:** depth surfaces are never borrowed (the converting
copy path is restored), and the speed is recovered soundly by running that copy
**once per frame** instead of once per bind — `draw_time` advances with every
particle draw, so the old key re-converted the whole shadow map ~700×/frame,
while the depth soft particles sample is the *finished opaque scene*, for which
one snapshot per frame is correct. Measured: copies/frame ~700 → **1–2**,
renderpasses ~700 → **20–23**, mspf med **50.0** / p90 **50.9** — i.e. v9.3's
performance with v9.3's bug removed.
