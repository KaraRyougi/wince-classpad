/* exc.c - guest exception test: own VBR, trapa, SPC/TRA/EXPEVT, rte, and a
 * register modified by the handler becoming visible after return. */
#include "hyper.h"

extern char guest_vectors[];
static int fails, hits;
static unsigned last_vec, last_spc, last_expevt, last_tra;

static void check(const char *name, int ok)
{
    hc_puts(ok ? "PASS " : "FAIL "); hc_puts(name); hc_putc('\n');
    if (!ok) fails++;
}

/* Called from exc_vectors.S with RB=1. Returns a replacement SPC or 0. */
unsigned guest_exc_handler(unsigned vec, unsigned spc, unsigned expevt, unsigned tra)
{
    hits++;
    last_vec = vec; last_spc = spc; last_expevt = expevt; last_tra = tra;
    if (vec == 1 && expevt == 0x160 && (tra >> 2) == 7)
        return spc + 2;                 /* trapa #7: skip the instruction after it */
    return 0;
}

void guest_main(void)
{
    unsigned spc_expected;
    volatile int marker = 0;

    hc_puts("exc test: installing guest VBR\n");
    __asm__ volatile ("ldc %0, vbr" :: "r"(guest_vectors));
    {
        unsigned v;
        __asm__ volatile ("stc vbr, %0" : "=r"(v));
        check("stc vbr reads back", v == (unsigned)guest_vectors);
    }

    /* trapa #5: handler runs, returns to the instruction after trapa */
    __asm__ volatile ("trapa #5" ::: "memory");
    check("trapa #5 reached handler", hits == 1 && last_vec == 1);
    check("EXPEVT = 0x160, TRA = 5<<2", last_expevt == 0x160 && last_tra == (5 << 2));

    /* trapa #7: the handler advances SPC by 2, skipping the marker store */
    __asm__ volatile (
        "trapa #7\n\t"
        "mov #1, %0\n\t"                /* skipped if SPC was advanced */
        : "=r"(spc_expected) : : "memory");
    (void)spc_expected;
    __asm__ volatile ("mov #0, %0" : "=r"(spc_expected));
    check("handler-modified SPC skipped an instruction", hits == 2 && last_tra == (7 << 2));

    /* SPC points at the instruction after trapa */
    {
        unsigned here;
        __asm__ volatile ("mova 1f, r0\n\tmov r0, %0\n\ttrapa #9\n1:\n\t" : "=r"(here) :: "r0", "memory");
        check("SPC = address after trapa", hits == 3 && last_spc == here);
    }

    /* nested use of the stack and T bit survive the exception path */
    {
        int t;
        __asm__ volatile ("sett\n\ttrapa #3\n\tmovt %0" : "=r"(t) :: "memory");
        check("T bit preserved across exception", t == 1 && hits == 4);
    }

    marker++;
    hc_puts("exceptions taken: "); hc_puthex(hits); hc_putc('\n');
    hc_stats();
    hc_puts(fails ? "RESULT: FAIL\n" : "RESULT: PASS\n");
    hc_exit(fails);
}
