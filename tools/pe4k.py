#!/usr/bin/env python3
"""Realign a Windows CE PE (Pocket PC SH3, 1 KiB SectionAlignment) to 4 KiB.

  pe4k.py <in> <out>          write the realigned copy, then check it
  pe4k.py --check <in> <out>  only check an existing pair
  pe4k.py --relocs <in> <out> give a /FIXED SH EXE a .reloc section first
                              (reconstruct_relocs), then realign it

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

Refused: no relocations (an EXE linked /FIXED cannot be moved as it is; see
reconstruct_relocs below), relocation types other than HIGHLOW, TLS, load
config, bound or delay imports, a fixup whose target is outside the image, a
.reloc section that is not last in the file. The check re-reads both files
and compares what the loader and romimage see: imports, exports (and the code
at each), resources, the entry point code, every section's bytes after
relocating both images to the same base.
"""
import bisect
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


# ------------------------------------------------- relocations for /FIXED EXEs
def reconstruct_relocs(data, addresses=(), constants=()):
    """The HIGHLOW fixups of a /FIXED SH EXE, found by scanning.

    Below 64 KiB every own address is recognisable by itself (next
    paragraph). A larger image goes to reconstruct_relocs_pdata, which tells
    addresses from numbers by what refers to them and where they point;
    addresses / constants are the RVAs of words whose call it overrides
    (from a review by hand, pinned with the input in stage.py).

    CE 6 on SH-4 loads no EXE whose sections are not 4 KiB aligned, not even
    from a file (loader.c ReadO32Info: "Bad EXE/DLL format"), and a Pocket PC
    SH3 EXE linked /FIXED has no .reloc to move its sections with. For an
    image at 0x10000 smaller than 64 KiB every address inside it is
    0x0001xxxx, which as the upper halfword of a 4-aligned word is no SH
    instruction (0x0001 is undefined), so in code such a word is a literal
    pool entry: an address. In .rdata/.data/.pdata the aligned words inside
    the image are taken too; .rsrc is left out (its 0x00010000 words are
    version numbers and icon header fields, and nothing in it is relocated),
    and so are the import/export/resource/debug RVA fields (rva_fields).
    Returns the sorted RVAs. Only for what was checked by hand (stage.py pins
    the input's SHA-256 and the count)."""
    pe = PE(data)
    if struct.unpack_from("<H", pe.d, pe.pe + 4)[0] not in (0x1A2, 0x1A3, 0x1A6):
        raise PEError("reconstruct_relocs: not an SH image")
    if pe.base != 0x10000:
        raise PEError("reconstruct_relocs: only images at 0x10000")
    m, fx = loaded(pe, pe.base)
    if fx:
        raise PEError("reconstruct_relocs: the image has relocations")
    rf = rva_fields(pe, m)
    if pe.image_size > 0x10000:
        return reconstruct_relocs_pdata(pe, m, rf, addresses, constants)[0]
    if addresses or constants:
        raise PEError("reconstruct_relocs: overrides are for the .pdata scan")
    lo, hi = pe.base, pe.base + pe.image_size
    out = []
    for s in pe.secs:
        if s["name"] == ".rsrc":
            continue
        n = max(s["vs"], s["rs"])
        for a in range(s["va"], s["va"] + n - 3, 4):
            v = struct.unpack_from("<I", m, a)[0]
            if lo <= v <= hi and a not in rf:
                out.append(a)
    return out


# SH-3 import thunk (Pocket PC tools): mov.l @(4,pc),r0; mov.l @r0,r0;
# jmp @r0; nop; .long <IAT slot>
THUNK = bytes.fromhex("01d002602b400900")


def _sh_writes(op):
    """The register an SH instruction writes (Rn, or R0 for the #imm/GBR/
    displacement forms), else None. Close enough for following a loaded
    address over a few instructions; MAC/T/system registers are ignored."""
    hi, n, lo, k = op >> 12, (op >> 8) & 15, op & 0xFF, op & 0xF
    if hi in (0x5, 0x6, 0x7, 0x9, 0xD, 0xE):
        return n
    if hi == 0x0:
        return n if k in (0xC, 0xD, 0xE) or lo in (0x29, 0x02, 0x12, 0x22, 0x0A, 0x1A, 0x2A) else None
    if hi == 0x2:
        return n if k in (0x4, 0x5, 0x6, 0x9, 0xA, 0xB, 0xD) else None
    if hi == 0x3:
        return None if k in (0x0, 0x2, 0x3, 0x5, 0x6, 0x7, 0xD) else n
    if hi == 0x4:
        if lo in (0x00, 0x01, 0x04, 0x05, 0x08, 0x09, 0x10, 0x18, 0x19, 0x20, 0x21, 0x24, 0x25, 0x28, 0x29):
            return n                                   # shifts, rotates, dt
        return n if k in (0x2, 0x3, 0x6, 0x7, 0xC, 0xD) else None   # @-Rn, @Rn+, shad/shld
    if hi == 0x8:
        return 0 if n in (0x4, 0x5) else None          # mov.b/w @(disp,Rm),R0
    if hi == 0xC:
        return 0 if n in (0x4, 0x5, 0x6, 0x7, 0x9, 0xA, 0xB) else None
    return None


