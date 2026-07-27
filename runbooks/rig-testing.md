# Rig testing — the flat / in-headset session runbook

The at-the-machine procedure for any PenguinBox rig session. Consolidates the
operational content of [the plan](../docs/vr/00-mvp-plan.md) §4/§7/§8 and the
launcher's own enforcement; the plan stays the design record, this is what you
follow with your hands on the keyboard. Hard rules referenced by number are in
[AGENTS.md](../AGENTS.md) §1. The rig's connection details (SSH user/address)
live in the **git-ignored `.env.local`** at the repo root (template:
`.env.local.example`) — never in this public tree.

---

## 0. Pre-flight (every session, no exceptions)

1. **Live-session check (H-2)** — three emulators share the rig and the one
   headset. Run all three, as separate commands, and **never chain a launch
   behind them**:
   ```
   pgrep -x pcsx2-qt
   pgrep -x mupen64plus
   pgrep -f qemu-system-i386      # -f, NOT -x: the name exceeds pgrep's 15-char comm limit
   ```
   Any hit → someone may be in the headset: **stop and ask.**
2. **Rig-sync check (AGENTS §5, ISS-B09)** — the rig checkout must be on the
   target branch AT origin before you build, or you test stale/orphaned code.
   From the **dev box**:
   ```
   tools/vr/rig-sync-check.sh            # default: vr/mvp, ~/xemu_vr
   ```
   `IN SYNC ✓` → proceed. BEHIND / DIVERGED / ORPHANED / WRONG-BRANCH →
   reconcile before building: `git fetch origin && git reset --hard
   origin/<branch>` (branch-save any local commits first). The rig can't
   self-fetch the private repo without a credential (ISS-B09) — add a
   PAT/credential-helper or SSH deploy key first.
3. **Fresh binary (AGENTS §5)** — after sync, rebuild and confirm the binary is
   newer than your change before trusting any result:
   ```
   cd ~/xemu_vr && ./build.sh
   ls -l --time-style=full-iso build/qemu-system-i386
   ```
3. **Assets present** — the launcher pre-flights `~/xemu-assets/`:
   `mcpx_1.0.bin`, `xbox-4627_debug.bin` (the retail 3944 dump service-screens
   every title — do not use, ISS-B06), `xemu-test.toml`.
4. **For VR runs:** WiVRn server running; Quest connected via the WiVRn client,
   awake, sitting in the waiting room. (The WiVRn dashboard cannot be started
   over SSH.)

## 1. Launch

From a **desktop terminal on the rig** — not bare SSH; the XR runtime needs the
session environment.

```
~/xemu-assets/boot-test.sh "<rom>"        # flat (default ROM: OFP Elite)
XEMU_VR=1 ~/xemu-assets/boot-test.sh      # VR (headset run)
XEMU_VR=1 XEMU_VR_RUNTIME=system ...      # force the system runtime instead of WiVRn
```

Never launch the bare binary for a VR test: the launcher is what sets
`XR_RUNTIME_JSON` (else you silently bind the HMD-less system runtime and "test"
flat — ISS-B02), forces the NVIDIA GPU (PRIME), manages the toml's `[vr]`
section, and pre-checks the WiVRn compositor socket (H-3/H-6).

## 2. Expected log sequence (VR on)

```
xemu-vr launcher: VR ENABLED
bootstrapping OpenXR (Phase A)
OpenXR runtime: WiVRn 26.6.1          ← NOT "Monado"!
HMD: Meta Quest 3
runtime supports Vulkan a.b - c.d
Physical device selected by OpenXR runtime: NVIDIA ... 4050 ...
XR session created
compositor initialized
session state: UNKNOWN -> IDLE -> READY
session began
XR swapchain created (eye 0): WxH     ← then the screen appears in the headset
```

The headset stays black/void between "session began" and the guest's first
NV2A-rendered frame (VGA-blit boot phases produce no XR frames) — a few seconds
is normal.

## 3. Failure → meaning

| Symptom in log | Meaning |
|---|---|
| `runtime: Monado` instead of WiVRn | `XR_RUNTIME_JSON` not applied — you ran the bare binary; use the launcher |
| `no HMD system available` | headset not connected/awake in WiVRn (or wrong runtime selected) |
| `xrCreateInstance failed` | WiVRn server not running, or its compositor socket unreachable from this environment |
| stuck at `IDLE`, never `READY` | client connected but the app session not granted — check nothing else holds an XR session (another emulator, a leftover process) |
| `session state: ... -> STOPPING` shortly after start | headset client disconnected mid-run (WiVRn sleep/standby) |
| flat mirror fine, headset black, log healthy incl. swapchain | **report immediately** — compositor-layer bug class, not setup |
| desktop mirror minor tearing with VR on | known accepted risk (ISS-B07); report if visibly bad |
| a title misbehaves inexplicably | suspect the debug BIOS before the emulator (ISS-B06) |

## 4. During the session

- **F8** = recenter ("this is my new forward" — screen AND, when head-look is
  wired, the camera base).
- **F9** = full-RAM hunt dump, if `[vr] hunt_enable = true` — capture protocol
  in [RESEARCH.md](../RESEARCH.md) §2–§3 (hold still at scripted poses; write
  down the dump→state order — the metadata records head pose, not game state).
- Note evidence for the queue in its format: "flat ✅ menu 2026-07-17" /
  "Tier-1 ✅ in-headset 30 min, no judder".

## 5. Teardown — the H-9 order, every time

```
quit xemu  →  close the headset client  →  only then touch wivrn-server
```
**Never stop/restart `wivrn-server` while a client is connected** — 26.6.1
segfaults (H-9).

## 6. Post-test

1. Capture `~/xemu-vr-*.log` / terminal scrollback; hand the `xemu-vr:` lines
   to the agent — whatever happened.
2. Update the game's row in [TEST_QUEUE.md](../TEST_QUEUE.md) (State column,
   date + one-line evidence; move rows between priorities as they clear).
3. Anything surprising → a row in [KNOWN_ISSUES.md](../KNOWN_ISSUES.md); if
   it's operational, add it to §3's table here too.
4. If you edited the deployed launcher: port the change back to
   `tools/vr/rig-boot-test.sh` — the repo copy is canonical; deployed-only
   edits WILL drift.
