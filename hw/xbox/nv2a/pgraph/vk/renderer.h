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

#ifndef HW_XBOX_NV2A_PGRAPH_VK_RENDERER_H
#define HW_XBOX_NV2A_PGRAPH_VK_RENDERER_H

#include "qemu/osdep.h"
#include "qemu/thread.h"
#include "qemu/queue.h"
#include "qemu/lru.h"
#include "hw/hw.h"
#include "hw/xbox/nv2a/nv2a_int.h"
#include "hw/xbox/nv2a/nv2a_regs.h"
#include "hw/xbox/nv2a/pgraph/surface.h"
#include "hw/xbox/nv2a/pgraph/texture.h"
#include "hw/xbox/nv2a/pgraph/glsl/shaders.h"

#include <vulkan/vulkan.h>
#include <glslang/Include/glslang_c_interface.h>
#include <volk.h>
#include <spirv_reflect.h>
#include <vk_mem_alloc.h>

#include "debug.h"
#include "constants.h"
#include "glsl.h"

#define HAVE_EXTERNAL_MEMORY 1

typedef struct QueueFamilyIndices {
    int queue_family;
} QueueFamilyIndices;

typedef struct MemorySyncRequirement {
    hwaddr addr, size;
} MemorySyncRequirement;

typedef struct RenderPassState {
    VkFormat color_format;
    VkFormat zeta_format;
} RenderPassState;

typedef struct RenderPass {
    RenderPassState state;
    VkRenderPass render_pass;
} RenderPass;

typedef struct PipelineKey {
    bool clear;
    RenderPassState render_pass_state;
    ShaderState shader_state;
    uint32_t regs[9];
    VkVertexInputBindingDescription binding_descriptions[NV2A_VERTEXSHADER_ATTRIBUTES];
    VkVertexInputAttributeDescription attribute_descriptions[NV2A_VERTEXSHADER_ATTRIBUTES];
} PipelineKey;

typedef struct PipelineBinding {
    LruNode node;
    PipelineKey key;
    VkPipelineLayout layout;
    VkPipeline pipeline;
    VkRenderPass render_pass;
    unsigned int draw_time;
    bool has_dynamic_line_width;
} PipelineBinding;

enum Buffer {
    BUFFER_STAGING_DST,
    BUFFER_STAGING_SRC,
    BUFFER_COMPUTE_DST,
    BUFFER_COMPUTE_SRC,
    BUFFER_INDEX,
    BUFFER_INDEX_STAGING,
    BUFFER_VERTEX_RAM,
    BUFFER_VERTEX_INLINE,
    BUFFER_VERTEX_INLINE_STAGING,
    BUFFER_UNIFORM,
    BUFFER_UNIFORM_STAGING,
    BUFFER_COUNT
};

typedef struct StorageBuffer {
    VkBuffer buffer;
    VkBufferUsageFlags usage;
    VmaAllocationCreateInfo alloc_info;
    VmaAllocation allocation;
    VkMemoryPropertyFlags properties;
    size_t buffer_offset;
    /* narrow-fence delta-sync: high-water mark of the staging buffer already
     * copied to its device buffer. Each submit copies only
     * [synced_offset, buffer_offset); buffer_offset is reset (with this) only
     * in the finish recycle phase, so appends after an early submit land
     * beyond synced regions and never overlap an in-flight copy or draw read.
     * Design: docs/sb-graphics-findings.md §11. */
    size_t synced_offset;
    size_t buffer_size;
    uint8_t *mapped;
} StorageBuffer;

typedef struct SurfaceBinding {
    QTAILQ_ENTRY(SurfaceBinding) entry;
    MemAccessCallback *access_cb;

    hwaddr vram_addr;

    SurfaceShape shape;
    uintptr_t dma_addr;
    uintptr_t dma_len;
    bool color;
    bool swizzle;

    unsigned int width;
    unsigned int height;
    unsigned int pitch;
    size_t size;

    bool cleared;
    int frame_time;
    int draw_time;
    bool draw_dirty;
    bool download_pending;
    bool upload_pending;
    /* sb-graphics-research predictive readback: last frame (count+1, 0 =
     * never) the CPU read this surface through the access callback, and
     * the last frame (count+1) an unbind-time eager download ran. */
    unsigned int cpu_read_frame;
    unsigned int predl_frame;
    /* narrow-fence: r->submit_count of the submission that last wrote this
     * surface via the main command buffer; SURFACE_NO_WRITE = nothing in
     * flight. Stamped in pgraph_vk_set_surface_dirty. last_use_submit is
     * the submission that last READ the image on the GPU (sampled via
     * surface-to-texture) — eviction/overwrite must wait BOTH. */
    uint32_t last_write_submit;
    uint32_t last_use_submit;
    /* sb-graphics-research per-surface attribution (reset each flip). */
    unsigned int reads_frame;
    unsigned int dl_frame;

    BasicSurfaceFormatInfo fmt;
    SurfaceFormatInfo host_fmt;

    VkImage image;
    VkImageView image_view;
    VmaAllocation allocation;

    /* loc-graphics-research (zero-copy): tracked layout of `image`. Resting
     * state is (COLOR|DEPTH_STENCIL)_ATTACHMENT_OPTIMAL; the only code that
     * may LEAVE a different resting layout is the zero-copy texture bind
     * (SHADER_READ_ONLY_OPTIMAL) — begin_render_pass restores bound targets.
     * All transitions of `image` go through pgraph_vk_surface_transition. */
    VkImageLayout image_layout;

    // Used for scaling
    VkImage image_scratch;
    VkImageLayout image_scratch_current_layout;
    VmaAllocation allocation_scratch;

    bool initialized;
} SurfaceBinding;

typedef struct ShaderModuleInfo {
    int refcnt;
    char *glsl;
    GByteArray *spirv;
    VkShaderModule module;
    SpvReflectShaderModule reflect_module;
    SpvReflectDescriptorSet **descriptor_sets;
    ShaderUniformLayout uniforms;
    ShaderUniformLayout push_constants;
} ShaderModuleInfo;

typedef struct ShaderModuleCacheKey {
    VkShaderStageFlagBits kind;
    union {
        struct {
            VshState state;
            GenVshGlslOptions glsl_opts;
        } vsh;
        struct {
            GeomState state;
            GenGeomGlslOptions glsl_opts;
        } geom;
        struct {
            PshState state;
            GenPshGlslOptions glsl_opts;
        } psh;
    };
} ShaderModuleCacheKey;

typedef struct ShaderModuleCacheEntry {
    LruNode node;
    ShaderModuleCacheKey key;
    ShaderModuleInfo *module_info;
} ShaderModuleCacheEntry;

typedef struct ShaderBinding {
    LruNode node;
    ShaderState state;
    struct {
        ShaderModuleInfo *module_info;
        VshUniformLocs uniform_locs;
    } vsh;
    struct {
        ShaderModuleInfo *module_info;
    } geom;
    struct {
        ShaderModuleInfo *module_info;
        PshUniformLocs uniform_locs;
    } psh;
} ShaderBinding;

