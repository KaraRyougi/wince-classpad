#!/usr/bin/env python3
"""Map DBTSTAT physical ROM histogram buckets to executable image sections.

Usage: map_rom_buckets.py NK.bin ADDRESS:COUNT ... [--shift 15]
Reports every overlapping module, without assigning a coarse bucket to one
function. Uses o32_dataptr (ROM location), not o32_realaddr (mapped VA).
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
from nkbin import BinImage


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('nk', type=Path)
    p.add_argument('buckets', nargs='+')
    p.add_argument('--shift', type=int, default=15)
    a = p.parse_args()
    if not 0 <= a.shift <= 25:
        p.error('shift must be between 0 and 25')
    data = a.nk.read_bytes()
    im = BinImage(data)
    pointer, header = im.romhdr()
    sections = []
    for name, _, e32, o32, _ in im.modules(pointer, header):
        count = struct.unpack('<H', im.read(e32, 2))[0]
        for i in range(count):
            vs, rva, ps, dp, ra, flags = struct.unpack('<6I', im.read(o32 + i * 24, 24))
            # Compressed sections cannot map physical offsets to instructions.
            if flags & 0x20000000 and not flags & 0x2000:
                sections.append((dp, dp + min(vs, ps), name, rva, ra))
    rows = []
    for bucket in a.buckets:
        address, count = bucket.split(':')
        lo = (int(address, 16) & 0x1fffffff) | 0x80000000
        hi = lo + (1 << a.shift)
        overlaps = []
        for start, end, name, rva, runtime in sections:
            if start < hi and end > lo:
                lower, upper = max(lo, start), min(hi, end)
                overlaps.append(dict(module=name, rom_start=hex(lower), rom_end=hex(upper),
                                     rva_start=hex(rva + lower - start),
                                     runtime_start=hex(runtime + lower - start)))
        rows.append(dict(address=hex(lo), samples=int(count), sections=overlaps))
    print(json.dumps(dict(nk_sha256=hashlib.sha256(data).hexdigest(),
                         bucket_bytes=1 << a.shift, buckets=rows,
                         caveat='Interrupt-biased counts are not CPU time; overlapping sections cannot share counts reliably.'), indent=2))


if __name__ == '__main__':
    main()
