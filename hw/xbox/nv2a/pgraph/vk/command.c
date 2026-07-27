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
#include "hw/xbox/nv2a/pgraph/phase_timers.h"

static void create_command_pool(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    QueueFamilyIndices indices =
        pgraph_vk_find_queue_families(r->physical_device);

    VkCommandPoolCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = indices.queue_family,
    };
    VK_CHECK(
        vkCreateCommandPool(r->device, &create_info, NULL, &r->command_pool));
}

static void destroy_command_pool(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkDestroyCommandPool(r->device, r->command_pool, NULL);
}

static void create_command_buffers(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    /* narrow-fence: one main + one aux command buffer per ring slot, so up
     * to CB_FENCE_RING_SIZE submissions can be in flight, each recording
     * into its own pair while an earlier pair executes on the GPU. The pool
     * has RESET_COMMAND_BUFFER_BIT; a slot's CBs are reset (via
     * vkBeginCommandBuffer) only after its fence retires that submission. */
    VkCommandBuffer slot_cbs[2 * CB_FENCE_RING_SIZE];
    VkCommandBufferAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = r->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = ARRAY_SIZE(slot_cbs),
    };
    VK_CHECK(vkAllocateCommandBuffers(r->device, &alloc_info, slot_cbs));

    for (int i = 0; i < CB_FENCE_RING_SIZE; i++) {
        r->cb_fence_ring[i].cb = slot_cbs[2 * i];
        r->cb_fence_ring[i].aux_cb = slot_cbs[2 * i + 1];
    }

    /* Current-slot aliases so all recording code compiles untouched; they
     * are repointed per acquired slot in pgraph_vk_begin_command_buffer. */
    r->current_slot = &r->cb_fence_ring[0];
    r->command_buffer = r->cb_fence_ring[0].cb;
    r->aux_command_buffer = r->cb_fence_ring[0].aux_cb;

    /* narrow-fence Part A: dedicated single-time command buffer + fence. */
    VkCommandBufferAllocateInfo single_time_alloc_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = r->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VK_CHECK(vkAllocateCommandBuffers(r->device, &single_time_alloc_info,
                                      &r->single_time_command_buffer));

    VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    };
    VK_CHECK(vkCreateFence(r->device, &fence_info, NULL,
                           &r->single_time_fence));
}

static void destroy_command_buffers(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkDestroyFence(r->device, r->single_time_fence, NULL);
    r->single_time_fence = VK_NULL_HANDLE;

    vkFreeCommandBuffers(r->device, r->command_pool, 1,
                         &r->single_time_command_buffer);
    r->single_time_command_buffer = VK_NULL_HANDLE;

    VkCommandBuffer slot_cbs[2 * CB_FENCE_RING_SIZE];
    for (int i = 0; i < CB_FENCE_RING_SIZE; i++) {
        slot_cbs[2 * i] = r->cb_fence_ring[i].cb;
        slot_cbs[2 * i + 1] = r->cb_fence_ring[i].aux_cb;
    }
    vkFreeCommandBuffers(r->device, r->command_pool, ARRAY_SIZE(slot_cbs),
                         slot_cbs);
    for (int i = 0; i < CB_FENCE_RING_SIZE; i++) {
        r->cb_fence_ring[i].cb = VK_NULL_HANDLE;
        r->cb_fence_ring[i].aux_cb = VK_NULL_HANDLE;
    }

    r->current_slot = NULL;
    r->command_buffer = VK_NULL_HANDLE;
    r->aux_command_buffer = VK_NULL_HANDLE;
}

VkCommandBuffer pgraph_vk_begin_single_time_commands(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    assert(!r->in_aux_command_buffer);
    r->in_aux_command_buffer = true;

    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VK_CHECK(vkBeginCommandBuffer(r->single_time_command_buffer, &begin_info));

    return r->single_time_command_buffer;
}

void pgraph_vk_end_single_time_commands(PGRAPHState *pg, VkCommandBuffer cmd)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    assert(r->in_aux_command_buffer);
    assert(cmd == r->single_time_command_buffer);

    VK_CHECK(vkEndCommandBuffer(cmd));

    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    /* narrow-fence Part A: wait ONLY this copy's fence instead of
     * vkQueueWaitIdle (which would drain the entire queue — re-draining any
     * batches the async flip put in flight). */
    VK_CHECK(vkResetFences(r->device, 1, &r->single_time_fence));
    VK_CHECK(vkQueueSubmit(r->queue, 1, &submit_info, r->single_time_fence));
    nv2a_profile_inc_counter(NV2A_PROF_QUEUE_SUBMIT_AUX);
    /* pfifo2 method_synctex: this vkWaitForFences BLOCKS the pfifo thread until
     * the single-time submit (copy_surface_to_texture / draw-time texture
     * upload) retires on the GPU. When it fires during a draw (texture bind), it
     * is inside the pusher and thus part of `method` — redirect it out of the
     * interrupted category (PIPE) into method_synctex, the deferrable GPU wait.
     * A no-op when no draw cursor is active (e.g. surface-op single-times). */
    {
        NV2A_PHASE_MSUB_REDIRECT(PF_MSUB_SYNCTEX);
        VK_CHECK(vkWaitForFences(r->device, 1, &r->single_time_fence, VK_TRUE,
                                 UINT64_MAX));
    }

    r->in_aux_command_buffer = false;
}

void pgraph_vk_init_command_buffers(PGRAPHState *pg)
{
    create_command_pool(pg);
    create_command_buffers(pg);
}

void pgraph_vk_finalize_command_buffers(PGRAPHState *pg)
{
    destroy_command_buffers(pg);
    destroy_command_pool(pg);
}