def _called(m, insn, limit=40):
    """Whether the register loaded by the mov.l at insn reaches a jsr/jmp
    before it is overwritten (copies followed, calls clobber r0-r7)."""
    regs = {(struct.unpack_from("<H", m, insn)[0] >> 8) & 15}
    a = insn + 2
    for _ in range(limit):
        op = struct.unpack_from("<H", m, a)[0]
        n = (op >> 8) & 15
        if (op & 0xF0FF) in (0x400B, 0x402B):            # jsr / jmp @Rn
            if n in regs:
                return True
            if (op & 0xF0FF) == 0x402B:
                return False
            regs -= set(range(8))
        elif op & 0xF00F == 0x6003 and (op >> 4) & 15 in regs:
            regs.add(n)                                # mov Rm,Rn
        else:
            w = _sh_writes(op)
            if w is not None:
                regs.discard(w)
        if op == 0x000B or op >> 12 == 0xA:            # rts / bra: one delay slot left
            limit = 0
        if not regs:
            return False
        a += 2
    return False


def reconstruct_relocs_pdata(pe, m, rf, addresses=(), constants=()):
    """The HIGHLOW fixups of a large /FIXED SH-3 EXE, told from numbers.

    Above 64 KiB an address 0x0001xxxx..0x00HHxxxx is also a possible
    instruction pair (0x0009 nop as the upper halfword) and a possible
    integer, so a word counts only when the code's own structure says it is
    an address:

      - .text: only literal-pool words, the targets of the mov.l @(disp,PC)
        instructions in the .pdata functions, the import thunks and the
        leaf functions (no .pdata entry: traced along their control flow
        from every code entry outside the .pdata functions, bsr targets
        included); the decoding skips halfwords that are themselves pool
        data, iterated to a fixed point; .pdata: the function-start word of every entry, not
        the prologue/length word; .rdata, .data: every aligned word that is
        not an import/export/resource/debug RVA field. .rsrc: nothing.
      - a word pointing into .rdata or .data is an address;
      - a word pointing into .text is an address when its target is a code
        entry: a .pdata function start, an import thunk, the target of a
        loaded register that reaches jsr/jmp (far branches inside one
        function use mov.l + jmp @Rn), or an even target outside every
        .pdata function (leaf functions have no .pdata entry) that follows
        the end of something (rts and its delay slot, a pool word, a
        function's last instruction, nop/zero padding). Anything else that
        lands inside a function is a number (MAKELONG pairs, table data);
      - a word pointing at the headers is left alone (they do not move),
        and words pointing into .pdata/.rsrc are numbers.

    Returns (sorted RVAs, report dict)."""
    lo_img, size = pe.base, pe.image_size
    u16 = lambda a: struct.unpack_from("<H", m, a)[0]
    u32 = lambda a: struct.unpack_from("<I", m, a)[0]
    secs = {s["name"]: s for s in pe.secs}
    if ".text" not in secs:
        raise PEError("reconstruct_relocs: no .text")
    text = secs[".text"]
    tlo, thi = text["va"], text["va"] + max(text["vs"], text["rs"])
    prva, psize = pe.dir(3)
    if not prva:
        raise PEError("reconstruct_relocs: no .pdata to find the functions with")

    funcs, pstart = [], set()
    exc_words = set()
    for a in range(prva, prva + psize - 7, 8):
        st, w = u32(a), u32(a + 4)
        if not st:
            continue
        n = ((w >> 8) & 0x3FFFFF) * (4 if (w >> 30) & 1 else 2)
        r = st - lo_img
        if not (tlo <= r and r + n <= thi):
            raise PEError("reconstruct_relocs: .pdata entry at 0x%x outside .text" % a)
        funcs.append((r, n))
        pstart.add(a)
        if w >> 31:                                    # handler, handler data before the function
            exc_words |= {r - 8, r - 4}
    funcs.sort()
    fstarts = {r for r, _ in funcs}
    fends = {r + n for r, n in funcs}
    starts = [r for r, _ in funcs]

    def in_func(r):
        i = bisect.bisect_right(starts, r) - 1
        return i >= 0 and r < funcs[i][0] + funcs[i][1]

    thunks = [a for a in range(tlo & ~3, thi - 11, 4)
              if m[a:a + 8] == THUNK and not in_func(a)]
    thunk_set = set(thunks)
    thunk_hw = {a + i for a in thunks for i in range(0, 12, 2)}

    def leaf_body(entry, data_hw):
        """Halfwords of the leaf function at entry (no .pdata entry), by
        following its control flow: conditional branches both ways, bra/rts/
        jmp/braf end a path after the delay slot; never into a .pdata
        function, a thunk or pool data."""
        seen, todo = set(), [entry]
        while todo:
            a = todo.pop()
            while tlo <= a < thi and a not in seen and a not in data_hw and not in_func(a) \
                    and a not in thunk_hw:
                seen.add(a)
                op = u16(a)
                hi = op >> 12
                if hi == 0x8 and (op >> 8) & 15 in (0x9, 0xB, 0xD, 0xF):   # bt, bf, bt/s, bf/s
                    d = op & 0xFF
                    todo.append(a + 4 + (d - 256 if d & 0x80 else d) * 2)
                    if (op >> 8) & 15 in (0xD, 0xF):
                        seen.add(a + 2)
                        a += 4
                        continue
                elif hi == 0xA or op in (0x000B, 0x002B) or (op & 0xF0FF) in (0x402B, 0x0023):
                    if hi == 0xA:                      # bra
                        d = op & 0xFFF
                        todo.append(a + 4 + (d - 4096 if d & 0x800 else d) * 2)
                    seen.add(a + 2)                    # delay slot, then the path ends
                    break
                a += 2
        return seen

    def bsr_targets(addrs):
        out = set()
        for a in addrs:
            op = u16(a)
            if op >> 12 == 0xB:                        # bsr
                d = op & 0xFFF
                out.add(a + 4 + (d - 4096 if d & 0x800 else d) * 2)
        return out

    def boundary(r, pool):
        if r & 1 or in_func(r):
            return False
        p = r
        for _ in range(8):                             # back over nop / zero padding
            if p in fends or (p - 4) in pool:
                return True
            if u16(p - 4) == 0x000B or u16(p - 4) >> 12 == 0xA:    # rts / bra, delay slot
                return True
            if u16(p - 2) not in (0x0009, 0x0000):
                return False
            p -= 2
        return False

    # Pool decoding over the .pdata functions, the thunks and the leaf
    # functions (no .pdata entry, so their literal pools are found only by
    # tracing them from a call: SpreadCE's one-line "is this character the
    # list separator" helpers load the separator's address from a pool),
    # iterated to a fixed point: halfwords that are pool data are skipped,
    # and new pools and calls reveal new leaf functions.
    data_words = [a for s in pe.secs if s["name"] in (".rdata", ".data")
                  for a in range(s["va"], s["va"] + max(s["vs"], s["rs"]) - 3, 4) if a not in rf]
    data_hw, leaves, leaf_hw = set(), set(), set()
    for _ in range(32):
        code_hw = set()
        for s, n in funcs:
            code_hw.update(range(s, s + n, 2))
        for a in thunks:
            code_hw.update(range(a, a + 8, 2))
        code_hw |= leaf_hw
        pool, hw = {}, set()
        for a in code_hw:
            if a in data_hw:
                continue
            op = u16(a)
            if op >> 12 == 0xD:                        # mov.l @(disp,PC),Rn
                t = (a & ~3) + 4 + (op & 0xFF) * 4
                pool.setdefault(t, []).append(a)
                hw |= {t, t + 2}
            elif op >> 12 == 0x9:                      # mov.w @(disp,PC),Rn
                hw.add(a + 4 + (op & 0xFF) * 2)
        entries = fstarts | thunk_set
        for t, ins in pool.items():
            v = u32(t) - lo_img
            if tlo <= v < thi and v not in entries and any(_called(m, i) for i in ins):
                entries.add(v)
        targets = entries | bsr_targets(a for a in code_hw if a not in hw)
        for a in data_words:                           # function pointers in .rdata/.data
            r = u32(a) - lo_img
            if tlo <= r < thi and boundary(r, pool):
                targets.add(r)
        new = {r for r in targets
               if not r & 1 and tlo <= r < thi and not in_func(r) and r not in thunk_hw}
        if hw == data_hw and new <= leaves:
            break
        data_hw = hw
        leaves |= new
        leaf_hw = set()
        for r in leaves:
            leaf_hw |= leaf_body(r, data_hw)
    else:
        raise PEError("reconstruct_relocs: pool decoding does not settle")

    def seg(r):
        for s in pe.secs:
            if s["va"] <= r < s["va"] + max(s["vs"], s["rs"]):
                return s["name"]
        return "headers" if 0 <= r < pe.secs[0]["va"] else None

    words = set(pool) | pstart | exc_words
    for s in pe.secs:
        if s["name"] not in (".text", ".rdata", ".data", ".pdata", ".rsrc"):
            raise PEError("reconstruct_relocs: unknown section %s" % s["name"])
        if s["name"] in (".rdata", ".data"):
            words |= set(range(s["va"], s["va"] + max(s["vs"], s["rs"]) - 3, 4))
    words -= rf
    out, report = [], {}
    for a in sorted(words):
        v = u32(a)
        if not (lo_img <= v < lo_img + size):
            continue
        r = v - lo_img
        tgt = seg(r)
        if tgt in (".rdata", ".data"):
            why = "address"
        elif tgt == ".text":
            if r in entries:
                why = "address"
            elif boundary(r, pool):
                why = "address"
            else:
                why = "number"
        elif tgt == "headers":
            why = "headers"
        else:
            why = "number"
        if a in addresses:
            why = "address" if why != "headers" else why
        elif a in constants:
            why = "number"
        key = "%s %s->%s" % (why, seg(a), tgt)
        report[key] = report.get(key, 0) + 1
        if why == "address":
            out.append(a)
    for a in list(addresses) + list(constants):
        if a not in words or not (lo_img <= u32(a) < lo_img + size):
            raise PEError("reconstruct_relocs: override 0x%x is no candidate word" % a)
    report["thunks"] = len(thunks)
    report["code entries"] = len(entries)
    return out, report


