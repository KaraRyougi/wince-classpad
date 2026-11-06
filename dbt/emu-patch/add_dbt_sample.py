#!/usr/bin/env python3
"""Add optional host-side PC/sleep sampling without changing emulated time.

Usage: python3 add_dbt_sample.py /path/to/casio-emu
Run with HLE_DBT_SAMPLE=<first timer tick>; results print on normal exit.
Samples are one per timer quantum, not exact instruction-time attribution.
"""
from pathlib import Path
import sys

INSERT = r"""/* Host-only diagnostic: never alters guest registers, clocks or RAM. */
static unsigned dbtSampleStart, dbtSampleTotal, dbtSampleSleep, dbtSampleCode, dbtSampleOther;
static unsigned dbtSampleC[65536];
static unsigned dbtSampleOpsEnabled, dbtSampleOps[65536];
static unsigned dbtSampleCacheBase = 0x8cd00000u;
/* Optional block-owner attribution. Exact ELF symbols and MAX_BLOCKS must
 * be supplied by the caller. Read backing RAM directly: do not perform
 * emulated MMIO/cache/TLB operations or change guest timing. */
extern void *pageTable[0x100000];
static unsigned dbtSampleBlocks, dbtSampleCounts, dbtSamplePerBank, dbtSampleUnmapped;
static unsigned dbtSampleHeaderBytes = 36;
static unsigned dbtSampleArenaBase, dbtSampleArenaBankBytes;
/* Exact fill-entry trace, gated in the host interpreter. Reads only backing
 * RAM, so observing allocation pressure adds no guest instructions. */
unsigned dbtTraceEntry;
static unsigned dbtTraceBankAddress, dbtTraceCodeAddress, dbtTraceArenaAddress;
static unsigned dbtTraceScratchNeed, dbtTraceTick, dbtTraceEvents;

/* Optional raw host-RAM counters, snapshotted only at interval boundaries.
 * The exact ELF address/layout must accompany the report. */
static unsigned dbtCounterBase, dbtCounterWords, dbtCounterStarted;
static unsigned dbtCounterFirst[512];
static struct { unsigned pc, hits; } dbtSampleGuest[32768];
static unsigned dbtDeviceCtx, dbtDeviceLo, dbtDeviceHi, dbtDeviceOther;
static struct {unsigned address, samples;} dbtDevice[64];
static unsigned dbtSampleWord(unsigned address) {
  if(address<0x8c000000u || address>=0x8d000000u || (address&3))return 0;
  const unsigned *page=(const unsigned *)pageTable[address>>12];
  return page ? page[(address&4095u)>>2] : 0;
}
void dbtTraceFill(void) {
  if(!dbtTraceEntry || !dbtSampleStart || dbtTraceTick<dbtSampleStart ||
     !dbtSampleCounts || !dbtSamplePerBank || !dbtSampleArenaBankBytes || !dbtTraceScratchNeed)return;
  unsigned bank=dbtSampleWord(dbtTraceBankAddress);
  unsigned banks=(0x8d000000u-dbtSampleCacheBase)/0x40000u;
  if(bank>=banks)return;
  unsigned count=dbtSampleWord(dbtSampleCounts+4*bank);
  unsigned code=dbtSampleWord(dbtTraceCodeAddress);
  unsigned arena=dbtSampleWord(dbtTraceArenaAddress);
  unsigned reasons=(count>=dbtSamplePerBank ? 1u : 0u) |
    (code+1024u>(bank+1)*0x40000u ? 2u : 0u) |
    (arena+dbtTraceScratchNeed>(bank+1)*dbtSampleArenaBankBytes ? 4u : 0u);
  if(reasons) {
    ++dbtTraceEvents;
    printf("[DBTFILL] tick=%u bank=%u headers=%u code=%u metadata=%u reasons=%u\n",
      dbtTraceTick,bank,count,code-bank*0x40000u,arena-bank*dbtSampleArenaBankBytes,reasons);
  }
}
/* Residency samples inside the exact fallback function, not call counts.
 * Only stores without base writeback: a predecrement could already have
 * changed the saved base when sampled, so it cannot be reconstructed here.
 * dbt_gctx: r[0] at +20. SAVE_CTX_RT does not save DSP slots; the
 * helper argument remains in live X0 while masked native C executes. */
static void dbtSampleDevice(unsigned pc) {
  if(!dbtDeviceCtx || pc<dbtDeviceLo || pc>=dbtDeviceHi)return;
  unsigned op=cpu.reg.x0&65535u;
  unsigned n=(op>>8)&15u, ea;
  if((op&0xf00fu)==0x2002u)ea=dbtSampleWord(dbtDeviceCtx+20+4*n);
  else if((op&0xf000u)==0x1000u)ea=dbtSampleWord(dbtDeviceCtx+20+4*n)+4*(op&15u);
  else if((op&0xf00fu)==0x0006u)ea=dbtSampleWord(dbtDeviceCtx+20+4*n)+dbtSampleWord(dbtDeviceCtx+20);
  else {dbtDeviceOther++;return;}
  if((ea>>24)!=255u){dbtDeviceOther++;return;}
  for(unsigned i=0;i<64;i++)if(!dbtDevice[i].samples || dbtDevice[i].address==ea){
    dbtDevice[i].address=ea;dbtDevice[i].samples++;return;
  }
  dbtDeviceOther++;
}
static void dbtSampleGuestPc(unsigned pc) {
  if(!dbtSampleBlocks || !dbtSampleCounts || !dbtSamplePerBank)return;
  unsigned bank=(pc-dbtSampleCacheBase)/0x40000u;
  unsigned count=dbtSampleWord(dbtSampleCounts+bank*4);
  if(count>dbtSamplePerBank){dbtSampleUnmapped++;return;}
  unsigned lo=bank*dbtSamplePerBank, hi=lo+count;
  while(lo<hi){
    unsigned mid=lo+(hi-lo)/2, block=dbtSampleBlocks+mid*dbtSampleHeaderBytes;
    unsigned base=dbtSampleWord(block+4), length=dbtSampleWord(block+20)&65535u;
    if(pc<base)hi=mid;
    else if(pc-base>=length)lo=mid+1;
    else {
      unsigned guest=dbtSampleWord(block), slot=((guest>>1)^(guest>>16))&32767u;
      for(unsigned n=0;n<32768;n++,slot=(slot+1)&32767u){
        if(!dbtSampleGuest[slot].hits || dbtSampleGuest[slot].pc==guest){
          dbtSampleGuest[slot].pc=guest;dbtSampleGuest[slot].hits++;return;
        }
      }
      break;
    }
  }
  dbtSampleUnmapped++;
}
static void dbtSampleDump(void) {
  if (!dbtSampleStart) return;
  if(dbtCounterStarted)for(unsigned i=0;i<dbtCounterWords;i++){
    unsigned last=dbtSampleWord(dbtCounterBase+i*4);
    printf("[DBTCOUNTER] %08x first=%u last=%u delta=%u\n",dbtCounterBase+i*4,dbtCounterFirst[i],last,last-dbtCounterFirst[i]);
  }
  if(dbtDeviceCtx){
    for(unsigned i=0;i<64;i++)if(dbtDevice[i].samples)
      printf("[DBTDEVICE sample] %08x %u\n",dbtDevice[i].address,dbtDevice[i].samples);
    printf("[DBTDEVICE other] %u\n",dbtDeviceOther);
  }
  if(dbtSampleBlocks && dbtSampleCounts && dbtSamplePerBank) {
    unsigned banks=(0x8d000000u-dbtSampleCacheBase)/0x40000u;
    for(unsigned bank=0;bank<banks;bank++) {
      unsigned count=dbtSampleWord(dbtSampleCounts+bank*4);
      if(!count || count>dbtSamplePerBank)continue;
      unsigned last=dbtSampleBlocks+(bank*dbtSamplePerBank+count-1)*dbtSampleHeaderBytes;
      unsigned base=dbtSampleWord(last+4), length=dbtSampleWord(last+20)&65535u;
      unsigned links=dbtSampleWord(last+16), fields=dbtSampleWord(last+28);
      unsigned nlinks=(fields>>(dbtSampleHeaderBytes==32?8:0))&255u;
      unsigned meta_end=dbtSampleArenaBase ? links+nlinks*20-dbtSampleArenaBase-bank*dbtSampleArenaBankBytes : 0;
      printf("[DBTBANK] bank=%u headers=%u capacity=%u code_end=%u metadata_end=%u\n",bank,count,dbtSamplePerBank,base+length-dbtSampleCacheBase-bank*0x40000u,meta_end);
    }
  }
  printf("[DBTSAMPLE] total=%u sleep=%u code=%u other=%u\n", dbtSampleTotal, dbtSampleSleep, dbtSampleCode, dbtSampleOther);
  for (unsigned i=0;i<65536;i++) if (dbtSampleC[i])
    printf("[DBTSAMPLE pc] %08x %u\n", 0x8c900000u+i*64u, dbtSampleC[i]);
  if(dbtSampleOpsEnabled)for(unsigned i=0;i<65536;i++)if(dbtSampleOps[i])
    printf("[DBTSAMPLE opcode] %04x %u\n",i,dbtSampleOps[i]);
  if(dbtSampleBlocks){
    printf("[DBTSAMPLE guest-unmapped] %u\n",dbtSampleUnmapped);
    for(unsigned i=0;i<32768;i++)if(dbtSampleGuest[i].hits)
      printf("[DBTSAMPLE guest] %08x %u\n",dbtSampleGuest[i].pc,dbtSampleGuest[i].hits);
  }
}
static void dbtSampleTick(unsigned tick) {
  static int initialized;
  if (!initialized) {
    const char *s=getenv("HLE_DBT_SAMPLE");
    if (s) { dbtSampleStart=(unsigned)strtoul(s,0,0); atexit(dbtSampleDump); }
    s=getenv("HLE_DBT_SAMPLE_OPS");if(s)dbtSampleOpsEnabled=(unsigned)strtoul(s,0,0);
    s=getenv("HLE_DBT_CACHE_BASE");if(s)dbtSampleCacheBase=(unsigned)strtoul(s,0,0);
    if(dbtSampleCacheBase<0x8c900000u || dbtSampleCacheBase>0x8cd00000u ||
       (dbtSampleCacheBase&0x3ffffu)) { fprintf(stderr,"invalid HLE_DBT_CACHE_BASE\n"); exit(2); }
    s=getenv("HLE_DBT_SAMPLE_BLOCKS");if(s)dbtSampleBlocks=(unsigned)strtoul(s,0,0);
    s=getenv("HLE_DBT_SAMPLE_COUNTS");if(s)dbtSampleCounts=(unsigned)strtoul(s,0,0);
    s=getenv("HLE_DBT_ARENA_BASE");if(s)dbtSampleArenaBase=(unsigned)strtoul(s,0,0);
    s=getenv("HLE_DBT_ARENA_BANK_BYTES");if(s)dbtSampleArenaBankBytes=(unsigned)strtoul(s,0,0);
    s=getenv("HLE_DBT_SAMPLE_HEADER_BYTES");if(s)dbtSampleHeaderBytes=(unsigned)strtoul(s,0,0);
    if(dbtSampleHeaderBytes!=32 && dbtSampleHeaderBytes!=36){fprintf(stderr,"invalid HLE_DBT_SAMPLE_HEADER_BYTES\n");exit(2);}
    s=getenv("HLE_DBT_SAMPLE_PER_BANK");if(s)dbtSamplePerBank=(unsigned)strtoul(s,0,0);
    s=getenv("HLE_DBT_COUNTER_BASE");if(s)dbtCounterBase=(unsigned)strtoul(s,0,0);
    s=getenv("HLE_DBT_COUNTER_WORDS");if(s)dbtCounterWords=(unsigned)strtoul(s,0,0);
    if(dbtCounterWords>512 || !dbtCounterBase || (dbtCounterBase&3) ||
       dbtCounterBase<0x8c900000u || dbtCounterBase>=0x8cd00000u ||
       dbtCounterWords>(0x8cd00000u-dbtCounterBase)/4)dbtCounterWords=0;
    s=getenv("HLE_DBT_DEVICE_CTX");if(s)dbtDeviceCtx=(unsigned)strtoul(s,0,0);
    s=getenv("HLE_DBT_DEVICE_LO");if(s)dbtDeviceLo=(unsigned)strtoul(s,0,0);
    s=getenv("HLE_DBT_DEVICE_HI");if(s)dbtDeviceHi=(unsigned)strtoul(s,0,0);
    if(dbtDeviceCtx<0x8c900000u || dbtDeviceCtx>0x8cffff40u || (dbtDeviceCtx&3) ||
       dbtDeviceLo<0x8c900000u || dbtDeviceHi>dbtSampleCacheBase || dbtDeviceLo>=dbtDeviceHi)dbtDeviceCtx=0;
    s=getenv("HLE_DBT_TRACE_ENTRY");if(s)dbtTraceEntry=(unsigned)strtoul(s,0,0);
    s=getenv("HLE_DBT_TRACE_BANK");if(s)dbtTraceBankAddress=(unsigned)strtoul(s,0,0);
    s=getenv("HLE_DBT_TRACE_CODE");if(s)dbtTraceCodeAddress=(unsigned)strtoul(s,0,0);
    s=getenv("HLE_DBT_TRACE_ARENA");if(s)dbtTraceArenaAddress=(unsigned)strtoul(s,0,0);
    s=getenv("HLE_DBT_TRACE_SCRATCH");if(s)dbtTraceScratchNeed=(unsigned)strtoul(s,0,0);
    initialized=1;
  }
  dbtTraceTick=tick;
  if (!dbtSampleStart || tick<dbtSampleStart) return;
  if(dbtCounterWords && !dbtCounterStarted){
    for(unsigned i=0;i<dbtCounterWords;i++)dbtCounterFirst[i]=dbtSampleWord(dbtCounterBase+i*4);
    dbtCounterStarted=1;
  }
  unsigned pc=cpu.reg.PC;
  dbtSampleDevice(pc);
  dbtSampleTotal++;
  if (cpu.isSleeping) dbtSampleSleep++;
  else if (pc>=0x8c900000u && pc<dbtSampleCacheBase) dbtSampleC[(pc-0x8c900000u)>>6]++;
  else if (pc>=dbtSampleCacheBase && pc<0x8d000000u) {
    dbtSampleCode++; dbtSampleGuestPc(pc);
    if(dbtSampleOpsEnabled) {
      unsigned word=dbtSampleWord(pc&~3u);
      unsigned opcode=(pc&2u) ? (word&65535u) : (word>>16);
      dbtSampleOps[opcode]++;
    }
  }
  else dbtSampleOther++;
}
"""

