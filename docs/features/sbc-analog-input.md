# Steel Battalion controller — analog input & pedal calibration notes

**Provenance: SECONDHAND.** Relayed from the community `#emulation` Discord
(Quizerno, who reports reading the xemu source and the `ogx360_t4` adapter code),
captured 2026-07-25. **Treat as leads to verify against the code (§4), not as
established fact** — though note the source is credible: this community runs
near-cutting-edge builds (SBC gameplay requires the unmerged PR #1803, so
they are building from source, not running stock), and we are only slightly
ahead of them. Their findings are peer-relevant to our build. Recorded because it directly informs Phase 3 (binding real
sticks/pedals to the emulated SBC) — see
[steel-battalion-bringup.md](../steel-battalion-bringup.md).

## 1. The reported facts

- **Real SB pedal hardware is a linear 10k potentiometer.** → the response curve
  is *linear*; a straight raw→device mapping is hardware-faithful, no curve
  shaping needed for fidelity.
- **Pedals default to the max value `0xFFFF`** in both xemu and `ogx360_t4`
  ("by default they set the pedals to go to the max value").
- **Per a HOTAS configuration's notes, pedal values run `0x0000`–`0xFF00`** —
  note this is *not* `0xFFFF`; full-scale and the default may disagree.
- **Different physical devices use different potentiometer values**, so binding
  e.g. controller triggers to pedals requires **mapping the ranges properly**.
- **The current emulator binding path is key/digital-oriented:** *"in the case of
  the emulator you have to set individual values to keys and then have a joystick
  mapper go to those keys on each pressurized sense."*
- **Open community question:** *"would it be possible to spoof passthrough for
  pedals only?"* — i.e. pass a real USB pedal set straight through rather than
  binding it.

## 2. Why this matters to us — the calibration layer

Our plan keeps **real pedals physical** (a 3-pedal racing set) feeding the
*emulated* SBC. Three consequences fall out of §1:

1. **Range calibration is required, not optional.** Our host pedals report their
   own device-specific raw range (HID); the emulated SBC expects its own scale
   (reportedly `0x0000`–`0xFF00`). We need an explicit **raw→device map with
   per-device min/max calibration** (rest position and full travel differ between
   pedal sets, and even between the three pedals of one set). Because the real
   part is a *linear* pot, a linear map is the correct default — the calibration
   is about **endpoints**, not curve shape. This is the user's instinct
   ("recalibrate the equipment") and it is exactly right.
2. **The `0xFFFF` default is a trap.** If an axis isn't actively driven by a
   binding, the device may report **max = fully pressed**. Symptom to watch for
   on the rig: the VT behaves as though a pedal is held down at rest. Any real
   pedal binding must **drive the axis explicitly and idle at the correct rest
   value** rather than inherit the default. Also reconcile `0xFF00` (reported
   full scale) vs `0xFFFF` (reported default) — at least one is wrong, or they
   mean different things.
3. **Digital-vs-analog is the actual gap.** If the SBC binding path is key-based,
   analog pedals cannot express travel: the workaround (map pedal positions to
   several key thresholds) is a **stair-step, not analog**. Making the SBC accept
   true analog axes is the real Phase-3 work — consistent with PR #1803's own
   author noting that real multi-device binding is unbuilt, and with the port
   currently shipping a keyboard/mouse placeholder.

## 3. Relationship to our VR plan

Pedals stay **real hardware** (operated blind by feel) — this doc is about making
that binding faithful. It does **not** affect the VR button overlay (PadXR), which
handles the ~30 panel buttons. Calibration UI, if we build one, is a natural
overlay readout later ("press each pedal fully" with a live bar).

## 4. Verify in code before relying on any of this

- `hw/xbox/xid-steel-battalion.c` — the pedal report fields: actual width/type,
  the default/idle values, and whether full scale is `0xFF00` or `0xFFFF`.
- `ui/xemu-input.c` (SBC path) — does it expose **analog axes**, or only key
  bindings? This decides whether Phase 3 is "bind axes" or "extend the device".
