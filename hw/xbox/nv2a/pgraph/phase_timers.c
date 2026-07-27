/*
 * PenguinBox NV2A frame-phase wall-clock decomposition (Phase 0)
 *
 * Copyright (c) 2026 Patrick Carey
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
 *
 * See phase_timers.h for the model, the output-line format, the clock
 * choice, and the concurrency argument.
 */

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "qemu/atomic.h"
#include "phase_timers.h"

/* -1 = not yet probed, 0 = off, 1 = on. See nv2a_phase_timers_on(). */
int nv2a_phase_timers_state = -1;

/*
 * All phase state is private to this translation unit — no struct escapes
 * into a shared header, so nothing here can collide with debug.h's
 * NV2AStats / XMAC list or renderer.h.
 *
 * THREAD MAP (which thread writes which field):
 *   vcpu_blocked_ns, vcpu_dl_ns, vcpu_dl_count  -- vCPU (TCG) thread only,
 *       in steady state. Append-only running totals; never reset here. Read
 *       (via qatomic_*_i64) once per window by the pfifo thread. The only
 *       theoretical extra writers are (a) nv2a_lock_fifo during a device
 *       reset [not steady state] and (b) pgraph_vk_wait_for_surface_download
 *       on the display thread in the non-HAVE_EXTERNAL_MEMORY build [not
 *       compiled on the rig]; both are racy-by-a-frame-acceptable.
 *   pfifo_idle_ns, dl_proc_ns, present_ns       -- pfifo thread only.
 *   pf_leaf_ns[], pf_method_ns, pf_child_ns,     -- pfifo thread only. The
 *       pf_leaf_depth, pf_pusher_child_snap          pfifo_work sub-bucket
 *       decomposition. Every writer (nv2a_phase_leaf_open/_close,
 *       nv2a_phase_method_begin/_end) runs on the pfifo thread: all
 *       pgraph_vk_finish callers, the draw-time texture/surface ops, and the
 *       query drain are pfifo-thread. Plain int64, no atomics, like the other
 *       pfifo-private totals. (The only non-steady-state exceptions are the
 *       savevm/reset finish paths, which are racy-by-a-frame-acceptable, same
 *       caveat as the trio above.)
 *   pf_msub_ns[], pf_psub_ns[], and the cursor    -- pfifo thread only. The
 *       transients (pf_msub_active/_cur/_last_ns/     second-level `pfifo2:`
 *       _child_snap)                                  decomposition. Writers
 *       (nv2a_phase_msub_open/_close/_mark, nv2a_phase_add_psub) are the draw
 *       dispatch (flush_draw + its begin_pre_draw/create_pipeline/begin_draw
 *       callees) and the flip/sync present path — all pfifo-thread. Plain
 *       int64, no atomics, same discipline as pf_leaf_ns.
 *   last_flip_ns + all win_* + all snap_*        -- pfifo thread only
 *       (touched exclusively inside nv2a_phase_flip / its callees).
 *
 * Consequence: the cross-thread fields are single-writer / single-reader
 * with the reader only diffing monotonic totals, so no lock is needed and
 * no reset can be torn. Plain int64 accessed via qatomic_*_i64 for the
 * cross-thread trio (tear-free on 32-bit hosts); plain access for the
 * pfifo-private fields (never seen by another thread).
 */
static struct PhaseTimers {
    /* Cross-thread running totals (vCPU writes, pfifo reads). */
    int64_t vcpu_blocked_ns; /* all bracketed vCPU blocking waits          */
    int64_t vcpu_dl_ns;      /* surface-download round-trip subset          */
    int64_t vcpu_dl_count;   /* number of surface-download round-trips      */

    /* pfifo-thread running totals. */
    int64_t pfifo_idle_ns;   /* parked on fifo_cond                          */
    int64_t dl_proc_ns;      /* pfifo-side download processing               */
    int64_t present_ns;      /* flip finish (GPU drain) + VR submit          */

    /* pfifo_work sub-bucket decomposition (pfifo thread only). Running totals
     * of disjoint operation spans; see phase_timers.h SUB-BUCKET section. */
    int64_t pf_leaf_ns[PF_LEAF__COUNT]; /* texhash/upload/download/submit/    */
                                        /* queries/present outermost spans    */
    int64_t pf_method_ns;    /* pushbuffer dispatch SELF time (pusher-child)  */
    int64_t pf_child_ns;     /* running sum of outermost-leaf spans (for the  */
                             /* method self-time diff); pfifo thread only     */

