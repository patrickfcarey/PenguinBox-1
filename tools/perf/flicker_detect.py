#!/usr/bin/env python3
"""
flicker_detect.py — objective, automated inter-frame luminance-instability
("flicker") detector for the PenguinBox / xemu rig.

Steel Battalion mission scenes show light/glow flicker under some emulator
configs (the async-queries path). "Hard to tell" by eye is the enemy: this
tool quantifies flicker as a NUMBER so configs can be compared and a fix can
be verified without human eyes.

What flicker is, physically: a pixel that should hold a steady luminance
instead oscillates up/down frame-to-frame. That is different from genuine
animation (a radar sweep, a counting HUD clock, a panning camera), which
either drifts smoothly or moves an edge through a pixel ONCE. The metric is
built to separate the two:

  * high-pass temporal energy  — luminance minus its short moving average,
    then per-pixel temporal std. Removes slow animation drift; what survives
    is fast frame-to-frame instability. This is the primary, threshold-free
    A/B discriminator (`hp_energy`).
  * oscillation count          — number of *significant* luminance reversals
    (sign changes of the inter-frame delta, above a noise amplitude). Flicker
    reverses many times; a moving edge reverses ~once. (`osc_index`,
    `flicker_frac`).
  * mean abs inter-frame delta — the simple baseline (`mad`), reported over
    the whole frame and over the auto-detected hot (HUD/light) region.

Because we cannot perfectly mask genuinely-animated regions, the tool is
designed for RELATIVE comparison: same save state, same window, same burst
length ⇒ animated content is statistically the same between two configs, so
any DELTA in these metrics is the flicker the config added. `ab` reports the
per-run numbers, the delta, the ratio, and a split-half noise band so you can
judge whether the separation is real.

Dependencies: numpy + Pillow only (present on both the rig and the dev box).

Subcommands
  xwd2png   convert+crop a directory of raw X11 .xwd root dumps to cropped
            PNGs (run on the rig, where captures live).
  analyze   analyze a directory of PNG frames -> metrics.json + heatmaps.
  ab        compare two run directories -> delta table + verdict + report.
  inject    write a copy of a PNG run with synthetic flicker injected into a
            region (builds a known-amplitude positive control from real
            frames when a live rig A/B cannot be run).
  selftest  synthetic validation: prove the metric separates flicker from
            animation, and that animation alone does not trip it.

Safety: this file never launches, kills, or talks to any emulator. It only
reads image files. Capture/orchestration (which can launch instances) lives
in the rig-side shell scripts and guards a live session before doing so.
"""

import argparse
import glob
import json
import os
import struct
import sys
import time

import numpy as np
from PIL import Image

# Rec.709 luma weights.
_LW = np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)

# ---------------------------------------------------------------------------
# XWD (X Window Dump) reader.
#
# xwd is the only screen-capture tool guaranteed on the rig (no ffmpeg / mss /
# ImageMagick / netpbm, and Pillow has no XWD plugin). The format is a header
# of 25 big-or-little-endian CARD32s, an optional window-name string, an
# optional colormap (ncolors * 12 bytes), then ZPixmap scanline data. We
# detect header endianness from the file_version field (== 7), take the pixel
# byte order and channel masks from the header, and slice with the recorded
# scanline stride. Validated against a real 1920x1080 depth-24 rig capture
# (4 bytes/pixel, masks R=0xff0000 G=0xff00 B=0xff, 256-entry colormap).
# ---------------------------------------------------------------------------

