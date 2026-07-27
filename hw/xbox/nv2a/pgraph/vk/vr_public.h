/*
 * xemu-VR — UI-safe public surface (no target headers)
 *
 * SPDX-FileCopyrightText: 2026 Patrick Carey
 * SPDX-License-Identifier: LGPL-2.0-or-later
 *
 * Includable from UI translation units (ui/xemu.c and the ui/xui sources),
 * which cannot see target-specific headers like pgraph.h/cpu.h. The
 * renderer-facing API lives in vr.h (which includes this file).
 */

#ifndef HW_XBOX_NV2A_PGRAPH_VK_VR_PUBLIC_H
#define HW_XBOX_NV2A_PGRAPH_VK_VR_PUBLIC_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* UI-thread-safe: request a screen re-anchor to the current heading + eye
 * height (F8, sibling convention); consumed by the pfifo thread at the
 * next VR frame. No-op when VR is inactive. */
void xemu_vr_request_recenter(void);

/* UI-thread-safe status read: true while the VR session + compositor are
 * live (for HUD display only — not a synchronization primitive). */
bool xemu_vr_active(void);

/* UI-thread-safe (F9): request a Tier-3 hunt dump (full guest-RAM snapshot
 * to ~/xemu-vr-hunt/). Consumed only when [vr] hunt_enable is set. */
void xemu_vr_request_hunt_dump(void);

#ifdef __cplusplus
}
#endif

#endif /* HW_XBOX_NV2A_PGRAPH_VK_VR_PUBLIC_H */
