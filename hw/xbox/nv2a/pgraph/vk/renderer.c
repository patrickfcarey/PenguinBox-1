/*
 * Geforce NV2A PGRAPH Vulkan Renderer
 *
 * Copyright (c) 2024-2025 Matt Borgerson
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

#include "hw/xbox/nv2a/nv2a_int.h"
#include "hw/xbox/xbox-game-hacks.h"
#include "hw/xbox/nv2a/pgraph/phase_timers.h"
#include "renderer.h"
#include "vr.h"

#include "gloffscreen.h"

#if HAVE_EXTERNAL_MEMORY
static GloContext *g_gl_context;
#endif

static void early_context_init(void)
{
#if HAVE_EXTERNAL_MEMORY
    g_gl_context = glo_context_create();
#endif
}

static void pgraph_vk_init(NV2AState *d, Error **errp)
{
    PGRAPHState *pg = &d->pgraph;

    pg->vk_renderer_state = (PGRAPHVkState *)g_malloc0(sizeof(PGRAPHVkState));

#if HAVE_EXTERNAL_MEMORY
    glo_set_current(g_gl_context);
#endif

    pgraph_vk_debug_init();

    pgraph_vk_init_instance(pg, errp);
    if (*errp) {
        return;
    }

    pgraph_vk_init_command_buffers(pg);
    pgraph_vk_init_buffers(d);
    pgraph_vk_init_surfaces(pg);
    pgraph_vk_init_shaders(pg);
    pgraph_vk_init_pipelines(pg);
    pgraph_vk_init_textures(pg);
    pgraph_vk_init_reports(pg);
    pgraph_vk_init_compute(pg);
    pgraph_vk_init_display(pg);

    pgraph_vk_update_vertex_ram_buffer(&d->pgraph, 0, d->vram_ptr,
                                   memory_region_size(d->vram));

    pgraph_vk_determine_gpu_properties(d);
}

static void pgraph_vk_finalize(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;

    pgraph_vk_finalize_display(pg);
    pgraph_vk_finalize_compute(pg);
    pgraph_vk_finalize_reports(pg);
    pgraph_vk_finalize_textures(pg);
    pgraph_vk_finalize_pipelines(pg);
    pgraph_vk_finalize_shaders(pg);
    pgraph_vk_finalize_surfaces(pg);
    pgraph_vk_finalize_buffers(d);
    pgraph_vk_finalize_command_buffers(pg);
    pgraph_vk_finalize_instance(pg);

    g_free(pg->vk_renderer_state);
    pg->vk_renderer_state = NULL;
}

static void pgraph_vk_flush(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;

    pgraph_vk_finish(pg, VK_FINISH_REASON_FLUSH);
    /* S2 async present §5.1(7): the finish drained the cb_fence_ring, but a
     * submitted-without-wait composite lives in the PRESENT ring — retire it
     * too before loadvm/reset/renderer-switch tear state down. */
    pgraph_vk_wait_all_present_slots(pg);
    pgraph_vk_surface_flush(d);
    /* Whole-VRAM invalidate: force stock page-granular flagging of EVERY
     * texture (use_exact_spans=false) — this is a real full invalidate, not
     * an exact-writer event, so it must never be narrowed by the spans. */
    pgraph_vk_mark_textures_possibly_dirty(d, 0, memory_region_size(d->vram),
                                           false);
    pgraph_vk_update_vertex_ram_buffer(&d->pgraph, 0, d->vram_ptr,
                                       memory_region_size(d->vram));
    for (int i = 0; i < 4; i++) {
        pg->texture_dirty[i] = true;
    }

    /* FIXME: Flush more? */

    qatomic_set(&d->pgraph.flush_pending, false);
    qemu_event_set(&d->pgraph.flush_complete);
}

static void pgraph_vk_sync(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;

    /* pfifo: present bucket — the display composite (render_display, incl. its
     * PRESENTING finish and framebuffer upload, both absorbed by this leaf)
     * plus the VR submit below. pfifo thread; runs from process_pending, so it
     * is outside the pusher and not charged to `method`. */
    NV2A_PHASE_LEAF(PF_LEAF_PRESENT);

    /* present_composite (pfifo2): the NV2A framebuffer -> host display
     * scale/blit + pvideo overlay. Includes its own PRESENTING finish, which
     * the present leaf above already absorbs, so a plain clock diff is right. */
    bool pt = nv2a_phase_timers_on();
    int64_t pt_c0 = pt ? nv2a_phase_now_ns() : 0;
    pgraph_vk_render_display(pg);
    if (pt) {
        nv2a_phase_add_psub(PF_PSUB_COMPOSITE, pt_c0);
    }

    /* VR frame hook: copy r->display.image to the XR swapchain and submit
     * the quad layer. Same thread + same queue as render_display, so the
     * copy sees the finished frame by submission order. Never blocks. */
    int64_t pt_t0 = pt ? nv2a_phase_now_ns() : 0;
    xemu_vr_frame(pg);
    if (pt) {
        nv2a_phase_add_present(pt_t0); /* VR-submit half of present cost */
        nv2a_phase_add_psub(PF_PSUB_VRSUBMIT, pt_t0); /* present2 third */
    }

    qatomic_set(&d->pgraph.sync_pending, false);
    qemu_event_set(&d->pgraph.sync_complete);
}

