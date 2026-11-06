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
static void dbtSampleDump(void) {
  if (!dbtSampleStart) return;
  printf("[DBTSAMPLE] total=%u sleep=%u code=%u other=%u\n", dbtSampleTotal, dbtSampleSleep, dbtSampleCode, dbtSampleOther);
  for (unsigned i=0;i<65536;i++) if (dbtSampleC[i])
    printf("[DBTSAMPLE pc] %08x %u\n", 0x8c900000u+i*64u, dbtSampleC[i]);
}
static void dbtSampleTick(unsigned tick) {
  static int initialized;
  if (!initialized) {
    const char *s=getenv("HLE_DBT_SAMPLE");
    if (s) { dbtSampleStart=(unsigned)strtoul(s,0,0); atexit(dbtSampleDump); }
    initialized=1;
  }
  if (!dbtSampleStart || tick<dbtSampleStart) return;
  unsigned pc=cpu.reg.PC;
  dbtSampleTotal++;
  if (cpu.isSleeping) dbtSampleSleep++;
  else if (pc>=0x8c900000u && pc<0x8cd00000u) dbtSampleC[(pc-0x8c900000u)>>6]++;
  else if (pc>=0x8cd00000u && pc<0x8d000000u) dbtSampleCode++;
  else dbtSampleOther++;
}
"""

def main():
    path = Path(sys.argv[1]) / 'src/hardware/timers/timers.c'
    source = path.read_text()
    if 'static void dbtSampleTick(' in source:
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