    /* Second-level decomposition (pfifo thread only). pf_msub_ns cracks
     * `method` into per-draw Vulkan-recording categories; pf_psub_ns cracks
     * `present` into composite/gpuwait/vrsubmit thirds. See phase_timers.h
     * METHOD/PRESENT SUB2 section. Running totals of disjoint spans. */
    int64_t pf_msub_ns[PF_MSUB__COUNT]; /* const/attr/pipe/desc/draw          */
    int64_t pf_psub_ns[PF_PSUB__COUNT]; /* gpuwait/composite/vrsubmit         */

    /* Transient leaf-nesting state (pfifo thread only, not reported). */
    int      pf_leaf_depth;        /* current leaf nesting depth              */
    int64_t  pf_pusher_child_snap; /* pf_child_ns at the last method_begin    */

    /* Transient per-draw method-category cursor (pfifo thread only). */
    int      pf_msub_active;    /* 1 between msub_open and msub_close          */
    int      pf_msub_cur;       /* category currently being charged            */
    int64_t  pf_msub_last_ns;   /* clock at the last cursor event              */
    int64_t  pf_msub_child_snap;/* pf_child_ns at the last cursor event        */

    /* Flip / window state (pfifo thread only). */
    int64_t last_flip_ns;    /* 0 until the first flip is seen              */

    /* Snapshot of the running totals at the start of the current window. */
    int64_t snap_blocked_ns;
    int64_t snap_dl_ns;
    int64_t snap_dl_count;
    int64_t snap_pfifo_idle_ns;
    int64_t snap_dl_proc_ns;
    int64_t snap_present_ns;
    int64_t snap_pf_leaf_ns[PF_LEAF__COUNT];
    int64_t snap_pf_method_ns;
    int64_t snap_pf_msub_ns[PF_MSUB__COUNT];
    int64_t snap_pf_psub_ns[PF_PSUB__COUNT];

    /* Per-window histogram of frame wall time. */
    unsigned win_frames;
    int64_t  win_wall_ns;    /* sum of per-frame wall over the window       */
    int64_t  win_min_ns;
    int64_t  win_max_ns;
    unsigned win_light;
    unsigned win_heavy;
} s;

#define PHASE_HEAVY_NS ((int64_t)PHASE_HEAVY_MS * 1000000)

bool nv2a_phase_timers_probe(void)
{
    /* getenv once. A first-call race between threads is benign: getenv is a
     * pure read here (nothing calls setenv), so both racers compute and
     * store the same value. */
    int e = getenv("XEMU_PHASE_TIMERS") ? 1 : 0;
    qatomic_set(&nv2a_phase_timers_state, e);
    if (e) {
        fprintf(stderr, "phase: XEMU_PHASE_TIMERS on — per-60-flip frame-phase "
                        "decomposition enabled\n");
    }
    return e == 1;
}

int64_t nv2a_phase_now_ns(void)
{
    return qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
}

/* --- vCPU-thread blocked-time accumulators ----------------------------- */

void nv2a_phase_add_dl_wait(int64_t start_ns)
{
    if (!nv2a_phase_timers_on()) {
        return;
    }
    int64_t d = nv2a_phase_now_ns() - start_ns;

    qatomic_set_i64(&s.vcpu_dl_ns, qatomic_read_i64(&s.vcpu_dl_ns) + d);
    qatomic_set_i64(&s.vcpu_dl_count, qatomic_read_i64(&s.vcpu_dl_count) + 1);
    qatomic_set_i64(&s.vcpu_blocked_ns,
                    qatomic_read_i64(&s.vcpu_blocked_ns) + d);
}

void nv2a_phase_add_fifo_wait(int64_t start_ns)
{
    if (!nv2a_phase_timers_on()) {
        return;
    }
    int64_t d = nv2a_phase_now_ns() - start_ns;

    qatomic_set_i64(&s.vcpu_blocked_ns,
                    qatomic_read_i64(&s.vcpu_blocked_ns) + d);
}

