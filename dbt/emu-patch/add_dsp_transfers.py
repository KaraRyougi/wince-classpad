#!/usr/bin/env python3
"""Add the SH-DSP CPU<->DSP transfer instructions to casio-emu.

The translator uses A0/X0/X1/Y0/Y1 as spill slots via
    lds Rm,{A0,X0,X1,Y0,Y1}   0100mmmm {0111,1000,1001,1010,1011} 1010
    sts {A0,X0,X1,Y0,Y1},Rn   0000nnnn {0111,1000,1001,1010,1011} 1010
casio-emu only implements `lds rm, x0`. This script adds the other nine to
scripts/instructions.json and src/generated_instructions.h (functions and the
65536-entry handler table), keeping *.pre-dsp.orig backups, then you run
    node scripts/genDispatch.mjs && cmake --build build
Usage: add_dsp_transfers.py /path/to/casio-emu
"""
import json, re, shutil, sys, os

def main(root):
    jp = os.path.join(root, 'scripts', 'instructions.json')
    hp = os.path.join(root, 'src', 'generated_instructions.h')
    for p in (jp, hp):
        if not os.path.exists(p + '.pre-dsp.orig'):
            shutil.copy2(p, p + '.pre-dsp.orig')
    regs = [('x0', '1000', 41), ('x1', '1001', 42), ('y0', '1010', 43), ('y1', '1011', 44), ('a0', '0111', 45)]
    mreg = {"type": "register", "isConstant": False, "mask": ["0000", "1111", "0000", "0000"], "shift": 8,
            "indirect": False, "preDecrement": False, "postIncrement": False}

    d = json.load(open(jp))
    names = {it['name'] for it in d}
    nid = max(it['id'] for it in d) + 1
    added = []
    for r, code, const in regs:
        n = 'lds rm, %s' % r
        if n not in names:
            d.append({"id": nid, "name": n, "family": "lds", "formatString": "TODO", "opcode": ["0100", None, code, "1010"],
                      "operands": {"m": mreg, r: {"type": "register", "isConstant": True, "constantValue": const}},
                      "delaySlotVariant": False, "instructionSize": 4}); nid += 1; added.append(n)
        n = 'sts %s, rn' % r
        if n not in names:
            d.append({"id": nid, "name": n, "family": "sts", "formatString": "TODO", "opcode": ["0000", None, code, "1010"],
                      "operands": {r: {"type": "register", "isConstant": True, "constantValue": const}, "n": mreg},
                      "delaySlotVariant": False, "instructionSize": 4}); nid += 1; added.append(n)
    json.dump(d, open(jp, 'w'), indent=2)
    print("instructions.json: added", added)

    s = open(hp).read()
    funcs = []
    for r, code, const in regs:
        if 'void autogen_lds_rm_%s(' % r not in s:
            funcs.append('// lds rm, %s\nvoid autogen_lds_rm_%s(u16 instr) {\n  #ifdef PRINT_INSTRUCTIONS\n  printf("lds rm, %s\\n");\n  #endif\n'
                         '  u32 m = (instr & 0b0000111100000000) >> 8;\n  u32 %s = %d;\n  u32 src = cpu.reg.regArray[m];\n'
                         '  u32 dst = cpu.reg.regArray[%s];\n  u32 result = lds(instr, src, dst);\n  cpu.reg.regArray[%s] = result;\n}\n\n'
                         % (r, r, r, r, const, r, r))
        if 'void autogen_sts_%s_rn(' % r not in s:
            funcs.append('// sts %s, rn\nvoid autogen_sts_%s_rn(u16 instr) {\n  #ifdef PRINT_INSTRUCTIONS\n  printf("sts %s, rn\\n");\n  #endif\n'
                         '  u32 %s = %d;\n  u32 n = (instr & 0b0000111100000000) >> 8;\n  u32 src = cpu.reg.regArray[%s];\n'
                         '  u32 dst = cpu.reg.regArray[n];\n  u32 result = sts(instr, src, dst);\n  cpu.reg.regArray[n] = result;\n}\n\n'
                         % (r, r, r, r, const, r))
    anchor = s.index('// lds rm, x0\nvoid autogen_lds_rm_x0')
    s = s[:anchor] + ''.join(funcs) + s[anchor:]
    m = re.search(r'(void \(\*instructionHandlers\[65536\]\)\(u16\) = \{)([\s\S]*?)(\};)', s)
    entries = [x.strip() for x in m.group(2).split(',') if x.strip()]
    assert len(entries) == 65536
    chg = 0
    for r, code, const in regs:
        c = int(code, 2)
        for x in range(16):
            i = 0x4000 | (x << 8) | (c << 4) | 0xA; nm = 'autogen_lds_rm_%s' % r
            if entries[i] != nm: entries[i] = nm; chg += 1
            i = (x << 8) | (c << 4) | 0xA; nm = 'autogen_sts_%s_rn' % r
            if entries[i] != nm: entries[i] = nm; chg += 1
    body = '\n' + ',\n'.join('  ' + e for e in entries) + '\n'
    s = s[:m.start(2)] + body + s[m.end(2):]
    open(hp, 'w').write(s)
    print("generated_instructions.h: functions added", len(funcs), "table entries changed", chg)
    print("now run: node scripts/genDispatch.mjs && cmake --build build")

if __name__ == '__main__':
    if len(sys.argv) != 2:
        print(__doc__); sys.exit(2)
    main(sys.argv[1])
