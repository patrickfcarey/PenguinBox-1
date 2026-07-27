/*
 * xemu-VR — Steel Battalion in-headset cockpit console panel (content side)
 *
 * SPDX-FileCopyrightText: 2026 Patrick Carey
 * SPDX-License-Identifier: LGPL-2.0-or-later
 *
 * WHAT THIS IS
 *   The virtual center console: a fixed 768x256 RGBA instrument panel that the
 *   compositor composites as a second world-locked quad beside the Tier-1
 *   screen. It mirrors the shipped flat desktop overlay (ui/xui/sbc-overlay.cc)
 *   row-for-row — gear / tuner / pedals, toggle lamps, startup momentaries,
 *   combat core + aim — but rasterized by hand (baked bitmap font + rects) so
 *   it can live in-headset, which the ImGui mirror overlay never can.
 *
 * THREADING MODEL
 *   Two threads, one small lock-free slot between them:
 *     - vr_sbc_panel_publish() runs on the UI thread (poll_events, right after
 *       xemu_input_update_controllers()). It reads the keyboard-fed SBC state
 *       via the existing xemu_input_get_keyboard_sbc() accessor, resolves the
 *       key labels with SDL_GetScancodeName (UI-thread-only SDL call), and — if
 *       anything actually changed — publishes a snapshot into a seqlocked slot.
 *     - vr_sbc_panel_frame() runs on the pfifo thread (the VK-owning thread).
 *       It reads the slot through the seqlock, and if the content generation is
 *       unchanged since its last raster it returns false without touching the
 *       buffer. It never calls SDL/GL/Vulkan and never allocates.
 *   The slot is a QemuSeqLock with a single writer (the UI thread), so no
 *   writer mutex is needed; a zero-initialised seqlock is already valid, so the
 *   module needs no init hook (which it has no owner to call anyway). This is
 *   the seqlock generalisation the design doc calls for, in place of the
 *   recenter_requested qatomic.
 *
 * OFF-STATE GUARANTEE
 *   publish() returns after a single g_config read when VR is disabled, and
 *   publishes an "inactive" snapshot (no label work) when VR is on but no SBC
 *   controller is bound. frame() reports *active=false and returns false in
 *   both cases, so a VR-off / panel-off / no-SBC run does effectively nothing
 *   and never composites a panel. The slot is zero-initialised (active=false),
 *   so even before the first publish the panel is hidden.
 *
 * FIELD-VERIFIED SEMANTICS (do not re-derive — these are copied from the
 * shipped input/overlay code):
 *   - gear decode 254=R, 255=N, 1..5 (ISS-B11; an "improved" encoding was
 *     falsified by field evidence and reverted);
 *   - a toggle lamp bit is (SBC_BUTTON_* >> 32) & 0xFF, exactly the byteMask
 *     xemu_input.c flips into sbc.toggleSwitches;
 *   - pedals are unipolar 0..32767; aim X/Y and the rotation lever are bipolar
 *     -32768..32767, centre-zero;
 *   - axis indices come from enum steel_battalion_state_axis_index.
 *
 * WIP BOUNDARY (the "easy to hook up later" seam)
 *   Rendering is complete and real. INTERACTION is a stubbed boundary: the
 *   const cell table (vr_sbc_panel_cells) maps every rendered control to a hit
 *   rect + its SBC button mask + its config scancode field, and
 *   vr_sbc_panel_inject() is a deliberate LOG-ONLY no-op. Wiring a press into
 *   the real SBC input path — and the full game-logic layer, including any
 *   guest-memory work — is future scope, gated on the Touch backlog. See the
 *   comment block on vr_sbc_panel_inject().
 */

#include "qemu/osdep.h"
#include "qemu/seqlock.h"
#include "ui/xemu-settings.h"
#include "ui/xemu-input.h" /* SteelBattalionState, SBC_*, accessors; pulls SDL */
#include "vr_sbc_panel.h"

/* ===================================================================== *
 *  Palette — SRGB byte values, straight alpha. Background is translucent
 *  (the compositor blends it over the scene); content is opaque.
 * ===================================================================== */

typedef uint8_t Rgba[4];

static const Rgba COL_BG     = {  9, 11, 15, 210 }; /* panel backdrop        */
static const Rgba COL_ROW    = { 19, 23, 30, 235 }; /* per-row sub-panel      */
static const Rgba COL_SEP    = { 34, 40, 50, 255 }; /* hairline separators    */
static const Rgba COL_LABEL  = {158, 168, 180, 255 };/* control labels        */
static const Rgba COL_DIM    = { 96, 104, 118, 255 };/* inactive / key text   */
static const Rgba COL_ON     = { 86, 226, 122, 255 };/* lit / pressed / meter  */
static const Rgba COL_AMBER  = {242, 150,  54, 255 };/* reverse gear / eject   */
static const Rgba COL_TRACK  = { 38, 44, 55, 255 }; /* meter track / dark cell*/
static const Rgba COL_INK    = { 10, 13, 17, 255 }; /* text on inverted cell  */

