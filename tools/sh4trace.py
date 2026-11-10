#!/usr/bin/env python3
"""sh4trace.py - run a routine of Casio's OS in a small SH-4 interpreter and
record what it does to the hardware.

    sh4trace.py NOR.bin ENTRY [--r4 N] [--r5 N] [--fill 0|1] [--lcd-id HEX]
                [--stub ADDR=RET ...] [--delay ADDR ...] [--max N] [--quiet-ram]

NOR.bin is a dump of the calculator's NOR flash (the OS at 0x80000000, big
endian). The routine at ENTRY runs until it returns. Every access outside
the ROM and the interpreter's own RAM is "hardware": reads return all zeros
(--fill 0) or all ones (--fill 1) and writes are printed in order. Running a
routine with both fills shows a read-modify-write as its AND and OR masks
(merge_rmw below): with fill 0 the value written is the OR part, with fill 1
it is AND | OR.

The LCD data port 0xB4000000 is modelled just enough: with Port R bit 4 low
a write is a command, otherwise a parameter; a read after command 0xDA
returns --lcd-id (default 16). --stub makes a call to ADDR return RET at
once; --delay prints "delay <r4>" for a call to ADDR instead of running it.

Only what the OS's driver routines use is implemented; an unknown
instruction stops the run with its address.
"""
import struct
import sys

ROM_BASE = 0x80000000
RAM_LO, RAM_HI = 0x8C000000, 0x8D000000
STACK_TOP = 0x8CFF0000
RET_MAGIC = 0xDEAD0000


class Stop(Exception):
    pass


def s8(v):
    return v - 0x100 if v & 0x80 else v


def s12(v):
    return v - 0x1000 if v & 0x800 else v


def s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def s32(v):
    v &= 0xFFFFFFFF
    return v - 0x100000000 if v & 0x80000000 else v


