# Runbook — save states for repeatable, unattended testing

**Why this exists:** most of our field work has been blocked on *"a human must be
in-game at the right spot"* — the F9 RAM-hunt captures, perf A/Bs, VR passes.
Save states remove that bottleneck: **xemu can boot straight into a saved state
from the command line**, so a measurement can be re-run identically any number of
times, by an agent, with nobody playing.

## The two facts that make it work

1. **`-loadvm <name>` works.** xemu inherits stock QEMU's flag
   (`system/vl.c:2882` → `load_snapshot(loadvm, ...)`), so a state can be loaded
   at launch rather than through the UI.
2. **Snapshots live *inside* the Xbox HDD image**, not as loose files. They are
   QEMU vmstate snapshots stored in the qcow2 at `sys.files.hdd_path`
   (`ui/xemu-snapshots.c:157`) — on the rig, `~/xemu-assets/xbox_hdd.qcow2`.
   **There is no `.savestate` file to copy around**; sharing states means sharing
   (or importing into) an HDD image.

## Inspecting an image (non-destructive, do this first)

```
qemu-img snapshot -l <image.qcow2>      # lists snapshot IDs/names/sizes
```
`qemu-img` is already installed on the rig. This is read-only — run it before
touching any config. The listed **name** is what you pass to `-loadvm`.

## Launching into a state

```
qemu-system-i386 -config_path <toml> -dvd_path <iso> -loadvm '<snapshot name>'
```
(Wrap the name in quotes — snapshot names often contain spaces.)

## Compatibility caveat — states are build-sensitive

A vmstate captures the whole machine, **including the device tree**. Our fork
adds the **Steel Battalion controller device** (PR #1803 port) and the #2514 DMA
changes, so a state saved on a *materially different* build may fail to load or
load subtly wrong.

In practice this is **usually fine for states from our community**: they run
near-cutting-edge builds (SBC gameplay *requires* the unmerged PR #1803, so they
build from source), meaning their device tree is close to ours. Sanity-check on
load rather than assuming breakage — but if a state behaves strangely, suspect
build drift before suspecting the game.

### FIELD HIT 2026-07-25 — USB input devices break `-loadvm` (confirmed)
A state saved with the **Steel Battalion controller bound** fails to load:
```
Unknown section or instance 'pci.0:02.0/1.3/usb-steel-battalion' 0.
Make sure that your current VM setup matches your saved VM setup...
```
`-loadvm` then aborts the restore and boots FRESH (qemu stays alive, so it
*looks* launched — verify you're actually in the saved scene, e.g. via a
surface dump, before trusting it). Reproduced with the **identical config**
that saved the state, so it is NOT a config-text mismatch: the SBC USB device
isn't instantiated at the hub path (`1.3`) the vmstate expects at restore time
(xemu binds input devices late / by peripheral state, not deterministically at
machine init). **Consequence: CLI-boot `-loadvm` of SBC-bound states fails today —
but the WORKAROUND IS PROVEN (2026-07-26): runtime monitor loadvm.**
Launch with `-monitor unix:/tmp/xemu-mon.sock,server,nowait`, wait for
boot (~45 s), then `echo "loadvm <tag>" | nc -q 8 -U /tmp/xemu-mon.sock`
(the `-q` matters: without it nc hangs forever on the never-closing
socket). By that point the SBC USB device exists, and restore succeeds —
same path as the in-UI Machine-menu restore the owner verified. This
makes unattended mission-content measurement fully scriptable
(see ~/ladder.sh on the rig). Workarounds, in order of preference:
1. **Measure live** (no `-loadvm`) — drive into the scene, capture counters
   in-session. This is what actually worked (the radar-live numbers in
   `docs/sb-graphics-findings.md` came from a live "grab it now" capture).
2. For a deterministic A/B without re-driving, make the thing-under-test
   **runtime-toggleable** (a key / a watched file), so ONE live entry yields
   both arms — instead of relying on two identical `-loadvm` boots.
3. If loadvm is truly needed, save the state with a **Duke gamepad** bound
   (stock USB device, deterministic path) rather than the SBC — then swap
   input after load. Untested; the game may refuse non-SBC input (ISS-B11).

## What this unblocks

- **Perf A/B with a fixed scene** — e.g. ISS-B15 (film grain): boot the same
  state twice, once with the grain-removal patch applied purely as a
  *measurement tool*, and compare MangoHud timings + the NV2A counters. Same
  scene, same build, no human variance.
- **Repeatable VR passes** — land in a known spot to test the cockpit console,
  overlay, or head-look without replaying a mission.
- **Tier-3 RAM hunts** — if a state sits in a useful scene, F9 captures become
  reproducible instead of a one-shot capture session.

## Handling an imported image safely

**Do not repoint `sys.files.hdd_path` at a foreign image casually** — that is the
working HDD for every title on the rig. Prefer a **separate toml** that points at
the imported image, so the default setup is untouched and a bad image cannot cost
us the working one.
