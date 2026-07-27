# xemu-VR — Port map: pcsx2-VR Vulkan OpenXR code → xemu

Status: source analysis complete, 2026-07-16. Read against pcsx2-VR @ current
master (`pcsx2/VR/`: XRSession, XRCompositor, VRVulkanBridge, VRInternal,
VRManager — all headers already re-attributed `SPDX-FileCopyrightText: 2026
Patrick Carey`, outbound `GPL-3.0`; re-license on port per plan §2a).
Companion to `00-mvp-plan.md` (W1–W3). Line refs are into pcsx2-VR.

## 0. Fragment-audit verdict (plan §2a hygiene item 2)

**Clean.** All five TUs are self-contained OpenXR/Vulkan logic. The only
PCSX2-proper touchpoints are *interface calls* (listed per function below):
`GSTextureVK::TransitionToLayout/GetImage/GetWidth/GetHeight/GetArrayLayers`,
`GSDeviceVK::GetInstance()->ExecuteCommandBuffer`, `GSConfig.AspectRatio`,
`EmuConfig.VR`, `VMManager::GetDiscSerial/GetDiscCRC`,
`Host::AddKeyedOSDMessage`, `Console.*`, `Threading::SetNameOfCurrentThread`,
`common/Pcsx2Defs.h` typedefs, `fmt::format`. No function bodies resemble
GSDeviceVK internals; `ImageBarrier()` (XRCompositor.cpp:150) is a generic
15-line idiomatic vkCmdPipelineBarrier wrapper written in the module's own
style, not a lift. Nothing to strip; only interfaces to replace.

## 1. OpenXR loader linkage — the one build-system delta

PCSX2 **statically links a vendored OpenXR loader** (`3rdparty/openxr`,
`target_link_libraries(PCSX2_FLAGS INTERFACE openxr_loader)`,
SearchForStuff.cmake:110-122) and calls `xr*` symbols directly. Only the
`XR_KHR_vulkan_enable2` entry points are fetched via `xrGetInstanceProcAddr`
(XRSession.cpp:37-40, 192-207) because the loader doesn't export them.

For xemu the plan is `dlopen("libopenxr_loader.so.1")`. Consequences:
- Every direct `xr*` call in the port (~40 call sites) must go through
  function pointers: `dlsym` only `xrGetInstanceProcAddr`, then fetch
  `xrCreateInstance`, `xrEnumerateInstanceExtensionProperties`, etc. via
  `xrGetInstanceProcAddr(XR_NULL_HANDLE, ...)` (spec-legal for the
  loader-exported subset), and instance-scoped ones after instance creation.
  Mechanical but touches every function; wrap in a `vr_xr_loader.{h,cpp}`
  that mirrors volk's pattern (a `struct` of PFNs + a load function).
- Alternative (closer-to-verbatim port): vendor OpenXR-SDK as a meson
  subproject wrap like PCSX2 does with CMake. xemu already vendors volk,
  glslang, SPIRV-Reflect the same way. **Decision needed** — dlopen keeps the
  binary free of a hard libopenxr dependency (VR-off users unaffected), the
  wrap keeps the diff minimal. Recommend dlopen for a QEMU-style project.
- Headers: `#include <openxr/openxr.h>` + `#define XR_USE_GRAPHICS_API_VULKAN`
  + `<openxr/openxr_platform.h>` still needed at compile time — vendor the
  OpenXR headers (headers-only, no loader) under `thirdparty/` or a wrap.
  Pin to a 1.0-era or current SDK; **target `XR_API_VERSION_1_0` at runtime**
  regardless (rig loader is 1.0.20; do not pass 1.1 CURRENT_API_VERSION).

## 2. Language/primitive mapping (port-wide)

