//
// PenguinBox — Steel Battalion cockpit input overlay (desktop mirror)
//
// Copyright (C) 2026 Patrick Carey
//
// SPDX-License-Identifier: MIT
//
// A compact, click-through ImGui panel showing LIVE Steel Battalion Controller
// state while the keyboard drives the SBC — the only surface that shows the
// ~49 SBC bindings (the Input-menu rebind table covers just the 25-entry
// gamepad map). Motivated by the ISS-B11 "can't move" session: the mech idled
// in REVERSE, which a gear readout would have made a non-event.
//
// Design + scope: docs/features/sbc-cockpit-overlay.md.
//
// THREADING: writer (xemu_input_update_controllers) and reader (this draw) run
// sequentially on the display-loop thread under xemu_main_loop_lock — plain
// reads, no atomics.
//
// OFF-STATE: with no keyboard-fed SBC bound, every entry point is inert and
// nothing is drawn — Duke-bound runs are pixel-identical.
//
#pragma once

/// True when a keyboard-fed SBC is bound (gates the overlay, the hotkey, and
/// the View-menu item's enabled state).
bool SbcOverlayAvailable(void);

/// Draw the overlay. No-op unless enabled AND a keyboard-fed SBC is bound.
/// Never captures input (NoInputs) — the mouse is the aiming lever.
void SbcOverlayDraw(void);

/// Edge-detected toggle (config display.ui.sbc_overlay.toggle_scancode,
/// default F3). Call once per frame from the hotkey site. Cockpit-wins guard:
/// if the scancode collides with any SBC binding the hotkey stands down with a
/// one-shot notification — the View-menu item still works.
void SbcOverlayProcessHotkey(void);
