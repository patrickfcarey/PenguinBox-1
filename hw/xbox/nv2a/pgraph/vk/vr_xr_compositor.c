/*
 * xemu-VR — XR frame pacing, swapchain, copy, quad layer
 *
 * SPDX-FileCopyrightText: 2026 Patrick Carey
 * SPDX-License-Identifier: LGPL-2.0-or-later
 *
 * C port of the team's PCSX2-VR XRCompositor.cpp (docs/vr/01-port-map.md §6;
 * logic verbatim, std::thread/mutex/atomic → qemu_thread/QemuMutex/qatomic,
 * GSTextureVK* → r->display.image, Console → fprintf, xr* via xr_pfn).
 *
 * Threading (port map §3, reproduced exactly): everything here runs on the
 * PFIFO thread EXCEPT the pacer thread, whose sole OpenXR call is
 * xrWaitFrame (spec-sanctioned). The pacer publishes the resulting
 * XrFrameState into a one-slot mailbox; vr_compositor_frame (pfifo thread)
 * consumes it and NEVER blocks — xrWaitFrame's "blocks until the previous
 * frame is begun" semantics ARE the handshake; no condition variables.
 * begin_owed + wait_pending + the shutdown drain encode real runtime
 * edge-cases — do not simplify them away.
 *
 * Copy contract (port map §5 + delta D3): the source is r->display.image,
 * which render_display leaves in SHADER_READ_ONLY_OPTIMAL (display.c
 * transitions it COLOR_ATTACHMENT→SHADER_READ_ONLY after its render pass;
 * next frame it transitions from UNDEFINED, i.e. discards). Unlike PCSX2 —
 * whose GSTextureVK layout tracker re-transitions lazily on next use — xemu
 * has no tracker AND the GL mirror samples this image right after our hook
 * (sync_complete → UI thread), so we barrier it back to
 * SHADER_READ_ONLY_OPTIMAL after the copy. Same queue + same thread as
 * render_display's (blocking) submit, so submission order makes the frame
 * visible to the copy with no extra semaphores. UNORM source bytes are
 * raw-copied into the R8G8B8A8_SRGB swapchain image: the "gamma-encoded
 * bytes, encoded once" rule — headset brightness matches the desktop.
 *
 * Aspect (delta D8): source (== display image) extent ratio. The display
 * image is already the scaled VGA-resolution output; anamorphic/widescreen
 * refinement is tracked as plan U7.
 *
 * EnsureFrameSubmitted is intentionally NOT ported (port map §5): the hook
 * site runs immediately after render_display submits on the same queue.
 */

#include "qemu/osdep.h"

#include <math.h>

#include "ui/xemu-settings.h"
#include "renderer.h"
#include "vr.h"
#include "vr_internal.h"
#include "vr_sbc_panel.h" /* panel content contract: VR_SBC_PANEL_W/H + frame() */

#define ONE_SECOND_NS 1000000000LL

typedef enum VRCopyResult {
    /* image copied + released; the layer shows the fresh frame */
    VR_COPY_COPIED,
    /* could not update this frame; fall back to the last released image */
    VR_COPY_SKIPPED,
    /* acquired image not touched/released (re-waited next visit); submit
     * zero layers this frame */
    VR_COPY_TIMEOUT_ZERO_LAYER,
} VRCopyResult;

/* Mailbox guard. Init-once for the process lifetime: cheap, and removes any
 * destroy/re-init race on renderer switches. */
static QemuMutex vr_frame_mutex;
static bool vr_frame_mutex_inited;

static bool check_xr(XrResult res, const char *what)
{
    if (XR_SUCCEEDED(res)) {
        return true;
    }

    char buf[XR_MAX_RESULT_STRING_SIZE] = "?";
    if (g_xemu_vr.xr.instance != XR_NULL_HANDLE &&
        xr_pfn.ResultToString != NULL) {
        xr_pfn.ResultToString(g_xemu_vr.xr.instance, res, buf);
    }
    fprintf(stderr, "xemu-vr: %s failed: %s (%d)\n", what, buf, (int)res);
    return false;
}

static void image_barrier(VkCommandBuffer cmd, VkImage image,
                          VkImageLayout old_layout, VkImageLayout new_layout,
                          VkAccessFlags src_access, VkAccessFlags dst_access,
                          VkPipelineStageFlags src_stage,
                          VkPipelineStageFlags dst_stage)
{
    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1,
                         &barrier);
}

/* Hamilton product a*b (unit quaternions): the composition that applies b
 * first, then a. Used to tilt the console panel about X after applying the
 * yaw-only anchor orientation (q = anchor_yaw * pitch). Field order is
 * XrQuaternionf {x, y, z, w}. */
