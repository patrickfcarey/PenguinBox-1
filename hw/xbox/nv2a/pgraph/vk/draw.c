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

#include "qemu/osdep.h"
#include "qemu/fast-hash.h"
#include "hw/xbox/nv2a/pgraph/phase_timers.h"
#include "renderer.h"
#include <math.h>

/* XEMU_PIPE_OPT (force-off wins: XEMU_NO_PIPE_OPT). Read once at first use.
 * Default OFF: check_pipeline_dirty / create_pipeline stay byte-identical to
 * upstream. ON: a draw whose ONLY dirty input is texture image data skips the
 * redundant PipelineKey hash + LRU lookup, because the graphics pipeline never
 * depends on texture image data (it depends on shader_state, regs, vertex
 * layout, render-pass formats — all captured by shader_bindings_changed / the
 * register dirty checks). Process-wide, examined only at init, so a mid-run env
 * change is a no-op. */
bool pgraph_vk_pipe_opt_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        enabled = (getenv("XEMU_PIPE_OPT") != NULL &&
                   getenv("XEMU_NO_PIPE_OPT") == NULL)
                      ? 1
                      : 0;
        fprintf(stderr,
                "pipe-opt: texture-only-dirty pipeline-lookup skip %s\n",
                enabled ? "ON" : "OFF");
    }
    return enabled;
}

/* XEMU_FRAME_OVERLAP (force-off wins: XEMU_NO_FRAME_OVERLAP). Read once at
 * first use; one-time stderr state line per the fork convention. Default OFF:
 * every recycle/descriptor/framebuffer path stays byte-identical to today.
 * ON: the S1 overlap-ready decomposition — per-slot descriptor pools,
 * per-slot framebuffer destroy-lists, per-slot staging-arena watermarks,
 * all-retired-gated uploaded_bitmap clear, and the begin_query owner-fence
 * armed. S1 keeps EVERY finish/wait, so ON must be golden-image and
 * mspf-identical to OFF (the gate for this stage); the structure is what
 * later stages (async present/reports) build on. Process-wide, examined only
 * at init (structural resource shape cannot flip mid-run — same init-latched
 * precedent as XEMU_PIPE_OPT / XEMU_ASYNC_QUERIES / XEMU_CONST_SKIP).
 * Design: docs/vr/overlap-resource-audit.md §6; docs/vr/present-wall-plan.md
 * stage S1. */
bool pgraph_vk_frame_overlap_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        enabled = (getenv("XEMU_FRAME_OVERLAP") != NULL &&
                   getenv("XEMU_NO_FRAME_OVERLAP") == NULL)
                      ? 1
                      : 0;
        fprintf(stderr,
                "frame-overlap: S1 overlap-ready per-slot resource recycling %s\n",
                enabled ? "ON" : "OFF");
    }
    return enabled;
}

/* frame-overlap S1: execute a slot's deferred retire-work. Caller guarantees
 * the slot's last submission (if any) has retired — either the acquisition
 * fence wait just returned, or the recycle phase's wait_all_slots did.
 * Destroys the framebuffers superseded during the slot's recording and, if
 * the slot's last submission consumed descriptor sets, resets its pool and
 * re-allocates the set array (a pool reset invalidates the handles). Safe to
 * call repeatedly: both branches self-clear. */
static void overlap_process_slot_retire(PGRAPHState *pg,
                                        struct CbFenceSlot *slot)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    bool did_work = false;

    if (slot->ov_num_pending_fb > 0) {
        for (int i = 0; i < slot->ov_num_pending_fb; i++) {
            vkDestroyFramebuffer(r->device, slot->ov_pending_fb[i], NULL);
            slot->ov_pending_fb[i] = VK_NULL_HANDLE;
        }
        slot->ov_num_pending_fb = 0;
        did_work = true;
    }

    if (slot->ov_desc_dirty) {
        VK_CHECK(vkResetDescriptorPool(r->device, slot->ov_desc_pool, 0));
        pgraph_vk_overlap_alloc_slot_desc_sets(r, slot);
        slot->ov_desc_dirty = false;
        nv2a_profile_inc_counter(NV2A_PROF_OVERLAP_DESC_RESET);
        did_work = true;
    }

    if (did_work) {
        nv2a_profile_inc_counter(NV2A_PROF_OVERLAP_SLOT_RETIRE);
    }
}

void pgraph_vk_overlap_acquire_slot(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    assert(r->frame_overlap);
    if (r->ov_slot_acquired) {
        return;
    }

    /* Mirror of the OFF-path acquisition in pgraph_vk_begin_command_buffer:
     * select ring slot submit_count % N, wait its previous occupant's fence
     * (the ring's natural depth bound), then additionally run the occupant's
     * retire-work before the slot's per-slot resources are reused. Runs
     * EARLY relative to vkBeginCommandBuffer (update_descriptor_sets calls
     * this before the command buffer opens) — that only moves the same
     * fence wait a few calls forward; recording still starts afterward. */
    if (r->submit_count == SURFACE_NO_WRITE) {
        r->submit_count++; /* never use the sentinel as a real index */
    }
    struct CbFenceSlot *slot =
        &r->cb_fence_ring[r->submit_count % CB_FENCE_RING_SIZE];
    if (slot->submit_index != SURFACE_NO_WRITE &&
        vkGetFenceStatus(r->device, slot->fence) != VK_SUCCESS) {
        VK_CHECK(vkWaitForFences(r->device, 1, &slot->fence, VK_TRUE,
                                 UINT64_MAX));
    }
    overlap_process_slot_retire(pg, slot);

    r->current_slot = slot;
    r->command_buffer = slot->cb;
    r->aux_command_buffer = slot->aux_cb;
    /* Descriptor consumption is per-slot under overlap: the fresh slot
     * starts at 0, which also forces pgraph_vk_update_descriptor_sets to
     * WRITE (never reuse) on the first draw of a slot — a cross-slot set
     * reference is impossible by construction. */
    r->descriptor_set_index = 0;
    r->ov_slot_acquired = true;
}

void pgraph_vk_draw_begin(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;

    NV2A_VK_DPRINTF("NV097_SET_BEGIN_END: 0x%x", d->pgraph.primitive_mode);

    uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    bool mask_alpha = control_0 & NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE;
    bool mask_red = control_0 & NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE;
    bool mask_green = control_0 & NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE;
    bool mask_blue = control_0 & NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE;
    bool color_write = mask_alpha || mask_red || mask_green || mask_blue;
    bool depth_test = control_0 & NV_PGRAPH_CONTROL_0_ZENABLE;
    bool stencil_test =
        pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1) & NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE;
    bool is_nop_draw = !(color_write || depth_test || stencil_test);

    pgraph_vk_surface_update(d, true, true, depth_test || stencil_test);

    if (is_nop_draw) {
        NV2A_VK_DPRINTF("nop!");
        return;
    }
}

static VkPrimitiveTopology get_primitive_topology(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    int polygon_mode = r->shader_binding->state.geom.polygon_front_mode;
    int primitive_mode = r->shader_binding->state.geom.primitive_mode;

    // FIXME: Replace with LUT
    switch (primitive_mode) {
    case PRIM_TYPE_POINTS:
        return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case PRIM_TYPE_LINES:
        return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case PRIM_TYPE_LINE_LOOP:
        // FIXME: line strips, except that the first and last vertices are also used as a line
        return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case PRIM_TYPE_LINE_STRIP:
        return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case PRIM_TYPE_TRIANGLES:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case PRIM_TYPE_TRIANGLE_STRIP:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case PRIM_TYPE_TRIANGLE_FAN:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    case PRIM_TYPE_QUADS:
        return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
    case PRIM_TYPE_QUAD_STRIP:
        return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;
    case PRIM_TYPE_POLYGON:
        if (polygon_mode == POLY_MODE_LINE) {
            return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; // FIXME
        } else if (polygon_mode == POLY_MODE_FILL) {
            return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
        }
        assert(!"PRIM_TYPE_POLYGON with invalid polygon_mode");
        return 0;
    default:
        assert(!"Invalid primitive_mode");
        return 0;
    }
}

static void pipeline_cache_entry_init(Lru *lru, LruNode *node,
                                      const void *state)
{
    PipelineBinding *snode = container_of(node, PipelineBinding, node);
    snode->layout = VK_NULL_HANDLE;
    snode->pipeline = VK_NULL_HANDLE;
    snode->draw_time = 0;
}

static void pipeline_cache_entry_post_evict(Lru *lru, LruNode *node)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, pipeline_cache);
    PipelineBinding *snode = container_of(node, PipelineBinding, node);

    assert((!r->in_command_buffer ||
            snode->draw_time < r->command_buffer_start_time) &&
           "Pipeline evicted while in use!");

    /* narrow-fence UAF gate: the assert only covers the OPEN command
     * buffer; an already-submitted in-flight batch may still execute with
     * this pipeline bound. Retire in-flight work before destroying. */
    pgraph_vk_wait_all_slots(r);

    vkDestroyPipeline(r->device, snode->pipeline, NULL);
    snode->pipeline = VK_NULL_HANDLE;

    vkDestroyPipelineLayout(r->device, snode->layout, NULL);
    snode->layout = VK_NULL_HANDLE;
}

static bool pipeline_cache_entry_compare(Lru *lru, LruNode *node,
                                         const void *key)
{
    PipelineBinding *snode = container_of(node, PipelineBinding, node);
    return memcmp(&snode->key, key, sizeof(PipelineKey));
}

static void init_pipeline_cache(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkPipelineCacheCreateInfo cache_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
        .flags = 0,
        .initialDataSize = 0,
        .pInitialData = NULL,
        .pNext = NULL,
    };
    VK_CHECK(vkCreatePipelineCache(r->device, &cache_info, NULL,
                                   &r->vk_pipeline_cache));

    const size_t pipeline_cache_size = 2048;
    lru_init(&r->pipeline_cache);
    r->pipeline_cache_entries =
        g_malloc_n(pipeline_cache_size, sizeof(PipelineBinding));
    assert(r->pipeline_cache_entries != NULL);
    for (int i = 0; i < pipeline_cache_size; i++) {
        lru_add_free(&r->pipeline_cache, &r->pipeline_cache_entries[i].node);
    }

    r->pipeline_cache.init_node = pipeline_cache_entry_init;
    r->pipeline_cache.compare_nodes = pipeline_cache_entry_compare;
    r->pipeline_cache.post_node_evict = pipeline_cache_entry_post_evict;
}

static void finalize_pipeline_cache(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    lru_flush(&r->pipeline_cache);
    g_free(r->pipeline_cache_entries);
    r->pipeline_cache_entries = NULL;

    vkDestroyPipelineCache(r->device, r->vk_pipeline_cache, NULL);
}

static char const *const quad_glsl =
    "#version 450\n"
    "void main()\n"
    "{\n"
    "    float x = -1.0 + float((gl_VertexIndex & 1) << 2);\n"
    "    float y = -1.0 + float((gl_VertexIndex & 2) << 1);\n"
    "    gl_Position = vec4(x, y, 0, 1);\n"
    "}\n";

static char const *const solid_frag_glsl =
    "#version 450\n"
    "layout(location = 0) out vec4 fragColor;\n"
    "void main()\n"
    "{\n"
    "    fragColor = vec4(1.0);"
    "}\n";

static void init_clear_shaders(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    r->quad_vert_module = pgraph_vk_create_shader_module_from_glsl(
        r, VK_SHADER_STAGE_VERTEX_BIT, quad_glsl);
    r->solid_frag_module = pgraph_vk_create_shader_module_from_glsl(
        r, VK_SHADER_STAGE_FRAGMENT_BIT, solid_frag_glsl);
}

static void finalize_clear_shaders(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    pgraph_vk_destroy_shader_module(r, r->quad_vert_module);
    pgraph_vk_destroy_shader_module(r, r->solid_frag_module);
}

static void init_render_passes(PGRAPHVkState *r)
{
    r->render_passes = g_array_new(false, false, sizeof(RenderPass));
}

static void finalize_render_passes(PGRAPHVkState *r)
{
    for (int i = 0; i < r->render_passes->len; i++) {
        RenderPass *p = &g_array_index(r->render_passes, RenderPass, i);
        vkDestroyRenderPass(r->device, p->render_pass, NULL);
    }
    g_array_free(r->render_passes, true);
    r->render_passes = NULL;
}

void pgraph_vk_init_pipelines(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    r->pipe_opt = pgraph_vk_pipe_opt_enabled();

    init_pipeline_cache(pg);
    init_clear_shaders(pg);
    init_render_passes(r);

    /* narrow-fence: per-slot binary semaphore (aux->main ordering) and fence.
     * Fences created SIGNALED so the first reuse-wait on any slot is a no-op;
     * slots start un-owned. */
    VkSemaphoreCreateInfo semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO
    };
    VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    for (int i = 0; i < CB_FENCE_RING_SIZE; i++) {
        VK_CHECK(vkCreateFence(r->device, &fence_info, NULL,
                               &r->cb_fence_ring[i].fence));
        VK_CHECK(vkCreateSemaphore(r->device, &semaphore_info, NULL,
                                   &r->cb_fence_ring[i].sem));
        r->cb_fence_ring[i].submit_index = SURFACE_NO_WRITE;
        r->cb_fence_ring[i].query_first = 0;
        r->cb_fence_ring[i].query_count = 0;
    }
}

void pgraph_vk_finalize_pipelines(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    finalize_clear_shaders(pg);
    finalize_pipeline_cache(pg);
    finalize_render_passes(r);

    for (int i = 0; i < CB_FENCE_RING_SIZE; i++) {
        /* frame-overlap S1: drain any per-slot pending-destroy framebuffers
         * (same GPU-idle assumption as the fence destroy below; empty in
         * practice — every finish's recycle clears the lists). */
        if (r->frame_overlap) {
            struct CbFenceSlot *slot = &r->cb_fence_ring[i];
            for (int j = 0; j < slot->ov_num_pending_fb; j++) {
                vkDestroyFramebuffer(r->device, slot->ov_pending_fb[j], NULL);
                slot->ov_pending_fb[j] = VK_NULL_HANDLE;
            }
            slot->ov_num_pending_fb = 0;
        }
        vkDestroyFence(r->device, r->cb_fence_ring[i].fence, NULL);
        vkDestroySemaphore(r->device, r->cb_fence_ring[i].sem, NULL);
    }
}

static void init_render_pass_state(PGRAPHState *pg, RenderPassState *state)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    state->color_format = r->color_binding ?
                              r->color_binding->host_fmt.vk_format :
                              VK_FORMAT_UNDEFINED;
    state->zeta_format = r->zeta_binding ? r->zeta_binding->host_fmt.vk_format :
                                           VK_FORMAT_UNDEFINED;
}