def read_xwd(path):
    """Return (rgb uint8 HxWx3, meta dict) for an xwd v7 ZPixmap dump."""
    with open(path, "rb") as f:
        raw = f.read()
    if len(raw) < 100:
        raise ValueError("%s: too small to be an xwd file" % path)
    # Endianness of the HEADER itself: file_version (field 1) must read as 7.
    (_, ver_be) = struct.unpack_from(">II", raw, 0)
    endian = ">" if ver_be == 7 else "<"
    h = struct.unpack_from(endian + "25I", raw, 0)
    (header_size, file_version, pixmap_format, pixmap_depth, width, height,
     _xoff, byte_order, _bunit, _bborder, _bpad, bits_per_pixel,
     bytes_per_line, _vclass, red_mask, green_mask, blue_mask, _bprgb,
     _cmap_entries, ncolors, _ww, _wh, _wx, _wy, _wbdr) = h
    if file_version != 7:
        raise ValueError("%s: unsupported xwd version %d" % (path, file_version))
    if pixmap_format != 2:
        raise ValueError("%s: only ZPixmap (2) supported, got %d"
                         % (path, pixmap_format))
    pix_off = header_size + ncolors * 12
    stride = bytes_per_line
    # Trust the stride over bits_per_pixel: some servers report depth (24)
    # in bits_per_pixel while storing 32-bit units. stride/width is exact.
    bpp = stride // width
    need = stride * height
    if pix_off + need > len(raw):
        raise ValueError("%s: truncated pixel data" % path)
    data = np.frombuffer(raw, dtype=np.uint8, count=need, offset=pix_off)
    data = data.reshape(height, stride)[:, :width * bpp].reshape(height, width, bpp)

    if bpp == 4:
        b = data.astype(np.uint32)
        if byte_order == 0:  # LSBFirst -> little-endian 32-bit unit
            P = b[..., 0] | (b[..., 1] << 8) | (b[..., 2] << 16) | (b[..., 3] << 24)
        else:                # MSBFirst
            P = b[..., 3] | (b[..., 2] << 8) | (b[..., 1] << 16) | (b[..., 0] << 24)
    elif bpp == 3:
        b = data.astype(np.uint32)
        if byte_order == 0:
            P = b[..., 0] | (b[..., 1] << 8) | (b[..., 2] << 16)
        else:
            P = b[..., 2] | (b[..., 1] << 8) | (b[..., 0] << 16)
    else:
        raise ValueError("%s: unsupported bytes/pixel %d" % (path, bpp))

    def chan(mask):
        if mask == 0:
            return np.zeros((height, width), np.uint8)
        shift = (mask & -mask).bit_length() - 1
        return ((P & mask) >> shift).astype(np.uint8)

    rgb = np.dstack([chan(red_mask), chan(green_mask), chan(blue_mask)])
    meta = dict(width=int(width), height=int(height), depth=int(pixmap_depth),
                bpp=int(bpp), byte_order=int(byte_order),
                red=int(red_mask), green=int(green_mask), blue=int(blue_mask))
    return rgb, meta


# ---------------------------------------------------------------------------
# Frame loading / luminance.
# ---------------------------------------------------------------------------

def luma(rgb):
    """HxWx3 uint8/float -> HxW float32 Rec.709 luminance (0..255)."""
    return (rgb[..., :3].astype(np.float32) * _LW).sum(-1)


# PNGs this tool writes INTO a run dir — never load them as capture frames
# (analyze writes them alongside the frames, so re-analyzing must skip them).
_ARTIFACT_PNGS = {"ref.png", "heat_variance.png", "heat_flicker.png",
                  "heat_oscillation.png", "flicker_overlay.png"}


def _is_artifact(p):
    b = os.path.basename(p)
    return b in _ARTIFACT_PNGS or b.endswith("_osc_side_by_side.png")


def load_luma_stack(png_dir, pattern="*.png"):
    """Load sorted capture PNGs -> (L float32 (N,H,W), ref_rgb, paths).
    Skips artifact PNGs the tool writes, so re-analysis is idempotent."""
    paths = [p for p in sorted(glob.glob(os.path.join(png_dir, pattern)))
             if not _is_artifact(p)]
    if not paths:
        raise SystemExit("no capture frames matching %s in %s" % (pattern, png_dir))
    frames = []
    ref_rgb = None
    shape = None
    for i, p in enumerate(paths):
        im = Image.open(p).convert("RGB")
        a = np.asarray(im)
        if shape is None:
            shape = a.shape
        elif a.shape != shape:
            raise SystemExit("frame %s shape %s != %s (crop drifted mid-burst?)"
                             % (p, a.shape, shape))
        frames.append(luma(a))
        if i == len(paths) // 2:
            ref_rgb = a.copy()
    L = np.stack(frames, 0)
    return L, ref_rgb, paths


