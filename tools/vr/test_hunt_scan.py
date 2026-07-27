#!/usr/bin/env python3
# Synthetic-dump self-test for tools/vr/hunt_scan.py (the Tier-3 hunt scanner).
#
# SPDX-FileCopyrightText: 2026 Patrick Carey
# SPDX-License-Identifier: LGPL-2.0-or-later
#
# Closes the "Tool discipline" gap in tools/vr/README.md: the scanner is
# load-bearing for every Tier-3 hunt and rig capture time is the scarce
# resource, so it carries a self-test against synthetic dumps (the family
# convention for math/measure tools). Run:
#
#     python3 tools/vr/test_hunt_scan.py
#
# It BUILDS synthetic F9-style dumps in a temp dir (raw guest-RAM images,
# offset 0 == physical 0, no header -- matching hunt_write_thread() in
# hw/xbox/nv2a/pgraph/vk/vr_camera.c, which fwrite()s d->vram_ptr verbatim
# to ~/xemu-vr-hunt/dump-NNN.bin plus a dump-NNN.txt pose sidecar the
# scanner never reads) and drives the REAL CLI via subprocess -- the same
# end-to-end path an operator uses, so argument parsing, printed addresses
# and exit codes are all under test. The printed address is what gets typed
# into [vr] headlook_yaw_addr: an off-by-anything there poisons a session,
# so planted offsets are asserted EXACTLY.
#
# Scanner contracts exercised (as discovered from hunt_scan.py):
#   angle mode  - dumps are read as a flat <f4 array from byte 0, so only
#                 4-aligned offsets exist in its universe; reported address
#                 is index*4 == file offset == physical address.
#               - a candidate must match the scripted deltas (vs dump 0)
#                 under one of rad/deg/-rad/-deg scaling, wrap-aware over
#                 that unit's period, within max(15% of the step, 0.02 rad
#                 equivalent); where the script repeats an angle the value
#                 must return within 0.02 rad equivalent (the repeated-pose
#                 filter that kills self-drifting values).
#               - non-finite and |v| >= 1e6 cells are ignored; dumps are
#                 truncated to the shortest one; trailing non-multiple-of-4
#                 bytes are silently dropped; empty input -> "no candidates"
#                 (exit 0), not a traceback.
#   flag mode   - byte-wise; candidate positions are those where EVERY
#                 ON-dump byte differs from EVERY OFF-dump byte (value SETS
#                 disjoint -- enum-aware, values within a group may differ);
#                 suggests the highest-popcount (mask, value) with
#                 (v & mask) == value for all first-group values and != for
#                 all second-group values, else "chain-gate needed".
#   resolve     - u32 pointer at ptr (+base_off) per dump; floats at
#                 base+off; reports base CONSTANT vs VARIES.
#   sibling     - float scan of a window for cells ~constant across `hold`
#                 dumps but varying across `vary` (span ratio 5x, span >
#                 0.5, |v| < 720).
#
# BUGS FOUND BY THIS SUITE'S FIRST RUN — both FIXED in the scanner the
# same day (2026-07-24); the checks below now assert the FIXED behavior
# and act as regressions:
#
#   BUG-1 (FIXED): half-period seam one-sided slop rejection at the
#          documented yaw protocol's 180 step — the residual is now
#          re-wrapped (`np.abs(wrap_diff(gotw - expect, period)) <= tol`),
#          so 179.5 and 180.5 deg readings at the scripted-180 pose are
#          both accepted symmetrically. Regression: test_angle_halfperiod_
#          slop_bug.
#
#   FOOTGUN-2 (FIXED): flag mode's mask/value suggestion now keys off the
#          group NAMED "on" regardless of CLI argument order; `flag
#          off=... on=...` suggests the same gate as on-first. Regression:
#          test_flag_group_order_footgun.

import math
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import random

HERE = os.path.dirname(os.path.abspath(__file__))
SCANNER = os.path.join(HERE, "hunt_scan.py")

# ---------------------------------------------------------------------------
# tiny check framework (family self-test style: PASS/FAIL lines + summary)
# ---------------------------------------------------------------------------
CHECKS = 0
FAILURES = 0
KNOWN_BUGS = 0


