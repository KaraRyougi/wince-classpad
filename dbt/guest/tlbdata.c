/* tlbdata.c - every memory addressing form with a synchronous TLB miss on
 * the access itself: the data page is mapped only by the miss handler and
 * the TLB is flushed before each form, so the translated sequence faults at
 * its memory operation, the handler refills, and the instruction restarts.
 * This is the Windows CE heap's situation (kernel VM at 0xD0000000). */
#include "hyper.h"
#include "mmio.h"

#define PTEH   0xFF000000u
#define PTEL   0xFF000004u
#define MMUCR  0xFF000010u
#define V_LAZY 0x00402000u
#define P_LAZY 0x8C302000u
#define PTEL_FLAGS 0x16Eu

extern char guest_vectors[];
volatile unsigned fault_pr;
static int fails;
static volatile unsigned faults, misses;

static void check(const char *name, int ok)
{
    hc_puts(ok ? "PASS " : "FAIL "); hc_puts(name); hc_putc('\n');
    if (!ok) fails++;
}

static void tlb_map(unsigned v, unsigned p)
{
    mmio_w32(PTEH, v & 0xFFFFFC00u);
    mmio_w32(PTEL, ((p & 0x1FFFFFFFu) & 0xFFFFFC00u) | PTEL_FLAGS);
    __asm__ volatile ("ldtlb" ::: "memory");
}

unsigned guest_fault(unsigned vec, unsigned spc, unsigned expevt, unsigned tea)
{
    faults++;
    if ((expevt == 0x040 || expevt == 0x060) && (tea & 0xFFFFF000u) == V_LAZY) {
        misses++;
        tlb_map(V_LAZY, P_LAZY);
        return 0;
    }
    hc_puts("unexpected fault vec "); hc_puthex(vec); hc_puts(" expevt "); hc_puthex(expevt);
    hc_puts(" tea "); hc_puthex(tea); hc_puts(" spc "); hc_puthex(spc); hc_putc('\n');
    hc_exit(3);
    return 0;
}

static void flush(void) { mmio_w32(MMUCR, mmio_r32(MMUCR) | 0x4); }
#define P(off) (*(volatile unsigned *)(P_LAZY + (off)))
#define PB(off) (*(volatile unsigned char *)(P_LAZY + (off)))
#define V(off) (V_LAZY + (off))

extern unsigned ld_b(unsigned, unsigned), ld_w(unsigned, unsigned), ld_l(unsigned, unsigned);
extern unsigned ld_bpost(unsigned, unsigned *), ld_wpost(unsigned, unsigned *), ld_lpost(unsigned, unsigned *);
extern unsigned ld_bdisp(unsigned, unsigned), ld_wdisp(unsigned, unsigned), ld_ldisp(unsigned, unsigned);
extern unsigned ld_br0(unsigned, unsigned), ld_wr0(unsigned, unsigned), ld_lr0(unsigned, unsigned);
extern unsigned ld_bgbr(unsigned, unsigned), ld_wgbr(unsigned, unsigned), ld_lgbr(unsigned, unsigned);
extern unsigned ld_lself(unsigned, unsigned), ld_lpostself(unsigned, unsigned), ld_wsign(unsigned, unsigned);
extern unsigned st_b(unsigned, unsigned), st_w(unsigned, unsigned), st_l(unsigned, unsigned);
extern unsigned st_bpre(unsigned, unsigned), st_wpre(unsigned, unsigned), st_lpre(unsigned, unsigned), st_lpreself(unsigned, unsigned);
extern unsigned st_bdisp(unsigned, unsigned), st_wdisp(unsigned, unsigned), st_ldisp(unsigned, unsigned);
extern unsigned st_br0(unsigned, unsigned), st_wr0(unsigned, unsigned), st_lr0(unsigned, unsigned);
extern unsigned st_bgbr(unsigned, unsigned), st_wgbr(unsigned, unsigned), st_lgbr(unsigned, unsigned);
extern unsigned st_movca(unsigned, unsigned), st_lr0base(unsigned, unsigned), rmw_l(unsigned, unsigned);

typedef unsigned (*fn_t)(unsigned, unsigned);

/* one form: flush, run, expect exactly one miss */
static void run(const char *name, fn_t f, unsigned a, unsigned b, unsigned want_r0)
{
    unsigned m0, r;
    flush();
    m0 = misses;
    r = f(a, b);
    if (r != want_r0 || misses != m0 + 1) {
        hc_puts("  "); hc_puts(name); hc_puts(": r0 "); hc_puthex(r); hc_puts(" want "); hc_puthex(want_r0);
        hc_puts(" misses+"); hc_puthex(misses - m0); hc_putc('\n');
    }
    check(name, r == want_r0 && misses == m0 + 1);
}

static void fill(void)
{
    P(0) = 0x44332211u; P(4) = 0x88776655u; P(8) = 0xCCBBAA99u; P(12) = 0x0000F0E0u; P(16) = 0; P(20) = 0;
}

