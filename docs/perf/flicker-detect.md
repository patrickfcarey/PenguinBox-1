# flicker-detect — objective inter-frame luminance-instability detector

Steel Battalion mission scenes show light/glow **flicker** under some xemu
configs (the async-queries path). Judging a fix by eye is unreliable — "hard
to tell" is the enemy. This tool turns flicker into a **number** so two
configs can be compared and a fix verified without human eyes.

Tools (all under `tools/perf/`):

| file | where | role |
|------|-------|------|
| `flicker_detect.py`     | rig + local | XWD parse, metrics, heatmaps, A/B, selftest |
| `rig_flicker_capture.sh`| rig         | read-only capture of one burst from the xemu window |
| `rig_flicker_ab.sh`     | rig         | detached A/B runner (launches its OWN instances) |
| `flicker_run.sh`        | local       | orchestrator: deploy, drive, fetch, analyze |

Dependencies: `numpy` + `Pillow` only (present on the rig and the dev box).

## What it measures

A pixel that should hold steady but instead oscillates up/down frame-to-frame
is flicker. That must be told apart from genuine **animation** (radar sweep,
counting HUD clock, panning camera), which either drifts smoothly or moves an
edge through a pixel *once*. The metrics are built for that:

- **`flicker_score`** — *primary.* Per-pixel high-pass temporal amplitude
  (luminance minus its short moving average, then temporal std) **weighted by
  oscillation frequency** (count of significant luminance reversals ÷ frames).
  Flicker = steady amplitude at high reversal frequency → scores high. Genuine
  animation = high amplitude but *low* reversal frequency (an edge crosses
  once) → suppressed. Capture noise = low amplitude → suppressed.
- **`osc_index`** — mean significant reversals/pixel ×1000.
- **`flicker_frac` / `flicker_px`** — fraction/count of pixels flagged as
  flickering (enough amplitude AND ≥`min_osc` reversals).
- **`flicker_energy`** — mean high-pass amplitude over the flagged pixels.
- **`hp_energy`** — total high-pass instability (includes animation; reported
  as context, not the discriminator).
- **`mad` / `hot_mad`** — mean |ΔL| over the whole frame / the auto-detected
  hot (HUD/light) region. `hot_mad` answers spec item (a) directly.
- Heatmaps (spec item c): `heat_oscillation.png` (flicker localization),
  `heat_flicker.png` (amplitude), `heat_variance.png` (any change),
  `flicker_overlay.png` (flagged pixels tinted red over a reference frame).

### Relative, not absolute

Animated content cannot be perfectly masked. The tool is therefore built for
**A/B**: same save state, same window, same burst length ⇒ animated content is
statistically identical between two configs, so any **delta** in the metrics is
the flicker a config added. `ab` reports each run, the delta, the ratio, and a
split-half (even vs odd frame) noise band, so you can judge whether the
separation is real.

## Usage

### One-shot A/B (baseline vs async-queries), from the dev box

```bash
# requires the rig to be FREE (no xemu / pcsx2 / mupen running).
tools/perf/flicker_run.sh ab 60      # 60 frames/leg; runs detached on the rig
```

This deploys the trio to `~/flicker-tools/`, launches `rig_flicker_ab.sh`
detached (survives ssh drops), polls for the `DONE` marker, fetches both legs
to `./flicker-results/ab-<stamp>/`, and prints the comparison. The runner
**refuses to start** if any xemu/pcsx2/mupen instance is already up (it only
manages instances it launches).

### Read-only capture from a running instance (no launch/kill)

```bash
tools/perf/flicker_run.sh live baseline 40   # crop+capture the CURRENT window
```

### Analyze / compare already-captured PNG runs

```bash
tools/perf/flicker_detect.py analyze <run_dir> --label mylabel
tools/perf/flicker_detect.py ab <run_A> <run_B> --label-a base --label-b fix
```

### Rig-side capture directly (advanced)

