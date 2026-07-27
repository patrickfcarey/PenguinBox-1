# Steel Battalion: how the game renders — the deconstruction

**The canonical record of everything we reverse-engineered about SB's
rendering, 2026-07-23 → 07-26.** Sources: NV2A counter forensics, surface
dumps, guest-memory access traces, XBE static disassembly, and live
bisection. Perf work that BUILDS on this lives in
`sb-graphics-findings.md`; this doc is the game-side knowledge itself.
Title ID `0x43430002` (Capcom #2), XBE base 0x10000, entry 0x192c47,
statically-linked XDK D3D8 (own `D3D` section). Parser: `tools/perf/xbe_tool.py`.

## 1. The frame at a glance (mission scene, measured)
- ~2,100 draw calls across ~218 Vulkan-visible render passes per frame;
  immediate-mode D3D8 style (inline elements dominate: ~1,700/frame).
- 185–500 hardware occlusion queries per frame (see §4).
- ~35 MB/frame of texture content genuinely rewritten (animated effects
  — smoke, muzzle flash, glow sprites — re-uploaded and re-hashed every
  frame). Cutscenes/menus: mostly static textures (~0.4–13 MB).
- 4 GPU-surface downloads to RAM per frame (see §3).
- Frame pacing: menus run uncapped (300+ FPS); in-engine frame times are
  CONTINUOUS 17–37+ ms (no hard 30 FPS quantization observed in our
  histogram) — the game renders as fast as the work allows. Real
  hardware reportedly holds 60 in light scenes.
- Film-grain overlay: rendered among the GPU draws (NOT a CPU write,
  NOT a 2D blit — both refuted by counters: SURF_CPU_WRITE=0, 2D_BLIT=0).

## 2. The cockpit's screen topology
The cockpit is composited from multiple render targets:
- **Main window** (640×480 color + zeta): the world view.
- **Sub-monitors** (MFD-class): rendered surfaces re-SAMPLED as textures
  (SURF_TO_TEX 28–356/frame) — classic render-to-texture.
- **Front-view mini camera** (bottom center): a second, smaller render
  of essentially the main view.
- **Radar** (top center): the crown jewel — see §3.
Surface addresses are runtime-allocated (radar seen at 0x03830000 across
boots, but NOT an ABI constant — its base pointer lives in a global).

## 3. The radar pipeline (fully deconstructed, instruction-level)
The most unusual mechanism in the game — a GPU→CPU→GPU round trip every
frame, free on the Xbox's unified memory:
1. **GPU renders the sweep** into a 128×128×4 BGRA helper target
   (pitch 0x200): VT origin dot + forward scan cone + sweep edge (we
   DUMPED and looked at it — `XEMU_SURF_DUMP=auto`).
2. **The base pointer is published at guest global `0x351bdc`**
   (the D3D LockRect result; 10 references in the whole 6.6 MB image,
   all reads; the writer stores it struct-relative, so only runtime
   capture pins the Lock site).
3. **Four twin scan-AND-draw functions** — entries `0x25d40`, `0x26460`,
   `0x26b80`, `0x27250`, one per RGBA channel — each called once per
   frame from the HUD dispatcher `0x450c0` (callers 0x45278/0x45294/
   0x4568f/0x456ab). Inner loop: `cmp byte [eax],0 / add eax,4` scanning
   a per-row window; outer loop advances rows by 0x200. On contact hit,
   the routine IMMEDIATELY emits D3D draws for the blip — the scan's
   output is DRAW CALLS, not a persisted result buffer.
   Read volume scales with contacts: ~4,600/frame quiet → ~23,000 combat.
4. **Gameplay logic reads separately**: a point-occupancy helper at
   `0x21dd0` (19 call sites — lock-on/targeting/warning class) tests
   ~4 individual pixels per call against the same buffer via the same
   base pointer. This is DISJOINT from the blip-drawing scanners —
   skipping the scanners cannot break lock-on.
5. Offset arithmetic verified against live traces: (row 13, col 11) →
   13·512 + 11·4 = 0x1a2c = the first read we ever logged.
6. **Native scanner gating (re-verified 2026-07-26):** the game ITSELF
   skips scanners 3/4 whenever guest flag `[0x356ae0]==0` (confirmed by
   disassembly, live monitor peek, and the skip counter reading K=2) —
   temporal gating of the radar draw path is native to Capcom's design.
   Call-site pairing (corrected): 0x25d40←0x45278, 0x26460←0x4568f,
   0x26b80←0x45294, 0x27250←0x456ab. Scanners write only the XDK D3D
   deferred-render-state block (texture-stage-0 filter fields) — no
   gameplay globals; downstream HUD re-establishes blend/alpha state.
Consequences for emulation: every frame the surface must be downloaded
GPU→RAM once (first CPU read after the draw), and the scan itself is
~23K emulated byte-loads of guest CPU work (the dominant TCG cost —
the skip-N/interception design in findings §8.5 targets exactly this).

## 3b. The hangar doorway (exterior light) — classified 2026-07-26
The bright doorway region pulses as a smooth ~3 s periodic wave — the
rotating alarm beacon's beam sweeping past the entrance — with fine
±1-luma film-grain shimmer on top (strongest on bright surfaces). Both by
design; no artifact signature. Useful calibration: this is what LEGIT
"flicker-looking" content looks like in captures.

## 4. Occlusion queries drive the lights
SB polls zpass pixel counts constantly — the targeting reticle and
light-glow/lens-flare intensities are visibility-driven (glow brightness
follows how many pixels of the emitter survived the depth test). The
game requests report writes when the pushbuffer runs dry (2–4×/frame),
which is why query-resolution stalls appear as "STALLED" finishes in the
emulator. Any mishandling of query timing/results shows up VISUALLY as
light flicker — the lights are the canary for query correctness.

## 5. Texture streaming behavior
Mission scenes rewrite ~35 MB of texture bytes per frame (animated
effects) — these are GENUINE updates (verified: exact-byte-range dirty
tracking found ~nothing falsely flagged in-mission, while cutscenes
showed 60% false-positive page aliasing against the radar's pages).
Practical law: **in missions, texture-dirty work is real; in menus and
cutscenes it is mostly page-aliasing noise.** Any dirty-tracking
optimization MUST respect co-page guest writes or animated textures
flicker (field-bisected 2026-07-26).

## 6. Unified-memory idioms (the emulation tax list)
Everything below is free on hardware and costs sync work in emulation:
- Render-then-CPU-scan (the radar, §3) — reads, NOT writes.
- Render-to-texture sub-monitors (§2) — surface-as-texture sampling.
- Vertex data overlapping rendered surfaces (occasional) — forces
  surface download before vertex upload.
- Surface lifecycle churn — helper surfaces evicted/rebound per frame.
The per-frame tax signature (stock emulator): 4 surface downloads +
2–4 query drains + arena/flip drains ≈ 8 serialization walls/frame.

## 7. Performance profile (who pays, measured on the rig)
Mission scene at ~30 FPS: guest-CPU (TCG) thread ~93% busy (~31 ms of
emulated game work — the game's own logic + the radar scan), pfifo/
Vulkan thread ~51%, GPU 50–72%, memory util ~4%. The game is CPU-logic-
heavy, exactly as a 2002 mech sim on a 733 MHz Pentium III would be;
fill-rate is a non-issue (surface_scale 1 vs 4 measured identical).

## 8. Tooling that produced this knowledge (all in-repo)
- `tools/perf/xbe_tool.py` — XISO→XBE extraction + section parser.
- `tools/perf/scan_hunt_loops.py`, `refhunt.py` — loop-shape scoring +
  global-xref hunting over flat i386 disassembly.
- `docs/perf/scan-pc-capture.patch` — runtime EIP+regs capture of the
  hot reading instructions (apply-ready, XEMU_SCAN_PC_CAPTURE).
- NV2A profiler counters + H6 self-logging summary; `XEMU_SURF_DUMP`,
  `XEMU_SURF_ATTR`, `XEMU_SURF_ACCESS_LOG` (see findings §4).
- Runtime monitor loadvm workflow for repeatable mission measurement
  (runbooks/save-state-testing.md).
