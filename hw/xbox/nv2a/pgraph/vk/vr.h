/*
 * xemu-VR — public interface of the VR module (Tier-1 head-tracked screen)
 *
 * SPDX-FileCopyrightText: 2026 Patrick Carey
 * SPDX-License-Identifier: LGPL-2.0-or-later
 *
 * Design contract: docs/vr/00-mvp-plan.md (W1-W3) and docs/vr/01-port-map.md.
 * Ported from the team's PCSX2-VR module (team-authored; see plan §2a).
 *
 * All functions are called on the pfifo thread (the thread that owns every
 * Vulkan call in the VK renderer), except where noted. Every function is safe
 * to call when VR is disabled or bootstrap failed — the module then behaves
 * as "inactive" and the caller proceeds with the normal flat path. A VR
 * failure must NEVER fail renderer init or crash the emulator.
 *
 * CALL SEQUENCE (mirrors PCSX2 VRVulkanBridge.h, seams in instance.c):
 *   1. xemu_vr_begin_bootstrap()      — top of pgraph_vk_init_instance.
 *                                       Phase A: XrInstance + system + gfx
 *                                       requirements. No-op unless
 *                                       g_config.vr.enable.
 *   2. xemu_vr_create_instance()      — replaces vkCreateInstance (passes
 *                                       through when inactive).
 *   3. xemu_vr_get_physical_device()  — XR-mandated VkPhysicalDevice, or
 *                                       VK_NULL_HANDLE when inactive (caller
 *                                       falls back to its own selection).
 *   4. xemu_vr_create_device()        — replaces vkCreateDevice (passes
 *                                       through when inactive).
 *   5. xemu_vr_on_device_created()    — after vkGetDeviceQueue. Phase B:
 *                                       XrSession + reference space +
 *                                       compositor/pacer start.
 *   6. xemu_vr_frame()                — inside pgraph_vk_sync, right after
 *                                       pgraph_vk_render_display. Consumes
 *                                       the pacer mailbox; never blocks.
 *   7. xemu_vr_on_device_destroyed()  — top of pgraph_vk_finalize_instance,
 *                                       before vkDestroyDevice. Must tolerate
 *                                       any partial state. Idempotent.
 */

#ifndef HW_XBOX_NV2A_PGRAPH_VK_VR_H
#define HW_XBOX_NV2A_PGRAPH_VK_VR_H

#include "hw/xbox/nv2a/pgraph/pgraph.h"
#include <vulkan/vulkan.h>
#include "vr_public.h"

#ifdef __cplusplus
extern "C" {
#endif

bool xemu_vr_wanted(void);
void xemu_vr_begin_bootstrap(PGRAPHState *pg);
bool xemu_vr_bootstrap_active(void);

VkResult xemu_vr_create_instance(const VkInstanceCreateInfo *create_info,
                                 VkInstance *out_instance);
VkPhysicalDevice xemu_vr_get_physical_device(VkInstance instance);
VkResult xemu_vr_create_device(VkPhysicalDevice physical_device,
                               const VkDeviceCreateInfo *create_info,
                               VkDevice *out_device);
void xemu_vr_on_device_created(PGRAPHState *pg);
void xemu_vr_on_device_destroyed(PGRAPHState *pg);

void xemu_vr_frame(PGRAPHState *pg);

/* xemu_vr_request_recenter / xemu_vr_active: see vr_public.h (UI-safe). */

#ifdef __cplusplus
}
#endif

#endif /* HW_XBOX_NV2A_PGRAPH_VK_VR_H */