# ---------------------------------------------------------------------------
# Metric core.
# ---------------------------------------------------------------------------

def _moving_avg_time(L, w):
    """Centered moving average along axis 0 (time), edge-replicated. w odd."""
    n = L.shape[0]
    if w <= 1 or n < 3:
        return L.copy()
    if w % 2 == 0:
        w += 1
    w = min(w, n if n % 2 == 1 else n - 1)
    if w < 3:
        return L.copy()
    pad = w // 2
    Lp = np.pad(L, ((pad, pad), (0, 0), (0, 0)), mode="edge")
    cs = np.cumsum(np.pad(Lp, ((1, 0), (0, 0), (0, 0))), axis=0)
    return (cs[w:] - cs[:-w]) / w  # length n


def _osc_count(diffs, amp_thresh):
    """Per-pixel count of SIGNIFICANT luminance reversals. A significant diff
    has |dL| > amp_thresh; we carry the last significant sign forward and
    count how often the next significant diff flips it. A flickering pixel
    reverses many times; a moving animation edge passes through ~once."""
    h, w = diffs.shape[1:]
    sg = np.zeros(diffs.shape, np.int8)
    sg[diffs > amp_thresh] = 1
    sg[diffs < -amp_thresh] = -1
    cf = np.zeros((h, w), np.int8)                  # last significant sign
    osc = np.zeros((h, w), np.int32)
    for t in range(sg.shape[0]):
        st = sg[t]
        osc += (st != 0) & (cf != 0) & (st != cf)
        cf = np.where(st != 0, st, cf)
    return osc


def _primary(L, amp_thresh, hp_window):
    """Return (hp_energy, flicker_score, resid_std, osc, base). The primary
    metric `flicker_score` is high-pass amplitude WEIGHTED by oscillation
    frequency: mean_px( resid_std * osc/(N-1) ). Flicker = steady-amplitude,
    high-frequency oscillation, so it scores high; genuine animation has high
    amplitude but LOW oscillation frequency (an edge crosses once), so it is
    suppressed; capture noise has low amplitude, also suppressed."""
    n = L.shape[0]
    diffs = np.diff(L, axis=0)
    base = _moving_avg_time(L, hp_window)
    resid_std = (L - base).std(0)
    osc = _osc_count(diffs, amp_thresh)
    hp_energy = float(resid_std.mean())
    m = max(1, n - 1)
    flicker_score = float((resid_std * (osc.astype(np.float32) / m)).mean())
    return hp_energy, flicker_score, resid_std, osc, diffs


