# Known Issues — PenguinBox bug log & investigation state

**What this is:** the master list of every bug we've hit, its current state, the
root cause (or leading hypothesis), and the next diagnostic step. One row per
bug. Newest field findings float to the top of each section. This is the Xbox
core's issue log; the shared paradigm (severity/state markers, one row per bug)
matches the sibling forks. Issue IDs are `ISS-B##` (B = PenguinBox) so
cross-repo references stay unambiguous.

**Severity:** **P0** gates a milestone · **P1** hurts a target game · **P2**
quality/comfort · **P3** cosmetic/edge.
**State:** OPEN · INVESTIGATING · ROOT-CAUSED (fix known, not shipped) · FIXED ·
ACCEPTED (deliberate tradeoff / spec-wart) · WONTFIX/UPSTREAM.

---

## P0 — milestone gates

None open. Tier-1 MVP is signed off (OFP Elite, in-headset). The active front
(Tier-3 head-look, W4 polish) is tracked in
[CURRENT_WORKING_LOG.md](CURRENT_WORKING_LOG.md) and
[TEST_QUEUE.md](TEST_QUEUE.md) — those are *pending work*, not bugs.

---

## Recently FIXED (kept for the record — these were once blockers)

### ISS-B10 · F9 RAM-hunt dump never fired — gated behind a live VR session · FIXED
- **Symptom (2026-07-19):** the OFP hunt was "done" (F9 pressed in each state)
  but `~/xemu-vr-hunt/` was **empty** — zero dumps, a whole capture session lost.
- **Root cause:** `vr_camera_frame` (which consumes the F9 request and writes
  the dump) was called only from the VR compositor path, and `xemu_vr_frame`
  early-returns when `!session_running`. So the RAM hunt only worked with a
  **live VR session** — but hunting is normally done **flat**, watching the
  desktop mirror. Every F9 set the request flag; nothing ever consumed it. A
  RAM hunt is a pure guest-memory snapshot with nothing to do with VR — gating
  it on a VR session was the bug.
- **Fix:** split the dump into `vr_camera_hunt_poll()`, called from
  `xemu_vr_frame` **before** the session gate (runs every display sync), gated
  only on `hunt_enable`. Head pose became optional metadata (recorded only if a
  VR session happens to be live). **F9 hunt now works in flat mode.** Head-look
  *poking* stays VR-gated (it needs the head pose). Needs a rig rebuild to
  deploy (ISS-B09 — the rig can't fetch; deploy by scp+build or fix the cred).

### ISS-B01 · Frame-loop wedge on a transient xrBeginFrame failure · FIXED
- `vr_compositor_frame` early-returned when the mailbox was empty, but the
  `begin_owed` retry lived only *after* the mailbox consume. One transient
  `xrBeginFrame` failure → pacer parked in `xrWaitFrame` waiting for that begin
  → mailbox never refills → retry unreachable → loop wedged until shutdown.
- **Fix:** settle owed begins *before* the mailbox check (`vr_xr_compositor.c`).
  (plan §7 F1.)

### ISS-B02 · VR run silently binds the wrong runtime (Monado → flat) · FIXED (operational)
- Nothing set `XR_RUNTIME_JSON`, so an enabled-VR run bound the system default
  runtime (Monado, no HMD here) and **silently fell back to flat** — the
  in-headset test would "work flat" with a healthy-looking log (the most
  expensive false pass).
- **Fix:** the canonical launcher `tools/vr/rig-boot-test.sh` (WiVRn manifest
  autodetect + compositor-socket pre-check + PRIME forcing), deployed as
  `~/xemu-assets/boot-test.sh`. Enforced as H-6. (plan §7 F3.)

### ISS-B03 · Image vertically inverted in the headset · FIXED
- **Symptom (first in-headset run, 2026-07-16):** OFP Elite appeared in the
  Quest 3 correct in every way **except upside-down**.
- **Root cause:** xemu's display image is GL-heritage memory (row 0 = picture
  bottom — why the GL desktop mirror is correct with `flip_required=false`);
  Vulkan/OpenXR reads row 0 as top. The PS2 sibling never hit this
  (its texture is Vulkan-native).