static XrQuaternionf quat_mul(XrQuaternionf a, XrQuaternionf b)
{
    return (XrQuaternionf){
        .x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        .y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        .z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        .w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

/* Wait for (and reset) any in-flight copy fences. pfifo thread; device must
 * still be alive. */
static void wait_all_fences(VRCompositor *c)
{
    for (uint32_t i = 0; i < VR_COMP_NUM_CMD_BUFFERS; i++) {
        if (!c->fence_submitted[i]) {
            continue;
        }

        VkResult res =
            vkWaitForFences(c->device, 1, &c->fences[i], VK_TRUE,
                            ONE_SECOND_NS);
        if (res == VK_TIMEOUT) {
            fprintf(stderr, "xemu-vr: copy fence %u still busy after 1s "
                            "during teardown.\n", i);
        }
        vkResetFences(c->device, 1, &c->fences[i]);
        c->fence_submitted[i] = false;
    }
}

static void destroy_swapchains(VRCompositor *c)
{
    /* Nothing may reference the images once a swapchain is gone. */
    wait_all_fences(c);
    for (int e = 0; e < 2; e++) {
        VREyeChain *chain = &c->chains[e];
        if (chain->swapchain != XR_NULL_HANDLE) {
            xr_pfn.DestroySwapchain(chain->swapchain);
            chain->swapchain = XR_NULL_HANDLE;
        }
        g_free(chain->images);
        chain->images = NULL;
        chain->num_images = 0;
        chain->ever_released = false;
        chain->wait_pending = false; /* outstanding acquires die with it */
    }
    c->swapchain_width = 0;
    c->swapchain_height = 0;
    g_free(c->flip_regions);
    c->flip_regions = NULL;
    c->flip_regions_count = 0;
}

static bool create_chain(VRCompositor *c, uint32_t eye, uint32_t w,
                         uint32_t h)
{
    VREyeChain *chain = &c->chains[eye];

    XrSwapchainCreateInfo ci = {
        .type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
        .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                      XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT,
        .format = (int64_t)VK_FORMAT_R8G8B8A8_SRGB,
        .sampleCount = 1,
        .width = w,
        .height = h,
        .faceCount = 1,
        .arraySize = 1,
        .mipCount = 1,
    };

    XrResult res = xr_pfn.CreateSwapchain(g_xemu_vr.xr.session, &ci,
                                          &chain->swapchain);
    if (XR_FAILED(res)) {
        chain->swapchain = XR_NULL_HANDLE;
        if (!c->warned_swapchain) {
            c->warned_swapchain = true;
            fprintf(stderr, "xemu-vr: xrCreateSwapchain failed (%d) for "
                            "%ux%u; frames skipped until it succeeds.\n",
                    (int)res, w, h);
        }
        return false;
    }

    uint32_t count = 0;
    if (!check_xr(xr_pfn.EnumerateSwapchainImages(chain->swapchain, 0, &count,
                                                  NULL),
                  "xrEnumerateSwapchainImages") ||
        count == 0) {
        return false;
    }

    chain->images = g_new0(XrSwapchainImageVulkan2KHR, count);
    for (uint32_t i = 0; i < count; i++) {
        chain->images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR;
    }
    if (!check_xr(xr_pfn.EnumerateSwapchainImages(
                      chain->swapchain, count, &count,
                      (XrSwapchainImageBaseHeader *)chain->images),
                  "xrEnumerateSwapchainImages")) {
        return false;
    }
    chain->num_images = count;

    fprintf(stderr, "xemu-vr: XR swapchain created (eye %u): %ux%u, %u "
                    "images (VK_FORMAT_R8G8B8A8_SRGB).\n", eye, w, h, count);
    return true;
}

/* Ensure chains[0] exists and matches (w, h); recreate on size change.
 * Returns false (frame skipped) on failure without tearing down VR. */
static bool ensure_swapchains(VRCompositor *c, uint32_t w, uint32_t h)
{
    if (c->swapchain_width != 0 &&
        (c->swapchain_width != w || c->swapchain_height != h)) {
        destroy_swapchains(c);
    }

    if (c->chains[0].swapchain == XR_NULL_HANDLE) {
        if (!create_chain(c, 0, w, h)) {
            destroy_swapchains(c);
            return false;
        }
    }

    /* (Re)build the Y-flip region table for this size (see vr_internal.h:
     * flip_regions — GL-heritage rows copied bottom-up into the VK/XR
     * top-down swapchain image, raw bytes preserved). */
    if (c->flip_regions_count != h) {
        g_free(c->flip_regions);
        c->flip_regions = g_new0(VkImageCopy, h);
        c->flip_regions_count = h;
        for (uint32_t y = 0; y < h; y++) {
            c->flip_regions[y] = (VkImageCopy){
                .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                .srcOffset = { 0, (int32_t)y, 0 },
                .dstOffset = { 0, (int32_t)(h - 1 - y), 0 },
                .extent = { w, 1, 1 },
            };
        }
    }

    c->swapchain_width = w;
    c->swapchain_height = h;
    c->warned_swapchain = false; /* fresh warning if a later size flaps */
    return true;
}

/* Record + submit the copy of the display image into swapchain image `dst`.
 * Returns false if the command buffer could not be recorded/submitted. */
static bool record_and_submit_copy(VRCompositor *c, VkImage src, VkImage dst)
{
    const uint32_t i = c->next_cmd_buffer;
    c->next_cmd_buffer = (c->next_cmd_buffer + 1) % VR_COMP_NUM_CMD_BUFFERS;

    if (c->fence_submitted[i]) {
        VkResult wr = vkWaitForFences(c->device, 1, &c->fences[i], VK_TRUE,
                                      ONE_SECOND_NS);
        if (wr != VK_SUCCESS) {
            if (!c->warned_fence_timeout) {
                c->warned_fence_timeout = true;
                fprintf(stderr, "xemu-vr: copy fence wait returned %d; "
                                "skipping this frame's copy.\n", (int)wr);
            }
            return false;
        }
        vkResetFences(c->device, 1, &c->fences[i]);
        c->fence_submitted[i] = false;
    }

    VkCommandBuffer cmd = c->cmd_buffers[i];
    /* Pool has RESET_COMMAND_BUFFER_BIT: beginning implicitly resets. */
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VkResult vr = vkBeginCommandBuffer(cmd, &bi);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "xemu-vr: vkBeginCommandBuffer failed (%d).\n",
                (int)vr);
        return false;
    }

    /* Source: SHADER_READ_ONLY (render_display's final layout) →
     * TRANSFER_SRC. Its producing command buffer was already submitted on
     * this queue; this barrier's first scope includes it (same-queue
     * submission order), so the copy waits for it. */
    image_barrier(cmd, src,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT);

    /* Destination: COLOR_ATTACHMENT → TRANSFER_DST. Full-extent overwrite
     * makes pre-copy contents (and the first acquire's true UNDEFINED
     * layout) irrelevant. */
    image_barrier(cmd, dst,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                      VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
                  VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT);

    /* Vertical flip (F8): one single-row region per line, raw byte copy.
     * NVIDIA batches multi-region copies fine; ~480-720 regions is cheap. */
    vkCmdCopyImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   c->flip_regions_count, c->flip_regions);

    /* Destination back: TRANSFER_DST → COLOR_ATTACHMENT (the layout the
     * runtime's compositor expects). */
    image_barrier(cmd, dst,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    /* Source back to SHADER_READ_ONLY (delta D3): xemu has no layout
     * tracker, and the GL mirror samples this image as soon as the pfifo
     * sync completes. */
    image_barrier(cmd, src,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    vr = vkEndCommandBuffer(cmd);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "xemu-vr: vkEndCommandBuffer failed (%d).\n",
                (int)vr);
        return false;
    }

    /* The pfifo thread owns the graphics queue here (externally
     * synchronized by construction) — no additional locking. */
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    vr = vkQueueSubmit(c->queue, 1, &si, c->fences[i]);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "xemu-vr: vkQueueSubmit failed (%d).\n", (int)vr);
        return false;
    }
    c->fence_submitted[i] = true;
    return true;
}

static VRCopyResult copy_to_swapchain(VRCompositor *c, VkImage src,
                                      VREyeChain *chain)
{
    uint32_t index = 0;
    if (chain->wait_pending) {
        /* A previous visit acquired this image but its wait timed out or
         * failed. It is still acquired and the wait targets the same
         * oldest-unwaited image (spec) — RE-WAIT it rather than acquiring
         * another. */
        index = chain->pending_index;
    } else {
        XrSwapchainImageAcquireInfo ai = {
            .type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO,
        };
        XrResult res = xr_pfn.AcquireSwapchainImage(chain->swapchain, &ai,
                                                    &index);
        if (XR_FAILED(res)) {
            if (!c->warned_acquire) {
                c->warned_acquire = true;
                fprintf(stderr, "xemu-vr: xrAcquireSwapchainImage failed "
                                "(%d).\n", (int)res);
            }
            return VR_COPY_SKIPPED;
        }
        chain->pending_index = index;
        chain->wait_pending = true; /* acquired; cleared once a wait works */
    }

    XrSwapchainImageWaitInfo wi = {
        .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO,
        .timeout = ONE_SECOND_NS,
    };
    XrResult res = xr_pfn.WaitSwapchainImage(chain->swapchain, &wi);
    if (res == XR_TIMEOUT_EXPIRED) {
        /* Do NOT touch the image. Skip copy AND release, and submit a
         * zero-layer frame this visit; wait_pending stays set so the next
         * visit re-waits this same image. */
        if (!c->warned_wait_timeout) {
            c->warned_wait_timeout = true;
            fprintf(stderr, "xemu-vr: xrWaitSwapchainImage timed out; "
                            "skipping copy this frame.\n");
        }
        return VR_COPY_TIMEOUT_ZERO_LAYER;
    }
    if (XR_FAILED(res)) {
        /* Non-timeout wait failure: the image is not safely usable and must
         * not be released without a successful wait. It remains acquired
         * (wait_pending), so the next visit re-waits; a dead session tears
         * the chains down anyway. */
        if (!c->warned_wait_fail) {
            c->warned_wait_fail = true;
            fprintf(stderr, "xemu-vr: xrWaitSwapchainImage failed (%d).\n",
                    (int)res);
        }
        return VR_COPY_TIMEOUT_ZERO_LAYER;
    }
    chain->wait_pending = false; /* wait satisfied; image MUST be released */

    bool submitted =
        record_and_submit_copy(c, src, chain->images[index].image);

    /* The image was successfully waited on, so release it regardless of
     * whether our copy submission succeeded (else the acquire leaks). */
    XrSwapchainImageReleaseInfo ri = {
        .type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO,
    };
    res = xr_pfn.ReleaseSwapchainImage(chain->swapchain, &ri);
    if (XR_FAILED(res)) {
        if (!c->warned_release) {
            c->warned_release = true;
            fprintf(stderr, "xemu-vr: xrReleaseSwapchainImage failed "
                            "(%d).\n", (int)res);
        }
        return VR_COPY_SKIPPED;
    }

    return submitted ? VR_COPY_COPIED : VR_COPY_SKIPPED;
}