typedef struct TextureKey {
    TextureShape state;
    hwaddr texture_vram_offset;
    hwaddr texture_length;
    hwaddr palette_vram_offset;
    hwaddr palette_length;
    float scale;
    uint32_t filter;
    uint32_t address;
    uint32_t border_color;
    uint32_t max_anisotropy;
} TextureKey;

typedef struct TextureBinding {
    LruNode node;
    TextureKey key;
    VkImage image;
    VkImageLayout current_layout;
    VkImageView image_view;
    VmaAllocation allocation;
    VkSampler sampler;
    bool possibly_dirty;
    uint64_t hash;
    unsigned int draw_time;
    uint32_t submit_time;
    /* loc-graphics-research (XEMU_SURF2TEX_ZEROCOPY): this node's image_view
     * borrows a surface's VkImage (image/allocation are VK_NULL_HANDLE and
     * must not be destroyed). borrow_image + borrow_gen (snapshot of
     * r->surface_generation) staleness-check the borrow on cache hit. */
    bool borrowed;
    uint32_t borrow_gen;
    VkImage borrow_image;
    /* Valid ONLY while borrow_gen == r->surface_generation (no invalidation
     * since the borrow was taken); used for the bind-time layout ensure. */
    SurfaceBinding *borrow_surface;
} TextureBinding;

typedef struct QueryReport {
    QSIMPLEQ_ENTRY(QueryReport) entry;
    bool clear;
    uint32_t parameter;
    unsigned int query_count;
    /* Async report delivery (XEMU_ASYNC_REPORTS): g_get_monotonic_time() at
     * enqueue, used by the RPCS3-style 300 us age-out trigger (submit the
     * open batch when the queue head grows stale instead of waiting for the
     * pushbuffer to run dry). Written only when async-reports is ON; 0
     * otherwise. Purely a submit-timing input — never affects values. */
    int64_t enqueue_us;
} QueryReport;

/* deferred-epoch query ring (XEMU_QUERY_RING): a descriptor for one retired
 * report-resolve epoch. Each epoch owns a DISJOINT slot range [base, base+count)
 * in the enlarged occlusion pool; last_submit is the r->submit_count of the
 * submission that carried this epoch's final queries (its fence proves the
 * results are complete). A STALLED resolve serves the guest from the epoch a
 * CONSTANT lag back, so occlusion counts are a fixed number of epochs stale
 * (imperceptible phase offset) rather than drained mid-frame. Indexed by
 * seq % query_num_regions; `seq` disambiguates a stale descriptor whose region
 * has since been recycled. Only touched when query_ring_enabled. */
typedef struct QueryEpoch {
    uint32_t seq;         /* epoch sequence number occupying this region slot */
    uint32_t base;        /* first occlusion-pool index of this epoch's range */
    uint32_t count;       /* queries recorded in this epoch */
    uint32_t last_submit; /* submit_count carrying the epoch's queries (fence) */
    bool valid;           /* holds a real, submitted epoch */
} QueryEpoch;

typedef struct PvideoState {
    bool enabled;
    hwaddr base;
    hwaddr limit;
    hwaddr offset;

    int pitch;
    int format;

    int in_width;
    int in_height;
    int out_width;
    int out_height;

    int in_s;
    int in_t;
    int out_x;
    int out_y;

    float scale_x;
    float scale_y;

    bool color_key_enabled;
    uint32_t color_key;
} PvideoState;

/* S2 async present (XEMU_ASYNC_PRESENT — docs/vr/overlap-present-path.md
 * §5.1): one slot of the N=2 present ring. Each slot owns a dedicated
 * command buffer (NOT the shared single_time CB), a fence (waited/reset only
 * at slot REUSE, or waited — never reset — by the destruction-path guard
 * pgraph_vk_wait_all_present_slots), its own descriptor set (kills the
 * update-while-in-use hazard of the single display set), and a ping-pong
 * exported display image with its GL import (so the display thread samples
 * slot A while the GPU still writes slot B). CB/fence/desc_set live from
 * init to finalize; image resources are (re)created lazily at the first
 * async composite for a given display size and destroyed alongside the
 * singleton display image. Modeled on the VR copy ring
 * (vr_xr_compositor.c record_and_submit_copy). */
#define ASYNC_PRESENT_RING_SIZE 2
typedef struct PresentSlot {
    VkCommandBuffer cb;   /* dedicated composite CB for this slot */
    VkFence fence;
    bool fence_submitted; /* submitted and not yet reset (reset only at reuse) */
    VkDescriptorSet desc_set;

    /* Ping-pong exported display image + GL import (mirrors the singleton
     * fields of PGRAPHVkDisplayState below; VK_NULL_HANDLE image = not
     * created for the current size yet). */
    VkImage image;
    VkImageView image_view;
    VkDeviceMemory memory;
    VkFramebuffer framebuffer;
#ifdef WIN32
    HANDLE handle;
#else
    int fd;
#endif
    GLuint gl_memory_obj;
    GLuint gl_texture_id;
    int width, height; /* size the image resources were created at */
} PresentSlot;

typedef struct PGRAPHVkDisplayState {
    ShaderModuleInfo *display_frag;

    VkDescriptorPool descriptor_pool;
    VkDescriptorSetLayout descriptor_set_layout;
    VkDescriptorSet descriptor_set;

    VkPipelineLayout pipeline_layout;
    VkPipeline pipeline;

    VkRenderPass render_pass;
    VkFramebuffer framebuffer;

    VkImage image;
    VkImageView image_view;
    VkDeviceMemory memory;
    VkSampler sampler;

    struct {
        PvideoState state;
        int width, height;
        VkImage image;
        VkImageView image_view;
        VmaAllocation allocation;
        VkSampler sampler;
    } pvideo;

    int width, height;
    int draw_time;

    // OpenGL Interop
#ifdef WIN32
    HANDLE handle;
#else
    int fd;
#endif
    GLuint gl_memory_obj;
    GLuint gl_texture_id;

    /* --- S2 async present: appended fields only (additive; OFF path never
     * reads them except the near-free present_published_valid==false and
     * fence_submitted==false checks) --- */
    PresentSlot present_slots[ASYNC_PRESENT_RING_SIZE];
    uint32_t present_count; /* monotonic slot-acquire counter (pfifo only) */
    /* Published by the async composite (pfifo, before sync_complete is set)
     * and consumed by (a) the display thread's fence gate in
     * pgraph_vk_get_framebuffer_surface — the sync_complete event pair
     * provides the release/acquire ordering for these plain fields — and
     * (b) the VR copy later in the same pfifo sync. valid==false means the
     * last composite went the synchronous path: consumers use the singleton
     * disp->image / gl_texture_id exactly as before. */
    VkImage present_published_image;
    VkFence present_published_fence;
    GLuint present_published_gl_id;
    bool present_published_valid;
} PGRAPHVkDisplayState;

typedef struct ComputePipelineKey {
    VkFormat host_fmt;
    bool pack;
    int workgroup_size;
} ComputePipelineKey;

