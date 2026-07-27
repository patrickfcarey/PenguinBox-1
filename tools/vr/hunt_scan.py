#!/usr/bin/env python3
# xemu-VR Tier-3 hunt scanner: find camera-angle floats in guest-RAM dumps.
#
# SPDX-FileCopyrightText: 2026 Patrick Carey
# SPDX-License-Identifier: LGPL-2.0-or-later
#
# Protocol (see TEST_QUEUE / plan §9): in-game, aim the CAMERA (right stick)
# at scripted headings, pressing F9 at each. Then:
#
#   hunt_scan.py yaw   dump-000.bin=0 dump-001.bin=90 dump-002.bin=180 \
#                      dump-003.bin=270 dump-004.bin=0
#   hunt_scan.py pitch dump-005.bin=0 dump-006.bin=60 dump-007.bin=-60
#
# The value after '=' is the scripted angle in DEGREES. Repeating an angle
# (e.g. ...=0 twice) is highly recommended — it filters out everything that
# drifts on its own (timers, positions, particles).
#
# Finds 4-aligned float32 addresses whose pairwise deltas are proportional
# to the scripted deltas (testing radians, degrees, and negated variants,
# with wrap handling), and near-equal where the script repeats an angle.
# Prints candidates as PHYSICAL addresses ready for [vr] headlook_*_addr.

import struct
import sys
import numpy as np

TOL_FRAC = 0.15        # fractional tolerance on delta matching
STABLE_TOL = 0.02      # radians: max |delta| where script says "same"
SCALES = [("rad", 1.0), ("deg", 57.29577951308232),
          ("-rad", -1.0), ("-deg", -57.29577951308232)]


def load(path):
    a = np.fromfile(path, dtype="<f4")
    return a


def wrap_diff(a, period):
    return (a + period / 2.0) % period - period / 2.0


def suggest_mask(on_vals, off_vals):
    """Find (mask, value) with (v & mask) == value for all ON values and
    != for all OFF values. Prefers the most specific mask."""
    for mask in sorted(range(1, 256), key=lambda m: -bin(m).count("1")):
        v0 = on_vals[0] & mask
        if all((v & mask) == v0 for v in on_vals) and \
           all((o & mask) != v0 for o in off_vals):
            return mask, v0
    return None


def flag_mode(args):
    """hunt_scan.py flag on=ads.bin on=adszoom.bin off=hip.bin \
                         off=zoom.bin off=thirdperson.bin
    Byte-wise gate scan. ON group = states where head-look must fire
    (ADS, ADS+zoom); OFF group = every state where it must not (hip,
    plain zoom, third person). Handles enums: a byte qualifies when its
    ON-value set and OFF-value set are DISJOINT (values within a group
    may differ, e.g. view_mode 2=ads / 3=ads+zoom). Prints a suggested
    headlook_gate_mask/value per candidate."""
    groups = {}
    for arg in args:
        name, _, path = arg.partition("=")
        groups.setdefault(name, []).append(
            np.fromfile(path, dtype=np.uint8))
    if len(groups) != 2:
        sys.exit("flag mode needs exactly two group names (on=, off=)")
    # Key off the group NAMES, not CLI order: `flag off=... on=...` must
    # still gate ON in the on-states (self-test FOOTGUN-2). The first
    # group is the one the suggested mask/value should MATCH; prefer the
    # name 'on' when present, else keep given order.
    names = list(groups)
    if "on" in names and names[0] != "on":
        names.reverse()
    (na, da), (nb, db) = ((n, groups[n]) for n in names)
    size = min(min(len(x) for x in da), min(len(x) for x in db))
    a = np.stack([x[:size] for x in da])   # ON rows
    b = np.stack([x[:size] for x in db])   # OFF rows

    disjoint = np.ones(size, dtype=bool)
    for i in range(a.shape[0]):
        for j in range(b.shape[0]):
            disjoint &= a[i] != b[j]
    hits = np.nonzero(disjoint)[0]

    if not len(hits):
        print("[flag] no candidates — dumps too far apart in time, or the "
              "flag is wider than a byte.")
        return
    print(f"[flag] {len(hits)} candidate byte(s):")
    for i in hits[:50]:
        on_vals = sorted(set(int(v) for v in a[:, i]))
        off_vals = sorted(set(int(v) for v in b[:, i]))
        s = suggest_mask(on_vals, off_vals)
        sug = (f"gate_mask={s[0]} gate_value={s[1]}" if s
               else "no single mask/value — chain-gate needed")
        print(f"  0x{int(i):08X}  {na}={on_vals} {nb}={off_vals}  -> {sug}")
    if len(hits) > 50:
        print(f"  ... {len(hits) - 50} more. Add one more dump per state "
              f"(minimal time/motion between dumps) and re-run to narrow.")
    print("\nPick a candidate (prefer small enum-like values), set "
          "[vr] headlook_gate_addr='0x...', headlook_gate_mask and "
          "headlook_gate_value from the suggestion.")


