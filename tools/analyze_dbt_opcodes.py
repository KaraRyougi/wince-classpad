#!/usr/bin/env python3
"""Group host-opcode residency samples; these are not execution counts.

Decode sampled 16-bit words with the target binutils, without executing them.
Unrecognized DSP words remain explicit instead of inferring guest semantics.
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import re
import subprocess
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('log', type=Path)
p.add_argument('--objdump', default='sh-elf-objdump')
a = p.parse_args()
text = a.log.read_text()
counts = Counter()
for opcode, count in re.findall(r'\[DBTSAMPLE opcode\] ([0-9a-f]{4}) (\d+)', text):
    counts[int(opcode, 16)] += int(count)
summary = re.search(r'\[DBTSAMPLE\] total=\d+ sleep=\d+ code=(\d+)', text)
if not counts or not summary:
    raise SystemExit('Missing completed opcode samples')
assert sum(counts.values()) == int(summary[1])
opcodes = sorted(counts)
with tempfile.TemporaryDirectory(prefix='dbt-opcodes-') as directory:
    path = Path(directory) / 'opcodes.bin'
    path.write_bytes(b''.join(op.to_bytes(2, 'big') + b'\x00\x09' for op in opcodes))
    output = subprocess.check_output([a.objdump, '-D', '-b', 'binary', '-m', 'sh4',
                                      '-EB', '--no-show-raw-insn', str(path)], text=True)
mnemonics = {}
for address, instruction in re.findall(r'^\s*([0-9a-f]+):\s+(.+)$', output, re.M):
    address = int(address, 16)
    if address % 4 == 0 and address // 4 < len(opcodes):
        mnemonics[opcodes[address // 4]] = instruction.split()[0]
groups = Counter()
for op, count in counts.items():
    groups[mnemonics.get(op, '<undecoded>')] += count
print(json.dumps(dict(total_samples=sum(counts.values()),
    meaning='Uniform timer-quantum residency samples in generated code; not instruction execution counts.',
    mnemonics=[dict(mnemonic=m, samples=c, modeled_seconds=c/32768)
               for m, c in groups.most_common()],
    opcodes=[dict(opcode=hex(op), mnemonic=mnemonics.get(op, '<undecoded>'), samples=c)
             for op, c in counts.most_common(40)]), indent=2))