/* ===================================================================== *
 *  Baked 5x7 bitmap font. One row per byte, bit4 = leftmost column. Only
 *  the glyphs the panel and SDL key labels need; unknown chars and space
 *  render blank. Uppercase-only — the renderer folds a-z to A-Z. Cosmetic
 *  only: an off pixel can never read or write out of bounds.
 * ===================================================================== */

/* charset order MUST match k_glyphs row-for-row. */
static const char k_charset[] =
    " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-+/.,:[]()<>?*='" ";";

static const uint8_t k_glyphs[sizeof(k_charset) - 1][7] = {
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00 }, /* space */
    { 0x0E,0x11,0x13,0x15,0x19,0x11,0x0E }, /* 0 */
    { 0x04,0x0C,0x04,0x04,0x04,0x04,0x0E }, /* 1 */
    { 0x0E,0x11,0x01,0x02,0x04,0x08,0x1F }, /* 2 */
    { 0x1F,0x02,0x04,0x02,0x01,0x11,0x0E }, /* 3 */
    { 0x02,0x06,0x0A,0x12,0x1F,0x02,0x02 }, /* 4 */
    { 0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E }, /* 5 */
    { 0x06,0x08,0x10,0x1E,0x11,0x11,0x0E }, /* 6 */
    { 0x1F,0x01,0x02,0x04,0x08,0x08,0x08 }, /* 7 */
    { 0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E }, /* 8 */
    { 0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C }, /* 9 */
    { 0x0E,0x11,0x11,0x1F,0x11,0x11,0x11 }, /* A */
    { 0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E }, /* B */
    { 0x0E,0x11,0x10,0x10,0x10,0x11,0x0E }, /* C */
    { 0x1C,0x12,0x11,0x11,0x11,0x12,0x1C }, /* D */
    { 0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F }, /* E */
    { 0x1F,0x10,0x10,0x1E,0x10,0x10,0x10 }, /* F */
    { 0x0E,0x11,0x10,0x17,0x11,0x11,0x0F }, /* G */
    { 0x11,0x11,0x11,0x1F,0x11,0x11,0x11 }, /* H */
    { 0x0E,0x04,0x04,0x04,0x04,0x04,0x0E }, /* I */
    { 0x07,0x02,0x02,0x02,0x12,0x12,0x0C }, /* J */
    { 0x11,0x12,0x14,0x18,0x14,0x12,0x11 }, /* K */
    { 0x10,0x10,0x10,0x10,0x10,0x10,0x1F }, /* L */
    { 0x11,0x1B,0x15,0x15,0x11,0x11,0x11 }, /* M */
    { 0x11,0x11,0x19,0x15,0x13,0x11,0x11 }, /* N */
    { 0x0E,0x11,0x11,0x11,0x11,0x11,0x0E }, /* O */
    { 0x1E,0x11,0x11,0x1E,0x10,0x10,0x10 }, /* P */
    { 0x0E,0x11,0x11,0x11,0x15,0x12,0x0D }, /* Q */
    { 0x1E,0x11,0x11,0x1E,0x14,0x12,0x11 }, /* R */
    { 0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E }, /* S */
    { 0x1F,0x04,0x04,0x04,0x04,0x04,0x04 }, /* T */
    { 0x11,0x11,0x11,0x11,0x11,0x11,0x0E }, /* U */
    { 0x11,0x11,0x11,0x11,0x11,0x0A,0x04 }, /* V */
    { 0x11,0x11,0x11,0x15,0x15,0x1B,0x11 }, /* W */
    { 0x11,0x11,0x0A,0x04,0x0A,0x11,0x11 }, /* X */
    { 0x11,0x11,0x0A,0x04,0x04,0x04,0x04 }, /* Y */
    { 0x1F,0x01,0x02,0x04,0x08,0x10,0x1F }, /* Z */
    { 0x00,0x00,0x00,0x0E,0x00,0x00,0x00 }, /* - */
    { 0x00,0x04,0x04,0x1F,0x04,0x04,0x00 }, /* + */
    { 0x01,0x01,0x02,0x04,0x08,0x10,0x10 }, /* / */
    { 0x00,0x00,0x00,0x00,0x00,0x06,0x06 }, /* . */
    { 0x00,0x00,0x00,0x00,0x04,0x04,0x08 }, /* , */
    { 0x00,0x04,0x04,0x00,0x04,0x04,0x00 }, /* : */
    { 0x0E,0x08,0x08,0x08,0x08,0x08,0x0E }, /* [ */
    { 0x0E,0x02,0x02,0x02,0x02,0x02,0x0E }, /* ] */
    { 0x02,0x04,0x08,0x08,0x08,0x04,0x02 }, /* ( */
    { 0x08,0x04,0x02,0x02,0x02,0x04,0x08 }, /* ) */
    { 0x02,0x04,0x08,0x10,0x08,0x04,0x02 }, /* < */
    { 0x08,0x04,0x02,0x01,0x02,0x04,0x08 }, /* > */
    { 0x0E,0x11,0x01,0x02,0x04,0x00,0x04 }, /* ? */
    { 0x00,0x04,0x15,0x0E,0x15,0x04,0x00 }, /* * */
    { 0x00,0x00,0x1F,0x00,0x1F,0x00,0x00 }, /* = */
    { 0x04,0x04,0x08,0x00,0x00,0x00,0x00 }, /* ' */
    { 0x00,0x04,0x04,0x00,0x04,0x04,0x08 }, /* ; */
};