/* ---- SBC cockpit panel (second quad layer) ------------------------------
 *
 * A fixed VR_SBC_PANEL_W x VR_SBC_PANEL_H R8G8B8A8_SRGB swapchain, uploaded
 * from a persistently-mapped host-visible staging buffer via
 * vkCmdCopyBufferToImage on a dedicated command buffer + fence. The panel
 * raster is authored top-down SRGB, so — unlike the screen copy — there is NO
 * Y-flip: a single full-extent region. This mirrors the chains[0]
 * acquire/wait/release + fence discipline exactly; every failure degrades to
 * "no fresh panel this frame" and never fails VR. */

static bool create_panel_chain(VRCompositor *c)
{
    VREyeChain *chain = &c->panel_chain;

    XrSwapchainCreateInfo ci = {
        .type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
        .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                      XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT,
        .format = (int64_t)VK_FORMAT_R8G8B8A8_SRGB,
        .sampleCount = 1,
        .width = VR_SBC_PANEL_W,
        .height = VR_SBC_PANEL_H,
        .faceCount = 1,
        .arraySize = 1,
        .mipCount = 1,
    };

    XrResult res = xr_pfn.CreateSwapchain(g_xemu_vr.xr.session, &ci,
                                          &chain->swapchain);
    if (XR_FAILED(res)) {
        chain->swapchain = XR_NULL_HANDLE;
        if (!c->warned_panel) {
            c->warned_panel = true;
            fprintf(stderr, "xemu-vr: panel xrCreateSwapchain failed (%d); "
                            "cockpit panel stays hidden.\n", (int)res);
        }
        return false;
    }

    uint32_t count = 0;
    if (!check_xr(xr_pfn.EnumerateSwapchainImages(chain->swapchain, 0, &count,
                                                  NULL),
                  "xrEnumerateSwapchainImages (panel)") ||
        count == 0) {
        return false;
    }

    chain->images = g_new0(XrSwapchainImageVulkan2KHR, count);
    for (uint32_t i = 0; i < count; i++) {
        chain->images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR;
    }
    if (!check_xr(xr_pfn.EnumerateSwapchainImages(
                      chain->swapchain, count, &count,
                      (XrSwapchainImageBaseHeader *)chain->images),
                  "xrEnumerateSwapchainImages (panel)")) {
        return false;
    }
    chain->num_images = count;

    fprintf(stderr, "xemu-vr: SBC panel swapchain created: %ux%u, %u images "
                    "(VK_FORMAT_R8G8B8A8_SRGB).\n",
            (unsigned)VR_SBC_PANEL_W, (unsigned)VR_SBC_PANEL_H, count);
    return true;
}

static bool create_panel_staging(VRCompositor *c, PGRAPHState *pg)
{
    const VkDeviceSize size = (VkDeviceSize)VR_SBC_PANEL_W * VR_SBC_PANEL_H * 4;

    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (vkCreateBuffer(c->device, &bci, NULL, &c->panel_staging_buffer) !=
        VK_SUCCESS) {
        c->panel_staging_buffer = VK_NULL_HANDLE;
        if (!c->warned_panel) {
            c->warned_panel = true;
            fprintf(stderr, "xemu-vr: panel staging vkCreateBuffer failed.\n");
        }
        return false;
    }

    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(c->device, c->panel_staging_buffer, &mr);
    uint32_t mem_type =
        pgraph_vk_get_memory_type(pg, mr.memoryTypeBits,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mem_type == 0xFFFFFFFF) {
        if (!c->warned_panel) {
            c->warned_panel = true;
            fprintf(stderr, "xemu-vr: panel staging: no host-visible coherent "
                            "memory type.\n");
        }
        return false;
    }

    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size,
        .memoryTypeIndex = mem_type,
    };
    if (vkAllocateMemory(c->device, &mai, NULL, &c->panel_staging_memory) !=
        VK_SUCCESS) {
        c->panel_staging_memory = VK_NULL_HANDLE;
        if (!c->warned_panel) {
            c->warned_panel = true;
            fprintf(stderr, "xemu-vr: panel staging vkAllocateMemory "
                            "failed.\n");
        }
        return false;
    }
    if (vkBindBufferMemory(c->device, c->panel_staging_buffer,
                           c->panel_staging_memory, 0) != VK_SUCCESS) {
        fprintf(stderr, "xemu-vr: panel staging vkBindBufferMemory failed.\n");
        return false;
    }
    /* Persistent map: the panel raster writes straight into this pointer, and
     * HOST_COHERENT means no explicit flush (the host->transfer barrier in the
     * copy orders visibility). */
    if (vkMapMemory(c->device, c->panel_staging_memory, 0, size, 0,
                    &c->panel_staging_mapped) != VK_SUCCESS) {
        c->panel_staging_mapped = NULL;
        fprintf(stderr, "xemu-vr: panel staging vkMapMemory failed.\n");
        return false;
    }
    /* Start fully transparent (alpha 0): the panel blends on source alpha, so
     * any bytes a provider leaves unwritten — including a first upload that
     * somehow precedes a raster — read as invisible, never as garbage. */
    memset(c->panel_staging_mapped, 0, size);
    return true;
}

static bool create_panel_cmd(VRCompositor *c)
{
    /* One dedicated primary buffer from the compositor's own pool (which has
     * RESET_COMMAND_BUFFER_BIT), plus its own fence. */
    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = c->cmd_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    if (vkAllocateCommandBuffers(c->device, &cbai, &c->panel_cmd) !=
        VK_SUCCESS) {
        c->panel_cmd = VK_NULL_HANDLE;
        if (!c->warned_panel) {
            c->warned_panel = true;
            fprintf(stderr, "xemu-vr: panel vkAllocateCommandBuffers "
                            "failed.\n");
        }
        return false;
    }

    VkFenceCreateInfo fci = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, /* unsignaled */
    };
    if (vkCreateFence(c->device, &fci, NULL, &c->panel_fence) != VK_SUCCESS) {
        c->panel_fence = VK_NULL_HANDLE;
        vkFreeCommandBuffers(c->device, c->cmd_pool, 1, &c->panel_cmd);
        c->panel_cmd = VK_NULL_HANDLE;
        if (!c->warned_panel) {
            c->warned_panel = true;
            fprintf(stderr, "xemu-vr: panel vkCreateFence failed.\n");
        }
        return false;
    }
    c->panel_fence_submitted = false;
    return true;
}

