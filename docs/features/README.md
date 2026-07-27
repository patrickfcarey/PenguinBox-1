# Per-Feature Design Docs

**What this is:** one design doc per feature — the *why it exists, how it works,
what it costs, and how to change it safely*. Where a feature is still thoroughly
covered inside [docs/vr/00-mvp-plan.md](../vr/00-mvp-plan.md), this index points
there rather than duplicating it; features that outgrow the plan get a dedicated
doc here. Same paradigm as the sibling forks' `docs/features/`.

Start with the system view in [ARCHITECTURE.md](../../ARCHITECTURE.md); this is
the drill-down per feature.

## Index

| Feature | What it does | Design home |
|---|---|---|
| **Three-tier VR model** | screen / stereo / camera-injection layering | [ARCHITECTURE.md](../../ARCHITECTURE.md) §2 |
| **The compositor & frame loop** | pacer thread, mailbox, swapchain, the Y-flip copy, quad/cylinder layer, the pfifo-thread hook | plan §W3 + [increment log §6](../vr/00-mvp-plan.md) + `vr_xr_compositor.c` |
| **The Vulkan bridge seams** | XR dictates the physical device via `XR_KHR_vulkan_enable2` at xemu's instance/device creation | plan §W2 + [01-port-map.md](../vr/01-port-map.md) + `vr_manager.c` |
| **Tier-3 head-look (find-and-poke)** | freelook-zeroing float pokes + the ADS-style byte gate, fed by the head pose | plan §9/§9a + [RESEARCH.md](../../RESEARCH.md) + `vr_camera.c` |
| **The RAM hunt tooling** | F9 physical-RAM dumps + `hunt_scan.py` angle/gate scanner | [RESEARCH.md](../../RESEARCH.md) + `tools/vr/hunt_scan.py` |
| **Quest Touch input (backlog)** | OpenXR actions → SDL3 virtual gamepad | plan §10 (not built) |
| **Gesture→button (backlog)** | velocity-gated pose recognizer → virtual pad | [02-gesture-mapping-concept.md](../vr/02-gesture-mapping-concept.md) (concept) |
| **Graphical dumps (Shift+F8, pending)** | single-frame render-stream capture for Tier-2 stereo analysis (xemu analog of pcsx2's GS dump) | [graphical-dumps.md](graphical-dumps.md) (pending) |
| **SBC cockpit input overlay (pending)** | in-game state + live-bindings panel for the Steel Battalion controller keyboard placeholder (gear/toggles/tuner/keys) | [sbc-cockpit-overlay.md](sbc-cockpit-overlay.md) (scoped, not built) |

## Design-doc conventions

- **State the problem first** — what field symptom or need created the feature.
- **Show the invariant** — the one property that must hold (e.g. off-state:
  "VR-off build is byte-identical to upstream"; the compositor: "emulation never
  blocks on XR pacing").
- **Name the deploy/field hazards** — what breaks if it's half-configured (the
  silent GL fallback, the wrong runtime binding) or mis-set.
- **Point at the code** by `file:line`, and at the tests/self-tests that guard it.
- **Mark confidence** — WORKING / STAGED / HUNTED / PENDING for anything not yet
  field-validated (RESEARCH §6), so nobody mistakes staged infra for a proven
  feature.
