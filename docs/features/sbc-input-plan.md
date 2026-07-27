# Steel Battalion input architecture — implementation plan

**Status: PLAN (2026-07-25).** Synthesis of five parallel research investigations
(latency, USB permissions/claiming, protocol fidelity, passthrough-vs-emulated,
unknown-unknowns). Implements the §12 decision in
[sbc-analog-input.md](sbc-analog-input.md): **one input pathway — everything
feeds the emulated SBC device.**

---

## 0. What the research changed

Three of my earlier claims were wrong or incomplete. Recorded so nobody builds on them:

| earlier claim | corrected |
|---|---|
| "`state->buttons \|= bits` is the injection seam — one line." | **Too simple for the SBC.** State is *zeroed and repopulated* every refresh, from **two callers at unrelated cadences**; toggles are **XOR edge-detected**; gear/tuner are **absolute positions** where OR-ing is meaningless. Needs a merge layer (§3). |
| "Shift+F8 is free for graphical dumps." | **Already taken** — `ui/xui/main.cc:308-311` binds **Shift+F8 = save snapshot slot 4**, and `ui/xemu.c:933-935` guards F8 only against CTRL/ALT so **Shift falls through to VR recenter too**. Both fire today. |
| "Extra parse hop may cost latency." | **It is a ~14 ms *improvement*.** Today's path is quantized at **display frame rate** (SDL cached state, pumped on the vsync UI thread, plus a 2500 µs limiter) — 16.7 ms, 4× worse than the 4 ms USB floor. |

And two things turned out better than assumed: **passthrough is already built in and free** (enabling it was a 5-file, +7/−4, *zero-lines-of-C* change, already in our tree), and **no new thread or lock is needed** for the libusb reader.

---

## 1. Verified facts the plan rests on

- **Real SBC:** VID/PID `0x0A7B:0xD000`, interface class `0x58`/`0x42`/`0x00`, EP `0x82` IN + `0x01` OUT, `wMaxPacketSize` 0x20, **`bInterval` 4 (≈4 ms)**. The **emulated descriptor is byte-identical** — no interval mismatch either way.
- **`xpad` WILL claim it** — via the catch-all `{ USB_INTERFACE_INFO('X','B',0) }`, which matches *every* Xbox XID device. It binds as "Generic X-Box pad" and misparses the 26-byte report as the Duke's 20-byte layout. **Detach is mandatory**; `libusb_claim_interface()` returns `BUSY` otherwise.
- **libusb is wired into the build already** (`meson.build:2223-2227`, `CONFIG_USB_LIBUSB`, `debian/control` has the dev package) and `host-libusb.c` links **statically** (modules disabled). **But `CONFIG_USB_LIBUSB` is `#undef` on the rig** — the dev package is missing there.
- **The emulated path is where every feature lives — verified, and stronger than first stated.** PR #2854's passthrough renderer really does use `static ControllerState fake_state = { 0 };` and the UI really does print *"USB Passthrough devices can't be displayed."* Decisively: grepping its **entire 1462-line diff** for `usb_packet_copy`, `p->iov`, `wButtons`, `in_state`, `memcpy` returns **zero hits** — it adds **no input-report parsing anywhere**. The zeroed state is not a UI shortcut; it is the only state that exists. Under passthrough the overlay, calibration and diagnostics are **impossible by construction**.
- **A structural reason passthrough misbehaves with real hardware, still unfixed in xemu master:** `hw/usb/hcd-ohci.c:986` — *"We only allow one active packet per controller… as long as devices respond in a timely manner."* A real device over libusb has real latency; an **emulated device completes instantly**. That escape clause is precisely why issue #1239 (filed against a real Steel Battalion controller, **open ~4 years**) bites passthrough and not emulation.
- **Upstream agrees with this architecture.** PR **#2933** (lightgun, 2026-07-12, +1846/−6) reads real hardware host-side and feeds an *emulated* device — our exact model — and its emulated device accepts the game's `SET_REPORT` **calibration** report in-code. *(Corrected: the claim "which passthrough cannot expose" was unsourced — under passthrough a SET_REPORT is still **delivered** to the real device; what xemu loses is **visibility** into it. The xemu-sourced reason lightguns can't be passed through is unrelated — the yellow wire.)* And mborgerson (xemu lead, issue #389): *"we consider the path 'advanced' and don't really provide user support for it… we should not consider them complete until they are properly supported in the user interface."*
- **Cross-emulator precedent — a TRADE, not a rout** (corrected after verification). Dolphin's "Real Wii Remote" is indeed a host-side driver feeding the **emulated** adapter (our model), and its separate **Bluetooth Passthrough** documents real costs: **savestates do not work**, it **cannot be mixed** with emulated remotes, and the **host OS loses the adapter**. But be honest about the source: Dolphin's own article is **pro-passthrough** — *"With something as complicated as the Wii Remote, less emulation is better"* — they added it **because** their emulated adapter had worse fidelity. **Cite Dolphin for the concrete costs, never as proof that emulation wins**; they ship both and default to emulated. **PCSX2** has **no** passthrough at all (18 emulated USB device types). **RPCS3** ships both and **has been steadily adding emulated implementations of devices that previously required passthrough** (not "migrating" — no such statement exists) — and **its RPCN overlay softlocks with a passthrough device**, escapable only via a pad bound through the normal menu. That last one is PadXR's exact failure mode, observed in the wild.