def compute_metrics(L, amp_thresh=6.0, hp_window=5, min_osc=3):
    """
    L: (N,H,W) float32 luminance.
    Returns (scalars dict, maps dict). Maps are HxW float arrays for heatmaps.
    """
    n, h, w = L.shape
    npx = h * w
    m = max(1, n - 1)

    hp_energy, flicker_score, resid_std, osc, diffs = _primary(L, amp_thresh, hp_window)
    absd = np.abs(diffs)
    mad = float(absd.mean())
    absd_map = absd.mean(0)                         # per-pixel mean |dL|
    var_map = L.var(0)                              # raw variance (any change)
    osc_map = osc.astype(np.float32)
    osc_index = float(osc_map.mean() * 1000.0)      # reversals/px/burst x1000

    # noise floor: median resid_std over the calmest half of pixels — the
    # capture/quantization/dither floor, not scene motion.
    calm = resid_std <= np.median(resid_std)
    noise_floor = float(np.median(resid_std[calm])) if calm.any() else 0.0

    # flicker pixels: enough amplitude AND enough reversals (oscillating, not
    # a one-shot animation edge). Amplitude gate floats above the noise floor.
    amp_gate = max(amp_thresh * 0.5, noise_floor * 2.0)
    flicker_mask = (resid_std > amp_gate) & (osc >= min_osc)
    flicker_px = int(flicker_mask.sum())
    flicker_frac = float(flicker_px / npx)
    flicker_energy = float(resid_std[flicker_mask].sum() / npx) if flicker_px else 0.0

    # hot region = bbox of the top-1% temporal-variance pixels (where the
    # HUD / lights / radar live). Report mean |dL| there.
    thr = np.percentile(var_map, 99.0)
    hot = var_map >= thr
    ys, xs = np.where(hot)
    if len(ys):
        y0, y1, x0, x1 = int(ys.min()), int(ys.max()), int(xs.min()), int(xs.max())
        hot_mad = float(absd[:, y0:y1 + 1, x0:x1 + 1].mean())
        hot_bbox = [x0, y0, x1 - x0 + 1, y1 - y0 + 1]
    else:
        hot_mad, hot_bbox = 0.0, [0, 0, w, h]

    # split-half stability (even vs odd frames) -> a self-noise estimate for
    # the primary metric, so A/B separation can be judged against it. Even and
    # odd sub-bursts sample the same scene, so their spread is measurement
    # noise, not signal.
    def score_of(sub):
        if sub.shape[0] < 3:
            return flicker_score
        return _primary(sub, amp_thresh, hp_window)[1]
    fs_even = score_of(L[0::2])
    fs_odd = score_of(L[1::2])
    fs_split = abs(fs_even - fs_odd)

    scalars = dict(
        frames=int(n), height=int(h), width=int(w),
        amp_thresh=float(amp_thresh), hp_window=int(hp_window),
        min_osc=int(min_osc), amp_gate=float(amp_gate),
        noise_floor=noise_floor,
        mad=mad,
        flicker_score=flicker_score,   # PRIMARY A/B metric (amp x osc-freq)
        flicker_split_noise=fs_split,  # self-noise band on flicker_score
        hp_energy=hp_energy,           # total high-pass instability (incl. anim)
        osc_index=osc_index,
        flicker_px=flicker_px,
        flicker_frac=flicker_frac,
        flicker_energy=flicker_energy,
        hot_bbox=hot_bbox,
        hot_mad=hot_mad,
    )
    maps = dict(absd=absd_map, var=var_map, resid_std=resid_std,
                osc=osc_map, flicker_mask=flicker_mask.astype(np.float32))
    return scalars, maps


# ---------------------------------------------------------------------------
# Heatmap rendering (numpy-only "heat" colormap: black->red->yellow->white).
# ---------------------------------------------------------------------------

def _heat_lut(x01):
    """x in [0,1] -> HxWx3 uint8 heat color."""
    x = np.clip(x01, 0.0, 1.0)
    r = np.clip(x * 3.0, 0, 1)
    g = np.clip((x - 1 / 3.0) * 3.0, 0, 1)
    b = np.clip((x - 2 / 3.0) * 3.0, 0, 1)
    return (np.dstack([r, g, b]) * 255).astype(np.uint8)


def render_heatmap(m, ref_rgb, out_path, robust_pct=99.5, gamma=0.6):
    """Normalize map m (HxW), colorize, blend over a dimmed reference frame."""
    vmax = np.percentile(m, robust_pct)
    if vmax <= 0:
        vmax = float(m.max()) or 1.0
    norm = np.clip(m / vmax, 0, 1) ** gamma
    heat = _heat_lut(norm)
    if ref_rgb is not None:
        dim = (ref_rgb.astype(np.float32) * 0.35).astype(np.uint8)
        a = norm[..., None]
        out = (dim.astype(np.float32) * (1 - a) + heat.astype(np.float32) * a)
        out = out.astype(np.uint8)
    else:
        out = heat
    Image.fromarray(out).save(out_path)
    return float(vmax)