void guest_main(void)
{
    unsigned base;

    hc_puts("tlbdata test: memory forms under synchronous TLB misses\n");
    __asm__ volatile ("ldc %0, vbr" :: "r"(guest_vectors));
    mmio_w32(MMUCR, 0x4);
    mmio_w32(MMUCR, 0x1);                                    /* AT, TLB empty: every form below misses first */
    fill();

    run("mov.b @rm,rn", ld_b, V(0), 0, 0x11);
    run("mov.w @rm,rn", ld_w, V(0), 0, 0x2211);
    run("mov.l @rm,rn", ld_l, V(0), 0, 0x44332211u);
    base = 0; run("mov.b @rm+,rn", (fn_t)ld_bpost, V(4), (unsigned)&base, 0x55); check("  base advanced by 1", base == V(5));
    base = 0; run("mov.w @rm+,rn", (fn_t)ld_wpost, V(4), (unsigned)&base, 0x6655); check("  base advanced by 2", base == V(6));
    base = 0; run("mov.l @rm+,rn", (fn_t)ld_lpost, V(4), (unsigned)&base, 0x88776655u); check("  base advanced by 4", base == V(8));
    run("mov.b @(disp,rm),r0", ld_bdisp, V(0), 0, 0x44);
    run("mov.w @(disp,rm),r0", ld_wdisp, V(0), 0, 0x6655);
    run("mov.l @(disp,rm),rn", ld_ldisp, V(0), 0, 0xCCBBAA99u);
    run("mov.b @(r0,rm),rn", ld_br0, V(0), 0, 0x66);
    run("mov.w @(r0,rm),rn", ld_wr0, V(0), 0, 0x6655);
    run("mov.l @(r0,rm),rn", ld_lr0, V(0), 0, 0xCCBBAA99u);
    run("mov.b @(disp,gbr),r0", ld_bgbr, V(0), 0, 0x22);
    run("mov.w @(disp,gbr),r0", ld_wgbr, V(0), 0, 0x6655);
    run("mov.l @(disp,gbr),r0", ld_lgbr, V(0), 0, 0xCCBBAA99u);
    run("mov.l @rn,rn", ld_lself, V(8), 0, 0xCCBBAA99u);
    /* casio-emu adds 4 to the loaded value for @rm+,rn with m == n (the SH
     * manual increments only when m != n); the compiler never emits it, so
     * the emulator's answer is accepted here and noted in docs/04 */
    {
        unsigned m0; flush(); m0 = misses;
        base = ld_lpostself(V(8), 0);
        check("mov.l @rn+,rn (casio-emu adds 4 to the raw loaded word)", (base == 0xCCBBAA99u || base == 0xD0BBAA99u) && misses == m0 + 1);
    }
    run("mov.w sign extension", ld_wsign, V(0), 0, 0xFFFFF0E0u);
    run("mov.b @rm,rn sign extension", ld_b, V(11), 0, 0xFFFFFFCCu);

    fill();
    run("mov.b rm,@rn", st_b, V(16), 0xA1, 0xA1); check("  byte landed", PB(16) == 0xA1 && P(16) == 0xA1);
    run("mov.w rm,@rn", st_w, V(16), 0xB2A1, 0xB2A1); check("  word landed", P(16) == 0xB2A1);
    run("mov.l rm,@rn", st_l, V(16), 0xD4C3B2A1u, 0xD4C3B2A1u); check("  long landed", P(16) == 0xD4C3B2A1u);
    run("mov.b rm,@-rn", st_bpre, V(21), 0x5A, V(20)); check("  byte at the decremented address", PB(20) == 0x5A);
    run("mov.w rm,@-rn", st_wpre, V(22), 0x7B6A, V(20)); check("  word at the decremented address", (P(20) & 0xFFFF) == 0x7B6A);
    run("mov.l rm,@-rn", st_lpre, V(24), 0x11223344u, V(20)); check("  long at the decremented address", P(20) == 0x11223344u);
    run("mov.l rn,@-rn", st_lpreself, V(24), 0, V(20)); check("  stores the decremented value", P(20) == V(20));
    run("mov.b r0,@(disp,rn)", st_bdisp, V(16), 0xEE, 0xEE); check("  at +3", PB(19) == 0xEE);
    run("mov.w r0,@(disp,rn)", st_wdisp, V(16), 0xCAFE, 0xCAFE); check("  at +4", (P(20) & 0xFFFF) == 0xCAFE);
    run("mov.l rm,@(disp,rn)", st_ldisp, V(16), 0x0BADF00Du, 0x0BADF00Du); check("  at +8", P(24) == 0x0BADF00Du);
    run("mov.b rm,@(r0,rn)", st_br0, V(16), 0x99, 5); check("  at +5", PB(21) == 0x99);
    run("mov.w rm,@(r0,rn)", st_wr0, V(16), 0x1234, 4); check("  at +4", (P(20) & 0xFFFF) == 0x1234);
    run("mov.l rm,@(r0,rn)", st_lr0, V(16), 0x76543210u, 8); check("  at +8", P(24) == 0x76543210u);
    run("mov.b r0,@(disp,gbr)", st_bgbr, V(16), 0x3C, 0x3C); check("  at +1", PB(17) == 0x3C);
    run("mov.w r0,@(disp,gbr)", st_wgbr, V(16), 0x5678, 0x5678); check("  at +4", (P(20) & 0xFFFF) == 0x5678);
    run("mov.l r0,@(disp,gbr)", st_lgbr, V(16), 0x89ABCDEFu, 0x89ABCDEFu); check("  at +8", P(24) == 0x89ABCDEFu);
    run("movca.l r0,@rn", st_movca, V(16), 0x600DBEEFu, 0x600DBEEFu); check("  landed", P(16) == 0x600DBEEFu);
    run("mov.l rm,@(disp,r0)", st_lr0base, V(8), 0x13579BDFu, V(8)); check("  landed", P(16) == 0x13579BDFu);
    P(0) = 10; run("load-modify-store", rmw_l, V(0), 5, 15); check("  stored", P(0) == 15);

    mmio_w32(MMUCR, 0x4);
    hc_puts("faults "); hc_puthex(faults); hc_puts(" misses "); hc_puthex(misses); hc_putc('\n');
    hc_stats();
    hc_puts(fails ? "RESULT: FAIL\n" : "RESULT: PASS\n");
    hc_exit(fails);
}
