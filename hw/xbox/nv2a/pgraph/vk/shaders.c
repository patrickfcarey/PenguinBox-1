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
#include "qemu/mstring.h"
#include "renderer.h"

#define VSH_UBO_BINDING 0
#define PSH_UBO_BINDING 1
#define PSH_TEX_BINDING 2

const size_t MAX_UNIFORM_ATTR_VALUES_SIZE = NV2A_VERTEXSHADER_ATTRIBUTES * 4 * sizeof(float);

static void create_descriptor_pool(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    size_t num_sets = ARRAY_SIZE(r->descriptor_sets);

    VkDescriptorPoolSize pool_sizes[] = {
        {
            .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 2 * num_sets,
        },
        {
            .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = NV2A_MAX_TEXTURES * num_sets,
        }
    };

    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .poolSizeCount = ARRAY_SIZE(pool_sizes),
        .pPoolSizes = pool_sizes,
        .maxSets = ARRAY_SIZE(r->descriptor_sets),
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
    };
    VK_CHECK(vkCreateDescriptorPool(r->device, &pool_info, NULL,
                                    &r->descriptor_pool));
}

static void destroy_descriptor_pool(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkDestroyDescriptorPool(r->device, r->descriptor_pool, NULL);
    r->descriptor_pool = VK_NULL_HANDLE;
}

static void create_descriptor_set_layout(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkDescriptorSetLayoutBinding bindings[2 + NV2A_MAX_TEXTURES];

    bindings[0] = (VkDescriptorSetLayoutBinding){
        .binding = VSH_UBO_BINDING,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
    };
    bindings[1] = (VkDescriptorSetLayoutBinding){
        .binding = PSH_UBO_BINDING,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        bindings[2 + i] = (VkDescriptorSetLayoutBinding){
            .binding = PSH_TEX_BINDING + i,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        };
    }
    VkDescriptorSetLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = ARRAY_SIZE(bindings),
        .pBindings = bindings,
    };
    VK_CHECK(vkCreateDescriptorSetLayout(r->device, &layout_info, NULL,
                                         &r->descriptor_set_layout));
}

static void destroy_descriptor_set_layout(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkDestroyDescriptorSetLayout(r->device, r->descriptor_set_layout, NULL);
    r->descriptor_set_layout = VK_NULL_HANDLE;
}

static void create_descriptor_sets(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkDescriptorSetLayout layouts[ARRAY_SIZE(r->descriptor_sets)];
    for (int i = 0; i < ARRAY_SIZE(layouts); i++) {
        layouts[i] = r->descriptor_set_layout;
    }

    VkDescriptorSetAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = r->descriptor_pool,
        .descriptorSetCount = ARRAY_SIZE(r->descriptor_sets),
        .pSetLayouts = layouts,
    };
    VK_CHECK(
        vkAllocateDescriptorSets(r->device, &alloc_info, r->descriptor_sets));
}

static void destroy_descriptor_sets(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkFreeDescriptorSets(r->device, r->descriptor_pool,
                         ARRAY_SIZE(r->descriptor_sets), r->descriptor_sets);
    for (int i = 0; i < ARRAY_SIZE(r->descriptor_sets); i++) {
        r->descriptor_sets[i] = VK_NULL_HANDLE;
    }
}

/* --- frame-overlap S1 (XEMU_FRAME_OVERLAP): per-slot descriptor pools ---
 *
 * Under overlap the hazard is vkUpdateDescriptorSets rewriting a set an
 * unretired submission still references (audit §2e — the forecast #1 crash).
 * Fix shape: each CbFenceSlot owns a pool + a full-size set array; a slot's
 * sets are only ever written between its acquisition (previous occupant
 * retired) and its submit, and only ever read by its own submission. On
 * retire, the pool is vkResetDescriptorPool'd and the array re-allocated
 * (reset invalidates handles) — see overlap_process_slot_retire (draw.c).
 * Created only when the toggle is ON; the shared pool/sets above are still
 * created either way (kept: zero risk of a missed OFF-path reference; the
 * unused pool costs sub-MB). */

