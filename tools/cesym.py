#!/usr/bin/env python3
"""Map guest addresses of a running CE image to module symbols.

Usage: cesym.py NK.bin RELDIR addr [addr...]      (addresses in hex, P1 or P2)
       cesym.py NK.bin RELDIR -                   (addresses read from stdin, one per line or
                                                   'translate gpc=...' lines)
RELDIR is the flat release directory holding the <module>.map files from the
Platform Builder build. The module table and the o32 section tables come from
the image itself, so ROMIMAGE's section placement is honoured.
"""
import re, struct, sys, os

def load_image(path):
    d = open(path, 'rb').read()
    assert d[:7] == b'B000FF\n'
    base, size = struct.unpack('<II', d[7:15]); off = 15
    img = bytearray(size)
    while True:
        a, l, c = struct.unpack('<III', d[off:off + 12]); off += 12
        if a == 0 and c == 0: break
        img[a - base:a - base + l] = d[off:off + l]; off += l
    return base, img

class Image:
    def __init__(self, path, reldir):
        self.base, self.img = load_image(path)
        self.reldir = reldir
        self.modules = []          # (name, [(realaddr, vsize, rva)], mapfile)
        self.maps = {}
        self.parse_toc()
    def u32(self, a): return struct.unpack('<I', self.img[a - self.base:a - self.base + 4])[0]
    def u16(self, a): return struct.unpack('<H', self.img[a - self.base:a - self.base + 2])[0]
    def cstr(self, a):
        o = a - self.base; e = self.img.index(b'\0', o)
        return self.img[o:e].decode('ascii', 'replace')
    def parse_toc(self):
        hdr = self.u32(self.base + 0x44)
        nummods = self.u32(hdr + 16)
        toc = hdr + 84
        for i in range(nummods):
            te = toc + 32 * i
            name = self.cstr(self.u32(te + 16)); e32 = self.u32(te + 20); o32 = self.u32(te + 24)
            objcnt = self.u16(e32); vbase = self.u32(e32 + 8); vsize = self.u32(e32 + 20)
            secs = []
            for k in range(objcnt):
                o = o32 + 24 * k
                vs, rva, ps, dp, ra, fl = [self.u32(o + 4 * j) for j in range(6)]
                secs.append((ra, vs, rva))
            self.modules.append((name, secs, vbase, vsize))
    def mapsyms(self, name):
        if name in self.maps: return self.maps[name]
        stem = os.path.splitext(name)[0]
        cand = [stem + '.map', ('kern' if stem == 'kernel' else stem) + '.map', ('oal' if stem == 'nk' else stem) + '.map']
        syms = []
        for c in cand:
            p = os.path.join(self.reldir, c)
            if os.path.exists(p):
                for l in open(p, errors='replace'):
                    m = re.match(r'\s0001:([0-9a-f]{8})\s+(\S+)\s+([0-9a-f]{8})', l)
                    if m: syms.append((int(m.group(1), 16), m.group(2)))
                break
        syms.sort()
        self.maps[name] = syms
        return syms
    def lookup(self, addr):
        a = (addr & 0x1FFFFFFF) | 0x80000000 if addr >= 0x80000000 and addr < 0xC0000000 else addr
        # sections first across every module (ROMIMAGE places a module's
        # sections anywhere, also below another module's load address), the
        # fixed-up VM address range only as a fallback
        hits = []
        for name, secs, vbase, vsize in self.modules:
            r = None
            for ra, vs, rva in secs:
                if ra <= a < ra + vs: r = rva + (a - ra)
            if r is not None: hits.append((name, r))
        if not hits:
            for name, secs, vbase, vsize in self.modules:
                if vbase and vbase <= addr < vbase + vsize:
                    hits.append((name, addr - vbase))     # kernel-mode DLL running at its fixed-up VM address
                    break
        for name, r in hits:
            if True:
                off = r - 0x1000
                syms = self.mapsyms(name)
                best = None
                for o, n in syms:
                    if o <= off: best = (o, n)
                    else: break
                if best: return '%s!%s+0x%x' % (name, best[1], off - best[0])
                return '%s+rva 0x%x' % (name, r)
        return '?'

def main(argv):
    if len(argv) < 4: print(__doc__); return 2
    im = Image(argv[1], argv[2])
    if argv[3] == '-':
        for l in sys.stdin:
            m = re.search(r'gpc=([0-9a-fA-F]+)', l) or re.search(r'\b(0x)?([0-9a-fA-F]{8})\b', l)
            if not m: print(l.rstrip()); continue
            a = int(m.group(1) if 'gpc' in l else m.group(2), 16)
            print('%s  %s' % (l.rstrip(), im.lookup(a)))
    else:
        for s in argv[3:]:
            a = int(s, 16); print('%08x %s' % (a, im.lookup(a)))
    return 0

if __name__ == '__main__':
    sys.exit(main(sys.argv))
