/*
 * QEMU Geforce NV2A profiling and debug helpers
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2018-2025 Matt Borgerson
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

#ifndef HW_XBOX_NV2A_DEBUG_H
#define HW_XBOX_NV2A_DEBUG_H

#include <stdint.h>

#define NV2A_XPRINTF(x, ...) do { \
    if (x) { \
        fprintf(stderr, "nv2a: " __VA_ARGS__); \
    } \
} while (0)

#ifndef DEBUG_NV2A
# define DEBUG_NV2A 0
#endif

#if DEBUG_NV2A
# define NV2A_DPRINTF(format, ...)       printf("nv2a: " format, ## __VA_ARGS__)
#else
# define NV2A_DPRINTF(format, ...)       do { } while (0)
#endif

/* Debug prints to identify when unimplemented or unconfirmed features
 * are being exercised. These cases likely result in graphical problems of
 * varying degree, but should otherwise not crash the system. Enable this
 * macro for debugging.
 */
#ifndef DEBUG_NV2A_FEATURES
# define DEBUG_NV2A_FEATURES 0
#endif

#if DEBUG_NV2A_FEATURES

/* Feature which has not yet been confirmed */
#define NV2A_UNCONFIRMED(format, ...) do { \
    fprintf(stderr, "nv2a: Warning unconfirmed feature: " format "\n", ## __VA_ARGS__); \
} while (0)

/* Feature which is not implemented */
#define NV2A_UNIMPLEMENTED(format, ...) do { \
    fprintf(stderr, "nv2a: Warning unimplemented feature: " format "\n", ## __VA_ARGS__); \
} while (0)

#else

#define NV2A_UNCONFIRMED(...) do {} while (0)
#define NV2A_UNIMPLEMENTED(...) do {} while (0)

#endif

/*
 * Single source of truth for the per-frame profiler counters. Each _X(NAME)
 * entry auto-generates: the enum value (below), the stringified HUD/summary
 * label (profile.c, stripped of the "NV2A_PROF_" prefix), the per-frame reset
 * (memset of frame_working at flip), the 1-in-60 "nv2a-prof:" stderr summary,
 * and the ImPlot HUD row. Adding an entry needs no other edit.
 *
 * The final group (TLB_FLUSH_ALL .. MMIO_DISPATCH) are the Phase-0
 * emulation-overhead counters: they quantify per-frame emulator-internal costs
 * charged to the guest-CPU thread (see docs/perf-roadmap-30-to-60.md):
 *   TLB_FLUSH_ALL / MEMCB_REGISTER  -- H-B: #2514 mem-callback full-TLB-flush
 *                                      churn from surface watch-range cycling.
 *   MEMCB_READ_DISPATCH / _WRITE_DISPATCH -- mem-access-callback dispatch volume
 *                                      post read-fast-path (reads should ~= 0).
 *   TB_TRANSLATE / TB_RECYCLE       -- TCG translation-block churn (fresh gen
 *                                      vs invalidated-TB reuse) from flush storm.
 *   TLB_FILL                        -- TLB slow-path refills (cost of flushes).
 *   MMIO_DISPATCH                   -- H-E: guest NV2A register MMIO traps
 *                                      (pushbuffer kicks + register polls).
 * They are incremented at cross-file sites (system/physmem.c, accel/tcg/
 * cputlb.c, accel/tcg/translate-all.c, hw/xbox/nv2a/nv2a_int.h) via the inline
 * nv2a_profile_inc_counter() below; those TUs reach this header through
 * -iquote <source-root> as "hw/xbox/nv2a/debug.h". All increments are plain
 * non-atomic ints, tolerating the vCPU/pfifo thread split exactly as the
 * existing SURF_CPU_READ counter does (magnitude is what matters).
 *
 * The ARENA_* .. OVERLAP_* tail group is the present-wall S0/S1 set
 * (docs/vr/present-wall-plan.md, docs/vr/overlap-resource-audit.md §6):
 *   ARENA_{IDX,VTX,UNI}_KB       -- per-frame staging-arena consumption (sum
 *                                   of pre-reset buffer_offset at each recycle,
 *                                   KB); sizes the uniform-arena headroom
 *                                   go/no-go (7-8 MB vs 8 MB capacity).
 *   ARENA_{IDX,VTX,UNI}_PK_KB    -- per-frame MAX of a single recycle's
 *                                   pre-reset offset (high-water; stored via a
 *                                   direct max-write, made per-frame by the
 *                                   flip-time reset).
 *   DESC_MAX / DESC_MAX_COMP     -- per-frame max graphics / compute
 *                                   descriptor_set_index at its pre-reset peak.
 *   TEX_UPLOAD_KB                -- decoded texture bytes staged per frame
 *                                   (sizes the D5 in-batch upload ring).
 *   VTX_RAM_SYNC_KB              -- bytes memcpy'd in place into the GPU-read
 *                                   vertex-RAM mirror (audit §2b).
 *   VTX_RAM_WAR_WAIT             -- steady-state dirty-range syncs at the draw
 *                                   path (each is a WAR wait_all_slots under
 *                                   overlap — the #1 overlap-killer if high).
 *   OVERLAP_*                    -- S1 XEMU_FRAME_OVERLAP self-counters
 *                                   (always 0 when OFF): full vs deferred
 *                                   recycles, slot retire-work executions,
 *                                   framebuffers routed via per-slot
 *                                   destroy-lists, per-slot descriptor-pool
 *                                   resets. RECYCLE_DEFER must stay 0 under
 *                                   S1-alone (armed regression detector).
 */
