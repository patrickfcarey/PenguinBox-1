/*
 * Geforce NV2A PGRAPH Vulkan Renderer
 *
 * Copyright (c) 2024 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "renderer.h"

/* XEMU_QUERY_RING (force-off wins: XEMU_NO_QUERY_RING). Read once at first use.
 * Default OFF: the STALLED occlusion-query resolve stays byte-identical to
 * upstream (a full pipeline finish). Process-wide, examined only at init and at
 * the single STALLED site, so a mid-run env change is a no-op — fine. */
bool pgraph_vk_query_ring_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        enabled = (getenv("XEMU_QUERY_RING") != NULL &&
                   getenv("XEMU_NO_QUERY_RING") == NULL)
                      ? 1
                      : 0;
        fprintf(stderr,
                "query-ring: deferred-epoch STALLED occlusion-query resolve %s\n",
                enabled ? "ON" : "OFF");
    }
    return enabled;
}

/* Optional integer tunable, clamped to [lo, hi]. Lets the coordinator sweep the
 * staleness lag / region geometry from the environment without a rebuild. */
static uint32_t query_ring_env_u32(const char *name, uint32_t def, uint32_t lo,
                                   uint32_t hi)
{
    const char *v = getenv(name);
    if (v == NULL || *v == '\0') {
        return def;
    }
    long n = strtol(v, NULL, 10);
    if (n < (long)lo) {
        n = (long)lo;
    }
    if (n > (long)hi) {
        n = (long)hi;
    }
    return (uint32_t)n;
}

void pgraph_vk_init_reports(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    QSIMPLEQ_INIT(&r->report_queue);
    r->num_queries_in_flight = 0;
    r->queries_submitted = 0;
    r->new_query_needed = false;
    r->query_in_flight = false;
    r->zpass_pixel_count_result = 0;

    r->async_queries_enabled = pgraph_vk_async_queries_enabled();

    /* Stage S3/S4 toggles (see the headers on their _enabled functions). */
    r->async_reports_enabled = pgraph_vk_async_reports_enabled();
    r->batch_qreset_enabled = pgraph_vk_batch_qreset_enabled();
    r->report_marker_pending = false;
    r->report_marker_watermark = 0;
    r->report_marker_submit = 0;
    pg->report_marker_pending = false;
    r->query_pool_reset_pending = false;

    /* Deferred-epoch ring geometry. OFF: one region the size of the historical
     * 1024-slot pool, reused every resolve at base 0 — byte-identical to
     * upstream. ON: query_num_regions disjoint regions of query_region_size
     * slots; the pool grows to their product. Steel Battalion records ~185
     * queries/frame across ~3 STALLED resolves, so a 512-slot region carries a
     * whole frame's worth of one epoch with headroom; the pool-exhaustion
     * backstop (draw.c begin_pre_draw) folds any epoch that would overrun a
     * region. R=8 with lag<R keeps every region unread-and-retired before reuse
     * (the pool-wrap invariant, documented at serve_reports_ring).
     *
     * LAG (the flicker-critical knob): an epoch here is ONE STALLED report
     * resolve (finishes resolve in place, they do not rotate the region — see
     * advance_query_epoch), and SB runs ~3 STALLED resolves/frame, so lag=3
     * serves the report from the SAME poll one whole frame back — phase-aligned,
     * "one-frame-stale" per the design. A lag that is NOT a whole number of
     * frames reads a different intra-frame poll and can reintroduce flicker; if
     * the STALLED-per-frame cadence is not ~3, set XEMU_QUERY_RING_LAG to
     * round(QUERY_RING_SERVED per frame) and re-check flicker_detect.py. */
    r->query_ring_enabled = pgraph_vk_query_ring_enabled();

    /* NEVER-STALE interlock: the deferred-epoch ring serves a RETIRED epoch's
     * counts — the refuted stale tier (culled live walls, owner-bisected
     * 2026-07-26; independently replicated at RPCS3). Its serving path must
     * never be reachable while the drain-identical async-reports /
     * batch-qreset modes are on, so the ring is force-disabled here rather
     * than left to dispatch order. */
    if (r->query_ring_enabled &&
        (r->async_reports_enabled || r->batch_qreset_enabled)) {
        r->query_ring_enabled = false;
        fprintf(stderr, "query-ring: force-OFF (superseded by "
                        "async-reports/batch-qreset; stale serve refuted)\n");
    }
    /* The S3 marker path replaces the legacy submit+wait-own-fence resolve
     * outright (dispatch below prefers it); note the shadowing once. */
    if (r->async_queries_enabled && r->async_reports_enabled) {
        fprintf(stderr, "async-reports: legacy XEMU_ASYNC_QUERIES resolve "
                        "shadowed (marker path takes precedence)\n");
    }

    if (r->query_ring_enabled) {
        r->query_region_size =
            query_ring_env_u32("XEMU_QUERY_RING_SLOTS", 512, 64, 4096);
        r->query_num_regions =
            query_ring_env_u32("XEMU_QUERY_RING_REGIONS", 8, 2, 64);
        r->query_ring_lag = query_ring_env_u32("XEMU_QUERY_RING_LAG", 3, 1,
                                               r->query_num_regions - 1);
    } else if (r->batch_qreset_enabled) {
        /* Ping-pong: frame N+1 allocates from region B while region A's tail
         * results may still be read at leisure (an async-reports marker
         * crossing the flip in the composed design); regions are disjoint
         * slot ranges of ONE VkQueryPool, which gives the same read-vs-reset
         * isolation as two separate pools with none of the pool-handle
         * plumbing (vkCmdResetQueryPool is range-scoped). SB peaks ~500
         * slots/frame against 1024 per region; overflow swaps regions via
         * the begin_pre_draw backstop (post-finish, both regions quiescent). */
        r->query_region_size = 1024;
        r->query_num_regions = 2;
        r->query_ring_lag = 0;
    } else {
        r->query_region_size = 1024;
        r->query_num_regions = 1;
        r->query_ring_lag = 0;
    }
    r->query_epoch_seq = 0;
    r->query_epoch_base = 0;
    r->query_region_origin = 0;
    r->max_queries_in_flight =
        (int)(r->query_region_size * r->query_num_regions);

    /* batch-qreset: region 0 must be batch-reset before its first query;
     * pgraph_vk_begin_command_buffer records it at the next CB begin. */
    r->query_pool_reset_pending = r->batch_qreset_enabled;

    /* Per-epoch descriptor ring (valid=false via g_new0 => sync-drain startup
     * until `lag` epochs have retired). */
    r->query_epochs = g_new0(QueryEpoch, r->query_num_regions);

    /* fix-query-flicker: per-pool-index owner map used by begin_query to fence
     * off cross-epoch slot reuse. Sized to the whole (possibly enlarged) pool,
     * initialised to SURFACE_NO_WRITE (0xFFFFFFFF) so the first use of every
     * index waits nothing. See renderer.h. */
    r->query_index_submit =
        g_malloc_n(r->max_queries_in_flight, sizeof(uint32_t));
    memset(r->query_index_submit, 0xFF,
           r->max_queries_in_flight * sizeof(uint32_t));

    VkQueryPoolCreateInfo pool_create_info = (VkQueryPoolCreateInfo){
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_OCCLUSION,
        .queryCount = r->max_queries_in_flight,
    };
    VK_CHECK(
        vkCreateQueryPool(r->device, &pool_create_info, NULL, &r->query_pool));

    if (r->query_ring_enabled) {
        fprintf(stderr,
                "query-ring: %u regions x %u slots (pool %d), lag %u epochs\n",
                r->query_num_regions, r->query_region_size,
                r->max_queries_in_flight, r->query_ring_lag);
    }
}

