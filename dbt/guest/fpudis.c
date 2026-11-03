/* fpudis.c - SR.FD under the translator: an FPU instruction with the FPU
 * disabled must reach the guest's general exception handler as EXPEVT
 * 0x800 (0x820 in a delay slot) with SPC naming the instruction (the
 * branch for a slot), exactly like the SH-4 the CE kernel's lazy FPU
 * context switch expects; after the handler clears FD the instruction
 * (or the whole branch) is re-executed. */
#include "hyper.h"

extern void guest_vectors(void);
extern unsigned fault_ssr_and;
volatile unsigned fault_pr;                       /* written by the vectors */
extern unsigned fpudis_lds(unsigned), fpudis_bra(unsigned), fpudis_jsr(unsigned), fpudis_bts(unsigned);
extern unsigned fpudis_stsfpscr(void), fpudis_fsts(unsigned);
extern char fpudis_lds_site[], fpudis_bra_site[], fpudis_jsr_site[], fpudis_bts_site[], fpudis_stsfpscr_site[], fpudis_fsts_site[];

static volatile unsigned faults, last_vec, last_expevt, last_spc;
static int fails;
static void check(const char *name, int ok)
{
    hc_puts(ok ? "PASS " : "FAIL "); hc_puts(name); hc_putc('\n');
    if (!ok) fails++;
}

unsigned guest_fault(unsigned vec, unsigned spc, unsigned expevt, unsigned tea)
{
    faults++;
    last_vec = vec; last_expevt = expevt; last_spc = spc;
    if (vec == 1 && (expevt == 0x800 || expevt == 0x820)) {
        fault_ssr_and = ~0x8000u;                /* the handler gives the thread the FPU: clear FD in SSR */
        return 0;                                /* retry at SPC */
    }
    hc_puts("unexpected fault vec "); hc_puthex(vec); hc_puts(" expevt "); hc_puthex(expevt);
    hc_puts(" tea "); hc_puthex(tea); hc_puts(" spc "); hc_puthex(spc); hc_putc('\n');
    hc_exit(2);
    return 0;
}

static unsigned read_sr(void) { unsigned v; __asm__ volatile ("stc sr, %0" : "=r"(v)); return v; }
static void set_fd(int on)
{
    unsigned v = read_sr();
    v = on ? (v | 0x8000u) : (v & ~0x8000u);
    __asm__ volatile ("ldc %0, sr" :: "r"(v) : "memory");
}

void guest_main(void)
{
    unsigned r, f, fpscr;
    hc_puts("fpudis test: SR.FD raises the FPU-disable exception\n");
    __asm__ volatile ("ldc %0, vbr" :: "r"(guest_vectors));
    fault_ssr_and = 0xFFFFFFFFu;

    set_fd(0);
    r = fpudis_lds(5);
    check("FD=0: lds/sts fpul runs without a fault", r == 5 && faults == 0);
    fpscr = fpudis_stsfpscr();

    set_fd(1);
    check("SR keeps FD when written", (read_sr() & 0x8000u) != 0);
    r = fpudis_lds(9);
    check("FD=1: general FPU-disable exception (vector 0x100, EXPEVT 0x800)", faults == 1 && last_vec == 1 && last_expevt == 0x800);
    check("SPC = the FPU instruction", last_spc == (unsigned)fpudis_lds_site);
    check("instruction re-executed after the handler cleared FD", r == 9);
    check("FD clear afterwards", (read_sr() & 0x8000u) == 0);
    r = fpudis_lds(10);
    check("no further exception with FD=0", r == 10 && faults == 1);

    set_fd(1); f = faults;
    r = fpudis_bra(11);
    check("bra delay slot: EXPEVT 0x820", faults == f + 1 && last_expevt == 0x820);
    check("SPC = the bra, not the slot", last_spc == (unsigned)fpudis_bra_site);
    check("branch and slot re-executed", r == 11);

    set_fd(1); f = faults;
    r = fpudis_jsr(13);
    check("jsr delay slot: EXPEVT 0x820, SPC = the jsr", faults == f + 1 && last_expevt == 0x820 && last_spc == (unsigned)fpudis_jsr_site);
    check("jsr re-executed with the right PR", r == 13);

    set_fd(1); f = faults;
    r = fpudis_bts(17);
    check("bt/s delay slot: EXPEVT 0x820, SPC = the bt/s", faults == f + 1 && last_expevt == 0x820 && last_spc == (unsigned)fpudis_bts_site);
    check("conditional branch re-executed", r == 17);

    set_fd(1); f = faults;
    r = fpudis_stsfpscr();
    check("sts fpscr: EXPEVT 0x800 at the sts", faults == f + 1 && last_expevt == 0x800 && last_spc == (unsigned)fpudis_stsfpscr_site);
    check("fpscr read back the same", r == fpscr);

    set_fd(1); f = faults;
    r = fpudis_fsts(21);
    check("four FPU instructions: one exception, at the first", faults == f + 1 && last_expevt == 0x800 && last_spc == (unsigned)fpudis_fsts_site);
    check("value went through fr0 (fsts/flds ran with FD clear)", r == 21);

    check("translator counted the FPU-disable exceptions", hc_getstat(8) == faults);
    hc_stats();
    hc_puts(fails ? "RESULT: FAIL\n" : "RESULT: PASS\n");
    hc_exit(fails);
}
