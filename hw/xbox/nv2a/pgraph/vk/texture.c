/*
 * Geforce NV2A PGRAPH Vulkan Renderer
 *
 * Copyright (c) 2024 Matt Borgerson
 *
 * Based on GL implementation:
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2018-2024 Matt Borgerson
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
#include "hw/xbox/nv2a/pgraph/s3tc.h"
#include "hw/xbox/nv2a/pgraph/swizzle.h"
#include "qemu/fast-hash.h"
#include "qemu/lru.h"
#include "hw/xbox/nv2a/pgraph/phase_timers.h"
#include "renderer.h"

static void texture_cache_release_node_resources(PGRAPHVkState *r, TextureBinding *snode);

static const VkImageType dimensionality_to_vk_image_type[] = {
    0,
    VK_IMAGE_TYPE_1D,
    VK_IMAGE_TYPE_2D,
    VK_IMAGE_TYPE_3D,
};
static const VkImageViewType dimensionality_to_vk_image_view_type[] = {
    0,
    VK_IMAGE_VIEW_TYPE_1D,
    VK_IMAGE_VIEW_TYPE_2D,
    VK_IMAGE_VIEW_TYPE_3D,
};

static VkSamplerAddressMode lookup_texture_address_mode(int idx)
{
    assert(0 < idx && idx < ARRAY_SIZE(pgraph_texture_addr_vk_map));
    return pgraph_texture_addr_vk_map[idx];
}

// FIXME: Move to common
// FIXME: We can shrink the size of this structure
// FIXME: Use simple allocator
typedef struct TextureLevel {
    unsigned int width, height, depth;
    hwaddr vram_addr;
    void *decoded_data;
    size_t decoded_size;
} TextureLevel;

typedef struct TextureLayer {
    TextureLevel levels[16];
} TextureLayer;

typedef struct TextureLayout {
    TextureLayer layers[6];
} TextureLayout;

// FIXME: Move to common
static enum S3TC_DECOMPRESS_FORMAT kelvin_format_to_s3tc_format(int color_format)
{
    switch (color_format) {
    case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5:
        return S3TC_DECOMPRESS_FORMAT_DXT1;
    case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT23_A8R8G8B8:
        return S3TC_DECOMPRESS_FORMAT_DXT3;
    case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT45_A8R8G8B8:
        return S3TC_DECOMPRESS_FORMAT_DXT5;
    default:
        assert(!"Invalid texture color format");
    }
}

// FIXME: Move to common
static void memcpy_image(void *dst, void *src, int min_stride, int dst_stride, int src_stride, int height)
{
    uint8_t *dst_ptr = (uint8_t *)dst;
    uint8_t *src_ptr = (uint8_t *)src;

    for (int i = 0; i < height; i++) {
        memcpy(dst_ptr, src_ptr, min_stride);
        src_ptr += src_stride;
        dst_ptr += dst_stride;
    }
}

// FIXME: Move to common
static size_t get_cubemap_layer_size(PGRAPHState *pg, TextureShape s)
{
    BasicColorFormatInfo f = kelvin_color_format_info_map[s.color_format];
    bool is_compressed =
        pgraph_is_texture_format_compressed(pg, s.color_format);
    unsigned int block_size;

    unsigned int w = s.width, h = s.height;
    size_t length = 0;

    if (!f.linear && s.border) {
        w = MAX(16, w * 2);
        h = MAX(16, h * 2);
    }

    if (is_compressed) {
        block_size =
            s.color_format == NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5 ?
                8 :
                16;
    }

    for (int level = 0; level < s.levels; level++) {
        if (is_compressed) {
            length += w / 4 * h / 4 * block_size;
        } else {
            length += w * h * f.bytes_per_pixel;
        }

        w /= 2;
        h /= 2;
    }

    return ROUND_UP(length, NV2A_CUBEMAP_FACE_ALIGNMENT);
}

// FIXME: Move to common
// FIXME: More refactoring
// FIXME: Possible parallelization of decoding
// FIXME: Bounds checking
static TextureLayout *get_texture_layout(PGRAPHState *pg, int texture_idx)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    TextureShape s = pgraph_get_texture_shape(pg, texture_idx);
    BasicColorFormatInfo f = kelvin_color_format_info_map[s.color_format];

    NV2A_VK_DGROUP_BEGIN("Texture %d: cubemap=%d, dimensionality=%d, color_format=0x%x, levels=%d, width=%d, height=%d, depth=%d border=%d, min_mipmap_level=%d, max_mipmap_level=%d, pitch=%d",
        texture_idx,
        s.cubemap,
        s.dimensionality,
        s.color_format,
        s.levels,
        s.width,
        s.height,
        s.depth,
        s.border,
        s.min_mipmap_level,
        s.max_mipmap_level,
        s.pitch
        );

    // Sanity checks on below assumptions
    if (f.linear) {
        assert(s.dimensionality == 2);
    }
    if (s.cubemap) {
        assert(s.dimensionality == 2);
        assert(!f.linear);
    }
    assert(s.dimensionality > 1);

    const hwaddr texture_vram_offset = pgraph_get_texture_phys_addr(pg, texture_idx);
    void *texture_data_ptr = (char *)d->vram_ptr + texture_vram_offset;

    size_t texture_palette_data_size;
    const hwaddr texture_palette_vram_offset =
        pgraph_get_texture_palette_phys_addr_length(pg, texture_idx,
                                                    &texture_palette_data_size);
    void *palette_data_ptr = (char *)d->vram_ptr + texture_palette_vram_offset;

    unsigned int adjusted_width = s.width, adjusted_height = s.height,
                 adjusted_pitch = s.pitch, adjusted_depth = s.depth;

    if (!f.linear && s.border) {
        adjusted_width = MAX(16, adjusted_width * 2);
        adjusted_height = MAX(16, adjusted_height * 2);
        adjusted_pitch = adjusted_width * (s.pitch / s.width);
        adjusted_depth = MAX(16, s.depth * 2);
    }

    TextureLayout *layout = g_malloc0(sizeof(TextureLayout));

    if (f.linear) {
        assert(s.pitch % f.bytes_per_pixel == 0 && "Can't handle strides unaligned to pixels");

        size_t converted_size;
        uint8_t *converted = pgraph_convert_texture_data(
            s, texture_data_ptr, palette_data_ptr, adjusted_width,
            adjusted_height, 1, adjusted_pitch, 0, &converted_size);

        if (!converted) {
            int dst_stride = adjusted_width * f.bytes_per_pixel;
            assert(adjusted_width <= s.width);
            converted_size = dst_stride * adjusted_height;
            converted = g_malloc(converted_size);
            memcpy_image(converted, texture_data_ptr, adjusted_width * f.bytes_per_pixel, dst_stride,
                         adjusted_pitch, adjusted_height);
        }

        assert(s.levels == 1);
        layout->layers[0].levels[0] = (TextureLevel){
            .width = adjusted_width,
            .height = adjusted_height,
            .depth = 1,
            .decoded_size = converted_size,
            .decoded_data = converted,
        };

        NV2A_VK_DGROUP_END();
        return layout;
    }

    bool is_compressed = pgraph_is_texture_format_compressed(pg, s.color_format);
    size_t block_size = 0;
    if (is_compressed) {
        bool is_dxt1 =
            s.color_format == NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5;
        block_size = is_dxt1 ? 8 : 16;
    }

    if (s.dimensionality == 2) {
        hwaddr layer_size = s.cubemap ? get_cubemap_layer_size(pg, s) : 0;
        const int num_layers = s.cubemap ? 6 : 1;
        for (int layer = 0; layer < num_layers; layer++) {
            unsigned int width = adjusted_width, height = adjusted_height;
            texture_data_ptr = (char *)d->vram_ptr + texture_vram_offset +
                               layer * layer_size;

            for (int level = 0; level < s.levels; level++) {
                NV2A_VK_DPRINTF("Layer %d Level %d @ %x", layer, level, (int)((char*)texture_data_ptr - (char*)d->vram_ptr));

                width = MAX(width, 1);
                height = MAX(height, 1);
                if (is_compressed) {
                    // https://docs.microsoft.com/en-us/windows/win32/direct3d10/d3d10-graphics-programming-guide-resources-block-compression#virtual-size-versus-physical-size
                    unsigned int tex_width = width, tex_height = height;
                    unsigned int physical_width = (width + 3) & ~3,
                                 physical_height = (height + 3) & ~3;

                    size_t converted_size = width * height * 4;
                    uint8_t *converted = s3tc_decompress_2d(
                        kelvin_format_to_s3tc_format(s.color_format),
                        texture_data_ptr, width, height);
                    assert(converted);

                    if (s.cubemap && adjusted_width != s.width) {
                        // FIXME: Consider preserving the border.
                        // There does not seem to be a way to reference the border
                        // texels in a cubemap, so they are discarded.

                        // glPixelStorei(GL_UNPACK_SKIP_PIXELS, 4);
                        // glPixelStorei(GL_UNPACK_SKIP_ROWS, 4);
                        tex_width = s.width;
                        tex_height = s.height;
                        // if (physical_width == width) {
                        //     glPixelStorei(GL_UNPACK_ROW_LENGTH, adjusted_width);
                        // }

                        // FIXME: Crop by 4 pixels on each side
                    }

                    layout->layers[layer].levels[level] = (TextureLevel){
                        .width = tex_width,
                        .height = tex_height,
                        .depth = 1,
                        .decoded_size = converted_size,
                        .decoded_data = converted,
                    };

                    texture_data_ptr +=
                        physical_width / 4 * physical_height / 4 * block_size;
                } else {
                    unsigned int pitch = width * f.bytes_per_pixel;
                    unsigned int tex_width = width, tex_height = height;

                    size_t converted_size = height * pitch;
                    uint8_t *unswizzled = (uint8_t*)g_malloc(height * pitch);
                    unswizzle_rect(texture_data_ptr, width, height,
                                   unswizzled, pitch, f.bytes_per_pixel);

                    uint8_t *converted = pgraph_convert_texture_data(
                        s, unswizzled, palette_data_ptr, width, height, 1,
                        pitch, 0, &converted_size);

                    if (converted) {
                        g_free(unswizzled);
                    } else {
                        converted = unswizzled;
                    }

                    if (s.cubemap && adjusted_width != s.width) {
                        // FIXME: Consider preserving the border.
                        // There does not seem to be a way to reference the border
                        // texels in a cubemap, so they are discarded.
                        // glPixelStorei(GL_UNPACK_ROW_LENGTH, adjusted_width);
                        tex_width = s.width;
                        tex_height = s.height;
                        // pixel_data += 4 * f.bytes_per_pixel + 4 * pitch;

                        // FIXME: Crop by 4 pixels on each side
                    }

                    layout->layers[layer].levels[level] = (TextureLevel){
                        .width = tex_width,
                        .height = tex_height,
                        .depth = 1,
                        .decoded_size = converted_size,
                        .decoded_data = converted,
                    };

                    texture_data_ptr += width * height * f.bytes_per_pixel;
                }

                width /= 2;
                height /= 2;
            }
        }
    } else if (s.dimensionality == 3) {
        assert(!f.linear);
        unsigned int width = adjusted_width, height = adjusted_height,
                     depth = adjusted_depth;

        for (int level = 0; level < s.levels; level++) {
            if (is_compressed) {
                width = MAX(width, 1);
                height = MAX(height, 1);
                unsigned int physical_width = (width + 3) & ~3,
                             physical_height = (height + 3) & ~3;
                depth = MAX(depth, 1);

                size_t converted_size = width * height * depth * 4;
                uint8_t *converted = s3tc_decompress_3d(
                    kelvin_format_to_s3tc_format(s.color_format),
                    texture_data_ptr, width, height, depth);
                assert(converted);

                layout->layers[0].levels[level] = (TextureLevel){
                    .width = width,
                    .height = height,
                    .depth = depth,
                    .decoded_size = converted_size,
                    .decoded_data = converted,
                };

                texture_data_ptr += physical_width / 4 * physical_height / 4 * depth * block_size;
            } else {
                width = MAX(width, 1);
                height = MAX(height, 1);
                depth = MAX(depth, 1);

                unsigned int row_pitch = width * f.bytes_per_pixel;
                unsigned int slice_pitch = row_pitch * height;

                size_t unswizzled_size = slice_pitch * depth;
                uint8_t *unswizzled = g_malloc(unswizzled_size);
                unswizzle_box(texture_data_ptr, width, height, depth,
                              unswizzled, row_pitch, slice_pitch,
                              f.bytes_per_pixel);

                size_t converted_size;
                uint8_t *converted = pgraph_convert_texture_data(
                    s, unswizzled, palette_data_ptr, width, height, depth,
                    row_pitch, slice_pitch, &converted_size);

                if (converted) {
                    g_free(unswizzled);
                } else {
                    converted = unswizzled;
                    converted_size = unswizzled_size;
                }

                layout->layers[0].levels[level] = (TextureLevel){
                    .width = width,
                    .height = height,
                    .depth = depth,
                    .decoded_size = converted_size,
                    .decoded_data = converted,
                };

                texture_data_ptr += width * height * depth * f.bytes_per_pixel;
            }

            width /= 2;
            height /= 2;
            depth /= 2;
        }
    }

    NV2A_VK_DGROUP_END();
    return layout;
}

/* ── sb-graphics-research: exact-range dirty narrowing v2 — CHANNEL SEPARATION
 * DEFAULT OFF (opt-in XEMU_EXACT_DIRTY=1; XEMU_NO_EXACT_DIRTY=1 force-off wins).
 * OFF => byte-identical to stock page-granular behavior.
 *
 * v1 (default-off, ISS-B16 Source 1) narrowed the texture re-hash by
 * intersecting each texture's true byte range against the per-frame spans of
 * exact NV2A_TEX writers (surface download, 2D blit), skipping textures that
 * merely SHARED A PAGE with a writer. That fixed the radar's -60% co-page
 * re-hash storm but caused mission FLICKER: v1 read the page bitmap and the
 * exact spans TOGETHER, so a texture whose page tested dirty and also happened
 * to sit on an exact writer's page was attributed to that writer and
 * page-hit-SKIPPED — even when the dirty bit was really a genuine guest CPU
 * store to byte-disjoint co-page texture memory. Stale one frame, fresh the
 * next => flicker (the documented co-page FALSE NEGATIVE).
 *
 * v2 removes that false negative BY CONSTRUCTION by separating the dirty signal
 * into two channels keyed on writer class (full rationale on
 * PGRAPHVkState.exact_dirty_spans in renderer.h):
 *   - exact-knowable writers (download, blit) contribute ONLY to the exact-span
 *     channel and STOP setting the DIRTY_MEMORY_NV2A_TEX page bitmap for their
 *     range (surface.c / blit.c);
 *   - unattributed writers (guest CPU stores) remain on the page bitmap and are
 *     consulted page-conservatively (always hash on a page hit).
 * A texture is dirty iff (bytes intersect an exact span) OR (its pages are
 * dirty in the bitmap). Since an exact writer no longer sets the page bitmap, a
 * page-dirty bit now ALWAYS means a genuine unattributed write => must hash =>
 * the co-page false negative cannot occur. Byte-exact for our writers (keeps
 * the radar win), page-conservative for guest writes (no flicker). Default
 * stays OFF until rig flicker_detect.py + owner eyes clear the mission scene. */
