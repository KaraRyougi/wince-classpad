#!/usr/bin/env python3
"""Check direct-hash ASM/C indices and real shared-owner removal paths."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'dbt/rt/rt.c').read_text()
def function(name):
    pos=s.index(name+'('); start=s.rfind('\n',0,pos)+1
    if name == 'hash_remove':
        return s[start:s.index('\n/* Physical address', start)]
    end=s.index('{',pos)+1; depth=1
    while depth:
        depth+=(s[end]=='{')-(s[end]=='}');end+=1
    return s[start:end]

def portable(name):
    body=function(name)
    if name=='hash_insert':
        body=body.replace('__asm__ volatile ("stc sr, %0" : "=r"(sr));','sr = 0;')
        body=body.replace('__asm__ volatile ("ldc %0, sr" :: "r"(sr | SR_IMASK) : "memory");','(void)sr;')
        body=body.replace('__asm__ volatile ("ldc %0, sr" :: "r"(sr) : "memory");','(void)sr;')
        assert '__asm__' not in body
    return body
program=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t sf_u32;
#define HSIZE 8192
#define IS_USER(pc) ((pc)<0x80000000u)
#define ASID_ANY 0xffffu
#define DBT_HASH_TWO_WAY
#define DBT_HASH_ASID_TAG
#define SR_IMASK 0xf0
static unsigned g_asid;
#define DBT_INTERIOR_HASH
typedef struct {sf_u32 gpc,host,asid;} hent;
static hent g_hash[HSIZE];
typedef struct {sf_u32 guest_pc,host_base,host_len,guest_len;} dbt_block;
static dbt_block g_blocks[2];
static struct {unsigned user,asid;} g_binfo[2];
static unsigned char g_alias_blocks[1];
static unsigned g_alias_hash_removed;
'''+function('hidx')+'\n#define fast_hidx(gpc, asid) (hidx(gpc) & ~1u)\n'+'\n'.join(portable(n) for n in ['hash_insert','hash_find','hash_remove'])+r'''
int main(void) {
 unsigned pc=0x400000, other=pc+0x4000;
 assert(fast_hidx(pc,0)==fast_hidx(other,0));
 for(unsigned i=0;i<HSIZE;++i)g_hash[i].gpc=1;
 g_asid=7;hash_insert(pc,0x8cc80000);hash_insert(other,0x8cc80100);
 assert(hash_find(pc)==0x8cc80000 && hash_find(other)==0x8cc80100);
 hash_insert(pc,0x8cc80004);assert(hash_find(other)==0x8cc80100 && hash_find(pc)==0x8cc80004);
 g_asid=8;assert(!hash_find(pc));hash_insert(pc,0x8cc80200);
 assert(hash_find(pc)==0x8cc80200);g_asid=7;
 assert(hash_find(other)==0x8cc80100 && !hash_find(pc));
 hash_insert(pc,0x8cc80000);g_asid=8;hash_insert(pc,0x8cc80000);
 dbt_block *b=&g_blocks[0];*b=(dbt_block){pc,0x8cc80000,128,32};
 g_binfo[0].asid=ASID_ANY;g_binfo[0].user=1;
 hash_remove(b);assert(!hash_find(pc));g_asid=7;assert(!hash_find(pc));
 hash_insert(pc+2,b->host_base+8);g_asid=8;hash_insert(pc+2,b->host_base+12);
 g_alias_blocks[0]=1;hash_remove(b);assert(!hash_find(pc+2));g_asid=7;assert(!hash_find(pc+2));
 hash_insert(pc+2,b->host_base+128);hash_remove(b);assert(hash_find(pc+2)==b->host_base+128);
 g_asid=1;hash_insert(0x8c300000,0x8cc81000);g_asid=255;
 assert(hash_find(0x8c300000)==0x8cc81000);
 puts("PASS: colliding PCs, ASID isolation, refresh, eviction, shared-owner and alias invalidation, global kernel keys");
}
'''
with tempfile.TemporaryDirectory() as td:
    c=Path(td)/'test.c';exe=Path(td)/'test';c.write_text(program)
    for flags in [[],['-DDBT_ALIAS_LOCAL_REMOVE']]:
        subprocess.run(['cc','-O2','-fsanitize=address,undefined',*flags,str(c),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
