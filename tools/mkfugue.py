#!/usr/bin/env python3
"""mkfugue.py - build a test image of Casio's storage memory (Fugue + FAT16)
for the CE driver (bsp/classpad50/SRC/DRIVERS/FUGUEDSK, classpad_fugue.h,
classpad_fuguewr.h) and list what such an image holds.

  mkfugue.py -o storage.bin [file ...]     a volume holding the files (8.3 names, root directory)
  mkfugue.py --list <storage.bin|nor.bin>  the root directory of a volume or of a 64 MiB chip dump

The output is the storage area of the chip, 242 sectors of 128 KiB (chip
offset 0x1A20000; `nkflash.py --fugue` puts it into the emulator's NOR). The
FAT geometry is the one OS 02.01.7003 formats (MBR with one type 0E
partition at LBA 29, 49503 sectors, 8 sectors per cluster, 2 FATs of 25
sectors, 512 root entries). The Fugue side is written the way the fx-CP400's
OS writes it (classpad_fuguewr.h): a FINFO record opening each sector used,
the DETAIL record with the erase counters, DATA records with the payload CRC
and slice ECCs for the clusters that have content, and the tail cluster
(0x1837). The driver's writer accepts it. Whether Casio's OS does has not
been tried: do not flash it to a calculator.
"""
import os, struct, sys

SECTORS, SECTOR, PAGE, META_OFF, META = 242, 0x20000, 0x1000, 0x1F000, 0x40
CHIP_OFF = 0x01A20000
TAIL_LC = 0x1837
HIDDEN, TOTAL, SPC, RESERVED, FATS, FATSZ, ROOT = 29, 49503, 8, 1, 2, 25, 512
ROOT_SECS = ROOT * 32 // 512
DATA_START = RESERVED + FATS * FATSZ + ROOT_SECS            # in the partition
CLUSTERS = (TOTAL - DATA_START) // SPC

COL = (0x15, 0x16, 0x19, 0x1A, 0x25, 0x26, 0x29, 0x2A)


def ecc(m):
    """the 3 ECC bytes over a record's first 37 bytes (classpad_fugue.h ClasspadFugueEcc)"""
    r1 = r4 = r7 = 0xFF
    for i, b in enumerate(m[:37]):
        for j in range(8):
            if b >> j & 1:
                r4 ^= COL[j]
        if bin(b).count("1") & 1:
            r1 ^= i
            r7 ^= ~i & 0xFF
    out = [r4 | 0xC0]
    for k in range(2):
        s7 = sum(((r7 >> (4 * k + j)) & 1) << (2 * j) for j in range(4))
        s1 = sum(((r1 >> (4 * k + j)) & 1) << (2 * j + 1) for j in range(4))
        out.append((0xAA | s7) & (0x55 | s1))
    return bytes(out)


def crc16(data):
    """CRC-16/CCITT-FALSE of a payload (record bytes 8..9)"""
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def slice_ecc(data):
    """the 3 ECC bytes of one 512-byte slice (classpad_fuguewr.h ClasspadFugueSliceEcc)"""
    r4, r1, r7 = 0xFF, 0x1FF, 0x1FF
    for i, b in enumerate(data):
        if not b:
            continue
        for j in range(8):
            if b >> j & 1:
                r4 ^= COL[j]
        if bin(b).count("1") & 1:
            r1 ^= i
            r7 ^= ~i & 0x1FF
    out = [((r4 & 0x3F) << 2) | ((r1 >> 8 & 1) << 1) | (r7 >> 8 & 1)]
    for k in (1, 0):
        s7 = sum(((r7 >> (4 * k + j)) & 1) << (2 * j) for j in range(4))
        s1 = sum(((r1 >> (4 * k + j)) & 1) << (2 * j + 1) for j in range(4))
        out.append((0xAA | s7) & (0x55 | s1))
    return bytes(out)