static bool exact_dirty_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        if (getenv("XEMU_NO_EXACT_DIRTY")) {
            enabled = 0;
        } else {
            enabled = getenv("XEMU_EXACT_DIRTY") ? 1 : 0;
        }
    }
    return enabled;
}

bool pgraph_vk_note_exact_dirty(NV2AState *d, hwaddr addr, hwaddr size)
{
    if (!exact_dirty_enabled() || size == 0) {
        return false; /* disabled/empty => caller keeps the page bitmap (stock) */
    }

    PGRAPHVkState *r = d->pgraph.vk_renderer_state;
    if (!r->exact_spans_valid) {
        /* Pre-first-flip only (g_malloc0 leaves valid=false): Channel S is not
         * yet trusted, so behave as stock — caller sets the page bitmap. In v2
         * this flag is NEVER cleared mid-frame, so no already-recorded span can
         * be stranded by a later flip to false. */
        return false;
    }

    if (r->exact_dirty_count >= EXACT_DIRTY_MAX_SPANS) {
        /* Channel S is full. v2 does NOT invalidate the frame here (v1 did):
         * the spans already recorded stay valid and consulted — invalidating
         * would orphan the exact writers that ALREADY skipped the page bitmap
         * for those spans (Channel S ignored + Channel P skipped => the write
         * vanishes => silent texture corruption). Instead THIS overflowing write
         * degrades to Channel P: return false so the caller sets the page bitmap
         * for its range. That texture re-hashes page-conservatively — sound, and
         * still never a false negative. */
        return false;
    }

    r->exact_dirty_spans[r->exact_dirty_count].addr = addr;
    r->exact_dirty_spans[r->exact_dirty_count].size = size;
    r->exact_dirty_count++;
    return true; /* recorded in Channel S => caller MUST skip the page bitmap */
}

struct pgraph_texture_possibly_dirty_struct {
    hwaddr addr, end;       /* page-expanded range (stock, page-granular mode) */
    bool use_exact_spans;   /* narrow by exact byte spans instead of pages */
    PGRAPHVkState *r;
};

/* sb-graphics-research: does this cached texture's ACTUAL byte range (or its
 * palette's) intersect any exact-writer span recorded this frame? Byte-precise
 * on purpose — the whole point is to NOT flag a texture that merely shares a
 * page with the writer. */
static bool tnode_intersects_exact_spans(PGRAPHVkState *r,
                                         TextureBinding *tnode)
{
    hwaddr t_lo = tnode->key.texture_vram_offset;
    hwaddr t_hi = t_lo + tnode->key.texture_length;
    bool has_pal = tnode->key.palette_length > 0;
    hwaddr p_lo = tnode->key.palette_vram_offset;
    hwaddr p_hi = p_lo + tnode->key.palette_length;

    for (unsigned int i = 0; i < r->exact_dirty_count; i++) {
        hwaddr s_lo = r->exact_dirty_spans[i].addr;
        hwaddr s_hi = s_lo + r->exact_dirty_spans[i].size;
        if (t_lo < s_hi && s_lo < t_hi) {
            return true;
        }
        if (has_pal && p_lo < s_hi && s_lo < p_hi) {
            return true;
        }
    }
    return false;
}

static void mark_textures_possibly_dirty_visitor(Lru *lru, LruNode *node, void *opaque)
{
    struct pgraph_texture_possibly_dirty_struct *test = opaque;

    TextureBinding *tnode = container_of(node, TextureBinding, node);
    if (tnode->possibly_dirty) {
        return;
    }

    bool overlapping;
    if (test->use_exact_spans) {
        /* Channel-S (byte-exact) fan-out: flag ONLY textures whose real bytes
         * intersect an exact writer's span, not every texture that merely shares
         * a page. The page-granular fan-out is the false-positive storm — one
         * radar download would flag every VRAM-adjacent texture. Sound because a
         * texture the exact writer's bytes miss is clean w.r.t. that writer, and
         * any genuine unattributed (guest CPU) write to it lands on Channel P
         * (the page bitmap) and is caught page-conservatively on its own bind.
         * In v2 the exact writer does not set Channel P at all, so this narrow
         * flagging cannot hide such a write. */
        overlapping = tnode_intersects_exact_spans(test->r, tnode);
    } else {
        uintptr_t k_tex_addr = tnode->key.texture_vram_offset;
        uintptr_t k_tex_end = k_tex_addr + tnode->key.texture_length - 1;
        overlapping = !(test->addr > k_tex_end || k_tex_addr > test->end);

        if (tnode->key.palette_length > 0) {
            uintptr_t k_pal_addr = tnode->key.palette_vram_offset;
            uintptr_t k_pal_end = k_pal_addr + tnode->key.palette_length - 1;
            overlapping |= !(test->addr > k_pal_end || k_pal_addr > test->end);
        }
    }

    tnode->possibly_dirty |= overlapping;
}

void pgraph_vk_mark_textures_possibly_dirty(NV2AState *d,
    hwaddr addr, hwaddr size, bool use_exact_spans)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;
    hwaddr end = TARGET_PAGE_ALIGN(addr + size) - 1;
    addr &= TARGET_PAGE_MASK;
    assert(end <= memory_region_size(d->vram));

    struct pgraph_texture_possibly_dirty_struct test = {
        .addr = addr,
        .end = end,
        /* Only narrow when the caller vouched for exact spans AND they are
         * enabled and trustworthy this frame; otherwise fall back to stock
         * page overlap (e.g. the whole-VRAM invalidate from pgraph_vk_flush,
         * which passes use_exact_spans=false and MUST flag everything). */
        .use_exact_spans =
            use_exact_spans && exact_dirty_enabled() && r->exact_spans_valid,
        .r = r,
    };

    lru_visit_active(&r->texture_cache,
                     mark_textures_possibly_dirty_visitor,
                     &test);
}

static bool check_texture_dirty(NV2AState *d, hwaddr addr, hwaddr size)
{
    hwaddr end = TARGET_PAGE_ALIGN(addr + size);
    addr &= TARGET_PAGE_MASK;
    assert(end < memory_region_size(d->vram));
    return memory_region_test_and_clear_dirty(d->vram, addr, end - addr,
                                              DIRTY_MEMORY_NV2A_TEX);
}

/* sb-graphics-research v2: does this texture/palette byte range share actual
 * BYTES with any exact-writer span recorded this frame (Channel S)? Byte-precise
 * on purpose — a range that merely shares a PAGE with a writer is NOT a hit
 * (that page-only overlap is exactly the radar false-positive v2 drops). v2 has
 * no page_hit concept: exact writers no longer touch the page bitmap, so a
 * texture's page relationship to a writer is irrelevant — page-dirtiness is a
 * separate, independent signal (Channel P). Caller must hold exact_spans_valid. */
static bool range_intersects_exact_spans(PGRAPHVkState *r, hwaddr addr,
                                         unsigned int size)
{
    hwaddr t_lo = addr;
    hwaddr t_hi = addr + size;

    for (unsigned int i = 0; i < r->exact_dirty_count; i++) {
        hwaddr s_lo = r->exact_dirty_spans[i].addr;
        hwaddr s_hi = s_lo + r->exact_dirty_spans[i].size;
        if (t_lo < s_hi && s_lo < t_hi) {
            return true;
        }
    }
    return false;
}

/* sb-graphics-research v2, MEASUREMENT ONLY: does this range share a PAGE (not
 * necessarily bytes) with any exact-writer span? Used solely to count genuine
 * avoided hashes for NV2A_PROF_TEX_HASH_SKIPPED: a clean texture whose pages an
 * exact writer touched is one STOCK would have re-hashed (stock's page bit would
 * have been set by that writer, which v2 no longer does), so v2 skipping it is a
 * real win that "absorbs" a stock hash. A texture with no span near its pages
 * was clean under stock too and is NOT counted. This page relationship NEVER
 * feeds the skip decision — putting it back into the decision is exactly the v1
 * co-page false-negative bug. */
static bool range_pages_overlap_exact_spans(PGRAPHVkState *r, hwaddr addr,
                                            unsigned int size)
{
    hwaddr tp_lo = addr & TARGET_PAGE_MASK;
    hwaddr tp_hi = TARGET_PAGE_ALIGN(addr + size);

    for (unsigned int i = 0; i < r->exact_dirty_count; i++) {
        hwaddr sp_lo = r->exact_dirty_spans[i].addr & TARGET_PAGE_MASK;
        hwaddr sp_hi = TARGET_PAGE_ALIGN(r->exact_dirty_spans[i].addr +
                                         r->exact_dirty_spans[i].size);
        if (tp_lo < sp_hi && sp_lo < tp_hi) {
            return true;
        }
    }
    return false;
}

/* Decide whether one texture/palette byte range needs a content re-hash, and
 * propagate the possibly-dirty flag exactly as far as is sound. Returns true
 * => hash. This is the CHANNEL-SEPARATION consumer (v2): the two dirty channels
 * are consulted INDEPENDENTLY and OR-combined.
 *
 *   Channel P — the DIRTY_MEMORY_NV2A_TEX page bitmap. In v2 it carries ONLY
 *     unattributed writers (guest CPU/DMA stores, set via the softmmu notdirty
 *     path on TB exit) plus any exact write that overflowed Channel S. Exact
 *     writers whose span was recorded do NOT set it. So a set page bit ALWAYS
 *     denotes a genuine write that must hash — we test_and_clear it and fan out
 *     page-conservatively, exactly like stock.
 *   Channel S — the exact-writer spans (range_intersects_exact_spans). Byte-
 *     precise; a hit means an exact writer overwrote our very bytes.
 *
 * False-negative-impossibility proof (the v1 flicker cannot return):
 *  - Radar page-neighbor (bytes disjoint from the download, guest never wrote
 *    it): Channel P bit is CLEAN because the download no longer sets it; Channel
 *    S misses (byte-disjoint) => SKIP. This is the narrowing win.
 *  - Animated co-page texture the guest CPU rewrites every frame (the v1 flicker
 *    case): the guest store set the Channel P bit (the download did NOT) =>
 *    page-dirty => HASH. Never one stale frame => no flicker. Nothing an exact
 *    writer does can appear on Channel P, so this case can never be skipped.
 *  - Texture the download genuinely overwrote (bytes overlap): Channel P clean
 *    (download skipped it), Channel S hits => HASH.
 *  - Overflow / pre-first-flip exact write: it fell back to Channel P (see
 *    pgraph_vk_note_exact_dirty) => caught by the page test => HASH.
 * XEMU_NO_EXACT_DIRTY=1 (or default OFF) takes the stock single-channel page
 * test below, byte-for-byte identical to upstream. */