static void pgraph_vk_process_pending(NV2AState *d)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;

    if (qatomic_read(&r->downloads_pending) ||
        qatomic_read(&r->download_dirty_surfaces_pending) ||
        qatomic_read(&d->pgraph.sync_pending) ||
        qatomic_read(&d->pgraph.flush_pending)
    ) {
        qemu_mutex_unlock(&d->pfifo.lock);
        qemu_mutex_lock(&d->pgraph.lock);
        if (qatomic_read(&r->downloads_pending)) {
            pgraph_vk_process_pending_downloads(d);
        }
        if (qatomic_read(&r->download_dirty_surfaces_pending)) {
            pgraph_vk_download_dirty_surfaces(d);
        }
        if (qatomic_read(&d->pgraph.sync_pending)) {
            pgraph_vk_sync(d);
        }
        if (qatomic_read(&d->pgraph.flush_pending)) {
            pgraph_vk_flush(d);
        }
        qemu_mutex_unlock(&d->pgraph.lock);
        qemu_mutex_lock(&d->pfifo.lock);
    }
}

static void pgraph_vk_flip_stall(NV2AState *d)
{
    /* Phase 0: the flip is the frame boundary AND the flip-time GPU drain.
     * Bracket the drain into present cost; nv2a_phase_flip() below closes
     * the frame's wall-time bucket. Both run on the pfifo thread. */
    /* pfifo: flip-finish GPU drain. The present leaf makes the FLIP_STALL
     * finish attribute to `present` (not `submit`); it runs inside the pusher
     * via the FLIP method, so method_end correctly excludes it from `method`. */
    {
        NV2A_PHASE_LEAF(PF_LEAF_PRESENT);
        bool pt = nv2a_phase_timers_on();
        int64_t pt_t0 = pt ? nv2a_phase_now_ns() : 0;
        if (pgraph_vk_async_present_enabled() &&
            !pgraph_vk_batch_qreset_enabled()) {
            /* S2 async present §5.1(6): submit the open batch at the flip —
             * no wait (otherwise the wall just moves from composite to
             * gpuwait). Recycle stays deferred (recycle_pending -> the next
             * real finish; the NEED_BUFFER_SPACE backstop self-heals a
             * no-other-finish frame). Guest flip pacing (READ_3D/vblank)
             * below is untouched.
             *
             * S2+S4 COMPOSITION INTERLOCK (coordinator, from Agent C's
             * Lane-1 note at the finish-path region swap): batch-qreset's
             * ping-pong swap is only safe after a full drain has applied
             * the report queue and rewound the slot window — guarantees
             * this submit path does not provide. Rather than a hasty
             * relaxed-swap (ISS-B16 risk class), S4 yields HERE: when both
             * toggles are on, the flip keeps the sync finish (else branch),
             * whose quiescent swap is proven, while S2 retains its main
             * win at the composite site (display.c). present_gpuwait then
             * measures the true cost of this interlock; if the matrix
             * shows it material, the follow-up is a region-scoped window
             * model per the comment at the swap site in draw.c. */
            PGRAPHState *pg = &d->pgraph;
            PGRAPHVkState *r = pg->vk_renderer_state;
            pgraph_vk_submit_batch(pg, VK_FINISH_REASON_FLIP_STALL);
            /* Keep the VMA budget tick working: its FLIP_STALL-keyed
             * trigger lives in the finish recycle phase we just skipped —
             * run it here at the flip submit site instead (same
             * once-per-frame cadence). */
            vmaSetCurrentFrameIndex(r->allocator, r->submit_count);
            r->allocator_last_submit_index = r->submit_count;
            pgraph_vk_check_memory_budget(pg);
        } else {
            pgraph_vk_finish(&d->pgraph, VK_FINISH_REASON_FLIP_STALL);
        }
        if (pt) {
            nv2a_phase_add_present(pt_t0); /* flip-finish half of present cost */
            /* present_gpuwait (pfifo2): the vkWaitForFences drain that blocks
             * the pfifo thread on the frame's GPU completion — the
             * frame-pipelining lever if it is a multi-ms wait. */
            nv2a_phase_add_psub(PF_PSUB_GPUWAIT, pt_t0);
        }
    }

    /* Refresh the predictive-readback runtime toggle once per frame so the
     * hot unbind path just reads a cached bool (live A/B via the watch
     * file — sb-graphics-findings.md). */
    pgraph_vk_refresh_predl_toggle();
    pgraph_vk_refresh_narrowfence_toggle();
    pgraph_vk_refresh_async_present_toggle();

    /* Steel Battalion radar-scan temporal downsampling: re-read the cadence and
     * arm/disarm the guest-EIP skip breakpoints for the upcoming frame (inert
     * unless the title matches and N>=2). See hw/xbox/xbox-game-hacks.c. */
    xbox_game_hacks_frame_tick();

    /* Predictive readback moved to unbind_surface (surface.c) — the
     * flip-time variant fired too late: the game draws and CPU-reads
     * within the same frame (v1 finding, sb-graphics-findings.md). */

    /* sb-graphics-research per-surface attribution: every 30 flips, one
     * stderr line PER surface that saw reads or downloads this frame —
     * addr / dims / color-vs-zeta / reads / downloads. Names which
     * surface (radar vs front-view camera vs monitors) actually carries
     * the cost. XEMU_SURF_ATTR=1. Reset every frame regardless. */
    {
        static int attr = -1;
        if (attr < 0) attr = getenv("XEMU_SURF_ATTR") ? 1 : 0;
        PGRAPHVkState *r = d->pgraph.vk_renderer_state;
        SurfaceBinding *s;
        bool report = attr && (g_nv2a_stats.frame_count % 30) == 0;
        QTAILQ_FOREACH(s, &r->surfaces, entry) {
            if (report && (s->reads_frame || s->dl_frame)) {
                fprintf(stderr,
                        "surf-attr: %08" HWADDR_PRIx " %ux%u %s reads=%u dl=%u\n",
                        s->vram_addr, s->width, s->height,
                        s->color ? "color" : "zeta",
                        s->reads_frame, s->dl_frame);
            }
            s->reads_frame = 0;
            s->dl_frame = 0;
        }
    }

    /* sb-graphics-research exact-dirty narrowing v2 (CHANNEL SEPARATION): start
     * each frame with an empty, trusted Channel-S span list. Exact NV2A_TEX
     * writers (surface download, 2D blit) append their byte ranges during the
     * frame and skip the page bitmap for those ranges; the texture-bind dirty
     * check consults these spans (Channel S) and, independently, the page bitmap
     * (Channel P — unattributed guest CPU writes). In v2 nothing clears
     * exact_spans_valid mid-frame: a span-array overflow degrades only that one
     * write back to the page bitmap (pgraph_vk_note_exact_dirty returns false),
     * leaving the already-recorded spans valid and consulted — clearing the flag
     * would strand writers that already skipped the bitmap. See renderer.h /
     * docs/sb-graphics-findings.md §8.7. */
    {
        PGRAPHVkState *r = d->pgraph.vk_renderer_state;
        r->exact_dirty_count = 0;
        r->exact_spans_valid = true;
    }

    /* Phase 0 frame anchor: close this frame's wall-time bucket and, every
     * PHASE_WINDOW flips, emit the decomposition line and reset the window. */
    nv2a_phase_flip();

    pgraph_vk_debug_frame_terminator();
}

