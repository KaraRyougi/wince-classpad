#!/usr/bin/env python3
"""mkextrom.py - build test EXT regions: small Windows CE ROM regions in the
format makeimg writes for config.bib's EXT region (docs/03), as B000FF .bin
files that the flashers and tools/nkflash.py treat like an XIP NK.bin.

The real EXT region (Math Xpander, XIP) is built by makeimg in the VM; this
tool exists for the tests of the code that finds and chains it
(bsp/classpad50/SRC/INC/classpad_extrom.h): bsp/tests/test_extrom.c and the
extrom guest.

  mkextrom.py <manifest> <out.bin> [--base 0x83860000] [--max 0x7A0000] [--root DIR]

The manifest has one entry per line, '#' starts a comment:

    <ROM name>  <source path>  [flags]

flags: S (system), H (hidden), M (a module: a TOCentry with an e32_rom and
one o32_rom whose section is the file's bytes, enough for the structure
checks, not a runnable image). Names with spaces are quoted. Source paths
are relative to --root (default: the manifest's directory); $NAME expands
from the environment.

Layout: base+0x40 "ECEC" (ROM_SIGNATURE), +0x44 pTOC, +0x48 pTOC - base;
base+0x50 ROMHDR (84 bytes); TOCentry[nummods], FILESentry[numfiles]; names,
e32/o32 records, then the data, 4-byte aligned, uncompressed. One record per
64 KiB.
"""
import os, struct, sys, time

ROM_SIGNATURE = 0x43454345
FILE_ATTRIBUTE_READONLY, FILE_ATTRIBUTE_HIDDEN, FILE_ATTRIBUTE_SYSTEM = 1, 2, 4
ROMHDR_AT = 0x50
ROMHDR_SIZE = 84
TOC_ENTRY = 32
FILES_ENTRY = 28
E32_SIZE = 112
O32_SIZE = 24
CPU_SH4 = 0x01A6
RECORD = 0x10000


def filetime(t):
    return int(t * 10_000_000) + 116444736000000000


def parse_manifest(path, root):
    entries = []
    for n, line in enumerate(open(path, encoding="utf-8"), 1):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        # names may contain spaces: "Math Xpander.lnk" is quoted
        parts, cur, quoted = [], "", False
        for ch in line:
            if ch == '"':
                quoted = not quoted
            elif ch.isspace() and not quoted:
                if cur:
                    parts.append(cur); cur = ""
            else:
                cur += ch
        if cur:
            parts.append(cur)
        if len(parts) not in (2, 3):
            sys.exit(f"{path}:{n}: expected <ROM name> <source> [flags]")
        name, src = parts[0], os.path.expandvars(parts[1])
        flags = parts[2].upper() if len(parts) == 3 else ""
        if not os.path.isabs(src):
            src = os.path.join(root, src)
        if not os.path.isfile(src):
            sys.exit(f"{path}:{n}: no file {src}")
        attr = FILE_ATTRIBUTE_READONLY
        if "S" in flags: attr |= FILE_ATTRIBUTE_SYSTEM
        if "H" in flags: attr |= FILE_ATTRIBUTE_HIDDEN
        if len(name.encode("ascii")) > 63:
            sys.exit(f"{path}:{n}: ROM name too long")
        entries.append((name, src, attr, "M" in flags))
    names = [e[0].lower() for e in entries]
    dup = {x for x in names if names.count(x) > 1}
    if dup:
        sys.exit(f"duplicate ROM names: {sorted(dup)}")
    return entries