- **Fix:** the swapchain copy is a per-row Y-flip `vkCmdCopyImage` (one
  single-row region per line — raw bytes preserved; a `vkCmdBlitImage` UNORM→SRGB
  would have re-encoded gamma and washed the image out). Region table rebuilt on
  size change. User-confirmed. (plan §7/§8 F8.)

---

## P2 — accepted tradeoffs & operational caveats

### ISS-B09 · Rig checkout drifts from origin; sync blocked by private-repo auth · OPEN (operational)
- **Symptom:** the rig's `~/xemu_vr` is stuck on the pre-rewrite **orphaned
  lineage** (`db99c20a65`) after the 2026-07-17 history rewrite, and can't
  self-sync: `git fetch` on the rig fails `could not read Username for
  'https://github.com'` because the repo went **private** (privacy sweep) and
  the rig's HTTPS remote has no non-interactive credential. There was also no
  standing check to *detect* the drift.
- **Impact:** currently **non-blocking** — the rig binary is functionally
  current (all VR code is in `9f7b6c8768`; every commit since is
  docs/launchers) and the rig only builds, never pushes. **Escalates to P1 the
  moment the rig must build code newer than `9f7b6c8768`** — it would silently
  build stale/orphaned source (the branch-shaped "stale binary" false negative).
- **Unblock:** give the rig a credential for the private repo — a PAT in a
  credential helper, token-in-URL
  (`git remote set-url origin https://<PAT>@github.com/patrickfcarey/xemu_vr.git`),
  or switch the remote to an SSH deploy key — then
  `git fetch origin && git reset --hard origin/vr/mvp` (branch-save any local
  commits first). The dev-box clone is unaffected (it has creds; in sync at
  `2a844de071`).
- **Mitigation (now in place):** the rig-sync discipline (AGENTS.md §5) + the
  drift detector `tools/vr/rig-sync-check.sh` (run from the dev box) + the
  rig-testing pre-flight step. These make the drift *visible* every session;
  the credential fix is what makes the rig *self-syncable*.

### ISS-B12 · SBC keyboard defaults collide with global + VR hotkeys · FIXED (gated), rig-verify pending
- The ported PR #1803 keyboard placeholder binds SBC toggles onto F5–F12,
  colliding with upstream globals (F5–F8 snapshots, F11 fullscreen, F12
  screenshot, F10 renderdoc) **and ours: F8 = VR recenter, F9 = RAM-hunt
  dump** (`ui/xemu.c`). With the SBC driver bound, F8 both toggled filter
  control and recentered VR.
- **Fix (2026-07-23): the cockpit wins.** New predicate
  `xemu_input_keyboard_feeds_sbc()` (`ui/xemu-input.c` — true when any
  bound port runs `usb-steel-battalion` fed by the keyboard) gates all
  five hotkey sites: F5–F8 snapshots (`ui/xui/main.cc`), F10/F11/F12
  (`ui/xui/menubar.cc`), and our F8/F9 VR handlers (`ui/xemu.c`). All
  suppressed actions stay reachable via menus. SBC defaults left verbatim
  (conflict-surface discipline); a real-hardware SBC (not keyboard-fed)
  leaves every global hotkey live. Also note: the PR removes
  right-click-opens-popup-menu for everyone (RMB became SBC Lock-On);
  F2/guide-button still opens it.
- **Alternate recenter shipped (2026-07-24).** The suppression above left
  the keyboard-fed SBC with no VR recenter — exactly the mode a forthcoming
  in-headset cockpit panel targets, since that panel anchors to the
  recenter pose. Added an explicit **Ctrl+F8** chord (`ui/xemu.c`) that
  always recenters, including under the cockpit gate: the SBC map is a raw
  `SDL_GetKeyboardState` read that binds no modifier chords, so the Ctrl
  chord clears the gate without the SBC claiming it (stock layout: F8 is the
  filter-control toggle, so Ctrl+F8 also flips it once — momentary,
  accepted; the rebound layout leaves the F-row free). Bare F8/F9 behavior
  is unchanged. Also added a **View ▸ Recenter VR View** item ("Ctrl+F8",
  `ui/xui/menubar.cc`), enabled unconditionally since
  `xemu_vr_request_recenter()` is a no-op when VR is off. This closes the
  panel's recenter-anchor blocker.

