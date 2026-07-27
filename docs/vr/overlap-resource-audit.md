# Over the Present Wall — Lane 3: The Frames-in-Flight Resource Audit

**Branch `sb-graphics-research`, audited 2026-07-26.** Every file:line below refers to that branch (the shipping config: all overlap toggles OFF). Companion lanes: Lane 1 = present path (`overlap-present-path.md`), Lane 2 = query pools/drains (`overlap-query-attacks.md`), Lane 4 = prior art (`overlap-prior-art.md`). Every claim is tagged **MEASURED** (rig numbers from road-to-60.md / sb-graphics-findings.md), **READ-IN-CODE** (file:line), **READ-IN-SPEC** (Vulkan spec text), or **HYPOTHESIS**.

## Executive summary

1. **The multi-submission architecture already exists.** `CB_FENCE_RING_SIZE=4` ring slots, each with its own main+aux command buffer, fence, and semaphore (`renderer.h:390-410`), per-surface `last_write_submit`/`last_use_submit`, delta-synced staging arenas, targeted `wait_for_submission`. It is default-OFF scaffolding; what is missing for frame overlap is not the ring — it is decomposing the **recycle phase** (`draw.c:1498-1526`) that today runs only after wait-ALL.
2. **Data hazards are almost entirely host-side.** Device-local resources (surfaces, textures, the device halves of the arenas, compute buffers) are GPU-ordered on the **single queue** (`instance.c:486-489,577`) by the existing queue-scoped barriers. The CPU-write-vs-GPU-read hazards are: the three bump-allocated staging arenas' offset reset, the in-place `BUFFER_VERTEX_RAM` memcpy, descriptor-set rewrite, framebuffer destruction, and query-pool slot reset.
3. **A spec fact reframes the plan (READ-IN-SPEC):** a `vkQueueSubmit` fence signal's sync scope includes **all commands earlier in submission order**. So every "narrow" single-time fence wait (`command.c:164` — texture uploads, surface uploads, downloads, the display composite) transitively drains *every in-flight ring slot*. Until uploads move in-batch, each one partially collapses overlap. This is an unnamed contributor to the narrow-fence A/B's "no win."
4. **Downloads are separable.** The radar readback can keep today's full-drain semantics (byte-correct by construction, ≈1.8 ms MEASURED) while draws/present overlap; per-surface fences already exist for a later narrowing.
5. **v1 memory cost ≈ zero.** Ring the offsets inside the existing (enormous) buffers; per-slot descriptor pools and framebuffer destroy-lists are sub-MB. The honest risk is the uniform arena's per-frame consumption (~7-8 MB vs 8 MB capacity — HYPOTHESIS, counter needed) and the unmeasured vertex-RAM dirty-sync frequency.

---

## 0. Scope, method, and the one spec fact

Audited files: `hw/xbox/nv2a/pgraph/vk/{command,draw,buffer,vertex,surface,surface-compute,texture,shaders,reports,display,blit,renderer,instance,gpuprops,debug}.c` and `renderer.h`, on `sb-graphics-research`. The shipping config runs with `XEMU_NARROW_FENCE` unset and no `/tmp/narrowfence-on` (default OFF, `surface.c:887-915`), `XEMU_ASYNC_QUERIES` hard-OFF, `XEMU_QUERY_RING` OFF — so today's behavior is: one open command buffer at a time, every finish waits everything.

**Single queue — confirmed (READ-IN-CODE).** One queue family requiring `GRAPHICS|COMPUTE` (`instance.c:281-284`), `queueCount = 1` (`instance.c:486-489`), fetched once (`instance.c:577`). Every submit site targets it: the ring submit (`draw.c:1472`), single-time (`command.c:154`), the init-time gpuprops probe (`gpuprops.c:454`), and the VR compositor, which aliases the same queue (`vr_xr_compositor.c:976`). There is no transfer or second compute queue. Consequence: **all GPU-GPU ordering is submission order + barriers; all cross-frame device-resource reuse is automatically ordered**; only CPU↔GPU boundaries need fences.

**The fence-scope fact (READ-IN-SPEC).** Vulkan spec, Fence Signaling: *"Fence signal operations that are defined by vkQueueSubmit … additionally include in the first synchronization scope all commands that occur earlier in submission order."* Waiting any fence submitted later therefore waits everything submitted before it on that queue. Two consequences:

