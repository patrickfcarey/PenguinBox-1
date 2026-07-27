# tools/perf — performance research & measurement kit

Born of the Steel Battalion campaign (docs/sb-graphics-findings.md,
docs/sb-rendering-anatomy.md); everything here generalizes to other titles.

## Guest-code analysis (host-side, run anywhere)
- `xbe_tool.py` — XISO→default.xbe extraction + XBE header/section parser
  (`all` prints title-id, entry, sections). Feeds flat i386 disassembly:
  `objdump -D -b binary -m i386 -M intel --adjust-vma=<vaddr>`.
- `scan_hunt_loops.py` — scores disassembly for scan-loop shapes
  (byte-load + stride patterns) — how the radar scanners were found.
- `refhunt.py` — cross-reference a guest global across the image
  (e.g. `python3 refhunt.py 0x351bdc`).
- `../..../docs/perf/scan-pc-capture.patch` — cputlb diagnostic: log
  deduped EIP+regs of instructions reading watched surfaces
  (XEMU_SCAN_PC_CAPTURE=64).
- `guest_profile_symbolize.py` — offline half of the **guest-PC profiler**
  (Phase 0). Reads the raw `{u32 eip, u32 idx}` ring dumped by the in-emulator
  sampler and prints the ranked 31 ms breakdown: coarse ROLE split (D3D8 driver
  vs radar vs game/other .text vs kernel), a per-function histogram, the top
  individual EIPs, and a spin-cluster report (+ spin fraction). Reuses
  `xbe_tool.read_xbe()` + capstone to harvest function starts from CALL targets.

### Guest-PC profiler (Phase 0) — the ranked 31 ms breakdown

Statistical sampler of the guest program counter, built into the emulator
(`accel/tcg/xemu-guest-profile.c`), OFF by default and inert unless armed. It
samples `env->eip`/`TCGTBCPUState.pc` at TB boundaries **on the vCPU thread**
(no cross-thread env race — the reason a QEMUTimer was rejected), forcing TB
chaining off while armed so hot loops and spins are sampled faithfully. It
answers *where guest code runs* (game logic / D3D8 driver / radar / spin); it
does **not** see host-side overhead (TB re-translation, MMIO emulation) — those
are not a guest EIP.

Rig recipe (coordinator; needs the live-session check H-2 first):

```sh
# 1 in N TBs sampled; ring flushed every K frame-flips (atomically renamed).
XEMU_GUEST_PROFILE=4096 \
XEMU_GUEST_PROFILE_DUMP_FLIPS=300 \
XEMU_GUEST_PROFILE_PATH=$HOME/guest-profile.bin \
  ./build/qemu-system-i386 ...            # look for "[guest-profile] ARMED" on stderr

# run the mission-save scene for >= K flips, then symbolize the dumped ring:
python3 tools/perf/guest_profile_symbolize.py $HOME/guest-profile.bin \
  --xbe /path/to/default.xbe --top 40
```

`XEMU_GUEST_PROFILE` is the sample interval N (any positive int; unset = OFF).
The file is rewritten (not appended) every flush, so read it after the run.

## Rig measurement scripts (`rig/` — canonical copies; deploy to ~ on the rig)
- `ring-launch.sh` — detached xemu launch (survives ssh drops): PRIME env,
  session display autodetect, RenderDoc LD_LIBRARY_PATH, and
  `-monitor unix:/tmp/xemu-mon.sock` for runtime control.
- `ladder.sh` + `start-ladder.sh` — the toggle-ladder: N legs × (fresh
  boot → monitor `loadvm` of the mission snapshot → counter sample),
  self-detaching starter. Per-feature env toggles per leg.
- `ab-interleaved.sh` — live ABABA A/B over a watch-file toggle with
  per-segment counter extraction (defeats scene-drift confounds).

## The counters (in-emulator, always available)
H6 60-frame stderr summary self-logs every non-zero NV2A profiler
counter (SURF_CPU_READ, TEX_HASH_KB, FINISH_*, QUERY_ASYNC_RESOLVE...).
Env diagnostics: XEMU_SURF_DUMP / XEMU_SURF_ATTR / XEMU_SURF_ACCESS_LOG.
Dump workflows: runbooks/graphical-dumps.md.