def check(name, cond, detail=""):
    global CHECKS, FAILURES
    CHECKS += 1
    print("  [{}] {}".format("PASS" if cond else "FAIL", name))
    if not cond:
        FAILURES += 1
        if detail:
            for line in str(detail).splitlines():
                print("         | " + line)


def check_known_bug(name, still_buggy, detail=""):
    """Expected-failure marker for BUG-1 (see header). While the scanner
    still has the bug this prints [KNOWN-BUG] and does NOT fail the run;
    if the behavior changes (bug fixed) the run FAILS so the documentation
    here gets updated deliberately (unittest expectedFailure semantics)."""
    global CHECKS, FAILURES, KNOWN_BUGS
    CHECKS += 1
    if still_buggy:
        KNOWN_BUGS += 1
        print("  [KNOWN-BUG] {} (expected failure, see BUG-1 in header)".format(name))
    else:
        FAILURES += 1
        print("  [UNEXPECTED-PASS] {} -- scanner behavior changed; retire the"
              " BUG-1 note and turn this into a plain check".format(name))
        if detail:
            print("         | " + str(detail))


# ---------------------------------------------------------------------------
# helpers: synthetic RAM images + scanner invocation + output parsing
# ---------------------------------------------------------------------------
def f32(x):
    """Round-trip through float32 -- the exact value the scanner will read."""
    return struct.unpack("<f", struct.pack("<f", float(x)))[0]


class Ram:
    """A synthetic guest-RAM image. Offset here == physical address ==
    what the scanner must print."""

    def __init__(self, size):
        self.buf = bytearray(size)

    def pf32(self, off, val):
        struct.pack_into("<f", self.buf, off, float(val))

    def pf32_misaligned(self, off, val):
        # raw bytes of a float at a NON-4-aligned offset
        self.buf[off:off + 4] = struct.pack("<f", float(val))

    def pu32(self, off, val):
        struct.pack_into("<I", self.buf, off, val)

    def pu8(self, off, val):
        self.buf[off] = val & 0xFF

    def save(self, path):
        with open(path, "wb") as f:
            f.write(self.buf)


def run_scanner(args, cwd, expect_rc0=True, label=""):
    r = subprocess.run([sys.executable, SCANNER] + list(args),
                       capture_output=True, text=True, cwd=cwd, timeout=120)
    tag = label or " ".join(args[:2])
    check("{}: no traceback on stderr".format(tag),
          "Traceback" not in r.stderr, r.stderr)
    if expect_rc0:
        check("{}: exit code 0".format(tag), r.returncode == 0,
              "rc={} stderr={}".format(r.returncode, r.stderr.strip()))
    return r


ANGLE_LINE = re.compile(
    r"^\s+0x([0-9A-Fa-f]{8})\s+\(\s*(-?(?:rad|deg))\)\s+values: (.+)$",
    re.MULTILINE)
FLAG_LINE = re.compile(
    r"^\s+0x([0-9A-Fa-f]{8})\s+(\w+)=\[([^\]]*)\] (\w+)=\[([^\]]*)\]"
    r"\s+->\s+(.+)$", re.MULTILINE)


def parse_angle(stdout):
    """-> {addr_int: (label, values_str)}"""
    out = {}
    for m in ANGLE_LINE.finditer(stdout):
        out[int(m.group(1), 16)] = (m.group(2), m.group(3).strip())
    return out


def parse_flag(stdout):
    """-> {addr_int: (grp1_name, [vals], grp2_name, [vals], suggestion)}"""
    out = {}
    for m in FLAG_LINE.finditer(stdout):
        g1 = [int(v) for v in m.group(3).split(",")] if m.group(3).strip() else []
        g2 = [int(v) for v in m.group(5).split(",")] if m.group(5).strip() else []
        out[int(m.group(1), 16)] = (m.group(2), g1, m.group(4), g2,
                                    m.group(6).strip())
    return out


def write_dumps(dirpath, rams, prefix="dump"):
    os.makedirs(dirpath, exist_ok=True)
    names = []
    for i, ram in enumerate(rams):
        name = "{}-{:03d}.bin".format(prefix, i)
        ram.save(os.path.join(dirpath, name))
        names.append(name)
    return names