---

## 2. Phase 0 — Prerequisites (little or no new code)

**P0.1 — Make libusb real on the rig.** `sudo apt install libusb-1.0-0-dev`, **reconfigure** (a plain rebuild will not re-probe), confirm `CONFIG_USB_LIBUSB` is defined and the binary mtime moved (stale-binary rule).

**P0.2 — Install the udev rule.** Ship **both** mechanisms:
- `TAG+="uaccess"` — correct modern mechanism, grants an ACL to the active local seat.
- `MODE="0660", GROUP="plugdev"` — **required fallback**, because a session launched over **SSH has no seat** and uaccess silently never applies. This project runs on SSH constantly; without the fallback it looks like "the rule doesn't work."
File sorts **before** `73-seat-late.rules`; lowercase 4-digit hex; never `MODE="0666"`.
Verify with `getfacl /dev/bus/usb/BBB/DDD` **before writing any code**.

**P0.3 — Zero-code smoke test.** xemu forwards argv and `usb-host` is in-tree, so via Debug→Monitor (`~`):
```
device_add usb-hub,port=1.3
device_add usb-host,vendorid=0x0a7b,productid=0xd000,port=1.3.1
```
If the game sees the controller, **permissions + kernel detach are proven end-to-end** with nothing written. This is a *bring-up test*, not the product.

**P0.4 — Settle one open number.** Count `update_sbc_input()` invocations/sec: **~250 ⇒ the guest honours bInterval=4**; ~125 ⇒ it polls at 8 ms and the real floor is double. One counter, decides the latency budget.

---

## 3. Phase 1 — Foundation fixes (BEFORE new features)

These are pre-existing defects that the new work would otherwise inherit or amplify.

**F1 — Hotkey arbiter.** Three independent consumers read the keyboard (ImGui snapshots, SDL scancodes, the polled emulated pad). **Shift+F8 currently fires two actions.** And **ISS-B12 as written is unimplementable**: the keyboard→SBC path is a *polled global snapshot* (`SDL_GetKeyboardState`), so there is no "consume" step to gate on. Build one `xemu_hotkeys_dispatch()` consulted by all paths, with an explicit *"keyboard is owned by the guest device"* predicate.
→ **Also correct the hotkey table in `graphical-dumps.md`**, which omits the F5–F8 snapshot bindings and wrongly claims Shift+F8 is free.

**F2 — Driver-string safety.** `get_bound_driver()` returns `DRIVER_DUKE` for *any* unrecognised string, and `xemu_input_bind()` then **writes it back**, destroying an SBC selection. Two builds sharing one home dir silently corrupt config — the ISS-B09 rig-drift shape. Fail loudly; never write back an unrecognised binding.

**F3 — Save-state guard.** The vmstate section id embeds the **device type name**, so changing a port Duke→SBC makes every snapshot for that port unloadable — and it fails **mid-stream**, after RAM/CPU are already overwritten, leaving a torn paused VM. Record the bound driver per port in the snapshot's existing extra-data blob and **refuse the load up front with a clear message**.

