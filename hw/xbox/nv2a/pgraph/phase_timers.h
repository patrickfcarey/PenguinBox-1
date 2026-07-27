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
 * ------------------------------------------------------------------------
 * WHAT THIS IS
 * ------------------------------------------------------------------------
 * A frame-phase profiler that decomposes each frame's wall time into
 * buckets so the guest-vs-pfifo-vs-blocked split is measured, not guessed
 * (docs/perf-roadmap-30-to-60.md Phase 0). Gated entirely by the
 * environment variable XEMU_PHASE_TIMERS=1 (probed once). When OFF, every
 * hook is a single cached-int read plus a predicted branch — no clock
 * calls, no accumulation, no output.
 *
 * Every 60 flips it prints THREE compact stderr lines, e.g.
 *
 *   phase: mspf=33.1[31.0-51.2] run=27.4 blocked=5.7 dl_stall=5.7(4.0x) \
 *          pfifo_work=8.9 pfifo_idle=24.2 present=6.1 light=57 heavy=3
 *   pfifo: work=31.0 method=18.4 texhash=4.1 queries=3.2 download=2.0 \
 *          upload=0.6 submit=1.9 present=0.5 residual=0.3
 *   pfifo2: method_synctex=7.4 method_const=2.6 method_attr=2.1 method_pipe=1.0 \
 *           method_desc=0.7 method_draw=1.1 method_rpchurn=0.4 \
 *           method_resid=1.1 present_gpuwait=5.3 present_composite=2.2 \
 *           present_vrsubmit=0.0 present_resid=0.5
 *
 * The `phase:` line is the guest-vs-pfifo-vs-blocked split (unchanged). The
 * `pfifo:` line decomposes pfifo_work (== the frame) into disjoint per-
 * operation buckets: method dispatch, texture hashing, occlusion-query drain,
 * surface GPU<->RAM download/upload, mid-pushbuffer GPU submits, present, and
 * an always-non-negative residual (see the SUB-BUCKET section below). The
 * `pfifo2:` line is the SECOND-LEVEL decomposition (this file's extension): it
 * cracks open the two dominant `pfifo:` buckets — `method` (the ~2037-draw
 * pushbuffer dispatch) into per-draw Vulkan-recording categories, and `present`
 * into its composite / GPU-drain / VR-submit thirds (see the METHOD/PRESENT
 * SUB2 section below). Each group carries its own always-non-negative residual.
 * All three lines are per-frame MEANS in ms over the just-completed 60-flip
 * window.
 *
 * All `phase:` bucket figures are per-frame MEANS in milliseconds over the
 * just-completed 60-flip window; mspf is mean[min-max]; the (Nx) after
 * dl_stall is the mean number of blocking surface-download round-trips per
 * frame; light/heavy are frame COUNTS in the window (threshold PHASE_HEAVY_MS).
 *
 *   run        = mspf - blocked  (guest vCPU actually executing)
 *   blocked    = all bracketed vCPU blocking waits (superset of dl_stall)
 *   dl_stall   = the surface-download round-trip subset + count
 *   pfifo_work = mspf - pfifo_idle  (pfifo thread doing real work)
 *   pfifo_idle = pfifo thread parked on its fifo_cond condvar
 *   present    = flip-time GPU drain (pgraph_vk_finish) + VR submit
 *
 * ------------------------------------------------------------------------
 * CLOCK
 * ------------------------------------------------------------------------
 * nv2a_phase_now_ns() == qemu_clock_get_ns(QEMU_CLOCK_REALTIME), which on
 * the (Linux/NVIDIA) rig bottoms out at clock_gettime(CLOCK_MONOTONIC) via
 * timer.h get_clock(). Monotonic, vDSO-cheap, thread-safe, ns resolution.
 * This is the same clock family the tree's own frame timing uses
 * (profile.c: qemu_clock_get_us(QEMU_CLOCK_REALTIME)).
 *
 * ------------------------------------------------------------------------
 * CONCURRENCY
 * ------------------------------------------------------------------------
 * Two threads write the stats. The vCPU (TCG) thread accumulates the
 * blocked-time totals (nv2a_phase_add_dl_wait / _add_fifo_wait); the pfifo
 * thread accumulates everything else AND owns the flip reconciliation
 * (nv2a_phase_flip). The design keeps the vCPU thread a pure APPEND-ONLY
 * writer of monotonic running totals it never resets; the pfifo thread only
 * READS those totals, once per 60-flip window, and diffs them against a
 * snapshot. There is no shared reset, so there is no torn-reset race.
 * 64-bit totals are read/written through qatomic_*_i64 so a 32-bit host
 * cannot observe a torn value. Being off by one frame's worth of a bucket
 * at a window boundary is acceptable for a profiler and is the only
 * residual imprecision. See phase_timers.c for the per-field thread map.
 *
 * The struct and all state live privately in phase_timers.c; this header
 * exposes only functions, so it cannot collide with debug.h's NV2AStats /
 * XMAC counter list or the renderer.h ownership.
 */

#ifndef HW_XBOX_NV2A_PGRAPH_PHASE_TIMERS_H
#define HW_XBOX_NV2A_PGRAPH_PHASE_TIMERS_H

#include <stdbool.h>
#include <stdint.h>

/* qatomic_read, used by the inline getenv-once gate below. Included here so the
 * header is self-contained: TUs that include phase_timers.h before their
 * nv2a_int.h (e.g. vk/draw.c, vk/texture.c) still resolve the atomic. Safe
 * after the mandatory qemu/osdep.h that every .c includes first. */
#include "qemu/atomic.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Frames per report window. */
#define PHASE_WINDOW 60

/* Light/heavy frame split threshold (ms of wall time). The 4-download tail
 * frames land ~47-51ms; light frames ~31-34ms. */
#define PHASE_HEAVY_MS 40

/*
 * getenv-once gate. nv2a_phase_timers_state: -1 = not yet probed, 0 = off,
 * 1 = on. Defined in phase_timers.c; probed lazily by the slow path below.
 * The inline fast path is a single relaxed atomic read plus a branch so the
 * OFF path never makes a function call, never reads a clock, and never
 * touches the accumulators.
 */
extern int nv2a_phase_timers_state;
bool nv2a_phase_timers_probe(void); /* slow path: getenv once, caches state */

static inline bool nv2a_phase_timers_on(void)
{
    int s = qatomic_read(&nv2a_phase_timers_state);
    if (s < 0) {
        return nv2a_phase_timers_probe();
    }
    return s == 1;
}

/* Monotonic timestamp in nanoseconds (CLOCK_MONOTONIC). Call ONLY behind an
 * nv2a_phase_timers_on() guard so the OFF path stays clock-free. */
int64_t nv2a_phase_now_ns(void);

/*
 * vCPU-thread blocked-time hooks. Pass the nv2a_phase_now_ns() timestamp
 * taken immediately before the blocking wait; the hook reads the clock again
 * and folds (now - start) into the running totals. Each self-gates on the
 * env flag before reading the end clock, so an accidental ungated call is
 * still clock-free when OFF.
 */
void nv2a_phase_add_dl_wait(int64_t start_ns);   /* surface-download round-trip */
void nv2a_phase_add_fifo_wait(int64_t start_ns); /* fifo-quiesce (MMIO) wait   */

/* pfifo-thread accumulators (same start/end convention). */
void nv2a_phase_add_pfifo_idle(int64_t start_ns); /* parked on fifo_cond      */
void nv2a_phase_add_dl_proc(int64_t start_ns);    /* download processing work */
void nv2a_phase_add_present(int64_t start_ns);    /* flip finish + VR submit  */

/*
 * ------------------------------------------------------------------------
 * pfifo_work SUB-BUCKET DECOMPOSITION (the `pfifo:` line)
 * ------------------------------------------------------------------------
 * pfifo_work (== wall - pfifo_idle) is the pfifo/GPU-emulation thread's real
 * per-frame work — the ~31ms that IS the Steel Battalion frame. These hooks
 * break it into disjoint operation buckets so the GPU-wall attack can be
 * prioritised (docs/perf-roadmap-30-to-60.md). ALL of these run on the pfifo
 * thread ONLY (every pgraph_vk_finish caller, every draw-time texture/surface
 * op, and the query drain are pfifo-thread); the accumulators are therefore
 * pfifo-private plain int64 — no atomics, exactly like pfifo_idle_ns above.
 *
 * The six leaf buckets are measured with a depth-guarded scope timer so that a
 * GPU round-trip (pgraph_vk_finish) nested INSIDE a compound op — the
 * SURFACE_DOWN finish inside a download, the SURFACE_CREATE finish inside an
 * upload, the STALLED finish inside the query drain, the FLIP_STALL/PRESENTING
 * finish inside present — is absorbed by the ENCLOSING leaf and never
 * double-counted. Only the OUTERMOST leaf on the pfifo thread's call stack
 * records its span; a top-level (un-nested) pgraph_vk_finish is thus exactly
 * the mid-pushbuffer mandatory flush cost = the `submit` bucket. Because the
 * leaves are disjoint sub-intervals of one thread, their sum <= pfifo_work and
 * residual = pfifo_work - sum(buckets) is >= 0 by construction.
 *
 * `method` is the pushbuffer/pgraph_method dispatch SELF time: the pusher span
 * minus whatever leaf children ran inside it (a draw's texhash/upload/submit,
 * a mid-pusher FLIP present). It is not a leaf; it is derived by the
 * method_begin/method_end pair which diffs the child accumulator.
 */
enum {
    PF_LEAF_TEXHASH = 0, /* fast_hash(texture_data,...) content hash          */
    PF_LEAF_UPLOAD,      /* pgraph_vk_upload_surface_data (RAM->GPU)          */
    PF_LEAF_DOWNLOAD,    /* download_surface_to_buffer   (GPU->RAM)           */
    PF_LEAF_SUBMIT,      /* top-level pgraph_vk_finish (mid-pusher GPU flush) */
    PF_LEAF_QUERIES,     /* process_pending_reports (occlusion + STALLED)     */
    PF_LEAF_PRESENT,     /* render_display + VR submit + flip-finish drain    */
    PF_LEAF__COUNT
};

/*
 * Depth-guarded leaf token. nv2a_phase_leaf_open() reads the clock and pushes
 * the pfifo leaf-nesting depth; nv2a_phase_leaf_close() (invoked automatically
 * via the cleanup attribute at scope exit) pops it and, only if this leaf was
 * the outermost, folds its span into bucket[bucket] and the child accumulator.
 * The token is OFF-inert: when the profiler is off, open() reads no clock and
 * pushes nothing, close() is a single predicted branch.
 */
typedef struct NV2APhaseLeaf {
    int bucket;
    int active;      /* 1 only when the profiler was on at open() time */
    int64_t start_ns;
} NV2APhaseLeaf;

NV2APhaseLeaf nv2a_phase_leaf_open(int bucket);
void nv2a_phase_leaf_close(NV2APhaseLeaf *l);

#define NV2A_PHASE_LEAF_CAT2(a, b) a##b
#define NV2A_PHASE_LEAF_CAT(a, b) NV2A_PHASE_LEAF_CAT2(a, b)
/*
 * Bracket the enclosing C scope as one leaf operation. Place at the top of the
 * op (after any no-work early-return guard). Auto-closes on every return via
 * the cleanup attribute (as g_autofree does), so multi-return functions are
 * safe. The inline nv2a_phase_timers_on() gate means the OFF path constructs
 * an inactive token WITHOUT calling nv2a_phase_leaf_open (no function call, no
 * clock); the cleanup then hits nv2a_phase_leaf_close's leading !active branch.
 */
#define NV2A_PHASE_LEAF(bucket_id)                                       \
    NV2APhaseLeaf NV2A_PHASE_LEAF_CAT(nv2a_phase_leaf_, __LINE__)        \
        __attribute__((cleanup(nv2a_phase_leaf_close))) =               \
        nv2a_phase_timers_on()                                          \
            ? nv2a_phase_leaf_open(bucket_id)                            \
            : (NV2APhaseLeaf){ .bucket = (bucket_id), .active = 0,       \
                               .start_ns = 0 }

/*
 * Pushbuffer/method dispatch SELF-time bracket (pfifo thread). method_begin()
 * snapshots the child accumulator and reads the clock; method_end() adds
 * (span - child_delta) to the method bucket, so only the dispatch work that is
 * NOT one of the leaf children above is attributed to method. Not reentrant
 * (pfifo_run_pusher is called once per pfifo loop iteration, never nested).
 */
int64_t nv2a_phase_method_begin(void);
void nv2a_phase_method_end(int64_t start_ns);

/*
 * ------------------------------------------------------------------------
 * METHOD / PRESENT SECOND-LEVEL DECOMPOSITION (the `pfifo2:` line)
 * ------------------------------------------------------------------------
 * The `pfifo:` line proved `method` (~16ms, the 2037-draw pushbuffer dispatch)
 * and `present` (~8ms, the flip/composite) are the wall. This second level
 * cracks each open so the hard-wall attack can pick a target. All hooks run on
 * the pfifo thread only (draw dispatch + flip are both pfifo-thread), so the
 * accumulators are pfifo-private plain int64 — same discipline as pf_leaf_ns.
 *
 * METHOD sub-categories (pf_msub_ns[]) — a per-draw category cursor.
 *   The six categories are disjoint sub-intervals of the per-draw work (five
 *   CPU-recording categories plus SYNCTEX, the blocking GPU wait). A single
 *   running cursor charges elapsed time to whichever category is current,
 *   switching at boundaries (nv2a_phase_msub_mark). To
 *   stay consistent with `method` self-time — which EXCLUDES leaf children
 *   (a mid-draw pgraph_vk_finish is a SUBMIT leaf, subtracted via pf_child_ns)
 *   — every charge also subtracts the pf_child_ns delta accrued in the segment,
 *   so a nested finish is absorbed exactly as method_end absorbs it. This makes
 *   sum(method sub) <= method and method_resid = method - sum(sub) >= 0 by
 *   construction (the residual is the inter-draw register-dispatch self-time
 *   plus any un-bracketed draw glue, e.g. clears). The cursor is a NO-OP unless
 *   a draw opened it (nv2a_phase_msub_open), so stray marks from the clear path
 *   — pgraph_vk_clear_surface reuses begin_pre_draw/begin_draw — are ignored.
 *
 * The bracket map (which code region feeds which category):
 *   CONST   — pgraph_vk_bind_shaders: the per-draw uniform recompute + content
 *             hash (update_shader_uniforms) and the SHADER_UBO_DIRTY staging
 *             append. The transform-constant churn.
 *   ATTR    — pgraph_vk_bind_vertex_attributes + sync/remap + the inline
 *             vertex/element/array staging buffer build.
 *   PIPE    — texture bind + pipeline LRU lookup/create + render-pass/framebuffer
 *             + begin_draw's command binds (pipeline/descriptor/push commands).
 *   DESC    — pgraph_vk_update_descriptor_sets (vkUpdateDescriptorSets + the
 *             descriptor-set management around it).
 *   DRAW    — vkCmdDraw/vkCmdDrawIndexed + the vertex-buffer bind + end_draw.
 *   SYNCTEX — the BLOCKING vkWaitForFences in pgraph_vk_end_single_time_commands
 *             (command.c): the single-time-command GPU round-trip that
 *             copy_surface_to_texture (SURF_TO_TEX / QUEUE_SUBMIT_AUX, ~28/frame)
 *             and draw-time texture uploads perform DURING texture bind — inside
 *             the pusher, so it lands in `method`. This is the GPU wait we could
 *             DEFER (overlap with guest work), split out from real CPU recording.
 *             Redirected out of whatever category (usually PIPE) it interrupts;
 *             single-time commands outside a draw are ignored (cursor inactive).
 *
 * method_resid = method - sum(the six) is the inter-draw register-dispatch
 * self-time plus un-bracketed glue (clears). The DEFERRABLE-vs-CPU split the
 * hard-wall attack needs is: SYNCTEX (deferrable GPU wait) vs
 * CONST+ATTR+PIPE+DESC+DRAW+resid (genuine CPU recording).
 */
enum {
    PF_MSUB_CONST = 0, /* uniform/UBO recompute+hash+append (bind_shaders)     */
    PF_MSUB_ATTR,      /* vertex-attr bind + inline staging buffer build       */
    PF_MSUB_PIPE,      /* pipeline lookup/create + render pass + cmd binds     */
    PF_MSUB_DESC,      /* descriptor-set update (vkUpdateDescriptorSets)       */
    PF_MSUB_DRAW,      /* vkCmdDraw[Indexed] + vertex-buffer bind + end_draw   */
    PF_MSUB_SYNCTEX,   /* BLOCKING single-time vkWaitForFences (surf->tex etc) */
    PF_MSUB_RPCHURN,   /* query-path render-pass churn (present-wall S0): the
                        * end_render_pass + end/begin_query bookkeeping in
                        * begin_draw's visibility block, plus the
                        * begin_render_pass re-open it forced — carved out of
                        * PIPE via redirects so the pfifo2 line shows how much
                        * of method_pipe is query-driven pass churn (Lane 2
                        * Attack B's target, ~205 passes/frame). */
    PF_MSUB__COUNT
};

/*
 * Per-draw method category cursor (pfifo thread only, non-reentrant — a draw is
 * never nested inside another draw). open() activates the cursor at category
 * `cat`, snapshotting the clock and pf_child_ns; mark() charges the running
 * segment to the current category (elapsed minus pf_child_ns delta) then
 * switches; close() charges the final segment and deactivates. mark() is inert
 * when the cursor is not active. All self-gate on the env flag; the macros below
 * keep the OFF path a single cached-atomic read with no function call, mirroring
 * NV2A_PHASE_LEAF.
 */
typedef struct NV2APhaseMsub {
    int active; /* 1 only when the profiler was on (and, for a redirect, a draw
                 * cursor was active) at open() time */
    int saved;  /* redirect tokens only: the category to restore at close */
} NV2APhaseMsub;

NV2APhaseMsub nv2a_phase_msub_open(int cat);
void nv2a_phase_msub_close(NV2APhaseMsub *m);
void nv2a_phase_msub_mark(int cat);

/*
 * Temporarily redirect the ACTIVE draw cursor to category `cat` for the
 * enclosing scope, restoring the previous category on exit. Used to carve a
 * nested span (the SYNCTEX blocking wait) out of the category it interrupts,
 * without a hardcoded "restore to PIPE" (the token remembers the predecessor,
 * so redirects nest correctly). Inert unless a draw opened the cursor, so a
 * single-time command outside any draw is ignored.
 */
NV2APhaseMsub nv2a_phase_msub_redirect_open(int cat);
void nv2a_phase_msub_redirect_close(NV2APhaseMsub *m);

/*
 * Open the per-draw category cursor for the enclosing C scope, auto-closing on
 * every return via the cleanup attribute (as NV2A_PHASE_LEAF does). Place at the
 * top of the drawing region (after the no-binding early-return). OFF path builds
 * an inactive token without calling open() — no function call, no clock.
 */
#define NV2A_PHASE_MSUB_SCOPE(cat)                                       \
    NV2APhaseMsub NV2A_PHASE_LEAF_CAT(nv2a_phase_msub_, __LINE__)        \
        __attribute__((cleanup(nv2a_phase_msub_close))) =               \
        nv2a_phase_timers_on()                                          \
            ? nv2a_phase_msub_open(cat)                                  \
            : (NV2APhaseMsub){ .active = 0 }

/* Switch the cursor to category `cat`. OFF path is a single predicted branch. */
#define NV2A_PHASE_MSUB(cat)                                              \
    do {                                                                 \
        if (nv2a_phase_timers_on()) {                                    \
            nv2a_phase_msub_mark(cat);                                   \
        }                                                                \
    } while (0)

/*
 * Redirect the active draw cursor to `cat` for the enclosing C scope (e.g. wrap
 * the SYNCTEX blocking wait). Auto-restores on every return. OFF path builds an
 * inactive token without a call, like NV2A_PHASE_MSUB_SCOPE.
 */
#define NV2A_PHASE_MSUB_REDIRECT(cat)                                    \
    NV2APhaseMsub NV2A_PHASE_LEAF_CAT(nv2a_phase_msub_rd_, __LINE__)     \
        __attribute__((cleanup(nv2a_phase_msub_redirect_close))) =      \
        nv2a_phase_timers_on()                                          \
            ? nv2a_phase_msub_redirect_open(cat)                         \
            : (NV2APhaseMsub){ .active = 0, .saved = 0 }

/*
 * PRESENT sub-thirds (pf_psub_ns[]). present (the PF_LEAF_PRESENT total) is the
 * display composite + the flip-time GPU drain + the VR submit; these three
 * disjoint spans sum to it. Each is a plain clock bracket at the flip/sync site
 * (renderer.c) folded in via nv2a_phase_add_psub(); no child subtraction is
 * needed because the nested PRESENTING finish inside render_display is already
 * absorbed by the enclosing present leaf and belongs in present_composite.
 *   GPUWAIT   — pgraph_vk_finish(FLIP_STALL): the vkWaitForFences drain that
 *               blocks the pfifo thread until the frame's GPU work retires. THE
 *               frame-pipelining candidate if it is a multi-ms completion wait.
 *   COMPOSITE — pgraph_vk_render_display: NV2A framebuffer -> host scale/blit
 *               (+ pvideo overlay, + its own PRESENTING finish).
 *   VRSUBMIT  — xemu_vr_frame: copy to the XR swapchain + submit the quad layer
 *               (expected ~0 in the flat sb-floor config).
 */
enum {
    PF_PSUB_GPUWAIT = 0, /* pgraph_vk_finish(FLIP_STALL) drain               */
    PF_PSUB_COMPOSITE,   /* pgraph_vk_render_display composite/blit          */
    PF_PSUB_VRSUBMIT,    /* xemu_vr_frame XR copy + submit                   */
    PF_PSUB__COUNT
};

/* Fold (now - start_ns) into present sub-third `which`. Self-gates; call behind
 * an nv2a_phase_timers_on() guard so the OFF path never reads the end clock. */
void nv2a_phase_add_psub(int which, int64_t start_ns);

/*
 * pfifo-thread flip anchor. Closes the current frame (wall = now - last
 * flip), folds it into the window histogram, and every PHASE_WINDOW flips
 * emits the summary line and resets the window. Self-gates on the env flag.
 */
void nv2a_phase_flip(void);

#ifdef __cplusplus
}
#endif

#endif /* HW_XBOX_NV2A_PGRAPH_PHASE_TIMERS_H */
