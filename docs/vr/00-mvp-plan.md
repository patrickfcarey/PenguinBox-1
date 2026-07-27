# xemu-VR — MVP plan (Tier-1 head-tracked virtual screen)

Status: plan 2026-07-16 (deep-review pass same day: threading traced to code,
bridge seams pinned to lines, licensing constraint added, headless assumption
corrected). **Implementation underway on branch `vr/mvp` — increments 1-3
landed same day** (see §6 increment log): W1-W4 are code-complete (seams,
loader, session, compositor); remaining for Tier-1 MVP: settings/hotkey
polish, in-session validation, and the in-headset test (W5).

Framework sources this plan is built from:
- `retro-vr-framework/docs/10-xemu-xbox-vr-feasibility.md` — the xemu feasibility
  verdict and tier mapping (the "document about how to implement the VR framework
  into xemu").
- `retro-vr-framework/docs/01-integration-spec.md` — DuckStation's binding module
  map / lifecycle / frame-hook spec; our closest structural template.
- `mupen64plus-VR/docs/vr/S0-rig-findings.md` — rig environment facts and the
  GL-session-is-poison / Vulkan-compositor decision.
- `pcsx2-VR/pcsx2/VR/` — the working Vulkan OpenXR implementation to port
  (`XRSession`, `XRCompositor`, `VRVulkanBridge`, `VRManager`, `VRProfileDB`, …).
- `pcsx2-VR/runbooks/ssh-into-the-test-rig.md` + `mupen64plus-VR/runbooks/rig-config.md`
  — how we operate the shared Ubuntu test rig.

---

## 1. The one finding that changes the feasibility doc's plan

**This fork already has a Vulkan NV2A renderer, and it already contains the
VK↔GL external-memory interop.** Doc 10 (§3, §7) assumed xemu was GL-only and
budgeted a GL→VK bridge as the biggest presentation chunk, flagging "is a Vulkan
path in progress upstream?" as an open question. Verified in this tree:

- `hw/xbox/nv2a/pgraph/` has three renderer backends: `gl/`, `vk/`, `null/`,
  selected by config `display.renderer = NULL | OPENGL | VULKAN`
  (`config_spec.yml:227`).
- With the VULKAN backend, the finished guest frame exists as a **VkImage**
  (`pgraph_vk_render_display`, `hw/xbox/nv2a/pgraph/vk/display.c:1062`), which is
  then exported to the SDL/ImGui GL window through
  `vkGetMemoryFdKHR` + `glImportMemoryFdEXT` on Linux (`display.c:689-697`) —
  i.e. xemu ships the exact interop machinery mupen planned to build, in the
  VK→GL direction, for its desktop mirror.

**Consequence:** the MVP does not need a GL→VK bridge at all. Run xemu with
`renderer = VULKAN`, port our Vulkan `XRSession`/`XRCompositor` (our own code —
see §2a; only the emulator-side glue is rewritten) against xemu's own VkDevice,
and copy the final VkImage into the XR swapchain. The desktop mirror keeps
working unchanged (the existing VK→GL export feeds the SDL window as before).
This is strictly simpler than what doc 10 and mupen budgeted. The GL pgraph
backend is irrelevant to VR except as a flat-play fallback.

Two renderer-selection gotchas (verified in `pgraph.c:254-339`):
`get_default_renderer()` prefers **OPENGL**, so the rig config must explicitly
set `display.renderer = 'VULKAN'`; and if VK init fails, `init_renderer()`
**silently falls back** to the default renderer with only a notification — VR
must detect "renderer is not VK" loudly rather than half-initialize.

Mupen's S0 hard lesson still binds: **never create a GL OpenXR session**
(GLVND dispatch corruption on WiVRn/Monado — segfaults on the rig). A Vulkan XR
session while the app's window is GL is exactly mupen's endorsed shape; PCSX2
proves the Vulkan XR path works on this rig.

## 2. MVP definition

**Goal:** xemu running a game on the Ubuntu rig, presented as a world-locked,
head-tracked flat screen in the Quest 3 over WiVRn (Tier 1 of the three-tier
model), with a recenter hotkey and screen distance/height settings. Desktop
window keeps mirroring. Any VR failure degrades to flat play with one warning.

**Non-goals for MVP** (post-MVP tiers, per doc 10 §6 sequencing): Tier-2 stereo
(per-eye NV2A transform injection), Tier-3 head-look camera injection, the
profile database/precedence port, autocalibration, cylinder layers.

## 2a. Licensing — what ports and what doesn't (verified against the fork)

**Principle.** "The VR framework is GPLv3" is true only as the *outbound*
license the pcsx2-VR fork offers to third parties. The team wrote the VR
module and holds its copyright; a copyright holder is never bound by their own
outbound license and can additionally license their own original code any way
they choose (standard dual-licensing). What GPLv3 *does* permanently bind is
(a) the fork **as a combined work**, and (b) any code that is a **derivative
work of PCSX2's code**. That yields exactly two categories, both verified in
the pcsx2-VR tree (2026-07-16):

**Category A — portable (ours).** `pcsx2/VR/` — 9 .cpp + 9 .h, ~4,600 lines
(`XRSession`, `XRCompositor`, `VRManager`, `VRVulkanBridge`, `VRProfileDB`,
`CameraDriver`, `HeadPose`, `PadLook`, `StereoState`). These are original
team-authored files. Their PCSX2 references are **interface use** — includes
like `common/Console.h`, `Config.h`, `GS/Renderers/Vulkan/GSTextureVK.h` and
calls to those APIs — which does not transfer PCSX2's protected expression
into our files. All of this can be ported into xemu under a license of our
choosing. (The includes/calls themselves get replaced with xemu equivalents
during the port regardless.)

**Category B — NOT portable (GPL-3-bound, derivative of PCSX2).** The
modifications made *inside* PCSX2's own files to wire the module in —
verified list: `Config.h`, `Hotkeys.cpp`, `VMManager.cpp`,
`GS/Renderers/Common/GSDevice.h`, `GSRenderer.cpp`, `GSRendererHW.cpp`,
`GSTextureCache.cpp`, `GSDeviceVK.cpp`, `SIO/Pad/PadDualshock2.cpp`. These
diffs are inseparable from GPL-3 files and cannot enter xemu as code. This
costs nothing: they are per-emulator glue with no xemu analog — W2/W3 rewrite
that layer against `pgraph/vk/instance.c` and `pgraph_vk_sync` anyway. The
*knowledge* of where and how to hook (the design) is not copyrightable and
transfers freely.

**Required hygiene before/during the port:**
1. **Fix the attribution record in pcsx2-VR first.** The VR module files
   currently carry `SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team` —
   copied boilerplate that wrongly credits PCSX2 with our files and muddies
   the ownership basis for this port. Change Category-A headers to the team's
   own copyright line (keeping `GPL-3.0` as that fork's outbound license is
   fine), and record the dual-license decision somewhere durable (e.g. a
   `pcsx2/VR/LICENSE.note`).
2. **Fragment audit during port review**: confirm no Category-A file contains
   function bodies copied/adapted *from* PCSX2 (the one to eyeball is
   XRCompositor's Vulkan barrier/copy code vs `GSDeviceVK` — calling
   `GSTextureVK::TransitionToLayout` is fine; having pasted its
   implementation would not be). ~4,600 lines: this is an hour of diff-eyeball
   work, not a research project.
3. In xemu, ported files carry the team's copyright + a neighborhood-matching
   license (LGPLv2+ under `hw/xbox/nv2a/`, MIT under `ui/`) — both are
   GPLv2-compatible, so the xemu aggregate stays clean.
4. Same rule applies to the DuckStation fork (GPLv3-pinned) and mupen fork
   (GPLv2+): our `vr_*` files are ours; their emulator-side diffs are theirs.

Doc 10 §4's "xemu is MIT" line remains wrong (the binary is
QEMU-GPLv2-anchored per `pcsx2-VR/docs/emulator-core-licensing.md` §4) — it
constrains only which licenses our *new* files may carry, not whether our own
code may be used.

