/*
 * xemu-VR — Tier-3 head-look camera driver + RAM-hunt support
 *
 * SPDX-FileCopyrightText: 2026 Patrick Carey
 * SPDX-License-Identifier: LGPL-2.0-or-later
 *
 * Design: retro-vr-framework doc 10 §2 Tier 3 (find-and-poke) adapted to
 * xemu. The Xbox's 64 MB is unified and directly host-mapped
 * (d->vram_ptr), so both the HUNT and the POKE operate on PHYSICAL
 * addresses — no VA translation, and addresses found by diffing dumps are
 * exactly the addresses we poke. MVP scope: two float pokes (yaw, pitch)
 * configured via [vr] headlook_* keys; the per-game profile DB port comes
 * later once the first title (OFP: Elite) proves the loop.
 *
 * Semantics (freelook-style): at (re)zero — session start and every F8
 * recenter — capture the game's current yaw/pitch as the base; each frame
 * write base + head_delta * sensitivity. Head deltas are measured against
 * the same recenter anchor the screen uses, so F8 means "this is my new
 * forward" for both the screen and the game camera at once.
 *
 * HUNT (config vr.hunt_enable): F9 snapshots the full 64 MB of guest RAM
 * plus a metadata line (head yaw/pitch at dump time) into
 * ~/xemu-vr-hunt/dump-NNN.{bin,txt}. The memcpy runs on the pfifo thread
 * (~10 ms hitch — acceptable while hunting); the file write happens on a
 * detached worker thread.
 *
 * All functions here run on the pfifo thread (called from
 * vr_compositor_frame after the head pose is located), except the
 * qatomic request flags set by the UI thread.
 */

#include "qemu/osdep.h"
#include <math.h>
#include "qemu/thread.h"
#include "ui/xemu-settings.h"
#include "hw/xbox/nv2a/nv2a_int.h"
#include "renderer.h"
#include "vr.h"
#include "vr_internal.h"

#define VR_HUNT_DIR_FMT    "%s/xemu-vr-hunt"
#define WRAP_PI(a) \
    ((a) > (float)M_PI  ? (a) - 2.0f * (float)M_PI : \
     (a) < -(float)M_PI ? (a) + 2.0f * (float)M_PI : (a))

/* Full modulo wrap into [-period/2, +period/2]. Unlike WRAP_PI (a single-step
 * ±pi wrap for head radians), this folds a seam crossing of ANY magnitude —
 * needed for the game's own yaw, whose unit may be degrees (period 360) and
 * whose stick-aim deltas cross the ±180 seam. period<=0 → 2*pi (radians). */
static inline float wrap_period(float x, float period)
{
    if (period <= 0.0f) {
        period = 2.0f * (float)M_PI;
    }
    return x - period * roundf(x / period);
}

typedef struct VRHuntJob {
    void *data;
    size_t size;
    uint32_t index;
    float head_yaw, head_pitch;
} VRHuntJob;

/* ---- address parsing (cached; re-parsed when the config string changes) */

