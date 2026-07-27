/*
 * xemu-VR — OpenXR instance/system/session + Vulkan-through-XR wrappers
 *
 * SPDX-FileCopyrightText: 2026 Patrick Carey
 * SPDX-License-Identifier: LGPL-2.0-or-later
 *
 * C port of the team's PCSX2-VR XRSession.cpp (docs/vr/01-port-map.md §6 —
 * verdict V/A throughout; logic verbatim, xr* calls routed through the
 * dlopen PFN table, Console → fprintf, app name "xemu").
 *
 * Pins honored here (port map §1/§4, mupen S0): apiVersion is
 * XR_MAKE_VERSION(1, 0, 0) — NEVER the 1.1 headers' XR_CURRENT_API_VERSION
 * (rig loader is 1.0.20); XR_KHR_vulkan_enable2 required, cylinder-layer
 * extension probed and enabled opportunistically;
 * xrGetVulkanGraphicsRequirements2KHR is MANDATORY before session creation;
 * pfnGetInstanceProcAddr for the wrappers is volk's global
 * vkGetInstanceProcAddr (loaded by volkInitialize() before the first
 * wrapper runs — pgraph_vk's create_instance ordering guarantees it).
 *
 * THREADING: pfifo thread only, except g_xemu_vr.xr.session_running which
 * the (future) pacer thread reads via qatomic_read.
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "renderer.h"
#include "vr.h"
#include "vr_internal.h"

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

static const char *session_state_name(XrSessionState state)
{
    switch (state) {
    case XR_SESSION_STATE_IDLE:         return "IDLE";
    case XR_SESSION_STATE_READY:        return "READY";
    case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
    case XR_SESSION_STATE_VISIBLE:      return "VISIBLE";
    case XR_SESSION_STATE_FOCUSED:      return "FOCUSED";
    case XR_SESSION_STATE_STOPPING:     return "STOPPING";
    case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
    case XR_SESSION_STATE_EXITING:      return "EXITING";
    default:                            return "UNKNOWN";
    }
}

static void handle_session_state_change(XrSessionState new_state)
{
    VRXrSession *xr = &g_xemu_vr.xr;

    fprintf(stderr, "xemu-vr: session state: %s -> %s\n",
            session_state_name(xr->session_state),
            session_state_name(new_state));
    xr->session_state = new_state;

    switch (new_state) {
    case XR_SESSION_STATE_READY:
    {
        XrSessionBeginInfo begin_info = {
            .type = XR_TYPE_SESSION_BEGIN_INFO,
            .primaryViewConfigurationType =
                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
        };
        if (check_xr(xr_pfn.BeginSession(xr->session, &begin_info),
                     "xrBeginSession")) {
            fprintf(stderr, "xemu-vr: session began.\n");
            qatomic_set(&xr->session_running, true);
        } else {
            xr->lost = true;
        }
        break;
    }

    case XR_SESSION_STATE_STOPPING:
        qatomic_set(&xr->session_running, false);
        check_xr(xr_pfn.EndSession(xr->session), "xrEndSession");
        fprintf(stderr, "xemu-vr: session ended (runtime request); may "
                        "begin again when READY.\n");
        break;

    case XR_SESSION_STATE_LOSS_PENDING:
    case XR_SESSION_STATE_EXITING:
        qatomic_set(&xr->session_running, false);
        xr->lost = true;
        break;

    default:
        break;
    }
}

bool vr_xr_create_instance_and_system(void)
{
    VRXrSession *xr = &g_xemu_vr.xr;

    assert(xr->instance == XR_NULL_HANDLE);

    /* Optional extensions: probe the runtime, enable what it offers.
     * Cylinder layers back the curved-screen option (vr.screen_arc). */
    xr->cylinder_supported = false;
    uint32_t ext_count = 0;
    if (XR_SUCCEEDED(xr_pfn.EnumerateInstanceExtensionProperties(
            NULL, 0, &ext_count, NULL)) && ext_count > 0) {
        g_autofree XrExtensionProperties *props =
            g_new0(XrExtensionProperties, ext_count);
        for (uint32_t i = 0; i < ext_count; i++) {
            props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
        }
        if (XR_SUCCEEDED(xr_pfn.EnumerateInstanceExtensionProperties(
                NULL, ext_count, &ext_count, props))) {
            for (uint32_t i = 0; i < ext_count; i++) {
                if (strcmp(props[i].extensionName,
                           XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME) ==
                    0) {
                    xr->cylinder_supported = true;
                }
            }
        }
    }

    const char *enabled_extensions[2] = {
        XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME, NULL
    };
    uint32_t enabled_count = 1;
    if (xr->cylinder_supported) {
        enabled_extensions[enabled_count++] =
            XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME;
    }
    fprintf(stderr, "xemu-vr: cylinder composition layers: %s.\n",
            xr->cylinder_supported ? "supported (curved screen available)" :
                                     "not offered by the runtime");

    XrInstanceCreateInfo ici = {
        .type = XR_TYPE_INSTANCE_CREATE_INFO,
        .applicationInfo = {
            .applicationVersion = 1,
            .engineVersion = 1,
            /* Rig loader is OpenXR 1.0.20 — pin 1.0, not the header's
             * XR_CURRENT_API_VERSION (1.1). */
            .apiVersion = XR_MAKE_VERSION(1, 0, 0),
        },
        .enabledExtensionCount = enabled_count,
        .enabledExtensionNames = enabled_extensions,
    };
    pstrcpy(ici.applicationInfo.applicationName,
            sizeof(ici.applicationInfo.applicationName), "xemu");
    pstrcpy(ici.applicationInfo.engineName,
            sizeof(ici.applicationInfo.engineName), "xemu");

    XrResult res = xr_pfn.CreateInstance(&ici, &xr->instance);
    if (XR_FAILED(res)) {
        /* No runtime (or one without vulkan_enable2) just means flat. */
        fprintf(stderr, "xemu-vr: xrCreateInstance failed (%d) — is an "
                        "OpenXR runtime installed and active? Running "
                        "flat.\n", (int)res);
        xr->instance = XR_NULL_HANDLE;
        return false;
    }

    if (!vr_xr_load_instance_pfns(xr->instance)) {
        fprintf(stderr, "xemu-vr: failed to resolve OpenXR entry points — "
                        "running flat.\n");
        vr_xr_destroy_instance();
        return false;
    }

    XrInstanceProperties ip = { .type = XR_TYPE_INSTANCE_PROPERTIES };
    if (check_xr(xr_pfn.GetInstanceProperties(xr->instance, &ip),
                 "xrGetInstanceProperties")) {
        fprintf(stderr, "xemu-vr: OpenXR runtime: %s %u.%u.%u\n",
                ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
                XR_VERSION_MINOR(ip.runtimeVersion),
                XR_VERSION_PATCH(ip.runtimeVersion));
    }

    XrSystemGetInfo sgi = {
        .type = XR_TYPE_SYSTEM_GET_INFO,
        .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY,
    };
    res = xr_pfn.GetSystem(xr->instance, &sgi, &xr->system_id);
    if (XR_FAILED(res)) {
        fprintf(stderr, "xemu-vr: no HMD system available (%d) — headset "
                        "connected and awake? Running flat.\n", (int)res);
        vr_xr_destroy_instance();
        return false;
    }

    XrSystemProperties sp = { .type = XR_TYPE_SYSTEM_PROPERTIES };
    if (check_xr(xr_pfn.GetSystemProperties(xr->instance, xr->system_id, &sp),
                 "xrGetSystemProperties")) {
        fprintf(stderr, "xemu-vr: HMD: %s\n", sp.systemName);
    }

    xr->lost = false;
    return true;
}