- `pgraph_vk_begin_command_buffer`'s slot-reuse wait (`draw.c:1546-1553`) is *stronger* than it looks — waiting slot k's fence retires all older slots too. Harmless (they are older), and it makes the ring's backpressure exact.
- **Every single-time op is a full drain of all in-flight work** (`command.c:150-166`: submit + `vkWaitForFences` on `single_time_fence`). Today that is invisible (nothing else is ever in flight at those points). Under overlap, each texture upload, surface upload, surface download copy-out, and display composite becomes a serialize-everything event. The `QUEUE_SUBMIT_AUX` counter (road-to-60 §5 calls it "the drain count") already counts exactly these.

---

## 1. Command buffer lifecycle (Q1 anchor)

READ-IN-CODE:

- **Allocated:** one pool with `RESET_COMMAND_BUFFER_BIT` (`command.c:30-36`); 2×4 ring CBs (main+aux per slot, `command.c:55-67`) plus 1 dedicated single-time CB + fence (`command.c:76-89`). Slot fences created SIGNALED, semaphores per slot (`draw.c:251-269`).
- **Begun:** `pgraph_vk_begin_command_buffer` (`draw.c:1534-1566`) acquires slot `submit_count % 4`, waits that slot's fence if its previous occupant is unretired (the natural depth bound), repoints `r->command_buffer`/`aux_command_buffer` aliases, `vkBeginCommandBuffer` (ONE_TIME_SUBMIT) implicitly resets.
- **Ended + submitted:** `pgraph_vk_submit_batch` (`draw.c:1394-1478`): ends render pass + open query, ends main CB, stamps the slot's occlusion-query range (`draw.c:1423-1426`), records the aux CB with the three staging **delta-sync copies** + vertex-RAM flush (`draw.c:1437-1444`), then a two-batch submit — aux signals the slot semaphore, main waits it — against the slot fence (`draw.c:1450-1473`); `submit_count++`, `recycle_pending = true`.
- **Waited + recycled:** `pgraph_vk_finish` (`draw.c:1480-1532`) = submit_batch + `wait_all_slots` (`draw.c:1364-1377`) + the recycle phase: `descriptor_set_index = 0`, `destroy_framebuffers`, `uploaded_bitmap` clear, all three staging arenas' `buffer_offset = synced_offset = 0` (`draw.c:1501-1513`), then report resolve (`draw.c:1529`) and compute-descriptor reset (`draw.c:1531`).
- **Single-time:** `command.c:120-169`; used by surface download copy-out (`surface.c:231`), surface upload (`surface.c:1395`), texture upload (`texture.c:816`), display composite (`display.c:931`). Fully synchronous by construction.

Today's per-frame drain census (mission scene, MEASURED): `FINISH_SURFACE_DOWN=4`, STALLED query resolves 2-4, PRESENTING 1, FLIP_STALL 1, `QUEUE_SUBMIT` 6.3-8, `SURF_UPLOAD≈2`, `FINISH_NEED_BUFFER_SPACE=0` (descriptor cliff refuted), plus fenced single-times (`method_synctex 0.7 ms`, `upload 0.2 ms`, `download 1.8 ms`). `present_composite 6.6 ms` = the display-copy fence chain blocking on the whole frame's GPU work.

---

## 2. Resource-by-resource hazard audit (Q1)

For each: lifetime today, and the hazard class once frame N+1 records while frame N executes.

### 2a. The three bump-allocated staging arena pairs — the core CPU-write hazard, already half-solved

`BUFFER_INDEX_STAGING→BUFFER_INDEX`, `BUFFER_VERTEX_INLINE_STAGING→BUFFER_VERTEX_INLINE`, `BUFFER_UNIFORM_STAGING→BUFFER_UNIFORM` (`buffer.c:89-138`; staging halves host-visible mapped, `buffer.c:146-155`). Append-only bump allocation (`pgraph_vk_append_to_buffer`, `buffer.c:183-207`); device halves filled by **delta-sync**: each submit copies only `[synced_offset, buffer_offset)` in the slot's aux CB with a following barrier (`draw.c:1224-1287`), so appends after a submit never touch bytes an in-flight copy or draw reads (`renderer.h:105-111`). Draws bind absolute offsets (`draw.c:2113-2115`, `bind_descriptor_sets` UBO offsets `shaders.c:192-196`).