static bool parse_addr(const char *str, uint64_t ram_size, uint32_t *out)
{
    if (str == NULL || str[0] == '\0') {
        return false;
    }
    char *end = NULL;
    unsigned long long v = strtoull(str, &end, 0);
    if (end == NULL || *end != '\0' || v == 0 || (v & 3) ||
        v + sizeof(float) > ram_size) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

static bool parse_gate_addr(const char *str, uint64_t ram_size,
                            uint32_t *out)
{
    /* Gate is a byte compare — no alignment requirement. */
    if (str == NULL || str[0] == '\0') {
        return false;
    }
    char *end = NULL;
    unsigned long long v = strtoull(str, &end, 0);
    if (end == NULL || *end != '\0' || v == 0 || v >= ram_size) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

static void refresh_addrs(VRCamera *cam, uint64_t ram_size)
{
    const char *ys = g_config.vr.headlook_yaw_addr;
    const char *ps = g_config.vr.headlook_pitch_addr;
    const char *gs = g_config.vr.headlook_gate_addr;

    if (g_strcmp0(gs, cam->gate_addr_str) != 0) {
        g_free(cam->gate_addr_str);
        cam->gate_addr_str = g_strdup(gs);
        cam->gate_addr_valid = parse_gate_addr(gs, ram_size,
                                               &cam->gate_addr);
        cam->gate_was_on = false;
        if (gs && gs[0] && !cam->gate_addr_valid) {
            fprintf(stderr, "xemu-vr: headlook_gate_addr '%s' invalid\n",
                    gs);
        }
    }

    if (g_strcmp0(ys, cam->yaw_addr_str) != 0) {
        g_free(cam->yaw_addr_str);
        cam->yaw_addr_str = g_strdup(ys);
        cam->yaw_addr_valid = parse_addr(ys, ram_size, &cam->yaw_addr);
        cam->base_captured = false;
        if (ys && ys[0] && !cam->yaw_addr_valid) {
            fprintf(stderr, "xemu-vr: headlook_yaw_addr '%s' invalid "
                            "(need 4-aligned address < 0x%" PRIx64 ")\n",
                    ys, ram_size);
        }
    }
    if (g_strcmp0(ps, cam->pitch_addr_str) != 0) {
        g_free(cam->pitch_addr_str);
        cam->pitch_addr_str = g_strdup(ps);
        cam->pitch_addr_valid = parse_addr(ps, ram_size, &cam->pitch_addr);
        cam->base_captured = false;
        if (ps && ps[0] && !cam->pitch_addr_valid) {
            fprintf(stderr, "xemu-vr: headlook_pitch_addr '%s' invalid\n",
                    ps);
        }
    }
}

/* ---- dynamic camera base (durable anchoring across boots) ----
 * The camera is a heap object that relocates every boot, so absolute poke
 * addresses die on reboot. A static POINTER to the struct (stored in the
 * game's fixed image/globals) or an AOB SIGNATURE relocates it each frame.
 * In pointer/aob mode, headlook_yaw_addr/_pitch_addr are OFFSETS from the
 * resolved base; the gate stays absolute. */

static void parse_aob(VRCamera *cam, const char *s)
{
    cam->aob_len = 0;
    if (s == NULL) {
        return;
    }
    while (*s && cam->aob_len < (int)sizeof(cam->aob_bytes)) {
        while (*s == ' ') {
            s++;
        }
        if (!*s) {
            break;
        }
        if (*s == '?') {                        /* wildcard: '?' or '??' */
            cam->aob_bytes[cam->aob_len] = 0;
            cam->aob_mask[cam->aob_len] = 0;
            cam->aob_len++;
            s += (s[1] == '?') ? 2 : 1;
        } else {
            char buf[3] = { s[0], s[1] ? s[1] : '\0', '\0' };
            cam->aob_bytes[cam->aob_len] = (uint8_t)strtoul(buf, NULL, 16);
            cam->aob_mask[cam->aob_len] = 0xFF;
            cam->aob_len++;
            s += buf[1] ? 2 : 1;
        }
    }
}

static bool aob_match_at(const VRCamera *cam, const uint8_t *ram, uint64_t off)
{
    for (int i = 0; i < cam->aob_len; i++) {
        if (cam->aob_mask[i] && ram[off + i] != cam->aob_bytes[i]) {
            return false;
        }
    }
    return true;
}

static void refresh_base(VRCamera *cam, uint64_t ram_size)
{
    const char *ms = g_config.vr.headlook_base_mode;
    const char *ps = g_config.vr.headlook_base_ptr;
    const char *as = g_config.vr.headlook_base_aob;

    if (g_strcmp0(ms, cam->base_mode_str) != 0) {
        g_free(cam->base_mode_str);
        cam->base_mode_str = g_strdup(ms);
        cam->base_mode = (ms && !strcmp(ms, "pointer")) ? 1 :
                         (ms && !strcmp(ms, "aob"))     ? 2 : 0;
        cam->base_captured = false;
        cam->aob_resolved = false;
    }
    if (g_strcmp0(ps, cam->base_ptr_str) != 0) {
        g_free(cam->base_ptr_str);
        cam->base_ptr_str = g_strdup(ps);
        cam->base_ptr_valid = parse_addr(ps, ram_size, &cam->base_ptr);
        cam->base_captured = false;
        if (ps && ps[0] && !cam->base_ptr_valid) {
            fprintf(stderr, "xemu-vr: headlook_base_ptr '%s' invalid\n", ps);
        }
    }
    if (g_strcmp0(as, cam->base_aob_str) != 0) {
        g_free(cam->base_aob_str);
        cam->base_aob_str = g_strdup(as);
        parse_aob(cam, as);
        cam->aob_resolved = false;
        cam->base_captured = false;
    }
}

/* Resolve the camera struct base for this frame. false → base unavailable
 * (menu/loading/relocated/misconfigured) → caller sidelines the poke. In
 * absolute mode base=0 (offsets ARE the addresses). */
static bool resolve_cam_base(VRCamera *cam, NV2AState *d, uint64_t ram,
                             uint32_t *base)
{
    const int32_t boff = g_config.vr.headlook_base_offset;

    if (cam->base_mode == 0) {                       /* absolute */
        *base = 0;
        return true;
    }

    if (cam->base_mode == 1) {                       /* pointer-deref */
        if (!cam->base_ptr_valid || (uint64_t)cam->base_ptr + 4 > ram) {
            return false;
        }
        uint32_t p = *(const uint32_t *)(d->vram_ptr + cam->base_ptr);
        int64_t b = (int64_t)p + boff;
        if (b <= 0 || (b & 3) || (uint64_t)b + 64 > ram) {
            return false;                            /* null/unaligned/OOR */
        }
        int32_t vo = g_config.vr.headlook_base_validate_offset;
        uint32_t ve = (uint32_t)g_config.vr.headlook_base_validate_equals;
        if (vo != 0 || ve != 0) {                    /* optional sanity dword */
            int64_t va = (int64_t)p + vo;
            if (va < 0 || (uint64_t)va + 4 > ram ||
                *(const uint32_t *)(d->vram_ptr + va) != ve) {
                return false;
            }
        }
        *base = (uint32_t)b;
        return true;
    }

    /* aob scan (fallback): cache the match, re-validate cheaply each frame,
     * rescan (throttled) only when the cached match stops matching. */
    if (cam->aob_len == 0) {
        return false;
    }
    if (cam->aob_resolved) {
        int64_t moff = (int64_t)cam->aob_base - boff;
        if (moff >= 0 && (uint64_t)moff + cam->aob_len <= ram &&
            aob_match_at(cam, d->vram_ptr, (uint64_t)moff)) {
            *base = cam->aob_base;
            return true;
        }
        cam->aob_resolved = false;
    }
    if (cam->aob_throttle > 0) {
        cam->aob_throttle--;
        return false;
    }
    uint64_t s = g_config.vr.headlook_base_scan_start;
    uint64_t e = (uint64_t)g_config.vr.headlook_base_scan_end;
    if (e == 0 || e > ram) {
        e = ram;
    }
    const bool first_wild = (cam->aob_mask[0] == 0);
    const uint8_t first = cam->aob_bytes[0];
    uint64_t off = s;
    while (off + cam->aob_len <= e) {
        if (!first_wild) {
            const uint8_t *hit = memchr(d->vram_ptr + off, first,
                                        e - off - cam->aob_len + 1);
            if (hit == NULL) {
                break;
            }
            off = (uint64_t)(hit - d->vram_ptr);
        }
        if (aob_match_at(cam, d->vram_ptr, off)) {
            int64_t b = (int64_t)off + boff;
            if (b > 0 && (uint64_t)b + 64 <= ram) {
                cam->aob_base = (uint32_t)b;
                cam->aob_resolved = true;
                *base = cam->aob_base;
                fprintf(stderr, "xemu-vr: camera AOB resolved: base 0x%08X\n",
                        cam->aob_base);
                return true;
            }
        }
        off++;
    }
    cam->aob_throttle = 60;                           /* not found; retry ~1s */
    return false;
}

/* ---- head angles relative to the recenter anchor ---- */

static void head_angles_rel(const VRCompositor *c, float *yaw, float *pitch)
{
    const XrQuaternionf q = c->head_pose.orientation;
    /* forward = q * (0,0,-1); see the recenter math in vr_xr_compositor.c */
    const float fx = -2.0f * (q.x * q.z + q.w * q.y);
    const float fy = 2.0f * (q.w * q.x - q.y * q.z);
    const float fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));

    float head_yaw = atan2f(-fx, -fz);
    float head_pitch = asinf(fy < -1.0f ? -1.0f : (fy > 1.0f ? 1.0f : fy));

    const XrQuaternionf rq = c->recenter_pose.orientation;
    float anchor_yaw = atan2f(2.0f * (rq.w * rq.y),
                              1.0f - 2.0f * (rq.y * rq.y));

    *yaw = WRAP_PI(head_yaw - anchor_yaw);
    *pitch = head_pitch; /* anchor is yaw-only; pitch is already relative */
}

