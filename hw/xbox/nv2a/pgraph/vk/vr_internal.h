/*
 * xemu-VR — internal state + interfaces between vr_* translation units
 *
 * SPDX-FileCopyrightText: 2026 Patrick Carey
 * SPDX-License-Identifier: LGPL-2.0-or-later
 *
 * Included by vr_*.c only. Field names keep PCSX2 parity where it helps the
 * port review (docs/vr/01-port-map.md).
 */

#ifndef HW_XBOX_NV2A_PGRAPH_VK_VR_INTERNAL_H
#define HW_XBOX_NV2A_PGRAPH_VK_VR_INTERNAL_H

#include "hw/xbox/nv2a/pgraph/pgraph.h"
#include "qemu/thread.h"
#include "vr_xr_loader.h"

/*
 * OpenXR instance/system/session state (vr_xr_session.c).
 * State model (port of PCSX2 XRSession.h):
 *   zero --create_instance_and_system--> InstanceReady
 *   InstanceReady --create_session-----> SessionCreated
 *   SessionCreated --pump: READY-------> xrBeginSession -> Running
 *   Running --pump: STOPPING-----------> xrEndSession -> SessionCreated
 *   any --pump: LOSS_PENDING/EXITING---> lost (torn down by manager)
 *
 * THREADING: everything here runs on the pfifo thread ONLY, except
 * session_running which the (future) pacer thread reads via qatomic.
 */
typedef struct VRXrSession {
    XrInstance instance;
    XrSystemId system_id;
    XrSession session;
    XrSpace space;               /* LOCAL reference space, identity pose */
    XrSessionState session_state;
    bool session_running;        /* qatomic_read/set — pacer thread reads */
    bool lost;
    bool cylinder_supported;     /* XR_KHR_composition_layer_cylinder on */
    XrVersion vk_min_version;    /* runtime's supported Vulkan range, from */
    XrVersion vk_max_version;    /* xrGetVulkanGraphicsRequirements2KHR    */
} VRXrSession;

/*
 * Compositor state (vr_xr_compositor.c) — port of PCSX2 XRCompositor's
 * file-scope singleton. pfifo thread only, EXCEPT: pacer_exit/pacer_done/
 * pending-frame mailbox (shared with the pacer thread; see the mailbox
 * contract in vr_xr_compositor.c's header comment).
 */
#define VR_COMP_NUM_CMD_BUFFERS 2

typedef struct VREyeChain {
    XrSwapchain swapchain;
    XrSwapchainImageVulkan2KHR *images; /* g_new0/g_free */
    uint32_t num_images;
    bool ever_released;    /* gates layer submission                       */
    /* An image was acquired but its xrWaitSwapchainImage timed out/failed:
     * per spec it STAYS acquired and the next wait targets the same
     * oldest-unwaited image — the next visit must RE-WAIT, not re-acquire
     * (re-acquiring leaks one acquire per timeout until the chain wedges
     * with CALL_ORDER_INVALID). */
    bool wait_pending;
    uint32_t pending_index;
} VREyeChain;

