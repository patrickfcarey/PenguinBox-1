# PenguinBox — Architecture (10-minute overview)

This is the front door. It orients you to the shape of the system and links into
the deep docs for detail — it deliberately does not re-derive them. The
authoritative deep source is [docs/vr/00-mvp-plan.md](docs/vr/00-mvp-plan.md)
(especially §1, §3, §6-§9); everything here traces back to it or to the code
cited inline.

## 1. What this is

PenguinBox is a **sidecar VR layer bolted onto xemu** (the original-Xbox
emulator, itself a QEMU fork) — not a fork of the renderer, a module that plugs
into it. It adds OpenXR output (Monado/WiVRn on Linux) over xemu's existing
**Vulkan NV2A** renderer, giving Xbox games a head-tracked world-locked display
and — per game — head-driven camera control.

**The one finding that shaped the whole port:** xemu already has a Vulkan NV2A
renderer *and* the VK↔GL external-memory interop (`vkGetMemoryFdKHR` +
`glImportMemoryFdEXT`, `hw/xbox/nv2a/pgraph/vk/display.c`). So there is **no
GL→VK bridge to build** (the biggest chunk the feasibility doc feared): run
xemu with `renderer = VULKAN`, and the finished guest frame is already a
`VkImage` (`pgraph_vk_render_display`) we copy straight into the XR swapchain.
The desktop mirror keeps working unchanged. (plan §1.)

## 2. The three tiers

Every game gets Tier 1 for free; Tiers 2/3 are tuning and per-game work. A
game's config declares what it targets; the player can always step down.

| Tier | Name | What changes | Status |
|---|---|---|---|
| 1 | **Virtual Screen** | Head-tracked flat/curved screen in a void, correct scale, judder-free | **MVP SIGNED OFF** (OFP Elite, in-headset) |
| 2 | **Stereo Screen** | Tier 1 + true geometric per-eye depth (per-eye NV2A transform injection) | Post-MVP non-goal (plan §2) |
| 3 | **Immersive** | Game's own camera follows your head | **In progress** — infra landed, first target OFP Elite |

Some titles run Tier-1-only when their camera is authored/directed (head-look
would fight the director) — a normal, supported end state, not a failure.

## 3. The two-clocks insight

The game keeps running at its **native Xbox framerate**. The OpenXR compositor
submits a world-locked layer at **headset rate** (72/90/120 Hz), and the
runtime's own reprojection fills the gap between game frames. Source fps never
has to hit 90 — the compositor re-samples the last image between updates. This
is why low-fps titles are comfortable in headset.

**Be honest about what this is:** standard OpenXR composition-layer behavior —
the same mechanism Bigscreen/Virtual Desktop use daily — not something invented
here (H-7). The contribution is **execution** inside an Xbox emulator's
renderer plus the per-game reverse-engineering grind.

## 4. The sidecar design