def resolve_mode(args):
    """hunt_scan.py resolve ptr=0x79D64 [base_off=0] off=0xC18 off=0xC1C dump-*.bin

    The DECISIVE durability check — run it the moment a pointer candidate
    appears, before capturing anything new. Reads u32 at `ptr` in every dump
    (base = that + base_off), then the float at base+off for each `off`. If the
    base is CONSTANT across all dumps AND base+off tracks the known sweep, the
    pointer is session-stable: the durable-anchor candidate. (Only cross-BOOT
    stability then needs a reboot — but that necessary session half is fully
    checkable from the single-session dumps you already have.) This is exactly
    the check that would have confirmed OFP's 0x00079D64 -> struct anchor and
    pinned pitch days sooner than a fresh gdbstub capture. See RESEARCH.md §2."""
    ptr = None
    base_off = 0
    offs = []
    dumps = []
    for a in args:
        k, sep, v = a.partition("=")
        if not sep:
            dumps.append(a)
        elif k == "ptr":
            ptr = int(v, 0)
        elif k == "base_off":
            base_off = int(v, 0)
        elif k == "off":
            offs.append(int(v, 0))
        else:
            sys.exit(f"resolve: unknown key '{k}'")
    if ptr is None or not dumps:
        sys.exit("usage: resolve ptr=0x.. [base_off=0] off=0x.. [off=..] dump ...")
    if not offs:
        offs = [0]
    RAM = 64 * 1024 * 1024
    print("dump".ljust(20) + "base".rjust(12) +
          "".join(f"+{o:#x}".rjust(13) for o in offs))
    bases = []
    for d in dumps:
        with open(d, "rb") as f:
            f.seek(ptr)
            base = struct.unpack("<I", f.read(4))[0] + base_off
            vals = []
            for o in offs:
                a = base + o
                if 0 <= a and a + 4 <= RAM:
                    f.seek(a)
                    b = f.read(4)
                    vals.append(struct.unpack("<f", b)[0]
                                if len(b) == 4 else float("nan"))
                else:
                    vals.append(float("nan"))
        bases.append(base)
        name = d.rsplit("/", 1)[-1]
        print(name.ljust(20) + f"{base:#010x}".rjust(12) +
              "".join(f"{v:13.2f}" for v in vals))
    uniq = set(bases)
    if len(uniq) == 1:
        print(f"\nbase CONSTANT across {len(dumps)} dumps ({bases[0]:#x}) "
              f"-> session-stable pointer. Confirm cross-boot with a reboot.")
    else:
        print(f"\nbase VARIES ({', '.join(hex(b) for b in sorted(uniq))}) "
              f"-> not a stable anchor at this ptr/offset.")