static VkRenderPass create_render_pass(PGRAPHVkState *r, RenderPassState *state)
{
    NV2A_VK_DPRINTF("Creating render pass");

    VkAttachmentDescription attachments[2];
    int num_attachments = 0;

    bool color = state->color_format != VK_FORMAT_UNDEFINED;
    bool zeta = state->zeta_format != VK_FORMAT_UNDEFINED;

    VkAttachmentReference color_reference;
    if (color) {
        attachments[num_attachments] = (VkAttachmentDescription){
            .format = state->color_format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        };
        color_reference = (VkAttachmentReference){
            num_attachments, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
        };
        num_attachments++;
    }

    VkAttachmentReference depth_reference;
    if (zeta) {
        attachments[num_attachments] = (VkAttachmentDescription){
            .format = state->zeta_format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
            .initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        };
        depth_reference = (VkAttachmentReference){
            num_attachments, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        };
        num_attachments++;
    }

    VkSubpassDependency dependency = {
        .srcSubpass = VK_SUBPASS_EXTERNAL,
    };

    if (color) {
        dependency.srcStageMask |=
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.srcAccessMask |= VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependency.dstStageMask |=
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstAccessMask |= VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    }

    if (zeta) {
        dependency.srcStageMask |=
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependency.srcAccessMask |=
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependency.dstStageMask |=
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependency.dstAccessMask |=
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }

    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = color ? 1 : 0,
        .pColorAttachments = color ? &color_reference : NULL,
        .pDepthStencilAttachment = zeta ? &depth_reference : NULL,
    };

    VkRenderPassCreateInfo renderpass_create_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = num_attachments,
        .pAttachments = attachments,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &dependency,
    };
    VkRenderPass render_pass;
    VK_CHECK(vkCreateRenderPass(r->device, &renderpass_create_info, NULL,
                                &render_pass));
    return render_pass;
}

static VkRenderPass add_new_render_pass(PGRAPHVkState *r, RenderPassState *state)
{
    RenderPass new_pass;
    memcpy(&new_pass.state, state, sizeof(*state));
    new_pass.render_pass = create_render_pass(r, state);
    g_array_append_vals(r->render_passes, &new_pass, 1);
    return new_pass.render_pass;
}

static VkRenderPass get_render_pass(PGRAPHVkState *r, RenderPassState *state)
{
    for (int i = 0; i < r->render_passes->len; i++) {
        RenderPass *p = &g_array_index(r->render_passes, RenderPass, i);
        if (!memcmp(&p->state, state, sizeof(*state))) {
            return p->render_pass;
        }
    }
    return add_new_render_pass(r, state);
}

static void create_frame_buffer(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    NV2A_VK_DPRINTF("Creating framebuffer");

    assert(r->color_binding || r->zeta_binding);

    if (r->framebuffer_index >= ARRAY_SIZE(r->framebuffers)) {
        pgraph_vk_finish(pg, VK_FINISH_REASON_NEED_BUFFER_SPACE);
    }

    VkImageView attachments[2];
    int attachment_count = 0;

    if (r->color_binding) {
        attachments[attachment_count++] = r->color_binding->image_view;
    }
    if (r->zeta_binding) {
        attachments[attachment_count++] = r->zeta_binding->image_view;
    }

    SurfaceBinding *binding = r->color_binding ? : r->zeta_binding;

    VkFramebufferCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = r->render_pass,
        .attachmentCount = attachment_count,
        .pAttachments = attachments,
        .width = binding->width,
        .height = binding->height,
        .layers = 1,
    };
    pgraph_apply_scaling_factor(pg, &create_info.width, &create_info.height);
    VK_CHECK(vkCreateFramebuffer(r->device, &create_info, NULL,
                                 &r->framebuffers[r->framebuffer_index++]));
}

static void destroy_framebuffers(PGRAPHState *pg)
{
    NV2A_VK_DPRINTF("Destroying framebuffer");
    PGRAPHVkState *r = pg->vk_renderer_state;

    for (int i = 0; i < r->framebuffer_index; i++) {
        vkDestroyFramebuffer(r->device, r->framebuffers[i], NULL);
        r->framebuffers[i] = VK_NULL_HANDLE;
    }
    r->framebuffer_index = 0;
}

static void create_clear_pipeline(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    NV2A_VK_DGROUP_BEGIN("Creating clear pipeline");

    PipelineKey key;
    memset(&key, 0, sizeof(key));
    key.clear = true;
    init_render_pass_state(pg, &key.render_pass_state);

    key.regs[0] = r->clear_parameter;

    uint64_t hash = fast_hash((void *)&key, sizeof(key));
    LruNode *node = lru_lookup(&r->pipeline_cache, hash, &key);
    PipelineBinding *snode = container_of(node, PipelineBinding, node);

    if (snode->pipeline != VK_NULL_HANDLE) {
        NV2A_VK_DPRINTF("Cache hit");
        r->pipeline_binding_changed = r->pipeline_binding != snode;
        r->pipeline_binding = snode;
        NV2A_VK_DGROUP_END();
        return;
    }

    NV2A_VK_DPRINTF("Cache miss");
    nv2a_profile_inc_counter(NV2A_PROF_PIPELINE_GEN);
    memcpy(&snode->key, &key, sizeof(key));

    bool clear_any_color_channels =
        r->clear_parameter & NV097_CLEAR_SURFACE_COLOR;
    bool clear_all_color_channels =
        (r->clear_parameter & NV097_CLEAR_SURFACE_COLOR) ==
        (NV097_CLEAR_SURFACE_R | NV097_CLEAR_SURFACE_G | NV097_CLEAR_SURFACE_B |
         NV097_CLEAR_SURFACE_A);
    bool partial_color_clear =
        clear_any_color_channels && !clear_all_color_channels;

    int num_active_shader_stages = 0;
    VkPipelineShaderStageCreateInfo shader_stages[2];
    shader_stages[num_active_shader_stages++] =
        (VkPipelineShaderStageCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = r->quad_vert_module->module,
            .pName = "main",
        };
    if (partial_color_clear) {
        shader_stages[num_active_shader_stages++] =
            (VkPipelineShaderStageCreateInfo){
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                .module = r->solid_frag_module->module,
                .pName = "main",
            };
     }

    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };

    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .primitiveRestartEnable = VK_FALSE,
    };

    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };

    VkPipelineRasterizationStateCreateInfo rasterizer = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .lineWidth = 1.0f,
        .cullMode = VK_CULL_MODE_BACK_BIT,
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .depthBiasEnable = VK_FALSE,
    };

    VkPipelineMultisampleStateCreateInfo multisampling = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .sampleShadingEnable = VK_FALSE,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };

    VkPipelineDepthStencilStateCreateInfo depth_stencil = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable =
            (r->clear_parameter & NV097_CLEAR_SURFACE_Z) ? VK_TRUE : VK_FALSE,
        .depthCompareOp = VK_COMPARE_OP_ALWAYS,
        .depthBoundsTestEnable = VK_FALSE,
    };

    if (r->clear_parameter & NV097_CLEAR_SURFACE_STENCIL) {
        depth_stencil.stencilTestEnable = VK_TRUE;
        depth_stencil.front.failOp = VK_STENCIL_OP_REPLACE;
        depth_stencil.front.passOp = VK_STENCIL_OP_REPLACE;
        depth_stencil.front.depthFailOp = VK_STENCIL_OP_REPLACE;
        depth_stencil.front.compareOp = VK_COMPARE_OP_ALWAYS;
        depth_stencil.front.compareMask = 0xff;
        depth_stencil.front.writeMask = 0xff;
        depth_stencil.front.reference = 0xff;
        depth_stencil.back = depth_stencil.front;
    }

    VkColorComponentFlags write_mask = 0;
    if (r->clear_parameter & NV097_CLEAR_SURFACE_R)
        write_mask |= VK_COLOR_COMPONENT_R_BIT;
    if (r->clear_parameter & NV097_CLEAR_SURFACE_G)
        write_mask |= VK_COLOR_COMPONENT_G_BIT;
    if (r->clear_parameter & NV097_CLEAR_SURFACE_B)
        write_mask |= VK_COLOR_COMPONENT_B_BIT;
    if (r->clear_parameter & NV097_CLEAR_SURFACE_A)
        write_mask |= VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendAttachmentState color_blend_attachment = {
        .colorWriteMask = write_mask,
        .blendEnable = VK_TRUE,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
        .srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_COLOR,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_CONSTANT_ALPHA,
    };

    VkPipelineColorBlendStateCreateInfo color_blending = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable = VK_FALSE,
        .logicOp = VK_LOGIC_OP_COPY,
        .attachmentCount = r->color_binding ? 1 : 0,
        .pAttachments = r->color_binding ? &color_blend_attachment : NULL,
    };

    VkDynamicState dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT,
                                        VK_DYNAMIC_STATE_SCISSOR,
                                        VK_DYNAMIC_STATE_BLEND_CONSTANTS };
    VkPipelineDynamicStateCreateInfo dynamic_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = partial_color_clear ? 3 : 2,
        .pDynamicStates = dynamic_states,
    };

    VkPipelineLayoutCreateInfo pipeline_layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
    };

    VkPipelineLayout layout;
    VK_CHECK(vkCreatePipelineLayout(r->device, &pipeline_layout_info, NULL,
                                    &layout));

    VkGraphicsPipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = num_active_shader_stages,
        .pStages = shader_stages,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = r->zeta_binding ? &depth_stencil : NULL,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_state,
        .layout = layout,
        .renderPass = get_render_pass(r, &key.render_pass_state),
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
    };

    VkPipeline pipeline;
    VK_CHECK(vkCreateGraphicsPipelines(r->device, r->vk_pipeline_cache, 1,
                                       &pipeline_info, NULL, &pipeline));

    snode->pipeline = pipeline;
    snode->layout = layout;
    snode->render_pass = pipeline_info.renderPass;
    snode->draw_time = pg->draw_time;

    r->pipeline_binding = snode;
    r->pipeline_binding_changed = true;

    NV2A_VK_DGROUP_END();
}

static bool check_render_pass_dirty(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    assert(r->pipeline_binding);

    RenderPassState state;
    init_render_pass_state(pg, &state);

    return memcmp(&state, &r->pipeline_binding->key.render_pass_state,
                  sizeof(state)) != 0;
}

// Quickly check for any state changes that would require more analysis
static bool check_pipeline_dirty(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    /* pipe-opt: the graphics pipeline never depends on texture IMAGE data — its
     * key (PipelineKey) is {render_pass_state, shader_state, the 9 fixed-
     * function regs, vertex bind/attr descriptions}. A texture change that DOES
     * affect the pipeline does so through the fragment combiner, i.e. through
     * shader_state, which sets shader_bindings_changed (checked below). So a
     * draw whose ONLY dirty input is texture image data yields a PipelineKey
     * identical to the currently-bound pipeline's — upstream still hashes it and
     * performs an LRU lookup that is a GUARANTEED cache hit to the same
     * pipeline. When XEMU_PIPE_OPT is on, drop texture_bindings_changed from the
     * dirty set so that redundant hash+lookup is skipped. OFF keeps it, so the
     * dirty decision is byte-identical to upstream. */
    bool tex_forces_dirty = r->texture_bindings_changed && !r->pipe_opt;

    if (!r->pipeline_binding || r->shader_bindings_changed ||
        tex_forces_dirty || check_render_pass_dirty(pg)) {
        return true;
    }

    const unsigned int regs[] = {
        NV_PGRAPH_BLEND,       NV_PGRAPH_BLENDCOLOR,  NV_PGRAPH_CONTROL_0,
        NV_PGRAPH_CONTROL_1,   NV_PGRAPH_CONTROL_2,   NV_PGRAPH_CONTROL_3,
        NV_PGRAPH_SETUPRASTER, NV_PGRAPH_ZOFFSETBIAS, NV_PGRAPH_ZOFFSETFACTOR,
    };

    for (int i = 0; i < ARRAY_SIZE(regs); i++) {
        if (pgraph_is_reg_dirty(pg, regs[i])) {
            return true;
        }
    }

    // FIXME: Use dirty bits instead
    if (memcmp(r->vertex_attribute_descriptions,
               r->pipeline_binding->key.attribute_descriptions,
               r->num_active_vertex_attribute_descriptions *
                   sizeof(r->vertex_attribute_descriptions[0])) ||
        memcmp(r->vertex_binding_descriptions,
               r->pipeline_binding->key.binding_descriptions,
               r->num_active_vertex_binding_descriptions *
                   sizeof(r->vertex_binding_descriptions[0]))) {
        return true;
    }

    /* pipe-opt self-measure: this draw is genuinely not pipeline-dirty. If
     * pipe-opt suppressed a texture-only dirty flag that upstream would have
     * hashed + looked up (always the same-pipeline cache hit), record the
     * skipped lookup. Counts only the draws pipe-opt actually changed. */
    if (r->pipe_opt && r->texture_bindings_changed) {
        nv2a_profile_inc_counter(NV2A_PROF_PIPELINE_LOOKUP_SKIP);
    }

    nv2a_profile_inc_counter(NV2A_PROF_PIPELINE_NOTDIRTY);

    return false;
}

static void init_pipeline_key(PGRAPHState *pg, PipelineKey *key)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    memset(key, 0, sizeof(*key));
    init_render_pass_state(pg, &key->render_pass_state);
    memcpy(&key->shader_state, &r->shader_binding->state, sizeof(ShaderState));
    memcpy(key->binding_descriptions, r->vertex_binding_descriptions,
           sizeof(key->binding_descriptions[0]) *
               r->num_active_vertex_binding_descriptions);
    memcpy(key->attribute_descriptions, r->vertex_attribute_descriptions,
           sizeof(key->attribute_descriptions[0]) *
               r->num_active_vertex_attribute_descriptions);

    // FIXME: Register masking
    // FIXME: Use more dynamic state updates
    const int regs[] = {
        NV_PGRAPH_BLEND,       NV_PGRAPH_BLENDCOLOR,  NV_PGRAPH_CONTROL_0,
        NV_PGRAPH_CONTROL_1,   NV_PGRAPH_CONTROL_2,   NV_PGRAPH_CONTROL_3,
        NV_PGRAPH_SETUPRASTER, NV_PGRAPH_ZOFFSETBIAS, NV_PGRAPH_ZOFFSETFACTOR,
    };
    assert(ARRAY_SIZE(regs) == ARRAY_SIZE(key->regs));
    for (int i = 0; i < ARRAY_SIZE(regs); i++) {
        key->regs[i] = pgraph_reg_r(pg, regs[i]);
    }
}

