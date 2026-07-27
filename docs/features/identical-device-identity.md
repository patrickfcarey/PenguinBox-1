# Telling two identical controllers apart (LEFT vs RIGHT)

**Researched 2026-07-25** (three parallel investigations: Linux/udev, SDL+xemu
source, and sim-community prior art). Driving case: the recommended twin-stick
loadout is **2× Thrustmaster T.16000M** (`044f:b10a`) — identical VID/PID, name,
and axis/button counts. This is our **default configuration**, not a corner case.

## 1. The two failure modes (do not conflate them)

- **(a) Identity collision** — software literally cannot tell the devices apart,
  so bindings merge or cross.
- **(b) Order instability** — identity is fine but the *index* shuffles across
  reboots/replugs. This is an OS-level fact, not a bug in any one app.

Fixing (a) does not fix (b) and vice versa. Keying on a **physical-port identity**
fixes both, because it is neither name-derived nor order-derived.

## 2. Confirmed facts

- **The T.16000M exposes NO USB serial.** Proven from a real dual-stick
  `udevadm` capture (`ID_SERIAL=Thrustmaster_T.16000M` with no serial component)
  plus systemd's `usb_id` builtin, which appends `_Serial` **only** when the
  descriptor supplies one and sets `ID_SERIAL_SHORT` **only** when a serial
  exists. Verify locally: `lsusb -v -d 044f:b10a | grep iSerial` (expect
  `iSerial 0`).
- **`/dev/input/by-id/` COLLIDES** — both sticks want
  `usb-Thrustmaster_T.16000M-joystick`; the second silently overwrites the first,
  and udev documents the winner as *"undefined."* Unusable.
- **`/dev/input/by-path/` DISAMBIGUATES** — `…usb-0:1:1.0` vs `…usb-0:2:1.0`.
- **`ID_PATH` is reboot-stable by design.** systemd's `path_id` builtin
  **deliberately discards the USB bus number** (replacing it with `0`) precisely
  because it changes across reboots. ⇒ **Match `ID_PATH`, never `KERNELS=="3-1"`**
  (the common blog advice), which embeds the unstable bus number.
- **Nothing SDL exposes is both unique and stable on Linux:**

| SDL API | Unique per unit? | Stable across replug/reboot? |
|---|---|---|
| `SDL_GetJoystickGUID` | ❌ byte-identical (serial deliberately excluded) | ✅ |
| `SDL_GetJoystickSerial` | ❌ NULL (no iSerial) | — |
| `SDL_GetJoystickPath` | ✅ (`/dev/input/eventN`) | ❌ renumbered on hotplug |
| `SDL_GetJoystickID` / `PlayerIndex` | ✅ | ❌ connection-order artifacts |

  ⇒ the app must resolve the **port topology itself**.

## 3. Prior art — who solved it, who did not

| Sim | Keys on | Works? |
|---|---|---|
| **DCS World** | per-instance GUID, in the *filename* | ✅ **best in class** — and human-repairable by renaming |
| Elite Dangerous | GUID in XML | ⚠️ format yes, UI no — and it **silently rewrites binds when a device is absent** |
| Star Citizen | USB enumeration order | ❌ swaps every reboot |
| MSFS | device **name** | ❌ acknowledged bug; profiles merge |
| RetroArch | device index | ❌ open complaints since 2021 |

**The most damning data point: Thrustmaster's own T.A.R.G.E.T. cannot do it** —
*"there isn't a reliable way to tell which joystick is the left one… the only way
to change it seems to be randomly plugging the joysticks to new USB slots."* The
vendor's flagship tool for this exact hardware fails. Nobody is coming to save us;
**we key on port path or we have the bug everyone else has.**

**Writing a unique serial into the device is a dead end** — only VIRPIL exposes
firmware VID/PID/name editing. Thrustmaster and VKB do not.

## 4. The design

**Primary key = OS physical-port path.** Linux: resolve the devnode from
`SDL_GetJoystickPath()` against `/dev/input/by-path/*-event-joystick` (compare
`st_rdev`), or read `ID_PATH` via libudev. Windows: `SDL_GetJoystickPath()`
already returns the stable HID instance path there — **the platforms differ, do
not assume**.

**Recovery = interactive assignment.** *"Move the stick you want as LEFT."*
Identification by **observed input** is the only method that survives every
identity change, because it uses the one identifier that never breaks: the human.
It also *is* the left/right assignment we need regardless.

**Degradation ladder (run at every startup):**
1. **Both resolve by path** → silent, done.
2. **Exactly one resolves** → the other must be the remaining identical device →
   auto-heal, and say so quietly.
3. **Neither resolves** → **do not guess, do not silently rebind** → prompt the
   identification wizard.

**Explicitly rejected as persistence keys:** SDL device index, `GUID`, product
name, `SDL_GetJoystickSerial()`. All four are provably non-unique or non-stable
here, and every sim that shipped one has a years-old complaint thread.

## 5. UX conventions worth copying (hard-won by other projects)

1. **Interactive assignment as a first-run wizard *and* an always-available
   "Re-identify devices" action.**
2. **Never silently rewrite bindings when a device is missing.** Treat absent as
   **pending**, not deleted. (Elite Dangerous's most-cursed behaviour: opening the
   controls screen unplugged destroys the mapping.)