def record(lc, version, payload=None, kind=0x11):
    m = bytearray(b"\xff" * META)
    m[0], m[1] = kind, 0x0F
    m[2:6] = struct.pack(">I", version)
    m[6:8] = struct.pack(">H", lc)
    if payload is not None:
        m[8:10] = struct.pack(">H", crc16(payload))
        for q in range(8):
            m[10 + 3 * q:13 + 3 * q] = slice_ecc(payload[q * 512:(q + 1) * 512])
    m[34:37] = b"ZZZ"
    m[37:40] = ecc(m)
    return bytes(m)


def name83(path):
    base = os.path.basename(path).upper()
    stem, _, ext = base.partition(".")
    if not stem or len(stem) > 8 or len(ext) > 3 or "." in ext or not all(c.isalnum() or c in "_-~" for c in stem + ext):
        sys.exit("mkfugue: %s is not an 8.3 name" % base)
    return stem.ljust(8).encode() + ext.ljust(3).encode()


def build(files):
    disk = {}                                               # LBA -> 512 bytes (only what has content)

    def put(lba, data):
        for i in range(0, len(data), 512):
            disk[lba + i // 512] = data[i:i + 512].ljust(512, b"\0")

    mbr = bytearray(512)
    mbr[446:462] = struct.pack("<B3sB3sII", 0, b"\x01\x01\x00", 0x0E, b"\x00\x00\x00", HIDDEN, TOTAL)
    mbr[510:512] = b"\x55\xaa"
    put(0, bytes(mbr))
    bs = bytearray(512)
    bs[0:3] = b"\xeb\x3c\x90"
    bs[3:11] = b"MSDOS5.0"
    struct.pack_into("<HBHBHHBHHHII", bs, 11, 512, SPC, RESERVED, FATS, ROOT, TOTAL, 0xF8, FATSZ, 63, 255, HIDDEN, 0)
    bs[36:39] = b"\x80\x00\x29"
    bs[39:43] = b"CASI"
    bs[43:54] = b"NO NAME    "
    bs[54:62] = b"FAT16   "
    bs[510:512] = b"\x55\xaa"
    put(HIDDEN, bytes(bs))

    fat = [0xFFF8, 0xFFFF] + [0] * CLUSTERS
    root = bytearray()
    nxt = 2
    for path in files:
        data = open(path, "rb").read()
        n = (len(data) + SPC * 512 - 1) // (SPC * 512)
        if nxt + n > CLUSTERS + 2:
            sys.exit("mkfugue: the files do not fit the volume")
        first = nxt if n else 0
        for c in range(nxt, nxt + n):
            fat[c] = c + 1 if c + 1 < nxt + n else 0xFFFF
        put(HIDDEN + DATA_START + (nxt - 2) * SPC, data)
        nxt += n
        root += name83(path) + bytes([0x20]) + bytes(14) + struct.pack("<HI", first, len(data))
    if len(root) > ROOT * 32:
        sys.exit("mkfugue: too many files for the root directory")
    fatb = b"".join(struct.pack("<H", e) for e in fat).ljust(FATSZ * 512, b"\0")
    for k in range(FATS):
        put(HIDDEN + RESERVED + k * FATSZ, fatb)
    put(HIDDEN + RESERVED + FATS * FATSZ, bytes(root).ljust(ROOT_SECS * 512, b"\0"))
    # the last sector of the partition, as Casio's formatter writes it (zeros)
    put(HIDDEN + TOTAL - 1, bytes(512))

    vol = bytearray(b"\xff" * (SECTORS * SECTOR))
    state = {"slot": 0}                                     # running page index over all sectors, 30 pages each after the header

    def append(kind, lc, page):
        s, p = divmod(state["slot"], 30)
        if s >= SECTORS:
            sys.exit("mkfugue: the volume is full")
        if p == 0:                                          # a new sector: its FINFO record, words counting down
            word = 0xFFFFFFFE - s
            head = (struct.pack(">II", word, ~word & 0xFFFFFFFF) * 3).ljust(PAGE, b"\xff")
            vol[s * SECTOR:s * SECTOR + PAGE] = head
            vol[s * SECTOR + META_OFF:s * SECTOR + META_OFF + META] = record(0xFFFF, 0xFFFFFFFF, head, 0x22)
        p += 1
        vol[s * SECTOR + p * PAGE:s * SECTOR + (p + 1) * PAGE] = page
        vol[s * SECTOR + META_OFF + p * META:s * SECTOR + META_OFF + (p + 1) * META] = record(lc, 0xFFFFFFFE, page, kind)
        state["slot"] += 1

    # the DETAIL record: no erase announced, every sector's erase count 0
    detail = bytearray(b"\xff" * PAGE)
    detail[0x200:0x200 + 3 * SECTORS] = bytes(3 * SECTORS)
    append(0x88, 0xFFFF, bytes(detail))
    for lc in sorted({lba // 8 for lba in disk}):
        append(0x11, lc, b"".join(disk.get(lc * 8 + i, b"\xff" * 512) for i in range(8)))
    # the tail cluster: the last raw sector of the 200-sector span, ending in CCCCCCCC
    append(0x11, TAIL_LC, b"\xff" * (7 * 512) + bytes(508) + b"\xcc" * 4)
    return bytes(vol)


def volume_of(data):
    if len(data) >= CHIP_OFF + SECTORS * SECTOR:
        return data[CHIP_OFF:CHIP_OFF + SECTORS * SECTOR]
    if len(data) == SECTORS * SECTOR:
        return data
    sys.exit("mkfugue: neither a %d-byte volume nor a chip dump" % (SECTORS * SECTOR))


def live_map(vol):
    live = {}
    for s in range(SECTORS):
        for p in range(1, 31):
            m = vol[s * SECTOR + META_OFF + p * META:s * SECTOR + META_OFF + (p + 1) * META]
            if m[0] != 0x11 or m[1] != 0x0F or m[34:37] != b"ZZZ" or m[37:40] != ecc(m):
                continue
            v, lc = struct.unpack(">IH", m[2:8])
            if lc not in live or v < live[lc][0]:
                live[lc] = (v, s * SECTOR + p * PAGE)
    return live


def listing(vol):
    live = live_map(vol)

    def lba(n):
        if n // 8 not in live:
            return b"\xff" * 512
        o = live[n // 8][1] + n % 8 * 512
        return vol[o:o + 512]

    mbr = lba(0)
    if mbr[510:512] != b"\x55\xaa":
        sys.exit("mkfugue: no MBR in the volume")
    start, length = struct.unpack("<II", mbr[454:462])
    bs = lba(start)
    res, nfat, nroot, fatsz = struct.unpack_from("<H", bs, 14)[0], bs[16], struct.unpack_from("<H", bs, 17)[0], struct.unpack_from("<H", bs, 22)[0]
    print("%d live clusters; partition type %02X at LBA %d, %d sectors; %s" % (len(live), mbr[450], start, length, bs[54:62].decode("ascii", "replace").strip()))
    for i in range(nroot):
        e = lba(start + res + nfat * fatsz + i // 16)[i % 16 * 32:i % 16 * 32 + 32]
        if e[0] == 0:
            break
        if e[0] == 0xE5 or e[11] == 0x0F:
            continue
        name = e[0:8].decode("ascii", "replace").rstrip() + ("." + e[8:11].decode("ascii", "replace").rstrip() if e[8:11].strip() else "")
        print("  %-12s %s %9d" % (name, "<DIR>" if e[11] & 0x10 else "     ", struct.unpack_from("<I", e, 28)[0]))


def main(argv):
    args = argv[1:]
    if len(args) == 2 and args[0] == "--list":
        listing(volume_of(open(args[1], "rb").read()))
        return 0
    if len(args) >= 2 and args[0] == "-o":
        vol = build(args[2:])
        open(args[1], "wb").write(vol)
        sys.stderr.write("mkfugue: %s, %d files\n" % (args[1], len(args) - 2))
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