static const uint8_t *glyph_rows(char c)
{
    if (c >= 'a' && c <= 'z') {
        c = (char)(c - 32); /* uppercase-only font */
    }
    if (c != '\0') {
        const char *p = strchr(k_charset, c);
        if (p != NULL) {
            return k_glyphs[(size_t)(p - k_charset)];
        }
    }
    return k_glyphs[0]; /* space / unknown -> blank */
}

/* ===================================================================== *
 *  Raster primitives. Every write is clipped to the panel; nothing here
 *  allocates.
 * ===================================================================== */

static void fill_rect(uint8_t *rgba, int x, int y, int w, int h,
                      const Rgba c)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > VR_SBC_PANEL_W ? VR_SBC_PANEL_W : x + w;
    int y1 = y + h > VR_SBC_PANEL_H ? VR_SBC_PANEL_H : y + h;
    for (int yy = y0; yy < y1; yy++) {
        uint8_t *p = rgba + ((size_t)yy * VR_SBC_PANEL_W + x0) * 4;
        for (int xx = x0; xx < x1; xx++) {
            *p++ = c[0];
            *p++ = c[1];
            *p++ = c[2];
            *p++ = c[3];
        }
    }
}

static void outline_rect(uint8_t *rgba, int x, int y, int w, int h, int t,
                         const Rgba c)
{
    fill_rect(rgba, x, y, w, t, c);             /* top    */
    fill_rect(rgba, x, y + h - t, w, t, c);     /* bottom */
    fill_rect(rgba, x, y, t, h, c);             /* left   */
    fill_rect(rgba, x + w - t, y, t, h, c);     /* right  */
}

static void draw_glyph(uint8_t *rgba, int x, int y, char c, int scale,
                       const Rgba col)
{
    const uint8_t *rows = glyph_rows(c);
    for (int gy = 0; gy < 7; gy++) {
        uint8_t bits = rows[gy];
        for (int gx = 0; gx < 5; gx++) {
            if (bits & (0x10 >> gx)) {
                fill_rect(rgba, x + gx * scale, y + gy * scale, scale, scale,
                          col);
            }
        }
    }
}

/* Advance is 6*scale per glyph (5 wide + 1 gap). Returns the next pen x. */
static int draw_text(uint8_t *rgba, int x, int y, const char *s, int scale,
                     const Rgba col)
{
    for (; *s != '\0'; s++) {
        draw_glyph(rgba, x, y, *s, scale, col);
        x += 6 * scale;
    }
    return x;
}

/* A meter: dark track + a green fill. Unipolar fills left-to-right; bipolar
 * fills out from the centre (with a centre tick), matching the flat overlay's
 * PedalMeter / AxisMeter. */
static void draw_meter(uint8_t *rgba, int x, int y, int w, int h, int16_t v,
                       bool bipolar)
{
    fill_rect(rgba, x, y, w, h, COL_TRACK);
    if (bipolar) {
        int half = w / 2;
        int len = (int)((float)v / 32768.0f * (float)half);
        if (len >= 0) {
            fill_rect(rgba, x + half, y, len, h, COL_ON);
        } else {
            fill_rect(rgba, x + half + len, y, -len, h, COL_ON);
        }
        fill_rect(rgba, x + half, y, 1, h, COL_LABEL); /* centre tick */
    } else {
        int vv = v < 0 ? 0 : v;
        fill_rect(rgba, x, y, (int)((float)vv / 32767.0f * (float)w), h,
                  COL_ON);
    }
    outline_rect(rgba, x, y, w, h, 1, COL_DIM);
}

/* ===================================================================== *
 *  Field-verified decode helpers (mirrors of the shipped input/overlay).
 * ===================================================================== */

/* 254=R, 255=N, 1..5 (ISS-B11). Single glyph for the big readout. */
static char gear_glyph_char(uint8_t g)
{
    switch (g) {
    case 254: return 'R';
    case 255: return 'N';
    case 1:   return '1';
    case 2:   return '2';
    case 3:   return '3';
    case 4:   return '4';
    case 5:   return '5';
    default:  return '?';
    }
}

/* The lamp bit for a toggle button, exactly as xemu_input.c derives byteMask. */
static inline uint8_t toggle_bit(uint64_t mask)
{
    return (uint8_t)(mask >> 32);
}

/* ===================================================================== *
 *  Layout geometry — the single source of truth shared by the raster and
 *  the interaction cell table, so a hit rect can never drift from what is
 *  drawn. Panel-pixel coordinates, top-left origin.
 * ===================================================================== */

#define PAD          8

