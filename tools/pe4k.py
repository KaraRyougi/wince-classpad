#!/usr/bin/env python3
"""Realign a Windows CE PE (Pocket PC SH3, 1 KiB SectionAlignment) to 4 KiB.

  pe4k.py <in> <out>          write the realigned copy, then check it
  pe4k.py --check <in> <out>  only check an existing pair

romimage puts only 4 KiB-aligned modules into an XIP region ("only 4k page
alignments supported at this time"), and the Pocket PC 2000/2002 SH3 tools
linked with 0x400. The file layout stays as it is (same FileAlignment, same
PointerToRawData, same raw bytes apart from patched fields): only the virtual
layout moves. Each section keeps its old span (up to the next section) and
starts on the next 4 KiB boundary, so an address anywhere in a section or in
the gap after it maps by that section's delta. Patched:

  - optional header: entry point, BaseOfCode/Data, SectionAlignment,
    SizeOfImage, CheckSum (0), the data directory RVAs
  - section headers' VirtualAddress
  - export directory (name, the three arrays, every function and name RVA),
    import descriptors and both thunk arrays (name/hint RVAs), resource data
    entries, debug directory AddressOfRawData
  - every HIGHLOW fixup's stored address, and the .reloc table rebuilt for the
    new pages (a 4 KiB page can now hold parts of two old sections); .pdata's
    function addresses are HIGHLOW fixups like any other

Refused: no relocations (an EXE linked /FIXED cannot be moved: put it in the
image as a file instead), relocation types other than HIGHLOW, TLS, load
config, bound or delay imports, a fixup whose target is outside the image, a
.reloc section that is not last in the file. The check re-reads both files
and compares what the loader and romimage see: imports, exports (and the code
at each), resources, the entry point code, every section's bytes after
relocating both images to the same base.
"""
import struct
import sys

HIGHLOW = 3
ABSOLUTE = 0
NEW_ALIGN = 0x1000


class PEError(Exception):
    pass


def align(v, a):
    return (v + a - 1) & ~(a - 1)


