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

#include "renderer.h"
#include <math.h>

static uint8_t *convert_texture_data__CR8YB8CB8YA8(uint8_t *data_out,
                                                   const uint8_t *data_in,
                                                   unsigned int width,
                                                   unsigned int height,
                                                   unsigned int pitch)
{
    int x, y;
    for (y = 0; y < height; y++) {
        const uint8_t *line = &data_in[y * pitch];
        const uint32_t row_offset = y * width;
        for (x = 0; x < width; x++) {
            uint8_t *pixel = &data_out[(row_offset + x) * 4];
            convert_yuy2_to_rgb(line, x, &pixel[0], &pixel[1], &pixel[2]);
            pixel[3] = 255;
        }
    }
    return data_out;
}

static float pvideo_calculate_scale(unsigned int din_dout,
                                    unsigned int output_size)
{
    float calculated_in = din_dout * (output_size - 1);
    calculated_in = floorf(calculated_in / (1 << 20) + 0.5f);
    return (calculated_in + 1.0f) / output_size;
}

static void destroy_pvideo_image(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    PGRAPHVkDisplayState *d = &r->display;

    if (d->pvideo.sampler != VK_NULL_HANDLE) {
        vkDestroySampler(r->device, d->pvideo.sampler, NULL);
        d->pvideo.sampler = VK_NULL_HANDLE;
    }

    if (d->pvideo.image_view != VK_NULL_HANDLE) {
        vkDestroyImageView(r->device, d->pvideo.image_view, NULL);
        d->pvideo.image_view = VK_NULL_HANDLE;
    }

    if (d->pvideo.image != VK_NULL_HANDLE) {
        vmaDestroyImage(r->allocator, d->pvideo.image, d->pvideo.allocation);
        d->pvideo.image = VK_NULL_HANDLE;
        d->pvideo.allocation = VK_NULL_HANDLE;
    }
}

static void create_pvideo_image(PGRAPHState *pg, int width, int height)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    PGRAPHVkDisplayState *d = &r->display;

    if (d->pvideo.image == VK_NULL_HANDLE || d->pvideo.width != width ||
        d->pvideo.height != height) {
        destroy_pvideo_image(pg);
    }

    VkImageCreateInfo image_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .extent.width = width,
        .extent.height = height,
        .extent.depth = 1,
        .mipLevels = 1,
        .arrayLayers = 1,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
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
    VK_CHECK(vmaCreateImage(r->allocator, &image_create_info,
                            &alloc_create_info, &d->pvideo.image,
                            &d->pvideo.allocation, NULL));

    VkImageViewCreateInfo image_view_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = d->pvideo.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .subresourceRange.baseMipLevel = 0,
        .subresourceRange.levelCount = image_create_info.mipLevels,
        .subresourceRange.baseArrayLayer = 0,
        .subresourceRange.layerCount = image_create_info.arrayLayers,
    };
    VK_CHECK(vkCreateImageView(r->device, &image_view_create_info, NULL,
                               &d->pvideo.image_view));

    VkSamplerCreateInfo sampler_create_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .borderColor = VK_BORDER_COLOR_INT_OPAQUE_WHITE,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
    };
    VK_CHECK(vkCreateSampler(r->device, &sampler_create_info, NULL,
                             &d->pvideo.sampler));
}

