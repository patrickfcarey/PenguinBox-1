#!/usr/bin/env python3
"""
scan_hunt.py — Find the radar-scan loop in Steel Battalion's .text by SHAPE.

We look for loops (backward branches) whose bodies read memory a BYTE at a
time (movzx/movsx/mov r8,[mem] / cmp/test byte) — especially with a scale-4
index (reading one channel of a 32-bit RGBA pixel) — and that advance a
pointer by the radar row stride 0x200 (=512=128px*4Bpp), or reference the
frame constants 128 / 0x4000(=128*128) / 512.

Strategy:
  1. Linear-sweep disassemble with capstone, resyncing past bad bytes.
  2. Collect backward-branch loop bodies [target, branch].
  3. Score each body on byte-loads, scale-4, 0x200 stride, radar constants,
     and nesting (a body that contains a smaller loop body = outer of a
     nested scan).  Rank + dump the top candidates with disassembly.
"""
import sys, struct
from capstone import *
from capstone.x86 import *

XBE = "default.xbe"

# (name, raw_off, raw_size, vaddr) for exec sections we care about.
# .text is the primary; D3D is the statically-linked D3D8 (LockRect lives here).
SECTIONS = [
    (".text", 0x2000,   0x1fa1cc, 0x12000),
    ("D3D",   0x1fd000, 0xede0,   0x20c1e0),
]

RADAR_CONSTS = {0x200, 512, 0x80, 128, 0x4000, 0x1fc, 0x1fe, 0x204, 0x208,
                0x2000, 0x100, 256}
STRIDE_CONSTS = {0x200, 512, 0x1fc, 0x1fe, 0x204, 0x208}

def load_section(data, raw_off, raw_size):
    return data[raw_off:raw_off+raw_size]

def disasm_all(code, vaddr):
    """Linear sweep with resync. Returns list of dicts."""
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    out = []
    off = 0
    n = len(code)
    while off < n:
        got = False
        for insn in md.disasm(code[off:off+16], vaddr + off):
            out.append(insn)
            off += insn.size
            got = True
            break  # one at a time so we keep control of resync
        if not got:
            off += 1  # bad byte, skip
    return out

def is_byte_load(insn):
    """True if insn reads a byte from memory. Returns (True, scale, has_index)."""
    for op in insn.operands:
        if op.type == X86_OP_MEM and op.size == 1:
            # source read? for movzx/movsx/cmp/test/mov r8 the mem is a read
            m = insn.mnemonic
            if m in ("movzx", "movsx", "cmp", "test", "mov", "movsxd",
                     "or", "and", "add", "xor", "sub"):
                return True, op.mem.scale, (op.mem.index != 0)
    return False, 0, False

def imm_and_disp_consts(insn):
    vals = set()
    for op in insn.operands:
        if op.type == X86_OP_IMM:
            vals.add(op.imm & 0xffffffff)
        elif op.type == X86_OP_MEM:
            if op.mem.disp:
                vals.add(op.mem.disp & 0xffffffff)
    return vals

def main():
    with open(XBE, "rb") as f:
        data = f.read()

    all_insns = []
    idx_by_addr = {}
    for name, raw_off, raw_size, vaddr in SECTIONS:
        code = load_section(data, raw_off, raw_size)
        sys.stderr.write(f"disasm {name} ({raw_size} bytes @ 0x{vaddr:x})...\n")
        ins = disasm_all(code, vaddr)
        for i in ins:
            i._sec = name
        all_insns.extend(ins)
    all_insns.sort(key=lambda i: i.address)
    for k, i in enumerate(all_insns):
        idx_by_addr[i.address] = k
    sys.stderr.write(f"total instructions: {len(all_insns)}\n")

    # index address -> position for backward-branch body extraction
    addr_list = [i.address for i in all_insns]
    import bisect

    # find backward branches -> loop bodies
    JCC = set(["jmp","je","jne","jz","jnz","jg","jge","jl","jle","ja","jae",
               "jb","jbe","js","jns","jo","jno","jp","jnp","jecxz","loop",
               "loope","loopne","jc","jnc","jcxz"])
    bodies = []  # (start_addr, end_addr, branch_insn)
    for i in all_insns:
        if i.mnemonic in JCC and len(i.operands) == 1 and i.operands[0].type == X86_OP_IMM:
            tgt = i.operands[0].imm & 0xffffffff
            if tgt < i.address and (i.address - tgt) < 8192 and tgt >= addr_list[0]:
                bodies.append((tgt, i.address, i))

    # precompute per-instruction features
    feat = []
    for i in all_insns:
        bl, scale, hasidx = is_byte_load(i)
        consts = imm_and_disp_consts(i)
        feat.append((bl, scale, hasidx, consts))

    def body_score(start, end):
        lo = bisect.bisect_left(addr_list, start)
        hi = bisect.bisect_right(addr_list, end)
        byteloads = 0
        scale4 = 0
        stride = False
        radarc = set()
        for k in range(lo, hi):
            bl, scale, hasidx, consts = feat[k]
            if bl:
                byteloads += 1
                if scale == 4:
                    scale4 += 1
            if consts & STRIDE_CONSTS:
                stride = True
            radarc |= (consts & RADAR_CONSTS)
        score = 0
        score += min(byteloads, 8) * 2
        score += min(scale4, 6) * 3
        score += 6 if stride else 0
        score += 2 * len(radarc & {0x80,128,0x4000})
        return score, byteloads, scale4, stride, radarc, lo, hi

    scored = []
    for (start, end, br) in bodies:
        s, bloads, s4, stride, radarc, lo, hi = body_score(start, end)
        if bloads == 0:
            continue
        # nesting bonus: does another (smaller) body sit fully inside?
        nested = any(start <= b[0] and b[1] < end and (b[1]-b[0]) < (end-start)
                     for b in bodies if not (b[0]==start and b[1]==end))
        if nested:
            s += 5
        scored.append((s, start, end, bloads, s4, stride, radarc, nested, hi-lo))

    scored.sort(reverse=True)
    # de-dup overlapping bodies: keep highest-scoring per ~region
    seen_regions = []
    printed = 0
    print("="*80)
    print("TOP LOOP CANDIDATES (score, [start-end], byteloads, scale4, stride0x200, consts, nested)")
    print("="*80)
    for entry in scored:
        s, start, end, bloads, s4, stride, radarc, nested, ninsn = entry
        # skip if within 64 bytes of an already-printed start
        if any(abs(start - r) < 64 for r in seen_regions):
            continue
        seen_regions.append(start)
        cs = ",".join(hex(c) for c in sorted(radarc))
        print(f"\n#{printed}  score={s}  loop[0x{start:08x}..0x{end:08x}]  "
              f"insns={ninsn} byteloads={bloads} scale4={s4} stride0x200={stride} nested={nested}")
        print(f"     radar-consts seen: {cs}")
        printed += 1
        if printed >= 25:
            break
    print(f"\n(total scored loop bodies with >=1 byteload: {len(scored)})")

if __name__ == "__main__":
    main()
