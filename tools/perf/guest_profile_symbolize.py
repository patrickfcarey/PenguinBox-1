#!/usr/bin/env python3
"""
guest_profile_symbolize.py — turn raw guest-PC profiler samples into the ranked
31 ms breakdown that picks the Phase-1 fleet.

The in-emulator sampler (accel/tcg/xemu-guest-profile.c, armed by
XEMU_GUEST_PROFILE) dumps a flat little-endian array of 8-byte records:

    struct { uint32_t eip; uint32_t idx; }   # idx is a monotonic 1-based index

This tool reads that file, bins each EIP to the nearest preceding function in
the game's XBE (function starts are harvested from direct CALL targets via
capstone), and prints:

  * a coarse ROLE breakdown — D3D8 driver vs radar vs lock-on vs game/other
    .text vs kernel/HLE — the headline "where does the guest-CPU time go",
  * a ranked TOP FUNCTIONS histogram (% of samples, with landmarks named),
  * the TOP INDIVIDUAL EIPs, and
  * a SPIN-CLUSTER report: runs of near-identical consecutive EIPs, and the
    spin fraction (the single highest-value number for Phase 1 · H-A).

Usage:
    guest_profile_symbolize.py <samples.bin> --xbe default.xbe [options]

Options:
    --xbe PATH            the game's default.xbe (default: ./default.xbe)
    --top N               how many functions / eips to list (default 30)
    --scan-sections LIST  comma-separated sections to disassemble for call
                          targets (default ".text,D3D"; "all" = every exec sec)
    --spin-window BYTES   max EIP span of a spin run (default 256)
    --spin-min-run N      min consecutive samples to call it a spin (default 16)
    --eip-only            input is 4-byte {u32 eip} records (no idx)

NOTE ON SCOPE.  These are *guest* PC samples, so this measures where guest code
executes.  Host-side emulator overhead (TB re-translation, MMIO-trap
emulation) is not a guest EIP and is invisible here by construction — those
need separate host-side timers.  What this settles: game logic vs the XDK D3D8
driver vs the radar vs spin waits.
"""

import sys
import os
import struct
import argparse
import bisect
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xbe_tool  # noqa: E402  (local module, path set above)

try:
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32
    HAVE_CAPSTONE = True
except ImportError:
    HAVE_CAPSTONE = False

# Known guest virtual addresses from the Steel Battalion campaign
# (docs/sb-rendering-anatomy.md, docs/perf/scan-pc-capture.patch).
LANDMARKS = {
    0x25d40: "radar_scan_render",
    0x26460: "radar_scan_2",
    0x26b80: "radar_scan_3",
    0x27250: "radar_scan_4",
    0x21dd0: "lock_on",
}
RADAR_LANDMARKS = {a for a, n in LANDMARKS.items() if n.startswith("radar")}

XBOX_KERNEL_BASE = 0x80000000  # guest EIPs at/above here are the Xbox kernel


# ----------------------------------------------------------------------------
# input
# ----------------------------------------------------------------------------
def load_samples(path, eip_only):
    with open(path, "rb") as f:
        blob = f.read()
    rec = 4 if eip_only else 8
    n = len(blob) // rec
    if n == 0:
        raise SystemExit(f"{path}: no samples ({len(blob)} bytes, "
                         f"record size {rec})")
    if len(blob) % rec:
        sys.stderr.write(f"warning: {len(blob) % rec} trailing bytes ignored "
                         f"(partial record)\n")
    if eip_only:
        eips = struct.unpack_from(f"<{n}I", blob, 0)
        samples = [(e, i + 1) for i, e in enumerate(eips)]
    else:
        flat = struct.unpack_from(f"<{2 * n}I", blob, 0)
        samples = [(flat[2 * i], flat[2 * i + 1]) for i in range(n)]
        # Chronological order for spin-run analysis (the dumper already writes
        # oldest->newest, but sort by idx defensively; skip empty idx==0 slots).
        samples = [s for s in samples if s[1] != 0]
        samples.sort(key=lambda s: s[1])
    return samples


