#!/usr/bin/env python3
"""Point an SH-3 module's floating-point imports from COREDLL at SH3CRT.dll.

  sh3imports.py <in> <out>    rewrite, then check

CE 6.0's SH-4 coredll exports the software floating-point helpers (_adds,
_addd, _dtoi, ...) and the double C library functions (sin, pow, atof, ...)
with the SH-4 FPU calling convention, so a Pocket PC SH-3 module that imports
them gets wrong results (bsp/classpad/SRC/APPS/SH3CRT/sh3crt.c). SH3CRT.dll
exports the same names with the SH-3 convention. This tool gives the module
a second import descriptor, for SH3CRT.dll by name, in a new last section
(.sh3crt: descriptors, thunk arrays, names), and moves every reference to an
affected COREDLL IAT slot to the matching new slot: the references are
HIGHLOW fixups (the import thunks' literal-pool words), so the .reloc table
finds them all. The old slots stay (still bound to coredll, now unused).

Input: a module with a .reloc table (after tools/pe4k.py). Imports by
ordinal are named with COREDLL_ORDINALS (CE coredll ordinals are fixed);
imports by name are matched directly.
"""
import struct
import sys

import pe4k

# coredll.def ordinals of the names SH3CRT.dll exports (sh3crt.def)
COREDLL_ORDINALS = {
    "_adds": 2008, "_subs": 2010, "_muls": 2024, "_divs": 2004, "_negs": 2026,
    "_eqs": 2012, "_nes": 2014, "_lts": 2022, "_les": 2020, "_gts": 2018, "_ges": 2016,
    "_itos": 2028, "_utos": 2030, "_stoi": 2033, "_stou": 2032,
    "_addd": 2007, "_subd": 2009, "_muld": 2023, "_divd": 2003, "_negd": 2025,
    "_eqd": 2011, "_ned": 2013, "_ltd": 2021, "_led": 2019, "_gtd": 2017, "_ged": 2015,
    "_dtoi": 2036, "_dtou": 2035, "_dtos": 2034, "_stod": 2031, "_itod": 2027,
    "_utod": 2029, "_i64tod": 2082, "_ui64tod": 2084, "_dtoi64": 2093, "_dtoui64": 2092,
    "sin": 1058, "cos": 1004, "tan": 1075, "asin": 990, "acos": 989, "atan": 991,
    "sinh": 1059, "cosh": 1005, "tanh": 1076, "exp": 1009, "log": 1033, "log10": 1034,
    "sqrt": 1060, "floor": 1013, "ceil": 999, "fabs": 1010, "atan2": 992, "pow": 1051,
    "fmod": 1014, "_hypot": 1023, "atof": 995, "wcstod": 1081,
    "_isnan": 1024, "_finite": 1012, "_fpclass": 1015,
}
BY_ORDINAL = {v: k for k, v in COREDLL_ORDINALS.items()}
SHIM = b"SH3CRT.dll"
SH3_MACHINES = (0x1A2, 0x1A3)


def coredll_slots(pe, m):
    """[(slot rva, name)] of the COREDLL imports SH3CRT.dll replaces."""
    out = []
    rva, _ = pe.dir(1)
    while rva:
        oft, ts, fc, name, ft = struct.unpack_from("<IIIII", m, rva)
        if not (oft or name or ft):
            break
        dll = bytes(m[name:m.index(b"\0", name)]).lower()
        if dll == b"coredll.dll":
            t, s = oft or ft, ft
            while True:
                e = struct.unpack_from("<I", m, t)[0]
                if not e:
                    break
                if e & 0x80000000:
                    n = BY_ORDINAL.get(e & 0xFFFF)
                else:
                    n = bytes(m[e + 2:m.index(b"\0", e + 2)]).decode("latin1")
                    n = n if n in COREDLL_ORDINALS else None
                if n:
                    out.append((s, n))
                t += 4
                s += 4
        rva += 20
    return out