static void upload_pvideo_image(PGRAPHState *pg, PvideoState state)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHVkState *r = pg->vk_renderer_state;
    PGRAPHVkDisplayState *disp = &r->display;

    create_pvideo_image(pg, state.in_width, state.in_height);

    // FIXME: Dirty tracking. We don't necessarily need to upload so much.

    // Copy texture data to mapped device buffer
    uint8_t *mapped_memory_ptr;

    VK_CHECK(vmaMapMemory(r->allocator,
                          r->storage_buffers[BUFFER_STAGING_SRC].allocation,
                          (void *)&mapped_memory_ptr));

    convert_texture_data__CR8YB8CB8YA8(
        mapped_memory_ptr, d->vram_ptr + state.base + state.offset,
        state.in_width, state.in_height, state.pitch);

    vmaFlushAllocation(r->allocator,
                       r->storage_buffers[BUFFER_STAGING_SRC].allocation, 0,
                       VK_WHOLE_SIZE);

    vmaUnmapMemory(r->allocator,
                   r->storage_buffers[BUFFER_STAGING_SRC].allocation);

    // FIXME: Merge with display renderer command buffer

    VkCommandBuffer cmd = pgraph_vk_begin_single_time_commands(pg);

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

    pgraph_vk_transition_image_layout(
        pg, cmd, disp->pvideo.image, VK_FORMAT_R8_UNORM,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkBufferImageCopy region = {
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .imageSubresource.mipLevel = 0,
        .imageSubresource.baseArrayLayer = 0,
        .imageSubresource.layerCount = 1,
        .imageOffset = (VkOffset3D){ 0, 0, 0 },
        .imageExtent = (VkExtent3D){ state.in_width, state.in_height, 1 },
    };
    vkCmdCopyBufferToImage(cmd, r->storage_buffers[BUFFER_STAGING_SRC].buffer,
                           disp->pvideo.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    pgraph_vk_transition_image_layout(pg, cmd, disp->pvideo.image,
                                      VK_FORMAT_R8G8B8A8_UNORM,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    pgraph_vk_end_single_time_commands(pg, cmd);
}

static const char *display_frag_glsl =
    "#version 450\n"
    "layout(binding = 0) uniform sampler2D tex;\n"
    "layout(binding = 1) uniform sampler2D pvideo_tex;\n"
    "layout(push_constant, std430) uniform PushConstants {\n"
    "    float line_offset;\n"
    "    vec2 display_size;\n"
    "    bool pvideo_enable;\n"
    "    vec2 pvideo_in_pos;\n"
    "    vec4 pvideo_pos;\n"
    "    vec4 pvideo_scale;\n"
    "    bool pvideo_color_key_enable;\n"
    "    vec3 pvideo_color_key;\n"
    "};\n"
    "layout(location = 0) out vec4 out_Color;\n"
    "void main()\n"
    "{\n"
    "    vec2 tex_coord = gl_FragCoord.xy/display_size;\n"
    "    float rel = display_size.y/textureSize(tex, 0).y/line_offset;\n"
    "    tex_coord.y = 1 + rel*(tex_coord.y - 1);\n"
    "    tex_coord.y = 1 - tex_coord.y;\n" // GL compat
    "    out_Color.rgba = texture(tex, tex_coord);\n"
    "    if (pvideo_enable) {\n"
    "        vec2 screen_coord = vec2(gl_FragCoord.x, display_size.y - gl_FragCoord.y) * pvideo_scale.z;\n"
    "        vec4 output_region = vec4(pvideo_pos.xy, pvideo_pos.xy + pvideo_pos.zw);\n"
    "        bvec4 clip = bvec4(lessThan(screen_coord, output_region.xy),\n"
    "                           greaterThan(screen_coord, output_region.zw));\n"
    "        if (!any(clip) && (!pvideo_color_key_enable || out_Color.rgb == pvideo_color_key)) {\n"
    "            vec2 out_xy = screen_coord - pvideo_pos.xy;\n"
    "            vec2 in_st = (pvideo_in_pos + out_xy * pvideo_scale.xy) / textureSize(pvideo_tex, 0);\n"
    "            out_Color.rgba = texture(pvideo_tex, in_st);\n"
    "        }\n"
    "    }\n"
    "}\n";

/* S2 async present (docs/vr/overlap-present-path.md §5.1): submit the
 * composite without waiting; the display thread fence-gates before GL
 * sampling. Toggle trio (M-5), modeled on the narrowfence toggle
 * (surface.c): env XEMU_ASYNC_PRESENT=1 forces ON, XEMU_NO_ASYNC_PRESENT=1
 * forces OFF (env wins), else watch file /tmp/asyncpresent-on PRESENT => ON
 * (own file — /tmp/narrowfence-on is NOT overloaded; that config carries the
 * measured-negative upload/eviction gates). Re-read once per frame from
 * flip_stall; hot sites read the cached bool. TCG-only: under KVM the
 * display.c force-upload present branch keeps synchronous semantics, so
 * !tcg_enabled() pins the feature OFF. Default OFF; OFF path byte-identical
 * (both present sites keep calling pgraph_vk_finish exactly as today). */
static bool g_async_present_enabled = false;

void pgraph_vk_refresh_async_present_toggle(void)
{
    static const char *watch;
    static int forced = -1; /* -1 unset, 0 force-off, 1 force-on, 2 file */
    if (forced < 0) {
        if (!tcg_enabled() || getenv("XEMU_NO_ASYNC_PRESENT")) {
            forced = 0;
        } else if (getenv("XEMU_ASYNC_PRESENT")) {
            forced = 1;
        } else {
            forced = 2;
            watch = getenv("XEMU_ASYNCPRESENT_TOGGLE_FILE");
            if (!watch) {
                watch = "/tmp/asyncpresent-on";
            }
        }
    }
    bool now_on = (forced == 1) ||
                  (forced == 2 && access(watch, F_OK) == 0);
    static int last = -1;
    if ((int)now_on != last) {
        fprintf(stderr, "asyncpresent: submit-no-wait present %s\n",
                now_on ? "ON" : "OFF");
        last = now_on;
    }
    g_async_present_enabled = now_on;
}

bool pgraph_vk_async_present_enabled(void)
{
    return g_async_present_enabled;
}

static void create_descriptor_pool(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkDescriptorPoolSize pool_sizes = {
        .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        /* 2 combined-image-samplers per set: the singleton set + one set
         * per async-present slot (§5.1(1): per-slot sets kill the
         * update-while-in-use hazard). */
        .descriptorCount = 2 * (1 + ASYNC_PRESENT_RING_SIZE),
    };

    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_sizes,
        .maxSets = 1 + ASYNC_PRESENT_RING_SIZE,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
    };
    VK_CHECK(vkCreateDescriptorPool(r->device, &pool_info, NULL,
                                    &r->display.descriptor_pool));
}

static void destroy_descriptor_pool(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkDestroyDescriptorPool(r->device, r->display.descriptor_pool, NULL);
    r->display.descriptor_pool = VK_NULL_HANDLE;
}

static void create_descriptor_set_layout(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkDescriptorSetLayoutBinding bindings[2];

    for (int i = 0; i < ARRAY_SIZE(bindings); i++) {
        bindings[i] = (VkDescriptorSetLayoutBinding){
            .binding = i,
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
                                         &r->display.descriptor_set_layout));
}

static void destroy_descriptor_set_layout(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkDestroyDescriptorSetLayout(r->device, r->display.descriptor_set_layout,
                                 NULL);
    r->display.descriptor_set_layout = VK_NULL_HANDLE;
}

static void create_descriptor_sets(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkDescriptorSetLayout layout = r->display.descriptor_set_layout;

    VkDescriptorSetAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = r->display.descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &layout,
    };
    VK_CHECK(vkAllocateDescriptorSets(r->device, &alloc_info,
                                      &r->display.descriptor_set));

    /* S2 async present: one set per present slot, same layout. Inert until
     * the async path writes/binds them. Freed with the pool. */
    for (int i = 0; i < ASYNC_PRESENT_RING_SIZE; i++) {
        VK_CHECK(vkAllocateDescriptorSets(
            r->device, &alloc_info, &r->display.present_slots[i].desc_set));
    }
}

static void create_render_pass(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkAttachmentDescription attachment;

    VkAttachmentReference color_reference;
    attachment = (VkAttachmentDescription){
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    color_reference = (VkAttachmentReference){
        0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
    };

    VkSubpassDependency dependency = {
        .srcSubpass = VK_SUBPASS_EXTERNAL,
    };

    dependency.srcStageMask |=
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask |=
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_reference,
    };

    VkRenderPassCreateInfo renderpass_create_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &attachment,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &dependency,
    };
    VK_CHECK(vkCreateRenderPass(r->device, &renderpass_create_info, NULL,
                                &r->display.render_pass));
}

static void destroy_render_pass(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    vkDestroyRenderPass(r->device, r->display.render_pass, NULL);
    r->display.render_pass = VK_NULL_HANDLE;
}

static void create_display_pipeline(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    r->display.display_frag =
        pgraph_vk_create_shader_module_from_glsl(
            r, VK_SHADER_STAGE_FRAGMENT_BIT, display_frag_glsl);

    VkPipelineShaderStageCreateInfo shader_stages[] = {
        (VkPipelineShaderStageCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = r->quad_vert_module->module,
            .pName = "main",
        },
        (VkPipelineShaderStageCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = r->display.display_frag->module,
            .pName = "main",
        },
     };

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
        .depthTestEnable = VK_FALSE,
        .depthCompareOp = VK_COMPARE_OP_ALWAYS,
        .depthBoundsTestEnable = VK_FALSE,
    };

    VkPipelineColorBlendAttachmentState color_blend_attachment = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        .blendEnable = VK_FALSE,
    };

    VkPipelineColorBlendStateCreateInfo color_blending = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable = VK_FALSE,
        .logicOp = VK_LOGIC_OP_COPY,
        .attachmentCount = 1,
        .pAttachments = &color_blend_attachment,
    };

    VkDynamicState dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT,
                                        VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = dynamic_states,
    };

    VkPushConstantRange push_constant_range = {
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = r->display.display_frag->push_constants.total_size,
    };

    VkPipelineLayoutCreateInfo pipeline_layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &r->display.descriptor_set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_constant_range,
    };
    VK_CHECK(vkCreatePipelineLayout(r->device, &pipeline_layout_info, NULL,
                                    &r->display.pipeline_layout));

    VkGraphicsPipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = ARRAY_SIZE(shader_stages),
        .pStages = shader_stages,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = r->zeta_binding ? &depth_stencil : NULL,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_state,
        .layout = r->display.pipeline_layout,
        .renderPass = r->display.render_pass,
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
    };
    VK_CHECK(vkCreateGraphicsPipelines(r->device, r->vk_pipeline_cache, 1,
                                       &pipeline_info, NULL,
                                       &r->display.pipeline));
}