**F4 — Set the driver before boot.** Switching drivers mid-run does `qdev_unplug` + re-add; SB almost certainly enumerates once at boot, so a late switch is not seen. **This is also a plausible explanation for the long-standing "no input at the menu" symptom** — worth testing directly.

---

## 4. Phase 2 — The merge layer (the core new work)

**In NEW files**, so our diff stays off the files PR #1803 refactors (keeping it rebasable and upstreamable).

**Contract:** every source publishes a **complete, immutable frame of absolute values** — buttons as *level*, toggles as *level*, gear/tuner as *position*, axes as *value*. A **pure function** merges frames → one frame, applied **inside** the refresh so nothing clobbers it.

**Per-control policy (explicit, not implicit):**
- momentary buttons → **OR**
- latching toggles, gear, tuner → **last-writer-wins with a defined owner**. With a real SBC present it **owns** these absolutely and the overlay is advisory — otherwise they desync permanently (the emulator's XOR model vs. the physical switch's true level).
- axes → priority, or largest deviation from centre.

**Drop `previousButtons`/XOR entirely**; each source owns its own latch state.

**Why this matters concretely:** in the current model a VR overlay "tap" shorter than one refresh interval is **never observed** — it would present as a dead button, not a timing bug.

**Testability (there is currently zero input-layer test coverage):** because the merge is a pure function, it is unit-testable **off-rig**. Add a **synthetic replay source** that plays back a recorded SBC report stream — that validates the device *and* the merge with **no controller and no headset**, which is the only way any of this gets tested before hardware arrives.

---

## 5. Phase 3 — The libusb SBC reader