/* Row 1: drivetrain — gear, tuner, three pedals. */
#define GEAR_X       PAD
#define GEAR_Y       6
#define GEAR_W       48
#define GEAR_H       54
#define TUN_X        64
#define TUN_Y        6
#define TUN_W        76
#define TUN_H        54
#define PED_X        152
#define PED_LBL_W    14
#define PED_BAR_X    (PED_X + PED_LBL_W)
#define PED_BAR_W    196
#define PED_Y0       10
#define PED_DY       16
#define PED_H        13
#define PED_CW       (PED_LBL_W + PED_BAR_W)

/* Row 2: five toggle lamps. */
#define TOG_Y        66
#define TOG_H        42
#define TOG_CW       150
#define TOG_X(i)     (PAD + (i) * TOG_CW)

/* Row 3: startup momentaries (EJECT is the bordered covered cell). */
#define STU_Y        112
#define STU_H        42
#define STU_CW       150
#define STU_X(i)     (PAD + (i) * STU_CW)

/* Row 4a: combat core (six cells). */
#define CMB_Y        160
#define CMB_H        30
#define CMB_CW       125
#define CMB_X(i)     (PAD + (i) * CMB_CW)

/* Row 4b: aim + rotation meters. */
#define AXR_Y        198
#define AXR_H        22
#define AIM_X        PAD
#define AIM_W        440
#define ROT_X        520
#define ROT_W        240

/* ===================================================================== *
 *  Row content tables. Rendering is driven from these; the cell table
 *  below reuses the same masks + geometry.
 * ===================================================================== */

/* Key-label slots carried in the snapshot (the controls whose key we show). */
enum {
    LBL_FILT, LBL_OXY, LBL_FUEL, LBL_BUF, LBL_VT,
    LBL_HATCH, LBL_IGN, LBL_START, LBL_EJECT, LBL_OVERRIDE,
    LBL_MAG, LBL_CHAFF, LBL_EXT,
    LBL_COUNT
};
#define LBL_NONE (-1)
#define LBL_MAX  9 /* 8 chars + NUL */

typedef struct RowItem {
    const char *label;
    uint64_t    mask;
    int         lbl; /* index into snapshot key_labels, or LBL_NONE */
} RowItem;

static const RowItem k_toggles[5] = {
    { "FILT", SBC_BUTTON_FILT_CONTROL_SYSTEM,     LBL_FILT },
    { "OXY",  SBC_BUTTON_OXYGEN_SUPPLY_SYSTEM,    LBL_OXY  },
    { "FUEL", SBC_BUTTON_FUEL_FLOW_RATE,          LBL_FUEL },
    { "BUF",  SBC_BUTTON_BUFFER_MATERIAL,         LBL_BUF  },
    { "VT",   SBC_BUTTON_VT_LOCATION_MEASUREMENT, LBL_VT   },
};

static const RowItem k_startup[5] = {
    { "HATCH", SBC_BUTTON_COCKPIT_HATCH, LBL_HATCH    },
    { "IGN",   SBC_BUTTON_IGNITION,      LBL_IGN      },
    { "START", SBC_BUTTON_START,         LBL_START    },
    { "EJECT", SBC_BUTTON_EJECT,         LBL_EJECT    },
    { "OVRD",  SBC_BUTTON_OVERRIDE,      LBL_OVERRIDE },
};

static const RowItem k_combat[6] = {
    { "MAIN", SBC_BUTTON_MAIN_WEAPON,     LBL_NONE  }, /* mouse-hardwired */
    { "SUB",  SBC_BUTTON_SUB_WEAPON,      LBL_NONE  }, /* mouse-hardwired */
    { "LOCK", SBC_BUTTON_LOCK_ON,         LBL_NONE  }, /* mouse-hardwired */
    { "MAG",  SBC_BUTTON_MAGAZINE_CHANGE, LBL_MAG   },
    { "CHAF", SBC_BUTTON_CHAFF,           LBL_CHAFF },
    { "EXT",  SBC_BUTTON_EXTINGUISHER,    LBL_EXT   },
};

/* ===================================================================== *
 *  Cross-thread snapshot slot (single writer = UI thread; seqlock).
 * ===================================================================== */

typedef struct VrSbcSnapshot {
    SteelBattalionState sbc;                 /* ~40 B field-verified state   */
    bool                active;              /* panel should be shown        */
    char                labels[LBL_COUNT][LBL_MAX];
} VrSbcSnapshot;

static void raster_panel(uint8_t *rgba, const VrSbcSnapshot *s);

static QemuSeqLock    s_seq;          /* zero-init is a valid seqlock       */
static VrSbcSnapshot  s_slot;         /* seqlock-protected (both threads)   */
static VrSbcSnapshot  s_pub_last;     /* UI-thread private: change detection */
static unsigned       s_raster_seq;   /* pfifo private: last rastered gen    */

/* ---- publish (UI thread) ---- */