typedef struct VRCompositor {
    bool initialized;

    /* Vulkan handles captured at start (PCSX2 VulkanHandles equivalent) so
     * vr_compositor_stop(void) is self-sufficient. */
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;

    QemuThread pacer;
    bool pacer_started;
    bool pacer_exit;                  /* qatomic */
    bool pacer_done;                  /* qatomic */
    XrFrameState pending_frame_state; /* mailbox slot (frame_mutex)        */
    bool has_pending_frame;           /* mailbox flag (frame_mutex)        */

    /* xrBeginFrame failed after a mailbox frame was consumed: the pacer's
     * next xrWaitFrame stays blocked until *some* frame is begun; later
     * visits retry begin(+zero-end) until the pairing is satisfied. */
    bool begin_owed;
    XrTime begin_owed_display_time;

    /* Mono (Tier 1) uses only chains[0] with an eyeVisibility=BOTH layer;
     * chains[1] is dormant until stereo (kept for structure parity). */
    VREyeChain chains[2];
    uint32_t swapchain_width;
    uint32_t swapchain_height;

    /* Y-flip copy regions (field-test finding F8): xemu's display image is
     * GL-heritage memory — row 0 is the picture's BOTTOM (that is why the
     * GL mirror shows it correctly); Vulkan/OpenXR reads row 0 as TOP. The
     * copy flips vertically with one VkImageCopy per row, preserving raw
     * bytes (a vkCmdBlitImage UNORM→SRGB would re-encode gamma and wash the
     * image out). Sized to swapchain_height by ensure_swapchains. */
    VkImageCopy *flip_regions;      /* g_new/g_free */
    uint32_t flip_regions_count;

    /* VIEW reference space; head pose stashed for the Tier-3 feed (no
     * consumer yet; pfifo thread only). Creation failure is non-fatal. */
    XrSpace view_space;
    bool head_pose_valid;
    XrPosef head_pose;

    /* Recenter (W4): UI thread sets recenter_requested (qatomic); the
     * frame consumes it, flattening the current head pose to yaw+position.
     * recenter_pose is the screen anchor (identity until first recenter —
     * preserving the pre-W4 LOCAL-forward placement). pfifo thread only. */
    bool recenter_requested;          /* qatomic */
    XrPosef recenter_pose;

    VkCommandPool cmd_pool;
    VkCommandBuffer cmd_buffers[VR_COMP_NUM_CMD_BUFFERS];
    VkFence fences[VR_COMP_NUM_CMD_BUFFERS];
    bool fence_submitted[VR_COMP_NUM_CMD_BUFFERS];
    uint32_t next_cmd_buffer;

    /* SBC cockpit panel (gated on vr.sbc_panel_enable): a SECOND, fixed-size
     * (VR_SBC_PANEL_W x VR_SBC_PANEL_H, R8G8B8A8_SRGB) XR swapchain composited
     * as an XrCompositionLayerQuad low/near the screen. chains[1] stays
     * stereo-reserved — the panel gets its own chain so the two never alias.
     * CPU-rastered (top-down, no Y-flip) into a persistently-mapped host
     * staging buffer, then vkCmdCopyBufferToImage into the acquired image on a
     * dedicated command buffer + fence. All of it is created lazily on the
     * first enabled frame and destroyed in lockstep with the screen chains in
     * vr_compositor_stop (H-9-correct: fences waited before teardown). Steady
     * state (unchanged content) does no acquire — the layer re-presents the
     * last released image, exactly like chains[0]. pfifo thread only. */
    VREyeChain panel_chain;
    VkBuffer panel_staging_buffer;
    VkDeviceMemory panel_staging_memory;
    void *panel_staging_mapped;       /* persistent map; raster target */
    VkCommandBuffer panel_cmd;        /* dedicated; allocated from cmd_pool */
    VkFence panel_fence;
    bool panel_fence_submitted;
    /* "Content changed but not yet uploaded" — set when the provider reports a
     * change, cleared only once an upload actually releases. Keeps a repaint
     * pending across unchanged frames if an upload's image-wait timed out, so
     * the change-gated path can never strand new content behind a stale one. */
    bool panel_dirty;

    /* Log-once guards (reset each start; warned_pacer is the pacer's). */
    bool warned_pacer;
    bool warned_beginframe;
    bool warned_endframe;
    bool warned_acquire;
    bool warned_wait_timeout;
    bool warned_wait_fail;
    bool warned_release;
    bool warned_fence_timeout;
    bool warned_swapchain;
    bool warned_panel;                /* panel chain/staging/cmd creation */
    bool warned_panel_acquire;
    bool warned_panel_wait;
    bool warned_panel_release;
    bool warned_panel_upload;
} VRCompositor;

/*
 * Tier-3 head-look driver + hunt state (vr_camera.c). pfifo thread only,
 * except hunt_requested (UI thread sets, qatomic).
 */
