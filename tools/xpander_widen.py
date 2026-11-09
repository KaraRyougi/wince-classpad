#!/usr/bin/env python3
"""xpander_widen.py - make Math Xpander's graph and geometry planes as wide as the fx-CP400 screen.

  xpander_widen.py <dir with GraphFrame.dll and GeometryPane.dll> [--check]

Math Xpander was written for the HP Xpander's 240x320 screen, and three
places keep that size (docs/08, 2026-11-09):
  - GraphFrame.dll and GeometryPane.dll lay out their plane view and build
    the GraphicalDomainC (the world-to-pixel mapping) with a pixel width of
    231 (height 161 for the graph, 211 for geometry). SH-3 code builds 231
    as "mov #127,rN; add #104,rN" (an immediate is at most 127); each such
    pair becomes "mov #78,rN; shll2 rN" = 312, the widest value two
    instructions give that fits the 320-pixel pane with the margins of the
    original (240 - 231 = 9, 320 - 312 = 8). The heights stay.
  - GraphicsPlane.dll's RectPlaneViewC::OnCreate draws the plane through a
    back buffer from CreateCompatibleBitmap(dc, 240, 320): everything past
    x = 240 was cut off whatever the window and domain said. The two
    arguments ("mov #120,r5; shll r5", "mov #80,r6; shll2 r6") become
    320 x 512 ("mov #80,r5; shll2 r5", "mov #2,r6; shll8 r6"): the screen
    width, and the tallest value two instructions give below the screen
    height (a plane never gets the whole 528).
The sites are found by pattern, not by address, and their count is checked,
so the tool refuses a module it does not recognise; --check only reports. The staged copies in the VM's
PLATFORM\\CLASSPAD\\FILES\\xpander are pe4k-realigned, which leaves .text
alone, so the tool applies to them and to the original cab files alike.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pe4k

# module -> expected number of sites (231-width pairs, or the one back-buffer call)
MODULES = {"GraphFrame.dll": 4, "GeometryPane.dll": 4, "GraphicsPlane.dll": 1}
OLD_VALUE = 127 + 104
NEW_MOV_IMM = 78            # 78 << 2 = 312

# GraphicsPlane.dll, RectPlaneViewC::OnCreate: CreateCompatibleBitmap(dc, r5 = 240, r6 = 320)
#   mov #120,r5 / (one instruction) / shll r5 / mov #80,r6 / shll2 r6
BITMAP_OLD = (0xE578, None, 0x4500, 0xE650, 0x4608)
BITMAP_NEW = (0xE550, None, 0x4508, 0xE602, 0x4618)      # mov #80,r5; shll2 r5 = 320; mov #2,r6; shll8 r6 = 512


def text_section(pe):
    for s in pe.secs:
        if s["name"].startswith(".text"):
            return s
    raise pe4k.PEError("no .text section")


def find_sites(pe):
    """[(mov file offset, add file offset, register)] of every mov #127,rN ... add #104,rN pair."""
    s = text_section(pe)
    d = pe.d
    sites = []
    o = s["rp"]
    end = s["rp"] + s["rs"]
    while o + 2 <= end:
        w = struct.unpack_from("<H", d, o)[0]
        if (w & 0xF0FF) == 0xE07F:                          # mov #127,rN  (1110 nnnn 0111 1111)
            n = (w >> 8) & 0xF
            for k in range(1, 8):                           # the add follows within a few instructions
                p = o + 2 * k
                if p + 2 > end:
                    break
                w2 = struct.unpack_from("<H", d, p)[0]
                if w2 == 0x7000 | (n << 8) | 104:           # add #104,rN  (0111 nnnn iiii iiii)
                    sites.append((o, p, n))
                    break
                # another instruction writing rN in between: not our pair
                if (w2 & 0xF000) in (0xE000, 0x7000) and ((w2 >> 8) & 0xF) == n:
                    break
        o += 2
    return sites


