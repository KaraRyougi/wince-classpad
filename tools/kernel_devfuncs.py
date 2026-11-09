#!/usr/bin/env python3
"""Find the kernel.dll functions that may access P4 (on-chip control
registers) and emit dbt/rt/kernel_dev.h: one relocation-insensitive
signature per function, which the translator looks for in the image at boot
(rt/kdev.c) so that only those functions get the device-aware run-time test.

usage: kernel_devfuncs.py NK.bin kern.map [-o kernel_dev.h] [--table NAME]
                          [--check OTHER_NK.bin [SITES.txt]]

The kernel module is kernel.dll (CE 6.0) or, without one, nk.exe (CE 5.0:
kernel + OAL; the OAL functions that reach P4, OEMCacheRangeFlush, are kept
by the same rules). CE 6.0: kern.map is kernel.dll's map (kern.dll); CE 5.0:
kern.exe's.

A function is kept when, in its own words:
  - a mov.l @(disp,PC) literal has the top byte 0xFF (what the translator's
    run-time test means by P4) and is not a small negative (>= 0xFFFF0000),
  - it moves to or from a banked register (stc/ldc Rn_BANK: CE keeps the
    SH3CTL base, 0xFF000000, in r5_bank1),
  - it builds such a value from mov #-1,Rn and shifts of Rn,
or it lies in the exception vector area (ExceptionBase to
_ExceptionEndAddress: the handlers run with RB=1, where r0-r7 are the banked
registers themselves, so their P4 base shows no pattern).

--check locates the signatures in another image as rt/kdev.c will, and
checks that every P4 site observed in that image ("[devsite] pc count" lines
from -DDBT_DEV_SITES) lies in a located function.
"""
import argparse, re, struct, sys, zlib
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import cesym

def kernel_text(im):
    """The kernel module's .text: kernel.dll (CE 6.0), else nk.exe (CE 5.0 and
    earlier: kernel + OAL in one module), as rt/nkbin.c chooses it."""
    for want in ('kernel.dll', 'nk.exe'):
        for name, secs, vb, vs in im.modules:
            if name == want:
                ra, vsz, rva = secs[0]
                return ra, vsz, rva
    raise SystemExit('no kernel.dll or nk.exe')

def functions(mapfile, text_rva, text_len):
    """(rva_lo, rva_hi, name) of code symbols in .text, ending at the next symbol."""
    text = None; syms = []
    for l in open(mapfile, errors='replace'):
        m = re.match(r'\s0001:([0-9a-f]{8}) ([0-9a-f]{8})H \.text\s', l)
        if m: text = (int(m.group(1), 16), int(m.group(2), 16))
        m = re.match(r'\s0001:([0-9a-f]{8})\s+(\S+)\s+[0-9a-f]{8}', l)
        if m: syms.append((int(m.group(1), 16), m.group(2)))
    if not text: raise SystemExit('no .text in map')
    lo, ln = text
    offs = sorted({o for o, n in syms if lo <= o < lo + ln})
    names = {}
    for o, n in syms:
        if lo <= o < lo + ln: names.setdefault(o, n)
    out = []
    for i, o in enumerate(offs):
        end = offs[i + 1] if i + 1 < len(offs) else lo + ln
        out.append((text_rva + o, text_rva + end, names[o]))   # map offsets count from the section start
    return out

