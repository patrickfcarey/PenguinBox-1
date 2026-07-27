//
// PenguinBox — Steel Battalion cockpit input overlay (desktop mirror)
//
// Copyright (C) 2026 Patrick Carey
//
// SPDX-License-Identifier: MIT
//
// See sbc-overlay.hh and docs/features/sbc-cockpit-overlay.md.
//
#include "sbc-overlay.hh"
#include "common.hh"
#include "viewport-manager.hh"

#include "../xemu-input.h"
#include "../xemu-notifications.h"

#include <SDL3/SDL.h>
#include <iterator> // std::size — QEMU's ARRAY_SIZE macro is C-only (typeof)

namespace
{

// ── The control table ────────────────────────────────────────────────────
// One row per keyboard-bound SBC control. It backs three things at once: the
// key labels, the collapsible "all controls" list, and the toggle-scancode
// collision guard (so the guard can never drift out of sync with the map).
//
// `scancode` points straight at the generated config field — the SAME source
// xemu_input.c populates sdl_sbc_kbd_scancode_map[] from, so a rebound layout
// shows its own keys with no extra plumbing.
struct SbcControl {
    const char *label;
    int *scancode;   // NULL => not keyboard-bound (mouse-hardwired)
    uint64_t button; // 0 => axis/other, handled by the caller
};

// Mouse-hardwired in xemu_input.c (raw SDL_GetMouseState) — no scancode.
#define MOUSE_BOUND NULL

// Momentary / latching buttons, grouped as the cockpit blocks are.
SbcControl g_startup[] = {
    { "Hatch",    &g_config.input.keyboard_sbc_scancode_map.cockpit_hatch, SBC_BUTTON_COCKPIT_HATCH },
    { "Ignition", &g_config.input.keyboard_sbc_scancode_map.ignition,      SBC_BUTTON_IGNITION      },
    { "Start",    &g_config.input.keyboard_sbc_scancode_map.start,         SBC_BUTTON_START         },
    { "Eject",    &g_config.input.keyboard_sbc_scancode_map.eject,         SBC_BUTTON_EJECT         },
    { "Override", &g_config.input.keyboard_sbc_scancode_map.override,      SBC_BUTTON_OVERRIDE      },
};

SbcControl g_combat[] = {
    { "Main",    MOUSE_BOUND,                                                 SBC_BUTTON_MAIN_WEAPON     },
    { "Sub",     MOUSE_BOUND,                                                 SBC_BUTTON_SUB_WEAPON      },
    { "Lock-on", MOUSE_BOUND,                                                 SBC_BUTTON_LOCK_ON         },
    { "Mag",     &g_config.input.keyboard_sbc_scancode_map.magazine_change,   SBC_BUTTON_MAGAZINE_CHANGE },
    { "Chaff",   &g_config.input.keyboard_sbc_scancode_map.chaff,             SBC_BUTTON_CHAFF           },
    { "Ext",     &g_config.input.keyboard_sbc_scancode_map.extinguisher,      SBC_BUTTON_EXTINGUISHER    },
};

// The five latching toggles. The lamp bit is (button >> 32) — exactly how
// xemu_input.c derives its byteMask when flipping sbc.toggleSwitches.
SbcControl g_toggles[] = {
    { "FILT",   &g_config.input.keyboard_sbc_scancode_map.filt_control_system,     SBC_BUTTON_FILT_CONTROL_SYSTEM     },
    { "OXY",    &g_config.input.keyboard_sbc_scancode_map.oxygen_supply_system,    SBC_BUTTON_OXYGEN_SUPPLY_SYSTEM    },
    { "FUEL",   &g_config.input.keyboard_sbc_scancode_map.fuel_flow_rate,          SBC_BUTTON_FUEL_FLOW_RATE          },
    { "BUFFER", &g_config.input.keyboard_sbc_scancode_map.buffer_material,         SBC_BUTTON_BUFFER_MATERIAL         },
    { "VT LOC", &g_config.input.keyboard_sbc_scancode_map.vt_location_measurement, SBC_BUTTON_VT_LOCATION_MEASUREMENT },
};

// Everything else, for the collapsed "all controls" section.
SbcControl g_rest[] = {
    { "Open/Close",    &g_config.input.keyboard_sbc_scancode_map.open_close,              SBC_BUTTON_OPEN_CLOSE            },
    { "Map Zoom",      &g_config.input.keyboard_sbc_scancode_map.map_zoom_in_out,         SBC_BUTTON_MAP_ZOOM_IN_OUT       },
    { "Mode Select",   &g_config.input.keyboard_sbc_scancode_map.mode_select,             SBC_BUTTON_MODE_SELECT           },
    { "Sub Monitor",   &g_config.input.keyboard_sbc_scancode_map.sub_monitor_mode_select, SBC_BUTTON_SUB_MONITOR_MODE_SELECT },
    { "Zoom In",       &g_config.input.keyboard_sbc_scancode_map.zoom_in,                 SBC_BUTTON_ZOOM_IN               },
    { "Zoom Out",      &g_config.input.keyboard_sbc_scancode_map.zoom_out,                SBC_BUTTON_ZOOM_OUT              },
    { "FSS",           &g_config.input.keyboard_sbc_scancode_map.fss,                     SBC_BUTTON_FSS                   },
    { "Manipulator",   &g_config.input.keyboard_sbc_scancode_map.manipulator,             SBC_BUTTON_MANIPULATOR           },
    { "Line Color",    &g_config.input.keyboard_sbc_scancode_map.line_color_change,       SBC_BUTTON_LINE_COLOR_CHANGE     },
    { "Washing",       &g_config.input.keyboard_sbc_scancode_map.washing,                 SBC_BUTTON_WASHING               },
    { "Tank Detach",   &g_config.input.keyboard_sbc_scancode_map.tank_detach,             SBC_BUTTON_TANK_DETACH           },
    { "Night Scope",   &g_config.input.keyboard_sbc_scancode_map.night_scope,             SBC_BUTTON_NIGHT_SCOPE           },
    { "Func 1",        &g_config.input.keyboard_sbc_scancode_map.func1,                   SBC_BUTTON_FUNC1                 },
    { "Func 2",        &g_config.input.keyboard_sbc_scancode_map.func2,                   SBC_BUTTON_FUNC2                 },
    { "Func 3",        &g_config.input.keyboard_sbc_scancode_map.func3,                   SBC_BUTTON_FUNC3                 },
    { "Main Wpn Ctl",  &g_config.input.keyboard_sbc_scancode_map.main_weapon_control,     SBC_BUTTON_MAIN_WEAPON_CONTROL   },
    { "Sub Wpn Ctl",   &g_config.input.keyboard_sbc_scancode_map.sub_weapon_control,      SBC_BUTTON_SUB_WEAPON_CONTROL    },
    { "COM 1",         &g_config.input.keyboard_sbc_scancode_map.com1,                    SBC_BUTTON_COM1                  },
    { "COM 2",         &g_config.input.keyboard_sbc_scancode_map.com2,                    SBC_BUTTON_COM2                  },
    { "COM 3",         &g_config.input.keyboard_sbc_scancode_map.com3,                    SBC_BUTTON_COM3                  },
    { "COM 4",         &g_config.input.keyboard_sbc_scancode_map.com4,                    SBC_BUTTON_COM4                  },
    { "COM 5",         &g_config.input.keyboard_sbc_scancode_map.com5,                    SBC_BUTTON_COM5                  },
    { "Sight Change",  &g_config.input.keyboard_sbc_scancode_map.sight_change,            SBC_BUTTON_SIGHT_CHANGE          },
    { "Gear Up",       &g_config.input.keyboard_sbc_scancode_map.gear_up,                 SBC_BUTTON_GEAR_UP               },
    { "Gear Down",     &g_config.input.keyboard_sbc_scancode_map.gear_down,               SBC_BUTTON_GEAR_DOWN             },
    { "Tuner Left",    &g_config.input.keyboard_sbc_scancode_map.tuner_left,              SBC_BUTTON_TUNER_LEFT            },
    { "Tuner Right",   &g_config.input.keyboard_sbc_scancode_map.tuner_right,             SBC_BUTTON_TUNER_RIGHT           },
};

// Axis-driving keys — shown in "all controls" with their key labels only
// (their effect is already visible in the pedal/rotation meters).
SbcControl g_axis_keys[] = {
    { "Rotate L",   &g_config.input.keyboard_sbc_scancode_map.rotation_left,      0 },
    { "Rotate R",   &g_config.input.keyboard_sbc_scancode_map.rotation_right,     0 },
    { "Pedal L",    &g_config.input.keyboard_sbc_scancode_map.left_pedal,         0 },
    { "Pedal M",    &g_config.input.keyboard_sbc_scancode_map.middle_pedal,       0 },
    { "Pedal R",    &g_config.input.keyboard_sbc_scancode_map.right_pedal,        0 },
    { "Sight Up",   &g_config.input.keyboard_sbc_scancode_map.sight_change_up,    0 },
    { "Sight Down", &g_config.input.keyboard_sbc_scancode_map.sight_change_down,  0 },
    { "Sight Left", &g_config.input.keyboard_sbc_scancode_map.sight_change_left,  0 },
    { "Sight Rght", &g_config.input.keyboard_sbc_scancode_map.sight_change_right, 0 },
};

const ImVec4 kOn(0.35f, 1.00f, 0.45f, 1.00f);
const ImVec4 kOff(0.45f, 0.45f, 0.45f, 1.00f);
const ImVec4 kDim(0.70f, 0.70f, 0.70f, 1.00f);
const ImVec4 kWarn(1.00f, 0.55f, 0.25f, 1.00f);

// SDL_GetScancodeName can return "" (or NULL) for unmapped/unknown codes.
const char *KeyLabel(const int *scancode)
{
    if (scancode == NULL) {
        return "mouse";
    }
    if (*scancode <= SDL_SCANCODE_UNKNOWN || *scancode >= SDL_SCANCODE_COUNT) {
        return "--";
    }
    const char *name = SDL_GetScancodeName((SDL_Scancode)*scancode);
    return (name && name[0]) ? name : "--";
}

// The FIELD-VERIFIED gear decode. Mirrors ui/xui/gl-helpers.cc's transmission
// switch exactly (254=R, 255=N, 1..5). Do NOT re-derive this: an "improved"
// encoding was falsified by field evidence and reverted (ISS-B11).
const char *GearGlyph(uint8_t gear)
{
    switch (gear) {
    case 254: return "R";
    case 255: return "N";
    case 1:   return "1";
    case 2:   return "2";
    case 3:   return "3";
    case 4:   return "4";
    case 5:   return "5";
    default:  return "?";
    }
}

void LabelledLamp(const char *label, bool on, const int *scancode)
{
    ImGui::TextColored(on ? kOn : kOff, "%s", on ? "*" : "o");
    ImGui::SameLine(0.0f, 3.0f);
    if (g_config.display.ui.sbc_overlay.show_key_labels && scancode) {
        ImGui::TextColored(on ? kOn : kDim, "%s[%s]", label, KeyLabel(scancode));
    } else {
        ImGui::TextColored(on ? kOn : kDim, "%s", label);
    }
}

// Unipolar 0..32767 meter (pedals).
void PedalMeter(const char *label, int16_t v)
{
    float f = (float)(v < 0 ? 0 : v) / 32767.0f;
    ImGui::TextColored(kDim, "%s", label);
    ImGui::SameLine(0.0f, 3.0f);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, f > 0.01f ? kOn : kOff);
    ImGui::ProgressBar(f, g_viewport_mgr.Scale(ImVec2(46, 10)), "");
    ImGui::PopStyleColor();
}

