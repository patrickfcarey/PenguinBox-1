# Test Queue — games that need eyes on the rig

**What this is:** the standalone list of what needs testing on the Ubuntu rig,
in priority order — the "grab a rig evening and do these" list (same role as
the sibling forks' queues). Full project state:
[CURRENT_WORKING_LOG.md](CURRENT_WORKING_LOG.md) (in flight) ·
[KNOWN_ISSUES.md](KNOWN_ISSUES.md) (bugs) ·
[docs/vr/00-mvp-plan.md](docs/vr/00-mvp-plan.md) (plan of record); this is the
*action* view. The at-the-rig procedure is
[runbooks/rig-testing.md](runbooks/rig-testing.md).
**As of:** 2026-07-23.

**Standing setup** (details: plan §8): binary `~/xemu_vr/build/qemu-system-i386`
(branch `vr/mvp`), BIOS `xbox-4627_debug.bin` (the retail 3944 dump
service-screens on every title — do not use), renderer VULKAN on the RTX 4050.
Launch: `~/xemu-assets/boot-test.sh "<rom>"` — flat by default,
`XEMU_VR=1` for the headset. Before any run: live-session check
(`pgrep -x pcsx2-qt; pgrep -x mupen64plus; pgrep -f "[q]emu-system-i386"` —
the bracket matters over ssh, H-2).

**Legend — what a game is waiting on:**
- 🔴 **P0/milestone** — gates the Tier-1 MVP sign-off.
- 🟠 **flat-verify** — needs a flat boot check on the current build/BIOS.
- 🟡 **vr-pass** — flat-verified; needs an in-headset Tier-1 pass
  (screen appears, head-tracks, no judder, mirror unaffected).
- 🟢 **verified** — Tier-1 field-validated; listed for regression re-checks.
- ⚫ **parked** — known-bad or deprioritized; revisit deliberately.

---

## Priority 1 — ✅ COMPLETE 2026-07-16: the MVP moment

| Game | State | Waiting on | Action |
|---|---|---|---|
| 🟢 **Operation Flashpoint: Elite** | flat ✅; **Tier-1 🟢 in-headset VALIDATED 2026-07-16** (first launch worked end-to-end; F8 inversion found + fixed + user-confirmed same day, `761ccd5090`) | 🟠 **Tier-3 camera hunt** (2026-07-17: infra landed — `[vr] hunt_enable`, F9 dumps, `tools/vr/hunt_scan.py`) | Run the plan §9 hunt protocol: in-mission, stick-aim at N/E/S/W/N pressing F9 at each, then a pitch pass; hand the dumps to the agent (or run hunt_scan.py); wire the found addresses into `headlook_*_addr`, flip `headlook_enable`, verify head-look in-headset. |

**Tier-1 MVP is signed off.** The active front is now P2 (Tier-3 anchors)
and W4 polish (recenter hotkey, HUD toggle) — see plan §6/§8.

## Priority 1b — `steel-battalion` branch verification (2026-07-23 ports)

Branch `steel-battalion`; launch via `~/sb-launch.sh <label>` (SB) or
`boot-test.sh` (Halos). Ports under test: #1803 SBC device, #2514 DMA
surface-sync, ISS-B12 hotkey gate, #2874 nearest-filter 2D at >1x,
#2941 lock-free fence polling.

| Game | State | Waiting on | Action |
|---|---|---|---|
| 🟢 **Steel Battalion** | **COMBAT-VERIFIED 2026-07-23** (cockpit → ritual → gear → walking → aim/fire/lock-on/magazine, all on the stock PR encoding; run log clean — ISS-B11) | 🟠 Q+arrows sights only | Last box: sight_change (Q) + arrows in a mission; then a full-mission soak with the diagnostic silent. |
| 🟢 **Halo CE** | **flat ✅ boots → in-game 2026-07-24** (scale 4, new 595 driver, Xorg session; HUD/scope crisp in first capture — #2874 looking right) | 🟠 focused #2874/#2941 pass | In-game verified same-day as ported. Still wanted: menu-text close-up at scale 4, pistol-zoom/flashlight stall-spot feel (#2941 vs #659), shadow artifact severity note (#2174, known-broken upstream). |
| 🟢 **Halo 2** | **flat ✅ FIRST BOOT 2026-07-24** (scale 1: logo→title→FMV cinematics→attract demo reel all clean; AT-SPI key injection drives it) | 🟠 scale-4 + deep-menu pass | Needs exclusive GPU (two scale-4 instances = VK OOM on the 6GB card): menu sharpness at scale 4 (#2874), Ivory Tower map card (#2311 — expect absent on VK), splitscreen flashlight perf (#2941). Interactive or scripted-nav session. |

## Priority 2 — Tier-3 anchors (prove them early, they carry the roadmap)

| Game | State | Waiting on | Action |
|---|---|---|---|
| 🟠 **Star Wars: Jedi Outcast** | untested | flat boot on 4627_debug | Flat first; if menu reached, immediate `XEMU_VR=1` pass. Q3-engine source is public GPL — this title anchors Tier-3 head-look. |
| 🟢 **Star Wars: Jedi Academy** | **flat ✅ boots (user firsthand, 2026-07-24)** | 🟡 vr-pass + Tier-3 anchor eval | Boots on the current build. Next: XEMU_VR=1 Tier-1 pass; compare against Outcast for the head-look anchor. |

## Priority 3 — breadth on the current build (one boot each)

| Game | State | Waiting on | Action |
|---|---|---|---|
| 🟠 **Fuzion Frenzy** | service screen on 3944 (BIOS fault, not game) | flat re-test on 4627_debug | 60 fps title — best judder/pacing probe once flat-verified. |
| 🟠 **Riddick: Escape from Butcher Bay** | untested | flat boot | Also a Tier-3 candidate (PC counterpart). Graphically demanding — good VK-backend stress. |
| 🟠 **Ninja Gaiden Black** | untested | flat boot | Xbox flagship; good NV2A coverage. |
| 🟠 **MechAssault (Rev 1)** | untested | flat boot | — |

## Priority 4 — parked

| Game | State | Why | Revisit when |
|---|---|---|---|
| ⚫ **MechAssault 2: Lone Wolf** | user-reported no-boot in xemu (prior setup) | don't burn rig time on a known-bad | after P1-P3 are green — one boot on our build/4627 to confirm or clear the report |

## Priority 5 — staged on the SD card (22 more titles, not yet on the rig)

Full library at `/media/pacarey/SD512/retrodeck/roms/xbox/`. Copy to
`~/roms/xbox/` as needed. Notables for later tiers: **KOTOR 1/2** (PC
counterparts, Odyssey engine), **Ghost Recon ×3 / Rainbow Six 3** (PC
counterparts, tactical FPS), **Unreal Championship / UC2 / Unreal II**
(UE2, PC counterparts), **Far Cry Instincts ×2**, **Baldur's Gate DA**,
**ESPN NFL 2K5**, **Jade Empire**, **Mafia**, **Star Wars Battlefront /
Clone Wars / KOTOR-era**, **THPS 2x**, **Gauntlet DL**, **Dead to Rights**,
**Yu-Gi-Oh**.

---

**Recording results:** update the State column in place (date + one-line
evidence, e.g. "flat ✅ menu 2026-07-16" / "Tier-1 ✅ in-headset 30 min, no
judder"), and move rows between priorities as they clear. Anything
surprising (BIOS-suspect behavior, VK rendering artifacts, pacing weirdness)
gets a row in [KNOWN_ISSUES.md](KNOWN_ISSUES.md) (and the
[rig runbook](runbooks/rig-testing.md)'s failure table when it's operational) —
the 4627 BIOS is a debug kernel, so per-title weirdness should suspect the
BIOS first (ISS-B06).