void pgraph_vk_overlap_alloc_slot_desc_sets(PGRAPHVkState *r,
                                            struct CbFenceSlot *slot)
{
    VkDescriptorSetLayout layouts[ARRAY_SIZE(slot->ov_desc_sets)];
    for (int i = 0; i < ARRAY_SIZE(layouts); i++) {
        layouts[i] = r->descriptor_set_layout;
    }

    VkDescriptorSetAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = slot->ov_desc_pool,
        .descriptorSetCount = ARRAY_SIZE(slot->ov_desc_sets),
        .pSetLayouts = layouts,
    };
    VK_CHECK(
        vkAllocateDescriptorSets(r->device, &alloc_info, slot->ov_desc_sets));
}

static void create_overlap_slot_descriptor_pools(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    for (int s = 0; s < CB_FENCE_RING_SIZE; s++) {
        struct CbFenceSlot *slot = &r->cb_fence_ring[s];
        size_t num_sets = ARRAY_SIZE(slot->ov_desc_sets);

        VkDescriptorPoolSize pool_sizes[] = {
            {
                .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .descriptorCount = 2 * num_sets,
            },
            {
                .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .descriptorCount = NV2A_MAX_TEXTURES * num_sets,
            }
        };

        /* No FREE_DESCRIPTOR_SET_BIT: these pools are recycled wholesale
         * via vkResetDescriptorPool at slot retire, never one set at a
         * time. */
        VkDescriptorPoolCreateInfo pool_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .poolSizeCount = ARRAY_SIZE(pool_sizes),
            .pPoolSizes = pool_sizes,
            .maxSets = num_sets,
        };
        VK_CHECK(vkCreateDescriptorPool(r->device, &pool_info, NULL,
                                        &slot->ov_desc_pool));
        pgraph_vk_overlap_alloc_slot_desc_sets(r, slot);
        slot->ov_desc_dirty = false;
    }
}

static void destroy_overlap_slot_descriptor_pools(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    for (int s = 0; s < CB_FENCE_RING_SIZE; s++) {
        struct CbFenceSlot *slot = &r->cb_fence_ring[s];
        /* Destroying the pool frees all its sets implicitly. */
        vkDestroyDescriptorPool(r->device, slot->ov_desc_pool, NULL);
        slot->ov_desc_pool = VK_NULL_HANDLE;
        for (int i = 0; i < ARRAY_SIZE(slot->ov_desc_sets); i++) {
            slot->ov_desc_sets[i] = VK_NULL_HANDLE;
        }
    }
}

