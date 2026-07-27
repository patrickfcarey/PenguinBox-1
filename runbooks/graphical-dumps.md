# Runbook — graphical dumps (the PCSX2-GS-dump analog for PenguinBox)

**"Graphical dump" = capturing what the game feeds the GPU, for offline
renderer debugging.** PCSX2 has .gs dumps; xemu has no replayable dump
format, but three capture layers cover the same needs — all usable on a
LIVE session via the QEMU monitor (launch includes
`-monitor unix:/tmp/xemu-mon.sock,server,nowait` via ~/ring-launch.sh).

## Layer 1 — raw NV2A command stream (closest GS-dump analog)
Runtime-toggleable trace of every pushbuffer method the game submits:
```
(echo "trace-event nv2a_pgraph_method on"; sleep 2;
 echo "trace-event nv2a_pgraph_method off"; sleep 1) \
  | nc -q 3 -U /tmp/xemu-mon.sock
```
Output floods the instance's stderr log (~70K lines/s in mission scenes
— the game hitches while on; keep bursts to 1-3 s). Slice it out by
byte-offset (`wc -c` before, `tail -c +N` after). Format:
`nv2a_pgraph_method <subch>: <class> -> <addr> NAME[i] <value>`.
Useful companions: `nv2a_pgraph_surface_download`,
`_surface_render_to_texture`, `_surface_create_color`, `_texture_*`
(list all: `info trace-events nv2a_*` via the monitor).

## Layer 2 — surface/image dumps (what the GPU produced)
Env at launch (not runtime): `XEMU_SURF_DUMP=auto` writes every small
(≤512²) CPU-read/downloaded color surface to
`~/surf-dump-<addr>-<WxH>.ppm` per download (radar buffer etc.).
`XEMU_SURF_ATTR=1`, `XEMU_SURF_ACCESS_LOG=1` for access forensics.

## Layer 3 — replayable state (the true "replay" story)
QCOW2 snapshots ARE fully replayable machine state: `savevm` via
monitor, restore with runtime `loadvm <tag>` (CLI -loadvm fails with
SBC — see save-state-testing.md). Snapshot + Layer-1 burst = a
reproducible frame-stream capture of any scene.

## Layer 4 — RenderDoc frame captures (BUILT OUT 2026-07-26)
The strongest layer: full host-API frame captures (every Vulkan call,
texture, shader, state), PCSX2-GS-dump parity and beyond. Enabled in
build.sh (--enable-renderdoc; vendored header, runtime dlopen — no hard
dep). Runtime: portable RenderDoc 1.39 at ~/renderdoc on the rig
(no sudo); ~/ring-launch.sh exports LD_LIBRARY_PATH so librenderdoc.so
resolves. Trigger in-app: the Debug menu (frame-capture entry; Ctrl for
trace variant) or the bound capture hotkey (ui/xui/main.cc
g_capture_renderdoc_frame). Captures open in ~/renderdoc/bin/qrenderdoc
on the rig.

## First captured reference dump (2026-07-26, mission scene, ~2 s)
146,473 methods — profile of SB's immediate-mode style:
59K SET_TRANSFORM_CONSTANT (per-draw uniform spam!), 34K
VERTEX_DATA4F_M + 12K 2F_M + 6K 4UB (inline vertex pushes), 5.9K
TRANSFORM_CONSTANT_LOAD, 4.9K BEGIN_END (~800 draws/frame visible),
4.4K SET_TRANSFORM_PROGRAM (shader reloads per frame!), combiner
reconfig ~700/frame. Confirms sb-rendering-anatomy §1: constant/shader
churn per draw is huge — an upstream-relevant batching target.