typedef struct ComputePipeline {
    LruNode node;
    ComputePipelineKey key;
    VkPipeline pipeline;
} ComputePipeline;

typedef struct PGRAPHVkComputeState {
    VkDescriptorPool descriptor_pool;
    VkDescriptorSetLayout descriptor_set_layout;
    VkDescriptorSet descriptor_sets[1024];
    int descriptor_set_index;
    VkPipelineLayout pipeline_layout;
    Lru pipeline_cache;
    ComputePipeline *pipeline_cache_entries;
    /* loc-graphics-research: lazy-created Morton deswizzle pipeline for the
     * surface-as-swizzled-texture bounce (XEMU_SURF2TEX_EXT). */
    VkPipeline deswizzle_pipeline;
} PGRAPHVkComputeState;

typedef struct PGRAPHVkState {
    uint32_t vk_api_version;
    VkInstance instance;
    VkDebugUtilsMessengerEXT debug_messenger;
    int debug_depth;

    bool debug_utils_extension_enabled;
    bool custom_border_color_extension_enabled;
    bool memory_budget_extension_enabled;

    VkPhysicalDevice physical_device;
    VkPhysicalDeviceFeatures enabled_physical_device_features;
    VkPhysicalDeviceProperties device_props;
    VkDevice device;
    VmaAllocator allocator;
    uint32_t allocator_last_submit_index;

    VkQueue queue;
    VkCommandPool command_pool;

    VkCommandBuffer command_buffer;
    /* sb-graphics-research narrow-fence: a ring of per-submission slots,
     * each with its OWN main + aux command buffers, fence and binary
     * semaphore, so up to CB_FENCE_RING_SIZE submissions can be in flight —
     * one executing on the GPU while the next records into the following
     * slot. A surface download waits on ONLY the submission that wrote it
     * (via its recorded last_write_submit) instead of a whole-pipeline
     * finish. Per-slot semaphores are REQUIRED: a single aux->main semaphore
     * cannot have two in-flight signal/wait pairs. Recycle detected by
     * submit_index identity. Design: docs/sb-graphics-findings.md §11. */
#define CB_FENCE_RING_SIZE 4
#define SURFACE_NO_WRITE   0xFFFFFFFFu
    struct CbFenceSlot {
        VkFence  fence;
        VkCommandBuffer cb;     /* main command buffer for this slot */
        VkCommandBuffer aux_cb; /* staging-sync aux command buffer */
        VkSemaphore sem;        /* per-slot aux->main ordering (binary) */
        uint32_t submit_index;  /* SURFACE_NO_WRITE = slot never used */
        /* Occlusion-query range this submission carries: pool indices
         * [query_first, query_first + query_count). Stamped in
         * pgraph_vk_submit_batch from the r->queries_submitted watermark.
         * With async-reports/batch-qreset OFF a resolve immediately follows
         * every submit (see pgraph_vk_process_pending_reports_internal), so
         * query_first is always query_epoch_base and query_count ==
         * num_queries_in_flight-at-submit. Under XEMU_ASYNC_REPORTS submit
         * and resolve ARE decoupled (the generalization these fields
         * anticipated): the marker's prefix-apply consumes only slots below
         * the submitted watermark, so a slot's result is read strictly after
         * its owning submission's fence. */
        uint32_t query_first;
        uint32_t query_count;

        /* --- frame-overlap S1 (XEMU_FRAME_OVERLAP, default OFF) ---
         * Per-slot overlap-ready resources; every field below is created,
         * written, and read ONLY when r->frame_overlap is set (OFF path
         * byte-identical). Purely additive at the struct end so parallel
         * branches merge cleanly. Design: overlap-resource-audit.md §6.
         *
         * Retire-work contract: submit_batch stamps ov_desc_dirty /
         * ov_pending_fb / ov_arena_end on the slot it submits; the
         * retire-work (destroy pending framebuffers, reset + re-allocate
         * the descriptor pool) runs exactly once per retirement, either at
         * slot re-acquisition (after the fence wait) or in the recycle
         * phase's all-retired sweep — whichever comes first. */
        VkDescriptorPool ov_desc_pool;      /* per-slot graphics pool */
        VkDescriptorSet ov_desc_sets[1024]; /* sets from ov_desc_pool; same
                                             * capacity as the shared array so
                                             * exhaustion is never earlier
                                             * than OFF */
        bool ov_desc_dirty;   /* sets consumed by this slot's last submission;
                               * pool reset + re-alloc due at retire */
        /* Framebuffers superseded during this slot's recording; moved here
         * from the live window at submit, vkDestroyFramebuffer'd at retire.
         * Bounded by one recording's creations (< the 50-entry window):
         * acquisition drains the list before recording restarts. */
        VkFramebuffer ov_pending_fb[50];
        int ov_num_pending_fb;
        /* Staging-arena offsets (index/vertex-inline/uniform, in that
         * order) at this slot's submit — the per-slot high-water marks the
         * later reclaim-on-retire stages consume. Zeroed on full reset. */
        VkDeviceSize ov_arena_end[3];
    } cb_fence_ring[CB_FENCE_RING_SIZE];
    struct CbFenceSlot *current_slot; /* slot the open command buffer records into */
    unsigned int command_buffer_start_time;
    bool in_command_buffer;
    uint32_t submit_count;
    /* Set by pgraph_vk_submit_batch when a batch is submitted; the finish
     * recycle phase runs iff this is set (so a batch early-submitted without
     * a following open CB is still recycled at the next full finish). */
    bool recycle_pending;

    VkCommandBuffer aux_command_buffer;
    bool in_aux_command_buffer;

    /* narrow-fence Part A: a dedicated command buffer + fence for
     * single-time GPU work (surface/texture uploads & downloads). Formerly
     * these borrowed aux_command_buffer and ended with vkQueueWaitIdle,
     * which drains the WHOLE queue — fatal once batches are in flight (it
     * would re-drain everything the async flip is trying to overlap). Now
     * each single-time submit waits ONLY its own fence, and it is decoupled
     * from a slot's aux CB so mid-recording uploads can't collide with an
     * in-flight submission. Design: docs/sb-graphics-findings.md §11. */
    VkCommandBuffer single_time_command_buffer;
    VkFence single_time_fence;

    /* S5 in-batch uploads (XEMU_INBATCH_UPLOADS=1, default OFF;
     * XEMU_NO_INBATCH_UPLOADS=1 force-off wins; resolved once in
     * pgraph_vk_init_buffers). ON => texture/surface uploads memcpy their
     * texels into a ring region of BUFFER_STAGING_SRC and record the
     * vkCmdCopyBufferToImage + layout transitions into the frame's OWN
     * command stream (the nondraw path the zeta surf-to-tex already uses)
     * instead of the blocking single-time submit+wait. Rationale
     * (docs/vr/overlap-resource-audit.md §0/§3): a single-time fence wait
     * transitively drains every earlier submission on the queue, so under
     * overlap each upload re-serializes all in-flight slots. OFF => the
     * historical single-time path, byte-identical. */
    bool inbatch_uploads;
    /* Ring allocator carved out of BUFFER_STAGING_SRC when ON. The first
     * STAGING_RING_RESERVE bytes are NEVER handed out: foreign single-time
     * users that write the buffer at offset 0 and are out of S5 scope
     * (upload_pvideo_image in display.c, bounded well under the reserve;
     * the init-only dummy texture) stay safe without being modified. The
     * ring works in VIRTUAL monotonically-increasing offsets; physical
     * offset = STAGING_RING_RESERVE + (voff % capacity). A region is
     * registered against the submission that consumes it and its bytes are
     * reused only after that submission's fence has signaled (checked
     * lazily at the next allocation). Ring full => the caller falls back
     * to a full finish + ring reset + the old blocking path (counted by
     * NV2A_PROF_UPLOAD_STAGING_WAIT) — correctness over stall-freedom. */
#define STAGING_RING_RESERVE     (8 * 1024 * 1024)
#define STAGING_RING_ALIGN       256
#define STAGING_RING_MAX_PENDING 16
    struct {
        uint64_t head; /* virtual offset of next allocation */
        uint64_t tail; /* virtual offset of oldest un-reclaimed byte */
        uint64_t capacity; /* BUFFER_STAGING_SRC size - STAGING_RING_RESERVE */
        struct StagingRingRegion {
            uint64_t begin, end;   /* virtual [begin, end) incl. wrap pad */
            uint32_t submit_index; /* consuming submission; SURFACE_NO_WRITE
                                    * = transient single-time consumer that
                                    * fence-waited synchronously (reclaimable
                                    * immediately) */
        } pending[STAGING_RING_MAX_PENDING];
        int pending_first; /* oldest entry (FIFO reclaim order) */
        int pending_count;
    } staging_ring;

    VkFramebuffer framebuffers[50];
    int framebuffer_index;
    bool framebuffer_dirty;

    VkRenderPass render_pass;
    GArray *render_passes; // RenderPass
    bool in_render_pass;
    bool in_draw;

    Lru pipeline_cache;
    VkPipelineCache vk_pipeline_cache;
    PipelineBinding *pipeline_cache_entries;
    PipelineBinding *pipeline_binding;
    bool pipeline_binding_changed;
    /* XEMU_PIPE_OPT, cached once at init (pgraph_vk_pipe_opt_enabled). OFF =>
     * byte-identical to upstream. ON => texture-only-dirty draws skip the
     * redundant PipelineKey hash + LRU lookup (the pipeline never depends on
     * texture image data, only on shader_state, which shader_bindings_changed
     * already covers). See check_pipeline_dirty / create_pipeline (draw.c). */
    bool pipe_opt;

    VkDescriptorPool descriptor_pool;
    VkDescriptorSetLayout descriptor_set_layout;
    VkDescriptorSet descriptor_sets[1024];
    int descriptor_set_index;

    StorageBuffer storage_buffers[BUFFER_COUNT];

    MemorySyncRequirement vertex_ram_buffer_syncs[NV2A_VERTEXSHADER_ATTRIBUTES];
    size_t num_vertex_ram_buffer_syncs;
    unsigned long *uploaded_bitmap;
    size_t bitmap_size;

    VkVertexInputAttributeDescription vertex_attribute_descriptions[NV2A_VERTEXSHADER_ATTRIBUTES];
    int vertex_attribute_to_description_location[NV2A_VERTEXSHADER_ATTRIBUTES];
    int num_active_vertex_attribute_descriptions;

    VkVertexInputBindingDescription vertex_binding_descriptions[NV2A_VERTEXSHADER_ATTRIBUTES];
    int num_active_vertex_binding_descriptions;
    hwaddr vertex_attribute_offsets[NV2A_VERTEXSHADER_ATTRIBUTES];

    QTAILQ_HEAD(, SurfaceBinding) surfaces;
    QTAILQ_HEAD(, SurfaceBinding) invalid_surfaces;
    /* loc-graphics-research (zero-copy): bumped on every invalidate_surface;
     * borrowed texture nodes staleness-check against it on cache hit. */
    uint32_t surface_generation;
    SurfaceBinding *color_binding, *zeta_binding;
    bool downloads_pending;
    QemuEvent downloads_complete;
    bool download_dirty_surfaces_pending;
    QemuEvent dirty_surfaces_download_complete; // common

    /* sb-graphics-research exact-dirty narrowing v2 — CHANNEL SEPARATION
     * (branch perf-exactdirty-v2). A per-frame list of byte-exact VRAM spans
     * that KNOWN NV2A_TEX writers dirtied this frame — the surface download
     * (surface.c) and the 2D blit (blit.c), both of which have the exact
     * {addr,size} they wrote.
     *
     * The dirty signal is split into two INDEPENDENT channels by writer class:
     *   - Channel S (these spans): exact-knowable writers. When a writer's
     *     span is recorded here it STOPS setting the page-granular
     *     DIRTY_MEMORY_NV2A_TEX bitmap for that range (see surface.c/blit.c) —
     *     it is represented ONLY as an exact byte span.
     *   - Channel P (the DIRTY_MEMORY_NV2A_TEX page bitmap): now carries ONLY
     *     unattributed writers — guest CPU/DMA stores to texture memory, which
     *     set NV2A_TEX themselves via the softmmu notdirty path on TB exit and
     *     are NOT byte-knowable — plus any exact write that overflowed Channel
     *     S (fell back per-write to the bitmap).
     * A texture is dirty iff its bytes intersect a Channel-S span OR its pages
     * are dirty in Channel P. Both channels are ALWAYS consulted; neither can
     * mask the other. Because a recorded exact writer never touches Channel P,
     * a page-dirty bit ALWAYS denotes a genuine unattributed write that must be
     * hashed — the v1 co-page false negative (an animated texture sharing a
     * page with a download's range getting its genuine update page-hit-skipped)
     * is IMPOSSIBLE BY CONSTRUCTION. Byte-exact for our writers (drops the
     * radar's false co-page re-hash storm), page-conservative for everyone else
     * (correct, never a false negative → no flicker). See the two-term cost
     * model (SLOPE term) and §8.7 re-enable design in sb-graphics-findings.md.
     *
     * exact_spans_valid gates whether Channel S is trusted. g_malloc0 leaves it
     * false, so behavior is stock page-granular until the first flip (safe);
     * flip sets count=0, valid=true (renderer.c flip_stall). In v2 it is NEVER
     * cleared mid-frame: clearing it after a writer already skipped Channel P
     * for its span would ORPHAN that span (Channel S ignored, Channel P skipped
     * => the write vanishes => silent texture corruption). Overflow past
     * EXACT_DIRTY_MAX_SPANS is therefore handled PER-WRITE — the overflowing
     * write falls back to Channel P (pgraph_vk_note_exact_dirty returns false so
     * its caller sets the bitmap) while the recorded spans stay valid. All
     * three accesses (record at download/blit, consult at bind, reset at flip)
     * are on the pgraph/pfifo thread; Channel P is the softmmu dirty bitmap,
     * which is itself thread-safe against the vCPU-thread guest stores. */
#define EXACT_DIRTY_MAX_SPANS 32
    struct {
        hwaddr addr;
        hwaddr size;
    } exact_dirty_spans[EXACT_DIRTY_MAX_SPANS];
    unsigned int exact_dirty_count;
    bool exact_spans_valid;

    Lru texture_cache;
    TextureBinding *texture_cache_entries;
    TextureBinding *texture_bindings[NV2A_MAX_TEXTURES];
    TextureBinding dummy_texture;
    bool texture_bindings_changed;
    VkFormatProperties *texture_format_properties;

    Lru shader_cache;
    ShaderBinding *shader_cache_entries;
    ShaderBinding *shader_binding;
    ShaderModuleInfo *quad_vert_module, *solid_frag_module;
    bool shader_bindings_changed;
    bool use_push_constants_for_uniform_attrs;

    Lru shader_module_cache;
    ShaderModuleCacheEntry *shader_module_cache_entries;

    // FIXME: Merge these into a structure
    uint64_t uniform_buffer_hashes[2];
    size_t uniform_buffer_offsets[2];
    bool uniforms_changed;

    /* perf-const-skip (XEMU_CONST_SKIP): the uniform recompute + UBO content
     * hash in update_shader_uniforms runs on every draw (~2000x/frame), even
     * though ~85% of draws genuinely change the UBO and the rest are pure
     * redundancy. When enabled, a draw that reuses the previous shader binding
     * with no changed uniform input reuses the previous UBO offset/descriptor,
     * skipping the recompute AND the hash. Correctness rests on a COMPLETE set
     * of change signals: shader/texture binding-change flags, the per-draw
     * regs_dirty bitmap (all register-derived inputs), the consumable
     * vsh_constants_dirty[]/ltctxa|b|ltc1_dirty[] arrays, and this snapshot of
     * the residual inputs that have NO consumable dirty signal. Any input whose
     * signal is dirty forces the full recompute+hash (self-correcting via the
     * existing content hash). Captured on every real recompute; consulted only
     * when a skip is otherwise possible. See update_shader_uniforms. */
    struct {
        bool valid; /* a prior recompute's values are captured below */
        float material_alpha;
        float point_params[8];
        float specular_power;
        float light_infinite_half_vector[NV2A_MAX_LIGHTS][3];
        float light_infinite_direction[NV2A_MAX_LIGHTS][3];
        float light_local_position[NV2A_MAX_LIGHTS][3];
        float light_local_attenuation[NV2A_MAX_LIGHTS][3];
        int surface_width, surface_height; /* pg->surface_binding_dim */
        unsigned int aa, z_format, zeta_format; /* pg->surface_shape */
    } const_skip;

    VkQueryPool query_pool;
    int max_queries_in_flight; // FIXME: Move out to constant
    int num_queries_in_flight;
    /* Low watermark for the next submission's query range: count of window
     * slots already handed to an in-flight submission. Advanced in
     * pgraph_vk_submit_batch; reset to 0 (with num_queries_in_flight) by a
     * full apply; rebased down by a prefix apply. With async-reports OFF a
     * resolve immediately follows every submit, so this is 0 during draw
     * accumulation and only transiently equals num_queries_in_flight between
     * a submit and its resolve; with async-reports ON it holds at the
     * marker's covered watermark while the fence is polled — exactly the
     * decoupling this field anticipated. */
    uint32_t queries_submitted;
    /* fix-query-flicker: per-pool-index owning submission. query_index_submit[i]
     * = the r->submit_count of the submission that last recorded a query at
     * occlusion-pool index i (SURFACE_NO_WRITE = never used). num_queries_in_flight
     * restarts at 0 every resolve-epoch, so the async resolve path
     * (XEMU_ASYNC_QUERIES) re-resets pool index i in a NEW submission while up
     * to CB_FENCE_RING_SIZE earlier submissions that also used index i can still
     * be in flight (no wait_all_slots between async resolves). begin_query waits
     * this owner's fence before vkCmdResetQueryPool so a slot is never reset
     * while a submission that used it is unretired — the per-slot happens-before
     * the sync path got for free from wait_all_slots. Allocated (size
     * max_queries_in_flight) and initialised to SURFACE_NO_WRITE in
     * pgraph_vk_init_reports; only read/written when async_queries_enabled, so
     * the OFF path never touches it. */
    uint32_t *query_index_submit;
    /* Cached once at init from XEMU_ASYNC_QUERIES (see
     * pgraph_vk_async_queries_enabled). Gates the async resolve AND the
     * begin_query reuse fence; OFF => both are skipped and behaviour is
     * byte-identical to upstream. */
    bool async_queries_enabled;
    /* --- deferred-epoch query ring (XEMU_QUERY_RING), cached at init --- */
    /* Master gate. OFF => query_num_regions==1, query_region_size==pool size,
     * query_epoch_base pinned 0: a single region reused every resolve, byte-
     * identical to upstream. ON => a ring of disjoint per-epoch regions served
     * at a constant staleness lag. */
    bool query_ring_enabled;
    uint32_t query_region_size; /* occlusion-pool slots per epoch region */
    uint32_t query_num_regions; /* R: distinct regions in the ring */
    uint32_t query_ring_lag;    /* K: constant staleness, in epochs (>=1, <R) */
    uint32_t query_epoch_seq;   /* monotonic epoch counter (bumped per resolve) */
    /* First occlusion-pool index of the CURRENT epoch's region; begin_query
     * records at query_epoch_base + num_queries_in_flight. Always 0 when OFF. */
    uint32_t query_epoch_base;
    QueryEpoch *query_epochs;   /* ring[query_num_regions] of retired-epoch descriptors */

    /* ---- Stage S3: async report delivery (XEMU_ASYNC_REPORTS), and
     * ---- Stage S4: batched query reset + in-pass queries (XEMU_BATCH_QRESET).
     * Shared slot-lifecycle model (the "window"): pool indices
     * [query_epoch_base, query_epoch_base + num_queries_in_flight) are the
     * allocated-but-not-yet-applied slots; queries_submitted is the prefix of
     * that window already carried by submitted batches; report->query_count is
     * window-relative. A PREFIX apply (async-reports Phase C) consumes the
     * first `take` slots and rebases base += take / num -= take / submitted -=
     * take / each surviving report's query_count -= take, so absolute slot
     * indices (base + offset) of live slots never change. A FULL apply empties
     * the window: num = submitted = 0 and base returns to query_region_origin
     * (OFF/A: index reuse, protected by per-slot reset + the owner fence) or
     * advances monotonically past the consumed slots (batch-qreset: a slot is
     * NEVER reused within a region entry, because the region is reset as a
     * whole only at entry). With both toggles OFF no prefix apply ever runs,
     * base stays pinned at query_region_origin == 0, and every expression
     * reduces to the historical upstream arithmetic. */

    /* XEMU_ASYNC_REPORTS (force-off: XEMU_NO_ASYNC_REPORTS), cached at init.
     * Turns the STALLED occlusion resolve into submit-now/fence-poll-later:
     * Phase S submits the open batch and arms the marker below; Phase C (the
     * per-iteration pfifo poll) applies the retired prefix when the fence
     * signals. Values are byte-identical to the synchronous drain — only the
     * pfifo thread's blocking changes. */
    bool async_reports_enabled;
    /* Pending-resolve marker (valid while report_marker_pending): every queued
     * report with query_count <= report_marker_watermark is fully determined
     * by slots [query_epoch_base, +watermark) — all carried by submissions up
     * to report_marker_submit (the newest at arm time). Its fence retiring
     * therefore proves those reports' exact drain-identical values exist.
     * Mirrored into pg->report_marker_pending for the pfifo timed park. */
    bool report_marker_pending;
    uint32_t report_marker_watermark;
    uint32_t report_marker_submit;

    /* XEMU_BATCH_QRESET (force-off: XEMU_NO_BATCH_QRESET), cached at init.
     * Kills query-forced renderpass churn: one vkCmdResetQueryPool of the
     * frame's whole ping-pong region recorded at the first command buffer
     * after region entry (outside any pass, submission-ordered after all
     * prior GPU work), monotonic slot allocation through the frame, and
     * vkCmdBeginQuery/vkCmdEndQuery recorded INSIDE render passes. */
    bool batch_qreset_enabled;
    /* First pool index of the region the CURRENT frame allocates from.
     * 0 with everything OFF (and the full historical pool is one region);
     * the current epoch's region base under the (refuted, mutually-exclusive)
     * query ring; parity * query_region_size under batch-qreset ping-pong.
     * begin_query's capacity check and begin_pre_draw's backstop measure
     * window extent relative to this origin. */
    uint32_t query_region_origin;
    /* batch-qreset: the current region must be reset before its first use;
     * set at every region entry (init, flip swap, backstop swap), consumed by
     * pgraph_vk_begin_command_buffer which records the batched reset. */
    bool query_pool_reset_pending;

    bool new_query_needed;
    bool query_in_flight;
    uint32_t zpass_pixel_count_result;
    QSIMPLEQ_HEAD(, QueryReport) report_queue; // FIXME: Statically allocate

    SurfaceFormatInfo kelvin_surface_zeta_vk_map[3];

    uint32_t clear_parameter;

    PGRAPHVkDisplayState display;
    PGRAPHVkComputeState compute;

    /* --- frame-overlap S1 (XEMU_FRAME_OVERLAP; force-off
     * XEMU_NO_FRAME_OVERLAP; default OFF), cached once at init --- Gates the
     * decomposed recycle machinery: per-slot descriptor pools, per-slot
     * framebuffer destroy-lists, per-slot staging-arena watermarks, the
     * all-retired-only uploaded_bitmap clear, and the begin_query owner-fence
     * arming. S1 is scaffolding: every finish/wait stays in place, so with
     * ON and no later stage present, behavior is identical to OFF (the
     * recycle phase always runs with all slots retired) — only the
     * bookkeeping shape changes. OFF => all ov_* state untouched and every
     * path byte-identical to today. Fields appended at the struct end for
     * clean merges. Design: docs/vr/overlap-resource-audit.md §6. */
    bool frame_overlap;
    /* The upcoming submission's ring slot has been acquired (previous
     * occupant's fence waited + its retire-work executed) and
     * current_slot/descriptor_set_index target it. Set by
     * pgraph_vk_overlap_acquire_slot (idempotent), cleared when
     * submit_batch hands the slot to the GPU. Allows the acquisition to
     * happen EARLY — pgraph_vk_update_descriptor_sets runs before
     * pgraph_vk_ensure_command_buffer in begin_pre_draw, and its set
     * writes must target the slot the draw will record into. */
    bool ov_slot_acquired;
} PGRAPHVkState;

