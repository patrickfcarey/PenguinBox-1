# FUTURE_STATE — PenguinBox roadmap & vision

**What this is:** the short version of where PenguinBox is going. Detail lives in
[docs/vr/00-mvp-plan.md](docs/vr/00-mvp-plan.md); this is the map above it. Same
paradigm as the sibling forks' roadmap docs.

## The tier ladder

| Tier | State | What's left |
|---|---|---|
| **1 — Virtual Screen** | ✅ **SIGNED OFF** (OFP Elite, in-headset) | W4 polish tail (HUD toggle) |
| **3 — Immersive head-look** | 🔨 **in progress** | The OFP RAM hunt (rig-gated), then anchor titles (Jedi Outcast/Academy — public GPL engine); then the profile-DB port for permanent per-title profiles |
| **2 — Stereo Screen** | ⏸ **post-MVP non-goal** | Per-eye NV2A transform injection — deferred until Tier-3 is proven (plan §2) |

Tier 3 is intentionally ahead of Tier 2 here: Xbox FPS titles (OFP, Jedi
Knight) make head-look the higher-value near-term win, and the per-eye stereo
injection is a heavier renderer change.

## Forward ideas (backlog)

- **Quest Touch controllers as input** (plan §10): OpenXR actions → SDL3 virtual
  gamepad → xemu's existing controller stack. Enables VR-native input and, as a
  bonus, emulated-pad rumble → `xrApplyHapticFeedback`. Sequenced after the OFP
  Tier-3 hunt; xemu leads, then the pattern ports to the siblings.
- **Gesture→button mapping** (plan §10a): velocity-gated pose recognizer →
  virtual-pad button (e.g. arm-thrust → melee). Depends on the Touch backend.
- **Profile DB + per-title config** (plan U8): a sidecar per-title store keyed on
  **XBE Title ID** (xemu has a single global config — no per-title layering
  today). Graduates HUNTED addresses to permanent, AOB-anchored profiles and is
  the port target for the family's shared profile schema.
- **Retail BIOS spare** (ISS-B06): source a retail dump so titles that dislike
  the debug kernel have a fallback.
- **Upstream sync policy** (plan U9): the fork point is recent but upstream
  xemu moves fast — decide the cadence, then port the sibling's guarded
  upstream-sync tooling.

## Family & framework position

PenguinBox is the **Xbox core** of the PenguinVR family — sibling to `pcsx2-VR`
(PS2), `duckstation_vr` (PS1), `mupen64plus-VR` (N64), all deriving shared design
from `retro-vr-framework`. The three-tier model, the RE paradigm, the hard
rules, and eventually the profile schema are shared; the emulator-side glue is
per-core. Cross-core forward work beyond what's listed above is owned at the
framework level (in the private framework repo) and is deliberately not
enumerated here — this repo is public (H-8).

**Positioning stays honest (H-7):** the value is execution + the per-game grind +
cadence across the family, never a claim to have invented world-locked screens,
reprojection, or camera injection.