/* --- pfifo-thread accumulators (single writer, plain int64) ------------ */

void nv2a_phase_add_pfifo_idle(int64_t start_ns)
{
    if (!nv2a_phase_timers_on()) {
        return;
    }
    s.pfifo_idle_ns += nv2a_phase_now_ns() - start_ns;
}

void nv2a_phase_add_dl_proc(int64_t start_ns)
{
    if (!nv2a_phase_timers_on()) {
        return;
    }
    s.dl_proc_ns += nv2a_phase_now_ns() - start_ns;
}

void nv2a_phase_add_present(int64_t start_ns)
{
    if (!nv2a_phase_timers_on()) {
        return;
    }
    s.present_ns += nv2a_phase_now_ns() - start_ns;
}

/* --- pfifo_work sub-bucket decomposition (pfifo thread) ---------------- */

/*
 * Depth-guarded leaf timer. open() pushes the pfifo leaf-nesting depth and
 * takes the start clock; close() pops it and, ONLY when this leaf was the
 * outermost (depth returns to 0), folds its span into its bucket and into the
 * child accumulator. Nested GPU round-trips (a SURFACE_DOWN finish inside a
 * download, a STALLED finish inside the query drain, ...) are thereby absorbed
 * by the enclosing leaf and never double-counted, which keeps every bucket a
 * disjoint sub-interval of pfifo_work. pfifo thread only; plain state.
 */
NV2APhaseLeaf nv2a_phase_leaf_open(int bucket)
{
    NV2APhaseLeaf l = { .bucket = bucket, .active = 0, .start_ns = 0 };
    if (!nv2a_phase_timers_on()) {
        return l;
    }
    s.pf_leaf_depth++;
    l.active = 1;
    l.start_ns = nv2a_phase_now_ns();
    return l;
}

void nv2a_phase_leaf_close(NV2APhaseLeaf *l)
{
    if (!l->active) {
        return;
    }
    int64_t d = nv2a_phase_now_ns() - l->start_ns;
    /* Only the outermost leaf on the stack records; nested leaves are absorbed
     * by it (their time is already inside this span). */
    if (--s.pf_leaf_depth == 0) {
        s.pf_leaf_ns[l->bucket] += d;
        s.pf_child_ns += d;
    }
}

/*
 * Pushbuffer/pgraph_method dispatch SELF-time. method_begin snapshots the
 * child accumulator; method_end attributes (span - child_delta) to method, so
 * a draw's texhash/upload/submit and a mid-pusher FLIP present — all counted
 * as leaves during the span — are excluded, leaving pure dispatch cost. Not
 * reentrant (pfifo_run_pusher is called once per loop iteration).
 */
int64_t nv2a_phase_method_begin(void)
{
    if (!nv2a_phase_timers_on()) {
        return 0;
    }
    s.pf_pusher_child_snap = s.pf_child_ns;
    return nv2a_phase_now_ns();
}

void nv2a_phase_method_end(int64_t start_ns)
{
    if (!nv2a_phase_timers_on()) {
        return;
    }
    int64_t span = nv2a_phase_now_ns() - start_ns;
    int64_t child = s.pf_child_ns - s.pf_pusher_child_snap;
    int64_t self = span - child;
    if (self < 0) {
        /* Clock jitter across the snapshot boundary only; never structural
         * (children are strictly nested sub-intervals of the span). */
        self = 0;
    }
    s.pf_method_ns += self;
}

/* --- method second-level: per-draw category cursor (pfifo thread) ------ */

/*
 * Charge the running cursor segment to its current category and re-baseline.
 * The charge is (elapsed) minus (pf_child_ns accrued in the segment), so a
 * mid-draw pgraph_vk_finish — a top-level SUBMIT leaf that lands in pf_child_ns
 * — is absorbed exactly as method_end absorbs it, keeping sum(msub) <= method.
 * `now` is passed in so open/mark/close share one clock read per event.
 */
static void msub_charge(int64_t now)
{
    int64_t span = now - s.pf_msub_last_ns;
    int64_t child = s.pf_child_ns - s.pf_msub_child_snap;
    int64_t self = span - child;
    if (self < 0) {
        self = 0; /* jitter / a finish straddling the event boundary */
    }
    s.pf_msub_ns[s.pf_msub_cur] += self;
    s.pf_msub_last_ns = now;
    s.pf_msub_child_snap = s.pf_child_ns;
}