3. **Re-detect on hotplug**, and offer **device substitution** — let the user
   re-point an existing profile at a different physical device instead of forcing
   a full rebind.
4. **Show which is which.** Name and VID/PID are identical, so the UI must
   disambiguate: display the resolved port path, and offer an **"Identify"**
   action (rumble it / highlight on movement). Users cannot self-serve without
   this — it is the #1 complaint about ED's dual-stick UI.
5. **Keep the config human-repairable.** DCS and SC users built their own repair
   tooling *because* the formats were text. An opaque blob would be unrecoverable.
6. **Physically label the two USB ports.** The #1 fragility is a human swapping
   the plugs — it inverts left/right **silently, with no error anywhere**.

## 6. xemu-specific work (three tiers, in order)

**Tier 0 — BLOCKER: xemu cannot see ANY non-gamepad device** — not a T.16000M,
not a **G923 wheel**, not a standalone pedal set. Verified against origin:
`SDL_Init(SDL_INIT_GAMEPAD)` (`ui/xemu-input.c:277`), only
`SDL_EVENT_GAMEPAD_ADDED` handled (`:468`), and the add path bails when
`SDL_OpenGamepad()` returns NULL (`:473`). SDL calls a device a *gamepad* only if
it has a **mapping**; everything else is a *joystick*, and is invisible.
**This happens before any configuration exists, so no config or calibration tool
can reach it.**

Two facts make it worse than it looks:
- **The documented escape hatch is dead code.** `input.gamecontrollerdb_path`
  (`config_spec.yml:54`) has **zero readers** — no
  `SDL_AddGamepadMappingsFromFile`, no `SDL_HINT_GAMECONTROLLERCONFIG`
  (lost in the SDL2→SDL3 migration). We cannot even *supply* a mapping today.
- **A mapping would not be enough anyway.** SDL's gamepad abstraction exposes
  **6 axes**; the SBC needs **8** (aiming X/Y, rotation lever, 3 pedals,
  sight-change X/Y) — and a wheel alone is steering + 3 pedals competing for
  thumbstick/trigger slots.

⇒ **The fix is the joystick path, not the mappings file.** Handle
`SDL_EVENT_JOYSTICK_ADDED` / `SDL_OpenJoystick` and read raw joystick axes: that
removes the 6-axis ceiling *and* makes devices visible with no mapping at all.
Restoring the mappings loader is a worthwhile separate courtesy, but it does
**not** substitute. *Nothing else matters until Tier 0 is done.*

**The hardware is fine; the emulator is not:**
- **Logitech G923** — `046d:c267` (PS/PC) / `046d:c26d` (Xbox), works on Linux as
  a standard HID joystick. Known quirk: **pedals report inverted**, which the
  calibration design already handles by capturing rest and full travel
  (`sbc-analog-input.md` §6).
- **Thrustmaster T.16000M** — `044f:b10a`, absent from SDL 3.4.10's built-in DB.

**One Tier-0 fix serves the twin sticks, the wheel and the pedals** — the single
highest-leverage change in the whole effort.

**Tier 1 — identity (~30 lines).** xemu persists **GUID alone**
(`ui/xemu-input.c:687-699`) and restores by `strcmp` (`:313-329`), so two
identical sticks fall through to "first available port" and can **flip between
boots with no error**. The code already admits this for X360 receivers
(`:383-394`). Fix: persist `"<guid>:<unit_id>"` where `unit_id` is the port path
(or serial when non-NULL), matching the extended key first and falling back to
bare GUID for backward compatibility. Also note **per-device remaps are
GUID-keyed too** (`ui/xemu-settings.cc:275`), so today both sticks share one
mapping entry and a reset clears both.

**Tier 2 — UX.** The identify flow, port-path display, and disambiguating labels
in the controller picker (`ui/xui/main-menu.cc:283-288`, currently two visually
identical rows).

## 7. Optional: a udev helper (sudo, separate scope)

**Not required** — §4 works entirely in userspace. Offered as a power-user
convenience: a rule matching `ENV{ID_PATH}` can mint readable
`/dev/input/js-left` / `js-right` symlinks.

**The elegant trick:** setting `ENV{ID_SERIAL_SHORT}="left"` makes
**`SDL_GetJoystickSerial()` return `"left"`/`"right"` with zero application
changes**, because SDL reads exactly that udev property. Since the T.16000M never
sets it, it is free real estate. (Holds on SDL's evdev path; a HIDAPI-routed
device would read the descriptor instead — not applicable here.)

Rule must sort **after** `60-persistent-input.rules` so `ID_PATH`/`ID_VENDOR_ID`
are populated. **Layering is non-negotiable: the emulator itself must never
require root.** The script hardens the baseline; it is not a prerequisite.

## 8. Stability ranking (what breaks port identity)

| Rank | Event | Effect |
|---|---|---|
| 1 **fatal** | human swaps the two plugs | **silently inverts left/right** — label the ports |
| 2 | stick moved to another port | link disappears → wizard |
| 3 | hub added/removed | port chain gains a level |
| 4 | PCIe USB card / PCI renumber | path prefix changes |
| 5 low | systemd major upgrade | format could gain components |
| **safe** | reboot · replug · enumeration race · kernel upgrade · hub order | **no effect** — this is the whole point |