- **Hazard:** not the appends — the **reset**. `buffer_offset = synced_offset = 0` happens only in the recycle phase after wait-ALL (`draw.c:1507-1513`). Reset any earlier and new appends overwrite staging bytes whose aux-CB copy for an older slot is still pending, and device offsets that older draws still read. Sync-val would report WRITE_AFTER_READ on the copy; on screen: exploding geometry, wrong constants.
- **Fix shape:** ring allocator inside the existing buffers — per-slot high-water marks; advance the reclaim tail when a slot's fence retires. No new memory. **Capacity risk (uniform):** ~1,700 genuinely-dirty UBO updates/frame (MEASURED) × ~4.5 KB/update (vsh `c[192]` ≈ 3 KB + psh; HYPOTHESIS from struct layout) ≈ 7-8 MB/frame against an 8 MB arena (`buffer.c:131`). Today ~9 finishes/frame reset it long before full; with one recycle point per frame it rides the edge, and N=2 needs two frames resident. Index/vertex-inline consumption is trivially small versus their (huge) arenas.

### 2b. `BUFFER_VERTEX_RAM` — the in-place mapped mirror (the ugliest hazard)

A host-visible, persistently-mapped, GPU-read vertex buffer the size of guest VRAM (64 MiB, `buffer.c:103-107`), bound directly by draws at vram-address offsets (`draw.c:2107-2110`). On dirty vertex memory, the CPU memcpys **in place** (`vertex.c:64`) — a classic write-while-GPU-reads race. Guards today: (a) if the target pages were already uploaded this era → full finish `VERTEX_BUFFER_DIRTY` (`vertex.c:56-61` via `uploaded_bitmap`); (b) narrow-fence WAR gate: `wait_all_slots` before every dirty-range memcpy (`draw.c:1919-1925`).

- **Hazard under overlap:** every dirty vertex range mid-frame = wait-ALL = overlap collapses for that frame. Frequency for SB is **unmeasured** (`GEOM_BUFFER_UPDATE_1` counter exists; the `attr 1.6 ms` bucket contains these memcpys — MEASURED aggregate only).
- **Fix shape:** v1 keep the wait (correct, costs what it costs — measure first). v2: copy-on-write — redirect dirtied ranges into a per-frame ring region and patch the bind offsets; invasive because binds equal vram addresses.

### 2c. `BUFFER_STAGING_SRC/DST` — single-time staging (64 MiB each)

CPU maps/memcpys per upload (`surface.c:1375-1393`, `texture.c:767-813`) and per download read-back (`surface.c:478-492`). Safe today **because** every use is bracketed by the synchronous single-time fence (`command.c:153-166`). Under overlap they stay safe as long as single-time stays blocking — but each block transitively drains all slots (§0). Converting uploads to in-batch copies requires turning these into rings too (they are reused immediately after the wait).

### 2d. `BUFFER_COMPUTE_SRC/DST` — depth-stencil pack/unpack (800 MiB each, device-local)

Used by download pack (`surface.c:352-455`), upload unpack (`surface.c:1436-1539`), and — the interesting one — **zeta surface-to-texture recorded into the open main CB** (`texture.c:859-1068` via `begin_nondraw_commands`). Cross-submission reuse is GPU-ordered: every use is bracketed by pre/post `vkCmdPipelineBarrier`s whose scopes are queue-wide (all earlier / all later commands in submission order), e.g. the post-pack `SHADER_READ→TRANSFER_WRITE` barrier (`texture.c:982-993`) orders the *next* submission's re-fill against this one's compute read. **No CPU access — no host hazard. Leave alone.**

### 2e. Graphics descriptor pool/sets

One pool, `descriptor_sets[1024]` allocated once (`shaders.c:31-127`, `renderer.h:454-457`), bump-consumed per state-change draw (`shaders.c:140-229`), **index reset to 0 in the recycle phase** (`draw.c:1501`); exhaustion forces a NEED_BUFFER_SPACE finish (`shaders.c:166-172`) — MEASURED 0 in mission scene. Sets are rewritten via `vkUpdateDescriptorSets` after reset.

- **Hazard:** rewriting set i while an unretired slot's draws still reference it — VUID-class "descriptor set in use" + garbage texturing.
- **Fix shape:** per-slot pools (`vkResetDescriptorPool` on slot retire) or partition the 1024 into N ranges. Driver-side cost: sub-MB per slot. Note per-frame set *usage* with only one recycle/frame is unmeasured (counter: max `descriptor_set_index`).

### 2f. Compute descriptor pool/sets

