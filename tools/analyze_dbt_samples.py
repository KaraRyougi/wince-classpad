#!/usr/bin/env python3
"""Summarize host PC samples using symbols from the exact translator ELF.

PC bins are 64 bytes: attribution uses their midpoint and is approximate,
especially at function boundaries. Modeled seconds are samples / 32768,
not wall time or a hardware measurement.
"""
import argparse
import bisect
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import subprocess


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('log', type=Path)
    p.add_argument('elf', type=Path)
    p.add_argument('--nm', default='sh-elf-nm')
    a = p.parse_args()
    symbols = []
    for line in subprocess.check_output([a.nm, '-n', '--defined-only', str(a.elf)], text=True).splitlines():
        fields = line.split()
        if len(fields) == 3 and fields[1] in ('t', 'T'):
            symbols.append((int(fields[0], 16), fields[2]))
    symbols.sort()
    addresses = [addr for addr, _ in symbols]
    log = a.log.read_text()
    summary = re.search(r'\[DBTSAMPLE\] total=(\d+) sleep=(\d+) code=(\d+) other=(\d+)', log)
    if not summary or not symbols:
        raise SystemExit('Missing completed sample summary or ELF code symbols')
    buckets = Counter()
    for address, count in re.findall(r'\[DBTSAMPLE pc\] ([0-9a-f]+) (\d+)', log):
        index = bisect.bisect_right(addresses, int(address, 16) + 32) - 1
        buckets[symbols[index][1] if index >= 0 else 'unattributed'] += int(count)
    totals = dict(zip(('total', 'sleep', 'translated_code', 'other'), map(int, summary.groups())))
    totals['runtime'] = sum(buckets.values())
    assert totals['total'] == sum(totals[k] for k in ('sleep', 'translated_code', 'other', 'runtime'))
    result = {
        'elf_sha256': hashlib.sha256(a.elf.read_bytes()).hexdigest(),
        'log_sha256': hashlib.sha256(a.log.read_bytes()).hexdigest(),
        'samples': totals,
        'runtime_functions_approximate': [
            dict(symbol=name, samples=count, modeled_seconds=count / 32768)
            for name, count in buckets.most_common(30)
        ],
    }
    guest = Counter()
    for address, count in re.findall(r'\[DBTSAMPLE guest\] ([0-9a-f]+) (\d+)', log):
        guest[int(address, 16)] += int(count)
    unmapped = re.search(r'\[DBTSAMPLE guest-unmapped\] (\d+)', log)
    if unmapped:
        result['guest_unmapped'] = int(unmapped[1])
        assert sum(guest.values()) + result['guest_unmapped'] == totals['translated_code']
        result['guest_block_starts'] = [
            dict(pc=hex(pc), samples=count, modeled_seconds=count / 32768)
            for pc, count in guest.most_common(50)
        ]
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