void pgraph_vk_finalize_reports(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    /* Renderer switch/shutdown: the preceding flush's finish force-completed
     * any pending marker (and served the queue); drop the stale park hint in
     * the cross-renderer state regardless. */
    pg->report_marker_pending = false;

    QueryReport *report;
    while ((report = QSIMPLEQ_FIRST(&r->report_queue)) != NULL) {
        QSIMPLEQ_REMOVE_HEAD(&r->report_queue, entry);
        g_free(report);
    }

    vkDestroyQueryPool(r->device, r->query_pool, NULL);

    g_free(r->query_index_submit);
    r->query_index_submit = NULL;

    g_free(r->query_epochs);
    r->query_epochs = NULL;
}

void pgraph_vk_clear_report_value(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    QueryReport *report = g_malloc(sizeof(QueryReport)); // FIXME: Pre-allocate
    report->clear = true;
    report->parameter = 0;
    report->query_count = r->num_queries_in_flight;
    report->enqueue_us =
        r->async_reports_enabled ? g_get_monotonic_time() : 0;
    QSIMPLEQ_INSERT_TAIL(&r->report_queue, report, entry);

    r->new_query_needed = true;
}

void pgraph_vk_get_report(NV2AState *d, uint32_t parameter)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    uint8_t type = GET_MASK(parameter, NV097_GET_REPORT_TYPE);
    assert(type == NV097_GET_REPORT_TYPE_ZPASS_PIXEL_CNT);

    QueryReport *report = g_malloc(sizeof(QueryReport)); // FIXME: Pre-allocate
    report->clear = false;
    report->parameter = parameter;
    report->query_count = r->num_queries_in_flight;
    report->enqueue_us =
        r->async_reports_enabled ? g_get_monotonic_time() : 0;
    QSIMPLEQ_INSERT_TAIL(&r->report_queue, report, entry);

    r->new_query_needed = true;
}

