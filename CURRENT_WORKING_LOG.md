# Current Working Log — what's in flight right now

**What this is:** the live session log — the state of active work, what just
changed, and the immediate next action for each thread. Skim this to answer
"where were we?" It is intentionally dated and disposable; settled facts migrate
to [KNOWN_ISSUES.md](KNOWN_ISSUES.md), the design docs, or the plan's increment
log. Same paradigm as the sibling forks' working logs.

**As of:** 2026-07-26 (late) — SIX-AGENT PRE-FLIGHT ALL GREEN; radar-skip
verified end-to-end on the shipping binary (arm/tick/disarm fired live);
doorway "flicker" classified legitimate (beacon sweep + grain); async
queries hard-OFF with the whole theory graveyard recorded (ISS-B16);
awaiting the owner's live cadence session (runbooks/radar-skip-test.md).

---

## Front: SB performance campaign (branch `sb-graphics-research`) — 20→30 FPS shipped; agent branches merged, build green

- **Shipped + owner-verified:** read fast path (23K→1 trapped reads/frame)
  + TLB fast path = SB mission gameplay 20→30 FPS, clean picture
  (double-verified). Authoritative: docs/sb-graphics-findings.md;
  game-side knowledge: docs/sb-rendering-anatomy.md.
- **Flicker saga:** two independent sources bisected (ISS-B16) —
  exact-dirty FIXED (default-off, opt-in XEMU_EXACT_DIRTY=1); async-queries
  hard-OFF — every value-correctness fix (owner-fence, stale-latch) fell to
  the flicker detector, working conclusion is resolve-latency timing variance,
  destination = deferred-epoch ring. Detector tools/perf/flicker_detect.py is
  LANDED and is the arbiter.
- **Radar skip-N (temporal downsampling) MERGED** (perf-radar-skip →
  8fa7778be2), not yet executed: breakpoint hooks on the four located scanner
  routines, live-tunable via /tmp/radar-skip-n; THE pending live test = radar
  blips stale-smooth vs blink (owner-eyes); lock-on unaffected by design.
- **New capabilities landed:** runtime monitor-loadvm measurement
  workflow (runbooks/save-state-testing.md), graphical-dump layers 1-4
  incl. RenderDoc build-out (runbooks/graphical-dumps.md), NV2A method
  dump captured (146K methods — found 59K SET_TRANSFORM_CONSTANT/2s:
  upstream-relevant batching target).
- **Next actions:** everything-build green (2026-07-26 06:15:58 — spec-fix +
  radar-skip + RenderDoc enabled, portable runtime at ~/renderdoc). Pending
  live tests, owner-driven: (1) radar skip-N cadence (blip persistence vs
  blink); (2) async deferred-epoch-ring redesign + adversarial review of the
  timing-variance conclusion; then Tier-1 full scanner interception if skip-N
  validates.

---

## NEW front: Steel Battalion bring-up — GAMEPLAY REACHED (branch `steel-battalion`)

- **Status (2026-07-23): in-cockpit, verified on the rig** — intro → menus →
  signup → cutscene → in the mech, clean log (zero nv2a/assert lines). A first
  for original SB on any xemu build. Full story: ISS-B11 (FIXED) +
  [docs/steel-battalion-bringup.md](docs/steel-battalion-bringup.md).
- **Landed on the branch:** PR #1803 port (SBC device; keyboard/mouse
  placeholder), PR #2514 port (DMA surface-sync — the root fix for the
  xemu#1238 crash), levels==0 diagnostic, ISS-B12 hotkey gating (cockpit wins
  F5–F12 while the keyboard feeds the SBC), `tools/vr/sb-launch.sh` +
  `sb-verify.toml` (flat verify recipe; PRIME forced — GL must sit on the
  NVIDIA device even flat, run1 segfaulted on the iGPU).
- **Next actions:** in-cockpit control pass on the **rebound keys** (`H` hatch
  → `F O U B V` toggles → `I` ignition → `LShift`; sb-verify.toml rebind —
  the F-row defaults collided, ISS-B12); then real multi-device SBC binding
  (Phase 3); VR integration after.
