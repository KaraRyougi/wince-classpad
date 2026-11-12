/* fuguebench.c - what an optimise of Casio's storage memory costs under the
 * translator, apart from the chip's own time: the writer of
 * classpad_fuguewr.h on the translator's NOR service, on the volume
 * `make GUEST=fuguebench run` puts in the emulated NOR (tools/mkfugue.py).
 * 300 clusters are written and then written again, which leaves ten sectors
 * of superseded records in front of the head; the optimise reclaims them.
 * The emulator's flash programs and erases in no time, so the run's length
 * is the writer's and the translator's share. The guest prints the counts;
 * the time is the emulator's (`time make ... run`, or its tick at the exit). */
#include "hyper.h"
#include "../../bsp/classpad50/SRC/INC/classpad_fuguewr.h"

#define HC_NOR_TAG      0x4E4F5200u
#define CHIP_OFF        (CLASSPAD_FUGUE_PA - 0x80000000u)
#define CLUSTERS        CLASSPAD_FUGUE_MAX_CLUSTERS(CLASSPAD_FUGUE_SECTORS)

static unsigned hc_nor(unsigned op, unsigned a, unsigned b, unsigned c)
{
    register unsigned r4 __asm__("r4") = HC_NOR_TAG | op; register unsigned r5 __asm__("r5") = a;
    register unsigned r6 __asm__("r6") = b; register unsigned r7 __asm__("r7") = c; register unsigned r0 __asm__("r0");
    __asm__ volatile ("trapa #0xFF" : "=r"(r0) : "r"(r4), "r"(r5), "r"(r6), "r"(r7) : "memory");
    return r0;
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

void guest_main(void)
{
    unsigned pass, lc, i, x = 1, n, bad = 0;
    int status;

    ClasspadFugueScan(&f, (const void *)CLASSPAD_FUGUE_PA, CLASSPAD_FUGUE_SECTORS, loc, ver, CLUSTERS);
    if (!ClasspadFugueWrInit(&w, &f, page, 0, nor_program, nor_erase)) { hc_puts("fuguebench: no writable volume\n"); hc_exit(1); }
    for (pass = 0; pass < 2; pass++)
        for (lc = 100; lc < 400; lc++) {
            for (i = 0; i < CLASSPAD_FUGUE_PAGE_BYTES; i++) { x = x * 1664525u + 1013904223u; data[i] = (unsigned char)(x >> 24); }
            bad += ClasspadFugueWrite(&w, lc, data) != CLASSPAD_FUGUE_OK;
        }
    hc_puts("fuguebench: written, programs "); hc_puthex(programs); hc_putc('\n');
    programs = erases = 0;
    n = ClasspadFugueOptimize(&w, &status);
    hc_puts("fuguebench: optimised, sectors "); hc_puthex(n); hc_puts(" programs "); hc_puthex(programs);
    hc_puts(" erases "); hc_puthex(erases); hc_puts(" status "); hc_puthex((unsigned)status); hc_puts(" bad "); hc_puthex(bad); hc_putc('\n');
    hc_exit((status || bad) ? 1 : 0);
}