The PCSX2 module is C++ (std::thread/mutex/atomic, fmt, vectors). xemu's
`hw/xbox/nv2a/` is C. **Recommended: keep the VR TUs C++** (xemu already
links C++: imgui, xemu-settings.cc; meson mixes fine) but make them
**self-contained** — do NOT include QEMU/nv2a headers from C++ TUs (QEMU
headers are not C++-clean). Boundary: a small `extern "C"` surface
(`vr_hooks.h`) taking plain handles. Then std::thread/std::mutex/std::atomic
port verbatim and no qemu_thread rewrite is needed.

| PCSX2 dependency | xemu replacement |
|---|---|
| `common/Pcsx2Defs.h` (`u32`, `s32`) | `<cstdint>` (`uint32_t`, `int32_t`) |
| `Console.WriteLn/Warning/Error` | `fprintf(stderr, "xemu-vr: ...")`; user-facing one-shots via `xemu_queue_notification()`/`xemu_queue_error_message()` **through the C boundary** (they're C, ui/xemu-notifications.h) |
| `Threading::SetNameOfCurrentThread` | `pthread_setname_np` (Linux-only is fine for MVP) |
| `fmt::format` (VRManager only) | `snprintf`/std::string append |
| `pxAssertRel` | `assert` |
| VKLoader.h (vk fn pointers) | **volk** (`volk.h`, already vendored & used by pgraph/vk; C header, links against the same process-wide volk symbols — vk pointers are already loaded by `pgraph_vk_init_instance`) |
| `Internal::VulkanHandles` {instance, physical_device, device, queue, queue_family} | populate from `PGRAPHVkState` (`r->instance`, `r->physical_device`, `r->device`, `r->queue`, queue-family index from `instance.c`'s `QueueFamilyIndices`) at the Phase-B seam |
| GS thread | **pfifo thread** (identical single-thread ownership incl. queue submission) |
| `GSTextureVK* current` (EndOfFrame arg) | `VkImage` + extent + layout of `r->display.image` passed through the C boundary |

## 3. Pacer-thread + mailbox contract (reproduce exactly)

As implemented (XRCompositor.cpp:66-129, 466-507, 925-958):

- **State**: `std::thread pacer; std::atomic_bool pacer_exit, pacer_done;
  std::mutex frame_mutex` guarding `{XrFrameState pending_frame_state; bool
  has_pending_frame}` — a **single-slot mailbox** (newer publish overwrites).
- **Pacer loop**: while `!pacer_exit`: if `!IsSessionRunning()` (atomic read)
  → sleep 20 ms, continue. Else `xrWaitFrame(session, nullptr, &fs)`:
  - `XR_ERROR_SESSION_NOT_RUNNING` → continue (benign STOPPING race);
  - other failure → warn once, sleep 100 ms, continue (session loss arrives
    via events on the consumer thread);
  - success → lock, store fs, `has_pending_frame = true`.
  On exit sets `pacer_done = true`. **No condition variables anywhere**:
  xrWaitFrame's own "blocks until the previous frame is begun" semantics are
  the pacer↔consumer handshake.
- **Consumer** (per-frame hook, xemu: `pgraph_vk_sync`): lock, if no pending
  frame → return (submit nothing this visit); else take fs, clear flag. So
  the pfifo thread NEVER blocks on XR pacing — matches plan W3's correction.
- **`begin_owed` invariant** (:78-84, 509-535): if `xrBeginFrame` fails after
  a mailbox frame was consumed, the pacer's next `xrWaitFrame` stays blocked
  until *some* frame is begun. The consumer records `begin_owed` (+ the
  displayTime) and every later visit first retries begin + zero-layer end
  (`SettleOwedBegin`) before normal processing. Prevents an unbounded pacer
  join at teardown. `XR_ERROR_SESSION_NOT_RUNNING` on the retry clears the
  debt (frame-loop state resets on next session begin).
- **Shutdown drain** (:925-958): (1) `pacer_exit = true`; (2) while
  `!pacer_done` (bounded 2 s, 50 ms poll): `SettleOwedBegin()` +
  `DrainOnePacedFrameForShutdown()` — which consumes a pending mailbox frame
  with begin + zero-layer end so a pacer blocked inside xrWaitFrame returns;
  (3) `pacer.join()`; (4) destroy Vulkan/XR resources. `pacer_done` exists
  because std::thread has no timed join.

## 4. OpenXR call sequences (as-built, with extensions)

**Bring-up** (split across bridge steps; xemu seams in parentheses):
1. `xrEnumerateInstanceExtensionProperties` → probe optional
   `XR_KHR_composition_layer_cylinder` (XRSession.cpp:130-145).
2. `xrCreateInstance` — app/engine "PCSX2" (→ "xemu"), apiVersion
   `XR_API_VERSION_1_0`, extensions: `XR_KHR_vulkan_enable2` [+ cylinder if
   offered]. Failure = warn + run flat, not an error.
3. `xrGetInstanceProperties` (log runtime), `xrGetSystem(HMD form factor)`
   (failure → destroy instance, flat), `xrGetSystemProperties` (log).
4. Fetch via `xrGetInstanceProcAddr`: `xrGetVulkanGraphicsRequirements2KHR`,
   `xrCreateVulkanInstanceKHR`, `xrGetVulkanGraphicsDevice2KHR`,
   `xrCreateVulkanDeviceKHR`.
5. `xrGetVulkanGraphicsRequirements2KHR` — **mandatory before session
   creation** (else `XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING`). Steps 1-5
   = Phase A (xemu: top of `pgraph_vk_init_instance`, pfifo thread).
6. `xrCreateVulkanInstanceKHR` (wraps app's `VkInstanceCreateInfo`;
   `pfnGetInstanceProcAddr` = the loader's `vkGetInstanceProcAddr` — xemu:
   volk's) → replaces `vkCreateInstance` (instance.c:230).
7. `xrGetVulkanGraphicsDevice2KHR` → the returned VkPhysicalDevice MUST be
   used (overrides `preferred_physical_device`; instance.c:369 seam).
8. `xrCreateVulkanDeviceKHR` (wraps `VkDeviceCreateInfo`) → replaces
   `vkCreateDevice` (instance.c:440 seam).
9. Phase B (after `vkGetDeviceQueue`, instance.c:555): `xrCreateSession` with
   `XrGraphicsBindingVulkan2KHR{instance, physicalDevice, device,
   queueFamilyIndex, queueIndex=0}`; `xrCreateReferenceSpace(LOCAL, identity
   pose)`. Session is NOT begun here.
10. Compositor init: `xrEnumerateSwapchainFormats` — **require
    `VK_FORMAT_R8G8B8A8_SRGB`** (abort VR politely if absent); create private
    cmd pool/buffers/fences; `xrCreateReferenceSpace(VIEW)` (optional, for
    head pose); start pacer. Swapchain itself is **lazy** — created on first
    frame when the source size is known, recreated on size change
    (`EnsureSwapchains`, :253).

**Per-frame** (consumer side, after mailbox hit):
`XRSession::PumpEvents()` (drain `xrPollEvent`: READY → `xrBeginSession
(PRIMARY_STEREO)` + set atomic running; STOPPING → `xrEndSession` + clear;
LOSS_PENDING/EXITING/INSTANCE_LOSS → mark lost; consumer tears down VR on
lost) → consume mailbox → `SettleOwedBegin` → `xrBeginFrame` →
[`xrLocateSpace(view_space, LOCAL, predictedDisplayTime)` → publish head pose
(Tier-3 feed; optional for MVP)] → if `fs.shouldRender && current`:
`EnsureSwapchains(w,h)`; `xrAcquireSwapchainImage` →
`xrWaitSwapchainImage(1 s)` → copy → `xrReleaseSwapchainImage` →
`xrEndFrame{displayTime = fs.predictedDisplayTime, OPAQUE, layers}`.

**Layer struct** (quad, mono): `XrCompositionLayerQuad{layerFlags=0 (opaque),
space=LOCAL space, eyeVisibility=BOTH, subImage={swapchain, rect {0,0,w,h},
imageArrayIndex 0}, pose={identity orientation, position {0, voffset,
-distance}}, size={height*aspect, height}}`. Cylinder variant when arc ≥ 5°
and runtime offers it: `XrCompositionLayerCylinderKHR{pose = centre {0,
voffset, 0}, radius=distance, centralAngle=arc_rad, aspectRatio=aspect}`.
**Zero layers** whenever nothing has ever been released on a chain, or on a
swapchain-image wait timeout (`force_zero_layers`) — never submit a quad
referencing an unreleased/timed-out image.

**Swapchain edge-cases carried by flags (port these exactly):**
- `wait_pending`/`pending_index` (:96-101, 371-427): if `xrWaitSwapchainImage`
  times out or fails, the image STAYS acquired and the next visit must
  RE-WAIT the same image, not re-acquire (re-acquiring leaks one acquire per
  timeout until the chain wedges with CALL_ORDER_INVALID).
- After a successful wait the image MUST be released even if the copy
  submission failed (:429-434), else the acquire leaks.
- `ever_released` per chain gates layer submission (:897-907).

## 5. Vulkan copy contract (RecordAndSubmitCopy, :283-369)

- Pool: `VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT` on the graphics
  queue family; **2** primary command buffers + 2 unsignaled fences, ring
  (`next_cmd_buffer`); before reusing slot i, wait its fence (1 s timeout →
  skip frame's copy), reset fence.
- Record (ONE_TIME_SUBMIT):
  1. Source → `TRANSFER_SRC_OPTIMAL`. PCSX2 does this via GSTextureVK's
     layout tracker. **xemu: manual `ImageBarrier` both ways** — see §7
     delta D3.
  2. Dest XR image: `COLOR_ATTACHMENT_OPTIMAL → TRANSFER_DST_OPTIMAL`
     (srcAccess = COLOR_ATTACHMENT_WRITE|READ, dstAccess = TRANSFER_WRITE,
     stages COLOR_ATTACHMENT_OUTPUT → TRANSFER). First-acquire true layout is
     UNDEFINED but full-extent overwrite makes it irrelevant (comment :325).
  3. `vkCmdCopyImage` full extent, srcSubresource layer = eye layer (0 for
     mono), UNORM source bytes → SRGB dest ("gamma-encoded bytes, encoded
     once" — matches xemu's display image `VK_FORMAT_R8G8B8A8_UNORM`-class).
  4. Dest back: `TRANSFER_DST_OPTIMAL → COLOR_ATTACHMENT_OPTIMAL` (runtime
     compositor's expected layout).
- Submit on the graphics queue from the owning thread (externally
  synchronized by construction — pfifo thread in xemu), fence slot marked
  submitted. Queue-ordering after the frame's producing submission is the
  correctness argument (same queue, submission order) — holds in xemu since
  `render_display` submits on the same queue earlier in `pgraph_vk_sync`.
- Ordering with desktop present: PCSX2 needs `EnsureFrameSubmitted()` on the
  skipped-present path (VRManager.cpp:312-324, calls
  `GSDeviceVK::ExecuteCommandBuffer(false)`). **xemu: no equivalent needed** —
  the hook site runs immediately after `pgraph_vk_render_display` submits on
  the same queue/thread (and display.c uses blocking single-time commands).

## 6. Function-by-function

Legend: **V** ports verbatim (mechanical renames only) · **A** needs
adaptation (interface swap, same logic) · **R** rewrite for xemu.

### XRSession (.h 107 / .cpp 471 lines) — file-scope singleton state

| Function | Role | PCSX2 deps → xemu | Verdict |
|---|---|---|---|
| `CheckXR` (static, :43) | XrResult → log helper | Console → fprintf | **V** |
| `SessionStateName` (:55) | enum → string | — | **V** |
| `HandleSessionStateChange` (:71) | READY→beginSession/STOPPING→endSession/loss→lost; sets atomic running | Console | **V** |
| `CreateInstanceAndSystem` (:116) | ext probe, xrCreateInstance(1.0, vulkan_enable2 [+cylinder]), xrGetSystem, PFN fetch | Console; app name "PCSX2"→"xemu" | **V** (+ dlopen PFN table per §1) |
| `DestroyInstance` (:214) | destroy session+instance, null PFNs | — | **V** |
| `HasInstance/GetInstance/GetSystemId` | trivial reads | — | **V** |
| `QueryVulkanGraphicsRequirements` (:250) | mandatory pre-session req query; logs min/max | Console. Note: PCSX2 assumes VK 1.1 fits (comment :263); xemu negotiates min(driver,1.3) ≥ 1.1 — also fits, keep the log | **V** |
| `CreateVulkanInstanceThroughXR` (:269) | xrCreateVulkanInstanceKHR wrapper; `pfnGetInstanceProcAddr = vkGetInstanceProcAddr` | VKLoader's global → **volk's** `vkGetInstanceProcAddr` | **A** (one identifier) |
| `GetVulkanGraphicsDevice` (:288) | xrGetVulkanGraphicsDevice2KHR | — | **V** |
| `CreateVulkanDeviceThroughXR` (:301) | xrCreateVulkanDeviceKHR wrapper | same volk note | **A** |
| `CreateSessionVK` (:321) | xrCreateSession(GraphicsBindingVulkan2) + LOCAL space | — | **V** |
| `DestroySession` (:358) | end-if-running + destroy space/session | — | **V** |
| `HasSession/GetSession/GetSpace` | trivial | — | **V** |
| `PumpEvents` (:396) | drain xrPollEvent, drive state machine, set lost | Console | **V** |
| `IsSessionRunning/HasCylinderLayer/IsLost/GetStateName` | atomic/plain reads | — | **V** |

### XRCompositor (.h 95 / .cpp 1044 lines)

| Function | Role | PCSX2 deps → xemu | Verdict |
|---|---|---|---|
| screen-param atomics + `UpdateScreenParams` (:53, 1037) | thread-safe screen placement | caller = xemu settings apply | **V** |
| `CheckXR` (:138) | duplicate of session's | — | **V** |
| `ImageBarrier` (:150) | generic layout-transition helper | — | **V** (own code, audit-clean) |
| `WaitAllFences` (:168) | wait+reset in-flight copy fences | VulkanHandles → PGRAPHVkState handles via C boundary | **A** |
| `DestroySwapchains` (:184) | fence-wait then destroy both chains | — | **V** |
| `CreateChain` (:203) | xrCreateSwapchain(SRGB, COLOR_ATTACHMENT+TRANSFER_DST) + enumerate images | — | **V** |
| `EnsureSwapchains` (:253) | lazy create/recreate on size change | — | **V** |
| `RecordAndSubmitCopy` (:283) | fence-ring, barriers, vkCmdCopyImage, queue submit | `src->TransitionToLayout(...)` + `src->GetImage()` → plain VkImage + **manual source barriers both ways** (delta D3); `h.queue` → `r->queue` | **A** |
| `CopyToSwapchain` (:371) | acquire/wait(re-wait!)/copy/release with `wait_pending` machinery | GSTextureVK* → VkImage+layer | **A** (arg types only; logic verbatim) |
| `ComputeAspect` (:448) | aspect from GSConfig.AspectRatio | **R** — xemu source: swapchain (=display image) dims as default; optionally honor xemu widescreen setting later (plan U7) |
| `PacerThreadMain` (:466) | §3 loop | Threading::SetNameOfCurrentThread → pthread_setname_np | **V** |
| `SettleOwedBegin` (:511) | begin-debt retry | — | **V** |
| `DrainOnePacedFrameForShutdown` (:539) | shutdown mailbox drain | — | **V** |
| `Initialize` (:567) | format check, pool/buffers/fences, VIEW space, start pacer | VulkanHandles → PGRAPHVkState | **A** |
| `EndOfFrame` (:716) | mailbox consume → begin → head-pose publish → copy → layer build → end | GSTexture arg → VkImage/extent; `GetArrayLayers()` stereo detection → drop (mono MVP; keep chains[2] structure dormant); HeadPose publish → keep behind a stub or defer (Tier-3) | **A** |
| `Shutdown` (:925) | §3 drain, join, destroy | VulkanHandles | **A** |
| `!ENABLE_VULKAN` stubs (:1023) | inert fallbacks | n/a — xemu VR is compiled only with the VK backend available; keep cheap stubs for `renderer != VULKAN` at runtime | **A** |

### VRVulkanBridge (.h 79 / .cpp 143 lines) — thin; this is the layer that
binds to the emulator, so it is the **most rewritten** part

| Function | Role | xemu counterpart | Verdict |
|---|---|---|---|
| `AbortBootstrap` (:21) | compositor→session→instance teardown + clear handles | same order, C-callable | **V** |
| `Internal::GetVulkanHandles` (:33) | singleton handles | keep (fed from PGRAPHVkState) | **V** |
| `BeginVulkanBootstrap` (:40) | gate on WantsVR; instance+system+graphics-requirements (Phase A) | gate on `g_config.vr.enable && renderer==VULKAN`; call at top of `pgraph_vk_init_instance` | **A** |
| `VulkanBootstrapActive` (:62) | active predicate | — | **V** |
| `CreateVulkanInstanceThroughXR` (:67) | wrapper + abort-on-fail | seam: instance.c:230; **caller falls back to plain vkCreateInstance after abort** (contract in .h:52-56) — replicate that fallback in instance.c | **A** |
| `GetXrVulkanPhysicalDevice` (:79) | runtime-dictated device + abort-on-fail | seam: instance.c:369 (overrides `preferred_physical_device`, warn on mismatch) | **A** |
| `CreateVulkanDeviceThroughXR` (:91) | wrapper + abort | seam: instance.c:440 | **A** |
| `OnVulkanDeviceCreated` (:103) | fill handles, CreateSessionVK(queue_family, **queueIndex 0**), compositor init | seam: after instance.c:555 `vkGetDeviceQueue` (xemu also uses queue 0 of the family — binding matches actual queue ✓) | **A** |
| `OnGSDeviceDestroyed` (:133) | idempotent full teardown | seam: `pgraph_vk_finalize_instance` head, **before** vkDestroyDevice; also runs on renderer switch via finalize ops | **A** |

### VRManager (skim — settings snapshot pattern)

- Pattern (:190-305): a `std::mutex`-guarded **copy** of the config block
  (`s_settings`), written on the config thread (`UpdateSettings`), read
  anywhere (`WantsVR()`). Profile lookup layers per-game values over the
  snapshot before pushing `XRCompositor::UpdateScreenParams(...)` (atomics).
  **xemu MVP**: no profile DB yet; read `g_config.vr.*` (already
  thread-published by xemu's config system) at the same two points: bootstrap
  gate + screen-param push on settings apply. `GetRuntimeInfoReport` (:64) is
  the `vr-info` probe (plan W5.1) — port it early, it needs only the loader.
- `EndOfFrame` (:326): pump events → if `IsLost()`: shutdown
  compositor+session+instance (flat continues) → route mono/stereo to
  compositor. xemu: same shape inside the `pgraph_vk_sync` hook, minus stereo
  routing and the `PCSX2_VR_INTERLEAVE` debug path (drop).
- `EnsureFrameSubmitted` (:312): **do not port** (see §5 ordering note).

## 7. Deltas vs. plan-doc assumptions (explicit list)

- **D1 (new work item)**: the dlopen decision (§1) is not in the plan's W1
  file list — add `vr_xr_loader.{h,cpp}` (or a meson wrap for the loader).
  OpenXR *headers* must be vendored either way.
- **D2 (confirms plan)**: pacer/mailbox model is exactly as the plan's W3
  correction assumed; no xrWaitFrame ever on the consumer thread; port
  `begin_owed` + `wait_pending` + shutdown-drain machinery verbatim — these
  encode real runtime edge-cases.
- **D3 (plan addition)**: PCSX2 never barriers the source back after the copy
  (GSTextureVK's tracker transitions lazily on next use — .cpp:342-347 note).
  xemu has **no layout tracker**: the port must barrier `r->display.image`
  back to the layout `display.c` leaves/expects (determine at W3 —
  render-pass final layout in `vk/display.c`), or the next
  `render_display`/GL-interop use sees a wrong-layout image. This is the one
  place the port is *stricter* than the original.
- **D4 (confirms plan)**: swapchain format hard-requires
  `VK_FORMAT_R8G8B8A8_SRGB`; raw UNORM→SRGB copy is the gamma rule. xemu's
  display image is RGBA8-UNORM-class (GL_RGBA8 interop) — compatible.
- **D5 (plan `vr.*` settings)**: PCSX2 ships 4 screen params (distance,
  height, **arc_deg**, **vertical_offset**) + stereo block. Plan W4 listed
  only distance/height — add `screen_arc_deg` and `screen_vertical_offset`
  to the `vr` config section (cylinder support comes free with the port;
  sibling-consistent semantics).
- **D6 (scope)**: `EndOfFrame` also publishes the VIEW-space head pose each
  frame (Tier-3 feed, HeadPose.h — 47-line self-contained TU). Cheap to port
  now even though Tier-3 is post-MVP; recommend porting HeadPose with the
  compositor to keep `EndOfFrame` verbatim-shaped.
- **D7 (teardown ordering, confirms plan W2)**: bridge teardown must run
  BEFORE `vkDestroyDevice` and joins the pacer before `DestroySession`
  (XRSession.h:18-22 contract). xemu's `pgraph_vk_finalize` → seam at the
  head of `pgraph_vk_finalize_instance` satisfies both; renderer-switch
  passes through the same ops.
- **D8 (aspect)**: `ComputeAspect` is the only genuinely PCSX2-shaped logic
  in the compositor; xemu MVP should default to source (=display image)
  aspect and revisit widescreen/anamorphic in U7.
- **D9 (queue index)**: session binding uses queueIndex **0** hardcoded
  (bridge .h:67-69). xemu also takes queue 0 (`vkGetDeviceQueue(..., 0,
  &r->queue)`) — consistent, no change, but keep the constant visible.

## 8. Suggested xemu file layout (refines plan W1)

```
hw/xbox/nv2a/pgraph/vk/vr/          (C++ TUs, self-contained; LGPL-2.0+ or MIT headers, © Patrick Carey)
  vr_xr_loader.h/.cpp    dlopen + PFN table (new, D1)
  vr_xr_session.h/.cpp   ← XRSession (V/A per table)
  vr_xr_compositor.h/.cpp← XRCompositor + HeadPose (D6)
  vr_bridge.h/.cpp       ← VRVulkanBridge + VulkanHandles + settings snapshot
  vr_hooks.h             extern "C" surface consumed by instance.c / renderer.c:
                         vr_bootstrap_begin/active, vr_wrap_create_instance,
                         vr_get_physical_device, vr_wrap_create_device,
                         vr_on_device_created, vr_on_device_destroyed,
                         vr_end_of_frame(VkImage, w, h), vr_update_screen_params
ui/  (C)                 settings plumbing (config_spec `vr` section, D5) + recenter keybinding
```

Note the plan's recenter feature (W3/W4) has no counterpart in these three
TUs — PCSX2 keeps the LOCAL-space anchor runtime-managed (recenter arrives as
`XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING`, XRSession.cpp:432, i.e.
the user recenters via the runtime/WiVRn gesture, and the DuckStation-style
explicit `recenter_pose ⊕ offset` lives elsewhere in the sibling). For MVP,
runtime-recenter (do nothing, log the event) is sufficient and verbatim;
an in-app recenter hotkey can follow the DuckStation model post-MVP.