```bash
# on the rig; env auto-derived from gnome-shell, geometry auto-detected:
FDPY=~/flicker-tools/flicker_detect.py \
  bash ~/flicker-tools/rig_flicker_capture.sh ~/out 60 0 mylabel
# override geometry with GEOM="x y w h" if auto-detect picks the wrong window.
```

### Validate the detector math

```bash
tools/perf/flicker_detect.py selftest --write /tmp/st   # synthetic pass/fail
```

Tunables: `--amp` (significant ΔL threshold, default 6.0 luma), and in
`compute_metrics` `hp_window` (moving-average length, default 5) and `min_osc`
(reversals to flag a flicker pixel, default 3).

## Control run — the acceptance test (2026-07-26, current research binary)

Real A/B on the rig: same mission snapshot `vm-20260725222138`, 60 frames/leg
captured at ~17 fps, crop 1280×960. **leg A = baseline** (no async queries),
**leg B = `XEMU_ASYNC_QUERIES=1`** (the known-flickery positive control).

```
  metric          baseline      async-queries   ratio
  flicker_score     0.1143          0.6993       6.12x   <- PRIMARY
  flicker_energy    0.3778          3.6068       9.55x
  hp_energy         0.8639          3.8771       4.49x
  hot_mad           2.1272          8.1000       3.81x   (HUD/light region |ΔL|)
  mad               1.0868          2.7722       2.55x
  flicker_frac      0.0946          0.2385       2.52x   (9.5% -> 23.9% of px)
  osc_index      1958.97         2652.99         1.35x

  VERDICT: SEPARATED — 'async-queries' flickers more
           flicker_score 6.12x; delta 0.585 vs split-half noise band 0.161
```

Every metric moves the same way (async flickers more), and the primary is
**6.12×** with the delta **3.6× above** the combined noise band — an
unambiguous, objective separation. `heat_oscillation.png` / `heat_flicker.png`
localize the async instability to the cockpit canopy glow, radar, front-view
panel, and HUD readouts (the light/glow elements), while the static cockpit
frame and gauges stay cold.

Synthetic selftest (metric-math sanity, animation vs flicker):
`flicker_score` **8.13×** (anim 0.182 → flick 1.484), 100% of flagged pixels
localized to the injected patch, animation alone flags 0.01% of pixels. PASS.

## Sensitivity / limitations (honest)

- **Capture is lossless and noise-free.** `xwd -root` is a direct framebuffer
  read, so static pixels are byte-identical frame-to-frame (`noise_floor` ≈ 0).
  There is no sensor noise floor — every luminance change is real. `--amp`
  exists only to ignore sub-threshold dithering/animation.
- **Capture rate ~17–21 fps** (xwd root grab ≈ 47 ms; ~17 fps while the
  emulator is actively rendering). Fast flicker aliases, but flicker that is
  not phase-locked to the capture still produces frequent reversals, so it is
  detected. The separation above is with this aliasing already in effect.
- **Relative, not absolute.** A legitimately blinking light would raise the
  score too — but it appears in *both* legs and cancels in the delta. Trust the
  A/B delta, not one run's absolute number.
- **Window occlusion.** The capture crops the xemu window geometry. If another
  window overlaps it (in the control run, a Nautilus window covered the right
  ~28%), those pixels are static and identical in both legs, so they only
  dilute absolute numbers — the ratio is unaffected. For maximum sensitivity,
  close overlapping windows before an A/B. No `wmctrl`/`xdotool` on the rig, so
  the runner cannot auto-raise the window.
- **Snapshot fragility.** `vm-20260725222138` is close to a crash state; the
  runner restores, settles 5 s, and captures a short (~3 s) burst to stay
  inside the safe window. The capture tolerates an instance dying mid-burst
  (records however many frames it got).

## Safety

`flicker_detect.py` and `rig_flicker_capture.sh` never launch, kill, focus, or
talk to any emulator — they only read pixels/files, so they are safe against a
live user session. Only `rig_flicker_ab.sh` launches/kills, and it manages
**only instances it launched**: it aborts on start if any
xemu/pcsx2/mupen64plus is already running.