static void create_pipeline(PGRAPHState *pg)
{
    NV2A_VK_DGROUP_BEGIN("Creating pipeline");

    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHVkState *r = pg->vk_renderer_state;

    pgraph_vk_bind_textures(d);
    /* pfifo2: bind_textures ran in PIPE (its surf->tex single-time waits were
     * redirected to method_synctex). Enter CONST for the uniform recompute +
     * content hash + UBO staging append, then return to PIPE for the pipeline
     * lookup/create below (placed before the cache-hit early returns so the
     * cursor always leaves this function in PIPE). */
    NV2A_PHASE_MSUB(PF_MSUB_CONST);
    pgraph_vk_bind_shaders(pg);
    NV2A_PHASE_MSUB(PF_MSUB_PIPE);

    // FIXME: If nothing was dirty, don't even try creating the key or hashing.
    //        Just use the same pipeline.
    bool pipeline_dirty = check_pipeline_dirty(pg);

    pgraph_clear_dirty_reg_map(pg);
    // FIXME: We could clear less

    if (r->pipeline_binding && !pipeline_dirty) {
        NV2A_VK_DPRINTF("Cache hit");
        /* pipe-opt: not-dirty => the selected pipeline is identical to the one
         * already bound, so it did NOT change. Upstream's texture-only path
         * reaches this equivalently via the LRU cache hit below, which sets
         * pipeline_binding_changed = (pipeline_binding != snode) == false. Match
         * that here so begin_draw does not emit a redundant vkCmdBindPipeline.
         * Safe: not-dirty implies the pipeline was unchanged since it was last
         * bound; a fresh command buffer has in_render_pass == false, so
         * begin_draw still force-binds on the render-pass (re)begin. OFF leaves
         * the flag untouched (byte-identical to upstream). */
        if (r->pipe_opt) {
            r->pipeline_binding_changed = false;
        }
        NV2A_VK_DGROUP_END();
        return;
    }

    PipelineKey key;
    init_pipeline_key(pg, &key);
    uint64_t hash = fast_hash((void *)&key, sizeof(key));

    LruNode *node = lru_lookup(&r->pipeline_cache, hash, &key);
    PipelineBinding *snode = container_of(node, PipelineBinding, node);
    if (snode->pipeline != VK_NULL_HANDLE) {
        NV2A_VK_DPRINTF("Cache hit");
        r->pipeline_binding_changed = r->pipeline_binding != snode;
        r->pipeline_binding = snode;
        NV2A_VK_DGROUP_END();
        return;
    }

    NV2A_VK_DPRINTF("Cache miss");
    nv2a_profile_inc_counter(NV2A_PROF_PIPELINE_GEN);

    memcpy(&snode->key, &key, sizeof(key));

    uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    bool depth_test = control_0 & NV_PGRAPH_CONTROL_0_ZENABLE;
    bool depth_write = !!(control_0 & NV_PGRAPH_CONTROL_0_ZWRITEENABLE);
    bool stencil_test =
        pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1) & NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE;

    int num_active_shader_stages = 0;
    VkPipelineShaderStageCreateInfo shader_stages[3];

    shader_stages[num_active_shader_stages++] =
        (VkPipelineShaderStageCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = r->shader_binding->vsh.module_info->module,
            .pName = "main",
        };
    if (r->shader_binding->geom.module_info) {
        shader_stages[num_active_shader_stages++] =
            (VkPipelineShaderStageCreateInfo){
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_GEOMETRY_BIT,
                .module = r->shader_binding->geom.module_info->module,
                .pName = "main",
            };
    }
    shader_stages[num_active_shader_stages++] =
        (VkPipelineShaderStageCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = r->shader_binding->psh.module_info->module,
            .pName = "main",
        };

    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount =
            r->num_active_vertex_binding_descriptions,
        .pVertexBindingDescriptions = r->vertex_binding_descriptions,
        .vertexAttributeDescriptionCount =
            r->num_active_vertex_attribute_descriptions,
        .pVertexAttributeDescriptions = r->vertex_attribute_descriptions,
    };

    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = get_primitive_topology(pg),
        .primitiveRestartEnable = VK_FALSE,
    };

    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };

    void *rasterizer_next_struct = NULL;

    VkPipelineRasterizationStateCreateInfo rasterizer = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = VK_TRUE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = pgraph_polygon_mode_vk_map[r->shader_binding->state
                                                      .geom.polygon_front_mode],
        .lineWidth = 1.0f,
        .frontFace = (pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER) &
                      NV_PGRAPH_SETUPRASTER_FRONTFACE) ?
                         VK_FRONT_FACE_COUNTER_CLOCKWISE :
                         VK_FRONT_FACE_CLOCKWISE,
        .depthBiasEnable = VK_FALSE,
        .pNext = rasterizer_next_struct,
    };

    if (pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER) & NV_PGRAPH_SETUPRASTER_CULLENABLE) {
        uint32_t cull_face = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER),
                                      NV_PGRAPH_SETUPRASTER_CULLCTRL);
        assert(cull_face < ARRAY_SIZE(pgraph_cull_face_vk_map));
        rasterizer.cullMode = pgraph_cull_face_vk_map[cull_face];
    } else {
        rasterizer.cullMode = VK_CULL_MODE_NONE;
    }

    VkPipelineMultisampleStateCreateInfo multisampling = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .sampleShadingEnable = VK_FALSE,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };

    VkPipelineDepthStencilStateCreateInfo depth_stencil = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthWriteEnable = depth_write ? VK_TRUE : VK_FALSE,
    };

    if (depth_test) {
        depth_stencil.depthTestEnable = VK_TRUE;
        uint32_t depth_func =
            GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0), NV_PGRAPH_CONTROL_0_ZFUNC);
        assert(depth_func < ARRAY_SIZE(pgraph_depth_func_vk_map));
        depth_stencil.depthCompareOp = pgraph_depth_func_vk_map[depth_func];
    }

    if (stencil_test) {
        depth_stencil.stencilTestEnable = VK_TRUE;
        uint32_t stencil_func = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1),
                                         NV_PGRAPH_CONTROL_1_STENCIL_FUNC);
        uint32_t stencil_ref = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1),
                                        NV_PGRAPH_CONTROL_1_STENCIL_REF);
        uint32_t mask_read = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1),
                                      NV_PGRAPH_CONTROL_1_STENCIL_MASK_READ);
        uint32_t mask_write = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1),
                                       NV_PGRAPH_CONTROL_1_STENCIL_MASK_WRITE);
        uint32_t op_fail = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_2),
                                    NV_PGRAPH_CONTROL_2_STENCIL_OP_FAIL);
        uint32_t op_zfail = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_2),
                                     NV_PGRAPH_CONTROL_2_STENCIL_OP_ZFAIL);
        uint32_t op_zpass = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_2),
                                     NV_PGRAPH_CONTROL_2_STENCIL_OP_ZPASS);

        assert(stencil_func < ARRAY_SIZE(pgraph_stencil_func_vk_map));
        assert(op_fail < ARRAY_SIZE(pgraph_stencil_op_vk_map));
        assert(op_zfail < ARRAY_SIZE(pgraph_stencil_op_vk_map));
        assert(op_zpass < ARRAY_SIZE(pgraph_stencil_op_vk_map));

        depth_stencil.front.failOp = pgraph_stencil_op_vk_map[op_fail];
        depth_stencil.front.passOp = pgraph_stencil_op_vk_map[op_zpass];
        depth_stencil.front.depthFailOp = pgraph_stencil_op_vk_map[op_zfail];
        depth_stencil.front.compareOp =
            pgraph_stencil_func_vk_map[stencil_func];
        depth_stencil.front.compareMask = mask_read;
        depth_stencil.front.writeMask = mask_write;
        depth_stencil.front.reference = stencil_ref;
        depth_stencil.back = depth_stencil.front;
    }

    VkColorComponentFlags write_mask = 0;
    if (control_0 & NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE)
        write_mask |= VK_COLOR_COMPONENT_R_BIT;
    if (control_0 & NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE)
        write_mask |= VK_COLOR_COMPONENT_G_BIT;
    if (control_0 & NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE)
        write_mask |= VK_COLOR_COMPONENT_B_BIT;
    if (control_0 & NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE)
        write_mask |= VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendAttachmentState color_blend_attachment = {
        .colorWriteMask = write_mask,
    };

    float blend_constant[4] = { 0, 0, 0, 0 };

    if (pgraph_reg_r(pg, NV_PGRAPH_BLEND) & NV_PGRAPH_BLEND_EN) {
        color_blend_attachment.blendEnable = VK_TRUE;

        uint32_t sfactor =
            GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_BLEND), NV_PGRAPH_BLEND_SFACTOR);
        uint32_t dfactor =
            GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_BLEND), NV_PGRAPH_BLEND_DFACTOR);
        assert(sfactor < ARRAY_SIZE(pgraph_blend_factor_vk_map));
        assert(dfactor < ARRAY_SIZE(pgraph_blend_factor_vk_map));
        color_blend_attachment.srcColorBlendFactor =
            pgraph_blend_factor_vk_map[sfactor];
        color_blend_attachment.dstColorBlendFactor =
            pgraph_blend_factor_vk_map[dfactor];
        color_blend_attachment.srcAlphaBlendFactor =
            pgraph_blend_factor_vk_map[sfactor];
        color_blend_attachment.dstAlphaBlendFactor =
            pgraph_blend_factor_vk_map[dfactor];

        uint32_t equation =
            GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_BLEND), NV_PGRAPH_BLEND_EQN);
        assert(equation < ARRAY_SIZE(pgraph_blend_equation_vk_map));

        color_blend_attachment.colorBlendOp =
            pgraph_blend_equation_vk_map[equation];
        color_blend_attachment.alphaBlendOp =
            pgraph_blend_equation_vk_map[equation];

        uint32_t blend_color = pgraph_reg_r(pg, NV_PGRAPH_BLENDCOLOR);
        pgraph_argb_pack32_to_rgba_float(blend_color, blend_constant);
    }

    VkPipelineColorBlendStateCreateInfo color_blending = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable = VK_FALSE,
        .logicOp = VK_LOGIC_OP_COPY,
        .attachmentCount = r->color_binding ? 1 : 0,
        .pAttachments = r->color_binding ? &color_blend_attachment : NULL,
        .blendConstants[0] = blend_constant[0],
        .blendConstants[1] = blend_constant[1],
        .blendConstants[2] = blend_constant[2],
        .blendConstants[3] = blend_constant[3],
    };

    VkDynamicState dynamic_states[3] = { VK_DYNAMIC_STATE_VIEWPORT,
                                         VK_DYNAMIC_STATE_SCISSOR };
    int num_dynamic_states = 2;

    snode->has_dynamic_line_width =
        (r->enabled_physical_device_features.wideLines == VK_TRUE) &&
        (r->shader_binding->state.geom.polygon_front_mode == POLY_MODE_LINE ||
         r->shader_binding->state.geom.primitive_mode == PRIM_TYPE_LINES ||
         r->shader_binding->state.geom.primitive_mode == PRIM_TYPE_LINE_LOOP ||
         r->shader_binding->state.geom.primitive_mode == PRIM_TYPE_LINE_STRIP);
    if (snode->has_dynamic_line_width) {
        dynamic_states[num_dynamic_states++] = VK_DYNAMIC_STATE_LINE_WIDTH;
    }

    VkPipelineDynamicStateCreateInfo dynamic_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = num_dynamic_states,
        .pDynamicStates = dynamic_states,
    };

    // FIXME: Dither
    // if (pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0) &
    //         NV_PGRAPH_CONTROL_0_DITHERENABLE))
    // FIXME: point size
    // FIXME: Edge Antialiasing
    // bool anti_aliasing = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_ANTIALIASING),
    // NV_PGRAPH_ANTIALIASING_ENABLE);
    // if (!anti_aliasing && pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER) &
    //                           NV_PGRAPH_SETUPRASTER_LINESMOOTHENABLE) {
    // FIXME: VK_EXT_line_rasterization
    // }

    // if (!anti_aliasing && pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER) &
    //                           NV_PGRAPH_SETUPRASTER_POLYSMOOTHENABLE) {
    // FIXME: No direct analog. Just do it with MSAA.
    // }


    VkPipelineLayoutCreateInfo pipeline_layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &r->descriptor_set_layout,
    };

    VkPushConstantRange push_constant_range;
    if (r->use_push_constants_for_uniform_attrs) {
        int num_uniform_attributes =
            __builtin_popcount(r->shader_binding->state.vsh.uniform_attrs);
        if (num_uniform_attributes) {
            push_constant_range = (VkPushConstantRange){
                .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
                .offset = 0,
                // FIXME: Minimize push constants
                .size = num_uniform_attributes * 4 * sizeof(float),
            };
            pipeline_layout_info.pushConstantRangeCount = 1;
            pipeline_layout_info.pPushConstantRanges = &push_constant_range;
        }
    }

    VkPipelineLayout layout;
    VK_CHECK(vkCreatePipelineLayout(r->device, &pipeline_layout_info, NULL,
                                    &layout));

    VkGraphicsPipelineCreateInfo pipeline_create_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = num_active_shader_stages,
        .pStages = shader_stages,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = r->zeta_binding ? &depth_stencil : NULL,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_state,
        .layout = layout,
        .renderPass = get_render_pass(r, &key.render_pass_state),
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
    };
    VkPipeline pipeline;
    VK_CHECK(vkCreateGraphicsPipelines(r->device, r->vk_pipeline_cache, 1,
                                       &pipeline_create_info, NULL, &pipeline));

    snode->pipeline = pipeline;
    snode->layout = layout;
    snode->render_pass = pipeline_create_info.renderPass;
    snode->draw_time = pg->draw_time;

    r->pipeline_binding = snode;
    r->pipeline_binding_changed = true;

    NV2A_VK_DGROUP_END();
}

static void push_vertex_attr_values(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (!r->use_push_constants_for_uniform_attrs) {
        return;
    }

    // FIXME: Partial updates

    float values[NV2A_VERTEXSHADER_ATTRIBUTES][4];
    int num_uniform_attrs = 0;

    pgraph_get_inline_values(pg, r->shader_binding->state.vsh.uniform_attrs,
                             values, &num_uniform_attrs);

    if (num_uniform_attrs > 0) {
        vkCmdPushConstants(r->command_buffer, r->pipeline_binding->layout,
                           VK_SHADER_STAGE_VERTEX_BIT, 0,
                           num_uniform_attrs * 4 * sizeof(float),
                           &values);
    }
}