NV2APhaseMsub nv2a_phase_msub_open(int cat)
{
    NV2APhaseMsub m = { .active = 0 };
    if (!nv2a_phase_timers_on()) {
        return m;
    }
    /* Non-reentrant: a draw is never nested in a draw. Just (re)baseline. */
    s.pf_msub_active = 1;
    s.pf_msub_cur = cat;
    s.pf_msub_last_ns = nv2a_phase_now_ns();
    s.pf_msub_child_snap = s.pf_child_ns;
    m.active = 1;
    return m;
}

void nv2a_phase_msub_close(NV2APhaseMsub *m)
{
    if (!m->active) {
        return;
    }
    msub_charge(nv2a_phase_now_ns());
    s.pf_msub_active = 0;
}

void nv2a_phase_msub_mark(int cat)
{
    if (!nv2a_phase_timers_on() || !s.pf_msub_active) {
        /* Inert unless a draw opened the cursor: the clear path reuses
         * begin_pre_draw/begin_draw without a scope, and its marks are dropped
         * (clears fall into method_resid). */
        return;
    }
    msub_charge(nv2a_phase_now_ns());
    s.pf_msub_cur = cat;
}

/*
 * Redirect the active cursor to `cat`, remembering the current category in the
 * returned token so close() restores it. Inert unless a draw cursor is active
 * (a single-time command outside a draw must not fabricate method_synctex).
 * Nesting-safe: each token restores its own predecessor.
 */
NV2APhaseMsub nv2a_phase_msub_redirect_open(int cat)
{
    NV2APhaseMsub m = { .active = 0, .saved = 0 };
    if (!nv2a_phase_timers_on() || !s.pf_msub_active) {
        return m;
    }
    msub_charge(nv2a_phase_now_ns());
    m.saved = s.pf_msub_cur;
    m.active = 1;
    s.pf_msub_cur = cat;
    return m;
}

void nv2a_phase_msub_redirect_close(NV2APhaseMsub *m)
{
    if (!m->active) {
        return;
    }
    msub_charge(nv2a_phase_now_ns());
    s.pf_msub_cur = m->saved;
}

/* --- present second-level: composite / gpuwait / vrsubmit (pfifo thread) - */

void nv2a_phase_add_psub(int which, int64_t start_ns)
{
    if (!nv2a_phase_timers_on()) {
        return;
    }
    s.pf_psub_ns[which] += nv2a_phase_now_ns() - start_ns;
}

/* --- flip anchor + window reporting (pfifo thread) --------------------- */

static void phase_snapshot(void)
{
    /* Baseline the window against the current running totals. The vCPU trio
     * is read atomically; the pfifo-private totals are plain reads on the
     * pfifo thread. */
    s.snap_blocked_ns = qatomic_read_i64(&s.vcpu_blocked_ns);
    s.snap_dl_ns = qatomic_read_i64(&s.vcpu_dl_ns);
    s.snap_dl_count = qatomic_read_i64(&s.vcpu_dl_count);
    s.snap_pfifo_idle_ns = s.pfifo_idle_ns;
    s.snap_dl_proc_ns = s.dl_proc_ns;
    s.snap_present_ns = s.present_ns;
    for (int i = 0; i < PF_LEAF__COUNT; i++) {
        s.snap_pf_leaf_ns[i] = s.pf_leaf_ns[i];
    }
    s.snap_pf_method_ns = s.pf_method_ns;
    for (int i = 0; i < PF_MSUB__COUNT; i++) {
        s.snap_pf_msub_ns[i] = s.pf_msub_ns[i];
    }
    for (int i = 0; i < PF_PSUB__COUNT; i++) {
        s.snap_pf_psub_ns[i] = s.pf_psub_ns[i];
    }

    s.win_frames = 0;
    s.win_wall_ns = 0;
    s.win_min_ns = INT64_MAX;
    s.win_max_ns = 0;
    s.win_light = 0;
    s.win_heavy = 0;
}

