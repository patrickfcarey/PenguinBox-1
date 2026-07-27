# Prior art: how mature emulators pipeline the GPU thread and still answer correctness-critical readbacks

Lane 4 of the "over the present wall" investigation. Question: how do RPCS3, DXVK, PCSX2, Dolphin and Xenia overlap CPU command recording with GPU execution *without* lying to a game that consumes occlusion-query results or read-back pixels — the two ways Steel Battalion is hostile. Companion lanes: `overlap-present-path.md`, `overlap-query-attacks.md`, `overlap-resource-audit.md`.

Snapshot: all code cited was read from each project's `master` on GitHub, 2026-07-26. Line numbers are from that snapshot and will drift; function names are the stable anchors. Citation tags: **[src]** = read the project's source; **[disc]** = PR/issue/blog/tweet/docs; **UNVERIFIED** = claim seen only in a search snippet or summary that could not be opened directly.

## Executive summary

- Every emulator studied keeps ONE method-decode thread and pipelines *behind* it: a ring of command buffers with fence counters, mid-frame submits triggered by heuristics, and waits that are always scoped to one fence — never record → submit → wait-idle per frame. Nobody threads the decoder: RPCS3 ships "Multithreaded RSX" default-off [src], desktop Dolphin ships dual-core default-off [src].
- RPCS3's ZCull is the exact NV2A-zpass analog, and its answer is: queue the report write, retire it *in order* from a ~100 µs tick on the RSX thread, and if the queue head is >300 µs old, **submit** the command buffer mid-frame (a sync *hint*) — do not wait. A blocking wait happens only when a correct value is demanded right now, and then it waits the fence of an already-submitted buffer.
- Serving approximate or stale values to a game that consumes them is a *proven* failure in the field: RPCS3 shipped saturated/approximate zpass counts, hit flickering broken graphics (Brutal Legend et al.), and made "Accurate ZCULL stats" the default; its "Relaxed" sync mode officially "can greatly improve performance in some games or completely break others" [src: tooltips.h]. This independently replicates the see-through-wall flicker our own stale-epoch query ring produced.
- DXVK never blocks a `GetData()` poll: it returns not-ready and lets the game spin, while flush heuristics guarantee convergence — "if the GPU is about to go idle, flush aggressively. This may be required if the application is spinning on a query" [src: util_flush.cpp], plus a per-query stall history that pre-submits at query End.
- PCSX2 keeps readbacks byte-exact (the radar analog) and attacks their *cost* instead: dirty-rect tracking to shrink them, fence-scoped waits, submit-if-still-unsubmitted, and an optional GPU-spin to keep clocks up during the wait.
- Dolphin and Xenia relax correctness *by default* (skip EFB access / no readbacks, per-game opt-ins) — the right call for their libraries, and exactly the wrong call for Steel Battalion; they define the tier we must not ship as default.

## Shape mapping

| | xemu (today) | RPCS3 | DXVK (D3D9) | PCSX2 | Dolphin | Xenia |
|---|---|---|---|---|---|---|
| Single method-decode thread | pfifo | RSX thread (MTRSX off by default) | app thread → **dxvk-cs** thread | EE → **MTGS** GS thread | video thread (dual-core off on desktop) | "GPU Commands" thread |
| Backend pipelining depth | 1 CB, wait-idle per frame | up to 512 CB chunks in flight | CS chunks + submit thread, frame latency 3 | 3 CBs (fence ring) | 8 CBs, 2 frames in flight, optional submit thread | submit per PM4 primary buffer end |
| Occlusion-query consumer | SB spin-polls report RAM | game polls report RAM (identical) | game spin-polls `GetData` | n/a (GS has no queries) | game reads PE counters (MMIO) | n/a here |
| Game-logic readback | radar surface | semaphores/reports | `GetRenderTargetData` etc. | VIF1 local→host FIFO | EFB peeks / EFB→RAM | resolve/memexport |
| Who blocks when a correct value is demanded | **pfifo, on the whole pipeline** | nobody usually; RSX waits one fence in the worst case | the game (spins); never the CS thread | EE (ring drain) + GS (one fence) | CPU thread (blocking event) | GPU thread (full drain, opt-in) |

---

## 1. RPCS3 — RSX ZCull reports (the closest analog; prioritized)

Files read [src]: `rpcs3/Emu/RSX/RSXZCULL.{h,cpp}`, `rpcs3/Emu/RSX/RSXThread.{h,cpp}`, `rpcs3/Emu/system_config.h`, `rpcs3/rpcs3qt/tooltips.h`, `rpcs3/Emu/RSX/VK/VKGSRender.{h,cpp}`, `rpcs3/Emu/RSX/VK/VKQueryPool.{h,cpp}`, `rpcs3/Emu/RSX/VK/VKGSRenderTypes.hpp`, `rpcs3/Emu/RSX/VK/vkutils/device.h`.