# ----------------------------------------------------------------------------
# XBE geometry + function table
# ----------------------------------------------------------------------------
class Image:
    def __init__(self, xbe_path, scan_sections):
        x = xbe_tool.read_xbe(xbe_path)
        self.data = x["data"]
        self.base = x["base"]
        self.title = x["title_name"]
        self.title_id = x["title_id"]
        self.image_end = x["base"] + x["sizeof_image"]
        self.sections = x["sections"]  # (name, vaddr, vsize, raddr, rsize, flags)

        # section lookup by ascending vaddr
        self._sec_sorted = sorted(self.sections, key=lambda s: s[1])
        self._sec_starts = [s[1] for s in self._sec_sorted]

        self.exec_sections = [s for s in self.sections if (s[5] & 4)]
        self.text = self._named(".text")
        self.d3d = self._named("D3D")

        self.func_starts, self.func_name = self._build_functions(scan_sections)

    def _named(self, name):
        for s in self.sections:
            if s[0] == name:
                return s
        return None

    def section_of(self, eip):
        i = bisect.bisect_right(self._sec_starts, eip) - 1
        if i < 0:
            return None
        nm, vaddr, vsize, _, _, _ = self._sec_sorted[i]
        return nm if vaddr <= eip < vaddr + vsize else None

    def _read_section_bytes(self, sec):
        _, vaddr, vsize, raddr, rsize, _ = sec
        return self.data[raddr:raddr + rsize], vaddr

    def _build_functions(self, scan_sections):
        """Function starts = direct CALL targets (harvested) + section starts +
        landmarks.  Returns (sorted_starts, {start: name})."""
        exec_ranges = [(s[1], s[1] + s[2]) for s in self.exec_sections]

        def in_exec(addr):
            for lo, hi in exec_ranges:
                if lo <= addr < hi:
                    return True
            return False

        targets = set()
        if HAVE_CAPSTONE:
            if scan_sections == "all":
                to_scan = self.exec_sections
            else:
                want = {n.strip() for n in scan_sections.split(",") if n.strip()}
                to_scan = [s for s in self.exec_sections if s[0] in want]
            for sec in to_scan:
                code, vaddr = self._read_section_bytes(sec)
                targets |= _harvest_call_targets(code, vaddr, in_exec)
        else:
            sys.stderr.write("warning: capstone not available — function table "
                             "limited to section starts + landmarks\n")

        starts = set(targets)
        names = {}
        for s in self.sections:
            starts.add(s[1])
            names[s[1]] = s[0]           # section start -> section name
        for a, nm in LANDMARKS.items():
            starts.add(a)
            names[a] = nm                 # landmarks win the name
        # keep only addresses within the loaded image
        starts = {a for a in starts if self.base <= a < self.image_end}
        return sorted(starts), names

    def func_bin(self, eip):
        """Greatest function start <= eip (or None)."""
        i = bisect.bisect_right(self.func_starts, eip) - 1
        return self.func_starts[i] if i >= 0 else None

    def func_label(self, start):
        if start is None:
            return "?"
        return self.func_name.get(start, "sub_%06x" % start)

    def role_of(self, eip):
        sec = self.section_of(eip)
        if sec is None:
            return "kernel/HLE" if eip >= XBOX_KERNEL_BASE else "off-image"
        if sec == "D3D":
            return "D3D8 driver"
        if sec == ".text":
            fb = self.func_bin(eip)
            if fb in RADAR_LANDMARKS:
                return "radar scan"
            if fb is not None and self.func_name.get(fb) == "lock_on":
                return "lock-on"
            return "game/other .text"
        return "other-exec: %s" % sec