def rewrite(data):
    pe = pe4k.PE(data)
    if struct.unpack_from("<H", pe.d, pe.pe + 4)[0] not in SH3_MACHINES:
        return bytes(pe.d), []
    m, fixups = pe4k.loaded(pe, pe.base)
    if not fixups:
        raise pe4k.PEError("sh3imports: no relocations")
    slots = coredll_slots(pe, m)
    if not slots:
        return bytes(pe.d), []
    hdr = pe.sh + 40 * pe.nsec
    if hdr + 40 > min(s["rp"] for s in pe.secs) or any(pe.d[hdr:hdr + 40]):
        raise pe4k.PEError("sh3imports: no room for another section header")
    last = pe.secs[-1]
    va = pe4k.align(last["va"] + max(last["vs"], last["rs"]), pe.salign)

    # descriptor array: the old ones, SH3CRT, null
    irva, _ = pe.dir(1)
    old = []
    o = irva
    while True:
        desc = bytes(m[o:o + 20])
        if not any(desc):
            break
        old.append(desc)
        o += 20
    n = len(slots)
    d_off = 0
    oft_off = 20 * (len(old) + 2)
    ft_off = oft_off + 4 * (n + 1)
    names_off = ft_off + 4 * (n + 1)
    blob = bytearray(names_off)
    hint = {}
    for _, name in slots:
        if name not in hint:
            hint[name] = len(blob)
            e = struct.pack("<H", 0) + name.encode() + b"\0"
            blob += e + (b"\0" if len(e) & 1 else b"")
    dll_off = len(blob)
    blob += SHIM + b"\0"
    blob += bytes(-len(blob) % 4)
    for i, desc in enumerate(old):
        blob[d_off + 20 * i:d_off + 20 * i + 20] = desc
    struct.pack_into("<IIIII", blob, d_off + 20 * len(old), va + oft_off, 0, 0, va + dll_off, va + ft_off)
    for i, (_, name) in enumerate(slots):
        struct.pack_into("<I", blob, oft_off + 4 * i, va + hint[name])
        struct.pack_into("<I", blob, ft_off + 4 * i, va + hint[name])

    # move the references: every fixup holding an old slot's address
    newslot = {s: va + ft_off + 4 * i for i, (s, _) in enumerate(slots)}
    moved = {s: 0 for s in newslot}
    d = pe.d
    for r in fixups:
        off = pe.off(r, 4)
        t = (pe.u32(off) - pe.base) & 0xFFFFFFFF
        if t in newslot:
            pe.p32(off, pe.base + newslot[t])
            moved[t] += 1

    rp = len(d)
    rs = pe4k.align(len(blob), pe.falign)
    d += blob + bytes(rs - len(blob))
    d[hdr:hdr + 40] = b".sh3crt\0" + struct.pack("<IIIIIIHHI", len(blob), va, rs, rp, 0, 0, 0, 0, 0xC0000040)
    struct.pack_into("<H", d, pe.pe + 6, pe.nsec + 1)
    struct.pack_into("<II", d, pe.dir_off(1), va + d_off, 20 * (len(old) + 2))
    struct.pack_into("<I", d, pe.opt + 56, pe4k.align(va + len(blob), pe.salign))
    report = [(name, moved[s]) for s, name in slots]
    return bytes(d), report


def check(old_data, new_data):
    """The new module imports the same functions, the replaced ones from
    SH3CRT.dll; no fixup still points at a replaced COREDLL slot; every
    replaced slot is referenced at least once from the new slots."""
    errs = []
    old, new = pe4k.PE(old_data), pe4k.PE(new_data)
    mo, fo = pe4k.loaded(old, old.base)
    mn, fn = pe4k.loaded(new, new.base)
    slots = coredll_slots(old, mo)
    replaced = {s for s, _ in slots}
    io, inew = pe4k.imports(old, mo), pe4k.imports(new, mn)
    if inew[:len(io)] != io:
        errs.append("old imports changed")
    shim = [e for e in inew[len(io):]]
    if [(e[0], e[2].decode()) for e in shim] != [(SHIM, n) for _, n in slots]:
        errs.append("SH3CRT imports differ")
    for r in fn:
        t = (struct.unpack_from("<I", mn, r)[0] - new.base) & 0xFFFFFFFF
        if t in replaced:
            errs.append("fixup at 0x%x still points at COREDLL slot 0x%x" % (r, t))
    if sorted(fo) != sorted(fn):
        errs.append("fixup set changed")
    return errs


def main(argv):
    if len(argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    data = open(argv[1], "rb").read()
    try:
        new, report = rewrite(data)
    except pe4k.PEError as e:
        print("sh3imports: %s: %s" % (argv[1], e), file=sys.stderr)
        return 1
    errs = check(data, new) if report else []
    for e in errs:
        print("sh3imports: %s: %s" % (argv[1], e), file=sys.stderr)
    if errs:
        return 1
    open(argv[2], "wb").write(new)
    print("sh3imports: %s: %d imports to SH3CRT.dll, %d references moved%s" % (
        argv[1], len(report), sum(c for _, c in report),
        "; unreferenced: " + ",".join(n for n, c in report if not c) if any(not c for _, c in report) else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
