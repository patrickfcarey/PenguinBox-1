/*
 * PenguinBox per-game runtime performance hacks
 *
 * Steel Battalion radar-scan temporal downsampling (perf Tier-1).
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
 *
 * ---------------------------------------------------------------------------
 * DESIGN (why it is correct)
 *
 * Mechanism: a host-side QEMU CPU breakpoint (BP_CPU) at each of the four
 * scanner entry EIPs.  We do NOT patch guest bytes (that would contaminate
 * savestates and churn TB invalidation); breakpoints are host-only and
 * savestate-clean.
 *
 * Cadence via toggle, NOT always-installed: the breakpoints are inserted only
 * for SKIP frames and removed for SCAN frames.  This matters for performance:
 * a live CPU breakpoint de-optimises its entire 4 KiB code page (every insn in
 * the page single-steps via CF_BP_PAGE).  The scanners' hot byte-scan loops
 * live in those pages, so an always-installed breakpoint would single-step the
 * scanner body on scan frames -- a periodic multi-millisecond stutter.  By
 * removing the breakpoints on scan frames the scanner runs fully optimised, and
 * on skip frames its body never executes at all (we ret-emulate at entry).
 *
 * Why per-frame insert/remove is cheap AND takes effect without a TB flush:
 * cpu_breakpoint_insert()/remove() here are just linked-list ops (no flush --
 * verified in cpu-common.c).  A newly inserted breakpoint would normally be
 * bypassed by an already-chained TB, BUT every call to a scanner is a
 * CROSS-PAGE `call` from the HUD dispatcher (0x45xxx -> 0x25/26/27xxx), and
 * i386 goto_tb chaining is same-page only (translator_use_goto_tb() ->
 * translator_is_same_page()).  Cross-page control transfers always route
 * through helper_lookup_tb_ptr()/the main loop, both of which call
 * check_for_breakpoints() before executing.  So an insert/remove is observed on
 * the very next scanner call with no flush.  (If this analysis were ever wrong
 * the failure mode is benign: the skip simply would not happen -- never a
 * crash.)
 *
 * Why plain `ret` emulation is exact (verified by static disassembly of
 * default.xbe, title 0x43430002): all four scanners end in `ret` (0xC3, NOT
 * `ret imm16`), take zero arguments (their sole call sites push nothing and do
 * no post-call `add esp`), preserve ebx/esi/edi/ebp, and the dispatcher ignores
 * their return value (the instruction after each call never reads eax).  So
 * emulating `eip = *(u32*)esp; esp += 4` reproduces the exact (eip, esp) a real
 * return would leave, and leaving the other registers untouched is safe.
 *
 * Threading: the cadence decision is taken on the pfifo thread (frame tick) and
 * published as a plain atomic int the trap handler reads; racy-by-a-frame is
 * fine.  Breakpoint list mutation must be serialised against the vCPU's
 * traversal, so it is done via async_safe_run_on_cpu() (same pattern as
 * mem_access_callback_insert()).
 *
 * Title gating and savestates: hooks install only when the resident XBE's
 * cert title-id == 0x43430002.  Breakpoints are host-only so they never enter a
 * savestate; every decision is recomputed per frame so loadvm mid-session is
 * safe.  OFF (N<2) => zero breakpoints => byte-identical to stock.
 */

#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/bswap.h"           /* ldl_le_p */
#include "cpu.h"                  /* CPUX86State, X86_CPU, RF_MASK, R_ESP/R_SS */
#include "exec/cpu-common.h"      /* cpu_loop_exit_noexc */
#include "hw/core/cpu.h"          /* cpu_breakpoint_*, async_safe_run_on_cpu,
                                     cpu_memory_rw_debug, BP_CPU, qemu_get_cpu */
#include "hw/xbox/nv2a/debug.h"   /* nv2a_profile_inc_counter, g_nv2a_stats */
#include "hw/xbox/xbox-game-hacks.h"

#define SB_TITLE_ID          0x43430002u
#define XBE_MAGIC            0x48454258u /* 'XBEH' */
#define XBE_HDR_VADDR        0x00010000u
#define XBE_HDR_CERT_OFF     0x118u      /* offsetof(xbe_header, m_certificate_addr) */
#define XBE_CERT_TITLEID_OFF 0x8u        /* offsetof(xbe_certificate, m_titleid) */