def expected_values_str(vals):
    """Reproduce the scanner's values column for planted float32 data."""
    return ", ".join("{:+.4f}".format(f32(v)) for v in vals)


# ---------------------------------------------------------------------------
# angle mode
# ---------------------------------------------------------------------------
# Planted offsets (all inside the smallest dump, all 4-aligned unless the
# test is ABOUT misalignment). Chosen to be recognizable in failures.
YAW_RAD = 0x00001230
YAW_DEG = 0x00002340
YAW_NRAD = 0x00003450
YAW_NDEG = 0x00004560
DECOY_DRIFT = 0x00005670
CONST_F = 0x00006780
MISALIGNED = 0x00007002          # deliberately NOT 4-aligned


def build_angle_set(script_deg, size=0x40000, noise=True, drift_decoy=True,
                    seed=0xB07):
    """One Ram per scripted pose with the standard plant set."""
    rng = random.Random(seed)
    rams = []
    n_noise = 0x1000                       # floats at 0x8000..0xC000
    for pose_idx, a in enumerate(script_deg):
        ram = Ram(size)
        ram.pf32(YAW_RAD, math.radians(a))
        ram.pf32(YAW_DEG, float(a))
        ram.pf32(YAW_NRAD, -math.radians(a))
        ram.pf32(YAW_NDEG, -float(a))
        if drift_decoy:
            # Tracks the script in radians at every UNIQUE pose but drifts
            # by itself: at the final repeat of the first angle it reads
            # 0.30 rad instead of returning. Only the repeated-pose filter
            # can reject it (verified below by scanning without the repeat).
            if pose_idx == len(script_deg) - 1:
                ram.pf32(DECOY_DRIFT, 0.30)
            else:
                ram.pf32(DECOY_DRIFT, math.radians(a))
        ram.pf32(CONST_F, 1.234)           # never moves -> must be rejected
        ram.pf32_misaligned(MISALIGNED, math.radians(a))
        if noise:
            # region 1: plausible in-range floats that churn every dump
            # (timers/positions); region 2: raw random bytes (also
            # exercises the finite / |v|<1e6 filters). Deterministic seed.
            for k in range(n_noise):
                ram.pf32(0x8000 + 4 * k, rng.uniform(-1000.0, 1000.0))
            ram.buf[0x10000:0x11000] = bytes(rng.getrandbits(8)
                                             for _ in range(0x1000))
        rams.append(ram)
    return rams


def angle_args(mode, names, script_deg):
    return [mode] + ["{}={}".format(n, a) for n, a in zip(names, script_deg)]


def test_angle_primary(tmp):
    print("[angle] primary yaw hunt: 4 unit variants + decoy + noise "
          "(script 0/90/135/270/0)")
    script = [0, 90, 135, 270, 0]
    d = os.path.join(tmp, "angle_primary")
    names = write_dumps(d, build_angle_set(script))
    r = run_scanner(angle_args("yaw", names, script), d, label="yaw primary")
    got = parse_angle(r.stdout)

    check("header names the label ([yaw] ... candidate(s))",
          "[yaw]" in r.stdout and "candidate(s)" in r.stdout, r.stdout)

    expected = {YAW_RAD: "rad", YAW_DEG: "deg",
                YAW_NRAD: "-rad", YAW_NDEG: "-deg"}
    for addr, lab in sorted(expected.items()):
        check("planted {} tracker found at EXACT address 0x{:08X}"
              .format(lab, addr), addr in got, r.stdout)
        if addr in got:
            check("  ... with unit label '{}'".format(lab),
                  got[addr][0] == lab, "got label {}".format(got[addr][0]))

    check("self-drifting decoy at 0x{:08X} REJECTED (repeated-pose filter)"
          .format(DECOY_DRIFT), DECOY_DRIFT not in got, r.stdout)
    check("constant float at 0x{:08X} rejected".format(CONST_F),
          CONST_F not in got)
    check("no candidate at the misaligned plant's straddle cells "
          "(0x7000/0x7004)", 0x7000 not in got and 0x7004 not in got)
    check("every reported address is 4-aligned",
          all(a % 4 == 0 for a in got))
    check("no false positives: reported set == planted set exactly",
          set(got) == set(expected),
          "extra: {} missing: {}".format(
              sorted(hex(a) for a in set(got) - set(expected)),
              sorted(hex(a) for a in set(expected) - set(got))))

    # Values-column fidelity: the printed per-dump values must be the
    # planted float32 data in CLI order (catches column/row transposition).
    if YAW_RAD in got:
        want = expected_values_str([math.radians(a) for a in script])
        check("values column reproduces planted rad data in dump order",
              got[YAW_RAD][1] == want,
              "got:  {}\nwant: {}".format(got[YAW_RAD][1], want))

    # Same dumps, script truncated to drop the repeated pose: the decoy now
    # tracks perfectly and MUST appear -- proving the repeat (not luck) is
    # what rejected it above.
    r2 = run_scanner(angle_args("yaw", names[:-1], script[:-1]), d,
                     label="yaw no-repeat control")
    got2 = parse_angle(r2.stdout)
    check("control: decoy IS accepted when the script has no repeated pose "
          "(repeat filter is load-bearing)", DECOY_DRIFT in got2, r2.stdout)