/* ---- hunt dump ---- */

static void *hunt_write_thread(void *opaque)
{
    VRHuntJob *job = opaque;
    g_autofree char *dir =
        g_strdup_printf(VR_HUNT_DIR_FMT, g_get_home_dir());
    g_mkdir_with_parents(dir, 0755);

    g_autofree char *bin =
        g_strdup_printf("%s/dump-%03u.bin", dir, job->index);
    g_autofree char *txt =
        g_strdup_printf("%s/dump-%03u.txt", dir, job->index);

    FILE *f = fopen(bin, "wb");
    if (f != NULL) {
        fwrite(job->data, 1, job->size, f);
        fclose(f);
    }
    f = fopen(txt, "w");
    if (f != NULL) {
        fprintf(f, "head_yaw_deg=%.2f\nhead_pitch_deg=%.2f\nsize=%zu\n",
                job->head_yaw * (180.0f / (float)M_PI),
                job->head_pitch * (180.0f / (float)M_PI), job->size);
        fclose(f);
    }
    fprintf(stderr, "xemu-vr: hunt dump %03u written (%s).\n", job->index,
            bin);

    g_free(job->data);
    g_free(job);
    return NULL;
}

static void hunt_dump(VRCamera *cam, NV2AState *d, float yaw, float pitch)
{
    const size_t size = memory_region_size(d->vram);

    VRHuntJob *job = g_new0(VRHuntJob, 1);
    job->data = g_malloc(size);
    job->size = size;
    job->index = cam->hunt_count++;
    job->head_yaw = yaw;
    job->head_pitch = pitch;
    memcpy(job->data, d->vram_ptr, size); /* ~10 ms on the pfifo thread */

    QemuThread t;
    qemu_thread_create(&t, "vr-hunt-write", hunt_write_thread, job,
                       QEMU_THREAD_DETACHED);
}

