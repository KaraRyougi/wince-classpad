#!/usr/bin/env python3
"""Convert cumulative DBT hardware diagnostics into interval rates.

--timer-hz must match TMU1's actual input clock; it is not inferred from CE
milliseconds. Intervals must be shorter than one 32-bit timer wrap. TMU1
stops during suspend, so these are timer-running seconds, not wall seconds.
"""
import argparse
import json
import re
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('log', type=Path)
p.add_argument('--timer-hz', type=float, required=True)
a = p.parse_args()
if a.timer_hz <= 0:
    p.error('--timer-hz must be positive')
snapshots = []
for line in a.log.read_text().splitlines():
    values = {k: int(v, 16) for k, v in re.findall(r'(\w+)=([0-9a-fA-F]+)', line)}
    if line.startswith('[hw] '):
        snapshots.append(dict(values=values, irq={}, ptel={}))
    elif snapshots and line.startswith('[hw irq] '):
        snapshots[-1]['irq'][hex(values['intevt'])] = values['count']
    elif snapshots and line.startswith('[hw p0-ldtlb] '):
        snapshots[-1]['ptel'] = values
intervals = []
for before, after in zip(snapshots, snapshots[1:]):
    delta = lambda x, y: (y - x) & 0xffffffff
    seconds = delta(before['values']['ticks'], after['values']['ticks']) / a.timer_hz
    if not seconds:
        continue
    keys = ('vec100', 'vec400', 'vec600', 'internal_trap', 'other')
    rates = {k: delta(before['values'][k], after['values'][k]) / seconds for k in keys}
    irq = {k: delta(before['irq'].get(k, 0), v) / seconds for k, v in after['irq'].items()}
    ptel = {k: delta(before['ptel'].get(k, 0), v) for k, v in after['ptel'].items()}
    intervals.append(dict(timer_seconds=seconds, ccr=hex(after['values']['ccr']),
                          exceptions_per_second=rates, intevt_per_second=irq,
                          p0_ldtlb_counts=ptel))
print(json.dumps(dict(timer_hz=a.timer_hz, snapshots=len(snapshots), intervals=intervals), indent=2))