// renderer.c
void pgraph_vk_check_memory_budget(PGRAPHState *pg);

// debug.c
#define RGBA_RED     (float[4]){1,0,0,1}
#define RGBA_YELLOW  (float[4]){1,1,0,1}
#define RGBA_GREEN   (float[4]){0,1,0,1}
#define RGBA_BLUE    (float[4]){0,0,1,1}
#define RGBA_PINK    (float[4]){1,0,1,1}
#define RGBA_DEFAULT (float[4]){0,0,0,0}

void pgraph_vk_debug_init(void);
void pgraph_vk_insert_debug_marker(PGRAPHVkState *r, VkCommandBuffer cmd,
                                   float color[4], const char *format, ...) G_GNUC_PRINTF(4, 5);
void pgraph_vk_begin_debug_marker(PGRAPHVkState *r, VkCommandBuffer cmd,
                                  float color[4], const char *format, ...) G_GNUC_PRINTF(4, 5);
void pgraph_vk_end_debug_marker(PGRAPHVkState *r, VkCommandBuffer cmd);

// instance.c
void pgraph_vk_init_instance(PGRAPHState *pg, Error **errp);
void pgraph_vk_finalize_instance(PGRAPHState *pg);
QueueFamilyIndices pgraph_vk_find_queue_families(VkPhysicalDevice device);
uint32_t pgraph_vk_get_memory_type(PGRAPHState *pg, uint32_t type_bits,
                                   VkMemoryPropertyFlags properties);