## 3. Work items

### W0 — Rig + dev-loop bring-up (no VR code)
1. Clone this fork on the rig (`~/xemu_vr`), install deps, build
   (`./build.sh`; confirm the Vulkan backend compiles on Ubuntu 24.04 /
   NVIDIA 595.71.05).
2. Provision Xbox assets: MCPX boot ROM, flash BIOS, HDD image, and at least one
   game. **Known-unknown: where these live** — `~/.local/share/xemu` does not
   exist on the rig (checked 2026-07-16) and no asset location is recorded in
   any sibling repo. User must supply/point to them. Config keys exist for all
   of them (`sys.files.{bootrom,flashrom,eeprom,hdd,dvd}_path`,
   `config_spec.yml:363-367`), so a scripted run is
   `xemu -config_path <toml>` with a self-contained toml — a clean agent
   harness with no interactive setup.
3. Verify flat (non-VR) boot on the rig with `renderer = VULKAN`, on the rig's
   own X11 display. This alone answers two known-unknowns (VK backend stability
   on the RTX 4050; whether the chosen game renders correctly under VK).
   Force the dGPU via the existing config
   `display.vulkan.preferred_physical_device` (`instance.c:387` — no code
   needed), and confirm the boot log doesn't show the silent GL fallback (§1).
4. Stand up a **headless run harness** for agent-driven testing over SSH:
   xemu's UI is a hard SDL window (`ui/xemu.c` main loop — there is no QEMU
   `-display none` path in xemu), so headless means `xvfb-run` (or
   `DISPLAY=:0` onto the rig's live X session — only with user clearance).
   **Corrected expectation:** the VULKAN renderer's display path *requires*
   GL external-memory interop — `HAVE_EXTERNAL_MEMORY` is hardcoded `1`
   (`vk/renderer.h:44`) and `get_framebuffer_surface` has no import-free
   display path — so under Xvfb's llvmpipe GL (no
   `GL_EXT_memory_object_fd`) the VK renderer will likely fail or show
   nothing. Use `renderer = NULL` (or OPENGL) for Xvfb smoke tests; VULKAN
   validation needs the rig's real X + NVIDIA GL. Scripted pattern per the
   pcsx2 runbook: `ssh -o BatchMode=yes -o ConnectTimeout=8 …` under `timeout`.

### W1 — VR module scaffolding
Mirror the DuckStation module map (doc 01) with flat `vr_*` naming. Suggested
home after the review pass: session/compositor/bridge under
`hw/xbox/nv2a/pgraph/vk/` (they are pfifo-thread VK code and that directory's
LGPLv2+ license fits — see §2a); only settings/HUD glue under `ui/`. Modules:
`vr_manager` (facade + settings snapshot), `vr_xr_session` (XrInstance/system/
session/spaces, event pump), `vr_xr_compositor` (swapchain, submit target, copy,
quad layer, pacing), `vr_vulkan_bridge` (seams into xemu's VK instance/device
creation). Meson option `ENABLE_VR` (mirrors `xemu-*` option style); every call
site guarded; all failure paths degrade to flat rendering.

### W2 — Vulkan bridge seams (the real integration risk — now pinned to lines)
xemu creates its own VkInstance/device in `hw/xbox/nv2a/pgraph/vk/instance.c`,
**entirely on the pfifo thread** (`pfifo.c:456` → `pgraph_init_thread` →
`renderer->ops.init` → `pgraph_vk_init_instance`). This is *simpler* than
DuckStation's cross-thread two-phase ordering: Phase A (XrInstance + system +
`XrGraphicsRequirementsVulkan2KHR`) can run inline at the top of
`pgraph_vk_init_instance`, same thread as everything that follows. The bridge
call sequence from PCSX2's `VRVulkanBridge.h` maps 1:1 onto these seams:
- `vkCreateInstance` (`instance.c:230`) → `xrCreateVulkanInstanceKHR` wrapper.
  Note xemu uses volk; `volkLoadInstance()` works on an XR-created instance.
