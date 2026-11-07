#!/usr/bin/env python3
"""Exhaustively compare the optional decoder initializer with the baseline."""
import pathlib
import subprocess
import tempfile
import sys

root = pathlib.Path(__file__).resolve().parents[1]
fields = ('kind op size is_store am base data disp ctrl ctrl_bank movca br '
          'br_disp br_reg has_slot pcrel pc_reg priv_mem writes_t touched defs').split()
with tempfile.TemporaryDirectory(prefix='decode-template-') as directory:
    tmp = pathlib.Path(directory)
    for name, defines in [('baseline', ['-DDBT_DECODE_MEMSET']), ('candidate', [*sys.argv[1:]])]:
        subprocess.run(['cc', '-std=c99', '-O2', '-I', str(root),
                        '-Dsh4_decode=decode_' + name,
                        '-Dsh4_slot_legal=slot_' + name, *defines, '-c',
                        str(root / 'dbt/src/sh4dec.c'), '-o', str(tmp / (name + '.o'))], check=True)
    checks = '\n'.join(f'if (a.{f} != b.{f}) {{ printf("opcode %04x field {f} differs\\n", op); return 1; }}' for f in fields)
    source = '''#include <stdio.h>
#include "dbt/src/sh4dec.h"
void decode_baseline(sf_u32, sh_insn *);
void decode_candidate(sf_u32, sh_insn *);
int main(void) {
unsigned op, pass; sh_insn a, b, discarded;
for(op=0;op<65536;op++) {
decode_baseline(op,&a);
for(pass=0;pass<4;pass++) {
/* 0x204 has zero hash under (op ^ (op>>7)) & 511. Evict then
 * re-read to cover both collision misses and immediate hits. */
if(pass==2) decode_candidate(op ^ 0x204u,&discarded);
decode_candidate(op | (pass==3 ? 0xffff0000u : 0),&b);
''' + checks + '\n}\n}\nputs("PASS: all 65536 opcodes, all 21 decoded fields, repeated hits and collisions"); return 0; }\n'
    (tmp / 'test.c').write_text(source)
    subprocess.run(['cc', '-std=c99', '-O2', '-I', str(root), str(tmp / 'test.c'),
                    str(tmp / 'baseline.o'), str(tmp / 'candidate.o'),
                    str(root / 'fpuemu/src/fpuemu.c'), str(root / 'fpuemu/src/softfloat.c'),
                    '-o', str(tmp / 'test')], check=True)
    subprocess.run([str(tmp / 'test')], check=True)
