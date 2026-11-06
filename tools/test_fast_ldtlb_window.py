#!/usr/bin/env python3
"""Exercise the real fast-LDTLB recovery block at every return-tail boundary."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'dbt/rt/rt.c').read_text()
start=source.index('    if (IN(spc, dbt_tlbfast_a, dbt_tlbfast_end))')
end=source.index('\n#endif',start)
body=source[start:end]
asm=(root/'dbt/rt/stubs.S').read_text()
tail=asm[asm.index('_dbt_tlbfast_a:'):asm.index('_dbt_tlbfast_end:')]
# The model advances the actual three restore instructions in this order.
assert 'STSX    SLOT_Y0, 0' in tail and 'STSX    SLOT_A0, 2' in tail
assert tail.index('STSX    SLOT_Y0, 0') < tail.index('STSX    SLOT_A0, 2') < tail.index('jmp     @r1') < tail.index('STSX    SLOT_Y1, 1')
program=r'''
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
typedef uintptr_t sf_u32;
typedef struct { sf_u32 host_spc,r[16],slots[16],ssr; } dbt_gctx;
static char code[8];
#define dbt_tlbfast_a (code+0)
#define dbt_tlbfast_b (code+2)
#define dbt_tlbfast_c (code+4)
#define dbt_tlbfast_end (code+8)
#define SLOT_Y0 10
#define SLOT_Y1 11
#define SLOT_A0 7
#define IN(spc,a,b) ((spc)>=(sf_u32)(a) && (spc)<(sf_u32)(b))
#define AFTER(spc,l) ((spc)>(sf_u32)(l))
#define UNDO(ctx,reg,slot) ((ctx)->r[reg]=(ctx)->slots[slot])
static int g_window_kind, calls;
static sf_u32 guest_pc_of_host(sf_u32 host,const dbt_gctx *ctx) {
 assert(host==0x8cd01234); ++calls;
 assert(ctx->r[0]==0x11223344 && ctx->r[1]==0x55667788 && ctx->r[2]==0x99aabbcc);
 return 0x00401002;
}
static int recover(dbt_gctx *ctx,sf_u32 *guest_pc) {
 sf_u32 spc=ctx->host_spc;
'''+body+r'''
 return 0;
}
int main(void) {
 for(unsigned t=0;t<2;++t) for(unsigned offset=0;offset<8;offset+=2) {
  dbt_gctx ctx; memset(&ctx,0,sizeof(ctx));
  ctx.host_spc=(sf_u32)code+offset;
  ctx.ssr=0x40001000|t;
  ctx.slots[SLOT_Y0]=0x11223344;ctx.slots[SLOT_Y1]=0x55667788;ctx.slots[SLOT_A0]=0x99aabbcc;
  ctx.r[0]=offset>=2?0x11223344:0xdead0000;
  ctx.r[1]=0x8cd01234;
  ctx.r[2]=offset>=4?0x99aabbcc:0xdead0002;
  for(unsigned i=3;i<16;++i) ctx.r[i]=0x12340000+i;
  sf_u32 pc=0;
  assert(recover(&ctx,&pc)==1 && pc==0x00401002 && g_window_kind==2);
  assert(ctx.ssr==(0x40001000|t));
  for(unsigned i=3;i<16;++i) assert(ctx.r[i]==0x12340000+i);
 }
 dbt_gctx ctx,original; memset(&ctx,0x5a,sizeof(ctx));ctx.host_spc=(sf_u32)code+8;original=ctx;
 sf_u32 pc=0;assert(!recover(&ctx,&pc) && !memcmp(&ctx,&original,sizeof(ctx)) && pc==0);
 assert(calls==8);
 puts("PASS: all fast-LDTLB return boundaries, both T values, outside-window rejection");
}
'''
with tempfile.TemporaryDirectory() as td:
 c=Path(td)/'test.c'; exe=Path(td)/'test';c.write_text(program)
 subprocess.run(['cc','-O2','-fsanitize=address,undefined',str(c),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