def _harvest_call_targets(code, vaddr, in_exec):
    """Linear-sweep disassemble @code (loaded at @vaddr), collecting the
    targets of direct near calls that land in executable space.  Resyncs past
    data islands one byte at a time (the tools/perf house pattern)."""
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    targets = set()
    n = len(code)
    off = 0
    while off < n:
        progressed = False
        for (address, size, mnemonic, op_str) in md.disasm_lite(code[off:],
                                                                 vaddr + off):
            progressed = True
            if mnemonic == "call" and op_str[:2] == "0x":
                try:
                    t = int(op_str, 16)
                except ValueError:
                    t = None
                if t is not None and in_exec(t):
                    targets.add(t)
            off = address - vaddr + size
        if not progressed:
            off += 1
    return targets


# ----------------------------------------------------------------------------
# spin detection
# ----------------------------------------------------------------------------
def find_spin_clusters(samples, window, min_run):
    """A spin cluster is a maximal run of consecutive (idx-contiguous) samples
    whose EIP span stays within @window bytes.  A tight poll loop keeps that
    span tiny for a long run; varied hot code blows past @window immediately."""
    clusters = []
    n = len(samples)
    i = 0
    total_spin = 0
    while i < n:
        lo = hi = samples[i][0]
        j = i
        while j + 1 < n:
            e_eip, e_idx = samples[j + 1]
            if e_idx != samples[j][1] + 1:      # ring-wrap / gap: break the run
                break
            nlo, nhi = min(lo, e_eip), max(hi, e_eip)
            if nhi - nlo > window:
                break
            lo, hi = nlo, nhi
            j += 1
        run = j - i + 1
        if run >= min_run:
            eips = Counter(s[0] for s in samples[i:j + 1])
            clusters.append({
                "count": run, "lo": lo, "hi": hi,
                "mode_eip": eips.most_common(1)[0][0],
                "distinct": len(eips),
            })
            total_spin += run
        i = j + 1
    clusters.sort(key=lambda c: -c["count"])
    return clusters, total_spin