- **2026-07-23 late:** combat loop fully user-verified (move/aim/fire/
  lock-on/magazine) — on the STOCK PR SBC encoding; the gear-encoding fix
  was falsified by field evidence and reverted (`84ae0fe73f`). Remaining:
  Q+arrows sights, full-mission soak.
- **2026-07-24: SBC cockpit input overlay SCOPED** —
  [docs/features/sbc-cockpit-overlay.md](docs/features/sbc-cockpit-overlay.md)
  (compact ImGui panel, F3 toggle, live bindings from config; ~1–2 days;
  would have prevented the shifter-in-R incident). Build order: this
  before any in-headset panel work.
- **2026-07-24 (later): overlay BUILT — T1–T7 landed, T8 (build+verify) OPEN.**
  New MIT sidecar `ui/xui/sbc-overlay.cc/.hh` (~330 lines): gear glyph
  (decode copied verbatim from `gl-helpers.cc`, R shown amber — the ISS-B11
  readout), tuner, three pedal meters, five toggle lamps, startup
  momentaries, combat core + aim/rotation meters, collapsible all-controls.
  Labels resolve live from `keyboard_sbc_scancode_map` via
  `SDL_GetScancodeName`, so a rebound layout shows its own keys. Click-through
  (`NoInputs`) so the mouse stays the aiming lever; inert with no keyboard-fed
  SBC. F3 toggle at the F1/F2 site with a cockpit-wins collision guard
  (one-shot notification, View-menu item still works) + View item + config
  block under `display.ui.sbc_overlay`.
  **T8 CLOSED (compile) 2026-07-24 19:47:** first build failed — QEMU's
  `ARRAY_SIZE` is C-only (`typeof` + bitfield-in-sizeof) and the overlay
  was the first C++ TU to call it; fixed with C++17 `std::size`
  (`489fef05d4`), rebuild green, binary restored on the rig WITH the
  overlay. Runtime VERIFIED same evening (screenshot):
  panel renders over the cockpit boot screen, gear [N] + unlit lamps
  state-correct, labels resolve the REBOUND layout (H/I/Return/Backspace,
  F/O/U/B/V) exactly. Remaining: F3-toggle feel + live state tracking
  in-mission (user's next flight); sb-verify.toml now ships enable=true. Deviation from the design's ≈8-line upstream diff: it is
  ~20, because `xemu_input_get_keyboard_sbc()` was added beside
  `xemu_input_keyboard_feeds_sbc()` (now a one-line wrapper) to hand the
  overlay the state without duplicating `bound_drivers[]` logic.
- **2026-07-24 (late): VR COCKPIT CONSOLE — IN BUILD, lane claimed** (this
  session, three parallel agents; do not collide). Display-only MVP: a
  second world-locked quad (dedicated 768x256 swapchain, console anchor —
  low/near/tilted between the player's physical sticks) rastering live
  SBC state on the pfifo thread (`vr_sbc_panel.{c,h}` + compositor
  plumbing + `[vr] sbc_panel_*` keys), plus an alternate recenter
  (Ctrl+F8 chord + View item) since bare F8 is cockpit-gated.
  **Interaction is a stubbed boundary only** (cell hit-rect table +
  log-only inject API, rough-wired by design). **Full interactivity —
  Touch presses, synthetic input dispatch, and the game-logic /
  guest-memory-address layer — is WIP / future scope by owner decision.**
  LANDED + FIRST COMPILE GREEN (d813994e91, binary 21:57): 1,468 lines,
  zero errors first try. Alternate recenter shipped (13d75541f4).
  Remaining: the HEADSET validation (user) —
  `XEMU_VR=1 XEMU_TOML=~/xemu-assets/sb-vr.toml ~/xemu-assets/boot-test.sh
  ~/roms/xbox/'Steel Battalion (USA).xiso.iso'`. Watch: tilt-direction
  sign and WiVRn two-layer acceptance (the pre-registered likely
  one-line tweaks).
- **Halo polish rider (2026-07-23, ISS-B13):** scouting found both Halos
  Playable upstream; ported the two worthwhile open PRs — **#2874**
  (sharp 2D at >1x — VR-relevant) and **#2941** (lock-free fence polling,
  perf). Verify per TEST_QUEUE Priority 1b. Watch #2174/#659 (broken
  upstream, no fix exists anywhere).