static double ns_to_ms_mean(int64_t total_ns, unsigned frames)
{
    if (!frames) {
        return 0.0;
    }
    return (double)total_ns / (double)frames / 1e6;
}

static double clamp_nonneg(double v)
{
    /* run and pfifo_work are (wall - subset). A wait that straddles the
     * 60-flip snapshot boundary can shift a fraction of a millisecond and
     * make the mean marginally negative; clamp for display sanity. */
    return v < 0.0 ? 0.0 : v;
}

static void phase_emit(void)
{
    unsigned n = s.win_frames;

    int64_t blocked_d = qatomic_read_i64(&s.vcpu_blocked_ns) - s.snap_blocked_ns;
    int64_t dl_d = qatomic_read_i64(&s.vcpu_dl_ns) - s.snap_dl_ns;
    int64_t dlc_d = qatomic_read_i64(&s.vcpu_dl_count) - s.snap_dl_count;
    int64_t idle_d = s.pfifo_idle_ns - s.snap_pfifo_idle_ns;
    int64_t proc_d = s.dl_proc_ns - s.snap_dl_proc_ns;
    int64_t present_d = s.present_ns - s.snap_present_ns;

    double mspf = ns_to_ms_mean(s.win_wall_ns, n);
    double min_ms = (double)s.win_min_ns / 1e6;
    double max_ms = (double)s.win_max_ns / 1e6;
    double blocked = ns_to_ms_mean(blocked_d, n);
    double run = clamp_nonneg(mspf - blocked);
    double dl = ns_to_ms_mean(dl_d, n);
    double dl_n = (double)dlc_d / (double)n;
    double pfifo_idle = ns_to_ms_mean(idle_d, n);
    double pfifo_work = clamp_nonneg(mspf - pfifo_idle);
    double dl_proc = ns_to_ms_mean(proc_d, n);
    double present = ns_to_ms_mean(present_d, n);

    fprintf(stderr,
            "phase: mspf=%.1f[%.1f-%.1f] run=%.1f blocked=%.1f "
            "dl_stall=%.1f(%.1fx) dl_proc=%.1f pfifo_work=%.1f "
            "pfifo_idle=%.1f present=%.1f light=%u heavy=%u\n",
            mspf, min_ms, max_ms, run, blocked, dl, dl_n, dl_proc,
            pfifo_work, pfifo_idle, present, s.win_light, s.win_heavy);

    /* pfifo_work sub-bucket decomposition. Every bucket is a disjoint leaf (or
     * the pusher self-time), so their sum <= pfifo_work and residual >= 0. */
    double method = ns_to_ms_mean(s.pf_method_ns - s.snap_pf_method_ns, n);
    double texhash = ns_to_ms_mean(
        s.pf_leaf_ns[PF_LEAF_TEXHASH] - s.snap_pf_leaf_ns[PF_LEAF_TEXHASH], n);
    double upload = ns_to_ms_mean(
        s.pf_leaf_ns[PF_LEAF_UPLOAD] - s.snap_pf_leaf_ns[PF_LEAF_UPLOAD], n);
    double download = ns_to_ms_mean(
        s.pf_leaf_ns[PF_LEAF_DOWNLOAD] - s.snap_pf_leaf_ns[PF_LEAF_DOWNLOAD], n);
    double submit = ns_to_ms_mean(
        s.pf_leaf_ns[PF_LEAF_SUBMIT] - s.snap_pf_leaf_ns[PF_LEAF_SUBMIT], n);
    double queries = ns_to_ms_mean(
        s.pf_leaf_ns[PF_LEAF_QUERIES] - s.snap_pf_leaf_ns[PF_LEAF_QUERIES], n);
    double pf_present = ns_to_ms_mean(
        s.pf_leaf_ns[PF_LEAF_PRESENT] - s.snap_pf_leaf_ns[PF_LEAF_PRESENT], n);
    double residual = clamp_nonneg(pfifo_work - method - texhash - upload -
                                   download - submit - queries - pf_present);

    fprintf(stderr,
            "pfifo: work=%.1f method=%.1f texhash=%.1f queries=%.1f "
            "download=%.1f upload=%.1f submit=%.1f present=%.1f residual=%.1f\n",
            pfifo_work, method, texhash, queries, download, upload, submit,
            pf_present, residual);

    /* pfifo2: second-level decomposition of the two dominant buckets. The
     * method sub-categories are disjoint cursor segments (each already net of
     * its leaf children) so their sum <= method; method_resid is the inter-draw
     * dispatch self-time plus un-bracketed glue (e.g. clears). The present
     * thirds sum to `present` (== pf_present above); present_resid is the tiny
     * leaf/scope boundary slack. */
    double m_const = ns_to_ms_mean(
        s.pf_msub_ns[PF_MSUB_CONST] - s.snap_pf_msub_ns[PF_MSUB_CONST], n);
    double m_attr = ns_to_ms_mean(
        s.pf_msub_ns[PF_MSUB_ATTR] - s.snap_pf_msub_ns[PF_MSUB_ATTR], n);
    double m_pipe = ns_to_ms_mean(
        s.pf_msub_ns[PF_MSUB_PIPE] - s.snap_pf_msub_ns[PF_MSUB_PIPE], n);
    double m_desc = ns_to_ms_mean(
        s.pf_msub_ns[PF_MSUB_DESC] - s.snap_pf_msub_ns[PF_MSUB_DESC], n);
    double m_draw = ns_to_ms_mean(
        s.pf_msub_ns[PF_MSUB_DRAW] - s.snap_pf_msub_ns[PF_MSUB_DRAW], n);
    double m_synctex = ns_to_ms_mean(
        s.pf_msub_ns[PF_MSUB_SYNCTEX] - s.snap_pf_msub_ns[PF_MSUB_SYNCTEX], n);
    double m_rpchurn = ns_to_ms_mean(
        s.pf_msub_ns[PF_MSUB_RPCHURN] - s.snap_pf_msub_ns[PF_MSUB_RPCHURN], n);
    double m_resid = clamp_nonneg(method - m_const - m_attr - m_pipe - m_desc -
                                  m_draw - m_synctex - m_rpchurn);

    double p_gpuwait = ns_to_ms_mean(
        s.pf_psub_ns[PF_PSUB_GPUWAIT] - s.snap_pf_psub_ns[PF_PSUB_GPUWAIT], n);
    double p_composite = ns_to_ms_mean(
        s.pf_psub_ns[PF_PSUB_COMPOSITE] - s.snap_pf_psub_ns[PF_PSUB_COMPOSITE],
        n);
    double p_vrsubmit = ns_to_ms_mean(
        s.pf_psub_ns[PF_PSUB_VRSUBMIT] - s.snap_pf_psub_ns[PF_PSUB_VRSUBMIT], n);
    double p_resid =
        clamp_nonneg(pf_present - p_gpuwait - p_composite - p_vrsubmit);

    fprintf(stderr,
            "pfifo2: method_synctex=%.1f method_const=%.1f method_attr=%.1f "
            "method_pipe=%.1f method_desc=%.1f method_draw=%.1f "
            "method_rpchurn=%.1f method_resid=%.1f present_gpuwait=%.1f "
            "present_composite=%.1f present_vrsubmit=%.1f present_resid=%.1f\n",
            m_synctex, m_const, m_attr, m_pipe, m_desc, m_draw, m_rpchurn,
            m_resid, p_gpuwait, p_composite, p_vrsubmit, p_resid);
}

void nv2a_phase_flip(void)
{
    if (!nv2a_phase_timers_on()) {
        return;
    }

    int64_t now = nv2a_phase_now_ns();

    if (s.last_flip_ns == 0) {
        /* First flip ever: establish the wall baseline and window snapshot,
         * but do not score a frame (there is no previous flip to close). */
        s.last_flip_ns = now;
        phase_snapshot();
        return;
    }

    int64_t wall = now - s.last_flip_ns;
    s.last_flip_ns = now;

    s.win_frames++;
    s.win_wall_ns += wall;
    if (wall < s.win_min_ns) {
        s.win_min_ns = wall;
    }
    if (wall > s.win_max_ns) {
        s.win_max_ns = wall;
    }
    if (wall >= PHASE_HEAVY_NS) {
        s.win_heavy++;
    } else {
        s.win_light++;
    }

    if (s.win_frames >= PHASE_WINDOW) {
        phase_emit();
        phase_snapshot(); /* re-baseline for the next window */
    }
}