static void resolve_label(char dst[LBL_MAX], int scancode)
{
    const char *name = NULL;
    if (scancode > SDL_SCANCODE_UNKNOWN && scancode < SDL_SCANCODE_COUNT) {
        name = SDL_GetScancodeName((SDL_Scancode)scancode);
    }
    if (name == NULL || name[0] == '\0') {
        name = "--";
    }
    int i = 0;
    for (; i < LBL_MAX - 1 && name[i] != '\0'; i++) {
        dst[i] = name[i];
    }
    dst[i] = '\0';
}

void vr_sbc_panel_publish(void)
{
    /* Off-state fast path: VR disabled is the dominant case — two bool
     * reads, return. */
    if (!g_config.vr.enable || !g_config.vr.sbc_panel_enable) {
        return;
    }

    /* The keyboard-fed SBC controller, or NULL — the same predicate the flat
     * overlay uses. We never dereference bound_controllers[] ourselves (unbind
     * UAF); the accessor hands back a snapshot-safe pointer we read once here
     * on the UI thread. */
    ControllerState *cs = xemu_input_get_keyboard_sbc();

    VrSbcSnapshot cand;
    memset(&cand, 0, sizeof(cand));
    cand.active = (cs != NULL);

    if (cand.active) {
        cand.sbc = cs->sbc;

        /* Field-access path only (no dependency on the generated struct tag). */
#define KMAP g_config.input.keyboard_sbc_scancode_map
        resolve_label(cand.labels[LBL_FILT],     KMAP.filt_control_system);
        resolve_label(cand.labels[LBL_OXY],      KMAP.oxygen_supply_system);
        resolve_label(cand.labels[LBL_FUEL],     KMAP.fuel_flow_rate);
        resolve_label(cand.labels[LBL_BUF],      KMAP.buffer_material);
        resolve_label(cand.labels[LBL_VT],       KMAP.vt_location_measurement);
        resolve_label(cand.labels[LBL_HATCH],    KMAP.cockpit_hatch);
        resolve_label(cand.labels[LBL_IGN],      KMAP.ignition);
        resolve_label(cand.labels[LBL_START],    KMAP.start);
        resolve_label(cand.labels[LBL_EJECT],    KMAP.eject);
        resolve_label(cand.labels[LBL_OVERRIDE], KMAP.override);
        resolve_label(cand.labels[LBL_MAG],      KMAP.magazine_change);
        resolve_label(cand.labels[LBL_CHAFF],    KMAP.chaff);
        resolve_label(cand.labels[LBL_EXT],      KMAP.extinguisher);
#undef KMAP
    }

    /* Change detection is on the UI (writer) side: only take the seqlock and
     * bump the generation when the content actually differs from the last
     * publish. That keeps the seqlock sequence a true content generation, so
     * the pfifo side can change-gate its raster against it. */
    if (memcmp(&cand, &s_pub_last, sizeof(cand)) == 0) {
        return;
    }
    s_pub_last = cand;

    seqlock_write_begin(&s_seq);
    s_slot = cand;
    seqlock_write_end(&s_seq);

    /* Debug capture (XEMU_VR_PANEL_DUMP=<path>.ppm): on every content
     * change, raster the panel here on the UI thread into a scratch
     * buffer and write a PPM (alpha composited over the void color).
     * Lets the panel's exact pixels be inspected with no headset — the
     * placement is XR-only, the CONTENT is fully verifiable this way. */
    const char *dump = getenv("XEMU_VR_PANEL_DUMP");
    if (dump && cand.active) {
        static uint8_t scratch[VR_SBC_PANEL_W * VR_SBC_PANEL_H * 4];
        raster_panel(scratch, &cand);
        FILE *f = fopen(dump, "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", VR_SBC_PANEL_W, VR_SBC_PANEL_H);
            for (size_t i = 0; i < sizeof(scratch); i += 4) {
                unsigned a = scratch[i + 3];
                uint8_t px[3] = {
                    (uint8_t)((scratch[i + 0] * a + 16 * (255 - a)) / 255),
                    (uint8_t)((scratch[i + 1] * a + 18 * (255 - a)) / 255),
                    (uint8_t)((scratch[i + 2] * a + 26 * (255 - a)) / 255),
                };
                fwrite(px, 1, 3, f);
            }
            fclose(f);
        }
    }
}

/* ===================================================================== *
 *  frame (pfifo thread): consume the snapshot, change-gate, raster.
 * ===================================================================== */

/* Draw one lamp+label+key cell (toggles). Lit lamps + label go green. */
static void draw_lamp_cell(uint8_t *rgba, int x, int y, const RowItem *it,
                           bool on, const char *key)
{
    const Rgba *lc = on ? &COL_ON : &COL_TRACK;
    fill_rect(rgba, x + 4, y + 6, 12, 12, *lc);
    outline_rect(rgba, x + 4, y + 6, 12, 12, 1, on ? COL_ON : COL_DIM);
    draw_text(rgba, x + 22, y + 6, it->label, 2, on ? COL_ON : COL_LABEL);
    if (key != NULL) {
        int px = draw_text(rgba, x + 22, y + 24, "[", 1, COL_DIM);
        px = draw_text(rgba, px, y + 24, key, 1, COL_DIM);
        draw_text(rgba, px, y + 24, "]", 1, COL_DIM);
    }
}