def main():
    interpreter = Path(sys.argv[1]) / 'src/interpreter.c'
    code = interpreter.read_text()
    if 'dbtTraceFill();' not in code:
        anchor = 'fetch_instruction:\n'
        if code.count(anchor) != 1: raise SystemExit('instruction fetch anchor changed')
        code = code.replace(anchor, anchor + '    /* Host-only DBT fill trace. */\n'
            '    { extern unsigned dbtTraceEntry; extern void dbtTraceFill(void);\n'
            '      if(dbtTraceEntry && cpu.reg.PC==dbtTraceEntry)dbtTraceFill(); }\n')
        interpreter.write_text(code)
    path = Path(sys.argv[1]) / 'src/hardware/timers/timers.c'
    source = path.read_text()
    if 'static void dbtSampleTick(' in source:
        start = source.index('/* Host-only diagnostic:')
        end = source.index('\nextern void setKeydown(', start)
        source = source[:start] + INSERT + source[end:]
        path.write_text(source)
        return
    anchor = 'extern CpuState cpu;'
    tick = '  count++;\n  hleTimerTick();'
    if anchor not in source or tick not in source:
        raise SystemExit('timer source anchors changed')
    source = source.replace(anchor, anchor + '\n' + INSERT, 1)
    source = source.replace(tick, '  count++;\n  dbtSampleTick(count);\n  hleTimerTick();', 1)
    path.write_text(source)

if __name__ == '__main__':
    main()
