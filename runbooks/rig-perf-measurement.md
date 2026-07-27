# Runbook — measuring SB performance on the rig (and the traps)

Hard-won operational knowledge from the 2026-07 perf campaign. Two of my
confident wrong calls this session came from measurement traps, not bad code —
they're all avoidable. Read before trusting any FPS/frame-time number.

## Seeing FPS on screen (the instrument)

**Use xemu's built-in Video Debug overlay. NOT MangoHud.**
- xemu: move the mouse to wake the auto-hiding menu bar → **Debug** (3rd menu) →
  **Video**. Shows `FPS:N` (= `g_nv2a_stats.increment_fps` = the true NV2A flip
  rate = true guest fps) + `MSPF` (NV2A render ms). No hotkey. `~` is the text
  Monitor console, not FPS. The window persists across loadvm.
- **MangoHud is unreliable here:** on the rig's PRIME/GLVND setup it hooks a
  spurious offscreen GL context and reports garbage (e.g. 1272 fps on the wrong
  GPU). xemu presents its window via GL on the iGPU while NV2A renders in Vulkan
  offscreen, so MangoHud mis-attributes everything. (The local 0.8.4 shim in
  demo-p64/demo-pbox works only because those cores present Vulkan-native.)
- Phase-timer `mspf` in the launch stderr log (`grep 'phase:' … | grep -oE
  'mspf=[0-9.]+'`) is the scriptable equivalent: 33.3 ms = 30 fps, 16.7 = 60.

## The four measurement traps (each burned us)

1. **The dead-screen-60 trap.** Steel Battalion's menus AND its KILLED-IN-ACTION
   / results screens render at **60 fps natively** (pfifo_work → ~1 ms). A "60 fps"
   reading on a static/menu/death screen is NOT a gameplay unlock. **Only trust a
   number taken during live, moving gameplay.**
2. **The near-death-save contamination.** The snapshot `vm-20260725222138` is
   "one hit from death" and **self-fails to the end-screen in ~19-20 s**
   unattended. A 35 s measurement leg therefore captures the dead screen at the
   end, not the live scene — log-scraping "last N samples" reported a false 30 fps
   when live combat was actually ~23. Also, that save is an atypical state
   (`state[0x3b90e0]==0`, present path 0x46556) — NOT normal gameplay
   (`state==0xA`, flip+governor path). **Measure representative scenes; a fresh
   cold-boot into normal gameplay beats a convenient near-death snapshot.**
3. **Log-scrape vs live eyes.** The FPS overlay watched during the actual heavy
   frame is truth; a tail of the phase-timer log is not (see #2). When they
   disagree, the live overlay wins. Grab a screenshot as evidence
   (`~/lane4-grab.sh <out>` — grabs the composited root and crops to the xemu
   window; GL windows are blank in their own pixmap so a direct window-grab fails).
4. **gdb perturbs the wall-clock mspf.** Attaching gdb / stopping the target
   desyncs the phase-timer's wall-clock deltas → garbage mspf (16.7, 4, etc.) for
   a few seconds after detach. Read mspf only after the emu has free-run a few
   seconds with no debugger attached. Also: attaching gdb briefly PAUSES the
   guest — never do it mid-firefight on a live session (the mech can get hit).

## Clean launch (full overlap stack, cold boot)

`~/coldboot-play.sh` — kills stale emu, boots the DVD with the full stack
(`XEMU_FRAME_OVERLAP/ASYNC_PRESENT/ASYNC_REPORTS/INBATCH_UPLOADS/BATCH_QRESET/
PIPE_OPT/ELEMENT_BATCH` + `TLB_FASTPATH`, `PHASE_TIMERS`), monitor at
`/tmp/xemu-mon.sock`, gdbstub `tcp:127.0.0.1:5555`, no loadvm. Confirm
`GL_RENDERER: NVIDIA … RTX 4050` (discrete via PRIME, not the iGPU) and the
toggle "ON" lines in `~/coldboot.log`. **H-2 first, as its own command** (pgrep
`-x pcsx2-qt`, `-x mupen64plus`, `-f "[q]emu-system-i386"` with the `[q]` bracket
over ssh) — never chain a launch behind the check.

**Firmware (for reproducibility).** All measurements used: boot ROM
`mcpx_1.0.bin` (MCPX v1.0, sha1 `5d270675…211cef`) + flash BIOS
`xbox-4627_debug.bin` (1 MB debug kernel 4627, sha1 `a5503364…64bb7d`),
via `bootrom_path`/`flashrom_path`. Microsoft firmware is user-supplied, not
in-repo. Full hashes in `docs/discord-perf-writeup.md` § Firmware.

## Reusable frame-limiter RE method (for other titles)

The toolchain that cracked SB's 30-lock, reusable per-title:
1. **Guest-PC profiler** (`XEMU_GUEST_PROFILE=N`) → where the guest spends time;
   a tight kernel-idle spin dominating = the game is blocked on vblank (a
   present/interval wait), not compute-bound.
2. **XBE static RE**: capstone (CS_MODE_32) + section→file map (base 0x10000;
   .text raw=va−0x10000; D3D raw=va−0x20c1e0+0x1fd000); deobfuscate the kernel
   thunk table (retail XOR keys entry 0xA8FC57AB / thunk 0x5B6D40B6) to find
   KeWaitForSingleObject / KeSetEvent callers = the wait & signal sites.
3. **Live gdb single-hit backtrace** at the vblank-wait primitive: break ONCE,
   read `$esp` for the caller chain, detach. Do NOT loop `continue` on a 30-60 Hz
   breakpoint (deadlocks the stub).
4. **Live RAM diff** (`pmemsave <addr> <size> FILE` — RELATIVE filename, writes to
   the emu CWD; absolute /tmp paths fail) across scene transitions to find state
   words / counters.
5. **The timestep question is decisive**: before "unlocking" any fixed-30 game,
   confirm whether logic is per-present (→ unlock = 2× speed, needs frame-gen) or
   per-elapsed-time (→ free). SB was per-present (fixed 1/30 s, frame-count
   timers, no QPC). See `docs/sb-frame-limiter.md`.

## Analysis scripts (this session, scratchpad/ + rig ~)

capstone disasm helpers, thunk-table parsers, the gdb single-hit/backtrace
scripts, `lane4-grab.sh` (screenshot), `lane4-xwd2png.py`. Kept as references;
not part of the build.