def test_angle_canonical_protocol(tmp):
    print("[angle] canonical documented protocol (0/90/180/270/0)")
    script = [0, 90, 180, 270, 0]
    d = os.path.join(tmp, "angle_canonical")
    names = write_dumps(d, build_angle_set(script))
    r = run_scanner(angle_args("yaw", names, script), d, label="yaw canonical")
    got = parse_angle(r.stdout)
    # All four unit variants on ideal data: the 180 step sits on the BUG-1
    # seam but exact-tracking float32 values cancel exactly in the
    # scanner's float32 delta arithmetic and land on the good side (see
    # header; the slop test below shows where real data falls off).
    for addr, lab in [(YAW_RAD, "rad"), (YAW_DEG, "deg"),
                      (YAW_NRAD, "-rad"), (YAW_NDEG, "-deg")]:
        check("canonical script finds the {} tracker at 0x{:08X}"
              .format(lab, addr),
              addr in got and got[addr][0] == lab, r.stdout)


def test_angle_seam_cross(tmp):
    print("[angle] wrap-aware delta across the +/-180 seam (170/-170/170)")
    script = [170, -170, 170]
    d = os.path.join(tmp, "angle_seam")
    # No drift decoy here: at scripted poses {170,-170} its rad values are
    # legitimate; the repeat still holds. Keep the plant set minimal.
    names = write_dumps(d, build_angle_set(script, drift_decoy=False))
    r = run_scanner(angle_args("yaw", names, script), d, label="yaw seam")
    got = parse_angle(r.stdout)
    # The stored rad value jumps 2.967 -> -2.967 (raw delta -5.934, i.e.
    # -340 deg) yet the scripted delta is +20 deg after wrapping: only a
    # wrap-aware matcher finds it.
    check("rad tracker crossing the +/-pi seam found (raw delta -340deg "
          "== +20deg wrapped)", got.get(YAW_RAD, ("",))[0] == "rad", r.stdout)
    check("deg tracker crossing the 0/360 seam found",
          got.get(YAW_DEG, ("",))[0] == "deg", r.stdout)
    return names, got  # reused by the metadata-sidecar test


def test_angle_pitch(tmp):
    print("[pitch] same machinery under the pitch label (0/60/0/-60)")
    script = [0, 60, 0, -60]
    d = os.path.join(tmp, "angle_pitch")
    rams = []
    for a in script:
        ram = Ram(0x8000)
        ram.pf32(0x00000C1C, math.radians(a))   # the OFP pitch offset shape
        rams.append(ram)
    names = write_dumps(d, rams)
    r = run_scanner(angle_args("pitch", names, script), d, label="pitch")
    got = parse_angle(r.stdout)
    check("header carries the pitch label", "[pitch]" in r.stdout, r.stdout)
    check("planted pitch tracker found at EXACT address 0x00000C1C as rad",
          got.get(0x00000C1C, ("",))[0] == "rad", r.stdout)
    check("pitch scan reports nothing else", set(got) == {0x00000C1C})


