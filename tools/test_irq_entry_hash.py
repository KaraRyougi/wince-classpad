#!/usr/bin/env python3
"""Exercise runtime IRQ alias insertion/lookup guards with real block metadata."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'dbt/rt/rt.c').read_text()
start = source.index('static struct { sf_u32 gpc, host, asid, generation, index; } g_irq_entry')
end = source.index('\n#endif\n\nstatic sf_u32 op_swapb', start)
body = source[start:end]
program = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define dbt_get_meta original_get_meta
#include "translate.h"
#undef dbt_get_meta
static unsigned reads, pending;
static void dbt_get_meta(const dbt_block *b, int i, dbt_imeta *out) {
 ++reads; original_get_meta(b,i,out);
}
int dbt_block_pending_spills(const dbt_block *b,int i,sf_u32 off,dbt_spill *out,int max) {
 return pending;
}
static dbt_block g_blocks[2];
static struct { unsigned dead; } g_binfo[2];
static struct { sf_u32 flushes; } dbt_stat;
static sf_u32 g_asid, g_irq_entry_hits;
''' + body + r'''
static dbt_imeta meta[3];
static void reset(void) {
 memset(g_irq_entry,0,sizeof g_irq_entry);
 memset(g_blocks,0,sizeof g_blocks); memset(g_binfo,0,sizeof g_binfo);
 memset(meta,0,sizeof meta); reads=pending=0; g_asid=3; dbt_stat.flushes=10;
 g_blocks[0].guest_pc=0x1000; g_blocks[0].guest_len=16;
 g_blocks[0].host_base=0x8cc80000; g_blocks[0].borrow=-1;
 g_blocks[0].meta=meta; g_blocks[0].nmeta=2;
 meta[0].kind=META_INSN;meta[0].guest_off=2;meta[0].host_off=10;
 meta[1].kind=META_LINEAR;meta[1].guest_off=4;meta[1].host_off=20;
 meta[2].host_off=28;
 g_blocks[1]=g_blocks[0];g_blocks[1].guest_pc=0x1020;g_blocks[1].host_base+=1024;
}
int main(void) {
 reset(); remember_irq_entry(&g_blocks[0],0x1002);
 assert(find_irq_entry(0x1002)==0x8cc8000a);
 unsigned n=reads; remember_irq_entry(&g_blocks[0],0x1002); assert(reads==n);
 g_asid++; assert(!find_irq_entry(0x1002));
 remember_irq_entry(&g_blocks[0],0x1002); assert(reads>n && find_irq_entry(0x1002));
 dbt_stat.flushes++; assert(!find_irq_entry(0x1002));
 remember_irq_entry(&g_blocks[0],0x1002);assert(find_irq_entry(0x1002));
 g_binfo[0].dead=1; assert(!find_irq_entry(0x1002));
 reset();remember_irq_entry(&g_blocks[0],0x1002);
 assert(irq_entry_slot(0x1002)==irq_entry_slot(0x1022));
 remember_irq_entry(&g_blocks[1],0x1022);
 assert(!find_irq_entry(0x1002) && find_irq_entry(0x1022)==0x8cc8040a);
 reset();pending=1;remember_irq_entry(&g_blocks[0],0x1002);assert(!find_irq_entry(0x1002));
 pending=0;g_blocks[0].borrow=0;remember_irq_entry(&g_blocks[0],0x1002);assert(!find_irq_entry(0x1002));
 g_blocks[0].borrow=-1;g_binfo[0].dead=1;remember_irq_entry(&g_blocks[0],0x1002);assert(!find_irq_entry(0x1002));
 reset();remember_irq_entry(&g_blocks[0],0x1000);remember_irq_entry(&g_blocks[0],0x1010);
 assert(!find_irq_entry(0x1000) && !find_irq_entry(0x1010));
 remember_irq_entry(&g_blocks[0],0x1006); assert(find_irq_entry(0x1006)==0x8cc80016);
 remember_irq_entry(&g_blocks[0],0x100c); assert(!find_irq_entry(0x100c));
 puts("PASS: IRQ alias ASID/generation/dead guards, collisions, reuse, spill/borrow and linear boundaries");
}
'''
with tempfile.TemporaryDirectory() as td:
    c = Path(td) / 'test.c'; exe = Path(td) / 'test'
    c.write_text(program)
    subprocess.run(['cc', '-O2', '-DDBT_IRQ_ENTRY_HASH', '-fsanitize=address,undefined',
                    '-I'+str(root/'dbt/src'), '-I'+str(root/'fpuemu/src'),
                    str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