static bool check_texture_range_dirty(NV2AState *d, hwaddr addr,
                                      unsigned int size)
{
    if (size == 0) {
        return false;
    }

    PGRAPHVkState *r = d->pgraph.vk_renderer_state;

    if (!exact_dirty_enabled() || !r->exact_spans_valid) {
        /* Stock: page-granular test_and_clear + conservative page fan-out of
         * every overlapping cached texture. exact_spans_valid is false only
         * before the first flip; v2 keeps it true for the rest of the process. */
        if (check_texture_dirty(d, addr, size)) {
            pgraph_vk_mark_textures_possibly_dirty(d, addr, size, false);
            return true;
        }
        return false;
    }

    /* Channel P first: consume + fan out any GENUINE unattributed dirty. We
     * test_and_clear unconditionally so a co-page sibling sharing a guest
     * writer's page is flagged now (stock semantics) instead of racing the
     * cleared bit. A hit here is a real CPU/DMA write or an overflow fallback —
     * never an exact writer's page aliasing — so hashing is always correct, and
     * the v1 page-hit SKIP that produced the false negative is simply gone. */
    if (check_texture_dirty(d, addr, size)) {
        pgraph_vk_mark_textures_possibly_dirty(d, addr, size, false);
        return true;
    }

    /* Channel S: our bytes intersect a recorded exact writer's span. Byte-exact
     * fan-out to the OTHER textures those spans also touched. Channel S is a
     * read-only intersection (it never consumes a shared bit), so — unlike
     * Channel P — there is no test-and-clear hazard: every byte-intersecting
     * texture also re-checks itself here on its own bind, making this fan-out
     * belt-and-suspenders rather than load-bearing. */
    if (range_intersects_exact_spans(r, addr, size)) {
        pgraph_vk_mark_textures_possibly_dirty(d, addr, size, true);
        return true;
    }

    /* Clean in BOTH channels => genuinely clean => skip the hash. Count it as an
     * avoided hash ONLY when this range shares a page with an exact writer — i.e.
     * when STOCK would have found the page dirty (from that writer) and hashed.
     * A range with no exact writer near its pages was clean under stock too, so
     * counting it would inflate the metric and break the "SKIPPED absorbs the
     * TEX_HASH/TEX_HASH_KB drop" relationship. Measurement only — it never
     * affects the skip above (that stays purely byte-exact + page-bitmap). */
    if (range_pages_overlap_exact_spans(r, addr, size)) {
        nv2a_profile_inc_counter(NV2A_PROF_TEX_HASH_SKIPPED);
    }
    return false;
}

// Check if any of the pages spanned by the a texture are dirty.
static bool check_texture_possibly_dirty(NV2AState *d,
                                         hwaddr texture_vram_offset,
                                         unsigned int length,
                                         hwaddr palette_vram_offset,
                                         unsigned int palette_length)
{
    bool possibly_dirty = false;
    if (check_texture_range_dirty(d, texture_vram_offset, length)) {
        possibly_dirty = true;
    }
    if (palette_length &&
        check_texture_range_dirty(d, palette_vram_offset, palette_length)) {
        possibly_dirty = true;
    }
    return possibly_dirty;
}

// FIXME: Make sure we update sampler when data matches. Should we add filtering
// options to the textureshape?
/* in_place: true when overwriting an EXISTING image (cache-hit content
 * re-upload), false when filling a freshly created one (cache miss — no
 * recorded draw can reference the new image, and the LRU-recycled node's
 * submit_time is stale garbage from its previous occupant). */
static void upload_texture_image(PGRAPHState *pg, int texture_idx,
                                 TextureBinding *binding, bool in_place)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    TextureShape *state = &binding->key.state;
    VkColorFormatInfo vkf = kelvin_color_format_vk_map[state->color_format];

    nv2a_profile_inc_counter(NV2A_PROF_TEX_UPLOAD);

    /* S5 in-batch uploads (docs/vr/overlap-resource-audit.md §0/§3): when
     * ON, record the staging copy + layout transitions into the OPEN frame
     * command buffer (the nondraw path copy_zeta_surface_to_texture already
     * uses) instead of the blocking single-time submit+wait whose fence
     * transitively drains every in-flight submission.
     *
     * Ordering-semantics guard: the single-time path executes BEFORE the
     * open batch in queue order, so draws already recorded in the open
     * batch that sampled THIS image observe the NEW texels (the shipped
     * upstream approximation). An in-stream copy executes AFTER those
     * draws, flipping them to the OLD texels. For an in-place re-upload of
     * an image bound during the current recording window
     * (binding->submit_time == r->submit_count, stamped at every bind by
     * update_timestamps), keep the single-time submit+wait so pixels match
     * the OFF path; its staging bytes come from a transient ring region so
     * they cannot stomp pending in-batch regions. Fresh images
     * (!in_place) have no such draws by construction and always ride
     * in-batch. */
    bool inbatch = false;
    bool guard = false;
    if (r->inbatch_uploads) {
        guard = in_place && r->in_command_buffer &&
                binding->submit_time == r->submit_count;
        inbatch = !guard;
    }

    if (!inbatch) {
        /* narrow-fence WAR gate: this overwrites the texture image in
         * place. If the submission that last SAMPLED it
         * (binding->submit_time, stamped at bind) is still in flight, wait
         * for exactly that one to retire. Recording-but-unsubmitted use is
         * safe: the upload's copy is submitted (and fenced) before the
         * open batch, so queue order protects it. On the in-batch path
         * this host-side wait is unnecessary: the copy rides the frame's
         * own command stream and the queue-scoped
         * SHADER_READ->TRANSFER_DST transition barrier below orders it
         * after every earlier submission's sampling reads. */
        pgraph_vk_wait_for_submission(r, binding->submit_time);
    }

    g_autofree TextureLayout *layout = get_texture_layout(pg, texture_idx);
    const int num_layers = state->cubemap ? 6 : 1;

    // Calculate decoded texture data size
    size_t texture_data_size = 0;
    for (int layer_idx = 0; layer_idx < num_layers; layer_idx++) {
        TextureLayer *layer = &layout->layers[layer_idx];
        for (int level_idx = 0; level_idx < state->levels; level_idx++) {
            size_t size = layer->levels[level_idx].decoded_size;
            assert(size);
            texture_data_size += size;
        }
    }

    /* S0 overlap counter (single line, coordinated with the upload-path
     * owner): decoded bytes staged per frame (KB) — sizes the D5 in-batch
     * upload staging ring (docs/vr/present-wall-plan.md S0). */
    g_nv2a_stats.frame_working.counters[NV2A_PROF_TEX_UPLOAD_KB] +=
        (int)((texture_data_size + 1023) / 1024);

    assert(texture_data_size <=
           r->storage_buffers[BUFFER_STAGING_SRC].buffer_size);

    /* S5: carve the staging bytes out of the ring when ON. In-batch copies
     * are consumed by the open command buffer (open it FIRST so
     * r->submit_count is final — begin may skip the SURFACE_NO_WRITE
     * sentinel); the guard's single-time copy fence-waits before returning
     * (SURFACE_NO_WRITE = reclaimable immediately). Ring full => blocking
     * fallback: a full finish retires every consumer, the ring resets
     * empty, and the historical single-time path below runs at offset 0 —
     * correctness over stall-freedom, self-counted. */
    VkDeviceSize staging_base = 0;
    if (r->inbatch_uploads) {
        bool ring_ok;
        if (inbatch) {
            pgraph_vk_begin_nondraw_commands(pg);
            ring_ok = pgraph_vk_staging_ring_alloc(
                pg, texture_data_size, &staging_base, r->submit_count);
        } else {
            ring_ok = pgraph_vk_staging_ring_alloc(
                pg, texture_data_size, &staging_base, SURFACE_NO_WRITE);
        }
        if (!ring_ok) {
            nv2a_profile_inc_counter(NV2A_PROF_UPLOAD_STAGING_WAIT);
            pgraph_vk_finish(pg, VK_FINISH_REASON_NEED_BUFFER_SPACE);
            pgraph_vk_staging_ring_reset(r);
            inbatch = false;
            staging_base = 0;
        }
    }

    // Copy texture data to mapped device buffer
    uint8_t *mapped_memory_ptr;

    VK_CHECK(vmaMapMemory(r->allocator,
                          r->storage_buffers[BUFFER_STAGING_SRC].allocation,
                          (void *)&mapped_memory_ptr));

    int num_regions = num_layers * state->levels;
    g_autofree VkBufferImageCopy *regions =
        g_malloc0_n(num_regions, sizeof(VkBufferImageCopy));

    VkBufferImageCopy *region = regions;
    VkDeviceSize buffer_offset = 0;

    for (int layer_idx = 0; layer_idx < num_layers; layer_idx++) {
        TextureLayer *layer = &layout->layers[layer_idx];
        NV2A_VK_DPRINTF("Layer %d", layer_idx);
        for (int level_idx = 0; level_idx < state->levels; level_idx++) {
            TextureLevel *level = &layer->levels[level_idx];
            NV2A_VK_DPRINTF(" - Level %d, w=%d h=%d d=%d @ %08" HWADDR_PRIx,
                            level_idx, level->width, level->height,
                            level->depth, buffer_offset);
            memcpy(mapped_memory_ptr + staging_base + buffer_offset,
                   level->decoded_data, level->decoded_size);
            *region = (VkBufferImageCopy){
                .bufferOffset = staging_base + buffer_offset,
                .bufferRowLength = 0, // Tightly packed
                .bufferImageHeight = 0,
                .imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .imageSubresource.mipLevel = level_idx,
                .imageSubresource.baseArrayLayer = layer_idx,
                .imageSubresource.layerCount = 1,
                .imageOffset = (VkOffset3D){ 0, 0, 0 },
                .imageExtent =
                    (VkExtent3D){ level->width, level->height, level->depth },
            };
            buffer_offset += level->decoded_size;
            region++;
        }
    }
    assert(staging_base + buffer_offset <=
           r->storage_buffers[BUFFER_STAGING_SRC].buffer_size);

    /* Region-scoped flush when the ring handed out a region (re-flushing a
     * pending in-flight region would be benign but pointless); the OFF
     * path keeps the historical whole-buffer flush. */
    vmaFlushAllocation(r->allocator,
                       r->storage_buffers[BUFFER_STAGING_SRC].allocation,
                       staging_base,
                       r->inbatch_uploads ? texture_data_size :
                                            VK_WHOLE_SIZE);

    vmaUnmapMemory(r->allocator,
                   r->storage_buffers[BUFFER_STAGING_SRC].allocation);

    /* S5: in-batch copies ride the open frame command buffer; the guard,
     * the ring-full fallback and the OFF path keep the single-time CB. */
    VkCommandBuffer cmd;
    if (inbatch) {
        nv2a_profile_inc_counter(NV2A_PROF_UPLOAD_INBATCH);
        cmd = pgraph_vk_begin_nondraw_commands(pg);
    } else {
        cmd = pgraph_vk_begin_single_time_commands(pg);
    }
    pgraph_vk_begin_debug_marker(r, cmd, RGBA_GREEN, __func__);

    VkBufferMemoryBarrier host_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = r->storage_buffers[BUFFER_STAGING_SRC].buffer,
        .size = VK_WHOLE_SIZE
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                         &host_barrier, 0, NULL);

    pgraph_vk_transition_image_layout(pg, cmd, binding->image, vkf.vk_format,
                                      binding->current_layout,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    binding->current_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    vkCmdCopyBufferToImage(cmd, r->storage_buffers[BUFFER_STAGING_SRC].buffer,
                           binding->image, binding->current_layout,
                           num_regions, regions);

    pgraph_vk_transition_image_layout(pg, cmd, binding->image, vkf.vk_format,
                                      binding->current_layout,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    binding->current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    if (!inbatch) {
        nv2a_profile_inc_counter(NV2A_PROF_QUEUE_SUBMIT_4);
    }
    pgraph_vk_end_debug_marker(r, cmd);
    if (inbatch) {
        /* No submit, no wait: the copy executes with the frame's own
         * submission; eviction/overwrite gates cover it through
         * binding->submit_time (stamped to r->submit_count at this bind by
         * update_timestamps). */
        pgraph_vk_end_nondraw_commands(pg, cmd);
    } else {
        pgraph_vk_end_single_time_commands(pg, cmd);
    }

    // Release decoded texture data
    for (int layer_idx = 0; layer_idx < num_layers; layer_idx++) {
        TextureLayer *layer = &layout->layers[layer_idx];
        for (int level_idx = 0; level_idx < state->levels; level_idx++) {
            g_free(layer->levels[level_idx].decoded_data);
        }
    }
}

static void copy_zeta_surface_to_texture(PGRAPHState *pg, SurfaceBinding *surface,
                                         TextureBinding *texture)
{
    assert(!surface->color);

    PGRAPHVkState *r = pg->vk_renderer_state;
    TextureShape *state = &texture->key.state;
    VkColorFormatInfo vkf = kelvin_color_format_vk_map[state->color_format];

    bool use_compute_to_convert_depth_stencil =
        surface->host_fmt.vk_format == VK_FORMAT_D24_UNORM_S8_UINT ||
        surface->host_fmt.vk_format == VK_FORMAT_D32_SFLOAT_S8_UINT;

    bool compute_needs_finish = use_compute_to_convert_depth_stencil &&
                                pgraph_vk_compute_needs_finish(r);
    if (compute_needs_finish) {
        pgraph_vk_finish(pg, VK_FINISH_REASON_NEED_BUFFER_SPACE);
    }

    nv2a_profile_inc_counter(NV2A_PROF_SURF_TO_TEX);

    trace_nv2a_pgraph_surface_render_to_texture(
        surface->vram_addr, surface->width, surface->height);

    VkCommandBuffer cmd = pgraph_vk_begin_nondraw_commands(pg);
    pgraph_vk_begin_debug_marker(r, cmd, RGBA_GREEN, __func__);

    unsigned int scaled_width = surface->width,
                 scaled_height = surface->height;
    pgraph_apply_scaling_factor(pg, &scaled_width, &scaled_height);

    size_t copied_image_size =
        scaled_width * scaled_height * surface->host_fmt.host_bytes_per_pixel;
    size_t stencil_buffer_offset = 0;
    size_t stencil_buffer_size = 0;

    int num_regions = 0;
    VkBufferImageCopy regions[2];
    regions[num_regions++] = (VkBufferImageCopy){
        .bufferOffset = 0,
        .bufferRowLength = 0, // Tightly packed
        .bufferImageHeight = 0, // Tightly packed
        .imageSubresource.aspectMask = surface->color ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT,
        .imageSubresource.mipLevel = 0,
        .imageSubresource.baseArrayLayer = 0,
        .imageSubresource.layerCount = 1,
        .imageOffset = (VkOffset3D){0, 0, 0},
        .imageExtent = (VkExtent3D){scaled_width, scaled_height, 1},
    };

    if (surface->host_fmt.aspect & VK_IMAGE_ASPECT_STENCIL_BIT) {
        stencil_buffer_offset =
            ROUND_UP(scaled_width * scaled_height * 4,
                     r->device_props.limits.minStorageBufferOffsetAlignment);
        stencil_buffer_size = scaled_width * scaled_height;
        copied_image_size += stencil_buffer_size;

        regions[num_regions++] = (VkBufferImageCopy){
            .bufferOffset = stencil_buffer_offset,
            .bufferRowLength = 0, // Tightly packed
            .bufferImageHeight = 0, // Tightly packed
            .imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT,
            .imageSubresource.mipLevel = 0,
            .imageSubresource.baseArrayLayer = 0,
            .imageSubresource.layerCount = 1,
            .imageOffset = (VkOffset3D){0, 0, 0},
            .imageExtent = (VkExtent3D){scaled_width, scaled_height, 1},
        };
    }
    StorageBuffer *dst_storage_buffer = &r->storage_buffers[BUFFER_COMPUTE_DST];
    assert(dst_storage_buffer->buffer_size >= copied_image_size);

    pgraph_vk_transition_image_layout(
        pg, cmd, surface->image, surface->host_fmt.vk_format,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    vkCmdCopyImageToBuffer(
        cmd, surface->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        dst_storage_buffer->buffer,
        num_regions, regions);

    pgraph_vk_transition_image_layout(
        pg, cmd, surface->image, surface->host_fmt.vk_format,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

    VkBuffer texture_source_buffer;

    if (use_compute_to_convert_depth_stencil) {
        size_t packed_image_size = scaled_width * scaled_height * 4;

        VkBufferMemoryBarrier pre_pack_src_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = r->storage_buffers[BUFFER_COMPUTE_DST].buffer,
            .size = VK_WHOLE_SIZE
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL,
                             1, &pre_pack_src_barrier, 0, NULL);

        VkBufferMemoryBarrier pre_pack_dst_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = r->storage_buffers[BUFFER_COMPUTE_SRC].buffer,
            .size = VK_WHOLE_SIZE
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL,
                             1, &pre_pack_dst_barrier, 0, NULL);

        pgraph_vk_pack_depth_stencil(
            pg, surface, cmd,
            r->storage_buffers[BUFFER_COMPUTE_DST].buffer,
            r->storage_buffers[BUFFER_COMPUTE_SRC].buffer, false);

        VkBufferMemoryBarrier post_pack_src_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = r->storage_buffers[BUFFER_COMPUTE_DST].buffer,
            .size = VK_WHOLE_SIZE
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                             &post_pack_src_barrier, 0, NULL);

        VkBufferMemoryBarrier post_pack_dst_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = r->storage_buffers[BUFFER_COMPUTE_SRC].buffer,
            .size = packed_image_size
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                             &post_pack_dst_barrier, 0, NULL);

        texture_source_buffer = r->storage_buffers[BUFFER_COMPUTE_SRC].buffer;
    } else {
        VkBufferMemoryBarrier barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = dst_storage_buffer->buffer,
            .size = VK_WHOLE_SIZE
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL,
                             1, &barrier, 0, NULL);

        texture_source_buffer = dst_storage_buffer->buffer;
    }

    pgraph_vk_transition_image_layout(pg, cmd, texture->image, vkf.vk_format,
                                      texture->current_layout,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    texture->current_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    regions[0] = (VkBufferImageCopy){
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .imageSubresource.mipLevel = 0,
        .imageSubresource.baseArrayLayer = 0,
        .imageSubresource.layerCount = 1,
        .imageOffset = (VkOffset3D){ 0, 0, 0 },
        .imageExtent = (VkExtent3D){ scaled_width, scaled_height, 1 },
    };
    vkCmdCopyBufferToImage(
        cmd, texture_source_buffer, texture->image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, regions);

    VkBufferMemoryBarrier post_copy_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = texture_source_buffer,
        .size = VK_WHOLE_SIZE
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                         &post_copy_barrier, 0, NULL);

    pgraph_vk_transition_image_layout(pg, cmd, texture->image, vkf.vk_format,
                                      texture->current_layout,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    texture->current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    pgraph_vk_end_debug_marker(r, cmd);
    pgraph_vk_end_nondraw_commands(pg, cmd);

    texture->draw_time = surface->draw_time;
}