- `ogx360_t4` (external, adapter project) — a reference for real-hardware value
  ranges and how a physical SBC's pots are read.

## 5. Open questions
- True full-scale + rest values for each pedal (and whether the three differ).
- Whether "passthrough for pedals only" is viable in xemu's USB stack — it would
  sidestep binding/calibration entirely for anyone owning real pedals.
- Do the two sticks' analog axes have the same range/default trap as the pedals?

---

## 6. REQUIRED: a calibration utility (and a front end to launch it)

**Requirement (owner, 2026-07-25):** we need a **front-end tool that launches a
calibration utility**, because the game's expected potentiometer/axis limits must
be **matched to what the user's actual pedals present**. This is not optional
polish — without it, real pedals cannot be bound correctly on arbitrary hardware.

### Why it is mandatory
- **Every pedal set reports a different raw range.** A Logitech G29/G920/G923
  pedal unit, a standalone USB set, and a DIY-adapter rig all differ — in
  full-scale value, in resting value, and often **between the three pedals of one
  unit** (travel and spring differ per pedal).
- **The SBC expects its own scale** (reportedly `0x0000`–`0xFF00`, §1) — and the
  reported **`0xFFFF` default-to-max is a trap**: an undriven axis reads as
  *fully pressed*. Calibration is what guarantees a correct **idle/rest** value,
  not just a correct maximum.
- **The real pedal is a linear 10k pot** (§1), so the map is **linear** and the
  calibration is purely about **endpoints** — rest and full travel. That makes
  the utility simple and its output small.

### What it must do
1. **Identify the device** and show its axes with **live raw values** (the user
   must see the bar move to know it is reading the right axis).
2. **Guided capture, per pedal:** "release fully" → capture rest; "press fully"
   → capture max. Repeat for all three. Handle **inverted axes** (some report
   max at rest) and dead zones automatically from the captured endpoints.
3. **Compute + store a per-device profile:** raw min/max → SBC scale, linear,
   keyed by device (so swapping hardware does not silently break the mapping).
4. **Verify visually:** a live readback showing the resulting SBC value per
   pedal, so a bad calibration is obvious *before* entering the game.
5. Same machinery generalizes to the **two sticks' axes** — build it axis-generic,
   not pedal-specific.

### Front end — where it is launched from
Options, in rough order of preference:
- **In-emulator (ImGui) dialog** — same UI stack as the existing SBC cockpit
  overlay (`ui/xui/sbc-overlay.cc`), discoverable from a menu item, and it can
  reuse the input plumbing already there. **Must be usable flat, at the desk,
  before/without a headset** — you calibrate pedals with your eyes on a monitor.
- **Standalone tool** — simplest to iterate on, but a second binary to ship and
  it duplicates device enumeration.
- **In-VR overlay panel** — attractive later (live bars floating over the real
  pedals), but a poor *primary* choice: you should not need a headset on to make
  your pedals work.

**Recommendation: an in-emulator ImGui dialog as the primary front end**, with
the in-VR panel as a later convenience that reuses the same calibration core.
Re-runnable at any time, and it must persist so it is a one-time step per rig.

### Notes
- Calibration data is **per device, not per game** — store it so any title using
  analog input benefits.
