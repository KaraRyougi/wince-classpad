#!/usr/bin/env python3
"""nkflash.py - put a Windows CE NK.bin where the translator expects it in the
emulated NOR (casio-emu HLE_OSROM=<file> HLE_OSROM_PAD=<offset>).

  nkflash.py <NK.bin> <out> [default-offset]     -> prints the flash offset (hex)

A RAMIMAGE build (image start inside the guest RAM window, 0x8C000000 up) is
written as is, padded to a multiple of 4 bytes (casio-emu copies flash files
in 32-bit units), at the default offset (16 MiB): the runtime finds the
B000FF records there and copies them into RAM. A ROMIMAGE build (image start
in the NOR's P1 range 0x80000000-0x81FFFFFF) is flattened to the bytes the
NOR must hold and written at start - 0x80000000: the kernel then executes in
place and the loader only reads its ROMHDR.
"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nkbin

NOR_P1 = 0x80000000
NOR_SIZE = 0x02000000

def main(argv):
    if len(argv) < 3:
        print(__doc__); return 2
    data = open(argv[1], "rb").read()
    img = nkbin.BinImage(data)
    default = int(argv[3], 0) if len(argv) > 3 else 0x01000000
    os.makedirs(os.path.dirname(os.path.abspath(argv[2])), exist_ok=True)
    if NOR_P1 <= img.start < NOR_P1 + NOR_SIZE:
        flat = bytearray(img.flat(fill=0xFF))          # erased flash reads 0xFF
        flat += b"\xff" * (-len(flat) % 4)
        open(argv[2], "wb").write(flat)
        off = img.start - NOR_P1
        sys.stderr.write("nkflash: XIP image 0x%08x+0x%x -> flash offset 0x%08x (%d bytes)\n" % (img.start, img.length, off, len(flat)))
    else:
        out = data + b"\0" * (-len(data) % 4)
        open(argv[2], "wb").write(out)
        off = default
        sys.stderr.write("nkflash: RAM image 0x%08x+0x%x -> flash offset 0x%08x, copied by the loader\n" % (img.start, img.length, off))
    print("0x%08x" % off)
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv))
