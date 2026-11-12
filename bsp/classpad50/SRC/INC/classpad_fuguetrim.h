//------------------------------------------------------------------------------
//  classpad_fuguetrim.h - which logical clusters of Casio's storage memory the
//  FAT file system on it no longer uses
//
//  Fugue (classpad_fugue.h) keeps the newest record of every 4 KiB logical
//  cluster that was ever written and knows nothing of files: a deleted file's
//  clusters stay "live" for ever, are copied along by every reclaim, and a
//  volume that has been used for a while has almost no slack left, so every
//  write waits for a sector to be reclaimed. The FAT knows better. This walks
//  it and names the logical clusters that lie entirely in data clusters the
//  FAT has as free, so that the writer can drop their records
//  (classpad_fuguewr.h ClasspadFugueTrim): what an SSD calls TRIM.
//
//  The volume, as Casio's OS formats it: LBA 0 is an MBR, its first partition
//  a FAT16 file system (512-byte sectors). A logical cluster is 8 LBAs and
//  need not line up with the FAT's clusters, so it is free only when every
//  FAT cluster it touches is free, in every copy of the FAT. Anything that is
//  not exactly this layout (FAT12, FAT32, other sector sizes, a FAT that runs
//  past the device) names nothing.
//
//  Byte accesses only, like the reader. The caller hands out sectors (its
//  cache first, then the flash) and decides what to do with each cluster:
//  the CE driver leaves out the ones written since it started (their FAT
//  entry may not have reached the disk yet).
//------------------------------------------------------------------------------
#ifndef CLASSPAD_FUGUETRIM_H
#define CLASSPAD_FUGUETRIM_H

#define CLASSPAD_FUGUE_TRIM_SECTOR      512u
#define CLASSPAD_FUGUE_TRIM_LBAS        8u              // per logical cluster

typedef struct {
    void *ctx;
    // one 512-byte sector of the block device into buf; 0 = read
    int (*read)(void *ctx, unsigned lba, unsigned char *buf);
    // a logical cluster the file system does not use
    void (*unused)(void *ctx, unsigned lc);
    unsigned char sec[2][CLASSPAD_FUGUE_TRIM_SECTOR];   // one sector of each FAT copy
    unsigned have[2];                                   // which (LBA + 1, 0 = none)
} CLASSPAD_FUGUE_TRIM;

static unsigned ClasspadFugueTrimLe16(const unsigned char *p) { return p[0] | (unsigned)p[1] << 8; }
static unsigned ClasspadFugueTrimLe32(const unsigned char *p)
{
    return p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24;
}

// FAT entry c of copy k (at LBA fat): 0 = free, 1 = in use or unreadable
static int ClasspadFugueTrimUsed(CLASSPAD_FUGUE_TRIM *t, unsigned k, unsigned fat, unsigned c)
{
    unsigned lba = fat + c / 256;

    if (t->have[k] != lba + 1) {
        t->have[k] = 0;
        if (t->read(t->ctx, lba, t->sec[k])) return 1;
        t->have[k] = lba + 1;
    }
    return ClasspadFugueTrimLe16(t->sec[k] + (c % 256) * 2) != 0;
}

// Calls t->unused for every logical cluster of a device of blocks sectors
// that the FAT16 volume on it has as free. Returns how many.
static unsigned ClasspadFugueTrimScan(CLASSPAD_FUGUE_TRIM *t, unsigned blocks)
{
    unsigned char *b = t->sec[0];
    unsigned start, spc, reserved, nfats, rootEntries, total, fatSectors, data, clusters;
    unsigned lc, first, last, c, k, shift, n = 0;

    t->have[0] = t->have[1] = 0;
    if (t->read(t->ctx, 0, b) || b[510] != 0x55 || b[511] != 0xAA || b[446 + 4] == 0) return 0;
    start = ClasspadFugueTrimLe32(b + 454);
    if (!start || start >= blocks || t->read(t->ctx, start, b) || b[510] != 0x55 || b[511] != 0xAA) return 0;
    if (ClasspadFugueTrimLe16(b + 11) != CLASSPAD_FUGUE_TRIM_SECTOR) return 0;
    spc = b[13];
    reserved = ClasspadFugueTrimLe16(b + 14);
    nfats = b[16];
    rootEntries = ClasspadFugueTrimLe16(b + 17);
    total = ClasspadFugueTrimLe16(b + 19);
    if (!total) total = ClasspadFugueTrimLe32(b + 32);
    fatSectors = ClasspadFugueTrimLe16(b + 22);
    if (!spc || (spc & (spc - 1)) || !reserved || nfats < 1 || nfats > 2 || !fatSectors || !rootEntries) return 0;
    if (total > blocks - start) return 0;
    data = reserved + nfats * fatSectors + (rootEntries * 32 + CLASSPAD_FUGUE_TRIM_SECTOR - 1) / CLASSPAD_FUGUE_TRIM_SECTOR;
    if (data >= total) return 0;
    for (shift = 0; (1u << shift) < spc; shift++) ;     // a power of two: no division (a guest has none)
    clusters = (total - data) >> shift;
    if (clusters < 4085 || clusters >= 65525 || fatSectors * 256 < clusters + 2) return 0;      // FAT16 and nothing else
    data += start;                                      // LBA of FAT cluster 2

    for (lc = (data + CLASSPAD_FUGUE_TRIM_LBAS - 1) / CLASSPAD_FUGUE_TRIM_LBAS; ; lc++) {
        first = (lc * CLASSPAD_FUGUE_TRIM_LBAS - data) >> shift;                        // FAT clusters first + 2 .. last + 2
        last = (lc * CLASSPAD_FUGUE_TRIM_LBAS + CLASSPAD_FUGUE_TRIM_LBAS - 1 - data) >> shift;
        if (last >= clusters) break;
        for (c = first; c <= last; c++) {
            for (k = 0; k < nfats; k++) {
                if (ClasspadFugueTrimUsed(t, k, start + reserved + k * fatSectors, c + 2)) break;
            }
            if (k < nfats) break;
        }
        if (c <= last) continue;
        t->unused(t->ctx, lc);
        n++;
    }
    return n;
}

#endif