- **Own libusb context** (`libusb_init(&our_ctx)`), never the `NULL` default — `host-libusb.c` owns its own; separate contexts mean zero coupling.
- **`libusb_set_auto_detach_kernel_driver(h, 1)` immediately after open.** It uses `USBFS_DISCONNECT_CLAIM` (atomic detach-and-claim), so the kernel re-probes when the fd closes — **even on SIGKILL or a segfault**. The explicit-detach path (which QEMU's own `host-libusb.c` uses) leaves the device **driverless until replug** if we die. **Do not copy the in-tree pattern here.**
- **Async transfers, N ≥ 2–4 in flight, resubmit from the callback.** One in flight leaves "a gap in the bus schedule" (libusb's own guidance; Dolphin's known weakness). Never set `LIBUSB_TRANSFER_FREE_TRANSFER` when resubmitting.
- **No new thread.** Integrate libusb's pollfds into the **`qemu_main` loop** via `libusb_set_pollfd_notifiers` + `qemu_set_fd_handler` — exactly the in-tree pattern. Callbacks then run on **the same thread and under the same BQL** as `update_sbc_input()`, so publishing needs **no new mutex and creates no new race**. Only add a thread if measurement shows main-loop cadence bounds freshness — and if so, publish via seqlock/double-buffer, never take the BQL, and tear down with flag → `libusb_interrupt_event_handler()` → join (a quit flag alone can block 60 s).
- **Bypass `xemu_input_update_controller()` entirely** for this source — its 2500 µs limiter and SDL cache read are pure loss. Write the absolute frame straight into the merge layer.
- **Handle the phantom joystick:** on release, `xpad` rebinds and SDL sees a bogus "Generic X-Box pad." Suppress with SDL's ignore-device hint for `0x0a7b/0xd000` first; a udev unbind rule only if that proves insufficient.

---

## 6. Phase 4 — VR overlay + the rest

- **The overlay becomes another frame source**, publishing absolute values into the merge layer — **never** writing `ControllerState` directly. This is the corrected form of the "injection seam."
- **Do not extend `controller_input_device_type`.** A third enum value NULL-derefs the controller UI (`controller_map` is populated only for SDL gamepads), silently updates nothing, and auto-binds to the first unbound port via an empty-GUID `strcmp`. PR #1803 correctly branches on the **driver string** instead.
- **Force PR #1803's mouse aiming OFF in VR.** It derives aim from **absolute cursor position** relative to window centre; in VR the flat window is an unfocused mirror and the cursor is unattended, so aim pins hard-over wherever it sits.
- **`test_mode` (HUD focus) freezes the guest report** — a button held when the HUD opens stays held, and output stops. Materially worse mid-ignition or mid-eject on an SBC. Needs a neutral-report path.
- Then calibration / diagnostics / pre-flight (§6–§10 of `sbc-analog-input.md`), which now serve **100% of users**.

---

## 7. Explicitly NOT doing

- **PR #2854 (USB Passthrough UI).** +974/−106 across 13 files, open since 2026-05-08 with **zero maintainer engagement in ~2.5 months** — the only review it has ever received was an automated one, which found critical memory-safety defects (**since fixed by the author** — cite the *absence of human review*, not live bugs). It duplicates the whole binding path (shadow `bound_libusb_devices[]`, a shadow `xemu_input_bind_passthrough()`, and a second hotplug engine polling `libusb_get_device_list()` every 200 ms **on the VIRTUAL clock**, so its hotplug detection stalls whenever the VM is paused). It also matches only a hardcoded 6-entry device allow-list. And it **overlaps PR #1803 on 7 of 13 files** (not 6), both rewriting `xemu_input_bind()` and `MainMenuInputView::Draw()`. Note both PRs are by the **same author** — this is an unrebased stacked pair, not contested ownership — but whichever lands first still forces a rewrite of the other. Declining #2854 removes that conflict entirely.
- **Passthrough as a user-facing feature.** Keep the transport exactly as it is — linked in, no UI, no config key — and document the two-line Monitor recipe in a runbook labelled *unsupported dev tool: no overlay, no calibration, no snapshots, cannot run alongside the host-side reader*.
- **Passthrough as the A/B fidelity oracle.** It is contaminated: xemu issue **#1239** (*"Passed Through USB devices do not match hardware behavior"*, open since 2022, **filed against a real Steel Battalion controller**) is caused by our own OHCI *one-active-packet-per-controller* limitation; its fix (#1531) is an unmerged rewrite whose own author is unsatisfied. **Use a usbmon/Wireshark capture of the real controller instead** — no OHCI in the loop, and unlike passthrough it can run *concurrently* with our reader.

---

## 8. Q3 result — the protocol is clean; the risk is all in OUR host layer

**Verdict: nothing in the real controller's *protocol* fails to survive
parse-and-repopulate.** No init handshake, no keep-alive, no vendor control
transfers, no timing contract — enumerate, claim interface 0, read EP 0x82 and
input flows immediately. **All 39 button bits, and every axis field, match real
hardware exactly**; the emulated 26-byte report *is* the real report. This is the
single finding that de-risks the whole plan.

**The LED answer argues FOR one-pathway, not against it.** The guest's 22-byte
LED report already lands byte-perfect in `s->out_state` — but **nothing consumes
it** (`// update_output(s);` commented out at `xid-steel-battalion.c:262`; a TODO
at `:346-348`; no LED field in `SteelBattalionState`). Forwarding to a real
controller is a **verbatim 22-byte copy + one libusb interrupt write**, because
the guest hands us exactly the bytes the device wants:
- **Use interrupt EP 0x01, NOT `SET_REPORT`.** Verbatim from Cxbx-Reloaded's
  real-hardware driver (`src/common/input/LibusbDevice.cpp:295` — *not* the
  `src/devices/usb/` path cited earlier): *"a SET_REPORT control transfer to the
  SBC doesn't seem to work, the parameters might not be appropriate for it… So, we
  use the interrupt pipes for everything instead."*
- **Wire format to mirror:** byte 0 = `bReportId` = 0, byte 1 = `bLength`
  (**0x16 = 22** out, **0x1A = 26** in), then payload — sent via
  `libusb_interrupt_transfer` with `bInterval` (4) as the timeout.
- Add `uint8_t led[22]` to `SteelBattalionState` (precedent: `rumble_l/rumble_r`),
  write on change or throttled ~10 ms.
- **The same `state->sbc.led[]` drives PadXR's virtual lamps for free** — for real
  *and* substitute hardware. Under passthrough the overlay never sees them.

### Tier-1 host-layer breakages (all certain, all in the shim we are already writing)
1. **LED discarded** → a real cockpit stays dark.
2. **The toggle XOR latch destroys absolute position.** Feeding a real switch's
   level in: ON latches; OFF is a *falling* edge so no XOR fires → **the switch
   sticks ON forever.** All five toggles.
3. **Pedals lose half their travel, in a signed container.** `axis[]` is
   `int16_t`; full press writes `32767` → `0x7FFF` → the guest reads high byte
   `0x7F` ≈ **50% travel**. Real hardware reports `0..0xFFFF`. **And calibration
   arithmetic in int16 space misreads full press as negative — which §6 of
   `sbc-analog-input.md` explicitly requires.**
4. **"Between gears" (0) is rewritten to 255**; real hardware emits 0 transiently.
5. **No path for absolute gear/tuner** — both are edge-increment counters.

**Common fix, already in this plan:** the host reader writes
`state->sbc.{buttons, toggleSwitches, gearLever, tunerDial, axis}` **directly**
and never routes through the keyboard/SDL update functions (§5). Q3 independently
validates that decision.

**Gear encoding at HEAD is CORRECT** (254=R, 255=N, 1–5) — the `84ae0fe73f`
revert was right. But **the pedal half of that revert was collateral damage and
is still live** (D3 below).

### Defects found (file bugs)
| # | Where | Issue |
|---|---|---|
| **D1** | `xid-steel-battalion.c:178` | `bMaxOutputReportSize = 32`; real HW **and the struct** are **22 (0x16)** — **confirmed by four independent sources** (xboxdevwiki lsusb dump, a raw Teensy USB-host enumeration, ogx360's `BATTALION_DESC_XID`, Cxbx's `SBCOutput`). **"32" is `wMaxPacketSize`**, mistakenly propagated as the report size — and it spread because *the hardware tolerates over-length LED writes*, so wrong implementations appear to work. A 32-byte SET_REPORT **STALLs** against our own guard. |
| **D2** | `xid-steel-battalion.c:317-321` | `assert(false)` on an unexpected control transfer **aborts the emulator** instead of stalling. |
| **D3** | `xemu-input.c:773-777` | **Pedals top out at ~50% travel at HEAD.** |
| **D4** | `xid-steel-battalion.c:345` | `usb_packet_copy(p, &s->out_state, s->out_state.length)` — **`length` is guest-controlled**; use `sizeof()`. |
| **D5** | `xemu-input.c:657-665` | Toggle latch ORs momentary with latched → a keyboard press to turn a switch *off* reads as *on* while held. |

### Reusable prior art (host-side, real hardware)
- **`faha223/libSteelBattalion`** — libusb, C++, Linux-first. ⚠️ **Its `50-udev.rules` is NOT reusable — verified broken.** It is a raw `udevadm info -a` dump pasted into a file: **zero commas** (so udev reads ~28 *separate* rules, and the VID/PID match gates nothing), `NAME==`/`MODE==`/`GROUP==` use the *match* operator where assignment is required (they set nothing), and it pins transient attributes (`urbnum=="1501"`, `busnum`, `devnum`). **Write our own rule** (Phase 0 §2). ⚠️ Also **do not copy its output sizing — it writes 34 bytes**, over-long against both the 22-byte report and the 32-byte packet.
- **`Ryzee119/ogx360`** — wire-format reference. **`Cxbx-Reloaded/LibusbDevice.cpp`** — the SET_REPORT caveat.
- **Avoid `SantiagoSaldana/SBC`** — it reconstructs axes across field boundaries and collides the tuner dial into the right pedal.

### Verification — do Phase A first (no rig, no headset, no controller needed for step 2)
1. **usbmon capture** of a real controller, exercising every control → ground truth for the three fields nobody has ever dumped (input byte 7, the aiming-lever low bytes, the tuner's upper nibble).
2. **Golden-vector unit test:** replay captured reports through parser → `ControllerState` → `update_sbc_input()`, diff against the capture **byte-for-byte**. Any differing byte *is* a loss, enumerated mechanically.
⚠️ **Deployment risk found during verification: the SBC declares `MaxPower
500 mA`.** It will **not work reliably behind a bus-powered USB hub** — a real
user report of the device "stopping responding" traced to exactly this, fixed by
connecting directly. Put it in the pre-flight checklist (§10 of
`sbc-analog-input.md`) and the setup docs.

*Handshake nuance:* "no control transfers at all" is slightly too strong — Cxbx
issues one **class** `GET_DESCRIPTOR(0x42)` at open, purely informational and
explicitly failure-tolerant. And "no keep-alive" is **negative evidence across
four implementations**, not a positive proof.

3. **LED capture using our own emulator as the instrument** — dump `out_state` on every OUT transfer. This answers a question that is **undocumented globally** (the retail game's LED cadence) and is a genuine community contribution.

## 9. Open items


1. ~~Q3 outstanding~~ → **complete, folded in above (§8).**
3. **Physical panel LEDs.** PR #1803 captures the game's OUT report (`// TODO`, *"It's LED data"*). Lighting the *real* cockpit lamps is a small host-side OUT transfer using bytes we already have — **backlog item, not a blocker**, and explicitly *not* a reason to keep a second pathway.
4. **Windows** (community fork's platform): libusb **cannot detach on Windows at all** and needs a manual, admin-elevated **Zadig** WinUSB bind — which would also break any third-party SBC driver the user already installed. Low technical risk (class 0x58 has no in-box driver to steal from), real UX cost. Put the reader behind a thin platform interface and emit a *specific* error when the device is present but not WinUSB-bound.
5. **Measurement**: xemu already writes per-device pcap in **the same format and clock basis as Linux usbmon** — capture both, diff in Wireshark, no instrumented build needed.


---

## 10. Verification record (adversarial pass, 2026-07-25)

Every load-bearing claim was re-checked against primary sources — in-tree code
against `origin/steel-battalion`, external claims by two verification agents.

**In-tree — all CONFIRMED:** D1 (`:178` = 32, struct = 22) · D3 (`int16_t axis[]`,
keyboard writes `32767`, cast to `uint16` → `0x7FFF` → guest high byte `0x7F`) ·
D4 (`usb_packet_copy(..., s->out_state.length)`) · D5 (XOR rising-edge, then
`bMoreButtons |= toggleSwitches`) · F1 (three keyboard consumers) · F2 (silent
DUKE fallback — the *comment documents it*) · R8 (`test_mode` early return) ·
Q1 (2500 µs limiter, SDL **cached** reads, and **zero** event-driven axis/button
paths) · Q4 (`/* using async for interrupt packets breaks migration */`,
exempted only for host devices) · Shift+F8 collision · gear encoding correct.

**External — CONFIRMED:** xpad catch-all `{ USB_INTERFACE_INFO('X','B',0) }`
(xpad.c:497, present since v5.4; the usb-core veto does *not* apply because the
SBC's `bDeviceClass` is 0) · real descriptors incl. **`bMaxOutputReportSize` = 22**
(four sources) · no init handshake · the Cxbx SET_REPORT comment · PR #2854's
zeroed `fake_state` **and zero input-parsing anywhere in its 1462-line diff** ·
xemu #1239 (open ~4 years, filed against a real SBC) · the mborgerson #389 quotes ·
PR #2933's architecture · PCSX2 has no passthrough · RPCS3's overlay softlock.

**CORRECTED after verification:**
1. **`libSteelBattalion`'s udev rule is NOT reusable** (syntactically broken) —
   previously described as "known-good." **Write our own.**
2. **Cross-emulator precedent is a TRADE, not "unanimous."** Dolphin's own
   article is *pro*-passthrough; cite it for costs, not as proof emulation wins.
3. **PR #2854's bot-flagged defects were fixed** — cite the *absence of human
   review* (zero maintainer engagement in ~2.5 months), not live bugs.
4. **"passthrough cannot expose SET_REPORT" was unsourced** — passthrough still
   *delivers* it; xemu loses *visibility*.
5. **RPCS3 is not "migrating"** — it is *adding* emulated alternatives.
6. Cxbx file path corrected; `#2854`↔`#1803` overlap is **7** files, not 6, and
   both PRs share one author (an unrebased stack, not contested ownership).

**Nothing else was refuted.** The architecture decision stands, and its strongest
evidence (A3) proved *stronger* than originally stated.