- Because the binding is **bespoke SB code** (not xemu's generic binding UI), the
  calibration format and its application are entirely ours to define.

---

## 7. The same tool is a DIAGNOSTICS panel (owner insight, 2026-07-25)

Calibration and diagnostics are the same screen: both are *"show me, live, what
the emulator thinks my hardware is doing."* Building it as a diagnostics panel
that happens to calibrate — rather than a wizard that only runs once — makes it
far more valuable.

### What it answers
- **"Is my hardware working?"** The user watches gauges move as they press
  pedals, sticks, buttons. Immediate, self-evident.
- **"Is it *my* gear or *their* emulator?"** This is the support question that
  otherwise lands on us. A live panel lets a user answer it themselves and report
  precisely ("pedal 2 never passes 60%") instead of "it feels wrong."
- **"Does my real controller still work?"** Especially valuable for owners of an
  **actual Capcom SBC controller**: that hardware is 20+ years old, and
  potentiometers drift, buttons oxidize, pedals wear. A per-input readout tells
  them exactly which of the 44 functions still register.
- **"Can I fix it?"** If a real controller's pots have drifted out of their
  original range, **recalibration is the fix** — so the diagnostics view must be
  able to hand off to the calibration flow *for real hardware too*, not just for
  substitute pedals.

### What it must show
- **All 44 SBC functions**, not just the axes under calibration: pedals, both
  sticks, the toggles, the gear lever, the tuner dial, every button.
- **Raw value AND mapped value** side by side — that is what distinguishes "the
  device is broken" from "our mapping is wrong."
- **Automatic fault hints** where they are unambiguous:
  - *axis pinned at maximum* → the **`0xFFFF` default-to-max trap** (§2.2),
    visible instantly instead of being discovered as mysterious in-game behavior;
  - *axis never reaches full travel* → calibration needed, or worn pot;
  - *input never fires* → unbound, dead switch, or wrong device;
  - *axis jitters at rest* → dirty/worn potentiometer.
- **Source attribution** per input — real HID device / VR overlay / keyboard
  placeholder — so it is clear *what* is driving each function.

### Scope note
It must work for **both** paths: the emulated SBC fed by bound devices/overlay,
**and** a real SBC controller (passthrough/adapter). Same panel, same gauges —
only the source differs.

### Code precedent to build on
**xemu already has a controller test mode**: `xemu_input_get_test_mode()`, checked
at the top of `update_input()` in `hw/xbox/xid.c:57`, which suppresses reporting
to the guest while the user is testing a controller. That is exactly the pattern
this panel needs — read and display inputs **without** feeding the running game —
and it is the natural thing to extend rather than reinvent.

---

## 8. Per-game profiles drive the panel (owner correction, 2026-07-25)

**The problem with §7 as written:** it implied the panel always shows all 44 SBC
functions. That is wrong outside Steel Battalion — nobody calibrating pedals for
a racing title wants to wade through a mech's comms buttons and tuner dial. **The
panel must show only what the active profile declares.**

### The distinction that was collapsed — calibrate the DEVICE, map per GAME

| layer | scope | changes when… | example |
|---|---|---|---|
| **Device calibration** (raw endpoints) | **per DEVICE**, once | you change hardware | "this G29's brake axis rests at 130, maxes at 65100, inverted" |
| **Mapping + display** (which functions exist, names, expected ranges) | **per GAME profile** | you change title | "SB: 44 functions, pedals → `0x0000`–`0xFF00`" vs "Halo: standard pad" |

**Your pedal's raw range does not change based on which game you launch.** So
calibration is a **one-time, per-device** step whose output is a *normalized*
axis; each game profile then maps that normalized value onto its own
expectations. Consequences:
- Calibrate once, benefit everywhere — no re-calibrating per title.
- Swapping pedals invalidates *calibration* only, not every game profile.
- A game profile is authored without knowing anything about the user's hardware.

### What the panel renders
**Whatever the active profile declares, and nothing else.**
- **Steel Battalion** → the full 44: 3 pedals, 2 sticks, tuner, gear lever, ~30
  panel buttons.
- **A standard-pad title** (Halo, OFP) → sticks, triggers, ~12 buttons. No mech
  controls anywhere in the UI.
- **Device-only mode** (no game / hardware check) → just the raw axes and buttons
  the *device* exposes, unmapped — the honest "is my hardware alive?" view, which
  is also the right screen for someone verifying a real SBC controller before
  worrying about any title.

### One profile, three consumers
This unifies with what the profile system already does. The same per-title
profile declares the control set for:
1. the **VR overlay** (which widgets to render — PadXR `13-padxr-spec` §7),
2. the **input injection** (what each control drives),
3. the **calibration/diagnostics panel** (what to display and test).

**Single source of truth** — a title's controls are described once. Adding a game
means adding a profile, not touching the calibration UI. This is also why the
panel stays useful beyond Steel Battalion instead of being an SB-only tool.

---

## 9. Calibration validity: one version, device-bound, invalidated on change

**Owner requirement (2026-07-25):** a profile carries **exactly one saved
calibration at any time**, it is **device-specific**, and **changing the hardware
invalidates it** — you must calibrate again.

**Why this matters (it is the stale-binary trap, wearing a different hat).** A
calibration silently applied to *different* hardware produces confidently wrong
results: axes that never reach full travel, or sit pinned at max (the `0xFFFF`
trap, §2.2). The user then experiences it as "the game feels broken" or "the
emulator is broken" — the failure is invisible at the layer where it happened.
Same class of bug as testing on a stale binary, and it deserves the same
treatment: **fail loudly rather than proceed on a stale artifact.**

### The invariant
1. **One calibration per profile, live at a time.** No history, no variants, no
   ambiguity about which is active.
2. **Bound to device identity.** Store a fingerprint alongside the values:
   **USB VID/PID + serial (when present) + product name + axis/button counts**.
   (Many HID devices expose no unique serial, so the name + layout fingerprint is
   the practical fallback.)
3. **Verified at bind time, every launch.** Fingerprint mismatch ⇒ the
   calibration is **invalid**: do **not** apply it, say so plainly, and route the
   user to recalibrate.
4. **Never silently substitute defaults.** An uncalibrated axis is a *known*
   state to be surfaced, not papered over — especially given the default-to-max
   trap.

### DECIDED (2026-07-25): keyed by device fingerprint

Calibrations are stored in a **map of `device fingerprint → calibration`**:
- **One calibration per device** — satisfies "one version at a time."
- **A changed device has no entry**, so it is invalid and prompts calibration —
  exactly the required behaviour.
- **Swapping back to previous hardware just works**, with no redundant
  re-calibration (the reason this beats a strict single slot).

The non-negotiable part is unchanged: **a foreign or absent calibration is never
silently used.** Provide a "clear calibrations" action so stale entries from
long-gone hardware can be pruned.

### EDGE CASE — two identical devices → RESEARCHED, see the dedicated doc

The recommended twin-stick loadout is **2× Thrustmaster T.16000M**: identical
VID/PID, name and counts, and **no USB serial at all**, so a fingerprint alone
cannot tell LEFT from RIGHT and the keyed store would collide. This is our
*default* configuration, not a corner case.

**Fully researched 2026-07-25 — see
[identical-device-identity.md](identical-device-identity.md).** Summary:
- **Primary key = OS physical-port path** (Linux `ID_PATH` / `by-path`; Windows
  HID instance path). It is the only stable *and* unique discriminator, and it
  fixes both identity collision and enumeration-order shuffling.
- **Recovery = interactive assignment** ("move the stick you want as LEFT"),
  triggered automatically whenever the stored path fails to resolve.
- **Rejected:** SDL index, GUID, product name, `SDL_GetJoystickSerial()` — all
  provably non-unique or non-stable here.
- **No root required** for the baseline; an optional udev helper is a
  power-user convenience only.
- **Blocker found:** xemu cannot currently enumerate a T.16000M **at all**
  (gamepad-only init + no SDL mapping-DB entry + a dead
  `input.gamecontrollerdb_path` config key). That must be fixed first.

### Related
Device calibration is per-device and normalized (§8); the per-game profile maps
that normalized value. So invalidating a calibration affects **only** the
hardware layer — every game profile stays valid and needs no re-authoring.

---

## 10. Pre-flight checklist — gate the launch (owner requirement, 2026-07-25)

**Requirement:** when the emulator recognizes **Steel Battalion**, the
diagnostics/calibration tool launches **first**, as a **pre-flight checklist**.
Only once everything is green — or the user deliberately presses **Skip** — does
the game boot.

**The failure it prevents:** a user whose racing wheel came unplugged (or whose
pedals moved to another PC, or whose calibration was invalidated by a hardware
swap, §9) should **never reach a mission before finding out**. Discovering dead
pedals mid-combat is the worst possible time; the checklist moves that discovery
to before boot, where it is a five-second fix instead of a lost sortie.

**Thematic fit (a genuine bonus, not decoration).** Steel Battalion's own startup
is a ritual — cockpit hatch, systems toggles, ignition, start. A pre-flight
hardware check *is* that ritual, one layer out. The utility reads as part of the
experience rather than as a settings dialog.

### Flow
1. Title recognized (**XBE Title ID** — the same key the profile system uses).
2. Profile declares the expected control set (§8) **and which controls are
   `required` vs `optional`** — a schema addition this feature needs.
3. Checklist evaluates and shows a per-item verdict:
   - **✅ ready** — device present, calibration valid (fingerprint match, §9),
     inputs responding.
   - **⚠️ warning** — present but uncalibrated, or an optional control missing
     (e.g. no shifter → the overlay covers it). Proceed allowed.
   - **❌ blocking** — a **required** device absent, calibration **invalid**, or a
     fault detected (axis pinned at max = the `0xFFFF` trap, §2.2; axis never
     reaching full travel; input never firing).
4. **All green → continue** (auto-proceed, or a single confirm).
5. **Not green → do not launch.** State plainly *what* is wrong and offer the fix
   inline: **Calibrate**, **Re-scan devices**, or **Skip anyway**.
6. **Skip is always available** — the user stays in charge — but it must be a
   deliberate press, never the default path.

### Design notes
- **Live gauges, not just a static verdict.** The checklist is the §7 diagnostics
  panel in a gating role: the user should be able to press each pedal and *watch*
  it move. "Ready" that the user has personally seen is worth far more than
  "ready" asserted by software.
- **Profile-driven, SB-first — not SB-only.** The mechanism is general: any title
  whose profile declares required hardware gets a checklist. Titles needing only
  a standard pad show a trivial check or none at all. SB is simply the flagship
  case, because it is the one with 44 inputs and real hardware to lose.
- **Where it hooks:** after title identification, before the guest boots —
  either as a modal in xemu's boot path or in the launcher wrapper. **Implementation
  decision pending.**
- Must be **re-openable during play** (not launch-only), so a mid-session
  disconnect can be diagnosed without quitting.

---

## 11. USB passthrough — the community's existing path, and what it means for us

**Context (relayed by the owner, 2026-07-25):** the Steel Battalion community
maintains a **private repo working on USB passthrough for xemu**, and they have
**real OEM Steel Battalion controllers working with the emulator** through it.
**Before our keyboard binding landed, passthrough was the only way to play at
all.** (Upstream also has an open passthrough effort: xemu PR **#2854**, "UI: USB
Passthrough".) This also explains the Discord question recorded in §1 —
*"would it be possible to spoof passthrough for pedals only?"* — that community
is working from a passthrough starting point, not a binding one.

### Two user populations, and we serve different ones

| population | input path | needs our calibration? |
|---|---|---|
| **Owns an OEM SBC controller** | **USB passthrough** — guest talks to the real device | ❌ no — the real controller *is* the device |
| **Everyone else** (substitute sticks/pedals, or keyboard) | **emulated SBC device** fed by bound HID + overlay | ✅ yes — §6–§10 exist for them |

These are **complementary, not competing**. The OEM controller is rare and
expensive; our binding + calibration + overlay work is what makes Steel Battalion
playable for people who will never own one — and the VR layer is additive for
both groups. Worth stating plainly so the effort is not mistaken for duplicating
what they already have.

### ⚠️ Architectural tension: passthrough bypasses our injection seam

This needs deciding before the overlay work goes deep. **USB passthrough hands
the guest the real device directly** — it does *not* route through
`update_input()` / `ControllerState` in `hw/xbox/xid.c`. But that function is
**exactly** where the PadXR overlay injects (`state->buttons |= …`, the confirmed
seam). Therefore:

- **Emulated SBC + bound hardware** → overlay injection **works** (our path).
- **USB passthrough** → **no injection point**; the VR button overlay cannot add
  inputs, because nothing of ours sits between the controller and the guest.

So **passthrough and the VR overlay are, as things stand, mutually exclusive** —
and that matters most for the very users most invested in this game.

> **RESOLVED — see §12.** The decision is to converge on a single pathway: the
> real controller becomes an *input source* feeding the **emulated** device, so
> the overlay (and haptics, calibration, diagnostics) work for everyone.

### Open questions (do not guess — these change the plan)
1. **Can an OEM SBC controller be read as a normal HID device on Linux** and fed
   into the *emulated* device instead of passed through? If yes, OEM owners get
   **real controller + VR overlay together**, which is the best of both. If no
   (it is an XID-class device, which is *why* passthrough is used), then OEM
   owners must choose: authentic passthrough, or overlay-augmented emulation.
2. **Could the overlay inject at the USB layer** for passthrough sessions —
   merging synthetic reports with the real device's? Much harder than the
   `ControllerState` seam, but it would remove the exclusivity.
3. **"Passthrough for pedals only"** (their question) — a hybrid where real
   pedals pass through while other functions come from binding/overlay. Would
   sidestep calibration entirely for pedal owners.
4. **Coordinate rather than duplicate.** They are ahead of us on passthrough; we
   are ahead on the DMA/surface fix (#2514) and the entire VR layer. Worth
   comparing notes before building anything passthrough-shaped.

### 11a. The community fork: `quizerno/Xemu-SB-VK` (inspected 2026-07-25)

**It is PUBLIC**, not private — https://github.com/quizerno/Xemu-SB-VK
*"Xemu with more features for Steel Battalion and QOL."* A fork of
**`faha223/xemu`** (the author of PR #1803 and of the OHCI fix #1531), by
**quizerno** + **avibodek**, collaborating with **SpecialFred** as part of a
Steel Battalion newcomer controller guide. Last pushed 2026-07-15. C, SDL3 for
the VK build / SDL2 for the NVK build, cross-compiled for **Windows** via
Docker/Podman.

**What it adds:** hotkey toggles, fullscreen click-toggle, menu show/hide,
cursor visibility, SBC controller graphics, and — VK-only — **fully rebindable
keyboard config inside the emulator**. Planned: unified hotkeys, mouse rebinding,
**gear shifter / tuner dial options** (overlaps our §6 work).

**Their input path:** SBC controller via **USB passthrough using libusb**; users
currently need **JoystickGremlin** externally to map keyboard configs onto
joysticks — precisely the workaround our native binding + calibration (§6–§10)
would remove.

**Branches of interest** (inherited from faha223 + their own):
- **`enable_libusb`** — the USB passthrough path (§11). Last touched
  **2025-11-08**; not recently active.
- `DeviceEmulation-SteelBattalionController` — PR #1803's branch (we ported it).
- **`VulkanLineLoopFix`** — *"Initial attempt to draw line loops using line
  strips"* (SpecialFred, 2025-01-15). **Directly relevant to us: we force the
  Vulkan renderer for VR** (H-3), so any line-loop gap affects our build's
  HUD/reticle rendering. **Watch item — evaluate whether we need it.**
- `ohci_fix` (#1531), `LightGun`, `DeviceEmulation-ArcadeStick`, `deb`,
  `ppa-snapshot`.

### 11b. A concrete two-way exchange

**They do NOT have the #2514 DMA/surface-sync fix** — confirmed by two searches
over their `master` (zero matching commits). And their stated limitation is
*"performance remains inconsistent… some users can only reliably play the first
four levels"* — which matches the Discord report of mission 04 being slow.

**That is very plausibly the bug we already fixed.** Our port of #2514 (guest DMA
writes over a live GPU surface now trigger the surface download/invalidate rather
than silently scribbling) is what took us from the #1238 crash to **in-cockpit
and combat-verified with a clean log**. Worth offering them — it is a concrete,
high-value contribution and costs us nothing.

**Conversely, `VulkanLineLoopFix` is something we may want to take**, since we are
Vulkan-only for VR and they are not.

| we give | we take |
|---|---|
| **#2514 DMA/surface-sync port** — likely fixes their past-level-4 instability | **`VulkanLineLoopFix`** — line-loop rendering under Vulkan |
| the VR layer (entirely ours) | passthrough experience (`enable_libusb`) |

Platforms are complementary too: **they target Windows, we target Linux.**

---

## 12. ARCHITECTURAL DECISION — one pathway: everything feeds the EMULATED device

**Owner decision (2026-07-25):** build on the community's work, but converge on
**a single input pathway — the emulated SBC device** — that *everyone* hooks into,
**whether or not they own the real controller.** Real OEM controllers become an
*input source* feeding the emulated device, rather than being passed straight
through to the guest.

```
   real OEM SBC controller ─┐
   sticks / pedals / wheel  ├─► host input layer ─► EMULATED SBC device ─► game
   keyboard                 │      (ControllerState)        ▲
   VR overlay (PadXR) ──────┘                               │
                                          haptics · calibration · diagnostics
```

### Why this is the right call
This **resolves the §11 exclusivity** instead of living with it:
1. **One injection point** — the PadXR overlay works for *everyone*, including OEM
   owners. Under passthrough they could never have it (nothing of ours sits
   between controller and guest).
2. **Every feature composes.** Overlay + real controller + haptics + readouts all
   coexist, because they all meet in one place.
3. **Calibration and diagnostics apply uniformly** — including to a **real
   controller**, which matters enormously for 20-year-old hardware with drifting
   pots (§7). Passthrough offers no place to correct or even observe that.
4. **One code path** to build, test and debug — not two experiences diverging.
5. **No second-class users.** The person with substitute pedals and the person
   with an OEM cockpit get the same feature set.

### The technical crux — and why it is tractable
The OEM SBC is an **XID-class device**, not standard HID, which is *why*
passthrough exists: it does not present as a normal joystick. So we need a
**host-side reader** rather than a kernel joystick:
1. **Claim the device with libusb** — exactly what their `enable_libusb` branch
   already does for passthrough. **We build on that**; the claiming/detach work is
   the same.
2. **Parse its XID input reports** — and we already know this format, because
   PR #1803's `hw/xbox/xid-steel-battalion.c` *implements* it. Reading is the
   mirror of emulating.
3. **Populate `ControllerState`** instead of forwarding raw USB to the guest.

In other words: **passthrough, but routed through our input layer.** The
difference from their branch is *where the bytes go* — parsed into our state
rather than piped to the guest.

### What we take from their work
- **`enable_libusb`** — the libusb claiming/permissions groundwork.
- **PR #1803's report layout** — the authoritative SBC report definition.
- Their field knowledge of real-controller behaviour (pedal ranges, §1).

### Open questions
1. **Latency** — one extra parse/populate hop vs. raw forwarding. Expected
   negligible (USB HID rates), but **measure**, since this is a twitch game.
2. **USB permissions** — claiming a device from userspace needs udev access rules
   (`uaccess`/`plugdev`). Same category as the optional helper in
   [identical-device-identity.md](identical-device-identity.md) §7 — a documented
   one-time setup step, never a requirement for the emulator to run.
3. **Fidelity** — does any SBC function fail to survive parse-and-repopulate
   (e.g. timing-sensitive or vendor-specific reports)? Verify against a real
   controller before declaring parity with passthrough.
4. **Should passthrough remain available** as a fallback for anyone who wants
   byte-exact authenticity and no VR? Cheap to keep, and a useful A/B reference
   for validating (3).

### Consequence for the plan
This makes the emulated-device path the **trunk**, not a substitute-hardware
side-branch — so the calibration utility (§6), diagnostics panel (§7), per-game
profiles (§8), device identity (§9) and pre-flight checklist (§10) all serve
**100% of users**, not just those without the real controller.