### ISS-B13 · Halo CE / Halo 2 upstream-fix watch · TRACKING (ports landed 2026-07-23)
- **Scouting result (Opus run, 2026-07-23):** both Halos are Playable
  start-to-finish upstream — no rescue needed; remaining defects are
  polish that *scales with internal resolution*, i.e. exactly what VR
  render scale amplifies.
- **Ported** (verify per TEST_QUEUE Priority 1b): **#2874** nearest-filter
  scaled surface blits (sharp 2D/HUD at >1x, `vk/surface.c`, 2 lines);
  **#2941** lock-free fence polling (perf; author cites PGR2 17→30fps —
  candidate for Halo CE #659 stalls + Halo 2 splitscreen flashlight).
- **Watch — broken upstream with NO fix anywhere:** **#2174** (0.5
  hardware-rounding offset → shadow artifacting >1x, names Halo CE) and
  **#659** (effect slowdowns, thread blames surf_download). Fixing either
  would be original fork work.
- **Probably moot for us:** #2311 Halo 2 Ivory Tower menu corruption —
  the only fix (coldhex `441490a`) is GL-only (`GL_UNPACK_ALIGNMENT`);
  our VK path packs uploads differently. Verify absent, else port a VK
  equivalent.
- **Defer — high overlap with our #2514 port:** abaire's surface-sync
  series (#2211, #2158, #2190, #2478, #2537) lives in the same
  files/subsystem; only pursue against a reproduced, traced glitch.
- **Not applicable:** #2554/#2100 (bad dumps), #523/#524 (M1-Mac-only).
- **Operational note (2026-07-24):** two concurrent xemu instances at
  `surface_scale = 4` exhaust the RTX 4050's 6 GB —
  `VK_ERROR_OUT_OF_DEVICE_MEMORY` at `create_display_image`
  (`vk/display.c:659`). Parallel-instance checks (HDD-clone trick) must
  run secondary instances at scale 1; scale-4 verification wants the GPU
  exclusive.

### ISS-B04 · VR frame cost rides the pfifo thread while the UI waits · ACCEPTED
- The VR hook runs inside `pgraph_vk_sync`, between the UI thread's sync request
  and `sync_complete`, so `xrBegin/EndFrame` + the copy submit add latency to
  both emulation-side pgraph processing and the desktop mirror. Bounded by the
  1 s `xrWaitSwapchainImage` timeout with a zero-layer escape.
- Same class of tradeoff the PS2 sibling accepts on its GS thread, with one
  extra coupling (the blocked UI pull). **If in-headset testing shows mirror
  hitches:** a second VkQueue on a dedicated submit thread, or decoupling the
  mirror pull from the VR submit. **Measure first.** (plan §7 F4.)

### ISS-B05 · xrEndSession called from a non-STOPPING state · ACCEPTED (spec-wart)
- `vr_xr_destroy_session` calls `xrEndSession` even when FOCUSED; spec-wise that
  returns `XR_ERROR_SESSION_NOT_STOPPING`. The result is ignored and
  `xrDestroySession` (always legal) cleans up — correct behavior, one silent
  spec violation. The graceful path (`xrRequestExitSession` + bounded pump to
  STOPPING) is ~20 lines + one PFN — post-MVP polish. (plan §7 F2.)

### ISS-B06 · The working BIOS is a debug kernel · KNOWN (operational, H-5)
- `xbox-4627_debug.bin` boots games (proven: OFP Elite); the retail
  `xbox-3944_256k.bin` lands on the service screen for **every** title on the
  rig. Debug kernels can behave differently from retail for some titles (devkit
  paths, memory-layout probes). **If a title misbehaves inexplicably, suspect
  the BIOS before the emulator.** Acquiring a retail 5530/5713/5838-era dump as
  a spare remains worthwhile. (plan §7 F5.)

### ISS-B07 · Desktop mirror minor tearing with VR on · ACCEPTED (watch)
- The async copy's layout round-trip (SHADER_READ_ONLY→TRANSFER_SRC and back)
  vs the GL mirror sampling the same image can tear. Same-vendor NVIDIA tolerates
  it; report if visibly bad. Tied to ISS-B04's threading. (plan §8 failure table.)

---

## P1 — title-specific

### ISS-B16 closure addendum (2026-07-26 late): the doorway light CLASSIFIED — legitimate
Owner localized the residual visible "flicker" to the hangar DOORWAY where
exterior light enters. Spectral capture (80-frame burst, mission window):
the doorway region is a **smooth ~3 s periodic brightness wave** — the
rotating hangar beacon's beam sweeping past the entrance (rhythmic, by
design) — plus fine ±1-luma shimmer consistent with FILM GRAIN on a bright
surface (also by design). Zero erratic-artifact signature in the clean
window (the "aperiodic" tail in the raw analysis was the save's ~35 s
unattended self-fail cutscene transition, not flicker). With Source 1
fixed, Source 2 hard-OFF, and the doorway explained, **no unexplained
visual artifact remains in the shipping config**. Final owner eye-pass
rides along with the radar-skip live session.

