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
/* Optional block-owner attribution. Exact ELF symbols and MAX_BLOCKS must
 * be supplied by the caller. Read backing RAM directly: do not perform
 * emulated MMIO/cache/TLB operations or change guest timing. */
extern void *pageTable[0x100000];
static unsigned dbtSampleBlocks, dbtSampleCounts, dbtSamplePerBank, dbtSampleUnmapped;
/* Optional raw host-RAM counters, snapshotted only at interval boundaries.
 * The exact ELF address/layout must accompany the report. */
static unsigned dbtCounterBase, dbtCounterWords, dbtCounterStarted;
static unsigned dbtCounterFirst[512];
static struct { unsigned pc, hits; } dbtSampleGuest[32768];
static unsigned dbtSampleWord(unsigned address) {
  if(address<0x8c900000u || address>=0x8cd00000u || (address&3))return 0;
  const unsigned *page=(const unsigned *)pageTable[address>>12];
  return page ? page[(address&4095u)>>2] : 0;
}
static void dbtSampleGuestPc(unsigned pc) {
  if(!dbtSampleBlocks || !dbtSampleCounts || !dbtSamplePerBank)return;
  unsigned bank=(pc-0x8cd00000u)/0x40000u;
  unsigned count=dbtSampleWord(dbtSampleCounts+bank*4);
  if(count>dbtSamplePerBank){dbtSampleUnmapped++;return;}
  unsigned lo=bank*dbtSamplePerBank, hi=lo+count;
  while(lo<hi){
    unsigned mid=lo+(hi-lo)/2, block=dbtSampleBlocks+mid*36;
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
  printf("[DBTSAMPLE] total=%u sleep=%u code=%u other=%u\n", dbtSampleTotal, dbtSampleSleep, dbtSampleCode, dbtSampleOther);
  for (unsigned i=0;i<65536;i++) if (dbtSampleC[i])
    printf("[DBTSAMPLE pc] %08x %u\n", 0x8c900000u+i*64u, dbtSampleC[i]);
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
    s=getenv("HLE_DBT_SAMPLE_BLOCKS");if(s)dbtSampleBlocks=(unsigned)strtoul(s,0,0);
    s=getenv("HLE_DBT_SAMPLE_COUNTS");if(s)dbtSampleCounts=(unsigned)strtoul(s,0,0);
    s=getenv("HLE_DBT_SAMPLE_PER_BANK");if(s)dbtSamplePerBank=(unsigned)strtoul(s,0,0);
    s=getenv("HLE_DBT_COUNTER_BASE");if(s)dbtCounterBase=(unsigned)strtoul(s,0,0);
    s=getenv("HLE_DBT_COUNTER_WORDS");if(s)dbtCounterWords=(unsigned)strtoul(s,0,0);
    if(dbtCounterWords>512 || !dbtCounterBase || (dbtCounterBase&3) ||
       dbtCounterBase<0x8c900000u || dbtCounterBase>=0x8cd00000u ||
       dbtCounterWords>(0x8cd00000u-dbtCounterBase)/4)dbtCounterWords=0;
    initialized=1;
  }
  if (!dbtSampleStart || tick<dbtSampleStart) return;
  if(dbtCounterWords && !dbtCounterStarted){
    for(unsigned i=0;i<dbtCounterWords;i++)dbtCounterFirst[i]=dbtSampleWord(dbtCounterBase+i*4);
    dbtCounterStarted=1;
  }
  unsigned pc=cpu.reg.PC;
  dbtSampleTotal++;
  if (cpu.isSleeping) dbtSampleSleep++;
  else if (pc>=0x8c900000u && pc<0x8cd00000u) dbtSampleC[(pc-0x8c900000u)>>6]++;
  else if (pc>=0x8cd00000u && pc<0x8d000000u) { dbtSampleCode++; dbtSampleGuestPc(pc); }
  else dbtSampleOther++;
}
"""

def main():
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
