/* fugue.c - writing Casio's storage memory from a translated guest: the
 * writer of classpad_fuguewr.h on the translator's NOR programming service
 * (rt/nor.c), against the emulator's NOR command decoder. `make GUEST=fugue
 * run` puts a volume from tools/mkfugue.py at the storage memory's place
 * (chip 0x01A20000) and turns the decoder on (HLE_FLASH_CMD=1).
 *
 * What the CE driver does is done here in small: mount, rewrite clusters,
 * fill a sector so that a second one is opened, reclaim a sector (the erase),
 * then scan the flash from scratch and compare. */
#include "hyper.h"
#include "../../bsp/classpad50/SRC/INC/classpad_fuguewr.h"
#include "../../bsp/classpad50/SRC/INC/classpad_fuguetrim.h"

#define HC_NOR_TAG      0x4E4F5200u
#define HC_NOR_MAGIC    0x4E4F5231u
#define CHIP_OFF        (CLASSPAD_FUGUE_PA - 0x80000000u)
#define CLUSTERS        CLASSPAD_FUGUE_MAX_CLUSTERS(CLASSPAD_FUGUE_SECTORS)

static unsigned hc_nor(unsigned op, unsigned a, unsigned b, unsigned c)
{
    register unsigned r4 __asm__("r4") = HC_NOR_TAG | op; register unsigned r5 __asm__("r5") = a;
    register unsigned r6 __asm__("r6") = b; register unsigned r7 __asm__("r7") = c; register unsigned r0 __asm__("r0");
    __asm__ volatile ("trapa #0xFF" : "=r"(r0) : "r"(r4), "r"(r5), "r"(r6), "r"(r7) : "memory");
    return r0;
}

static int fails;
static void check(const char *name, int ok)
{
    hc_puts(ok ? "PASS " : "FAIL "); hc_puts(name); hc_putc('\n');
    if (!ok) fails++;
}

static CLASSPAD_FUGUE f;
static CLASSPAD_FUGUE_WR w;
static unsigned short loc[CLUSTERS];
static unsigned ver[CLUSTERS];
static unsigned char page[CLASSPAD_FUGUE_PAGE_BYTES], data[CLASSPAD_FUGUE_PAGE_BYTES];
static unsigned programs, erases;

static int nor_program(void *ctx, unsigned off, const unsigned char *src, unsigned len)
{
    (void)ctx; programs++;
    return hc_nor(1, CHIP_OFF + off, (unsigned)src, len) != 0;
}

static int nor_erase(void *ctx, unsigned sector)
{
    (void)ctx; erases++;
    return hc_nor(2, CHIP_OFF + sector * CLASSPAD_FUGUE_SECTOR_BYTES, 0, 0) != 0;
}

static void pattern(unsigned lc, unsigned gen)
{
    unsigned i, x = lc * 2654435761u + gen * 40503u + 1;
    for (i = 0; i < CLASSPAD_FUGUE_PAGE_BYTES; i++) { x = x * 1664525u + 1013904223u; data[i] = (unsigned char)(x >> 24); }
}

static int holds(unsigned lc, unsigned gen)
{
    const unsigned char *p = ClasspadFugueCluster(&f, lc);
    unsigned i;
    if (!p) return 0;
    pattern(lc, gen);
    for (i = 0; i < CLASSPAD_FUGUE_PAGE_BYTES; i++) if (p[i] != data[i]) return 0;
    return 1;
}

/* the FAT walk of the trim, on the mounted volume */
static CLASSPAD_FUGUE_TRIM trim;
static unsigned unusedCount, unusedLow, unused100;
static int trim_read(void *ctx, unsigned lba, unsigned char *buf) { (void)ctx; ClasspadFugueRead(&f, lba, 1, buf); return 0; }
static void trim_unused(void *ctx, unsigned lc)
{
    (void)ctx; unusedCount++;
    if (lc < 8) unusedLow++;                    /* the MBR, the boot sector, the FATs: never */
    if (lc >= 100 && lc < 110) unused100++;
}

