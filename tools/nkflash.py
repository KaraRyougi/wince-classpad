#!/usr/bin/env python3
"""nkflash.py - put a Windows CE NK.bin (and an EXT region) where the translator
and the OAL expect them in the emulated NOR (casio-emu HLE_OSROM=<file>
HLE_OSROM_PAD=<offset>).

  nkflash.py <NK.bin|-> <out> [default-offset] [--ext EXT.bin]   -> prints the flash offset (hex)

A RAMIMAGE build (image start inside the guest RAM window, 0x8C000000 up) is
written as is, padded to a multiple of 4 bytes (casio-emu copies flash files
in 32-bit units), at the default offset (16 MiB): the runtime finds the
B000FF records there and copies them into RAM. A ROMIMAGE build (image start
in the NOR's P1 range 0x80000000-0x83FFFFFF) is flattened to the bytes the
NOR must hold and written at start - 0x80000000: the kernel then executes in
place and the loader only reads its ROMHDR.

--ext adds the files-only EXT region of tools/mkextrom.py (an XIP .bin at
0x80EE0000, docs/03) to the same flat file: casio-emu takes one HLE_OSROM
blob, so it then covers both images with erased bytes (0xFF) between them,
which is harmless for the add-in boot (the emulated NOR is otherwise erased).
"-" instead of NK.bin flashes only the EXT region (the extrom guest).
"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nkbin

NOR_P1 = 0x80000000
NOR_SIZE = 0x04000000          # the fx-CP400 chip (32 MiB on the fx-CG100: its images stay below)


def xip_part(path):
    img = nkbin.BinImage(open(path, "rb").read())
    if not NOR_P1 <= img.start < NOR_P1 + NOR_SIZE:
        sys.exit("nkflash: %s is not built for the NOR (start 0x%08x)" % (path, img.start))
    return img.start - NOR_P1, bytes(img.flat(fill=0xFF)), img


def main(argv):
    args = argv[1:]
    ext = None
    if "--ext" in args:
        i = args.index("--ext"); ext = args[i + 1]; del args[i:i + 2]
    if len(args) < 2:
        print(__doc__); return 2
    nk, out = args[0], args[1]
    default = int(args[2], 0) if len(args) > 2 else 0x01000000
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    parts = []                                           # (flash offset, bytes)
    if nk != "-":
        data = open(nk, "rb").read()
        img = nkbin.BinImage(data)
        if NOR_P1 <= img.start < NOR_P1 + NOR_SIZE:
            off, flat, _ = xip_part(nk)
            parts.append((off, flat))
            sys.stderr.write("nkflash: XIP image 0x%08x+0x%x -> flash offset 0x%08x\n" % (img.start, img.length, off))
        else:
            if ext:
                sys.exit("nkflash: --ext needs an XIP NK image (a RAM image is copied, not flattened)")
            parts.append((default, data))
            sys.stderr.write("nkflash: RAM image 0x%08x+0x%x -> flash offset 0x%08x, copied by the loader\n" % (img.start, img.length, default))
    if ext:
        off, flat, img = xip_part(ext)
        parts.append((off, flat))
        sys.stderr.write("nkflash: EXT region 0x%08x+0x%x -> flash offset 0x%08x\n" % (img.start, img.length, off))
    if not parts:
        print(__doc__); return 2
    parts.sort()
    for (a, da), (b, _) in zip(parts, parts[1:]):
        if a + len(da) > b:
            sys.exit("nkflash: images overlap at flash offset 0x%08x" % b)
    lo = parts[0][0]
    hi = max(o + len(d) for o, d in parts)
    blob = bytearray(b"\xff" * (hi - lo))
    for o, d in parts:
        blob[o - lo:o - lo + len(d)] = d
    blob += b"\xff" * (-len(blob) % 4)
    open(out, "wb").write(blob)
    sys.stderr.write("nkflash: %d bytes at flash offset 0x%08x\n" % (len(blob), lo))
    print("0x%08x" % lo)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
