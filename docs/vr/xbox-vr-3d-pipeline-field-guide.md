# Hooking a console emulator's 3D pipeline for VR — field guide from the PS2 effort

**Audience:** the agent about to hook the Xbox (NV2A) render pipeline for stereo VR.
**Source:** everything learned shipping stereo in a PCSX2 fork — including the bugs
that took three hunt rounds and a fleet of verification agents to kill. Concepts
only; no PS2 code needed. Where a lesson cost us a week, it's marked ★.

---

## 1. Why your job is easier than ours — and where it is identical

**Easier:**
- **You have real projection matrices.** Xbox games push their projection (or
  composed WVP) through vertex-shader constants / fixed-function state. You can
  recover the actual frustum per draw and render TRUE per-eye off-axis
  projections. We never could: the PS2's GS receives post-projection screen
  vertices, so our stereo is a screen-space displacement driven by the vertex's
  1/w — a heuristic with permanent artifact classes (see §3) that you can
  mostly avoid by doing real reprojection.
- **You have a clean 2D discriminator.** D3D-style pre-transformed vertices
  (XYZRHW) are your "this is UI/screen-space, do not displace" signal. Ours
  (the FST texture-coordinate mode + uniform-Q heuristics) is far mushier and
  produced whack-a-mole bug classes. Draws with a real projection = world
  (stereo); RHW draws = screen (pin at zero disparity). Start there; add
  per-game overrides only when a game lies.

**Identical (this is where our scars transfer 1:1):**
- Emulated consoles composite their display through render-target machinery —
  offscreen scene targets, blit-to-display, post passes, render-to-texture.
  **Everything in §4 (the promotion rulebook) will happen to you** regardless
  of how clean your projection math is, because it's about *target/layer
  plumbing*, not projection.
- Mixed-content frames (3D world + 2D HUD + video + fades) are the eternal
  per-game tuning surface. Budget for a data-driven per-title layer (§7).

## 2. Architecture that survived contact

- **Two-layer array targets + single-pass stereo rendering** (Vulkan
  multiview in our case; OVR_multiview2 or instanced stereo if you're GL).
  One draw call renders both eyes; the eye index selects the projection.
- **A separate OpenXR compositor stage** consumes the 2-layer result per
  vsync: copy layer 0 → left swapchain, layer 1 → right, submit as a
  world-locked quad/cylinder ("virtual screen") — head-look comes free from
  the runtime's reprojection of the screen layer, before you ever touch the
  game camera.
- **Rejected after field testing: frame-interleaved stereo** (alternate eyes
  on alternate vsyncs). Deinterlacing mixes eyes and the 16 ms stagger is
  visible. Do single-pass from day one.
- **Promote lazily at the display chain.** Don't make every target stereo —
  find the scanned-out buffer (and whatever feeds it) and promote those to
  2 layers. Everything else stays mono and costs nothing. Track the display
  chain explicitly (we note the framebuffer addresses seen at scanout plus
  "feed edges": mono targets sampled by draws into stereo targets join the
  chain and promote on their next incarnation).

## 3. Stereo math — do it better than we could

- Prefer **true off-axis projection**: per eye, translate the view by ±IPD/2
  in view space and skew the projection (standard asymmetric frustum). All
  depth cues come out geometrically right, and three of our permanent
  artifact classes never exist for you.
- Keep our fallback for content you can't reproject (RHW/video/UI composites
  you decide to give depth): `offset = eye_sign · separation ·
  max(0, 1 − convergence · (1/w))`, with the max() as a comfort clamp that
  forbids in-front-of-screen (crossed) disparity.
- Know our artifact classes so you can recognize them if you inherit any
  screen-space path:
  - **Bimodal depth**: with 1/w-driven offsets and one global convergence,
    near content pins to the screen while everything past a threshold
    saturates to full separation — mid-field objects double. Real projection
    fixes this outright.
  - **Degenerate-depth billboards**: effects drawn with bogus w (flares/glows
    at "infinity") detach from their anchors. With real matrices you inherit
    the game's own placement — mostly immune.
  - **Clamp knee**: any hard max()/clamp in the disparity function creases
    geometry that spans the boundary. If you clamp for comfort, clamp in
    *view space* before projection, not on the output disparity.

## 4. ★ THE TARGET-PROMOTION RULEBOOK — every line here was a shipped bug

Once a display target has 2 layers, **every code path that writes pixels into
any promotable target must be layer-audited**. Our census of writer classes and
the rule each must follow:

1. **Game draws** through your stereo pipeline: fine by construction.
2. **Raw copies (blit/copy-region):**
   - equal layer counts → copy layer-for-layer;
   - mono source → stereo dest → broadcast to both layers;
   - stereo source → mono dest → take eye 0 (define this rule once,
     explicitly — it is your "mono consumers see the left eye" law).
3. **Stretches/format converts through non-stereo shader paths** ★: these
   sample layer 0 unless told otherwise. Route by SOURCE layer count:
   stereo→stereo must run per-layer (through per-layer views), or you will
   *flatten* real stereo (sample left, write both) — we shipped that
   regression ourselves and needed an adversarial audit to find it.
4. **CPU-uploaded content applied to a promoted target** (video frames, menu
   art, dirty-rect refreshes): mono by construction → mirror into both layers
   after applying, or one eye holds stale garbage. Our original P0 (an entire
   boot menu missing in the right eye) was exactly this.
5. **Every helper that allocates a replacement texture** ★★: resizes, format
   conversions, effect-source snapshots, "copy the target so we can sample
   it" paths. If the allocation call has a default `layers = 1` argument, a
   promoted target gets silently demoted or mono-copied there. We hit this
   FOUR separate times in four different helpers. Grep every allocation site
   that can receive a promoted target and pass the layer count through.
