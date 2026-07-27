# Over the Present Wall — Lane 1: the flip/present path, why pfifo blocks, and the async-present design

**Branch surveyed:** `sb-graphics-research` (all citations are to that branch's files). Read-only investigation; nothing was edited, built, or run. Companion lanes: Lane 2 = query attacks (`overlap-query-attacks.md`), Lane 3 = resource audit (`overlap-resource-audit.md`), Lane 4 = prior art (`overlap-prior-art.md`).

**Tags:** MEASURED = numbers from the rig campaign (road-to-60.md / sb-graphics-findings.md / phase timers). READ-IN-CODE = verified in source at the cited line. HYPOTHESIS = inference, with the test that would settle it.

## Executive summary

- The present path runs **entirely on the pfifo thread**, triggered by the SDL display thread: `gl_render_frame` → `nv2a_get_framebuffer_surface` → sets `sync_pending`, kicks pfifo, and **blocks** on `sync_complete` (`ui/xemu.c:830`, `hw/xbox/nv2a/pgraph/vk/renderer.c:305-309`). The pfifo thread services it in `pgraph_vk_sync` (`renderer.c:114-146`): composite (`pgraph_vk_render_display`) then VR submit (`xemu_vr_frame`).
- **The 6.6 ms lives in one line-chain:** `render_display` finds the scanout surface was drawn in the still-open command buffer and calls `pgraph_vk_finish(pg, VK_FINISH_REASON_PRESENTING)` (`display.c:917`), which submits the open CB and then **waits every ring-slot fence** in `pgraph_vk_wait_all_slots` — the blocking `vkWaitForFences` at `draw.c:1374-1375`, reached via `draw.c:1496`. The composite's own copy is <0.1 ms (MEASURED); the wait is the frame's GPU tail.
- `present_gpuwait` (0.3 ms) is the *second* drain at the FLIP method (`renderer.c:188`); it is small because the composite already paid. `present_vrsubmit` = `xemu_vr_frame`, 0.0 ms flat — the VR copy path is **already async** (submit-without-wait + fence ring, `vr_xr_compositor.c:257-361`) and is the in-repo proof of the pattern.
- There is **no VK↔GL semaphore anywhere** (grep-verified). The interop contract is purely CPU-side: fence-complete on pfifo, then `sync_complete`, then the display thread samples the imported GL texture. Async present must preserve exactly that contract — by moving the fence wait to the display thread, not by deleting it.
- The narrow-fence scaffold (default-OFF, twice measured perf-negative as a drain-retimer) already provides ~70% of the machinery: a 4-slot CB/fence/semaphore ring, submit/finish split, per-surface writer/reader stamps, targeted waits. What is missing for async present: a present-side resource ring (per-slot composite CB + fence + descriptor set + ping-pong display image/GL texture), the flip-stall no-wait, destruction-path waits, and the display-thread fence gate.
- **Honest expectation:** present-only async saves ~2-4 ms of the 6.6 (HYPOTHESIS — the tail re-absorbs into the next STALLED-query/download drain, exactly as the 2026-07-26 A/B matrix showed in reverse); the full 6.6 and the 45+ path require Lanes 2/3 to fall in the same config. Stale query results and radar bytes are untouched by design — those paths keep their drains.

---

## 1. The exact flip/present sequence (Q1)

### 1.1 The five threads

| Thread | Present-path role | Key sites (READ-IN-CODE) |
|---|---|---|
| **vCPU (TCG)** | Writes pushbuffer; writes `PCRTC_START` (scanout addr) by MMIO; guest vblank ISR advances `READ_3D` to complete the flip; blocks on surface-download round-trips | `pcrtc.c:65-68`; `pgraph.c:131-141`; `surface.c:620-641` |
| **pfifo** | Everything Vulkan: pusher/method dispatch, all queue submits and fence waits, the composite, the flip drain, the whole VR compositor frame | loop `pfifo.c:453-514`; invariant "all VK queue/fence ops stay on the pfifo thread" |
| **SDL/main (display)** | `poll_events` + `gl_render_frame` loop; requests the composite and **blocks** until pfifo finishes it; samples the shared GL texture, renders HUD, `glFinish`, `SDL_GL_SwapWindow` (vsync per `display.window.vsync`, `ui/xemu.c:1140`) | `ui/xemu.c:1413-1416`, `808-876` |
| **vblank timer** | Fixed 60 Hz (`vblank_interval_ns = 16666666`, `ui/xemu.c:77`): `process_vblank` → `graphic_hw_update` → `nv2a_vga_gfx_update` raises the guest's vblank IRQ | `ui/xemu.c:755-782`; `nv2a.c:197-207` |
| **vr-pacer** (VR only) | Sole job: blocking `xrWaitFrame`, publishing `XrFrameState` to a one-slot mailbox | `vr_xr_compositor.c:854-893` |

### 1.2 Sequence per guest frame (mission scene)

1. **Draws.** The pusher (`pfifo.c:477`) dispatches ~2,037 draws across ~200 renderpasses into the current ring slot's open command buffer (MEASURED). Every draw into the bound color surface bumps `pg->draw_time` and stamps `surface->draw_time` (`draw.c:1824-1826`), and stamps `last_write_submit = r->submit_count` (`draw.c:2145`, `2158`). Mid-frame, ~2-4 STALLED query drains (`reports.c:464`), ~4 download drains (`surface.c:199`), and ~2 upload drains (`surface.c:1331`) each do a full `pgraph_vk_finish` = submit + wait-all + recycle (MEASURED counts; READ-IN-CODE sites).
2. **Composite request (display-thread cadence, asynchronous to the guest frame).** `gl_render_frame` (`ui/xemu.c:830`) → `nv2a_get_framebuffer_surface` (`pgraph.c:380-395`, takes `renderer_lock`, sets `framebuffer_in_use`) → VK `pgraph_vk_get_framebuffer_surface` (`renderer.c:283-316`): under `pfifo.lock`, finds the scanout surface via `pgraph_vk_surface_get_within(d, d->pcrtc.start + line_offset)` (`renderer.c:293-294`), then `sync_pending=true`, `pfifo_kick`, and **`qemu_event_wait(sync_complete)`** (`renderer.c:305-309`). The display thread is now parked.
3. **Composite execution (pfifo).** At the top of its loop, `pgraph_vk_process_pending` (`renderer.c:148-174`; note the `pfifo.lock`→`pgraph.lock` swap at 157-158) runs `pgraph_vk_sync` (`renderer.c:114-146`):
   - `pgraph_vk_render_display` (`renderer.c:129`; body `display.c:1074-1105` → `render_display` `display.c:901-1014`):
     - **THE WALL:** if `r->in_command_buffer && surface->draw_time >= r->command_buffer_start_time` → `pgraph_vk_finish(pg, VK_FINISH_REASON_PRESENTING)` (`display.c:907-919`, the finish at 917). `pgraph_vk_finish` (`draw.c:1480-1532`) = `pgraph_vk_submit_batch` (`draw.c:1495`; submit at `draw.c:1472-1473`) + **`pgraph_vk_wait_all_slots` (`draw.c:1496` → blocking `vkWaitForFences` at `draw.c:1374-1375`)** + the recycle phase (`draw.c:1498-1526`) + query-report resolve (`draw.c:1529`).
     - `pgraph_vk_upload_surface_data(d, surface, !tcg_enabled())` (`display.c:921`) — no-op under TCG when the surface is GPU-rendered (`surface.c:1310-1312`); under KVM the `force=true` makes present re-upload every frame (out of scope, noted).
     - pvideo overlay if enabled (`display.c:923-926`; its own blocking single-time submit `display.c:163-201`) — off in SB missions.
     - `update_descriptor_set` (`display.c:929` → `vkUpdateDescriptorSets` at 776) pointing binding 0 at the scanout surface's view — **one descriptor set total** (`maxSets=1`, `display.c:253-254`).
     - Records the composite into the **shared single-time CB** (`display.c:931` → `command.c:120-134`): scanout surface COLOR_ATTACHMENT→SHADER_READ_ONLY, display image UNDEFINED→COLOR_ATTACHMENT (discard), fullscreen-triangle sample into `disp->image`, transitions back (`display.c:935-1007`); `pgraph_vk_end_single_time_commands` (`display.c:1010` → `command.c:136-169`) submits on the same queue with `single_time_fence` (`command.c:153-154`) and **blocks** on it (`command.c:164-165`) — small here (<0.1 ms MEASURED) because the PRESENTING finish already drained; ticks `QUEUE_SUBMIT_5` (`display.c:1011`).
   - `xemu_vr_frame` (`renderer.c:138`) — §3.
   - `sync_complete` is set (`renderer.c:145`); the display thread wakes with `r->display.gl_texture_id` (`renderer.c:310`), samples it (HUD, snapshots thumbnail `ui/xemu.c:846-847`), `glFinish()` (`ui/xemu.c:859`), releases (`ui/xemu.c:867` → `pgraph.c:397-405`), swaps.
4. **FLIP (pusher, pfifo).** `NV097_FLIP_STALL` (`pgraph.c:931-943`) → `surface_update(d,false,true,true)` (download-marking only) → `pgraph_vk_flip_stall` (`renderer.c:176-259`): `pgraph_vk_finish(VK_FINISH_REASON_FLIP_STALL)` (`renderer.c:188`) — the second, now-nearly-empty drain — then toggle refreshes, `nv2a_phase_flip()` (`renderer.c:256`). `waiting_for_flip=true` stalls the pusher (`pfifo.c:126-141`) until the guest's vblank ISR advances `READ_3D` (`pgraph.c:131-141`); MEASURED `pfifo_idle` ≈0.4-2 ms in mission ⇒ this pacing stall is effectively already satisfied when SB arrives at it.

### 1.3 Exact phase-timer semantics (READ-IN-CODE)

- **`pfifo:` `present`** = `PF_LEAF_PRESENT`, the union of two depth-guarded leaf scopes: all of `pgraph_vk_sync` (`renderer.c:122`) and the flip-drain block (`renderer.c:185`). Nested finishes are absorbed by the leaf (`phase_timers.c:214-238`), so nothing double-counts into `submit`.
- **`present_composite`** = wall-clock of the `pgraph_vk_render_display(pg)` call, plain bracket `renderer.c:127-131`. It CONTAINS the PRESENTING finish (the 6.6 ms), the descriptor/record work, and the composite's own single-time fence wait.
- **`present_gpuwait`** = wall-clock of `pgraph_vk_finish(FLIP_STALL)` only, bracket `renderer.c:186-195`.
- **`present_vrsubmit`** = wall-clock of `xemu_vr_frame(pg)`, bracket `renderer.c:137-141`.
- **`present_resid`** = leaf minus the three brackets (scope slack, ~0; `phase_timers.c:498-499`).
- Caveat: the `phase:` line's `present` field is a *different, older* accumulator (`present_ns`) fed only by the flip drain and VR brackets (`renderer.c:140`, `190`) — the composite is NOT in it. The authoritative decomposition is the `pfifo:`/`pfifo2:` pair: present 6.9 = composite 6.6 + gpuwait 0.3 + vrsubmit 0.0 (MEASURED, mission).

### 1.4 Why composite is 6.6 and gpuwait 0.3 (the ordering fact)

For `present_composite` to carry the tail, the PRESENTING branch must actually fire — i.e. at composite time the scanout surface has draws in the *open* CB. Two corroborations: (a) the only submit+wait inside the composite bracket is that finish (READ-IN-CODE; everything else is host-side or the <0.1 ms copy); (b) in the interleaved A/B, removing the mid-frame query drains grew composite 6.6→9.3 — and in that async-queries config the resolve waits its own newest submission (`reports.c:461`), leaving nothing in flight afterward, so the growth can only be open-CB work drained *at the composite* (MEASURED + READ-IN-CODE inference). Meaning: whenever the display refresh lands, SB's scanout surface has fresh draws in the open buffer, the composite drains the partial frame, and by FLIP time little remains (gpuwait 0.3). Whether that is because SB is effectively single-buffered or draws HUD/composite passes into the front buffer is an UNKNOWN (cheap probe in §7) — the design does not depend on it.

---

## 2. What the present wait actually protects (Q2)

Enumerated per the drain's side effects (all READ-IN-CODE):

- **(a) Reuse of the composite command buffer and fence.** The composite records into the ONE shared `single_time_command_buffer` with the ONE `single_time_fence` (`command.c:75-89`, asserts non-reentrancy `command.c:124`, `141`). Every texture upload, surf-to-tex copy, and download also uses it. Leaving the composite in flight without its own CB/fence would corrupt the next single-time op. Also the ONE display descriptor set (`display.c:253-254`) is rewritten each composite (`display.c:776`) — update-while-in-use hazard if the prior composite still executes. Also `disp->framebuffer`/`disp->image` are singletons (`renderer.h:309-314`).
- **(b) The GL side reading the shared image.** The display image is exported VK memory imported into GL (`display.c:623-660` export, `690-717` import; `glTexStorageMem2DEXT` `display.c:714`). **There is no semaphore or GL sync object crossing the boundary — grep-verified zero hits for `glSemaphore`/`GL_EXT_semaphore`.** The entire cross-API contract is: composite fence-completed on the CPU (`command.c:164`) *before* `sync_complete` (`renderer.c:145`) *before* the display thread samples; plus the `framebuffer_in_use`/`framebuffer_released` handshake (`pgraph.c:386-405`) and the display thread's own `glFinish()` (`ui/xemu.c:859`). ISS-B07 documents the accepted same-vendor tear for the VR-era mirror; the flat primary path today never tears because of this CPU-side serialization.
- **(c) Swapchain/frame pacing.** None on the VK side — there is no VK swapchain at all; scanout is the GL window (`SDL_GL_SwapWindow`, vsync optional `ui/xemu.c:1140`). Guest pacing is separate: FLIP_STALL waits the vblank-ISR `READ_3D` increment (§1.2 step 4), which async present must NOT touch (it is the guest-visible flip contract).
- **(d) Surface content stability for next-frame guest reads.** Nothing at present. Guest reads are served by the download protocol (`surface.c:620-641` vCPU round-trip; `surface.c:643-664` pfifo side), which does its own targeted drains (`surface.c:189-206`) and writes RAM before `downloads_complete`. The composite only READS surface images; radar-byte correctness is orthogonal to present.
- **(e) Everything else the finish's recycle phase does.** Because the present wait is a *full finish*, it also: resets the shared descriptor-pool cursor and destroys per-batch framebuffers (`draw.c:1501-1502`), resets the three delta-synced staging arenas + `uploaded_bitmap` (`draw.c:1506-1513`), drives the VMA budget tick keyed on FLIP_STALL (`draw.c:1517-1524`), resolves pending query reports (`draw.c:1529`), and completes compute-pool recycling (`draw.c:1531`). Any async design must either keep these attached to a later real finish (the async-queries precedent already defers recycle: `reports.c:441-450` comment; `recycle_pending` machinery `draw.c:1477`, `1498`) or re-home them.

Additional structural fact: eviction (`invalidate_surface`, `surface.c:975-1017`) and upload (`surface.c:1304-1331`) protect image lifetime/WAR with finishes or (narrow-fence mode) targeted waits `surface.c:983-995`, `1326-1327` — with the live assert "Surface evicted while in use!" at `surface.c:1000-1002`. These paths know nothing about a possible in-flight *present* submission; today that is safe only because present is synchronous.

---

## 3. The VR path (Q3)

READ-IN-CODE, `vr_xr_compositor.c` (+ `vr_manager.c:166-190`, hook order `vr.h:31`):

- **Frame pacing never blocks pfifo.** `xrWaitFrame` — the OpenXR throttle — runs on the dedicated `vr-pacer` thread (`vr_xr_compositor.c:854-893`), publishing `XrFrameState` into a mutex-guarded one-slot mailbox. `vr_compositor_frame` (pfifo, called from `pgraph_vk_sync` right after the composite, `renderer.c:138`) consumes it non-blocking; empty mailbox ⇒ submit nothing this visit (`vr_xr_compositor.c:1227-1237`). `xrBeginFrame`/`xrEndFrame` run on pfifo with owed-begin retry bookkeeping (`vr_xr_compositor.c:895-924`, `1246-1268`).
- **The copy is already async present in miniature.** `record_and_submit_copy` (`vr_xr_compositor.c:257-361`): a ring of `VR_COMP_NUM_CMD_BUFFERS = 2` CBs+fences (`vr_internal.h:49`); waits a fence ONLY when reusing a slot still in flight (`:262-275`); submits with **no host wait** (`:354`); relies on same-queue submission order for visibility of the finished display image (`:290-293` comment) and barriers the image back to SHADER_READ_ONLY for the GL mirror (`:330-338`). `xrWaitSwapchainImage` can block but is timeout-guarded with zero-layer degradation (`:391-418`). MEASURED: `present_vrsubmit = 0.0 ms` (flat config; VR-on bucket numbers were never measured — unknown).
- **Interaction with async present:** the XR copy reads `disp->image` after the composite *by submission order*, so it inherits correctness from any design that keeps the composite submitted before `xemu_vr_frame` on the same queue. Under a ping-pong display image (§5) the copy must be handed the current slot's image (`vr_xr_compositor.c:1306-1315` reads `disp->image` directly today). ISS-B07's accepted mirror tear is exactly the layout round-trip vs GL race this file documents (`:20-31`).

---

## 4. What the narrow-fence scaffold already provides (Q4)

Default-OFF (`XEMU_NARROW_FENCE=1` / `XEMU_NO_NARROW_FENCE=1` / watch-file `/tmp/narrowfence-on`, refreshed once per frame — `surface.c:882-920`, hook `renderer.c:202`). Measured perf-negative twice as a drain-retimer, but the machinery is real:

**Exists (READ-IN-CODE):**
- **4-slot CB/fence ring:** `CbFenceSlot {fence, cb, aux_cb, sem, submit_index, query_first, query_count}` × `CB_FENCE_RING_SIZE=4` (`renderer.h:390-409`); per-slot CBs allocated `command.c:46-73`.
- **Submit/finish split:** `pgraph_vk_submit_batch` (`draw.c:1394-1478`; per-slot aux staging-delta sync + semaphore-ordered two-part submit, fence stamped, `recycle_pending=true`) vs `pgraph_vk_finish` (`draw.c:1480-1532`) which is now submit + `wait_all_slots` + deferred-recycle block.
- **Slot acquisition backpressure:** `pgraph_vk_begin_command_buffer` waits only the slot's previous occupant (`draw.c:1547-1553`) — the natural in-flight depth bound.
- **Targeted waits:** `wait_all_slots` (`draw.c:1364-1377`), `wait_for_submission(idx)` with the three short-circuits (`draw.c:1379-1392`), `wait_for_surface_write` (`surface.c:509-530`).
- **Producer/consumer stamps:** `SurfaceBinding.last_write_submit` (`draw.c:2145`, `2158`) and `.last_use_submit` (surf-as-texture sampling, `texture.c:1078`); `TextureBinding.submit_time`.
- **Narrowed sites when ON:** download (`surface.c:191-206`), eviction (`surface.c:983-995`), upload WAR gates (`surface.c:1326-1327`), present (`display.c:909-915`), unbind early-submit (`surface.c:926-945`).
- **Single-time fence** replacing `vkQueueWaitIdle` (`command.c:150-166`) — prerequisite for having ring batches in flight at all.
- **Deferred-recycle precedent, sync-validation-cleared:** async-queries submits without recycling and defers to the next real finish (`reports.c:441-462`); ISS-B16 graveyard #1 cleared this with a real armed validation run.

**Missing for async present (the gap this lane fills):**
1. The present path when ON still *waits* (`display.c:915` waits the scanout surface's write fence — a narrowed but synchronous present). No submit-and-go present exists.
2. The composite has no resources to leave in flight: no per-slot present CB/fence, one display descriptor set, one display image/GL texture, one framebuffer (§2a/b).
3. `flip_stall` always full-finishes (`renderer.c:188`).
4. Destruction paths (`invalidate_surface`, `destroy_current_display_image` `display.c:533-567`, `finalize_display` `display.c:1057-1072`, flush/loadvm `renderer.c:91-112` ← `nv2a.c:302`, `442`, `pgraph.c:3291`) know nothing about an in-flight present submission.
5. No display-thread-side completion gate (today completion is implicit in `sync_complete`).

---

## 5. Design sketch: ASYNC PRESENT (Q5)

**Principle:** pfifo records + submits the composite and continues immediately; completion is consumed where it is needed — on the display thread, which has a 16.7 ms vsync budget and is currently parked anyway. Queries/downloads/uploads keep today's drains (stale-is-never-OK stays untouchable: SB culls geometry off query results and reads radar bytes — owner-bisected MEASURED constraint).

### 5.1 Minimal version (present-only async)

New per-slot present ring, N=2 (N=3 only if the probe in §7 shows two composites per guest frame regularly overlap):

```
struct PresentSlot {
    VkCommandBuffer cb;          // dedicated; NOT the shared single_time CB
    VkFence         fence;
    VkDescriptorSet desc_set;    // display pool maxSets -> N
    VkImage         image;       // ping-pong display image (exported)
    VkDeviceMemory  memory; int fd;
    VkImageView     view; VkFramebuffer fb;
    GLuint          gl_memory_obj, gl_texture_id;
    uint32_t        src_surface_generation; // for eviction targeting (optional)
};
```

Changes, site by site:
1. **`render_display` (`display.c:907-919`):** replace the PRESENTING finish/wait with `pgraph_vk_submit_batch(pg, VK_FINISH_REASON_PRESENTING)` — submit the open CB, **no wait**. Queue order alone makes the frame visible to the composite (single in-order `r->queue`; same argument the VR copy documents at `vr_xr_compositor.c:290-293`).
2. **Slot acquire:** `slot = ring[present_count % N]`; wait `slot->fence` only if still unsignaled from N presents ago (mirrors `vr_xr_compositor.c:262-275`). The existing display handshake makes this near-always signaled: slot s is reused only two `gl_render_frame` cycles later, and the display thread completed its use of s before the next `get_framebuffer_surface` could even start (sequential loop `ui/xemu.c:1413-1416`, re-entry guarded by the `rendering` xchg `ui/xemu.c:811`). This same ordering prevents the vkResetFences-while-waited VUID.
3. **Record composite into `slot->cb`** (same commands as today, targeting `slot->image`/`slot->fb`, descriptor writes into `slot->desc_set`), submit with `slot->fence`, **do not wait**. The layout round-trip on the scanout surface (ATTACHMENT→READ→ATTACHMENT, `display.c:935-938`/`999-1002`) is inside the CB, so frame N+1's draws into that surface are correctly ordered on the queue with zero CPU coupling.
4. **Publish** `{gl_texture_id, fence}` of this slot; `xemu_vr_frame` runs next, its copy reading `slot->image` (plumb the image through instead of `disp->image`); signal `sync_complete`; return. pfifo proceeds into frame N+1 immediately.
5. **Display thread gate:** in `pgraph_vk_get_framebuffer_surface` after `qemu_event_wait(sync_complete)` (`renderer.c:309`), `vkWaitForFences(published fence)` **on the display thread** before returning the GL id. Fence *waits* are thread-safe; queue ops stay pfifo-only. This preserves the exact CPU-complete-before-GL-sample interop contract of §2b — no new NVIDIA-interop risk class. Expected wait ≈ remaining GPU tail, paid inside the display thread's vsync budget instead of the pfifo frame.
6. **`flip_stall` (`renderer.c:188`):** replace `pgraph_vk_finish(FLIP_STALL)` with `pgraph_vk_submit_batch(FLIP_STALL)` (no wait). Without this, the wall just moves from composite to gpuwait. Recycle stays deferred (`recycle_pending` → next real finish; the async-queries precedent and its self-healing `NEED_BUFFER_SPACE` backstop `draw.c:2165-2173` cover the no-other-finish frame — MEASURED `FINISH_NEED_BUFFER_SPACE=0` in-scene means this backstop is idle headroom, not a new cost). Keep the VMA budget tick by moving its trigger to the FLIP_STALL submit site.
7. **Destruction-path waits (the UAF fence-off):** add `wait_all_present_slots()` (wait+reset both present fences) to: `invalidate_surface` (`surface.c:975` — conservative: always; two `vkGetFenceStatus` calls are ~free), `destroy_current_display_image` (`display.c:533` — covers resize and finalize), `pgraph_vk_flush` (`renderer.c:91` — covers loadvm `nv2a.c:442`, reset `nv2a.c:302`, renderer switch `pgraph.c:3291`), and `pgraph_vk_finalize_display`. Uploads need no present wait: their single-time work is queue-ordered after the composite (WAR satisfied on the GPU timeline); only CPU-side *destruction* ignores queue order.
8. **Unchanged:** the guest flip pacing (READ_3D/vblank), the download protocol end-to-end, STALLED query resolution, pvideo (stays synchronous inside its rare path), savevm (`pre_savevm` downloads don't touch present state; composite never writes RAM).

### 5.2 What it saves

- `present_composite` 6.6 → ~0.3-0.5 ms (record + submit + descriptor update; the copy itself <0.1 MEASURED). `present_gpuwait` 0.3 → ~0.
- **But the tail re-absorbs** into frame N+1's first full drain: STALLED queries (2-4×/frame, `reports.c:464`), downloads (~4×, `surface.c:199`), uploads (~2×, `surface.c:1331`) all `wait_all_slots`. The GPU gets only the recording time between flip and that first drain to retire the tail. Model: real GPU ≈9-10 ms/frame, ~3 ms already hidden, tail 6.6; overlap window ≈3-5 ms of recording ⇒ **net ≈2-4 ms/frame alone (HYPOTHESIS; 30 → ~27 ms)**. This is the same fungibility the 2026-07-26 matrix measured (queries −2.7 ⇒ composite +2.7, net flat) — pre-register it as the expected shape, not a failure.
- **Combined config** (Lane 2 removes/retimes query drains correctly; Lane 3 narrows download/upload/eviction): the last wall between producer and consumer falls, the full ~6.5 ms is real, and the currently-absorbed pipe/const levers (~5-6 ms built, default-OFF) become claimable. That is the 45+ road (road-to-60 §3 lever #1).
- Secondary wins: the display thread stops burning 6.6 ms parked in `sync_complete` (smoother mirror/HUD); VR copy latency drops by the same amount.

### 5.3 What can break (failure modes, written down)

| Risk | Mechanism | Containment |
|---|---|---|
| Mirror tear / partial frames | GL samples slot image while GPU still writes it (if the display-thread fence gate is skipped or gates the wrong slot) | The §5.1(5) gate is mandatory; ping-pong means the *other* slot is being written. ISS-B07-class symptom; flat-mode tearing = kill condition |
| Eviction UAF → device lost | `vkDestroyImage` of a surface the in-flight composite samples | §5.1(7) waits; keep the `surface.c:1000-1002` assert LIVE |
| Descriptor update-while-in-use | Single desc set rewritten while prior composite executes | Per-slot sets (pool `maxSets` N) — do not ship without |
| vkResetFences-while-waited VUID | Display thread waiting a fence pfifo resets | Reset only at slot-reuse, which the handshake orders after display-side use (§5.1(2)); validation layers armed |
| Recycle starvation | No finish all frame ⇒ staging arenas never reset | `NEED_BUFFER_SPACE` self-heal (`draw.c:2168`); counter for finishes/frame floor |
| Resize/renderer-switch/loadvm with work in flight | Old image/GL texture destroyed under an in-flight composite | §5.1(7) sites; resize already runs inside sync while the display thread is parked (`display.c:1100-1102`) |
| Screenshot/monitor paths | Snapshot thumbnail uses the gated GL texture (`ui/xemu.c:846`) — safe; QMP `screendump` uses the VGA console surface, already stale for 3D today (not a regression) | Note in docs; verify thumbnail in validation |
| savevm/loadvm | savevm: downloads unaffected (present writes no RAM). loadvm: `flush_pending` full-drain now also waits present ring | §5.1(7); savevm/loadvm cycle in the test matrix |
| Perf-flat alone | Re-absorption (§5.2) | Expected; measure combined config before judging |
| KVM | `display.c:921` force-upload branch changes present behavior | Out of scope; TCG-only feature gate initially |
| VR | Copy must use the slot image; XR release-after-submit already legal (existing pattern) | Plumb slot image; VR validation ride-along |

### 5.4 Gating and measurement (project convention)

- **M-5 trio:** env `XEMU_ASYNC_PRESENT=1` binds at boot, watch-file `/tmp/asyncpresent-on` flips live (own toggle — do NOT overload `/tmp/narrowfence-on`; that config carries the measured-negative upload/eviction gates), a state-change stderr line, and self-measuring counters (`ASYNC_PRESENT_SUBMIT`, display-fence-wait-fired count + ns). **Default OFF; OFF path byte-identical** (both sites keep calling `pgraph_vk_finish`).
- **Instruments:** pfifo2 buckets (composite must collapse; watch `queries`/`download`/`submit`/`upload` for the re-absorption signature), interleaved same-boot ABABA via `ab-interleaved.sh` (M-8), `flicker_detect.py` mission A/B + per-present dump variant (ISS-B16 addendum's capture-beat lesson), VK validation layers with the banner-present assert (M-3), radar-surface golden PPM byte-diff, savevm/loadvm cycle, resize storm, snapshot thumbnail. Two independent runs before any "cleared" (M-1); owner eyes before default-ON (M-4).
- **Pre-registered kill conditions:** any new-class VUID; flicker detector separation vs baseline; flat-mode visible tearing; ABABA mspf regression >1 ms; radar PPM diff nonzero.

---

## 6. Lane interactions (Q6)

- **Lane 2 (queries):** the STALLED drain is the first re-absorber of this lane's win (§5.2) and the largest remaining wall once present falls. The slot struct already carries `query_first/query_count` for a decoupled future (`renderer.h:398-408`, `draw.c:1415-1426`); the correctness bar is theirs: same-epoch, never-stale (owner-bisected).
- **Lane 3 (frames-in-flight hazards):** the recycle phase (`draw.c:1498-1526`), upload's unconditional `SURFACE_CREATE` finish (`surface.c:1331`), and eviction narrowing are theirs; minimal async present deliberately keeps recycle-at-next-finish (sync-validation-cleared precedent) and adds only the destruction-path present waits.
- **Lane 4 (prior art):** the in-repo precedent is the VR copy ring (§3) — submit-no-wait, fence-at-reuse, queue-order visibility, measured 0.0 ms. External anchors: Dolphin 8 / PCSX2 3 command buffers in flight; DO-NOT list (no cross-frame secondary CBs, no pushbuffer JIT).

---

## 7. Confidence & unknowns

**High confidence (READ-IN-CODE + MEASURED):** the thread map (§1.1); the wait chain `display.c:917` → `draw.c:1496` → `draw.c:1374-1375` as the home of the 6.6 ms; the exact bracket semantics of composite/gpuwait/vrsubmit (`renderer.c:122-141`, `185-195`); zero VK↔GL semaphores — CPU-side interop contract only; the VR pacer/mailbox/copy-ring structure; the narrow-fence inventory of §4; the re-absorption behavior of freed present time (measured in reverse, 2026-07-26 matrix).

**Medium confidence (inference from measurement, one step removed):** that the PRESENTING branch fires at essentially every expensive composite in the mission scene (§1.4 — required by the bucket arithmetic, not directly counted); that the display handshake makes N=2 sufficient and reset-while-waited unreachable (§5.1(2) — an ordering argument over `ui/xemu.c:808-876`, needs validation-layer confirmation).

**Unknowns / decisive next measurements:**
1. **Scanout buffering topology** (single vs double buffered; who draws into the scanout mid-frame). One evening: log `FINISH_PRESENTING`/frame (counter exists, `debug.h:103`) + one stderr line at composite with scanout addr and `draw_time` vs `command_buffer_start_time`, plus `XEMU_SURF_ATTR`. Changes the mirror-latency story, not the design.
2. **Actual overlap gain of present-only async** — the 2-4 ms is a model; only the ABABA run settles it.
3. **Composites per guest frame** in-mission (display 60 Hz vs guest ~30 FPS suggests ~2; affects N and the per-frame composite CPU cost).
4. **VR-mode bucket numbers** — `present_vrsubmit=0.0` was measured flat-only; a VR session may shift the sync cadence and the copy's fence-reuse behavior.
5. **Second imported GL image** (ping-pong) — allocation/import path (`display.c:623-717`) exercised once per size today; two live imports is untested on the rig's driver.
6. **`pfifo.lock` vs `pgraph.lock` coverage of the surface list** read by the display thread (`renderer.c:288-299` under `pfifo.lock` while pgraph methods mutate under `pgraph.lock`) — pre-existing upstream looseness (`pfifo.c:165` "TODO think more about locking"), unchanged by this design, but worth knowing it exists before anyone moves the lookup.