def add_relocs(data, rvas):
    """data with a .reloc section for the HIGHLOW fixups at rvas appended as
    the last section (old SectionAlignment), RELOCS_STRIPPED cleared."""
    pe = PE(data)
    if pe.dir(5)[0]:
        raise PEError("add_relocs: the image has a .reloc already")
    last = pe.secs[-1]
    if last["rp"] + last["rs"] != len(pe.d):
        raise PEError("add_relocs: data after the last section")
    hdr = pe.sh + 40 * pe.nsec
    if hdr + 40 > min(s["rp"] for s in pe.secs) or any(pe.d[hdr:hdr + 40]):
        raise PEError("add_relocs: no room for another section header")
    pages = {}
    for r in sorted(set(rvas)):
        pages.setdefault(r & ~0xFFF, []).append(r & 0xFFF)
    tab = bytearray()
    for page in sorted(pages):
        ents = [HIGHLOW << 12 | x for x in pages[page]]
        if len(ents) & 1:
            ents.append(0)
        tab += struct.pack("<II", page, 8 + 2 * len(ents)) + struct.pack("<%dH" % len(ents), *ents)
    va = align(last["va"] + max(last["vs"], last["rs"]), pe.salign)
    rp = len(pe.d)
    rs = align(len(tab), pe.falign)
    d = pe.d
    d += tab + bytes(rs - len(tab))
    d[hdr:hdr + 40] = b".reloc\0\0" + struct.pack("<IIIIIIHHI", len(tab), va, rs, rp, 0, 0, 0, 0, 0x42000040)
    struct.pack_into("<H", d, pe.pe + 6, pe.nsec + 1)
    struct.pack_into("<H", d, pe.pe + 22, pe.chars & ~1)
    struct.pack_into("<II", d, pe.dir_off(5), va, len(tab))
    struct.pack_into("<I", d, pe.opt + 56, align(va + len(tab), pe.salign))
    return bytes(d)


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
    if len(argv) == 4 and argv[1] == "--relocs":
        data = open(argv[2], "rb").read()
        try:
            rvas = reconstruct_relocs(data)
            fixed = add_relocs(data, rvas)
            new, what = realign(fixed)
            errs = check(fixed, new)
        except PEError as e:
            print("pe4k: %s: %s" % (argv[2], e), file=sys.stderr)
            return 1
        for e in errs:
            print("pe4k: %s: %s" % (argv[2], e), file=sys.stderr)
        if errs:
            return 1
        open(argv[3], "wb").write(new)
        print("pe4k: %s: %d reconstructed fixups; %s" % (argv[2], len(rvas), what))
        return 0
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