6. **Auxiliary mask/precision passes** (stencil-from-destination-alpha,
   high-precision blend brackets, any pass that reads the target to build
   per-pixel state): must run per-eye. A mask built from eye 0's data and
   applied to eye 1 culls geometry along a parallax-shifted edge — "the
   statue's face is cut off in one eye".
7. **★★★ The composite-seed rule (our final boss, found today):** when you
   promote a target whose current content is mono, you'll be tempted to seed
   both layers with that mono image ("both eyes same = safe"). It is safe
   ONLY if the game then fully overwrites the buffer. If the game
   **alpha-blends** its display composite over the old contents (ours blended
   56 % new / 44 % old, every frame), the zero-disparity seed *survives the
   blend* in the eye whose true content is offset — a permanent translucent
   ghost of the scene, offset by the full disparity, visible in exactly one
   eye. Correct seeding for a buffer that gets composited from a true stereo
   source is that source's own matching eye layer — or don't pre-seed and
   accept one dark frame. Audit every game whose display blit has blending
   enabled; read the blend factors out of the draw state and you can predict
   the ghost's exact intensity.

## 5. Build the diagnostics BEFORE the features

These paid for themselves within days:

- **Headless eye-pair replays**: your emulator's frame-dump/replay runner +
  an env var that redirects snapshots to a chosen eye layer. Deterministic
  replay means ANY pixel difference between the two runs is pure stereo.
  Add the same env to per-draw target dumps and you can watch one eye's
  framebuffer build **draw by draw** — that capability root-caused our
  final ghost in a single session after a week of theorizing.
- **A zero-cost-off census lane**: env-gated one-line-per-draw logging of
  the classes you care about (blend-on-display-composite, mask passes,
  RHW draws on stereo targets, layer-count mismatches, stereo→mono
  handoffs). When a field artifact arrives, one replay names the class.
- **A cheap metric for "double image"**: residual-at-zero-shift divided by
  residual-at-best-shift over a region, between the two eyes. ~1.0 = ghost,
  ≥ ~2 = clean. Trivially scriptable, removes all eyeballing.
- **Golden-image pins** on known-good eye pairs so upstream merges and your
  own refactors can't silently regress stereo.
- **A simulated-HMD runtime** for headset-free verification (we use a null
  compositor + simulated HMD). Wire it into your replay harness early;
  session warm-up takes seconds, so grade LATE frames of a replay, not the
  first ones (they render mono while the session starts).

## 6. Process traps that burned real time

- ★ **Non-default build targets**: our replay runner and test binary are not
  in the default build target. Eight straight diagnostic runs executed a
  stale binary and returned confident zeros; a fix was "verified ineffective"
  without ever running. Always build the runner explicitly and check its
  timestamp before trusting a run.
- **Log sinks differ per binary**: our GS-thread logging API prints in the
  GUI app but is swallowed by the headless runner (a different sink survives
  in both). Verify your diagnostic lane actually reaches the log in the
  binary you replay with — with a marker line — before interpreting silence.
- **Single-frame dumps loop deterministically** — animation/pan theories
  about differences between two replay runs are usually your own frame
  misalignment. Verify within-run frames are identical before comparing
  across runs.
- **Guard tools that seize the headset.** Any VR-enabled replay steals the
  live session's display. Every tool that can do that must refuse to run
  while the emulator is up (check the process list), with an explicit
  override flag. Ours self-refuse; the one script that didn't caused a
  mid-play seizure incident.
- **One identity for commits, both fields** (author AND committer), set
  explicitly on every commit — if your project has the same attribution
  rules ours does, the committer field is the one everyone forgets.

## 7. The per-game data layer — plan for it now

- A profile catalog keyed by title identity (serial/title-ID + a CRC
  whitelist) carrying: separation, convergence, per-class overrides (pin
  rules), and comfort/FOV parameters. Data file, not code — the whole
  per-game grind lives here and ships as content.
- **Per-scene stereo** was our single biggest quality jump on mixed-content
  games: probe one or two game-RAM addresses per vsync (menu/screen-state
  flags found via savestate diffing), debounce a few frames, and switch
  sep/conv atomically with the frame. Menus at screen depth, gameplay deep.
  Xbox equivalent: same trick against guest RAM; your savestates give you
  the same diffing workflow.
- Validate flag hypotheses from **savestates** (full guest RAM snapshots)
  before ever going live — reading candidate addresses across a handful of
  labeled states kills or confirms a scene flag in minutes.
- **Head-look via controller injection** before touching game cameras: many
  games bind look to the right stick; mapping headset yaw/pitch onto the
  stick axes (deadzone + response curve per game) shipped for us in games
  whose cameras we never reverse engineered. Xbox pads are analog-rich; the
  same cheap win should exist.

## 8. Comfort defaults that survived user testing

- Behind-screen-only depth (no crossed disparity) as the default posture;
  it reads as "a deep window" and never stabs the viewer.
- One global convergence is always wrong somewhere — per-scene values are
  the fix, not endless global retuning.
- A one-frame full-black flash is worse than a one-frame stale eye: when one
  eye's frame misses a deadline, re-present its last image rather than
  blanking both. On a world-locked screen the runtime's reprojection makes a
  16 ms stale eye essentially invisible; black strobing is the thing users
  actually feel.
- Instrument an A/B path early (we do live "eye-doctor" ladders: flip a
  value, ask which is better). Depth tuning by description does not work;
  tuning by comparison is fast and decisive.

---

*Written 2026-07-17 off the back of the sessions that closed our right-eye P0
and the display-composite ghost. If a claim above seems overcautious, it has a
commit hash and a post-mortem behind it.*
