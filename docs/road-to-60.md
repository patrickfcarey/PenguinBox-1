# The Road to 60 — Steel Battalion performance, everything we learned

**The authoritative synthesis (2026-07-26).** Steel Battalion in the PenguinBox
xemu fork: from the 20 FPS crash-fixed baseline, to a measured, owner-verified
30 FPS, to a fully-decomposed map of the remaining wall and the ranked path
toward 60. This doc leads with what is TRUE now; the chronological lab-notebook
(with every superseded prediction) is `perf-roadmap-30-to-60.md`. Read this one.

---

## 1. Current state — what is banked, built, and true

| thing | status |
|---|---|
| **20 → 30 FPS** (radar-live gameplay) | **SHIPPED, owner-verified live.** Read fast-path (always-on) + TLB fast-path (`XEMU_TLB_FASTPATH=1`). ~44→30 ms/frame. |
| **possibly_dirty hashing fix** | **SHIPPED, default-ON.** Halved texture hashing (35→16 MB/frame). Correctness-verified (clean render, 0 VK errors). |
| Working baseline | VERIFIED: default config renders SB correctly — cockpit, radar, HUD, gauges all intact, 0 asserts/crashes. |
| query-ring, channel-exact-dirty, narrow-fence, radar-skip, async-queries | BUILT, **default-OFF**, opt-in, each needs validation before enabling. Save nothing until on. |

**Honest scorecard:** one big banked win (20→30, +50%), one small banked fix
(~1 ms), and a complete measured map of the next ~15 ms. The map is not savings
— it is the build list.

---

## 2. What the bottleneck actually IS (measured, not assumed)

**Steel Battalion is PFIFO/GPU-EMULATION-THROUGHPUT-bound.** The guest-CPU (TCG)
thread is ~94% busy but mostly SPINNING — waiting on the emulated GPU. The
pfifo (renderer) thread's work ≈ the whole frame. This was established by a
guest-PC statistical profiler (77% spin, kernel wait loop) and phase timers
(`pfifo_work ≈ 31 ms ≈ mspf`, `blocked ≈ 0.4 ms`).

**The frame is serial (zero CPU/GPU overlap):** ~16 ms CPU records 2,037 draws
→ ~6.6 ms blocked waiting for the GPU to finish → flip. One command buffer;
Dolphin runs 8, PCSX2 runs 3.

### The 30 ms pfifo frame, decomposed to the millisecond (mission save)
```
method 16.2 : pipe 5.0  const 4.1  resid 3.5  attr 1.6  desc 1.3  synctex 0.7  draw 0.3
present 6.9 : composite 6.6  gpuwait 0.3  vrsubmit 0.0
queries 3.1 · download 1.8 · texhash 1.0 · submit 0.6 · upload 0.2
```
- **present_composite 6.6** = the CPU thread BLOCKING on the display-copy fence, which by queue ordering waits for the frame's whole GPU render. It is the CPU↔GPU no-overlap wall, NOT slow compositing (the copy is <0.1 ms).
- **method_pipe 5.0** = per-draw texture-bind + pipeline lookup/create + render-pass churn (~200 renderpasses/frame).
- **method_const 4.1** = per-draw uniform recompute + UBO hash — the SET_TRANSFORM_CONSTANT churn (~1,700 genuinely-dirty/frame; `pg->vsh_constants_dirty[]` exists but "nobody reads it").
- **method_resid 3.5** = inter-draw pushbuffer method dispatch.

---

## 3. The Road to 60 — ranked levers by MEASURED milliseconds

To halve ~30 → ~15 ms (60 FPS). None built yet except where noted.

| # | lever | ms | class | risk |
|---|---|---|---|---|
| 1 | **CPU/GPU overlap (frames-in-flight)** — render frame N while recording N+1; hides present_composite's block | ~6.6 | STRUCTURAL | high (renderer surgery; narrow-fence ring half-scaffolds it) |
| 2 | **method_pipe** — reduce renderpass/pipeline switches, cache harder | ~5.0 (partial) | incremental | med |
| 3 | **method_const** — skip per-draw uniform recompute/hash when shader state unchanged (read `vsh_constants_dirty[]`) | ~4.1 (partial) | incremental | med |
| 4 | **query-ring** — serve occlusion from a retired epoch, no drain | **REFUTED, OUT for SB** | built, OFF — **do not enable** | Breaks geometry: SB drives occlusion *culling* off query results, so a stale epoch culls live walls → see-through flicker (owner-confirmed 2026-07-26, clean single-var bisect). AND perf absorbed: queries −2.7ms reappears in present_composite +2.7ms, net frame flat. |
| 5 | **method_resid** — the pushbuffer decoder | 3.5 (partial) | hard | — |
| — | possibly_dirty fix | ~1 (banked) | DONE | shipped |
| — | channel-exact-dirty | +texhash | built, OFF | flicker-validate |

