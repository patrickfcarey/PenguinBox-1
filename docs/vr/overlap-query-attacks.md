# Over the present wall — Lane 2: the query subsystem

**Scope:** occlusion-report delivery without blocking the pfifo thread (Attack A), and killing the ~200/frame renderpass churn that query boundaries force (Attack B). Read-only investigation of branch `sb-graphics-research` (tip `57774d14d8`); all `file:line` citations are against that commit. Labels: **MEASURED** (rig numbers from the campaign docs), **READ-IN-CODE** (verified in source this session), **HYPOTHESIS** (reasoned, unmeasured). Companion lanes: Lane 1 = present path (`overlap-present-path.md`), Lane 3 = resource audit (`overlap-resource-audit.md`), Lane 4 = prior art (`overlap-prior-art.md`).

## Executive summary

1. **The pass churn is query-caused and fully accounted.** Every `GET_REPORT` sets `new_query_needed`; the next zpass draw ends the render pass solely because `vkCmdResetQueryPool` must execute outside a pass, then reopens it (`draw.c:1700-1722`). ~181-188 query slots/frame + ~20 genuine breaks (clears, surface switches, non-draw copies, submits) ≈ the measured 200-218 renderpasses/frame. READ-IN-CODE + MEASURED.
2. **The churn is legal to kill.** `vkCmdBeginQuery`/`vkCmdEndQuery` may live inside a render pass; only the *reset* may not. Batch the reset once per frame (in-command-buffer, ordered for free) with monotonic slot allocation, or enable `VK_EXT_host_query_reset` (not currently plumbed — `instance.c:324-337` enables only custom-border-color and memory-budget). Passes then stay open across report boundaries: ~205 → ~20-30 passes/frame.
3. **The GPU-side "STORE+LOAD per churn" tiler bound is 1.0 GB/frame at scale 1 (5.2 ms of RTX 4050 bus) and 16 GB/frame at scale 4 (84 ms)** — and the measured ~9-10 ms GPU frame *proves the desktop IMR does not pay it*. The real GPU prize is ~205 full-attachment EXTERNAL-dependency bubbles: est. 0.5-2 ms. HYPOTHESIS on the bubble figure; the bound-vs-reality argument is arithmetic.
4. **Attack A is the existing `XEMU_ASYNC_QUERIES` minus its one remaining block.** As-built async still waits its own submission's fence (`reports.c:461`), which on one in-order queue ≡ waiting everything; its genuine win was recycle-deferral. Splitting resolve into submit-now / fence-poll-later at the top of the pfifo loop (`pfifo.c:488` already runs every iteration) delivers the *same bytes at the same wall-clock time* with zero pfifo blocking. The guest spin-waits on report RAM (`reports.c:332-334`; 77% spin MEASURED), so latency is absorbed; deadlock safety requires a timed wait replacing `pfifo.c:499`'s indefinite park.
5. **Correctness line honored:** Attack A and B change *when the pfifo thread is free*, never *which value is written* — unlike the refuted query-ring, which substituted a retired epoch's counts (`reports.c:397-409`) and culled live walls. Verified in code: async reads its own epoch's range; ring reads `(seq-lag)`'s.
6. **Standalone, both attacks are expected mspf-flat** (absorption law, measured twice). Their value is compositional: with Lane 1's async present, the STALLED submits become the early submissions that let GPU work overlap recording, and the only remaining mid-frame *blocking* points are genuine surface downloads (radar), the un-narrowed upload finish at `surface.c:1331`, and single-time-copy fences.

---

## 1. Q1 — reports.c anatomy: queue, drain, pool lifecycle

### 1.1 The report queue and its producers

READ-IN-CODE. Guest-facing entry points (all pfifo-thread method handlers):

- `NV097_GET_REPORT` (`pgraph/pgraph.c:2519-2525`, ZPASS_PIXEL_CNT only) → `pgraph_vk_get_report` (`pgraph/vk/reports.c:171-186`): allocates a `QueryReport {clear=false, parameter, query_count = num_queries_in_flight}` (`renderer.h:248-253`), appends to `r->report_queue` (QSIMPLEQ, heap-allocated per report — `reports.c:179` `// FIXME: Pre-allocate`), and sets `r->new_query_needed = true` (`reports.c:185`).
- `NV097_CLEAR_REPORT_VALUE` (`pgraph.c:2509-2512`) → `pgraph_vk_clear_report_value` (`reports.c:157-169`): same, with `clear=true` — on resolution it zeroes the running accumulator.
- `NV097_SET_ZPASS_PIXEL_COUNT_ENABLE` (`pgraph.c:2514-2517`) gates whether draws carry an active query at all.