/* Scale a zpass accumulator value back to guest resolution (the NV2A counts
 * native pixels; we render at surface_scale_factor x). Upstream truncates;
 * truncation loses up to scale^2 - 1 counted pixels per report — visible at
 * VR scale (findings P5). Round to nearest under the S3/S4 toggles; at
 * scale 1 divisor == 1 so rounding == truncation and the gate is provably
 * inert. Both apply paths (full and prefix) go through here, so a report's
 * value never depends on WHICH path served it. */
static uint32_t zpass_result_scaled(PGRAPHState *pg, PGRAPHVkState *r)
{
    const uint32_t divisor =
        (uint32_t)(pg->surface_scale_factor * pg->surface_scale_factor);
    if (r->async_reports_enabled || r->batch_qreset_enabled) {
        return (uint32_t)(((uint64_t)r->zpass_pixel_count_result +
                           divisor / 2) /
                          divisor);
    }
    return r->zpass_pixel_count_result / divisor;
}

/* Non-blocking twin of pgraph_vk_wait_for_submission: has submission `idx`
 * retired? Same ring-slot identity rules — never recorded, pushed out of the
 * 4-deep ring (=> long complete), or fence signaled. */
static bool submission_retired(PGRAPHVkState *r, uint32_t idx)
{
    if (idx == SURFACE_NO_WRITE) {
        return true;
    }
    struct CbFenceSlot *slot = &r->cb_fence_ring[idx % CB_FENCE_RING_SIZE];
    if (slot->submit_index != idx) {
        return true;
    }
    return vkGetFenceStatus(r->device, slot->fence) == VK_SUCCESS;
}

/* Retire the async-reports pending marker without serving anything: used when
 * a blocking path (any pgraph_vk_finish, the DMA-report remap drain, renderer
 * flush) supersedes it by applying the WHOLE queue — the marker's covered
 * prefix is a subset of what that full apply just delivered, with identical
 * bytes. Counted as a force-drain. */
static void consume_marker_forced(PGRAPHState *pg, PGRAPHVkState *r)
{
    if (r->report_marker_pending) {
        assert(submission_retired(r, r->report_marker_submit));
        r->report_marker_pending = false;
        pg->report_marker_pending = false;
        nv2a_profile_inc_counter(NV2A_PROF_REPORT_FORCE_DRAIN);
    }
}

/* Read occlusion counts from pool range [src_base, src_base + read) and apply
 * them to the pending report queue. The result-application loop below is
 * byte-for-byte the upstream resolve — the ONLY parameterization is WHICH pool
 * slots feed query_results:
 *   - finish / OFF path: the current window's own range (src_base ==
 *     query_epoch_base, src_count == num_queries_in_flight). In OFF mode
 *     query_epoch_base is 0, so this is the exact upstream read.
 *   - deferred ring serve: a retired prior epoch's range (src_base/src_count
 *     from its QueryEpoch descriptor).
 * The report loop always indexes by THIS epoch's num_queries_in_flight, so when
 * a shorter source epoch is read the tail [read, num) is zero-filled (g_malloc0)
 * — an undercount that self-corrects the next frame, never an out-of-bounds
 * read. Exit empties the window: counters return to 0, and the base returns to
 * the region origin (slot-index reuse: legal because per-slot resets +
 * the owner fence cover it) — or, under batch-qreset, advances monotonically
 * past the consumed slots (a region-entry's slots are reset ONCE, as a batch,
 * so an index must never be reused within the entry). A pending async-reports
 * marker is consumed here: this full apply just delivered a superset of its
 * covered reports with identical bytes.
 * Caller guarantees the source range is complete (WAIT_BIT then never blocks). */
