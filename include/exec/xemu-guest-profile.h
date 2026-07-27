/*
 * PenguinBox — statistical guest-CPU (TCG) PC profiler.
 *
 * A low-overhead, opt-in sampler that records the guest EIP at translation-
 * block boundaries on the vCPU thread, into a fixed lock-free ring, and dumps
 * it to a file for offline symbolisation against the game's XBE.  See the
 * header of accel/tcg/xemu-guest-profile.c for the mechanism and concurrency
 * model, and tools/perf/README.md ("guest-PC profiler") for the rig recipe.
 *
 * Everything here is compiled only for the Xbox (XBOX) build and is completely
 * inert unless the environment variable XEMU_GUEST_PROFILE is set.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */
#ifndef EXEC_XEMU_GUEST_PROFILE_H
#define EXEC_XEMU_GUEST_PROFILE_H

#ifdef XBOX

/*
 * Cached "profiler armed?" flag.  Written once, before main(), by the module
 * constructor from getenv("XEMU_GUEST_PROFILE"); read-only thereafter.  The
 * hot paths (cpu_loop_exec_tb, curr_cflags) gate on this so the OFF cost is a
 * single predictable, never-taken branch.
 */
extern bool xemu_gp_enabled;

/*
 * Per-TB sample hook.  MUST be called on the vCPU thread only, at a TB
 * boundary, with @eip == the guest linear PC of the block about to execute
 * (i.e. the low 32 bits of TCGTBCPUState.pc; on the Xbox cs_base is 0 so this
 * equals env->eip and maps 1:1 onto XBE virtual addresses).  Callers must gate
 * on xemu_gp_enabled first.
 */
void xemu_guest_profile_tb(uint32_t eip);

/*
 * Frame-flip trigger.  Called once per NV2A FLIP_STALL (frame boundary) from
 * the pgraph/pfifo thread.  Every XEMU_GUEST_PROFILE_DUMP_FLIPS flips it asks
 * the vCPU thread to flush the ring; it never touches the ring itself.  Safe
 * to call unconditionally (no-ops when the profiler is disarmed).
 */
void xemu_guest_profile_flip(void);

#endif /* XBOX */

#endif /* EXEC_XEMU_GUEST_PROFILE_H */