`report->query_count` snapshots how many query slots existed when the report was requested — this is the whole ordering contract: a report's value = sum of results of slots `[0, query_count)` plus the accumulator, applied in queue order.

### 1.2 Resolution: who writes the report, and when

READ-IN-CODE. One function applies results: `apply_reports_from_range(d, src_base, src_count)` (`reports.c:202-274`). It reads pool slots `[src_base, src_base+read)` via `vkGetQueryPoolResults(... VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT)` (`reports.c:222-225`), then walks the report queue: accumulate `query_results[]` into `r->zpass_pixel_count_result` up to each report's `query_count`, and either zero it (clear) or call `pgraph_write_zpass_pixel_cnt_report` (`reports.c:236-256`), which stores 16 bytes into guest RAM through the report DMA object: timestamp (hardcoded), the ÷scale² result, `done=0` (`pgraph.c:3237-3258`; the ÷scale² truncation is the known P5 VR bug, `reports.c:231-233,250-251`). Exit resets `num_queries_in_flight` and `queries_submitted` to 0 (`reports.c:263-272`) — slot indices restart at 0 for the next resolve epoch.

Resolution is reached from exactly two directions:

1. **Every full drain**: `pgraph_vk_finish` = `submit_batch` + `wait_all_slots` + recycle + `process_pending_reports_internal` (`draw.c:1480-1532`, reports applied at `draw.c:1529`). So any finish — FLIP_STALL (`renderer.c:188`), SURFACE_DOWN (`surface.c:199,515,997`), SURFACE_CREATE upload (`surface.c:1331`), PRESENTING (`display.c:917`), NEED_BUFFER_SPACE (`draw.c:424,1640,2168`, `texture.c:875`, `shaders.c:170`), FLUSH (`renderer.c:95`), VERTEX_BUFFER_DIRTY (`vertex.c:60`) — flushes the whole queue as a side effect.
2. **The STALLED site**: `pgraph_vk_process_pending_reports` (`reports.c:418-467`), called once per pfifo-loop iteration (`pfifo.c:486-489`, holding `pfifo.lock`) and from `SET_CONTEXT_DMA_REPORT` (`pgraph.c:997-1002`). It fires only when `dma_get == dma_put && r->in_command_buffer && !QSIMPLEQ_EMPTY(&report_queue)` (`reports.c:426-427`) — the pushbuffer ran dry with reports outstanding. Default path: `pgraph_vk_finish(pg, VK_FINISH_REASON_STALLED)` (`reports.c:464`) — **the synchronous drain that is the `queries` bucket**. MEASURED: `FINISH_STALLED=3`/frame, `queries` 2.8-3.1 ms.

Why a drain *must* happen there today: the pushbuffer is dry, so the pfifo loop is about to park on `fifo_cond` (`pfifo.c:491-503`), and the guest may be spin-reading report RAM with no further MMIO coming — nothing would ever wake the thread to write the report. The in-tree comment states the invariant: "a value is written to EVERY pending report on EVERY call (the guest spin-waits on report RAM — deferring the WRITE would hang it)" (`reports.c:332-335`). Deferring the write *unboundedly* deadlocks; deferring it *until GPU completion, with a bounded poll* does not — that is Attack A's opening.

### 1.3 VkQueryPool lifecycle

READ-IN-CODE:

- **Allocation**: one occlusion pool, created in `pgraph_vk_init_reports` (`reports.c:122-128`), `queryCount = max_queries_in_flight` = 1024 with ring OFF (`reports.c:100-107`). Destroyed in `pgraph_vk_finalize_reports` (`reports.c:148`).
- **Reset**: per-slot, `vkCmdResetQueryPool(cb, pool, idx, 1)` inside `begin_query` (`draw.c:1202`), immediately before `vkCmdBeginQuery(..., VK_QUERY_CONTROL_PRECISE_BIT)` (`draw.c:1203-1204`). There is **no batch reset anywhere**; each slot is reset lazily at reuse, which is precisely what drags the reset into the draw stream.
- **Slot mapping**: `idx = query_epoch_base + num_queries_in_flight` (`draw.c:1155`; base pinned 0 with ring OFF). One slot per *report interval*, not per draw: a query stays open across draws until (a) a report arrives (`new_query_needed`), (b) zpass is disabled, (c) the pass/CB is interrupted (submit `draw.c:1407-1408`, non-draw commands `draw.c:1584-1586`). On interruption the next zpass draw just opens a fresh slot (`draw.c:1705-1708`) and the accumulate-across-slots design (§1.2) makes the split semantically free — **the machinery already supports N slots per report interval**, which both attacks exploit.
- **In-flight tracking** (narrow-fence scaffolding, all landed): each submission stamps its slot range `slot->query_first/query_count` from the `queries_submitted` watermark (`draw.c:1415-1426`, fields `renderer.h:398-408` — the comment even says they "generalize if submit/resolve are ever decoupled"). A per-slot-index owner map `query_index_submit[]` (`reports.c:113-120`, `renderer.h:588-601`) lets `begin_query` fence a pool-slot's previous owning submission before re-resetting it when async/ring is on (`draw.c:1183-1199`).
- **Startup state**: counters zeroed in `pgraph_vk_init_reports` (`reports.c:64-69`).