def sibling_mode(args):
    """hunt_scan.py sibling win=0x3A70388:0x3A71400 hold=a.bin,b.bin vary=c.bin,d.bin

    Find a struct's OTHER angle fields once one axis + the struct base are
    known. Scans floats in the window [START:END) for an offset that stays
    ~CONSTANT across the `hold` dumps (the axis you held fixed) but VARIES
    across the `vary` dumps (the axis you swept) — the cross-axis discriminator.
    Isolates e.g. pitch INSIDE the struct after yaw is known, avoiding the
    plausible-but-wrong hits of a full-RAM scan. (This pinned OFP pitch at
    base+0xC1C, correcting a bogus full-RAM hit.) See RESEARCH.md §2."""
    win = None
    hold = []
    vary = []
    near = None
    for a in args:
        k, _, v = a.partition("=")
        if k == "win":
            s, _, e = v.partition(":")
            win = (int(s, 0), int(e, 0))
        elif k == "hold":
            hold = [p for p in v.split(",") if p]
        elif k == "vary":
            vary = [p for p in v.split(",") if p]
        elif k == "near":
            near = int(v, 0)          # a known axis offset; rank by adjacency
    if not win or not hold or not vary:
        sys.exit("usage: sibling win=START:END hold=a,b,.. vary=c,d,.. "
                 "[near=0xC18]")
    start, end = win

    def loadwin(d):
        with open(d, "rb") as f:
            f.seek(start)
            buf = f.read(end - start)
        a = np.frombuffer(buf, dtype="<f4").copy()
        a[~np.isfinite(a)] = 1e30
        return a

    hs = np.stack([loadwin(d) for d in hold])
    vs = np.stack([loadwin(d) for d in vary])
    hspan = hs.max(0) - hs.min(0)
    vspan = vs.max(0) - vs.min(0)
    # Keep only plausible ANGLE-range floats — else large-magnitude junk
    # (positions, counters) with a big raw span drowns the real field.
    ANGLE_MAX = 720.0
    inrange = (np.abs(hs) < ANGLE_MAX).all(0) & (np.abs(vs) < ANGLE_MAX).all(0)
    # varies >=5x more when swept than when held, and above a noise floor
    good = inrange & (hspan < 0.2 * vspan) & (vspan > 0.5)
    idx = np.nonzero(good)[0]
    if not len(idx):
        print("[sibling] no candidate — widen the window, or the field isn't a "
              "plain float in it (matrix/quat, or stored elsewhere).")
        return
    if near is not None:
        # Euler angles cluster — rank by adjacency to a known axis offset
        # (offsets are window-relative, matching the printed +0x.. column).
        order = sorted(idx, key=lambda i: (abs(i * 4 - near), -vspan[i]))
    else:
        order = sorted(idx, key=lambda i: -vspan[i])
    print(f"[sibling] {len(idx)} candidate(s) in [{start:#x},{end:#x}):")
    for i in order[:30]:
        addr = start + i * 4
        hv = " ".join(f"{v:.2f}" for v in hs[:, i])
        vv = " ".join(f"{v:.2f}" for v in vs[:, i])
        print(f"  {addr:#010x}  +{addr-start:#06x}  hold[{hv}]  vary[{vv}]")
    print("\nThe field with the cleanest vary-sweep shape is the sibling axis; "
          "its offset from the struct base is the [vr] headlook_*_addr offset.")


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__ or "usage: hunt_scan.py <label> dump=deg ...")
    label = sys.argv[1]
    if label == "flag":
        flag_mode(sys.argv[2:])
        return
    if label == "resolve":
        resolve_mode(sys.argv[2:])
        return
    if label == "sibling":
        sibling_mode(sys.argv[2:])
        return
    if len(sys.argv) < 4:
        sys.exit(__doc__ or "usage: hunt_scan.py <label> dump=deg ...")
    dumps, angles = [], []
    for arg in sys.argv[2:]:
        path, _, deg = arg.rpartition("=")
        dumps.append(load(path))
        angles.append(np.deg2rad(float(deg)))
    n = len(dumps)
    size = min(len(d) for d in dumps)
    data = np.stack([d[:size] for d in dumps])          # [n, floats]
    ang = np.array(angles)                              # radians

    finite = np.isfinite(data).all(axis=0)
    inrange = (np.abs(data) < 1e6).all(axis=0)
    cand = finite & inrange

    results = {}
    for name, scale in SCALES:
        period = 2.0 * np.pi * abs(scale)
        ok = cand.copy()
        for i in range(1, n):
            expect = wrap_diff((ang[i] - ang[0]) * scale, period)
            got = data[i] - data[0]
            gotw = wrap_diff(got, period)
            if abs(expect) < STABLE_TOL * abs(scale):
                ok &= np.abs(gotw) <= STABLE_TOL * abs(scale)
            else:
                tol = max(abs(expect) * TOL_FRAC,
                          STABLE_TOL * abs(scale))
                # Re-wrap the residual: at a half-period step (the yaw
                # protocol's 180) expect lands on -period/2 while a reading
                # a hair under lands wrapped to +period/2 — the raw
                # difference is ~period despite the value being dead on.
                # Without this, real captures reading slightly under 180
                # silently reject the true candidate (self-test BUG-1).
                ok &= np.abs(wrap_diff(gotw - expect, period)) <= tol
        idx = np.nonzero(ok)[0]
        for i in idx:
            results.setdefault(int(i), name)

    if not results:
        print(f"[{label}] no candidates — re-check the scripted angles, or "
              f"the value may not be a plain float (matrix/quat engine).")
        return

    print(f"[{label}] {len(results)} candidate(s):")
    for i in sorted(results):
        addr = i * 4
        vals = ", ".join(f"{v:+.4f}" for v in data[:, i])
        print(f"  0x{addr:08X}  ({results[i]:>4})  values: {vals}")
    print("\nPick the candidate whose values track the script exactly; "
          "set [vr] headlook_yaw_addr / headlook_pitch_addr = '0x...' and "
          "flip headlook_enable = true (sensitivity 1.0 for rad engines, "
          "57.2958 for deg engines; negate via invert/negative sens).")


if __name__ == "__main__":
    main()
