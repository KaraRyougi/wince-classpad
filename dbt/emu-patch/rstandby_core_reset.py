#!/usr/bin/env python3
"""Make casio-emu's ClassPad R-standby wake-up lose the core registers.

On the SH7305, waking from R-standby resets the core: the CPU registers
(r0-r15 of both banks, PR, GBR, VBR, MACH/MACL, SPC, SSR, SGR, DBR, the DSP
registers) are lost (SH7724 manual 18.4.6). casio-emu's powerClasspadWake
only swapped the banks and set SR/PC, so code that relied on a register
surviving passed in the emulator and reset the real fx-CP400 (the
translator's HC_STANDBY returned with rte on the stale SPC/SSR, 2026-11-06).
This patch fills them with a recognisable pattern (0xDEAD00nn) at the
wake-up; the stock OS restores everything from RS memory and is unaffected.
HLE_RSTANDBY_KEEPREGS=1 turns it off. Backup: power.c.pre-corereset.orig.
Then rebuild: cmake --build build
Usage: rstandby_core_reset.py /path/to/casio-emu
"""
import os, shutil, sys

def main(root):
    p = os.path.join(root, 'src', 'hardware', 'power', 'power.c')
    s = open(p).read()
    if 'HLE_RSTANDBY_KEEPREGS' in s:
        print('already patched'); return 0
    if not os.path.exists(p + '.pre-corereset.orig'):
        shutil.copy2(p, p + '.pre-corereset.orig')
    old = '  cpu.reg.SR=0x700000f0u;\n  cpu.reg.PC=BAR;\n'
    assert s.count(old) == 1, 'wake-up SR/PC assignment not found'
    new = ('  /* the core is reset (SH7724 18.4.6): CPU registers are lost; only the\n'
           '   * code at BAR can bring them back (wince-classpad emu-patch) */\n'
           '  if (!getenv("HLE_RSTANDBY_KEEPREGS")) {\n'
           '    u32 *lost[] = {&cpu.reg.PR, &cpu.reg.GBR, &cpu.reg.VBR, &cpu.reg.MACH, &cpu.reg.MACL,\n'
           '                   &cpu.reg.SSR, &cpu.reg.SPC, &cpu.reg.SGR, &cpu.reg.DBR,\n'
           '                   &cpu.reg.RS, &cpu.reg.RE, &cpu.reg.RC, &cpu.reg.x0, &cpu.reg.x1, &cpu.reg.y0, &cpu.reg.y1,\n'
           '                   &cpu.reg.a0, &cpu.reg.a0g, &cpu.reg.a1, &cpu.reg.a1g, &cpu.reg.m0, &cpu.reg.m1};\n'
           '    for (unsigned i=0;i<24;i++) cpu.reg.regArray[i]=0xdead0000u|i;\n'
           '    for (unsigned i=0;i<sizeof lost/sizeof lost[0];i++) *lost[i]=0xdead0080u|i;\n'
           '  }\n') + old
    s = s.replace(old, new)
    if '#include <stdlib.h>' not in s:
        s = '#include <stdlib.h>\n' + s
    open(p, 'w').write(s)
    print('patched', p)
    return 0

if __name__ == '__main__':
    if len(sys.argv) != 2: print(__doc__); sys.exit(2)
    sys.exit(main(sys.argv[1]))