static void bind_descriptor_sets(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    assert(r->descriptor_set_index >= 1);

    /* frame-overlap S1: sets live in the recording slot's own pool, so a
     * retired slot's pool reset can never invalidate a set another slot's
     * in-flight submission still references. index >= 1 under overlap
     * additionally proves the set was written since THIS slot's
     * acquisition (acquisition zeroes the index). OFF: the shared array,
     * exactly as today. */
    VkDescriptorSet *set = r->frame_overlap ?
        &r->current_slot->ov_desc_sets[r->descriptor_set_index - 1] :
        &r->descriptor_sets[r->descriptor_set_index - 1];

    vkCmdBindDescriptorSets(r->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            r->pipeline_binding->layout, 0, 1,
                            set, 0,
                            NULL);
}

static void begin_query(PGRAPHVkState *r)
{
    assert(r->in_command_buffer);
    if (r->batch_qreset_enabled) {
        /* batch-qreset: queries are recorded INSIDE the render pass — only
         * the RESET must stay outside, and it was hoisted to the region-entry
         * batch reset. begin_draw opens the pass before calling here, and the
         * reset was necessarily recorded already (it is consumed by
         * pgraph_vk_begin_command_buffer, which ran before any recording into
         * this CB — command/submission order puts it before this begin). */
        assert(r->in_render_pass);
        assert(!r->query_pool_reset_pending);
    } else {
        assert(!r->in_render_pass);
    }
    assert(!r->query_in_flight);

    // FIXME: We should handle this. Make the query buffer bigger, but at least
    // flush current queries.
    /* Window capacity: the allocated window [query_epoch_base, +num) may
     * extend at most to the current region's end. begin_pre_draw's backstop
     * resolves before this is hit. OFF: origin == base == 0 and this is
     * literally the historical num_queries_in_flight < query_region_size
     * assert; async-reports slides base forward as prefixes are consumed
     * (origin 0, region == whole pool); batch-qreset allocates monotonically
     * within the frame's ping-pong region (origin == the region base). */
    assert(r->query_epoch_base + (uint32_t)r->num_queries_in_flight -
               r->query_region_origin <
           r->query_region_size);

    /* Absolute occlusion-pool index for this query: the current window's
     * base plus its within-window offset. query_epoch_base is 0
     * when OFF, so idx == num_queries_in_flight, exactly as upstream. */
    uint32_t idx = r->query_epoch_base + (uint32_t)r->num_queries_in_flight;

    /* fix-query-flicker: the async resolve path (XEMU_ASYNC_QUERIES) submits
     * the open batch WITHOUT wait_all_slots and reads only this epoch's query
     * range with WAIT_BIT (reports.c), so up to CB_FENCE_RING_SIZE earlier
     * submissions can still be in flight when the next resolve-epoch re-resets
     * this same occlusion-pool index — num_queries_in_flight restarts at 0 on
     * every resolve, so every epoch reuses indices 0,1,2,... in the single
     * shared pool. vkCmdResetQueryPool orders only against commands referencing
     * the same query in submission order, and vkGetQueryPoolResults is not a
     * synchronization command, so cross-submission slot reuse was safe ONLY
     * because the sync path's wait_all_slots retired every prior submission
     * before the next epoch's reset. Restore that guarantee, narrowed to the
     * one owning submission: before resetting the slot, wait the submission
     * that last recorded a query here so the reset never lands while a
     * submission that used the slot is unretired. The prior epoch's WAIT_BIT
     * resolve already drained that submission's queries, so the owner is
     * (near-)retired and this is a no-op in steady state — counted via
     * QUERY_REUSE_WAIT only when it genuinely blocks, so the async win is
     * preserved. Gated on async: with it off, every resolve is preceded by
     * wait_all_slots (all prior submissions retired) and this would be a pure
     * no-op, so it is skipped to keep the OFF path byte-identical.
     *
     * The deferred-epoch ring (query_ring_enabled) reuses each pool index only
     * every query_num_regions epochs (disjoint ranges), so this owner fence is
     * a guaranteed no-op there — but it is armed as the belt-and-braces backstop
     * behind the pool-wrap invariant (serve_reports_ring), which is what makes a
     * cross-epoch vkCmdResetQueryPool safe without a preceding wait_all_slots.
     *
     * frame-overlap S1 additionally arms it: any general overlap mode MUST
     * own-fence the slot reset or a reset races an in-flight reader — the
     * ISS-B16 see-through-walls class (audit §2h says this must precede any
     * real overlap). Under S1-alone every resolve still follows a full
     * finish, so the wait is a guaranteed no-op (QUERY_REUSE_WAIT stays 0 —
     * armed detector) and only the owner map is maintained; the map is
     * allocated unconditionally in pgraph_vk_init_reports, so this arm is
     * NULL-safe in every mode.
     *
     * Armed likewise for the S3/S4 modes (both reuse indices strictly less
     * often than legacy async, so steady state stays a no-op):
     *   - async-reports (S3): index reuse happens only after a FULL apply
     *     rewound the base to the region origin, and every full apply sits
     *     inside a finish whose wait_all_slots already retired every possible
     *     owner (prefix applies advance the base and never reuse).
     *   - batch-qreset (S4): an index is reused only at region RE-ENTRY (two
     *     flips later, or a backstop swap), and both region entries follow a
     *     full finish. The fence matters if a later stage removes the
     *     flip-time finish (Lane 1 composition): it then really gates the
     *     re-entry batch reset against a still-in-flight prior user. */
    if (r->async_queries_enabled || r->query_ring_enabled ||
        r->frame_overlap ||
        r->async_reports_enabled || r->batch_qreset_enabled) {
        uint32_t owner = r->query_index_submit[idx];
        if (owner != SURFACE_NO_WRITE) {
            struct CbFenceSlot *os =
                &r->cb_fence_ring[owner % CB_FENCE_RING_SIZE];
            if (os->submit_index == owner &&
                vkGetFenceStatus(r->device, os->fence) != VK_SUCCESS) {
                nv2a_profile_inc_counter(NV2A_PROF_QUERY_REUSE_WAIT);
            }
            pgraph_vk_wait_for_submission(r, owner);
        }
        /* Record this open batch as the slot's owner for the next epoch. Its
         * eventual submit index is r->submit_count: begin_command_buffer
         * acquired ring[submit_count % CB_FENCE_RING_SIZE] without advancing
         * submit_count, and submit_batch stamps submit_index = submit_count. */
        r->query_index_submit[idx] = r->submit_count;
    }

    nv2a_profile_inc_counter(NV2A_PROF_QUERY);
    if (!r->batch_qreset_enabled) {
        /* Lazy per-slot reset — the very thing that forces begin_query out of
         * the render pass when OFF. batch-qreset replaces it with the
         * region-entry batch reset (begin_command_buffer); recording it here
         * would also be a straight VUID (reset inside a render pass). */
        vkCmdResetQueryPool(r->command_buffer, r->query_pool, idx, 1);
    }
    vkCmdBeginQuery(r->command_buffer, r->query_pool, idx,
                    VK_QUERY_CONTROL_PRECISE_BIT);

    r->query_in_flight = true;
    r->new_query_needed = false;
    r->num_queries_in_flight++;
}

static void end_query(PGRAPHVkState *r)
{
    assert(r->in_command_buffer);
    if (r->batch_qreset_enabled) {
        /* In-pass end: a query begun inside a render pass must end in the
         * SAME subpass (single-subpass passes here). Invariant maintained by
         * every pass-closing path ending the in-flight query first, so
         * query_in_flight implies the query was begun in the CURRENT pass. */
        assert(r->in_render_pass);
    } else {
        assert(!r->in_render_pass);
    }
    assert(r->query_in_flight);

    /* Close the query begin_query opened: current window base plus its
     * last within-window index. query_epoch_base is 0 when OFF (upstream).
     * Note the absolute index is invariant under an async-reports prefix
     * rebase (base += take, num -= take), so a Phase C apply landing while
     * this query is open cannot retarget it. */
    vkCmdEndQuery(r->command_buffer, r->query_pool,
                  r->query_epoch_base + (uint32_t)r->num_queries_in_flight - 1);
    r->query_in_flight = false;
}

static void sync_staging_buffer(PGRAPHState *pg, VkCommandBuffer cmd,
                                int index_src, int index_dst)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    StorageBuffer *b_src = &r->storage_buffers[index_src];
    StorageBuffer *b_dst = &r->storage_buffers[index_dst];

    /* narrow-fence delta-sync: copy only what was appended since the last
     * submit ([synced_offset, buffer_offset)). buffer_offset is NOT reset
     * here — that happens in the finish recycle phase — so appends made
     * after an early submit land beyond synced_offset and never overlap the
     * device-buffer region an in-flight submission is still copying into or
     * reading from. Device-buffer offsets equal staging offsets (draws bind
     * the append offset directly), so srcOffset == dstOffset. */
    if (b_src->buffer_offset == b_src->synced_offset) {
        return;
    }

    VkDeviceSize delta_offset = b_src->synced_offset;
    VkDeviceSize delta_size = b_src->buffer_offset - b_src->synced_offset;

    VkBufferCopy copy_region = {
        .srcOffset = delta_offset,
        .dstOffset = delta_offset,
        .size = delta_size,
    };
    vkCmdCopyBuffer(cmd, b_src->buffer, b_dst->buffer, 1, &copy_region);

    VkAccessFlags dst_access_mask;
    VkPipelineStageFlags dst_stage_mask;

    switch (index_dst) {
    case BUFFER_INDEX:
        dst_access_mask = VK_ACCESS_INDEX_READ_BIT;
        dst_stage_mask = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
        break;
    case BUFFER_VERTEX_INLINE:
        dst_access_mask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
        dst_stage_mask = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
        break;
    case BUFFER_UNIFORM:
        dst_access_mask = VK_ACCESS_UNIFORM_READ_BIT;
        dst_stage_mask = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
        break;
    default:
        assert(0);
        break;
    }

    VkBufferMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = dst_access_mask,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = b_dst->buffer,
        .offset = delta_offset,
        .size = delta_size,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, dst_stage_mask, 0,
                         0, NULL, 1, &barrier, 0, NULL);

    b_src->synced_offset = b_src->buffer_offset;
}

static void flush_memory_buffer(PGRAPHState *pg, VkCommandBuffer cmd)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VK_CHECK(vmaFlushAllocation(
        r->allocator, r->storage_buffers[BUFFER_VERTEX_RAM].allocation, 0,
        VK_WHOLE_SIZE));

    VkBufferMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = r->storage_buffers[BUFFER_VERTEX_RAM].buffer,
        .offset = 0,
        .size = VK_WHOLE_SIZE,
    };

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 0, NULL, 1,
                         &barrier, 0, NULL);
}