void pgraph_vk_update_descriptor_sets(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    /* frame-overlap S1: descriptor sets are per-slot. This runs BEFORE
     * pgraph_vk_ensure_command_buffer in begin_pre_draw, so acquire the
     * slot the upcoming draw will record into NOW — the writes below must
     * land in ITS pool, and descriptor_set_index must reflect ITS
     * consumption (acquisition zeroes it, which both forces the
     * first-draw-of-slot write via the index==0 condition below and makes
     * a cross-slot set reference impossible). No-op when the slot is
     * already acquired (mid-slot draws). OFF: untouched. */
    if (r->frame_overlap) {
        pgraph_vk_overlap_acquire_slot(pg);
    }

    bool need_uniform_write =
        r->uniforms_changed ||
        !r->storage_buffers[BUFFER_UNIFORM_STAGING].buffer_offset;

    if (!(r->shader_bindings_changed || r->texture_bindings_changed ||
          (r->descriptor_set_index == 0) || need_uniform_write)) {
        return; // Nothing changed
    }

    ShaderBinding *binding = r->shader_binding;
    ShaderUniformLayout *layouts[] = { &binding->vsh.module_info->uniforms,
                                       &binding->psh.module_info->uniforms };
    VkDeviceSize ubo_buffer_total_size = 0;
    for (int i = 0; i < ARRAY_SIZE(layouts); i++) {
        ubo_buffer_total_size += layouts[i]->total_size;
    }
    bool need_ubo_staging_buffer_reset =
        r->uniforms_changed &&
        !pgraph_vk_buffer_has_space_for(pg, BUFFER_UNIFORM_STAGING,
                                        ubo_buffer_total_size,
                                        r->device_props.limits.minUniformBufferOffsetAlignment);

    bool need_descriptor_write_reset =
        (r->descriptor_set_index >= ARRAY_SIZE(r->descriptor_sets));

    if (need_descriptor_write_reset || need_ubo_staging_buffer_reset) {
        pgraph_vk_finish(pg, VK_FINISH_REASON_NEED_BUFFER_SPACE);
        need_uniform_write = true;
        /* frame-overlap S1: the finish submitted the open batch (clearing
         * ov_slot_acquired) — re-acquire so the writes below target the
         * NEXT submission's slot and a fresh index. */
        if (r->frame_overlap) {
            pgraph_vk_overlap_acquire_slot(pg);
        }
    }

    VkWriteDescriptorSet descriptor_writes[2 + NV2A_MAX_TEXTURES];

    assert(r->descriptor_set_index < ARRAY_SIZE(r->descriptor_sets));

    /* frame-overlap S1: write target — the recording slot's own set when
     * ON, the shared array (exactly as today) when OFF. Same index either
     * way; the per-slot array is the same size, so the exhaustion check
     * above bounds both identically. */
    VkDescriptorSet dst_set = r->frame_overlap ?
        r->current_slot->ov_desc_sets[r->descriptor_set_index] :
        r->descriptor_sets[r->descriptor_set_index];

    if (need_uniform_write) {
        for (int i = 0; i < ARRAY_SIZE(layouts); i++) {
            void *data = layouts[i]->allocation;
            VkDeviceSize size = layouts[i]->total_size;
            r->uniform_buffer_offsets[i] = pgraph_vk_append_to_buffer(
                pg, BUFFER_UNIFORM_STAGING, &data, &size, 1,
                r->device_props.limits.minUniformBufferOffsetAlignment);
        }

        r->uniforms_changed = false;
    }

    VkDescriptorBufferInfo ubo_buffer_infos[2];
    for (int i = 0; i < ARRAY_SIZE(layouts); i++) {
        ubo_buffer_infos[i] = (VkDescriptorBufferInfo){
            .buffer = r->storage_buffers[BUFFER_UNIFORM].buffer,
            .offset = r->uniform_buffer_offsets[i],
            .range = layouts[i]->total_size,
        };
        descriptor_writes[i] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = dst_set,
            .dstBinding = i == 0 ? VSH_UBO_BINDING : PSH_UBO_BINDING,
            .dstArrayElement = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 1,
            .pBufferInfo = &ubo_buffer_infos[i],
        };
    }

    VkDescriptorImageInfo image_infos[NV2A_MAX_TEXTURES];
    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        image_infos[i] = (VkDescriptorImageInfo){
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .imageView = r->texture_bindings[i]->image_view,
            .sampler = r->texture_bindings[i]->sampler,
        };
        descriptor_writes[2 + i] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = dst_set,
            .dstBinding = PSH_TEX_BINDING + i,
            .dstArrayElement = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .pImageInfo = &image_infos[i],
        };
    }

    vkUpdateDescriptorSets(r->device, 6, descriptor_writes, 0, NULL);

    r->descriptor_set_index++;
}

static void update_shader_uniform_locs(ShaderBinding *binding)
{
    for (int i = 0; i < ARRAY_SIZE(binding->vsh.uniform_locs); i++) {
        binding->vsh.uniform_locs[i] = uniform_index(
            &binding->vsh.module_info->uniforms, VshUniformInfo[i].name);
    }

    for (int i = 0; i < ARRAY_SIZE(binding->psh.uniform_locs); i++) {
        binding->psh.uniform_locs[i] = uniform_index(
            &binding->psh.module_info->uniforms, PshUniformInfo[i].name);
    }
}