class Machine:
    def __init__(self, rom, fill=0, lcd_id=0x16, quiet_ram=False):
        self.rom = rom
        self.fill = fill
        self.lcd_id = lcd_id
        self.quiet_ram = quiet_ram
        self.r = [0] * 16
        self.pc = 0
        self.pr = RET_MAGIC
        self.t = 0
        self.sr = 0x700000F0
        self.gbr = self.vbr = self.mach = self.macl = 0
        self.bank = [0] * 8         # the other register bank (stc/ldc Rm_BANK)
        self.ram = {}
        self.events = []            # ('w', addr, size, value) | ('r', addr, size) | ('lcd', kind, value) | ('delay', n) | ('call', addr)
        self.stubs = {}
        self.delays = set()
        self.portr = 0xFF           # Port R data register image (bit 4 = LCD RS)
        self.lcd_cmd = None
        self.lcd_reads = 0
        self.steps = 0

    # ---- memory
    def is_rom(self, a):
        a &= 0x1FFFFFFF
        return a < len(self.rom)

    def is_ram(self, a):
        return RAM_LO <= (a & 0xDFFFFFFF | 0x80000000) < RAM_HI and (a >> 28) in (0x8, 0xA)

    def read(self, a, size):
        a &= 0xFFFFFFFF
        if (a >> 28) in (0x8, 0xA) and not self.is_ram(a) and self.is_rom(a):
            o = a & 0x1FFFFFFF
            return int.from_bytes(self.rom[o:o + size], 'big')
        if self.is_ram(a):
            k = a & 0x1FFFFFFF
            v = 0
            known = True
            for i in range(size):
                if (k + i) not in self.ram:
                    known = False
                v = (v << 8) | self.ram.get(k + i, 0)
            if not known and not self.quiet_ram and k < (STACK_TOP & 0x1FFFFFFF) - 0x10000:
                self.events.append(('ramr', a, size))
            return v
        return self.hw_read(a, size)

    def write(self, a, size, v):
        a &= 0xFFFFFFFF
        v &= (1 << (8 * size)) - 1
        if self.is_ram(a):
            k = a & 0x1FFFFFFF
            for i in range(size):
                self.ram[k + i] = (v >> (8 * (size - 1 - i))) & 0xFF
            if k < (STACK_TOP & 0x1FFFFFFF) - 0x10000 and not self.quiet_ram:
                self.events.append(('ramw', a, size, v))
            return
        self.hw_write(a, size, v)

    def hw_read(self, a, size):
        if a == 0xA405013C:
            return self.portr
        if a == 0xB4000000:
            self.lcd_reads += 1
            if self.lcd_cmd == 0xDA:
                v = self.lcd_id
            else:
                v = 0
            self.events.append(('lcd', 'read', self.lcd_cmd))
            return v
        self.events.append(('r', a, size))
        return ((1 << (8 * size)) - 1) if self.fill else 0

    def hw_write(self, a, size, v):
        if a == 0xA405013C:
            self.portr = v
            return
        if a == 0xB4000000:
            if not (self.portr & 0x10):
                self.lcd_cmd = v & 0xFF
                self.lcd_reads = 0
                self.events.append(('lcd', 'cmd', v & 0xFFFF))
            else:
                self.events.append(('lcd', 'data', v & 0xFFFF))
            return
        self.events.append(('w', a, size, v))

    # ---- execution
    def call(self, entry, args=(), max_steps=2000000):
        for i, v in enumerate(args):
            self.r[4 + i] = v & 0xFFFFFFFF
        self.r[15] = STACK_TOP
        self.pc = entry
        self.pr = RET_MAGIC
        while self.pc != RET_MAGIC:
            self.step()
            self.steps += 1
            if self.steps > max_steps:
                raise Stop('step limit at %08x' % self.pc)
        return self.r[0]

    def branch(self, target, delayed=True):
        """Take a branch: run the delay slot first, then go to target or a stub."""
        if delayed:
            pc = self.pc
            self.pc = pc + 2
            self.exec(self.fetch(pc + 2), in_slot=True)
        self.goto(target)

    def goto(self, target):
        target &= 0xFFFFFFFF
        self.pc = target

    def do_call(self, target, ret):
        """jsr/bsr after the delay slot has run."""
        target &= 0xFFFFFFFF
        t = target | 0x80000000 if (target >> 28) == 0xA else target
        if t in self.delays:
            self.events.append(('delay', self.r[4]))
            self.pc = ret
            return
        if t in self.stubs:
            self.events.append(('call', t))
            self.r[0] = self.stubs[t]
            self.pc = ret
            return
        self.pr = ret
        self.pc = target

    def fetch(self, pc):
        return self.read(pc, 2)

    def step(self):
        op = self.fetch(self.pc)
        self.exec(op, in_slot=False)

    def exec(self, op, in_slot):
        r = self.r
        pc = self.pc
        n = (op >> 8) & 0xF
        m = (op >> 4) & 0xF
        d = op & 0xF
        top = op >> 12
        nxt = pc + 2
        M32 = 0xFFFFFFFF

        def adv():
            if not in_slot:
                self.pc = nxt

        if op == 0x0009 or op == 0x00AB:                    # nop, synco
            adv(); return
        if (op & 0xF0FF) in (0x4013, 0x4023, 0x4033, 0x4043):     # stc.l GBR/VBR/SSR/SPC,@-Rn
            v = {0x4013: self.gbr, 0x4023: self.vbr, 0x4033: self.sr, 0x4043: 0}[op & 0xF0FF]
            r[n] = (r[n] - 4) & M32; self.write(r[n], 4, v); adv(); return
        if (op & 0xF0FF) in (0x4017, 0x4027, 0x4037, 0x4047):     # ldc.l @Rn+,GBR/VBR/SSR/SPC
            v = self.read(r[n], 4); r[n] = (r[n] + 4) & M32
            if (op & 0xF0FF) == 0x4017: self.gbr = v
            elif (op & 0xF0FF) == 0x4027: self.vbr = v
            adv(); return
        if (op & 0xF08F) == 0x0082:                         # stc Rm_BANK,Rn
            r[n] = self.bank[(op >> 4) & 7]; adv(); return
        if (op & 0xF08F) == 0x408E:                         # ldc Rn,Rm_BANK
            self.bank[(op >> 4) & 7] = r[n]; adv(); return
        if (op & 0xF08F) == 0x4083:                         # stc.l Rm_BANK,@-Rn
            r[n] = (r[n] - 4) & M32; self.write(r[n], 4, self.bank[(op >> 4) & 7]); adv(); return
        if (op & 0xF08F) == 0x4087:                         # ldc.l @Rn+,Rm_BANK
            self.bank[(op >> 4) & 7] = self.read(r[n], 4); r[n] = (r[n] + 4) & M32; adv(); return
        if top == 0xE:                                      # mov #imm,Rn
            r[n] = s8(op & 0xFF) & M32; adv(); return
        if top == 0x7:                                      # add #imm,Rn
            r[n] = (r[n] + s8(op & 0xFF)) & M32; adv(); return
        if top == 0x9:                                      # mov.w @(disp,PC),Rn
            r[n] = s16(self.read(pc + 4 + (op & 0xFF) * 2, 2)) & M32; adv(); return
        if top == 0xD:                                      # mov.l @(disp,PC),Rn
            r[n] = self.read(((pc + 4) & ~3) + (op & 0xFF) * 4, 4); adv(); return
        if top == 0x1:                                      # mov.l Rm,@(disp,Rn)
            self.write(r[n] + d * 4, 4, r[m]); adv(); return
        if top == 0x5:                                      # mov.l @(disp,Rm),Rn
            r[n] = self.read(r[m] + d * 4, 4); adv(); return
        if top == 0x2:
            if d == 0: self.write(r[n], 1, r[m])
            elif d == 1: self.write(r[n], 2, r[m])
            elif d == 2: self.write(r[n], 4, r[m])
            elif d == 4: r[n] = (r[n] - 1) & M32; self.write(r[n], 1, r[m])
            elif d == 5: r[n] = (r[n] - 2) & M32; self.write(r[n], 2, r[m])
            elif d == 6:
                v = r[m]; r[n] = (r[n] - 4) & M32; self.write(r[n], 4, v)
            elif d == 8: self.t = int((r[n] & r[m]) == 0)
            elif d == 9: r[n] &= r[m]
            elif d == 0xA: r[n] ^= r[m]
            elif d == 0xB: r[n] |= r[m]
            elif d == 0xC:                                  # cmp/str
                x = r[n] ^ r[m]
                self.t = int(any(((x >> s) & 0xFF) == 0 for s in (0, 8, 16, 24)))
            elif d == 0xD: r[n] = ((r[m] << 16) | (r[n] >> 16)) & M32       # xtrct
            elif d == 0xE: self.macl = ((r[n] & 0xFFFF) * (r[m] & 0xFFFF)) & M32    # mulu.w
            elif d == 0xF: self.macl = (s16(r[n]) * s16(r[m])) & M32                # muls.w
            else: raise Stop('unknown %04x at %08x' % (op, pc))
            adv(); return
        if top == 0x6:
            if d == 0: r[n] = s8(self.read(r[m], 1)) & M32
            elif d == 1: r[n] = s16(self.read(r[m], 2)) & M32
            elif d == 2: r[n] = self.read(r[m], 4)
            elif d == 3: r[n] = r[m]
            elif d == 4:
                v = s8(self.read(r[m], 1)) & M32
                if n != m: r[m] = (r[m] + 1) & M32
                r[n] = v
            elif d == 5:
                v = s16(self.read(r[m], 2)) & M32
                if n != m: r[m] = (r[m] + 2) & M32
                r[n] = v
            elif d == 6:
                v = self.read(r[m], 4)
                if n != m: r[m] = (r[m] + 4) & M32
                r[n] = v
            elif d == 7: r[n] = ~r[m] & M32
            elif d == 8: r[n] = ((r[m] & 0xFFFF0000) | ((r[m] & 0xFF) << 8) | ((r[m] >> 8) & 0xFF))   # swap.b
            elif d == 9: r[n] = ((r[m] << 16) | (r[m] >> 16)) & M32                                   # swap.w
            elif d == 0xA:                                  # negc
                v = (0 - r[m] - self.t)
                self.t = int(v < 0)
                r[n] = v & M32
            elif d == 0xB: r[n] = (-r[m]) & M32
            elif d == 0xC: r[n] = r[m] & 0xFF
            elif d == 0xD: r[n] = r[m] & 0xFFFF
            elif d == 0xE: r[n] = s8(r[m] & 0xFF) & M32
            elif d == 0xF: r[n] = s16(r[m]) & M32
            adv(); return
        if top == 0x3:
            a, b = r[n], r[m]
            if d == 0: self.t = int(a == b)
            elif d == 2: self.t = int(a >= b)
            elif d == 3: self.t = int(s32(a) >= s32(b))
            elif d == 6: self.t = int(a > b)
            elif d == 7: self.t = int(s32(a) > s32(b))
            elif d == 8: r[n] = (a - b) & M32
            elif d == 0xC: r[n] = (a + b) & M32
            elif d == 0xA:
                v = a - b - self.t; self.t = int(v < 0); r[n] = v & M32
            elif d == 0xE:
                v = a + b + self.t; self.t = int(v > M32); r[n] = v & M32
            elif d == 5:                                    # dmulu.l
                v = a * b; self.mach = (v >> 32) & M32; self.macl = v & M32
            elif d == 0xD:                                  # dmuls.l
                v = s32(a) * s32(b); self.mach = (v >> 32) & M32; self.macl = v & M32
            else: raise Stop('unknown %04x at %08x' % (op, pc))
            adv(); return
        if top == 0x4:
            lo = op & 0xFF
            if lo == 0x00: self.t = r[n] >> 31; r[n] = (r[n] << 1) & M32
            elif lo == 0x01: self.t = r[n] & 1; r[n] >>= 1
            elif lo == 0x08: r[n] = (r[n] << 2) & M32
            elif lo == 0x09: r[n] >>= 2
            elif lo == 0x18: r[n] = (r[n] << 8) & M32
            elif lo == 0x19: r[n] >>= 8
            elif lo == 0x28: r[n] = (r[n] << 16) & M32
            elif lo == 0x29: r[n] >>= 16
            elif lo == 0x20: self.t = r[n] >> 31; r[n] = (r[n] << 1) & M32      # shal
            elif lo == 0x21: self.t = r[n] & 1; r[n] = (s32(r[n]) >> 1) & M32   # shar
            elif lo == 0x04: self.t = r[n] >> 31; r[n] = ((r[n] << 1) | self.t) & M32   # rotl
            elif lo == 0x05: self.t = r[n] & 1; r[n] = (r[n] >> 1) | (self.t << 31)     # rotr
            elif lo == 0x24:
                t = r[n] >> 31; r[n] = ((r[n] << 1) | self.t) & M32; self.t = t          # rotcl
            elif lo == 0x25:
                t = r[n] & 1; r[n] = (r[n] >> 1) | (self.t << 31); self.t = t            # rotcr
            elif lo == 0x10: r[n] = (r[n] - 1) & M32; self.t = int(r[n] == 0)           # dt
            elif lo == 0x11: self.t = int(s32(r[n]) >= 0)
            elif lo == 0x15: self.t = int(s32(r[n]) > 0)
            elif lo == 0x22: r[n] = (r[n] - 4) & M32; self.write(r[n], 4, self.pr)      # sts.l pr,@-Rn
            elif lo == 0x26: self.pr = self.read(r[n], 4); r[n] = (r[n] + 4) & M32      # lds.l @Rn+,pr
            elif lo == 0x2A: self.pr = r[n]
            elif lo == 0x02: r[n] = (r[n] - 4) & M32; self.write(r[n], 4, self.mach)
            elif lo == 0x12: r[n] = (r[n] - 4) & M32; self.write(r[n], 4, self.macl)
            elif lo == 0x06: self.mach = self.read(r[n], 4); r[n] = (r[n] + 4) & M32
            elif lo == 0x16: self.macl = self.read(r[n], 4); r[n] = (r[n] + 4) & M32
            elif lo == 0x0A: self.mach = r[n]
            elif lo == 0x1A: self.macl = r[n]
            elif lo == 0x0E: self.sr = r[n]                                              # ldc Rn,sr
            elif lo == 0x1E: self.gbr = r[n]
            elif lo == 0x2E: self.vbr = r[n]
            elif lo == 0x03: r[n] = (r[n] - 4) & M32; self.write(r[n], 4, self.sr)
            elif lo == 0x07: self.sr = self.read(r[n], 4); r[n] = (r[n] + 4) & M32
            elif lo == 0x0B:                                                            # jsr @Rn
                target = r[n]
                self.pc = pc + 2
                self.exec(self.fetch(pc + 2), in_slot=True)
                self.do_call(target, pc + 4)
                return
            elif lo == 0x2B:                                                            # jmp @Rn
                target = r[n]
                self.pc = pc + 2
                self.exec(self.fetch(pc + 2), in_slot=True)
                t = target | 0x80000000 if (target >> 28) == 0xA else target
                if t in self.delays or t in self.stubs:                                 # a tail call to a stub
                    self.do_call(target, self.pr)
                else:
                    self.pc = target
                return
            elif (op & 0xF) == 0xC:                                                     # shad Rm,Rn
                sh = s32(r[m])
                if sh >= 0: r[n] = (r[n] << (sh & 31)) & M32
                else:
                    k = (-sh) & 31
                    r[n] = (s32(r[n]) >> (k if k else 31)) & M32 if k else (M32 if r[n] >> 31 else 0)
            elif (op & 0xF) == 0xD:                                                     # shld Rm,Rn
                sh = s32(r[m])
                if sh >= 0: r[n] = (r[n] << (sh & 31)) & M32
                else:
                    k = (-sh) & 31
                    r[n] = (r[n] >> k) if k else 0
            else: raise Stop('unknown %04x at %08x' % (op, pc))
            adv(); return
        if top == 0x0:
            lo = op & 0xFF
            if d == 4: self.write(r[n] + r[0], 1, r[m])
            elif d == 5: self.write(r[n] + r[0], 2, r[m])
            elif d == 6: self.write(r[n] + r[0], 4, r[m])
            elif d == 0xC: r[n] = s8(self.read(r[m] + r[0], 1)) & M32
            elif d == 0xD: r[n] = s16(self.read(r[m] + r[0], 2)) & M32
            elif d == 0xE: r[n] = self.read(r[m] + r[0], 4)
            elif d == 7: self.macl = (r[n] * r[m]) & M32
            elif lo == 0x02: r[n] = self.sr
            elif lo == 0x12: r[n] = self.gbr
            elif lo == 0x22: r[n] = self.vbr
            elif lo == 0x0A: r[n] = self.mach
            elif lo == 0x1A: r[n] = self.macl
            elif lo == 0x2A: r[n] = self.pr
            elif lo == 0x29: r[n] = self.t
            elif op == 0x0008: self.t = 0
            elif op == 0x0018: self.t = 1
            elif op == 0x0028: self.mach = self.macl = 0
            elif op == 0x000B:                                                          # rts
                target = self.pr
                self.pc = pc + 2
                self.exec(self.fetch(pc + 2), in_slot=True)
                self.pc = target
                return
            elif lo == 0x03:                                                            # bsrf Rn
                target = pc + 4 + r[n]
                self.pc = pc + 2
                self.exec(self.fetch(pc + 2), in_slot=True)
                self.do_call(target, pc + 4)
                return
            elif lo == 0x23:                                                            # braf Rn
                target = pc + 4 + r[n]
                self.pc = pc + 2
                self.exec(self.fetch(pc + 2), in_slot=True)
                self.pc = target & M32
                return
            else: raise Stop('unknown %04x at %08x' % (op, pc))
            adv(); return
        if top == 0x8:
            sub = (op >> 8) & 0xF
            imm = op & 0xFF
            if sub == 0x0: self.write(r[m] + d, 1, r[0])
            elif sub == 0x1: self.write(r[m] + d * 2, 2, r[0])
            elif sub == 0x4: r[0] = s8(self.read(r[m] + d, 1)) & M32
            elif sub == 0x5: r[0] = s16(self.read(r[m] + d * 2, 2)) & M32
            elif sub == 0x8: self.t = int(r[0] == (s8(imm) & M32))
            elif sub in (0x9, 0xB, 0xD, 0xF):
                taken = self.t == (1 if sub in (0x9, 0xD) else 0)
                target = pc + 4 + s8(imm) * 2
                if sub in (0xD, 0xF):                       # bt.s / bf.s
                    self.pc = pc + 2
                    self.exec(self.fetch(pc + 2), in_slot=True)
                    self.pc = target if taken else pc + 4
                else:
                    self.pc = target if taken else nxt
                return
            else: raise Stop('unknown %04x at %08x' % (op, pc))
            adv(); return
        if top == 0xA or top == 0xB:                        # bra / bsr
            target = pc + 4 + s12(op & 0xFFF) * 2
            self.pc = pc + 2
            self.exec(self.fetch(pc + 2), in_slot=True)
            if top == 0xB:
                self.do_call(target, pc + 4)
            else:
                t = target & M32
                if t in self.delays or t in self.stubs:
                    self.do_call(target, self.pr)
                else:
                    self.pc = t
            return
        if top == 0xC:
            sub = (op >> 8) & 0xF
            imm = op & 0xFF
            if sub == 0x0: self.write(self.gbr + imm, 1, r[0])
            elif sub == 0x1: self.write(self.gbr + imm * 2, 2, r[0])
            elif sub == 0x2: self.write(self.gbr + imm * 4, 4, r[0])
            elif sub == 0x4: r[0] = s8(self.read(self.gbr + imm, 1)) & M32
            elif sub == 0x5: r[0] = s16(self.read(self.gbr + imm * 2, 2)) & M32
            elif sub == 0x6: r[0] = self.read(self.gbr + imm * 4, 4)
            elif sub == 0x7: r[0] = (((pc + 4) & ~3) + imm * 4) & M32      # mova
            elif sub == 0x8: self.t = int((r[0] & imm) == 0)
            elif sub == 0x9: r[0] &= imm
            elif sub == 0xA: r[0] ^= imm
            elif sub == 0xB: r[0] |= imm
            else: raise Stop('unknown %04x at %08x' % (op, pc))
            adv(); return
        raise Stop('unknown %04x at %08x' % (op, pc))


