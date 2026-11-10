#!/usr/bin/env python3
"""hhkcheck.py - check a HollyHock program built by dbt/Makefile.

  hhkcheck.py <prog.hhk> [<dbt-image.bin> <dbt-image.g3a> <payload_base>]

Always: a big-endian SH ET_EXEC ELF whose loadable sections lie inside the
regions a HollyHock 3 program may use (the program window 0x8CFF0000-
0x8D000000; for a gint program also the VRAM backup area 0x8C052800 +
320*528*2 and the on-chip IL/X/Y memories, gint's fxcp_hh3.ld) or, for the
payload, at payload_base; a .note.hollyhock NAME note; an entry inside one
of those regions.
With the image and the .g3a: the .wince.payload section is byte-identical to
the translator image, and so is the image that casio-emu's .g3a carries, so
the device and the emulator run the same translator bytes.
Exit status 0 when every check passes.
"""
import struct, sys

REGIONS = [(0x8CFF0000, 0x8D000000), (0x8C052800, 0x8C052800 + 320 * 528 * 2),
           (0xE5200000, 0xE5201000), (0xE500E000, 0xE5012000)]

def inside(addr, size=1):
    return any(lo <= addr and addr + size <= hi for lo, hi in REGIONS)

def sections(elf):
    (e_shoff, e_shentsize, e_shnum, e_shstrndx) = (struct.unpack_from(">I", elf, 0x20)[0],) + struct.unpack_from(">HHH", elf, 0x2E)
    shdrs = [struct.unpack_from(">IIIIIIIIII", elf, e_shoff + i * e_shentsize) for i in range(e_shnum)]
    strtab = shdrs[e_shstrndx]
    names = elf[strtab[4]:strtab[4] + strtab[5]]
    out = []
    for sh in shdrs:
        name = names[sh[0]:names.index(b"\0", sh[0])].decode()
        out.append((name, sh[1], sh[2], sh[3], sh[4], sh[5]))   # name, type, flags, addr, off, size
    return out

def main(argv):
    if len(argv) not in (2, 5):
        print(__doc__); return 2
    bad = []
    elf = open(argv[1], "rb").read()
    if elf[:4] != b"\x7fELF" or elf[4] != 1 or elf[5] != 2:
        bad.append("not a 32-bit big-endian ELF")
    e_type, e_machine = struct.unpack_from(">HH", elf, 0x10)
    entry = struct.unpack_from(">I", elf, 0x18)[0]
    if e_type != 2 or e_machine != 42: bad.append("not an SH executable")
    if not inside(entry): bad.append("entry 0x%08x outside the HollyHock regions" % entry)
    image_base = int(argv[4], 0) if len(argv) == 5 else None
    payload = None
    note = False
    for name, typ, flags, addr, off, size in sections(elf):
        if name == ".wince.payload":
            payload = elf[off:off + size]
            if image_base is not None and addr != image_base:
                bad.append("payload at 0x%08x, expected 0x%08x" % (addr, image_base))
        elif flags & 2 and size:                                 # SHF_ALLOC
            if not inside(addr, size):
                bad.append("section %s 0x%08x+0x%x outside the HollyHock regions" % (name, addr, size))
        if name == ".note.hollyhock" and b"NAME\0" in elf[off:off + size]:
            note = True
    if not note: bad.append("no .note.hollyhock NAME note")
    if len(argv) == 5:
        image = open(argv[2], "rb").read()
        g3a = open(argv[3], "rb").read()
        if payload is None: bad.append("no .wince.payload section")
        elif payload != image: bad.append("payload differs from %s" % argv[2])
        if g3a.find(image) < 0: bad.append("the .g3a does not carry %s" % argv[2])
        else: print("hhkcheck: payload (%d bytes) identical in %s and %s" % (len(image), argv[1], argv[3]))
    for b in bad: print("hhkcheck: %s: %s" % (argv[1], b))
    if not bad: print("hhkcheck: %s ok (entry 0x%08x)" % (argv[1], entry))
    return 1 if bad else 0

if __name__ == "__main__":
    sys.exit(main(sys.argv))