## The active front: Tier-3 head-look on OFP: Elite

- **Status:** infrastructure LANDED (`vr_camera.c` + `[vr]` config + F9 hunt
  dump + `tools/vr/hunt_scan.py`, both the angle and gate modes). **Blocked on a
  rig capture session** — the hunt needs the user in-game pressing F9 (agents
  can't wear the headset or drive the game).
- **Next action (user, at the rig):** run the plan §9 hunt protocol —
  1. Angle: `[vr] hunt_enable = true`, in-mission, F9 at compass N/E/S/W/N (yaw)
     then a pitch pass (level/up/level/down). Ideally while ADS.
  2. Gate: still, F9 through hip / plain-zoom / ADS / ADS+zoom / hip / third-person.
  3. Hand the dumps + the dump→state mapping to the agent → `hunt_scan.py` →
     wire `headlook_{yaw,pitch}_addr` (+ `_gate_*`), flip `headlook_enable`,
     verify in-headset. F8 re-zeros.
- **Detail:** [docs/vr/00-mvp-plan.md](docs/vr/00-mvp-plan.md) §9 / §9a;
  [RESEARCH.md](RESEARCH.md) §2-§4.

## W4 polish (Tier-1 tail)

- Recenter hotkey (F8) + live screen tuning are in the HUD (`7aaae3704b`);
  remaining: HUD toggle surface + any settings-page ergonomics. Low priority
  behind the Tier-3 hunt.

## Backlog (not started)

- **Quest Touch controllers as emulator input** (plan §10): OpenXR action
  system → SDL3 virtual gamepad that xemu's controller stack handles as a normal
  pad. ~400 lines backend, near-zero UI. Sequenced **after** the OFP Tier-3 hunt
  validates; xemu first, then port the pattern to the PS2 sibling.
- **Gesture→button mapping** (plan §10a): a velocity-gated recognizer over Touch
  poses relative to the head → virtual-pad button. Depends on the Touch backend.

---

## Repo paradigm established (2026-07-17)

Ported the sibling forks' operating-doc set from `pcsx2-VR` **dev-testing** (the
newest/most-complete rules, confirmed by comparing rule-doc commit dates across
branches), adapted to xemu's realities and branded **PenguinBox**:
- [CLAUDE.md](CLAUDE.md) — session entry point + restated hard rules + doc map.
- [AGENTS.md](AGENTS.md) — operating manual: the Xbox-adapted hard-rules table
  (dropped PS2-specific rules; added the `qemu-system-i386` live-check trap,
  the VULKAN-force / silent-GL-fallback rule, the debug-BIOS rule, the
  runtime+PRIME rule), decision trees, the RE loop, tooling, build/test reality.
- [ARCHITECTURE.md](ARCHITECTURE.md) — three tiers, two-clocks, the sidecar
  design, the `vr_*` module map, pfifo-thread threading, the deltas vs the PS2
  sibling (physical RAM, no per-title config, C-not-C++, the Y-flip).
- [RESEARCH.md](RESEARCH.md) — the F9 physical-RAM hunt, `hunt_scan.py` angle +
  gate modes, freelook-zeroing, measure-don't-trust, banking markers.
- [KNOWN_ISSUES.md](KNOWN_ISSUES.md) — the bug log (ISS-B01..B08), seeded from
  the plan's F1-F8 review findings + the MechAssault-2 no-boot.
- [FUTURE_STATE.md](FUTURE_STATE.md) — the tier ladder + family/framework
  position + forward ideas.
- [docs/features/README.md](docs/features/README.md) — per-feature design-doc
  conventions.

**Naming carryover:** hard rules keep the **same H-numbers** as the family where
the rule is shared (H-1 git identity, H-2 live-session, H-7 positioning, H-8
secrecy, H-9 WiVRn stop order); Xbox-specific rules take the free slots (H-3
VULKAN-force, H-5 debug-BIOS, H-6 runtime+PRIME).