static void destroy_display_pipeline(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkDestroyPipeline(r->device, r->display.pipeline, NULL);
    r->display.pipeline = VK_NULL_HANDLE;

    vkDestroyPipelineLayout(r->device, r->display.pipeline_layout, NULL);
    r->display.pipeline_layout = VK_NULL_HANDLE;

    pgraph_vk_destroy_shader_module(r, r->display.display_frag);
    r->display.display_frag = NULL;
}

static void create_frame_buffer(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkFramebufferCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = r->display.render_pass,
        .attachmentCount = 1,
        .pAttachments = &r->display.image_view,
        .width = r->display.width,
        .height = r->display.height,
        .layers = 1,
    };
    VK_CHECK(vkCreateFramebuffer(r->device, &create_info, NULL,
                                 &r->display.framebuffer));
}

static void destroy_frame_buffer(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    vkDestroyFramebuffer(r->device, r->display.framebuffer, NULL);
    r->display.framebuffer = NULL;
}

/* S2 async present §5.1(7): retire every in-flight present-ring submission.
 * The destruction-path UAF guard — called before destroying anything an
 * in-flight composite may reference (the sampled scanout surface image at
 * invalidate_surface, the slot images at resize/finalize, everything at
 * flush/loadvm). Waits only; fences are reset exclusively at slot reuse in
 * render_display_async, so this can never race a concurrent display-thread
 * vkWaitForFences on the published fence with a reset (fence WAITS are
 * thread-safe without external synchronization; resets are not). Near-free
 * when OFF or idle: two fence_submitted bool checks. */
void pgraph_vk_wait_all_present_slots(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    PGRAPHVkDisplayState *d = &r->display;

    for (int i = 0; i < ASYNC_PRESENT_RING_SIZE; i++) {
        PresentSlot *slot = &d->present_slots[i];
        if (!slot->fence_submitted) {
            continue;
        }
        if (vkGetFenceStatus(r->device, slot->fence) == VK_SUCCESS) {
            continue;
        }
        VK_CHECK(vkWaitForFences(r->device, 1, &slot->fence, VK_TRUE,
                                 UINT64_MAX));
    }
}

/* Destroy one present slot's image resources (image/view/framebuffer/GL
 * import). The slot's CB/fence/desc_set live on (init-to-finalize). Caller
 * must have retired the slot's in-flight submission first
 * (pgraph_vk_wait_all_present_slots or a per-slot fence wait). Idempotent. */
static void destroy_present_slot_image(PGRAPHState *pg, PresentSlot *slot)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    PGRAPHVkDisplayState *d = &r->display;

    if (slot->image == VK_NULL_HANDLE) {
        return;
    }

    /* Anything published from this slot is about to dangle; the next
     * composite republishes. */
    d->present_published_valid = false;

    vkDestroyFramebuffer(r->device, slot->framebuffer, NULL);
    slot->framebuffer = VK_NULL_HANDLE;

#if HAVE_EXTERNAL_MEMORY
    glDeleteTextures(1, &slot->gl_texture_id);
    slot->gl_texture_id = 0;

    glDeleteMemoryObjectsEXT(1, &slot->gl_memory_obj);
    slot->gl_memory_obj = 0;

#ifdef WIN32
    CloseHandle(slot->handle);
    slot->handle = 0;
#endif
#endif

    vkDestroyImageView(r->device, slot->image_view, NULL);
    slot->image_view = VK_NULL_HANDLE;

    vkDestroyImage(r->device, slot->image, NULL);
    slot->image = VK_NULL_HANDLE;

    vkFreeMemory(r->device, slot->memory, NULL);
    slot->memory = VK_NULL_HANDLE;

    slot->width = 0;
    slot->height = 0;
}

static void destroy_current_display_image(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    PGRAPHVkDisplayState *d = &r->display;

    if (d->image == VK_NULL_HANDLE) {
        return;
    }

    /* S2 async present §5.1(7): an in-flight composite may still write a
     * slot image (and sample the scanout surface). Retire the ring, then
     * drop the slot images with the singleton — they are recreated lazily
     * at the next async composite for the new size. Covers resize (called
     * inside sync while the display thread is parked) and finalize. */
    pgraph_vk_wait_all_present_slots(pg);
    for (int i = 0; i < ASYNC_PRESENT_RING_SIZE; i++) {
        destroy_present_slot_image(pg, &d->present_slots[i]);
    }

    destroy_frame_buffer(pg);

#if HAVE_EXTERNAL_MEMORY
    glDeleteTextures(1, &d->gl_texture_id);
    d->gl_texture_id = 0;

    glDeleteMemoryObjectsEXT(1, &d->gl_memory_obj);
    d->gl_memory_obj = 0;

#ifdef WIN32
    CloseHandle(d->handle);
    d->handle = 0;
#endif
#endif

    vkDestroyImageView(r->device, d->image_view, NULL);
    d->image_view = VK_NULL_HANDLE;

    vkDestroyImage(r->device, d->image, NULL);
    d->image = VK_NULL_HANDLE;

    vkFreeMemory(r->device, d->memory, NULL);
    d->memory = VK_NULL_HANDLE;

    d->draw_time = 0;
}

// FIXME: We may need to use two images. One for actually rendering display,
// and another for GL in the correct tiling mode

static void create_display_image(PGRAPHState *pg, int width, int height)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    PGRAPHVkDisplayState *d = &r->display;

    if (r->display.image != VK_NULL_HANDLE) {
        destroy_current_display_image(pg);
    }

    const GLint gl_internal_format = GL_RGBA8;
    bool use_optimal_tiling = true;

#if HAVE_EXTERNAL_MEMORY
    GLint num_tiling_types;
    glGetInternalformativ(GL_TEXTURE_2D, gl_internal_format,
                          GL_NUM_TILING_TYPES_EXT, 1, &num_tiling_types);
    // XXX: Apparently on AMD GL_OPTIMAL_TILING_EXT is reported to be
    // supported, but doesn't work? On nVidia, GL_LINEAR_TILING_EXT may not
    // be supported so we must use optimal. Default to optimal unless
    // linear is explicitly specified...
    GLint tiling_types[num_tiling_types];
    glGetInternalformativ(GL_TEXTURE_2D, gl_internal_format,
                          GL_TILING_TYPES_EXT, num_tiling_types, tiling_types);
    for (int i = 0; i < num_tiling_types; i++) {
        if (tiling_types[i] == GL_LINEAR_TILING_EXT) {
            use_optimal_tiling = false;
            break;
        }
    }