static void pgraph_vk_pre_savevm_trigger(NV2AState *d)
{
    qatomic_set(&d->pgraph.vk_renderer_state->download_dirty_surfaces_pending, true);
    qemu_event_reset(&d->pgraph.vk_renderer_state->dirty_surfaces_download_complete);
}

static void pgraph_vk_pre_savevm_wait(NV2AState *d)
{
    qemu_event_wait(&d->pgraph.vk_renderer_state->dirty_surfaces_download_complete);
}

static void pgraph_vk_pre_shutdown_trigger(NV2AState *d)
{
    // qatomic_set(&d->pgraph.vk_renderer_state->shader_cache_writeback_pending, true);
    // qemu_event_reset(&d->pgraph.vk_renderer_state->shader_cache_writeback_complete);
}

static void pgraph_vk_pre_shutdown_wait(NV2AState *d)
{
    // qemu_event_wait(&d->pgraph.vk_renderer_state->shader_cache_writeback_complete);   
}

static int pgraph_vk_get_framebuffer_surface(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    qemu_mutex_lock(&d->pfifo.lock);

    VGADisplayParams vga_display_params;
    d->vga.get_params(&d->vga, &vga_display_params);

    SurfaceBinding *surface = pgraph_vk_surface_get_within(
        d, d->pcrtc.start + vga_display_params.line_offset);
    if (surface == NULL || !surface->color) {
        qemu_mutex_unlock(&d->pfifo.lock);
        return 0;
    }

    assert(surface->color);

    surface->frame_time = pg->frame_time;

#if HAVE_EXTERNAL_MEMORY
    qemu_event_reset(&d->pgraph.sync_complete);
    qatomic_set(&pg->sync_pending, true);
    pfifo_kick(d);
    qemu_mutex_unlock(&d->pfifo.lock);
    qemu_event_wait(&d->pgraph.sync_complete);

    /* S2 async present §5.1(5): when this sync's composite was submitted
     * without waiting, gate HERE — on the display thread — until its fence
     * signals, then hand back the slot's imported GL texture. This
     * preserves the exact CPU-complete-before-GL-sample interop contract
     * (there is no VK<->GL semaphore in this codebase); the GPU tail is
     * paid inside the display thread's vsync budget instead of the pfifo
     * frame. Thread safety: fence WAITS need no external synchronization,
     * all queue ops stay on pfifo, and the fence is reset only at slot
     * reuse — which the serialized gl_render_frame loop orders strictly
     * after this wait returns. The publish fields are plain stores on
     * pfifo ordered by the sync_complete event pair. valid==false (feature
     * OFF, or a pvideo/fallback frame) => singleton path, unchanged. */
    if (r->display.present_published_valid) {
        if (vkGetFenceStatus(r->device, r->display.present_published_fence) !=
            VK_SUCCESS) {
            nv2a_profile_inc_counter(NV2A_PROF_ASYNC_PRESENT_GATE_WAIT);
            VK_CHECK(vkWaitForFences(r->device, 1,
                                     &r->display.present_published_fence,
                                     VK_TRUE, UINT64_MAX));
        }
        return r->display.present_published_gl_id;
    }
    return r->display.gl_texture_id;
#else
    qemu_mutex_unlock(&d->pfifo.lock);
    pgraph_vk_wait_for_surface_download(surface);
    return 0;
#endif
}

