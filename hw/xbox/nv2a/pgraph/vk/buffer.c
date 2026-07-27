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

static void create_buffer(PGRAPHState *pg, StorageBuffer *buffer)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkBufferCreateInfo buffer_create_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = buffer->buffer_size,
        .usage = buffer->usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VK_CHECK(vmaCreateBuffer(r->allocator, &buffer_create_info,
                             &buffer->alloc_info, &buffer->buffer,
                             &buffer->allocation, NULL));
}

static void destroy_buffer(PGRAPHState *pg, StorageBuffer *buffer)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vmaDestroyBuffer(r->allocator, buffer->buffer, buffer->allocation);
    buffer->buffer = VK_NULL_HANDLE;
    buffer->allocation = VK_NULL_HANDLE;
}

/* S5 in-batch uploads: resolve the toggle once (getenv-once, M-5 state
 * line). XEMU_NO_INBATCH_UPLOADS=1 force-off wins over
 * XEMU_INBATCH_UPLOADS=1; default OFF. */
static bool resolve_inbatch_uploads_toggle(void)
{
    static int resolved = -1;
    if (resolved < 0) {
        if (getenv("XEMU_NO_INBATCH_UPLOADS")) {
            resolved = 0;
        } else {
            resolved = getenv("XEMU_INBATCH_UPLOADS") ? 1 : 0;
        }
        fprintf(stderr, "inbatch-uploads: %s\n", resolved ? "ON" : "OFF");
    }
    return resolved == 1;
}

/* Empty the staging ring. ONLY legal when every registered consumer has
 * retired — i.e. immediately after a pgraph_vk_finish (which submits the
 * open batch and waits ALL ring slots) or at (re)init. */
void pgraph_vk_staging_ring_reset(PGRAPHVkState *r)
{
    r->staging_ring.head = 0;
    r->staging_ring.tail = 0;
    r->staging_ring.pending_first = 0;
    r->staging_ring.pending_count = 0;
}

/* Pop retired regions off the FIFO and advance the reclaim tail. A region
 * is retired when its consuming submission's fence has signaled (or its
 * ring slot identity shows it long complete — the same test
 * pgraph_vk_wait_for_submission uses, minus the wait). Regions registered
 * against a submission that has not been SUBMITTED yet (the open command
 * buffer: index >= r->submit_count) are never reclaimable; FIFO order
 * means everything behind them is at least as new, so stop there. */
static void staging_ring_reclaim(PGRAPHVkState *r)
{
    while (r->staging_ring.pending_count > 0) {
        struct StagingRingRegion *reg =
            &r->staging_ring.pending[r->staging_ring.pending_first];
        uint32_t idx = reg->submit_index;

        if (idx != SURFACE_NO_WRITE) { /* sentinel = retired synchronously */
            if (idx >= r->submit_count) {
                break; /* consumer not submitted yet (open CB) */
            }
            struct CbFenceSlot *slot =
                &r->cb_fence_ring[idx % CB_FENCE_RING_SIZE];
            if (slot->submit_index == idx &&
                vkGetFenceStatus(r->device, slot->fence) != VK_SUCCESS) {
                break; /* still in flight */
            }
        }

        r->staging_ring.tail = reg->end;
        r->staging_ring.pending_first =
            (r->staging_ring.pending_first + 1) % STAGING_RING_MAX_PENDING;
        r->staging_ring.pending_count--;
    }
    if (r->staging_ring.pending_count == 0) {
        /* Empty: renormalize for maximal contiguity. */
        r->staging_ring.head = 0;
        r->staging_ring.tail = 0;
        r->staging_ring.pending_first = 0;
    }
}

/* Allocate `size` bytes of BUFFER_STAGING_SRC from the upload staging ring
 * and register them against `consumer_submit_index` (r->submit_count for a
 * copy recorded into the open command buffer; SURFACE_NO_WRITE for a
 * transient single-time consumer that will fence-wait before returning).
 * On success *offset receives the physical byte offset into
 * BUFFER_STAGING_SRC. Returns false when the ring cannot fit the request
 * (pending consumers still in flight, or the FIFO is full) — the caller
 * must then fall back to the blocking path AFTER a full finish + ring
 * reset. Never waits. pfifo thread only. */