// Bipolar -32768..32767 meter (aim / rotation lever), centre-zero.
void AxisMeter(const char *label, int16_t v)
{
    float f = ((float)v + 32768.0f) / 65535.0f;
    ImGui::TextColored(kDim, "%s", label);
    ImGui::SameLine(0.0f, 3.0f);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, kOff);
    ImGui::ProgressBar(f, g_viewport_mgr.Scale(ImVec2(60, 10)), "");
    ImGui::PopStyleColor();
}

void ButtonRow(const SbcControl *controls, size_t count, uint64_t buttons,
               int per_line)
{
    for (size_t i = 0; i < count; i++) {
        const bool on = (buttons & controls[i].button) != 0;
        if (per_line > 0 && (i % (size_t)per_line) != 0) {
            ImGui::SameLine(0.0f, 8.0f);
        }
        if (g_config.display.ui.sbc_overlay.show_key_labels) {
            ImGui::TextColored(on ? kOn : kDim, "%s[%s]", controls[i].label,
                               KeyLabel(controls[i].scancode));
        } else {
            ImGui::TextColored(on ? kOn : kDim, "%s", controls[i].label);
        }
    }
}

// The keyboard-fed SBC controller, or NULL. Cheap; called once per frame.
ControllerState *BoundSbc(void)
{
    return xemu_input_get_keyboard_sbc();
}