### ISS-B16 addendum (2026-07-26, adversarial review) · TIMING-VARIANCE THEORY DEAD; SEPARATION LIKELY A MEASUREMENT ARTIFACT
- The 4th theory (resolve-latency jitter) fails its first link: baseline
  and async frame-time distributions are IDENTICAL (CV 0.211 vs 0.214,
  same logs). Code-level proof: post-spec-fix async and sync write
  byte-identical report values (wait_for_submission(newest) ≡
  wait_all_slots on one in-order queue), and recycle-deferral provably
  changes no pixels.
- The detector separations DO NOT REPRODUCE: async flicker_score
  0.22/0.38/0.70 across three same-binary runs (baseline ±3%). The
  falsifying "3.19x" run was one capture window opening on a ~0.5s
  smooth whole-frame brightness settle (autocorr +0.87 — a transient,
  NOT an alternation). Free-running 17fps xwd vs ~25fps present =
  capture-beat-sensitive methodology. M-3 lesson recursed: the
  instrument itself needed an armed-and-calibrated check.
- Case-file core error named: Source 1 (exact-dirty — REAL alternation,
  fixed) and "Source 2" were conflated; eye-visible flicker was
  plausibly Source-1-dominated; async's residual may be ghost.
- CONSEQUENCES: deferred-epoch ring NOT justified on current evidence
  (could add staleness-phase-jitter of its own — do not build yet).
  DECISIVE NEXT EXPERIMENT (designed): per-present framebuffer-dump A/B
  — one composited frame per guest flip (capture beat eliminated by
  construction), same detector math. async≈baseline ⇒ feature innocent,
  reopen for shipping; async≫baseline ⇒ genuine guest-timeline residual,
  ring then justified. Owner live observation (sync baseline, same day):
  remaining visible "flicker" localized to the legitimate red alarm
  strobe — consistent with the ghost reading; glow-tracks-lamp check
  pending.

### ISS-B16 · SB mission-scene light/texture flicker under perf features · SPLIT: exact-dirty FIXED (default-off) / async-queries hard-OFF (redesign pending)

**CURRENT STATE (2026-07-26):**
- **Known-good config (double-verified visually):** read fast path (always-on)
  + `XEMU_TLB_FASTPATH=1`, both flicker-source features OFF — clean picture,
  ~30-31 FPS mission content.
- Two INDEPENDENT flicker sources, owner-bisected across four configs. The
  first bisect wrongly exonerated one because they overlapped — lesson: a
  bisect ends only when the LAST variable flips alone. Full table:
  sb-graphics-findings.md §8.7.