// FIXME: Should be able to skip the copy and sample the original surface image
static void copy_surface_to_texture(PGRAPHState *pg, SurfaceBinding *surface,
                                    TextureBinding *texture)
{
    /* narrow-fence: the GPU READS this surface image here; eviction and
     * overwrite must wait for the submission carrying that read.
     * Conservative when the copy is fenced-synchronous, exact when it
     * records into the open batch. */
    surface->last_use_submit = pg->vk_renderer_state->submit_count;

    if (!surface->color) {
        copy_zeta_surface_to_texture(pg, surface, texture);
        return;
    }

    PGRAPHVkState *r = pg->vk_renderer_state;
    TextureShape *state = &texture->key.state;
    VkColorFormatInfo vkf = kelvin_color_format_vk_map[state->color_format];

    nv2a_profile_inc_counter(NV2A_PROF_SURF_TO_TEX);

    trace_nv2a_pgraph_surface_render_to_texture(
        surface->vram_addr, surface->width, surface->height);

    VkCommandBuffer cmd = pgraph_vk_begin_nondraw_commands(pg);
    pgraph_vk_begin_debug_marker(r, cmd, RGBA_GREEN, __func__);

    pgraph_vk_transition_image_layout(
        pg, cmd, surface->image, surface->host_fmt.vk_format,
        surface->color ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL :
                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    pgraph_vk_transition_image_layout(pg, cmd, texture->image, vkf.vk_format,
                                      texture->current_layout,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    texture->current_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    VkImageCopy region = {
        .srcSubresource.aspectMask = surface->host_fmt.aspect,
        .srcSubresource.layerCount = 1,
        .dstSubresource.aspectMask = surface->host_fmt.aspect,
        .dstSubresource.layerCount = 1,
        .extent.width = surface->width,
        .extent.height = surface->height,
        .extent.depth = 1,
    };
    pgraph_apply_scaling_factor(pg, &region.extent.width,
                                &region.extent.height);
    vkCmdCopyImage(cmd, surface->image,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, texture->image,
                   texture->current_layout, 1, &region);

    pgraph_vk_transition_image_layout(
        pg, cmd, surface->image, surface->host_fmt.vk_format,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        surface->color ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL :
                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

    pgraph_vk_transition_image_layout(pg, cmd, texture->image, vkf.vk_format,
                                      texture->current_layout,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    texture->current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    pgraph_vk_end_debug_marker(r, cmd);
    pgraph_vk_end_nondraw_commands(pg, cmd);

    texture->draw_time = surface->draw_time;
}

static unsigned int vk_format_texel_size(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_R8_UNORM:                return 1;
    case VK_FORMAT_R8G8_UNORM:              return 2;
    case VK_FORMAT_A1R5G5B5_UNORM_PACK16:   return 2;
    case VK_FORMAT_R5G6B5_UNORM_PACK16:     return 2;
    case VK_FORMAT_A4R4G4B4_UNORM_PACK16:   return 2;
    case VK_FORMAT_R16_UNORM:               return 2;
    case VK_FORMAT_R8G8B8_SNORM:            return 3;
    case VK_FORMAT_B8G8R8A8_UNORM:          return 4;
    case VK_FORMAT_R8G8B8A8_UNORM:          return 4;
    case VK_FORMAT_R32_UINT:                return 4;
    default:                                return 0;
    }
}

/* loc-graphics-research: reason-coded twin of the historical bool check, so
 * the bind site can attribute WHY a surface-as-texture bind missed the GPU
 * copy path (the S2T_MISS_* counters). Leg order mirrors the original
 * expression exactly; the bool wrapper below is behavior-identical to the
 * historical check_surface_to_texture_compatiblity. */
typedef enum S2TCompat {
    S2T_COMPAT_OK = 0,
    S2T_COMPAT_PITCH,   /* linear surface, pitch != shape->pitch */
    S2T_COMPAT_DIMS,    /* width/height mismatch */
    S2T_COMPAT_CUBEMAP, /* texture shape is a cubemap */
    S2T_COMPAT_MIPS,    /* texture shape has levels > 1 */
    S2T_COMPAT_FORMAT,  /* unsupported vk format or texel-size mismatch */
} S2TCompat;

static S2TCompat surface_to_texture_compat_reason(const SurfaceBinding *surface,
                                                  const TextureShape *shape)
{
    if (!surface->swizzle && surface->pitch != shape->pitch) {
        return S2T_COMPAT_PITCH;
    }
    if (surface->width != shape->width || surface->height != shape->height) {
        return S2T_COMPAT_DIMS;
    }
    if (shape->cubemap) {
        return S2T_COMPAT_CUBEMAP;
    }
    if (shape->levels > 1) {
        return S2T_COMPAT_MIPS;
    }

    if (!surface->color) {
        return S2T_COMPAT_OK;
    }

    VkColorFormatInfo tex_vkf = kelvin_color_format_vk_map[shape->color_format];
    if (!(tex_vkf.vk_format && surface->host_fmt.host_bytes_per_pixel ==
                                   vk_format_texel_size(tex_vkf.vk_format))) {
        return S2T_COMPAT_FORMAT;
    }
    return S2T_COMPAT_OK;
}

static bool check_surface_to_texture_compatiblity(const SurfaceBinding *surface,
                                                  const TextureShape *shape)
{
    return surface_to_texture_compat_reason(surface, shape) == S2T_COMPAT_OK;
}

/* loc-graphics-research: XEMU_SURF2TEX_EXT getenv-once gate. -1 = unprobed,
 * 0 = off, 1 = on. XEMU_NO_SURF2TEX_EXT is the kill-switch and wins over
 * XEMU_SURF2TEX_EXT. Default (neither set) is OFF, silently — the OFF path
 * is byte-identical to the historical bind flow. */
static bool surf2tex_ext_enabled(void)
{
    static int state = -1;
    if (state < 0) {
        if (getenv("XEMU_NO_SURF2TEX_EXT")) {
            state = 0;
            if (getenv("XEMU_SURF2TEX_EXT")) {
                fprintf(stderr,
                        "nv2a: XEMU_NO_SURF2TEX_EXT overrides XEMU_SURF2TEX_EXT "
                        "— extended surface-as-texture gather off\n");
            }
        } else {
            state = getenv("XEMU_SURF2TEX_EXT") ? 1 : 0;
            if (state == 1) {
                fprintf(stderr,
                        "nv2a: XEMU_SURF2TEX_EXT on — multi-surface gather GPU "
                        "copy for surface-as-texture binds (replaces the "
                        "download+rehash fallback where row-compatible)\n");
            }
        }
    }
    return state == 1;
}

/* loc-graphics-research: gather plan for the extended surface-as-texture path.
 *
 * The historical GPU copy path requires ONE resident surface whose base and
 * shape exactly match the texture. Line of Contact's combat scene issues
 * hundreds of surface-as-texture binds per frame that miss that test (surface
 * evicted / base offset inside a larger framebuffer / tiled sub-surfaces /
 * width-height mismatch) and each miss takes the fallback: a blocking GPU->CPU
 * download of every dirty overlapping surface plus a full re-hash and CPU
 * re-upload of the texture bytes (~600 downloads and ~177MB hashed per frame,
 * ~750ms worst frames — the 4fps wall).
 *
 * The gather plan replaces that with row-aligned VkImageCopy regions taken
 * directly from every overlapping resident color surface, entirely GPU-side:
 * the texture is linear-addressed, so with equal row pitch and row-aligned
 * base deltas the texel rows of the texture are exactly the texel rows of the
 * surfaces that cover its VRAM range. Anything not row-representable (swizzled
 * surface, pitch mismatch, non-row-aligned overlap, zeta) bails to the
 * historical fallback untouched. */
#define SURF2TEX_GATHER_MAX_REGIONS 16

typedef struct Surf2TexGatherPlan {
    int num_regions;
    bool full_coverage;      /* gathered rows cover every texture row */
    /* Dedup key: sum of gathered draw_times (mixed with region count).
     * draw_time is per-surface monotonic, so the sum strictly changes when
     * ANY gathered surface is redrawn — a max() key would miss a redraw of a
     * lower-draw_time region while another region still holds the max. */
    uint32_t draw_time_key;
    struct {
        SurfaceBinding *surface;
        unsigned int src_y;  /* first row inside the surface image (unscaled) */
        unsigned int dst_y;  /* first row inside the texture image (unscaled) */
        unsigned int rows;
        unsigned int width;
    } regions[SURF2TEX_GATHER_MAX_REGIONS];
} Surf2TexGatherPlan;

/* Why a gather plan could not be built — ticked (storm-path only) into the
 * S2T_REJ_* counters so the next extension is designed from data, not guesses. */
typedef enum S2TGatherResult {
    S2T_GATHER_OK = 0,
    S2T_GATHER_REJ_TEXSHAPE,      /* texture shape outside row math (swizzled/
                                     mips/cubemap/3D/no pitch/unsupported fmt) */
    S2T_GATHER_REJ_SWIZSURF,      /* an overlapping surface is swizzled */
    S2T_GATHER_REJ_PITCH,         /* overlapping surface pitch != texture pitch */
    S2T_GATHER_REJ_BPP,           /* texel size mismatch */
    S2T_GATHER_REJ_ALIGN,         /* overlap not row-aligned */
    S2T_GATHER_REJ_OVERFLOW,      /* more overlapping surfaces than regions */
    S2T_GATHER_REJ_NONE_RESIDENT, /* nothing resident overlaps the range */
} S2TGatherResult;

static S2TGatherResult build_surf2tex_gather_plan(NV2AState *d, hwaddr tex_base,
                                                  const TextureShape *shape,
                                                  Surf2TexGatherPlan *plan)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;
    BasicColorFormatInfo f_basic =
        kelvin_color_format_info_map[shape->color_format];

    /* Row math only holds for plain 2D single-level linear texture shapes. */
    if (shape->levels != 1 || shape->cubemap || shape->dimensionality != 2 ||
        !f_basic.linear || !shape->pitch) {
        return S2T_GATHER_REJ_TEXSHAPE;
    }

    VkColorFormatInfo tex_vkf = kelvin_color_format_vk_map[shape->color_format];
    unsigned int bpp = vk_format_texel_size(tex_vkf.vk_format);
    if (!tex_vkf.vk_format || !bpp) {
        return S2T_GATHER_REJ_TEXSHAPE;
    }

    unsigned int tex_rows = shape->height;
    hwaddr tex_end = tex_base + (hwaddr)shape->pitch * tex_rows;

    plan->num_regions = 0;
    plan->draw_time_key = 0;
    unsigned int covered_rows = 0;

    SurfaceBinding *s;
    QTAILQ_FOREACH(s, &r->surfaces, entry) {
        hwaddr s_end = s->vram_addr + s->size;
        if (s_end <= tex_base || s->vram_addr >= tex_end) {
            continue; /* no overlap */
        }
        /* Every overlapping surface must be row-representable, else the
         * fallback must handle the whole bind (it downloads ALL dirty
         * overlaps — a partial gather would leave those rows stale). */
        if (!s->color || s->swizzle) {
            return S2T_GATHER_REJ_SWIZSURF;
        }
        if (s->pitch != shape->pitch) {
            return S2T_GATHER_REJ_PITCH;
        }
        if (s->host_fmt.host_bytes_per_pixel != bpp) {
            return S2T_GATHER_REJ_BPP;
        }
        hwaddr ovl_start = MAX(s->vram_addr, tex_base);
        hwaddr ovl_end = MIN(s_end, tex_end);
        if ((ovl_start - s->vram_addr) % s->pitch ||
            (ovl_start - tex_base) % s->pitch) {
            return S2T_GATHER_REJ_ALIGN; /* overlap not row-aligned */
        }
        unsigned int src_y = (ovl_start - s->vram_addr) / s->pitch;
        unsigned int dst_y = (ovl_start - tex_base) / s->pitch;
        unsigned int rows = (ovl_end - ovl_start) / s->pitch;
        if (src_y < s->height && dst_y < tex_rows) {
            rows = MIN(rows, s->height - src_y);
            rows = MIN(rows, tex_rows - dst_y);
        } else {
            rows = 0;
        }
        if (!rows) {
            continue;
        }
        if (plan->num_regions >= SURF2TEX_GATHER_MAX_REGIONS) {
            return S2T_GATHER_REJ_OVERFLOW;
        }
        plan->regions[plan->num_regions].surface = s;
        plan->regions[plan->num_regions].src_y = src_y;
        plan->regions[plan->num_regions].dst_y = dst_y;
        plan->regions[plan->num_regions].rows = rows;
        plan->regions[plan->num_regions].width = MIN(s->width, shape->width);
        plan->num_regions++;
        covered_rows += rows;
        plan->draw_time_key += (uint32_t)s->draw_time;
    }

    if (plan->num_regions == 0) {
        return S2T_GATHER_REJ_NONE_RESIDENT;
    }
    /* Mix the region count in so a same-sum different-set plan re-keys. */
    plan->draw_time_key = plan->draw_time_key * 31u +
                          (uint32_t)plan->num_regions;
    /* Resident surfaces never overlap each other (creation invalidates
     * overlaps), so summed rows can't double-count. */
    plan->full_coverage = covered_rows >= tex_rows;
    return S2T_GATHER_OK;
}

/* loc-graphics-research: XEMU_SURF2TEX_DEBUG=1 — rate-limited geometry dump of
 * surface-fed binds that still fall back after the gather attempt, so the next
 * extension is designed against real shapes. Max ~20 lines / 5 s. */
static void surf2tex_debug_dump_miss(NV2AState *d, hwaddr tex_base,
                                     const TextureShape *shape,
                                     SurfaceBinding *exact)
{
    static int gate = -1;
    if (gate < 0) {
        gate = getenv("XEMU_SURF2TEX_DEBUG") ? 1 : 0;
    }
    if (!gate) {
        return;
    }
    static int64_t win_start;
    static int win_count;
    int64_t now = g_get_monotonic_time();
    if (now - win_start > (int64_t)5 * G_USEC_PER_SEC) {
        win_start = now;
        win_count = 0;
    }
    if (win_count++ >= 20) {
        return;
    }
    BasicColorFormatInfo fb = kelvin_color_format_info_map[shape->color_format];
    fprintf(stderr,
            "s2t-miss: tex=%08" HWADDR_PRIx " %ux%u pitch=%u fmt=%02x lin=%d "
            "lvls=%d",
            tex_base, shape->width, shape->height, shape->pitch,
            shape->color_format, fb.linear, shape->levels);
    SurfaceBinding *s = exact ? exact
                              : pgraph_vk_surface_get_within(
                                    d, tex_base);
    if (s) {
        fprintf(stderr,
                " | surf=%08" HWADDR_PRIx " %ux%u pitch=%u swz=%d color=%d "
                "bpp=%u\n",
                s->vram_addr, s->width, s->height, s->pitch, s->swizzle,
                s->color, s->host_fmt.host_bytes_per_pixel);
    } else {
        fprintf(stderr, " | surf=none-within\n");
    }
}

static void copy_gather_plan_to_texture(PGRAPHState *pg,
                                        Surf2TexGatherPlan *plan,
                                        TextureBinding *texture, bool fresh)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    TextureShape *state = &texture->key.state;
    VkColorFormatInfo vkf = kelvin_color_format_vk_map[state->color_format];
    unsigned int scale = pg->surface_scale_factor;

    nv2a_profile_inc_counter(NV2A_PROF_S2T_EXT_COPY);
    g_nv2a_stats.frame_working.counters[NV2A_PROF_S2T_EXT_REGIONS] +=
        plan->num_regions;
    if (!plan->full_coverage) {
        nv2a_profile_inc_counter(NV2A_PROF_S2T_EXT_PARTIAL);
    }

    VkCommandBuffer cmd = pgraph_vk_begin_nondraw_commands(pg);
    pgraph_vk_begin_debug_marker(r, cmd, RGBA_GREEN, __func__);

    pgraph_vk_transition_image_layout(pg, cmd, texture->image, vkf.vk_format,
                                      texture->current_layout,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    texture->current_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    if (fresh && !plan->full_coverage) {
        /* Fresh image with rows no resident surface covers: define them.
         * (Real hardware would sample whatever bytes follow in VRAM; black is
         * deterministic and the uncovered rows are not expected to be
         * sampled.) On a reused binding the previous contents stay. */
        VkClearColorValue black = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } };
        VkImageSubresourceRange range = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
        };
        vkCmdClearColorImage(cmd, texture->image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1,
                             &range);
    }

    for (int i = 0; i < plan->num_regions; i++) {
        SurfaceBinding *s = plan->regions[i].surface;

        /* narrow-fence: the GPU reads this surface image here; eviction and
         * overwrite must wait for the submission carrying that read (same
         * bookkeeping as the exact-match copy path). */
        s->last_use_submit = r->submit_count;

        pgraph_vk_transition_image_layout(
            pg, cmd, s->image, s->host_fmt.vk_format,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

        VkImageCopy region = {
            .srcSubresource.aspectMask = s->host_fmt.aspect,
            .srcSubresource.layerCount = 1,
            .dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .dstSubresource.layerCount = 1,
            .srcOffset.y = plan->regions[i].src_y * scale,
            .dstOffset.y = plan->regions[i].dst_y * scale,
            .extent.width = plan->regions[i].width,
            .extent.height = plan->regions[i].rows,
            .extent.depth = 1,
        };
        pgraph_apply_scaling_factor(pg, &region.extent.width,
                                    &region.extent.height);

        vkCmdCopyImage(cmd, s->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       texture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                       &region);

        pgraph_vk_transition_image_layout(
            pg, cmd, s->image, s->host_fmt.vk_format,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }

    pgraph_vk_transition_image_layout(pg, cmd, texture->image, vkf.vk_format,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    texture->current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    pgraph_vk_end_debug_marker(r, cmd);
    pgraph_vk_end_nondraw_commands(pg, cmd);

    /* Bit-pattern equality is all the dedup needs; the key is not a real
     * draw_time but reuses the field so the exact-match path's semantics
     * (fresh binding never matches) hold unchanged. */
    texture->draw_time = (int)plan->draw_time_key;
}

/* loc-graphics-research helpers for the two same-base swizzled-texture paths
 * (XEMU_SURF2TEX_EXT). Line of Contact's shadow pipeline drives both:
 *
 *  QUADRANT — a square pow2 swizzled texture over a LARGER square pow2
 *  swizzled surface at the same base. Morton order nests: the first w*h texels
 *  of the larger Morton square are exactly its top-left w x h block in the
 *  smaller square's Morton order, and both images are stored deswizzled — so
 *  the texture content is precisely the surface's top-left w x h rect.
 *
 *  BOUNCE — a square pow2 swizzled 4-bpp texture over a LINEAR surface at the
 *  same base (LoC: a 256x256 swizzled A8R8G8B8 texture sampling the 1800x1800
 *  linear D16 shadow map ~600x/frame — the 4fps storm). Replicates the CPU
 *  fallback byte-for-byte, GPU-side: surface image -> raw byte stream in
 *  BUFFER_COMPUTE_DST (vkCmdCopyImageToBuffer at the surface's row pitch,
 *  gap bytes zero-filled) -> Morton deswizzle compute into BUFFER_COMPUTE_SRC
 *  -> vkCmdCopyBufferToImage into the texture image. No CPU round-trip, no
 *  hash, no guest-RAM touch.
 */
static bool is_pow2_square(unsigned int w, unsigned int h)
{
    return w == h && w != 0 && (w & (w - 1)) == 0;
}

/* Any OTHER resident surface overlapping [base, base+len) would contribute
 * bytes the single-source paths cannot see — bail to the fallback there. */
static bool other_surface_overlaps_range(PGRAPHVkState *r,
                                         const SurfaceBinding *self,
                                         hwaddr base, size_t len)
{
    SurfaceBinding *s;
    QTAILQ_FOREACH(s, &r->surfaces, entry) {
        if (s == self) {
            continue;
        }
        if (s->vram_addr + s->size > base && s->vram_addr < base + len) {
            return true;
        }
    }
    return false;
}

static void copy_swizzled_quadrant_to_texture(PGRAPHState *pg,
                                              SurfaceBinding *surface,
                                              TextureBinding *texture)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    TextureShape *state = &texture->key.state;
    VkColorFormatInfo vkf = kelvin_color_format_vk_map[state->color_format];

    nv2a_profile_inc_counter(NV2A_PROF_S2T_EXT_COPY);
    nv2a_profile_inc_counter(NV2A_PROF_S2T_EXT_QUAD);

    surface->last_use_submit = r->submit_count;

    VkCommandBuffer cmd = pgraph_vk_begin_nondraw_commands(pg);
    pgraph_vk_begin_debug_marker(r, cmd, RGBA_GREEN, __func__);

    pgraph_vk_transition_image_layout(pg, cmd, surface->image,
                                      surface->host_fmt.vk_format,
                                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    pgraph_vk_transition_image_layout(pg, cmd, texture->image, vkf.vk_format,
                                      texture->current_layout,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    texture->current_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    VkImageCopy region = {
        .srcSubresource.aspectMask = surface->host_fmt.aspect,
        .srcSubresource.layerCount = 1,
        .dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .dstSubresource.layerCount = 1,
        .extent.width = state->width,
        .extent.height = state->height,
        .extent.depth = 1,
    };
    pgraph_apply_scaling_factor(pg, &region.extent.width,
                                &region.extent.height);
    vkCmdCopyImage(cmd, surface->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   texture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &region);

    pgraph_vk_transition_image_layout(pg, cmd, surface->image,
                                      surface->host_fmt.vk_format,
                                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    pgraph_vk_transition_image_layout(pg, cmd, texture->image, vkf.vk_format,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    texture->current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    pgraph_vk_end_debug_marker(r, cmd);
    pgraph_vk_end_nondraw_commands(pg, cmd);

    texture->draw_time = surface->draw_time;
}

static void bounce_linear_surface_to_swizzled_texture(PGRAPHState *pg,
                                                      SurfaceBinding *surface,
                                                      TextureBinding *texture,
                                                      hwaddr tex_base)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    TextureShape *state = &texture->key.state;
    VkColorFormatInfo vkf = kelvin_color_format_vk_map[state->color_format];

    unsigned int surf_bpp = surface->host_fmt.host_bytes_per_pixel;
    size_t tex_len = (size_t)state->width * state->height * 4;
    /* The texture may start inside the surface (LoC: swizzled sub-blocks at
     * word-aligned offsets in the shadow map). Copy whole surface rows
     * [first_row, end_row) into the buffer and bias the compute's source
     * index by the intra-row byte remainder (word-aligned, precondition). */
    hwaddr delta = tex_base - surface->vram_addr;
    unsigned int first_row = delta / surface->pitch;
    unsigned int rem_bytes = delta % surface->pitch;
    unsigned int end_row =
        DIV_ROUND_UP(delta + tex_len, (size_t)surface->pitch);
    unsigned int rows = end_row - first_row;
    size_t fill_len = (size_t)rows * surface->pitch;

    nv2a_profile_inc_counter(NV2A_PROF_S2T_EXT_COPY);
    nv2a_profile_inc_counter(NV2A_PROF_S2T_EXT_BOUNCE);

    surface->last_use_submit = r->submit_count;

    VkImageLayout rest_layout =
        surface->color ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL :
                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkBuffer in_buf = r->storage_buffers[BUFFER_COMPUTE_DST].buffer;
    VkBuffer out_buf = r->storage_buffers[BUFFER_COMPUTE_SRC].buffer;

    VkCommandBuffer cmd = pgraph_vk_begin_nondraw_commands(pg);
    pgraph_vk_begin_debug_marker(r, cmd, RGBA_GREEN, __func__);

    pgraph_vk_transition_image_layout(pg, cmd, surface->image,
                                      surface->host_fmt.vk_format, rest_layout,
                                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    /* Guard against the PREVIOUS bounce still using these buffers (compute
     * read of in_buf, transfer read of out_buf) before we overwrite them. */
    VkBufferMemoryBarrier reuse_barriers[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_READ_BIT |
                             VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = in_buf,
            .offset = 0,
            .size = VK_WHOLE_SIZE,
        },
        {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT |
                             VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = out_buf,
            .offset = 0,
            .size = VK_WHOLE_SIZE,
        },
    };
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_TRANSFER_BIT |
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT |
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, NULL, 2, reuse_barriers, 0, NULL);

    /* Zero the row-pitch gap bytes the image does not cover (surface width *
     * bpp < pitch). Real hardware would read whatever stale bytes sit there;
     * zero is deterministic and those texels land in unsampled padding. */
    vkCmdFillBuffer(cmd, in_buf, 0, fill_len, 0);
    VkBufferMemoryBarrier fill_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = in_buf,
        .offset = 0,
        .size = fill_len,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                         &fill_barrier, 0, NULL);

    VkBufferImageCopy to_buf = {
        .bufferOffset = 0,
        .bufferRowLength = surface->pitch / surf_bpp,
        .bufferImageHeight = 0,
        .imageSubresource.aspectMask = surface->host_fmt.aspect,
        .imageSubresource.layerCount = 1,
        .imageOffset.y = first_row,
        .imageExtent.width = surface->width,
        .imageExtent.height = MIN(rows, surface->height - first_row),
        .imageExtent.depth = 1,
    };
    vkCmdCopyImageToBuffer(cmd, surface->image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, in_buf, 1,
                           &to_buf);

    pgraph_vk_transition_image_layout(pg, cmd, surface->image,
                                      surface->host_fmt.vk_format,
                                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                      rest_layout);

    VkBufferMemoryBarrier pre_compute = fill_barrier;
    pre_compute.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    pre_compute.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 1,
                         &pre_compute, 0, NULL);

    pgraph_vk_dispatch_deswizzle_u32(pg, cmd, state->width, state->height,
                                     rem_bytes / 4);

    VkBufferMemoryBarrier post_compute = fill_barrier;
    post_compute.buffer = out_buf;
    post_compute.size = tex_len;
    post_compute.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    post_compute.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                         &post_compute, 0, NULL);

    pgraph_vk_transition_image_layout(pg, cmd, texture->image, vkf.vk_format,
                                      texture->current_layout,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    texture->current_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    VkBufferImageCopy to_img = {
        .bufferOffset = 0,
        .bufferRowLength = 0, /* tightly packed */
        .bufferImageHeight = 0,
        .imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .imageSubresource.layerCount = 1,
        .imageExtent.width = state->width,
        .imageExtent.height = state->height,
        .imageExtent.depth = 1,
    };
    vkCmdCopyBufferToImage(cmd, out_buf, texture->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &to_img);

    pgraph_vk_transition_image_layout(pg, cmd, texture->image, vkf.vk_format,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    texture->current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    pgraph_vk_end_debug_marker(r, cmd);
    pgraph_vk_end_nondraw_commands(pg, cmd);

    texture->draw_time = surface->draw_time;
}

static void create_dummy_texture(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkImageCreateInfo image_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .extent.width = 16,
        .extent.height = 16,
        .extent.depth = 1,
        .mipLevels = 1,
        .arrayLayers = 1,
        .format = VK_FORMAT_R8_UNORM,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .flags = 0,
    };

    VmaAllocationCreateInfo alloc_create_info = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    };

    VkImage texture_image;
    VmaAllocation texture_allocation;

    VK_CHECK(vmaCreateImage(r->allocator, &image_create_info,
                            &alloc_create_info, &texture_image,
                            &texture_allocation, NULL));

    VkImageViewCreateInfo image_view_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = texture_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8_UNORM,
        .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .subresourceRange.baseMipLevel = 0,
        .subresourceRange.levelCount = image_create_info.mipLevels,
        .subresourceRange.baseArrayLayer = 0,
        .subresourceRange.layerCount = image_create_info.arrayLayers,
        .components = (VkComponentMapping){ VK_COMPONENT_SWIZZLE_R,
                                            VK_COMPONENT_SWIZZLE_R,
                                            VK_COMPONENT_SWIZZLE_R,
                                            VK_COMPONENT_SWIZZLE_R },
    };
    VkImageView texture_image_view;
    VK_CHECK(vkCreateImageView(r->device, &image_view_create_info, NULL,
                               &texture_image_view));

    VkSamplerCreateInfo sampler_create_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .anisotropyEnable = VK_FALSE,
        .borderColor = VK_BORDER_COLOR_INT_OPAQUE_WHITE,
        .unnormalizedCoordinates = VK_FALSE,
        .compareEnable = VK_FALSE,
        .compareOp = VK_COMPARE_OP_ALWAYS,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
    };

    VkSampler texture_sampler;
    VK_CHECK(vkCreateSampler(r->device, &sampler_create_info, NULL,
                             &texture_sampler));

    // Copy texture data to mapped device buffer
    uint8_t *mapped_memory_ptr;
    size_t texture_data_size =
        image_create_info.extent.width * image_create_info.extent.height;

    VK_CHECK(vmaMapMemory(r->allocator,
                          r->storage_buffers[BUFFER_STAGING_SRC].allocation,
                          (void *)&mapped_memory_ptr));
    memset(mapped_memory_ptr, 0xff, texture_data_size);

    vmaFlushAllocation(r->allocator,
                       r->storage_buffers[BUFFER_STAGING_SRC].allocation, 0,
                       VK_WHOLE_SIZE);

    vmaUnmapMemory(r->allocator,
                   r->storage_buffers[BUFFER_STAGING_SRC].allocation);

    VkCommandBuffer cmd = pgraph_vk_begin_single_time_commands(pg);
    pgraph_vk_begin_debug_marker(r, cmd, RGBA_GREEN, __func__);

    pgraph_vk_transition_image_layout(
        pg, cmd, texture_image, VK_FORMAT_R8_UNORM, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkBufferImageCopy region = {
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .imageSubresource.mipLevel = 0,
        .imageSubresource.baseArrayLayer = 0,
        .imageSubresource.layerCount = 1,
        .imageOffset = (VkOffset3D){ 0, 0, 0 },
        .imageExtent = (VkExtent3D){ image_create_info.extent.width,
                                     image_create_info.extent.height, 1 },
    };
    vkCmdCopyBufferToImage(cmd, r->storage_buffers[BUFFER_STAGING_SRC].buffer,
                           texture_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &region);

    pgraph_vk_transition_image_layout(pg, cmd, texture_image,
                                      VK_FORMAT_R8_UNORM,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    pgraph_vk_end_debug_marker(r, cmd);
    pgraph_vk_end_single_time_commands(pg, cmd);

    r->dummy_texture = (TextureBinding){
        .key.scale = 1.0,
        .image = texture_image,
        .current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .allocation = texture_allocation,
        .image_view = texture_image_view,
        .sampler = texture_sampler,
    };
}

static void destroy_dummy_texture(PGRAPHVkState *r)
{
    texture_cache_release_node_resources(r, &r->dummy_texture);
}

static void set_texture_label(PGRAPHState *pg, TextureBinding *texture)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    g_autofree gchar *label = g_strdup_printf(
        "Texture %" HWADDR_PRIx "h fmt:%02xh %dx%dx%d lvls:%d",
        texture->key.texture_vram_offset, texture->key.state.color_format,
        texture->key.state.width, texture->key.state.height,
        texture->key.state.depth, texture->key.state.levels);

    VkDebugUtilsObjectNameInfoEXT name_info = {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
        .objectType = VK_OBJECT_TYPE_IMAGE,
        .objectHandle = (uint64_t)texture->image,
        .pObjectName = label,
    };

    if (r->debug_utils_extension_enabled) {
        vkSetDebugUtilsObjectNameEXT(r->device, &name_info);
    }
    vmaSetAllocationName(r->allocator, texture->allocation, label);
}