bool vr_xr_query_graphics_requirements(void)
{
    VRXrSession *xr = &g_xemu_vr.xr;

    /* MANDATORY before xrCreateSession
     * (XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING otherwise). */
    XrGraphicsRequirementsVulkan2KHR reqs = {
        .type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR,
    };
    if (!check_xr(xr_pfn.GetVulkanGraphicsRequirements2KHR(
                      xr->instance, xr->system_id, &reqs),
                  "xrGetVulkanGraphicsRequirements2KHR")) {
        return false;
    }

    /* Stored for the instance-creation wrapper's apiVersion clamp (doc 01
     * task #5 parity — PCSX2 clamps the requested VkInstance apiVersion
     * into the runtime's range; some runtimes reject out-of-range). */
    xr->vk_min_version = reqs.minApiVersionSupported;
    xr->vk_max_version = reqs.maxApiVersionSupported;
    fprintf(stderr, "xemu-vr: runtime supports Vulkan %u.%u - %u.%u\n",
            XR_VERSION_MAJOR(reqs.minApiVersionSupported),
            XR_VERSION_MINOR(reqs.minApiVersionSupported),
            XR_VERSION_MAJOR(reqs.maxApiVersionSupported),
            XR_VERSION_MINOR(reqs.maxApiVersionSupported));
    return true;
}