static void begin_render_pass(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    assert(r->in_command_buffer);
    assert(!r->in_render_pass);

    nv2a_profile_inc_counter(NV2A_PROF_PIPELINE_RENDERPASSES);

    /* loc-graphics-research (zero-copy): the cached render passes declare
     * ATTACHMENT_OPTIMAL initial layouts. A zero-copy texture bind may have
     * left a bound target in SHADER_READ_ONLY_OPTIMAL — restore it (also the
     * write-after-read barrier for the borrowed sampling). No-op otherwise. */
    if (r->color_binding &&
        r->color_binding->image_layout !=
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        pgraph_vk_surface_transition(pg, r->command_buffer, r->color_binding,
                                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }
    if (r->zeta_binding &&
        r->zeta_binding->image_layout !=
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
        pgraph_vk_surface_transition(
            pg, r->command_buffer, r->zeta_binding,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    }

    unsigned int vp_width = pg->surface_binding_dim.width,
                 vp_height = pg->surface_binding_dim.height;
    pgraph_apply_scaling_factor(pg, &vp_width, &vp_height);

    assert(r->framebuffer_index > 0);

    VkRenderPassBeginInfo render_pass_begin_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = r->render_pass,
        .framebuffer = r->framebuffers[r->framebuffer_index - 1],
        .renderArea.extent.width = vp_width,
        .renderArea.extent.height = vp_height,
        .clearValueCount = 0,
        .pClearValues = NULL,
    };
    vkCmdBeginRenderPass(r->command_buffer, &render_pass_begin_info,
                         VK_SUBPASS_CONTENTS_INLINE);
    r->in_render_pass = true;

}

static void end_render_pass(PGRAPHVkState *r)
{
    if (r->in_render_pass) {
        /* batch-qreset enforcement point: a query begun in a subpass must be
         * ended in that subpass before vkCmdEndRenderPass — every pass-closing
         * path (submit_batch, ensure_not_in_render_pass, begin_draw's clear /
         * interval-close branches) ends the in-flight query first. A genuine
         * pass break thus ends the query in-pass, and the next zpass draw
         * opens a fresh slot in the next pass (the accumulate-across-slots
         * split machinery makes that semantically free). */
        assert(!r->batch_qreset_enabled || !r->query_in_flight);
        vkCmdEndRenderPass(r->command_buffer);
        r->in_render_pass = false;
    }
}

const enum NV2A_PROF_COUNTERS_ENUM finish_reason_to_counter_enum[] = {
    [VK_FINISH_REASON_VERTEX_BUFFER_DIRTY] = NV2A_PROF_FINISH_VERTEX_BUFFER_DIRTY,
    [VK_FINISH_REASON_SURFACE_CREATE] = NV2A_PROF_FINISH_SURFACE_CREATE,
    [VK_FINISH_REASON_SURFACE_DOWN] = NV2A_PROF_FINISH_SURFACE_DOWN,
    [VK_FINISH_REASON_NEED_BUFFER_SPACE] = NV2A_PROF_FINISH_NEED_BUFFER_SPACE,
    [VK_FINISH_REASON_FRAMEBUFFER_DIRTY] = NV2A_PROF_FINISH_FRAMEBUFFER_DIRTY,
    [VK_FINISH_REASON_PRESENTING] = NV2A_PROF_FINISH_PRESENTING,
    [VK_FINISH_REASON_FLIP_STALL] = NV2A_PROF_FINISH_FLIP_STALL,
    [VK_FINISH_REASON_FLUSH] = NV2A_PROF_FINISH_FLUSH,
    [VK_FINISH_REASON_STALLED] = NV2A_PROF_FINISH_STALLED,
    [VK_FINISH_REASON_QUERY_ASYNC] = NV2A_PROF_QUERY_ASYNC_RESOLVE,
};

void pgraph_vk_wait_all_slots(PGRAPHVkState *r)
{
    for (int i = 0; i < CB_FENCE_RING_SIZE; i++) {
        struct CbFenceSlot *slot = &r->cb_fence_ring[i];
        if (slot->submit_index == SURFACE_NO_WRITE) {
            continue; /* never used */
        }
        if (vkGetFenceStatus(r->device, slot->fence) == VK_SUCCESS) {
            continue; /* already signaled */
        }
        VK_CHECK(vkWaitForFences(r->device, 1, &slot->fence, VK_TRUE,
                                 UINT64_MAX));
    }
}

void pgraph_vk_wait_for_submission(PGRAPHVkState *r, uint32_t idx)
{
    if (idx == SURFACE_NO_WRITE) {
        return; /* nothing recorded */
    }
    struct CbFenceSlot *slot = &r->cb_fence_ring[idx % CB_FENCE_RING_SIZE];
    if (slot->submit_index != idx) {
        return; /* pushed out of the ring => long complete */
    }
    if (vkGetFenceStatus(r->device, slot->fence) == VK_SUCCESS) {
        return;
    }
    VK_CHECK(vkWaitForFences(r->device, 1, &slot->fence, VK_TRUE, UINT64_MAX));
}

void pgraph_vk_submit_batch(PGRAPHState *pg, FinishReason finish_reason)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (!r->in_command_buffer) {
        return;
    }

    nv2a_profile_inc_counter(finish_reason_to_counter_enum[finish_reason]);

    if (r->batch_qreset_enabled) {
        /* In-pass queries: close the query INSIDE the pass, then the pass —
         * the reverse of the OFF order (where queries live outside passes).
         * Also keeps vkEndCommandBuffer legal (no active query). */
        if (r->query_in_flight) {
            end_query(r);
        }
        end_render_pass(r);
    } else {
        if (r->in_render_pass) {
            end_render_pass(r);
        }
        if (r->query_in_flight) {
            end_query(r);
        }
    }
    VK_CHECK(vkEndCommandBuffer(r->command_buffer));

    struct CbFenceSlot *slot = r->current_slot;
    assert(slot == &r->cb_fence_ring[r->submit_count % CB_FENCE_RING_SIZE]);

    /* Stamp the absolute occlusion-pool range this submission carries: indices
     * [query_epoch_base + queries_submitted, query_epoch_base +
     * num_queries_in_flight) recorded since the previous submit. end_query above
     * does not touch num_queries_in_flight (only begin_query bumps it), so it
     * already counts every query in this batch. query_epoch_base is 0 when OFF.
     * queries_submitted is a within-epoch watermark; the deferred-epoch ring
     * uses the QueryEpoch descriptors (not these fields) to locate a retired
     * epoch, so query_first/query_count remain purely descriptive here. */
    assert((uint32_t)r->num_queries_in_flight >= r->queries_submitted);
    slot->query_first = r->query_epoch_base + r->queries_submitted;
    slot->query_count = (uint32_t)r->num_queries_in_flight - r->queries_submitted;
    r->queries_submitted = (uint32_t)r->num_queries_in_flight;

    /* Record the staging delta-sync directly into this slot's aux command
     * buffer (the single-time helpers own a separate dedicated CB). The
     * synced deltas are append-only regions no in-flight submission
     * touches, so no cross-submission hazard. bitmap_clear and the offset
     * resets happen only in the finish recycle phase. */
    VkCommandBufferBeginInfo aux_begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VK_CHECK(vkBeginCommandBuffer(slot->aux_cb, &aux_begin_info));
    sync_staging_buffer(pg, slot->aux_cb, BUFFER_INDEX_STAGING, BUFFER_INDEX);
    sync_staging_buffer(pg, slot->aux_cb, BUFFER_VERTEX_INLINE_STAGING,
                        BUFFER_VERTEX_INLINE);
    sync_staging_buffer(pg, slot->aux_cb, BUFFER_UNIFORM_STAGING,
                        BUFFER_UNIFORM);
    flush_memory_buffer(pg, slot->aux_cb);
    VK_CHECK(vkEndCommandBuffer(slot->aux_cb));

    /* Two-element submit: aux (staging copies) signals the slot's semaphore,
     * main waits it. Per-slot semaphores — a binary semaphore may not carry
     * two in-flight signal/wait pairs. The slot's fence was retired by
     * begin_command_buffer's acquisition; reset it just before submit. */
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo submit_infos[] = {
        {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &slot->aux_cb,
            .signalSemaphoreCount = 1,
            .pSignalSemaphores = &slot->sem,
        },
        {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &slot->cb,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &slot->sem,
            .pWaitDstStageMask = &wait_stage,
        }
    };
    nv2a_profile_inc_counter(NV2A_PROF_QUEUE_SUBMIT);
    uint32_t submit_idx = r->submit_count;
    assert(submit_idx != SURFACE_NO_WRITE);
    vkResetFences(r->device, 1, &slot->fence);
    VK_CHECK(vkQueueSubmit(r->queue, ARRAY_SIZE(submit_infos), submit_infos,
                           slot->fence));
    slot->submit_index = submit_idx;

    /* frame-overlap S1: stamp the per-slot resource state this submission
     * carries, for retire-time bookkeeping. OFF: nothing here runs. */
    if (r->frame_overlap) {
        /* Descriptor sets consumed since acquisition => the slot's pool
         * needs a reset + re-alloc once this submission retires. */
        if (r->descriptor_set_index > 0) {
            slot->ov_desc_dirty = true;
        }

        /* Framebuffers superseded during this recording move to the slot's
         * pending-destroy list; the CURRENT binding (framebuffers[index-1])
         * survives in the live window — the next slot's render passes may
         * keep using it, and single-queue in-order completion guarantees
         * any slot list it later lands on retires no earlier than every
         * submission that referenced it. Bounded: acquisition drained this
         * list, and one recording creates < ARRAY_SIZE(framebuffers)
         * entries (the array-full path forces a finish first). */
        if (r->framebuffer_index > 1) {
            int move = r->framebuffer_index - 1;
            assert(slot->ov_num_pending_fb + move <=
                   (int)ARRAY_SIZE(slot->ov_pending_fb));
            for (int i = 0; i < move; i++) {
                slot->ov_pending_fb[slot->ov_num_pending_fb++] =
                    r->framebuffers[i];
                nv2a_profile_inc_counter(NV2A_PROF_OVERLAP_FB_DEFER);
            }
            r->framebuffers[0] = r->framebuffers[move];
            r->framebuffer_index = 1;
        }

        /* Staging-arena high-water marks at submit (== synced_offset after
         * the delta-sync above): the reclaim boundaries a later stage's
         * watermark-advance consumes. */
        static const int ov_arena_ids[3] = { BUFFER_INDEX_STAGING,
                                             BUFFER_VERTEX_INLINE_STAGING,
                                             BUFFER_UNIFORM_STAGING };
        for (int i = 0; i < 3; i++) {
            slot->ov_arena_end[i] =
                r->storage_buffers[ov_arena_ids[i]].buffer_offset;
        }

        /* The slot now belongs to the GPU; the next acquisition (of the
         * NEXT ring slot) must do its own fence wait + retire-work. */
        r->ov_slot_acquired = false;
    }

    r->submit_count = submit_idx + 1;
    r->in_command_buffer = false;
    r->recycle_pending = true;
}

void pgraph_vk_finish(PGRAPHState *pg, FinishReason finish_reason)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    /* pfifo: the GPU round-trip (submit_batch + wait_all_slots + recycle +
     * report resolve). The depth-guarded leaf folds this into the `submit`
     * bucket ONLY when it is the outermost pfifo leaf — i.e. a mid-pushbuffer
     * mandatory flush (NEED_BUFFER_SPACE / VERTEX_BUFFER_DIRTY / ...). Finishes
     * nested inside a download/upload/query/present leaf are absorbed by that
     * enclosing op instead of double-counting here. pfifo thread only. */
    NV2A_PHASE_LEAF(PF_LEAF_SUBMIT);

    assert(!r->in_draw);
    assert(r->debug_depth == 0);

    pgraph_vk_submit_batch(pg, finish_reason);
    pgraph_vk_wait_all_slots(r);

    if (r->recycle_pending) {
        r->recycle_pending = false;

        /* S0 overlap counters (always-on, behavior-neutral): sample the
         * staging arenas and the descriptor bump index at their peak — just
         * before this recycle resets them. The ..._KB counters accumulate
         * per-recycle consumption into a per-frame total (each recycle
         * restarts the bump at 0, so the sum of pre-reset offsets IS the
         * frame's demand — the uniform-arena go/no-go number); the _PK_
         * counters keep the frame max via a direct max-write. Per-frame
         * semantics come from the flip-time counter reset
         * (nv2a_profile_flip_stall). docs/vr/present-wall-plan.md S0. */
        {
            static const int arena_ids[3] = { BUFFER_INDEX_STAGING,
                                              BUFFER_VERTEX_INLINE_STAGING,
                                              BUFFER_UNIFORM_STAGING };
            static const int arena_sum[3] = { NV2A_PROF_ARENA_IDX_KB,
                                              NV2A_PROF_ARENA_VTX_KB,
                                              NV2A_PROF_ARENA_UNI_KB };
            static const int arena_peak[3] = { NV2A_PROF_ARENA_IDX_PK_KB,
                                               NV2A_PROF_ARENA_VTX_PK_KB,
                                               NV2A_PROF_ARENA_UNI_PK_KB };
            for (int i = 0; i < 3; i++) {
                int kb = (int)(r->storage_buffers[arena_ids[i]].buffer_offset /
                               1024);
                g_nv2a_stats.frame_working.counters[arena_sum[i]] += kb;
                if (kb > g_nv2a_stats.frame_working.counters[arena_peak[i]]) {
                    g_nv2a_stats.frame_working.counters[arena_peak[i]] = kb;
                }
            }
            if (r->descriptor_set_index >
                g_nv2a_stats.frame_working.counters[NV2A_PROF_DESC_MAX]) {
                g_nv2a_stats.frame_working.counters[NV2A_PROF_DESC_MAX] =
                    r->descriptor_set_index;
            }
        }

        r->descriptor_set_index = 0;

        if (r->frame_overlap) {
            /* frame-overlap S1: the decomposed recycle. The wait_all_slots
             * above retired every slot, so under S1-alone this always takes
             * the all-retired branch: every slot's deferred retire-work runs
             * (its pending framebuffers destroyed, its consumed descriptor
             * pool reset) and then the same full reset as OFF — end state
             * identical, only the bookkeeping shape changed. The all-retired
             * check stays DYNAMIC so a later stage that reaches recycle with
             * slots still in flight defers the reset instead of stomping
             * bytes an in-flight aux copy or draw still reads (audit §2a);
             * arena reclaim then simply waits for the next full drain — the
             * existing NEED_BUFFER_SPACE finishes are the capacity backstop.
             * OVERLAP_RECYCLE_DEFER must read 0 under S1-alone (armed
             * detector: nonzero means a finish stopped draining). */
            bool all_retired = true;
            for (int i = 0; i < CB_FENCE_RING_SIZE; i++) {
                struct CbFenceSlot *slot = &r->cb_fence_ring[i];
                if (slot->submit_index == SURFACE_NO_WRITE) {
                    continue; /* never used */
                }
                if (vkGetFenceStatus(r->device, slot->fence) != VK_SUCCESS) {
                    all_retired = false;
                    continue;
                }
                overlap_process_slot_retire(pg, slot);
            }

            if (all_retired) {
                destroy_framebuffers(pg);

                /* All submissions retired: reset the delta-synced staging
                 * arenas and clear the vertex-RAM upload map. The bitmap's
                 * "era" is tied to TRUE full drains (audit §2n): clearing
                 * it while an older slot still flies would reintroduce the
                 * exact WAR race it exists to catch. */
                bitmap_clear(r->uploaded_bitmap, 0, r->bitmap_size);
                const int ov_arenas[] = { BUFFER_INDEX_STAGING,
                                          BUFFER_VERTEX_INLINE_STAGING,
                                          BUFFER_UNIFORM_STAGING };
                for (size_t i = 0; i < ARRAY_SIZE(ov_arenas); i++) {
                    r->storage_buffers[ov_arenas[i]].buffer_offset = 0;
                    r->storage_buffers[ov_arenas[i]].synced_offset = 0;
                }
                /* Watermarks below the reset are meaningless; zero them so
                 * a stale mark can never be mistaken for a live boundary. */
                for (int i = 0; i < CB_FENCE_RING_SIZE; i++) {
                    for (int a = 0; a < 3; a++) {
                        r->cb_fence_ring[i].ov_arena_end[a] = 0;
                    }
                }
                nv2a_profile_inc_counter(NV2A_PROF_OVERLAP_RECYCLE_FULL);
            } else {
                nv2a_profile_inc_counter(NV2A_PROF_OVERLAP_RECYCLE_DEFER);
            }
        } else {
            destroy_framebuffers(pg);

            /* All submissions retired: reset the delta-synced staging arenas
             * and the vertex-RAM upload map for the next era. */
            bitmap_clear(r->uploaded_bitmap, 0, r->bitmap_size);
            const int arenas[] = { BUFFER_INDEX_STAGING,
                                   BUFFER_VERTEX_INLINE_STAGING,
                                   BUFFER_UNIFORM_STAGING };
            for (size_t i = 0; i < ARRAY_SIZE(arenas); i++) {
                r->storage_buffers[arenas[i]].buffer_offset = 0;
                r->storage_buffers[arenas[i]].synced_offset = 0;
            }
        }

        // Periodically check memory budget
        const int max_num_submits_before_budget_update = 5;
        if (finish_reason == VK_FINISH_REASON_FLIP_STALL ||
            (r->submit_count - r->allocator_last_submit_index) >
                max_num_submits_before_budget_update) {

            // VMA queries budget via vmaSetCurrentFrameIndex
            vmaSetCurrentFrameIndex(r->allocator, r->submit_count);
            r->allocator_last_submit_index = r->submit_count;
            pgraph_vk_check_memory_budget(pg);
        }
    }

    NV2AState *d = container_of(pg, NV2AState, pgraph);
    pgraph_vk_process_pending_reports_internal(d);

    if (r->batch_qreset_enabled &&
        finish_reason == VK_FINISH_REASON_FLIP_STALL) {
        /* Stage S4 frame boundary: ping-pong to the other query region. Safe
         * exactly here — the wait_all_slots above retired every submission
         * and the apply just above emptied the report queue and consumed any
         * async-reports marker, so both regions are quiescent (the swap
         * asserts all of it). LANE-1 COMPOSITION NOTE: if a later stage makes
         * the flip submit-without-wait (removing this finish), the swap must
         * move to that flip path and force-complete any marker still
         * referencing the INCOMING region first; the begin_query owner fence
         * then backstops the re-entry reset against in-flight prior users. */
        pgraph_vk_swap_query_region(r);
    }

    pgraph_vk_compute_finish_complete(r);
}