def trace(rom, entry, args=(), fill=0, lcd_id=0x16, stubs=None, delays=(), max_steps=2000000, quiet_ram=True, ram=None):
    m = Machine(rom, fill=fill, lcd_id=lcd_id, quiet_ram=quiet_ram)
    m.stubs = dict(stubs or {})
    m.delays = set(delays)
    for a, (size, v) in (ram or {}).items():
        m.write(a, size, v)
    m.events = []
    m.call(entry, args, max_steps)
    return m.events


def merge_rmw(ev0, ev1):
    """The two traces of one routine (fill 0 and fill 1) as a list of
    ('op', addr, size, and_mask, or_mask) for hardware writes, with the other
    events passed through. A plain write has and_mask 0."""
    out = []
    if len(ev0) != len(ev1):
        raise Stop('the two runs took different paths (%d and %d events)' % (len(ev0), len(ev1)))
    for a, b in zip(ev0, ev1):
        if a[0] != b[0]:
            raise Stop('the two runs differ: %r / %r' % (a, b))
        if a[0] == 'w':
            if a[1:3] != b[1:3]:
                raise Stop('the two runs differ: %r / %r' % (a, b))
            mask = (1 << (8 * a[2])) - 1
            orv = a[3]
            andv = (b[3] & ~orv) & mask
            out.append(('op', a[1], a[2], andv, orv))
        else:
            out.append(a)
    return out