void vr_xr_destroy_instance(void)
{
    VRXrSession *xr = &g_xemu_vr.xr;

    vr_xr_destroy_session();

    if (xr->instance != XR_NULL_HANDLE) {
        if (xr_pfn.DestroyInstance != NULL) {
            xr_pfn.DestroyInstance(xr->instance);
        }
        xr->instance = XR_NULL_HANDLE;
    }

    xr->system_id = XR_NULL_SYSTEM_ID;
    xr->lost = false;
    vr_xr_clear_instance_pfns();
}

VkResult vr_xr_create_vulkan_instance(const VkInstanceCreateInfo *create_info,
                                      VkInstance *out_instance)
{
    VRXrSession *xr = &g_xemu_vr.xr;

    /* Clamp the requested Vulkan apiVersion into the runtime's supported
     * range (PCSX2 does the same; xemu asks for min(driver, 1.3), which a
     * conservative runtime may exceed on the low side or undercut on the
     * high side). Shallow-copy the create infos; never mutate xemu's. */
    VkInstanceCreateInfo ci_local = *create_info;
    VkApplicationInfo ai_local;
    if (create_info->pApplicationInfo != NULL && xr->vk_max_version != 0) {
        ai_local = *create_info->pApplicationInfo;
        uint32_t lo = VK_MAKE_API_VERSION(
            0, XR_VERSION_MAJOR(xr->vk_min_version),
            XR_VERSION_MINOR(xr->vk_min_version), 0);
        uint32_t hi = VK_MAKE_API_VERSION(
            0, XR_VERSION_MAJOR(xr->vk_max_version),
            XR_VERSION_MINOR(xr->vk_max_version), 0);
        uint32_t req = ai_local.apiVersion;
        uint32_t clamped = req < lo ? lo : (req > hi ? hi : req);
        if (clamped != req) {
            fprintf(stderr,
                    "xemu-vr: clamping Vulkan apiVersion %u.%u -> %u.%u to "
                    "match the XR runtime's supported range.\n",
                    VK_API_VERSION_MAJOR(req), VK_API_VERSION_MINOR(req),
                    VK_API_VERSION_MAJOR(clamped),
                    VK_API_VERSION_MINOR(clamped));
            ai_local.apiVersion = clamped;
        }
        ci_local.pApplicationInfo = &ai_local;
    }

    XrVulkanInstanceCreateInfoKHR xci = {
        .type = XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR,
        .systemId = xr->system_id,
        .pfnGetInstanceProcAddr = vkGetInstanceProcAddr, /* volk's */
        .vulkanCreateInfo = &ci_local,
    };

    VkResult vk_result = VK_SUCCESS;
    if (!check_xr(xr_pfn.CreateVulkanInstanceKHR(xr->instance, &xci,
                                                 out_instance, &vk_result),
                  "xrCreateVulkanInstanceKHR")) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (vk_result != VK_SUCCESS) {
        fprintf(stderr, "xemu-vr: xrCreateVulkanInstanceKHR: "
                        "vkCreateInstance failed (%d)\n", (int)vk_result);
        return vk_result;
    }
    return VK_SUCCESS;
}

VkPhysicalDevice vr_xr_get_vulkan_physical_device(VkInstance instance)
{
    VRXrSession *xr = &g_xemu_vr.xr;

    XrVulkanGraphicsDeviceGetInfoKHR info = {
        .type = XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR,
        .systemId = xr->system_id,
        .vulkanInstance = instance,
    };

    VkPhysicalDevice pd = VK_NULL_HANDLE;
    if (!check_xr(xr_pfn.GetVulkanGraphicsDevice2KHR(xr->instance, &info,
                                                     &pd),
                  "xrGetVulkanGraphicsDevice2KHR")) {
        return VK_NULL_HANDLE;
    }
    return pd;
}

VkResult vr_xr_create_vulkan_device(VkPhysicalDevice physical_device,
                                    const VkDeviceCreateInfo *create_info,
                                    VkDevice *out_device)
{
    VRXrSession *xr = &g_xemu_vr.xr;

    XrVulkanDeviceCreateInfoKHR xci = {
        .type = XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR,
        .systemId = xr->system_id,
        .pfnGetInstanceProcAddr = vkGetInstanceProcAddr, /* volk's */
        .vulkanPhysicalDevice = physical_device,
        .vulkanCreateInfo = create_info,
    };

    VkResult vk_result = VK_SUCCESS;
    if (!check_xr(xr_pfn.CreateVulkanDeviceKHR(xr->instance, &xci, out_device,
                                               &vk_result),
                  "xrCreateVulkanDeviceKHR")) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (vk_result != VK_SUCCESS) {
        fprintf(stderr, "xemu-vr: xrCreateVulkanDeviceKHR: vkCreateDevice "
                        "failed (%d)\n", (int)vk_result);
        return vk_result;
    }
    return VK_SUCCESS;
}