void pgraph_vk_begin_command_buffer(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    assert(!r->in_command_buffer);

    if (r->frame_overlap) {
        /* frame-overlap S1: same slot selection + fence wait, factored so
         * pgraph_vk_update_descriptor_sets can acquire EARLY (its writes
         * must target this slot's descriptor pool), plus the previous
         * occupant's retire-work. Idempotent; a no-op if the slot was
         * already acquired since the last submit. */
        pgraph_vk_overlap_acquire_slot(pg);
    } else {
        /* narrow-fence: acquire the ring slot for submission index
         * r->submit_count. The slot's previous occupant (from
         * CB_FENCE_RING_SIZE submissions ago) must be retired before
         * vkBeginCommandBuffer implicitly resets the slot's command buffers.
         * This is the natural backpressure bounding in-flight depth. */
        if (r->submit_count == SURFACE_NO_WRITE) {
            r->submit_count++; /* never use the sentinel as a real index */
        }
        struct CbFenceSlot *slot =
            &r->cb_fence_ring[r->submit_count % CB_FENCE_RING_SIZE];
        if (slot->submit_index != SURFACE_NO_WRITE &&
            vkGetFenceStatus(r->device, slot->fence) != VK_SUCCESS) {
            VK_CHECK(vkWaitForFences(r->device, 1, &slot->fence, VK_TRUE,
                                     UINT64_MAX));
        }
        r->current_slot = slot;
        r->command_buffer = slot->cb;
        r->aux_command_buffer = slot->aux_cb;
    }

    VkCommandBufferBeginInfo command_buffer_begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VK_CHECK(vkBeginCommandBuffer(r->command_buffer,
                                  &command_buffer_begin_info));

    if (r->batch_qreset_enabled && r->query_pool_reset_pending) {
        /* Region-entry batched reset (Stage S4): reset the frame's whole
         * ping-pong region ONCE, at the first command buffer after region
         * entry — a fresh CB is outside any render pass by construction, so
         * the reset never fights the pass again. Ordering is free:
         * vkCmdResetQueryPool carries a submission-order execution dependency
         * against prior uses of these queries (so the previous entry's writes
         * complete first), and every begin_query of this entry is recorded
         * after it in this or a later CB. Host reads never race it: reads
         * only target fence-retired slots, and a cross-frame reader (an
         * async-reports marker under the composed Lane-1 design) reads the
         * OTHER region — that isolation is the whole point of the ping-pong. */
        vkCmdResetQueryPool(r->command_buffer, r->query_pool,
                            r->query_region_origin, r->query_region_size);
        r->query_pool_reset_pending = false;
    }

    r->command_buffer_start_time = pg->draw_time;
    r->in_command_buffer = true;
}

// FIXME: Refactor below

void pgraph_vk_ensure_command_buffer(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (!r->in_command_buffer) {
        pgraph_vk_begin_command_buffer(pg);
    }
}

void pgraph_vk_ensure_not_in_render_pass(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (r->batch_qreset_enabled) {
        /* In-pass queries end INSIDE the pass (same-subpass rule); the next
         * zpass draw opens a fresh slot in the next pass. */
        if (r->query_in_flight) {
            end_query(r);
        }
        end_render_pass(r);
    } else {
        end_render_pass(r);
        if (r->query_in_flight) {
            end_query(r);
        }
    }
}

VkCommandBuffer pgraph_vk_begin_nondraw_commands(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    pgraph_vk_ensure_command_buffer(pg);
    pgraph_vk_ensure_not_in_render_pass(pg);
    return r->command_buffer;
}

void pgraph_vk_end_nondraw_commands(PGRAPHState *pg, VkCommandBuffer cmd)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    assert(cmd == r->command_buffer);
}

// FIXME: Add more metrics for determining command buffer 'fullness' and
// conservatively flush. Unfortunately there doesn't appear to be a good
// way to determine what the actual maximum capacity of a command buffer
// is, but we are obviously not supposed to endlessly append to one command
// buffer. For other reasons though (like descriptor set amount, surface
// changes, etc) we do flush often.

static void begin_pre_draw(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    assert(r->color_binding || r->zeta_binding);
    assert(!r->color_binding || r->color_binding->initialized);
    assert(!r->zeta_binding || r->zeta_binding->initialized);

    /* pfifo2: close the ATTR segment (attribute bind + sync/remap) and enter
     * PIPE (texture bind + pipeline lookup/create + render pass). */
    NV2A_PHASE_MSUB(PF_MSUB_PIPE);

    /* Occlusion-query region-exhaustion backstop. The allocated window can
     * extend at most to the current region's end; past that begin_query trips
     * its capacity assert. If this draw will start a visibility query and only
     * one slot remains, resolve now with a conservative full finish — it
     * drains, applies the window and empties it — so the region never
     * overruns. Finish here is safe: it precedes
     * pgraph_vk_ensure_command_buffer below, like create_frame_buffer's and
     * the descriptor-set finishes already in this function. OFF =>
     * query_region_size == the whole 1024-slot pool, the historical bound. For
     * Steel Battalion (~185 queries/frame across ~3 STALLED resolves), the
     * window stays far under any region, so this never fires in practice and
     * leaves the baseline unperturbed. NEED_BUFFER_SPACE (not STALLED) keeps
     * the A/B query counters clean. */
    if (!pg->clearing && pg->zpass_pixel_count_enable &&
        r->query_epoch_base + (uint32_t)r->num_queries_in_flight -
                r->query_region_origin >=
            r->query_region_size - 1) {
        /* Window-relative bound: OFF => base == origin == 0, literally the
         * historical num_queries_in_flight >= query_region_size - 1 check.
         * async-reports: the base slid forward past prefix-applied slots, and
         * this full finish rewinds it to the origin — space reclaimed.
         * batch-qreset: allocation is monotonic (the finish does NOT rewind),
         * so reclaim by entering the other ping-pong region: post-finish both
         * regions are quiescent (all submissions retired, all reports applied,
         * marker consumed) and the incoming region gets its batch reset at the
         * next command buffer. Effective capacity per frame becomes
         * region_size per swap, each costing one full drain — the same
         * self-healing class as the descriptor/buffer backstops (SB peaks
         * ~500 slots/frame against 1023, so this should never fire; counters
         * FINISH_NEED_BUFFER_SPACE would say if it does). */
        pgraph_vk_finish(pg, VK_FINISH_REASON_NEED_BUFFER_SPACE);
        if (r->batch_qreset_enabled) {
            pgraph_vk_swap_query_region(r);
        }
    }

    if (pg->clearing) {
        create_clear_pipeline(pg);
    } else {
        create_pipeline(pg);
    }

    bool render_pass_dirty = r->pipeline_binding->render_pass != r->render_pass;

    if (r->framebuffer_dirty || render_pass_dirty) {
        pgraph_vk_ensure_not_in_render_pass(pg);
    }
    if (render_pass_dirty) {
        r->render_pass = r->pipeline_binding->render_pass;
    }
    if (r->framebuffer_dirty) {
        create_frame_buffer(pg);
        r->framebuffer_dirty = false;
    }
    if (!pg->clearing) {
        /* pfifo2: enter DESC for the descriptor-set update (clears have none). */
        NV2A_PHASE_MSUB(PF_MSUB_DESC);
        pgraph_vk_update_descriptor_sets(pg);
    }
    if (r->framebuffer_index == 0) {
        create_frame_buffer(pg);
    }

    pgraph_vk_ensure_command_buffer(pg);
}

static float clamp_line_width_to_device_limits(PGRAPHState *pg, float width)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    float min_width = r->device_props.limits.lineWidthRange[0];
    float max_width = r->device_props.limits.lineWidthRange[1];
    float granularity = r->device_props.limits.lineWidthGranularity;

    if (granularity != 0.0f) {
        float steps = roundf((width - min_width) / granularity);
        width = min_width + steps * granularity;
    }
    return fminf(fmaxf(min_width, width), max_width);
}

static void begin_draw(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    assert(r->in_command_buffer);

    /* pfifo2: close the ATTR inline-buffer-build segment and enter PIPE for the
     * render-pass begin, pipeline bind, and the descriptor/push command binds
     * (folded into PIPE — they are cheap command-buffer recording). */
    NV2A_PHASE_MSUB(PF_MSUB_PIPE);

    /* S0 rpchurn sub-timer (present-wall plan): the visibility-query
     * bookkeeping below is what breaks render passes at report boundaries
     * (~90% of the ~205 passes/frame — Lane 2 Attack B's target). Carve the
     * end_render_pass + end/begin_query work, and the query-forced
     * begin_render_pass re-open, out of PIPE into the pfifo2 method_rpchurn
     * accumulator via the msub redirect (inert when the phase profiler is
     * off or outside a draw cursor, i.e. clears). rpchurn_reopen remembers
     * that the query path (not a fresh CB / framebuffer change / clear)
     * closed an open pass, so only the re-opens it caused are charged. */
    bool rpchurn_reopen = false;

    // Visibility testing
    if (r->batch_qreset_enabled) {
        /* batch-qreset: a report boundary no longer breaks the pass — close a
         * finished counting interval IN-pass here; the replacement slot is
         * begun (also in-pass) below, after the pass is (re)opened. The pass
         * now ends only for clears (own pass, next branch), surface/format
         * switches, non-draw commands, and submits. A query in flight was
         * begun in the CURRENT pass (see end_query), so the in-pass end is
         * always same-subpass. No rpchurn instrumentation here — this mode
         * eliminates the churn, so method_rpchurn reading ~0 under S4 is
         * itself the validation signal. */
        if (!pg->clearing && pg->zpass_pixel_count_enable) {
            if (r->new_query_needed && r->query_in_flight) {
                end_query(r);
            }
        } else if (r->query_in_flight) {
            end_query(r);
        }
    } else {
        if (!pg->clearing && pg->zpass_pixel_count_enable) {
            if (r->new_query_needed && r->query_in_flight) {
                rpchurn_reopen |= r->in_render_pass;
                NV2A_PHASE_MSUB_REDIRECT(PF_MSUB_RPCHURN);
                end_render_pass(r);
                end_query(r);
            }
            if (!r->query_in_flight) {
                rpchurn_reopen |= r->in_render_pass;
                NV2A_PHASE_MSUB_REDIRECT(PF_MSUB_RPCHURN);
                end_render_pass(r);
                begin_query(r);
            }
        } else if (r->query_in_flight) {
            rpchurn_reopen |= r->in_render_pass && !pg->clearing;
            NV2A_PHASE_MSUB_REDIRECT(PF_MSUB_RPCHURN);
            end_render_pass(r);
            end_query(r);
        }
    }

    if (pg->clearing) {
        end_render_pass(r);
    }

    bool must_bind_pipeline = r->pipeline_binding_changed;

    if (!r->in_render_pass) {
        if (rpchurn_reopen && !pg->clearing) {
            /* Query-forced re-open: the pass was live and only the query
             * bookkeeping above closed it. */
            NV2A_PHASE_MSUB_REDIRECT(PF_MSUB_RPCHURN);
            begin_render_pass(pg);
        } else {
            begin_render_pass(pg);
        }
        must_bind_pipeline = true;
    }

    if (r->batch_qreset_enabled && !pg->clearing &&
        pg->zpass_pixel_count_enable && !r->query_in_flight) {
        begin_query(r); /* in-pass: the pass above is open by now */
    }

    if (must_bind_pipeline) {
        nv2a_profile_inc_counter(NV2A_PROF_PIPELINE_BIND);
        vkCmdBindPipeline(r->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          r->pipeline_binding->pipeline);
        r->pipeline_binding->draw_time = pg->draw_time;

        unsigned int vp_width = pg->surface_binding_dim.width,
                     vp_height = pg->surface_binding_dim.height;
        pgraph_apply_scaling_factor(pg, &vp_width, &vp_height);

        VkViewport viewport = {
            .width = vp_width,
            .height = vp_height,
            .minDepth = 0.0,
            .maxDepth = 1.0,
        };
        vkCmdSetViewport(r->command_buffer, 0, 1, &viewport);

        /* Surface clip */
        /* FIXME: Consider moving to PSH w/ window clip */
        unsigned int xmin = pg->surface_shape.clip_x,
                     ymin = pg->surface_shape.clip_y;

        unsigned int scissor_width = pg->surface_shape.clip_width,
                     scissor_height = pg->surface_shape.clip_height;

        pgraph_apply_anti_aliasing_factor(pg, &xmin, &ymin);
        pgraph_apply_anti_aliasing_factor(pg, &scissor_width, &scissor_height);

        pgraph_apply_scaling_factor(pg, &xmin, &ymin);
        pgraph_apply_scaling_factor(pg, &scissor_width, &scissor_height);

        VkRect2D scissor = {
            .offset.x = xmin,
            .offset.y = ymin,
            .extent.width = scissor_width,
            .extent.height = scissor_height,
        };
        vkCmdSetScissor(r->command_buffer, 0, 1, &scissor);

        if (r->pipeline_binding->has_dynamic_line_width) {
            float line_width =
                clamp_line_width_to_device_limits(pg, pg->surface_scale_factor);
            vkCmdSetLineWidth(r->command_buffer, line_width);
        }
    }

    if (!pg->clearing) {
        bind_descriptor_sets(pg);
        push_vertex_attr_values(pg);
    }

    r->in_draw = true;
}

static void end_draw(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    assert(r->in_command_buffer);
    assert(r->in_render_pass);

    if (pg->clearing) {
        end_render_pass(r);
    }

    r->in_draw = false;
}

void pgraph_vk_draw_end(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    bool mask_alpha = control_0 & NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE;
    bool mask_red = control_0 & NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE;
    bool mask_green = control_0 & NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE;
    bool mask_blue = control_0 & NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE;
    bool color_write = mask_alpha || mask_red || mask_green || mask_blue;
    bool depth_test = control_0 & NV_PGRAPH_CONTROL_0_ZENABLE;
    bool stencil_test =
        pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1) & NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE;
    bool is_nop_draw = !(color_write || depth_test || stencil_test);

    if (is_nop_draw) {
        // FIXME: Check PGRAPH register 0x880.
        // HW uses bit 11 in 0x880 to enable or disable a color/zeta limit
        // check that will raise an exception in the case that a draw should
        // modify the color and/or zeta buffer but the target(s) are masked
        // off. This check only seems to trigger during the fragment
        // processing, it is legal to attempt a draw that is entirely
        // clipped regardless of 0x880. See xemu#635 for context.
        NV2A_VK_DPRINTF("nop draw!\n");
        return;
    }

    pgraph_vk_flush_draw(d);

    pg->draw_time++;
    if (r->color_binding && pgraph_color_write_enabled(pg)) {
        r->color_binding->draw_time = pg->draw_time;
    }
    if (r->zeta_binding && pgraph_zeta_write_enabled(pg)) {
        r->zeta_binding->draw_time = pg->draw_time;
    }

    pgraph_vk_set_surface_dirty(pg, color_write, depth_test || stencil_test);
}