> **THE WALL IS DOWN (2026-07-26): see
> [`vr/present-wall-plan.md`](vr/present-wall-plan.md)** — mapped by the
> four-lane investigation (docs/vr/overlap-*.md), built by a five-agent
> parallel fleet, measured in a 7-leg matrix: **pfifo 28.8 → 18.6 ms**
> (present 6.6→0.4, queries 2.9→0.1, renderpasses 205→33), 0 VK errors,
> all stages default-OFF pending owner-eyes gates. **The wall behind the
> wall: SB vsync-locks itself to 30 FPS (frame pins at exactly 2 vblanks =
> 33.3 ms; pfifo idles waiting for the guest).** RESOLVED 2026-07-26 (see
> [`sb-frame-limiter.md`](sb-frame-limiter.md), four-lane RE): SB is a **fixed
> 30 Hz-timestep engine** (one sim step per present, hardcoded 1/30 s, no
> wall-clock/QPC/vblank-delta). The "60fps game all along" hypothesis is
> **REFUTED** — a present-unlock byte-patch = 2× game speed; true 60 needs
> sim/render decoupling (frame-gen), not a patch. Live combat measured ~23 fps
> (an earlier log-scrape wrongly reported 30). Titles without a 30-lock still
> inherit the overlap's ~10 ms immediately.