/* Tear down every panel resource in the safe order: the fence is waited first
 * so no in-flight copy is still reading the staging buffer or writing a panel
 * image, THEN the XR swapchain, staging buffer/memory, command buffer, and
 * fence are destroyed. Idempotent — safe when the panel was never created.
 * Runs inside vr_compositor_stop while the device + command pool are still
 * alive, in lockstep with destroy_swapchains (H-9-correct). */
static void destroy_panel(VRCompositor *c)
{
    if (c->panel_fence != VK_NULL_HANDLE && c->panel_fence_submitted) {
        VkResult res = vkWaitForFences(c->device, 1, &c->panel_fence, VK_TRUE,
                                       ONE_SECOND_NS);
        if (res == VK_TIMEOUT) {
            fprintf(stderr, "xemu-vr: panel copy fence still busy after 1s "
                            "during teardown.\n");
        }
        c->panel_fence_submitted = false;
    }

    if (c->panel_chain.swapchain != XR_NULL_HANDLE) {
        xr_pfn.DestroySwapchain(c->panel_chain.swapchain);
        c->panel_chain.swapchain = XR_NULL_HANDLE;
    }
    g_free(c->panel_chain.images);
    c->panel_chain.images = NULL;
    c->panel_chain.num_images = 0;
    c->panel_chain.ever_released = false;
    c->panel_chain.wait_pending = false;

    if (c->panel_staging_mapped != NULL) {
        vkUnmapMemory(c->device, c->panel_staging_memory);
        c->panel_staging_mapped = NULL;
    }
    if (c->panel_staging_buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(c->device, c->panel_staging_buffer, NULL);
        c->panel_staging_buffer = VK_NULL_HANDLE;
    }
    if (c->panel_staging_memory != VK_NULL_HANDLE) {
        vkFreeMemory(c->device, c->panel_staging_memory, NULL);
        c->panel_staging_memory = VK_NULL_HANDLE;
    }
    if (c->panel_cmd != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(c->device, c->cmd_pool, 1, &c->panel_cmd);
        c->panel_cmd = VK_NULL_HANDLE;
    }
    if (c->panel_fence != VK_NULL_HANDLE) {
        vkDestroyFence(c->device, c->panel_fence, NULL);
        c->panel_fence = VK_NULL_HANDLE;
    }
    c->panel_fence_submitted = false;
}

/* Lazily bring up the whole panel pipeline (swapchain + staging + cmd/fence)
 * on the first enabled frame, like ensure_swapchains. Returns true once
 * everything is ready. On any failure everything is torn back down, so the
 * swapchain handle is a reliable "fully up" flag and the next frame retries
 * cleanly with the panel simply hidden. */
static bool ensure_panel(VRCompositor *c, PGRAPHState *pg)
{
    if (c->panel_chain.swapchain != XR_NULL_HANDLE) {
        return true; /* fully up — partial states are never left behind */
    }
    if (!create_panel_chain(c) || !create_panel_staging(c, pg) ||
        !create_panel_cmd(c)) {
        destroy_panel(c);
        return false;
    }
    return true;
}

/* Record + submit the staging-buffer -> panel-image copy on the dedicated
 * panel command buffer. The caller has already waited the panel fence, so the
 * buffer is free to re-record and the host-written staging bytes are settled.
 * Top-down SRGB bytes: one full-extent region, NO Y-flip. */