def test_angle_metadata_sidecars(tmp, seam_names, seam_got):
    print("[angle] dump-NNN.txt head-pose sidecars don't perturb scanning")
    d = os.path.join(tmp, "angle_seam")
    # Sidecar format from vr_camera.c hunt_write_thread(); the scanner takes
    # explicit .bin paths and must ignore these files entirely.
    for i, n in enumerate(seam_names):
        with open(os.path.join(d, n.replace(".bin", ".txt")), "w") as f:
            f.write("head_yaw_deg={:.2f}\nhead_pitch_deg=0.00\nsize=262144\n"
                    .format(float([170, -170, 170][i])))
    script = [170, -170, 170]
    r = run_scanner(angle_args("yaw", seam_names, script), d,
                    label="yaw with sidecars")
    got = parse_angle(r.stdout)
    check("candidate set identical with pose sidecars present",
          got == seam_got,
          "with: {}\nwithout: {}".format(sorted(got), sorted(seam_got)))


def test_angle_degenerate(tmp):
    print("[angle] degenerate inputs and CLI misuse")
    d = os.path.join(tmp, "angle_degenerate")
    os.makedirs(d, exist_ok=True)

    # empty (0-byte) dumps: clean "no candidates", not a traceback
    for n in ("empty-000.bin", "empty-001.bin"):
        open(os.path.join(d, n), "wb").close()
    r = run_scanner(["yaw", "empty-000.bin=0", "empty-001.bin=90"], d,
                    label="yaw empty dumps")
    check("empty dumps -> 'no candidates' message",
          "no candidates" in r.stdout, r.stdout)

    # size not a multiple of 4: trailing bytes silently dropped, no crash
    for n in ("odd-000.bin", "odd-001.bin"):
        with open(os.path.join(d, n), "wb") as f:
            f.write(b"\x00" * 1003)
    r = run_scanner(["yaw", "odd-000.bin=0", "odd-001.bin=90"], d,
                    label="yaw odd-size dumps")
    check("non-multiple-of-4 dump handled ('no candidates')",
          "no candidates" in r.stdout, r.stdout)

    # ragged sizes: scan window is the SHORTEST dump. A tracker inside the
    # common window is found; one planted only beyond it cannot be.
    script = [0, 90, 0]
    sizes = [0x8000, 0x8000, 0x6000]
    rams = []
    for a, size in zip(script, sizes):
        ram = Ram(size)
        ram.pf32(0x1230, math.radians(a))
        if size > 0x7000:
            ram.pf32(0x7000, math.radians(a))   # outside the common window
        rams.append(ram)
    names = write_dumps(d, rams, prefix="ragged")
    r = run_scanner(angle_args("yaw", names, script), d, label="yaw ragged")
    got = parse_angle(r.stdout)
    check("ragged dumps: tracker inside the common (min-size) window found",
          got.get(0x1230, ("",))[0] == "rad", r.stdout)
    check("ragged dumps: offset beyond the shortest dump not reported",
          0x7000 not in got)

    # CLI misuse: a single dump cannot be scanned -> usage error, rc != 0
    r = subprocess.run([sys.executable, SCANNER, "yaw", "ragged-000.bin=0"],
                       capture_output=True, text=True, cwd=d)
    check("single-dump invocation exits non-zero with usage text",
          r.returncode != 0 and "usage" in (r.stderr + r.stdout).lower(),
          "rc={} out={} err={}".format(r.returncode, r.stdout, r.stderr))


def test_angle_halfperiod_slop_bug(tmp):
    print("[angle] BUG-1 repro: one-sided slop rejection at the 180 step")
    d = os.path.join(tmp, "angle_bug1b")
    script = [0, 180]

    def two_dumps(prefix, second_deg_as_rad):
        rams = []
        for v in (0.0, math.radians(second_deg_as_rad)):
            ram = Ram(0x2000)
            ram.pf32(0x1230, v)
            rams.append(ram)
        return write_dumps(d, rams, prefix=prefix)

    # 179.5 deg at the scripted-180 press: within the advertised "few
    # degrees of slop ... 15%" (README / RESEARCH.md section 2). Before
    # the BUG-1 fix the unwrapped residual vs expect=-pi was ~2*pi and
    # this was rejected; the wrapped-residual scanner must accept it.
    names = two_dumps("low", 179.5)
    r = run_scanner(angle_args("yaw", names, script), d, label="yaw slop-low")
    got_low = parse_angle(r.stdout)
    check("rad tracker with 179.5deg reading at the scripted-180 pose "
          "accepted (BUG-1 regression: slop below 180)",
          0x1230 in got_low, r.stdout)

    # ... while the SAME slop on the other side passes: the asymmetry that
    # makes the bug silent and session-poisoning.
    names = two_dumps("high", 180.5)
    r = run_scanner(angle_args("yaw", names, script), d, label="yaw slop-high")
    got_high = parse_angle(r.stdout)
    check("mirror control: 180.5deg reading at the scripted-180 pose IS "
          "accepted (one-sidedness demonstrated)",
          got_high.get(0x1230, ("",))[0] == "rad", r.stdout)