/* ---- public (vr_internal.h) ---- */

void vr_camera_request_hunt_dump(void)
{
    qatomic_set(&g_xemu_vr.cam.hunt_requested, true);
}

void vr_camera_reset(void)
{
    /* Force base re-capture (called on recenter and session start). */
    g_xemu_vr.cam.base_captured = false;
}

/* F9 RAM-hunt dump — VR-INDEPENDENT. A hunt is a pure guest-RAM snapshot for
 * Tier-3 address finding; it has nothing to do with VR and MUST fire in flat
 * mode too (hunting is normally done watching the desktop mirror, no headset).
 * Called every display sync from xemu_vr_frame, BEFORE its session gate. Head
 * pose is optional metadata (recorded only if a VR session happens to be live;
 * the scan uses the RAM contents + the operator's dump→state map, not the
 * pose). */
void vr_camera_hunt_poll(PGRAPHState *pg)
{
    VRCamera *cam = &g_xemu_vr.cam;
    VRCompositor *c = &g_xemu_vr.comp;
    NV2AState *d = container_of(pg, NV2AState, pgraph);

    if (!g_config.vr.hunt_enable) {
        return;
    }
    if (!qatomic_xchg(&cam->hunt_requested, false)) {
        return;
    }

    float yaw = 0.0f, pitch = 0.0f;
    if (c->head_pose_valid) {
        head_angles_rel(c, &yaw, &pitch);
    }
    hunt_dump(cam, d, yaw, pitch);
}