def build(entries, base, maxlen):
    al = lambda x: (x + 3) & ~3
    mods = [e for e in entries if e[3]]
    files = [e for e in entries if not e[3]]
    toc_at = base + ROMHDR_AT + ROMHDR_SIZE
    files_at = toc_at + TOC_ENTRY * len(mods)
    pos = files_at + FILES_ENTRY * len(files)
    name_addr = {}
    for e in mods + files:
        name_addr[e[0]] = pos
        pos += len(e[0]) + 1
    pos = al(pos)
    e32_addr, o32_addr = {}, {}
    for e in mods:
        e32_addr[e[0]] = pos; pos += E32_SIZE
        o32_addr[e[0]] = pos; pos += O32_SIZE
    data_addr, blob = {}, {}
    for e in mods + files:
        b = open(e[1], "rb").read()
        data_addr[e[0]] = pos; blob[e[0]] = b
        pos = al(pos + len(b))
    end = pos
    if end - base > maxlen:
        sys.exit(f"EXT region needs {end - base} bytes, the window has {maxlen}")
    img = bytearray(b"\xff" * (end - base))
    put = lambda a, fmt, *v: struct.pack_into(fmt, img, a - base, *v)
    img[0:0x40] = b"\0" * 0x40
    put(base + 0x40, "<III", ROM_SIGNATURE, base + ROMHDR_AT, ROMHDR_AT)
    img[0x4C:ROMHDR_AT] = b"\0" * (ROMHDR_AT - 0x4C)
    put(base + ROMHDR_AT, "<IIIIIIIIIIIIIIIIIHHIII",
        0, 0, base, end, len(mods),    # dllfirst, dlllast, physfirst, physlast, nummods
        0, 0, 0, 0, 0, 0, 0,           # RAM start/free/end, copy entries/offset, profile len/offset
        len(files), 0, 0, 0, 0,        # numfiles, kernel flags, FS RAM %, drivglob start/len
        CPU_SH4, 0, 0, 0, 0)           # usCPUType, usMiscFlags, pExtensions, tracking start/len
    for name, src, attr, _ in mods + files:
        img[name_addr[name] - base:name_addr[name] - base + len(name) + 1] = name.encode("ascii") + b"\0"
        d = data_addr[name]
        img[d - base:d - base + len(blob[name])] = blob[name]
    for i, (name, src, attr, _) in enumerate(mods):
        ft, n = filetime(os.path.getmtime(src)), len(blob[name])
        put(toc_at + TOC_ENTRY * i, "<IIIIIIII", attr, ft & 0xFFFFFFFF, ft >> 32, n,
            name_addr[name], e32_addr[name], o32_addr[name], data_addr[name])
        e32 = bytearray(E32_SIZE)
        struct.pack_into("<HHIIHHIIIII", e32, 0, 1, 0x0102, 0, 0x10000000, 3, 0, 0, al(n) + 0x1000, 0, 0, 0)
        struct.pack_into("<H", e32, 108, 9)           # e32_subsys: Windows CE GUI
        img[e32_addr[name] - base:e32_addr[name] - base + E32_SIZE] = e32
        put(o32_addr[name], "<IIIIII", n, 0x1000, n, data_addr[name], 0, 0x60000020)
    for i, (name, src, attr, _) in enumerate(files):
        ft, n = filetime(os.path.getmtime(src)), len(blob[name])
        put(files_at + FILES_ENTRY * i, "<IIIIIII", attr, ft & 0xFFFFFFFF, ft >> 32,
            n, n, name_addr[name], data_addr[name])
    return bytes(img), end


def write_bin(path, base, img):
    out = bytearray(b"B000FF\n" + struct.pack("<II", base, len(img)))
    for off in range(0, len(img), RECORD):
        chunk = img[off:off + RECORD]
        out += struct.pack("<III", base + off, len(chunk), sum(chunk)) + chunk
    out += struct.pack("<III", 0, 0, 0)   # no entry point: not bootable on its own
    open(path, "wb").write(out)


def main(argv):
    args = argv[1:]
    opts = {"--base": "0x83860000", "--max": "0x7A0000", "--root": None}
    pos = []
    while args:
        a = args.pop(0)
        if a in opts:
            opts[a] = args.pop(0)
        else:
            pos.append(a)
    if len(pos) != 2:
        print(__doc__); return 2
    manifest, out = pos
    base, maxlen = int(opts["--base"], 0), int(opts["--max"], 0)
    root = opts["--root"] or os.path.dirname(os.path.abspath(manifest))
    entries = parse_manifest(manifest, root)
    img, end = build(entries, base, maxlen)
    write_bin(out, base, img)
    sys.stderr.write("mkextrom: %d entries, %08x..%08x (%d of %d bytes)\n" % (len(entries), base, end, end - base, maxlen))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