# ---------------------------------------------------------------------------
# flag mode
# ---------------------------------------------------------------------------
GATE_ENUM = 0x00000777    # ON {2,3} vs OFF {0,1}   -> mask 254 value 2
GATE_BOOL = 0x00000200    # ON {1}   vs OFF {0}     -> mask 255 value 1
GATE_CHAIN = 0x00000300   # ON {1,2} vs OFF {0,3}   -> disjoint, no mask
OVERLAP = 0x00000100      # value 6 appears in both -> must be rejected


def build_flag_set(tmp):
    d = os.path.join(tmp, "flag_primary")
    os.makedirs(d, exist_ok=True)
    rng = random.Random(0xF1A6)
    on_vals = [2, 3]                  # ads, ads+zoom (enum values differ)
    off_vals = [0, 1, 0]              # hip, plain zoom, hip again
    on_overlap = [5, 6]
    off_overlap = [6, 7, 7]           # 6 collides with an ON dump
    on_chain = [1, 2]
    off_chain = [0, 3, 0]
    noise_pos = [0x800 + 8 * k for k in range(8)]   # sparse per-dump churn
    sizes_off = [0x1000, 0x1000, 0x0A00]            # one short OFF dump

    def build(gate, ovl, chain, size):
        ram = Ram(size)
        ram.pu8(GATE_ENUM, gate)
        ram.pu8(GATE_BOOL, 1 if gate in (2, 3) else 0)
        ram.pu8(GATE_CHAIN, chain)
        ram.pu8(OVERLAP, ovl)
        for p in noise_pos:
            ram.pu8(p, rng.getrandbits(8))
        return ram

    files = []
    for i in range(2):
        n = "on-{:03d}.bin".format(i)
        build(on_vals[i], on_overlap[i], on_chain[i], 0x1000).save(
            os.path.join(d, n))
        files.append("on=" + n)
    for i in range(3):
        n = "off-{:03d}.bin".format(i)
        build(off_vals[i], off_overlap[i], off_chain[i], sizes_off[i]).save(
            os.path.join(d, n))
        files.append("off=" + n)
    return d, files


def test_flag_primary(tmp):
    print("[flag] gate-byte hunt: enum + boolean + chain-gate + overlap")
    d, files = build_flag_set(tmp)
    r = run_scanner(["flag"] + files, d, label="flag primary")
    got = parse_flag(r.stdout)

    check("planted enum gate found at EXACT address 0x{:08X}"
          .format(GATE_ENUM), GATE_ENUM in got, r.stdout)
    if GATE_ENUM in got:
        na, g1, nb, g2, sug = got[GATE_ENUM]
        check("  ... ON/OFF value sets reported correctly",
              (na, g1, nb, g2) == ("on", [2, 3], "off", [0, 1]),
              str(got[GATE_ENUM]))
        check("  ... enum-aware suggestion: gate_mask=254 gate_value=2 "
              "(drops the ads/ads+zoom bit)",
              sug == "gate_mask=254 gate_value=2", sug)

    if GATE_BOOL in got:
        sug = got[GATE_BOOL][4]
        check("boolean gate suggestion is the most specific mask "
              "(gate_mask=255 gate_value=1)",
              sug == "gate_mask=255 gate_value=1", sug)
    else:
        check("planted boolean gate found at 0x{:08X}".format(GATE_BOOL),
              False, r.stdout)

    check("disjoint-but-unmaskable byte listed with chain-gate advice",
          GATE_CHAIN in got and "chain-gate" in got[GATE_CHAIN][4],
          r.stdout)
    check("byte overlapping between states (6 in ON and OFF) REJECTED",
          OVERLAP not in got)
    still = [0x0000, 0x0250, 0x07F0, 0x0900]     # identical in every dump
    check("still bytes (identical across dumps) rejected",
          all(a not in got for a in still))
    check("short OFF dump truncates the scan window without losing plants "
          "(all plants < 0xA00)", GATE_ENUM in got and GATE_BOOL in got)


