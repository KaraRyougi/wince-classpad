/* flush.c - code-cache pressure: more blocks than the translator keeps
 * (MAX_BLOCKS 2048, 2 MiB), so full flushes happen in the middle of
 * resolving direct links and inline caches. The result of every pass must
 * be the same: a link literal or an IC refill written after the flush
 * would land in the freshly reused cache. */
#include "hyper.h"

#define NFN   3000                 /* rts; nop each */
#define NCHAIN 3000                /* bra +4; nop each, then rts */

static unsigned short fns[NFN * 3];          /* mov r4,r0 ; rts ; add #1,r0 */
static unsigned short chain[NCHAIN * 2 + 3];  /* mov r4,r0 ; (bra +4 ; add #1,r0)* ; rts ; nop */
static int fails;
static void check(const char *name, int ok)
{
    hc_puts(ok ? "PASS " : "FAIL "); hc_puts(name); hc_putc('\n');
    if (!ok) fails++;
}

typedef unsigned (*fn_t)(unsigned);

void guest_main(void)
{
    unsigned i, pass, sum, want, f0 = hc_getstat(7), fl;

    hc_puts("flush test: translations beyond the cache\n");
    for (i = 0; i < NFN; i++) { fns[3 * i] = 0x6043; fns[3 * i + 1] = 0x000B; fns[3 * i + 2] = 0x7001; }
    chain[0] = 0x6043;
    for (i = 0; i < NCHAIN; i++) { chain[1 + 2 * i] = 0xA000; chain[2 + 2 * i] = 0x7001; }   /* bra pc+4 ; add #1,r0 */
    chain[1 + 2 * NCHAIN] = 0x000B; chain[2 + 2 * NCHAIN] = 0x0009;
    hc_flush((unsigned)fns, sizeof fns); hc_flush((unsigned)chain, sizeof chain);

    want = 0;
    for (pass = 0; pass < 3; pass++) {
        sum = 0;
        for (i = 0; i < NFN; i++) sum = ((fn_t)&fns[3 * i])(sum);           /* indirect: inline caches */
        sum = ((fn_t)&chain[0])(sum);                                         /* direct chain: link literals */
        if (pass == 0) want = sum;
        fl = hc_getstat(7) - f0;
        hc_puts("pass "); hc_puthex(pass); hc_puts(" sum "); hc_puthex(sum); hc_puts(" flushes "); hc_puthex(fl); hc_putc('\n');
        check("pass result", sum == want && sum == NFN + NCHAIN);
    }
    check("the cache was flushed at least twice", hc_getstat(7) - f0 >= 2);
    hc_stats();
    hc_puts(fails ? "RESULT: FAIL\n" : "RESULT: PASS\n");
    hc_exit(fails);
}