#endif

    // Create image
    VkImageCreateInfo image_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .extent.width = width,
        .extent.height = height,
        .extent.depth = 1,
        .mipLevels = 1,
        .arrayLayers = 1,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .tiling = use_optimal_tiling ? VK_IMAGE_TILING_OPTIMAL : VK_IMAGE_TILING_LINEAR,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        /* TRANSFER_SRC: the VR compositor copies this image into the XR
         * swapchain (vr_xr_compositor.c). */
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };

    VkExternalMemoryImageCreateInfo external_memory_image_create_info = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
#ifdef WIN32
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT,
#else
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR,
#endif
    };
    image_create_info.pNext = &external_memory_image_create_info;

    VK_CHECK(vkCreateImage(r->device, &image_create_info, NULL, &d->image));

    // Allocate and bind image memory
    VkMemoryRequirements memory_requirements;
    vkGetImageMemoryRequirements(r->device, d->image, &memory_requirements);

    VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memory_requirements.size,
        .memoryTypeIndex =
            pgraph_vk_get_memory_type(pg, memory_requirements.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };

    VkExportMemoryAllocateInfo export_memory_alloc_info = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .handleTypes =
#ifdef WIN32
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT_KHR
#else
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT
#endif
            ,
    };
    alloc_info.pNext = &export_memory_alloc_info;

    VK_CHECK(vkAllocateMemory(r->device, &alloc_info, NULL, &d->memory));
    VK_CHECK(vkBindImageMemory(r->device, d->image, d->memory, 0));

    // Create Image View
    VkImageViewCreateInfo image_view_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = d->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = image_create_info.format,
        .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .subresourceRange.levelCount = 1,
        .subresourceRange.layerCount = 1,
    };
    VK_CHECK(vkCreateImageView(r->device, &image_view_create_info, NULL,
                               &d->image_view));

#if HAVE_EXTERNAL_MEMORY

#ifdef WIN32

    VkMemoryGetWin32HandleInfoKHR handle_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR,
        .memory = d->memory,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT_KHR
    };
    VK_CHECK(vkGetMemoryWin32HandleKHR(r->device, &handle_info, &d->handle));

    glCreateMemoryObjectsEXT(1, &d->gl_memory_obj);
    glImportMemoryWin32HandleEXT(d->gl_memory_obj, memory_requirements.size, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, d->handle);
    assert(glGetError() == GL_NO_ERROR);

#else

    VkMemoryGetFdInfoKHR fd_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
        .memory = d->memory,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
    };
    VK_CHECK(vkGetMemoryFdKHR(r->device, &fd_info, &d->fd));

    glCreateMemoryObjectsEXT(1, &d->gl_memory_obj);
    glImportMemoryFdEXT(d->gl_memory_obj, memory_requirements.size,
                        GL_HANDLE_TYPE_OPAQUE_FD_EXT, d->fd);
    assert(glIsMemoryObjectEXT(d->gl_memory_obj));
    assert(glGetError() == GL_NO_ERROR);

#endif // WIN32

    glGenTextures(1, &d->gl_texture_id);
    glBindTexture(GL_TEXTURE_2D, d->gl_texture_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_TILING_EXT,
                    use_optimal_tiling ? GL_OPTIMAL_TILING_EXT :
                                         GL_LINEAR_TILING_EXT);
    glTexStorageMem2DEXT(GL_TEXTURE_2D, 1, gl_internal_format,
                         image_create_info.extent.width,
                         image_create_info.extent.height, d->gl_memory_obj, 0);
    assert(glGetError() == GL_NO_ERROR);

#endif // HAVE_EXTERNAL_MEMORY

    d->width = image_create_info.extent.width;
    d->height = image_create_info.extent.height;

    create_frame_buffer(pg);
}

/* S2 async present §5.1(1): create one slot's ping-pong exported display
 * image + view + framebuffer + GL memory-object/texture import. A faithful
 * per-slot duplicate of create_display_image's export/import sequence
 * (deliberately NOT a refactor of that function: the singleton path is the
 * shipping default and stays byte-identical). Runs on the pfifo thread from
 * within the sync — the same thread/context the singleton import already
 * uses. Only reached under HAVE_EXTERNAL_MEMORY (ensure_present_slots). */
static void create_present_slot_image(PGRAPHState *pg, PresentSlot *slot,
                                      int width, int height)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    assert(slot->image == VK_NULL_HANDLE);

    const GLint gl_internal_format = GL_RGBA8;
    bool use_optimal_tiling = true;

#if HAVE_EXTERNAL_MEMORY
    GLint num_tiling_types;
    glGetInternalformativ(GL_TEXTURE_2D, gl_internal_format,
                          GL_NUM_TILING_TYPES_EXT, 1, &num_tiling_types);
    /* Same probe as create_display_image: default to optimal unless linear
     * is explicitly available (see the AMD/nVidia note there). */
    GLint tiling_types[num_tiling_types];
    glGetInternalformativ(GL_TEXTURE_2D, gl_internal_format,
                          GL_TILING_TYPES_EXT, num_tiling_types, tiling_types);
    for (int i = 0; i < num_tiling_types; i++) {
        if (tiling_types[i] == GL_LINEAR_TILING_EXT) {
            use_optimal_tiling = false;
            break;
        }
    }
#endif

    VkImageCreateInfo image_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .extent.width = width,
        .extent.height = height,
        .extent.depth = 1,
        .mipLevels = 1,
        .arrayLayers = 1,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .tiling = use_optimal_tiling ? VK_IMAGE_TILING_OPTIMAL :
                                       VK_IMAGE_TILING_LINEAR,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        /* TRANSFER_SRC: the VR compositor copies the current slot's image
         * into the XR swapchain (vr_xr_compositor.c). */
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };

    VkExternalMemoryImageCreateInfo external_memory_image_create_info = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
#ifdef WIN32
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT,
#else
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR,
#endif
    };
    image_create_info.pNext = &external_memory_image_create_info;

    VK_CHECK(vkCreateImage(r->device, &image_create_info, NULL, &slot->image));

    VkMemoryRequirements memory_requirements;
    vkGetImageMemoryRequirements(r->device, slot->image, &memory_requirements);

    VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memory_requirements.size,
        .memoryTypeIndex =
            pgraph_vk_get_memory_type(pg, memory_requirements.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };

    VkExportMemoryAllocateInfo export_memory_alloc_info = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .handleTypes =
#ifdef WIN32
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT_KHR
#else
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT
#endif
            ,
    };
    alloc_info.pNext = &export_memory_alloc_info;

    VK_CHECK(vkAllocateMemory(r->device, &alloc_info, NULL, &slot->memory));
    VK_CHECK(vkBindImageMemory(r->device, slot->image, slot->memory, 0));

    VkImageViewCreateInfo image_view_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = slot->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = image_create_info.format,
        .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .subresourceRange.levelCount = 1,
        .subresourceRange.layerCount = 1,
    };
    VK_CHECK(vkCreateImageView(r->device, &image_view_create_info, NULL,
                               &slot->image_view));