def test_flag_degenerate(tmp):
    print("[flag] degenerate inputs and CLI misuse")
    d = os.path.join(tmp, "flag_degenerate")
    os.makedirs(d, exist_ok=True)
    ram = Ram(0x400)
    ram.pu8(0x123, 7)
    for n in ("same-on.bin", "same-off.bin"):
        ram.save(os.path.join(d, n))
    r = run_scanner(["flag", "on=same-on.bin", "off=same-off.bin"], d,
                    label="flag identical dumps")
    check("identical ON/OFF dumps -> 'no candidates' message",
          "no candidates" in r.stdout, r.stdout)

    for n in ("e-on.bin", "e-off.bin"):
        open(os.path.join(d, n), "wb").close()
    r = run_scanner(["flag", "on=e-on.bin", "off=e-off.bin"], d,
                    label="flag empty dumps")
    check("empty flag dumps -> 'no candidates', no traceback",
          "no candidates" in r.stdout, r.stdout)

    r = subprocess.run([sys.executable, SCANNER, "flag",
                        "on=same-on.bin", "on=same-off.bin"],
                       capture_output=True, text=True, cwd=d)
    check("single-group invocation exits non-zero "
          "('needs exactly two group names')",
          r.returncode != 0 and "two group" in (r.stderr + r.stdout),
          "rc={} err={}".format(r.returncode, r.stderr))


def test_flag_group_order_footgun(tmp):
    print("[flag] FOOTGUN-2: suggestion follows CLI group ORDER, not the "
          "'on' name (documented behavior pin)")
    d = os.path.join(tmp, "flag_order")
    os.makedirs(d, exist_ok=True)
    on = Ram(0x40)
    on.pu8(0x20, 1)
    off = Ram(0x40)
    off.pu8(0x20, 0)
    on.save(os.path.join(d, "on.bin"))
    off.save(os.path.join(d, "off.bin"))

    r = run_scanner(["flag", "on=on.bin", "off=off.bin"], d,
                    label="flag on-first")
    sug = parse_flag(r.stdout).get(0x20, ("", [], "", [], ""))[4]
    check("on= first (documented usage): suggestion gates the ON state "
          "(gate_value=1)", sug == "gate_mask=255 gate_value=1", r.stdout)

    r = run_scanner(["flag", "off=off.bin", "on=on.bin"], d,
                    label="flag off-first")
    sug = parse_flag(r.stdout).get(0x20, ("", [], "", [], ""))[4]
    # FOOTGUN-2 regression: the suggestion keys off the group NAMED "on",
    # so off-first must yield the same gate as on-first.
    check("off= first: suggestion still gates the ON state "
          "(gate_value=1) -- name-keyed, order-insensitive",
          sug == "gate_mask=255 gate_value=1", r.stdout)


# ---------------------------------------------------------------------------
# resolve + sibling smoke (same load-bearing address arithmetic)
# ---------------------------------------------------------------------------
def test_resolve_smoke(tmp):
    print("[resolve] pointer-anchor durability check (smoke)")
    d = os.path.join(tmp, "resolve")
    os.makedirs(d, exist_ok=True)
    PTR, BASE, OFF = 0x100, 0x2000, 0xC18
    sweep = [0.0, 45.0, 90.0]
    rams = []
    for v in sweep:
        ram = Ram(0x4000)
        ram.pu32(PTR, BASE)
        ram.pf32(BASE + OFF, v)
        rams.append(ram)
    names = write_dumps(d, rams, prefix="res")
    r = run_scanner(["resolve", "ptr=0x100", "off=0xC18"] + names, d,
                    label="resolve constant base")
    check("constant base detected and reported",
          "base CONSTANT" in r.stdout and "0x2000" in r.stdout, r.stdout)
    rows = [ln for ln in r.stdout.splitlines() if ln.startswith("res-")]
    ok_vals = (len(rows) == 3 and
               all("{:.2f}".format(v) in rows[i]
                   for i, v in enumerate(sweep)))
    check("base+off column shows the planted sweep per dump "
          "(0.00/45.00/90.00)", ok_vals, r.stdout)

    # varying base -> NOT a stable anchor
    rams[1].pu32(PTR, BASE + 0x100)
    rams[1].save(os.path.join(d, names[1]))
    r = run_scanner(["resolve", "ptr=0x100", "off=0xC18"] + names, d,
                    label="resolve varying base")
    check("varying base reported as not stable", "base VARIES" in r.stdout,
          r.stdout)


