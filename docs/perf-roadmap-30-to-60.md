# Roadmap: Steel Battalion 30 → 60 FPS — getting guest-CPU cost down

> **⚠ THIS IS THE CHRONOLOGICAL LAB-NOTEBOOK** (predictions, corrections, and
> reversals in the order they happened — kept as the falsification trail).
> **The authoritative synthesis is [`road-to-60.md`](road-to-60.md) — read that
> first.** This file is the evidence behind it.


**2026-07-26.** The read-sync campaign took SB 20 → 30 FPS by removing
emulator *paranoia* (redundant sync/checking). That well is now mostly dry.
The remaining wall is a different class of problem, and this roadmap attacks it.

## The wall, quantified (measured this session)
- Guest-CPU (TCG) thread **~94% busy ≈ 31 ms of executed guest instructions
  per frame**. pfifo/render thread ~51%, GPU 50-72%, memory ~4%.
- Scene is **guest-CPU-bound**: surface_scale 1 == 4 (fill-rate irrelevant),
  GPU has headroom, and blocking surface downloads run on the PFIFO thread
  (guest SLEEPS during them → they show as the idle 6%, NOT the 94%).
- Therefore the target is literally **executed guest instructions**. To hit
  60 we need that ~31 ms under ~16.7 ms — a **~1.85× guest-thread speedup**.
- What runs in that 31 ms (hypothesized, UNMEASURED — Phase 0 settles it):
  (a) game logic/physics/AI; (b) the statically-linked **XDK Direct3D8
  driver** building the pushbuffer (~2,100 draws + ~1,000 constant-uploads
  per frame — huge); (c) **spin-waits** (SB busy-polls report RAM — a spin
  loop is 100% "busy" under TCG, burning real cycles); (d) TB re-translation
  from TLB-flush churn; (e) the radar scan (~2-5 ms, skip machinery built);
  (f) MMIO trap overhead (pushbuffer kicks, register pokes).

## Phase 0 — THE MISSING INSTRUMENT (non-negotiable, do first)
Everything below is gated on this. Build a **statistical guest-PC profiler**:
a periodic timer on the vCPU thread samples `current_cpu`'s `env->eip`, bins
it, and symbolizes against the XBE (we HAVE default.xbe + xbe_tool.py +
capstone disassembly + the PC-capture patch as the seed). Alternatively check
whether xemu's QEMU has the **TCG plugin API** enabled (`-plugin`) — a
bb-vector plugin gives exact hot-function counts.
**Deliverable: the ranked 31 ms breakdown** — game-logic vs D3D8-driver vs
spin vs re-translation vs radar vs MMIO. This decides which Phase-1
workstreams are real and which are ghosts (M-1/M-8: never guess where time
goes). Add a spin-detector probe: sample whether the sampled EIP sits in
tight polling loops (same small address range, no forward progress) — spin
fraction is the highest-value single number.

## Phase 1 — Incremental levers (each launched ONLY if Phase 0 confirms it's fat)
Ranked by expected payoff × tractability:

- **H-A · Spin-wait short-circuit.** If a fat slice is the guest busy-polling
  report/semaphore RAM the pfifo thread will fill: detect the spin
  (PAUSE/tight-poll on a known address), and either yield the vCPU or
  complete the awaited value immediately. Classic emulator speedhack; cheap
  if Phase 0 shows spin is fat. Pure waste → pure win.