#if HAVE_EXTERNAL_MEMORY

#ifdef WIN32

    VkMemoryGetWin32HandleInfoKHR handle_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR,
        .memory = slot->memory,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT_KHR
    };
    VK_CHECK(vkGetMemoryWin32HandleKHR(r->device, &handle_info,
                                       &slot->handle));

    glCreateMemoryObjectsEXT(1, &slot->gl_memory_obj);
    glImportMemoryWin32HandleEXT(slot->gl_memory_obj,
                                 memory_requirements.size,
                                 GL_HANDLE_TYPE_OPAQUE_WIN32_EXT,
                                 slot->handle);
    assert(glGetError() == GL_NO_ERROR);

#else

    VkMemoryGetFdInfoKHR fd_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
        .memory = slot->memory,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
    };
    VK_CHECK(vkGetMemoryFdKHR(r->device, &fd_info, &slot->fd));

    glCreateMemoryObjectsEXT(1, &slot->gl_memory_obj);
    glImportMemoryFdEXT(slot->gl_memory_obj, memory_requirements.size,
                        GL_HANDLE_TYPE_OPAQUE_FD_EXT, slot->fd);
    assert(glIsMemoryObjectEXT(slot->gl_memory_obj));
    assert(glGetError() == GL_NO_ERROR);

#endif // WIN32

    glGenTextures(1, &slot->gl_texture_id);
    glBindTexture(GL_TEXTURE_2D, slot->gl_texture_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_TILING_EXT,
                    use_optimal_tiling ? GL_OPTIMAL_TILING_EXT :
                                         GL_LINEAR_TILING_EXT);
    glTexStorageMem2DEXT(GL_TEXTURE_2D, 1, gl_internal_format,
                         image_create_info.extent.width,
                         image_create_info.extent.height,
                         slot->gl_memory_obj, 0);
    assert(glGetError() == GL_NO_ERROR);

#endif // HAVE_EXTERNAL_MEMORY

    VkFramebufferCreateInfo fb_create_info = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = r->display.render_pass,
        .attachmentCount = 1,
        .pAttachments = &slot->image_view,
        .width = width,
        .height = height,
        .layers = 1,
    };
    VK_CHECK(vkCreateFramebuffer(r->device, &fb_create_info, NULL,
                                 &slot->framebuffer));

    slot->width = width;
    slot->height = height;
}

/* Make every present slot's image resources exist at the current display
 * size. Returns false when the async path cannot run (no GL interop build,
 * or no singleton display image yet) — the caller falls back to the
 * synchronous composite for this frame. Normally a no-op after the first
 * async composite per size: resize destroys the slot images together with
 * the singleton (destroy_current_display_image), and this recreates them. */
static bool ensure_present_slots(PGRAPHState *pg)
{
#if HAVE_EXTERNAL_MEMORY
    PGRAPHVkState *r = pg->vk_renderer_state;
    PGRAPHVkDisplayState *disp = &r->display;

    if (disp->image == VK_NULL_HANDLE || disp->width <= 0 ||
        disp->height <= 0) {
        return false;
    }

    for (int i = 0; i < ASYNC_PRESENT_RING_SIZE; i++) {
        PresentSlot *slot = &disp->present_slots[i];
        if (slot->image != VK_NULL_HANDLE && slot->width == disp->width &&
            slot->height == disp->height) {
            continue;
        }
        if (slot->image != VK_NULL_HANDLE) {
            /* Defensive: a stale-size slot outside the resize path. Retire
             * this slot's submission before destroying (no reset — reuse
             * owns the reset). */
            if (slot->fence_submitted &&
                vkGetFenceStatus(r->device, slot->fence) != VK_SUCCESS) {
                VK_CHECK(vkWaitForFences(r->device, 1, &slot->fence, VK_TRUE,
                                         UINT64_MAX));
            }
            destroy_present_slot_image(pg, slot);
        }
        create_present_slot_image(pg, slot, disp->width, disp->height);
    }
    return true;
#else
    return false;
#endif
}

/* Write the display bindings into `dst_set`. The synchronous path passes the
 * singleton r->display.descriptor_set (behavior unchanged); the async path
 * passes the acquired slot's set, whose prior use retired at slot acquire —
 * never an update-while-in-use (§5.3). */
static void update_descriptor_set_for(PGRAPHState *pg, SurfaceBinding *surface,
                                      VkDescriptorSet dst_set)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkDescriptorImageInfo image_infos[2];
    VkWriteDescriptorSet descriptor_writes[2];

    // Display surface
    image_infos[0] = (VkDescriptorImageInfo){
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .imageView = surface->image_view,
        .sampler = r->display.sampler,
    };
    descriptor_writes[0] = (VkWriteDescriptorSet){
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = dst_set,
        .dstBinding = 0,
        .dstArrayElement = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .pImageInfo = &image_infos[0],
    };

    // FIXME: PVIDEO Overlay
    if (r->display.pvideo.state.enabled) {
        assert(r->display.pvideo.image_view != VK_NULL_HANDLE);
        assert(r->display.pvideo.sampler != VK_NULL_HANDLE);
        image_infos[1] = (VkDescriptorImageInfo){
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .imageView = r->display.pvideo.image_view,
            .sampler = r->display.pvideo.sampler,
        };
    } else {
        image_infos[1] = (VkDescriptorImageInfo){
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .imageView = r->dummy_texture.image_view,
            .sampler = r->dummy_texture.sampler,
        };
    }
    descriptor_writes[1] = (VkWriteDescriptorSet){
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = dst_set,
        .dstBinding = 1,
        .dstArrayElement = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .pImageInfo = &image_infos[1],
    };

    vkUpdateDescriptorSets(r->device, ARRAY_SIZE(descriptor_writes),
                           descriptor_writes, 0, NULL);
}

static void update_descriptor_set(PGRAPHState *pg, SurfaceBinding *surface)
{
    update_descriptor_set_for(pg, surface,
                              pg->vk_renderer_state->display.descriptor_set);
}

