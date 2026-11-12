/* fugue_churn.c - a chip image whose storage memory has been through reclaim
 * and erase, for trying on Casio's OS.
 *
 *   fugue_churn IN.bin OUT.bin [rewrites]      (64 MiB chip images; default 400)
 *
 * The writer of classpad_fuguewr.h (the code the CE driver runs) works on
 * the storage memory of IN.bin in an array with NOR semantics. The churn
 * keeps the file system as it is: the root directory's first file entry gets
 * a new modification time `rewrites` times, each a rewrite of the directory's
 * cluster, so the head sector fills with superseded records and new sectors
 * are opened. Then the volume is optimised (ClasspadFugueOptimize): every
 * sector with dead records is reclaimed, Casio's own sectors included, which
 * moves its live records (the MBR, the FATs, the DETAIL record, the tail
 * cluster) and erases the sectors behind them.
 *
 * Checked before OUT.bin is written: the volume scans from scratch, every
 * logical cluster reads as before except the directory's, the erase counters
 * of the live DETAIL record add up to the erases done, a program never had
 * to set a bit. The rest of the chip is copied unchanged. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../classpad50/SRC/INC/classpad_fuguewr.h"

#define SECTORS     CLASSPAD_FUGUE_SECTORS
#define CLUSTERS    CLASSPAD_FUGUE_MAX_CLUSTERS(SECTORS)
#define CHIP_BYTES  0x04000000u
#define OFF         (CLASSPAD_FUGUE_PA - 0x80000000u)
#define VOL_BYTES   (SECTORS * CLASSPAD_FUGUE_SECTOR_BYTES)

static unsigned char *vol;
static unsigned short loc[CLUSTERS];
static unsigned ver[CLUSTERS];
static unsigned char page[CLASSPAD_FUGUE_PAGE_BYTES];
static CLASSPAD_FUGUE f;
static CLASSPAD_FUGUE_WR w;
static unsigned erases, bad;

static int program(void *ctx, unsigned off, const unsigned char *src, unsigned len)
{
    unsigned i;
    (void)ctx;
    if ((off | len) & 1 || off + len > VOL_BYTES) { bad++; return -1; }
    for (i = 0; i < len; i++) {
        if ((vol[off + i] & src[i]) != src[i]) bad++;
        vol[off + i] &= src[i];
    }
    return 0;
}

static int erase(void *ctx, unsigned sector)
{
    (void)ctx;
    memset(vol + sector * CLASSPAD_FUGUE_SECTOR_BYTES, 0xFF, CLASSPAD_FUGUE_SECTOR_BYTES);
    erases++;
    return 0;
}

static unsigned le16(const unsigned char *p) { return p[0] | (unsigned)p[1] << 8; }

static void states(const char *when)
{
    unsigned s, full = 0, part = 0, blank = 0, counts = 0;
    for (s = 0; s < SECTORS; s++) {
        if (!w.used[s]) blank++; else if (w.used[s] >= CLASSPAD_FUGUE_RECORDS) full++; else part++;
        counts += ClasspadFugueEraseCount(&w, s);
    }
    printf("%-8s %3u full, %u with room, %3u erased sectors; %u live clusters; erase counters add up to %u; head %u\n",
           when, full, part, blank, f.live, counts, w.head);
}

int main(int argc, char **argv)
{
    static unsigned char dir[CLASSPAD_FUGUE_PAGE_BYTES], b[512];
    unsigned char *chip, *before;
    unsigned rewrites = argc > 3 ? (unsigned)atoi(argv[3]) : 400, start, rootLba, lc, i, e, n, lcs, differ = 0, counts0 = 0, counts = 0, s;
    const unsigned char *p;
    int status;
    FILE *fp;

    if (argc < 3) { fprintf(stderr, "usage: fugue_churn IN.bin OUT.bin [rewrites]\n"); return 2; }
    chip = malloc(CHIP_BYTES);
    fp = fopen(argv[1], "rb");
    if (!fp || fread(chip, 1, CHIP_BYTES, fp) != CHIP_BYTES) { fprintf(stderr, "%s is not a 64 MiB chip image\n", argv[1]); return 1; }
    fclose(fp);
    vol = chip + OFF;

    if (!ClasspadFugueScan(&f, vol, SECTORS, loc, ver, CLUSTERS) || !ClasspadFugueWrInit(&w, &f, page, 0, program, erase)) {
        fprintf(stderr, "no writable volume in %s\n", argv[1]); return 1;
    }
    states("before:");
    for (s = 0; s < SECTORS; s++) counts0 += ClasspadFugueEraseCount(&w, s);
    lcs = f.blocks / CLASSPAD_FUGUE_BLOCKS_PER_PAGE;
    before = malloc((size_t)CLUSTERS * CLASSPAD_FUGUE_PAGE_BYTES);
    for (lc = 0; lc < CLUSTERS; lc++) {
        p = ClasspadFugueCluster(&f, lc);
        if (p) memcpy(before + lc * CLASSPAD_FUGUE_PAGE_BYTES, p, CLASSPAD_FUGUE_PAGE_BYTES);
        else memset(before + lc * CLASSPAD_FUGUE_PAGE_BYTES, 0xFF, CLASSPAD_FUGUE_PAGE_BYTES);
    }

    /* the root directory and its first file entry */
    ClasspadFugueRead(&f, 0, 1, b);
    start = b[454] | (unsigned)b[455] << 8 | (unsigned)b[456] << 16 | (unsigned)b[457] << 24;
    ClasspadFugueRead(&f, start, 1, b);
    rootLba = start + le16(b + 14) + b[16] * le16(b + 22);
    lc = rootLba / CLASSPAD_FUGUE_BLOCKS_PER_PAGE;
    memcpy(dir, before + lc * CLASSPAD_FUGUE_PAGE_BYTES, sizeof dir);
    for (e = (rootLba % 8) * 512; e < sizeof dir; e += 32) {
        if (dir[e] && dir[e] != 0xE5 && dir[e + 11] != 0x0F && !(dir[e + 11] & 0x08) && dir[e] != ' ') break;
    }
    if (e >= sizeof dir || lc >= lcs) { fprintf(stderr, "no file entry in the root directory's first cluster\n"); return 1; }
    printf("churn: the modification time of \"%.11s\" (root directory, logical cluster %u), %u times\n", dir + e, lc, rewrites);

    for (i = 0; i < rewrites; i++) {
        n = le16(dir + e + 22) + 1;                     /* two seconds later */
        dir[e + 22] = (unsigned char)n; dir[e + 23] = (unsigned char)(n >> 8);
        status = ClasspadFugueWrite(&w, lc, dir);
        if (status != CLASSPAD_FUGUE_OK) { fprintf(stderr, "write %u failed (%d)\n", i, status); return 1; }
    }
    states("churned:");
    n = ClasspadFugueOptimize(&w, &status);
    if (status != CLASSPAD_FUGUE_OK) { fprintf(stderr, "optimise failed (%d)\n", status); return 1; }
    printf("optimise: %u sectors reclaimed, %u erases\n", n, erases);

    /* a boot: everything from the flash alone */
    if (!ClasspadFugueScan(&f, vol, SECTORS, loc, ver, CLUSTERS) || f.rejected || !ClasspadFugueWrInit(&w, &f, page, 0, program, erase)) {
        fprintf(stderr, "the volume does not mount again\n"); return 1;
    }
    states("after:");
    memcpy(before + lc * CLASSPAD_FUGUE_PAGE_BYTES, dir, sizeof dir);
    for (i = 0; i < CLUSTERS; i++) {
        p = ClasspadFugueCluster(&f, i);
        if (p ? memcmp(p, before + i * CLASSPAD_FUGUE_PAGE_BYTES, CLASSPAD_FUGUE_PAGE_BYTES) != 0
              : !ClasspadFugueBlank(before + i * CLASSPAD_FUGUE_PAGE_BYTES, CLASSPAD_FUGUE_PAGE_BYTES)) differ++;
    }
    for (s = 0; s < SECTORS; s++) counts += ClasspadFugueEraseCount(&w, s);
    if (differ || bad || counts != counts0 + erases) {
        fprintf(stderr, "check failed: %u clusters differ, %u bad programs, counters %u for %u erases\n", differ, bad, counts - counts0, erases);
        return 1;
    }
    printf("checked: every logical cluster as before (the directory with its new time), counters match the erases, no bit set by a program\n");

    fp = fopen(argv[2], "wb");
    if (!fp || fwrite(chip, 1, CHIP_BYTES, fp) != CHIP_BYTES) { fprintf(stderr, "cannot write %s\n", argv[2]); return 1; }
    fclose(fp);
    printf("wrote %s\n", argv[2]);
    return 0;
}