static void apply_reports_from_range(NV2AState *d, uint32_t src_base,
                                     uint32_t src_count)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    NV2A_VK_DGROUP_BEGIN("Processing queries");

    assert(!r->in_command_buffer);

    const uint32_t consumed = (uint32_t)r->num_queries_in_flight;

    // Fetch query results (from the caller-selected source range)
    g_autofree uint64_t *query_results = NULL;

    if (r->num_queries_in_flight > 0) {
        query_results = g_malloc0_n(r->num_queries_in_flight,
                                    sizeof(uint64_t)); // FIXME: Pre-allocate
        uint32_t read = MIN(src_count, (uint32_t)r->num_queries_in_flight);
        if (read > 0) {
            VkResult result;
            do {
                result = vkGetQueryPoolResults(
                    r->device, r->query_pool, src_base, read,
                    read * sizeof(uint64_t), query_results, sizeof(uint64_t),
                    VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
            } while (result == VK_NOT_READY);
        }
    }

    // Write out queries
    int num_results_counted = 0;

    QueryReport *report;
    while ((report = QSIMPLEQ_FIRST(&r->report_queue)) != NULL) {
        assert(report->query_count >= num_results_counted);
        assert(report->query_count <= r->num_queries_in_flight);

        while (num_results_counted < report->query_count) {
            r->zpass_pixel_count_result +=
                query_results[num_results_counted++];
        }

        if (report->clear) {
            NV2A_VK_DPRINTF("Cleared");
            r->zpass_pixel_count_result = 0;
        } else {
            pgraph_write_zpass_pixel_cnt_report(d, report->parameter,
                                                zpass_result_scaled(pg, r));
        }

        QSIMPLEQ_REMOVE_HEAD(&r->report_queue, entry);
        g_free(report);
    }

    // Add remaining results
    while (num_results_counted < r->num_queries_in_flight) {
        r->zpass_pixel_count_result += query_results[num_results_counted++];
    }

    r->num_queries_in_flight = 0;
    /* Resolve boundary: every result in [src_base, src_base + read) has now
     * been read (WAIT_BIT above), so the report queue is drained and the
     * window counters return to 0. The disjoint-region base is rotated
     * separately (advance_query_epoch / pgraph_vk_swap_query_region) — a
     * mid-epoch finish resolves in place. Keeping queries_submitted in
     * lockstep with num_queries_in_flight is what guarantees a pool slot is
     * never vkCmdResetQueryPool'd (by a future begin_query) before its result
     * was read. batch-qreset: the base moves FORWARD past the consumed slots
     * instead of rewinding — a batch-reset region entry hands out each index
     * exactly once (vkCmdBeginQuery requires a slot in the reset state, and
     * there is no per-slot reset to relatch it). */
    r->queries_submitted = 0;
    r->query_epoch_base = r->batch_qreset_enabled ?
                              r->query_epoch_base + consumed :
                              r->query_region_origin;

    consume_marker_forced(pg, r);
    NV2A_VK_DGROUP_END();
}

void pgraph_vk_process_pending_reports_internal(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    /* Finish / OFF path: resolve the current epoch from its OWN slots. The
     * enclosing pgraph_vk_finish already ran wait_all_slots, so the WAIT_BIT
     * read never blocks. OFF => query_epoch_base == 0 and src_count ==
     * num_queries_in_flight, i.e. byte-identical to upstream. */
    apply_reports_from_range(d, r->query_epoch_base,
                             (uint32_t)r->num_queries_in_flight);
}

/* Stage S3 Phase C — prefix apply. Serve every queued report whose value is
 * fully determined by the first `take` slots of the current window, reading
 * pool indices [query_epoch_base, query_epoch_base + take). The caller has
 * PROVEN those slots' owning submissions retired (the marker fence), so the
 * WAIT_BIT read completes without blocking AND without the stale-latch spec
 * wart (each covered slot's reset executed in a retired submission before its
 * begin — fence-first discipline, ISS-B16 theory #2's fix pattern).
 *
 * NEVER-STALE: the loop below is the upstream resolve verbatim, merely
 * stopping at the first report that needs slots beyond `take`. A report's
 * bytes = f(accumulator, results of window slots [0, query_count)) — all
 * final (fence-retired) when read — so the value written here is bit-equal to
 * what the full drain would write for the same report; only the pfifo
 * thread's blocking is gone. Slots >= take (possibly in flight or still
 * unsubmitted in the open CB) are NEVER touched: reading one with WAIT_BIT
 * could wait forever on a never-submitted slot — the prefix rule plus the
 * asserts below are the deadlock guard (Lane 2 SS4.3 failure mode 2).
 *
 * Exit rebases the window in lockstep (base += take, num -= take, submitted
 * -= take, surviving reports' query_count -= take): live slots keep their
 * absolute pool indices (base + offset is invariant), so the open CB's
 * recorded vkCmdBeginQuery/EndQuery indices, the submit-stamped
 * query_first/query_count ranges, and the owner map all stay coherent. */