---

## 2. Q2 — why render passes end at query boundaries

### 2.1 The forcing chain

READ-IN-CODE, mechanical:

1. Vulkan requires `vkCmdResetQueryPool` **outside** a render pass instance (VUID `vkCmdResetQueryPool-renderpass`). This is the only *hard* API constraint in the chain.
2. xemu resets lazily at `begin_query` (§1.3), so `begin_query` must run outside a pass — enforced by `assert(!r->in_render_pass)` (`draw.c:1141`). Since a query begun outside a pass must also end outside one, `end_query` asserts the same (`draw.c:1214`).
3. `begin_draw` therefore breaks the pass at every report boundary (`draw.c:1699-1712`):
   ```c
   if (!pg->clearing && pg->zpass_pixel_count_enable) {
       if (r->new_query_needed && r->query_in_flight) {
           end_render_pass(r); end_query(r);
       }
       if (!r->query_in_flight) {
           end_render_pass(r); begin_query(r);
       }
   } else if (r->query_in_flight) {
       end_render_pass(r); end_query(r);
   }
   ```
   then reopens: `begin_render_pass` (`draw.c:1720-1722`), which also forces `must_bind_pipeline` → a redundant `vkCmdBindPipeline` + `vkCmdSetViewport` + `vkCmdSetScissor` per restart (`draw.c:1722-1770`). `NV2A_PROF_PIPELINE_RENDERPASSES` increments in `begin_render_pass` (`draw.c:1320`).

So the pass ends at a query boundary **only because the reset lives there**. Ending the *query* at the report point is semantically required (the counting interval closes); ending the *pass* is not.

### 2.2 Accounting for ~205 renderpasses/frame

MEASURED inputs: `QUERY` = 181-188 slots/frame, `BEGIN_ENDS` = 2037-2100 draws, renderpasses ~200-218/frame, `FINISH_STALLED` = 3, `QUEUE_SUBMIT` = 6.3-8.

READ-IN-CODE accounting: each of SB's ~181-188 report/probe cycles forces exactly one end+begin pass pair via the §2.1 chain (both the `new_query_needed` path and the zpass enable/disable toggle path land in the same restart). Non-query pass breaks: clears (`pg->clearing`, `draw.c:1714-1716`), surface-target switches (`framebuffer_dirty`/`render_pass_dirty` → `draw.c:1649-1660`; `surface.c:1800`), non-draw command insertions (`pgraph_vk_begin_nondraw_commands`, `draw.c:1589-1595`, from `texture.c:883,1094`), and batch submits (`draw.c:1404-1406`) — together ~20/frame given 4 downloads + 28 surf-to-tex + ~7 submits, many coinciding. **~185 query-forced + ~20 genuine ≈ 205. Roughly 90% of all renderpass churn is query-forced**, and it scales with polls: SB's 500-poll combat scenes imply ~500+ passes/frame exactly where frame time collapses 44→140 ms.

Average draws per pass today: ~2050/205 ≈ 10. With query breaks removed: ~2050/25 ≈ 80 — a ~8-10× cut in pass count.

---

## 3. Q3 — XEMU_ASYNC_QUERIES as built, and the ISS-B16 record

### 3.1 Mechanism

