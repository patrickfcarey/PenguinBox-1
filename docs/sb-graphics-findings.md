# Steel Battalion performance — consolidated findings

> Game-side rendering knowledge (how SB actually renders — the radar
> pipeline, query-driven lights, texture streaming, XBE map) lives in
> **[sb-rendering-anatomy.md](sb-rendering-anatomy.md)**; this doc is the
> emulator-side performance investigation built on it.

**Branch `sb-graphics-research`. Last synthesis: 2026-07-26.**
This is the authoritative reference; it supersedes the earlier chronological
log. Organized by what we KNOW vs SUSPECT vs REFUTED, so nobody re-walks a
dead hypothesis. History/falsification trail preserved (it's evidence).

## Current state (2026-07-26) — what shipped, what's parked, what's off
The investigation below is chronological; sections that predate this banner
are history. As of now:
- **SHIPPED, always-on — read fast path:** trapped surface reads 23K→1-2/frame
  (quiesced mem-access-callback skip at the physmem walk). With
  `XEMU_TLB_FASTPATH=1` this is the **known-good config** — SB mission gameplay
  20→30 FPS, clean picture, owner-verified. §11 (V2 iteration).
- **PARKED, opt-in — exact-dirty hash narrowing (`XEMU_EXACT_DIRTY=1`, default
  OFF):** −60% hash volume on cutscene/menu content, but a co-page false
  negative flickers animated mission textures (ISS-B16 source 1). Off until the
  dirty-channel-separation re-enable is built. §8.7.
- **HARD-OFF — async query readback (`XEMU_ASYNC_QUERIES`):** absorbs the
  STALLED occlusion-query walls but introduces light flicker; four root-cause
  theories are dead, working conclusion is resolve-latency timing variance,
  destination is the deferred-epoch ring. Full graveyard: **ISS-B16**. §8.7.
- **MERGED, not yet executed — radar skip-N** (temporal downsampling of the
  scan-and-draw routines, live-tunable `/tmp/radar-skip-n`): the pending live
  test is whether skipped-frame blips read stale-smooth or blink; lock-on is
  unaffected by design (§3/§8.5). Branch perf-radar-skip → `8fa7778be2`.
- **REFUTED, retained as scaffolding — narrow-fence async early-submit:**
  measured no-win twice (§11); the fence-ring architecture is kept default-OFF
  as the prerequisite for the deferred-epoch-ring redesign. Predictive readback
  (§5/§9) is likewise refuted — §9 below is superseded history; §10's
  next-steps were refreshed to the current queue.

---

## 1. TL;DR — the two-term cost model

Steel Battalion runs at ~20 FPS (≈44 ms/frame) even in near-empty scenes and
collapses to ~7 FPS (140 ms) in heavy combat, on **both** the community
machine and our rig. The cost decomposes into two independent terms, and two
parallel investigations each found one of them:

- **FLOOR (~44 ms, scene- and scale-independent):** *read-triggered surface
  sync.* The game renders small helper surfaces (radar, monitors) then
  **CPU-reads them every frame** for game logic — free on the Xbox's unified
  memory, but here the first read after each draw forces a synchronous
  GPU→CPU download + full pipeline drain. Measured **4 full GPU drains/frame**
  (`FINISH_SURFACE_DOWN=4`), ~10 ms each ≈ the whole floor. This term is
  constant across scenes.
- **SLOPE (44 → 140 ms with scene complexity):** *texture re-hashing.* Those
  same render-then-read buffers sit adjacent to texture VRAM, so each update
  marks overlapping textures dirty → full-texture `fast_hash` on next bind.
  Measured **~4.5 MB/frame hashed in standby, ~35 MB/frame radar-live** — this
  scales with contacts/effects and is what carries the frame time up in combat.

**One root, two symptoms:** a small, frequently render-then-read/sampled buffer
thrashing the surface+texture sync machinery. Downloads, re-hashes, and
surf-to-tex copies all cascade from it. **This is xemu-wide, not SB-specific**
(any title with a small frequently-rewritten VRAM buffer adjacent to texture
data), which makes a fix genuinely upstreamable and likely subsuming the
level-transition slowdown family (#659, #2333) and ISS-B15 (film grain = the
same mechanism with a GPU writer instead of the radar).

---

## 2. CONFIRMED (measured on our rig, counter binary)

**Two scenes measured.** "Standby/probe" = cockpit boot state (one click from
title; NOT representative — the radar isn't live). "Radar-live" = a real
mission scene with the radar sweeping (user-verified, live capture).

| Counter (per frame) | Standby | Radar-live | Meaning |
|---|---|---|---|
| `FINISH_SURFACE_DOWN` | 4 | 4 | full GPU drains — **the floor term, constant** |
| `SURF_CPU_DL_WAIT` | 1–2 | 1 | blocking vCPU→pfifo round-trips |
| `SURF_CPU_READ` | ~4,600 | ~23,000 | guest CPU reads of live surfaces |
| `SURF_CPU_WRITE` | 0 | 0 | **reads, NOT writes** (kills the grain-write theory) |
| `TEX_HASH` | 34 | ~310 | full-texture hashes (**not** 4,600) |
| `TEX_HASH_KB` | ~4,590 | ~35,000 | **MB/frame hashed — the slope term** |
| `2D_BLIT` | 0 | 0 | grain is not blit-composited |
| `SURF_TO_TEX` | 26–356 | 28 | surfaces sampled as textures (variable) |
| `SURF_TO_TEX_FALLBACK` | 1 | — | RTT slow-path (low here) |
| `BEGIN_ENDS` / `QUERY` | ~2100 / ~188 | ~2100 / ~185 | draw fragmentation |
| mspf | 44–48 (noisy 22–48) | 47–51 | |

**Established facts:**
1. The 44 ms floor **reproduces on our rig** and is **scale-independent**
   (surface_scale 1 == 4, mspf unchanged) — it is NOT fill-rate; the GPU is
   mostly idle. Community machine and ours agree because the cost is
   workload-structural, not hardware.
2. The floor is **read-triggered surface sync**: 4 downloads + 4 full pipeline
   finishes per frame. The data copied is tiny (~32 KB); **the drain is the
   cost, not the data.**
3. The slope is **texture hashing volume**, which jumped 4.5 → 35 MB/frame the
   instant the radar went live and scales further with combat.
4. **The reader was SEEN:** dumping the 128×128 CPU-read surface (`0x03830000`)
   to PPM rendered a **radar sweep** — VT at a red origin, forward scan cone
   (red→white range falloff), blue leading edge. The game renders this then
   scans it row-by-row (4.6K reads quiet, 23K radar-live) to composite the
   cockpit radar with contact blips. Image at
   `scratchpad/helper-surface.png` (session artifact).

---

## 3. REFUTED / superseded (the falsification trail — don't re-chase)

- **Grain is CPU-written over the framebuffer** → REFUTED. `SURF_CPU_WRITE=0`;
  grain is among the ~2,000 GPU draws, not a CPU write. ISS-B15's original
  "CPU-write tax" framing is wrong; the tax is real but READ-triggered.
- **Gamepad / USB-DMA polling causes the reads** (owner hypothesis) → REFUTED
  by the access logger: no fixed-address descriptor-sized reads; the reads are
  scattered len=1 across one 128×128 color surface. (Good hypothesis — the
  logger was built specifically to discriminate it.)
- **Intentional framebuffer readback** → REFUTED. Not the 640×480 main
  surface; a 128×128 helper.
- **"4,600 hashes/frame"** (parallel session's worst case) → REFUTED. It's
  4,600 cheap len=1 READS that coalesce to 4 downloads; only **34** full
  hashes. Their *alternative* ("many lookups, few hashes") was correct.
- **Scale-dependence** → REFUTED for the floor (scale 1 == 4).
- **Predictive-readback v1 (download at flip)** → too late: the game draws AND
  reads the radar within the same frame, so the stall already fired before
  flip. Retired; v2 moved the download to surface-unbind.
- **"It's definitely the radar alone"** → NOT PROVEN. `0x03830000` is the only
  *small* color surface hitting the download path, but per-surface attribution
  (`XEMU_SURF_ATTR`, built) never produced a clean table — the front-view
  camera and SUB/MAIN monitors are not ruled out. State it as "a small
  render-then-read buffer, imaged as the radar," not "the radar is the whole
  cost."

---

## 4. Instrumentation & tooling built (all on this branch, env-gated)

**Profiler counters** (XMAC in `debug.h`, auto per-frame reset, auto HUD):
`SURF_CPU_READ`, `SURF_CPU_WRITE`, `SURF_DMA_WRITE` (len>8 discriminates DMA
from stores), `SURF_CPU_DL_WAIT`, `2D_BLIT`, `SURF_TO_TEX_FALLBACK` (filled the
VK blind spot — GL counted it, VK never did), `TEX_HASH`, `TEX_HASH_KB`,
`SURF_PREDL`. Plus per-surface `reads_frame`/`dl_frame` on the binding.

**Env-gated diagnostics:**
- `XEMU_SURF_ACCESS_LOG=1` — capped 200-line per-access logger:
  direction / addr / offset / **len** / surface addr+dims / color-vs-zeta /
  draw_dirty. Names the reader without drowning the log.
- `XEMU_SURF_DUMP=auto` (or `<hex addr>`) — writes every small (≤512²)
  CPU-read/downloaded color surface to `~/surf-dump-<addr>-<WxH>.ppm`. This is
  how we SAW the radar. Address moves per boot → use `auto`.
- `XEMU_SURF_ATTR=1` — per-surface read/download attribution line every 30
  flips (built; not yet cleanly captured — the isolation blocker).
- `XEMU_NO_PREDICTIVE_READBACK=1` — kill-switch for the fix (for A/B).
- H6: 60-frame stderr summary of mspf + non-zero counters → **flat runs
  self-log their own diagnosis**, no HUD needed.

**Fix prototype:** predictive readback v2 — surfaces CPU-read within 60 frames
get eagerly downloaded at **surface-unbind** (draws done, reads not yet
started), once/frame, pfifo context. `SURF_PREDL` fires but improvement
UNVALIDATED (no clean A/B).

**Workflow tooling:** `flat-launch.sh` gained `XEMU_BIN` / `XEMU_LOADVM`
overrides; a git **worktree** `~/xemu_vr-research` isolates branch builds from
the rig's main checkout and the other session.

---

## 5. Candidate fixes (ranked by leverage)

1. **Predictive readback — A/B RUN 2026-07-25: REFUTED (no improvement).**
   Live radar scene, runtime toggle, identical frame: mspf ~51 with predictive
   ON *and* OFF; `FINISH_SURFACE_DOWN=4` unchanged in both arms. **Why it
   failed:** the prototype pre-downloads via `pgraph_vk_surface_download_if_dirty`
   → `download_surface` → `pgraph_vk_finish` — a FULL GPU drain. So it just
   MOVES the drain from mid-frame-read to unbind-time; same 4 drains, same cost.
   Also revealing: only 1 of the 4 SURFACE_DOWN finishes was CPU-read-triggered
   (`SURF_CPU_DL_WAIT=1`) — the floor is NOT mostly the radar reads. **The real
   fix must ELIMINATE the drain, not retime it:** fence only the specific
   submission that wrote the surface (a narrow VkFence wait), not
   `pgraph_vk_finish` (whole-pipeline idle). Redirect here.
2. **Dirty-range narrowing** (parallel session's design) — attacks the SLOPE
   (hashing). `mark_textures_possibly_dirty` flags every texture overlapping
   the page range; intersecting against each texture's actual byte range would
   cut most false-positive re-hashes. Sub-page tracking is correct-but-invasive
   (QEMU's dirty bitmap is page-based).
3. **Combine** — the real win is making the radar buffer's read+sample stop
   forcing full GPU drains AND stop dirtying neighbors. Both are upstreamable.
4. **#2537 port** (mission-transition FMV livelock) — separate symptom (hang,
   not slow), cheap insurance; confirmed absent from our tree, directly
   portable. Queued regardless.

---

## 6. Open questions / blockers

- **Isolate the reader precisely** — run `XEMU_SURF_ATTR` to get the
  per-surface read/download table (radar vs camera vs monitors). Blocked only
  on a clean capture.
- ~~**Validate predictive readback**~~ — CLOSED: predictive readback was
  REFUTED (§5); no A/B to run.
- **RESOLVED: SBC save states won't CLI `-loadvm`** — a state saved with the
  Steel Battalion controller bound fails CLI restore (`Unknown section
  'usb-steel-battalion' at 1.3`) and silently boots fresh, because the SBC USB
  device isn't instantiated at the vmstate's expected hub path at restore time
  (xemu binds input devices late). **Worked around (2026-07-26): runtime
  monitor `loadvm`** — by the time the monitor accepts commands the device
  exists, so restore succeeds; this is what drives the mission-content A/Bs
  now (§8.7). Full write-up in `runbooks/save-state-testing.md`. The
  runtime-toggle path (one live drive-in, both arms via a watched file) remains
  the alternative and is what `ab-interleaved.sh` uses.
- **Rig ssh dropped mid-command repeatedly this session** (exit 255) — several
  measurements died to connectivity, not code. Use detached (`setsid`/nohup)
  launch + separate short harvest commands.

---

## 7. Broader graphics-risk priority map (from the two-agent sweep)

Beyond the perf floor, the sweep catalogued what else can bite SB. Kept for
when new content (deep campaign, night missions) reaches new code.

- **P1 — Mission-transition FMV livelock → port #2537.** SB does a D3D reset +
  FMV briefing at every mission boundary = the #2537 trigger (vblank misses
  the frame counter → pushbuffer waits forever on a flip). Hang-flavored, not
  slow. Confirmed absent from our tree; small Conexant vblank-counter fix.
- **P4 — Assert-hardening (the next #1238s), ranked:** pushbuffer errors
  (`pfifo.c:426/443`, HW-accurate recovery sketched in comments — cheapest
  high-value log-and-survive); shadow cluster (`pgraph.c:2982`,
  `glsl/psh.c:654/678/690`); linear-texture levels≠1 (`vk/texture.c:210`,
  unshielded sibling of the fixed #1238); color_format holes
  (`texture.c:261/266`, before our diagnostic); legal-but-unmapped surface
  formats 0x05/06/07 (`vk/surface.c:1332`); DMA-limit assert family
  (texture.c:99/137, pgraph.c:2887 semaphore runs every frame, vk/vertex.c:208,
  vk/blit.c:111).
- **P5 — scale-4 (VR) exposures:** zpass counts ÷ scale² truncate to 0
  (`vk/reports.c:113-132`) → occlusion/lock-on logic silently changes at
  scale 4; #2174-class 0.5-rounding shadow/decal acne (`glsl/vsh.c:228-233`,
  `psh.c:1007/1041`, no upstream fix).
- **P6 — watch:** #2902 (Vulkan-only menu/text corruption, our base window —
  eyeball SB menus); FMV color/aspect (#2565/#2254/#2388); smoke #2478
  (portable if SB smoke shows black splotches); #2691/#2862 vsync-readback
  (we carry the decouple, not the fix — AMD-centric, rig is NVIDIA);
  signed-texture/bump #587 (only if metal looks flat). De-risked: Dino Crisis
  3 GL framebuffer crash — VK immune.

---

## 8. Community field evidence (the mission-3 slowdown, verified)

Owner personally watched a recorded demo (another machine/build) of mission 3
running slow, and supplied four screenshots with xemu's Video Debug overlay:

| Scene | FPS / MSPF |
|---|---|
| Empty grassland, no combat, x1.00 | 19 / 44 |
| Forest combat + effects | 7 / 104 |
| Urban, heavy smoke, x6.00 zoom | 6 / 141 |

The empty-field 44 ms = the constant floor (§1); the climb to 141 ms tracks
scene complexity = the hashing slope. Both reproduced/explained above. The
community build necessarily carries a #2514-class fix (SBC gameplay requires
the unmerged PR #1803), so the slowdown coexists with that fix — consistent
with the tax model.

---

## 8.5 TIER-1 RECON RESULT (2026-07-26): the radar scan, located in guest code
Static analysis of default.xbe (title 0x43430002, tools/perf/xbe_tool.py):
- **Radar buffer base pointer = guest global `0x351bdc`** (VERY HIGH conf;
  10 refs in 6.6MB, all reads; offset math reproduces the measured trace
  exactly: row13*512+col11*4 = 0x1a2c).
- **Four twin scan-AND-draw functions** 0x25d40/0x26460/0x26b80/0x27250
  (one RGBA channel each), called 1x/frame from HUD dispatcher 0x450c0;
  inner cmp-byte loop + outer row stride 0x200 match all measured
  signatures. Lock-on/targeting uses a SEPARATE point-check helper
  (0x21dd0, 19 callers) that stays live if scanners are skipped.
- **Key wrinkle:** scanners output IMMEDIATE-MODE D3D DRAWS (no persisted
  result buffer). Skip-N therefore = "blips not drawn that frame"; visual
  outcome (smooth-stale vs flicker) depends on persistent-RT vs
  backbuffer — ONLY testable at runtime (hook skip-2, look at radar).
  Fallback if flicker: cache+replay the draw calls.
- **PC-capture patch ready** (docs/perf/scan-pc-capture.patch, applies
  clean to cputlb.c; XEMU_SCAN_PC_CAPTURE=64): logs deduped EIP+regs of
  every distinct reading instruction via cpu_restore_state — settles the
  scanner-vs-helper attribution and hands the hook addresses.
- **Skip hook MERGED (2026-07-26, perf-radar-skip → 8fa7778be2), not yet
  executed:** breakpoint hooks on the four scanner entries, cadence
  live-tunable via `/tmp/radar-skip-n`; the smooth-stale-vs-blink question
  above is the pending owner-eyes gate. Lock-on (helper 0x21dd0) stays live
  by design, so skipping the scanners cannot break targeting.

## 8.7 FOUR-WORKSTREAM CAMPAIGN MEASURED (2026-07-26, cutscene + REAL MISSION via monitor-loadvm)
Four parallel Opus implementations merged (c9aa0025a7): exact-dirty
hash narrowing (default ON at merge — since flipped to opt-in
`XEMU_EXACT_DIRTY=1` / default OFF, see the flicker regression below),
async query readback (XEMU_ASYNC_QUERIES — since hard-OFF, ISS-B16),
TLB read fast path (XEMU_TLB_FASTPATH), Tier-1 scan recon (committed).
Toggle-ladder run TWICE — intro cutscene AND real mission content
(snapshot restored at runtime).

**BREAKTHROUGH WORKFLOW: runtime monitor `loadvm` WORKS** where CLI
-loadvm fails (device tree exists by then). `-monitor
unix:/tmp/xemu-mon.sock,server,nowait` + `echo "loadvm <tag>" | nc -q 8
-U` = unattended repeatable MISSION-CONTENT measurement. (nc without -q
hangs forever on the never-closing socket — field hit.) Runbook updated.

| leg | cutscene TEX_HASH_KB | mission TEX_HASH_KB | mission STALLED | mission mspf |
|---|---|---|---|---|
| stock | 34,000 | 34,700 | 3 | 30-49 |
| +exact-dirty | **13,000 (-60%)** | 34,700 (**no change**) | 3 | 30-34 |
| +async-queries | 13,000 | 35,500 | **0** (ASYNC=3) | **28 steady** |
| +TLB (all) | 13,000 | 35,400 | 0 | 29-31 |

(These legs' "zero VK errors" greps were later found BLIND — validation
output wasn't being surfaced; the authoritative VK check is the ISS-B16
sync-validation run.) **The per-feature reading below was overtaken by the
flicker regression that follows: async is now hard-OFF and exact-dirty
opt-in. Kept as the measured record.** Interpretation as-measured:
1. **Async queries: mechanism absorbed the walls in real combat** — STALLED
   walls fully absorbed; mspf steadied ~28-31 vs stock 30-49 wobble. Real,
   modest (~2-3ms class). **(SUPERSEDED — introduced light flicker, now
   hard-OFF; ISS-B16.)**
2. **Exact-dirty: scene-dependent.** Cutscene/menu-class content: -60%
   hash volume (false-positive page aliasing). Mission content: ~0% —
   the 35MB/frame there is GENUINE (animated effect textures rewritten
   per frame ⇒ byte-overlap real ⇒ must hash). Wins where they exist,
   costs nothing where they don't — **but the default since flipped to OFF /
   opt-in, decided by the mission-content flicker, not the perf (ISS-B16).**
3. **TLB fast path: stable in mission** (its win is guest-thread time;
   isolating it needs thread-busy sampling, not mspf).
4. Remaining mission-scene budget is where the model said: the TCG
   guest-work wall (93%-busy thread) — Tier-1 skip-N (scan routines
   located at 0x25d40/0x26460/0x26b80/0x27250, PC-capture patch staged)
   is the lever aimed at it.
**FIELD REGRESSION (2026-07-26, owner-bisected TWICE): mission-scene
light/texture flicker had TWO INDEPENDENT SOURCES.** Full bisect table:
| exact-dirty | async-queries | TLB | verdict |
|---|---|---|---|
| ON | ON | ON | flicker |
| ON | off | ON | flicker (source 1 visible) |
| off | off | ON | **CLEAN** (TLB exonerated) |
| off | ON | ON | **flicker again** (source 2 = queries) |
**Source 1 — exact-dirty (FIXED by default-off):** co-page false
negative — animated textures sharing a page with a download span got
genuine updates skipped. Default now opt-in (XEMU_EXACT_DIRTY=1).
Proper re-enable design (channel separation): downloads/blits STOP
setting DIRTY_MEMORY_NV2A_TEX for their ranges and are consulted ONLY
via exact spans; the page bitmap then carries ONLY unattributed writers
(guest CPU) — byte-exact for our writers, page-conservative for
everyone else, no false negatives possible.
_v2 IMPLEMENTED (branch perf-exactdirty-v2, 43d5dda2ea), PENDING rig
validation._ note_exact_dirty now returns whether it recorded the span;
the writer sets the page bitmap ONLY on false (disabled/overflow), so
every exact write lands on exactly one channel. exact_spans_valid is
never cleared mid-frame (that would orphan an already-bitmap-skipped
span → corruption); overflow degrades that one write to the page bitmap;
the guest-store-to-live-surface invalidation is removed (the store is
already on the page bitmap via softmmu notdirty — DIRTY_CLIENTS_NOCODE
includes NV2A_TEX). Still default OFF; OFF byte-identical to stock.
**NEW ROOT CAUSE (likely the bigger win) — VK possibly_dirty cache-HIT
leak, FIXED default-on (b662534cea).** The VK backend cleared
snode->possibly_dirty ONLY on the cache-MISS path; the cache-HIT path
returned with it still set. So any node flagged possibly_dirty once —
and pgraph_vk_flush marks ALL of VRAM possibly-dirty — re-hashed on every
subsequent bind FOREVER (the GL backend clears it unconditionally, hit or
miss). This directly retrodicts the −60%-cutscene / ~0%-mission split
above: narrowing the MARKING cannot un-stick a flag already stuck, and
resident mission textures (cache hits) stay poisoned while cutscene
churn recycles nodes through the miss-path clear. Fix clears on hit too;
output-identical (uploads still gated by content_hash != snode->hash),
removes redundant HASHING only. XEMU_NO_TEX_DIRTY_CLEAR=1 reverts for
same-binary A/B. PENDING: measure whether this alone collapses the
`texhash` pfifo bucket / TEX_HASH_KB — if so, channel separation is a
smaller add-on (or unneeded); if texhash persists, both are wanted.
**Source 2 — async-queries · hard-OFF, no working fix (redesign pending).**
The first-bisect suspect was query-pool slot reuse (a GPU write/reset hazard
the old full drain masked); that theory and three successors were all
subsequently killed — owner-fence PLACEBO, stale-latch spec-fix FALSIFIED,
deferred-recycle hazard sync-val-CLEARED. Current working conclusion:
resolve-latency TIMING VARIANCE, not a wrong value; destination = the
deferred-epoch ring. Full graveyard + evidence: **ISS-B16**. Feature stays
OFF; the automated flicker detector (`tools/perf/flicker_detect.py`, landed —
docs/perf/flicker-detect.md) is the arbiter.
Counter-lesson stands: visual canaries catch what mspf cannot; and a
bisect is only done when the LAST variable flips alone.

Caveat: the measurement snapshot is one-hit-from-death (owner note);
legs validated as pre-death by signature continuity (~34-35MB hashing
throughout = mission workload for the full sample).

## 8.9 SIX-AGENT PRE-FLIGHT + LIVE VERIFICATION (2026-07-26) — radar-skip GO
Before the owner's live cadence test, six medium-effort agents fact-checked
every uncertainty; ALL GREEN:
- **Trap code** (adversarial review): crash/hang surface clean; caveats =
  title-latch (set N only after SB runs) + cosmetic counter race.
- **XBE ground truth** (independent re-derivation): all four scanners,
  zero-arg C3-ret safety, single call sites, lock-on separation CONFIRMED;
  call-site pairing corrected (set identical); render-state leakage NAMED
  cosmetic-only (texture-filter field on two HUD icons) and the game itself
  natively skips scanners 3/4 via guest flag [0x356ae0].
- **Rig dry-run**: full procedure executed unattended — arm lines, counter,
  cadence sweep, silent loadvm re-arm, disarm — ZERO crashes; machine
  pre-read shows blips PERSIST (no blink) at 16-21 fps; save self-fails
  ~35 s unattended; idle-cockpit FPS unchanged (guest-bound), render thread
  −7pp.
- **Gate audit**: every feature verified inert-by-default; predl default-ON
  MATCHES the verified baseline (empirical: clean-run logs); stale
  watch-file hazard found → ring-launch now reports watch-file state.
- **Theory adversary**: killed the timing-variance conviction (see ISS-B16
  addendum) — detector separations were capture-beat artifacts; per-present
  dump is the decisive future experiment; deferred-epoch ring NOT justified.
- **Docs coherence**: nine contradictions fixed; current-state-first.
**Triple corroboration closed live:** [0x356ae0]=0 peeked via monitor ≡
counter K=2 ≡ disassembly gate. **End-to-end launch verification passed**
(toggle states, loadvm, arm/tick/disarm all fired on the shipping binary).
Operational runbook: runbooks/radar-skip-test.md.

## 9. THE A/B PROCEDURE (runtime toggle — one drive-in, no loadvm)

**SUPERSEDED (2026-07-26):** the fix this validates (predictive readback) was
REFUTED (§5), and the generalized runtime-toggle-A/B technique now lives as
`tools/perf/rig/ab-interleaved.sh` (interleaved ABABA, per-segment extraction).
Kept as the record of the toggle-A/B method.

Predictive readback is now flippable live via a watch file, so validating it
needs ONE session that you drive to a radar-live scene:

1. I launch the counter binary (worktree build) with H6 summary on.
2. You drive to a live-radar mission scene (game needs the SBC).
3. Say "radar's live." I then, on the rig, with NOTHING to rebuild:
   - `rm -f /tmp/predl-off` → predictive **ON**  (log prints `predl: ON`)
     — capture ~10 s of H6 lines (watch `SURF_PREDL>0`, note mspf)
   - `touch /tmp/predl-off` → predictive **OFF** (log prints `predl: OFF`)
     — capture ~10 s (`SURF_PREDL=0`, note mspf)
   - flip a couple more times to average out scene wobble.
4. Same scene, same binary, same frame content — the ONLY variable is the
   fix. mspf(OFF) − mspf(ON) = the fix's worth. `SURF_PREDL` and the
   `predl:` log lines mark each arm unambiguously.

Env: `XEMU_PREDL_TOGGLE_FILE` overrides the path; `XEMU_NO_PREDICTIVE_READBACK=1`
force-OFF. Built 2026-07-25, worktree `~/xemu_vr-research`.

## 10. Suggested execution order (next session)

Predictive-readback items are retired (§5 REFUTED). Live state is in the
Current-state banner above and CURRENT_WORKING_LOG.md; the durable
perf-research queue:
1. **Radar skip-N live cadence test** (merged, `/tmp/radar-skip-n`): drive to a
   radar-live mission, step skip-1..N, judge blip persistence vs blink (§8.5).
   Owner-eyes gate — the one pending live test.
2. **Deferred-epoch-ring redesign** for async queries (ISS-B16 destination),
   after the adversarial review of the timing-variance conclusion.
3. **Exact-dirty re-enable** via dirty-channel separation (§8.7 source 1), to
   reclaim the parked −60% cutscene hash win without the co-page flicker.
4. Capture `XEMU_SURF_ATTR` in the radar-live scene → the per-surface table
   (still the open reader-attribution blocker, §6).
5. Port #2537 (independent, cheap); assert-hardening batch (P4) as
   log-and-survive.

---

## 11. THE NARROW-FENCE FIX — build-ready spec (5-agent synthesis, 2026-07-25)

**STATUS (2026-07-26):** both staged steps LANDED. The read fast path split out
of this work and SHIPPED (always-on, the big win — see the V2 iteration below);
the narrow-fence ring itself is RETAINED default-OFF — the async early-submit
treatment measured no-win twice, but the architecture is the prerequisite for
the deferred-epoch-ring async-query redesign (ISS-B16). The build-ready spec and
its invariants are kept in full below as the reference for that redesign; the
"Landing plan" at the end is the original plan, now executed.

Five parallel investigations (drain anatomy, fence-tracking, batching, hazards,
prior art) converged on this design. **It is original work — upstream's
download path is byte-identical to ours** (prior-art agent confirmed).

### The problem, precisely
The VK renderer is FULLY SYNCHRONOUS: one `command_buffer_fence`, waited inside
every `pgraph_vk_finish` (draw.c:1280), plus a `vkQueueWaitIdle` in every
single-time copy (command.c:104). SB pays ~4 whole-batch drains + copy idles
per frame ≈ 40 ms. `pgraph_vk_finish` is NOT device-idle — it flushes the whole
open command buffer and waits its fence — but since everything is in one buffer,
that's a whole-pipeline drain. **A per-surface fence buys nothing unless the
synchronous drain in finish is also removed** (async submit) — that is the
crux, and it's why predictive *readback* (retiming the drain) failed; the win
is predictive *submission* (submit early, let the GPU run it during CPU work,
wait a near-signaled fence at read time).

### The mechanism (fence ring, option b — timeline semaphores not enabled here)
- **SurfaceBinding.`last_write_submit`** (uint32) — `r->submit_count` of the
  submission that last wrote it; `SURFACE_NO_WRITE`=0xFFFFFFFF sentinel.
  Mirrors the existing `TextureBinding.submit_time` idiom.
- **`cb_fence_ring[N]` of `{VkFence fence; uint32_t submit_index}`** replacing
  the single fence. Recycle detected by identity (`submit_index != idx` ⇒
  complete). N ≥ max in-flight submissions.
- **Set-site (one choke-point):** stamp `last_write_submit = r->submit_count` in
  `pgraph_vk_set_surface_dirty` (draw.c:1844) — hit by both draw_end AND clear.
- **Submit-site:** ring discipline in finish (draw.c:1261) — wait the prior
  occupant of the slot before reuse, submit against the slot's fence.
- **Read-site:** `pgraph_vk_wait_for_surface_write(pg,s)` — Case 1 (write still
  in open CB → finish/submit it), Case 2 (submitted → wait ONLY that fence,
  with 3 short-circuits: NO_WRITE / slot-identity-mismatch / GetFenceStatus).
- **Aux copy fence:** replace command.c:104 `vkQueueWaitIdle` with a real
  per-submission `VkFence` wait; the CPU map waits THIS copy fence.

### The 10 invariants any implementation MUST hold (hazard agent)
1. Flush-before-wait — narrow the WAIT, never skip the SUBMIT.
2. Wait on the last WRITER of the mapped bytes.
3. Wait on the LATEST submission touching the surface.
4. Cover the CONVERSION submission — depth/stencil compute-pack + scale>1
   downscale blit, not just the render (test {scale 1,>1}×{D16,D24S8}).
5. Keep host-visibility — the HOST_BIT barrier + `vmaInvalidateAllocation`.
6. Download stays synchronous end-to-end — RAM written + dirty bits set before
   `downloads_complete` fires (the #2514 round-trip, blit, savevm depend on it).
7. All VK queue/fence ops stay on the pfifo thread; vCPU only does the event
   round-trip. Change WHAT is waited, not WHICH thread waits.
8. Eviction (surface.c:804) waits on last USE, not last WRITE (image may be
   in-flight as texture source/attachment). Highest-risk site; keep the
   "Surface evicted while in use!" assert LIVE.
9. Savevm leaves no dirty surface behind — no global "flushed once" shortcut.
10. Compute descriptor pool must still be recycled (else pool-overflow assert).

### Per-site: NARROW vs KEEP
NARROW: surface.c:182 (download), command.c:104 (aux copy). REPLACE-with-pool-
reset: surface.c:183, texture.c:613 (compute pool, not a data hazard). KEEP-but-
on-last-USE: surface.c:804 (eviction). KEEP (out of scope): surface.c:1122
(upload), display.c:909 (present), and all NEED_BUFFER_SPACE/VERTEX/FLIP/FLUSH.

### The load-bearing hard part (agent 2 "outside charter", not yet fully designed)
Removing the drain means the post-finish RECYCLING now runs while GPU work is
in-flight: `destroy_framebuffers` (draw.c:1285), `descriptor_set_index=0`
(1283), staging-buffer reuse, query pool, `uploaded_bitmap`. All must be
deferred/double-buffered per in-flight submission (ring-depth-bounded). **This
is the piece to design carefully before flipping to async — a UAF here is a
GPU crash, not a torn pixel.**

### STATUS (2026-07-25 late): step 1 LANDED + smoke-tested
The fence-ring scaffolding is implemented (commit on branch), builds
clean, and RUNS on the rig with **zero VK validation errors, zero
asserts, zero segfaults** — the risky core-renderer surgery (per-
submission fence ring replacing the single command_buffer_fence inside
pgraph_vk_finish + last_write_submit tracking + wait_for_surface_write)
is proven functional and behavior-identical (drain retained). Full
golden-image identical-to-baseline verification (radar scene) pending a
driven session. **Step 2 (the async flip) is deliberately NOT started —
it needs the recycle-gating designed carefully, not rushed.**

### ASYNC FLIP: LANDED + A/B MEASURED 2026-07-26 — architecture PROVEN, first treatment NO WIN
All three commits landed (8f9124f90f single-time fence, e5880f2bae ring
CBs + submit/finish split + delta-sync + UAF/WAR gates, 88a46ea80c
toggle + early-submit). Build green; ran a live SB scene for minutes
across OFF->ON->OFF transitions with **zero VK validation errors, zero
asserts** — the multi-submission architecture (per-slot CBs/semaphores,
delta-synced staging, recycle deferral) is functionally correct.

**A/B (probe scene, single boot, /tmp/narrowfence-on live-toggled):**
| arm | mspf | FINISH_SURF_DOWN | QUEUE_SUBMIT | DL_WAIT | PREDL |
|---|---|---|---|---|---|
| OFF (early) | 22–31 | 4 | 7–8 | 1 | 0 |
| ON | 48–66 | 4 | 9 | 2 | 1 |
| OFF again (ABA) | 39–40 | 4 | 9 | 2 | 0 |

ABA shows the scene itself got heavier mid-run (OFF-again never returned
to the first baseline — the probe scene evolves), so the first OFF arm
overstates the delta. Against the like-for-like OFF-again: **ON is
~10–25 ms WORSE, never better.** Honest negative.

**Why no win (mechanism, from the drain census):**
1. Only ~1 of the 4 SURFACE_DOWN drains is the CPU-read path the early
   submit targets. The others (eviction at surface.c invalidate, etc.)
   still call FULL pgraph_vk_finish — and every full finish also retires
   the early-submitted batch, so the GPU never got useful background
   time before something drained it anyway.
2. The new conservative safety gates now BLOCK ON the in-flight batch:
   upload_surface_data waits all slots (SURF_UPLOAD=2/frame!), texture
   upload waits its sampler's submission. With a batch in flight these
   fire every frame — adding mid-frame drains where OFF-mode had none.
   The safety envelope ate the treatment.

**Next iteration (designed, not built):** (a) narrow the eviction-path
drain via per-surface last-USE tracking (stamp submit_count at
bind-as-texture/attachment; eviction waits only that submission);
(b) make the upload gates range/identity-narrow instead of wait-all;
(c) only then does the early submit get room to pay. Default stays OFF —
OFF-mode is verified behavior-identical, so the landed architecture is
pure upside for future work at zero risk to current users.

### V2 ITERATION MEASURED 2026-07-26 — READ FAST PATH: BIG WIN; async narrowing: still negative
Two more commits (0f76167240 read fast path, e503680b6f narrow all
download-class drains + fix the wait-all gates). Interleaved ABABA A/B
(15 s arms, per-segment extraction — defeats the scene-drift confound):

| arm | mspf | FSD | QS | trapped reads/frame |
|---|---|---|---|---|
| OFF | 23.0 | 4.0 | 6.3 | **1** |
| ON  | 35.4 | 2.9 | 7.6 | 2 |
| OFF | 34.2 | 4.0 | 8.0 | 2 |
| ON  | 37.6 | 3.0 | 8.0 | 2 |
| OFF | 32.3 | 4.0 | 8.0 | 2 |

**1. READ FAST PATH (unconditional, always on): PROVEN.** Trapped
surface reads collapsed **~23,000 → 1-2/frame** (the quiesced-callback
skip at the physmem walk). The one dispatched read per frame is the
first dirty read — the download protocol intact (dlw=1-2 ✓). This
removes the measured 2-5 ms trap tax + the vCPU-vs-pfifo pgraph.lock
contention, and it SCALES: radar-live/combat had 23K reads/frame.
Zero VK errors. This is the literal "cache hit" fix, landed and live.

**2. Async narrowing (toggle ON): consistently ~4-7 ms WORSE than
interleaved OFF neighbors**, despite verifiably removing ~1 full drain
(FSD 4→~3, of which ~1 is the early submit itself). **The wall
analysis:** the frame has ~8 serialization points (4 SURFACE_DOWN class
— now narrowed — plus 2-4 STALLED query-resolution finishes, arena
NEED_BUFFER_SPACE, FLIP_STALL). Every remaining full finish
wait-ALL-slots — so early-submitted work is re-absorbed by the next
untouched wall, and the extra submissions are pure overhead. Narrowing
k of n walls pays ~nothing until the LAST wall between producer and
consumer falls. The dominant untouched wall: **STALLED occlusion-query
resolution** (reports.c:158, 2-4/frame — SB polls zpass counts for the
targeting reticle). Narrowing it = async query readback (poll
vkGetQueryPoolResults on retired-submission ranges without draining) —
a distinct, well-scoped future redesign; not attempted tonight.
**(Since attempted as `XEMU_ASYNC_QUERIES`: it absorbed these walls but
introduced light flicker and is now hard-OFF; the redesign that survives is
the deferred-epoch ring — ISS-B16.)**

**Config verdict:** read fast path default-ON (pure win, no toggle);
narrowfence default-OFF (honest negative twice measured); all
architecture retained — it is the prerequisite the query-readback
redesign will build on.

### Landing plan (original — both stages have since landed; toggle-gated `/tmp/narrowfence-on`, default OFF)
1. **Scaffolding (behavior-identical, safe to land with the drain retained —
   agent 2 confirmed):** the fence ring + `last_write_submit` + set/submit/read
   sites + aux fence. With the drain still present every submission completes
   before the next, so the ring always finds a signaled slot; mspf and
   golden-image PPMs must be IDENTICAL to baseline (that's the correctness
   proof of the scaffolding).
2. **The async flip (the win, careful):** remove the finish drain + the
   recycle-gating above + predictive submit at unbind. Validate with VK
   validation layers ON, keep asserts live, golden-image PPM diff of the radar
   surface (nonzero diff = torn), depth/stencil×scale matrix, savestate diff —
   all A/B'd against the OFF baseline via the toggle.

### Validation kit (all already built)
`XEMU_SURF_DUMP=auto` (golden-image PPM diff), `XEMU_SURF_ATTR`, VK validation
layers, the live watch-file toggle, `SURF_CPU_DL_WAIT`/`FINISH_SURFACE_DOWN`
counters, H6 self-logging summary.
