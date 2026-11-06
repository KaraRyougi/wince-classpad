#!/usr/bin/env python3
"""Resolve sampled CE block starts against NK ROM sections and matching MAPs.

Only matching image/MAP timestamps are used. Overlapping process-local EXE
addresses remain explicitly ambiguous; no process identity is inferred.
"""
import argparse
import bisect
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import struct
from nkbin import BinImage


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('log', type=Path)
    p.add_argument('nk', type=Path)
    p.add_argument('maps', type=Path)
    a = p.parse_args()
    image = BinImage(a.nk.read_bytes())
    pointer, header = image.romhdr()
    sections, provenance, skipped = [], [], []
    for name, _, e32, o32, _ in image.modules(pointer, header):
        map_path = a.maps / (Path(name).stem + '.map')
        raw = image.read(e32, 36)
        count = struct.unpack_from('<H', raw)[0]
        stamp = struct.unpack_from('<I', raw, 32)[0]
        symbols = []
        if not map_path.exists():
            skipped.append(dict(module=name, reason='missing MAP'))
        else:
            data = map_path.read_bytes()
            text = data.decode('ascii', errors='replace')
            ts = re.search(r'Timestamp is ([0-9a-f]+)', text, re.I)
            preferred = re.search(r'Preferred load address is ([0-9a-f]+)', text, re.I)
            if not ts or int(ts[1], 16) != stamp or not preferred:
                skipped.append(dict(module=name, reason='MAP timestamp/header mismatch'))
            else:
                base = int(preferred[1], 16)
                symbols = sorted((int(address, 16) - base, symbol) for symbol, address in
                                 re.findall(r'^\s+[0-9a-f]+:[0-9a-f]+\s+(\S+)\s+([0-9a-f]{8})\s+f\b', text, re.I | re.M))
                provenance.append(dict(module=name, map_sha256=hashlib.sha256(data).hexdigest()))
        for i in range(count):
            size, rva, _, _, runtime, flags = struct.unpack('<6I', image.read(o32 + i * 24, 24))
            if not flags & 0x20000000:
                continue
            entries = sorted((runtime + offset - rva, symbol) for offset, symbol in symbols
                             if rva <= offset < rva + size)
            sections.append((runtime, runtime + size, name, entries, [v for v, _ in entries]))
    totals = Counter()
    modules = Counter()
    for address, hits in re.findall(r'\[DBTSAMPLE guest\] ([0-9a-f]+) (\d+)', a.log.read_text()):
        pc = int(address, 16)
        candidates = []
        for lo, hi, module, symbols, addresses in sections:
            if lo <= pc < hi:
                index = bisect.bisect_right(addresses, pc) - 1
                candidates.append(module + '!' + (symbols[index][1] if index >= 0 else '<no matching symbol>'))
        label = candidates[0] if len(candidates) == 1 else (
            'AMBIGUOUS: ' + ' | '.join(candidates) if candidates else 'UNRESOLVED ' + hex(pc))
        totals[label] += int(hits)
        module_label = candidates[0].split('!', 1)[0] if len(candidates) == 1 else (
            'AMBIGUOUS' if candidates else 'UNRESOLVED')
        modules[module_label] += int(hits)
    print(json.dumps(dict(nk_sha256=hashlib.sha256(a.nk.read_bytes()).hexdigest(),
                          maps=provenance, skipped=skipped,
                          total_guest_samples=sum(totals.values()),
                          modules=[dict(module=n, samples=c, modeled_seconds=c / 32768)
                                   for n, c in modules.most_common()],
                          functions=[dict(symbol=n, samples=c, modeled_seconds=c / 32768)
                                     for n, c in totals.most_common(50)]), indent=2))


if __name__ == '__main__':
    main()