static ShaderModuleInfo *
get_and_ref_shader_module_for_key(PGRAPHVkState *r,
                                  const ShaderModuleCacheKey *key)
{
    uint64_t hash = fast_hash((void *)key, sizeof(ShaderModuleCacheKey));
    LruNode *node = lru_lookup(&r->shader_module_cache, hash, key);
    ShaderModuleCacheEntry *module =
        container_of(node, ShaderModuleCacheEntry, node);
    pgraph_vk_ref_shader_module(module->module_info);
    return module->module_info;
}

static void shader_cache_entry_init(Lru *lru, LruNode *node, const void *state)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, shader_cache);
    ShaderBinding *binding = container_of(node, ShaderBinding, node);
    memcpy(&binding->state, state, sizeof(ShaderState));

    NV2A_VK_DPRINTF("cache miss");
    nv2a_profile_inc_counter(NV2A_PROF_SHADER_GEN);

    ShaderModuleCacheKey key;

    bool need_geometry_shader = pgraph_glsl_need_geom(&binding->state.geom);
    if (need_geometry_shader) {
        memset(&key, 0, sizeof(key));
        key.kind = VK_SHADER_STAGE_GEOMETRY_BIT;
        key.geom.state = binding->state.geom;
        key.geom.glsl_opts.vulkan = true;
        binding->geom.module_info = get_and_ref_shader_module_for_key(r, &key);
    } else {
        binding->geom.module_info = NULL;
    }

    memset(&key, 0, sizeof(key));
    key.kind = VK_SHADER_STAGE_VERTEX_BIT;
    key.vsh.state = binding->state.vsh;
    key.vsh.glsl_opts.vulkan = true;
    key.vsh.glsl_opts.prefix_outputs = need_geometry_shader;
    key.vsh.glsl_opts.use_push_constants_for_uniform_attrs =
        r->use_push_constants_for_uniform_attrs;
    key.vsh.glsl_opts.ubo_binding = VSH_UBO_BINDING;
    binding->vsh.module_info = get_and_ref_shader_module_for_key(r, &key);

    memset(&key, 0, sizeof(key));
    key.kind = VK_SHADER_STAGE_FRAGMENT_BIT;
    key.psh.state = binding->state.psh;
    key.psh.glsl_opts.vulkan = true;
    key.psh.glsl_opts.ubo_binding = PSH_UBO_BINDING;
    key.psh.glsl_opts.tex_binding = PSH_TEX_BINDING;
    binding->psh.module_info = get_and_ref_shader_module_for_key(r, &key);

    update_shader_uniform_locs(binding);
}

static void shader_cache_entry_post_evict(Lru *lru, LruNode *node)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, shader_cache);
    ShaderBinding *snode = container_of(node, ShaderBinding, node);

    ShaderModuleInfo *modules[] = {
        snode->vsh.module_info,
        snode->geom.module_info,
        snode->psh.module_info,
    };
    for (int i = 0; i < ARRAY_SIZE(modules); i++) {
        if (modules[i]) {
            pgraph_vk_unref_shader_module(r, modules[i]);
        }
    }
}

static bool shader_cache_entry_compare(Lru *lru, LruNode *node, const void *key)
{
    ShaderBinding *snode = container_of(node, ShaderBinding, node);
    return memcmp(&snode->state, key, sizeof(ShaderState));
}

static void shader_module_cache_entry_init(Lru *lru, LruNode *node,
                                           const void *key)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, shader_module_cache);
    ShaderModuleCacheEntry *module =
        container_of(node, ShaderModuleCacheEntry, node);
    memcpy(&module->key, key, sizeof(ShaderModuleCacheKey));

    MString *code;

    switch (module->key.kind) {
    case VK_SHADER_STAGE_VERTEX_BIT:
        code = pgraph_glsl_gen_vsh(&module->key.vsh.state,
                                   module->key.vsh.glsl_opts);
        break;
    case VK_SHADER_STAGE_GEOMETRY_BIT:
        code = pgraph_glsl_gen_geom(&module->key.geom.state,
                                    module->key.geom.glsl_opts);
        break;
    case VK_SHADER_STAGE_FRAGMENT_BIT:
        code = pgraph_glsl_gen_psh(&module->key.psh.state,
                                   module->key.psh.glsl_opts);
        break;
    default:
        assert(!"Invalid shader module kind");
        code = NULL;
    }

    module->module_info = pgraph_vk_create_shader_module_from_glsl(
        r, module->key.kind, mstring_get_str(code));
    pgraph_vk_ref_shader_module(module->module_info);
    mstring_unref(code);
}