static bool record_and_submit_panel_copy(VRCompositor *c, VkImage dst)
{
    VkCommandBuffer cmd = c->panel_cmd;
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) {
        if (!c->warned_panel_upload) {
            c->warned_panel_upload = true;
            fprintf(stderr, "xemu-vr: panel vkBeginCommandBuffer failed.\n");
        }
        return false;
    }

    /* Host raster -> transfer read on the staging buffer (mirrors display.c's
     * host_barrier; the memory is HOST_COHERENT so no explicit flush). */
    VkBufferMemoryBarrier host_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = c->panel_staging_buffer,
        .size = VK_WHOLE_SIZE,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                         &host_barrier, 0, NULL);

    /* Destination: COLOR_ATTACHMENT -> TRANSFER_DST. Full-extent overwrite
     * makes pre-copy contents (and the first acquire's true UNDEFINED layout)
     * irrelevant — same reasoning as the screen copy. */
    image_barrier(cmd, dst,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                      VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
                  VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkBufferImageCopy region = {
        .bufferOffset = 0,
        .bufferRowLength = 0,   /* tightly packed rows (VR_SBC_PANEL_W) */
        .bufferImageHeight = 0,
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageOffset = { 0, 0, 0 },
        .imageExtent = { VR_SBC_PANEL_W, VR_SBC_PANEL_H, 1 },
    };
    vkCmdCopyBufferToImage(cmd, c->panel_staging_buffer, dst,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    /* Destination back: TRANSFER_DST -> COLOR_ATTACHMENT (the layout the
     * runtime's compositor expects at release). */
    image_barrier(cmd, dst,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        if (!c->warned_panel_upload) {
            c->warned_panel_upload = true;
            fprintf(stderr, "xemu-vr: panel vkEndCommandBuffer failed.\n");
        }
        return false;
    }

    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    if (vkQueueSubmit(c->queue, 1, &si, c->panel_fence) != VK_SUCCESS) {
        if (!c->warned_panel_upload) {
            c->warned_panel_upload = true;
            fprintf(stderr, "xemu-vr: panel vkQueueSubmit failed.\n");
        }
        return false;
    }
    c->panel_fence_submitted = true;
    return true;
}

/* Acquire/wait/upload/release one panel image. Mirrors copy_to_swapchain's XR
 * discipline: a timed-out or failed wait leaves the image acquired, so the
 * NEXT call re-waits the same image (never re-acquires). Returns true only
 * when the image is released with fresh content (ever_released latches); any
 * failure returns false and leaves the panel re-presenting its last released
 * image (the caller keeps the repaint pending). */
static bool panel_upload(VRCompositor *c)
{
    VREyeChain *chain = &c->panel_chain;
    uint32_t index = 0;

    if (chain->wait_pending) {
        index = chain->pending_index; /* re-wait the same acquired image */
    } else {
        XrSwapchainImageAcquireInfo ai = {
            .type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO,
        };
        if (XR_FAILED(xr_pfn.AcquireSwapchainImage(chain->swapchain, &ai,
                                                   &index))) {
            if (!c->warned_panel_acquire) {
                c->warned_panel_acquire = true;
                fprintf(stderr, "xemu-vr: panel xrAcquireSwapchainImage "
                                "failed.\n");
            }
            return false;
        }
        chain->pending_index = index;
        chain->wait_pending = true; /* acquired; cleared once a wait works */
    }

    XrSwapchainImageWaitInfo wi = {
        .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO,
        .timeout = ONE_SECOND_NS,
    };
    XrResult res = xr_pfn.WaitSwapchainImage(chain->swapchain, &wi);
    if (res == XR_TIMEOUT_EXPIRED || XR_FAILED(res)) {
        /* Do NOT touch/release the image; it stays acquired (wait_pending) and
         * the next upload re-waits it. The last released image keeps showing. */
        if (!c->warned_panel_wait) {
            c->warned_panel_wait = true;
            fprintf(stderr, "xemu-vr: panel xrWaitSwapchainImage failed "
                            "(%d).\n", (int)res);
        }
        return false;
    }
    chain->wait_pending = false; /* wait satisfied; image MUST be released */

    bool copied = record_and_submit_panel_copy(c, chain->images[index].image);

    /* Release regardless of copy success (else the acquire leaks); only latch
     * ever_released when the fresh content was actually submitted. */
    XrSwapchainImageReleaseInfo ri = {
        .type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO,
    };
    if (XR_FAILED(xr_pfn.ReleaseSwapchainImage(chain->swapchain, &ri))) {
        if (!c->warned_panel_release) {
            c->warned_panel_release = true;
            fprintf(stderr, "xemu-vr: panel xrReleaseSwapchainImage "
                            "failed.\n");
        }
        return false;
    }
    if (copied) {
        chain->ever_released = true;
    }
    return copied;
}

static float compute_aspect(VRCompositor *c)
{
    /* Source (== swapchain == display image) extent ratio; see the header
     * comment (delta D8, plan U7 for anamorphic refinement). */
    return (c->swapchain_height > 0) ?
               (float)c->swapchain_width / (float)c->swapchain_height :
               4.0f / 3.0f;
}

static void *pacer_thread_main(void *opaque)
{
    VRCompositor *c = opaque;

    /* Stable for the pacer's lifetime: teardown joins this thread before
     * the session is destroyed (vr_manager teardown order). */
    const XrSession session = g_xemu_vr.xr.session;

    while (!qatomic_read(&c->pacer_exit)) {
        if (!vr_xr_session_running()) {
            /* Parked until the session begins running (READY → begin). */
            g_usleep(20 * 1000);
            continue;
        }

        XrFrameState fs = { .type = XR_TYPE_FRAME_STATE };
        XrResult res = xr_pfn.WaitFrame(session, NULL, &fs);
        if (XR_FAILED(res)) {
            if (res == XR_ERROR_SESSION_NOT_RUNNING) {
                continue; /* benign race with STOPPING */
            }
            if (!c->warned_pacer) {
                c->warned_pacer = true;
                fprintf(stderr, "xemu-vr: xrWaitFrame failed (%d); pacer "
                                "parked. Session-loss handling arrives via "
                                "events.\n", (int)res);
            }
            g_usleep(100 * 1000);
            continue;
        }

        qemu_mutex_lock(&vr_frame_mutex);
        c->pending_frame_state = fs;
        c->has_pending_frame = true;
        qemu_mutex_unlock(&vr_frame_mutex);
    }

    qatomic_set(&c->pacer_done, true);
    return NULL;
}

/* Retry a begin+zero-layer-end owed from a previously failed xrBeginFrame.
 * Returns true when nothing is owed (anymore). */
static bool settle_owed_begin(VRCompositor *c, XrSession session)
{
    if (!c->begin_owed) {
        return true;
    }

    XrFrameBeginInfo bi = { .type = XR_TYPE_FRAME_BEGIN_INFO };
    XrResult res = xr_pfn.BeginFrame(session, &bi);
    if (res == XR_ERROR_SESSION_NOT_RUNNING) {
        /* Session ended; frame-loop state resets on next begin-session. */
        c->begin_owed = false;
        return true;
    }
    if (XR_FAILED(res)) {
        return false;
    }

    XrFrameEndInfo ei = {
        .type = XR_TYPE_FRAME_END_INFO,
        .displayTime = c->begin_owed_display_time,
        .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
        .layerCount = 0,
        .layers = NULL,
    };
    xr_pfn.EndFrame(session, &ei);
    c->begin_owed = false;
    return true;
}

/* Shutdown helper: if a paced frame is pending, begin+end it (zero layers)
 * so a pacer blocked in xrWaitFrame returns and can observe pacer_exit. */
static void drain_one_paced_frame_for_shutdown(VRCompositor *c)
{
    XrFrameState fs;

    qemu_mutex_lock(&vr_frame_mutex);
    if (!c->has_pending_frame) {
        qemu_mutex_unlock(&vr_frame_mutex);
        return;
    }
    fs = c->pending_frame_state;
    c->has_pending_frame = false;
    qemu_mutex_unlock(&vr_frame_mutex);

    if (g_xemu_vr.xr.session == XR_NULL_HANDLE) {
        return;
    }

    const XrSession session = g_xemu_vr.xr.session;
    XrFrameBeginInfo bi = { .type = XR_TYPE_FRAME_BEGIN_INFO };
    if (XR_FAILED(xr_pfn.BeginFrame(session, &bi))) {
        return; /* e.g. SESSION_NOT_RUNNING — pacer isn't blocked then */
    }

    XrFrameEndInfo ei = {
        .type = XR_TYPE_FRAME_END_INFO,
        .displayTime = fs.predictedDisplayTime,
        .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
        .layerCount = 0,
        .layers = NULL,
    };
    xr_pfn.EndFrame(session, &ei);
}

bool vr_compositor_start(PGRAPHState *pg)
{
    VRCompositor *c = &g_xemu_vr.comp;
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (c->initialized) {
        return true;
    }

    if (g_xemu_vr.xr.session == XR_NULL_HANDLE) {
        fprintf(stderr, "xemu-vr: compositor start: no XR session.\n");
        return false;
    }

    c->device = r->device;
    c->queue = r->queue;
    c->queue_family =
        pgraph_vk_find_queue_families(r->physical_device).queue_family;

    const XrSession session = g_xemu_vr.xr.session;

    /* Require VK_FORMAT_R8G8B8A8_SRGB among the offered formats. */
    uint32_t fmt_count = 0;
    if (!check_xr(xr_pfn.EnumerateSwapchainFormats(session, 0, &fmt_count,
                                                   NULL),
                  "xrEnumerateSwapchainFormats") ||
        fmt_count == 0) {
        return false;
    }
    g_autofree int64_t *formats = g_new0(int64_t, fmt_count);
    if (!check_xr(xr_pfn.EnumerateSwapchainFormats(session, fmt_count,
                                                   &fmt_count, formats),
                  "xrEnumerateSwapchainFormats")) {
        return false;
    }

    bool has_srgb = false;
    for (uint32_t i = 0; i < fmt_count; i++) {
        if (formats[i] == (int64_t)VK_FORMAT_R8G8B8A8_SRGB) {
            has_srgb = true;
            break;
        }
    }
    if (!has_srgb) {
        fprintf(stderr, "xemu-vr: runtime does not offer "
                        "VK_FORMAT_R8G8B8A8_SRGB. Running flat.\n");
        return false;
    }

    /* Private command pool + primary buffers + unsignaled fences. */
    VkCommandPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = c->queue_family,
    };
    if (vkCreateCommandPool(c->device, &pci, NULL, &c->cmd_pool) !=
        VK_SUCCESS) {
        fprintf(stderr, "xemu-vr: vkCreateCommandPool failed.\n");
        c->cmd_pool = VK_NULL_HANDLE;
        return false;
    }

    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = c->cmd_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = VR_COMP_NUM_CMD_BUFFERS,
    };
    if (vkAllocateCommandBuffers(c->device, &cbai, c->cmd_buffers) !=
        VK_SUCCESS) {
        fprintf(stderr, "xemu-vr: vkAllocateCommandBuffers failed.\n");
        vkDestroyCommandPool(c->device, c->cmd_pool, NULL);
        c->cmd_pool = VK_NULL_HANDLE;
        return false;
    }

    for (uint32_t i = 0; i < VR_COMP_NUM_CMD_BUFFERS; i++) {
        VkFenceCreateInfo fci = {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, /* unsignaled */
        };
        if (vkCreateFence(c->device, &fci, NULL, &c->fences[i]) !=
            VK_SUCCESS) {
            fprintf(stderr, "xemu-vr: vkCreateFence failed.\n");
            for (uint32_t j = 0; j < i; j++) {
                vkDestroyFence(c->device, c->fences[j], NULL);
                c->fences[j] = VK_NULL_HANDLE;
            }
            vkFreeCommandBuffers(c->device, c->cmd_pool,
                                 VR_COMP_NUM_CMD_BUFFERS, c->cmd_buffers);
            vkDestroyCommandPool(c->device, c->cmd_pool, NULL);
            c->cmd_pool = VK_NULL_HANDLE;
            return false;
        }
        c->fence_submitted[i] = false;
    }

    /* Swapchain is created lazily (source size unknown until frame 1). */
    for (int e = 0; e < 2; e++) {
        c->chains[e] = (VREyeChain){ 0 };
    }
    c->swapchain_width = 0;
    c->swapchain_height = 0;
    c->next_cmd_buffer = 0;

    /* SBC panel resources are created lazily on the first enabled frame. */
    c->panel_chain = (VREyeChain){ 0 };
    c->panel_staging_buffer = VK_NULL_HANDLE;
    c->panel_staging_memory = VK_NULL_HANDLE;
    c->panel_staging_mapped = NULL;
    c->panel_cmd = VK_NULL_HANDLE;
    c->panel_fence = VK_NULL_HANDLE;
    c->panel_fence_submitted = false;
    c->panel_dirty = false;

    c->begin_owed = false;
    c->begin_owed_display_time = 0;
    c->head_pose_valid = false;
    qatomic_set(&c->recenter_requested, false);
    c->recenter_pose = (XrPosef){ .orientation.w = 1.0f };

    c->warned_pacer = false;
    c->warned_beginframe = false;
    c->warned_endframe = false;
    c->warned_acquire = false;
    c->warned_wait_timeout = false;
    c->warned_wait_fail = false;
    c->warned_release = false;
    c->warned_fence_timeout = false;
    c->warned_swapchain = false;
    c->warned_panel = false;
    c->warned_panel_acquire = false;
    c->warned_panel_wait = false;
    c->warned_panel_release = false;
    c->warned_panel_upload = false;

    /* VIEW reference space for head-pose publishing (Tier-3 feed).
     * Non-fatal on failure. */
    XrReferenceSpaceCreateInfo rsci = {
        .type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
        .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW,
        .poseInReferenceSpace.orientation.w = 1.0f,
    };
    if (XR_FAILED(xr_pfn.CreateReferenceSpace(session, &rsci,
                                              &c->view_space))) {
        c->view_space = XR_NULL_HANDLE;
        fprintf(stderr, "xemu-vr: VIEW reference space creation failed; "
                        "head pose will stay inert.\n");
    }

    /* Start the pacer (it idles until the session begins running). */
    if (!vr_frame_mutex_inited) {
        qemu_mutex_init(&vr_frame_mutex);
        vr_frame_mutex_inited = true;
    }
    qatomic_set(&c->pacer_exit, false);
    qatomic_set(&c->pacer_done, false);
    qemu_mutex_lock(&vr_frame_mutex);
    c->has_pending_frame = false;
    qemu_mutex_unlock(&vr_frame_mutex);

    c->initialized = true;
    qemu_thread_create(&c->pacer, "vr-pacer", pacer_thread_main, c,
                       QEMU_THREAD_JOINABLE);
    c->pacer_started = true;

    fprintf(stderr, "xemu-vr: compositor initialized (%u copy command "
                    "buffers; swapchain created on first frame).\n",
            VR_COMP_NUM_CMD_BUFFERS);
    return true;
}

