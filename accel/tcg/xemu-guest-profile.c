/*
 * PenguinBox — statistical guest-CPU (TCG) PC profiler (implementation).
 *
 * WHY THIS EXISTS
 * ---------------
 * Steel Battalion's guest-CPU (TCG) thread is ~94% busy (~31 ms of executed
 * guest instructions per frame).  To attack that number we must first know
 * where in the *guest* it is spent: game logic vs the statically-linked XDK
 * Direct3D8 driver vs busy-wait spin loops vs the radar scan.  This module is
 * that instrument: a periodic statistical sampler of the guest program
 * counter, symbolised offline against the game's XBE
 * (tools/perf/guest_profile_symbolize.py).
 *
 * MECHANISM (and why it is correct)
 * ---------------------------------
 * The sample point is cpu_loop_exec_tb() in accel/tcg/cpu-exec.c, i.e. ON the
 * vCPU thread, at a TB boundary, immediately before the block executes.  The
 * guest PC passed in is TCGTBCPUState.pc, which x86 get_tb_cpu_state() computes
 * fresh from the *current* env each iteration (cs_base + eip); on the Xbox the
 * flat protected-mode segments have base 0, so it equals env->eip and maps 1:1
 * onto XBE virtual addresses.  Sampling here — not from a QEMUTimer — is the
 * whole point: a timer fires on the timer/main thread and would read another
 * thread's env->eip concurrently with the vCPU updating it (a torn, meaningless
 * PC and a data race).  Being on the vCPU thread, the read is race-free by
 * construction.
 *
 * TB CHAINING.  With goto_tb / goto_ptr chaining, a hot loop runs entirely in
 * generated host code and never returns to cpu_loop_exec_tb — so a naive
 * counter there would *under*-sample exactly the hottest code (and the tight
 * spin loops we most want to find).  To make the sample distribution faithful,
 * curr_cflags() forces CF_NO_GOTO_TB | CF_NO_GOTO_PTR whenever the profiler is
 * armed, so every TB returns to the loop and is eligible for sampling — the
 * same mechanism cpu_exec_step_atomic() relies on to run exactly one TB.  This
 * perturbs absolute speed (chaining is a host optimisation) but that is the
 * standard, accepted profiling trade-off and it makes the *ranking* — the
 * deliverable — reflect real guest work.
 *
 * CONCURRENCY.  The ring is written AND read back only by the vCPU thread, so
 * there is no producer/consumer race on it at all — deliberately simpler and
 * more reliable than a SPSC/seqlock across threads.  The only cross-thread
 * datum is gp_dump_request, a single atomic flag: the pgraph/pfifo thread sets
 * it from the frame-flip hook, and the vCPU thread notices it at the next
 * sample and performs the flush itself.  The flush therefore causes a brief
 * hitch on the vCPU thread every few hundred frames, which is fine for a
 * measurement capture.
 *
 * INERTNESS.  Unless XEMU_GUEST_PROFILE is set, xemu_gp_enabled stays false:
 * curr_cflags() adds nothing, cpu_loop_exec_tb() takes one never-taken branch,
 * and no state is touched — behaviour is bit-identical to upstream, including
 * under icount/replay.  (Do NOT combine the profiler with -icount / record /
 * replay: forcing no-chain changes TB structure and timing.)
 *
 * SCOPE / HONESTY.  This samples where *guest* code executes.  It does not, and
 * cannot by construction, attribute *host*-side emulator overhead — TB
 * re-translation or MMIO-trap emulation cost — because those are not a guest
 * EIP.  It answers "which guest functions run the most, and how much of that is
 * spinning", which covers game logic, the D3D8 driver, the radar, and spin
 * waits; re-translation and MMIO overhead need separate host-side timers.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"

#ifdef XBOX

#include "qemu/atomic.h"
#include "exec/xemu-guest-profile.h"

/* ---- configuration (cached once at startup) ---------------------------- */

bool xemu_gp_enabled;                 /* armed? read by the hot paths */
static uint32_t gp_interval = 4096;   /* record 1 sample per this many TBs */
static uint32_t gp_dump_flips = 300;  /* flush the ring every this many flips */
static const char *gp_path = "guest-profile.bin"; /* output file */

/* ---- the sample ring (vCPU thread private) ----------------------------- */

typedef struct GpSample {
    uint32_t eip;   /* guest linear PC at the TB head */
    uint32_t idx;   /* monotonic 1-based sample index (0 == empty slot) */
} GpSample;

#define GP_RING_BITS 20
#define GP_RING_SIZE (1u << GP_RING_BITS)
#define GP_RING_MASK (GP_RING_SIZE - 1u)