class PE:
    def __init__(self, data):
        self.d = bytearray(data)
        d = self.d
        if d[:2] != b"MZ":
            raise PEError("not an MZ file")
        self.pe = struct.unpack_from("<I", d, 0x3C)[0]
        if d[self.pe:self.pe + 4] != b"PE\0\0":
            raise PEError("no PE signature")
        self.nsec = struct.unpack_from("<H", d, self.pe + 6)[0]
        self.chars = struct.unpack_from("<H", d, self.pe + 22)[0]
        osz = struct.unpack_from("<H", d, self.pe + 20)[0]
        self.opt = self.pe + 24
        if struct.unpack_from("<H", d, self.opt)[0] != 0x10B:
            raise PEError("not PE32")
        self.base = self.u32(self.opt + 28)
        self.salign = self.u32(self.opt + 32)
        self.falign = self.u32(self.opt + 36)
        self.image_size = self.u32(self.opt + 56)
        self.hdr_size = self.u32(self.opt + 60)
        self.ndirs = self.u32(self.opt + 92)
        self.sh = self.opt + osz
        self.secs = []
        for i in range(self.nsec):
            o = self.sh + 40 * i
            name = bytes(d[o:o + 8]).rstrip(b"\0").decode("ascii", "replace")
            vs, va, rs, rp = struct.unpack_from("<IIII", d, o + 8)
            self.secs.append({"name": name, "hdr": o, "vs": vs, "va": va, "rs": rs, "rp": rp})

    def u32(self, o):
        return struct.unpack_from("<I", self.d, o)[0]

    def p32(self, o, v):
        struct.pack_into("<I", self.d, o, v & 0xFFFFFFFF)

    def dir(self, i):
        if i >= self.ndirs:
            return 0, 0
        return struct.unpack_from("<II", self.d, self.opt + 96 + 8 * i)

    def dir_off(self, i):
        return self.opt + 96 + 8 * i

    def off(self, rva, n=1):
        """File offset of [rva, rva+n) in a section's raw data."""
        for s in self.secs:
            if s["va"] <= rva and rva + n <= s["va"] + s["rs"]:
                return s["rp"] + rva - s["va"]
        if rva + n <= self.hdr_size:
            return rva
        raise PEError("rva 0x%x (%d bytes) not in any section's raw data" % (rva, n))

    def cstr(self, rva):
        o = self.off(rva)
        e = self.d.index(b"\0", o)
        return bytes(self.d[o:e])

    def relocs(self):
        """(page rva + offset) of every HIGHLOW fixup, in table order."""
        rva, size = self.dir(5)
        out = []
        if not rva:
            return out
        o = self.off(rva, size)
        end = o + size
        while o < end:
            page, bs = struct.unpack_from("<II", self.d, o)
            if bs < 8 or o + bs > end:
                if bs == 0:
                    break
                raise PEError("bad relocation block at 0x%x" % o)
            for k in range((bs - 8) // 2):
                e = struct.unpack_from("<H", self.d, o + 8 + 2 * k)[0]
                t = e >> 12
                if t == HIGHLOW:
                    out.append(page + (e & 0xFFF))
                elif t != ABSOLUTE:
                    raise PEError("relocation type %d at 0x%x" % (t, page + (e & 0xFFF)))
            o += bs
        return out


def plan(pe):
    """Old span and new VirtualAddress of each section, and the rva map."""
    spans = []
    for i, s in enumerate(pe.secs):
        if i + 1 < len(pe.secs):
            end = pe.secs[i + 1]["va"]
        else:
            end = align(s["va"] + max(s["vs"], s["rs"]), pe.salign)
        if end < s["va"]:
            raise PEError("sections not in address order")
        spans.append((s["va"], end))
    new = []
    va = align(max(pe.hdr_size, spans[0][0] if spans else 0), NEW_ALIGN)
    for lo, hi in spans:
        new.append(va)
        va = align(va + hi - lo, NEW_ALIGN)
    new_image = va
    first = spans[0][0]

    def rmap(r):
        if r < first:
            return r                     # headers stay at 0
        for (lo, hi), nva in zip(spans, new):
            if lo <= r < hi:
                return r - lo + nva
        if r == spans[-1][1]:
            return new_image             # one past the end of the image
        raise PEError("rva 0x%x outside the image" % r)

    return spans, new, new_image, rmap


def realign(data):
    pe = PE(data)
    if pe.salign == NEW_ALIGN:
        return bytes(pe.d), "already 4 KiB"
    if pe.salign > NEW_ALIGN or NEW_ALIGN % pe.salign:
        raise PEError("SectionAlignment 0x%x" % pe.salign)
    if pe.chars & 1 or not pe.dir(5)[0]:
        raise PEError("no relocations (linked /FIXED): it cannot be moved; put it in the image as a file")
    for i, what in ((4, "certificates"), (9, "TLS"), (10, "load config"), (11, "bound imports"), (13, "delay imports")):
        if pe.dir(i)[0]:
            raise PEError("has %s (data directory %d)" % (what, i))
    names = [s["name"] for s in pe.secs]
    if names[-1] != ".reloc" or pe.dir(5)[0] != pe.secs[-1]["va"]:
        raise PEError(".reloc is not the last section")
    rel = pe.secs[-1]
    if rel["rp"] + rel["rs"] != len(pe.d):
        raise PEError(".reloc is not at the end of the file")

    spans, newva, new_image, rmap = plan(pe)
    out = PE(bytes(pe.d))                      # patched in place; offsets unchanged
    d = out.d

    # Fixups: the stored address, then the table for the new pages.
    fix = pe.relocs()
    newfix = []
    for r in fix:
        o = pe.off(r, 4)
        v = pe.u32(o)
        t = (v - pe.base) & 0xFFFFFFFF
        if t > pe.image_size:
            raise PEError("fixup at rva 0x%x holds 0x%08x, outside the image" % (r, v))
        out.p32(o, pe.base + rmap(t))
        newfix.append(rmap(r))

    # Export directory.
    erva, esz = pe.dir(0)
    if erva:
        eo = pe.off(erva, 40)
        nfun, nnam = pe.u32(eo + 20), pe.u32(eo + 24)
        afun, anam, aord = pe.u32(eo + 28), pe.u32(eo + 32), pe.u32(eo + 36)
        for k in (12, 28, 32, 36):
            if pe.u32(eo + k):
                out.p32(eo + k, rmap(pe.u32(eo + k)))
        fo = pe.off(afun, 4 * nfun) if nfun else 0
        for i in range(nfun):
            f = pe.u32(fo + 4 * i)
            if f:
                out.p32(fo + 4 * i, rmap(f))
        no = pe.off(anam, 4 * nnam) if nnam else 0
        for i in range(nnam):
            out.p32(no + 4 * i, rmap(pe.u32(no + 4 * i)))

    # Import descriptors and thunks.
    irva, isz = pe.dir(1)
    done = set()
    if irva:
        o = pe.off(irva, 20)
        while True:
            oft, ts, fc, name, ft = struct.unpack_from("<IIIII", pe.d, o)
            if not (oft or name or ft):
                break
            if ts:
                raise PEError("bound import descriptor")
            for k, v in ((0, oft), (12, name), (16, ft)):
                if v:
                    out.p32(o + k, rmap(v))
            for arr in (oft, ft):
                if not arr:
                    continue
                to = pe.off(arr, 4)
                while True:
                    e = pe.u32(to)
                    if e == 0:
                        break
                    if to not in done and not e & 0x80000000:
                        out.p32(to, rmap(e))
                    done.add(to)
                    to += 4
            o += 20

    # Resource data entries.
    rrva, rsz = pe.dir(2)
    if rrva:
        root = pe.off(rrva, 16)
        seen = set()

        def walk(doff):
            n = struct.unpack_from("<HH", pe.d, root + doff + 12)
            for i in range(n[0] + n[1]):
                e = root + doff + 16 + 8 * i
                tgt = pe.u32(e + 4)
                if tgt & 0x80000000:
                    walk(tgt & 0x7FFFFFFF)
                elif tgt not in seen:
                    seen.add(tgt)
                    out.p32(root + tgt, rmap(pe.u32(root + tgt)))

        walk(0)

    # Debug directory.
    drva, dsz = pe.dir(6)
    if drva:
        o = pe.off(drva, dsz)
        for i in range(dsz // 28):
            a = pe.u32(o + 28 * i + 20)
            if a:
                out.p32(o + 28 * i + 20, rmap(a))

    # New .reloc table.
    pages = {}
    for r in sorted(set(newfix)):
        pages.setdefault(r & ~0xFFF, []).append(r & 0xFFF)
    tab = bytearray()
    for page in sorted(pages):
        ents = [HIGHLOW << 12 | x for x in pages[page]]
        if len(ents) & 1:
            ents.append(0)
        tab += struct.pack("<II", page, 8 + 2 * len(ents)) + struct.pack("<%dH" % len(ents), *ents)
    rs = align(len(tab), pe.falign)
    del d[rel["rp"]:]
    d += tab + bytes(rs - len(tab))
    struct.pack_into("<II", d, rel["hdr"] + 8, len(tab), newva[-1])
    struct.pack_into("<I", d, rel["hdr"] + 16, rs)

    # Section headers, data directories, optional header.
    for s, nva in zip(pe.secs[:-1], newva[:-1]):
        out.p32(s["hdr"] + 12, nva)
    for i in range(pe.ndirs):
        rva, size = pe.dir(i)
        if rva and i != 4:
            out.p32(pe.dir_off(i), rmap(rva))
    out.p32(pe.dir_off(5) + 4, len(tab))
    for k in (16, 20, 24):
        v = pe.u32(pe.opt + k)
        if v:
            out.p32(pe.opt + k, rmap(v))
    out.p32(pe.opt + 32, NEW_ALIGN)
    out.p32(pe.opt + 56, new_image)
    out.p32(pe.opt + 64, 0)
    return bytes(d), "%d sections, %d fixups, image 0x%x -> 0x%x" % (len(pe.secs), len(fix), pe.image_size, new_image)


# --------------------------------------------------------------------- check
def loaded(pe, base):
    """The image as the loader maps it at base."""
    m = bytearray(max(pe.image_size, max(s["va"] + max(s["vs"], s["rs"]) for s in pe.secs)))
    m[:pe.hdr_size] = pe.d[:pe.hdr_size]
    for s in pe.secs:
        m[s["va"]:s["va"] + s["rs"]] = pe.d[s["rp"]:s["rp"] + s["rs"]]
    delta = base - pe.base
    fix = pe.relocs()
    for r in fix:
        struct.pack_into("<I", m, r, (struct.unpack_from("<I", m, r)[0] + delta) & 0xFFFFFFFF)
    return m, set(fix)


def rva_fields(pe, m):
    """Image offsets of the words that hold RVAs (not fixups): the import
    descriptors and thunks, the export directory and arrays, resource data
    entries, debug AddressOfRawData. Their new values must be the mapped RVAs;
    the section comparison checks that and everything else byte for byte."""
    out = set()
    rva, size = pe.dir(0)
    if rva:
        for k in (12, 28, 32, 36):
            if struct.unpack_from("<I", m, rva + k)[0]:
                out.add(rva + k)
        nfun, nnam, afun, anam = struct.unpack_from("<IIII", m, rva + 20)
        out.update(afun + 4 * i for i in range(nfun) if struct.unpack_from("<I", m, afun + 4 * i)[0])
        out.update(anam + 4 * i for i in range(nnam))
    rva, _ = pe.dir(1)
    while rva:
        oft, ts, fc, name, ft = struct.unpack_from("<IIIII", m, rva)
        if not (oft or name or ft):
            break
        out.update(rva + k for k, v in ((0, oft), (12, name), (16, ft)) if v)
        for t in (oft, ft):
            while t and struct.unpack_from("<I", m, t)[0]:
                if not struct.unpack_from("<I", m, t)[0] & 0x80000000:
                    out.add(t)
                t += 4
        rva += 20
    rva, _ = pe.dir(2)
    if rva:
        def walk(doff):
            n = struct.unpack_from("<HH", m, rva + doff + 12)
            for i in range(n[0] + n[1]):
                tgt = struct.unpack_from("<I", m, rva + doff + 16 + 8 * i + 4)[0]
                if tgt & 0x80000000:
                    walk(tgt & 0x7FFFFFFF)
                else:
                    out.add(rva + tgt)
        walk(0)
    rva, size = pe.dir(6)
    for i in range(size // 28 if rva else 0):
        if struct.unpack_from("<I", m, rva + 28 * i + 20)[0]:
            out.add(rva + 28 * i + 20)
    return out


def imports(pe, m):
    rva, _ = pe.dir(1)
    out = []
    while rva:
        oft, ts, fc, name, ft = struct.unpack_from("<IIIII", m, rva)
        if not (oft or name or ft):
            break
        dll = bytes(m[name:m.index(b"\0", name)])
        t = oft or ft
        while True:
            e = struct.unpack_from("<I", m, t)[0]
            if not e:
                break
            if e & 0x80000000:
                out.append((dll, e & 0xFFFF))
            else:
                hint = struct.unpack_from("<H", m, e)[0]
                out.append((dll, hint, bytes(m[e + 2:m.index(b"\0", e + 2)])))
            t += 4
        rva += 20
    return out


def exports(pe, m):
    rva, size = pe.dir(0)
    if not rva:
        return {}
    nfun, nnam, afun, anam, aord = struct.unpack_from("<IIIII", m, rva + 20)
    ordbase = struct.unpack_from("<I", m, rva + 16)[0]
    res = {}
    for i in range(nfun):
        f = struct.unpack_from("<I", m, afun + 4 * i)[0]
        res[ordbase + i] = f
    names = {}
    for i in range(nnam):
        n = struct.unpack_from("<I", m, anam + 4 * i)[0]
        o = struct.unpack_from("<H", m, aord + 2 * i)[0]
        names[bytes(m[n:m.index(b"\0", n)])] = ordbase + o
    return res, names, rva, size


def resources(pe, m):
    rva, _ = pe.dir(2)
    out = []
    if not rva:
        return out

    def walk(doff, path):
        n = struct.unpack_from("<HH", m, rva + doff + 12)
        for i in range(n[0] + n[1]):
            e = rva + doff + 16 + 8 * i
            nm, tgt = struct.unpack_from("<II", m, e)
            if tgt & 0x80000000:
                walk(tgt & 0x7FFFFFFF, path + (nm,))
            else:
                drva, dsize, cp, _ = struct.unpack_from("<IIII", m, rva + tgt)
                out.append((path + (nm,), cp, bytes(m[drva:drva + dsize])))

    walk(0, ())
    return out


def check(old_data, new_data):
    old, new = PE(old_data), PE(new_data)
    errs = []
    if new.salign != NEW_ALIGN:
        errs.append("SectionAlignment 0x%x" % new.salign)
    for s in new.secs:
        if s["va"] % NEW_ALIGN:
            errs.append("%s at 0x%x" % (s["name"], s["va"]))
    for a, b in zip(old.secs, new.secs):
        if a["rp"] != b["rp"] or (a["name"] != ".reloc" and a["rs"] != b["rs"]):
            errs.append("%s raw layout changed" % a["name"])
    base = 0x01000000
    mo, fo = loaded(old, base)
    mn, fn = loaded(new, base)
    spans, newva, _, rmap = plan(old)
    rf = rva_fields(old, mo)
    if sorted(rmap(r) for r in fo) != sorted(fn):
        errs.append("fixup sets differ")
    # Every section's bytes: equal, except fixups, which must hold the mapped address.
    for (lo, hi), s, nva in zip(spans, old.secs, newva):
        if s["name"] == ".reloc":
            continue
        n = max(s["vs"], s["rs"])
        a, b = mo[lo:lo + n], mn[nva:nva + n]
        for r in sorted(x for x in fo if lo <= x < lo + n):
            k = r - lo
            v = struct.unpack_from("<I", a, k)[0]
            w = struct.unpack_from("<I", b, k)[0]
            if w != base + rmap(v - base):
                errs.append("fixup at 0x%x: 0x%08x -> 0x%08x" % (r, v, w))
                break
            a[k:k + 4] = b[k:k + 4]
        for r in sorted(x for x in rf if lo <= x < lo + n):
            k = r - lo
            v = struct.unpack_from("<I", a, k)[0]
            w = struct.unpack_from("<I", b, k)[0]
            if w != rmap(v):
                errs.append("rva field at 0x%x: 0x%x -> 0x%x" % (r, v, w))
                break
            a[k:k + 4] = b[k:k + 4]
        if a != b:
            errs.append("%s content differs" % s["name"])
    if imports(old, mo) != imports(new, mn):
        errs.append("imports differ")
    eo, en = exports(old, mo), exports(new, mn)
    if eo or en:
        fo_, no_, ro, so = eo
        fn_, nn_, rn, sn = en
        if no_ != nn_:
            errs.append("export names differ")
        for o, f in fo_.items():
            g = fn_.get(o)
            if (f == 0) != (g == 0):
                errs.append("export %d presence" % o)
            elif f and ro <= f < ro + so:
                if bytes(mo[f:mo.index(b"\0", f)]) != bytes(mn[g:mn.index(b"\0", g)]):
                    errs.append("forwarder %d" % o)
            elif f and rmap(f) != g:
                errs.append("export %d address" % o)
    if resources(old, mo) != resources(new, mn):
        errs.append("resources differ")
    eo_, en_ = old.u32(old.opt + 16), new.u32(new.opt + 16)
    if eo_ and (rmap(eo_) != en_ or mo[eo_:eo_ + 32] != mn[en_:en_ + 32]):
        errs.append("entry point")
    return errs


def main(argv):
    if len(argv) == 4 and argv[1] == "--check":
        errs = check(open(argv[2], "rb").read(), open(argv[3], "rb").read())
        for e in errs:
            print("pe4k: %s: %s" % (argv[3], e), file=sys.stderr)
        return 1 if errs else 0
    if len(argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    data = open(argv[1], "rb").read()
    try:
        new, what = realign(data)
        errs = check(data, new)
    except PEError as e:
        print("pe4k: %s: %s" % (argv[1], e), file=sys.stderr)
        return 1
    if errs:
        for e in errs:
            print("pe4k: %s: %s" % (argv[1], e), file=sys.stderr)
        return 1
    open(argv[2], "wb").write(new)
    print("pe4k: %s: %s" % (argv[1], what))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
