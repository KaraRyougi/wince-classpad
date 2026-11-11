/* test_fugue.c - classpad_fugue.h: the read-only block view of Casio's storage
 * memory. A small volume is built here record by record (newest version wins
 * whatever the physical order, damaged records are skipped, missing clusters
 * read as 0xFF); with a NOR dump of an fx-CP400 the real volume is checked
 * too: every record's stored ECC against ClasspadFugueEcc, the MBR, the FAT16
 * boot sector and the root directory.
 *   test_fugue [nor-dump.bin]   (64 MiB chip image; skipped when absent) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../classpad50/SRC/INC/classpad_fugue.h"

static int fails;
#define CHECK(name, cond) do { if (!(cond)) { printf("FAIL %s\n", name); fails++; } } while (0)

#define T_SECTORS   3u
#define T_CLUSTERS  CLASSPAD_FUGUE_MAX_CLUSTERS(T_SECTORS)

static unsigned char *vol;
static unsigned short loc[T_CLUSTERS];
static unsigned ver[T_CLUSTERS];

static unsigned char *meta(unsigned s, unsigned slot)
{
    return vol + s * CLASSPAD_FUGUE_SECTOR_BYTES + CLASSPAD_FUGUE_META_OFF + slot * CLASSPAD_FUGUE_META_BYTES;
}

/* writes logical cluster lc, version v, filled with byte fill, into page slot of sector s */
static unsigned char *put(unsigned s, unsigned slot, unsigned lc, unsigned v, unsigned char fill)
{
    unsigned char *m = meta(s, slot), *p = vol + s * CLASSPAD_FUGUE_SECTOR_BYTES + slot * CLASSPAD_FUGUE_PAGE_BYTES;
    memset(p, fill, CLASSPAD_FUGUE_PAGE_BYTES);
    memset(m, 0xFF, CLASSPAD_FUGUE_META_BYTES);
    m[0] = CLASSPAD_FUGUE_KIND_DATA; m[1] = 0x0F;
    m[2] = (unsigned char)(v >> 24); m[3] = (unsigned char)(v >> 16); m[4] = (unsigned char)(v >> 8); m[5] = (unsigned char)v;
    m[6] = (unsigned char)(lc >> 8); m[7] = (unsigned char)lc;
    m[34] = m[35] = m[36] = 'Z';
    ClasspadFugueEcc(m, m + 37);
    return p;
}

static void mbr(unsigned char *p, unsigned start, unsigned len)
{
    memset(p, 0, 512);
    p[446 + 4] = 0x0E;
    p[454] = (unsigned char)start; p[455] = (unsigned char)(start >> 8);
    p[458] = (unsigned char)len; p[459] = (unsigned char)(len >> 8);
    p[510] = 0x55; p[511] = 0xAA;
}

static void synthetic(void)
{
    CLASSPAD_FUGUE f;
    unsigned char buf[3 * 512], *p;
    unsigned i, n;

    vol = malloc(T_SECTORS * CLASSPAD_FUGUE_SECTOR_BYTES);
    memset(vol, 0xFF, T_SECTORS * CLASSPAD_FUGUE_SECTOR_BYTES);
    CHECK("an erased area is no volume", ClasspadFugueScan(&f, vol, T_SECTORS, loc, ver, T_CLUSTERS) == 0 && f.live == 0 && f.rejected == 0);

    p = put(0, 2, 0, 0xFFFFFFFE, 0);            /* LC0: MBR, partition 29..100 */
    mbr(p, 29, 72);
    put(0, 3, 5, 0xFFFFFFFE, 0x51);             /* LC5 written three times: the smallest version is live, */
    put(2, 7, 5, 0xFFFFFFFC, 0x53);             /* wherever it lies */
    put(1, 1, 5, 0xFFFFFFFD, 0x52);
    put(1, 30, 6, 0xFFFFFFFE, 0x61);            /* last data page of a sector */
    meta(0, 1)[0] = 0x88;                       /* the mount anchor is not data */
    n = ClasspadFugueScan(&f, vol, T_SECTORS, loc, ver, T_CLUSTERS);
    CHECK("device size is the partition end rounded to a cluster", n == 104 && f.blocks == 104);
    CHECK("record counts", f.live == 3 && f.records == 5 && f.rejected == 0);
    CHECK("newest copy of LC5", ClasspadFugueCluster(&f, 5) == vol + 2 * CLASSPAD_FUGUE_SECTOR_BYTES + 7 * CLASSPAD_FUGUE_PAGE_BYTES);
    CHECK("no record, no cluster", ClasspadFugueCluster(&f, 4) == 0 && ClasspadFugueCluster(&f, T_CLUSTERS) == 0);

    ClasspadFugueRead(&f, 39, 3, buf);          /* LBA 39 = LC4 (missing), 40 and 41 = LC5 */
    for (i = 0, n = 0; i < 512; i++) n += buf[i] == 0xFF && buf[512 + i] == 0x53 && buf[1024 + i] == 0x53;
    CHECK("read: missing cluster is 0xFF, then the live data", n == 512);
    ClasspadFugueRead(&f, 47, 2, buf);          /* last block of LC5, first of LC6 */
    CHECK("read across a cluster boundary", buf[0] == 0x53 && buf[511] == 0x53 && buf[512] == 0x61 && buf[1023] == 0x61);
    ClasspadFugueRead(&f, 0, 1, buf);
    CHECK("LBA 0 is the MBR", buf[510] == 0x55 && buf[511] == 0xAA && buf[454] == 29);

    meta(2, 7)[7] ^= 1;                         /* a flipped bit in the newest record: skipped, the next newest is live */
    ClasspadFugueScan(&f, vol, T_SECTORS, loc, ver, T_CLUSTERS);
    CHECK("bad ECC is skipped", f.rejected == 1 && ClasspadFugueCluster(&f, 5) == vol + 1 * CLASSPAD_FUGUE_SECTOR_BYTES + 1 * CLASSPAD_FUGUE_PAGE_BYTES);
    meta(2, 7)[7] ^= 1;
    meta(2, 7)[1] = 0xFF;                       /* a record whose second byte was never programmed */
    ClasspadFugueScan(&f, vol, T_SECTORS, loc, ver, T_CLUSTERS);
    CHECK("incomplete record is skipped", f.rejected == 1 && f.records == 4);
    meta(2, 7)[1] = 0x0F;

    put(2, 8, T_CLUSTERS, 0xFFFFFFF0, 0x77);    /* a cluster number the volume cannot have */
    ClasspadFugueScan(&f, vol, T_SECTORS, loc, ver, T_CLUSTERS);
    CHECK("cluster out of range is rejected", f.rejected == 1 && f.live == 3);
    memset(meta(2, 8), 0xFF, CLASSPAD_FUGUE_META_BYTES);

    put(2, 9, 5, 0xFFFFFFFC, 0x54);             /* the same version twice: the first found stays */
    ClasspadFugueScan(&f, vol, T_SECTORS, loc, ver, T_CLUSTERS);
    CHECK("equal versions: first found", ClasspadFugueCluster(&f, 5)[0] == 0x53);

    p = put(1, 2, 0, 0xFFFFFFFD, 0);            /* a newer LC0 without the signature */
    CHECK("no MBR signature, no device", ClasspadFugueScan(&f, vol, T_SECTORS, loc, ver, T_CLUSTERS) == 0 && f.live == 3);
    mbr(p, 29, T_CLUSTERS * 8);                 /* a partition past what the volume can hold */
    CHECK("partition beyond the volume, no device", ClasspadFugueScan(&f, vol, T_SECTORS, loc, ver, T_CLUSTERS) == 0);
    free(vol);
}