static GpSample gp_ring[GP_RING_SIZE]; /* fixed, zero-init, never malloc'd */
static uint64_t gp_head;               /* total samples ever written */
static uint32_t gp_counter;            /* TBs seen since last sample */
static uint32_t gp_sample_idx;         /* value stamped into GpSample.idx */

/* ---- cross-thread flush trigger ---------------------------------------- */

static uint32_t gp_flip_count;         /* pgraph-thread private */
static int gp_dump_request;            /* atomic: flip thread -> vCPU thread */
static unsigned gp_dump_seq;           /* how many flushes done (vCPU thread) */

/* ---- flush (runs on the vCPU thread) ----------------------------------- */

static void xemu_gp_dump(void)
{
    uint64_t total = gp_head;
    uint64_t count = total < GP_RING_SIZE ? total : GP_RING_SIZE;
    g_autofree char *tmp = NULL;
    FILE *fp;

    if (count == 0) {
        return;
    }

    /* Write to a sibling temp file then rename, so a kill mid-flush never
     * leaves a truncated capture in place. */
    tmp = g_strdup_printf("%s.tmp", gp_path);
    fp = fopen(tmp, "wb");
    if (!fp) {
        fprintf(stderr, "[guest-profile] cannot open %s: %s\n",
                tmp, strerror(errno));
        return;
    }

    /* Emit oldest -> newest so the offline tool sees chronological order for
     * spin-run detection.  At most two contiguous spans across the wrap. */
    if (total < GP_RING_SIZE) {
        fwrite(gp_ring, sizeof(GpSample), total, fp);
    } else {
        uint32_t head_slot = (uint32_t)(total & GP_RING_MASK); /* == oldest */
        fwrite(&gp_ring[head_slot], sizeof(GpSample),
               GP_RING_SIZE - head_slot, fp);
        fwrite(&gp_ring[0], sizeof(GpSample), head_slot, fp);
    }

    fflush(fp);
    fclose(fp);

    if (rename(tmp, gp_path) != 0) {
        fprintf(stderr, "[guest-profile] rename %s -> %s failed: %s\n",
                tmp, gp_path, strerror(errno));
        return;
    }

    fprintf(stderr,
            "[guest-profile] flush #%u: %" PRIu64 " samples (of %" PRIu64
            " total, interval=%u) -> %s\n",
            ++gp_dump_seq, count, total, gp_interval, gp_path);
}

/* ---- per-TB hook (runs on the vCPU thread) ----------------------------- */

void xemu_guest_profile_tb(uint32_t eip)
{
    /* Common path: just count. Only every gp_interval-th TB do more. */
    if (++gp_counter < gp_interval) {
        return;
    }
    gp_counter = 0;

    gp_ring[gp_head & GP_RING_MASK] = (GpSample){
        .eip = eip,
        .idx = ++gp_sample_idx,
    };
    gp_head++;

    /* Service a flush request here (sample cadence), on this thread, so the
     * ring is never touched cross-thread. */
    if (unlikely(qatomic_read(&gp_dump_request))) {
        qatomic_set(&gp_dump_request, 0);
        xemu_gp_dump();
    }
}

/* ---- frame-flip trigger (runs on the pgraph/pfifo thread) --------------- */

void xemu_guest_profile_flip(void)
{
    if (!xemu_gp_enabled) {
        return;
    }
    if (++gp_flip_count >= gp_dump_flips) {
        gp_flip_count = 0;
        qatomic_set(&gp_dump_request, 1);
    }
}

/* ---- one-time arming from the environment ------------------------------ */

static void __attribute__((constructor)) xemu_gp_init(void)
{
    const char *e = getenv("XEMU_GUEST_PROFILE");
    const char *v;
    int n;

    if (!e || !e[0]) {
        return;                       /* disarmed: stay completely inert */
    }

    n = atoi(e);
    if (n > 0) {
        gp_interval = (uint32_t)n;    /* XEMU_GUEST_PROFILE == interval N */
    }

    v = getenv("XEMU_GUEST_PROFILE_DUMP_FLIPS");
    if (v) {
        n = atoi(v);
        if (n > 0) {
            gp_dump_flips = (uint32_t)n;
        }
    }

    v = getenv("XEMU_GUEST_PROFILE_PATH");
    if (v && v[0]) {
        gp_path = v;
    }

    xemu_gp_enabled = true;
    fprintf(stderr,
            "[guest-profile] ARMED: interval=%u TBs, flush every %u flips, "
            "out=%s (chaining forced off while armed)\n",
            gp_interval, gp_dump_flips, gp_path);
}

#endif /* XBOX */