static bool is_linear_filter_supported_for_format(PGRAPHVkState *r,
                                                  int kelvin_format)
{
    return r->texture_format_properties[kelvin_format].optimalTilingFeatures &
           VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
}

static void create_texture(PGRAPHState *pg, int texture_idx)
{
    NV2A_VK_DGROUP_BEGIN("Creating texture %d", texture_idx);

    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHVkState *r = pg->vk_renderer_state;
    TextureShape state = pgraph_get_texture_shape(pg, texture_idx); // FIXME: Check for pad issues
    BasicColorFormatInfo f_basic = kelvin_color_format_info_map[state.color_format];

    const hwaddr texture_vram_offset = pgraph_get_texture_phys_addr(pg, texture_idx);
    size_t texture_length = pgraph_get_texture_length(pg, &state);
    hwaddr texture_palette_vram_offset = 0;
    size_t texture_palette_data_size = 0;

    uint32_t filter =
        pgraph_reg_r(pg, NV_PGRAPH_TEXFILTER0 + texture_idx * 4);
    uint32_t address =
        pgraph_reg_r(pg, NV_PGRAPH_TEXADDRESS0 + texture_idx * 4);
    uint32_t border_color_pack32 =
        pgraph_reg_r(pg, NV_PGRAPH_BORDERCOLOR0 + texture_idx * 4);
    bool is_indexed = (state.color_format ==
            NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8);
    uint32_t max_anisotropy =
        1 << (GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_TEXCTL0_0 + texture_idx*4),
                       NV_PGRAPH_TEXCTL0_0_MAX_ANISOTROPY));

    TextureKey key;
    memset(&key, 0, sizeof(key));
    key.state = state;
    key.texture_vram_offset = texture_vram_offset;
    key.texture_length = texture_length;
    if (is_indexed) {
        texture_palette_vram_offset =
            pgraph_get_texture_palette_phys_addr_length(
                pg, texture_idx, &texture_palette_data_size);
        key.palette_vram_offset = texture_palette_vram_offset;
        key.palette_length = texture_palette_data_size;
    }
    key.scale = 1;

    // FIXME: Separate sampler from texture
    key.filter = filter;
    key.address = address;
    key.border_color = border_color_pack32;
    key.max_anisotropy = max_anisotropy;

    bool possibly_dirty = false;
    bool possibly_dirty_checked = false;
    bool surface_to_texture = false;

    // Check active surfaces to see if this texture was a render target
    Surf2TexGatherPlan s2t_plan = { .num_regions = 0 };
    bool s2t_plan_valid = false;
    /* loc-graphics-research: attribute WHY a surface-fed bind missed the GPU
     * copy path. Default reason NOSURF; refined below. Only ticked when the
     * fallback actually downloads (forced > 0), so plain texture binds that
     * overlap nothing dirty never count. */
    enum NV2A_PROF_COUNTERS_ENUM s2t_miss_counter = NV2A_PROF_S2T_MISS_NOSURF;
    SurfaceBinding *surface = pgraph_vk_surface_get(d, texture_vram_offset);
    if (surface && state.levels == 1) {
        S2TCompat compat = surface_to_texture_compat_reason(surface, &state);
        surface_to_texture = (compat == S2T_COMPAT_OK);

        if (!surface_to_texture && surface->color) {
            trace_nv2a_pgraph_surface_texture_compat_failed(
                surface->shape.color_format,
                state.color_format);
        }
        switch (compat) {
        case S2T_COMPAT_PITCH:
            s2t_miss_counter = NV2A_PROF_S2T_MISS_PITCH;
            break;
        case S2T_COMPAT_DIMS:
            s2t_miss_counter = NV2A_PROF_S2T_MISS_DIMS;
            break;
        case S2T_COMPAT_CUBEMAP:
            s2t_miss_counter = NV2A_PROF_S2T_MISS_CUBEMAP;
            break;
        case S2T_COMPAT_MIPS:
            s2t_miss_counter = NV2A_PROF_S2T_MISS_MIPS;
            break;
        case S2T_COMPAT_FORMAT:
            s2t_miss_counter = NV2A_PROF_S2T_MISS_FORMAT;
            break;
        default:
            break;
        }

        if (surface_to_texture && surface->upload_pending) {
            pgraph_vk_upload_surface_data(d, surface, false);
        }
    } else if (surface) {
        s2t_miss_counter = NV2A_PROF_S2T_MISS_MIPS; /* levels > 1 gate */
    }

    /* loc-graphics-research: extended surface-as-texture paths
     * (XEMU_SURF2TEX_EXT). Where the exact-match test failed, try in order:
     * QUADRANT (swizzled tex over larger swizzled surface, same base),
     * BOUNCE (swizzled tex over linear surface, same base — LoC's shadow-map
     * storm), GATHER (row-aligned copies from overlapping linear surfaces).
     * All fully GPU-side, replacing the download+rehash fallback below. */
    enum {
        S2T_PATH_NONE = 0,
        S2T_PATH_GATHER,
        S2T_PATH_QUAD,
        S2T_PATH_BOUNCE,
    } s2t_path = S2T_PATH_NONE;
    SurfaceBinding *s2t_surface = NULL;
    S2TGatherResult s2t_rej = S2T_GATHER_REJ_NONE_RESIDENT;
    if (!surface_to_texture && surf2tex_ext_enabled()) {
        PGRAPHVkState *r_ = pg->vk_renderer_state;
        VkColorFormatInfo tex_vkf =
            kelvin_color_format_vk_map[state.color_format];
        unsigned int tex_bpp = vk_format_texel_size(tex_vkf.vk_format);
        BasicColorFormatInfo tex_basic =
            kelvin_color_format_info_map[state.color_format];
        size_t tex_len = (size_t)state.width * state.height * tex_bpp;

        /* The texture base may sit at an exact surface base (surface) or
         * inside a larger one (WITHIN — LoC's swizzled sub-blocks in the
         * shadow map's footprint). */
        SurfaceBinding *host =
            surface ? surface :
                      pgraph_vk_surface_get_within(d, texture_vram_offset);

        if (host && state.levels == 1 && !tex_basic.linear &&
            state.dimensionality == 2 && !state.cubemap &&
            tex_vkf.vk_format && tex_bpp) {
            hwaddr delta = texture_vram_offset - host->vram_addr;
            if (host == surface && host->swizzle && host->color &&
                is_pow2_square(state.width, state.height) &&
                is_pow2_square(host->width, host->height) &&
                host->width >= state.width &&
                host->host_fmt.host_bytes_per_pixel == tex_bpp &&
                !other_surface_overlaps_range(r_, host,
                                              texture_vram_offset, tex_len)) {
                s2t_path = S2T_PATH_QUAD;
            } else if (!host->swizzle && tex_bpp == 4 &&
                       is_pow2_square(state.width, state.height) &&
                       pg->surface_scale_factor == 1 &&
                       host->host_fmt.vk_format !=
                           VK_FORMAT_D24_UNORM_S8_UINT &&
                       host->host_fmt.vk_format !=
                           VK_FORMAT_D32_SFLOAT_S8_UINT &&
                       host->pitch != 0 &&
                       (host->pitch %
                        host->host_fmt.host_bytes_per_pixel) == 0 &&
                       (delta % 4) == 0 &&
                       DIV_ROUND_UP(delta + tex_len, (size_t)host->pitch) <=
                           host->height &&
                       !pgraph_vk_compute_needs_finish(r_) &&
                       !other_surface_overlaps_range(r_, host,
                                                     texture_vram_offset,
                                                     tex_len)) {
                s2t_path = S2T_PATH_BOUNCE;
            }
        }

        if (s2t_path != S2T_PATH_NONE) {
            s2t_surface = host;
            if (s2t_surface->upload_pending) {
                pgraph_vk_upload_surface_data(d, s2t_surface, false);
            }
            surface_to_texture = true;
        } else {
            s2t_rej = build_surf2tex_gather_plan(d, texture_vram_offset,
                                                 &state, &s2t_plan);
            if (s2t_rej == S2T_GATHER_OK) {
                s2t_path = S2T_PATH_GATHER;
                s2t_plan_valid = true;
                surface_to_texture = true;
                for (int i = 0; i < s2t_plan.num_regions; i++) {
                    SurfaceBinding *s = s2t_plan.regions[i].surface;
                    if (s->upload_pending) {
                        pgraph_vk_upload_surface_data(d, s, false);
                    }
                }
            }
        }
    }

    if (!surface_to_texture) {
        // FIXME: Restructure to support rendering surfaces to cubemap faces

        // Writeback any surfaces which this texture may index
        int forced = pgraph_vk_download_surfaces_in_range_if_dirty(
            pg, texture_vram_offset, texture_length);

        /* sb-graphics-research H5: the VK backend never counted this
         * (only GL did). Tick ONLY when a dirty surface was actually
         * downloaded to feed this texture — the slow-path RTT consume
         * (blocking download + full-texture hash + CPU re-upload).
         * Steady per-frame non-zero here = the sub-monitor fallback
         * storm. Plain texture creations that overlap nothing dirty do
         * not count. */
        if (forced > 0) {
            nv2a_profile_inc_counter(NV2A_PROF_SURF_TO_TEX_FALLBACK);
            /* Refine NOSURF → WITHIN only on the storm path (forced>0), so
             * the common plain-texture bind never pays the list walk. */
            if (s2t_miss_counter == NV2A_PROF_S2T_MISS_NOSURF &&
                pgraph_vk_surface_get_within(d, texture_vram_offset)) {
                s2t_miss_counter = NV2A_PROF_S2T_MISS_WITHIN;
            }
            nv2a_profile_inc_counter(s2t_miss_counter);
            /* Gather-rejection breakdown + geometry dump (EXT runs only) —
             * why the gather could not take this bind either. */
            if (surf2tex_ext_enabled() && s2t_rej != S2T_GATHER_OK) {
                static const enum NV2A_PROF_COUNTERS_ENUM rej_counter[] = {
                    [S2T_GATHER_REJ_TEXSHAPE] = NV2A_PROF_S2T_REJ_TEXSHAPE,
                    [S2T_GATHER_REJ_SWIZSURF] = NV2A_PROF_S2T_REJ_SWIZSURF,
                    [S2T_GATHER_REJ_PITCH] = NV2A_PROF_S2T_REJ_PITCH,
                    [S2T_GATHER_REJ_BPP] = NV2A_PROF_S2T_REJ_BPP,
                    [S2T_GATHER_REJ_ALIGN] = NV2A_PROF_S2T_REJ_ALIGN,
                    [S2T_GATHER_REJ_OVERFLOW] = NV2A_PROF_S2T_REJ_OVERFLOW,
                    [S2T_GATHER_REJ_NONE_RESIDENT] =
                        NV2A_PROF_S2T_REJ_NONE_RESIDENT,
                };
                nv2a_profile_inc_counter(rej_counter[s2t_rej]);
                surf2tex_debug_dump_miss(d, texture_vram_offset, &state,
                                         surface);
            }
        }
    }

    if (surface_to_texture && pg->surface_scale_factor > 1) {
        key.scale = pg->surface_scale_factor;
    }

    uint64_t key_hash = fast_hash((void*)&key, sizeof(key));
    LruNode *node = lru_lookup(&r->texture_cache, key_hash, &key);
    TextureBinding *snode = container_of(node, TextureBinding, node);
    bool binding_found = snode->image != VK_NULL_HANDLE;

    if (binding_found) {
        NV2A_VK_DPRINTF("Cache hit");
        r->texture_bindings[texture_idx] = snode;
        possibly_dirty |= snode->possibly_dirty;
    } else {
        possibly_dirty = true;
    }

    if (!surface_to_texture && !possibly_dirty_checked) {
        possibly_dirty |= check_texture_possibly_dirty(
            d, texture_vram_offset, texture_length, texture_palette_vram_offset,
            texture_palette_data_size);
    }

    // Calculate hash of texture data, if necessary
    void *texture_data = (char*)d->vram_ptr + texture_vram_offset;
    void *palette_data = (char*)d->vram_ptr + texture_palette_vram_offset;

    uint64_t content_hash = 0;
    if (!surface_to_texture && possibly_dirty) {
        /* sb-graphics-research: this is the OTHER agent's cost site — a
         * full re-hash of the texture's bytes, triggered when the radar
         * buffer dirties adjacent texture VRAM. Count both the number of
         * hashes and the bytes hashed to settle "4600 hashes vs 4600
         * lookups with few hashes". */
        nv2a_profile_inc_counter(NV2A_PROF_TEX_HASH);
        g_nv2a_stats.frame_working.counters[NV2A_PROF_TEX_HASH_KB] +=
            (int)(texture_length >> 10);
        /* pfifo: texture-content hashing (the ~35MB/frame TEX_HASH work). The
         * leaf spans exactly the fast_hash calls below (scope closes at the
         * end of this if-block). create_texture runs on the pfifo thread,
         * inside the draw dispatch (bind_textures -> draw_begin -> pusher). */
        NV2A_PHASE_LEAF(PF_LEAF_TEXHASH);
        content_hash = fast_hash(texture_data, texture_length);
        if (is_indexed) {
            content_hash ^= fast_hash(palette_data, texture_palette_data_size);
        }
    }

    if (binding_found) {
        if (surface_to_texture) {
            // FIXME: Add draw time tracking
            if (s2t_plan_valid) {
                /* Gather dedup: re-copy only when any gathered surface was
                 * redrawn since this binding last consumed it. */
                if ((int)s2t_plan.draw_time_key != snode->draw_time) {
                    copy_gather_plan_to_texture(pg, &s2t_plan, snode, false);
                }
            } else if (s2t_path == S2T_PATH_QUAD) {
                if (s2t_surface->draw_time != snode->draw_time) {
                    copy_swizzled_quadrant_to_texture(pg, s2t_surface, snode);
                }
            } else if (s2t_path == S2T_PATH_BOUNCE) {
                if (s2t_surface->draw_time != snode->draw_time) {
                    bounce_linear_surface_to_swizzled_texture(
                        pg, s2t_surface, snode, texture_vram_offset);
                }
            } else if (surface->draw_time != snode->draw_time) {
                copy_surface_to_texture(pg, surface, snode);
            }
        } else {
            if (possibly_dirty && content_hash != snode->hash) {
                upload_texture_image(pg, texture_idx, snode,
                                     true /* in-place re-upload */);
                snode->hash = content_hash;
            }
        }

        /* sb-graphics-research (GL-parity hash-leak fix): clear the deferred-
         * dirty flag on the cache-HIT path too. VK historically cleared
         * snode->possibly_dirty ONLY on the cache-MISS path below, so a node
         * flagged possibly_dirty once — e.g. by the whole-VRAM mark in
         * pgraph_vk_flush, which flags every resident cache entry — then
         * RE-HASHED on every subsequent bind for the life of the node, a
         * permanent per-frame hash leak the GL backend never had (gl/texture.c
         * clears possibly_dirty unconditionally, hit or miss). This bind has
         * just serviced the deferred dirty (hashed above; uploaded iff
         * content_hash != snode->hash), so the flag is consumed; any FUTURE
         * genuine change re-arms it via the page bitmap / exact-span check on the
         * next bind. Output-identical — uploads stay guarded by the hash compare,
         * so this removes redundant HASHING only, never a real texture update.
         * Default ON; XEMU_NO_TEX_DIRTY_CLEAR=1 restores the old leaky behavior
         * for A/B measurement (the cache-MISS clear below is left untouched, so
         * the revert is byte-identical to the historical path). */
        {
            static int clear_on_hit = -1;
            if (clear_on_hit < 0) {
                clear_on_hit = getenv("XEMU_NO_TEX_DIRTY_CLEAR") ? 0 : 1;
            }
            if (clear_on_hit) {
                snode->possibly_dirty = false;
            }
        }

        NV2A_VK_DGROUP_END();
        return;
    }

    NV2A_VK_DPRINTF("Cache miss");

    memcpy(&snode->key, &key, sizeof(key));
    snode->current_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    snode->possibly_dirty = false;
    snode->hash = content_hash;

    VkColorFormatInfo vkf = kelvin_color_format_vk_map[state.color_format];
    assert(vkf.vk_format != 0);
    assert(0 < state.dimensionality);
    assert(state.dimensionality < ARRAY_SIZE(dimensionality_to_vk_image_type));
    assert(state.dimensionality <
           ARRAY_SIZE(dimensionality_to_vk_image_view_type));

    VkImageCreateInfo image_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = dimensionality_to_vk_image_type[state.dimensionality],
        .extent.width = state.width, // FIXME: Use adjusted size?
        .extent.height = state.height,
        .extent.depth = state.depth,
        .mipLevels = f_basic.linear ? 1 : state.levels,
        .arrayLayers = state.cubemap ? 6 : 1,
        .format = vkf.vk_format,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .flags = (state.cubemap ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0),
    };

    if (surface_to_texture) {
        pgraph_apply_scaling_factor(pg, &image_create_info.extent.width,
                                        &image_create_info.extent.height);
    }

    VmaAllocationCreateInfo alloc_create_info = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    };

    VK_CHECK(vmaCreateImage(r->allocator, &image_create_info,
                            &alloc_create_info, &snode->image,
                            &snode->allocation, NULL));

    VkImageViewCreateInfo image_view_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = snode->image,
        .viewType = state.cubemap ?
            VK_IMAGE_VIEW_TYPE_CUBE :
            dimensionality_to_vk_image_view_type[state.dimensionality],
        .format = vkf.vk_format,
        .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .subresourceRange.baseMipLevel = 0,
        .subresourceRange.levelCount = image_create_info.mipLevels,
        .subresourceRange.baseArrayLayer = 0,
        .subresourceRange.layerCount = image_create_info.arrayLayers,
        .components = vkf.component_map,
    };

    VK_CHECK(vkCreateImageView(r->device, &image_view_create_info, NULL,
                               &snode->image_view));


    void *sampler_next_struct = NULL;

    VkSamplerCustomBorderColorCreateInfoEXT custom_border_color_create_info;
    VkBorderColor vk_border_color;

    bool is_integer_type = vkf.vk_format == VK_FORMAT_R32_UINT;

    if (r->custom_border_color_extension_enabled) {
        vk_border_color = is_integer_type ? VK_BORDER_COLOR_INT_CUSTOM_EXT :
                                            VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
        custom_border_color_create_info =
            (VkSamplerCustomBorderColorCreateInfoEXT){
                .sType =
                    VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT,
                .format = image_view_create_info.format,
                .pNext = sampler_next_struct
            };
        if (is_integer_type) {
            float rgba[4];
            pgraph_argb_pack32_to_rgba_float(border_color_pack32, rgba);
            for (int i = 0; i < 4; i++) {
                custom_border_color_create_info.customBorderColor.uint32[i] =
                    (uint32_t)((double)rgba[i] * (double)0xffffffff);
            }
        } else {
            pgraph_argb_pack32_to_rgba_float(
                border_color_pack32,
                custom_border_color_create_info.customBorderColor.float32);
        }
        sampler_next_struct = &custom_border_color_create_info;
    } else {
        // FIXME: Handle custom color in shader
        if (is_integer_type) {
            vk_border_color = VK_BORDER_COLOR_INT_TRANSPARENT_BLACK;
        } else if (border_color_pack32 == 0x00000000) {
            vk_border_color = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        } else if (border_color_pack32 == 0xff000000) {
            vk_border_color = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        } else {
            vk_border_color = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        }
    }

    if (filter & NV_PGRAPH_TEXFILTER0_ASIGNED)
        NV2A_UNIMPLEMENTED("NV_PGRAPH_TEXFILTER0_ASIGNED");
    if (filter & NV_PGRAPH_TEXFILTER0_RSIGNED)
        NV2A_UNIMPLEMENTED("NV_PGRAPH_TEXFILTER0_RSIGNED");
    if (filter & NV_PGRAPH_TEXFILTER0_GSIGNED)
        NV2A_UNIMPLEMENTED("NV_PGRAPH_TEXFILTER0_GSIGNED");
    if (filter & NV_PGRAPH_TEXFILTER0_BSIGNED)
        NV2A_UNIMPLEMENTED("NV_PGRAPH_TEXFILTER0_BSIGNED");

    VkFilter vk_min_filter, vk_mag_filter;
    unsigned int mag_filter = GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MAG);
    assert(mag_filter < ARRAY_SIZE(pgraph_texture_mag_filter_vk_map));

    unsigned int min_filter = GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MIN);
    assert(min_filter < ARRAY_SIZE(pgraph_texture_min_filter_vk_map));

    if (is_linear_filter_supported_for_format(r, state.color_format)) {
        vk_mag_filter = pgraph_texture_min_filter_vk_map[mag_filter];
        vk_min_filter = pgraph_texture_min_filter_vk_map[min_filter];
    } else {
        vk_mag_filter = vk_min_filter = VK_FILTER_NEAREST;
    }

    bool mipmap_en =
        !f_basic.linear &&
        !(min_filter == NV_PGRAPH_TEXFILTER0_MIN_BOX_LOD0 ||
          min_filter == NV_PGRAPH_TEXFILTER0_MIN_TENT_LOD0 ||
          min_filter == NV_PGRAPH_TEXFILTER0_MIN_CONVOLUTION_2D_LOD0);

    bool mipmap_nearest =
        f_basic.linear || image_create_info.mipLevels == 1 ||
        min_filter == NV_PGRAPH_TEXFILTER0_MIN_BOX_NEARESTLOD ||
        min_filter == NV_PGRAPH_TEXFILTER0_MIN_TENT_NEARESTLOD;

    float lod_bias = pgraph_convert_lod_bias_to_float(
        GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MIPMAP_LOD_BIAS));
    if (lod_bias > r->device_props.limits.maxSamplerLodBias) {
        lod_bias = r->device_props.limits.maxSamplerLodBias;
    } else if (lod_bias < -r->device_props.limits.maxSamplerLodBias) {
        lod_bias = -r->device_props.limits.maxSamplerLodBias;
    }
    uint32_t sampler_max_anisotropy =
        MIN(r->device_props.limits.maxSamplerAnisotropy, max_anisotropy);

    VkSamplerCreateInfo sampler_create_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = vk_mag_filter,
        .minFilter = vk_min_filter,
        .addressModeU = lookup_texture_address_mode(
            GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRU)),
        .addressModeV = lookup_texture_address_mode(
            GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRV)),
        .addressModeW = (state.dimensionality > 2) ? lookup_texture_address_mode(
            GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRP)) : 0,
        .anisotropyEnable =
            r->enabled_physical_device_features.samplerAnisotropy &&
            sampler_max_anisotropy > 1,
        .maxAnisotropy = sampler_max_anisotropy,
        .borderColor = vk_border_color,
        .compareEnable = VK_FALSE,
        .compareOp = VK_COMPARE_OP_ALWAYS,
        .mipmapMode = mipmap_nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST :
                                       VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .minLod = mipmap_en ? MIN(state.min_mipmap_level, state.levels - 1) : 0.0,
        .maxLod = mipmap_en ? MIN(state.max_mipmap_level, state.levels - 1) : 0.0,
        .mipLodBias = lod_bias,
        .pNext = sampler_next_struct,
    };

    VK_CHECK(vkCreateSampler(r->device, &sampler_create_info, NULL,
                             &snode->sampler));

    set_texture_label(pg, snode);

    r->texture_bindings[texture_idx] = snode;

    if (surface_to_texture) {
        if (s2t_plan_valid) {
            copy_gather_plan_to_texture(pg, &s2t_plan, snode,
                                        true /* fresh image */);
        } else if (s2t_path == S2T_PATH_QUAD) {
            copy_swizzled_quadrant_to_texture(pg, s2t_surface, snode);
        } else if (s2t_path == S2T_PATH_BOUNCE) {
            bounce_linear_surface_to_swizzled_texture(pg, s2t_surface, snode,
                                                      texture_vram_offset);
        } else {
            copy_surface_to_texture(pg, surface, snode);
        }
    } else {
        upload_texture_image(pg, texture_idx, snode,
                             false /* fresh image, first fill */);
        snode->draw_time = 0;
    }

    NV2A_VK_DGROUP_END();
}

