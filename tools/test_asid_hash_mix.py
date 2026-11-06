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
asm=(root/'dbt/rt/stubs.S').read_text()
pos=asm.index('#ifdef DBT_HASH_ASID_MIX',asm.index('xor     r2, r1'))
lines=asm[pos:asm.index('#endif',pos)].splitlines()[1:]
actual=[' '.join(x.split('/*')[0].split()) for x in lines]
assert actual==['cmp/pz r0','bf 5f','mov.l .Lhash_asidA, r2','mov.l @r2, r2',
 'extu.b r2, r2','shll2 r2','shll2 r2','shll r2','xor r2, r1','5:']
program=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t sf_u32;
#define HSIZE 8192
#define IS_USER(pc) ((pc)<0x80000000u)
#define ASID_ANY 0xffffu
#define DBT_HASH_ASID_MIX
#define DBT_INTERIOR_HASH
typedef struct {sf_u32 gpc,host,asid;} hent;
static hent g_hash[HSIZE];
typedef struct {sf_u32 guest_pc,host_base,host_len,guest_len;} dbt_block;
static dbt_block g_blocks[2];
static struct {unsigned user,asid;} g_binfo[2];
static unsigned char g_alias_blocks[1];
static unsigned g_alias_hash_removed;
'''+'\n'.join(function(n) for n in ['hidx','fast_hidx','hash_remove'])+r'''
static unsigned asm_index(uint32_t pc,uint32_t asid) {
 uint32_t r1=pc>>1,r2=(pc<<16)|(pc>>16);
 r1^=r2;
 if((int32_t)pc>=0){r2=asid&255;r2<<=2;r2<<=2;r2<<=1;r1^=r2;}
 return r1&(HSIZE-1);
}
int main(void) {
 uint32_t random=1;
 for(unsigned i=0;i<1000000;++i){
  random^=random<<13;random^=random>>17;random^=random<<5;
  uint32_t pc=random,asid=i;
  assert(fast_hidx(pc,asid)==asm_index(pc,asid));
  if(pc&0x80000000u)assert(fast_hidx(pc,asid)==hidx(pc));
 }
 unsigned seen[HSIZE]={0};
 for(unsigned asid=0;asid<256;++asid)assert(!seen[fast_hidx(0x10000,asid)]++);
 dbt_block *b=&g_blocks[0];*b=(dbt_block){0x10000,0x8cc80000,128,32};
 g_binfo[0].user=1;g_binfo[0].asid=7;
 unsigned own=fast_hidx(b->guest_pc,7),other=fast_hidx(b->guest_pc,8);
 g_hash[own].host=b->host_base;g_hash[other].host=0x8cd00000;
 hash_remove(b);assert(!g_hash[own].host && g_hash[other].host==0x8cd00000);
 memset(g_hash,0,sizeof g_hash);g_binfo[0].asid=ASID_ANY;
 for(unsigned asid=0;asid<256;++asid) {
  g_hash[fast_hidx(b->guest_pc,asid)].host=b->host_base;
  g_hash[fast_hidx(b->guest_pc+2,asid)].host=b->host_base+8;
 }
 g_hash[fast_hidx(b->guest_pc+4,7)].host=b->host_base+128;
 hash_remove(b);assert(g_alias_hash_removed==512);
 for(unsigned asid=0;asid<256;++asid) {
  assert(!g_hash[fast_hidx(b->guest_pc,asid)].host);
  assert(!g_hash[fast_hidx(b->guest_pc+2,asid)].host);
 }
 assert(g_hash[fast_hidx(b->guest_pc+4,7)].host==b->host_base+128);
 memset(g_hash,0,sizeof g_hash);g_binfo[0].asid=9;g_alias_blocks[0]=1;
 g_hash[fast_hidx(b->guest_pc+2,9)].host=b->host_base+8;
 g_hash[fast_hidx(b->guest_pc+2,10)].host=b->host_base+128;
 hash_remove(b);assert(!g_hash[fast_hidx(b->guest_pc+2,9)].host);
 assert(g_hash[fast_hidx(b->guest_pc+2,10)].host==b->host_base+128);
 puts("PASS: 1M ASM/C indices, 256 distinct ASIDs, shared-owner and interior-alias removal");
}
'''
with tempfile.TemporaryDirectory() as td:
    c=Path(td)/'test.c';exe=Path(td)/'test';c.write_text(program)
    for flags in [[],['-DDBT_ALIAS_LOCAL_REMOVE']]:
        subprocess.run(['cc','-O2','-fsanitize=address,undefined',*flags,str(c),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
