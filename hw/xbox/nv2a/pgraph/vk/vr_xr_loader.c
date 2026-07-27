/*
 * xemu-VR — OpenXR runtime loader (dlopen + PFN table)
 *
 * SPDX-FileCopyrightText: 2026 Patrick Carey
 * SPDX-License-Identifier: LGPL-2.0-or-later
 *
 * See vr_xr_loader.h for the loading model.
 */

#include "qemu/osdep.h"

#include <dlfcn.h>

#include "vr_xr_loader.h"

XemuXrPfns xr_pfn;

static void *s_loader_handle;

static bool resolve(XrInstance instance, const char *name,
                    PFN_xrVoidFunction *out)
{
    XrResult res = xr_pfn.GetInstanceProcAddr(instance, name, out);
    if (XR_FAILED(res) || *out == NULL) {
        fprintf(stderr, "xemu-vr: failed to resolve %s (%d)\n", name,
                (int)res);
        *out = NULL;
        return false;
    }
    return true;
}

bool vr_xr_loader_init(void)
{
    if (s_loader_handle != NULL && xr_pfn.CreateInstance != NULL) {
        return true; /* idempotent */
    }

    if (s_loader_handle == NULL) {
        s_loader_handle = dlopen("libopenxr_loader.so.1",
                                 RTLD_NOW | RTLD_LOCAL);
    }
    if (s_loader_handle == NULL) {
        s_loader_handle = dlopen("libopenxr_loader.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (s_loader_handle == NULL) {
        fprintf(stderr, "xemu-vr: OpenXR loader not found (%s) — is an "
                        "OpenXR runtime installed? Running flat.\n",
                dlerror());
        return false;
    }

    xr_pfn.GetInstanceProcAddr =
        (PFN_xrGetInstanceProcAddr)dlsym(s_loader_handle,
                                         "xrGetInstanceProcAddr");
    if (xr_pfn.GetInstanceProcAddr == NULL) {
        fprintf(stderr, "xemu-vr: loader lacks xrGetInstanceProcAddr — "
                        "running flat.\n");
        return false;
    }

    /* Pre-instance functions: spec-legal to resolve with XR_NULL_HANDLE. */
    bool ok = true;
    ok &= resolve(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties",
                  (PFN_xrVoidFunction *)&xr_pfn
                      .EnumerateInstanceExtensionProperties);
    ok &= resolve(XR_NULL_HANDLE, "xrCreateInstance",
                  (PFN_xrVoidFunction *)&xr_pfn.CreateInstance);
    return ok;
}

bool vr_xr_load_instance_pfns(XrInstance instance)
{
    bool ok = true;

#define RESOLVE(name) \
    ok &= resolve(instance, "xr" #name, (PFN_xrVoidFunction *)&xr_pfn.name)

    RESOLVE(DestroyInstance);
    RESOLVE(GetInstanceProperties);
    RESOLVE(GetSystem);
    RESOLVE(GetSystemProperties);
    RESOLVE(ResultToString);
    RESOLVE(PollEvent);
    RESOLVE(CreateSession);
    RESOLVE(DestroySession);
    RESOLVE(BeginSession);
    RESOLVE(EndSession);
    RESOLVE(CreateReferenceSpace);
    RESOLVE(DestroySpace);
    RESOLVE(LocateSpace);
    RESOLVE(CreateSwapchain);
    RESOLVE(DestroySwapchain);
    RESOLVE(EnumerateSwapchainFormats);
    RESOLVE(EnumerateSwapchainImages);
    RESOLVE(AcquireSwapchainImage);
    RESOLVE(WaitSwapchainImage);
    RESOLVE(ReleaseSwapchainImage);
    RESOLVE(WaitFrame);
    RESOLVE(BeginFrame);
    RESOLVE(EndFrame);
    RESOLVE(GetVulkanGraphicsRequirements2KHR);
    RESOLVE(CreateVulkanInstanceKHR);
    RESOLVE(GetVulkanGraphicsDevice2KHR);
    RESOLVE(CreateVulkanDeviceKHR);
#undef RESOLVE

    return ok;
}

void vr_xr_clear_instance_pfns(void)
{
    PFN_xrGetInstanceProcAddr gipa = xr_pfn.GetInstanceProcAddr;
    PFN_xrEnumerateInstanceExtensionProperties eiep =
        xr_pfn.EnumerateInstanceExtensionProperties;
    PFN_xrCreateInstance ci = xr_pfn.CreateInstance;

    memset(&xr_pfn, 0, sizeof(xr_pfn));

    /* keep bootstrap + pre-instance PFNs (library stays loaded) */
    xr_pfn.GetInstanceProcAddr = gipa;
    xr_pfn.EnumerateInstanceExtensionProperties = eiep;
    xr_pfn.CreateInstance = ci;
}
