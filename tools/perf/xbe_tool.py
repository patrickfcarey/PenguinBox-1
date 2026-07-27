#!/usr/bin/env python3
"""
xbe_tool.py — Extract default.xbe from an Xbox XDVDFS (.xiso.iso) and parse it.

Usage:
  xbe_tool.py extract <iso> <out_dir>      # writes default.xbe into out_dir
  xbe_tool.py parse   <default.xbe>        # prints headers, sections, cert
  xbe_tool.py all     <iso> <out_dir>      # extract then parse

XDVDFS notes: sector size 2048. Volume descriptor at sector 32 (0x10000):
  0x00 char[20] "MICROSOFT*XBOX*MEDIA"
  0x14 u32 root dir table sector
  0x18 u32 root dir table size (bytes)
Directory entry (inside dir table):
  0x00 u16 left  (dword offset within THIS table; 0 or 0xFFFF = none)
  0x02 u16 right
  0x04 u32 start sector
  0x08 u32 size
  0x0C u8  attrs
  0x0D u8  name len
  0x0E name (ascii), padded to 4-byte boundary
"""
import sys, struct, os

SECTOR = 2048

def find_base_offset(f):
    # plain xiso: magic at 0x10000. redump: +0x18300000. Check both.
    for base in (0, 0x18300000, 0x18310000, 0x30600000):
        f.seek(base + 32 * SECTOR)
        if f.read(20) == b"MICROSOFT*XBOX*MEDIA":
            return base
    raise RuntimeError("XDVDFS magic not found at any known offset")

def read_dir_table(f, base, sector, size):
    f.seek(base + sector * SECTOR)
    return f.read(size)

def walk_dir(table):
    """Iteratively walk the directory btree; yield (name, start_sector, size, attrs)."""
    out = []
    stack = [0]
    seen = set()
    while stack:
        off = stack.pop()
        if off in seen:
            continue
        seen.add(off)
        if off + 14 > len(table):
            continue
        left, right, start, size, attrs, nlen = struct.unpack_from("<HHIIBB", table, off)
        # sentinel: unused padding entries are all 0xFF
        if left == 0xFFFF and right == 0xFFFF:
            continue
        name = table[off+14:off+14+nlen]
        try:
            name = name.decode("ascii")
        except UnicodeDecodeError:
            name = None
        if name is not None and nlen > 0:
            out.append((name, start, size, attrs))
        if left not in (0, 0xFFFF):
            stack.append(left * 4)
        if right not in (0, 0xFFFF):
            stack.append(right * 4)
    return out

