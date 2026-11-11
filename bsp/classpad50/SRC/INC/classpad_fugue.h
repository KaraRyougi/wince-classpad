//------------------------------------------------------------------------------
//  classpad_fugue.h - read-only view of Casio's storage memory (\fls0)
//
//  Casio's OS keeps its 24 MiB "storage memory" in the NOR at chip offsets
//  0x1A20000-0x385FFFF (docs/03) under Fugue, a log-structured translation
//  layer, with an ordinary FAT16 volume on top. This header turns the raw
//  flash into the block device that FAT sees, for reading only: nothing here
//  writes, so Casio's OS finds the volume exactly as it left it.
//
//  Layout (checked against OS 02.01.7003's volume, bsp/tests/test_fugue.c):
//    - 242 physical sectors of 128 KiB. A sector is 31 pages of 4 KiB followed
//      by a 4 KiB metadata page of 64-byte records, record n describing page n.
//    - page 0 / record 0: the sector's own FINFO record (kind 0x22).
//    - pages 1..30: DATA records (kind 0x11), one 4 KiB logical cluster each:
//        +0  kind      0x11
//        +1  0x0F
//        +2  version   32 bits, big-endian; a rewrite gets a SMALLER number,
//                      so the live copy of a cluster is the smallest version
//        +6  cluster   16 bits, big-endian: the logical cluster number
//        +34 "ZZZ"
//        +37 ECC       3 bytes over +0..+36 (ClasspadFugueEcc)
//      Old copies stay in the flash until Fugue reclaims their sector.
//    - the block device has 512-byte sectors: LBA n is bytes (n % 8) * 512 of
//      logical cluster n / 8. LBA 0 is an MBR whose first partition (FAT16,
//      start 29) is the volume. A cluster without a record reads as 0xFF.
//
//  A record is taken only when kind, the 0x0F byte, "ZZZ" and the ECC all
//  match; Casio's OS would also repair a single flipped bit, this code skips
//  such a record instead (a half-written record must not win).
//
//  Byte accesses only, so the same code runs in the CE driver (little-endian
//  guest on the big-endian chip: the translator leaves byte accesses alone),
//  in a host test and in a translator guest.
//------------------------------------------------------------------------------
#ifndef CLASSPAD_FUGUE_H
#define CLASSPAD_FUGUE_H

// The storage memory in the chip: P1 address, physical sectors.
#define CLASSPAD_FUGUE_PA               0x81A20000u
#define CLASSPAD_FUGUE_SECTORS          242u

#define CLASSPAD_FUGUE_SECTOR_BYTES     0x20000u
#define CLASSPAD_FUGUE_PAGE_BYTES       0x1000u         // one logical cluster
#define CLASSPAD_FUGUE_META_OFF         0x1F000u        // the metadata page of a sector
#define CLASSPAD_FUGUE_META_BYTES       0x40u
#define CLASSPAD_FUGUE_DATA_SLOTS       30u             // pages 1..30 of a sector
#define CLASSPAD_FUGUE_KIND_DATA        0x11u
#define CLASSPAD_FUGUE_KIND_FINFO       0x22u
#define CLASSPAD_FUGUE_BLOCK_BYTES      512u            // the block device's sector
#define CLASSPAD_FUGUE_BLOCKS_PER_PAGE  8u

// Most logical clusters a volume of n physical sectors can hold records for:
// the size of the loc[] and ver[] arrays ClasspadFugueScan wants.
#define CLASSPAD_FUGUE_MAX_CLUSTERS(n)  ((n) * CLASSPAD_FUGUE_DATA_SLOTS)

typedef struct {
    const unsigned char *mem;           // the volume's first byte
    unsigned sectors;                   // physical sectors scanned
    unsigned short *loc;                // per logical cluster: 1 + sector * 32 + page, 0 = no record
    unsigned clusters;                  // entries of loc[]
    unsigned live;                      // logical clusters that have a record
    unsigned records;                   // valid DATA records seen (live + superseded)
    unsigned rejected;                  // non-erased DATA slots not taken (bad ECC, foreign bytes, cluster out of range)
    unsigned blocks;                    // size of the block device in 512-byte sectors, 0 = no MBR
} CLASSPAD_FUGUE;

// The 3 ECC bytes of a metadata record (over its first 37 bytes) into e[].
// Casio's tables (OS 02.01.7003 at 0x808BFB00) are a column parity code per
// byte and a line parity over the offsets of the odd-parity bytes; computed
// here bit by bit, the test compares it with every record of a real volume.
static void ClasspadFugueEcc(const unsigned char *m, unsigned char *e)
{
    static const unsigned char col[8] = { 0x15, 0x16, 0x19, 0x1A, 0x25, 0x26, 0x29, 0x2A };
    unsigned r1 = 0xFF, r4 = 0xFF, r7 = 0xFF, i, j, b, odd, s1, s7, k;

    for (i = 0; i < 37; i++) {
        b = m[i];
        odd = 0;
        for (j = 0; j < 8; j++) {
            if (b & (1u << j)) { r4 ^= col[j]; odd ^= 1; }
        }
        if (odd) { r1 ^= i; r7 ^= ~i & 0xFF; }
    }
    e[0] = (unsigned char)(r4 | 0xC0);
    for (k = 0; k < 2; k++) {           // a nibble of r7 on the even bits, of r1 on the odd bits
        s7 = 0; s1 = 0;
        for (j = 0; j < 4; j++) {
            if (r7 & (1u << (4 * k + j))) s7 |= 1u << (2 * j);
            if (r1 & (1u << (4 * k + j))) s1 |= 2u << (2 * j);
        }
        e[1 + k] = (unsigned char)((0xAA | s7) & (0x55 | s1));
    }
}

