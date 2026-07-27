/*
 * xemu-VR — facade + Vulkan bridge seams
 *
 * SPDX-FileCopyrightText: 2026 Patrick Carey
 * SPDX-License-Identifier: LGPL-2.0-or-later
 *
 * Port of the team's PCSX2-VR VRVulkanBridge (the emulator-glue layer —
 * rewritten against pgraph_vk's instance.c seams per docs/vr/01-port-map.md
 * §6). Contract in vr.h. Increment status: Phase A + Phase B + compositor
 * are all live — with vr.enable=true and a reachable runtime, the frame
 * hook submits the head-tracked screen; any failure at any stage aborts VR
 * internally and the app runs flat.
 *
 * Failure semantics (PCSX2 VRVulkanBridge.h contract): any seam failure
 * aborts the bootstrap internally and FALLS BACK to the plain Vulkan call,
 * so a VR failure can never fail renderer init.
 */

#include "qemu/osdep.h"
#include "ui/xemu-settings.h"
#include "renderer.h"
#include "vr.h"
#include "vr_internal.h"

XemuVRState g_xemu_vr;

bool xemu_vr_wanted(void)
{
    return g_config.vr.enable &&
           g_config.display.renderer == CONFIG_DISPLAY_RENDERER_VULKAN;
}

/* Compositor -> session -> instance, in that order (D7). Idempotent. */
void vr_abort_bootstrap(void)
{
    vr_compositor_stop();
    vr_camera_shutdown();
    vr_xr_destroy_session();
    vr_xr_destroy_instance();
    g_xemu_vr.bootstrap_active = false;
    g_xemu_vr.session_running = false;
}

void xemu_vr_begin_bootstrap(PGRAPHState *pg)
{
    if (!xemu_vr_wanted()) {
        return;
    }

    fprintf(stderr, "xemu-vr: VR enabled — bootstrapping OpenXR (Phase A)\n");

    if (!vr_xr_loader_init() ||
        !vr_xr_create_instance_and_system() ||
        !vr_xr_query_graphics_requirements()) {
        vr_abort_bootstrap();
        return;
    }

    g_xemu_vr.bootstrap_active = true;
}

bool xemu_vr_bootstrap_active(void)
{
    return g_xemu_vr.bootstrap_active;
}

VkResult xemu_vr_create_instance(const VkInstanceCreateInfo *create_info,
                                 VkInstance *out_instance)
{
    if (g_xemu_vr.bootstrap_active) {
        VkResult res = vr_xr_create_vulkan_instance(create_info,
                                                    out_instance);
        if (res == VK_SUCCESS) {
            return VK_SUCCESS;
        }
        fprintf(stderr, "xemu-vr: instance creation through XR failed — "
                        "aborting VR, falling back to plain Vulkan.\n");
        vr_abort_bootstrap();
    }
    return vkCreateInstance(create_info, NULL, out_instance);
}

VkPhysicalDevice xemu_vr_get_physical_device(VkInstance instance)
{
    if (!g_xemu_vr.bootstrap_active) {
        return VK_NULL_HANDLE;
    }

    VkPhysicalDevice pd = vr_xr_get_vulkan_physical_device(instance);
    if (pd == VK_NULL_HANDLE) {
        fprintf(stderr, "xemu-vr: OpenXR did not yield a physical device — "
                        "aborting VR, using xemu's own selection.\n");
        vr_abort_bootstrap();
    }
    return pd;
}

VkResult xemu_vr_create_device(VkPhysicalDevice physical_device,
                               const VkDeviceCreateInfo *create_info,
                               VkDevice *out_device)
{
    if (g_xemu_vr.bootstrap_active) {
        VkResult res = vr_xr_create_vulkan_device(physical_device,
                                                  create_info, out_device);
        if (res == VK_SUCCESS) {
            return VK_SUCCESS;
        }
        fprintf(stderr, "xemu-vr: device creation through XR failed — "
                        "aborting VR, falling back to plain Vulkan.\n");
        vr_abort_bootstrap();
    }
    return vkCreateDevice(physical_device, create_info, NULL, out_device);
}

void xemu_vr_on_device_created(PGRAPHState *pg)
{
    if (!g_xemu_vr.bootstrap_active) {
        return;
    }

    if (!vr_xr_create_session(pg)) {
        fprintf(stderr, "xemu-vr: session creation failed — aborting VR, "
                        "running flat.\n");
        vr_abort_bootstrap();
        return;
    }

    if (!vr_compositor_start(pg)) {
        fprintf(stderr, "xemu-vr: compositor start failed — aborting VR, "
                        "running flat.\n");
        vr_abort_bootstrap();
        return;
    }

    g_xemu_vr.session_running = true;
    fprintf(stderr, "xemu-vr: VR active — headset presentation begins when "
                    "the runtime reports the session READY.\n");
}

void xemu_vr_on_device_destroyed(PGRAPHState *pg)
{
    /* Idempotent; also runs when VR never activated, and on renderer
     * switches via pgraph_vk_finalize_instance. */
    vr_abort_bootstrap();
}

void xemu_vr_request_recenter(void)
{
    /* UI thread. Harmless when VR is off (flag consumed only by a live
     * compositor frame). */
    qatomic_set(&g_xemu_vr.comp.recenter_requested, true);
}

void xemu_vr_request_hunt_dump(void)
{
    /* UI thread (F9). Consumed only when vr.hunt_enable is set. */
    vr_camera_request_hunt_dump();
}

bool xemu_vr_active(void)
{
    /* Status display only; racy read is fine. */
    return g_xemu_vr.session_running;
}

void xemu_vr_frame(PGRAPHState *pg)
{
    /* F9 RAM-hunt dump is VR-INDEPENDENT — it must fire in flat mode too
     * (hunting is done watching the desktop mirror, usually with no headset).
     * Poll it BEFORE the session gate; a pure guest-RAM snapshot needs no VR. */
    vr_camera_hunt_poll(pg);

    if (!g_xemu_vr.session_running) {
        return;
    }

    /* Drive the XR state machine first (READY → xrBeginSession is what
     * un-parks the pacer), then check for session/instance loss — on loss,
     * tear VR down completely; the VkDevice created through XR stays alive
     * and flat rendering continues (PCSX2 VRManager::EndOfFrame shape). */
    vr_xr_pump_events();
    if (g_xemu_vr.xr.lost) {
        fprintf(stderr, "xemu-vr: session/instance lost — shutting VR down; "
                        "flat rendering continues.\n");
        vr_abort_bootstrap();
        return;
    }

    vr_compositor_frame(pg);
}