static void shader_module_cache_entry_post_evict(Lru *lru, LruNode *node)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, shader_module_cache);
    ShaderModuleCacheEntry *module =
        container_of(node, ShaderModuleCacheEntry, node);
    pgraph_vk_unref_shader_module(r, module->module_info);
    module->module_info = NULL;
}

static bool shader_module_cache_entry_compare(Lru *lru, LruNode *node,
                                              const void *key)
{
    ShaderModuleCacheEntry *module =
        container_of(node, ShaderModuleCacheEntry, node);
    return memcmp(&module->key, key, sizeof(ShaderModuleCacheKey));
}

static void shader_cache_init(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    const size_t shader_cache_size = 1024;
    lru_init(&r->shader_cache);
    r->shader_cache_entries = g_malloc_n(shader_cache_size, sizeof(ShaderBinding));
    assert(r->shader_cache_entries != NULL);
    for (int i = 0; i < shader_cache_size; i++) {
        lru_add_free(&r->shader_cache, &r->shader_cache_entries[i].node);
    }
    r->shader_cache.init_node = shader_cache_entry_init;
    r->shader_cache.compare_nodes = shader_cache_entry_compare;
    r->shader_cache.post_node_evict = shader_cache_entry_post_evict;

    /* FIXME: Make this configurable */
    const size_t shader_module_cache_size = 50 * 1024;
    lru_init(&r->shader_module_cache);
    r->shader_module_cache_entries =
        g_malloc_n(shader_module_cache_size, sizeof(ShaderModuleCacheEntry));
    assert(r->shader_module_cache_entries != NULL);
    for (int i = 0; i < shader_module_cache_size; i++) {
        lru_add_free(&r->shader_module_cache,
                     &r->shader_module_cache_entries[i].node);
    }

    r->shader_module_cache.init_node = shader_module_cache_entry_init;
    r->shader_module_cache.compare_nodes = shader_module_cache_entry_compare;
    r->shader_module_cache.post_node_evict =
        shader_module_cache_entry_post_evict;
}

static void shader_cache_finalize(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    lru_flush(&r->shader_cache);
    g_free(r->shader_cache_entries);
    r->shader_cache_entries = NULL;

    lru_flush(&r->shader_module_cache);
    g_free(r->shader_module_cache_entries);
    r->shader_module_cache_entries = NULL;
}

static ShaderBinding *get_shader_binding_for_state(PGRAPHVkState *r,
                                                   const ShaderState *state)
{
    uint64_t hash = fast_hash((void *)state, sizeof(*state));
    LruNode *node = lru_lookup(&r->shader_cache, hash, state);
    ShaderBinding *binding = container_of(node, ShaderBinding, node);
    NV2A_VK_DPRINTF("shader state hash: %016" PRIx64 " %p", hash, binding);
    return binding;
}

static void apply_uniform_updates(ShaderUniformLayout *layout,
                                  const UniformInfo *info, int *locs,
                                  void *values, size_t count)
{
    for (int i = 0; i < count; i++) {
        if (locs[i] != -1) {
            uniform_copy(layout, locs[i], (char*)values + info[i].val_offs,
                         4, (info[i].size * info[i].count) / 4);
        }
    }
}

/*
 * perf-const-skip master gate. XEMU_CONST_SKIP=1 enables the per-draw uniform
 * recompute+hash skip; XEMU_NO_CONST_SKIP force-disables it. Read once at first
 * use. OFF (the default) leaves update_shader_uniforms byte-identical to
 * upstream: it unconditionally recomputes and hashes and never touches the
 * dirty arrays or the reuse snapshot.
 */