// Cockpit-wins guard: does the toggle scancode collide with any SBC binding?
bool ToggleCollides(int toggle_scancode)
{
    const SbcControl *tables[] = { g_startup, g_combat, g_toggles, g_rest,
                                   g_axis_keys };
    const size_t counts[] = { std::size(g_startup), std::size(g_combat),
                              std::size(g_toggles), std::size(g_rest),
                              std::size(g_axis_keys) };
    for (size_t t = 0; t < std::size(tables); t++) {
        for (size_t i = 0; i < counts[t]; i++) {
            const int *sc = tables[t][i].scancode;
            if (sc && *sc == toggle_scancode) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

bool SbcOverlayAvailable(void)
{
    return xemu_input_keyboard_feeds_sbc();
}

void SbcOverlayProcessHotkey(void)
{
    static bool was_down = false;
    static bool warned_collision = false;

    if (!SbcOverlayAvailable()) {
        was_down = false;
        return;
    }

    const int toggle = g_config.display.ui.sbc_overlay.toggle_scancode;
    if (toggle <= SDL_SCANCODE_UNKNOWN || toggle >= SDL_SCANCODE_COUNT) {
        return;
    }

    // Cockpit wins: a rebind that claims the toggle key disables the hotkey
    // (the View-menu item still works, and toggle_scancode is the escape
    // hatch). One-shot notification so it is discoverable, not silent.
    if (ToggleCollides(toggle)) {
        if (!warned_collision) {
            warned_collision = true;
            xemu_queue_notification(
                "SBC overlay hotkey disabled: its key is bound to a cockpit "
                "control. Use View > SBC Cockpit Overlay, or rebind "
                "display.ui.sbc_overlay.toggle_scancode.");
        }
        was_down = false;
        return;
    }

    const bool *kbd = SDL_GetKeyboardState(NULL);
    if (!kbd) {
        return;
    }
    const bool down = kbd[toggle];
    if (down && !was_down) {
        g_config.display.ui.sbc_overlay.enable =
            !g_config.display.ui.sbc_overlay.enable;
    }
    was_down = down;
}

void SbcOverlayDraw(void)
{
    if (!g_config.display.ui.sbc_overlay.enable) {
        return;
    }

    ControllerState *state = BoundSbc();
    if (state == NULL) {
        return; // off-state: nothing drawn, ever
    }

    const SteelBattalionState &sbc = state->sbc;
    const int corner = g_config.display.ui.sbc_overlay.corner;
    const float distance = g_viewport_mgr.Scale(ImVec2(10, 10)).x;

    // Corner enum order: top_left, top_right, bottom_left, bottom_right —
    // bit 0 = right, bit 1 = bottom (mirrors the notification recipe).
    ImGuiIO &io = ImGui::GetIO();
    ImVec2 pos((corner & 1) ? io.DisplaySize.x - distance : distance,
               (corner & 2) ? io.DisplaySize.y - distance : distance);
    if (!(corner & 2)) {
        pos.y = g_main_menu_height + distance; // clear the (fading) menubar
    }
    const ImVec2 pivot((corner & 1) ? 1.0f : 0.0f, (corner & 2) ? 1.0f : 0.0f);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);

    float opacity = g_config.display.ui.sbc_overlay.opacity;
    if (opacity < 0.05f) opacity = 0.05f;
    if (opacity > 1.0f)  opacity = 1.0f;
    ImGui::SetNextWindowBgAlpha(opacity);

    // Click-through and focus-free: the mouse is the aiming lever (raw
    // SDL_GetMouseState in xemu_input.c) — the overlay must never take it.
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoInputs;

    if (ImGui::Begin("SBC Cockpit", NULL, flags)) {
        // 1. Drivetrain — the readout that would have made ISS-B11 a non-event.
        const char *gear = GearGlyph(sbc.gearLever);
        const bool reverse = (sbc.gearLever == 254);
        ImGui::TextColored(kDim, "GEAR");
        ImGui::SameLine(0.0f, 4.0f);
        ImGui::TextColored(reverse ? kWarn : kOn, "[%s]", gear);
        ImGui::SameLine(0.0f, 10.0f);
        ImGui::TextColored(kDim, "TUNER");
        ImGui::SameLine(0.0f, 4.0f);
        ImGui::TextColored(kOn, "%02u", (unsigned)sbc.tunerDial);

        PedalMeter("L", sbc.axis[SBC_AXIS_LEFT_PEDAL]);
        ImGui::SameLine(0.0f, 6.0f);
        PedalMeter("M", sbc.axis[SBC_AXIS_MIDDLE_PEDAL]);
        ImGui::SameLine(0.0f, 6.0f);
        PedalMeter("R", sbc.axis[SBC_AXIS_RIGHT_PEDAL]);

        ImGui::Separator();

        // 2. Toggle lamps.
        for (size_t i = 0; i < std::size(g_toggles); i++) {
            const uint8_t bit = (uint8_t)(g_toggles[i].button >> 32);
            if (i) {
                ImGui::SameLine(0.0f, 8.0f);
            }
            LabelledLamp(g_toggles[i].label, (sbc.toggleSwitches & bit) != 0,
                         g_toggles[i].scancode);
        }

        ImGui::Separator();

        // 3. Startup ritual momentaries.
        ButtonRow(g_startup, std::size(g_startup), sbc.buttons, 5);

        // 4. Combat core + aim.
        ImGui::Separator();
        ButtonRow(g_combat, std::size(g_combat), sbc.buttons, 6);
        AxisMeter("AIM X", sbc.axis[SBC_AXIS_AIMING_X]);
        ImGui::SameLine(0.0f, 6.0f);
        AxisMeter("Y", sbc.axis[SBC_AXIS_AIMING_Y]);
        AxisMeter("ROT", sbc.axis[SBC_AXIS_ROTATION_LEVER]);

        // 5. Everything else (default collapsed).
        if (ImGui::CollapsingHeader(
                "All controls",
                g_config.display.ui.sbc_overlay.show_all_controls ?
                    ImGuiTreeNodeFlags_DefaultOpen : 0)) {
            ButtonRow(g_rest, std::size(g_rest), sbc.buttons, 3);
            ImGui::Separator();
            ButtonRow(g_axis_keys, std::size(g_axis_keys), 0, 3);
        }
    }
    ImGui::End();
}