def find_bitmap_sites(pe, words):
    """[file offset] of every occurrence of the 5-halfword pattern (None = any) in .text."""
    s = text_section(pe)
    d = pe.d
    out = []
    for o in range(s["rp"], s["rp"] + s["rs"] - 10, 2):
        if all(w is None or struct.unpack_from("<H", d, o + 2 * k)[0] == w for k, w in enumerate(words)):
            out.append(o)
    return out


def widen(data, name):
    """The module's bytes with its plane width widened; (new bytes, sites). Raises PEError when the
    module is not one of MODULES or its sites are not what this tool expects."""
    if name not in MODULES:
        raise pe4k.PEError("%s: not a module this tool widens" % name)
    pe = pe4k.PE(data)
    want = MODULES[name]
    if name == "GraphicsPlane.dll":
        sites = find_bitmap_sites(pe, BITMAP_OLD)
        if not sites and len(find_bitmap_sites(pe, BITMAP_NEW)) == want:
            return bytes(pe.d), []
        if len(sites) != want:
            raise pe4k.PEError("%s: found %d back-buffer sites, expected %d" % (name, len(sites), want))
        for o in sites:
            for k, w in enumerate(BITMAP_NEW):
                if w is not None:
                    struct.pack_into("<H", pe.d, o + 2 * k, w)
        return bytes(pe.d), [(o, o + 4, 5) for o in sites]
    sites = find_sites(pe)
    want = MODULES[name]
    if not sites and count_patched(pe) >= want:
        return bytes(pe.d), []                              # already widened (the pattern also matches a few unrelated mov/shll2 pairs)
    if len(sites) != want:
        raise pe4k.PEError("%s: found %d sites of %d, expected %d" % (name, len(sites), OLD_VALUE, want))
    for mov_off, add_off, n in sites:
        struct.pack_into("<H", pe.d, mov_off, 0xE000 | (n << 8) | NEW_MOV_IMM)   # mov #78,rN
        struct.pack_into("<H", pe.d, add_off, 0x4008 | (n << 8))                 # shll2 rN
    return bytes(pe.d), sites


def process(path, check):
    name = os.path.basename(path)
    data = open(path, "rb").read()
    try:
        new, sites = widen(data, name)
    except pe4k.PEError as e:
        sys.exit("%s: not touching it" % e)
    if not sites:
        print("%s: already widened" % name)
        return 0
    text = text_section(pe4k.PE(data))["rp"]
    for mov_off, add_off, n in sites:
        if name == "GraphicsPlane.dll":
            print("%s: %s back buffer at .text+0x%x: 240x320 -> 320x512" % (name, "would patch" if check else "patching", mov_off - text))
        else:
            print("%s: %s at .text+0x%x/0x%x r%d: %d -> %d" % (name, "would patch" if check else "patching",
                  mov_off - text, add_off - text, n, OLD_VALUE, NEW_MOV_IMM << 2))
    if not check:
        open(path, "wb").write(new)
        print("%s: written" % name)
    return len(sites)


def count_patched(pe):
    s = text_section(pe)
    d = pe.d
    count = 0
    o = s["rp"]
    end = s["rp"] + s["rs"]
    while o + 2 <= end:
        w = struct.unpack_from("<H", d, o)[0]
        if (w & 0xF0FF) == 0xE000 | NEW_MOV_IMM:
            n = (w >> 8) & 0xF
            for k in range(1, 8):
                p = o + 2 * k
                if p + 2 > end:
                    break
                if struct.unpack_from("<H", d, p)[0] == 0x4008 | (n << 8):
                    count += 1
                    break
        o += 2
    return count


def main(argv):
    check = "--check" in argv
    args = [a for a in argv[1:] if a != "--check"]
    if len(args) != 1:
        print(__doc__)
        return 2
    for name in MODULES:
        path = os.path.join(args[0], name)
        if not os.path.exists(path):
            sys.exit("%s: not found" % path)
        process(path, check)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