static void apply_reports_prefix(NV2AState *d, uint32_t take)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    NV2A_VK_DGROUP_BEGIN("Prefix-applying %u query slots", take);

    assert(r->async_reports_enabled);
    assert(take <= (uint32_t)r->num_queries_in_flight);
    assert(take <= r->queries_submitted);

    g_autofree uint64_t *query_results = NULL;
    if (take > 0) {
        query_results = g_malloc0_n(take, sizeof(uint64_t));
        VkResult result;
        do {
            result = vkGetQueryPoolResults(
                r->device, r->query_pool, r->query_epoch_base, take,
                take * sizeof(uint64_t), query_results, sizeof(uint64_t),
                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        } while (result == VK_NOT_READY);
    }

    uint32_t num_results_counted = 0;

    QueryReport *report;
    while ((report = QSIMPLEQ_FIRST(&r->report_queue)) != NULL &&
           (uint32_t)report->query_count <= take) {
        assert((uint32_t)report->query_count >= num_results_counted);

        while (num_results_counted < (uint32_t)report->query_count) {
            r->zpass_pixel_count_result +=
                query_results[num_results_counted++];
        }

        if (report->clear) {
            NV2A_VK_DPRINTF("Cleared");
            r->zpass_pixel_count_result = 0;
        } else {
            pgraph_write_zpass_pixel_cnt_report(d, report->parameter,
                                                zpass_result_scaled(pg, r));
        }

        QSIMPLEQ_REMOVE_HEAD(&r->report_queue, entry);
        g_free(report);
    }

    /* Fold the consumed slots' remaining results into the running
     * accumulator, exactly as the full apply does for its tail. */
    while (num_results_counted < take) {
        r->zpass_pixel_count_result += query_results[num_results_counted++];
    }

    /* Window rebase. Surviving reports were enqueued after the marker's
     * watermark, so each needs slots beyond the consumed prefix
     * (query_count > take — report query_counts are non-decreasing in queue
     * order because num_queries_in_flight only grows between applies). */
    QSIMPLEQ_FOREACH (report, &r->report_queue, entry) {
        assert((uint32_t)report->query_count > take);
        report->query_count -= take;
    }
    r->query_epoch_base += take;
    r->num_queries_in_flight -= (int)take;
    r->queries_submitted -= take;

    NV2A_VK_DGROUP_END();
}

/* Stage S4: enter the other ping-pong region. Precondition: both regions
 * quiescent — every submission retired and every queued report applied (i.e.
 * immediately after a full finish), no async-reports marker pending. The
 * incoming region's slots may hold stale latched availability from its last
 * use two entries ago; query_pool_reset_pending makes the next command buffer
 * open with a batched vkCmdResetQueryPool of the whole region, submission-
 * ordered before any of its begins. */
void pgraph_vk_swap_query_region(PGRAPHVkState *r)
{
    assert(r->batch_qreset_enabled);
    assert(r->num_queries_in_flight == 0);
    assert(r->queries_submitted == 0);
    assert(!r->report_marker_pending);
    assert(QSIMPLEQ_EMPTY(&r->report_queue));
    assert(r->query_num_regions == 2);

    r->query_region_origin =
        (r->query_region_origin == 0) ? r->query_region_size : 0;
    r->query_epoch_base = r->query_region_origin;
    r->query_pool_reset_pending = true;
}

/* Stage S3 Phase S: arm the pending-resolve marker. Covered = every window
 * slot already handed to a submitted batch ([base, base + queries_submitted)),
 * whose newest carrier is submission r->submit_count - 1; that fence retiring
 * proves every covered slot's result is final. The pfifo park must switch to
 * the timed wait from this moment (pg mirror read by pfifo_thread). */
static void arm_report_marker(PGRAPHState *pg, PGRAPHVkState *r)
{
    assert(r->async_reports_enabled);
    assert(r->submit_count > 0);
    r->report_marker_watermark = r->queries_submitted;
    r->report_marker_submit = r->submit_count - 1;
    r->report_marker_pending = true;
    pg->report_marker_pending = true;
}

/* XEMU_ASYNC_QUERIES=1 turns the STALLED occlusion-query resolution into an
 * async resolve (submit the open batch, then read exactly this epoch's query
 * range with WAIT_BIT — no whole-pipeline wait_all_slots + recycle). Default
 * OFF: the STALLED path stays byte-identical to upstream (full finish).
 * XEMU_NO_ASYNC_QUERIES=1 forces OFF and wins. Read once at first use — a
 * mid-run env change has no effect, which is fine: the choice is process-wide
 * and the toggle is examined at this single STALLED site only. */
bool pgraph_vk_async_queries_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        enabled = (getenv("XEMU_ASYNC_QUERIES") != NULL &&
                   getenv("XEMU_NO_ASYNC_QUERIES") == NULL)
                      ? 1
                      : 0;
        fprintf(stderr, "async-queries: STALLED occlusion-query resolve %s\n",
                enabled ? "ON" : "OFF");
    }
    return enabled;
}