static bool check_textures_dirty(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (!r->texture_bindings[i] || pg->texture_dirty[i]) {
            return true;
        }
    }
    return false;
}

static void update_timestamps(PGRAPHVkState *r)
{
    for (int i = 0; i < ARRAY_SIZE(r->texture_bindings); i++) {
        if (r->texture_bindings[i]) {
            r->texture_bindings[i]->submit_time = r->submit_count;
        }
    }
}

void pgraph_vk_bind_textures(NV2AState *d)
{
    NV2A_VK_DGROUP_BEGIN("%s", __func__);

    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    // FIXME: Check for modifications on bind fastpath (CPU hook)
    // FIXME: Mark textures that are sourced from surfaces so we can track them

    r->texture_bindings_changed = false;

    if (!check_textures_dirty(pg)) {
        NV2A_VK_DPRINTF("Not dirty");
        NV2A_VK_DGROUP_END();
        update_timestamps(r);
        return;
    }

    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (!pgraph_is_texture_enabled(pg, i)) {
            r->texture_bindings[i] = &r->dummy_texture;
            continue;
        }

        create_texture(pg, i);

        pg->texture_dirty[i] = false; // FIXME: Move to renderer?
    }

    r->texture_bindings_changed = true;
    update_timestamps(r);
    NV2A_VK_DGROUP_END();
}