def test_sibling_smoke(tmp):
    print("[sibling] cross-axis discriminator inside a struct window (smoke)")
    d = os.path.join(tmp, "sibling")
    os.makedirs(d, exist_ok=True)
    WIN_S, WIN_E = 0x1000, 0x1400
    TARGET = WIN_S + 0x18C          # 0x118C -- varies only when swept
    BIGVAL = WIN_S + 0x200          # varies but |v| > 720 -> filtered
    CONSTF = WIN_S + 0x100          # constant everywhere -> filtered

    def build(target_v, big_v):
        ram = Ram(0x2000)
        ram.pf32(TARGET, target_v)
        ram.pf32(BIGVAL, big_v)
        ram.pf32(CONSTF, 3.0)
        return ram

    holds, varys = [], []
    for i, v in enumerate([55.0, 55.0]):
        n = "hold-{}.bin".format(i)
        build(v, 5000.0).save(os.path.join(d, n))
        holds.append(n)
    for i, v in enumerate([10.0, 100.0, 300.0]):
        n = "vary-{}.bin".format(i)
        build(v, [5000.0, -4000.0, 8000.0][i]).save(os.path.join(d, n))
        varys.append(n)

    r = run_scanner(["sibling", "win=0x1000:0x1400", "near=0x188",
                     "hold=" + ",".join(holds), "vary=" + ",".join(varys)],
                    d, label="sibling")
    check("target field reported at EXACT absolute address 0x0000118c with "
          "window offset +0x018c",
          "0x0000118c" in r.stdout and "+0x018c" in r.stdout, r.stdout)
    check("  ... with hold/vary values in dump order",
          "hold[55.00 55.00]" in r.stdout and
          "vary[10.00 100.00 300.00]" in r.stdout, r.stdout)
    check("angle-range filter drops the large-magnitude mover (0x1200)",
          "0x00001200" not in r.stdout)
    check("everywhere-constant float dropped (0x1100)",
          "0x00001100" not in r.stdout)


# ---------------------------------------------------------------------------
def main():
    if not os.path.exists(SCANNER):
        print("cannot find hunt_scan.py next to this test: " + SCANNER)
        return 2
    pre = subprocess.run([sys.executable, "-c", "import numpy"],
                         capture_output=True, text=True)
    if pre.returncode != 0:
        print("hunt_scan.py needs numpy and this interpreter has none -- "
              "install numpy to run the self-test.")
        return 2

    tmp = tempfile.mkdtemp(prefix="hunt_scan_selftest_")
    print("hunt_scan self-test: synthetic dumps under {}".format(tmp))
    try:
        test_angle_primary(tmp)
        test_angle_canonical_protocol(tmp)
        seam_names, seam_got = test_angle_seam_cross(tmp)
        test_angle_pitch(tmp)
        test_angle_metadata_sidecars(tmp, seam_names, seam_got)
        test_angle_degenerate(tmp)
        test_angle_halfperiod_slop_bug(tmp)
        test_flag_primary(tmp)
        test_flag_degenerate(tmp)
        test_flag_group_order_footgun(tmp)
        test_resolve_smoke(tmp)
        test_sibling_smoke(tmp)
    finally:
        if FAILURES:
            print("(synthetic dumps kept for inspection: {})".format(tmp))
        else:
            shutil.rmtree(tmp, ignore_errors=True)

    if FAILURES:
        print("\nself-test: {} FAILURE(S) of {} checks".format(
            FAILURES, CHECKS))
        return 1
    print("\nself-test: ALL PASS -- {} checks".format(CHECKS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