READ-IN-CODE (`reports.c:434-462`): at the STALLED site, instead of `finish`, it (1) `pgraph_vk_submit_batch(pg, VK_FINISH_REASON_QUERY_ASYNC)` — submits the open batch *without* `wait_all_slots` and leaves `recycle_pending` set so descriptor/framebuffer/staging recycle defers to the next real finish; (2) `pgraph_vk_wait_for_submission(r, r->submit_count - 1)` (`reports.c:461`) — **a blocking fence wait on its own submission** (the "SPEC-CORRECT ORDERING" comment at `reports.c:450-460` explains why: query availability is a latch; without the fence, `WAIT_BIT` can be satisfied by a slot still latched available from its previous epoch before this epoch's reset executes — the Vulkan-spec stale-value wart); (3) `process_pending_reports_internal` → `apply_reports_from_range(d, query_epoch_base, num_queries_in_flight)` (`reports.c:276-287`) — reads **this epoch's own slots**.

Because the queue is in-order, waiting the newest submission's fence retires every earlier one too — the ISS-B16 adversarial addendum states it flatly: "wait_for_submission(newest) ≡ wait_all_slots on one in-order queue" (`KNOWN_ISSUES.md:207-213`). **So as-built async does not shrink the wait at all; it skips only the recycle phase and the redundant re-waits.** Its supporting cast: the `begin_query` owner fence (`draw.c:1183-1199`) restores the slot-reuse happens-before that `wait_all_slots` used to provide for free.

### 3.2 What it measurably achieved

MEASURED (ladder, mission content via monitor-loadvm): `STALLED` 3→0 (replaced by `QUERY_ASYNC_RESOLVE=3`), mspf 30-49 wobble → 28-31 steady. No large net win — consistent with §3.1 (same wait, different bucket) and with the absorption law (query-ring's queries 2.8→0.1 grew present_composite 6.6→9.3, net flat — MEASURED 2026-07-26 interleaved A/B).

### 3.3 The crucial distinction — confirmed

READ-IN-CODE, confirmed exactly as the campaign states it:

| | source slots read | value written | pfifo blocking |
|---|---|---|---|
| sync drain (`reports.c:464`) | own epoch `[0, num)` after full drain | true count for the polled interval | full pipeline wait |
| async (`reports.c:449-462`) | **own epoch** `[query_epoch_base, +num)` (`reports.c:285-286`) | **byte-identical to sync** (same slots, read after the owning fence) | own-submission fence ≈ full wait |
| query-ring (`reports.c:345-416`) | **retired epoch `(seq - lag)`**'s range (`reports.c:397,409`) | the *same poll one frame earlier* — deliberately stale | none |

Async delivers the **same answers, later-or-equal**; the guest polls until written. The ring substituted a different interval's answers instantly — and SB drives *culling* off those values, so staleness culled live geometry (see-through walls, owner-bisected live 2026-07-26). REFUTED for SB on both correctness and perf; its reusable remains are the parameterized `apply_reports_from_range`, the epoch descriptors, and the owner map.

### 3.4 The ISS-B16 trail, honestly

- **Conviction** (MEASURED, owner eyes): the four-config bisect showed flicker with async ON after exact-dirty was exonerated — M-2's "last variable flips alone" was satisfied and async went hard-OFF.
- **Theory graveyard** (`KNOWN_ISSUES.md:296-318`): reset-overlap — refuted by spec (reset carries submission-order dependency); owner-fence — placebo (its counter read 0; the 1.86x "clearance" retracted after a 4.13x rerun); stale-latch spec-fix (`4018495246`) — landed, correct per spec, but falsified by its own pre-registered test (3.19x separation persisted); deferred-recycle hazard — cleared by an *armed* sync-validation run (zero relevant VUIDs; the earlier "zero VK errors" greps were blind — M-3).
- **Reclassification** (`KNOWN_ISSUES.md:207-233`): the timing-variance theory died under adversarial review — baseline/async frame-time CVs identical (0.211 vs 0.214); detector separations did not reproduce (0.22/0.38/0.70 across same-binary runs); the "3.19x" was a capture window opening on a ~0.5 s smooth brightness settle (autocorr +0.87); free-running ~17 fps `xwd` against ~25 fps presents = capture-beat-sensitive instrument. The doorway "flicker" was classified as the legitimate rotating beacon + film grain (`KNOWN_ISSUES.md:194-205`). Code-level: post-spec-fix async writes byte-identical values.
- **What would settle it**: the designed-but-unrun **per-present framebuffer dump A/B** — one composited frame per guest flip (capture beat eliminated by construction), same detector math, pre-registered thresholds, replicated per M-1. async ≈ baseline ⇒ innocent, reopen for shipping; async ≫ baseline ⇒ a genuine guest-timeline residual exists.
- **What remains unproven**: whether async-queries produces *any* guest-visible difference at all. The eye-conviction stands as one un-reproduced observation from a session where Source-1 flicker and two legitimate flicker-look-alikes were present; the value-path is proven clean in code; the delivery-timing path has no surviving positive evidence against it — and no exonerating per-present run either. Any Attack A/B build inherits this gate: default OFF, toggle trio (M-5), flicker_detect + owner eyes (M-4), ideally after the per-present dump lands.

---

## 4. Q4 — Attack A: report delivery without pfifo blocking

### 4.1 Design

Split the STALLED resolve into two phases, both on the pfifo thread (no new threads — DO-NOT respected; "All VK queue/fence ops stay on the pfifo thread" kept):

**Phase S — submit at the boundary (existing code path).** Same trigger as today (`reports.c:426-427`). Do what async does at `reports.c:449` — `submit_batch(QUERY_ASYNC)`, recycle deferred — but **do not wait and do not apply**. Record a pending-resolve marker: `{watermark = queries_submitted-at-submit (already stamped as slot->query_first + query_count, draw.c:1424-1426), submit_idx = r->submit_count - 1}`. Return to the loop.

**Phase C — completion poll at the top of the pfifo loop.** `pgraph_process_pending_reports` runs every iteration already (`pfifo.c:488`); add a completion branch that runs *regardless of `dma_get/dma_put` and `in_command_buffer`*: if a marker is pending and `vkGetFenceStatus(fence(submit_idx)) == VK_SUCCESS` (non-blocking; ring-slot lookup pattern already exists at `draw.c:1379-1392`), call a **prefix-apply**: `apply_reports_from_range` over `[0, watermark)`, consuming only queue entries with `query_count <= watermark`, leaving later entries queued. Write those reports to guest RAM; clear the marker.

**Loop-park fix (the deadlock-safety piece).** While a marker is pending, the loop must not park indefinitely on `fifo_cond` (`pfifo.c:491-503`) — the spinning guest generates no kick. Replace the `qemu_cond_wait` with `qemu_cond_timedwait(&fifo_cond, &lock, ~0.5-1 ms)` when (and only when) async work is outstanding. Vulkan has no fence-completion callback; a dedicated fence-waiter thread that only calls `pfifo_kick` (`pfifo.c:93-97`) is a legal alternative (it processes no methods) but the timed wait is simpler and bounds tail latency to the poll period. Lane 1's async present needs the same wakeup treatment — shared infrastructure.

**Required code deltas beyond the above** (all READ-IN-CODE against current structure):
- Relax `assert(!r->in_command_buffer)` (`reports.c:210`) — at prefix-apply time a *new* open CB (new draws recorded while the fence was pending) may exist; the assert becomes "the source range's owning submission is retired".
- Do **not** reset `num_queries_in_flight` to 0 at prefix-apply; rebase both counters by the consumed prefix (or switch to monotonic allocation, §5.1 — cleaner). The existing `queries_submitted` watermark (`renderer.h:580-587`) is exactly the needed low-water mark; its comment already anticipates the decoupling.
- Force-complete (blocking) at: any `pgraph_vk_finish` (automatic — finish waits all slots, then applies the whole queue at `draw.c:1529`; the apply must simply tolerate the marker), `SET_CONTEXT_DMA_REPORT` (`pgraph.c:997-1002` — pending writes must land in the *old* DMA object; today's dispatch there can already miss this if the dry-condition is false, a latent upstream edge worth noting), savevm/flush (`renderer.c:95`), renderer switch.

### 4.2 Latency, and what SB tolerates

- **Worst-case delivery latency** = GPU execution of the submitted batch + one poll period. MEASURED anchors: real GPU execution ≈9-10 ms/frame spread over ~7-8 submissions → typical query-carrying batch ~1-2 ms of GPU work; worst case (whole frame in one batch) ~9-10 ms + ≤1 ms poll. **This equals today's STALLED wait** — the guest observes the write at essentially the same wall-clock moment; only the pfifo thread's 2.8-3.1 ms/frame of blocked time is reclaimed. Batch-delivery semantics are preserved exactly: all reports of the retired range are written together, as the drain writes them today.
- **Does SB spin-poll the report address?** Evidence, in strength order: (1) the in-tree invariant comment `reports.c:332-335` (guest spin-waits on report RAM); (2) MEASURED guest-profiler: TCG thread 77% spin in a kernel wait loop; (3) `sb-rendering-anatomy.md` §4: report requests correlate with pushbuffer-dry STALLED finishes 2-4×/frame — SB requests ~185 reports across the frame but *consumes* them in 2-4 spin-batches; (4) the query-ring live result: instantly-served (stale) values were silently consumed and drove culling — the guest reads whatever appears, whenever it appears; there is no handshake beyond the 16-byte write itself. **No evidence of any timeout/assume-after-N-frames path.** UNKNOWN residue: the exact D3D poll-loop instruction has not been disassembled; the campaign's `refhunt.py` could pin it via the report-buffer pointer, as done for the radar.
- **Poll crossing a frame boundary:** impossible today — the FLIP method's `pgraph_vk_finish(FLIP_STALL)` (`renderer.c:188`) drains and writes everything before the frame ends. Under Attack A alone this backstop remains. Under the composed design (Lane 1 removes the flip drain) a pending report may legitimately cross the flip; delivery then rides the Phase-C poll, and the pool-reset scheme must tolerate cross-frame pendency (§5.1's ping-pong pools solve this by construction).
- **WAIT_BIT vs availability-bit polling:** poll the **fence**, then read with `WAIT_BIT` (guaranteed non-blocking after the fence). Availability-bit polling without the fence re-opens the exact spec wart ISS-B16's theory #2 was about ("a stale value could be returned from a previous use of the query", quoted at `reports.c:453-456`) whenever slots are reused across epochs. The fence-first discipline is already in-tree and sync-val-cleared.
- **Composition with surface-download drains:** downloads force their own resolve today only via full finish; with narrow-fence ON the download path submits and waits only the writing surface's fence (`surface.c:189-206`). Attack A's Phase-S submits **help** here: the radar helper's draws get submitted at the nearest earlier query boundary, so the download's narrow fence is near-signaled by read time — the "predictive submission" the narrow-fence postmortem said was the missing win.

### 4.3 Failure modes (write-downs, not footnotes)

1. **Park-with-pending hang** — any path that leaves a marker pending and enters the *untimed* wait deadlocks the guest's spin. The timed-wait must be keyed on the marker, and an assert should fire if the untimed park is entered with a marker set.
2. **Prefix bookkeeping bug** — applying a report whose `query_count` exceeds the retired watermark makes `WAIT_BIT` block on slots in the *open* CB → infinite wait (never submitted). The prefix rule plus an assert (`report->query_count <= watermark` at apply) is mandatory.
3. **DMA-report remap race** — §4.1's force-drain at `SET_CONTEXT_DMA_REPORT`; without it, deferred writes land in the wrong buffer. Rare (games set it at init) but catastrophic if hit.
4. **Same-offset re-request** — if the guest re-polls the same report offset, two queued writes to one address now land at two distinct times; a guest read between them sees the older (correct-for-its-interval) value. Same hazard class exists today across two drains; benign, but worth stating.
5. **Absorption** — standalone, the freed 2.8 ms will reappear in `present_composite` (measured precedent: query-ring absorption; narrow-fence "no win twice"). Pre-register this: the standalone A/B's success criterion is *pfifo-bucket relocation with zero flicker-detector separation*, not mspf.
6. **ISS-B16 inheritance** — delivery timing per report changes (some earlier, none later). Gate per M-4/M-1: flicker_detect + owner eyes + replication; per-present dump first if available.

---

## 5. Q5 — Attack B: kill the renderpass churn

### 5.1 Legality and reset strategy

- In-pass occlusion queries are legal: `vkCmdBeginQuery`/`vkCmdEndQuery` have renderpass scope "both"; a query begun inside a pass must end in the *same subpass* — xemu's passes are single-subpass (`draw.c:373-385`), and report boundaries sit between draws of the same subpass. `occlusionQueryPrecise` is already a required, enabled feature (`instance.c:514`). Only the **reset** must leave the pass.
- **Reset strategy R1 (recommended): batched in-CB reset + monotonic slots.** Stop resetting per-slot in `begin_query` (`draw.c:1202`). Allocate slots monotonically through the frame (never rewind `num_queries_in_flight` at resolves — rebase watermarks instead, which Attack A wants anyway). Record one `vkCmdResetQueryPool(cb, pool, 0, POOL_SIZE)` in the first command buffer of each frame, before any pass (CB start is outside a pass by construction). GPU submission order alone guarantees the reset executes after frame N-1's queries complete — no host wait. SB uses 185-500 slots/frame against a 1024 pool (MEASURED); overflow hits the existing `begin_pre_draw` backstop (`draw.c:1622-1641`): full drain, reset, rebase — rare and correct.
- **Reset strategy R2: `VK_EXT_host_query_reset`.** **Not currently plumbed**: device extensions are only custom-border-color and memory-budget (`instance.c:324-337`); features go through legacy `pEnabledFeatures` plus an EXT pNext chain (`instance.c:540-562`) — adding `VkPhysicalDeviceHostQueryResetFeaturesEXT` follows the existing `custom_border_features` pattern exactly. Vulkan target: instance = min(driver, 1.3), floor 1.1 (`instance.c:159-168`), device-clamped (`instance.c:389,445`) — so core-1.2 `vkResetQueryPool` cannot be assumed; the EXT is the portable route (universally present on the rig's NVIDIA driver — plumbing READ-IN-CODE; driver support unverified on the rig). Host reset requires the slots not be in use by *pending* command buffers — a host-side ordering obligation that R1 gets for free; under Lane 1 (frames genuinely in flight) that obligation is real. **R1 is the better fit; R2 is optional polish.**
- **Cross-frame read-vs-reset hazard (composed design):** frame N+1's in-CB reset executes on the GPU possibly before the host has read frame N's tail slots (Attack A pendency crossing the flip). Fix by construction: **two pools, ping-pong per frame** — frame N+1 resets and allocates pool B while pool A's tail is read at leisure. Cost: one extra 1024-slot VkQueryPool (trivial). The ring's leftover machinery (range-parameterized apply, per-slot owner map `reports.c:117-120`, epoch descriptors) is exactly the scaffolding this reuses.
- With resets hoisted, `begin_query`/`end_query` drop their `!in_render_pass` asserts and record inside the pass; the `begin_draw` block (`draw.c:1699-1716`) stops ending the pass for queries; passes end only for clears, surface/format switches, non-draw commands, and submits.

### 5.2 The prize

**CPU side.** `method_pipe` = 4.9-5.0 ms (MEASURED) = texture-bind + pipeline lookup/create + renderpass churn. Per query-forced restart the driver records: `vkCmdEndRenderPass` + `vkCmdResetQueryPool` + `vkCmdBeginQuery`/`EndQuery` + `vkCmdBeginRenderPass` + forced `vkCmdBindPipeline` + `SetViewport` + `SetScissor` (`draw.c:1720-1770`). At ~185 restarts × ~3-8 µs driver cost ≈ **0.6-1.5 ms of method_pipe is churn** — HYPOTHESIS (the pipe bucket has never been split below its three components; `pipe-opt`, which only skipped redundant pipeline *lookups*, measured ~0.8 ms and was absorbed). Measure before building: bracket `begin/end_render_pass` + the query functions with a `pfifo2` sub-timer, one rig run.

**GPU side — the asked-for store/load arithmetic, then honesty.** Every pass uses `LOAD_OP_LOAD`/`STORE_OP_STORE` on color and `LOAD/STORE` on depth+stencil (`draw.c:309-335`) — the merge is semantically identity (no clears to reorder). On a *tiler*, each boundary would re-store and re-load both attachments. SB main target 640×480: color RGBA8 1.23 MB + zeta D24S8 1.23 MB; STORE+LOAD both = **4.9 MB/boundary; ×205 ≈ 1.01 GB/frame ≈ 5.2 ms** on the RTX 4050 Laptop's 192 GB/s bus (96-bit GDDR6 — spec figure). At `surface_scale=4` (VR): 78.6 MB/boundary, **16.1 GB/frame ≈ 84 ms**. **The measured ~9-10 ms total GPU frame refutes the literal model** — a desktop IMR renders in place; LOAD/STORE ops cost no copies. What the GPU *actually* pays per boundary is the pass's EXTERNAL→0 dependency (`draw.c:343-371`): a color+depth fragment-to-fragment barrier = a fragment-pipeline bubble ~205×/frame, plus driver-side compression-metadata (CBC) resolve decisions at each pass edge. Estimated recovery **0.5-2 ms of the 9-10 ms GPU frame** — HYPOTHESIS; directly measurable with timestamp queries or Nsight on the rig (not run; H-2). In the composed design a shorter GPU frame is pure win (it is the half being overlapped); standalone it hides inside `present_composite`.

**Ceiling honesty:** both prizes standalone are sub-2-ms class and sit behind the present wall — expected mspf-flat until Lane 1 lands. Attack B's compositional value is larger than its bucket: fewer submissions' worth of driver work, fewer barriers for the GPU scheduler, and ~8-10× fewer pass objects per frame for any future pass-merging/pipelining work.

### 5.3 What breaks

1. **Slot-lifecycle redesign** is the risk center: monotonic allocation + rebased watermarks + ping-pong pools replace the reset-to-zero epoch model (`reports.c:263-272`). Every consumer of `num_queries_in_flight`/`queries_submitted`/`query_epoch_base` (`draw.c:1150,1155,1219-1220,1415-1426`, `reports.c` throughout) must move to absolute-slot arithmetic. The ring's `apply_reports_from_range(src_base, src_count)` already generalizes the read side.
2. **ISS-B16 implications:** GPU timing shifts again (fewer bubbles ⇒ earlier query completion, uniformly). Same gate as Attack A: detector + owner eyes + replication; the per-present dump is the clean instrument.
3. **LOAD/STORE-op correctness at merged boundaries:** none — all ops are LOAD/STORE already (`draw.c:312-333`); merging cannot change attachment contents. Clears remain their own pass-break (`draw.c:1714-1716`), untouched.
4. **Surface-state transitions:** layout transitions and copies run via `begin_nondraw_commands` → `ensure_not_in_render_pass` (`draw.c:1589-1595`) and `update_surface_part`'s explicit pass-end (`surface.c:1800`) — these genuine breaks stay; an in-pass active query at such a break is ended in-pass before `vkCmdEndRenderPass` and a fresh slot opens in the next pass (the split machinery of §1.3). Slot consumption rises by one per genuine break (~+20/frame) — well inside the pool.
5. **Query-active-across-pass-end is illegal** — the in-pass end must be enforced by assert in `end_render_pass` (a query begun in-pass must end in that subpass; missing this is a validation error, and sync-val must be run *armed* per M-3).
6. **`framebuffer_dirty` recreation while a pass is open** (`draw.c:1651-1668`) is unchanged — it already forces the pass closed first.
7. **GL backend untouched** (no render-pass concept; `gl/reports.c`), savevm untouched (finish still drains everything).

---

## 6. Q6 — composition: A + B + Lane 1 (async present)

**The composed frame:** Attack B keeps passes open between genuine boundaries (~20-30/frame); Attack A turns the 2-4 STALLED sites into fire-and-poll submits; Lane 1 removes the flip/composite drain. The pfifo thread then *never blocks mid-frame on the GPU* except at:

| residual sync point | site | class | disposition |
|---|---|---|---|
| genuine surface downloads (radar + eviction/invalidate) | `surface.c:189-206,983-998` narrow path | real data dependency, ~4/frame, 1 CPU-read-triggered (MEASURED) | irreducible read; latency hidden because Attack A's Phase-S submits started the writes early — the near-signaled-fence design |
| **surface upload full finish** | `surface.c:1331` (`SURFACE_CREATE`, "FIXME: SURFACE_UP"), fires with `SURF_UPLOAD=2`/frame (MEASURED) | **un-narrowed full drain — the biggest residual, owned by no lane** | needs the same submit+narrow-wait treatment (WAW gates at `surface.c:1326-1327` already exist); without it, it re-serializes everything the other attacks freed |
| single-time-copy fence waits | `command.c:150-166` (`synctex` 0.7 ms MEASURED) | per-copy blocking round-trip | small; eliminable later by folding copies into the slot CB with semaphores |
| CB-fence-ring backpressure | `draw.c:1544-1553` at >4 in flight (`CB_FENCE_RING_SIZE`, `renderer.h:390`) | intended pipelining bound | tune ring depth if it ever bites |
| frame-boundary query-pool hygiene | §5.1 ping-pong | none (by construction) | — |
| `SET_CONTEXT_DMA_REPORT`, savevm/flush, renderer switch, pool/descriptor overflow backstops | `pgraph.c:997`, `renderer.c:95`, `draw.c:1640` etc. | rare force-drains | correct and kept |

**Do mid-frame drains disappear?** Yes for queries (the STALLED wait ceases to exist as a block), yes for pass churn, **no** for genuine downloads (they shrink to narrow, near-signaled fence waits) and **no** for the `surface.c:1331` upload finish until it is narrowed — that line is the flagged gap in the composed plan. The end-state frame is: CPU records ~continuously (method ~15-16 ms, the remaining true wall), GPU runs ~9-10 ms (less Attack B's bubble recovery) one frame behind, reports and radar bytes delivered by fence-poll at GPU completion inside the guest's natural spin windows. The measured absorption law says every millisecond freed here surfaces only after Lane 1's overlap lands — these lanes compose or they do not pay.

---

## Confidence & unknowns

**High confidence (READ-IN-CODE, multiply-anchored):** the report queue/resolve anatomy (§1); the reset-outside-pass forcing chain and the ~205/frame accounting (§2); async-vs-ring same-answers/stale-answers distinction (§3.3); as-built async's wait being ≡ full wait on an in-order queue (§3.1, corroborated by `KNOWN_ISSUES.md:210-212`); the extension-plumbing status (no host_query_reset today, clean EXT insertion path; §5.1); the drain census (§6 table).

**Medium confidence:** Attack A's latency equivalence (arithmetic on measured GPU times; unmeasured end-to-end); SB's spin-tolerance (four independent evidence lines, but the exact guest poll loop is undisassembled — pinnable with `refhunt.py`); the prefix-apply design (grounded in existing watermark scaffolding, but the slot-lifecycle rework in §5.3.1 is the riskiest code surface in this lane).

**Low confidence / must-measure-first (M-rules):** the churn's CPU slice of method_pipe (0.6-1.5 ms, HYPOTHESIS — one sub-timer rig run settles it); the GPU bubble recovery (0.5-2 ms, HYPOTHESIS — timestamp queries/Nsight); NVIDIA driver behavior for in-pass queries at this call density (expected native-fast, unverified); whether async-class delivery timing has *any* guest-visible effect (ISS-B16's open core — the per-present dump A/B is the decisive, still-unrun experiment, and every build here is gated behind flicker_detect + owner eyes + replication, default OFF with the toggle trio).

**Known gaps this lane hands to the coordinator:** the un-narrowed `surface.c:1331` upload finish (2/frame, full drain, no owner); the latent `SET_CONTEXT_DMA_REPORT` mid-stream remap edge (upstream-inherited); the `zpass ÷ scale²` truncation bug at VR scale (`reports.c:231-233`, P5) which any query rework should fix in passing (round, don't truncate); and the pfifo-loop timed-wait infrastructure that Lane 1 and Attack A should share.
