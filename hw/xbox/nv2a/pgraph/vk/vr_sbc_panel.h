/*
 * xemu-VR — Steel Battalion in-headset cockpit console panel (public contract)
 *
 * SPDX-FileCopyrightText: 2026 Patrick Carey
 * SPDX-License-Identifier: LGPL-2.0-or-later
 *
 * The CPU-rastered content side of the VR cockpit console: a fixed-size RGBA
 * surface the compositor uploads to a dedicated panel swapchain (a second
 * world-locked quad beside the Tier-1 screen). This header is the whole
 * contract between three callers:
 *
 *   - the compositor (pfifo thread) calls vr_sbc_panel_frame() to obtain the
 *     current raster and whether the panel should be shown;
 *   - the UI thread calls vr_sbc_panel_publish() once per poll, right after
 *     xemu_input_update_controllers(), to snapshot the live SBC state;
 *   - future Touch/pointer input walks vr_sbc_panel_cells() to hit-test and
 *     calls vr_sbc_panel_inject() (a WIP stub today — see vr_sbc_panel.c).
 *
 * It is intentionally UI-safe (no target headers): includable from ui/xemu.c
 * and the pfifo-side vr_* sources alike, exactly like vr_public.h.
 */

#ifndef HW_XBOX_NV2A_PGRAPH_VK_VR_SBC_PANEL_H
#define HW_XBOX_NV2A_PGRAPH_VK_VR_SBC_PANEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed panel dimensions (the compositor's dedicated panel swapchain is sized
 * to match). The caller's RGBA buffer is VR_SBC_PANEL_W * VR_SBC_PANEL_H * 4
 * bytes, top-down, R8G8B8A8, SRGB byte values, straight alpha. */
#define VR_SBC_PANEL_W 768
#define VR_SBC_PANEL_H 256

/*
 * Raster the panel into `rgba` (caller-owned, VR_SBC_PANEL_W*VR_SBC_PANEL_H*4
 * bytes). Runs on the pfifo thread. Consumes the latest UI-thread snapshot;
 * if nothing changed since the last raster, leaves `rgba` untouched and
 * returns false (the caller reuses its last upload). `*active` is always set
 * (when non-NULL) to whether the panel should be composited this frame — a
 * VR run with no keyboard-fed Steel Battalion controller reports inactive.
 *
 * Returns true when `rgba` was (re)written and needs re-uploading.
 * Does no dynamic allocation and makes no SDL/GL/Vulkan calls.
 */
bool vr_sbc_panel_frame(uint8_t *rgba, bool *active);

/*
 * Snapshot the keyboard-fed Steel Battalion state + resolved key labels into
 * the compositor-visible slot. Runs on the UI thread; call once per poll,
 * after xemu_input_update_controllers(). Cheap and idempotent: an off-state
 * run (VR disabled, or no SBC bound) does near-zero work.
 */
void vr_sbc_panel_publish(void);

/* ---- interaction boundary (WIP; see the big comment on vr_sbc_panel_inject) */

/*
 * One rendered control cell, for future pointer/Touch hit-testing. The rect is
 * in panel pixels (top-left origin). `button_mask` is an SBC_BUTTON_* bit, or 0
 * for pure-axis cells (pedals / aim / rotation). `scancode` points at the
 * generated g_config keyboard-map field that drives the cell, or NULL when the
 * cell has no keyboard binding (mouse-hardwired weapons, mouse aim).
 */
typedef struct VrSbcPanelCell {
    const char *id;        /* stable identifier, e.g. "eject", "toggle.filt" */
    uint16_t    x, y, w, h;/* hit rect, panel pixels                         */
    uint64_t    button_mask;/* SBC_BUTTON_* mask, or 0 for axis cells         */
    int        *scancode;  /* &g_config keyboard-map field, or NULL           */
} VrSbcPanelCell;

/* The full const cell table (stable for the process lifetime). `*count`
 * receives the number of cells. */
const VrSbcPanelCell *vr_sbc_panel_cells(size_t *count);

/* Inject a press/release for a cell by id. WIP LOG-ONLY STUB today — the real
 * input path is deliberate future scope (see vr_sbc_panel.c). */
void vr_sbc_panel_inject(const char *cell_id, bool pressed);

#ifdef __cplusplus
}
#endif

#endif /* HW_XBOX_NV2A_PGRAPH_VK_VR_SBC_PANEL_H */
