#!/usr/bin/env python3
"""mkextrom.py - build the EXT region: a files-only Windows CE ROM region for the
erased hole inside Casio's OS image (docs/03 "The EXT region"), as a B000FF .bin
that the flashers treat like an XIP NK.bin.

  mkextrom.py <manifest> <out.bin> [--base 0x80EE0000] [--max 0x500000] [--root DIR]

The manifest has one file per line, '#' starts a comment:

    <ROM name>  <source path>  [flags]

flags as in a .bib FILES line: S (system), H (hidden); every ROM file is also
read-only. Source paths are relative to --root (default: the manifest's
directory); $NAME expands from the environment. The ROM name is what the file
is called in \\Windows on the device (ROM files all appear there).

Layout (what the CE 6 kernel reads through g_pOemGlobal->pROMChain, see
bsp/classpad/SRC/INC/classpad_extrom.h):
    base+0x40  "ECEC" (ROM_SIGNATURE), +0x44 pTOC, +0x48 pTOC - base
    base+0x50  ROMHDR (84 bytes): nummods 0, numfiles n, physfirst base,
               physlast end, dllfirst/dlllast 0 (no DLL address space)
               FILESentry[n] right after it (no TOCentry: nummods is 0)
               the names, then each file's bytes, 4-byte aligned, uncompressed
One record per 64 KiB so the flashers stream it like any image.
"""
import os, struct, sys, time

ROM_SIGNATURE = 0x43454345
FILE_ATTRIBUTE_READONLY, FILE_ATTRIBUTE_HIDDEN, FILE_ATTRIBUTE_SYSTEM = 1, 2, 4
ROMHDR_AT = 0x50
ROMHDR_SIZE = 84
FILES_ENTRY = 28
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
        entries.append((name, src, attr))
    names = [e[0].lower() for e in entries]
    dup = {x for x in names if names.count(x) > 1}
    if dup:
        sys.exit(f"duplicate ROM names: {sorted(dup)}")
    return entries


def build(entries, base, maxlen):
    al = lambda x: (x + 3) & ~3
    nfiles = len(entries)
    files_at = base + ROMHDR_AT + ROMHDR_SIZE
    names_at = files_at + FILES_ENTRY * nfiles
    pos = names_at
    name_addr = []
    for name, _, _ in entries:
        name_addr.append(pos)
        pos += len(name) + 1
    pos = al(pos)
    data_addr, blobs = [], []
    for _, src, _ in entries:
        b = open(src, "rb").read()
        data_addr.append(pos); blobs.append(b)
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
        0, 0, base, end, 0,            # dllfirst, dlllast, physfirst, physlast, nummods
        0, 0, 0, 0, 0, 0, 0,           # RAM start/free/end, copy entries/offset, profile len/offset
        nfiles, 0, 0, 0, 0,            # numfiles, kernel flags, FS RAM %, drivglob start/len
        CPU_SH4, 0, 0, 0, 0)           # usCPUType, usMiscFlags, pExtensions, tracking start/len
    for i, ((name, src, attr), na, da, b) in enumerate(zip(entries, name_addr, data_addr, blobs)):
        ft = filetime(os.path.getmtime(src))
        put(files_at + FILES_ENTRY * i, "<IIIIIII", attr, ft & 0xFFFFFFFF, ft >> 32,
            len(b), len(b), na, da)
        img[na - base:na - base + len(name) + 1] = name.encode("ascii") + b"\0"
        img[da - base:da - base + len(b)] = b
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
    opts = {"--base": "0x80EE0000", "--max": "0x500000", "--root": None}
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
    sys.stderr.write("mkextrom: %d files, %08x..%08x (%d of %d bytes)\n" % (len(entries), base, end, end - base, maxlen))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