def render_flicker_overlay(flicker_mask, ref_rgb, out_path):
    """Tint flagged flicker pixels red over the reference frame."""
    if ref_rgb is None:
        return
    out = ref_rgb.astype(np.float32).copy()
    m = flicker_mask.astype(bool)
    out[m, 0] = np.minimum(255, out[m, 0] * 0.3 + 255 * 0.7)
    out[m, 1] = out[m, 1] * 0.3
    out[m, 2] = out[m, 2] * 0.3
    Image.fromarray(out.astype(np.uint8)).save(out_path)


# ---------------------------------------------------------------------------
# Subcommand: xwd2png (rig-side convert + crop)
# ---------------------------------------------------------------------------

def cmd_xwd2png(args):
    os.makedirs(args.dst, exist_ok=True)
    src = sorted(glob.glob(os.path.join(args.src, args.glob)))
    if not src:
        raise SystemExit("no files matching %s in %s" % (args.glob, args.src))
    n = 0
    for p in src:
        rgb, meta = read_xwd(p)
        H, W = rgb.shape[:2]
        x, y = args.x, args.y
        w = args.w if args.w > 0 else W - x
        hh = args.h if args.h > 0 else H - y
        x0, y0 = max(0, x), max(0, y)
        x1, y1 = min(W, x + w), min(H, y + hh)
        crop = rgb[y0:y1, x0:x1]
        base = os.path.splitext(os.path.basename(p))[0]
        Image.fromarray(crop).save(os.path.join(args.dst, base + ".png"))
        n += 1
    print("xwd2png: wrote %d PNGs to %s (crop %d,%d %dx%d)"
          % (n, args.dst, args.x, args.y, args.w, args.h))


# ---------------------------------------------------------------------------
# Subcommand: analyze
# ---------------------------------------------------------------------------

def _load_meta(run_dir):
    p = os.path.join(run_dir, "meta.json")
    if os.path.exists(p):
        with open(p) as f:
            return json.load(f)
    return {}

def analyze_run(run_dir, label=None, amp_thresh=6.0, write=True):
    L, ref_rgb, paths = load_luma_stack(run_dir)
    scalars, maps = compute_metrics(L, amp_thresh=amp_thresh)
    meta = _load_meta(run_dir)
    scalars["label"] = label or meta.get("label") or os.path.basename(
        os.path.normpath(run_dir))
    scalars["run_dir"] = os.path.abspath(run_dir)
    scalars["n_paths"] = len(paths)
    scalars["config"] = meta.get("config", {})
    if write:
        with open(os.path.join(run_dir, "metrics.json"), "w") as f:
            json.dump(scalars, f, indent=2)
        if ref_rgb is not None:
            Image.fromarray(ref_rgb).save(os.path.join(run_dir, "ref.png"))
        render_heatmap(maps["var"], ref_rgb,
                       os.path.join(run_dir, "heat_variance.png"))
        render_heatmap(maps["resid_std"], ref_rgb,
                       os.path.join(run_dir, "heat_flicker.png"))
        render_heatmap(maps["osc"], ref_rgb,
                       os.path.join(run_dir, "heat_oscillation.png"))
        render_flicker_overlay(maps["flicker_mask"], ref_rgb,
                               os.path.join(run_dir, "flicker_overlay.png"))
    return scalars, maps, ref_rgb


def cmd_analyze(args):
    scalars, _, _ = analyze_run(args.run_dir, label=args.label,
                                amp_thresh=args.amp, write=not args.no_write)
    print(_fmt_scalars(scalars))
    print("\nwrote metrics.json + heat_*.png + flicker_overlay.png to %s"
          % args.run_dir)


