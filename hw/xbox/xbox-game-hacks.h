/*
 * PenguinBox per-game runtime performance hacks
 *
 * Copyright (c) 2026 Patrick Carey
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 */

#ifndef HW_XBOX_GAME_HACKS_H
#define HW_XBOX_GAME_HACKS_H

#include "qemu/typedefs.h" /* CPUState */

/*
 * Steel Battalion radar-scan temporal downsampling (perf Tier-1).
 *
 * The game runs FOUR near-identical scan-AND-draw routines once per frame each
 * (guest EIPs 0x25d40 / 0x26460 / 0x26b80 / 0x27250) that walk the 128x128
 * GPU-rendered radar buffer byte-by-byte and immediately emit D3D blip draws.
 * That walk is a dominant chunk of the ~93%-busy TCG guest thread.  We skip the
 * routines on N-1 of every N frames via a host-side QEMU breakpoint that
 * emulates each routine's `ret` at entry, so blips refresh at 30/N Hz while the
 * guest work disappears.
 *
 * Everything here is INERT unless the loaded title is Steel Battalion
 * (title-id 0x43430002) AND the cadence N (>=2) has been armed.  With N absent
 * / 0 / 1 the feature is OFF and no breakpoints exist, so behaviour is
 * byte-identical to stock.  See xbox-game-hacks.c for the full design notes.
 */

/*
 * Per-frame tick.  Called from the NV2A flip site (pfifo thread).  Re-reads the
 * cadence (env XEMU_RADAR_SKIP_N as the initial value, watch file
 * /tmp/radar-skip-n live-overriding it), decides whether the UPCOMING frame is
 * a scan frame or a skip frame, and arms/disarms the guest-EIP breakpoints
 * accordingly via async_safe_run_on_cpu().
 */
void xbox_game_hacks_frame_tick(void);

/*
 * Called at the very top of the i386 breakpoint handler (vCPU thread) for every
 * BP_CPU hit.  If env->eip is one of the four radar scanners:
 *   - on a skip frame it emulates the scanner's `ret` (pop EIP, esp += 4) and
 *     resumes the guest WITHOUT running the scanner (does not return);
 *   - on a scan frame (only reachable during a cadence-transition race) it sets
 *     EFLAGS.RF to step over our own breakpoint so the scanner runs, then
 *     resumes (does not return).
 * For any other EIP it returns immediately and lets the normal handler run, so
 * gdb / guest debug-register breakpoints are unaffected.
 */
void xbox_game_hacks_maybe_skip_scanner(CPUState *cs);

#endif /* HW_XBOX_GAME_HACKS_H */