- **H-B · Kill the TLB/TB flush storm.** The #2514 mem-callback registers/
  unregisters surface watch-ranges with `tlb_flush_all_cpus_synced` — a FULL
  all-CPU TLB flush (the code's own FIXME: "flush only applicable pages").
  Surfaces cycle every frame in the 3-monitor cockpit → repeated full flushes
  → TLB refill + TB-chain breakage + re-translation. Fix = ranged/per-page
  flush. Emulator-WIDE benefit (every game), upstream-relevant.
- **H-C · Other per-frame hot loops.** The radar was one render-then-scan
  loop we found by accident (it touched surfaces). Phase 0's profiler finds
  the ones that DON'T touch surfaces (physics/AI/effects). Each gets the
  radar treatment: skip (temporal), or shortcut. Radar-skip itself is built —
  validate its combat win here.
- **H-D · Surface-download frame-time smoothing.** 4 blocking GPU→RAM
  downloads/frame create the heavy-frame tail (mspf 47-51 vs 31-34). Not
  guest-CPU-busy, but on the frame critical path. The naive attacks
  (predictive/narrow-fence) measured negative; the honest fix is the
  panel-designed deferred/retired-epoch approach done right, or cutting the
  download COUNT (why 4? which consumers?).
- **H-E · MMIO trap reduction.** If pushbuffer kicks / register pokes trap
  often, batch or fast-path them. Lower priority pending Phase 0.

## Phase 2 — Structural bets (parallel research spikes; high risk / high reward)
- **KVM / hardware-virtualized guest CPU (the nuclear option).** The Xbox CPU
  is a **standard x86** (Coppermine P3/Celeron); the host is x86-64. In
  principle KVM could execute guest CPU code NATIVELY and trap only to the
  emulated NV2A/MCPX — collapsing the 31 ms toward near-zero and demolishing
  the wall for EVERY game. xemu is TCG-only today (nobody built the KVM path).
  Honest showstopper list to spike BEFORE committing: device-model
  integration with KVM memory/MMIO, SMM/MTRR/cache-mode quirks, timing
  fidelity (the whole emulator assumes TCG timing), and interaction with the
  VR/pfifo threading. Deliverable: a feasibility verdict, not a promise.
  If viable, this is THE answer and everything in Phase 1 becomes moot.
- **TCG throughput.** QEMU version bump (xemu is pinned); TB cache size,
  chaining, `-accel tcg,tb-size=` tuning; check MTTCG relevance (single vCPU
  limits it, but device threads benefit).
- **D3D8 HLE (far future, philosophically heavy).** Intercept the XDK D3D8
  API and build the pushbuffer natively instead of executing the driver
  instruction-by-instruction — the only real way to cut term (b). Against
  xemu's LLE accuracy ethos; name it, don't plan it.

## Phase 3 — VR-native complement (make 30 FEEL like 90 without touching the CPU)
This is a VR fork. **Reprojection / timewarp** presents at headset rate
(72/90 Hz) from the latest rendered frame + head pose, decoupling display
smoothness from sim rate. It does NOT raise the game's sim rate, but it
removes the *perceived* judder that makes 30 feel bad in a headset — plausibly
the highest comfort-per-effort win in the actual product, and orthogonal to
all CPU work. Track in parallel.

## Honest ceiling estimate
The incremental levers (H-A..H-E) plausibly reclaim 10-15 ms of *waste*
(spin + flush-churn + radar + a hot loop or two) → ~31 → ~18-20 ms → **~45-50
FPS**. **Locked 60 in this scene almost certainly requires a Phase-2
structural bet (KVM) or the Phase-3 VR reprojection route for "feel."** Do not
promise 60 from incremental work; promise a measured, ranked assault on the
31 ms with a real shot at 45-50 and a research path to more.

## Validation discipline (the M-rules, forward-applied)
Every lever: Phase-0-confirmed-fat before building; runtime toggle + counter;
same-save A/B via the ladder (cross-boot mspf is noise); replicate before
"fixed"; owner eyes for anything visual. Per-frame instrumentation (not the
1-in-60 mspf log) where micro-timing matters.

## Suggested first move
Phase 0 profiler + spin probe — one focused build, one mission-save capture,
and we finally SEE the 31 ms. Then the ranked breakdown picks the Phase-1
fleet. Nothing downstream should start until that histogram exists.

---

## Appendix — adjacent avenues (other titles; the "three axes" framing)
Developed in discussion 2026-07-26. A CPU-heavy / RAM-starved title like **Half-Life 2 (Xbox)** decomposes into THREE independent levers — worth separating because they have wildly different risk/effort:

1. **Resolution → host GPU.** Internal render-scale is drawn on the host GPU (Vulkan), not in software — demonstrated free on SB (scale 1 == 4, CPU-bound). Available today; costs host VRAM/fill-rate + per-game high-res correctness fixes (zpass ÷scale², #2174 acne). This is the "very high res / VR-per-eye" win and it's the EASY axis.
2. **Memory/streaming → expanded emulated RAM + modified BIOS** (the "128 MB RAM-chip mod" the hardware scene does for HL2). HL2's Xbox port was severely 64 MB-starved (streaming hitches, pop-in). In EMULATION this is far easier than the solder mod: `xbox.c:194` already allocates `machine->ram_size` (a variable, not a hard 64 MB constant), and the APU/DSP read `memory_region_size(d->ram)` dynamically. Scoped experiment: (a) set guest RAM to 128 MB; (b) load a community **128 MB-aware modified BIOS** (mandatory — retail BIOS/kernel only sees 64 MB); (c) confirm/patch the MCPX/NV2A memory-controller size reporting to agree with the BIOS (the likely emulator touch-point); (d) determine whether HL2 uses the extra RAM stock (dynamic pool sizing) or needs a **game patch** (hardcoded 64 MB pools) — UNKNOWN, the experiment answers it. Bounded and testable, NOT a KVM-scale effort. Highest tractability-per-payoff for HL2 specifically.
3. **Framerate → KVM** (native guest-CPU execution; docs Phase 2). The speculative, hard, transformative bet. Best candidate = games that genuinely maxed the real Xbox CPU (HL2 qualifies). Note: the just-built `MMIO_DISPATCH` counter measures VM-exit pressure = a first read on KVM feasibility.

Order of tractability for HL2: **resolution (now) → RAM+BIOS (bounded experiment) → KVM (research spike).** Independent; pursue in order.

---

## PHASE 0 RESULT (2026-07-26) — THE ASSUMPTION WAS WRONG: it's a SPIN-WAIT on the emulated GPU, not CPU-logic cost
First instrumented capture (mission save, all three subsystems). **Reframes everything.**

**Guest-PC profile (77.5% spin fraction):**
- ROLE split: kernel/HLE **65.8%**, game/other .text 22.8%, D3D8 driver 9.6%, radar 1.2%, lock-on 0.25%.
- The kernel 65% is almost entirely ONE tight busy-wait: EIPs 0x80024a96-0x80024b81 (~64% across 4 hot addresses) — a kernel spin loop the game/driver calls to **wait on hardware (the GPU)**. Secondary spins: game `sub_10d310` (~9%), `sub_02db50`, D3D `sub_20f2c0`.
- **Actual game logic + D3D driver "real work" is a MINORITY of guest execution.** The guest mostly spins.

**Phase timers (same run):** `pfifo_work≈39ms` of a 50ms frame — the GPU-emulation thread is genuinely busy and ≈ the frame time. The guest `run` is ~full-frame but 77% of it is spin. → **the guest is spinning WAITING for the pfifo/GPU-emulation thread.**

**Overhead counters — H-B REFUTED:** `TLB_FLUSH_ALL=4`/frame (== MEMCB_REGISTER, NOT the hypothesized tens), `TB_TRANSLATE=6`/frame (no re-translation storm), `TB_RECYCLE=86`. The TLB-flush-storm hypothesis is dead — DROP H-B. `TLB_FILL=2222`, `MMIO_DISPATCH=1302`/frame (moderate; much of it is the spin polling a register), `MEMCB_READ_DISPATCH=1` (read fast-path confirmed holding).

**Caveat (honest):** the profiler forces TB-chaining OFF, which over-samples tight loops — so 77.5% is an INFLATED upper bound; true wall-time spin fraction in normal (chained) execution is lower but still first-order. Direction is robust; exact magnitude needs a chain-preserving confirmation.

### Strategic consequences (the roadmap pivots)
1. **NOT CPU-logic-bound — GPU-EMULATION-bound via a CPU spin.** The bottleneck is the pfifo thread's throughput (surface sync, pushbuffer, downloads, query resolves); the guest burns cycles spinning on it.
2. **KVM is DEFLATED for SB specifically.** Native CPU speeds up the ~22% real logic; the guest would just spin NATIVELY (fast) waiting for the same-speed emulated GPU. KVM helps far less than hoped HERE. (Title-specific — HL2 may genuinely differ; measure per-title, never assume.)
3. **New #1 lever — spin-detect-and-yield.** The spinning vCPU thread STARVES the pfifo thread of host CPU (they compete for cores). Detect the kernel spin (0x80024a96) and yield/HLT → the pfifo thread gets more host CPU → GPU work finishes sooner → FPS rises. This is roadmap H-A, now strongly promoted to #1, and it's a real frame-rate win (not just freeing a core).
4. **The pfifo/GPU-emulation optimizations were aimed at the RIGHT target** (query walls, surface downloads) — they're what the guest waits on. Worth revisiting with this understanding.
5. **On "is it a 60fps game":** it's GPU-EMULATION-bound, so the answer is neither pure-CPU nor pure-vblank — make the emulated GPU faster (or stop the guest starving it) and it climbs.

### Immediate next: chain-preserving confirmation of the spin magnitude, then attack #1 (spin-yield) + revisit pfifo throughput.

---

## KVM LANE RECONCILIATION (2026-07-26) — the two lanes converge; S0 answered GREEN, S3 reshaped RED-ish
The parallel `kvm-feasibility` lane (docs/vr/kvm-feasibility-study.md) did a strong static study: it found the dirty-tracking bridge is ALREADY plumbed (only a sync *trigger* missing), and correctly named the real risk as **MMIO inversion** (cheap under TCG, a VM-exit under KVM). Its pre-registered first experiment **S0 = count MMIO/frame under TCG** (kill if >20,000).

**Our Phase-0 run already measured it: `MMIO_DISPATCH = 1302/frame` (NV2A).**
- @1.5µs/exit → ~2ms/frame of KVM exit tax. Their table: 500="wins big", 5000="serious tax, still a win", 20000=kill. **1302 ⇒ S0 GREEN — KVM is NOT killed by MMIO density.** Their half-day experiment is done.

**But our guest-PC profile reshapes their S3** (does the guest-CPU wall collapse into FPS?):
- The guest is **~77% SPIN** (inflated upper bound, first-order), ~64% in one kernel busy-wait on the GPU. Real game logic + D3D driver is a MINORITY.
- **The spin is NOT MMIO** (1302/frame ≪ the spin's iteration count) → it polls guest RAM (a pfifo-written flag) or is a delay loop. **Under KVM that spin runs NATIVELY with zero exits — cheap.** But it's still *waiting for the pfifo/GPU-emulation thread*, so making it fast doesn't shorten the frame.
- ⇒ KVM's guest-CPU speedup lands mostly on the ~22% real work + a now-free spin; the frame stays gated by the guest↔pfifo handoff / pfifo throughput. **Their own "honest ceiling caveat" (KVM doesn't touch pfifo/GPU) is promoted from footnote to headline FOR SB.** KVM's FPS win here is likely small.

**CONVERGENCE (both lanes, independently):** the real target is the **pfifo/GPU-emulation thread**, not guest-CPU speed. KVM (their lane) doesn't touch it; the guest spins on it (our lane).

**Synthesis / what to tell both lanes:**
1. **S0 is GREEN — record it, skip to S1 (boot smoke) / S3.** The profiler (this lane) IS their S3 instrument.
2. KVM stays a live bet for OTHER titles — HL2 may be genuinely CPU-logic-bound (measure per-title; SB's spin verdict does NOT generalize). Their dirty-tracking design is real work worth doing.
3. **The dirty-tracking seam should be designed ONCE** to serve TCG's `check_texture_dirty`/#2514 today AND KVM's sync later — exactly the subsystem this perf campaign is already rebuilding. Their R5 (#2514 write path breaks under KVM) is the sharpest risk; our `MEMCB_WRITE_DISPATCH` counter measures how exercised it is (≈0 in SB's read-heavy radar scene, but title-dependent).
4. **Direct SB levers unchanged:** spin-yield (#1) + pfifo throughput. These help under TCG NOW and are prerequisites for KVM to free the core later.
(No edits made to the kvm-feasibility branch — reconciliation recorded here in the lane it references; that lane can pull it.)

---

## CLEAN CONFIRMATION (2026-07-26, no profiler bias) — it's PFIFO-THROUGHPUT-bound; spin-yield DEMOTED
Phase-timers + counters, mission save, NO guest-profiler (so TB-chaining intact — de-biased). Per-frame:
```
run=36  blocked=0.4  dl_stall=0.4(1x)  pfifo_work=31  pfifo_idle=5.5  present=0.4   (ms)
nv2a-prof mspf≈29 · FINISH_STALLED=3 · BEGIN_ENDS=2037 · QUERY=181 · TEX_HASH=287 · SURF_DOWNLOAD=4 · MMIO=1252
```
**Headline: `pfifo_work ≈ 31 ms ≈ the whole frame`.** The GPU-emulation thread's work time IS the frame time. `blocked=0.4ms` → the guest is NOT event-waiting; it spins (busy) on its own core alongside pfifo.

**CORRECTION to the earlier "spin-yield = #1 lever" (M-1 in action — that came from the profiler-BIASED run):** host has **16 cores**; `pfifo_idle=5.5ms` proves pfifo is NOT starved of CPU (it idles voluntarily when the guest hasn't fed it). So the spinning vCPU does NOT steal pfifo's cycles → **spin-yield saves power, NOT frame time. Demoted.** The frame is genuinely bounded by pfifo's ~31 ms of GPU-emulation throughput.

### THE GPU WALL = the pfifo thread's ~31 ms. To hit 60 (~16.7 ms) we must roughly HALVE it.
Likely composition (needs a pfifo-side decomposition to confirm — the next measurement):
- **FINISH_STALLED=3 drains** (occlusion-query pipeline drains) — est. ~6-9 ms. **We already have the flicker-free fix designed: the deferred-epoch query ring** (read a retired epoch; no mid-frame drain). Directly cuts pfifo.
- **TEX_HASH=287 ≈ 35 MB/frame hashed on the pfifo thread** — est. ~7 ms; scales to 310+ in combat. **We have the correct-by-construction design: channel-separated exact-dirty** (the flicker was the co-page false-negative; channel separation removes it). Directly cuts pfifo.
- **2037 draws + pushbuffer method processing** — the raw emulation cost, the biggest and hardest chunk; the 59K `SET_TRANSFORM_CONSTANT`/2 s redundancy (method dump) is a possible batching target.
- **4 surface downloads** (GPU→RAM).

**Key realization:** the earlier campaign's query + surface-sync work was aimed at EXACTLY this thread — we just didn't know it was THE bottleneck (we thought it was flicker/frametime-smoothing). Those shelved-but-designed fixes (query deferred-ring, correct exact-dirty) are now the **direct path to ~halving the frame**, and their combined est. ~13-16 ms saved would take 31 → ~15-18 ms ≈ near 60. This is the most promising concrete route on the board, and it needs NO KVM.

### Next: (1) decompose pfifo_work per-operation to prioritize; (2) implement the query deferred-ring; (3) correct+re-enable exact-dirty via channel separation.

---

## PFIFO DECOMPOSED (2026-07-26) — my "build query-ring + exact-dirty first" prediction was WRONG
Measured `pfifo:` line, mission save, clean:
```
work=31.5  method=16.0  present=8.0  queries=3.1  texhash=2.1  download=1.8  submit=0.6  upload=0.2  residual=0.1  (ms)
```
| bucket | ms | % of pfifo | what |
|---|---|---|---|
| **method** | **16.0** | **51%** | pushbuffer/method dispatch of 2037 draws — per-draw Vulkan state+draw recording (constants, attr binds, descriptor writes, draw calls). THE wall. |
| **present** | **8.0** | **25%** | render_display composite + FLIP_STALL full-pipeline drain (waits for the GPU to finish the frame's draws) + VR submit (no-op flat). Surprise #2 target. |
| queries | 3.1 | 10% | occlusion drains (the query-ring lever) |
| texhash | 2.1 | 7% | 35 MB hash (the exact-dirty lever) |
| download | 1.8 | 6% | 4× GPU→RAM |
| submit/upload/residual | 0.9 | 3% | |

**M-1 CORRECTION (recorded verbatim, prior claim falsified):** the roadmap said *"query-ring + exact-dirty-correct ~halves the frame."* FALSE. Those two total **~5.2 ms**, not ~half — building them first would take 31→~26 ms (~38 fps), useful but NOT the path to 60. **The real wall is `method` (16 ms) + `present` (8 ms) = 76% of the frame.** The decomposition did its job: it stopped us building the small levers thinking they were big.

### Re-ordered GPU-wall attack (by actual size)
1. **method dispatch — 16 ms (THE wall).** Emulating 2037 immediate-mode draws with ~1700 genuine per-frame state changes (SHADER_UBO_DIRTY~1700, ATTR_BIND~3500). Reducible via: dedup redundant constant/UBO uploads, cache pipeline/shader/descriptor binds, batch attribute binds — i.e. cut per-draw CPU recording cost. HARD, general renderer optimization, upstream-relevant. Needs a method-INTERNAL decomposition to find the specific sub-cost (constants vs attrs vs draws vs descriptors). This is where the SET_TRANSFORM_CONSTANT churn lives.
2. **present — 8 ms (surprise, possibly easier).** Much of it is the FLIP_STALL drain waiting for the GPU to finish the frame's draws — a candidate for FRAME PIPELINING (overlap frame N+1 guest work with frame N's GPU completion) rather than a hard barrier. Needs a present-internal decomposition (GPU-render-wait vs display composite vs removable drain).
3. **queries (3.1) + texhash (2.1) + download (1.8) = ~7 ms of designed, flicker-free, low-risk wins** (query deferred-ring + channel-separated exact-dirty). Worth banking (31→~24 ms ≈ 42 fps) even though they're not the headline.

**Honest ceiling, revised UP in difficulty:** the easy designed levers get ~42 fps. True 60 (16.7 ms) requires cracking `method` (16 ms) — the irreducible-looking cost of emulating immediate-mode D3D8 draw-by-draw. That is precisely what an HLE-D3D layer or deep recording-path optimization would collapse — reinforcing the roadmap's "incremental → ~45-50, structural bet for 60" framing. KVM still doesn't help (method is pfifo-side, not guest-CPU).

---

## RECONCILED with the pfifo-wall lane (kvm-feasibility branch, 4-agent synthesis) — 2026-07-26
That lane's prior-art brief (docs/vr/pfifo-wall-{agent-brief,four-agent-synthesis}.md) challenged my "method=16ms is CPU" reading. **Diagnostic confirms THEM:**
- `QUEUE_SUBMIT_AUX=28` == `SURF_TO_TEX=28` (exact) — **each surf-to-texture copy (3 sub-monitors + radar/frame) does a blocking GPU wait via end_single_time_commands, INSIDE the pusher → miscounted as `method` CPU time.** ~28 blocking round-trips/frame ≈ most of the 15.7ms method bucket.
- Nuance vs their brief (written against steel-battalion base): our branch's narrow-fence Part A (8f9124f90f) ALREADY replaced the full `vkQueueWaitIdle` with a per-submission `vkWaitForFences` — narrower, but STILL a blocking wait ×28/frame. The fix stands: **defer/eliminate the waits** (record copies into the main command stream; queue ordering serializes the dependent draw — no separate submit, no wait) = their Tier-1 #1, PCSX2 GSDownloadTextureVK / Dolphin WaitForFenceCounter pattern.
- `FINISH_NEED_BUFFER_SPACE=0` → their descriptor-cliff suspect (§5.2) is NOT active in this scene. Refuted for SB (in this scene).
- Their §3 one-line bug (VK never clears `possibly_dirty` on cache-HIT; GL does at gl/texture.c:374) retrodicts our unexplained "exact-dirty −60% cutscene / ~0% mission" — narrowing the MARKING can't un-stick already-stuck flags. This is the real texhash cause; a 1-line move of `snode->possibly_dirty=false` above the binding_found block may beat channel-separation.

### CORRECTED #1 lever: eliminate the 28 surf-to-tex blocking waits (defer via queue ordering). Est. ~10ms → ~48fps. Zero accuracy cost.
Their DO-NOT list (pushbuffer JIT, secondary CBs, GPU-side FIFO, full D3D8 HLE — all failed precedents) and DO-FIRST measurement discipline (renderer=NULL A/B, VK-vs-GL control, the pre-registered "if pfifo CPU <12ms abandon threading" kill) are adopted. Their lane's analysis supersedes my rushed method-CPU framing.

---

## pfifo2 DECOMPOSED (2026-07-26) — the surf-to-tex lever is DEAD; present_composite is the surprise #1
Measured (mission save, possibly_dirty fix default-ON):
```
method 16.2: pipe=5.0 const=4.1 resid=3.5 attr=1.6 desc=1.3 synctex=0.7 draw=0.3
present 6.9: composite=6.6 gpuwait=0.3 vrsubmit=0.0
```
**M-1 CORRECTION #3 (same spot, third time):** `method_synctex=0.7ms` — the "defer the 28 surf-to-tex blocking waits (~10ms)" #1 lever is FALSE; those copies are non-blocking (record into the open batch), waits are negligible. The decomp agent's code-reading beat both my and the pfifo-wall-brief's hypothesis. NEVER built it — measurement gate held.
**possibly_dirty fix CONFIRMED (other lane's §3, default-ON):** TEX_HASH_KB 35,000→16,364 (halved), texhash 2.1→1.0ms, TEX_HASH 287→68. Real shippable win, scales in combat.

### The wall, ranked by MEASURED ms (of ~30ms pfifo):
1. **present_composite = 6.6ms** — render_display (NV2A framebuffer → host display). BIGGEST + SURPRISE. A 640x480 scale-blit should be <1ms → something slow hides here: the Vulkan→GL external-memory display interop, a synchronous op, or a vsync/present stall. NEW #1 — needs its own decomposition/look (display.c render_display path).
2. **method_pipe = 5.0ms** — per-draw texture-bind + pipeline lookup/create + render-pass churn (~200 renderpasses/frame). Lever: reduce renderpass/pipeline switches, cache harder.
3. **method_const = 4.1ms** — per-draw uniform recompute + UBO hash (SET_TRANSFORM_CONSTANT churn, 85% dirty). Lever: skip recompute when shader-state unchanged (the pg->vsh_constants_dirty[] the pfifo-wall brief noted "nobody reads").
4. **method_resid = 3.5ms** — inter-draw pushbuffer method dispatch (the pure register handling; harder).
5. **queries = 3.1ms** — query-ring (built, default OFF, flicker-validate).

To halve 30→15ms: attack present_composite + method_pipe + method_const + queries (~19ms of targets). Even ~50% off each ≈ 9-10ms → ~20ms → 50fps; true 60 needs cracking present_composite + the method trio. **Shippable NOW (measured/low-risk): possibly_dirty fix (~1ms+, default-ON already).** Next investigation: WHY is render_display 6.6ms.

---

## present_composite EXPLAINED (2026-07-26) — it's the CPU↔GPU no-overlap wall, not slow compositing
render_display (display.c ~931-1010) copies the display image via `end_single_time_commands`, which BLOCKS on a fence; by queue ordering that fence signals only after the frame's prior GPU work. So **present_composite ≈ 6.6ms of the CPU thread WAITING for the GPU to finish the frame's rendering** (the ~1.2MB copy itself is <0.1ms). present_gpuwait (FLIP_STALL) is then 0.3ms because the GPU is already drained.

**This is the pfifo-wall brief's §4 thesis confirmed: ZERO CPU/GPU overlap (1 command buffer).** The frame is serial: ~16ms CPU records draws → ~6.6ms block waiting for GPU → flip. If the GPU render (6.6ms) OVERLAPPED the next frame's CPU recording (16ms), the frame → ~max(16, gpu)+tail ≈ 18-20ms instead of 30. **CPU/GPU overlap (frames-in-flight) is the structural lever worth ~6ms** — the brief's Tier-1 #4. (Our narrow-fence ring built multi-command-buffer scaffolding but default-off/unproven; this is its real justification.)

### FINAL synthesized wall (30ms) and the two lever CLASSES
- **~16ms CPU method recording** (pipe 5 / const 4 / resid 3.5 / attr 1.6 / desc 1.3) — genuine per-draw work for 2037 draws. Incremental levers: renderpass/pipeline caching (pipe), skip-unchanged-uniform (const via pg->vsh_constants_dirty[]). Est. reclaim a few ms.
- **~6.6ms GPU-wait (no overlap)** — structural lever: frames-in-flight / CPU-GPU overlap. Est. ~6ms.
- **~3.1ms queries** (query-ring, built, flicker-validate) + ~1.8ms download + ~1ms texhash (possibly_dirty fix banked).

**Shippable now, measured, low-risk: the possibly_dirty fix (default-ON, hashing halved).** The two big remaining classes — CPU/GPU overlap (~6ms) and method-CPU reduction (~several ms) — are the path from ~30ms toward ~18-20ms (~50fps); true 60 needs both fully cracked. KVM confirmed irrelevant throughout (all of this is pfifo-side).