static PGRAPHRenderer pgraph_vk_renderer = {
    .type = CONFIG_DISPLAY_RENDERER_VULKAN,
    .name = "Vulkan",
    .ops = {
        .init = pgraph_vk_init,
        .early_context_init = early_context_init,
        .finalize = pgraph_vk_finalize,
        .clear_report_value = pgraph_vk_clear_report_value,
        .clear_surface = pgraph_vk_clear_surface,
        .draw_begin = pgraph_vk_draw_begin,
        .draw_end = pgraph_vk_draw_end,
        .flip_stall = pgraph_vk_flip_stall,
        .flush_draw = pgraph_vk_flush_draw,
        .get_report = pgraph_vk_get_report,
        .image_blit = pgraph_vk_image_blit,
        .pre_savevm_trigger = pgraph_vk_pre_savevm_trigger,
        .pre_savevm_wait = pgraph_vk_pre_savevm_wait,
        .pre_shutdown_trigger = pgraph_vk_pre_shutdown_trigger,
        .pre_shutdown_wait = pgraph_vk_pre_shutdown_wait,
        .process_pending = pgraph_vk_process_pending,
        .process_pending_reports = pgraph_vk_process_pending_reports,
        .surface_update = pgraph_vk_surface_update,
        .set_surface_scale_factor = pgraph_vk_set_surface_scale_factor,
        .get_surface_scale_factor = pgraph_vk_get_surface_scale_factor,
        .get_framebuffer_surface = pgraph_vk_get_framebuffer_surface,
        .get_gpu_properties = pgraph_vk_get_gpu_properties,
    }
};

static void __attribute__((constructor)) register_renderer(void)
{
    pgraph_renderer_register(&pgraph_vk_renderer);
}

void pgraph_vk_check_memory_budget(PGRAPHState *pg)
{
#if 0 // FIXME
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkPhysicalDeviceMemoryProperties const *props;
    vmaGetMemoryProperties(r->allocator, &props);

    g_autofree VmaBudget *budgets = g_malloc_n(props->memoryHeapCount, sizeof(VmaBudget));
    vmaGetHeapBudgets(r->allocator, budgets);

    const float budget_threshold = 0.8;
    bool near_budget = false;

    for (int i = 0; i < props->memoryHeapCount; i++) {
        VmaBudget *b = &budgets[i];
        float use_to_budget_ratio =
            (double)b->statistics.allocationBytes / (double)b->budget;
        NV2A_VK_DPRINTF("Heap %d: used %lu/%lu MiB (%.2f%%)", i,
                        b->statistics.allocationBytes / (1024 * 1024),
                        b->budget / (1024 * 1024), use_to_budget_ratio * 100);
        near_budget |= use_to_budget_ratio > budget_threshold;
    }

    // If any heaps are near budget, free up some resources
    if (near_budget) {
        pgraph_vk_trim_texture_cache(pg);
    }
#endif

#if 0
    char *s;
    vmaBuildStatsString(r->allocator, &s, VK_TRUE);
    puts(s);
    vmaFreeStatsString(r->allocator, s);
#endif
}
