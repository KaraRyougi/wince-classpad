#!/usr/bin/env python3
"""Add an opt-in running RTC for direct emulator HLE boots (no Casio OS).
Usage: python3 enable_rtc_reference.py /path/to/casio-emu
Rebuild emulator, then use HLE_RTC_RUNNING=1 for tick/RTC diagnostics.
"""
from pathlib import Path
import sys
p = Path(sys.argv[1]) / 'src/hardware/rtc/rtc.c'
s = p.read_text()
needle = '  RYRCNT = bcd16(2010);'
if 'HLE_RTC_RUNNING' not in s:
    if needle not in s:
        raise SystemExit('RTC initialization layout not recognized')
    s = s.replace(needle, needle + '\n  /* Opt-in reference clock for direct HLE boots without the Casio OS. */\n  if (getenv("HLE_RTC_RUNNING")) RCR2 = 1;')
    p.write_text(s)
print('RTC reference option ready; rebuild emulator and set HLE_RTC_RUNNING=1')