def extract(iso, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    with open(iso, "rb") as f:
        base = find_base_offset(f)
        f.seek(base + 32 * SECTOR + 0x14)
        root_sector, root_size = struct.unpack("<II", f.read(8))
        sys.stderr.write(f"base_offset=0x{base:x} root_sector={root_sector} root_size={root_size}\n")
        table = read_dir_table(f, base, root_sector, root_size)
        entries = walk_dir(table)
        sys.stderr.write("root dir entries:\n")
        for name, start, size, attrs in sorted(entries):
            sys.stderr.write(f"  {name:32s} sector={start:8d} size={size:10d} attrs=0x{attrs:02x}\n")
        # find default.xbe (case-insensitive)
        target = None
        for name, start, size, attrs in entries:
            if name.lower() == "default.xbe":
                target = (name, start, size, attrs)
                break
        if not target:
            raise RuntimeError("default.xbe not found in root directory")
        name, start, size, attrs = target
        f.seek(base + start * SECTOR)
        data = f.read(size)
        out_path = os.path.join(out_dir, "default.xbe")
        with open(out_path, "wb") as o:
            o.write(data)
        sys.stderr.write(f"wrote {out_path} ({len(data)} bytes)\n")
        return out_path

def read_xbe(xbe_path):
    """Programmatic XBE accessor (no printing) for other tools to import.

    Returns a dict: data (raw bytes), base, sizeof_headers, sizeof_image,
    entry, entry_key ('retail'/'debug'/None), cert_size, title_id, title_name,
    allowed_media, game_region, version, and sections — a list of
    (name, vaddr, vsize, raddr, rsize, flags).  parse() below is the CLI
    pretty-printer; this is the same header math without the I/O.
    """
    with open(xbe_path, "rb") as f:
        d = f.read()
    if d[:4] != b"XBEH":
        raise RuntimeError("not an XBE (bad magic)")

    def u(off):
        return struct.unpack_from("<I", d, off)[0]

    base = u(0x104)
    sizeof_headers = u(0x108)
    sizeof_image = u(0x10C)
    entry_enc = u(0x128)
    cert_addr = u(0x118)
    num_sections = u(0x11C)
    sec_hdr_addr = u(0x120)

    def v2o(vaddr):
        return vaddr - base

    entry, entry_key = None, None
    for label, k in (("retail", 0xA8FC57AB), ("debug", 0x94859D4B)):
        ep = entry_enc ^ k
        if base <= ep < base + sizeof_image:
            entry, entry_key = ep, label
            break

    co = v2o(cert_addr)
    title_name = d[co+0x0C: co+0x0C+80].decode("utf-16-le", "replace").split("\x00")[0]

    so = v2o(sec_hdr_addr)
    SECHDR = 0x38
    sections = []
    for i in range(num_sections):
        eo = so + i * SECHDR
        flags = u(eo + 0x00)
        vaddr = u(eo + 0x04)
        vsize = u(eo + 0x08)
        raddr = u(eo + 0x0C)
        rsize = u(eo + 0x10)
        name_addr = u(eo + 0x14)
        no = v2o(name_addr)
        nm = d[no:no+16].split(b"\x00")[0].decode("ascii", "replace")
        sections.append((nm, vaddr, vsize, raddr, rsize, flags))

    return {
        "data": d,
        "base": base,
        "sizeof_headers": sizeof_headers,
        "sizeof_image": sizeof_image,
        "entry": entry,
        "entry_key": entry_key,
        "cert_size": u(co),
        "title_id": u(co + 0x08),
        "title_name": title_name,
        "allowed_media": u(co + 0x9C),
        "game_region": u(co + 0xA0),
        "version": struct.unpack_from("<i", d, co + 0xAC)[0],
        "sections": sections,
    }


def parse(xbe_path):
    with open(xbe_path, "rb") as f:
        d = f.read()
    if d[:4] != b"XBEH":
        raise RuntimeError("not an XBE (bad magic)")
    # header fields (offsets per xbe.htm / xemu-xbe.h)
    base = struct.unpack_from("<I", d, 0x104)[0]
    sizeof_headers = struct.unpack_from("<I", d, 0x108)[0]
    sizeof_image = struct.unpack_from("<I", d, 0x10C)[0]
    entry_enc = struct.unpack_from("<I", d, 0x128)[0]
    cert_addr = struct.unpack_from("<I", d, 0x118)[0]
    num_sections = struct.unpack_from("<I", d, 0x11C)[0]
    sec_hdr_addr = struct.unpack_from("<I", d, 0x120)[0]
    kthunk_enc = struct.unpack_from("<I", d, 0x158)[0]

    def v2o(vaddr):  # virtual addr -> file offset (headers are loaded at base)
        return vaddr - base

    print(f"XBE base            = 0x{base:08x}")
    print(f"sizeof_headers      = 0x{sizeof_headers:x} ({sizeof_headers})")
    print(f"sizeof_image        = 0x{sizeof_image:x} ({sizeof_image})")
    print(f"num_sections        = {num_sections}")
    print(f"cert_addr (virt)    = 0x{cert_addr:08x}")
    print(f"sec_hdr_addr (virt) = 0x{sec_hdr_addr:08x}")

    # entry point & kernel thunk are XOR-encrypted; decode with debug/retail keys
    ENTRY_DEBUG = 0x94859D4B
    ENTRY_RETAIL = 0xA8FC57AB
    KTHUNK_DEBUG = 0xEFB1F152
    KTHUNK_RETAIL = 0x5B6D40B6
    for label, k in (("retail", ENTRY_RETAIL), ("debug", ENTRY_DEBUG)):
        ep = entry_enc ^ k
        if base <= ep < base + sizeof_image:
            print(f"entry point         = 0x{ep:08x}  ({label} key)")
            break
    else:
        print(f"entry point (raw)   = 0x{entry_enc:08x}  (no key matched)")

    # certificate
    co = v2o(cert_addr)
    cert_size = struct.unpack_from("<I", d, co)[0]
    title_id = struct.unpack_from("<I", d, co + 0x08)[0]
    title_name = d[co+0x0C: co+0x0C+80].decode("utf-16-le", "replace").split("\x00")[0]
    allowed_media = struct.unpack_from("<I", d, co + 0x9C)[0]
    game_region = struct.unpack_from("<I", d, co + 0xA0)[0]
    version = struct.unpack_from("<i", d, co + 0xAC)[0]
    print(f"\n=== CERTIFICATE ===")
    print(f"cert_size           = 0x{cert_size:x}")
    print(f"title_id            = 0x{title_id:08x}  (ascii pub '{chr((title_id>>24)&0xff)}{chr((title_id>>16)&0xff)}' num {title_id & 0xFFFF})")
    print(f"title_name          = {title_name!r}")
    print(f"allowed_media       = 0x{allowed_media:08x}")
    print(f"game_region         = 0x{game_region:08x}")
    print(f"version             = {version}")

    # sections
    print(f"\n=== SECTIONS ({num_sections}) ===")
    print(f"{'name':16s} {'vaddr':>10s} {'vsize':>10s} {'rawaddr':>10s} {'rawsize':>10s} {'flags':>8s}")
    so = v2o(sec_hdr_addr)
    SECHDR = 0x38
    sections = []
    for i in range(num_sections):
        eo = so + i * SECHDR
        flags = struct.unpack_from("<I", d, eo + 0x00)[0]
        vaddr = struct.unpack_from("<I", d, eo + 0x04)[0]
        vsize = struct.unpack_from("<I", d, eo + 0x08)[0]
        raddr = struct.unpack_from("<I", d, eo + 0x0C)[0]
        rsize = struct.unpack_from("<I", d, eo + 0x10)[0]
        name_addr = struct.unpack_from("<I", d, eo + 0x14)[0]
        no = v2o(name_addr)
        nm = d[no:no+16].split(b"\x00")[0].decode("ascii", "replace")
        sections.append((nm, vaddr, vsize, raddr, rsize, flags))
        writable = "W" if (flags & 1) else "-"
        preload = "P" if (flags & 2) else "-"
        executable = "X" if (flags & 4) else "-"
        print(f"{nm:16s} 0x{vaddr:08x} 0x{vsize:08x} 0x{raddr:08x} 0x{rsize:08x} {flags:08x} {writable}{preload}{executable}")
    # Emit disasm hints for the .text section
    print(f"\n=== DISASM HINTS ===")
    for nm, vaddr, vsize, raddr, rsize, flags in sections:
        if (flags & 4) or nm.lower() in (".text",):
            print(f"# {nm}: exec section")
            print(f"objdump -D -b binary -m i386 -M intel --adjust-vma=0x{vaddr:08x} \\")
            print(f"  --start-address=0x{vaddr:08x} --stop-address=0x{vaddr+vsize:08x} \\")
            print(f"  <(dd if=default.xbe bs=1 skip={raddr} count={rsize} 2>/dev/null)")
            print(f"# raw file range: [0x{raddr:x} .. 0x{raddr+rsize:x}), maps to virt [0x{vaddr:x} .. 0x{vaddr+vsize:x})")
    return sections

if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "extract":
        extract(sys.argv[2], sys.argv[3])
    elif cmd == "parse":
        parse(sys.argv[2])
    elif cmd == "all":
        p = extract(sys.argv[2], sys.argv[3])
        print("="*70)
        parse(p)
    else:
        print(__doc__)
        sys.exit(1)