# ----------------------------------------------------------------------------
# report
# ----------------------------------------------------------------------------
def pct(x, total):
    return 100.0 * x / total if total else 0.0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("samples")
    ap.add_argument("--xbe", default="default.xbe")
    ap.add_argument("--top", type=int, default=30)
    ap.add_argument("--scan-sections", default=".text,D3D")
    ap.add_argument("--spin-window", type=int, default=256)
    ap.add_argument("--spin-min-run", type=int, default=16)
    ap.add_argument("--eip-only", action="store_true")
    args = ap.parse_args()

    samples = load_samples(args.samples, args.eip_only)
    img = Image(args.xbe, args.scan_sections)
    total = len(samples)

    # --- bin ---------------------------------------------------------------
    role_counts = Counter()
    func_counts = Counter()
    eip_counts = Counter()
    for eip, _ in samples:
        role_counts[img.role_of(eip)] += 1
        func_counts[img.func_bin(eip)] += 1
        eip_counts[eip] += 1

    clusters, total_spin = find_spin_clusters(samples, args.spin_window,
                                              args.spin_min_run)

    # --- header ------------------------------------------------------------
    text_hi = img.text[1] + img.text[2] if img.text else 0
    d3d_hi = img.d3d[1] + img.d3d[2] if img.d3d else 0
    print("=" * 72)
    print(f"guest-PC profile: {args.samples}")
    print(f"  samples          : {total}")
    print(f"  distinct EIPs    : {len(eip_counts)}")
    print(f"  XBE              : {img.title!r} (0x{img.title_id:08x})")
    print(f"  image            : base 0x{img.base:x} .. 0x{img.image_end:x}")
    if img.text:
        print(f"  .text (game)     : [0x{img.text[1]:x} .. 0x{text_hi:x})")
    if img.d3d:
        print(f"  D3D  (D3D8 drv)  : [0x{img.d3d[1]:x} .. 0x{d3d_hi:x})")
    print(f"  functions found  : {len(img.func_starts)}"
          f"  (capstone: {'yes' if HAVE_CAPSTONE else 'NO'})")

    # --- role breakdown ----------------------------------------------------
    print("\n" + "=" * 72)
    print("ROLE BREAKDOWN  (coarse — the headline 'where does the guest go')")
    print(f"  {'role':<24}{'samples':>12}{'%':>8}")
    role_order = ["D3D8 driver", "radar scan", "lock-on", "game/other .text"]
    seen = set()
    for role in role_order:
        if role in role_counts:
            print(f"  {role:<24}{role_counts[role]:>12}"
                  f"{pct(role_counts[role], total):>7.2f}%")
            seen.add(role)
    for role, c in role_counts.most_common():
        if role in seen:
            continue
        print(f"  {role:<24}{c:>12}{pct(c, total):>7.2f}%")

    # --- top functions -----------------------------------------------------
    print("\n" + "=" * 72)
    print(f"TOP {args.top} FUNCTIONS  (nearest preceding function start)")
    print(f"  {'#':>3} {'samples':>10} {'%':>7}  {'addr':<11}{'region':<14}name")
    for rank, (start, c) in enumerate(func_counts.most_common(args.top), 1):
        if start is None:
            addr, region, label = 0, "?", "<below first function>"
        else:
            addr = start
            region = img.section_of(start) or (
                "kernel/HLE" if start >= XBOX_KERNEL_BASE else "off-image")
            label = img.func_label(start)
        star = " *" if start in LANDMARKS else ""
        print(f"  {rank:>3} {c:>10} {pct(c, total):>6.2f}%  "
              f"0x{addr:08x} {region:<14}{label}{star}")

    # --- top individual eips ----------------------------------------------
    print("\n" + "=" * 72)
    print(f"TOP {args.top} INDIVIDUAL EIPs  (exact addresses — spikes = spins)")
    print(f"  {'#':>3} {'samples':>10} {'%':>7}  {'eip':<11}{'region':<14}"
          f"in-function")
    for rank, (eip, c) in enumerate(eip_counts.most_common(args.top), 1):
        region = img.section_of(eip) or (
            "kernel/HLE" if eip >= XBOX_KERNEL_BASE else "off-image")
        fb = img.func_bin(eip)
        off = (eip - fb) if fb is not None else 0
        label = img.func_label(fb)
        print(f"  {rank:>3} {c:>10} {pct(c, total):>6.2f}%  "
              f"0x{eip:08x} {region:<14}{label}+0x{off:x}")

    # --- spin report -------------------------------------------------------
    print("\n" + "=" * 72)
    print("SPIN CLUSTERS  (near-identical consecutive EIPs — busy-wait waste)")
    print(f"  spin fraction    : {pct(total_spin, total):.2f}%  "
          f"({total_spin} / {total} samples in {len(clusters)} runs)")
    print(f"  detector         : span <= {args.spin_window} B, "
          f"run >= {args.spin_min_run} samples")
    if clusters:
        # merge clusters that sit on the same loop (same mode EIP)
        by_site = Counter()
        site_span = {}
        for c in clusters:
            by_site[c["mode_eip"]] += c["count"]
            lohi = site_span.get(c["mode_eip"], (c["lo"], c["hi"]))
            site_span[c["mode_eip"]] = (min(lohi[0], c["lo"]),
                                        max(lohi[1], c["hi"]))
        print(f"\n  {'samples':>10} {'%':>7}  {'eip-range':<23}"
              f"{'region':<12}in-function")
        for eip, c in by_site.most_common(args.top):
            lo, hi = site_span[eip]
            region = img.section_of(eip) or (
                "kernel/HLE" if eip >= XBOX_KERNEL_BASE else "off-image")
            fb = img.func_bin(eip)
            off = (eip - fb) if fb is not None else 0
            rng = f"0x{lo:08x}-0x{hi:08x}"
            print(f"  {c:>10} {pct(c, total):>6.2f}%  {rng:<23}"
                  f"{region:<12}{img.func_label(fb)}+0x{off:x}")
    else:
        print("  (none detected — try a larger --spin-window / smaller "
              "--spin-min-run)")
    print("=" * 72)


if __name__ == "__main__":
    main()