static int compare_memory_sync_requirement_by_addr(const void *p1,
                                                   const void *p2)
{
    const MemorySyncRequirement *l = p1, *r = p2;
    if (l->addr < r->addr)
        return -1;
    if (l->addr > r->addr)
        return 1;
    return 0;
}

static void sync_vertex_ram_buffer(PGRAPHState *pg)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (r->num_vertex_ram_buffer_syncs == 0) {
        return;
    }

    // Align sync requirements to page boundaries
    NV2A_VK_DGROUP_BEGIN("Sync vertex RAM buffer");

    for (int i = 0; i < r->num_vertex_ram_buffer_syncs; i++) {
        NV2A_VK_DPRINTF("Need to sync vertex memory @%" HWADDR_PRIx
                        ", %" HWADDR_PRIx " bytes",
                        r->vertex_ram_buffer_syncs[i].addr,
                        r->vertex_ram_buffer_syncs[i].size);

        hwaddr start_addr =
            r->vertex_ram_buffer_syncs[i].addr & TARGET_PAGE_MASK;
        hwaddr end_addr = r->vertex_ram_buffer_syncs[i].addr +
                          r->vertex_ram_buffer_syncs[i].size;
        end_addr = ROUND_UP(end_addr, TARGET_PAGE_SIZE);

        NV2A_VK_DPRINTF("- %d: %08" HWADDR_PRIx " %zd bytes"
                          " -> %08" HWADDR_PRIx " %zd bytes", i,
                        r->vertex_ram_buffer_syncs[i].addr,
                        r->vertex_ram_buffer_syncs[i].size, start_addr,
                        end_addr - start_addr);

        r->vertex_ram_buffer_syncs[i].addr = start_addr;
        r->vertex_ram_buffer_syncs[i].size = end_addr - start_addr;
    }

    // Sort the requirements in increasing order of addresses
    qsort(r->vertex_ram_buffer_syncs, r->num_vertex_ram_buffer_syncs,
          sizeof(MemorySyncRequirement),
          compare_memory_sync_requirement_by_addr);

    // Merge overlapping/adjacent requests to minimize number of tests
    MemorySyncRequirement merged[16];
    int num_syncs = 1;

    merged[0] = r->vertex_ram_buffer_syncs[0];

    for (int i = 1; i < r->num_vertex_ram_buffer_syncs; i++) {
        MemorySyncRequirement *p = &merged[num_syncs - 1];
        MemorySyncRequirement *t = &r->vertex_ram_buffer_syncs[i];

        if (t->addr <= (p->addr + p->size)) {
            // Merge with previous
            hwaddr p_end_addr = p->addr + p->size;
            hwaddr t_end_addr = t->addr + t->size;
            hwaddr new_end_addr = MAX(p_end_addr, t_end_addr);
            p->size = new_end_addr - p->addr;
        } else {
            merged[num_syncs++] = *t;
        }
    }

    if (num_syncs < r->num_vertex_ram_buffer_syncs) {
        NV2A_VK_DPRINTF("Reduced to %d sync checks", num_syncs);
    }

    for (int i = 0; i < num_syncs; i++) {
        hwaddr addr = merged[i].addr;
        VkDeviceSize size = merged[i].size;

        NV2A_VK_DPRINTF("- %d: %08"HWADDR_PRIx" %zd bytes", i, addr, size);

        if (memory_region_test_and_clear_dirty(d->vram, addr, size,
                                               DIRTY_MEMORY_NV2A)) {
            NV2A_VK_DPRINTF("Memory dirty. Synchronizing...");
            /* S0 overlap counter: steady-state dirty-range syncs at the draw
             * path. Under overlap each one is a WAR wait_all_slots + in-place
             * memcpy — the audit's #1 overlap-killer frequency unknown
             * (docs/vr/overlap-resource-audit.md §2b, wait-site #8). */
            nv2a_profile_inc_counter(NV2A_PROF_VTX_RAM_WAR_WAIT);
            /* narrow-fence WAR gate: this memcpys into the host-visible
             * vertex-RAM buffer at absolute offsets an in-flight
             * submission's draws may still read. Retire all in-flight work
             * first (no-op when nothing is in flight). */
            pgraph_vk_wait_all_slots(r);
            pgraph_vk_update_vertex_ram_buffer(pg, addr, d->vram_ptr + addr,
                                               size);
        }
    }

    r->num_vertex_ram_buffer_syncs = 0;

    NV2A_VK_DGROUP_END();
}

void pgraph_vk_clear_surface(NV2AState *d, uint32_t parameter)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    nv2a_profile_inc_counter(NV2A_PROF_CLEAR);

    bool write_color = (parameter & NV097_CLEAR_SURFACE_COLOR);
    bool write_zeta =
        (parameter & (NV097_CLEAR_SURFACE_Z | NV097_CLEAR_SURFACE_STENCIL));

    pg->clearing = true;

    // FIXME: If doing a full surface clear, mark the surface for full clear
    // and we can just do the clear as part of the surface load.
    pgraph_vk_surface_update(d, true, write_color, write_zeta);

    SurfaceBinding *binding = r->color_binding ?: r->zeta_binding;
    if (!binding) {
        /* Nothing bound to clear */
        pg->clearing = false;
        return;
    }

    r->clear_parameter = parameter;

    uint32_t clearrectx = pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTX);
    uint32_t clearrecty = pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTY);

    unsigned int xmin = GET_MASK(clearrectx, NV_PGRAPH_CLEARRECTX_XMIN);
    unsigned int xmax = GET_MASK(clearrectx, NV_PGRAPH_CLEARRECTX_XMAX);
    unsigned int ymin = GET_MASK(clearrecty, NV_PGRAPH_CLEARRECTY_YMIN);
    unsigned int ymax = GET_MASK(clearrecty, NV_PGRAPH_CLEARRECTY_YMAX);

    NV2A_VK_DGROUP_BEGIN("CLEAR min=(%d,%d) max=(%d,%d)%s%s", xmin, ymin, xmax,
                         ymax, write_color ? " color" : "",
                         write_zeta ? " zeta" : "");

    begin_pre_draw(pg);
    pgraph_vk_begin_debug_marker(r, r->command_buffer,
        RGBA_BLUE, "Clear %08" HWADDR_PRIx,
        binding->vram_addr);
    begin_draw(pg);

    // FIXME: What does hardware do when min >= max?
    // FIXME: What does hardware do when min >= surface size?
    xmin = MIN(xmin, binding->width - 1);
    ymin = MIN(ymin, binding->height - 1);
    xmax = MAX(xmin, MIN(xmax, binding->width - 1));
    ymax = MAX(ymin, MIN(ymax, binding->height - 1));

    unsigned int scissor_width = MAX(0, xmax - xmin + 1);
    unsigned int scissor_height = MAX(0, ymax - ymin + 1);

    pgraph_apply_anti_aliasing_factor(pg, &xmin, &ymin);
    pgraph_apply_anti_aliasing_factor(pg, &scissor_width, &scissor_height);

    pgraph_apply_scaling_factor(pg, &xmin, &ymin);
    pgraph_apply_scaling_factor(pg, &scissor_width, &scissor_height);

    VkClearRect clear_rect = {
        .rect = {
            .offset = { .x = xmin, .y = ymin },
            .extent = { .width = scissor_width, .height = scissor_height },
        },
        .baseArrayLayer = 0,
        .layerCount = 1,
    };

    int num_attachments = 0;
    VkClearAttachment attachments[2];

    if (write_color && r->color_binding) {
        const bool clear_all_color_channels =
            (parameter & NV097_CLEAR_SURFACE_COLOR) ==
            (NV097_CLEAR_SURFACE_R | NV097_CLEAR_SURFACE_G |
             NV097_CLEAR_SURFACE_B | NV097_CLEAR_SURFACE_A);

        if (clear_all_color_channels) {
            attachments[num_attachments] = (VkClearAttachment){
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .colorAttachment = 0,
            };
            pgraph_get_clear_color(
                pg, attachments[num_attachments].clearValue.color.float32);
            num_attachments++;
        } else {
            float blend_constants[4];
            pgraph_get_clear_color(pg, blend_constants);
            vkCmdSetScissor(r->command_buffer, 0, 1, &clear_rect.rect);
            vkCmdSetBlendConstants(r->command_buffer, blend_constants);
            vkCmdDraw(r->command_buffer, 3, 1, 0, 0);
        }
    }

    if (write_zeta && r->zeta_binding) {
        int stencil_value = 0;
        float depth_value = 1.0;
        pgraph_get_clear_depth_stencil_value(pg, &depth_value, &stencil_value);

        VkImageAspectFlags aspect = 0;
        if (parameter & NV097_CLEAR_SURFACE_Z) {
            aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
        }
        if ((parameter & NV097_CLEAR_SURFACE_STENCIL) &&
            (r->zeta_binding->host_fmt.aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) {
            aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
        }

        attachments[num_attachments++] = (VkClearAttachment){
            .aspectMask = aspect,
            .clearValue.depthStencil.depth = depth_value,
            .clearValue.depthStencil.stencil = stencil_value,
        };
    }

    if (num_attachments) {
        vkCmdClearAttachments(r->command_buffer, num_attachments, attachments,
                              1, &clear_rect);
    }
    end_draw(pg);
    pgraph_vk_end_debug_marker(r, r->command_buffer);

    pg->clearing = false;

    pgraph_vk_set_surface_dirty(pg, write_color, write_zeta);

    NV2A_VK_DGROUP_END();
}

#if 0
static void pgraph_vk_debug_attrs(NV2AState *d)
{
    for (int vertex_idx = 0; vertex_idx < pg->draw_arrays_count[i]; vertex_idx++) {
        NV2A_VK_DGROUP_BEGIN("Vertex %d+%d", pg->draw_arrays_start[i], vertex_idx);
        for (int attr_idx = 0; attr_idx < NV2A_VERTEXSHADER_ATTRIBUTES; attr_idx++) {
            VertexAttribute *attr = &pg->vertex_attributes[attr_idx];
            if (attr->count) {
                char *p = (char *)d->vram_ptr + r->attribute_offsets[attr_idx] + (pg->draw_arrays_start[i] + vertex_idx) * attr->stride;
                NV2A_VK_DGROUP_BEGIN("Attribute %d data at %tx", attr_idx, (ptrdiff_t)(p - (char*)d->vram_ptr));
                for (int count_idx = 0; count_idx < attr->count; count_idx++) {
                    switch (attr->format) {
                    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F:
                        NV2A_VK_DPRINTF("[%d] %f", count_idx, *(float*)p);
                        p += sizeof(float);
                        break;
                    default:
                        assert(0);
                        break;
                    }
                }
                NV2A_VK_DGROUP_END();
            }
        }
        NV2A_VK_DGROUP_END();
    }
}
#endif

static void bind_vertex_buffer(PGRAPHState *pg, uint16_t inline_map,
                               VkDeviceSize offset)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (r->num_active_vertex_binding_descriptions == 0) {
        return;
    }

    VkBuffer buffers[NV2A_VERTEXSHADER_ATTRIBUTES];
    VkDeviceSize offsets[NV2A_VERTEXSHADER_ATTRIBUTES];

    for (int i = 0; i < r->num_active_vertex_binding_descriptions; i++) {
        int attr_idx = r->vertex_attribute_descriptions[i].location;
        int buffer_idx = (inline_map & (1 << attr_idx)) ? BUFFER_VERTEX_INLINE :
                                                          BUFFER_VERTEX_RAM;
        buffers[i] = r->storage_buffers[buffer_idx].buffer;
        offsets[i] = offset + r->vertex_attribute_offsets[attr_idx];
    }

    vkCmdBindVertexBuffers(r->command_buffer, 0,
                           r->num_active_vertex_binding_descriptions, buffers,
                           offsets);
}

static void bind_inline_vertex_buffer(PGRAPHState *pg, VkDeviceSize offset)
{
    bind_vertex_buffer(pg, 0xffff, offset);
}

void pgraph_vk_set_surface_dirty(PGRAPHState *pg, bool color, bool zeta)
{
    NV2A_DPRINTF("pgraph_set_surface_dirty(%d, %d) -- %d %d\n", color, zeta,
                 pgraph_color_write_enabled(pg), pgraph_zeta_write_enabled(pg));

    PGRAPHVkState *r = pg->vk_renderer_state;

    /* FIXME: Does this apply to CLEARs too? */
    color = color && pgraph_color_write_enabled(pg);
    zeta = zeta && pgraph_zeta_write_enabled(pg);
    pg->surface_color.draw_dirty |= color;
    pg->surface_zeta.draw_dirty |= zeta;

    /* narrow-fence: stamp the writing submission. submit_count is the index
     * the still-open command buffer's submission will receive (finish
     * increments after submit). Covers clears too — they route here and
     * never bump draw_time. */
    if (r->color_binding) {
        r->color_binding->draw_dirty |= color;
        r->color_binding->frame_time = pg->frame_time;
        r->color_binding->cleared = false;
        if (color) {
            r->color_binding->last_write_submit = r->submit_count;
            /* GPU is ahead of RAM again: guest reads must intercept until
             * the next download re-quiesces (read fast path off). */
            mem_access_callback_set_read_quiesced(r->color_binding->access_cb,
                                                  false);
        }
    }

    if (r->zeta_binding) {
        r->zeta_binding->draw_dirty |= zeta;
        r->zeta_binding->frame_time = pg->frame_time;
        r->zeta_binding->cleared = false;
        if (zeta) {
            r->zeta_binding->last_write_submit = r->submit_count;
            mem_access_callback_set_read_quiesced(r->zeta_binding->access_cb,
                                                  false);
        }
    }
}

static bool ensure_buffer_space(PGRAPHState *pg, int index, VkDeviceSize size)
{
    if (!pgraph_vk_buffer_has_space_for(pg, index, size, 1)) {
        pgraph_vk_finish(pg, VK_FINISH_REASON_NEED_BUFFER_SPACE);
        return true;
    }

    return false;
}