/* The four radar scan-AND-draw routine entry EIPs (fixed XBE vaddrs). */
static const uint32_t g_scanner_eip[4] = {
    0x25d40, 0x26460, 0x26b80, 0x27250,
};

/* ------- cross-thread state -------------------------------------------- */
/* Written by the frame tick (pfifo), read by the trap handler (vCPU). */
static int g_skip_this_frame;    /* atomic: 1 => skip scanners this frame */
/* Latched by the install callback once the title is known not to be SB, read
 * by the frame tick to stop re-arming forever on the wrong title. */
static int g_disabled;           /* atomic */

/* ------- pfifo-thread-only state --------------------------------------- */
static bool g_want_installed;    /* last cadence intent we scheduled */

/* ------- vCPU-thread-only state (async_safe_run_on_cpu callbacks) ------- */
static int g_title_state = -1;   /* -1 unknown, 0 not-SB, 1 SB */

/*
 * NB: we deliberately do NOT cache CPUBreakpoint* handles.  x86 reset
 * (x86_cpu_reset) and vmstate load (cpu_post_load) both call
 * cpu_breakpoint_remove_all(cs, BP_CPU) and g_free() our objects out from under
 * us, so cached pointers would dangle.  The install/remove callbacks are
 * therefore idempotent and address-keyed: after such a wipe the feature simply
 * re-arms on the next scan<->skip transition (immediate for the N=2/3 the owner
 * tests with).
 */

/* ---------------------------------------------------------------------- */

/* Read the live cadence N.  env XEMU_RADAR_SKIP_N is the initial/default value;
 * the watch file (path from XEMU_RADAR_SKIP_FILE, default /tmp/radar-skip-n)
 * live-overrides it when present.  N<2 means OFF. */
static int radar_skip_read_n(void)
{
    static int env_n = -1;
    static const char *watch;

    if (env_n < 0) {
        const char *e = getenv("XEMU_RADAR_SKIP_N");
        env_n = (e && atoi(e) > 0) ? atoi(e) : 0;
        watch = getenv("XEMU_RADAR_SKIP_FILE");
        if (!watch) {
            watch = "/tmp/radar-skip-n";
        }
    }

    int n = env_n;
    FILE *f = fopen(watch, "r");
    if (f) {
        int v;
        if (fscanf(f, "%d", &v) == 1) {
            n = v;
        }
        fclose(f);
    }
    return n;
}

/* Read a little-endian guest u32 at a virtual address (current CPU page
 * tables).  Returns false if the address is not currently mapped. */
static bool read_guest_u32(CPUState *cs, uint32_t vaddr, uint32_t *out)
{
    uint8_t b[4];
    if (cpu_memory_rw_debug(cs, vaddr, b, sizeof(b), false) != 0) {
        return false;
    }
    *out = (uint32_t)ldl_le_p(b);
    return true;
}

/* Probe the resident XBE cert title-id directly from guest memory (avoids the
 * shared-static reentrancy of xemu_get_xbe_info()).  Returns 1 = SB, 0 = other
 * title, -1 = XBE not resident yet (retry later). */
static int probe_title_is_sb(CPUState *cs)
{
    uint32_t magic, cert_va, title;

    if (!read_guest_u32(cs, XBE_HDR_VADDR, &magic) || magic != XBE_MAGIC) {
        return -1;
    }
    if (!read_guest_u32(cs, XBE_HDR_VADDR + XBE_HDR_CERT_OFF, &cert_va) ||
        cert_va == 0) {
        return -1;
    }
    if (!read_guest_u32(cs, cert_va + XBE_CERT_TITLEID_OFF, &title)) {
        return -1;
    }
    return (title == SB_TITLE_ID) ? 1 : 0;
}

/* async_safe callback (vCPU, exclusive context): arm the four breakpoints. */
static void radar_install_cb(CPUState *cs, run_on_cpu_data data)
{
    if (g_title_state < 0) {
        g_title_state = probe_title_is_sb(cs);
        if (g_title_state == 0) {
            qatomic_set(&g_disabled, 1); /* wrong title: never arm again */
            fprintf(stderr, "radar-skip: loaded title is not Steel Battalion "
                            "(0x43430002); disabled\n");
            return;
        }
        if (g_title_state < 0) {
            return; /* XBE not resident yet -- retry on the next transition */
        }
        fprintf(stderr, "radar-skip: Steel Battalion detected; "
                        "radar-scan breakpoints armed\n");
    }
    if (g_title_state != 1) {
        return;
    }
    for (int i = 0; i < 4; i++) {
        /* Idempotent: drop any stale copy (double-schedule, or a survivor of a
         * partial wipe) then add exactly one.  BP_CPU (not BP_GDB) so gdbstub
         * is unaffected and our handler runs before breakpoint_handler's
         * guest-#DB injection. */
        cpu_breakpoint_remove(cs, g_scanner_eip[i], BP_CPU);
        cpu_breakpoint_insert(cs, g_scanner_eip[i], BP_CPU, NULL);
    }
}