#define NV2A_PROF_COUNTERS_XMAC \
    _X(NV2A_PROF_FINISH_VERTEX_BUFFER_DIRTY) \
    _X(NV2A_PROF_FINISH_SURFACE_CREATE) \
    _X(NV2A_PROF_FINISH_SURFACE_DOWN) \
    _X(NV2A_PROF_FINISH_NEED_BUFFER_SPACE) \
    _X(NV2A_PROF_FINISH_FRAMEBUFFER_DIRTY) \
    _X(NV2A_PROF_FINISH_PRESENTING) \
    _X(NV2A_PROF_FINISH_FLIP_STALL) \
    _X(NV2A_PROF_FINISH_FLUSH) \
    _X(NV2A_PROF_FINISH_STALLED) \
    _X(NV2A_PROF_CLEAR) \
    _X(NV2A_PROF_QUEUE_SUBMIT) \
    _X(NV2A_PROF_QUEUE_SUBMIT_AUX) \
    _X(NV2A_PROF_PIPELINE_NOTDIRTY) \
    _X(NV2A_PROF_PIPELINE_GEN) \
    _X(NV2A_PROF_PIPELINE_BIND) \
    _X(NV2A_PROF_PIPELINE_RENDERPASSES) \
    _X(NV2A_PROF_PIPELINE_LOOKUP_SKIP) \
    _X(NV2A_PROF_BEGIN_ENDS) \
    _X(NV2A_PROF_DRAW_ARRAYS) \
    _X(NV2A_PROF_INLINE_BUFFERS) \
    _X(NV2A_PROF_INLINE_ARRAYS) \
    _X(NV2A_PROF_INLINE_ELEMENTS) \
    _X(NV2A_PROF_QUERY) \
    _X(NV2A_PROF_QUERY_ASYNC_RESOLVE) \
    _X(NV2A_PROF_QUERY_REUSE_WAIT) \
    _X(NV2A_PROF_QUERY_RING_SERVED) \
    _X(NV2A_PROF_REPORT_ASYNC_RETIRE) \
    _X(NV2A_PROF_REPORT_FORCE_DRAIN) \
    _X(NV2A_PROF_REPORT_AGEOUT_SUBMIT) \
    _X(NV2A_PROF_SHADER_GEN) \
    _X(NV2A_PROF_SHADER_BIND) \
    _X(NV2A_PROF_SHADER_BIND_NOTDIRTY) \
    _X(NV2A_PROF_SHADER_UBO_DIRTY) \
    _X(NV2A_PROF_SHADER_UBO_NOTDIRTY) \
    _X(NV2A_PROF_UNIFORM_SKIP) \
    _X(NV2A_PROF_ATTR_BIND) \
    _X(NV2A_PROF_TEX_UPLOAD) \
    _X(NV2A_PROF_UPLOAD_INBATCH) \
    _X(NV2A_PROF_UPLOAD_STAGING_WAIT) \
    _X(NV2A_PROF_GEOM_BUFFER_UPDATE_1) \
    _X(NV2A_PROF_GEOM_BUFFER_UPDATE_2) \
    _X(NV2A_PROF_GEOM_BUFFER_UPDATE_3) \
    _X(NV2A_PROF_GEOM_BUFFER_UPDATE_4) \
    _X(NV2A_PROF_GEOM_BUFFER_UPDATE_4_NOTDIRTY) \
    _X(NV2A_PROF_SURF_SWIZZLE) \
    _X(NV2A_PROF_SURF_CREATE) \
    _X(NV2A_PROF_SURF_DOWNLOAD) \
    _X(NV2A_PROF_SURF_UPLOAD) \
    _X(NV2A_PROF_SURF_TO_TEX) \
    _X(NV2A_PROF_SURF_TO_TEX_FALLBACK) \
    _X(NV2A_PROF_SURF_CPU_WRITE) \
    _X(NV2A_PROF_SURF_CPU_READ) \
    _X(NV2A_PROF_SURF_PREDL) \
    _X(NV2A_PROF_TEX_HASH) \
    _X(NV2A_PROF_TEX_HASH_KB) \
    _X(NV2A_PROF_TEX_HASH_SKIPPED) \
    _X(NV2A_PROF_SURF_CPU_DL_WAIT) \
    _X(NV2A_PROF_SURF_DMA_WRITE) \
    _X(NV2A_PROF_2D_BLIT) \
    _X(NV2A_PROF_QUEUE_SUBMIT_1) \
    _X(NV2A_PROF_QUEUE_SUBMIT_2) \
    _X(NV2A_PROF_QUEUE_SUBMIT_3) \
    _X(NV2A_PROF_QUEUE_SUBMIT_4) \
    _X(NV2A_PROF_QUEUE_SUBMIT_5) \
    _X(NV2A_PROF_RADAR_SCAN_SKIP) \
    _X(NV2A_PROF_TLB_FLUSH_ALL) \
    _X(NV2A_PROF_MEMCB_REGISTER) \
    _X(NV2A_PROF_MEMCB_READ_DISPATCH) \
    _X(NV2A_PROF_MEMCB_WRITE_DISPATCH) \
    _X(NV2A_PROF_TB_TRANSLATE) \
    _X(NV2A_PROF_TB_RECYCLE) \
    _X(NV2A_PROF_TLB_FILL) \
    _X(NV2A_PROF_MMIO_DISPATCH) \
    _X(NV2A_PROF_INLINE_BATCH) \
    _X(NV2A_PROF_ELEMENT_BATCH) \
    _X(NV2A_PROF_ARENA_IDX_KB) \
    _X(NV2A_PROF_ARENA_VTX_KB) \
    _X(NV2A_PROF_ARENA_UNI_KB) \
    _X(NV2A_PROF_ARENA_IDX_PK_KB) \
    _X(NV2A_PROF_ARENA_VTX_PK_KB) \
    _X(NV2A_PROF_ARENA_UNI_PK_KB) \
    _X(NV2A_PROF_DESC_MAX) \
    _X(NV2A_PROF_DESC_MAX_COMP) \
    _X(NV2A_PROF_TEX_UPLOAD_KB) \
    _X(NV2A_PROF_VTX_RAM_SYNC_KB) \
    _X(NV2A_PROF_VTX_RAM_WAR_WAIT) \
    _X(NV2A_PROF_OVERLAP_RECYCLE_FULL) \
    _X(NV2A_PROF_OVERLAP_RECYCLE_DEFER) \
    _X(NV2A_PROF_OVERLAP_SLOT_RETIRE) \
    _X(NV2A_PROF_OVERLAP_FB_DEFER) \
    _X(NV2A_PROF_OVERLAP_DESC_RESET) \
    _X(NV2A_PROF_ASYNC_PRESENT_SUBMIT) \
    _X(NV2A_PROF_ASYNC_PRESENT_GATE_WAIT) \
    _X(NV2A_PROF_ASYNC_PRESENT_REUSE_WAIT) \