static bool const_skip_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        enabled = (getenv("XEMU_CONST_SKIP") != NULL &&
                   getenv("XEMU_NO_CONST_SKIP") == NULL)
                      ? 1
                      : 0;
        fprintf(stderr,
                "const-skip: per-draw uniform recompute+hash skip %s\n",
                enabled ? "ON" : "OFF");
    }
    return enabled;
}

static bool any_bool_set(const bool *flags, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (flags[i]) {
            return true;
        }
    }
    return false;
}

/*
 * The uniform-computation inputs that have NO consumable dirty signal
 * (material_alpha, point_params, specular_power, the fixed-function light_*
 * vectors) plus the surface-derived scalars that SET_SURFACE_FORMAT writes
 * without dirtying a register (anti_aliasing) or that derive from a surface
 * lookup (surface_binding_dim). Compared byte-exact against the last recompute.
 */
static bool const_skip_residual_matches(PGRAPHState *pg, PGRAPHVkState *r)
{
    return memcmp(&r->const_skip.material_alpha, &pg->material_alpha,
                  sizeof(pg->material_alpha)) == 0 &&
           memcmp(&r->const_skip.specular_power, &pg->specular_power,
                  sizeof(pg->specular_power)) == 0 &&
           memcmp(r->const_skip.point_params, pg->point_params,
                  sizeof(pg->point_params)) == 0 &&
           memcmp(r->const_skip.light_infinite_half_vector,
                  pg->light_infinite_half_vector,
                  sizeof(pg->light_infinite_half_vector)) == 0 &&
           memcmp(r->const_skip.light_infinite_direction,
                  pg->light_infinite_direction,
                  sizeof(pg->light_infinite_direction)) == 0 &&
           memcmp(r->const_skip.light_local_position, pg->light_local_position,
                  sizeof(pg->light_local_position)) == 0 &&
           memcmp(r->const_skip.light_local_attenuation,
                  pg->light_local_attenuation,
                  sizeof(pg->light_local_attenuation)) == 0 &&
           r->const_skip.surface_width == pg->surface_binding_dim.width &&
           r->const_skip.surface_height == pg->surface_binding_dim.height &&
           r->const_skip.aa == pg->surface_shape.anti_aliasing &&
           r->const_skip.z_format == pg->surface_shape.z_format &&
           r->const_skip.zeta_format == pg->surface_shape.zeta_format;
}

/*
 * True iff the previous draw's UBO can be reused verbatim: the shader binding
 * is unchanged AND every input to update_shader_uniforms is provably unchanged
 * since the last recompute. The signal set is complete over the inputs read by
 * pgraph_glsl_set_vsh_uniform_values / set_psh_uniform_values and the texScale
 * override (see the completeness argument in const_skip capture, below):
 *   - shader/texture binding-change flags -> all ShaderState-baked inputs and
 *     the per-texture scale/format;
 *   - regs_dirty (cleared per-draw right after bind_shaders) -> ALL PGRAPH
 *     register-derived inputs, checked whole-bitmap so nothing is missed;
 *   - vsh_constants_dirty[] / ltctxa|ltctxb|ltc1_dirty[] -> the transform
 *     constants and FF light context (consumed on recompute);
 *   - uniform_attrs==0 gate -> the inlineValue uniform is not emitted, so the
 *     untracked pg->vertex_attributes[].inline_value cannot reach the UBO;
 *   - the residual snapshot -> the few inputs with no consumable signal.
 * A false positive (recompute when nothing changed) is merely wasted work; a
 * false negative (skip when something changed) is silent corruption, so every
 * predicate errs toward recompute.
 */
