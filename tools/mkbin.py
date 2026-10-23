#!/usr/bin/env python3
"""mkbin.py - wrap a flat little-endian binary as a Windows CE .bin (B000FF)
image, for testing the translator's image loader without Platform Builder.

  mkbin.py <flat.bin> <base> <entry> <out.bin> [--chunk N]

Records are emitted in chunks (default 4096 bytes) with the standard byte-sum
checksum; the terminator record carries the entry point. The flat binary is
expected to already contain the "ECEC" marker + ROMHDR pointer at offset 0x40
(the nkimg test guest does), as ROMIMAGE output would.
"""
import struct, sys

def main(argv):
    if len(argv) < 5:
        print(__doc__); return 2
    data = open(argv[1], "rb").read()
    base = int(argv[2], 0); entry = int(argv[3], 0)
    chunk = 4096
    if "--chunk" in argv: chunk = int(argv[argv.index("--chunk") + 1], 0)
    out = bytearray(b"B000FF\n" + struct.pack("<II", base, len(data)))
    for off in range(0, len(data), chunk):
        part = data[off:off + chunk]
        if not any(part):                      # ROMIMAGE skips all-zero runs too
            continue
        out += struct.pack("<III", base + off, len(part), sum(part) & 0xFFFFFFFF) + part
    out += struct.pack("<III", 0, entry, 0)
    out += b"\0" * (-len(out) % 4)                # casio-emu loads flash files in 32-bit units: keep the tail whole
    open(argv[4], "wb").write(out)
    print("wrote %s: %d bytes, image 0x%08x+0x%x, entry 0x%08x" % (argv[4], len(out), base, len(data), entry))
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv))
