#!/usr/bin/env python3
"""nkbin.py - inspect a Windows CE .bin image (B000FF format) and its ROMHDR.

Usage:
  nkbin.py NK.bin                 summary: image range, records, entry, ROMHDR, modules
  nkbin.py NK.bin --flat out.nb0  write a flat memory image starting at image start
  nkbin.py NK.bin --records       list every record

The .bin format (ROMIMAGE output):
  "B000FF\\n"  7-byte magic
  u32 image start, u32 image length
  records: u32 address, u32 length, u32 checksum, data[length]
  terminator: address == 0, checksum == 0, length == entry point
The ROMHDR is found via the "ECEC" signature at image_start+0x40, the
following u32 being the physical address of the ROMHDR structure.
"""
import struct, sys

MAGIC = b"B000FF\n"
CPU_NAMES = {0x01A2: "SH3", 0x01A3: "SH3DSP", 0x01A6: "SH4", 0x01C0: "ARM",
             0x01C2: "THUMB", 0x0166: "MIPS16/R4000", 0x014C: "x86", 0x0169: "MIPSFPU"}

ROMHDR_FMT = "<IIIIIIIIIIIIIIIIIHHIII"
ROMHDR_FIELDS = ("dllfirst", "dlllast", "physfirst", "physlast", "nummods",
                 "ulRAMStart", "ulRAMFree", "ulRAMEnd", "ulCopyEntries", "ulCopyOffset",
                 "ulProfileLen", "ulProfileOffset", "numfiles", "ulKernelFlags",
                 "ulFSRamPercent", "ulDrivglobStart", "ulDrivglobLen", "usCPUType",
                 "usMiscFlags", "pExtensions", "ulTrackingStart", "ulTrackingLen")
TOC_FMT = "<IIIIIIII"   # dwFileAttributes, ftTime(lo,hi), nFileSize, lpszFileName, ulE32Offset, ulO32Offset, ulLoadOffset


class BinImage:
    def __init__(self, data):
        if not data.startswith(MAGIC):
            raise ValueError("not a B000FF image")
        self.start, self.length = struct.unpack_from("<II", data, 7)
        self.records = []
        self.entry = None
        off = 7 + 8
        while off + 12 <= len(data):
            addr, ln, chk = struct.unpack_from("<III", data, off)
            off += 12
            if addr == 0 and chk == 0:
                self.entry = ln
                break
            payload = data[off:off + ln]
            if len(payload) != ln:
                raise ValueError("truncated record at 0x%x" % addr)
            if (sum(payload) & 0xFFFFFFFF) != chk:
                print("warning: checksum mismatch for record 0x%08x" % addr, file=sys.stderr)
            self.records.append((addr, payload))
            off += ln
        self.end = self.start + self.length

    def flat(self, fill=0x00):
        buf = bytearray([fill]) * self.length
        for addr, payload in self.records:
            o = addr - self.start
            if o < 0 or o + len(payload) > self.length:
                raise ValueError("record 0x%08x outside image range" % addr)
            buf[o:o + len(payload)] = payload
        return bytes(buf)

    def read(self, addr, n):
        for raddr, payload in self.records:
            if raddr <= addr and addr + n <= raddr + len(payload):
                o = addr - raddr
                return payload[o:o + n]
        return None

    def cstr(self, addr, limit=256):
        out = bytearray()
        while len(out) < limit:
            b = self.read(addr + len(out), 1)
            if not b or b == b"\0":
                break
            out += b
        return out.decode("ascii", "replace")

    def romhdr(self):
        sig = self.read(self.start + 0x40, 8)
        if not sig or sig[:4] != b"ECEC":
            return None, None
        (ptr,) = struct.unpack_from("<I", sig, 4)
        raw = self.read(ptr, struct.calcsize(ROMHDR_FMT))
        if raw is None:
            return ptr, None
        return ptr, dict(zip(ROMHDR_FIELDS, struct.unpack(ROMHDR_FMT, raw)))

    def modules(self, hdr_ptr, hdr):
        toc = hdr_ptr + struct.calcsize(ROMHDR_FMT)
        size = struct.calcsize(TOC_FMT)
        out = []
        for i in range(hdr["nummods"]):
            raw = self.read(toc + i * size, size)
            if raw is None:
                break
            attrs, tlo, thi, fsize, name_ptr, e32, o32, load = struct.unpack(TOC_FMT, raw)
            out.append((self.cstr(name_ptr), fsize, e32, o32, load))
        return out


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    data = open(argv[1], "rb").read()
    img = BinImage(data)
    print("image   : 0x%08x .. 0x%08x (%d bytes, %d records)" % (img.start, img.end, img.length, len(img.records)))
    print("entry   : 0x%08x" % (img.entry or 0))
    if "--records" in argv:
        for addr, payload in img.records:
            print("  record 0x%08x  %8d bytes" % (addr, len(payload)))
    ptr, hdr = img.romhdr()
    if hdr is None:
        print("ROMHDR  : not found (no ECEC signature at image start + 0x40)")
    else:
        print("ROMHDR  : at 0x%08x" % ptr)
        cpu = hdr["usCPUType"]
        print("  CPU            : 0x%04x (%s)" % (cpu, CPU_NAMES.get(cpu, "?")))
        print("  ROM (phys)     : 0x%08x .. 0x%08x" % (hdr["physfirst"], hdr["physlast"]))
        print("  RAM            : start 0x%08x free 0x%08x end 0x%08x" % (hdr["ulRAMStart"], hdr["ulRAMFree"], hdr["ulRAMEnd"]))
        print("  modules/files  : %d / %d   copy entries: %d   kernel flags: 0x%x   FS RAM %%: %d"
              % (hdr["nummods"], hdr["numfiles"], hdr["ulCopyEntries"], hdr["ulKernelFlags"], hdr["ulFSRamPercent"]))
        mods = img.modules(ptr, hdr)
        if mods:
            print("  modules:")
            for name, fsize, e32, o32, load in mods:
                print("    %-28s %8d bytes  e32 0x%08x  o32 0x%08x  load 0x%08x" % (name, fsize, e32, o32, load))
    if "--flat" in argv:
        out = argv[argv.index("--flat") + 1]
        open(out, "wb").write(img.flat())
        print("wrote flat image %s (%d bytes, base 0x%08x)" % (out, img.length, img.start))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
