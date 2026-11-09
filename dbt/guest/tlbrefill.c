/* tlbrefill.c - UTLB misses under Windows CE 6's own TLB miss handler,
 * which the translator recognises at VBR + 0x400 and runs itself (rt.c
 * tlb_refill, Makefile TLB_REFILL). A process page directory (TTB) and a
 * kernel one (fake g_pprcNK->ppdir, for addresses from the shared heap
 * base up) map test pages; each access first misses in an empty TLB. With
 * the refill the guest handler never runs; without it (TLB_REFILL=0, or a
 * failed walk) the handler's translated code does the same walk. Either
 * way the data must come from the mapped page. An entry without its valid
 * bit takes the handler's TLBMissError exit (here: map P_ERR, count). */
#include "hyper.h"
#include "mmio.h"

#define MMUCR  0xFF000010u
#define TTB    0xFF000008u
#define FLAGS  0x17Cu                  /* V, PR=11, 4 KiB, C, D */

#define DIR    0x8C304000u              /* process page directory (TTB) */
#define TBL    0x8C305000u              /* its table for 0x00400000-0x007FFFFF */
#define NKDIR  0x8C306000u              /* kernel page directory */
#define NKTBL  0x8C307000u              /* its table for 0x70400000-0x707FFFFF */
#define PRC    0x8C308000u              /* fake process: ppdir at +44 */
#define P_A    0x8C309000u
#define P_B    0x8C30A000u
#define P_ERR  0x8C30B000u

#define V_USER  0x00402000u             /* TBL[2] -> P_A */
#define V_INV   0x00403000u             /* TBL[3]: P_B without the valid bit */
#define V_HEAP  0x70402000u             /* NKTBL[2] -> P_B, through g_pprcNK */

extern char guest_vectors[];
unsigned fake_pprcNK = PRC;
volatile unsigned tlbr_errors, tlbr_unexpected_spc;
static int fails;

void tlbr_unexpected(void)
{
    hc_puts("FAIL unexpected exception at "); hc_puthex(tlbr_unexpected_spc); hc_putc('\n');
    hc_exit(3);
}

static void check(const char *name, int ok)
{
    hc_puts(ok ? "PASS " : "FAIL "); hc_puts(name); hc_putc('\n');
    if (!ok) fails++;
}

#define W(a) (*(volatile unsigned *)(a))
static void flush(void) { mmio_w32(MMUCR, 0x5); }      /* AT, TI: every access below misses first */

void guest_main(void)
{
    unsigned i, r0, e0, v;
    int refill = hc_getstat(431) != 0;

    hc_puts(refill ? "tlbrefill test: CE6 TLB miss handler, refilled by the translator\n"
                   : "tlbrefill test: CE6 TLB miss handler, run as guest code\n");
    for (i = 0; i < 1024; ++i) { W(DIR + 4 * i) = 0; W(TBL + 4 * i) = 0; W(NKDIR + 4 * i) = 0; W(NKTBL + 4 * i) = 0; }
    W(DIR + 4 * (V_USER >> 22)) = TBL;
    W(TBL + 4 * ((V_USER >> 12) & 0x3FF)) = (P_A & 0x1FFFFC00u) | FLAGS;
    W(TBL + 4 * ((V_INV >> 12) & 0x3FF)) = (P_B & 0x1FFFFC00u) | (FLAGS & ~0x100u);
    W(NKDIR + 4 * (V_HEAP >> 22)) = NKTBL;
    W(NKTBL + 4 * ((V_HEAP >> 12) & 0x3FF)) = (P_B & 0x1FFFFC00u) | FLAGS;
    W(PRC + 44) = NKDIR;
    W(P_A + 0x10) = 0xA1A2A3A4u; W(P_B + 0x10) = 0xB1B2B3B4u; W(P_ERR + 0x10) = 0xE1E2E3E4u;
    W(P_A + 0x24) = 0;

    /* CE's handler reads TEA/TTB/EXPEVT and writes PTEA/PTEL through r7_bank1 */
    hc_devrange((unsigned)guest_vectors, (unsigned)guest_vectors + 0x1000u);
    __asm__ volatile ("ldc %0, r7_bank" :: "r"(0xFF000000u));
    __asm__ volatile ("ldc %0, vbr" :: "r"(guest_vectors));
    mmio_w32(TTB, DIR);

    flush(); r0 = hc_getstat(430); e0 = tlbr_errors;
    v = W(V_USER + 0x10);
    check("read through the process page directory", v == 0xA1A2A3A4u);
    if (refill) check("...refilled by the translator", hc_getstat(430) == r0 + 1);
    check("...no TLBMissError", tlbr_errors == e0);

    flush(); r0 = hc_getstat(430);
    W(V_USER + 0x24) = 0x5A5A1234u;
    check("write miss (EXPEVT 0x060) lands in the mapped page", W(P_A + 0x24) == 0x5A5A1234u);
    if (refill) check("...refilled by the translator", hc_getstat(430) == r0 + 1);

    flush(); r0 = hc_getstat(430);
    v = W(V_HEAP + 0x10);
    check("shared heap / kernel address through g_pprcNK->ppdir", v == 0xB1B2B3B4u);
    if (refill) check("...refilled by the translator", hc_getstat(430) == r0 + 1);

    flush(); r0 = hc_getstat(430); e0 = tlbr_errors;
    v = W(V_INV + 0x10);
    check("entry without the valid bit: the guest handler's TLBMissError", v == 0xE1E2E3E4u && tlbr_errors == e0 + 1);
    if (refill) check("...not refilled by the translator", hc_getstat(430) == r0);

    flush();
    v = W(V_USER + 0x10) + W(V_HEAP + 0x10);
    check("two misses in one expression", v == 0xA1A2A3A4u + 0xB1B2B3B4u);

    hc_puts(fails ? "RESULT: FAIL\n" : "RESULT: PASS\n");
    hc_exit(fails);
}