static unsigned le16(const unsigned char *p) { return p[0] | (unsigned)p[1] << 8; }

static void real(const char *path)
{
    static unsigned short rloc[CLASSPAD_FUGUE_MAX_CLUSTERS(CLASSPAD_FUGUE_SECTORS)];
    static unsigned rver[CLASSPAD_FUGUE_MAX_CLUSTERS(CLASSPAD_FUGUE_SECTORS)];
    CLASSPAD_FUGUE f;
    unsigned char b[512], e[3], *nor, *root;
    const unsigned char *m;
    unsigned off = CLASSPAD_FUGUE_PA - 0x80000000u, size = CLASSPAD_FUGUE_SECTORS * CLASSPAD_FUGUE_SECTOR_BYTES;
    unsigned s, slot, recs = 0, bad = 0, n, start, res, fatsz, rootn, total, i, files = 0;
    FILE *fp = fopen(path, "rb");

    if (!fp) { printf("test_fugue: no dump at %s, real volume skipped\n", path); return; }
    nor = malloc(off + size);
    if (fread(nor, 1, off + size, fp) != off + size) { printf("FAIL %s is not a 64 MiB chip image\n", path); fails++; fclose(fp); free(nor); return; }
    fclose(fp);

    for (s = 0; s < CLASSPAD_FUGUE_SECTORS; s++) {      /* every record Casio's OS wrote carries the ECC we compute */
        for (slot = 0; slot <= CLASSPAD_FUGUE_DATA_SLOTS; slot++) {
            m = nor + off + s * CLASSPAD_FUGUE_SECTOR_BYTES + CLASSPAD_FUGUE_META_OFF + slot * CLASSPAD_FUGUE_META_BYTES;
            if (m[0] == 0xFF) continue;
            recs++;
            ClasspadFugueEcc(m, e);
            if (memcmp(e, m + 37, 3)) bad++;
        }
    }
    CHECK("real volume has records", recs > 0);
    CHECK("ECC of every real record", bad == 0);

    n = ClasspadFugueScan(&f, nor + off, CLASSPAD_FUGUE_SECTORS, rloc, rver, CLASSPAD_FUGUE_MAX_CLUSTERS(CLASSPAD_FUGUE_SECTORS));
    CHECK("real volume: block device found", n != 0);
    CHECK("real volume: nothing rejected", f.rejected == 0);
    ClasspadFugueRead(&f, 0, 1, b);
    start = b[454] | (unsigned)b[455] << 8;
    ClasspadFugueRead(&f, start, 1, b);
    res = le16(b + 14); fatsz = le16(b + 22); rootn = le16(b + 17); total = le16(b + 19);
    CHECK("real volume: FAT16 boot sector", b[510] == 0x55 && b[511] == 0xAA && le16(b + 11) == 512 && b[13] == 8 && !memcmp(b + 54, "FAT16", 5));
    CHECK("real volume: device covers the file system", start + total <= n && n - (start + total) < 8);
    root = malloc(rootn * 32);
    ClasspadFugueRead(&f, start + res + b[16] * fatsz, rootn * 32 / 512, root);
    for (i = 0; i < rootn && root[i * 32]; i++)
        if (root[i * 32] != 0xE5 && root[i * 32 + 11] != 0x0F) files++;
    printf("test_fugue: %s: %u records (%u data, %u live clusters), %u blocks, FAT16 %u sectors at LBA %u, %u root entries\n",
           path, recs, f.records, f.live, n, total, start, files);
    free(root); free(nor);
}

int main(int argc, char **argv)
{
    synthetic();
    if (argc > 1) real(argv[1]);
    if (fails) { printf("test_fugue: %d FAILED\n", fails); return 1; }
    printf("test_fugue: ok\n");
    return 0;
}