// 1 when the 64-byte record m is a complete record of the given kind.
static int ClasspadFugueRecordValid(const unsigned char *m, unsigned kind)
{
    unsigned char e[3];

    if (m[0] != kind || m[1] != 0x0F || m[34] != 'Z' || m[35] != 'Z' || m[36] != 'Z') return 0;
    ClasspadFugueEcc(m, e);
    return m[37] == e[0] && m[38] == e[1] && m[39] == e[2];
}

// The 4 KiB of logical cluster lc, or 0 when the volume has no record of it.
static const unsigned char *ClasspadFugueCluster(const CLASSPAD_FUGUE *f, unsigned lc)
{
    unsigned l;

    if (lc >= f->clusters || !(l = f->loc[lc])) return 0;
    l--;
    return f->mem + (l >> 5) * CLASSPAD_FUGUE_SECTOR_BYTES + (l & 31) * CLASSPAD_FUGUE_PAGE_BYTES;
}

// Copies count 512-byte sectors of the block device from LBA lba to out.
// Clusters without a record, and anything past them, read as 0xFF.
static void ClasspadFugueRead(const CLASSPAD_FUGUE *f, unsigned lba, unsigned count, unsigned char *out)
{
    const unsigned char *p;
    unsigned i;

    for (; count; count--, lba++, out += CLASSPAD_FUGUE_BLOCK_BYTES) {
        p = ClasspadFugueCluster(f, lba / CLASSPAD_FUGUE_BLOCKS_PER_PAGE);
        if (p) p += (lba % CLASSPAD_FUGUE_BLOCKS_PER_PAGE) * CLASSPAD_FUGUE_BLOCK_BYTES;
        for (i = 0; i < CLASSPAD_FUGUE_BLOCK_BYTES; i++) out[i] = p ? p[i] : 0xFF;
    }
}

// Builds the cluster map of the volume at mem (sectors physical sectors).
// loc[] and ver[] have clusters entries each; ver[] is only needed during the
// call. Returns the size of the block device in 512-byte sectors: the end of
// the MBR's first partition, rounded up to a whole cluster; 0 when LBA 0 is
// not an MBR with a partition (an erased or foreign area: nothing to mount).
static unsigned ClasspadFugueScan(CLASSPAD_FUGUE *f, const void *mem, unsigned sectors,
                                  unsigned short *loc, unsigned *ver, unsigned clusters)
{
    const unsigned char *m, *p;
    unsigned s, slot, i, lc, v, start, len, end;

    f->mem = (const unsigned char *)mem;
    f->sectors = sectors;
    f->loc = loc;
    f->clusters = clusters;
    f->live = f->records = f->rejected = f->blocks = 0;
    for (i = 0; i < clusters; i++) loc[i] = 0;

    for (s = 0; s < sectors; s++) {
        for (slot = 1; slot <= CLASSPAD_FUGUE_DATA_SLOTS; slot++) {
            m = f->mem + s * CLASSPAD_FUGUE_SECTOR_BYTES + CLASSPAD_FUGUE_META_OFF + slot * CLASSPAD_FUGUE_META_BYTES;
            if (m[0] != CLASSPAD_FUGUE_KIND_DATA) {
                // erased (0xFF) or another kind of record (0x88, the mount anchor): not data
                if (m[0] == 0xFF || m[0] == 0x88) continue;
                f->rejected++;
                continue;
            }
            lc = (unsigned)m[6] << 8 | m[7];
            if (!ClasspadFugueRecordValid(m, CLASSPAD_FUGUE_KIND_DATA) || lc >= clusters) { f->rejected++; continue; }
            v = (unsigned)m[2] << 24 | (unsigned)m[3] << 16 | (unsigned)m[4] << 8 | m[5];
            f->records++;
            if (!loc[lc]) f->live++;
            else if (v >= ver[lc]) continue;            // an older copy (or the same number twice: the first found stays)
            loc[lc] = (unsigned short)(1 + s * 32 + slot);
            ver[lc] = v;
        }
    }

    p = ClasspadFugueCluster(f, 0);                     // LBA 0: the MBR
    if (!p || p[510] != 0x55 || p[511] != 0xAA || p[446 + 4] == 0) return 0;
    start = p[454] | (unsigned)p[455] << 8 | (unsigned)p[456] << 16 | (unsigned)p[457] << 24;
    len   = p[458] | (unsigned)p[459] << 8 | (unsigned)p[460] << 16 | (unsigned)p[461] << 24;
    end = start + len;
    if (!start || !len || end < start || end > clusters * CLASSPAD_FUGUE_BLOCKS_PER_PAGE) return 0;
    f->blocks = (end + CLASSPAD_FUGUE_BLOCKS_PER_PAGE - 1) / CLASSPAD_FUGUE_BLOCKS_PER_PAGE * CLASSPAD_FUGUE_BLOCKS_PER_PAGE;
    return f->blocks;
}

#endif