def _fmt_scalars(s):
    return "\n".join([
        "run:            %s  (%d frames, %dx%d)" % (
            s["label"], s["frames"], s["width"], s["height"]),
        "noise_floor:    %7.3f  (capture/quant floor, luma)" % s["noise_floor"],
        "flicker_score:  %7.4f  <-- PRIMARY (amp x osc-freq)  +/- %.4f" % (
            s["flicker_score"], s["flicker_split_noise"]),
        "osc_index:      %7.3f  significant reversals/px x1000" % s["osc_index"],
        "flicker_frac:   %7.4f  (%d px flagged flicker)" % (
            s["flicker_frac"], s["flicker_px"]),
        "flicker_energy: %7.3f  mean amplitude over flicker px" % s["flicker_energy"],
        "hp_energy:      %7.3f  total high-pass instability (incl. animation)"
        % s["hp_energy"],
        "mad:            %7.3f  mean |dL| whole frame" % s["mad"],
        "hot_mad:        %7.3f  mean |dL| in HUD/light region %s" % (
            s["hot_mad"], s["hot_bbox"]),
    ])


# ---------------------------------------------------------------------------
# Subcommand: ab
# ---------------------------------------------------------------------------

def cmd_ab(args):
    sa, ma, ra = analyze_run(args.run_a, label=args.label_a, amp_thresh=args.amp)
    sb, mb, rb = analyze_run(args.run_b, label=args.label_b, amp_thresh=args.amp)

    rows = ["flicker_score", "osc_index", "flicker_frac", "flicker_energy",
            "hp_energy", "mad", "hot_mad"]
    lines = ["A/B flicker comparison",
             "  A = %s" % sa["label"],
             "  B = %s" % sb["label"], ""]
    for k in rows:
        va, vb = sa[k], sb[k]
        d = vb - va
        ratio = (vb / va) if va else float("inf")
        lines.append("  %-14s A=%9.4f  B=%9.4f  d(B-A)=%+9.4f  x%.2f"
                     % (k, va, vb, d, ratio))

    # verdict on the primary metric, judged against combined split-half noise.
    dprim = sb["flicker_score"] - sa["flicker_score"]
    band = sa["flicker_split_noise"] + sb["flicker_split_noise"]
    rprim = (sb["flicker_score"] / sa["flicker_score"]) if sa["flicker_score"] else float("inf")
    if abs(dprim) <= band or abs(dprim) < 1e-9:
        verdict = ("INSEPARABLE: |flicker_score delta| %.4f within noise band %.4f"
                   % (abs(dprim), band))
    else:
        more = sb["label"] if dprim > 0 else sa["label"]
        verdict = ("SEPARATED: '%s' flickers more  (flicker_score %.2fx, delta %.4f "
                   "vs noise band %.4f)" % (more, rprim, abs(dprim), band))
    lines += ["", "  VERDICT: " + verdict]

    report = dict(A=sa, B=sb,
                  delta={k: sb[k] - sa[k] for k in rows},
                  ratio={k: (sb[k] / sa[k] if sa[k] else None) for k in rows},
                  hp_noise_band=band, verdict=verdict)
    out = args.out or "flicker_ab_report.json"
    with open(out, "w") as f:
        json.dump(report, f, indent=2)

    # side-by-side oscillation heatmaps for the human eye
    try:
        if ra is not None and rb is not None:
            va = np.percentile(np.concatenate([ma["osc"].ravel(), mb["osc"].ravel()]), 99.5) or 1.0
            def col(m, ref):
                norm = np.clip(m / va, 0, 1) ** 0.6
                heat = _heat_lut(norm).astype(np.float32)
                dim = ref.astype(np.float32) * 0.35
                a = norm[..., None]
                return (dim * (1 - a) + heat * a).astype(np.uint8)
            ia, ib = col(ma["osc"], ra), col(mb["osc"], rb)
            h = min(ia.shape[0], ib.shape[0])
            gap = np.full((h, 8, 3), 40, np.uint8)
            sbs = np.concatenate([ia[:h], gap, ib[:h]], axis=1)
            Image.fromarray(sbs).save(os.path.splitext(out)[0] + "_osc_side_by_side.png")
    except Exception as e:
        print("(side-by-side render skipped: %s)" % e)

    print("\n".join(lines))
    print("\nwrote %s (+ *_osc_side_by_side.png)" % out)
    return report