typedef struct VRCamera {
    bool hunt_requested;              /* qatomic; F9 */
    uint32_t hunt_count;

    /* Cached parses of the [vr] headlook_*_addr config strings. yaw_addr /
     * pitch_addr are ABSOLUTE physical addresses when base_mode==absolute,
     * or OFFSETS from the resolved dynamic base otherwise. gate_addr is
     * always absolute (the gate lives in a stable region). */
    char *yaw_addr_str, *pitch_addr_str, *gate_addr_str;
    uint32_t yaw_addr, pitch_addr, gate_addr;
    bool yaw_addr_valid, pitch_addr_valid, gate_addr_valid;

    /* Dynamic camera base (durable anchoring — resolve_cam_base). The camera
     * is a heap object that relocates each boot; a static pointer or an AOB
     * signature relocates it. base_mode: 0 absolute, 1 pointer, 2 aob. */
    char *base_mode_str, *base_ptr_str, *base_aob_str;   /* change detection */
    int base_mode;
    uint32_t base_ptr; bool base_ptr_valid;              /* pointer mode */
    uint8_t aob_bytes[64], aob_mask[64]; int aob_len;    /* aob: 0xFF=fixed,0=wild */
    bool aob_resolved; uint32_t aob_base; int aob_throttle;

    /* ADS gate: pokes fire only while *(u8 *)gate_addr == gate_value (when
     * a gate is configured). gate_was_on drives the rising-edge zero. */
    bool gate_was_on;

    /* Zeroing (captured at gate rising edge / enable / F8): game angles
     * adopted as base, and the CURRENT head angles as the head reference —
     * so engaging ADS never snaps; deltas count from that moment. */
    bool base_captured;
    float yaw_base, pitch_base;
    float head_ref_yaw, head_ref_pitch;

    /* Compose mode: last values we wrote, to recognize the game's own
     * writes (stick aim) and fold them into the base instead of erasing
     * them — stick and head aim add together. */
    float last_written_yaw, last_written_pitch;
} VRCamera;

typedef struct XemuVRState {
    /* --- bootstrap / lifecycle (pfifo thread) --- */
    bool bootstrap_active;   /* Phase A succeeded; bridge seams are live   */
    bool session_running;    /* Phase B + compositor up; frame hook live   */

    VRXrSession xr;

    VRCompositor comp;

    VRCamera cam;
} XemuVRState;

extern XemuVRState g_xemu_vr;

/* vr_manager.c */
void vr_abort_bootstrap(void);

/* vr_xr_session.c — OpenXR instance/session + Vulkan-through-XR wrappers.
 * All return-false paths log and leave state destroyed/idempotent. */
bool vr_xr_create_instance_and_system(void);
bool vr_xr_query_graphics_requirements(void);
void vr_xr_destroy_instance(void);
VkResult vr_xr_create_vulkan_instance(const VkInstanceCreateInfo *create_info,
                                      VkInstance *out_instance);
VkPhysicalDevice vr_xr_get_vulkan_physical_device(VkInstance instance);
VkResult vr_xr_create_vulkan_device(VkPhysicalDevice physical_device,
                                    const VkDeviceCreateInfo *create_info,
                                    VkDevice *out_device);
bool vr_xr_create_session(PGRAPHState *pg);
void vr_xr_destroy_session(void);
void vr_xr_pump_events(void);
bool vr_xr_session_running(void);

/* vr_xr_compositor.c — pacer thread, XR swapchain, copy, quad layer */
bool vr_compositor_start(PGRAPHState *pg);
void vr_compositor_stop(void);
void vr_compositor_frame(PGRAPHState *pg);

/* vr_camera.c — Tier-3 head-look pokes + F9 RAM-hunt dumps */
void vr_camera_frame(PGRAPHState *pg);      /* head-look poke — VR-gated */
void vr_camera_hunt_poll(PGRAPHState *pg);  /* F9 RAM dump — VR-INDEPENDENT */
void vr_camera_reset(void);
void vr_camera_request_hunt_dump(void);
void vr_camera_shutdown(void);

#endif /* HW_XBOX_NV2A_PGRAPH_VK_VR_INTERNAL_H */
