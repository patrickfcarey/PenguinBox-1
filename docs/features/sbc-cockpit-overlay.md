# SBC cockpit input overlay (flat, desktop) — design

**Status: IMPLEMENTED, BUILT, AND VISUALLY VERIFIED IN-GAME** (scoped/built
2026-07-24; compiled + verified live 2026-07-26). It renders correctly
during gameplay — gear/tuner, L/M/R pedals, the five toggle lamps, the startup
ritual, and the combat controls, each with its live key binding. Screenshot:
`docs/features/img/sbc-cockpit-overlay-live.png` (also used in the community
write-up `docs/discord-perf-writeup.md`). The earlier "never compiled" caveat is
resolved — it built cleanly as part of the 2026-07-26 perf-stack builds.

## The problem

Steel Battalion is playable via the emulated Steel Battalion Controller with
the keyboard/mouse placeholder (ISS-B11) — but that means **~49 bindings with
zero in-game visibility**. The field incident that motivates this: the
"can't move" session (ISS-B11) where the mech idled in **Reverse** — a gear
readout would have made it a non-event. Additionally, the Input-menu rebind
table covers only the 25-entry gamepad keyboard map
(`ui/xui/main-menu.cc:640-757`), never the 52-entry SBC map — SBC bindings
today are toml-only and invisible in-app. This overlay is the only surface
showing them.

## What it is

A compact, semi-transparent, **click-through** ImGui window (corner-anchored,
default bottom-right) drawn during gameplay only while
`xemu_input_keyboard_feeds_sbc()` is true. Content rows mirror the cockpit
blocks:

1. **Drivetrain** — GEAR glyph `R/N/1..5` (decode 254/255/1-5 exactly as
   `ui/xui/gl-helpers.cc:1016-1038` — the field-verified switch; do not
   re-derive, see the reverted-encoding episode in ISS-B11), TUNER 0–15,
   three pedal mini-meters.
2. **Five toggle lamps** (from `sbc.toggleSwitches`).
3. **Startup ritual** momentaries (hatch/ignition/start/eject/override).
4. **Combat core** — main/sub/lock-on (mouse-hardwired,
   `ui/xemu-input.c:711-720`), magazine/chaff/extinguisher, aim axis bars.
5. **Collapsible "all controls"** (default collapsed) — the remaining ~20.

Key labels are resolved **live from
`g_config.input.keyboard_sbc_scancode_map.*`** via `SDL_GetScancodeName()` —
a rebound layout shows its own keys automatically. `show_key_labels` lets a
learned player drop to pure state display.

**Toggle:** F3 (verified free in stock and rebound layouts, outside the
ISS-B12 gated set) at the F1/F2 hotkey site (`ui/xui/main.cc:289-297`) + a
View-menu item + persisted `enable`. **Cockpit-wins guard:** if a future
rebind claims the toggle scancode, the hotkey stands down with a one-shot
notification (menu still works); `toggle_scancode` config is the escape
hatch.

## Invariants

- **Off-state:** with no keyboard-fed SBC bound, the overlay draws nothing
  and F3 is inert — Duke-bound runs are pixel-identical.
- **Never captures input:** window flags per the notification recipe
  (`ui/xui/notifications.cc:129-138`) incl. `NoInputs` — the mouse is the
  aiming lever (raw `SDL_GetMouseState`, `ui/xemu-input.c:709`); the overlay
  must be click-through and never take focus. Corner enum, no dragging.
- **Sidecar discipline (AGENTS §5):** all logic in new
  `ui/xui/sbc-overlay.cc/.hh` (MIT); upstream-file diff ≈ 8 lines
  (main.cc draw+hotkey, menubar.cc item, meson line) + one config block.
  **As built it is ~20 lines**, because the overlay also needs the SBC state
  itself: `xemu_input_get_keyboard_sbc()` was added beside the existing
  `xemu_input_keyboard_feeds_sbc()` (which is now a one-line wrapper around
  it, so the predicate has exactly one implementation). The alternative —
  guessing the keyboard-bound port from the overlay — would have duplicated
  the `bound_drivers[]` logic that lives in `xemu-input.c`.

## Threading

Writer (`xemu_input_update_controllers()`, `ui/xemu.c:963-965`) and reader
(the ImGui draw) run sequentially on the same display-loop thread under
`xemu_main_loop_lock` (`ui/xemu.c:1389-1392`) — plain reads, no atomics.

## Config (`config_spec.yml`, under `display.ui.sbc_overlay`)

`enable` (false) · `corner` (bottom_right) · `opacity` (0.85) ·
`show_key_labels` (true) · `show_all_controls` (false) ·
`toggle_scancode` (60 = F3). Codegen automatic on rebuild.

## Hazards

- **Desktop-mirror only:** the ImGui HUD does not appear in the headset —
  the headset sees the NV2A frame. An in-headset panel is a separate
  feature/design (out of scope here).
- **PR #1803 is unmerged upstream** — if `SteelBattalionState` shifts on a
  future re-port, this single new file is the fix site.
- Reuse of `RenderSteelBattalionController` was assessed and **rejected for
  v1**: no key labels (the point of the feature), unreadable below ~350 px,
  and a shared-FBO conflict with the Input menu. Possible later
  `style=full` mode.

## Deliberate scope cuts

No rebinding UI (display only) · no draggable window · no animations beyond
pressed-color swap · no gamepad-fed-SBC labels (Phase 3) · no in-headset
rendering.

## Task list

~~T1 config keys~~ **done** · ~~T2 window shell/gating~~ **done** ·
~~T3 panel content~~ **done** (~330 lines) · ~~T4 main.cc hooks + collision
guard~~ **done** · ~~T5 menu item~~ **done** · ~~T6 meson + MIT header~~
**done** · ~~T7 doc + working-log~~ **done** · **T8 rig verify — OPEN.**

T8 checklist (first build is also the first compile):
1. `./build.sh` — the codegen must emit `display.ui.sbc_overlay.*`; confirm
   the binary mtime is newer than the change (the stale-binary trap).
2. Stock layout: bind keyboard→SBC, F3, confirm the panel appears
   bottom-right with correct key labels and a live GEAR glyph (shift with
   LShift/LCtrl and watch `R/N/1..5` — the ISS-B11 readout).
3. Rebound layout: change one binding in the toml, confirm its label
   follows; set `toggle_scancode` to a bound key and confirm the hotkey
   stands down with the notification while the View item still works.
4. Off-state A/B: with a Duke bound (no keyboard-fed SBC), confirm F3 is
   inert, the menu item is greyed, and nothing draws.