/* Draw one momentary/combat button cell. Pressed = inverted bright fill with
 * dark ink. `bordered` draws the covered-eject frame (always) and uses amber. */
static void draw_button_cell(uint8_t *rgba, int x, int y, int w, int h,
                             const RowItem *it, bool pressed, const char *key,
                             bool bordered)
{
    const Rgba *accent = bordered ? &COL_AMBER : &COL_ON;
    fill_rect(rgba, x, y, w, h, pressed ? *accent : COL_TRACK);
    if (bordered) {
        /* The safety cover: a double amber frame around the eject cell. */
        outline_rect(rgba, x, y, w, h, 2, COL_AMBER);
        outline_rect(rgba, x + 3, y + 3, w - 6, h - 6, 1, COL_AMBER);
    } else {
        outline_rect(rgba, x, y, w, h, 1, pressed ? *accent : COL_DIM);
    }
    const Rgba *fg = pressed ? &COL_INK
                    : bordered ? &COL_AMBER
                    : &COL_LABEL;
    draw_text(rgba, x + 6, y + 5, it->label, 2, *fg);
    if (key != NULL) {
        draw_text(rgba, x + 6, y + h - 10, key, 1, pressed ? COL_INK : COL_DIM);
    } else {
        draw_text(rgba, x + 6, y + h - 10, "MOUSE", 1,
                  pressed ? COL_INK : COL_DIM);
    }
}

static void raster_panel(uint8_t *rgba, const VrSbcSnapshot *s)
{
    const SteelBattalionState *sbc = &s->sbc;

    /* Backdrop + per-row sub-panels + separators. */
    fill_rect(rgba, 0, 0, VR_SBC_PANEL_W, VR_SBC_PANEL_H, COL_BG);
    fill_rect(rgba, 2, 2, VR_SBC_PANEL_W - 4, 60, COL_ROW);
    fill_rect(rgba, 2, 64, VR_SBC_PANEL_W - 4, 44, COL_ROW);
    fill_rect(rgba, 2, 110, VR_SBC_PANEL_W - 4, 44, COL_ROW);
    fill_rect(rgba, 2, 156, VR_SBC_PANEL_W - 4, VR_SBC_PANEL_H - 158, COL_ROW);
    fill_rect(rgba, 2, 62, VR_SBC_PANEL_W - 4, 1, COL_SEP);
    fill_rect(rgba, 2, 108, VR_SBC_PANEL_W - 4, 1, COL_SEP);
    fill_rect(rgba, 2, 154, VR_SBC_PANEL_W - 4, 1, COL_SEP);

    /* ---- Row 1: gear (big; amber in reverse per ISS-B11) ---- */
    draw_text(rgba, GEAR_X, GEAR_Y, "GEAR", 1, COL_LABEL);
    {
        const Rgba *gc = (sbc->gearLever == 254) ? &COL_AMBER
                        : (sbc->gearLever == 255) ? &COL_DIM
                        : &COL_ON;
        char g[2] = { gear_glyph_char(sbc->gearLever), '\0' };
        /* scale 5 glyph = 25x35, centred in the gear cell */
        draw_text(rgba, GEAR_X + (GEAR_W - 25) / 2, GEAR_Y + 15, g, 5, *gc);
    }

    /* Tuner: 2-digit dial (0..15). */
    draw_text(rgba, TUN_X, TUN_Y, "TUNER", 1, COL_LABEL);
    {
        char t[3];
        unsigned d = sbc->tunerDial;
        t[0] = (char)('0' + (d / 10) % 10);
        t[1] = (char)('0' + d % 10);
        t[2] = '\0';
        draw_text(rgba, TUN_X + 2, TUN_Y + 16, t, 3, COL_ON);
    }

    /* Three pedals (unipolar 0..32767). */
    {
        static const char *pl[3] = { "L", "M", "R" };
        const int idx[3] = { SBC_AXIS_LEFT_PEDAL, SBC_AXIS_MIDDLE_PEDAL,
                             SBC_AXIS_RIGHT_PEDAL };
        for (int i = 0; i < 3; i++) {
            int y = PED_Y0 + i * PED_DY;
            draw_text(rgba, PED_X, y + 3, pl[i], 1, COL_LABEL);
            draw_meter(rgba, PED_BAR_X, y, PED_BAR_W, PED_H, sbc->axis[idx[i]],
                       false);
        }
    }

    /* ---- Row 2: toggle lamps ---- */
    for (int i = 0; i < 5; i++) {
        bool on = (sbc->toggleSwitches & toggle_bit(k_toggles[i].mask)) != 0;
        const char *key = k_toggles[i].lbl != LBL_NONE
                              ? s->labels[k_toggles[i].lbl] : NULL;
        draw_lamp_cell(rgba, TOG_X(i), TOG_Y, &k_toggles[i], on, key);
    }

    /* ---- Row 3: startup momentaries (eject bordered) ---- */
    for (int i = 0; i < 5; i++) {
        bool pressed = (sbc->buttons & k_startup[i].mask) != 0;
        const char *key = k_startup[i].lbl != LBL_NONE
                              ? s->labels[k_startup[i].lbl] : NULL;
        bool eject = (k_startup[i].mask == SBC_BUTTON_EJECT);
        draw_button_cell(rgba, STU_X(i), STU_Y, STU_CW - 6, STU_H, &k_startup[i],
                         pressed, key, eject);
    }

    /* ---- Row 4a: combat core ---- */
    for (int i = 0; i < 6; i++) {
        bool pressed = (sbc->buttons & k_combat[i].mask) != 0;
        const char *key = k_combat[i].lbl != LBL_NONE
                              ? s->labels[k_combat[i].lbl] : NULL;
        draw_button_cell(rgba, CMB_X(i), CMB_Y, CMB_CW - 6, CMB_H, &k_combat[i],
                         pressed, key, false);
    }

    /* ---- Row 4b: aim + rotation (bipolar, centre-zero) ---- */
    {
        int mid = AIM_X + (AIM_W - 60) / 2;
        draw_text(rgba, AIM_X, AXR_Y - 9, "AIM X", 1, COL_LABEL);
        draw_meter(rgba, AIM_X, AXR_Y, (AIM_W - 60) / 2 - 4, AXR_H,
                   sbc->axis[SBC_AXIS_AIMING_X], true);
        draw_text(rgba, mid, AXR_Y - 9, "Y", 1, COL_LABEL);
        draw_meter(rgba, mid, AXR_Y, (AIM_W - 60) / 2 - 4, AXR_H,
                   sbc->axis[SBC_AXIS_AIMING_Y], true);
        draw_text(rgba, ROT_X, AXR_Y - 9, "ROT", 1, COL_LABEL);
        draw_meter(rgba, ROT_X, AXR_Y, ROT_W - 4, AXR_H,
                   sbc->axis[SBC_AXIS_ROTATION_LEVER], true);
    }
}