bool pgraph_vk_staging_ring_alloc(PGRAPHState *pg, VkDeviceSize size,
                                  VkDeviceSize *offset,
                                  uint32_t consumer_submit_index)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    assert(r->inbatch_uploads);
    assert(consumer_submit_index == SURFACE_NO_WRITE ||
           (r->in_command_buffer &&
            consumer_submit_index == r->submit_count));

    staging_ring_reclaim(r);

    uint64_t cap = r->staging_ring.capacity;
    uint64_t need = ROUND_UP((uint64_t)size, STAGING_RING_ALIGN);
    if (need == 0 || need > cap) {
        return false;
    }

    /* A region may not straddle the physical wrap boundary (the copy needs
     * contiguous bytes): pad the head to the boundary if it would. The pad
     * is folded into this region so reclaim frees it with it. */
    uint64_t begin = r->staging_ring.head;
    uint64_t head = begin;
    uint64_t phys = head % cap;
    if (phys + need > cap) {
        head += cap - phys;
    }
    if (head + need - r->staging_ring.tail > cap) {
        return false; /* ring full */
    }
    uint64_t end = head + need;

    /* Register [begin, end): coalesce into the previous entry when it has
     * the same consumer and is contiguous (the common
     * many-uploads-one-batch case), else append. */
    struct StagingRingRegion *last = NULL;
    if (r->staging_ring.pending_count > 0) {
        int last_i = (r->staging_ring.pending_first +
                      r->staging_ring.pending_count - 1) %
                     STAGING_RING_MAX_PENDING;
        last = &r->staging_ring.pending[last_i];
    }
    if (last && last->submit_index == consumer_submit_index &&
        last->end == begin) {
        last->end = end;
    } else {
        if (r->staging_ring.pending_count >= STAGING_RING_MAX_PENDING) {
            return false; /* FIFO full — treat as ring full */
        }
        int slot_i = (r->staging_ring.pending_first +
                      r->staging_ring.pending_count) %
                     STAGING_RING_MAX_PENDING;
        r->staging_ring.pending[slot_i] = (struct StagingRingRegion){
            .begin = begin,
            .end = end,
            .submit_index = consumer_submit_index,
        };
        r->staging_ring.pending_count++;
    }

    r->staging_ring.head = end;
    *offset = STAGING_RING_RESERVE + (head % cap);
    return true;
}