/* Stage S3 — XEMU_ASYNC_REPORTS=1 (force-off wins: XEMU_NO_ASYNC_REPORTS=1).
 * Async report DELIVERY: at the STALLED site, submit the open batch and arm a
 * pending-resolve marker instead of blocking; the per-iteration pfifo poll
 * (Phase C) applies the retired prefix when the submission's fence signals.
 * The bytes written to guest report RAM are IDENTICAL to the synchronous
 * drain's — same slots, read only after their owning submission's fence, same
 * accumulate-in-order apply loop — only the pfifo thread's blocking changes
 * (the spin-waiting guest absorbs the identical GPU latency). Default OFF:
 * byte-identical to upstream. Read once at first use; process-wide. */
bool pgraph_vk_async_reports_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        enabled = (getenv("XEMU_ASYNC_REPORTS") != NULL &&
                   getenv("XEMU_NO_ASYNC_REPORTS") == NULL)
                      ? 1
                      : 0;
        fprintf(stderr,
                "async-reports: submit-then-poll STALLED report delivery %s\n",
                enabled ? "ON" : "OFF");
    }
    return enabled;
}

/* Stage S4 — XEMU_BATCH_QRESET=1 (force-off wins: XEMU_NO_BATCH_QRESET=1).
 * Renderpass-churn kill: batched per-region vkCmdResetQueryPool at frame
 * start, monotonic slot allocation, ping-pong regions swapped at flip, and
 * vkCmdBeginQuery/vkCmdEndQuery recorded INSIDE render passes so a report
 * boundary no longer ends the pass. Query VALUES and their delivery timing
 * are untouched (the STALLED resolve stays whatever the other toggles make
 * it). Default OFF: byte-identical to upstream. */
bool pgraph_vk_batch_qreset_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        enabled = (getenv("XEMU_BATCH_QRESET") != NULL &&
                   getenv("XEMU_NO_BATCH_QRESET") == NULL)
                      ? 1
                      : 0;
        fprintf(stderr,
                "batch-qreset: batched query reset + in-pass queries %s\n",
                enabled ? "ON" : "OFF");
    }
    return enabled;
}

/* Rotate to the next disjoint epoch region. Called ONLY from a STALLED ring
 * serve, so one epoch == one report-resolve cycle; the intervening finishes
 * resolve in place (apply_reports_from_range without advancing the base) and
 * reuse the current region. Never called when OFF, so query_epoch_base stays 0
 * and the pool is a single reused region. */
static void advance_query_epoch(PGRAPHVkState *r)
{
    r->query_epoch_seq++;
    r->query_epoch_base =
        (r->query_epoch_seq % r->query_num_regions) * r->query_region_size;
    r->query_region_origin = r->query_epoch_base;
}

/* Deferred-epoch ring serve for the STALLED site (XEMU_QUERY_RING).
 *
 * The guest has drained its pushbuffer spin-waiting on occlusion report RAM.
 * Instead of finishing (a full pipeline drain of THIS epoch's GPU work — the
 * ~3ms/frame `queries` wall), submit this epoch's queries WITHOUT waiting and
 * serve the pending reports from a retired epoch a CONSTANT `lag` back. The
 * value the guest reads is `lag` epochs stale — an imperceptible, FIXED phase
 * offset (a rotating beacon's flare shifts by a constant, it does not shimmer;
 * a variable lag would).
 *
 * DEADLOCK SAFETY: a value is written to EVERY pending report on EVERY call
 * (the guest spin-waits on report RAM — deferring the WRITE would hang it). We
 * defer only WHICH epoch's counts feed the write, never whether one happens.
 *
 * POOL-WRAP INVARIANT: region (seq % R) is next reset by begin_query when the
 * epoch counter reaches seq again, i.e. it was last written by epoch (seq - R).
 * Because lag < R, epoch (seq - R) was already served (consumed) at epoch
 * (seq - R + lag) < seq, and long retired (any intervening finish's
 * wait_all_slots, else the 4-deep CB-fence ring's begin_command_buffer
 * backpressure, forced it). So the slot a future begin_query recycles is always
 * unread-and-retired, and the source we read here (region (seq-lag) % R,
 * distinct from the current region since lag < R) is never mid-reset. The
 * per-index owner fence in begin_query is the belt-and-braces backstop. */