- **Source 1 — exact-dirty hash narrowing · FIXED by default-off.** Co-page
  false negative: animated textures sharing a page with a download span got
  genuine updates skipped → stale/fresh alternation. Now opt-in
  `XEMU_EXACT_DIRTY=1` (default OFF); the −60% cutscene hash win is parked.
  Proper re-enable design (dirty-channel separation) recorded in findings §8.7.
  - **v2 IMPLEMENTED, PENDING VALIDATION** (branch `perf-exactdirty-v2`,
    commit 43d5dda2ea): dirty-channel separation built — surface download +
    2D blit stop setting the `DIRTY_MEMORY_NV2A_TEX` page bitmap for their
    ranges and are consulted only via exact spans (Channel S); the page bitmap
    (Channel P) carries only unattributed guest-CPU writes. A page-dirty bit
    therefore always denotes a genuine CPU write ⇒ the co-page false negative is
    impossible by construction. Still `XEMU_EXACT_DIRTY=1` / default OFF; OFF is
    byte-identical to stock. Re-enable gate unchanged: `flicker_detect.py` clean
    in the mission scene + owner eyes (the v1 flicker MUST NOT return).
  - **NEW finding — VK `possibly_dirty` cache-HIT leak (separate root cause,
    likely bigger), FIXED default-on** (commit b662534cea): the VK texture
    backend cleared `snode->possibly_dirty` only on the cache-MISS path, so a
    node flagged once (e.g. by the whole-VRAM mark in `pgraph_vk_flush`)
    re-hashed on every subsequent bind for the life of the node — a VK-only
    per-frame hash leak the GL backend never had (it clears unconditionally).
    This retrodicts why v1 exact-dirty measured −60% on cutscenes but ~0% in
    mission (a stuck flag defeats any narrower marking). Fix clears the flag on
    the cache-HIT path too; output-identical (uploads stay guarded by the hash
    compare). Default ON, `XEMU_NO_TEX_DIRTY_CLEAR=1` reverts for A/B. Whether
    this alone collapses `texhash` (making channel-separation redundant or a
    small add-on) is the pending measurement.
- **Source 2 — async query readback (`XEMU_ASYNC_QUERIES`) · hard-OFF, no
  working fix; redesign pending.** Current working conclusion: the flicker is
  resolve-latency **TIMING VARIANCE**, not a wrong value — sync = uniformly
  slow resolves, async = variable-latency resolves ⇒ the spin-waiting guest's
  frame pacing jitters ⇒ light-animation samples unevenly ⇒ shimmer with zero
  wrong values. This explains why every value-correctness fix failed and every
  timing-touching change moved the detector. **An adversarial review of the
  timing-variance theory is in flight — treat it as the working conclusion,
  not settled fact.** If it holds, async-at-STALLED is architecturally
  unrescuable (correct same-epoch resolution inherently waits its own GPU
  work; the surviving async win is recycle-deferral only) and the
  deferred-epoch ring below is the only path.
