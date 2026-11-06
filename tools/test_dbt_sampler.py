#!/usr/bin/env python3
"""Validate host-only sampler layout and opcode extraction with synthetic RAM."""
import runpy
import subprocess
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]
insert = runpy.run_path(str(root / 'dbt/emu-patch/add_dbt_sample.py'))['INSERT']
prefix = r"""
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
void *pageTable[0x100000];
struct {struct {unsigned PC, x0;} reg; int isSleeping;} cpu;
"""
test = r"""
int main(void) {
 unsigned blocks[1024]={0}, counts[1024]={0}, code[1024]={0};
 pageTable[0x8c040000u>>12]=blocks; pageTable[0x8cc0e000u>>12]=counts;
 pageTable[0x8cc80000u>>12]=code;
 blocks[0]=0x83870000; blocks[1]=0x8cc80000; blocks[5]=64;
 counts[0]=1; code[8]=0x61280009; /* swap.b r2,r1; nop */
 setenv("HLE_DBT_SAMPLE","1",1); setenv("HLE_DBT_CACHE_BASE","0x8cc80000",1);
 setenv("HLE_DBT_SAMPLE_BLOCKS","0x8c040000",1);
 setenv("HLE_DBT_SAMPLE_COUNTS","0x8cc0e000",1);
 setenv("HLE_DBT_SAMPLE_PER_BANK","1539",1); setenv("HLE_DBT_SAMPLE_OPS","1",1);
 cpu.reg.PC=0x8cc80020; dbtSampleTick(1);
 cpu.reg.PC=0x8cc80022; dbtSampleTick(2);
 assert(dbtSampleCode==2 && dbtSampleUnmapped==0);
 assert(dbtSampleOps[0x6128]==1 && dbtSampleOps[9]==1);
 cpu.reg.PC=0x8c900040; dbtSampleTick(3); assert(dbtSampleC[1]==1);
 cpu.isSleeping=1; dbtSampleTick(4); assert(dbtSampleSleep==1);
 assert(dbtSampleWord(0x8c040000)==0x83870000);
 assert(dbtSampleWord(0xa4050000)==0);
 for(unsigned stride=32;stride<=36;stride+=4) {
  dbtSampleHeaderBytes=stride; counts[0]=2;
  memset(blocks+8,0,64);
  unsigned index=stride/4, guest=0x83871000+stride*256;
  blocks[index]=guest;blocks[index+1]=0x8cc80040;blocks[index+5]=64;
  blocks[index+4]=0x8c930064;blocks[index+7]=(stride==32?512:2);
  dbtSampleArenaBase=0x8c930000;dbtSampleArenaBankBytes=0x30000;
  cpu.isSleeping=0;cpu.reg.PC=0x8cc80060;dbtSampleTick(stride+5);
  unsigned slot=((guest>>1)^(guest>>16))&32767u;
  assert(dbtSampleGuest[slot].pc==guest && dbtSampleGuest[slot].hits==1 && dbtSampleUnmapped==0);
  dbtSampleDump(); /* both metadata count offsets, plus final exit dump */
 }
 unsigned ctx[1024]={0}, before[1024];
 pageTable[0x8c920000u>>12]=ctx;
 dbtDeviceCtx=0x8c920000u; dbtDeviceLo=0x8c90be84u; dbtDeviceHi=0x8c90c6c4u;
 ctx[9]=0xff000000u; cpu.reg.x0=0x2452u;
 memcpy(before,ctx,sizeof ctx); dbtSampleDevice(dbtDeviceLo);
 assert(!memcmp(before,ctx,sizeof ctx));
 assert(dbtDevice[0].address==0xff000000u && dbtDevice[0].samples==1);
 cpu.reg.x0=0x1454u; dbtSampleDevice(dbtDeviceLo);
 assert(dbtDevice[1].address==0xff000010u && dbtDevice[1].samples==1);
 ctx[5]=12; cpu.reg.x0=0x0456u; dbtSampleDevice(dbtDeviceLo);
 assert(dbtDevice[2].address==0xff00000cu && dbtDevice[2].samples==1);
 cpu.reg.x0=0x2456u; dbtSampleDevice(dbtDeviceLo); assert(dbtDeviceOther==1);
 dbtSampleDevice(dbtDeviceHi); assert(dbtDeviceOther==1);
 /* Exact allocation-pressure trace: thresholds, gating and read-only RAM. */
 dbtTraceEntry=0x8c900100; dbtTraceBankAddress=0x8c920000;
 dbtTraceCodeAddress=0x8c920004; dbtTraceArenaAddress=0x8c920008;
 dbtTraceScratchNeed=2768; counts[0]=1538; ctx[0]=0;
 ctx[1]=0x40000-1024; ctx[2]=dbtSampleArenaBankBytes-2768;
 dbtTraceTick=0;dbtTraceFill();assert(dbtTraceEvents==0);
 dbtTraceTick=100;dbtTraceFill();assert(dbtTraceEvents==0);
 counts[0]++;memcpy(before,ctx,sizeof ctx);dbtTraceFill();assert(dbtTraceEvents==1);
 assert(!memcmp(before,ctx,sizeof ctx) && counts[0]==1539);
 counts[0]--;ctx[1]++;dbtTraceFill();assert(dbtTraceEvents==2);
 ctx[2]++;dbtTraceFill();assert(dbtTraceEvents==3);
 counts[0]++;dbtTraceFill();assert(dbtTraceEvents==4);
 ctx[0]=99;dbtTraceFill();assert(dbtTraceEvents==4);
 ctx[0]=0;dbtTraceEntry=0;dbtTraceFill();assert(dbtTraceEvents==4);
 counts[0]=2;
 return 0;
}
"""
with tempfile.TemporaryDirectory(prefix='dbt-sampler-') as directory:
    source = Path(directory) / 'test.c'
    binary = Path(directory) / 'test'
    source.write_text(prefix + insert + test)
    subprocess.run(['cc', '-Wall', '-Wextra', '-o', str(binary), str(source)], check=True)
    result=subprocess.run([str(binary)], check=True, text=True, capture_output=True)
    assert result.stdout.count("[DBTBANK] bank=0 headers=2 capacity=1539 code_end=128 metadata_end=140") == 3
    for reason in [1,2,6,7]:
        assert result.stdout.count('reasons='+str(reason)+'\n') == 1
    print(result.stdout,end="")
print('sampler layout/opcode PASS')