**Realistic ceiling — CORRECTED by measurement (2026-07-26).** The earlier
"incremental levers → ~45-50" projection is **REFUTED for SB.** The 45-fps lever
batch was built and measured: query-ring is OUT (breaks geometry + absorbed),
inline-batch is inert (SB is ELEMENT-heavy, never fires), const-skip is near-inert
(82% draws genuinely dirty), pipe-opt is real-but-tiny (~0.8ms) and also absorbed.
The present wall (lever #1) **absorbs whatever the pfifo levers free upstream** —
so incremental pfifo optimization cannot move SB's frame time until CPU/GPU overlap
is cracked. **45+ runs THROUGH lever #1**, not around it. The only fresh
incremental candidate left is the ELEMENT-batch sibling (SB's real hot path,
`INLINE_ELEMENTS≈1162/frame`), still unbuilt and also likely absorbed unless #1 lands first.

**Descriptor cliff (brief §5.2) REFUTED for SB:** `FINISH_NEED_BUFFER_SPACE=0`
in-scene — not hit. Don't chase it.

---

## 4. The graveyard — hypotheses MEASURED and KILLED (do not re-walk)

Every one of these was confident, plausible, and wrong. The measurement caught
each before it cost build effort. This list is the most valuable thing here.

- **"CPU-logic-bound" (the game's physics/AI too heavy)** → REFUTED. Guest is 77% spin-on-GPU; real logic is a minority.
- **TLB-flush storm (H-B)** → REFUTED. `TLB_FLUSH_ALL=4`/frame, `TB_TRANSLATE=6` — no storm.
- **"Defer the 28 surf-to-tex blocking waits" (my #1 lever, THREE times)** → REFUTED. `method_synctex=0.7 ms`; those copies are non-blocking (open-batch). The pfifo-wall brief and the decomp agent both beat this hypothesis.
- **Spin-detect-and-yield** → REFUTED as an FPS lever. 16 host cores + `pfifo_idle=5.5 ms` ⇒ the spin doesn't starve pfifo; yielding saves power, not frame time.
- **KVM for SB** → DEFLATED. The guest spins on the emulated GPU; native CPU makes the spin fast but doesn't shorten the GPU wait. (KVM stays live for CPU-logic-bound titles — see §6.)
- **Predictive readback / narrow-fence early-submit** → measured NEGATIVE (retimes drains, doesn't remove them).
- **exact-dirty v1 "halves the frame"** → REFUTED (−60% cutscene / ~0% mission; the real cause was the possibly_dirty leak, not the marking).
- **async-query flicker = a real render bug** → mostly a capture-beat MEASUREMENT ARTIFACT + legitimate rotating-beacon/grain (the doorway light was classified legit).
- **"query-ring + exact-dirty ~halves the frame"** → REFUTED. Those total ~5 ms, not half.
- **query-ring "PERF-VALIDATED" (my banked #4 win)** → REFUTED ON BOTH AXES (2026-07-26, owner-confirmed). **Correctness:** SB drives occlusion *culling* off query results, so a retired-epoch (stale) answer culls live geometry → walls flicker see-through. Caught by owner eyes in a clean single-variable bisect (walls solid with query-ring off, see-through on; possibly_dirty + the other 3 levers all exonerated). A static screenshot CANNOT catch a motion-triggered flicker — labeling it "validated" on a screenshot was the error; the default-OFF-until-owner-eyes gate is what saved the ship build. **Perf:** the queries-bucket saving (2.8→0.1 ms) reappears in present_composite (6.6→9.3 ms) — the present wall absorbs it, net frame flat. The earlier "mspf 30→28" was scene/noise, not reproduced in the interleaved matrix. **Lesson:** occlusion results that DRIVE rendering (like SB's radar readback) must be correct, not lagged — same class as the DO-NOT "disable readback" dial.

---

## 5. The instrumentation built (reproduce any of this)

All env-gated, default-inert, on-branch. Rig recipe: launch with the env, `loadvm` the mission snapshot, read stderr.
- **Guest-PC profiler** (`XEMU_GUEST_PROFILE=N`) + `tools/perf/guest_profile_symbolize.py` — samples guest EIP, symbolizes vs the XBE, spin-cluster report.
- **Phase timers** (`XEMU_PHASE_TIMERS=1`) — `phase:` line (run/blocked/pfifo_work/idle) + `pfifo:` (method/texhash/queries/download/…) + `pfifo2:` (method_const/pipe/attr/desc/draw/synctex + present composite/gpuwait/vrsubmit).
- **Overhead counters** (always-on, H6 line) — `TLB_FLUSH_ALL`, `TB_TRANSLATE/RECYCLE`, `MMIO_DISPATCH`, `MEMCB_*`, plus `QUEUE_SUBMIT_AUX` (the drain count).
- **flicker_detect.py** — objective inter-frame luminance A/B (baseline vs config), heatmaps; the visual-regression gate.
- **Runtime monitor-loadvm** measurement workflow (`runbooks/save-state-testing.md`), the ladder/ABABA harness (`tools/perf/rig/`).
- **XBE tooling** (`tools/perf/xbe_tool.py`, `refhunt.py`, `scan_hunt_loops.py`) + the guest-code map (`docs/sb-rendering-anatomy.md`).

---

## 6. KVM & the cross-title picture (three axes)

**KVM feasibility (reconciled with the `kvm-feasibility` lane):** S0 GREEN —
`MMIO_DISPATCH=1302/frame` ≪ the 20,000 kill threshold, ~2 ms exit tax. The
dirty-tracking bridge is already plumbed (only a sync trigger missing). So KVM
is *feasible* — but for SB the win is small (the wall is pfifo-side; the guest
spin goes native but still waits the GPU). **KVM is a live bet for
CPU-logic-bound titles, measured per-title, never assumed.**

**A CPU/RAM-heavy title (e.g. Half-Life 2) has THREE independent axes:**
1. **Resolution → host GPU** (available now; render-scale is free on the host GPU — SB scale 1==4 proved it; costs host VRAM + per-game high-res correctness).
2. **Memory/streaming → 128 MB emulated RAM + modified BIOS** (the hardware-modders' fix, far easier in emulation; `xbox.c:194` allocates a variable RAM size; needs a 128 MB BIOS + MC-reporting + maybe a game patch — a bounded experiment).
3. **Framerate → KVM** (the structural bet; HL2 is a better candidate than SB — it genuinely maxed the real Xbox CPU).

---

## 7. DO-NOT (failed precedents, from the pfifo-wall lane's prior-art)

- **Pushbuffer JIT / command-sequence cache** — Dolphin's DLCache deleted 2014, PPSSPP's <1%.
- **Vulkan secondary command buffers reused across frames** — zero adoption (RPCS3/Xenia/Ryujinx).
- **GPU-side pushbuffer interpretation** — nobody ships it.
- **Full D3D8 HLE** — not worth it (but Cxbx's LLE slowness was Windows SEH, not LLE — don't cite it against LLE).
- **A user "disable readback" dial** — SB's radar surface DRIVES game logic (lock-on/blips); readback must be *correct*, not optional.
- **Threading the pfifo** — last resort; Dolphin Dual-Core is off-by-default, RPCS3 MT-RSX still off in 2026; the NV2A reads back constantly. An offload thread with a size threshold at most.

---

## 8. The method (why this map is trustworthy)

Everything here obeys the campaign's hard-won rules (AGENTS.md §1b M-1..M-9):
measure before building; a "fixed" claim needs replication; a negative check
must prove its detector was armed; new perf features default-OFF until objective
instrument AND owner eyes clear them; correctness-critical conclusions need two
independent lenses. Three confident hypotheses died to these rules before
costing a build. Two parallel agent-lanes (KVM, pfifo-wall) independently
converged on the same target (pfifo/GPU-emulation) from opposite directions.
That convergence, and the measured decomposition, are why the ranked path in §3
is a build list and not a guess.