static void texture_cache_entry_init(Lru *lru, LruNode *node, const void *state)
{
    TextureBinding *snode = container_of(node, TextureBinding, node);

    snode->image = VK_NULL_HANDLE;
    snode->allocation = VK_NULL_HANDLE;
    snode->image_view = VK_NULL_HANDLE;
    snode->sampler = VK_NULL_HANDLE;
}

static void texture_cache_release_node_resources(PGRAPHVkState *r, TextureBinding *snode)
{
    /* narrow-fence UAF gate: an already-submitted in-flight batch may still
     * sample this texture; the pre-evict check only covers the open command
     * buffer. Wait only the submission that last sampled it (no-op when
     * retired — a wait-all here measured as a mid-frame drain). */
    pgraph_vk_wait_for_submission(r, snode->submit_time);

    vkDestroySampler(r->device, snode->sampler, NULL);
    snode->sampler = VK_NULL_HANDLE;

    vkDestroyImageView(r->device, snode->image_view, NULL);
    snode->image_view = VK_NULL_HANDLE;

    vmaDestroyImage(r->allocator, snode->image, snode->allocation);
    snode->image = VK_NULL_HANDLE;
    snode->allocation = VK_NULL_HANDLE;
}

static bool texture_cache_entry_pre_evict(Lru *lru, LruNode *node)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, texture_cache);
    TextureBinding *snode = container_of(node, TextureBinding, node);

    // FIXME: Simplify. We don't really need to check bindings


    // Currently bound
    for (int i = 0; i < ARRAY_SIZE(r->texture_bindings); i++) {
        if (r->texture_bindings[i] == snode) {
            return false;
        }
    }

    // Used in command buffer
    if (r->in_command_buffer && snode->submit_time == r->submit_count) {
        return false;
    }

    return true;
}