# ---------------------------------------------------------------------------
# Subcommand: inject (build a known-amplitude positive control from real PNGs)
# ---------------------------------------------------------------------------

def cmd_inject(args):
    os.makedirs(args.dst, exist_ok=True)
    paths = sorted(glob.glob(os.path.join(args.src, "*.png")))
    if not paths:
        raise SystemExit("no PNGs in %s" % args.src)
    x, y, w, h = args.region
    rng = np.random.default_rng(args.seed)
    period = max(2, args.period)
    for i, p in enumerate(paths):
        a = np.asarray(Image.open(p).convert("RGB")).astype(np.float32)
        H, W = a.shape[:2]
        x0, y0 = max(0, x), max(0, y)
        x1, y1 = min(W, x + w), min(H, y + h)
        # square-wave luminance oscillation on the region (a glow popping)
        phase = 1.0 if (i // 1) % period < period / 2 else -1.0
        # add a little jitter so it is not a perfect square wave
        amp = args.amp * (0.7 + 0.3 * rng.random())
        a[y0:y1, x0:x1, :] = np.clip(a[y0:y1, x0:x1, :] + phase * amp, 0, 255)
        Image.fromarray(a.astype(np.uint8)).save(
            os.path.join(args.dst, os.path.basename(p)))
    meta = dict(label=args.label or "injected", config={"injected_flicker": {
        "region": [x, y, w, h], "amp": args.amp, "period": period}})
    with open(os.path.join(args.dst, "meta.json"), "w") as f:
        json.dump(meta, f, indent=2)
    print("inject: wrote %d frames to %s (region %s amp %.1f period %d)"
          % (len(paths), args.dst, args.region, args.amp, period))


# ---------------------------------------------------------------------------
# Subcommand: selftest (synthetic validation of the metric itself)
# ---------------------------------------------------------------------------

def _synth(n, h, w, flicker=False, seed=0):
    """Build a synthetic burst: static texture + genuine animation (a panning
    bright bar + a smooth global drift) and, optionally, a flickering patch.
    Returns L (N,H,W) float32."""
    rng = np.random.default_rng(seed)
    base = (rng.random((h, w)).astype(np.float32) * 30 + 60)  # static texture
    L = np.repeat(base[None], n, 0).copy()
    # capture-like noise floor
    L += rng.normal(0, 1.2, size=L.shape).astype(np.float32)
    # genuine animation 1: a bright vertical bar panning left->right
    for t in range(n):
        cx = int((t / max(1, n - 1)) * (w - 1))
        x0, x1 = max(0, cx - 8), min(w, cx + 8)
        L[t, h // 4:3 * h // 4, x0:x1] += 120
    # genuine animation 2: smooth global luminance drift (like a fade)
    drift = np.linspace(-20, 20, n).astype(np.float32)
    L += drift[:, None, None]
    if flicker:
        # a patch that should be steady but oscillates every frame (glow flicker)
        py0, py1, px0, px1 = h // 8, h // 8 + h // 6, w // 8, w // 8 + w // 6
        for t in range(n):
            L[t, py0:py1, px0:px1] += (60 if t % 2 == 0 else -60)
    return np.clip(L, 0, 255)


def cmd_selftest(args):
    n, h, w = 48, 240, 320
    L_anim = _synth(n, h, w, flicker=False, seed=1)     # negative control
    L_flick = _synth(n, h, w, flicker=True, seed=1)     # positive control
    s_anim, m_anim = compute_metrics(L_anim)
    s_flick, m_flick = compute_metrics(L_flick)

    ratio = s_flick["flicker_score"] / max(1e-6, s_anim["flicker_score"])
    band = s_anim["flicker_split_noise"] + s_flick["flicker_split_noise"]
    delta = s_flick["flicker_score"] - s_anim["flicker_score"]

    print("SELFTEST — animation-only (negative) vs animation+flicker (positive)")
    print("  flicker_score anim=%.4f flick=%.4f  ratio=%.2fx  delta=%.4f  noise_band=%.4f"
          % (s_anim["flicker_score"], s_flick["flicker_score"], ratio, delta, band))
    print("  hp_energy    anim=%.3f  flick=%.3f  (total instability, incl. animation)"
          % (s_anim["hp_energy"], s_flick["hp_energy"]))
    print("  osc_index    anim=%.3f  flick=%.3f" % (s_anim["osc_index"], s_flick["osc_index"]))
    print("  flicker_frac anim=%.4f flick=%.4f" % (s_anim["flicker_frac"], s_flick["flicker_frac"]))

    # localization check: the flagged flicker must sit in the injected patch,
    # NOT in the panning bar / drift.
    py0, py1, px0, px1 = h // 8, h // 8 + h // 6, w // 8, w // 8 + w // 6
    fm = m_flick["flicker_mask"] > 0
    total = int(fm.sum())
    inside = int(fm[py0:py1, px0:px1].sum())
    loc = (inside / total) if total else 0.0
    print("  localization: %d/%d flagged px inside injected patch (%.0f%%)"
          % (inside, total, loc * 100))

    if args.write:
        os.makedirs(args.write, exist_ok=True)
        ref = np.dstack([np.clip(L_flick[n // 2], 0, 255).astype(np.uint8)] * 3)
        render_heatmap(m_flick["osc"], ref,
                       os.path.join(args.write, "selftest_osc.png"))
        render_heatmap(m_anim["osc"], np.dstack([L_anim[n // 2].astype(np.uint8)] * 3),
                       os.path.join(args.write, "selftest_anim_osc.png"))

    ok = (ratio >= 3.0 and delta > band and loc >= 0.9
          and s_flick["flicker_frac"] > 5 * max(1e-6, s_anim["flicker_frac"]))
    print("  RESULT: %s" % ("PASS — metric cleanly separates flicker from animation"
                            if ok else "FAIL"))
    sys.exit(0 if ok else 1)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("xwd2png", help="convert+crop raw .xwd dumps to PNGs")
    p.add_argument("--src", required=True)
    p.add_argument("--dst", required=True)
    p.add_argument("--x", type=int, default=0)
    p.add_argument("--y", type=int, default=0)
    p.add_argument("--w", type=int, default=0)
    p.add_argument("--h", type=int, default=0)
    p.add_argument("--glob", default="*.xwd")
    p.set_defaults(func=cmd_xwd2png)

    p = sub.add_parser("analyze", help="analyze a PNG run dir")
    p.add_argument("run_dir")
    p.add_argument("--label")
    p.add_argument("--amp", type=float, default=6.0,
                   help="significant luminance-delta threshold (default 6)")
    p.add_argument("--no-write", action="store_true")
    p.set_defaults(func=cmd_analyze)

    p = sub.add_parser("ab", help="compare two run dirs")
    p.add_argument("run_a")
    p.add_argument("run_b")
    p.add_argument("--label-a")
    p.add_argument("--label-b")
    p.add_argument("--amp", type=float, default=6.0)
    p.add_argument("--out")
    p.set_defaults(func=cmd_ab)

    p = sub.add_parser("inject", help="inject synthetic flicker into a PNG run")
    p.add_argument("--src", required=True)
    p.add_argument("--dst", required=True)
    p.add_argument("--region", type=int, nargs=4, metavar=("X", "Y", "W", "H"),
                   required=True)
    p.add_argument("--amp", type=float, default=40.0)
    p.add_argument("--period", type=int, default=2)
    p.add_argument("--seed", type=int, default=0)
    p.add_argument("--label")
    p.set_defaults(func=cmd_inject)

    p = sub.add_parser("selftest", help="synthetic metric validation")
    p.add_argument("--write", help="dir to write demo heatmaps")
    p.set_defaults(func=cmd_selftest)

    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
