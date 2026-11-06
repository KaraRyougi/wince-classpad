#!/usr/bin/env python3
"""Add the SH7305 SDRAM mode-register command address to casio-emu.

The stock fx-CP400 OS re-initialises its SDRAM controller after R-standby
with (docs/06-deep-suspend.md section 3.6) RTCSR/SDCR/RTCNT/RTCOR writes at
0xFEC10044..50, which casio-emu already stores, and a 16-bit write to
0xFEC15040: the SDRAM mode register, address-encoded like the SH7720's SDMR
area. casio-emu only backs that page under HLE_COLDBOOT; the translator's
resume stub (rt/standby.S) issues the same write, so define it as a register
that accepts the value. Backup: cpuRegisters.c.pre-sdmr.orig. Then rebuild:
cmake --build build
Usage: add_sdmr.py /path/to/casio-emu
"""
import os, shutil, sys

def main(root):
    p = os.path.join(root, 'src', 'hardware', 'cpuRegisters', 'cpuRegisters.c')
    s = open(p).read()
    if '0xfec15040' in s:
        print('already patched'); return 0
    if not os.path.exists(p + '.pre-sdmr.orig'):
        shutil.copy2(p, p + '.pre-sdmr.orig')
    old = '  defineReg("SDRAM control", SDCR, 0xfec10044);\n'
    assert old in s, 'SDCR defineReg not found'
    new = old + ('  /* SDRAM mode register: the MRS command is the address, the data is ignored\n'
                 '   * (SH7720-style SDMR area; the OS writes 0x40 here after R-standby). */\n'
                 '  defineRegCB("SDRAM mode register", sdmrRead, sdmrWrite, 0xfec15040, 2);\n')
    s = s.replace(old, new)
    old = 'u32 SDCR'
    i = s.index(old); j = s.index('\n', i)
    s = s[:j + 1] + ('static u32 SDMR = 0;\n'
                     'static u32 sdmrRead(u32 address) { (void)address; return SDMR; }\n'
                     'static void sdmrWrite(u32 address, u32 value) { (void)address; SDMR = value; }\n') + s[j + 1:]
    open(p, 'w').write(s)
    print('patched', p)
    return 0

if __name__ == '__main__':
    if len(sys.argv) != 2: print(__doc__); sys.exit(2)
    sys.exit(main(sys.argv[1]))