- **Destination — deferred-epoch ring (panel design #2), not yet built:**
  serve reports INSTANTLY from the last retired epoch (disjoint per-epoch slot
  ranges): uniform zero-latency delivery (steadier than sync), walls
  eliminated (faster than async), one-epoch-stale counts (imperceptible for
  glow; one frame of lag on zpass gameplay logic).
- **Arbiter:** the automated flicker detector `tools/perf/flicker_detect.py`
  (+ rig A/B harness) — every clearance/falsification below is one of its A/B
  verdicts. Acceptance control: baseline vs `XEMU_ASYNC_QUERIES=1` = **6.12x
  SEPARATED** (docs/perf/flicker-detect.md); the heatmaps localize the async
  instability to canopy glow, radar, front-view panel, and HUD readouts (the
  query-driven lights — the owner's manifestation sites, incl. the rotating
  hangar alarm beacon).

**Source 2 — theory graveyard (all dead; newest first, preserved as evidence):**
1. **Deferred-recycle GPU hazard** (descriptors/framebuffers/staging recycled
   while batches in flight) — **CLEARED** by a real sync-validation run
   (mission content, async firing): **zero** sync-hazard / query / descriptor
   / framebuffer VUIDs. Note: validation layers ARE wired via
   `display.vulkan.validation_layers` and DO load — the *earlier* "zero VK
   errors" greps were blind, so this run (not those) is the authoritative
   check. Pre-existing both-config VUID noise logged separately: 1818x
   `vkCmdCopyBufferToImage-srcBuffer-00174` + 330x SPIR-V `pCode-08740` —
   upstream-class, unrelated, own watch item.
2. **Stale availability-latch** (WAIT_BIT satisfied instantly by a slot still
   latched from its previous epoch before this epoch's reset executes) —
   spec-correct fix landed (`4018495246`: wait own submission's fence before
   `vkGetQueryPoolResults`) and **FALSIFIED by its own pre-registered test**:
   detector still 3.19x SEPARATED on the fixed binary.
3. **Owner-fence wait** on query-slot reuse (`cc9c74cfb6`; per-index
   `QUERY_REUSE_WAIT`) — **PLACEBO.** Its earlier "detector-cleared, flicker
   6.12x→1.86x INSEPARABLE" clearance was **RETRACTED**: empirical rerun 4.13x
   still SEPARATED (the 1.86x was capture-phase luck), and QUERY_REUSE_WAIT=0
   throughout — the wait never fired.
4. **Reset-overlap** (batch N+1's `vkCmdResetQueryPool` racing batch N's query
   writes) — **REFUTED by spec**: reset carries a submission-order execution
   dependency, so a GPU reset cannot overlap prior writes.

**VR watch:** zpass ÷ scale² truncation is a separate light bug at scale>1
(findings §7 P5), independent of this entry.

### ISS-B15 · Steel Battalion: film grain *reported* as costly · OPEN (UNVERIFIED — measure first)
- **Provenance (corrected 2026-07-25): a COMMUNITY report, relayed second-hand —
  NOT a firsthand observation.** The user has *not* seen a bad effect themselves
  and was not watching FPS. Treat this as a hypothesis to measure, not a known
  defect. (Related lesson, ISS-B08: firsthand reports outrank secondhand ones;
  this is the weaker kind.)
- **HARD CONSTRAINT — the effect stays.** The user **likes the film grain and
  wants to keep it.** Any outcome here must *preserve the look*: optimize the
  path, don't disable the effect, and don't add a "turn grain off" toggle as the
  fix. If the only available win were removing it, the correct answer is to
  accept the cost and close this.
- **A removal patch exists (community, 2026-07-25).** Two implications, and only
  the second is actionable for us: (a) *weak corroboration* — somebody cared
  enough to build one, so others perceive a cost; (b) **it is a measurement
  tool.** Applying it temporarily gives a clean A/B (grain vs no grain, same
  scene, same build) to quantify the cost precisely — then it is **discarded**.
  **We do not ship it:** the user likes the grain (see the hard constraint
  above). Use the patch to *measure*, never to *fix*.
- **Step 0 — is it even real?** Measure before investigating: MangoHud frame
  timing (`demo-pbox.sh`) plus xemu's built-in NV2A counters
  (`hw/xbox/nv2a/debug.h:71`, `NV2A_PROF_COUNTERS_XMAC`) in a grain-heavy scene
  vs. a comparable one. If there's no measurable delta, close this as unfounded.
- **If a real cost is confirmed**, ranked hypotheses (effects that are nearly
  free on real NV2A often fall off a cliff under emulation):
  1. **Per-frame noise-texture upload** — animated grain re-uploads a noise
     texture each frame → a texture-cache miss per frame on the pfifo thread.
  2. **Framebuffer readback / surface download** — grain as a post-process that
     reads the frame back forces GPU→CPU sync, the classic emulator stall
     (`NV2A_PROF_FINISH_SURFACE_DOWN`).
  3. **Render-pass churn** — a full-screen blended overlay splitting render
     passes / regenerating pipelines (`NV2A_PROF_PIPELINE_RENDERPASSES|_GEN`).
  4. **Our PR #2514 port — now the LEADING hypothesis, and it cuts the wrong
     way (analysis 2026-07-25).** #2514 makes `address_space_map` honor
     mem-access callbacks, so *a guest DMA write over a live GPU surface now
     triggers a surface download/invalidate instead of silently scribbling via a
     direct RAM pointer.* That is a **correctness fix that ADDS work** — it
     converts a fast (wrong) write into a GPU→CPU download. If the grain effect
     DMAs noise over or near a live surface each frame, #2514 would *create*
     exactly the readback cost of hypothesis 2. So the hopeful reading — "our
     cache work may have already fixed this" — is probably backwards for this
     specific effect, even though the same port is what made the game run at all.
     **Build parity (corrected 2026-07-25): the community is NOT on stock xemu.**
     They run near-cutting-edge builds — necessarily so, since SBC gameplay
     requires the *unmerged* PR #1803 plus the texture/DMA fixes; none of that is
     possible on a stock build. **We are only slightly ahead of them.** So their
     observation is a **peer data point about a build close to ours** and gains
     weight as evidence — the opposite of this entry's first framing. Still
     measure on our own binary before acting (the small delta between us could be
     the very thing that matters, in either direction), but treat their report as
     credible and comparable, and compare notes with them rather than
     re-deriving from scratch.
     Bisect by testing with the port reverted (accepting the crash returns).
     Also relevant: upstream (abaire) notes residual freeze reports against
     #2514 still under review.
  Isolate the grain draws via a frame capture
  ([graphical-dumps.md](docs/features/graphical-dumps.md); note
  `CONFIG_RENDERDOC` is currently **off** in our build).
- **Why it's still worth a look:** VR adds frame cost on the pfifo thread
  (ISS-B04) and dropped frames punish far harder in-headset than on a monitor —
  so *if* the cost is real, it lands where we can least afford it, on the
  VR-cockpit flagship. That is the reason to measure, not a reason to assume.

### ISS-B11 · Steel Battalion: crash entering gameplay + dead menu input · FIXED — VERIFIED IN-GAME 2026-07-23
- **VERIFIED (2026-07-23, rig, flat run):** intro → menus (keyboard, SBC
  device bound) → character signup → opening cutscene → **in the mech
  (in-cockpit)** — and **driving and fighting it** (startup ritual, gear,
  walking, mouse aim, main weapon, lock-on, magazine change —
  user-verified 2026-07-23 late, ON THE STOCK PR ENCODING) — believed a
  first for original Steel Battalion on any xemu build via the emulated
  device. NOTE: a gear-encoding "fix" (`47e29684a8`) was authored from a
  reference conflict, never executed, falsified by the field evidence,
  and reverted (`84ae0fe73f`) — the game accepts N=255/gears 1-5 from
  the emulated device. The first-session "can't move" report: CONFIRMED (user-reproduced
  2026-07-24) — the shifter was in R (from N, one LCtrl = reverse;
  reverse creep is easy to miss, and it takes two LShifts from R back
  to 1st). Game state, not a code bug; ISS-B11 fully closed. Full session log clean: **zero** nv2a/assert lines; the
  MIPMAP_LEVELS==0 diagnostic never fired (the #2514 port prevents the
  stale-surface read outright, not just the abort). Launch recipe:
  `~/sb-launch.sh` on the rig (PRIME env + SDL x11 + `sb-verify.toml`:
  port1_driver usb-steel-battalion, port1 keyboard, vr off). In-cockpit
  control feel pending the SB startup sequence + ISS-B12 cleanup.
- **Symptoms (upstream compat "Intro"):** (a) `assert(levels > 0)` abort at
  `hw/xbox/nv2a/pgraph/texture.c:294` right after the intro FMV / on "start
  new game" (xemu#1238, dup #2252); (b) no input registers at the main menu.
- **Root causes (established 2026-07-23, agent runs L1–L4 — see
  [docs/steel-battalion-bringup.md](docs/steel-battalion-bringup.md)):**
  (a) NOT a register-decode bug (decode chain verified bit-faithful) and NOT a
  legal-but-unclamped config — real NV2A **faults** on `MIPMAP_LEVELS==0`
  (HW-verified upstream). The `0xFF000000` format word is **uninitialized
  guest memory** (`MmAllocateContiguousMemory` fill) read as a texture because
  xemu handed guest DMA a direct RAM pointer without invalidating the GPU
  surface first (async `NtReadFile` race). A bare `levels=MAX(levels,1)` clamp
  was tested upstream and **does not work**.
  (b) The game only polls the Steel Battalion Controller XID (`bType 0x80`)
  and ignores gamepad-type XIDs entirely — input requires the SBC device.
- **Fixes ported (this branch):** upstream **PR #2514** (route DMA through mem
  callbacks — the real (a) fix; upstream test builds reach the cockpit) as
  `3b7360159f`; upstream **PR #1803** (SBC device emulation, keyboard/mouse
  placeholder — proven to make menus navigable, xemu#2252) as `a5eaaa6732`;
  plus a log-and-survive diagnostic replacing the assert (`e72aa3f6b8`) —
  if it ever fires post-#2514, a surface-sync hole remains
  (`fmt==0xFF000000` = stale-fill signature).
- **Next:** rig build + in-game verify — DONE, see verify line above.
  **#2514 freeze status (re-checked 2026-07-24, PR thread):** NO
  freeze/deadlock reports with the fix applied; SB clean through 3
  levels upstream (Triticum0, NVIDIA), matching our clean sessions. An
  earlier research pass characterized freeze concerns as "under
  review" — unsupported by the thread; treat freezes as unconfirmed
  everywhere. Remaining real caveats: the PR is UNMERGED (may be
  revised before landing — re-diff on merge), deep-campaign SB reportedly CRASHES after ~2-3 levels
  (user-recalled report, consistent with upstream's claim being scoped
  to "first 3 levels" — expect a next blocker there; capture the log
  when hit), and the fix-uncovers-next-bug pattern is
  real (Fatal Frame II hit a NEW error post-fix). Minor: HiDPI
  mouse-aim skew (moot on the rig); SBC savestates minimal
  (`VMSTATE_USB_DEVICE` only, upstream FIXME).

### ISS-B14 · "Steel Battalion freeze" · RETRACTED — misdiagnosis; the session was live
- **What happened (2026-07-23):** the session was reported "frozen" from
  remote evidence and killed — but the user was actively playing (mech
  powered on). The kill was wrong; the diagnosis was wrong.
- **Why the evidence lied — recorded so it isn't trusted again:**
  (1) *identical `xwd` captures N seconds apart* — an occluded/fullscreen
  XWayland window serves a stale backing buffer; identical captures prove
  nothing about a live game. Capture via a compositor-live path (GNOME
  D-Bus screenshot) or ask the human at the screen.
  (2) *a single `/proc` sample of the main thread in D-state inside
  `os_acquire_rwlock_read`* — busy GPU processes transit NVIDIA driver
  rwlocks constantly; ONE sample is meaningless. A deadlock claim needs
  the same thread stuck across repeated samples over tens of seconds.
  (3) *one running thread + a forest of `futex_do_wait` workers* — that is
  a HEALTHY qemu (vCPU running, workers idle), not a wedge signature.
  (4) *`Resetting rate control` bursts* — occurs under normal load.
- **Diagnostic standard going forward:** "is it frozen?" gets findings +
  a request for the user to confirm what the screen shows; a kill happens
  only on the user's explicit word. Remote evidence alone never justifies
  killing a session (see also the family never-kill rule).
- **Status of the underlying risk:** the upstream #2514 review's
  freeze/deadlock caveat REMAINS a watch item (ISS-B11/ISS-B13) — but as
  of now there is **no confirmed field hit** on this fork.

### ISS-B08 · MechAssault 2 does not boot in xemu · WONTFIX (emulator-side), PARKED
- Does not boot despite its xemu.app "Playable" rating. **Do not use it** as a
  bring-up/target title. General lesson: **treat user firsthand boot reports as
  authoritative over compat-site ratings.** (plan U1.)

---

## Cross-cutting notes

- **Threading cluster:** ISS-B04 and ISS-B07 are the same surface (VR submit on
  the pfifo thread vs the UI pull + GL mirror). A fix or measurement for one
  informs the other — treat them together if mirror hitches appear in the field.
- Deploy/rig hazards and the git-identity rule that prevent *new* problems live
  in [AGENTS.md](AGENTS.md) (hard rules), not here.