- `select_physical_device` (`instance.c:369`) → `xrGetVulkanGraphicsDevice2KHR`.
  The XR-returned device **overrides** `preferred_physical_device` (spec
  requirement); if they disagree, warn and use the XR one.
- `create_logical_device` (`instance.c:440`) → `xrCreateVulkanDeviceKHR`.
- After `vkGetDeviceQueue` (`instance.c:555`) → Phase B: XrSession + reference
  space + compositor start.
- `pgraph_vk_finalize_instance` (via `pgraph_vk_finalize`) → session/compositor
  teardown *before* device destruction. The runtime **renderer-switch**
  machinery (`pgraph.c:3164-3220`) funnels through the same finalize/init ops,
  so VR teardown/rebuild on a live renderer switch comes for free if the seams
  live there.

Rig pins from S0: **target OpenXR API 1.0** (loader is 1.0.20 — do not use 1.1
headers' `XR_CURRENT_API_VERSION`), `XR_KHR_vulkan_enable2` +
`XrGraphicsBindingVulkan2KHR`, `XR_RUNTIME_JSON` pointed at the WiVRn flatpak
JSON. xemu targets Vulkan instance API `min(driver, 1.3)` and requires ≥ 1.1
(`instance.c:158-167`) — comfortably inside XR requirements, but the bridge
must pass xemu's negotiated version through, not invent its own.

### W3 — Frame hook + quad layer (threading now traced — was an unknown)
**The frame flow, verified in code:** a dedicated vblank timer thread
(`ui/xemu.c:750`) drives `gl_render_frame` at a fixed cadence (even while
paused); it calls `nv2a_get_framebuffer_surface`, whose VK implementation
(`vk/renderer.c:172`) sets `sync_pending`, kicks pfifo, and **blocks on
`sync_complete`**. The **pfifo thread** then runs `pgraph_vk_sync`
(`vk/renderer.c:107`) → `pgraph_vk_render_display` (`vk/display.c:1062`),
which renders the guest surface into `r->display.image` via a render pass and
signals completion; the UI thread samples the imported GL texture.

**Hook site: inside `pgraph_vk_sync`, right after `pgraph_vk_render_display`,
on the pfifo thread.** Consequences, all favorable:
- The pfifo thread is the analog of PCSX2's GS thread — *all* VK queue
  submission already happens there, so the VR copy submits on the same thread
  and queue access stays externally-synchronized by construction (same
  argument as PCSX2's `XRCompositor.h` GPU-copy contract).
- Same-queue submission order makes the copy see the finished display image
  without extra semaphores.
- The vblank-thread pull keeps frames flowing while paused — DuckStation's
  "keep-alive with no extra mechanism" property, for free.

**Correction to the earlier draft:** do NOT "start with xrWaitFrame on the
submitting thread." The pfifo thread executes guest-visible work; parking it
in `xrWaitFrame` would throttle emulation. Adopt PCSX2's **pacer-thread +
single-slot mailbox** model (documented in `XRCompositor.h`) from day one:
pacer runs only `xrWaitFrame`; the pfifo-thread hook consumes the mailbox and
skips submission when empty (XR self-caps at HMD rate, emulation never blocks).

Two concrete code facts for the implementer:
- `r->display.image` is created with usage `SAMPLED | COLOR_ATTACHMENT` only
  (`vk/display.c:615`) — add `TRANSFER_SRC_BIT` for the `vkCmdCopyImage`
  contract (verify the driver accepts it on an external-memory image; NVIDIA
  does), or render/sample into the XR image instead of copying.
- `get_framebuffer_surface` returns early with no sync when no NV2A hardware
  surface exists (`vk/renderer.c:184` — e.g. VGA-blit phases during early
  boot). During those phases no XR frame is produced; the headset holds the
  last image. Acceptable for MVP; note it in testing expectations.

Quad-layer semantics identical to the siblings: world-locked
`XrCompositionLayerQuad` in LOCAL space, pose
`recenter_pose ⊕ (0,0,-ScreenDistance)`, size
`(ScreenHeight × aspect, ScreenHeight)`; recenter flattens VIEW-in-LOCAL to
yaw+position at the next EndFrame; zero layers before first release.

### W4 — Settings + hotkey
Add a `vr` section to `config_spec.yml` (`enable`, `screen_distance`,
`screen_height` — same names/defaults as the siblings: 2.0 m / 1.4 m); the C
config struct is code-generated from the yml at build time
(`meson.build:3766`), so this is a spec edit + rebuild, not hand-written
plumbing. Surface
minimal toggles in the xemu HUD or leave config-file-only for MVP. Recenter as
an SDL keybinding (siblings use F8). Note: xemu has a single global config —
**no per-title layering** (PCSX2's `gamesettings/` top layer has no xemu
equivalent). Irrelevant for Tier-1 MVP; becomes a real design item at the
profile-port stage (doc 10 §7 flagged this — now confirmed).

### W5 — Validation ladder
1. Headless (no headset): a `vr-info`-style probe — port from
   `mupen64plus-VR` tools — proving instance/system/graphics-requirements
   against WiVRn+Monado from xemu's build environment.
2. Flat A/B: `ENABLE_VR` build with `vr.enable = false` behaves identically to
   a non-VR build (offstate discipline, per pcsx2's offstate runbook concept).
3. In-headset Tier-1 check with the user driving (agents cannot wear the
   headset): screen appears, head-tracks, recenter works, desktop mirror
   unaffected, no judder at game 60 Hz vs headset rate.

## 4. Operating the test rig (distilled from the sibling runbooks)

- One shared rig, reached over SSH with key auth (the address and user live in
  the git-ignored `.env.local` at the repo root — template
  `.env.local.example` — never in this public tree; connection verified
  working from this dev box 2026-07-16). Ubuntu 24.04, RTX 4050 Max-Q + AMD 680M iGPU,
  X11 session, Quest 3 over WiVRn 26.6.1 (flatpak), Monado system runtime,
  OpenXR loader 1.0.20. No passwordless sudo — hand sudo lines to the user.
- **Before any VR/GPU action on the rig:** check for live sessions. The pcsx2
  rule generalizes: `pgrep -x pcsx2-qt` **and** `pgrep -f qemu-system-i386`
  (NOT `pgrep -x` for xemu — the binary name `qemu-system-i386` exceeds
  pgrep's 15-char comm limit and `-x` silently never matches) (and mupen —
  three emulators now share this rig/headset). A live session means someone may
  be in the headset: stop and ask. Field incident 2026-07-14 (headset seized
  mid-play, evidence log truncated) is why this is a hard rule.
- WiVRn rules: never stop `wivrn-server` while a client is connected (26.6.1
  segfaults); recovery order is quit emulator → close headset client → restart
  server; the WiVRn dashboard cannot be started over SSH.
- Builds happen on the rig from its own checkout (no cross-copying binaries
  from Windows/WSL). Prefer read-only operations without explicit clearance.
  Adopt pcsx2's drift discipline (`rig-diff.sh` pattern) once we deploy
  configs/profiles rig-ward.
- Git identity: never commit as "Claude Code" — set author AND committer to the
  user's real name/email on every commit, both machines.
- Current rig state (2026-07-16): no xemu checkout, no xemu assets, no live
  emulator process. `~/pcsx2-VR`, `~/mupen64plus-VR`, `~/roms` present.

## 5. Known-unknowns (deliberately left open)

Resolved by the 2026-07-16 deep-review pass (kept here so the next reviewer
doesn't re-open them):
- ~~U3 threading/lock model~~ — **resolved**: all VK work runs on the pfifo
  thread; hook in `pgraph_vk_sync`; pacer thread for `xrWaitFrame` (see W3).
- ~~U4 init ordering / renderer switching~~ — **mostly resolved**: init and
  teardown both run on the pfifo thread through the renderer ops, and runtime
  renderer-switch machinery exists (`pgraph.c:3164-3220`) funneling through
  the same ops (see W2). Residual: verify `vr.enable` toggle → renderer
  reinit end-to-end once implemented.

| # | Unknown | Why it matters / first probe |
|---|---|---|
| U1 | **Xbox asset provisioning** — mostly RESOLVED 2026-07-16 via the user's RetroDECK SD card (mounted on the rig at `/media/pacarey/SD512`): 30-title XISO library, `xbox-eeprom.bin`, and `xbox_hdd.qcow2` (pristine blank) copied to rig `~/xemu-assets/` + `~/roms/xbox/`. **Still missing: MCPX boot ROM + flash BIOS dump** — absent from the card (RetroDECK xemu setup was never completed); user must supply. | Title strategy (revised per user's firsthand testing, 2026-07-16): **Operation Flashpoint: Elite** is the Tier-1 bring-up title (user-confirmed working in xemu; FPS camera useful for later tiers; smallest image in the library). **MechAssault 2 does NOT boot in xemu despite its xemu.app "Playable" rating — do not use it**; treat user boot reports as authoritative over compat-site ratings. **Jedi Outcast / Jedi Academy** remain the Tier-3 anchors (PC engine source publicly released, GPL, Raven 2013); Riddick, Ghost Recon, Unreal II/Championship, KOTOR 1/2, Fuzion Frenzy as alternates. Halo CE not owned. |
| U2 | **xemu VK renderer maturity** on this rig/driver and for the chosen title | W0.3 answers it empirically; fallback is the GL backend + a real GL→VK bridge (mupen's design), a much bigger MVP. |
| U5 | **Headless viability** — narrowed by review: VULKAN-renderer display *requires* NVIDIA GL interop (won't work under Xvfb/llvmpipe); open part is whether `renderer = NULL`/OPENGL under Xvfb boots far enough to be a useful smoke test | W0.4. |
| U6 | **Frame hook details** — pvideo overlay interaction, surface scaling, and whether `TRANSFER_SRC` can be added to the external-memory display image on this driver (or sample instead of copy) | W3; read `vk/display.c` fully before choosing copy-vs-sample. |
| U7 | **Aspect/geometry** — Xbox 480i/480p anamorphic + widescreen (`ui/xemu-widescreen.c`) vs the quad's aspect | W3/W4; siblings size the quad from content aspect. |
| U8 | **Per-title config layering absent** in xemu | Post-MVP (profile port); design a sidecar per-title store keyed on XBE Title ID (surfaced in `ui/xui/main-menu.cc`). |
| U9 | **Upstream sync policy** for this fork (fork point is recent upstream, 2026-06-28; upstream moves fast) | Team decision; affects how invasive the pgraph hooks should be. |
| U10 | **Which xemu process name** to add to the live-session check, and whether `~/vr-xbox.sh` launcher conventions should mirror `~/vr-n64.sh` | W0/W5 hygiene; confirm binary name (`xemu`) once built on the rig. |
| ~~U11~~ | **Resolved** (user, 2026-07-16): the VR framework is team-authored and can be ported into xemu directly (§2a). Residual hygiene only: strip PCSX2-derived fragments and re-header during the port. | Closed. |
| U12 | **GL-window + VK-XR-session coexistence on WiVRn** — xemu's SDL window stays GL while the XR session is Vulkan; mupen's S0 proved GL *XR sessions* are poison but this exact combination hasn't run on the rig yet | First in-headset run (W5.3) validates; no GL XR session is ever created, so risk is low. |

## 6. Increment log

**Increment 1 (2026-07-16, `efb455bf15`):** W1 inert skeleton — vr_manager /
vr_xr_session / vr_xr_compositor stubs, all five instance.c seams, the
pgraph_vk_sync frame hook, display-image TRANSFER_SRC, `[vr]` config section
(incl. D5 params), port map committed. Builds green on the rig.

**Increment 2 (2026-07-16, `55db038440`):** OpenXR loader + real session
port. `vr_xr_loader.{h,c}` (dlopen `libopenxr_loader.so.1`, PFN table —
port-map D1); Khronos OpenXR headers vendored under `pgraph/vk/openxr/`
(Apache-2.0 OR MIT); `vr_xr_session.c` is the full C port of XRSession.cpp
(Phase A pinned to OpenXR 1.0 + vulkan_enable2 [+cylinder probe], the three
Vulkan-through-XR wrappers, session + LOCAL space, event pump);
`vr_manager.c` carries the PCSX2 bridge failure contract (seam failure →
abort VR internally → fall back to the plain Vulkan call — renderer init can
never fail from VR). Compositor still stubbed: with `vr.enable=true` the
code proves Phase A+B then tears down and runs flat. Headless probe on the
rig (`~/xemu-vr-probe/`): dlopen OK, `XR_KHR_vulkan_enable2` YES, cylinder
YES (`PROBE_OK`; 24 exts = Monado default runtime — set `XR_RUNTIME_JSON`
to WiVRn's JSON for real sessions). In-session Phase A/B validation still
pending — S0 showed headless xrCreateInstance cannot work over bare SSH; do
not mistake that for a code failure.

**Increment 3 (2026-07-16):** the full compositor port
(`vr_xr_compositor.c` ← XRCompositor.cpp, logic verbatim in C). Pacer thread
(`qemu_thread` "vr-pacer", xrWaitFrame only) + single-slot mailbox with the
`begin_owed` / `wait_pending` / shutdown-drain machinery reproduced exactly;
lazy R8G8B8A8_SRGB swapchain (recreate on display-size change); 2-deep
fence-ring copy on `r->queue` (pfifo thread — externally synchronized by
construction). **D3 layout decision:** `render_display` leaves
`r->display.image` in `SHADER_READ_ONLY_OPTIMAL` (display.c's post-render
transition; next frame it re-transitions from UNDEFINED, but the GL mirror
samples it right after our hook) — the copy barriers it
SHADER_READ_ONLY→TRANSFER_SRC and **back**, so the GL side always sees the
expected layout. Quad layer in LOCAL space (distance/height/vertical-offset
from `g_config.vr.*`), cylinder layer when `screen_arc ≥ 5°` and the runtime
offers it; zero layers before first release / on wait-timeout. Head pose
(VIEW-in-LOCAL) stashed per frame for the Tier-3 feed (no consumer yet).
`vr_manager.c`'s frame hook now pumps XR events first (READY → begin is what
un-parks the pacer) and tears VR down on session/instance loss while flat
rendering continues. Deviations from PCSX2: mono-only layer build (chains[1]
dormant), `ComputeAspect` = display-image ratio (D8, U7 note), screen params
read directly from `g_config.vr.*` instead of atomics (same-thread reads;
PCSX2 needed atomics for its CPU→GS split), HeadPose TU not ported (pose
stashed in the compositor struct — out-of-scope file). EnsureFrameSubmitted
intentionally not ported (§5 of the port map).

**Next:** in-session Phase A→frame-loop validation on the rig (WiVRn
`XR_RUNTIME_JSON`, user present), then the first in-headset Tier-1 check
(W5.3); settings/hotkey polish (W4 UI surface, recenter) after the loop is
proven.

## 7. Deep review (Fable 5 max pass, 2026-07-16)

Full adversarial read of increments 1-3 against the PCSX2 originals, the
pgraph/vk code they hook, and the OpenXR contract. Verdict: **the port is
faithful and the architecture holds.** Findings, most severe first:

**F1 — FIXED (code): owed-begin wedge.** `vr_compositor_frame` early-returns
when the mailbox is empty, but the `begin_owed` retry lived only *after* the
mailbox consume. One transient `xrBeginFrame` failure → pacer parked in
`xrWaitFrame` waiting for that begin → mailbox never refills → the retry
path is unreachable → frame loop wedged until shutdown. Fixed by settling
owed begins before the mailbox check (`vr_xr_compositor.c`).

**F2 — noted (spec wart, deliberate): `xrEndSession` from non-STOPPING.**
`vr_xr_destroy_session` calls `xrEndSession` even when the state machine is
FOCUSED. Spec-wise that returns `XR_ERROR_SESSION_NOT_STOPPING`; the result
is ignored and `xrDestroySession` (always legal) cleans up. Correct behavior,
one silent spec violation. The graceful alternative (`xrRequestExitSession` +
bounded pump to STOPPING) needs one more PFN and ~20 lines — post-MVP polish.

**F3 — FIXED (operational): runtime selection missing.** Nothing set
`XR_RUNTIME_JSON`, so an enabled-VR run would bind the system default
runtime — Monado, which has no HMD here — and silently fall back to flat.
The in-headset test would have "worked flat" with a healthy-looking log.
Canonical launcher now lives at `tools/vr/rig-boot-test.sh` (WiVRn manifest
autodetect + compositor-socket pre-check + PRIME forcing) and is deployed as
`~/xemu-assets/boot-test.sh`.

**F4 — architecture note (accepted): VR frame cost rides the pfifo thread
while the UI waits.** The hook runs inside `pgraph_vk_sync`, i.e. between
the UI thread's sync request and `sync_complete`. xrBegin/EndFrame + the
copy submit add latency to both emulation-side pgraph processing and the
desktop mirror; a worst-case `xrWaitSwapchainImage` stall is bounded by our
1s timeout with the zero-layer escape. This is the same class of tradeoff
PCSX2 accepts on its GS thread, with one extra coupling (the blocked UI
pull). If in-headset testing shows mirror hitches: options are a second
VkQueue (NVIDIA exposes many per family) driven from a dedicated submit
thread, or decoupling the mirror pull from the VR submit. Measure first.

**F5 — operational caveat: the working BIOS is a debug kernel.**
`xbox-4627_debug.bin` boots games (proven: OFP Elite); the retail
`xbox-3944_256k.bin` lands on the service screen regardless of title. Debug
kernels can behave differently from retail for some titles (devkit paths,
memory-layout probes). If a title misbehaves inexplicably, suspect the BIOS
before the emulator; acquiring a retail 5530/5713/5838-era dump as a spare
remains worthwhile.

**F6 — reviewed clean:** the dlopen loader (idempotent init, complete PFN
table — all 27 instance-level calls used are resolved, failures degrade
flat); session state machine incl. re-begin after STOPPING→READY;
loss handling (pump sets `lost`, manager frame hook tears down to flat —
covers WiVRn server death mid-play); teardown ordering (pacer joined via
the drain protocol before session destroy, fences waited before pool/device
teardown, all inside `pgraph_vk_finalize_instance` before `vkDestroyDevice`);
the D3 barrier-back decision (display image restored to
`SHADER_READ_ONLY_OPTIMAL`, matching `render_display`'s final layout, since
the GL mirror samples it immediately after `sync_complete`); swapchain
usage/format contract (`R8G8B8A8_SRGB` required, raw UNORM-byte copy = the
siblings' gamma rule); acquire/wait/release edge machinery (re-wait on
timeout, release-after-successful-wait-only, `ever_released` gating layer
submission). The mailbox can never hold more than one frame by xrWaitFrame's
own blocking semantics — no overwrite path exists.

**Operational requirements pinned by this review** (launcher enforces all):
1. `__NV_PRIME_RENDER_OFFLOAD=1 __GLX_VENDOR_LIBRARY_NAME=nvidia` — MANDATORY
   with VR on: XR pins Vulkan to the NVIDIA GPU; the VK→GL mirror import
   requires GL on the same device.
2. `display.vulkan.preferred_physical_device = "NVIDIA GeForce RTX 4050
   Laptop GPU"` for flat runs (xemu auto-writes this key back on exit —
   check it after config edits).
3. `XR_RUNTIME_JSON` → WiVRn manifest for headset runs (F3).
4. Live-session check before rig VR actions: `pgrep -x pcsx2-qt`,
   `pgrep -x mupen64plus`, `pgrep -f qemu-system-i386` (`-x` misses names
   >15 chars) — and never chain a launch behind the check in one command.

## 8. First in-headset test — pre-flight checklist & expectations

Second review pass (2026-07-16) specifically hunting first-test failure
modes. Additional fixes beyond §7: the toml's `[vr]` section is now managed
by the launcher via `XEMU_VR=1` (xemu's auto-save omits default-valued
sections, so the section the user was told to edit had vanished from the
deployed toml); the Vulkan apiVersion clamp from doc 01 task #5 was ported
(missing); the WiVRn socket pre-check covers both known socket paths.
Verified-not-problems: D3 layout claims match `render_display`'s actual
barriers (UNDEFINED→COLOR_ATTACHMENT→render→SHADER_READ_ONLY, blocking
submit — our copy can never race the producer); gamepad bindings already
present in the rig toml; `[vr]` codegen proven by the compiling
`g_config.vr.*` references.

**Test procedure (user, at the rig):**
1. WiVRn server running, Quest 3 connected via the WiVRn client (headset
   awake, in the WiVRn waiting room).
2. From a DESKTOP terminal on the rig (not bare SSH — the XR runtime needs
   the session environment): `XEMU_VR=1 ~/xemu-assets/boot-test.sh`
3. Expected stderr sequence:
   `xemu-vr launcher: VR ENABLED` → `bootstrapping OpenXR (Phase A)` →
   `OpenXR runtime: WiVRn 26.6.1` (NOT "Monado"!) → `HMD: Meta Quest 3` →
   `runtime supports Vulkan a.b - c.d` → `Physical device selected by
   OpenXR runtime: NVIDIA GeForce RTX 4050 Laptop GPU` → `XR session
   created` → `compositor initialized` → `session state: UNKNOWN -> IDLE ->
   READY` → `session began` → `XR swapchain created (eye 0): WxH` —
   then the screen appears in the headset.
4. The headset stays black/void between "session began" and the guest's
   first NV2A-rendered frame (VGA-blit boot phases produce no XR frames) —
   a few seconds is normal.

**Failure → meaning table:**
| symptom in log | meaning |
|---|---|
| `runtime: Monado` instead of WiVRn | XR_RUNTIME_JSON not applied — run via the launcher, not the binary directly |
| `no HMD system available` | headset not connected/awake in WiVRn (or wrong runtime selected) |
| `xrCreateInstance failed` | WiVRn server not running, or its compositor socket unreachable from this environment |
| stuck at `IDLE`, never `READY` | WiVRn client connected but app session not granted — check nothing else holds an XR session (another emulator, a leftover process) |
| `session state: ... -> STOPPING` shortly after start | headset client disconnected mid-run (WiVRn sleep/standby) |
| flat mirror fine, headset black, log healthy incl. swapchain | report immediately — that's a compositor-layer bug (F-class), not setup |
| desktop mirror minor tearing with VR on | known accepted risk (async copy's layout round-trip vs GL sampling — same-vendor NVIDIA tolerates; report if visible) |

**Post-test:** whatever happens, capture `~/xemu-vr-*.log` / terminal
scrollback and hand the `xemu-vr:` lines to the agent. If the session ran:
check WiVRn recovery order before quitting anything (quit xemu first, then
headset client — never restart wivrn-server with a client connected).

**F8 — field-test finding (2026-07-16, FIRST IN-HEADSET RUN): image
vertically inverted.** The launch itself succeeded end-to-end on the first
live attempt — WiVRn runtime selection, Phase A/B, session, pacer,
swapchain, copy, quad layer all worked; OFP Elite appeared in the Quest 3,
upside down. Root cause: xemu's display image is GL-heritage memory (row 0
= picture bottom — exactly why the GL desktop mirror shows it correctly and
`flip_required=false` there); Vulkan/OpenXR interprets row 0 as top. PCSX2
never hit this because GSTextureVK is Vulkan-native. Fix: the swapchain
copy is now a per-row Y-flip `vkCmdCopyImage` (one single-row region per
line — raw bytes preserved; a `vkCmdBlitImage` UNORM→SRGB would have
re-encoded gamma and washed the image out). Region table rebuilt on size
change, freed with the chains.

---

**TIER-1 MVP SIGNED OFF — 2026-07-16.** User-validated in-headset: OFP Elite
on a world-locked screen in the Quest 3 over WiVRn, orientation correct
after F8, desktop mirror unaffected. Timeline: plan written, licensing
settled, rig provisioned, firmware sourced, three code increments, two
adversarial review passes, first launch worked end-to-end, one field bug
(F8) fixed and confirmed — all in one day. Remaining W4 polish (recenter
hotkey, HUD toggle) and the Tier-2/Tier-3 roadmap pick up from here.

## 9. Tier-3 head-look: infrastructure + the OFP hunt (2026-07-17)

Landed (`vr_camera.c` + config + F9 + `tools/vr/hunt_scan.py`): the Tier-3
find-and-poke loop, MVP-scoped to two float pokes ahead of the full profile
DB. Design deltas vs the PS2/N64 siblings, all simplifications:
- **Physical addresses end-to-end.** The Xbox's 64 MB is unified and host-
  mapped (`d->vram_ptr`); the hunt dumps physical RAM and the poke writes
  physical RAM — no VA translation, and a found address IS the poke address.
- **Freelook-style zeroing.** At enable and at every F8 recenter, the game's
  current yaw/pitch are adopted as base; pokes write base + head_delta ×
  sensitivity. F8 = "new forward" for screen AND camera at once.
- **Hunt tooling is built in**: `[vr] hunt_enable = true` + F9 → full-RAM
  snapshot with head-pose metadata to `~/xemu-vr-hunt/` (memcpy on the
  pfifo thread ~10 ms, file write on a detached worker).

**Hunt protocol (OFP: Elite, first title):**
1. `[vr] hunt_enable = true`, launch flat or VR, get in-mission.
2. Put the camera at ORIENTATIONS WE CAN WRITE DOWN, F9 at each. How you
   turn doesn't matter (normal aim controls); what matters is the heading
   at the moment of the press. Yaw pass: **align the view to the in-game
   compass** — bearing 0 → F9, 90 → F9, 180 → F9, 270 → F9, 0 again → F9
   (the repeat filters self-drifting values; a few degrees of slop is
   fine — the scanner tolerates 15% on the 90° steps and only deltas
   matter, not absolute zero). Pitch pass uses the engine's own clamps as
   references: sights level on the horizon(0) → F9, view full up until it
   stops climbing(+85) → F9, level again(0) → F9, full down until it
   stops(−85) → F9 — the true clamp being 80-90° is within tolerance.
   Every reference is a hold-still pose, so one hand can leave the pad to
   tap F9.
3. `tools/vr/hunt_scan.py yaw dump-000.bin=0 dump-001.bin=90 ...` — prints
   physical addresses whose deltas match the script (rad/deg, ± variants).
4. Set `[vr] headlook_yaw_addr/'headlook_pitch_addr' = '0x...'`,
   `headlook_enable = true`, sensitivity 1.0 (rad engine) or 57.2958 (deg);
   `headlook_invert_pitch` / negative sensitivity fix signs.
5. Verify in-headset; F8 re-zeros. Known risks, by design: the address can
   move across boots (re-hunt or graduate to an AOB anchor — the profile-DB
   port's job); if the engine stores a matrix/quaternion instead of eulers,
   the scanner reports nothing and Tier-3 for that title needs the full
   pattern machinery (doc 10 §2 Tier 3). OFP heritage (TrackIR existed on
   PC OFP) makes plain freelook angles likely.

### §9a — OFP ADS gate (user requirement, 2026-07-17)

**Head-look must fire ONLY while aiming down sights.** Implemented as a
byte gate: when `[vr] headlook_gate_addr` is set, pokes run only while
`*(u8*)gate_addr == headlook_gate_value`. The gate's rising edge re-zeros
both the game base and the head reference (raising the sights never snaps
the view; head deltas count from that moment), and the falling edge simply
stops poking — the game owns its camera again instantly. While gated-on,
compose mode folds the game's own writes (stick aim) into the base each
frame, so stick and head aim ADD rather than fight.

**Gate truth table (user spec, 2026-07-17):** OFP has hip, plain zoom,
ADS, ADS+zoom, and third-person views. Head-look must fire for **ADS and
ADS+zoom ONLY** — never plain zoom, never third person. These are likely
distinct values of one view-mode enum, so the gate is a mask test
(`(byte & gate_mask) == gate_value`) and the scanner suggests mask+value
that match the ON set while excluding the OFF set.

**Dump protocol for hunting the gate — user-facing, please follow
exactly** (F9 dumps are our "save states"; the flag hunt diffs bytes, so
minimizing time and motion between dumps is what makes it converge):

1. `[vr] hunt_enable = true`, get in-mission, stand somewhere quiet with a
   scoped-capable weapon. **Do not move, do not turn** for the whole
   sequence — only the view state changes.
2. Dump each state with **F9**, in this order, and WRITE DOWN the order:
   - hip → F9
   - plain zoom (no sights) → F9
   - ADS → F9
   - ADS + zoom → F9
   - back to hip → F9  *(repeat pass; strongly recommended: do the whole
     cycle a second time for 8-10 dumps total)*
   - third person → F9 *(once is enough)*
3. Tell the agent the dump-number → state mapping (the .txt metadata
   records head pose, not game state — the grouping must come from you).
4. Analysis: `tools/vr/hunt_scan.py flag on=<ads dumps> on=<ads+zoom dumps>
   off=<hip> off=<zoom> off=<thirdperson> ...` → candidates with suggested
   `gate_mask`/`gate_value`. Wire `headlook_gate_addr` + both keys, keep
   `headlook_enable = true`.
5. Verify all five states in-headset: head-look live on sights and zoomed
   sights, dead at hip / plain zoom / third person, no snap at any
   transition (rising edge re-zeros).

The yaw/pitch hunt (§9 step 2) should be run while ADS if possible — the
angles that matter are the ones the sights use; OFP may share one camera
for hip/ADS (fine either way, the gate decides when we write).

### §9b — Durable camera anchoring (implemented 2026-07-19)

**Problem found in-headset:** the OFP camera is a heap/D3D-pool object that
**relocates every boot**, so the absolute yaw/pitch poke addresses read dead
memory on the next launch (the ADS gate, in a low stable region, survived —
only the camera moved). A 5-agent investigation (scratchpad/inv-1..5) plus
re-analysis of the 18-dump capture corrected the targets and set the fix:

- The real camera struct base is `0x03A70388`; the **real yaw is base+0xC18**
  and the **real pitch is base+0xC1C** (yaw+4, the adjacent field). Both are
  degrees, negated. The old "primary" `0x03A5D8C4` was an isolated mirror; the
  old pitch `0x03EA6594` was a separate allocation that read garbage on the
  "look down" dump — the in-struct `+0xC1C` is valid there.
- A **static pointer anchors it: `0x00079D64 → 0x03A70388`**, holding that base
  in **all 18 dumps** (a second referrer `0x000DFFA8 → 0x03A7037C` agrees).
  Both yaw (+0xC18) and pitch (+0xC1C) were verified to resolve through it
  across the whole capture (scratchpad ptr_check/pitch_pin2/confirm). The
  anchor is proven session-stable; only **cross-boot** stability is unproven —
  that is the rig reboot test. `0x79D64` is a low static-region global (XBE, no
  ASLR), so the odds are good. Fallback if it fails: a **gdbstub `-s`
  write-watchpoint** on VA `0x83A70FA0` reveals the writing instruction's base
  register + offset directly.

**What shipped** (commits 2c2eb42ebf, 0cac0da160): `resolve_cam_base()` in
`vr_camera.c`, driven by new `[vr]` keys. `headlook_base_mode`:

- `''` (default) — absolute addresses, exactly the pre-existing behavior.
- `pointer` — `base = deref(headlook_base_ptr) + headlook_base_offset` each
  frame (one u32 read; optional `headlook_base_validate_offset/_equals` sanity
  dword). The robust default when a static pointer to the struct exists.
- `aob` — masked signature scan (`headlook_base_aob = "AA BB ?? DD"`), cached +
  revalidated, throttled rescan; fallback when no stable pointer exists.

In pointer/aob mode `headlook_yaw_addr`/`_pitch_addr` become **offsets** from
the resolved base; the gate stays absolute. An unresolved base sidelines the
poke and re-zeros on return (no snap, no dead-memory write). **Pitch is
optional** (yaw required), so an anchor can be proven for yaw before pitch is
pinned. `wrap_period()` + `headlook_engine_period` (OFP = 360) wrap the yaw
seam in engine units — a degrees engine's ±180° stick-aim crossing corrupted
the old radians-only `WRAP_PI` fold.

**Turn-key OFP pointer test** (rig `~/xemu-assets/xemu-test.toml` `[vr]`):
`headlook_base_mode='pointer'`, `headlook_base_ptr='0x00079D64'`,
`headlook_yaw_addr='0xC18'`, `headlook_pitch_addr='0xC1C'`,
`headlook_gate_addr='0x007ADB90'`, `headlook_gate_value=1`,
`headlook_sensitivity=-57.2958`, `headlook_invert_pitch=false`,
`headlook_engine_period=360.0`, `headlook_enable=true`. Deploy: fix the rig
credential (ISS-B09) → `git reset --hard origin/vr/mvp` → `ninja -C build`,
or scp the 4 changed sources. **Pass = head-look (yaw + pitch) tracks durably
across a reboot** → OFP is the first durable profile → port the profile-DB
(Title-ID keyed) with OFP as the first entry.

## 10. Backlog — Quest Touch controllers as emulator input (assessed 2026-07-17)

Verdict: moderate backend, near-zero UI. OpenXR action system (~400 lines
through the existing PFN table; `oculus/touch_controller` profile, WiVRn
supports it) feeding an **SDL3 virtual gamepad** (`SDL_AttachVirtualJoystick`)
— xemu's existing controller stack (ports, rebinding, settings UI,
auto-bind) then handles it as a normal pad, no new UI. pcsx2-VR analog: a
new modular InputSource; existing Qt binding dialogs pick it up. Real
design work: the Touch→Xbox mapping table (Touch has NO D-PAD — OFP squad
commands need a chord/layer decision); pfifo→UI-thread state mailbox
(trivial, recenter-flag pattern). Bonus once actions exist: emulated-pad
rumble → xrApplyHapticFeedback (feel the rifle kick). Sequencing: after
the OFP Tier-3 hunt validates; xemu first, then port the pattern to pcsx2.

### §10a — Gesture→button mapping (idea, 2026-07-17)

Natural extension once controller pose actions exist (§10): a small
recognizer watches Touch controller poses relative to HEAD (no torso
tracking — head is the only body anchor) and synthesizes a virtual-pad
button on a detected gesture. Example: lateral arm-thrust → stiff-arm in
ESPN NFL 2K5. Design notes: use a VELOCITY-gated gesture, not a static
pose (else it fires whenever the arm rests out); inherent recognition
latency makes this best for gesture-IS-the-fantasy moves with forgiving
timing (swing/punch/shove) rather than twitch-timed inputs — football
stiff-arm is maximally evocative but marginally practical vs a button.
Recognizer is a ~40-line state machine feeding the existing SDL
virtual-gamepad bridge; no new UI.