void pgraph_vk_init_buffers(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    // FIXME: Profile buffer sizes

    VmaAllocationCreateInfo host_alloc_create_info = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                 VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT,
    };
    VmaAllocationCreateInfo device_alloc_create_info = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT
    };

    r->storage_buffers[BUFFER_STAGING_DST] = (StorageBuffer){
        .alloc_info = host_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .buffer_size = 4096 * 4096 * 4,
    };

    r->storage_buffers[BUFFER_STAGING_SRC] = (StorageBuffer){
        .alloc_info = host_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .buffer_size = r->storage_buffers[BUFFER_STAGING_DST].buffer_size,
    };

    r->storage_buffers[BUFFER_COMPUTE_DST] = (StorageBuffer){
        .alloc_info = device_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .buffer_size = (1024 * 10) * (1024 * 10) * 8,
    };

    r->storage_buffers[BUFFER_COMPUTE_SRC] = (StorageBuffer){
        .alloc_info = device_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .buffer_size = r->storage_buffers[BUFFER_COMPUTE_DST].buffer_size,
    };

    r->storage_buffers[BUFFER_INDEX] = (StorageBuffer){
        .alloc_info = device_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        .buffer_size = sizeof(pg->inline_elements) * 100,
    };

    r->storage_buffers[BUFFER_INDEX_STAGING] = (StorageBuffer){
        .alloc_info = host_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .buffer_size = r->storage_buffers[BUFFER_INDEX].buffer_size,
    };

    // FIXME: Don't assume that we can render with host mapped buffer
    r->storage_buffers[BUFFER_VERTEX_RAM] = (StorageBuffer){
        .alloc_info = host_alloc_create_info,
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .buffer_size = memory_region_size(d->vram),
    };

    r->bitmap_size = memory_region_size(d->vram) / 4096;
    r->uploaded_bitmap = bitmap_new(r->bitmap_size);
    bitmap_clear(r->uploaded_bitmap, 0, r->bitmap_size);

    r->storage_buffers[BUFFER_VERTEX_INLINE] = (StorageBuffer){
        .alloc_info = device_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .buffer_size = NV2A_VERTEXSHADER_ATTRIBUTES * NV2A_MAX_BATCH_LENGTH *
                       4 * sizeof(float) * 10,
    };

    r->storage_buffers[BUFFER_VERTEX_INLINE_STAGING] = (StorageBuffer){
        .alloc_info = host_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .buffer_size = r->storage_buffers[BUFFER_VERTEX_INLINE].buffer_size,
    };

    r->storage_buffers[BUFFER_UNIFORM] = (StorageBuffer){
        .alloc_info = device_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .buffer_size = 8 * 1024 * 1024,
    };

    r->storage_buffers[BUFFER_UNIFORM_STAGING] = (StorageBuffer){
        .alloc_info = host_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .buffer_size = r->storage_buffers[BUFFER_UNIFORM].buffer_size,
    };

    for (int i = 0; i < BUFFER_COUNT; i++) {
        create_buffer(pg, &r->storage_buffers[i]);
    }

    // FIXME: Add fallback path for device using host mapped memory

    int buffers_to_map[] = { BUFFER_VERTEX_RAM,
                             BUFFER_INDEX_STAGING,
                             BUFFER_VERTEX_INLINE_STAGING,
                             BUFFER_UNIFORM_STAGING };

    for (int i = 0; i < ARRAY_SIZE(buffers_to_map); i++) {
        VK_CHECK(vmaMapMemory(
            r->allocator, r->storage_buffers[buffers_to_map[i]].allocation,
            (void **)&r->storage_buffers[buffers_to_map[i]].mapped));
    }

    /* S5 in-batch uploads: resolve the toggle and (re)initialize the
     * staging ring. OFF leaves every upload on the historical single-time
     * path and the ring untouched. */
    r->inbatch_uploads = resolve_inbatch_uploads_toggle();
    assert(r->storage_buffers[BUFFER_STAGING_SRC].buffer_size >
           STAGING_RING_RESERVE);
    r->staging_ring.capacity =
        r->storage_buffers[BUFFER_STAGING_SRC].buffer_size -
        STAGING_RING_RESERVE;
    pgraph_vk_staging_ring_reset(r);
}

void pgraph_vk_finalize_buffers(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    for (int i = 0; i < BUFFER_COUNT; i++) {
        if (r->storage_buffers[i].mapped) {
            vmaUnmapMemory(r->allocator, r->storage_buffers[i].allocation);
        }
        destroy_buffer(pg, &r->storage_buffers[i]);
    }

    g_free(r->uploaded_bitmap);
    r->uploaded_bitmap = NULL;
}

bool pgraph_vk_buffer_has_space_for(PGRAPHState *pg, int index,
                                    VkDeviceSize size,
                                    VkDeviceAddress alignment)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    StorageBuffer *b = &r->storage_buffers[index];
    return (ROUND_UP(b->buffer_offset, alignment) + size) <= b->buffer_size;
}

VkDeviceSize pgraph_vk_append_to_buffer(PGRAPHState *pg, int index, void **data,
                                        VkDeviceSize *sizes, size_t count,
                                        VkDeviceAddress alignment)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkDeviceSize total_size = 0;
    for (int i = 0; i < count; i++) {
        total_size += sizes[i];
    }
    assert(pgraph_vk_buffer_has_space_for(pg, index, total_size, alignment));

    StorageBuffer *b = &r->storage_buffers[index];
    VkDeviceSize starting_offset = ROUND_UP(b->buffer_offset, alignment);

    assert(b->mapped);

    for (int i = 0; i < count; i++) {
        b->buffer_offset = ROUND_UP(b->buffer_offset, alignment);
        memcpy(b->mapped + b->buffer_offset, data[i], sizes[i]);
        b->buffer_offset += sizes[i];
    }

    return starting_offset;
}