static void serve_reports_ring(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    uint32_t seq = r->query_epoch_seq;
    uint32_t base = r->query_epoch_base;
    uint32_t count = (uint32_t)r->num_queries_in_flight;

    /* Select the constant-lag source epoch, if it exists and is still intact
     * (its region not yet recycled — guaranteed by lag < R, checked defensively
     * via the stored seq). */
    QueryEpoch *src = NULL;
    if (seq >= r->query_ring_lag) {
        uint32_t src_seq = seq - r->query_ring_lag;
        QueryEpoch *cand = &r->query_epochs[src_seq % r->query_num_regions];
        if (cand->valid && cand->seq == src_seq) {
            src = cand;
        }
    }

    if (src == NULL) {
        /* Startup: fewer than `lag` epochs have retired. Fall back to the full
         * sync drain (identical to the OFF STALLED path) so the guest still
         * gets a correct value, then record this now-retired epoch so the ring
         * fills. count was captured before the finish drained num to 0. */
        pgraph_vk_finish(pg, VK_FINISH_REASON_STALLED);
        QueryEpoch *cur = &r->query_epochs[seq % r->query_num_regions];
        cur->seq = seq;
        cur->base = base;
        cur->count = count;
        cur->last_submit = r->submit_count - 1;
        cur->valid = true;
        advance_query_epoch(r);
        return;
    }

    /* Push THIS epoch's queries to the GPU without draining. Like the async
     * resolve, submit_batch leaves recycle_pending set so the descriptor /
     * framebuffer / staging recycle defers to the next real finish. */
    pgraph_vk_submit_batch(pg, VK_FINISH_REASON_QUERY_ASYNC);
    QueryEpoch *cur = &r->query_epochs[seq % r->query_num_regions];
    cur->seq = seq;
    cur->base = base;
    cur->count = count;
    cur->last_submit = r->submit_count - 1;
    cur->valid = true;

    /* The source is `lag` epochs old; steady state it is already retired
     * (see POOL-WRAP INVARIANT), so this is a non-blocking fence check. If it
     * ever must block, waiting HERE still serves epoch (seq-lag) — the constant
     * staleness, and thus the flicker-freedom, is preserved; only timing moves. */
    pgraph_vk_wait_for_submission(r, src->last_submit);

    /* Count the reports about to be served, for the self-measuring counter,
     * without perturbing the byte-identical apply loop. */
    unsigned int served = 0;
    QueryReport *rep;
    QSIMPLEQ_FOREACH (rep, &r->report_queue, entry) {
        if (!rep->clear) {
            served++;
        }
    }

    apply_reports_from_range(d, src->base, src->count);

    for (unsigned int i = 0; i < served; i++) {
        nv2a_profile_inc_counter(NV2A_PROF_QUERY_RING_SERVED);
    }

    advance_query_epoch(r);
}

/* RPCS3's max_zcull_delay_us: age-out the report-queue head by SUBMITTING the
 * open batch (never by waiting) once it has sat unserved this long. */
#define REPORT_AGEOUT_US 300

