# tools/vr — every tool + when to use it

Same convention as the sibling forks: the offline/rig tooling lives here, each
tool documented with what it does and when to reach for it. The family's
**golden principle** binds all of them: every tool **measures the artifact**
(real dump bytes, real sockets, real files) rather than trusting a theory about
how the engine works.

| Tool | Does | Reach for it when |
|---|---|---|
| `hunt_scan.py` | The Tier-3 hunt scanner, two modes. **Angle** (`yaw`/`pitch`): finds 4-aligned float32s in F9 RAM dumps whose pairwise deltas track the scripted headings (radians/degrees/negated, wrap-aware; repeated poses filter self-drifting values) → physical addresses ready for `[vr] headlook_yaw_addr`/`headlook_pitch_addr`. **Flag** (`flag on=... off=...`): finds bytes whose ON-state and OFF-state value sets are disjoint (enum-aware) → suggested `headlook_gate_mask`/`headlook_gate_value` for conditional head-look (the ADS gate). | After a capture session per [RESEARCH.md](../../RESEARCH.md) §2–§3. |
| `rig-boot-test.sh` | The canonical rig launcher: `[vr]` toml-section management (`XEMU_VR=1`; `XEMU_VR_RUNTIME=system` opts out of WiVRn auto-select), WiVRn manifest autodetect + compositor-socket pre-check, PRIME GPU forcing, asset pre-flight (MCPX, debug BIOS, toml). Default ROM: OFP Elite. Enforces H-3/H-6. | Every rig run — never launch the bare binary for a VR test (the wrong-runtime false pass, ISS-B02). |
| `demo-pbox.sh` | The **demo** launcher: everything `rig-boot-test.sh` does (PRIME forcing, WiVRn autodetect, `[vr]` management) **+ VR on by default + `MONADO_DEBUG=1`** (tee'd to `~/xemu-demo.log`) **+ a MangoHud GPU overlay** on the desktop mirror — live GPU utilisation/clocks/temp/power/VRAM/FPS + frametime graph + the GPU's own auto-detected name (`gpu_name`, pulled from the machine, nothing hard-coded). Loads MangoHud's GL path via the **shim** LD_PRELOAD (xemu presents through OpenGL, not a VK swapchain, so the Vulkan layer has nothing to hook; the overlay stays out of the headset). `XEMU_VR=0` runs flat+HUD (the HUD smoke-test mode). | Showing the project to an audience — the HUD makes "an Xbox game in VR on a laptop 4050" legible at a glance. |
| `rig-sync-check.sh` | Read-only **drift detector**: run from the dev box, it refreshes the dev-box `origin/<branch>` ref and reads the rig's HEAD over SSH (never fetches/resets on the rig) → reports IN-SYNC / BEHIND / AHEAD / DIVERGED / WRONG-BRANCH + the reconcile command. Rig target from `.env.local` (`$RIG_SSH`, never hardcoded). | **Every rig session, before building** (AGENTS §5 rig-sync discipline) — so a stale/orphaned rig tree can't silently back a build/test (ISS-B09). |
| `sb-launch.sh` | The **Steel Battalion flat-verify launcher** (canonical copy — deployed as `~/sb-launch.sh` on the rig; keep in sync): PRIME env forced (the VULKAN display path needs VK **and** GL on the NVIDIA device even for flat runs — a GL-on-iGPU launch segfaults at VK init, field-hit 2026-07-23), `SDL_VIDEODRIVER=x11` so the window is XWayland-backed (xwd screenshots / key injection), `vr.enable=false` via `sb-verify.toml`. Takes a run label arg → `~/sb-verify-<label>.log`. | Re-running the ISS-B11 verify or playing SB flat on the rig (`bash ~/sb-launch.sh run3`). |
| `sb-verify.toml` | The config behind `sb-launch.sh` (deployed as `~/xemu-assets/sb-verify.toml`): SBC driver on port 1 fed by the keyboard (`port1_driver = 'usb-steel-battalion'`, `port1 = 'keyboard'`), VULKAN renderer on the NVIDIA device, debug BIOS (H-5), SB ISO as `dvd_path`. Paths are rig-local. | Same runs; edit here first, then redeploy (canonical = repo). |
| `flat-launch.sh` | **Generic** flat-verify launcher (deployed as `~/flat-launch.sh`): any toml + any ROM (`flat-launch.sh <toml> <label> [rom]`, ROM via `-dvd_path`), same PRIME + SDL-x11 discipline as `sb-launch.sh`. | Any non-SB flat check — the Halo #2874/#2941 verification, TEST_QUEUE P2/P3 boots. |
| `flat-check.toml` | The generic config behind it (deployed as `~/xemu-assets/flat-check.toml`): Duke-on-keyboard, VULKAN/NVIDIA, `surface_scale = 4` (exercises the #2874 nearest-filter 2D path), debug BIOS; ROM supplied per-launch. | Same runs. |

## Tool discipline

- **The scanner is load-bearing for every Tier-3 hunt**, and rig capture time is
  the scarce resource — a silent scanner bug wastes a whole session. **Gap
  CLOSED (2026-07-24):** `test_hunt_scan.py` (91 checks, ~3 s, run
  `python3 tools/vr/test_hunt_scan.py` after any scanner change) builds
  synthetic dumps — planted rad/deg/negated trackers, drift decoys, wrap-seam
  scripts, gate bytes, resolve/sibling smoke. Its first run caught two real
  bugs (half-period slop rejection at the yaw protocol's own 180 step; flag
  suggestion keyed to CLI order instead of the `on=` group) — both fixed the
  same day, proving the discipline's point. Keep it green.
- **The launcher's canonical copy is the repo file.** The deployed
  `~/xemu-assets/boot-test.sh` must be kept in sync — deployed-only edits WILL
  drift (the sibling's rig-diff lesson).
- Ported from the siblings as need arises: a `vr-info`-style headless XR probe,
  stereo-sweep/FOV tooling (once Tier-2 lands), the profile-DB catalog checks.