// glsl.c
void pgraph_vk_init_glsl_compiler(void);
void pgraph_vk_finalize_glsl_compiler(void);
GByteArray *pgraph_vk_compile_glsl_to_spv(glslang_stage_t stage,
                                          const char *glsl_source);
VkShaderModule pgraph_vk_create_shader_module_from_spv(PGRAPHVkState *r,
                                                       GByteArray *spv);
ShaderModuleInfo *pgraph_vk_create_shader_module_from_glsl(
    PGRAPHVkState *r, VkShaderStageFlagBits stage, const char *glsl);
void pgraph_vk_ref_shader_module(ShaderModuleInfo *info);
void pgraph_vk_unref_shader_module(PGRAPHVkState *r, ShaderModuleInfo *info);
void pgraph_vk_destroy_shader_module(PGRAPHVkState *r, ShaderModuleInfo *info);

// buffer.c
void pgraph_vk_init_buffers(NV2AState *d);
void pgraph_vk_finalize_buffers(NV2AState *d);
bool pgraph_vk_buffer_has_space_for(PGRAPHState *pg, int index,
                                    VkDeviceSize size,
                                    VkDeviceAddress alignment);
VkDeviceSize pgraph_vk_append_to_buffer(PGRAPHState *pg, int index, void **data,
                                        VkDeviceSize *sizes, size_t count,
                                        VkDeviceAddress alignment);