### 1a. Pipelining mechanism

- One RSX thread decodes the FIFO and records Vulkan commands. `Multithreaded RSX` exists but defaults to `false` (`system_config.h:157`) — same DO-NOT as ours.
- The Vulkan backend keeps a chain of up to `VK_MAX_ASYNC_CB_COUNT = 512` command-buffer chunks (`VKGSRenderTypes.hpp:25`, `VKGSRender.h:109-113`). `flush_command_queue()` (`VKGSRender.cpp:1526`) closes and submits the current chunk and rotates to the next **without waiting**; `hard_sync` (wait + drain present queue) exists but is invoked only at flips and emergencies. Submission cadence is event-driven (flip, sync hints, DMA label releases, resource pressure), many times per frame.
- Frame contexts are queued (`m_queued_frames`) and presents are consumed asynchronously (`check_present_status`).

### 1b. How zpass reports are delivered

The pipeline is: *enqueue → tick-retire in order → bounded-delay submit hint → targeted wait only if truly forced.*

1. **Enqueue, never resolve inline.** `NV4097_GET_REPORT` → `ZCULL_control::read_report()` (`RSXZCULL.cpp:134`): end the running host occlusion query, push a `queued_report_write{sink, type, query}` onto `m_pending_writes`, immediately begin a fresh query, continue decoding. Report-spam with no active query is answered *immediately* from the running statistics counter for the current stat epoch (`RSXZCULL.cpp:156-161`) — a correct cumulative value, not a stale one.
2. **Tick-based in-order retire.** The RSX main loop calls `zcull_ctrl->update()` every 64 FIFO cycles (`RSXThread.cpp:1144-1160`). `update()` polls the *oldest* pending query non-blockingly (`check_occlusion_query_status`) and, when complete, writes the report into guest memory (`retire`/`write`, with a reservation lock) and pops it. Internal scheduling granularity: `min_zcull_tick_us = 100` (`RSXZCULL.h:91`).
3. **Bounded delay via submit-hint, not wait.** If the queue head is older than `max_zcull_delay_us = 300` (`RSXZCULL.h:90`) and a CPU consumer exists, `update()` raises `sync_hint(zcull_sync)`. The Vulkan override (`VKGSRender.cpp:1650-1717`) then checks: is that query recorded in the *current, unsubmitted* command buffer? If yes → `flush_command_queue()` **immediately** ("Unavoidable hard sync coming up, flush immediately. This heavyweight hint should be used with caution."). Submitting early is the whole trick: by the time anyone insists on the value, the GPU has usually already produced it.
4. **Targeted wait as last resort.** `get_occlusion_query_result()` (`VKGSRender.cpp:2780-2812`) busy-waits *only* if the query is still in the current CB (logging `[Performance warning] Unexpected ZCULL read caused a hard sync`), then polls that query slot alone. `read_barrier(address …)` variants drain only up to one sink address (`update(sync_address)` loop) — partial drain, not pipeline drain.
5. **Flush-on-read detection via page faults.** Report sink pages are `mprotect`ed no-access when a report is enqueued (`on_report_enqueued`, `RSXZCULL.cpp:818-839`). The first time the *guest CPU* touches one, the process AV handler (installed by the RSX thread, `RSXThread.cpp:693-696`) routes to `ZCULL_control::on_access_violation` → `disable_optimizations`: that memory location is permanently flagged `m_pages_accessed`, every subsequent report there counts as `m_critical_reports_in_flight`, and `sync()` (the pipeline-flush entry, `RSXThread.cpp:2666-2686`) actually drains from then on. Games that never read reports pay nothing — `sync()` early-outs when `!m_critical_reports_in_flight` (`RSXZCULL.cpp:418-422`), and leaked never-read reports are cleared at flip ("Some applications leave the zpass/stats gathering active but don't use the information… Seen in Diablo III and Yakuza 5", `RSXThread.cpp:3262-3268`).
6. **Backpressure.** 2048 logical query slots; at `max_safe_queue_depth = 1792` in-flight, `update()` force-flushes from the front (`RSXZCULL.h:92-93`, `RSXZCULL.cpp:538`). The VK pool holds 32768 physical query slots (`OCCLUSION_MAX_POOL_SIZE = DESCRIPTOR_MAX_DRAW_CALLS = 32768`, `VKHelpers.h:11`, `vkutils/device.h:12`); one logical zcull query maps to a *list* of physical queries, one per command buffer it spans (`m_occlusion_map[...].indices`, `VKGSRender.cpp:2725-2760`), so a query interrupted by a submit needs no render-pass or CB gymnastics.
7. **Latency-shaving on the result read.** `poke_query` reads with `VK_QUERY_RESULT_PARTIAL_BIT | WITH_AVAILABILITY_BIT` (`VKQueryPool.cpp:12-50`): in approximate mode a query is treated as done the moment *any* sample passed, before the GPU finishes the range. Conditional render on the GPU side uses `vkCmdCopyQueryPoolResults(..., WAIT_BIT)` into a predicate buffer — GPU-side wait, zero CPU stall (`VKQueryPool.cpp:183-187`, `VKGSRender.cpp:2847+`).

