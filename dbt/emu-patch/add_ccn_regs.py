#!/usr/bin/env python3
"""Add the software-only CCN registers the Windows CE kernel uses to casio-emu.

casio-emu defines PTEH, PTEL, MMUCR, CCR, TEA, TRA, EXPEVT at 0xFF0000xx but
not TTB (0xFF000008, the kernel's page-directory pointer, read back by its
TLB miss handler) nor PTEA (0xFF000034, written before ldtlb). The CE kernel
writes TTB in SHInit, which the emulator reports as an invalid memory write.
This script adds both as plain storage registers to
src/hardware/cpuRegisters/cpuRegisters.c (backup *.pre-ccn.orig), then you
rebuild: cmake --build build
Usage: add_ccn_regs.py /path/to/casio-emu
"""
import os, shutil, sys

def main(root):
    p = os.path.join(root, 'src', 'hardware', 'cpuRegisters', 'cpuRegisters.c')
    s = open(p).read()
    if 'TTB' in s and '0xff000008' in s:
        print('already patched'); return 0
    if not os.path.exists(p + '.pre-ccn.orig'):
        shutil.copy2(p, p + '.pre-ccn.orig')
    old = 'u32 PTEL = 0;   // Page Table Entry Low  (PPN | flags) - staged before ldtlb\n'
    new = old + ('u32 TTB = 0;    // Translation Table Base: software-only (page directory pointer)\n'
                 'u32 PTEA = 0;   // Page Table Entry Assistance: staged before ldtlb (SA/TC bits, unused here)\n')
    assert old in s, 'PTEL definition not found'
    s = s.replace(old, new)
    old = '  defineReg("Page Table Entry Low", PTEL, 0xff000004);\n'
    new = old + ('  defineReg("Translation Table Base", TTB, 0xff000008);\n'
                 '  defineReg("Page Table Entry Assistance", PTEA, 0xff000034);\n')
    assert old in s, 'PTEL defineReg not found'
    s = s.replace(old, new)
    open(p, 'w').write(s)
    print('patched', p)
    return 0

if __name__ == '__main__':
    if len(sys.argv) != 2: print(__doc__); sys.exit(2)
    sys.exit(main(sys.argv[1]))