bool pgraph_vk_staging_ring_alloc(PGRAPHState *pg, VkDeviceSize size,
                                  VkDeviceSize *offset,
                                  uint32_t consumer_submit_index);
void pgraph_vk_staging_ring_reset(PGRAPHVkState *r);

// command.c
void pgraph_vk_init_command_buffers(PGRAPHState *pg);
void pgraph_vk_finalize_command_buffers(PGRAPHState *pg);
VkCommandBuffer pgraph_vk_begin_single_time_commands(PGRAPHState *pg);
void pgraph_vk_end_single_time_commands(PGRAPHState *pg, VkCommandBuffer cmd);

// image.c
void pgraph_vk_transition_image_layout(PGRAPHState *pg, VkCommandBuffer cmd,
                                       VkImage image, VkFormat format,
                                       VkImageLayout oldLayout,
                                       VkImageLayout newLayout);

// vertex.c
void pgraph_vk_bind_vertex_attributes(NV2AState *d, unsigned int min_element,
                                      unsigned int max_element,
                                      bool inline_data,
                                      unsigned int inline_stride,
                                      unsigned int provoking_element);
void pgraph_vk_bind_vertex_attributes_inline(NV2AState *d);
void pgraph_vk_update_vertex_ram_buffer(PGRAPHState *pg, hwaddr offset, void *data,
                                    VkDeviceSize size);