static void get_size_and_count_for_format(VkFormat fmt, size_t *size, size_t *count)
{
    static const struct {
        size_t size;
        size_t count;
    } table[] = {
        [VK_FORMAT_R8_UNORM] =              { 1, 1 },
        [VK_FORMAT_R8G8_UNORM] =            { 1, 2 },
        [VK_FORMAT_R8G8B8_UNORM] =          { 1, 3 },
        [VK_FORMAT_R8G8B8A8_UNORM] =        { 1, 4 },
        [VK_FORMAT_R16_SNORM] =             { 2, 1 },
        [VK_FORMAT_R16G16_SNORM] =          { 2, 2 },
        [VK_FORMAT_R16G16B16_SNORM] =       { 2, 3 },
        [VK_FORMAT_R16G16B16A16_SNORM] =    { 2, 4 },
        [VK_FORMAT_R16_SSCALED] =           { 2, 1 },
        [VK_FORMAT_R16G16_SSCALED] =        { 2, 2 },
        [VK_FORMAT_R16G16B16_SSCALED] =     { 2, 3 },
        [VK_FORMAT_R16G16B16A16_SSCALED] =  { 2, 4 },
        [VK_FORMAT_R32_SFLOAT] =            { 4, 1 },
        [VK_FORMAT_R32G32_SFLOAT] =         { 4, 2 },
        [VK_FORMAT_R32G32B32_SFLOAT] =      { 4, 3 },
        [VK_FORMAT_R32G32B32A32_SFLOAT] =   { 4, 4 },
        [VK_FORMAT_R32_SINT] =              { 4, 1 },
    };

    assert(fmt < ARRAY_SIZE(table));
    assert(table[fmt].size);

    *size = table[fmt].size;
    *count = table[fmt].count;
}

typedef struct VertexBufferRemap {
    uint16_t attributes;
    size_t buffer_space_required;
    struct {
        VkDeviceAddress offset;
        VkDeviceSize old_stride;
        VkDeviceSize new_stride;
    } map[NV2A_VERTEXSHADER_ATTRIBUTES];
} VertexBufferRemap;

static VertexBufferRemap remap_unaligned_attributes(PGRAPHState *pg,
                                                    uint32_t num_vertices)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VertexBufferRemap remap = {0};

    VkDeviceAddress output_offset = 0;

    for (int attr_id = 0; attr_id < NV2A_VERTEXSHADER_ATTRIBUTES; attr_id++) {
        int desc_loc = r->vertex_attribute_to_description_location[attr_id];
        if (desc_loc < 0) {
            continue;
        }

        VkVertexInputBindingDescription *desc =
            &r->vertex_binding_descriptions[desc_loc];
        VkVertexInputAttributeDescription *attr =
            &r->vertex_attribute_descriptions[desc_loc];

        size_t element_size, element_count;
        get_size_and_count_for_format(attr->format, &element_size, &element_count);

        bool offset_valid =
            (r->vertex_attribute_offsets[attr_id] % element_size == 0);
        bool stride_valid = (desc->stride % element_size == 0);

        if (offset_valid && stride_valid) {
            continue;
        }

        remap.attributes |= 1 << attr_id;
        remap.map[attr_id].offset = ROUND_UP(output_offset, element_size);
        remap.map[attr_id].old_stride = desc->stride;
        remap.map[attr_id].new_stride = element_size * element_count;

        // fprintf(stderr,
        //         "attr %02d remapped: "
        //         "%08" HWADDR_PRIx "->%08" HWADDR_PRIx " "
        //         "stride=%d->%zd\n",
        //         attr_id, r->vertex_attribute_offsets[attr_id],
        //         remap.map[attr_id].offset,
        //         remap.map[attr_id].old_stride,
        //         remap.map[attr_id].new_stride);

        output_offset =
            remap.map[attr_id].offset + remap.map[attr_id].new_stride * num_vertices;
        desc->stride = remap.map[attr_id].new_stride;
    }

    remap.buffer_space_required = output_offset;

    // reserve space
    if (remap.attributes) {
        StorageBuffer *buffer = &r->storage_buffers[BUFFER_VERTEX_INLINE_STAGING];
        VkDeviceSize starting_offset = ROUND_UP(buffer->buffer_offset, 16);
        size_t total_space_required =
            (starting_offset - buffer->buffer_offset) + remap.buffer_space_required;
        ensure_buffer_space(pg, BUFFER_VERTEX_INLINE_STAGING, total_space_required);
        buffer->buffer_offset = ROUND_UP(buffer->buffer_offset, 16);
    }

    return remap;
}

static void copy_remapped_attributes_to_inline_buffer(PGRAPHState *pg,
                                                      VertexBufferRemap remap,
                                                      uint32_t start_vertex,
                                                      uint32_t num_vertices)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHVkState *r = pg->vk_renderer_state;
    StorageBuffer *buffer = &r->storage_buffers[BUFFER_VERTEX_INLINE_STAGING];

    if (!remap.attributes) {
        return;
    }

    assert(pgraph_vk_buffer_has_space_for(pg, BUFFER_VERTEX_INLINE_STAGING,
                                          remap.buffer_space_required, 256));

    // FIXME: SIMD memcpy
    // FIXME: Caching
    // FIXME: Account for only what is drawn
    assert(start_vertex == 0);
    assert(buffer->mapped);

    // Copy vertex data
    for (int attr_id = 0; attr_id < NV2A_VERTEXSHADER_ATTRIBUTES; attr_id++) {
        if (!(remap.attributes & (1 << attr_id))) {
            continue;
        }

        VkDeviceSize attr_buffer_offset =
            buffer->buffer_offset + remap.map[attr_id].offset;

        uint8_t *out_ptr = buffer->mapped + attr_buffer_offset;
        uint8_t *in_ptr = d->vram_ptr + r->vertex_attribute_offsets[attr_id];

        for (int vertex_id = 0; vertex_id < num_vertices; vertex_id++) {
            memcpy(out_ptr, in_ptr, remap.map[attr_id].new_stride);
            out_ptr += remap.map[attr_id].new_stride;
            in_ptr += remap.map[attr_id].old_stride;
        }

        r->vertex_attribute_offsets[attr_id] = attr_buffer_offset;
    }


    buffer->buffer_offset += remap.buffer_space_required;
}

void pgraph_vk_flush_draw(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (!(r->color_binding || r->zeta_binding)) {
        NV2A_VK_DPRINTF("No binding present!!!\n");
        return;
    }

    /* pfifo2: open the per-draw method-category cursor. It starts in ATTR
     * (attribute binding is first); begin_pre_draw/create_pipeline/begin_draw
     * switch it to PIPE/CONST/DESC, and the NV2A_PHASE_MSUB(DRAW) marks below
     * switch it to DRAW for the vkCmdDraw region. Auto-closes on every return.
     * Each begin/end/switch is one clock read behind an inline OFF gate. */
    NV2A_PHASE_MSUB_SCOPE(PF_MSUB_ATTR);

    r->num_vertex_ram_buffer_syncs = 0;

    if (pg->draw_arrays_length) {
        NV2A_VK_DGROUP_BEGIN("Draw Arrays");
        nv2a_profile_inc_counter(NV2A_PROF_DRAW_ARRAYS);

        assert(pg->inline_elements_length == 0);
        assert(pg->inline_buffer_length == 0);
        assert(pg->inline_array_length == 0);

        pgraph_vk_bind_vertex_attributes(d, pg->draw_arrays_min_start,
                                         pg->draw_arrays_max_count - 1, false,
                                         0, pg->draw_arrays_max_count - 1);
        uint32_t min_element = INT_MAX;
        uint32_t max_element = 0;
        for (int i = 0; i < pg->draw_arrays_length; i++) {
            min_element = MIN(pg->draw_arrays_start[i], min_element);
            max_element = MAX(max_element, pg->draw_arrays_start[i] + pg->draw_arrays_count[i]);
        }
        sync_vertex_ram_buffer(pg);
        VertexBufferRemap remap = remap_unaligned_attributes(pg, max_element);

        begin_pre_draw(pg);
        NV2A_PHASE_MSUB(PF_MSUB_ATTR); /* inline vertex staging build */
        copy_remapped_attributes_to_inline_buffer(pg, remap, 0, max_element);
        pgraph_vk_begin_debug_marker(r, r->command_buffer, RGBA_BLUE,
                                     "Draw Arrays");
        begin_draw(pg);
        NV2A_PHASE_MSUB(PF_MSUB_DRAW); /* vertex-buffer bind + vkCmdDraw */
        bind_vertex_buffer(pg, remap.attributes, 0);
        for (int i = 0; i < pg->draw_arrays_length; i++) {
            uint32_t start = pg->draw_arrays_start[i],
                     count = pg->draw_arrays_count[i];
            NV2A_VK_DPRINTF("- [%d] Start:%d Count:%d", i, start, count);
            vkCmdDraw(r->command_buffer, count, 1, start, 0);
        }
        end_draw(pg);
        pgraph_vk_end_debug_marker(r, r->command_buffer);

        NV2A_VK_DGROUP_END();
    } else if (pg->inline_elements_length) {
        NV2A_VK_DGROUP_BEGIN("Inline Elements");
        assert(pg->inline_buffer_length == 0);
        assert(pg->inline_array_length == 0);

        nv2a_profile_inc_counter(NV2A_PROF_INLINE_ELEMENTS);

        size_t index_data_size =
            pg->inline_elements_length * sizeof(pg->inline_elements[0]);

        ensure_buffer_space(pg, BUFFER_INDEX_STAGING, index_data_size);

        uint32_t min_element = (uint32_t)-1;
        uint32_t max_element = 0;
        for (int i = 0; i < pg->inline_elements_length; i++) {
            max_element = MAX(pg->inline_elements[i], max_element);
            min_element = MIN(pg->inline_elements[i], min_element);
        }
        pgraph_vk_bind_vertex_attributes(
            d, min_element, max_element, false, 0,
            pg->inline_elements[pg->inline_elements_length - 1]);
        sync_vertex_ram_buffer(pg);
        VertexBufferRemap remap = remap_unaligned_attributes(pg, max_element + 1);

        begin_pre_draw(pg);
        NV2A_PHASE_MSUB(PF_MSUB_ATTR); /* inline vertex + index staging build */
        copy_remapped_attributes_to_inline_buffer(pg, remap, 0, max_element + 1);
        VkDeviceSize buffer_offset = pgraph_vk_update_index_buffer(
            pg, pg->inline_elements, index_data_size);
        pgraph_vk_begin_debug_marker(r, r->command_buffer, RGBA_BLUE,
                                     "Inline Elements");
        begin_draw(pg);
        NV2A_PHASE_MSUB(PF_MSUB_DRAW); /* vertex/index bind + vkCmdDrawIndexed */
        bind_vertex_buffer(pg, remap.attributes, 0);
        vkCmdBindIndexBuffer(r->command_buffer,
                             r->storage_buffers[BUFFER_INDEX].buffer,
                             buffer_offset, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(r->command_buffer, pg->inline_elements_length, 1, 0, 0,
                         0);
        end_draw(pg);
        pgraph_vk_end_debug_marker(r, r->command_buffer);

        NV2A_VK_DGROUP_END();
    } else if (pg->inline_buffer_length) {
        NV2A_VK_DGROUP_BEGIN("Inline Buffer");
        nv2a_profile_inc_counter(NV2A_PROF_INLINE_BUFFERS);
        assert(pg->inline_array_length == 0);

        size_t vertex_data_size = pg->inline_buffer_length * sizeof(float) * 4;
        void *data[NV2A_VERTEXSHADER_ATTRIBUTES];
        size_t sizes[NV2A_VERTEXSHADER_ATTRIBUTES];
        size_t offset = 0;

        pgraph_vk_bind_vertex_attributes_inline(d);
        for (int i = 0; i < r->num_active_vertex_attribute_descriptions; i++) {
            int attr_index = r->vertex_attribute_descriptions[i].location;

            VertexAttribute *attr = &pg->vertex_attributes[attr_index];
            r->vertex_attribute_offsets[attr_index] = offset;

            data[i] = attr->inline_buffer;
            sizes[i] = vertex_data_size;

            attr->inline_buffer_populated = false;
            offset += vertex_data_size;
        }
        ensure_buffer_space(pg, BUFFER_VERTEX_INLINE_STAGING, offset);

        begin_pre_draw(pg);
        NV2A_PHASE_MSUB(PF_MSUB_ATTR); /* inline vertex staging build */
        VkDeviceSize buffer_offset = pgraph_vk_update_vertex_inline_buffer(
            pg, data, sizes, r->num_active_vertex_attribute_descriptions);
        pgraph_vk_begin_debug_marker(r, r->command_buffer, RGBA_BLUE,
                                     "Inline Buffer");
        begin_draw(pg);
        NV2A_PHASE_MSUB(PF_MSUB_DRAW); /* inline vertex bind + vkCmdDraw */
        bind_inline_vertex_buffer(pg, buffer_offset);
        vkCmdDraw(r->command_buffer, pg->inline_buffer_length, 1, 0, 0);
        end_draw(pg);
        pgraph_vk_end_debug_marker(r, r->command_buffer);

        NV2A_VK_DGROUP_END();
    } else if (pg->inline_array_length) {
        NV2A_VK_DGROUP_BEGIN("Inline Array");
        nv2a_profile_inc_counter(NV2A_PROF_INLINE_ARRAYS);

        VkDeviceSize inline_array_data_size = pg->inline_array_length * 4;
        ensure_buffer_space(pg, BUFFER_VERTEX_INLINE_STAGING,
                               inline_array_data_size);

        unsigned int offset = 0;
        for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
            VertexAttribute *attr = &pg->vertex_attributes[i];
            if (attr->count == 0) {
                continue;
            }

            /* FIXME: Double check */
            offset = ROUND_UP(offset, attr->size);
            attr->inline_array_offset = offset;
            NV2A_DPRINTF("bind inline attribute %d size=%d, count=%d\n", i,
                         attr->size, attr->count);
            offset += attr->size * attr->count;
            offset = ROUND_UP(offset, attr->size);
        }

        unsigned int vertex_size = offset;
        unsigned int index_count = pg->inline_array_length * 4 / vertex_size;

        NV2A_DPRINTF("draw inline array %d, %d\n", vertex_size, index_count);
        pgraph_vk_bind_vertex_attributes(d, 0, index_count - 1, true,
                                         vertex_size, index_count - 1);

        begin_pre_draw(pg);
        NV2A_PHASE_MSUB(PF_MSUB_ATTR); /* inline array staging build */
        void *inline_array_data = pg->inline_array;
        VkDeviceSize buffer_offset = pgraph_vk_update_vertex_inline_buffer(
            pg, &inline_array_data, &inline_array_data_size, 1);
        pgraph_vk_begin_debug_marker(r, r->command_buffer, RGBA_BLUE,
                                     "Inline Array");
        begin_draw(pg);
        NV2A_PHASE_MSUB(PF_MSUB_DRAW); /* inline vertex bind + vkCmdDraw */
        bind_inline_vertex_buffer(pg, buffer_offset);
        vkCmdDraw(r->command_buffer, index_count, 1, 0, 0);
        end_draw(pg);
        pgraph_vk_end_debug_marker(r, r->command_buffer);
        NV2A_VK_DGROUP_END();
    } else {
        NV2A_VK_DPRINTF("EMPTY NV097_SET_BEGIN_END");
        NV2A_UNCONFIRMED("EMPTY NV097_SET_BEGIN_END");
    }
}
