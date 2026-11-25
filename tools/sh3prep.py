#!/usr/bin/env python3
"""Prepare a Pocket PC SH-3 module for the ClassPad image's XIP regions.

  sh3prep.py <in> <out> [--name <bib name>]

The steps tools/windows/claude-build/stage.py runs on every platform.bib
module under FILES, for one file (the CE 5.0 tree has no stage.py run: its
PLATFORM\\CLASSPAD\\FILES keeps the prepared binaries, tools/stage_bsp50.py):

  1. a /FIXED EXE listed in RELOC_PINS gets its relocations rebuilt
     (tools/pe4k.py reconstruct_relocs), only for the copy reviewed by hand:
     the input's SHA-256 and the fixup count must match;
  2. 4 KiB sections (tools/pe4k.py realign; romimage takes no 1 KiB-aligned
     XIP module), checked against the original after relocating both;
  3. SH-3 floating-point imports from COREDLL to SH3CRT.dll
     (tools/sh3imports.py), checked;
  4. Math Xpander's three plane DLLs widened to the 320-pixel screen
     (tools/xpander_widen.py).

The name (default: the input's file name) selects the pin and the widening.
"""
import hashlib
import os
import sys

import pe4k
import sh3imports
import xpander_widen

# /FIXED EXEs whose relocations pe4k.reconstruct_relocs rebuilds: the SHA-256
# of the copy checked by hand, its fixup count, and the RVAs of the words the
# review found to be numbers although the scan's rules call them addresses.
RELOC_PINS = {
    # Math Xpander 1.0 PPC SH3: 157 literal-pool addresses in .text, 43
    # vtable and message-map pointers in .rdata, 2 in .data, 20 .pdata
    # function starts; .rsrc has none.
    "mathxpander.exe": ("09da9ddf8e965a6fe9cf3d2f35354bdc919a0839f7ccff65017fffdb2ea09417", 222, ()),
    # SpreadCE 2.05 for Pocket PC SH3, English (Bye Design, 2012-06-27;
    # sprep3en.zip): the .pdata scan (pe4k.reconstruct_relocs_pdata). 14977
    # literal-pool addresses into .text (7170 .pdata function starts, 7032 of
    # the 264 import thunks, the rest leaf functions and far-jump targets),
    # 3459 pool and 3496 .rdata/.data pointers to data (7 of the pool words
    # in leaf functions' pools: the list separator and decimal point
    # helpers, a getter of four globals), 1247 function pointers in
    # .rdata/.data tables, 2377 .pdata starts. The two numbers: 0x00150013
    # and 0x00150075, odd, in tables of 16-bit pairs in .rdata.
    "spreadce.exe": ("273d94296644ee611fa6cd78eecbe99d753bfb15b4a476f7fe01f87d67f3feac", 25556,
                     (0x13F53C, 0x145A84)),
}


def prepare(name, data):
    """(new bytes, [messages]) for the module called name; raises
    pe4k.PEError with what failed."""
    msgs = []
    pin = RELOC_PINS.get(name.lower())
    if pin:
        if hashlib.sha256(data).hexdigest() != pin[0]:
            raise pe4k.PEError("not the copy whose relocations were checked (SHA-256)")
        rvas = pe4k.reconstruct_relocs(data, constants=pin[2])
        if len(rvas) != pin[1]:
            raise pe4k.PEError("%d fixups found, %d expected" % (len(rvas), pin[1]))
        data = pe4k.add_relocs(data, rvas)
        msgs.append("%d relocations rebuilt" % len(rvas))
    new, what = pe4k.realign(data)
    errs = pe4k.check(data, new)
    if errs:
        raise pe4k.PEError("; ".join(errs[:5]))
    msgs.append(what)
    fixed, moved = sh3imports.rewrite(new)
    if moved:
        errs = sh3imports.check(new, fixed)
        if errs:
            raise pe4k.PEError("; ".join(errs[:5]))
        idle = [n for n, c in moved if not c]
        msgs.append("%d imports -> SH3CRT.dll" % len(moved))
        if idle:
            msgs.append("imports but never calls %s" % ",".join(idle))
        new = fixed
    if name in xpander_widen.MODULES:
        new, sites = xpander_widen.widen(new, name)
        if sites:
            msgs.append("plane width 231 -> 312 at %d sites" % len(sites))
    return new, msgs


def main(argv):
    args = argv[1:]
    name = None
    if "--name" in args:
        i = args.index("--name")
        name = args[i + 1]
        del args[i:i + 2]
    if len(args) != 2:
        print(__doc__)
        return 2
    src, dst = args
    data = open(src, "rb").read()
    name = name or os.path.basename(src)
    try:
        new, msgs = prepare(name, data)
    except pe4k.PEError as e:
        print("sh3prep: %s: %s" % (name, e), file=sys.stderr)
        return 1
    open(dst, "wb").write(new)
    print("sh3prep: %s: %s" % (name, "; ".join(msgs)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