static void texture_cache_entry_post_evict(Lru *lru, LruNode *node)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, texture_cache);
    TextureBinding *snode = container_of(node, TextureBinding, node);
    texture_cache_release_node_resources(r, snode);
}

static bool texture_cache_entry_compare(Lru *lru, LruNode *node,
                                        const void *key)
{
    TextureBinding *snode = container_of(node, TextureBinding, node);
    return memcmp(&snode->key, key, sizeof(TextureKey));
}

static void texture_cache_init(PGRAPHVkState *r)
{
    const size_t texture_cache_size = 1024;
    lru_init(&r->texture_cache);
    r->texture_cache_entries = g_malloc_n(texture_cache_size, sizeof(TextureBinding));
    assert(r->texture_cache_entries != NULL);
    for (int i = 0; i < texture_cache_size; i++) {
        lru_add_free(&r->texture_cache, &r->texture_cache_entries[i].node);
    }
    r->texture_cache.init_node = texture_cache_entry_init;
    r->texture_cache.compare_nodes = texture_cache_entry_compare;
    r->texture_cache.pre_node_evict = texture_cache_entry_pre_evict;
    r->texture_cache.post_node_evict = texture_cache_entry_post_evict;
}

static void texture_cache_finalize(PGRAPHVkState *r)
{
    lru_flush(&r->texture_cache);
    g_free(r->texture_cache_entries);
    r->texture_cache_entries = NULL;
}

void pgraph_vk_trim_texture_cache(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    // FIXME: Allow specifying some amount to trim by

    int num_to_evict = r->texture_cache.num_used / 4;
    int num_evicted = 0;

    while (num_to_evict-- && lru_try_evict_one(&r->texture_cache)) {
        num_evicted += 1;
    }

    NV2A_VK_DPRINTF("Evicted %d textures, %d remain", num_evicted, r->texture_cache.num_used);
}

void pgraph_vk_init_textures(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    texture_cache_init(r);
    create_dummy_texture(pg);

    r->texture_format_properties = g_malloc0_n(
        ARRAY_SIZE(kelvin_color_format_vk_map), sizeof(VkFormatProperties));
    for (int i = 0; i < ARRAY_SIZE(kelvin_color_format_vk_map); i++) {
        vkGetPhysicalDeviceFormatProperties(
            r->physical_device, kelvin_color_format_vk_map[i].vk_format,
            &r->texture_format_properties[i]);
    }
}

void pgraph_vk_finalize_textures(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    assert(!r->in_command_buffer);

    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        r->texture_bindings[i] = NULL;
    }

    destroy_dummy_texture(r);
    texture_cache_finalize(r);

    assert(r->texture_cache.num_used == 0);

    g_free(r->texture_format_properties);
    r->texture_format_properties = NULL;
}
