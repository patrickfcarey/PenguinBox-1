# Steel Battalion's 30 FPS frame limiter — CRACKED (and why 60 is not a byte-patch)

**2026-07-26.** After the present-wall overlap stack took pfifo_work 28→18 ms (GPU
emulation no longer the frame bound), SB still pinned to ~2 vblanks/frame. A
four-lane RE assault (static XBE analysis ×3 + live gdb/instrument ×1) cracked
the mechanism completely. **Headline: the limiter is fully understood, but SB is
a fixed-30 Hz-timestep engine — unlocking the present to 60 makes the whole game
run at 2× speed. There is no shippable byte-patch to true 60; it requires
sim/render decoupling (frame generation).** This supersedes the earlier
"D3DPRESENT_INTERVAL_TWO hypothesis" version of this doc — that guess was on the
right axis (a vblank-count present wait) but the real mechanism and the fatal
timestep coupling are below.

All addresses are guest vaddr (load base 0x10000). XBE section→file mapping:
.text raw = va−0x10000; D3D raw = va−0x20c1e0+0x1fd000. Retail XBE. Tags:
MEASURED (live), READ-IN-CODE (static, verified), HYPOTHESIS.

---

## THE VERDICT (Lane 3, HIGH confidence ~95%): fixed-timestep, 60 = 2× speed

SB is a **fixed 30 Hz-timestep engine**: exactly **one simulation step per
present**, advancing by a hardcoded **1/30 s** each frame, with its entire
timebase built from **+1-per-present frame counters**. There is **no wall-clock,
no KeQueryPerformanceCounter (absent from the entire XBE), no elapsed-vblank
delta** feeding the sim anywhere in the gameplay path. Halving the present
interval doubles how often the main loop iterates → the sim runs at 2× real
time. READ-IN-CODE:

- `main()` 0x467a0 → top-level state loop **0x46840** (`call 0xe3510` state
  selector; `jmp [eax*4+0x46888]` → menu 0x46854 / 0x71730 / 0xb5d20 / 0x746b0).
- Per-frame advance/present **0x4ac70**: `inc word [0x352898]` @0x4ac75 — a
  frame counter +1 EVERY present; then `call 0x4aa80` (update) then D3D swap.
  More +1/present counters: [0x352880], [0x351c90] (inc @0x4aa8c/0x4aa96). These
  counters are only ever advanced by +1 (never by an elapsed-vblank count → no
  catch-up), so they strictly equal presents rendered.
- Timebase IS frame-count: periodic logic `if (frameCtr & 3)==0` @0x2e8e1;
  frame-unit countdown timers (WORD decrement loop @0x4aad0; a 0x3c=60-frame
  "2 s @30fps" duration @0x2e921).
- Hardcoded **1/30 f = 0x3D088889 at VA 0x260a38 / file 0x25c958**, literal
  `fmul` at 14 sites (fixed-step integrator 0x8af60; frames→seconds 0x8afce
  `fild; fmul 1/30`; entity update 0x179ac6). Adjacent **1/60 f = 0x3C888889 at
  VA 0x260a3c / file 0x25c95c** (6 sites, incl. a `0.5·a·t²` term using (1/60)²
  @0x40aef — hardcoded NTSC field math, still no runtime dt).
- KeQueryPerformanceCounter/Frequency (ord 126/127): **absent** from the XBE.
  KeTickCount (ord 156, thunk 0x25d138, getter 0x18dbe0): used only on
  fade/transition/loading screens (e.g. 0xdff40: tick-delta vs fixed ms
  timeouts), **none of its callers are in the gameplay frame path**.

The live-profiler hint I earlier read as "maybe 60 is free" (clean 2→3-vblank
quantization) does NOT indicate an adaptive timestep — a fixed-step loop behind a
vblank-locked flip produces exactly that quantization. **My "leaning free" call
was wrong; the code refutes it.**

**Why a single-constant patch is NOT enough:** editing the 1/30 float
(0x25c958→0x3C888889) fixes only the float-integrator subsystems; the *majority*
of timing is integer frame-count based (the +1/present counters, ~149 readers,
frame-unit cooldowns, `frameCtr % N` events, animation indices) and would still
run 2× fast. Robust fix = **decouple sim rate from present rate** (advance the
guest main loop every other 60 Hz vblank, or render-side interpolation/frame-gen
for the extra presents). Not an ISO edit.

---

## THE MECHANISM (Lane 1, verified): two 1-vblank sleeps per gameplay frame

The 30-lock is **not** an "interval=2" constant — it is **two separate 1-vblank
sleeps** per gameplay frame (1+1 = 2 vblanks = 33.3 ms = 30 fps), both via the
vblank-wait primitive. READ-IN-CODE:

- Vblank-wait primitive **0x20ec60** (the ONLY KeWaitForSingleObject caller in
  the image): clears dword [device+0x2558], then
  `KeWaitForSingleObject(obj=device+0x2554, reason=6, mode=1, alertable=0,
  timeout=NULL)`. Device global pointer at **[0x21afe0]**.
- Gameplay dispatcher 0x106550 reads state word **[0x3b90e0]**; state **0xA/0x17
  → 0x102ba0** (in-mission frame loop; body loops 0x102bc0, governor interval
  `edi=1` set once @0x102bb6). Per iteration:
  - `0x102bc0 call 0x4aa80` — per-frame tick; its own vblank wait @0x4ac01 is
    **gated OFF in gameplay** (`je` @0x4abff when [0x351f40]==0) → no 3rd sleep.
  - `0x102d7f call 0x20e520` = **D3DDevice::Swap** → 0x20e4e0 → **0x20dbc0
    (flip): SLEEP 1 vblank @0x20dc08**; then AvSetDisplayMode [0x25d28c]; then
    conditionally `inc [device+0x2580]` (the flip/swap counter) @0x20dc75.
  - `0x102d8a call 0x2db50` = **frame governor: SLEEP 1 vblank @0x2db51**, then
    spin-polls until [device+0x2580] ≥ saved+interval (interval=1 → the flip's
    single increment satisfies it immediately → governor contributes exactly its
    one unconditional sleep).
  - `0x102da4 jmp 0x102bc0`.
- Alternate present path (also state 0xA): 0x103140 → 640×480 composite
  0xdfbc0 → governor epilogue `call 0x20e520; push 1; call 0x2db50` @0xdfe75.

**My earlier false-negative, explained:** I NOP'd 0x20dc29 (the `jne 0x20dc08`
retry) live and saw no change — because that branch is the AvSetDisplayMode-
failure retry, which essentially never fires. The always-executed sleep is the
`call 0x20ec60 @ 0x20dc08`, which I never touched. So 0x20dbc0 IS on the
per-frame path; my *edit* was inert, not the theory.

### Patch table (present-side; NONE is a shippable 60 unlock — see verdict)
| # | file_off | vaddr | old_bytes | new_bytes | effect | conf |
|---|---|---|---|---|---|---|
| P2 | 0x0f2d89 | 0x102d89 | `57 E8 C1 AD F2 FF 83 C4 04` | 9×`90` | delete governor sleep in gameplay loop only (flip stays vsync → 60 render, no tearing, menus untouched) | HIGH |
| P1 | 0x1fea28 | 0x20dc08 | `E8 53 10 00 00` | 5×`90` | delete flip sleep globally (governor remains as pacer; risk: tearing) | HIGH hits 60 / MED tearing |
| P3 | 0x01db51 | 0x2db51 | `E8 0A 11 1E 00` | 5×`90` | delete governor sleep globally (menu/loading callers w/ interval≥2 may busy-spin) | MED |

Never apply two at once (removing both per-frame sleeps uncaps into a spin). ALL
of these produce **60-render-at-2×-speed** per the Lane 3 verdict.

---

## THE SIGNALING SIDE (Lane 2, verified): not the limiter

The vblank event the game waits on (device+0x2554) is signaled
**unconditionally on every vblank-handler run** — no `%2`, no countdown, no
present-interval gate. So no signaling-side patch can raise 30→60; the limiter
is on the wait/present side. READ-IN-CODE:

- Vblank handler **0x216be0** (`this` = SWAPCTX = **DEV+0x23c0**, proven
  @0x21659b), reached via PCRTC vblank IRQ (`NV_PMC_INTR_0` bit 24). Vblank
  enabled once at 0x218ce0 (`NV_PCRTC_INTR_EN_0` write — the only one) → handler
  runs every field.
- Increments the **vblank/swap counter [SWAPCTX+0x1c0] = [DEV+0x2580]** @0x216c0a.
- **KeSetEvent @0x216ca8** signals `SWAPCTX+0x194 = DEV+0x2554` (exactly the game's
  frame event) — every code path from entry reaches it (the only post-signal
  branch skips an optional callback). The other KeSetEvent @0x217029 targets
  DEV+0x2564 (a different event).
- The 30 lives in the **swap-completion target gate**: fn 0x216b10
  (`cmp [SWAPCTX+0x1c0], target; jne`), where target spacing = present interval
  = bits 5-7 of the opcode-1 swap token, applied @0x216f78 (dispatcher 0x216ea0).
  interval=2 ⇒ targets 2 vblanks apart ⇒ 30 fps (HYPOTHESIS on the token
  construction; MEDIUM — an indirect jump table). This is consistent with, and
  the same wall as, Lane 1's governor spin on [DEV+0x2580].
