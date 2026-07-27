/*
 * xemu-VR — OpenXR runtime loader (dlopen + PFN table)
 *
 * SPDX-FileCopyrightText: 2026 Patrick Carey
 * SPDX-License-Identifier: LGPL-2.0-or-later
 *
 * Port-map delta D1: unlike PCSX2 (statically linked vendored loader), xemu
 * dlopens the system OpenXR loader at runtime so the binary carries no hard
 * libopenxr dependency — VR-off users are completely unaffected. Every xr*
 * call in vr_xr_session.c / vr_xr_compositor.c goes through the PFN table
 * below (volk's pattern).
 *
 * Loading model:
 *   vr_xr_loader_init()          dlopen("libopenxr_loader.so.1" || ".so"),
 *                                dlsym xrGetInstanceProcAddr, resolve the
 *                                pre-instance PFNs (spec-legal with
 *                                XR_NULL_HANDLE). Idempotent; the library
 *                                stays loaded for the process lifetime
 *                                (dlclose of a live runtime is not worth the
 *                                risk).
 *   vr_xr_load_instance_pfns(i)  resolve everything else against the live
 *                                XrInstance (includes the four
 *                                XR_KHR_vulkan_enable2 entry points, which
 *                                no loader exports).
 *   vr_xr_clear_instance_pfns()  null the instance-scoped table on
 *                                xrDestroyInstance.
 */

#ifndef HW_XBOX_NV2A_PGRAPH_VK_VR_XR_LOADER_H
#define HW_XBOX_NV2A_PGRAPH_VK_VR_XR_LOADER_H

#include <vulkan/vulkan.h>

#include "openxr/openxr.h"
#define XR_USE_GRAPHICS_API_VULKAN
#include "openxr/openxr_platform.h"

typedef struct XemuXrPfns {
    /* bootstrap (dlsym) */
    PFN_xrGetInstanceProcAddr GetInstanceProcAddr;

    /* pre-instance (resolved with XR_NULL_HANDLE) */
    PFN_xrEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties;
    PFN_xrCreateInstance CreateInstance;

    /* instance-scoped */
    PFN_xrDestroyInstance DestroyInstance;
    PFN_xrGetInstanceProperties GetInstanceProperties;
    PFN_xrGetSystem GetSystem;
    PFN_xrGetSystemProperties GetSystemProperties;
    PFN_xrResultToString ResultToString;
    PFN_xrPollEvent PollEvent;
    PFN_xrCreateSession CreateSession;
    PFN_xrDestroySession DestroySession;
    PFN_xrBeginSession BeginSession;
    PFN_xrEndSession EndSession;
    PFN_xrCreateReferenceSpace CreateReferenceSpace;
    PFN_xrDestroySpace DestroySpace;
    PFN_xrLocateSpace LocateSpace;
    PFN_xrCreateSwapchain CreateSwapchain;
    PFN_xrDestroySwapchain DestroySwapchain;
    PFN_xrEnumerateSwapchainFormats EnumerateSwapchainFormats;
    PFN_xrEnumerateSwapchainImages EnumerateSwapchainImages;
    PFN_xrAcquireSwapchainImage AcquireSwapchainImage;
    PFN_xrWaitSwapchainImage WaitSwapchainImage;
    PFN_xrReleaseSwapchainImage ReleaseSwapchainImage;
    PFN_xrWaitFrame WaitFrame;
    PFN_xrBeginFrame BeginFrame;
    PFN_xrEndFrame EndFrame;

    /* XR_KHR_vulkan_enable2 (per-instance only) */
    PFN_xrGetVulkanGraphicsRequirements2KHR GetVulkanGraphicsRequirements2KHR;
    PFN_xrCreateVulkanInstanceKHR CreateVulkanInstanceKHR;
    PFN_xrGetVulkanGraphicsDevice2KHR GetVulkanGraphicsDevice2KHR;
    PFN_xrCreateVulkanDeviceKHR CreateVulkanDeviceKHR;
} XemuXrPfns;

extern XemuXrPfns xr_pfn;

bool vr_xr_loader_init(void);
bool vr_xr_load_instance_pfns(XrInstance instance);
void vr_xr_clear_instance_pfns(void);

#endif /* HW_XBOX_NV2A_PGRAPH_VK_VR_XR_LOADER_H */