`compute.descriptor_sets[1024]`, bump index (`surface-compute.c:294-320`), reset only via `pgraph_vk_compute_finish_complete` at finish (`draw.c:1531`, `surface-compute.c:330-333`); exhaustion self-heals with a finish (`surface-compute.c:322-328`, checked at `surface.c:186-202`, `texture.c:872-876`). Same hazard/fix as 2e.

### 2g. Framebuffers

`framebuffers[50]` created on surface-bind change (`draw.c:415-451`), **all vkDestroyFramebuffer'd in the recycle phase** (`draw.c:453-463` from `draw.c:1502`); array overflow forces a finish (`draw.c:423-425`).

- **Hazard:** destroying framebuffers an unretired slot's render passes still use — UAF, device-lost.
- **Fix shape:** per-slot pending-destroy list executed on slot retire (or an LRU cache keyed by attachments — bigger change, also fixes the ~218 renderpasses/frame churn, but that is Lane-1/pipe-opt territory).

### 2h. Query pool (Lane 2 boundary — resource mechanics only)

One `VkQueryPool`, 1024 occlusion slots OFF-mode (`reports.c:106-128`). `begin_query` resets+begins slot `num_queries_in_flight` (`draw.c:1138-1209`); the counter resets to 0 at every resolve (`reports.c:263-272`), so **epochs reuse indices 0,1,2,… in the same pool**. Safe today only because the sync path's wait-ALL retires everything before the next epoch's `vkCmdResetQueryPool` (`draw.c:1157-1182` documents exactly this). A per-index owner map + fence exists (`query_index_submit`, `renderer.h:588-601`, wait at `draw.c:1183-1199`) **but is gated on `async_queries_enabled || query_ring_enabled`** — a general overlap mode MUST arm it too, or slot resets race in-flight readers (the ISS-B16 minefield: wrong zpass counts drive SB's culling — see-through walls; stale is never OK, owner-bisected).
- **Correctness constraint (MEASURED/owner):** results must be same-epoch correct. v1 keeps the STALLED full-finish resolve (`reports.c:464`); the guest-RAM write (`pgraph.c:3237`) stays synchronous with it.

### 2i. Fences, semaphores, submit bookkeeping

Per-slot fence + binary semaphore (`draw.c:251-269`); single-time fence (`command.c:85-89`). Recycle-by-identity (`slot->submit_index != idx` ⇒ long complete, `draw.c:1384-1387`). `SURFACE_NO_WRITE` sentinel skip at `draw.c:1544-1546`. No hazard; this is the solved part. One latent trap: `submit_count` is `uint32_t` and slot identity is `idx % 4` — wraparound after 2³² submissions is theoretical.

### 2j. Pipelines, shaders, render passes

Pipeline LRU (2048, `draw.c:167`): eviction destroy is guarded by `wait_all_slots` (`draw.c:125-144`) — correct under overlap (a rare drain; `PIPELINE_GEN` steady-state small). The in-use assert (`draw.c:130-132`) covers only the *open* CB — it will NOT catch submitted-but-unretired use; the wait above is the real guard — keep both. Render passes are append-only, never freed until shutdown (`draw.c:226-239`) — safe. Shader modules are refcounted; destruction while referencing pipelines exist is legal (modules are only needed at pipeline creation; `shaders.c:299-314`).

### 2k. Texture images (LRU 1024)

`TextureBinding.submit_time` stamped at every bind (`texture.c:1721-1728,1761`). In-place image overwrite on content change waits only the last sampling submission (`texture.c:742-747`); eviction likewise (`texture.c:1775-1792`), plus a pre-evict rejection if used by the open CB (`texture.c:1810-1812`). The upload itself is single-time (§2c hazard: transitive drain). Device-side reuse across frames is queue-ordered. **Already overlap-ready except for the single-time cost.**

### 2l. Surface images

`last_write_submit` stamped at `pgraph_vk_set_surface_dirty` (`draw.c:2137-2162` — covers draws AND clears); `last_use_submit` stamped at surface-to-texture sampling (`texture.c:1074-1078`). Targeted waits exist at upload (`surface.c:1320-1327`), invalidate/evict (`surface.c:983-998` narrowfence branch), download (`surface.c:503-530`). The evict assert "Surface evicted while in use!" (`surface.c:1000-1002`) again only sees the open CB — the waits are the guard. Render-target reuse frame-to-frame is GPU-ordered (single queue, render-pass external dependencies `draw.c:343-371`). **Surface CPU-access callbacks** (`surface.c:707-806`) run on the vCPU thread but only *flag* (`download_pending`) and kick pfifo — all VK ops stay on the pfifo thread.

### 2m. Display/composite chain (Lane 1 boundary)

`render_display`: PRESENTING finish (or narrowfence submit+targeted wait) (`display.c:907-919`), then framebuffer upload if pending (`display.c:921`), then a **single-time** composite draw into `disp->image` (`display.c:931-1010`) — that fence chain is the MEASURED 6.6 ms `present_composite` wall. Display image is recreated on resolution change with no wait (`display.c:533-570,1100-1102`) — today safe (previous composite fenced); under overlap Lane 1 must keep the composite's consumers (GL interop via external memory, VR compositor on the same queue) slot-aware. FLIP_STALL full finish (`renderer.c:188`) is the per-frame re-sync this campaign exists to remove.

### 2n. `uploaded_bitmap`

Tracks which vram pages were copied into `BUFFER_VERTEX_RAM` this era (`buffer.c:109-111`, set `vertex.c:66`, tested `vertex.c:56`), cleared only in recycle (`draw.c:1506`). Under overlap "era" must mean "since the oldest in-flight slot began" — clearing per-frame while older slots fly reintroduces the very race the bitmap exists to catch. v1: clear only when ALL slots retire (i.e., keep it tied to the true full-drain events); cost: more `VERTEX_BUFFER_DIRTY` finishes if games rewrite the same pages every frame — measure via `FINISH_VERTEX_BUFFER_DIRTY`.

---

## 3. Q2 — the texture upload path: hazard class decided

**Guest texture uploads are NOT host-mapped-memory-the-GPU-reads-later. They are staged copies — but via the blocking single-time path, not the frame's CB (READ-IN-CODE):** CPU memcpy of decoded texels into mapped `BUFFER_STAGING_SRC` (`texture.c:767-813`) → `vkCmdCopyBufferToImage` recorded in the dedicated single-time CB (`texture.c:816-839`) → submit + fence wait (`command.c:145-166`). So: **no data race today or under overlap** (the fence guarantees the staging buffer is consumed before reuse), **but** each upload (a) executes before the open batch in queue order — recorded-but-unsubmitted draws sample the NEW content, an upstream-identical approximation — and (b) under overlap, its fence transitively drains all in-flight slots (§0). Hazard class: **blocking-sync today → overlap-killer tomorrow; needs in-batch conversion + staging ring, not a correctness fix.** Surface uploads are the same shape (`surface.c:1395-1643`) with an extra unconditional full finish before them (`surface.c:1331`) — a per-upload drain (~2/frame MEASURED) the ring must also convert to submit-without-wait.

**Geometry and UBO data are the good case:** written into host-visible bump arenas, copied to device buffers by `vkCmdCopyBuffer` in the slot's aux CB **at submit time** with correct barriers (`draw.c:1437-1443`, `1224-1287`) — GPU-ordered, already overlap-safe by the delta-sync design. The exceptions are §2b (vertex-RAM in-place) and the arena reset (§2a).

---

## 4. Q3 — surface objects under overlap

- **Render targets reused frame-to-frame:** GPU-ordered on the single queue (§0); no CPU involvement; nothing to do.
- **Downloads (the radar readback):** today drain-the-world twice over — the explicit finish when the open CB wrote the surface (`surface.c:189-203`, narrowfence OFF path `199`), and then the single-time copy-out whose fence waits everything earlier anyway (§0). The machinery to need only S's last writer exists: `last_write_submit` + `pgraph_vk_wait_for_surface_write` (`surface.c:509-530`) and the conversion-covering compute check (`surface.c:186-201`). But it cannot become *actually* narrow until the copy-out leaves the single-time path.
- **Is the drain path separable? YES (READ-IN-CODE).** Downloads are a self-contained subgraph: flag→pfifo (`surface.c:620-664`), download (`surface.c:532-618`), RAM write + dirty bits + `downloads_complete` event, all synchronous end-to-end. A v1 overlap build can leave every download exactly as-is: byte-correct radar by construction, cost ≈ today's `download 1.8 ms` + whatever in-flight work it happens to drain (bounded by GPU exec ≈ 9-10 ms, amortized less since frame N is usually mostly done by the time SB reads — HYPOTHESIS on the amortization, the 1.8 ms is MEASURED). Nothing else in the design depends on narrowing it.
- **Eviction/upload/RTT sampling:** already targeted-wait via `last_write_submit`/`last_use_submit` (§2l), except the upload's `surface.c:1331` full finish (fix: submit-without-wait; queue order then protects the recorded draws).

---

## 5. Q4 — every wait/finish site in the VK backend

Full-drain = `pgraph_vk_finish` (submit + wait ALL slots + recycle). Targeted = single-fence wait.

| # | Site | Kind | Why it exists | Frequency (mission, MEASURED where numbered) |
|---|---|---|---|---|
| 1 | `renderer.c:188` FLIP_STALL | full | frame boundary; the present wall | 1/frame |
| 2 | `display.c:917` PRESENTING (`914` narrowfence submit variant) | full | composite must see finished frame | 1/frame |
| 3 | `surface.c:199` (+`515`) SURFACE_DOWN | full | download of surface written by open CB | ~4/frame (FSD=4) |
| 4 | `reports.c:464` STALLED (+`371` ring startup) | full | occlusion results must be complete (SB culling correctness) | 2-4/frame |
| 5 | `surface.c:1331` SURFACE_CREATE | full | surface upload orders vs recorded draws | ~2/frame (SURF_UPLOAD) |
| 6 | `command.c:164` single-time fence | fence, **transitively full under overlap** | staging reuse + synchronous semantics | per texture/surface upload, download, composite (`QUEUE_SUBMIT_AUX`; synctex 0.7 ms) |
| 7 | `vertex.c:60` VERTEX_BUFFER_DIRTY | full | vertex page rewritten after already uploaded this era | unmeasured; counter exists |
| 8 | `draw.c:1919-1925` vertex-RAM WAR `wait_all_slots` | wait-ALL | in-place mapped memcpy vs in-flight reads | per dirty vertex range; unmeasured |
| 9 | `shaders.c:170`, `draw.c:424`, `draw.c:2168`, `draw.c:1640`, `surface.c:202`, `texture.c:875` NEED_BUFFER_SPACE | full | descriptor/arena/framebuffer/query-region/compute-pool exhaustion | **0 in-scene** (cliff refuted); safety net |
| 10 | `draw.c:1546-1553` slot acquisition | fence | ring backpressure (depth bound) | benign; the design's own throttle |
| 11 | `surface.c:983-998` invalidate (narrowfence branch targeted; OFF branch `997` full) | both | eviction vs writer+reader in flight | per eviction; steady-state low |
| 12 | `surface.c:1326-1327`, `texture.c:747`, `texture.c:1781` WAR/UAF gates | targeted | upload/evict vs last write/use/sample | no-op when retired |
| 13 | `draw.c:125-144` pipeline evict `wait_all_slots` | wait-ALL | destroy vs in-flight bind | rare (LRU 2048) |
| 14 | `reports.c:222-227` `vkGetQueryPoolResults` WAIT_BIT | GPU-progress wait | result availability | inside every resolve |
| 15 | `reports.c:461` / `reports.c:397` / `draw.c:1183-1199` | targeted | async/ring query modes (OFF today) | 0 in shipping config |
| 16 | `renderer.c:95` FLUSH | full | reset (`nv2a.c:290`), **loadvm post_load** (`nv2a.c:430`), renderer switch (`pgraph.c:3291`) | rare |
| 17 | savevm dirty-surface download (`renderer.c:261-270` → `surface.c:666-677`) | full (per surface) | savevm consistency | rare |
| 18 | `surface.c:36-67` scale-factor change | full (download-all + FLUSH) | rebuild all surfaces at new scale | rare (user action) |
| 19 | `gpuprops.c:455` `vkQueueWaitIdle` | queue-idle | init-time GPU behavior probe | once at boot |

**Ring re-sync points:** #16-18 (plus shutdown/finalize) are the legitimate "drain the ring, reset all slot state" events — loadvm/savevm/reset/scale/renderer-switch. Everything in #1-8 is per-frame and is precisely what the overlap design must convert (or knowingly keep: #3, #4, #7, #8 in v1). There are **no** `vkDeviceWaitIdle` calls anywhere in the backend; the only `vkQueueWaitIdle` is #19 (init-only).

---

## 6. Q5 — the minimal N=2 ring design

Ring depth: the mechanism supports 4; run N=2 first (`XEMU_FRAME_OVERLAP=1`, default OFF, watch-file + log-line + self-counter per M-5). "Slot retire" = its fence observed signaled at the next slot acquisition or an explicit poll at flip.

| Resource | Lifetime today (file:line) | Hazard under overlap | Fix (v1 → v2) | Cost |
|---|---|---|---|---|
| Main/aux CBs, fences, semaphores | ring of 4 exists (`renderer.h:390-410`, `command.c:46-90`) | none — solved | use as-is | driver CB memory ~few MB × N (HYPOTHESIS) |
| Staging arenas ×3 (index/inline/uniform) | bump + delta-sync; reset at recycle (`draw.c:1507-1513`) | reset stomps in-flight reads/copies | per-slot watermarks, reclaim tail on retire; grow UNIFORM 8→32 MB if counter confirms | 0 B (+24 MB×2 only if needed) |
| `BUFFER_VERTEX_RAM` + `uploaded_bitmap` | in-place memcpy, WAR wait-ALL (`vertex.c:64`, `draw.c:1923`); bitmap cleared at recycle (`draw.c:1506`) | each dirty range = full drain | v1 keep (measure `GEOM_BUFFER_UPDATE_1`/`FINISH_VERTEX_BUFFER_DIRTY`); v2 COW ring + offset patch | 0 B; v2 ~8-16 MB |
| Graphics descriptor sets | 1024, index reset at recycle (`draw.c:1501`, `shaders.c:140-229`) | rewrite of in-use sets | per-slot pools, reset on retire | ~sub-MB × N |
| Compute descriptor sets | 1024, reset at finish (`draw.c:1531`) | same | same | negligible |
| Framebuffers | destroy-all at recycle (`draw.c:1502`, `453-463`) | UAF → device-lost | per-slot destroy lists | negligible |
| Query pool | 1024 slots, per-resolve index reset (`reports.c:263-272`); owner-fence gated async/ring-only (`draw.c:1183`) | reset races in-flight readers; **stale results forbidden (SB culling)** | v1 keep STALLED full-finish; ARM the owner fence for overlap mode | 0 B; Lane 2 owns better |
| Single-time ops (tex/surface upload, download copy-out, composite) | blocking fence (`command.c:150-166`) | transitive drain of all slots (spec) | v1 keep (partial overlap); v2 in-batch copies + staging rings | v2: ~2×40 MB host-visible (HYPOTHESIS; size from TEX_UPLOAD-bytes counter) |
| Surface downloads (radar) | full-drain semantics (`surface.c:189-206` + single-time) | none if kept | **keep full-drain in v1** — separable, byte-correct | ≈ today's 1.8 ms |
| Surface upload finish | `surface.c:1331` | per-upload drain | replace finish with submit-without-wait (queue order suffices) | 0 |
| Texture/surface images, pipelines, compute buffers | targeted waits / queue-ordered (§2d,j,k,l) | none new | keep gates + keep asserts live | 0 |
| Present/flip | full finish ×2 (`renderer.c:188`, `display.c:917`) | the wall itself | Lane 1's design; resource side ready after the rows above | — |

**Memory cost, N=2 vs N=3 (honest):** v1 is offsets-inside-existing-allocations — approximately **zero new bytes**; the existing allocations are already enormous (staging 64+64 MB, compute 2×800 MB, index 2×~210 MB, vertex-inline 2×~1.25 GB by `buffer.c:113-125` arithmetic — READ-IN-CODE; VMA may host-fallback). N=3 over N=2 adds only driver-side CB memory and one more arena third — nothing material. The only plausible real allocation is growing the uniform arena (+48 MB total) and, in v2, the upload staging ring (~80 MB).

**Order of implementation (every step shippable, default-OFF per M-4/M-5):**
1. **Counters only** (no behavior change): per-frame high-water for the 3 arenas + `descriptor_set_index` max + TEX_UPLOAD bytes + vertex-RAM dirty-sync count. This sizes every later step and settles the two open frequency unknowns.
2. **Recycle decomposition** behind `XEMU_FRAME_OVERLAP` (OFF): per-slot descriptor pools, framebuffer destroy-lists, arena watermarks — with all finishes still present. ON must be golden-image + mspf identical to OFF (the proven scaffolding methodology).
3. **The flip:** flip_stall/PRESENTING stop waiting (Lane 1's exact design); arm the query owner-fence; keep downloads/STALLED/vertex-RAM/single-time synchronous. Gate: sync-val clean (M-3: assert the layer banner), `flicker_detect.py`, radar-surface PPM byte-diff, owner eyes. Expected v1 gain is partial — bounded by remaining walls #3-#8 (the absorption lesson says the last wall between producer and consumer sets the frame).
4. **In-batch uploads** + staging rings (removes the transitive drains — likely where the big win lands).
5. v2 narrowings: vertex-RAM COW; per-surface download fences (radar LAST, after everything else is stable).

**Places that will crash first (prototype forecast, top 5):**
1. **VVL: descriptor set in use** (`vkUpdateDescriptorSets` on a set referenced by an unretired slot) — the moment step-2's pools are wired wrong. Symptom without layers: garbage textures.
2. **VVL: `vkDestroyFramebuffer` in use / device-lost** — recycle decomposition missed `destroy_framebuffers` (`draw.c:1502`).
3. **Sync-val WRITE_AFTER_READ on `BUFFER_UNIFORM`/`BUFFER_INDEX`/`BUFFER_VERTEX_INLINE`** aux-CB copies — arena reset/watermark bug. Symptom: exploding geometry, wrong colors.
4. **Query assert/corruption pair:** `assert(num_queries_in_flight < query_region_size)` (`draw.c:1150`) if resolves get deferred (SB records 185-500 queries/frame — 1024 pops within ~3 frames), and see-through walls if the `draw.c:1183` owner-fence gate isn't extended to the overlap mode (reset racing an in-flight reader).
5. **Bookkeeping asserts:** `assert(!r->in_aux_command_buffer)` (`command.c:124`) from a new path re-entering single-time, and `assert(slot == &r->cb_fence_ring[submit_count % 4])` (`draw.c:1413`) on submit-count divergence. **Warning to the builder:** the two "evicted while in use" asserts (`draw.c:130-132`, `surface.c:1000-1002`) check only the OPEN CB — they stay silent on submitted-but-unretired UAF; do not read their silence as safety (M-3). Also expect one **non-crash** trap: a prototype that "works" but shows zero speedup because every single-time fence still drains the ring — watch `QUEUE_SUBMIT_AUX`.

---

## 7. Lane interactions (Q6)

- **Lane 1 (present):** rows "Present/flip" and §2m are the boundary — this audit says the resource side can be made overlap-ready independently; the composite's single-time fence chain and the GL-interop/VR consumers of `disp->image` are Lane 1's to re-time. The VR compositor shares the one queue (`vr_xr_compositor.c:976`) — its submissions enter the same submission-order/fence-scope regime.
- **Lane 2 (queries):** §2h and wait-sites #4/#14/#15. v1 keeps STALLED resolves; the owner-fence arming is the one hard requirement this lane hands to Lane 2's redesign. ISS-B16's verdict binds: same-epoch correctness, no staleness.
- **Lane 4 (prior art):** the depth choice (N=2 vs PCSX2's 3 / Dolphin's 8) and the in-batch-upload pattern are where prior art should be checked against this table.

## Confidence & unknowns

- **High confidence (READ-IN-CODE, verified against branch files):** the ring architecture and all lifecycle/wait sites and line numbers; single-queue; the recycle phase contents; delta-sync semantics; the gated query owner-fence; downloads' separability.
- **High confidence (READ-IN-SPEC):** fence-signal scope includes all earlier submission-order commands ⇒ single-time waits are transitive drains under overlap. This is the audit's most consequential claim; it is also trivially testable on the rig (timestamp a single-time wait with a slot deliberately in flight).
- **MEASURED (from the campaign docs, mission scene):** 4 downloads, 2-4 STALLED, ~2 uploads, ~9 finishes/frame, NEED_BUFFER_SPACE=0, present_composite 6.6 ms, queries 3.1 ms, download 1.8 ms, synctex 0.7 ms, ~2,037 draws, ~218 renderpasses, 185-500 queries/frame.
- **Unknowns (each has a named counter; run step 1 before building):** per-frame UBO bytes vs the 8 MB arena (the ~7-8 MB is arithmetic, not measurement); vertex-RAM dirty-sync frequency (the #1 candidate overlap-killer if high); per-frame descriptor-set consumption under one-recycle-per-frame; TEX_UPLOAD bytes/frame (sizes the v2 staging ring); how much of the download drain is amortized by frame-N being nearly done at read time.
- **Not audited (out of lane):** the GL-interop handoff internals, VR compositor internals (Lane 1), the guest-side pacing effects of changed resolve timing (Lane 2 / ISS-B16's capture-beat lesson), and any KVM interaction (`tcg_enabled()` branches flip several paths, e.g. `surface.c:1944-1949` downloads at draw time — the overlap design as described assumes TCG).