static bool can_reuse_uniforms(PGRAPHState *pg, PGRAPHVkState *r)
{
    if (r->shader_bindings_changed || r->texture_bindings_changed) {
        return false;
    }
    if (!r->const_skip.valid) {
        return false; /* first draw / post-reset: nothing captured yet */
    }
    if (r->shader_binding->state.vsh.uniform_attrs != 0) {
        return false; /* inlineValue path is live; conservatively recompute */
    }
    if (!bitmap_empty(pg->regs_dirty, 0x2000 / sizeof(uint32_t))) {
        return false; /* some register-derived input may have changed */
    }
    if (any_bool_set(pg->vsh_constants_dirty, NV2A_VERTEXSHADER_CONSTANTS) ||
        any_bool_set(pg->ltctxa_dirty, NV2A_LTCTXA_COUNT) ||
        any_bool_set(pg->ltctxb_dirty, NV2A_LTCTXB_COUNT) ||
        any_bool_set(pg->ltc1_dirty, NV2A_LTC1_COUNT)) {
        return false;
    }
    return const_skip_residual_matches(pg, r);
}

/*
 * Capture the residual inputs for the next draw's reuse test and consume the
 * dirty arrays this recompute folded in. Called only on the recompute path and
 * only when const-skip is enabled.
 *
 * Completeness of the consumed arrays: vsh_constants_dirty[], ltctxa_dirty[],
 * ltctxb_dirty[] and ltc1_dirty[] are otherwise WRITE-ONLY in the tree (no
 * other reader clears them), so taking ownership of their clearing here is safe
 * and gives them "dirty since last recompute" semantics.
 */
static void capture_const_skip_state(PGRAPHState *pg, PGRAPHVkState *r)
{
    r->const_skip.material_alpha = pg->material_alpha;
    r->const_skip.specular_power = pg->specular_power;
    memcpy(r->const_skip.point_params, pg->point_params,
           sizeof(pg->point_params));
    memcpy(r->const_skip.light_infinite_half_vector,
           pg->light_infinite_half_vector,
           sizeof(pg->light_infinite_half_vector));
    memcpy(r->const_skip.light_infinite_direction,
           pg->light_infinite_direction,
           sizeof(pg->light_infinite_direction));
    memcpy(r->const_skip.light_local_position, pg->light_local_position,
           sizeof(pg->light_local_position));
    memcpy(r->const_skip.light_local_attenuation, pg->light_local_attenuation,
           sizeof(pg->light_local_attenuation));
    r->const_skip.surface_width = pg->surface_binding_dim.width;
    r->const_skip.surface_height = pg->surface_binding_dim.height;
    r->const_skip.aa = pg->surface_shape.anti_aliasing;
    r->const_skip.z_format = pg->surface_shape.z_format;
    r->const_skip.zeta_format = pg->surface_shape.zeta_format;
    r->const_skip.valid = true;

    memset(pg->vsh_constants_dirty, 0, sizeof(pg->vsh_constants_dirty));
    memset(pg->ltctxa_dirty, 0, sizeof(pg->ltctxa_dirty));
    memset(pg->ltctxb_dirty, 0, sizeof(pg->ltctxb_dirty));
    memset(pg->ltc1_dirty, 0, sizeof(pg->ltc1_dirty));
}