def literal_words(words, base):
    """Indices (into words, 16-bit) covered by PC-relative literals."""
    lit = set()
    for i, w in enumerate(words):
        pc = base + 2 * i
        if w >> 12 == 0xD:
            a = ((pc + 4) & ~3) + 4 * (w & 0xFF)
            j = (a - base) // 2
            lit.update((j, j + 1))
        elif w >> 12 == 0x9:
            a = pc + 4 + 2 * (w & 0xFF)
            lit.add((a - base) // 2)
    return lit

def p4_reasons(get16, get32, lo, hi):
    words = [get16(a) for a in range(lo, hi, 2)]
    lit = literal_words(words, lo)
    why = set()
    for i, w in enumerate(words):
        if i in lit: continue
        pc = lo + 2 * i
        if w >> 12 == 0xD:
            a = ((pc + 4) & ~3) + 4 * (w & 0xFF)
            v = get32(a)
            if v is not None and 0xFF000000 <= v < 0xFFFF0000: why.add('literal %08x' % v)
        if (w & 0xF08F) == 0x0082 or (w & 0xF08F) == 0x408E or (w & 0xF08F) == 0x4087:
            why.add('banked register')                        # stc Rm_BANK,Rn / ldc Rm,Rn_BANK / ldc.l @Rm+,Rn_BANK
        if (w & 0xF0FF) == 0xE0FF:                            # mov #-1,Rn, then shifts of Rn
            n = (w >> 8) & 15; v = 0xFFFFFFFF
            for w2 in words[i + 1:i + 8]:
                sh = {0x4000: 1, 0x4008: 2, 0x4018: 8, 0x4028: 16}.get(w2 & 0xF0FF) if (w2 >> 8) & 15 == n else None
                if sh: v = (v << sh) & 0xFFFFFFFF
                elif (w2 >> 8) & 15 == n and w2 >> 12 in (0x6, 0xE, 0xD, 0x9, 0x3, 0x7): break   # Rn rewritten
                if v >> 24 == 0xFF and v < 0xFFFF0000:
                    why.add('builds %08x' % v); break
    return why

def masked(words, base, lo, hi):
    """Words with what moves between kernel links taken out: PC-relative
    literals (fixed-up addresses) become 0, bra/bsr leaving [lo, hi) keep
    only their opcode, and so do aligned word pairs whose little-endian value
    is a P1/P2 address (data referenced from elsewhere). rt/kdev.c computes
    the same."""
    lit = literal_words(words, base)
    out = []
    for i, w in enumerate(words):
        if i in lit: w = 0
        elif w >> 12 in (0xA, 0xB):
            d = w & 0xFFF
            t = base + 2 * i + 4 + 2 * (d - 0x1000 if d & 0x800 else d)
            if not lo <= t < hi: w &= 0xF000
        out.append(w)
    for i in range(len(out) - 1):                 # aligned data words holding P1/P2 addresses (fixups)
        if (base + 2 * i) % 4 == 0 and 0x8000 <= words[i + 1] < 0xC000 and i not in lit:
            out[i] = out[i + 1] = 0
    return out

def signature(get16, lo, hi):
    """Relocation-insensitive identity: the first 4 words masked on their
    own with every bra/bsr displacement dropped, and the CRC32 of all masked
    words (big-endian halfwords)."""
    raw = [get16(a) for a in range(lo, hi, 2)]
    words = masked(raw, lo, lo, hi)
    if hi - lo > 4096: raise SystemExit('function at %08x longer than rt/kdev.c accepts' % lo)
    # the prefix is masked on its own, every bra/bsr displacement included,
    # so the boot-time scan computes it once per position for all functions
    return masked(raw[:4], lo, lo, lo), zlib.crc32(b''.join(struct.pack('>H', w) for w in words))

def locate(nk, sigs):
    """Find each ((first words, crc), length, name) in another image's
    kernel.dll text as rt/kdev.c does: a unique position where the first
    four words and the CRC over the length match. Returns [(lo, hi, name)]."""
    im = cesym.Image(nk, '/nonexistent')
    ra, vsz, rva = kernel_text(im)
    words = [im.u16(x) for x in range(ra, ra + vsz, 2)]
    out = []
    for (w4, crc), length, name in sigs:
        hits = [ra + 2 * i for i in range(len(words) - 3)
                if 2 * i + length <= vsz and
                masked(words[i:i + 4], ra + 2 * i, ra + 2 * i, ra + 2 * i) == w4 and
                signature(im.u16, ra + 2 * i, ra + 2 * i + length)[1] == crc]
        if len(hits) != 1: raise SystemExit('%s: %s found %d times' % (nk, name, len(hits)))
        out.append((hits[0], hits[0] + length, name))
    return out

def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('nk'); p.add_argument('map')
    p.add_argument('--check', nargs='+')
    p.add_argument('-o', '--output', type=Path)
    p.add_argument('--table', default='kdev_funcs',
                   help='name of the generated array (KDEV_FUNCS... macro follows it); kdev_funcs50 for the CE 5.0 nk.exe table')
    a = p.parse_args()
    im = cesym.Image(a.nk, '/nonexistent')
    ra, vsz, rva = kernel_text(im)
    def get16(addr):
        return im.u16(addr)
    def get32(addr):
        try: return im.u32(addr)
        except Exception: return None
    funcs = functions(a.map, rva, vsz)
    vec = {}                                   # aliases share an offset, so read them from the map itself
    for l in open(a.map, errors='replace'):
        m = re.match(r'\s0001:([0-9a-f]{8})\s+(ExceptionBase|_ExceptionEndAddress)\s', l)
        if m: vec[m.group(2)] = rva + int(m.group(1), 16)
    if len(vec) != 2: raise SystemExit('ExceptionBase/_ExceptionEndAddress not in the map')
    keep = []
    for flo, fhi, name in funcs:
        lo, hi = ra + flo - rva, ra + fhi - rva
        why = p4_reasons(get16, get32, lo, hi)
        if vec['ExceptionBase'] <= flo < vec['_ExceptionEndAddress']: why.add('exception vector area')
        if why and fhi - flo >= 8: keep.append((flo, fhi, name, sorted(why)))   # shorter: data words, no code
    sigs = [(signature(get16, ra + flo - rva, ra + fhi - rva), fhi - flo, name) for flo, fhi, name, why in keep]
    if a.check:
        located = locate(a.check[0], sigs)
        print('%s: all %d functions located' % (a.check[0], len(located)), file=sys.stderr)
        if len(a.check) > 1:
            n = 0
            for l in open(a.check[1]):
                m = re.match(r'\s*(?:\[devsite\] )?([0-9a-f]{8})', l)
                if not m: continue
                pc = (int(m.group(1), 16) & 0x1FFFFFFF) | 0x80000000; n += 1
                if not any(lo <= pc < hi for lo, hi, _ in located):
                    raise SystemExit('observed P4 site %08x is outside every located function' % pc)
            print('%d observed P4 sites, all inside located functions' % n, file=sys.stderr)
    macro = a.table.upper()
    lines = ['/* Generated by tools/kernel_devfuncs.py from %s and %s: the kernel' % (Path(a.nk).name, Path(a.map).name),
             ' * functions that may access P4 (rules in the script). rt/kdev.c finds each',
             ' * one by its signature; if any is missing the whole kernel module stays',
             ' * device-aware. Do not edit. */',
             '#define %s %d' % (macro, len(keep)),
             'static const struct { sf_u16 w[4]; sf_u32 len, crc; } %s[%s] = {' % (a.table, macro)]
    for flo, fhi, name, why in keep:
        w, crc = signature(get16, ra + flo - rva, ra + fhi - rva)
        lines.append('    { { 0x%04x, 0x%04x, 0x%04x, 0x%04x }, 0x%x, 0x%08xu },   /* %s: %s */' %
                     (*w, fhi - flo, crc, name, ', '.join(why)))
    lines.append('};')
    text = '\n'.join(lines) + '\n'
    if a.output: a.output.write_text(text)
    else: sys.stdout.write(text)
    print('%d of %d functions kept, %d bytes of %d' % (len(keep), len(funcs), sum(k[1] - k[0] for k in keep),
          sum(f[1] - f[0] for f in funcs)), file=sys.stderr)

if __name__ == '__main__':
    main()