void vr_compositor_stop(void)
{
    VRCompositor *c = &g_xemu_vr.comp;

    if (!c->initialized) {
        return;
    }

    /* 1. Signal exit. */
    qatomic_set(&c->pacer_exit, true);

    /* 2. The pacer may be blocked in xrWaitFrame (it returns when the
     * previous frame is begun). Drain the mailbox with begin+zero-end until
     * the pacer signals done, polling ~50ms (bounded ~2s). QemuThread has no
     * timed join, so pacer_done is the poll signal. */
    if (g_xemu_vr.xr.session != XR_NULL_HANDLE) {
        int64_t deadline = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
        while (!qatomic_read(&c->pacer_done)) {
            settle_owed_begin(c, g_xemu_vr.xr.session);
            drain_one_paced_frame_for_shutdown(c);
            if (qatomic_read(&c->pacer_done)) {
                break;
            }
            if (g_get_monotonic_time() >= deadline) {
                fprintf(stderr, "xemu-vr: pacer did not exit within 2s "
                                "during shutdown; joining anyway.\n");
                break;
            }
            g_usleep(50 * 1000);
        }
    }

    /* 3. Join, then destroy resources. */
    if (c->pacer_started) {
        qemu_thread_join(&c->pacer);
        c->pacer_started = false;
    }

    c->head_pose_valid = false;
    if (c->view_space != XR_NULL_HANDLE) {
        xr_pfn.DestroySpace(c->view_space);
        c->view_space = XR_NULL_HANDLE;
    }

    /* Teardown contract (vr.h): the VkDevice is still alive here — we run
     * at the head of pgraph_vk_finalize_instance, before vkDestroyDevice. The
     * panel is torn down in lockstep with the screen chains, before the shared
     * command pool + fences below (destroy_panel frees its buffer from that
     * pool and waits its own fence first — H-9-correct). */
    wait_all_fences(c);
    destroy_swapchains(c);
    destroy_panel(c);

    for (uint32_t i = 0; i < VR_COMP_NUM_CMD_BUFFERS; i++) {
        if (c->fences[i] != VK_NULL_HANDLE) {
            vkDestroyFence(c->device, c->fences[i], NULL);
            c->fences[i] = VK_NULL_HANDLE;
        }
    }
    if (c->cmd_pool != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(c->device, c->cmd_pool,
                             VR_COMP_NUM_CMD_BUFFERS, c->cmd_buffers);
        vkDestroyCommandPool(c->device, c->cmd_pool, NULL);
        c->cmd_pool = VK_NULL_HANDLE;
    }
    for (uint32_t i = 0; i < VR_COMP_NUM_CMD_BUFFERS; i++) {
        c->cmd_buffers[i] = VK_NULL_HANDLE;
        c->fence_submitted[i] = false;
    }
    c->next_cmd_buffer = 0;
    c->device = VK_NULL_HANDLE;
    c->queue = VK_NULL_HANDLE;

    c->initialized = false;
    fprintf(stderr, "xemu-vr: compositor shut down.\n");
}