enum NV2A_PROF_COUNTERS_ENUM {
    #define _X(x) x,
    NV2A_PROF_COUNTERS_XMAC
    #undef _X
    NV2A_PROF__COUNT
};

#define NV2A_PROF_NUM_FRAMES 300

typedef struct NV2AStats {
    int64_t last_flip_time;
    unsigned int frame_count;
    unsigned int increment_fps;
    struct {
        int mspf;
        int counters[NV2A_PROF__COUNT];
    } frame_working, frame_history[NV2A_PROF_NUM_FRAMES];
    unsigned int frame_ptr;
} NV2AStats;

#ifdef __cplusplus
extern "C" {
#endif

extern NV2AStats g_nv2a_stats;

const char *nv2a_profile_get_counter_name(unsigned int cnt);
int nv2a_profile_get_counter_value(unsigned int cnt);
void nv2a_profile_increment(void);
void nv2a_profile_flip_stall(void);

static inline void nv2a_profile_inc_counter(enum NV2A_PROF_COUNTERS_ENUM cnt)
{
    g_nv2a_stats.frame_working.counters[cnt] += 1;
}

#ifdef CONFIG_RENDERDOC
void nv2a_dbg_renderdoc_init(void);
void *nv2a_dbg_renderdoc_get_api(void);
bool nv2a_dbg_renderdoc_available(void);
void nv2a_dbg_renderdoc_capture_frames(int num_frames, bool trace);
extern int renderdoc_capture_frames;
extern bool renderdoc_trace_frames;
#endif

#ifdef __cplusplus
}
#endif

#endif