VkDeviceSize pgraph_vk_update_index_buffer(PGRAPHState *pg, void *data,
                                           VkDeviceSize size);
VkDeviceSize pgraph_vk_update_vertex_inline_buffer(PGRAPHState *pg, void **data,
                                                   VkDeviceSize *sizes,
                                                   size_t count);

// surface.c
void pgraph_vk_init_surfaces(PGRAPHState *pg);
void pgraph_vk_finalize_surfaces(PGRAPHState *pg);
void pgraph_vk_surface_flush(NV2AState *d);
void pgraph_vk_process_pending_downloads(NV2AState *d);
void pgraph_vk_surface_download_if_dirty(NV2AState *d, SurfaceBinding *surface);
void pgraph_vk_refresh_predl_toggle(void); /* once/frame, from flip_stall */
void pgraph_vk_refresh_narrowfence_toggle(void); /* once/frame, from flip_stall */
bool pgraph_vk_narrowfence_enabled(void);
void pgraph_vk_wait_for_surface_write(PGRAPHState *pg, SurfaceBinding *surface);
SurfaceBinding *pgraph_vk_surface_get_within(NV2AState *d, hwaddr addr);
bool pgraph_vk_alias_surfaces_enabled(void);
void pgraph_vk_surface_transition(PGRAPHState *pg, VkCommandBuffer cmd,
                                  SurfaceBinding *surface, VkImageLayout to);
VkImageLayout pgraph_vk_surface_rest_layout(const SurfaceBinding *surface);
void pgraph_vk_wait_for_surface_download(SurfaceBinding *e);
void pgraph_vk_download_dirty_surfaces(NV2AState *d);
int pgraph_vk_download_surfaces_in_range_if_dirty(PGRAPHState *pg, hwaddr start, hwaddr size);
void pgraph_vk_upload_surface_data(NV2AState *d, SurfaceBinding *surface,
                                   bool force);
void pgraph_vk_surface_update(NV2AState *d, bool upload, bool color_write,
                              bool zeta_write);
SurfaceBinding *pgraph_vk_surface_get(NV2AState *d, hwaddr addr);
void pgraph_vk_set_surface_dirty(PGRAPHState *pg, bool color, bool zeta);
void pgraph_vk_set_surface_scale_factor(NV2AState *d, unsigned int scale);
unsigned int pgraph_vk_get_surface_scale_factor(NV2AState *d);
void pgraph_vk_reload_surface_scale_factor(PGRAPHState *pg);

// surface-compute.c
void pgraph_vk_init_compute(PGRAPHState *pg);
bool pgraph_vk_compute_needs_finish(PGRAPHVkState *r);
void pgraph_vk_compute_finish_complete(PGRAPHVkState *r);
void pgraph_vk_finalize_compute(PGRAPHState *pg);
void pgraph_vk_dispatch_deswizzle_u32(PGRAPHState *pg, VkCommandBuffer cmd,
                                      unsigned int tex_w, unsigned int tex_h,
                                      unsigned int src_bias_words);
void pgraph_vk_pack_depth_stencil(PGRAPHState *pg, SurfaceBinding *surface,
                                  VkCommandBuffer cmd, VkBuffer src,
                                  VkBuffer dst, bool downscale);
void pgraph_vk_unpack_depth_stencil(PGRAPHState *pg, SurfaceBinding *surface,
                                    VkCommandBuffer cmd, VkBuffer src,
                                    VkBuffer dst);

// display.c
void pgraph_vk_init_display(PGRAPHState *pg);
void pgraph_vk_finalize_display(PGRAPHState *pg);
void pgraph_vk_render_display(PGRAPHState *pg);
/* S2 async present (XEMU_ASYNC_PRESENT / XEMU_NO_ASYNC_PRESENT / watch-file
 * /tmp/asyncpresent-on; TCG-only). Refreshed once per frame from flip_stall
 * (narrowfence pattern); hot sites read the cached bool. OFF => both present
 * sites keep calling pgraph_vk_finish exactly as today. */
void pgraph_vk_refresh_async_present_toggle(void); /* once/frame, from flip_stall */
bool pgraph_vk_async_present_enabled(void);
/* Destruction-path guard (overlap-present-path.md §5.1(7)): wait every
 * in-flight present-ring submission. Deliberately does NOT reset fences —
 * reset happens only at slot reuse, so a concurrent display-thread
 * vkWaitForFences on the published fence can never race a reset (thread
 * safety: fence waits need no external sync; resets do). Near-free when
 * OFF/idle (two fence_submitted checks). */
void pgraph_vk_wait_all_present_slots(PGRAPHState *pg);

// texture.c
void pgraph_vk_init_textures(PGRAPHState *pg);
void pgraph_vk_finalize_textures(PGRAPHState *pg);
void pgraph_vk_bind_textures(NV2AState *d);
/* use_exact_spans: when true (and exact-dirty narrowing is enabled + valid
 * this frame), flag only textures whose bytes intersect a recorded exact
 * span; when false, stock page-granular flagging of every overlapping
 * texture. The whole-VRAM invalidate in pgraph_vk_flush MUST pass false. */
void pgraph_vk_mark_textures_possibly_dirty(NV2AState *d, hwaddr addr,
                                            hwaddr size, bool use_exact_spans);
/* sb-graphics-research exact-dirty narrowing v2 (channel separation): record a
 * byte-exact VRAM span that a known exact writer just wrote (surface download,
 * 2D blit) into Channel S. Returns true iff the span was recorded and will be
 * consulted at bind time — in which case the caller MUST NOT set the
 * DIRTY_MEMORY_NV2A_TEX page bitmap for that range (the span channel now
 * represents it). Returns false when the narrowing is disabled, not yet trusted
 * (pre-first-flip), or the per-frame span array is full — in which case the
 * caller MUST set the page bitmap so the write lands on Channel P instead
 * (never on neither). See PGRAPHVkState.exact_dirty_spans. */
bool pgraph_vk_note_exact_dirty(NV2AState *d, hwaddr addr, hwaddr size);
void pgraph_vk_trim_texture_cache(PGRAPHState *pg);

