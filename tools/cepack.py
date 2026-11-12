#!/usr/bin/env python3
"""cepack.py - pack NK.bin / EXT.bin for the calculator's storage memory.

The loader (wince.hh3, dbt/hhk/cez.c) reads an image file either as Platform
Builder wrote it (B000FF) or packed by this tool, under the same name: the
file in \\fls0 is then about half the size, and the loader unpacks it while
it compares and writes.

    cepack.py NK.bin EXT.bin -o out/      out/NK.bin, out/EXT.bin (packed)
    cepack.py -d out/NK.bin -o raw/       back to what Platform Builder wrote

Format (all fields little-endian):
    +0   "CEZ2"
    +4   size of the unpacked file
    +8   block size (BLOCK: what the loader's buffer holds)
    +12  number of blocks
    +16  number of records in the record table (0: the file is not B000FF)
    +20  CRC-32 of the unpacked file
    +24  one word per block: its stored length; bit 31 set = stored as is
    then the record table, three words per B000FF record: its address, its
    length, the CRC-32 of its data. With it the loader knows what the image
    covers and can tell whether the flash already holds it (a CRC over the
    flash) without unpacking anything.
    then the blocks, each BLOCK bytes of the file (the last one shorter) as
    a raw deflate stream of its own, so the loader can start at any block.
("CEZ1", the first format, had no record count, CRC or record table; the
loader still reads it.)
"""
import argparse, os, struct, sys, zlib

MAGIC = b"CEZ2"
BLOCK = 32768
RAW = 0x80000000


def records(data):
    """(address, length, crc32) of every record of a B000FF file, [] for anything else"""
    if data[:7] != b"B000FF\n":
        return []
    out, pos = [], 15
    while True:
        if pos + 12 > len(data):
            raise ValueError("B000FF file ends inside a record")
        addr, length, check = struct.unpack_from("<III", data, pos)
        pos += 12
        if addr == 0 and check == 0:
            return out
        if pos + length > len(data):
            raise ValueError("B000FF file ends inside a record")
        out.append((addr, length, zlib.crc32(data[pos:pos + length]) & 0xFFFFFFFF))
        pos += length


def pack(data):
    blocks, table = [], []
    for i in range(0, len(data), BLOCK):
        piece = data[i:i + BLOCK]
        c = zlib.compressobj(9, zlib.DEFLATED, -15)
        z = c.compress(piece) + c.flush()
        if len(z) < len(piece):
            blocks.append(z); table.append(len(z))
        else:
            blocks.append(piece); table.append(len(piece) | RAW)
    recs = records(data)
    head = MAGIC + struct.pack("<IIIII", len(data), BLOCK, len(blocks), len(recs), zlib.crc32(data) & 0xFFFFFFFF)
    return (head + struct.pack("<%uI" % len(table), *table) +
            b"".join(struct.pack("<III", *r) for r in recs) + b"".join(blocks))


def unpack(data):
    if data[:4] not in (MAGIC, b"CEZ1"):
        raise ValueError("not a packed image")
    size, block, n = struct.unpack_from("<III", data, 4)
    nrec, crc = struct.unpack_from("<II", data, 16) if data[:4] == MAGIC else (0, None)
    first = 24 if data[:4] == MAGIC else 16
    table = struct.unpack_from("<%uI" % n, data, first)
    pos, out = first + 4 * n + 12 * nrec, []
    for t in table:
        length = t & ~RAW
        piece = data[pos:pos + length]
        out.append(piece if t & RAW else zlib.decompress(piece, -15))
        pos += length
    out = b"".join(out)
    if len(out) != size or pos != len(data) or (crc is not None and zlib.crc32(out) & 0xFFFFFFFF != crc):
        raise ValueError("packed image is damaged")
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("files", nargs="+")
    ap.add_argument("-o", "--out", required=True, help="directory for the results (same file names)")
    ap.add_argument("-d", "--unpack", action="store_true")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    for path in a.files:
        data = open(path, "rb").read()
        dest = os.path.join(a.out, os.path.basename(path))
        if os.path.exists(dest) and os.path.samefile(path, dest):
            sys.exit("%s: the result would overwrite the input" % path)
        if a.unpack:
            result = unpack(data)
        else:
            if data[:4] in (MAGIC, b"CEZ1"):
                sys.exit("%s is packed already" % path)
            result = pack(data)
            assert unpack(result) == data
        open(dest, "wb").write(result)
        print("%s: %u -> %u bytes (%u%%)" % (dest, len(data), len(result), len(result) * 100 // max(len(data), 1)))


if __name__ == "__main__":
    main()