void vr_camera_frame(PGRAPHState *pg)
{
    VRCamera *cam = &g_xemu_vr.cam;
    VRCompositor *c = &g_xemu_vr.comp;
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    const uint64_t ram_size = memory_region_size(d->vram);

    /* Head-look needs a valid head pose (hunt dumps are handled separately in
     * vr_camera_hunt_poll, which is not gated on VR). */
    if (!c->head_pose_valid) {
        return;
    }

    float yaw, pitch;
    head_angles_rel(c, &yaw, &pitch);

    if (!g_config.vr.headlook_enable) {
        cam->base_captured = false;
        return;
    }

    refresh_addrs(cam, ram_size);
    refresh_base(cam, ram_size);
    if (!cam->yaw_addr_valid) {   /* yaw is required; pitch is optional */
        return;
    }

    /* ADS gate (OFP requirement, plan §9a): when a gate is configured,
     * poke ONLY while (byte & gate_mask) == gate_value. The mask form
     * matches value SETS — OFP's view mode must gate ON for ADS and
     * ADS+zoom but OFF for hip, plain zoom, and third person, which are
     * likely distinct values of one enum. Rising edge re-zeros both the
     * game base and the head reference, so raising the sights never
     * snaps; falling edge stops the pokes — the game owns its camera
     * again instantly. The gate byte is ALWAYS absolute (never rebased). */
    if (cam->gate_addr_valid) {
        const uint8_t gate = *(const uint8_t *)(d->vram_ptr + cam->gate_addr);
        const bool gate_on =
            (gate & (uint8_t)g_config.vr.headlook_gate_mask) ==
            (uint8_t)g_config.vr.headlook_gate_value;
        if (gate_on && !cam->gate_was_on) {
            cam->base_captured = false; /* rising edge: fresh zero */
        }
        cam->gate_was_on = gate_on;
        if (!gate_on) {
            return;
        }
    }

    /* Resolve the (possibly dynamic) camera base. In pointer/aob mode the
     * struct relocates each boot; resolve_cam_base follows it. An unresolved
     * base (loading screen / relocation / bad config) sidelines the poke and
     * forces a fresh zero when it returns — never a snap, never a stray write
     * into dead memory. yaw_addr/pitch_addr are OFFSETS from cbase (or, in
     * absolute mode, absolute addresses with cbase==0). */
    uint32_t cbase;
    if (!resolve_cam_base(cam, d, ram_size, &cbase)) {
        cam->base_captured = false;
        return;
    }
    const uint64_t ya = (uint64_t)cbase + cam->yaw_addr;
    const uint64_t pa = (uint64_t)cbase + cam->pitch_addr;
    if (ya + sizeof(float) > ram_size) {
        return;
    }
    const bool have_pitch =
        cam->pitch_addr_valid && pa + sizeof(float) <= ram_size;
    float *yaw_ptr = (float *)(d->vram_ptr + ya);
    float *pitch_ptr = have_pitch ? (float *)(d->vram_ptr + pa) : NULL;

    if (!cam->base_captured) {
        /* Zero: adopt the game's current angles as base and the current
         * head angles as reference — no snap at enable/ADS/recenter. */
        cam->yaw_base = *yaw_ptr;
        cam->head_ref_yaw = yaw;
        cam->last_written_yaw = *yaw_ptr;
        if (pitch_ptr) {
            cam->pitch_base = *pitch_ptr;
            cam->head_ref_pitch = pitch;
            cam->last_written_pitch = *pitch_ptr;
        }
        cam->base_captured = true;
        fprintf(stderr, "xemu-vr: head-look zeroed (game yaw=%f%s, head ref "
                        "%.1f/%.1f deg, base 0x%08X).\n", cam->yaw_base,
                pitch_ptr ? "" : " [pitch off]",
                yaw * (180.0f / (float)M_PI),
                pitch * (180.0f / (float)M_PI), cbase);
    }

    const float sens = g_config.vr.headlook_sensitivity;
    const float sign = g_config.vr.headlook_invert_pitch ? -1.0f : 1.0f;
    const float period = g_config.vr.headlook_engine_period;

    /* Compose: fold the game's own writes since our last poke (stick aim)
     * into the base, so stick and head aim ADD instead of fighting. The
     * game's yaw seam is wrapped in ENGINE units (period); the head delta is
     * wrapped in radians (WRAP_PI) before sens converts it to engine units.
     * The output is wrapped back into the engine's range so it never drifts
     * unbounded — the fold's modulo wrap absorbs the engine's re-normalization. */
    const float game_yaw = *yaw_ptr;
    cam->yaw_base += wrap_period(game_yaw - cam->last_written_yaw, period);
    const float out_yaw = wrap_period(
        cam->yaw_base + WRAP_PI(yaw - cam->head_ref_yaw) * sens, period);
    *yaw_ptr = out_yaw;
    cam->last_written_yaw = out_yaw;

    if (pitch_ptr) {
        const float game_pitch = *pitch_ptr;
        cam->pitch_base += game_pitch - cam->last_written_pitch;
        const float out_pitch =
            cam->pitch_base + (pitch - cam->head_ref_pitch) * sens * sign;
        *pitch_ptr = out_pitch;
        cam->last_written_pitch = out_pitch;
    }
}

void vr_camera_shutdown(void)
{
    VRCamera *cam = &g_xemu_vr.cam;
    g_free(cam->yaw_addr_str);
    g_free(cam->pitch_addr_str);
    g_free(cam->gate_addr_str);
    g_free(cam->base_mode_str);
    g_free(cam->base_ptr_str);
    g_free(cam->base_aob_str);
    cam->yaw_addr_str = NULL;
    cam->pitch_addr_str = NULL;
    cam->gate_addr_str = NULL;
    cam->base_mode_str = NULL;
    cam->base_ptr_str = NULL;
    cam->base_aob_str = NULL;
    cam->yaw_addr_valid = false;
    cam->pitch_addr_valid = false;
    cam->gate_addr_valid = false;
    cam->base_ptr_valid = false;
    cam->base_mode = 0;
    cam->aob_resolved = false;
    cam->aob_len = 0;
    cam->gate_was_on = false;
    cam->base_captured = false;
}