void guest_main(void)
{
    unsigned i, lc, ok, head0, erased0, blocks, victim;
    const unsigned char *p;

    check("the translator has the NOR service", hc_nor(0, 0, 0, 0) == HC_NOR_MAGIC);
    check("a program outside the storage memory is refused", hc_nor(1, 0x00EE0000u, (unsigned)data, 64) == 1);
    check("an erase outside the storage memory is refused", hc_nor(2, 0x00EE0000u, 0, 0) == 1);
    check("a source that is not RAM is refused", hc_nor(1, CHIP_OFF + 0x1000000u, CLASSPAD_FUGUE_PA, 64) == 1);

    blocks = ClasspadFugueScan(&f, (const void *)CLASSPAD_FUGUE_PA, CLASSPAD_FUGUE_SECTORS, loc, ver, CLUSTERS);
    check("the volume is there", blocks == 49536);
    check("and can be written", ClasspadFugueWrInit(&w, &f, page, 0, nor_program, nor_erase));
    head0 = w.head; erased0 = w.erased;

    for (ok = 1, lc = 100; lc < 110; lc++) {            /* new clusters */
        pattern(lc, 1);
        ok &= ClasspadFugueWrite(&w, lc, data) == CLASSPAD_FUGUE_OK;
    }
    check("ten clusters written", ok);
    for (ok = 1, lc = 100; lc < 110; lc++) ok &= holds(lc, 1);
    check("and read back through the cached alias", ok);

    for (ok = 1, i = 2; i < 40; i++) {                  /* one cluster rewritten until the head sector is full */
        pattern(105, i);
        ok &= ClasspadFugueWrite(&w, 105, data) == CLASSPAD_FUGUE_OK;
    }
    check("38 rewrites of one cluster", ok && holds(105, 39));
    check("further sectors were opened", w.head != head0 && w.erased < erased0);

    /* a full sector of superseded records is behind the head now: reclaim it */
    erased0 = w.erased;
    check("reclaim", ClasspadFugueReclaim(&w) == CLASSPAD_FUGUE_OK && erases == 1 && w.erased == erased0 + 1);
    for (victim = 0; victim < CLASSPAD_FUGUE_SECTORS && ClasspadFugueEraseCount(&w, victim) == 0; victim++) ;
    check("the reclaimed sector is blank", victim < CLASSPAD_FUGUE_SECTORS &&
          ClasspadFugueBlank((const unsigned char *)CLASSPAD_FUGUE_PA + victim * CLASSPAD_FUGUE_SECTOR_BYTES, CLASSPAD_FUGUE_SECTOR_BYTES));

    /* a boot: everything from the flash alone */
    blocks = ClasspadFugueScan(&f, (const void *)CLASSPAD_FUGUE_PA, CLASSPAD_FUGUE_SECTORS, loc, ver, CLUSTERS);
    check("the volume mounts again", blocks == 49536 && f.rejected == 0 && ClasspadFugueWrInit(&w, &f, page, 0, nor_program, nor_erase));
    for (ok = 1, lc = 100; lc < 110; lc++) ok &= holds(lc, lc == 105 ? 39 : 1);
    check("every write is there after the reclaim", ok);
    check("the erase is counted", ClasspadFugueEraseCount(&w, victim) == 1 && w.used[victim] == 0);
    p = ClasspadFugueCluster(&f, 0);
    check("the MBR survived", p && p[510] == 0x55 && p[511] == 0xAA && p[454] == 29);
    p = ClasspadFugueCluster(&f, 0x1837);
    check("the tail cluster survived", p && p[4092] == 0xCC && p[4095] == 0xCC);

    /* trim: the test volume's FAT has clusters 100..109 as free (nothing of
     * the file system put them there), so the walk names them; a trimmed
     * cluster reads as nothing, one written after its trim reads as written
     * after the next mount although its older record is still in the flash */
    ClasspadFugueTrimInit(&w, ver);
    trim.ctx = 0; trim.read = trim_read; trim.unused = trim_unused;
    ClasspadFugueTrimScan(&trim, f.blocks);
    check("the FAT walk names unused clusters, none of the file system's own", unusedCount > 1000 && unusedLow == 0 && unused100 == 10);
    check("trim drops two clusters", ClasspadFugueTrim(&w, 105) && ClasspadFugueTrim(&w, 106) &&
          !ClasspadFugueCluster(&f, 105) && !ClasspadFugueCluster(&f, 106));
    pattern(105, 50);
    check("a trimmed cluster is written again", ClasspadFugueWrite(&w, 105, data) == CLASSPAD_FUGUE_OK && holds(105, 50));
    blocks = ClasspadFugueScan(&f, (const void *)CLASSPAD_FUGUE_PA, CLASSPAD_FUGUE_SECTORS, loc, ver, CLUSTERS);
    check("and is what the next mount finds", blocks == 49536 && holds(105, 50));
    check("the other one is gone or back as it was", !ClasspadFugueCluster(&f, 106) || holds(106, 1));

    hc_puts("programs "); hc_puthex(programs); hc_puts(" erases "); hc_puthex(erases); hc_putc('\n');
    hc_puts(fails ? "fugue: FAILED\n" : "fugue: ok\n");
    hc_exit(fails);
}