VR code is almost entirely **new `vr_*` files** under
`hw/xbox/nv2a/pgraph/vk/`, plus a small set of **surgical hooks** into xemu's
own pgraph/vk files. Per-game behavior is **config data** (the `[vr]` section),
not code. All VR code compiles with the Vulkan backend (the `vr_*` files are
added under `if vulkan.found()` in `hw/xbox/nv2a/pgraph/vk/meson.build`) — there
is **no compile-time VR option today** (plan §W1 proposed an `ENABLE_VR` meson
option; the code doesn't gate that way — trust the code). It is gated at
**runtime** by `vr.enable`: the frame hook (`xemu_vr_frame`) is a no-op when
off, so a VR-off run is behaviorally identical to upstream xemu (the manager's
bridge-failure contract guarantees a seam failure aborts VR internally and
returns the plain Vulkan call — renderer init can never fail *from* VR).

**Why `hw/xbox/nv2a/pgraph/vk/`:** these modules are pfifo-thread Vulkan code,
and that directory's LGPLv2+ license fits our team-authored files (plan §2a).
Only settings/HUD glue lives under `ui/`.

## 5. Module map

| Module | Role | File |
|---|---|---|
| `vr_manager` | Public facade; settings snapshot; the Vulkan **bridge seams** (into xemu's VkInstance/device creation); the per-frame hook; XR event pump; teardown-to-flat on session/instance loss | `hw/xbox/nv2a/pgraph/vk/vr_manager.c` |
| `vr.h` | The **pfifo-side public interface**: the five bridge-seam calls (`begin_bootstrap` → `create_instance` → `get_physical_device` → `create_device` → `on_device_created`) + the frame hook (`xemu_vr_frame`). Contract: every function is safe to call with VR off or failed — the module behaves inactive and the flat path proceeds | `.../vr.h` |
| `vr_xr_session` | OpenXR instance/system/session/reference-space ownership + event pump; the three `XR_KHR_vulkan_enable2` graphics wrappers (Phase A/B) | `.../vr_xr_session.c` |
| `vr_xr_compositor` | Frame loop on the pfifo thread: lazy `R8G8B8A8_SRGB` swapchain, the fence-ring copy (with the F8 Y-flip), quad/cylinder layer submission, the pacer thread + single-slot mailbox, head-pose stash | `.../vr_xr_compositor.c` |
| `vr_xr_loader` | `dlopen(libopenxr_loader.so.1)` + the OpenXR PFN table (idempotent, degrades to flat on any missing symbol) | `.../vr_xr_loader.{c,h}` |
| `vr_camera` | Tier-3 find-and-poke: freelook-zeroing float pokes, the ADS-style byte gate, and the F9 full-RAM hunt dump | `.../vr_camera.c` |
| `vr_public` | UI-safe public surface — **no target headers** (so `ui/` units can include it) | `.../vr_public.h` |
| `vr_internal` | Shared internal declarations across the `vr_*` TUs | `.../vr_internal.h` |

## 6. Threading

The pfifo thread is the analog of the PS2 sibling's GS thread — **all** Vulkan
queue submission already happens there, so the VR copy submits on the same
thread and queue access stays externally-synchronized by construction.

- **UI/vblank thread** (`ui/xemu.c`): a fixed-cadence timer drives
  `gl_render_frame` even while paused; its VK path (`nv2a_get_framebuffer_surface`)
  sets `sync_pending`, kicks pfifo, and **blocks on `sync_complete`**.
- **pfifo thread**: runs `pgraph_vk_sync` → `pgraph_vk_render_display` (renders
  the guest surface into `r->display.image`). **The VR hook sits right here,
  after `render_display`** — it consumes the pacer mailbox, copies the display
  image into the XR swapchain, and submits the quad layer. Same-queue order
  means the copy sees the finished image without extra semaphores.
- **Compositor pacer thread** (`qemu_thread` "vr-pacer"): runs `xrWaitFrame` in
  a loop — the one OpenXR call explicitly permitted off the submitting thread —
  and hands frame timing back through the single-slot mailbox. The pfifo hook
  never parks in `xrWaitFrame` (that would throttle emulation); it skips
  submission when the mailbox is empty, so XR self-caps at HMD rate and
  emulation never blocks.

The head pose (VIEW-in-LOCAL) is stashed per XR frame in the compositor for the
Tier-3 camera feed. **F4 accepted tradeoff:** the VR copy rides the pfifo thread
between the UI's sync request and `sync_complete`, so it adds latency to both
emulation and the desktop mirror; bounded by a 1 s `xrWaitSwapchainImage`
timeout with a zero-layer escape. Measure before optimizing (plan §7 F4).

## 7. The key deltas vs the PS2 sibling (pcsx2-VR)

The reference implementation is `pcsx2/VR/`; this port simplifies it in three
Xbox-specific ways (plan §9):

- **Physical addresses end-to-end.** The Xbox's 64 MB is unified and host-mapped
  (`d->vram_ptr`); the hunt dumps physical RAM and the poke writes physical RAM
  — no VA translation, and a found address IS the poke address. (The PS2 side
  translates EE virtual addresses.)
- **No per-title config layering.** xemu has a single global config; there is no
  equivalent of the PS2 fork's per-game settings layer. A sidecar per-title
  store keyed on XBE Title ID is the post-MVP profile-port design (plan U8).
- **C, not C++, and flat `vr_*` naming** (matching xemu's `hw/xbox` idiom)
  rather than the C++ `pcsx2/VR/` classes.

One Xbox-specific gotcha already burned and fixed: xemu's display image is
**GL-heritage memory** (row 0 = picture bottom), so the swapchain copy does a
per-row **Y-flip** — the PS2 side never hit this because its texture is
Vulkan-native (plan §7 F8 / KNOWN_ISSUES ISS-B03).

## 8. Repo layout

| Path | What's there |
|---|---|
| `hw/xbox/nv2a/pgraph/vk/vr_*` | The module itself — all new files, compiled with the Vulkan backend; runtime-gated by `vr.enable` |
| `hw/xbox/nv2a/pgraph/vk/openxr/` | Vendored Khronos OpenXR headers (Apache-2.0 OR MIT) |
| `config_spec.yml` `[vr]` section | Per-run VR config (code-generated into the C config struct at build time) |
| `tools/vr/` | Offline tooling: the `hunt_scan.py` camera scanner, the `rig-boot-test.sh` launcher |
| `docs/vr/` | The plan of record, the port map, backlog concepts |
| `docs/features/` | Per-feature design docs |

## Deeper reading

- [docs/vr/00-mvp-plan.md](docs/vr/00-mvp-plan.md) — the plan of record: MVP
  definition, licensing, W0–W5, the increment log, two deep-review passes, the
  Tier-3 hunt protocol, the backlog.
- [docs/vr/01-port-map.md](docs/vr/01-port-map.md) — the PCSX2→xemu port,
  task-by-task, with the bridge seams pinned to lines.
- [RESEARCH.md](RESEARCH.md) — how a game becomes head-look (the RAM hunt).
- [KNOWN_ISSUES.md](KNOWN_ISSUES.md) — the bug log & investigation state.