### 1c. Accuracy modes, defaults, field evidence

Two config bools presented in the GUI as a 3-way "ZCULL accuracy" (`tooltips.h:46` [src]):

| GUI mode | `precise_zpass_count` ("Accurate ZCULL stats") | `relaxed_zcull_sync` | Meaning |
|---|---|---|---|
| **Precise** (code default: `system_config.h:158,164` — relaxed=false, precise=**true**) | on | off | `VK_QUERY_CONTROL_PRECISE_BIT`, exact counts scaled down by resolution-scale² (`RSXZCULL.cpp:336-351`), full availability waits |
| **Approximate** ("recommended for most games" per tooltip) | off | off | any-hit short-circuits: first nonzero retires the whole chain, value written as saturated `0xFFFF` (`RSXZCULL.cpp:345-351,461-473`), PARTIAL_BIT early accept |
| **Relaxed** | — | on | `sync()` stops draining entirely; it just nudges `update(hint=true)` (`RSXZCULL.cpp:424-428`) — reports land whenever they land |

Field evidence:

- Tooltip [src]: "Precise is the most accurate to PS3 behaviour. Required for accurate visuals in some titles such as **Demon's Souls** and **The Darkness**… Relaxed… can greatly improve performance in some games or **completely break others**."
- Official RPCS3 account, Sept 2021 [disc, quoted from search results]: "we have implemented accurate ZCULL statistics counting, simulating the exact same behaviour as the RSX on a real PS3. This has fixed graphical issues on many games, including **flickering broken graphics on Brutal Legend**" (https://x.com/rpcs3/status/1435340848359563264). I.e., the *approximation itself* — not just timing — caused flicker in count-consuming games, and RPCS3's response was to make exact counts the default.
- Official RPCS3 account, Feb 2022 [disc, search snippet]: "If you're using ZCull Accuracy > Relaxed and you see **flickering graphics**, try going back to Approximate/Precise" (https://x.com/rpcs3/status/1494844123731398657). Also issue #12972 "Inconsistency with Relaxed Zcull" [disc, title only] and a wiki category of games pinned to each mode (**UNVERIFIED** content — 403 on fetch).
- History [disc, via WebFetch of PRs]: kd-11's PR #4265 (2018-03-13) introduced the deferred model — "Defers report updates slightly to avoid a hard stall GPU-side. Performance improvements can be huge depending on the game"; PR #6940 (merged 2019-11-04) — "avoid hard sync condition as much as possible", partial-result reads modeled on RADV behavior, delayed report transfers.

### 1d. Portable lessons for xemu

- The unit of deferral is the **report write**, not the query resolve: pfifo's `GET_REPORT` should enqueue `{sink address, query range}` and move on; SB's own spin loop *is* the wait, exactly as on PS3 (and on real NV2A, which also wrote reports asynchronously).
- Add a retire tick to the pfifo loop (RPCS3: every 64 FIFO cycles / 100 µs granularity) that polls the oldest fence/query non-blockingly and writes finished reports. Most of SB's 185–500 polls/frame are then answered from memory the guest already has.
- Copy the 300 µs rule: age-out the queue head by *submitting* the open Vulkan batch, never by waiting. This is `XEMU_ASYNC_QUERIES`'s submit-then-wait-own-fence path minus the wait in the common case — the wait remains only for a poll that arrives before the GPU catches up.
- SB is permanently in RPCS3's "critical/pages-accessed" state (it provably consumes reports), so skip the mprotect machinery for SB; it becomes interesting later as an auto-detector for other titles.
- Approximate/saturated counts are pre-validated as dangerous: RPCS3 walked it back. Do not adopt the any-hit short-circuit unless SB is proven to only test `!= 0`.

---

## 2. DXVK — D3D9 occlusion queries (the API-shape analog of SB's spin-poll)

Files read [src]: `src/d3d9/d3d9_query.{h,cpp}`, `src/d3d9/d3d9_device.{h,cpp}`, `src/d3d9/d3d9_swapchain.cpp`, `src/dxvk/dxvk_cs.{h,cpp}`, `src/dxvk/dxvk_queue.{h,cpp}`, `src/dxvk/dxvk_gpu_query.{h,cpp}`, `src/dxvk/dxvk_cmdlist.h`, `src/util/util_flush.{h,cpp}`.

### 2a. Pipelining mechanism

- Three stages: app thread appends typed commands into 16 KiB CS chunks (`DxvkCsChunkSize = 16384`, `dxvk_cs.h:15`) with monotone sequence numbers → **dxvk-cs** thread executes chunks into a `DxvkContext`, recording Vulkan → `DxvkSubmissionQueue` submits/presents on its own thread (`dxvk_queue.h`).
- Frame throttle: default max frame latency **3** (`DefaultFrameLatency = 3`, `d3d9_device.h:214`), enforced at Present by waiting a CPU-side `frameLatencySignal` (`D3D9SwapChainEx::SyncFrameLatency`, `d3d9_swapchain.cpp:1139-1152`).

### 2b. How a spin-polled query avoids stalling anything

- `GetData()` **never blocks** (`d3d9_query.cpp:129-211`): if the End hasn't even been recorded by the CS thread yet (`m_resetCtr != 0`) → `S_FALSE`; else it calls `vkGetQueryPoolResults` (64-bit, **no WAIT bit**) directly *from the app thread* (`dxvk_gpu_query.cpp:87-104`) — `VK_NOT_READY` → `S_FALSE`. First success is cached (`D3D9_VK_QUERY_CACHED`) so re-polls never touch Vulkan again.
- Convergence is guaranteed by the flush heuristic. On a polled-but-pending `GetData(FLUSH)`: `ConsiderFlush(GpuFlushType::ImplicitSynchronization)`. `GpuFlushTracker::considerFlush` (`util_flush.cpp:13-85`): if fewer than `minPendingSubmissions = 2` submissions are in flight → **flush now** ("If the GPU is about to go idle, flush aggressively. This may be required if the application is spinning on a query or resource"); otherwise flush once ≥ `min(20, pending×3)` chunks have accumulated. Strong hints flush at ≥3 chunks "to reduce readback latency", weak at ≥6; missed stronger hints are remembered and honored at the next opportunity.
- **Adaptive pre-submit:** each query keeps a 32-slot sliding stall mask; ≥16 stalls of the last 32 sets a sticky `IsStalling()` (`d3d9_query.h:59-70`). Ending a stalling query triggers `ConsiderFlush(ImplicitWeakHint)` — and a stalling EVENT query forces `ExecuteFlush` unconditionally (`d3d9_device.cpp:7828-7843`). So games that habitually poll early get their work submitted *at End time*, before the poll loop even starts.
- **Query mechanics under pipelining:** one logical `DxvkQuery` accumulates N physical pool queries; the query manager ends/re-begins physical queries at render-pass and command-buffer boundaries (`restartQueries`, `dxvk_gpu_query.cpp:387-430`), so queries stay legally inside render passes with no pass churn. Query resets use **host reset** — `vkResetQueryPool` called from the CS thread at allocation time (`dxvk_cmdlist.h:1288-1293`) — never `vkCmdResetQueryPool` mid-stream. Occlusion pool: 16384 slots (`dxvk_gpu_query.cpp:260-264`).
- D3D9 occlusion queries are always created **PRECISE** (`d3d9_query.cpp:23-27`) — D3D9 games read real sample counts. The any-hit early-accept exists but only for non-precise query types (`dxvk_gpu_query.cpp:45-51`).

### 2c. Defaults and accuracy

There is no accuracy toggle for query *values* — correctness is absolute; only submission timing is heuristic. The knobs (`d3d9.maxFrameLatency` etc.) affect latency, not truth.

### 2d. Portable lessons for xemu

- The "GPU nearly idle → submit immediately" rule is precisely the situation at SB's stall sites (`dma_get == dma_put` with reports pending): at that moment there is at most one submission in flight, so DXVK would flush instantly. Their thresholds (2 pending submissions; 3/6/20 chunks) are tuned numbers worth stealing wholesale.
- The stall-history bitmask is a cheap self-tuning trigger: after the first frame, every SB query would be flagged stalling and get submitted at End — equivalently, xemu could flush the open batch on `GET_REPORT` itself once reports have ever been observed to stall.
- Never make the answer path block the recording thread; make the *guest* spin (it already does) against memory, and spend engineering only on making the wait short.

---

## 3. PCSX2 — MTGS and EE readbacks of GS memory (the radar analog)

Files read [src]: `pcsx2/MTGS.cpp`, `pcsx2/GS/GS.cpp`, `pcsx2/Config.h`, `pcsx2/GS/Renderers/Vulkan/GSDeviceVK.{h,cpp}`, `pcsx2/GS/Renderers/Vulkan/GSTextureVK.cpp`, `pcsx2/GS/Renderers/OpenGL/GSTextureOGL.cpp`, `pcsx2/GS/Renderers/HW/GSTextureCache.cpp`, `pcsx2/GS/Renderers/Common/GSTexture.h`.

### 3a. Pipelining mechanism

- EE thread writes GIF packets into the MTGS ring; the GS thread consumes and renders. The GS thread is mandatory in release builds (a `SynchronousMTGS` wait exists for dev builds only, `MTGS.cpp:689-696`).
- The Vulkan device runs a fence ring of `NUM_COMMAND_BUFFERS = 3` (`GSDeviceVK.h:33`) with monotonically increasing fence counters; `WaitForFenceCounter(n)` waits exactly the fences needed to reach `n` (`GSDeviceVK.cpp:1106-1187`). Command buffers are submitted multiple times per frame as needed.

### 3b. Correctness-critical readbacks

Two-level block, each minimal:

1. **EE side:** `MTGS::InitAndReadFIFO` (VIF1 local→host transfer, i.e. game reads VRAM bytes) posts a ring packet and calls `WaitGS()` — EE blocks until the GS thread has consumed the ring (`MTGS.cpp:283-297,609-659`).
2. **GS side:** the HW renderer flushes relevant draws, then `GSDownloadTextureVK::Flush()` (`GSTextureVK.cpp:963-985`): if the copy's fence already completed → free; if the copy is in the **current unsubmitted** CB → `ExecuteCommandBufferForReadback()` (submit + wait); else → `WaitForFenceCounter(copy_fence)` only. The staging buffer is persistently mapped; GL implements the same contract with `glFenceSync`/`glClientWaitSync(GL_SYNC_FLUSH_COMMANDS_BIT)` (`GSTextureOGL.cpp:466-492`) — the VK rewrite changed the *wait primitive*, not the architecture.
3. **Volume minimization:** the texture cache tracks `m_drawn_since_read` per target and downloads only the intersecting rect of what the game asked for; a `readbacks_since_draw > 1` heuristic switches to one full-area grab when the game does scattered multi-reads (`GSTextureCache.cpp:4944-4990`).
4. **Duration minimization:** optional **GPU spin** — trivial compute submissions on a second queue during readback waits so the GPU doesn't clock down (`ExecuteCommandBufferForReadback`, `GSDeviceVK.cpp:5293-5310`), plus a CPU-spin option. This exists because power management, not throughput, dominates small-readback latency.

### 3c. Defaults and accuracy ladder

`GSHardwareDownloadMode` (`Config.h:399-406`, default `Enabled`, `Config.h:871`): `Enabled` (accurate, default) / `EnabledForceFull` (always full drawn area) / `NoReadbacks` (skip GPU download, keep sync) / `Unsynchronized` (serve the GS thread's shadow memory without any sync — stale-by-design) / `Disabled` (memset 0) (`MTGS.cpp:285-292`, `GSTextureCache.cpp:4944-4947`). The bottom three tiers are explicitly "wrong answers, fast" and exist as user escape hatches.

### 3d. Portable lessons for xemu

- The radar readback should be: copy enqueued into the current batch at the point the surface is dirtied or the read is detected, then on the actual guest read — submit-if-current, wait one fence, memcpy. Byte-exactness preserved; the wait shrinks to (GPU catch-up on that batch), not (whole pipeline).
- Track dirtied-since-last-read rects per surface; SB's radar is small and mostly static per frame.
- If measurements show the readback wait dominated by GPU clock ramp (PCSX2's experience), the spin trick applies — though on the VR rig, stealing GPU time during a wait may fight the compositor; measure first.

---

## 4. Dolphin — Vulkan pipelining, EFB access, perf queries, deferred copies

Files read [src]: `Source/Core/VideoBackends/Vulkan/{CommandBufferManager.h,CommandBufferManager.cpp,Constants.h,VKPerfQuery.h,VKPerfQuery.cpp}`, `Source/Core/VideoCommon/{AsyncRequests.h,AsyncRequests.cpp,EFBInterface.cpp,FramebufferManager.cpp,PixelEngine.cpp,VideoBackendBase.cpp,BoundingBox.cpp,TextureCacheBase.cpp,Fifo.cpp,VideoConfig.h}`, `Source/Core/Core/Config/{MainSettings.cpp,GraphicsSettings.cpp}`.

### 4a. Pipelining mechanism

- Vulkan backend: `NUM_COMMAND_BUFFERS = 8` in a ring, `NUM_FRAMES_IN_FLIGHT = 2` (`Constants.h:15,18`); fence counters identical in spirit to PCSX2's; optional threaded submission (`CommandBufferManager` submit worker). Multiple submits per frame are normal.
- **Dual core** (CPU thread vs video thread — the analog of threading pushbuffer decode) is **default OFF on desktop**, ON only for Android: "Currently enabled by default on Android because the performance boost is really needed" (`MainSettings.cpp:59-65`). Backend pipelining stays on regardless — the two are orthogonal, which is the exact separation our campaign asserts.

### 4b. CPU access to GPU-produced data

- **EFB peeks** (game logic reads framebuffer pixels): CPU thread → `AsyncRequests::PushBlockingEvent` (CPU thread blocks on a future) → video thread `PullEvents()` ("just flush the pipeline to get accurate results") → `PeekEFBColor/Depth` (`EFBInterface.cpp:36-100`, `AsyncRequests.cpp:17-31`). The framebuffer manager amortizes: it reads back one **tile** (default 64×64, `EFBAccessTileSize`, `GraphicsSettings.cpp:195`) into a staging texture and serves every subsequent peek from that CPU cache until a draw invalidates it (`FramebufferManager.cpp:443-524`, `PopulateEFBCache`). An `EFBAccessDeferInvalidation` hack (default off) keeps serving stale cache until the next natural refill — an explicitly stale tier.
- **Perf queries** (PE counters — the GC/Wii cousin of zpass, used by games for lens-flare/visibility): per-draw-batch VK occlusion queries into a 512-slot ring tagged with fence counters (`VKPerfQuery.cpp:43-84`, `VKPerfQuery.h:38`); results accumulated non-blockingly as fences complete, scaled back to native resolution (`ReadbackQueries`, `VKPerfQuery.cpp:160-230`). Backpressure: at half-full, submit the CB in the background; at full, blocking flush (`EnableQuery`, `VKPerfQuery.cpp:45-49`). A game reading the counter MMIO does `Fifo::SyncGPU` + blocking `FlushResults` (submit + `vkGetQueryPoolResults(WAIT_BIT)`) via a blocking async request (`VideoBackendBase.cpp: Video_GetQueryResult`, `PixelEngine.cpp:103-121`). Fully correct-when-enabled, drain-on-read, with early-submit keeping drains short. If precise queries are unsupported it falls back to boolean "which will be incorrect" (`VKPerfQuery.cpp:62-64`).
- **Bounding box** (Paper Mario): same pattern — SSBO readback on `Get` with dirty-value merge to skip syncs (`BoundingBox.cpp:58-95`).
- **Deferred EFB copies** (`DeferEFBCopies`, default true, `GraphicsSettings.cpp:201`): EFB→RAM copies are batched in `m_pending_efb_copies` and only flushed when the RAM might actually be consumed (`TextureCacheBase.cpp:2403-2523`) — readback work is moved from "when the game issues it" to "when the game could observe it".

### 4c. Defaults vs accuracy — the curated-per-title model

- `EFBAccessEnable` default **false** since PR #13140 (build 2409-254, Sept 2024): "Skip EFB Access from CPU enabled by default… due to some drivers resulting in GPU stalls when EFB access occurs in games where EFB is not even used. Most games that require this… already have this defined in their game inis" [disc; the default itself verified in `GraphicsSettings.cpp:192` [src]]. Skipped peeks return **0** (`EFBInterface.cpp:62-66`). Per-game re-enables keep landing (PR #13215 Neighbours from Hell, #13313 I SPY Spooky Mansion [disc, titles only]). Documented consumers: F-Zero GX heat blur reads the screen; Super Mario Galaxy Pull Stars use EFB peeks for pointer detection [disc — **UNVERIFIED** directly].
- `PerfQueriesEnable` default **false** (`GraphicsSettings.cpp:221`, section "GameSpecific") — counters read 0 unless a game ini turns them on. `BBoxEnable` default false with a write-through fallback so write-then-read games see their own values (`BoundingBox.cpp:75-95`). `EFBToTextureEnable` (skip EFB copies to RAM) default true.

### 4d. Portable lessons for xemu

- Tile-and-cache is the right shape for radar-like reads *if* SB reads the surface with CPU granularity; for a single bulk DMA it degenerates to PCSX2's rect tracking.
- "Defer the copy until the memory can be observed" maps to deferring the radar surface download to the actual guest read of those bytes.
- Dolphin proves per-game accuracy curation works operationally (game inis), but its *global* defaults return zeros — the tier that would kill SB. For a per-game VR product, the equivalent is: hard-pin the accurate path in the SB profile.

---

## 5. Xenia (minor datapoint)

Files read [src]: `src/xenia/gpu/command_processor.cc`, `src/xenia/gpu/d3d12/d3d12_command_processor.cc`, `docs/gpu.md`, `src/xenia/gpu/shared_memory.cc` (skimmed).

- Dedicated "GPU Commands" worker thread consumes the ring buffer (`command_processor.cc:73-90`) — pfifo-shaped.
- Readbacks are **off by default** with unusually blunt cvar text (`d3d12_command_processor.cc:36-48`): `d3d12_readback_memexport` — "Read data written by memory export… may be needed in some games… but causes mid-frame synchronization, so it has a huge performance impact"; `d3d12_readback_resolve` — "Read render-to-texture results on the CPU… huge performance impact." When enabled, the implementation is the maximal hammer: `AwaitAllQueueOperationsCompletion()` (full drain) then map + memcpy into guest memory (`d3d12_command_processor.cc:2422-2470, 2507+`). No middle tier.
- `d3d12_submit_on_primary_buffer_end` default **true**: "Submit the command list when a PM4 primary buffer ends if it's possible to submit immediately to try to reduce frame latency" — even the drain-happy design submits mid-frame eagerly.
- Lesson: wrong-by-default readbacks are viable only when the affected-title set is small and known; SB is the affected title. Xenia is the anti-pattern for our case, but its submit-on-buffer-end default independently corroborates the submit-early consensus.

---

## Synthesis: what to copy, what to avoid (ranked)

### Copy (highest value first)

1. **RPCS3's zcull delivery pipeline, whole.** Enqueue report writes at `GET_REPORT`; retire in order from a cheap tick in the pfifo loop; age-out the queue head (300 µs) by *submitting* the open Vulkan batch; hard-wait only a single fence, only when a poll outruns the GPU. It is the same hardware idiom (report written to guest RAM, guest spins on it), battle-tested against games that consume the values. [RSXZCULL.cpp, VKGSRender.cpp]
2. **Submit-early triggers, in this order:** (i) pushbuffer idle with reports pending → submit now (DXVK's "GPU about to go idle + app spinning" rule — this is literally SB's ~3 stall sites/frame); (ii) query-End of a historically-stalling query → submit (DXVK stall mask); (iii) queue-head age > 300 µs → submit (RPCS3); (iv) ring half-full → background submit (Dolphin `PartialFlush`). All four are cheap; together they make the later wait ~zero. [util_flush.cpp, d3d9_device.cpp, RSXZCULL.cpp, VKPerfQuery.cpp]
3. **Multiple command buffers in flight with fence counters and fence-scoped waits.** 3 (PCSX2) or 8 (Dolphin) buffers with per-buffer fences turns the end-of-frame wall into overlap; `WaitForFenceCounter(n)` replaces wait-idle everywhere, including present. This is the general mechanism the wall problem reduces to. [GSDeviceVK.h/.cpp, CommandBufferManager.h/.cpp]
4. **Byte-exact readback discipline for the radar (PCSX2 shape):** record the copy with the producing batch; on guest read: submit-if-current → wait that fence → memcpy from a persistently-mapped staging buffer; shrink volume with drawn-since-read rect tracking; consider GPU-spin only if clock-ramp measurably dominates. [GSTextureVK.cpp, GSTextureCache.cpp]
5. **Query-pool hygiene from DXVK:** host-side `vkResetQueryPool` (never `vkCmdResetQueryPool` mid-stream) and per-command-buffer physical query segments accumulated into one logical result so submits never fight render passes. [dxvk_cmdlist.h:1288, dxvk_gpu_query.cpp:387-430; corroborated by RPCS3's per-CB `occlusion_data.indices`]
6. **Backpressure + leak hygiene:** cap in-flight logical queries and force-flush from the front near the cap (RPCS3 1792/2048); clear never-consumed pending reports at flip (RPCS3's Diablo III/Yakuza 5 workaround). [RSXZCULL.h/.cpp, RSXThread.cpp]
7. **Later, for other titles:** RPCS3's mprotect-the-report-page trick to *detect* whether a game consumes reports, and Dolphin-style per-game accuracy pinning. SB needs neither (consumption is proven; the profile pins accurate mode).

### Avoid (strongest evidence first)

1. **Serving stale or approximate values to a consuming game.** Proven three ways: our own stale-epoch ring flickers (owner-confirmed); RPCS3's saturated counts flickered (Brutal Legend → "Accurate ZCULL stats" now default); RPCS3's Relaxed sync "can completely break" games per their own tooltip and support advice. The deferred-epoch `XEMU_QUERY_RING` should be retired as a direction, not retuned.
2. **Threading the method decoder.** RPCS3 `Multithreaded RSX` default false; desktop Dolphin dual-core default false. Both projects converged on: pipeline the backend, keep decode single-threaded. (Confirms the project DO-NOT.)
3. **Wrong-by-default readbacks** (Xenia's zeros/no-readback, Dolphin's peek-returns-0): acceptable only with per-title curation infrastructure and no affected flagship. SB is the flagship; its profile must never enter these tiers.
4. **Whole-pipeline drains as the readback primitive** (Xenia's `AwaitAllQueueOperationsCompletion`, our current `pgraph_vk_finish(STALLED)`): every other project scopes the wait to the producing submission's fence.
5. **Blocking the decode thread inside the query/report method itself.** DXVK returns not-ready; RPCS3 queues; both let the *game* spin. The pfifo thread should treat `GET_REPORT` as a write obligation with a deadline, not a synchronization point.

### Direct reading of xemu's current experiments against this prior art

`XEMU_ASYNC_QUERIES` (submit open batch at the stall site, wait only that submission's fence, then read exactly this epoch's query range) is RPCS3's `sync_hint(zcull_sync)` + targeted wait, and DXVK's idle-flush, compressed into one site — the prior art says this is the correct kernel and predicts its remaining cost (same-epoch GPU latency) shrinks further by adding lesson-2 triggers (submit at `GET_REPORT`/End time, tick-retire between stall sites) rather than by any form of staleness. `XEMU_QUERY_RING` corresponds to a mode no studied emulator ships for consumed queries; the closest analogs (RPCS3 Approximate/Relaxed, Dolphin defer-invalidation) are all opt-in and all documented to flicker or break exactly the way SB's walls did.

## Confidence & unknowns

**High confidence (read the code):** all mechanism claims in sections 1a-1b, 2a-2b, 3a-3b, 4a-4b, 5, and every named constant (300 µs / 100 µs / 2048 / 1792 / 32768 RPCS3; 16 KiB chunks / 3-6-20 chunk thresholds / 2 pending submissions / latency 3 / 16384 pool DXVK; 3 CBs PCSX2; 8 CBs / 2 frames / 512-slot query ring / 64 px tiles Dolphin), plus all quoted code comments and cvar strings, and every config default (RPCS3 relaxed=false + precise=true; PCSX2 `HWDownloadMode=Enabled`; Dolphin `EFBAccessEnable=false`, `PerfQueriesEnable=false`, `DeferEFBCopies=true`, desktop `CPUThread=false`; Xenia readbacks false).

**Medium confidence (project-authored discussion fetched via summarizer):** kd-11 PR #4265 and #6940 descriptions; Dolphin PR #13140 rationale (search snippet, but the resulting default verified in source).

**UNVERIFIED / snippet-only:** the two RPCS3 tweets (text arrived verbatim in search results and matches the source-verified feature names); the RPCS3 wiki ZCULL category contents (403); Dolphin performance-guide examples (F-Zero GX heat blur, Super Mario Galaxy Pull Stars); RPCS3 issue #12972 (title only).

**Unknowns that matter for applying this:**
- Whether SB's culling logic tests the zpass count against zero or against a threshold — decides if RPCS3-style any-hit/PARTIAL early-accept (a further latency cut) is safe. RPCS3's regression history says assume "counts matter" until proven otherwise.
- Whether the guest ever reads a *partially written* NV2A report (status/timestamp vs value ordering) — RPCS3 writes the 16-byte report atomically under a reservation lock; xemu's write ordering at the retire site needs the same care.
- RPCS3's exact submission-thread situation (a `VKAsyncScheduler`/queue-submit worker exists; which submits go through it was not traced).
- PCSX2's MTGS ring capacity and EE run-ahead depth in frames (ring is size-bounded, vsync-throttled; exact numbers not captured).
- Whether Dolphin's GameSettings ini set in current master still carries the historical EFB re-enables (PRs #13215/#13313 prove the mechanism is alive).
