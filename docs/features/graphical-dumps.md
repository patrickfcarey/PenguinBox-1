# Graphical (frame) dumps — Shift+F8

**Confidence: PENDING — not built. Tier-2 prerequisite.** User requirement,
2026-07-18. This doc records the requirement + the port source so it isn't lost;
implementation lands when Tier-2 (geometric stereo) work begins.

## The problem (why this exists)

Tier-2 stereo work — per-eye frustum injection, the convergence/strip-shift
sweep, and the **target-promotion audit** (every rule in
[xbox-vr-3d-pipeline-field-guide.md](../vr/xbox-vr-3d-pipeline-field-guide.md)
§4, and the diagnostics discipline in §5) — needs to analyze the **3D render
stream**: per-draw geometry, render targets, framebuffers, blend state. A RAM
snapshot cannot show any of that.

Today the only capture we have is the **F9 full-RAM dump** (`vr_camera.c`,
`hunt_enable`) — a save-state-style snapshot of guest RAM. That is exactly
right for **Tier-3 camera hunting** (finding camera floats / gate bytes in
RAM) and useless for **3D/stereo** analysis. The user's framing: "save states
work, but in 3D we need graphical dumps."

## The requirement

A **Shift+F8** hotkey that captures a **single-frame graphical dump** — the
xemu analog of pcsx2-VR's GS dump — written to disk, replayable offline. A
multi-frame variant (pcsx2 uses a Ctrl/Shift chord) is a nice-to-have.

**Port source (pcsx2-VR, verified 2026-07-18):** hotkeys `GSDumpSingleFrame`
("Save Single Frame GS Dump") / `GSDumpMultiFrame` → `GSQueueSnapshot(path,
gsdump_frames)` (`pcsx2/GS/GS.cpp`) → captured stream replayed offline by
`GSDumpReplayer.cpp`, Zstd-compressed (`GSDumpCompressionMethod`). The design
(hotkey → queue a frame-scoped render-stream capture → offline replayer) ports;
the capture target is the per-engine rewrite.

## xemu form (implementation lead, decide at Tier-2 start)

The PS2's GS-command dump becomes an **NV2A capture**: the pushbuffer / method
stream for the frame plus the referenced render targets, replayable against the
`vk` (or `gl`) backend offline. Two candidate backends:
- **RenderDoc** — xemu already integrates it (`hw/xbox/nv2a/pgraph/debug_renderdoc.c`);
  Shift+F8 could trigger a RenderDoc frame capture (fast path to per-draw
  inspection, no new replayer needed).
- **Native NV2A dump** — capture the pgraph method stream + surfaces ourselves
  for a purpose-built eye-pair replay harness (field guide §5) — more work, but
  scriptable/headless like `hunt_scan.py`.

## Invariant

The dump is a **copy** taken at frame scope — it must **not perturb the live
frame or the XR session** (same discipline as the F9 RAM dump: snapshot, hand
off to a writer, keep rendering). VR-off and non-dumping frames are unaffected.

## Field / deploy hazards

- **Modifier collision:** F8 alone is **VR recenter** (`ui/xemu.c`); Shift+F8
  must be a distinct branch that checks the Shift modifier and does NOT also
  fire recenter. F9 is the RAM hunt dump — keep the three (F8 / Shift+F8 / F9)
  cleanly separated.
- **Size:** graphical dumps can be large; compress and path them like the RAM
  hunt (`~/xemu-vr-hunt/` sibling, or a `~/xemu-gsdump/`), and never block the
  pfifo/render thread on the write.

## Code pointers (when built)

- Hotkey wiring: alongside the F8/F9 handlers in `ui/xemu.c` (guard `SHIFT`).
- Capture: the `vk` backend (`hw/xbox/nv2a/pgraph/vk/`) and/or the RenderDoc
  hook `hw/xbox/nv2a/pgraph/debug_renderdoc.c`.
- Reference: pcsx2-VR `GS.cpp` / `GSDumpReplayer.cpp` / `GSDump.cpp`.