bool vr_xr_create_session(PGRAPHState *pg)
{
    VRXrSession *xr = &g_xemu_vr.xr;
    PGRAPHVkState *r = pg->vk_renderer_state;

    assert(xr->session == XR_NULL_HANDLE);

    /* Same family instance.c used for r->queue; queueIndex 0 matches
     * vkGetDeviceQueue(..., 0, &r->queue) (port map D9). */
    QueueFamilyIndices indices =
        pgraph_vk_find_queue_families(r->physical_device);

    XrGraphicsBindingVulkan2KHR binding = {
        .type = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR,
        .instance = r->instance,
        .physicalDevice = r->physical_device,
        .device = r->device,
        .queueFamilyIndex = indices.queue_family,
        .queueIndex = 0,
    };

    XrSessionCreateInfo sci = {
        .type = XR_TYPE_SESSION_CREATE_INFO,
        .next = &binding,
        .systemId = xr->system_id,
    };

    if (!check_xr(xr_pfn.CreateSession(xr->instance, &sci, &xr->session),
                  "xrCreateSession")) {
        xr->session = XR_NULL_HANDLE;
        return false;
    }

    XrReferenceSpaceCreateInfo rsci = {
        .type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
        .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL,
        .poseInReferenceSpace.orientation.w = 1.0f,
    };
    if (!check_xr(xr_pfn.CreateReferenceSpace(xr->session, &rsci, &xr->space),
                  "xrCreateReferenceSpace")) {
        vr_xr_destroy_session();
        return false;
    }

    xr->session_state = XR_SESSION_STATE_UNKNOWN;
    fprintf(stderr, "xemu-vr: XR session created (Vulkan, queue family "
                    "%d).\n", indices.queue_family);
    return true;
}

void vr_xr_destroy_session(void)
{
    VRXrSession *xr = &g_xemu_vr.xr;

    /* PRECONDITION: the compositor has been stopped and the pacer joined —
     * nobody may be blocked in xrWaitFrame on this session. */
    if (xr->session == XR_NULL_HANDLE) {
        return;
    }

    if (qatomic_read(&xr->session_running)) {
        qatomic_set(&xr->session_running, false);
        xr_pfn.EndSession(xr->session);
    }

    if (xr->space != XR_NULL_HANDLE) {
        xr_pfn.DestroySpace(xr->space);
        xr->space = XR_NULL_HANDLE;
    }

    xr_pfn.DestroySession(xr->session);
    xr->session = XR_NULL_HANDLE;
    xr->session_state = XR_SESSION_STATE_UNKNOWN;
    fprintf(stderr, "xemu-vr: XR session destroyed.\n");
}

void vr_xr_pump_events(void)
{
    VRXrSession *xr = &g_xemu_vr.xr;

    if (xr->instance == XR_NULL_HANDLE) {
        return;
    }

    for (;;) {
        XrEventDataBuffer event = { .type = XR_TYPE_EVENT_DATA_BUFFER };
        XrResult res = xr_pfn.PollEvent(xr->instance, &event);
        if (res == XR_EVENT_UNAVAILABLE) {
            break;
        }
        if (XR_FAILED(res)) {
            check_xr(res, "xrPollEvent");
            xr->lost = true;
            break;
        }

        switch (event.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
        {
            const XrEventDataSessionStateChanged *e =
                (const XrEventDataSessionStateChanged *)&event;
            if (e->session == xr->session) {
                handle_session_state_change(e->state);
            }
            break;
        }

        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            fprintf(stderr, "xemu-vr: OpenXR instance loss pending (runtime "
                            "shutting down/updating).\n");
            qatomic_set(&xr->session_running, false);
            xr->lost = true;
            break;

        case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
            /* LOCAL space is rebased by the runtime on recenter; the quad
             * is positioned relative to LOCAL and follows automatically. */
            fprintf(stderr, "xemu-vr: reference space recentered.\n");
            break;

        case XR_TYPE_EVENT_DATA_EVENTS_LOST:
            fprintf(stderr, "xemu-vr: OpenXR event queue overflowed; some "
                            "events were lost.\n");
            break;

        default:
            break;
        }
    }
}

bool vr_xr_session_running(void)
{
    return qatomic_read(&g_xemu_vr.xr.session_running);
}