void pgraph_vk_process_pending_reports(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    uint32_t *dma_get = &d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET];
    uint32_t *dma_put = &d->pfifo.regs[NV_PFIFO_CACHE1_DMA_PUT];

    if (r->async_reports_enabled) {
        /* DMA-report remap force-drain: SET_CONTEXT_DMA_REPORT armed
         * pg->force_report_drain because every pending report write must land
         * in the OLD dma object (pg->dma_report is remapped only after this
         * call returns). Full blocking drain: applies the whole queue and
         * consumes any marker (counted REPORT_FORCE_DRAIN there). */
        if (pg->force_report_drain &&
            (r->report_marker_pending || !QSIMPLEQ_EMPTY(&r->report_queue))) {
            pgraph_vk_finish(pg, VK_FINISH_REASON_FLUSH);
            return;
        }

        /* Phase C — completion poll. Runs EVERY pfifo iteration regardless of
         * dma_get/dma_put and in_command_buffer (a new CB with new draws may
         * be open — the prefix apply records nothing and touches only slots
         * whose owning submissions the fence just proved retired). */
        if (r->report_marker_pending &&
            submission_retired(r, r->report_marker_submit)) {
            uint32_t take = r->report_marker_watermark;
            r->report_marker_pending = false;
            pg->report_marker_pending = false;
            apply_reports_prefix(d, take);
            nv2a_profile_inc_counter(NV2A_PROF_REPORT_ASYNC_RETIRE);
        }

        if (*dma_get == *dma_put && r->in_command_buffer &&
            !QSIMPLEQ_EMPTY(&r->report_queue)) {
            /* Phase S — the STALLED boundary: pushbuffer dry with reports
             * outstanding, the site that was a full synchronous drain.
             * Submit the open batch (no wait, recycle deferred — exactly the
             * legacy async submit) and arm the marker; the guest's own spin
             * on report RAM absorbs the identical GPU latency while the
             * pfifo thread keeps decoding. A marker already pending (fence
             * unsignaled, guest pushed more work without consuming) simply
             * coalesces to the newer submission: its fence retires after the
             * old one on the in-order queue and the new watermark covers a
             * superset of the old reports — batched delivery, never a
             * different value. */
            if (r->num_queries_in_flight == 0) {
                /* Report spam with zero window slots: every queued report's
                 * value is fully determined by the running accumulator (the
                 * drain would add no slot results either) — serve instantly,
                 * RPCS3-style. Final bytes, just not deferred. */
                apply_reports_prefix(d, 0);
            } else {
                pgraph_vk_submit_batch(pg, VK_FINISH_REASON_QUERY_ASYNC);
                arm_report_marker(pg, r);
            }
        } else if (!r->report_marker_pending &&
                   !QSIMPLEQ_EMPTY(&r->report_queue)) {
            /* Age-out (RPCS3 300us rule): the pushbuffer never went dry but
             * the queue head has waited long enough. Covered heads (slots
             * already submitted, e.g. via a download's early submit) cost
             * only a marker; an uncovered head means its tail slots sit in
             * the open CB — submit it. Either way the wait happens on the
             * guest's side of the fence, never here. */
            QueryReport *head = QSIMPLEQ_FIRST(&r->report_queue);
            if (g_get_monotonic_time() - head->enqueue_us > REPORT_AGEOUT_US) {
                if ((uint32_t)head->query_count > r->queries_submitted) {
                    assert(r->in_command_buffer);
                    pgraph_vk_submit_batch(pg, VK_FINISH_REASON_QUERY_ASYNC);
                    arm_report_marker(pg, r);
                } else if (r->queries_submitted > 0) {
                    /* Covered by in-flight submissions: fence-poll only. */
                    arm_report_marker(pg, r);
                } else {
                    /* Nothing submitted and head needs no slots: serve from
                     * the accumulator. */
                    apply_reports_prefix(d, 0);
                }
                nv2a_profile_inc_counter(NV2A_PROF_REPORT_AGEOUT_SUBMIT);
            }
        }
        return;
    }

    if (*dma_get == *dma_put && r->in_command_buffer &&
        !QSIMPLEQ_EMPTY(&r->report_queue)) {
        if (r->query_ring_enabled) {
            /* Deferred-epoch ring: serve from a constant-lag retired epoch,
             * never draining this epoch mid-frame. Supersedes the async resolve
             * below (which still waits its own submission's GPU work — the
             * stall the ring removes). See serve_reports_ring. */
            serve_reports_ring(d);
        } else if (r->async_queries_enabled) {
            /* The pushbuffer drained (dma_get == dma_put) with occlusion
             * reports pending. Rather than a full pipeline drain, submit the
             * open batch WITHOUT waiting (VK_FINISH_REASON_QUERY_ASYNC ticks
             * NV2A_PROF_QUERY_ASYNC_RESOLVE, not FINISH_STALLED) so the GPU runs
             * its queries, then let process_pending_reports_internal read
             * exactly this epoch's range [0, num_queries_in_flight) with
             * WAIT_BIT: the CPU blocks only until the GPU reaches those queries,
             * not until every in-flight batch retires, and result application
             * (zpass accumulation + guest report writes) is byte-identical to
             * the finish path. submit_batch left recycle_pending set, so the
             * descriptor / framebuffer / staging / compute-descriptor recycle
             * defers to the next real finish (each pool self-heals with its own
             * NEED_BUFFER_SPACE finish if it fills first). The FIFO thread owns
             * every queue/fence op here, exactly as pgraph_vk_finish did. */
            pgraph_vk_submit_batch(pg, VK_FINISH_REASON_QUERY_ASYNC);
            /* SPEC-CORRECT ORDERING (the real flicker root cause): query
             * availability is a LATCH. WAIT_BIT is satisfied instantly by a
             * slot still latched available from its PREVIOUS epoch if this
             * epoch's vkCmdResetQueryPool has not yet executed on the queue
             * ("a stale value could be returned from a previous use of the
             * query" — Vulkan spec, vkGetQueryPoolResults). The only host
             * ordering primitive is a fence: wait the query-carrying
             * submission we just made, THEN read. This still skips the
             * recycle phase and never waits older unrelated slots, but the
             * epoch's own GPU work must finish before its counts exist —
             * that stall is inherent to same-epoch resolution. */
            pgraph_vk_wait_for_submission(r, r->submit_count - 1);
            pgraph_vk_process_pending_reports_internal(d);
        } else {
            pgraph_vk_finish(pg, VK_FINISH_REASON_STALLED);
        }
    }
}
