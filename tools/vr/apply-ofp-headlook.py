#!/usr/bin/env python3
"""Apply the OFP: Elite Tier-3 head-look profile to an xemu config toml.

Set-or-insert each [vr] key idempotently (creating the [vr] section if
needed), leaving every other key untouched. Run this ONLY while xemu is NOT
running — xemu rewrites the toml on exit and would clobber hand-set keys.

The addresses are the durable POINTER anchor pinned from the 18-dump F9
capture (docs/vr/00-mvp-plan.md §9b): a static pointer -> the camera struct,
yaw/pitch as offsets from it, gate absolute. `enable` is intentionally left to
the launcher (XEMU_VR=1 boot-test.sh).

Usage: apply-ofp-headlook.py [path-to-toml]   (default ~/xemu-assets/xemu-test.toml)
"""
import os, re, sys

PROFILE = {
    "headlook_enable":        "true",
    "headlook_base_mode":     "'pointer'",
    "headlook_base_ptr":      "'0x00079D64'",   # static ptr -> struct base
    "headlook_yaw_addr":      "'0xC18'",        # yaw  = base + 0xC18
    "headlook_pitch_addr":    "'0xC1C'",        # pitch = base + 0xC1C (yaw+4)
    "headlook_gate_addr":     "'0x007ADB90'",   # ADS gate (absolute)
    "headlook_gate_value":    "1",
    "headlook_sensitivity":   "-57.2958",       # rad->deg, negated
    "headlook_invert_pitch":  "false",          # flip if pitch is backwards
    "headlook_engine_period": "360.0",          # OFP degrees engine
}

def apply(path):
    lines = open(path).read().splitlines() if os.path.exists(path) else []

    # Locate the [vr] section body [start, end).
    vr_hdr = next((i for i, l in enumerate(lines)
                   if l.strip() == "[vr]"), None)
    if vr_hdr is None:
        if lines and lines[-1].strip():
            lines.append("")
        lines.append("[vr]")
        vr_hdr = len(lines) - 1
    body_start = vr_hdr + 1
    body_end = next((i for i in range(body_start, len(lines))
                     if lines[i].lstrip().startswith("[")), len(lines))

    for key, val in PROFILE.items():
        newline = f"{key} = {val}"
        pat = re.compile(rf"^\s*{re.escape(key)}\s*=")
        idx = next((i for i in range(body_start, body_end)
                    if pat.match(lines[i])), None)
        if idx is not None:
            lines[idx] = newline
        else:
            lines.insert(body_end, newline)
            body_end += 1

    open(path, "w").write("\n".join(lines) + "\n")

if __name__ == "__main__":
    path = sys.argv[1] if len(sys.argv) > 1 else \
        os.path.expanduser("~/xemu-assets/xemu-test.toml")
    apply(path)
    print(f"OFP head-look profile applied to {path}")
    for k, v in PROFILE.items():
        print(f"  {k} = {v}")
    print("Launch with:  XEMU_VR=1 ~/xemu-assets/boot-test.sh")