static PvideoState get_pvideo_state(PGRAPHState *pg)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PvideoState state;

    // FIXME: This check against PVIDEO_SIZE_IN does not match HW behavior.
    // Many games seem to pass this value when initializing or tearing down
    // PVIDEO. On its own, this generally does not result in the overlay being
    // hidden, however there are certain games (e.g., Ultimate Beach Soccer)
    // that use an unknown mechanism to hide the overlay without explicitly
    // stopping it.
    // Since the value seems to be set to 0xFFFFFFFF only in cases where the
    // content is not valid, it is probably good enough to treat it as an
    // implicit stop.
    state.enabled = (d->pvideo.regs[NV_PVIDEO_BUFFER] & NV_PVIDEO_BUFFER_0_USE)
        && d->pvideo.regs[NV_PVIDEO_SIZE_IN] != 0xFFFFFFFF;
    if (!state.enabled) {
        return state;
    }

    state.base = d->pvideo.regs[NV_PVIDEO_BASE];
    state.limit = d->pvideo.regs[NV_PVIDEO_LIMIT];
    state.offset = d->pvideo.regs[NV_PVIDEO_OFFSET];

    state.pitch =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_FORMAT], NV_PVIDEO_FORMAT_PITCH);
    state.format =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_FORMAT], NV_PVIDEO_FORMAT_COLOR);

    /* TODO: support other color formats */
    assert(state.format == NV_PVIDEO_FORMAT_COLOR_LE_CR8YB8CB8YA8);

    state.in_width =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_SIZE_IN], NV_PVIDEO_SIZE_IN_WIDTH);
    state.in_height =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_SIZE_IN], NV_PVIDEO_SIZE_IN_HEIGHT);

    state.out_width =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_SIZE_OUT], NV_PVIDEO_SIZE_OUT_WIDTH);
    state.out_height =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_SIZE_OUT], NV_PVIDEO_SIZE_OUT_HEIGHT);

    state.in_s = GET_MASK(d->pvideo.regs[NV_PVIDEO_POINT_IN],
                        NV_PVIDEO_POINT_IN_S);
    state.in_t = GET_MASK(d->pvideo.regs[NV_PVIDEO_POINT_IN],
                        NV_PVIDEO_POINT_IN_T);

    uint32_t ds_dx = d->pvideo.regs[NV_PVIDEO_DS_DX];
    uint32_t dt_dy = d->pvideo.regs[NV_PVIDEO_DT_DY];
    state.scale_x = ds_dx == NV_PVIDEO_DIN_DOUT_UNITY ?
                        1.0f :
                        pvideo_calculate_scale(ds_dx, state.out_width);
    state.scale_y = dt_dy == NV_PVIDEO_DIN_DOUT_UNITY ?
                        1.0f :
                        pvideo_calculate_scale(dt_dy, state.out_height);

    // On HW, setting NV_PVIDEO_SIZE_IN larger than NV_PVIDEO_SIZE_OUT results
    // in them being capped to the output size, content is not scaled. This is
    // particularly important as NV_PVIDEO_SIZE_IN may be set to 0xFFFFFFFF
    // during initialization or teardown.
    if (state.in_width > state.out_width) {
        state.in_width = floorf((float)state.out_width * state.scale_x + 0.5f);
    }
    if (state.in_height > state.out_height) {
        state.in_height = floorf((float)state.out_height * state.scale_y + 0.5f);
    }

    state.out_x =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_POINT_OUT], NV_PVIDEO_POINT_OUT_X);
    state.out_y =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_POINT_OUT], NV_PVIDEO_POINT_OUT_Y);

    state.color_key_enabled =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_FORMAT], NV_PVIDEO_FORMAT_DISPLAY);

    // Note: PVIDEO color keying ignores alpha.
    state.color_key = d->pvideo.regs[NV_PVIDEO_COLOR_KEY] & 0xFFFFFF;

    assert(state.offset + state.pitch * state.in_height <= state.limit);
    hwaddr end = state.base + state.offset + state.pitch * state.in_height;
    assert(end <= memory_region_size(d->vram));

    return state;
}

static void update_uniforms(PGRAPHState *pg, SurfaceBinding *surface)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHVkState *r = pg->vk_renderer_state;
    ShaderUniformLayout *l = &r->display.display_frag->push_constants;

    int display_size_loc = uniform_index(l, "display_size");  // FIXME: Cache
    uniform2f(l, display_size_loc, r->display.width, r->display.height);

    VGADisplayParams vga_display_params;
    d->vga.get_params(&d->vga, &vga_display_params);
    int line_offset = vga_display_params.line_offset ?
                          surface->pitch / vga_display_params.line_offset :
                          1;
    int line_offset_loc = uniform_index(l, "line_offset");
    uniform1f(l, line_offset_loc, line_offset);

    PvideoState *pvideo = &r->display.pvideo.state;
    uniform1i(l, uniform_index(l, "pvideo_enable"), pvideo->enabled);
    if (pvideo->enabled) {
        uniform1i(l, uniform_index(l, "pvideo_color_key_enable"),
                  pvideo->color_key_enabled);
        uniform3f(
            l, uniform_index(l, "pvideo_color_key"),
            GET_MASK(pvideo->color_key, NV_PVIDEO_COLOR_KEY_RED) / 255.0,
            GET_MASK(pvideo->color_key, NV_PVIDEO_COLOR_KEY_GREEN) / 255.0,
            GET_MASK(pvideo->color_key, NV_PVIDEO_COLOR_KEY_BLUE) / 255.0);
        uniform2f(l, uniform_index(l, "pvideo_in_pos"), pvideo->in_s / 16.f,
                  pvideo->in_t / 8.f);
        uniform4f(l, uniform_index(l, "pvideo_pos"), pvideo->out_x,
                  pvideo->out_y, pvideo->out_width, pvideo->out_height);
        uniform4f(l, uniform_index(l, "pvideo_scale"), pvideo->scale_x,
                  pvideo->scale_y, 1.0f / pg->surface_scale_factor, 1.0);
    }
}

/* S2 ASYNC PRESENT (overlap-present-path.md §5.1, points 2-5): record the
 * composite into the acquired slot's dedicated CB targeting the slot's
 * ping-pong exported image, submit with the slot's fence, and return WITHOUT
 * any host wait — replacing the PRESENTING finish that was the measured
 * 6.6 ms present_composite wall. Correctness in place of that wait:
 *   - the open batch holding this frame's draws is submitted first (below);
 *     the single in-order queue makes the frame visible to the composite by
 *     submission order alone (the VR copy ring's argument,
 *     vr_xr_compositor.c:290-293);
 *   - the display thread fence-gates on the published fence before sampling
 *     the imported GL texture (pgraph_vk_get_framebuffer_surface) —
 *     preserving the exact CPU-complete-before-GL-sample interop contract
 *     (there is no VK<->GL semaphore in this codebase, by design);
 *   - the slot's fence is waited/reset only at slot REUSE, which the
 *     serialized gl_render_frame handshake orders after all display-side
 *     use of that slot — no reset-while-waited;
 *   - the per-slot descriptor set was last used by a submission that
 *     retired at slot acquire — no update-while-in-use;
 *   - destruction paths wait the ring (pgraph_vk_wait_all_present_slots).
 * pvideo frames and !TCG never reach here (fallbacks in render_display /
 * the toggle). Guest flip pacing (READ_3D/vblank) is untouched. */