// FIXME: Dirty tracking
static void update_shader_uniforms(PGRAPHState *pg)
{
    NV2A_VK_DGROUP_BEGIN("%s", __func__);

    PGRAPHVkState *r = pg->vk_renderer_state;
    nv2a_profile_inc_counter(NV2A_PROF_SHADER_BIND);

    assert(r->shader_binding);

    bool const_skip_on = const_skip_enabled();
    if (const_skip_on && can_reuse_uniforms(pg, r)) {
        /*
         * Reuse the previous UBO offset/descriptor. r->uniforms_changed is
         * false on entry (only update_shader_uniforms sets it and only
         * update_descriptor_sets resets it, immediately after), so leaving it
         * untouched makes update_descriptor_sets reuse the prior uniform buffer
         * offsets and descriptor set. This is behaviorally identical to running
         * the recompute: the module's uniform allocation already holds these
         * (unchanged) values, and the content hash would have left
         * uniforms_changed false.
         */
        nv2a_profile_inc_counter(NV2A_PROF_UNIFORM_SKIP);
        NV2A_VK_DGROUP_END();
        return;
    }

    ShaderBinding *binding = r->shader_binding;
    ShaderUniformLayout *layouts[] = { &binding->vsh.module_info->uniforms,
                                       &binding->psh.module_info->uniforms };

    VshUniformValues vsh_values;
    pgraph_glsl_set_vsh_uniform_values(pg, &binding->state.vsh,
                                  binding->vsh.uniform_locs, &vsh_values);
    apply_uniform_updates(&binding->vsh.module_info->uniforms, VshUniformInfo,
                          binding->vsh.uniform_locs, &vsh_values,
                          VshUniform__COUNT);

    PshUniformValues psh_values;
    pgraph_glsl_set_psh_uniform_values(pg, binding->psh.uniform_locs,
                                       &psh_values);
    for (int i = 0; i < 4; i++) {
        assert(r->texture_bindings[i] != NULL);
        float scale = r->texture_bindings[i]->key.scale;

        BasicColorFormatInfo f_basic =
            kelvin_color_format_info_map[pg->vk_renderer_state
                                             ->texture_bindings[i]
                                             ->key.state.color_format];
        if (!f_basic.linear) {
            scale = 1.0;
        }

        psh_values.texScale[i] = scale;
    }
    apply_uniform_updates(&binding->psh.module_info->uniforms, PshUniformInfo,
                          binding->psh.uniform_locs, &psh_values,
                          PshUniform__COUNT);

    for (int i = 0; i < ARRAY_SIZE(layouts); i++) {
        uint64_t hash =
            fast_hash(layouts[i]->allocation, layouts[i]->total_size);
        r->uniforms_changed |= (hash != r->uniform_buffer_hashes[i]);
        r->uniform_buffer_hashes[i] = hash;
    }

    nv2a_profile_inc_counter(r->uniforms_changed ?
                                 NV2A_PROF_SHADER_UBO_DIRTY :
                                 NV2A_PROF_SHADER_UBO_NOTDIRTY);

    if (const_skip_on) {
        capture_const_skip_state(pg, r);
    }

    NV2A_VK_DGROUP_END();
}

void pgraph_vk_bind_shaders(PGRAPHState *pg)
{
    NV2A_VK_DGROUP_BEGIN("%s", __func__);

    PGRAPHVkState *r = pg->vk_renderer_state;

    r->shader_bindings_changed = false;

    if (!r->shader_binding ||
        pgraph_glsl_check_shader_state_dirty(pg, &r->shader_binding->state)) {
        ShaderState new_state = pgraph_glsl_get_shader_state(pg);
        if (!r->shader_binding || memcmp(&r->shader_binding->state, &new_state,
                                         sizeof(ShaderState))) {
            r->shader_binding = get_shader_binding_for_state(r, &new_state);
            r->shader_bindings_changed = true;
        }
    } else {
        nv2a_profile_inc_counter(NV2A_PROF_SHADER_BIND_NOTDIRTY);
    }

    update_shader_uniforms(pg);

    NV2A_VK_DGROUP_END();
}

void pgraph_vk_init_shaders(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    /* frame-overlap S1: cache the getenv-once master gate before any
     * consumer (this init runs before pgraph_vk_init_pipelines and long
     * before the first draw; r comes from g_malloc0, so OFF is the default
     * even if a path checked earlier). */
    r->frame_overlap = pgraph_vk_frame_overlap_enabled();

    pgraph_vk_init_glsl_compiler();
    create_descriptor_pool(pg);
    create_descriptor_set_layout(pg);
    create_descriptor_sets(pg);
    if (r->frame_overlap) {
        create_overlap_slot_descriptor_pools(pg);
    }
    shader_cache_init(pg);

    r->use_push_constants_for_uniform_attrs =
        (r->device_props.limits.maxPushConstantsSize >=
         MAX_UNIFORM_ATTR_VALUES_SIZE);
}

void pgraph_vk_finalize_shaders(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    shader_cache_finalize(pg);
    if (r->frame_overlap) {
        destroy_overlap_slot_descriptor_pools(pg);
    }
    destroy_descriptor_sets(pg);
    destroy_descriptor_set_layout(pg);
    destroy_descriptor_pool(pg);
    pgraph_vk_finalize_glsl_compiler();
}