**Pending follow-up:** the code still says `xemu-VR` in file headers / log
strings — a mechanical `xemu-VR → PenguinBox` rebrand pass across the `vr_*`
sources, `hunt_scan.py`, and user-facing strings, in its own commit (AGENTS §5).

---

## Review round 2 (2026-07-17, owner-directed: "review everything, fill gaps")

Adversarial re-verification of every doc claim against source + a public-repo
safety sweep. **Corrections found and applied:**
1. **The stale-binary claim was WRONG for this repo** — `./build.sh` builds
   `qemu-system-i386` by default (`target=`, `build.sh:162`); the PS2 sibling's
   "not in the default target" trap doesn't transfer. Rule rewritten across
   CLAUDE/AGENTS/RESEARCH: the trap here is *skipping the rebuild* — verify the
   binary's mtime before every session.
2. **`vr.h` was missing from the module map** (the pfifo-side public interface
   carrying the five bridge-seam calls + the frame hook; the survey grep
   pattern `vr_` couldn't match the filename). Added to ARCHITECTURE §5.
3. **H-8 self-leak fixed:** the docs named the unannounced roadmap item in this
   PUBLIC repo — in a public tree, naming it IS the leak. Rephrased in
   CLAUDE/AGENTS/FUTURE_STATE to protect without telegraphing.

**Verified accurate against source:** hook site inside `pgraph_vk_sync` ✓,
`xemu_vr_wanted` + frame-hook no-op gating ✓, `"vr-pacer"` thread ✓
(`vr_xr_compositor.c:688`), the Y-flip per-row region table ✓, the ISS-B01
settle-before-mailbox fix ✓ (F1 comment in place), every launcher claim ✓
(plus newly captured: `XEMU_VR_RUNTIME=system` override, asset pre-flight,
canonical-vs-deployed drift rule), all cited `[vr]` config keys incl.
`screen_arc` ✓, vendored OpenXR header licenses ✓.

**Gaps filled:** `runbooks/` (README + [rig-testing.md](runbooks/rig-testing.md)
— the operational content as a grab-at-the-rig procedure with the expected-log
sequence and failure table), `tools/vr/README.md` (tool inventory + the
scanner's missing-self-test note), TEST_QUEUE wired to the new docs, doc-map
rows added, the U9 upstream-sync decision noted in FUTURE_STATE's backlog.

**Flagged to owner (pre-existing, deliberately not changed by the agent):**
this repo is public and the plan doc (§4) carries rig SSH user@LAN-IP details —
the private siblings keep those out of public trees. Scrub or accept, owner's
call.

## History rewrite + recovery (2026-07-17, owner-directed)

- **Rig connection details purged from ALL history** (owner order): the tip was
  scrubbed (now points at the git-ignored `.env.local`), then
  `git filter-repo --replace-text` rewrote the 21 branch commits (base +
  upstream hashes untouched, author/committer identities preserved) and the
  branch was force-pushed. Verified: `-S` search empty, every rewritten
  commit's tree greps clean, historical text reads `<redacted-rig>`.
- **Force-push near-miss, recovered with zero loss:** a commit pushed to
  `origin/vr/mvp` AFTER this session's clone (the Tier-2 field-guide import)
  was briefly discarded by the force-push. Detected from the push output's
  unexpected old-ref hash, recovered via the GitHub events API + fetch-by-SHA,
  and replayed onto the rewritten branch (original authorship preserved).
  Lesson recorded in AGENTS §5: fetch + `--force-with-lease` with an explicit
  expected value on every history rewrite.
- **⚠ EVERY OTHER CLONE MUST RESET before its next push** — pushing from a
  clone with pre-rewrite history resurrects the purged commits:
  `git fetch origin && git checkout vr/mvp && git reset --hard origin/vr/mvp`
  (applies to the rig checkout `~/xemu_vr` and any second dev box).
- Residual exposure, stated honestly: the old commits may stay fetchable
  by full SHA on GitHub until its GC runs (a support ticket can expedite),
  and any clone made during the exposure window keeps them. The address is a
  private LAN IP — low sensitivity, defense-in-depth cleanup.

## Unpushed / uncommitted

- Nothing — the paradigm set, the scrub, `.env.local` convention, and the
  recovered field-guide commit are all pushed (`vr/mvp`).