void vr_compositor_frame(PGRAPHState *pg)
{
    VRCompositor *c = &g_xemu_vr.comp;
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (!c->initialized) {
        return;
    }

    /* Deep-review fix F1: an owed begin must be settled even when the
     * mailbox is empty. The pacer is parked inside xrWaitFrame until the
     * owed frame is begun, so no new frame can ever arrive to reach the
     * settle call below — without this, one transient xrBeginFrame failure
     * wedges the frame loop permanently. */
    if (c->begin_owed) {
        settle_owed_begin(c, g_xemu_vr.xr.session);
    }

    /* Consume the single-slot mailbox; empty → submit nothing this visit
     * (the pfifo thread NEVER blocks on XR pacing). */
    XrFrameState fs = { .type = XR_TYPE_FRAME_STATE };
    qemu_mutex_lock(&vr_frame_mutex);
    if (!c->has_pending_frame) {
        qemu_mutex_unlock(&vr_frame_mutex);
        return;
    }
    fs = c->pending_frame_state;
    c->has_pending_frame = false;
    qemu_mutex_unlock(&vr_frame_mutex);

    /* Session stopped between publish and consume: drop the frame. */
    if (!vr_xr_session_running()) {
        return;
    }

    const XrSession session = g_xemu_vr.xr.session;

    /* Settle any begin owed from a previously failed xrBeginFrame; if it
     * still cannot be begun, this consumed frame becomes the owed one. */
    if (!settle_owed_begin(c, session)) {
        c->begin_owed_display_time = fs.predictedDisplayTime;
        return;
    }

    /* Begin the paced frame. */
    XrFrameBeginInfo bi = { .type = XR_TYPE_FRAME_BEGIN_INFO };
    XrResult res = xr_pfn.BeginFrame(session, &bi);
    if (XR_FAILED(res)) {
        if (res == XR_ERROR_SESSION_NOT_RUNNING) {
            return;
        }
        if (!c->warned_beginframe) {
            c->warned_beginframe = true;
            fprintf(stderr, "xemu-vr: xrBeginFrame failed (%d); will retry "
                            "the begin/end pairing.\n", (int)res);
        }
        c->begin_owed = true;
        c->begin_owed_display_time = fs.predictedDisplayTime;
        return;
    }

    /* Head pose: VIEW relative to LOCAL at the predicted display time.
     * Stashed for the Tier-3 camera feed (no consumer yet). */
    if (c->view_space != XR_NULL_HANDLE) {
        XrSpaceLocation loc = { .type = XR_TYPE_SPACE_LOCATION };
        if (XR_SUCCEEDED(xr_pfn.LocateSpace(c->view_space, g_xemu_vr.xr.space,
                                            fs.predictedDisplayTime, &loc)) &&
            (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
            c->head_pose = loc.pose;
            c->head_pose_valid = true;
        } else {
            c->head_pose_valid = false;
        }
    }

    /* Recenter (W4, F8): re-anchor the screen to the current heading + eye
     * height, yaw-flattened so the screen never tilts or rolls. Consumed
     * here so it takes effect on this very frame. */
    if (qatomic_xchg(&c->recenter_requested, false) && c->head_pose_valid) {
        const XrQuaternionf q = c->head_pose.orientation;
        float yaw = atan2f(2.0f * (q.x * q.z + q.w * q.y),
                           1.0f - 2.0f * (q.x * q.x + q.y * q.y));
        c->recenter_pose.orientation =
            (XrQuaternionf){ 0.0f, sinf(yaw * 0.5f), 0.0f, cosf(yaw * 0.5f) };
        c->recenter_pose.position = c->head_pose.position;
        vr_camera_reset(); /* head-look re-zeros with the screen (sibling
                            * semantics: F8 = "this is my new forward") */
        fprintf(stderr, "xemu-vr: recentered (yaw %.1f deg, eye height "
                        "%.2f m).\n", yaw * (180.0f / 3.14159265f),
                c->head_pose.position.y);
    }

    /* Tier-3: head-look pokes + hunt dumps (no-ops unless configured). */
    vr_camera_frame(pg);

    /* Copy the display image (mono: chains[0], src layer 0). The image may
     * be absent during VGA-blit boot phases — treated as nothing to show.
     * S2 async present: when this sync's composite went the async path, the
     * frame lives in the CURRENT present slot's image (same size as the
     * singleton, published just before this call on this same pfifo
     * thread), not in disp->image. Same-queue submission order makes the
     * copy see the finished composite — the exact argument this file's
     * record_and_submit_copy already documents. */
    PGRAPHVkDisplayState *disp = &r->display;
    VkImage frame_image = disp->present_published_valid ?
                              disp->present_published_image : disp->image;
    const bool have_frame = frame_image != VK_NULL_HANDLE &&
                            disp->width > 0 && disp->height > 0;
    bool force_zero_layers = false;
    if (fs.shouldRender && have_frame) {
        uint32_t w = (uint32_t)disp->width;
        uint32_t h = (uint32_t)disp->height;
        if (ensure_swapchains(c, w, h)) {
            VRCopyResult cr =
                copy_to_swapchain(c, frame_image, &c->chains[0]);
            if (cr == VR_COPY_COPIED) {
                c->chains[0].ever_released = true;
            } else if (cr == VR_COPY_TIMEOUT_ZERO_LAYER) {
                force_zero_layers = true;
            }
            /* Skipped: keep the last released image. */
        }
        /* ensure_swapchains failed: fall through, re-present last image. */
    }

    /* SBC cockpit panel: raster + change-gated upload (feature-gated on
     * vr.sbc_panel_enable, and independent of the screen copy above). When
     * disabled, zero panel work happens — the off-state stays byte-identical.
     * When enabled the content provider is polled EVERY frame (cheap when
     * inactive); a fresh image is uploaded only when it reports changed
     * content (or on the first upload), otherwise the last released image is
     * re-presented (chains[0]'s 'keep last image' pattern). Any failure here
     * only hides the panel — it never fails the frame or VR. */
    bool panel_show = false;
    if (g_config.vr.sbc_panel_enable && ensure_panel(c, pg)) {
        /* Reclaim the staging buffer before the raster may overwrite it: block
         * on the prior upload's fence (mirrors the screen copy's pre-reuse
         * fence wait; near-instant since the tiny panel copy completes long
         * before the next frame). */
        bool staging_free = true;
        if (c->panel_fence_submitted) {
            VkResult wr = vkWaitForFences(c->device, 1, &c->panel_fence,
                                          VK_TRUE, ONE_SECOND_NS);
            if (wr == VK_SUCCESS) {
                vkResetFences(c->device, 1, &c->panel_fence);
                c->panel_fence_submitted = false;
            } else {
                staging_free = false;
                if (!c->warned_panel_upload) {
                    c->warned_panel_upload = true;
                    fprintf(stderr, "xemu-vr: panel copy fence wait returned "
                                    "%d; skipping panel upload.\n", (int)wr);
                }
            }
        }

        if (staging_free) {
            bool active = false;
            /* Contract: rasters top-down SRGB into the mapped staging buffer
             * and returns true iff the content changed (false leaves it
             * untouched); active reports whether the panel should show. */
            if (vr_sbc_panel_frame((uint8_t *)c->panel_staging_mapped,
                                   &active)) {
                c->panel_dirty = true;
            }
            /* Upload when a repaint is pending (or on the first ever image);
             * clear the pending flag only once the upload actually releases. */
            if (active && (c->panel_dirty || !c->panel_chain.ever_released)) {
                if (panel_upload(c)) {
                    c->panel_dirty = false;
                }
            }
            panel_show = active && c->panel_chain.ever_released;
        } else {
            /* Pathological (1 s) fence timeout: re-present the last image if we
             * have one; do not raster/upload this frame. */
            panel_show = c->panel_chain.ever_released;
        }
    }

    /* Build the screen layer from chains[0] if it has ever released an
     * image (and no forced-zero this visit): flat quad by default, cylinder
     * section when screen_arc is set and the runtime offers it. */
    XrCompositionLayerQuad quad = { .type = XR_TYPE_COMPOSITION_LAYER_QUAD };
    XrCompositionLayerCylinderKHR cyl = {
        .type = XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR,
    };
    XrCompositionLayerQuad panel_quad = {
        .type = XR_TYPE_COMPOSITION_LAYER_QUAD,
    };
    const XrCompositionLayerBaseHeader *layers[2] = { NULL, NULL };
    uint32_t layer_count = 0;
    if (!force_zero_layers && c->chains[0].ever_released) {
        const float distance = g_config.vr.screen_distance;
        const float height = g_config.vr.screen_height;
        const float arc_deg = g_config.vr.screen_arc;
        const float voffset = g_config.vr.screen_vertical_offset;
        const float aspect = compute_aspect(c);
        const bool curved =
            (arc_deg >= 5.0f) && g_xemu_vr.xr.cylinder_supported;

        XrSwapchainSubImage sub_image = {
            .swapchain = c->chains[0].swapchain,
            .imageRect = { { 0, 0 },
                           { (int32_t)c->swapchain_width,
                             (int32_t)c->swapchain_height } },
            .imageArrayIndex = 0,
        };

        /* Anchor pose (W4): recenter_pose is identity until the first F8,
         * then a yaw-only orientation + head position. For a yaw-only quat
         * (0, sin(y/2), 0, cos(y/2)): sin(yaw) = 2wy, cos(yaw) = 1-2y². */
        const XrQuaternionf rq = c->recenter_pose.orientation;
        const XrVector3f rp = c->recenter_pose.position;
        const float sin_yaw = 2.0f * rq.w * rq.y;
        const float cos_yaw = 1.0f - 2.0f * rq.y * rq.y;

        if (curved) {
            /* layerFlags == 0: source alpha ignored — opaque screen. Pose is
             * the cylinder CENTRE (the viewer's origin); distance becomes
             * the radius; height follows from the aspect
             * (height = radius * centralAngle / aspectRatio). voffset rides
             * the whole section up/down. */
            cyl.layerFlags = 0;
            cyl.space = g_xemu_vr.xr.space;
            cyl.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            cyl.subImage = sub_image;
            cyl.pose.orientation = rq;
            cyl.pose.position =
                (XrVector3f){ rp.x, rp.y + voffset, rp.z };
            cyl.radius = distance;
            cyl.centralAngle = arc_deg * (3.14159265f / 180.0f);
            cyl.aspectRatio = aspect;
            layers[0] = (const XrCompositionLayerBaseHeader *)&cyl;
        } else {
            quad.layerFlags = 0;
            quad.space = g_xemu_vr.xr.space;
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.subImage = sub_image;
            /* Screen offset (0, voffset, -distance) rotated by the anchor
             * yaw, translated to the anchor position. */
            quad.pose.orientation = rq;
            quad.pose.position = (XrVector3f){
                rp.x - distance * sin_yaw,
                rp.y + voffset,
                rp.z - distance * cos_yaw,
            };
            quad.size = (XrExtent2Df){ height * aspect, height };
            layers[0] = (const XrCompositionLayerBaseHeader *)&quad;
        }
        layer_count = 1;
    }

    /* Append the SBC panel as a second quad layer, blended on its own alpha,
     * whenever it has content to show. Independent of the screen layer, so a
     * screen-side zero-layer visit can still present the panel alone.
     * panel_show already implies panel_chain.ever_released. */
    if (panel_show) {
        const float distance = g_config.vr.sbc_panel_distance;
        const float drop = g_config.vr.sbc_panel_drop;
        const float width = g_config.vr.sbc_panel_width_m;
        const float tilt_rad =
            g_config.vr.sbc_panel_tilt_deg * (3.14159265f / 180.0f);
        /* Pitch-up about X so the low panel's face tilts up toward the eyes.
         * In OpenXR's right-handed, -Z-forward space a quad's front faces +Z;
         * tilting its normal from +Z toward +Y (up) is a NEGATIVE rotation
         * about X — hence the -sin half-angle. */
        const XrQuaternionf q_pitch = {
            -sinf(tilt_rad * 0.5f), 0.0f, 0.0f, cosf(tilt_rad * 0.5f),
        };

        XrVector3f pos;
        XrQuaternionf orient;
        switch (g_config.vr.sbc_panel_anchor) {
        case CONFIG_VR_SBC_PANEL_ANCHOR_WORLD:
            /* Raw LOCAL-space offset + tilt; no recenter coupling (F8 leaves
             * it where it is). */
            pos = (XrVector3f){ 0.0f, -drop, -distance };
            orient = q_pitch;
            break;
        case CONFIG_VR_SBC_PANEL_ANCHOR_SCREEN: {
            /* A dash below the screen: the screen's yaw + distance, dropped by
             * sbc_panel_drop, no tilt (same facing as the screen quad). */
            const XrQuaternionf rq = c->recenter_pose.orientation;
            const XrVector3f rp = c->recenter_pose.position;
            const float sin_yaw = 2.0f * rq.w * rq.y;
            const float cos_yaw = 1.0f - 2.0f * rq.y * rq.y;
            const float sdist = g_config.vr.screen_distance;
            const float svoff = g_config.vr.screen_vertical_offset;
            pos = (XrVector3f){
                rp.x - sdist * sin_yaw,
                rp.y + svoff - drop,
                rp.z - sdist * cos_yaw,
            };
            orient = rq;
            break;
        }
        case CONFIG_VR_SBC_PANEL_ANCHOR_CONSOLE:
        default: {
            /* The virtual centre console: low + near, tilted up, anchored to
             * the SAME recenter_pose as the screen so F8 moves both. Offset
             * (0, -drop, -distance) yaw-rotated about the anchor (same
             * sin/cos-yaw derivation as the screen), then tilted up about X
             * (q = anchor_yaw * pitch). */
            const XrQuaternionf rq = c->recenter_pose.orientation;
            const XrVector3f rp = c->recenter_pose.position;
            const float sin_yaw = 2.0f * rq.w * rq.y;
            const float cos_yaw = 1.0f - 2.0f * rq.y * rq.y;
            pos = (XrVector3f){
                rp.x - distance * sin_yaw,
                rp.y - drop,
                rp.z - distance * cos_yaw,
            };
            orient = quat_mul(rq, q_pitch);
            break;
        }
        }

        /* The panel raster carries straight alpha → blend on the source. */
        panel_quad.layerFlags =
            XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
            /* The panel raster carries STRAIGHT alpha (not premultiplied). */
            XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
        panel_quad.space = g_xemu_vr.xr.space;
        panel_quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        panel_quad.subImage = (XrSwapchainSubImage){
            .swapchain = c->panel_chain.swapchain,
            .imageRect = { { 0, 0 }, { VR_SBC_PANEL_W, VR_SBC_PANEL_H } },
            .imageArrayIndex = 0,
        };
        panel_quad.pose.orientation = orient;
        panel_quad.pose.position = pos;
        panel_quad.size = (XrExtent2Df){
            width, width * (float)VR_SBC_PANEL_H / (float)VR_SBC_PANEL_W,
        };
        layers[layer_count++] =
            (const XrCompositionLayerBaseHeader *)&panel_quad;
    }

    /* End the frame with the composited layers (screen and/or panel, in that
     * order), or zero layers. */
    XrFrameEndInfo ei = {
        .type = XR_TYPE_FRAME_END_INFO,
        .displayTime = fs.predictedDisplayTime,
        .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
        .layerCount = layer_count,
        .layers = layer_count ? layers : NULL,
    };
    res = xr_pfn.EndFrame(session, &ei);
    if (XR_FAILED(res) && !c->warned_endframe) {
        c->warned_endframe = true;
        fprintf(stderr, "xemu-vr: xrEndFrame failed (%d).\n", (int)res);
    }
}