- No KeConnectInterrupt import → the NV2A-IRQ→handler wiring is kernel-owned; the
  handler's actual firing rate (60 Hz field vs 30 Hz frame) is not determinable
  from the XBE. The "no signaling gate" conclusion holds either way.

Useful addresses handed off: frame event `[0x21afe0]+0x2554` (SignalState
+0x2558); vblank/swap counter `[0x21afe0]+0x2580`; SWAPCTX = `[0x21afe0]+0x23c0`.

---

## LIVE REALITY (Lane 4, MEASURED): combat ~23 fps, and the save is atypical

On the live combat snapshot `vm-20260725222138` (clean full overlap stack, NO
patches), watched with the on-screen counter:

- **Combat runs ~24-26 fps** (mspf 38-43 ms, peak 52-57; pfifo_work ~22 ms;
  on-screen FPS 23-26 during the heavy attack/explosion). It does **NOT hold 30**
  in the worst instant. My earlier "solid 30, pfifo 18-19 ms" claim was a
  log-scrape artifact: this save **self-fails to KILLED IN ACTION at ~19 s**, so
  end-of-run samples caught the calm lead-in or the dead screen, not the blast
  frame. The careful live read (~23) is the truth — closer to the owner's
  remembered ~22 than my 30. (Screenshot: scratchpad/lane4-final.png.)
- **The dead-screen-60 trap:** the KILLED-IN-ACTION screen renders 60 fps /
  MSPF 1 (pfifo_work→1) — NOT an unlock. This is what fooled my first "60 fps"
  poke test earlier; only trust LIVE gameplay readings.
- **This save is an atypical near-death state.** Single-hit backtrace at 0x20ec60
  (5 samples, deterministic): the per-frame main-thread vblank wait is reached
  via **0x46556** (chain 0x46556→0x46859→0x192c34→0x19156a), with
  `state[0x3b90e0]==0` — NOT the state-0xA / 0x102bc0 flip+governor gameplay
  path Lane 1 mapped. So (a) we partly optimized/measured against a one-hit-
  from-death outlier, and (b) **P2 would likely NOT affect this specific scene**
  (its dominant sleep is on the 0x46556 path). A NORMAL combat save (state 0xA)
  is needed to measure representative combat AND to validate the P2 crack on the
  real gameplay path. RECONCILE (open): is 0x46556 the near-death/scripted path
  while 0x102bc0 is normal gameplay, or is [0x3b90e0] not the state var Lane 1
  assumed? Resolve with a normal combat save + re-trace.

### On-screen FPS instrument (solved)
Use **xemu's built-in Video Debug overlay**, NOT MangoHud. MangoHud on this box
(PRIME/GLVND) hooks a spurious offscreen GL context → garbage (1272 fps, wrong
GPU) because xemu presents its window via GL on the iGPU while NV2A renders in
Vulkan offscreen. xemu overlay: **move mouse to reveal the auto-hiding menu bar
→ Debug (3rd menu) → Video** → `FPS:N` (= g_nv2a_stats.increment_fps = NV2A flip
rate = true guest fps) + MSPF. No hotkey; `~` is the text Monitor, not FPS.
Persists across loadvm. Proven: 60 on menus/dead-screen, 23-26 in combat.

---

## What stands / what's blocked

- **Banked & real:** the overlap stack (pfifo 28→22 ms in combat) keeps heavy
  frames from collapsing further, and helps VR + every non-fixed-30 title. It
  cannot push SB past 30 (or hold 30 in the worst explosion frame), and cannot
  reach 60 without breaking speed.
- **Closed question:** "why 30, can a patch get 60" — answered cold. Present
  mechanism = two vblank sleeps (Lane 1) / swap-target interval (Lane 2);
  60-via-patch = 2× speed (Lane 3, fixed timestep).
- **Blocked on a normal combat save** (owner to stage): representative combat fps
  + validate the state-0xA present path / P2 on real gameplay (this save is a
  near-death outlier on the 0x46556 path).
- **The only true-60 path** = sim/render decouple / frame generation (run sim at
  30, synthesize extra presents) — a real emulator feature, not an ISO patch.

## Honest corrections logged (measurement discipline)
1. "60 fps game all along / leaning free" — REFUTED by Lane 3 (fixed timestep).
2. "combat holds a solid 30" — REFUTED by Lane 4 live (~23; my log-scrape caught
   the save's ~19 s death, not the blast frame).
3. The 0x20dc29 NOP "did nothing so 0x20dbc0 isn't it" — the edit was inert (retry
   branch); 0x20dbc0 IS the flip path (Lane 1).
4. The first "16.7 ms = 60 fps unlocked" poke — was the dead end-battle screen
   (renders 60 natively), not a live unlock. Always measure LIVE gameplay.