bool vr_sbc_panel_frame(uint8_t *rgba, bool *active)
{
    /* Read the seqlocked slot into a private copy (retry on a torn read). */
    VrSbcSnapshot snap;
    unsigned start;
    do {
        start = seqlock_read_begin(&s_seq);
        snap = s_slot;
    } while (seqlock_read_retry(&s_seq, start));

    if (active != NULL) {
        *active = snap.active;
    }
    if (!snap.active) {
        return false;            /* panel hidden — nothing to composite      */
    }
    if (start == s_raster_seq) {
        return false;            /* unchanged since last raster — reuse image */
    }

    raster_panel(rgba, &snap);
    s_raster_seq = start;
    return true;
}

/* ===================================================================== *
 *  Interaction boundary — const cell table + LOG-ONLY inject stub.
 *
 *  The table maps every rendered control to a panel-pixel hit rect, its
 *  SBC_BUTTON_* mask (0 for the pedal / aim / rotation axis cells) and the
 *  g_config scancode field that drives it (NULL for the mouse-hardwired
 *  weapons and the mouse aim). Geometry comes straight from the layout
 *  macros above, so a hit rect can never disagree with what is drawn.
 * ===================================================================== */

#define KM(field) (&g_config.input.keyboard_sbc_scancode_map.field)