/* async_safe callback (vCPU, exclusive context): disarm the four breakpoints. */
static void radar_remove_cb(CPUState *cs, run_on_cpu_data data)
{
    for (int i = 0; i < 4; i++) {
        cpu_breakpoint_remove(cs, g_scanner_eip[i], BP_CPU); /* -ENOENT ok */
    }
}

void xbox_game_hacks_frame_tick(void)
{
    int n = radar_skip_read_n();

    /* One stderr line whenever the requested cadence changes, so the owner can
     * confirm `echo N > /tmp/radar-skip-n` registered during a live A/B. */
    static int last_n = INT_MIN;
    if (n != last_n) {
        fprintf(stderr, "radar-skip: N=%d -> %s\n",
                n, (n >= 2) ? "ON (scan 1 frame in N)" : "OFF");
        last_n = n;
    }

    if (qatomic_read(&g_disabled)) {
        n = 0; /* known non-SB title: force OFF for the rest of the session */
    }

    bool active = (n >= 2);
    unsigned fc = g_nv2a_stats.frame_count;
    /* Scan on frames where fc % N == 0, skip otherwise. */
    bool skip = active && ((fc % (unsigned)n) != 0);
    qatomic_set(&g_skip_this_frame, skip ? 1 : 0);

    /* Breakpoints are armed only on skip frames (see design note above). */
    bool want = skip;
    if (want != g_want_installed) {
        CPUState *cs = qemu_get_cpu(0); /* Xbox is single-CPU */
        if (cs) {
            async_safe_run_on_cpu(cs,
                                  want ? radar_install_cb : radar_remove_cb,
                                  RUN_ON_CPU_NULL);
            g_want_installed = want;
        }
    }
}

void xbox_game_hacks_maybe_skip_scanner(CPUState *cs)
{
    /* Data watchpoint hit (e.g. the radar surface read callbacks), not our
     * instruction breakpoint -- leave it for the normal handler. */
    if (cs->watchpoint_hit) {
        return;
    }

    CPUX86State *env = &X86_CPU(cs)->env;
    uint32_t eip = env->eip;

    bool ours = false;
    for (int i = 0; i < 4; i++) {
        if (eip == g_scanner_eip[i]) {
            ours = true;
            break;
        }
    }
    if (!ours) {
        return; /* gdb / guest-DR breakpoint: let breakpoint_handler run it */
    }

    if (qatomic_read(&g_skip_this_frame)) {
        /* Emulate the scanner's `ret`: pop the return EIP the caller pushed and
         * drop that stack slot.  Verified exact for these cdecl/zero-arg
         * routines (they end in `ret`, 0xC3). */
        uint32_t esp = (uint32_t)env->regs[R_ESP];
        vaddr sp_lin = env->segs[R_SS].base + esp; /* flat SS on Xbox => esp */
        uint8_t b[4];

        if (cpu_memory_rw_debug(cs, sp_lin, b, sizeof(b), false) == 0) {
            env->eip = (uint32_t)ldl_le_p(b);
            env->regs[R_ESP] = esp + 4;
            nv2a_profile_inc_counter(NV2A_PROF_RADAR_SCAN_SKIP);
        } else {
            /* Return address unreadable (never expected) -- fail safe: run the
             * scanner instead of corrupting EIP. */
            env->eflags |= RF_MASK;
        }
    } else {
        /* Breakpoint still armed on a scan frame (cadence-transition race):
         * step over our own breakpoint so the scanner executes normally.  RF
         * suppresses the CPU breakpoint for exactly the next instruction
         * (x86_debug_check_breakpoint) and the translator clears it (gen_eob),
         * so the rest of the scanner runs unhindered. */
        env->eflags |= RF_MASK;
    }

    cpu_loop_exit_noexc(cs); /* resume the guest at env->eip; does not return */
}