def fmt(e):
    k = e[0]
    if k == 'op':
        _, a, size, andv, orv = e
        w = size * 2
        if andv == 0:
            return 'write%-2d %08X = %0*X' % (size * 8, a, w, orv)
        return 'rmw%-2d   %08X = (old & %0*X) | %0*X' % (size * 8, a, w, andv, w, orv)
    if k == 'w':
        return 'write%-2d %08X = %0*X' % (e[2] * 8, e[1], e[2] * 2, e[3])
    if k == 'r':
        return 'read%-2d  %08X' % (e[2] * 8, e[1])
    if k == 'lcd':
        if e[1] == 'read':
            return 'lcd read (after command %02X)' % (e[2] if e[2] is not None else 0)
        return 'lcd %-4s %04X' % (e[1], e[2])
    if k == 'delay':
        return 'delay %d' % e[1]
    if k == 'call':
        return 'call %08X (stubbed)' % e[1]
    if k == 'ramr':
        return 'ram read%d %08X (never written here)' % (e[2] * 8, e[1])
    if k == 'ramw':
        return 'ram write%d %08X = %X' % (e[2] * 8, e[1], e[3])
    return repr(e)


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    rom = open(argv[1], 'rb').read()
    entry = int(argv[2], 16)
    args = [0, 0, 0, 0]
    fill = None
    lcd_id = 0x16
    stubs, delays = {}, set()
    max_steps = 2000000
    quiet = False
    i = 3
    while i < len(argv):
        a = argv[i]
        if a in ('--r4', '--r5', '--r6', '--r7'):
            args[int(a[3]) - 4] = int(argv[i + 1], 0); i += 2
        elif a == '--fill':
            fill = int(argv[i + 1]); i += 2
        elif a == '--lcd-id':
            lcd_id = int(argv[i + 1], 16); i += 2
        elif a == '--stub':
            k, v = argv[i + 1].split('=')
            stubs[int(k, 16)] = int(v, 0); i += 2
        elif a == '--delay':
            delays.add(int(argv[i + 1], 16)); i += 2
        elif a == '--max':
            max_steps = int(argv[i + 1]); i += 2
        elif a == '--quiet-ram':
            quiet = True; i += 1
        else:
            print('unknown option', a)
            return 2
    try:
        if fill is None:
            e0 = trace(rom, entry, args, 0, lcd_id, stubs, delays, max_steps, quiet)
            e1 = trace(rom, entry, args, 1, lcd_id, stubs, delays, max_steps, quiet)
            ev = merge_rmw(e0, e1)
        else:
            ev = trace(rom, entry, args, fill, lcd_id, stubs, delays, max_steps, quiet)
    except Stop as s:
        print('stopped:', s)
        return 1
    for e in ev:
        if e[0] == 'r':
            continue
        print(fmt(e))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