static const VrSbcPanelCell k_cells[] = {
    /* Row 1 drivetrain (split the gear glyph into up/down halves, the tuner
     * number into left/right halves, so a future press can shift/tune). */
    { "gear.up",     GEAR_X, GEAR_Y,          GEAR_W, GEAR_H / 2,
      SBC_BUTTON_GEAR_UP,   KM(gear_up)   },
    { "gear.down",   GEAR_X, GEAR_Y + GEAR_H / 2, GEAR_W, GEAR_H / 2,
      SBC_BUTTON_GEAR_DOWN, KM(gear_down) },
    { "tuner.left",  TUN_X,          TUN_Y, TUN_W / 2, TUN_H,
      SBC_BUTTON_TUNER_LEFT,  KM(tuner_left)  },
    { "tuner.right", TUN_X + TUN_W / 2, TUN_Y, TUN_W / 2, TUN_H,
      SBC_BUTTON_TUNER_RIGHT, KM(tuner_right) },
    { "pedal.left",   PED_X, PED_Y0 + 0 * PED_DY, PED_CW, PED_H, 0,
      KM(left_pedal)   },
    { "pedal.middle", PED_X, PED_Y0 + 1 * PED_DY, PED_CW, PED_H, 0,
      KM(middle_pedal) },
    { "pedal.right",  PED_X, PED_Y0 + 2 * PED_DY, PED_CW, PED_H, 0,
      KM(right_pedal)  },

    /* Row 2 toggles. */
    { "toggle.filt", TOG_X(0), TOG_Y, TOG_CW - 4, TOG_H,
      SBC_BUTTON_FILT_CONTROL_SYSTEM,     KM(filt_control_system)     },
    { "toggle.oxy",  TOG_X(1), TOG_Y, TOG_CW - 4, TOG_H,
      SBC_BUTTON_OXYGEN_SUPPLY_SYSTEM,    KM(oxygen_supply_system)    },
    { "toggle.fuel", TOG_X(2), TOG_Y, TOG_CW - 4, TOG_H,
      SBC_BUTTON_FUEL_FLOW_RATE,          KM(fuel_flow_rate)          },
    { "toggle.buf",  TOG_X(3), TOG_Y, TOG_CW - 4, TOG_H,
      SBC_BUTTON_BUFFER_MATERIAL,         KM(buffer_material)         },
    { "toggle.vt",   TOG_X(4), TOG_Y, TOG_CW - 4, TOG_H,
      SBC_BUTTON_VT_LOCATION_MEASUREMENT, KM(vt_location_measurement) },

    /* Row 3 startup momentaries (eject is the covered cell). */
    { "hatch",    STU_X(0), STU_Y, STU_CW - 6, STU_H,
      SBC_BUTTON_COCKPIT_HATCH, KM(cockpit_hatch) },
    { "ignition", STU_X(1), STU_Y, STU_CW - 6, STU_H,
      SBC_BUTTON_IGNITION,      KM(ignition)      },
    { "start",    STU_X(2), STU_Y, STU_CW - 6, STU_H,
      SBC_BUTTON_START,         KM(start)         },
    { "eject",    STU_X(3), STU_Y, STU_CW - 6, STU_H,
      SBC_BUTTON_EJECT,         KM(eject)         },
    { "override", STU_X(4), STU_Y, STU_CW - 6, STU_H,
      SBC_BUTTON_OVERRIDE,      KM(override)      },

    /* Row 4a combat core (main/sub/lock are mouse-hardwired -> NULL scancode). */
    { "main",  CMB_X(0), CMB_Y, CMB_CW - 6, CMB_H, SBC_BUTTON_MAIN_WEAPON,   NULL },
    { "sub",   CMB_X(1), CMB_Y, CMB_CW - 6, CMB_H, SBC_BUTTON_SUB_WEAPON,    NULL },
    { "lock",  CMB_X(2), CMB_Y, CMB_CW - 6, CMB_H, SBC_BUTTON_LOCK_ON,       NULL },
    { "mag",   CMB_X(3), CMB_Y, CMB_CW - 6, CMB_H, SBC_BUTTON_MAGAZINE_CHANGE,
      KM(magazine_change) },
    { "chaff", CMB_X(4), CMB_Y, CMB_CW - 6, CMB_H, SBC_BUTTON_CHAFF,
      KM(chaff)           },
    { "ext",   CMB_X(5), CMB_Y, CMB_CW - 6, CMB_H, SBC_BUTTON_EXTINGUISHER,
      KM(extinguisher)    },

    /* Row 4b axis cells (mask 0). Aim is the 2D mouse lever; rotation is two
     * keys, so neither carries a single scancode. */
    { "aim",      AIM_X, AXR_Y, AIM_W, AXR_H, 0, NULL },
    { "rotation", ROT_X, AXR_Y, ROT_W, AXR_H, 0, NULL },
};

#undef KM

const VrSbcPanelCell *vr_sbc_panel_cells(size_t *count)
{
    if (count != NULL) {
        *count = ARRAY_SIZE(k_cells);
    }
    return k_cells;
}

void vr_sbc_panel_inject(const char *cell_id, bool pressed)
{
    /*
     * ===================== WIP: LOG-ONLY STUB =========================
     * This is the deliberate "super easy to hook up later" seam. Today it
     * only logs, so the panel's rendered layout and its hit-test table can
     * be exercised end to end without any input plumbing.
     *
     * Where the REAL wiring lands later (all future scope):
     *   1. Touch/pointer hit-testing (public plan §10: OpenXR actions ->
     *      SDL3 virtual gamepad) resolves a controller ray to a cell via
     *      vr_sbc_panel_cells(), then calls this with the cell id + edge.
     *   2. This function turns that into a synthetic press on the SBC input
     *      path — for a keyboard-bound cell, a synthetic scancode down/up
     *      into the same state xemu_input_update_sdl_kbd_controller_state()
     *      reads (the cell's ->scancode); for a mouse-hardwired weapon, a
     *      synthetic button. The momentary-vs-latching edge rules (toggles
     *      flip on the press edge, the tuner/gear step on the press edge)
     *      must be honoured there, matching xemu_input.c.
     *   3. The covered-eject cell gets its two-stage gesture guard at this
     *      layer (flip the cover, then the press) — the accidental-eject
     *      protection for the one control whose misfire deletes a save.
     *   4. The full game-logic layer — including any guest-memory address
     *      work — is explicitly out of scope here and stays WIP.
     *
     * No validation machinery is built for this yet on purpose; this is the
     * shape of the boundary, not its behaviour.
     * ================================================================== */
    fprintf(stderr,
            "xemu-vr: [sbc-panel WIP] inject cell='%s' pressed=%d — no-op "
            "(input wiring is future scope)\n",
            cell_id != NULL ? cell_id : "(null)", pressed);
}