static void render_display_async(PGRAPHState *pg, SurfaceBinding *surface)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHVkState *r = pg->vk_renderer_state;
    PGRAPHVkDisplayState *disp = &r->display;

    if (r->in_command_buffer &&
        surface->draw_time >= r->command_buffer_start_time) {
        /* Submit the open batch carrying this surface's draws — NO wait. */
        pgraph_vk_submit_batch(pg, VK_FINISH_REASON_PRESENTING);
    }

    /* The toggle pins async present to TCG, so this force flag is always
     * false here — kept spelled like the synchronous path so the two sites
     * can never diverge if the gate is ever relaxed. A pending guest-CPU
     * framebuffer upload still runs its blocking single-time copy (rare;
     * queue-ordered before the slot CB below). WAR guard: that upload
     * overwrites the scanout image, which the PREVIOUS frame's in-flight
     * present CB may still be sampling — a reader the surface
     * last_use_submit stamps do not cover. Retire the ring first (rare
     * path: upload_pending on a GPU-rendered scanout is ~never in-scene). */
    if (surface->upload_pending) {
        pgraph_vk_wait_all_present_slots(pg);
    }
    pgraph_vk_upload_surface_data(d, surface, !tcg_enabled());

    update_uniforms(pg, surface);

    /* Slot acquire (§5.1(3)): wait the fence only if this slot's submission
     * from ASYNC_PRESENT_RING_SIZE presents ago is still unretired; reset
     * only here, at reuse (the VR copy-ring pattern). */
    PresentSlot *slot =
        &disp->present_slots[disp->present_count % ASYNC_PRESENT_RING_SIZE];
    disp->present_count++;
    if (slot->fence_submitted) {
        if (vkGetFenceStatus(r->device, slot->fence) != VK_SUCCESS) {
            nv2a_profile_inc_counter(NV2A_PROF_ASYNC_PRESENT_REUSE_WAIT);
            VK_CHECK(vkWaitForFences(r->device, 1, &slot->fence, VK_TRUE,
                                     UINT64_MAX));
        }
        VK_CHECK(vkResetFences(r->device, 1, &slot->fence));
        slot->fence_submitted = false;
    }

    /* Safe to rewrite: this slot's prior composite retired above. pvideo is
     * disabled on this path, so binding 1 gets the dummy texture. */
    update_descriptor_set_for(pg, surface, slot->desc_set);

    /* Record the same composite the synchronous path records (barriers,
     * layouts, pass, draw), with disp->image/framebuffer/descriptor_set
     * replaced by the slot's. The pool has RESET_COMMAND_BUFFER_BIT:
     * beginning implicitly resets the CB. */
    VkCommandBuffer cmd = slot->cb;
    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VK_CHECK(vkBeginCommandBuffer(cmd, &begin_info));

    pgraph_vk_begin_debug_marker(r, cmd, RGBA_YELLOW,
        "Async Display Surface %08"HWADDR_PRIx, surface->vram_addr);

    pgraph_vk_transition_image_layout(pg, cmd, surface->image,
                                      surface->host_fmt.vk_format,
                                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    pgraph_vk_transition_image_layout(
        pg, cmd, slot->image, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    VkRenderPassBeginInfo render_pass_begin_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = disp->render_pass,
        .framebuffer = slot->framebuffer,
        .renderArea.extent.width = disp->width,
        .renderArea.extent.height = disp->height,
    };
    vkCmdBeginRenderPass(cmd, &render_pass_begin_info,
                         VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, disp->pipeline);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            disp->pipeline_layout, 0, 1, &slot->desc_set,
                            0, NULL);

    VkViewport viewport = {
        .width = disp->width,
        .height = disp->height,
        .minDepth = 0.0,
        .maxDepth = 1.0,
    };
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor = {
        .extent.width = disp->width,
        .extent.height = disp->height,
    };
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdPushConstants(cmd, disp->pipeline_layout,
                       VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       disp->display_frag->push_constants.total_size,
                       disp->display_frag->push_constants.allocation);

    vkCmdDraw(cmd, 3, 1, 0, 0);

    vkCmdEndRenderPass(cmd);

    pgraph_vk_transition_image_layout(pg, cmd, surface->image,
                                      surface->host_fmt.vk_format,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    pgraph_vk_transition_image_layout(pg, cmd, slot->image,
                                      VK_FORMAT_R8G8B8A8_UNORM,
                                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    pgraph_vk_end_debug_marker(r, cmd);
    VK_CHECK(vkEndCommandBuffer(cmd));

    /* Submit with the slot's fence — no host wait (§5.1(4)). The pfifo
     * thread owns the queue here (externally synchronized by construction,
     * same as every other submit site). */
    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    VK_CHECK(vkQueueSubmit(r->queue, 1, &submit_info, slot->fence));
    slot->fence_submitted = true;
    nv2a_profile_inc_counter(NV2A_PROF_ASYNC_PRESENT_SUBMIT);
    nv2a_profile_inc_counter(NV2A_PROF_QUEUE_SUBMIT_5);

    /* Publish (§5.1(5)) for (a) the display thread's fence gate + GL sample
     * and (b) the VR copy that runs next in this same pfifo sync. Plain
     * stores: qemu_event_set(sync_complete) after we return provides the
     * release/acquire ordering for the display thread. */
    disp->present_published_image = slot->image;
    disp->present_published_fence = slot->fence;
    disp->present_published_gl_id = slot->gl_texture_id;
    disp->present_published_valid = true;

    disp->draw_time = surface->draw_time;
}

static void render_display(PGRAPHState *pg, SurfaceBinding *surface)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHVkState *r = pg->vk_renderer_state;
    PGRAPHVkDisplayState *disp = &r->display;

    /* S2 async present: assume this composite goes the synchronous path;
     * render_display_async republishes at its end. Consumers only read
     * these after this sync completes. */
    disp->present_published_valid = false;

    bool async_candidate = pgraph_vk_async_present_enabled();
    bool pvideo_state_fetched = false;
    if (async_candidate) {
        /* pvideo overlay frames fall back to the synchronous path for the
         * frame (rare; correctness over cleverness): the overlay upload is
         * a blocking single-time op wired to the singleton composite. */
        disp->pvideo.state = get_pvideo_state(pg);
        pvideo_state_fetched = true;
        if (!disp->pvideo.state.enabled && ensure_present_slots(pg)) {
            render_display_async(pg, surface);
            return;
        }
    }

    if (r->in_command_buffer &&
        surface->draw_time >= r->command_buffer_start_time) {
        if (pgraph_vk_narrowfence_enabled()) {
            /* narrow-fence ON: submit the batch holding this surface's
             * draws (no drain) and wait only its write fence — the
             * present path's once-per-flip whole-pipeline stall becomes a
             * targeted wait. */
            pgraph_vk_submit_batch(pg, VK_FINISH_REASON_PRESENTING);
            pgraph_vk_wait_for_surface_write(pg, surface);
        } else {
            pgraph_vk_finish(pg, VK_FINISH_REASON_PRESENTING);
        }
    }

    pgraph_vk_upload_surface_data(d, surface, !tcg_enabled());

    if (!pvideo_state_fetched) {
        disp->pvideo.state = get_pvideo_state(pg);
    }
    if (disp->pvideo.state.enabled) {
        upload_pvideo_image(pg, disp->pvideo.state);
    }

    update_uniforms(pg, surface);
    update_descriptor_set(pg, surface);

    VkCommandBuffer cmd = pgraph_vk_begin_single_time_commands(pg);
    pgraph_vk_begin_debug_marker(r, cmd, RGBA_YELLOW,
        "Display Surface %08"HWADDR_PRIx, surface->vram_addr);

    pgraph_vk_transition_image_layout(pg, cmd, surface->image,
                                      surface->host_fmt.vk_format,
                                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    pgraph_vk_transition_image_layout(
        pg, cmd, disp->image, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    VkRenderPassBeginInfo render_pass_begin_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = disp->render_pass,
        .framebuffer = disp->framebuffer,
        .renderArea.extent.width = disp->width,
        .renderArea.extent.height = disp->height,
    };
    vkCmdBeginRenderPass(cmd, &render_pass_begin_info,
                         VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      disp->pipeline);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            disp->pipeline_layout, 0, 1, &disp->descriptor_set,
                            0, NULL);

    VkViewport viewport = {
        .width = disp->width,
        .height = disp->height,
        .minDepth = 0.0,
        .maxDepth = 1.0,
    };
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor = {
        .extent.width = disp->width,
        .extent.height = disp->height,
    };
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdPushConstants(cmd, disp->pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, disp->display_frag->push_constants.total_size,
                       disp->display_frag->push_constants.allocation);

    vkCmdDraw(cmd, 3, 1, 0, 0);

    vkCmdEndRenderPass(cmd);

#if 0
    VkImageCopy region = {
        .srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .srcSubresource.layerCount = 1,
        .dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .dstSubresource.layerCount = 1,
        .extent.width = surface->width,
        .extent.height = surface->height,
        .extent.depth = 1,
    };
    pgraph_apply_scaling_factor(pg, &region.extent.width,
                                &region.extent.height);

    vkCmdCopyImage(cmd, surface->image,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, disp->image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
#endif

    pgraph_vk_transition_image_layout(pg, cmd, surface->image,
                                      surface->host_fmt.vk_format,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    pgraph_vk_transition_image_layout(pg, cmd, disp->image,
                                      VK_FORMAT_R8G8B8_UNORM,
                                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    pgraph_vk_end_debug_marker(r, cmd);
    pgraph_vk_end_single_time_commands(pg, cmd);
    nv2a_profile_inc_counter(NV2A_PROF_QUEUE_SUBMIT_5);

    disp->draw_time = surface->draw_time;
}

static void create_surface_sampler(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

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

    VK_CHECK(vkCreateSampler(r->device, &sampler_create_info, NULL,
                             &r->display.sampler));
}

static void destroy_surface_sampler(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkDestroySampler(r->device, r->display.sampler, NULL);
    r->display.sampler = VK_NULL_HANDLE;
}

/* S2 async present: per-slot dedicated command buffer + fence, allocated at
 * init (like the cb_fence_ring: the structures always exist; behavior is
 * gated). NOT the shared single_time CB — the whole point is a composite
 * that can stay in flight while single-time ops keep their own CB/fence. */
static void create_present_slots(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    PGRAPHVkDisplayState *disp = &r->display;

    VkCommandBuffer cbs[ASYNC_PRESENT_RING_SIZE];
    VkCommandBufferAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = r->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = ASYNC_PRESENT_RING_SIZE,
    };
    VK_CHECK(vkAllocateCommandBuffers(r->device, &alloc_info, cbs));

    VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    };
    for (int i = 0; i < ASYNC_PRESENT_RING_SIZE; i++) {
        PresentSlot *slot = &disp->present_slots[i];
        slot->cb = cbs[i];
        VK_CHECK(vkCreateFence(r->device, &fence_info, NULL, &slot->fence));
        slot->fence_submitted = false;
    }
    disp->present_count = 0;
    disp->present_published_valid = false;
}

/* Caller must have retired the ring (pgraph_vk_wait_all_present_slots).
 * Slot images are destroyed with the singleton display image; this frees
 * only the CB/fence pairs (desc_sets die with the pool). */
static void destroy_present_slots(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    PGRAPHVkDisplayState *disp = &r->display;

    for (int i = 0; i < ASYNC_PRESENT_RING_SIZE; i++) {
        PresentSlot *slot = &disp->present_slots[i];
        /* Belt and braces: images are normally already gone via
         * destroy_current_display_image (idempotent). */
        destroy_present_slot_image(pg, slot);
        if (slot->fence != VK_NULL_HANDLE) {
            vkDestroyFence(r->device, slot->fence, NULL);
            slot->fence = VK_NULL_HANDLE;
        }
        if (slot->cb != VK_NULL_HANDLE) {
            vkFreeCommandBuffers(r->device, r->command_pool, 1, &slot->cb);
            slot->cb = VK_NULL_HANDLE;
        }
        slot->fence_submitted = false;
    }
    disp->present_published_valid = false;
}

void pgraph_vk_init_display(PGRAPHState *pg)
{
    create_descriptor_pool(pg);
    create_descriptor_set_layout(pg);
    create_descriptor_sets(pg);
    create_render_pass(pg);
    create_display_pipeline(pg);
    create_surface_sampler(pg);
    create_present_slots(pg);
}

void pgraph_vk_finalize_display(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    /* S2 async present §5.1(7): retire any in-flight composite before its
     * resources are destroyed below. */
    pgraph_vk_wait_all_present_slots(pg);

    destroy_pvideo_image(pg);

    if (r->display.image != VK_NULL_HANDLE) {
        destroy_current_display_image(pg);
    }

    destroy_present_slots(pg);
    destroy_surface_sampler(pg);
    destroy_display_pipeline(pg);
    destroy_render_pass(pg);
    destroy_descriptor_set_layout(pg);
    destroy_descriptor_pool(pg);
}

void pgraph_vk_render_display(PGRAPHState *pg)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHVkState *r = pg->vk_renderer_state;

    VGADisplayParams vga_display_params;
    d->vga.get_params(&d->vga, &vga_display_params);

    SurfaceBinding *surface = pgraph_vk_surface_get_within(
        d, d->pcrtc.start + vga_display_params.line_offset);
    if (surface == NULL || !surface->color || !surface->width ||
        !surface->height) {
        return;
    }

    unsigned int width = 0, height = 0;
    d->vga.get_resolution(&d->vga, (int *)&width, (int *)&height);

    /* Adjust viewport height for interlaced mode, used only in 1080i */
    if (d->vga.cr[NV_PRMCIO_INTERLACE_MODE] != NV_PRMCIO_INTERLACE_MODE_DISABLED) {
        height *= 2;
    }

    pgraph_apply_scaling_factor(pg, &width, &height);

    PGRAPHVkDisplayState *disp = &r->display;
    if (!disp->image || disp->width != width || disp->height != height) {
        create_display_image(pg, width, height);
    }

    render_display(pg, surface);
}