// shaders.c
void pgraph_vk_init_shaders(PGRAPHState *pg);
void pgraph_vk_finalize_shaders(PGRAPHState *pg);
void pgraph_vk_update_descriptor_sets(PGRAPHState *pg);
void pgraph_vk_bind_shaders(PGRAPHState *pg);
/* frame-overlap S1: (re-)allocate the full ov_desc_sets array from the
 * slot's own pool — at init and after each retire-time
 * vkResetDescriptorPool (a pool reset invalidates its set handles). */
void pgraph_vk_overlap_alloc_slot_desc_sets(PGRAPHVkState *r,
                                            struct CbFenceSlot *slot);

// reports.c
void pgraph_vk_init_reports(PGRAPHState *pg);
void pgraph_vk_finalize_reports(PGRAPHState *pg);
void pgraph_vk_clear_report_value(NV2AState *d);
void pgraph_vk_get_report(NV2AState *d, uint32_t parameter);
void pgraph_vk_process_pending_reports(NV2AState *d);
void pgraph_vk_process_pending_reports_internal(NV2AState *d);
/* XEMU_ASYNC_QUERIES, read once. Cached into r->async_queries_enabled at init;
 * exposed so begin_query (draw.c) gates the pool-slot reuse fence on it. */
bool pgraph_vk_async_queries_enabled(void);
/* XEMU_QUERY_RING (force-off: XEMU_NO_QUERY_RING), read once. Cached into
 * r->query_ring_enabled at init; exposed so begin_query (draw.c) gates the
 * pool-slot reuse fence on it as well. */
bool pgraph_vk_query_ring_enabled(void);
/* XEMU_ASYNC_REPORTS (force-off: XEMU_NO_ASYNC_REPORTS), read once. Cached
 * into r->async_reports_enabled at init (which also neutralizes the refuted
 * XEMU_QUERY_RING if both are set — the retired-epoch serve must never be
 * reachable from this mode). */
bool pgraph_vk_async_reports_enabled(void);
/* XEMU_BATCH_QRESET (force-off: XEMU_NO_BATCH_QRESET), read once. Cached into
 * r->batch_qreset_enabled at init (also ring-neutralizing). */
bool pgraph_vk_batch_qreset_enabled(void);
/* Batch-qreset region swap: enter the other ping-pong region (origin/base
 * move, window empties, batched reset scheduled). Callers must guarantee both
 * regions are quiescent (post-finish: all submissions retired, all reports
 * applied, no marker). Defined in reports.c; called from draw.c at the
 * flip-time finish and the pool-exhaustion backstop. */
void pgraph_vk_swap_query_region(PGRAPHVkState *r);
/* XEMU_PIPE_OPT (force-off: XEMU_NO_PIPE_OPT), read once. Cached into
 * r->pipe_opt at init; gates the texture-only-dirty pipeline-lookup skip in
 * check_pipeline_dirty / create_pipeline. OFF => byte-identical to upstream. */
bool pgraph_vk_pipe_opt_enabled(void);
/* XEMU_FRAME_OVERLAP (force-off: XEMU_NO_FRAME_OVERLAP), read once at first
 * use (getenv-once + one-time stderr state line). Cached into
 * r->frame_overlap in pgraph_vk_init_shaders; gates the S1 overlap-ready
 * per-slot resource machinery. OFF => byte-identical to today. */
bool pgraph_vk_frame_overlap_enabled(void);
/* frame-overlap S1: acquire the ring slot for submission index
 * r->submit_count — wait its previous occupant's fence, run that occupant's
 * deferred retire-work (framebuffer destroy-list, descriptor-pool reset),
 * repoint current_slot/command_buffer aliases and zero
 * descriptor_set_index. Idempotent until submit_batch clears
 * ov_slot_acquired. ON-mode only (callers gate on r->frame_overlap). */
void pgraph_vk_overlap_acquire_slot(PGRAPHState *pg);

typedef enum FinishReason {
    VK_FINISH_REASON_VERTEX_BUFFER_DIRTY,
    VK_FINISH_REASON_SURFACE_CREATE,
    VK_FINISH_REASON_SURFACE_DOWN,
    VK_FINISH_REASON_NEED_BUFFER_SPACE,
    VK_FINISH_REASON_FRAMEBUFFER_DIRTY,
    VK_FINISH_REASON_PRESENTING,
    VK_FINISH_REASON_FLIP_STALL,
    VK_FINISH_REASON_FLUSH,
    VK_FINISH_REASON_STALLED,
    /* Async occlusion-query resolve (XEMU_ASYNC_QUERIES): a submit_batch that
     * flushes the open batch WITHOUT the whole-pipeline wait_all_slots/recycle
     * a real finish does, so its query results can be read with a scoped
     * WAIT_BIT. Distinct reason so it ticks NV2A_PROF_QUERY_ASYNC_RESOLVE
     * instead of FINISH_STALLED — the A/B then reads FINISH_STALLED -> ~0 with
     * QUERY_ASYNC_RESOLVE absorbing the count. Submit-only; never passed to
     * pgraph_vk_finish. */
    VK_FINISH_REASON_QUERY_ASYNC,
} FinishReason;

// draw.c
void pgraph_vk_init_pipelines(PGRAPHState *pg);
void pgraph_vk_finalize_pipelines(PGRAPHState *pg);
void pgraph_vk_clear_surface(NV2AState *d, uint32_t parameter);
void pgraph_vk_draw_begin(NV2AState *d);
void pgraph_vk_draw_end(NV2AState *d);
void pgraph_vk_finish(PGRAPHState *pg, FinishReason why);
/* narrow-fence: submit the open batch WITHOUT waiting its fence (async early
 * submit) and reopen recording in the next ring slot on the next draw.
 * pgraph_vk_finish == submit_batch + wait_all_slots + recycle. */
void pgraph_vk_submit_batch(PGRAPHState *pg, FinishReason why);
/* Wait every in-flight ring submission (cheap when idle: skips signaled and
 * never-used slots). Callable before destroying GPU objects an in-flight
 * submission may still reference. */
void pgraph_vk_wait_all_slots(PGRAPHVkState *r);
/* Wait for exactly submission `idx` (a recorded submit_count value) to
 * retire; returns immediately if it never existed, was pushed out of the
 * ring (=> long complete), or is already signaled. */
void pgraph_vk_wait_for_submission(PGRAPHVkState *r, uint32_t idx);
void pgraph_vk_flush_draw(NV2AState *d);
void pgraph_vk_begin_command_buffer(PGRAPHState *pg);
void pgraph_vk_ensure_command_buffer(PGRAPHState *pg);
void pgraph_vk_ensure_not_in_render_pass(PGRAPHState *pg);

VkCommandBuffer pgraph_vk_begin_nondraw_commands(PGRAPHState *pg);
void pgraph_vk_end_nondraw_commands(PGRAPHState *pg, VkCommandBuffer cmd);

// blit.c
void pgraph_vk_image_blit(NV2AState *d);

// gpuprops.c
void pgraph_vk_determine_gpu_properties(NV2AState *d);
GPUProperties *pgraph_vk_get_gpu_properties(void);

#endif
