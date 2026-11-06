/* tmu.c - guest interrupt test: program TMU0 through pre-swapped MMIO, take
 * its interrupt through the guest's own VBR while running under the
 * translator, rte back into translated code. Also checks that register state
 * survives interrupts landing anywhere in a store-heavy loop, sleep wake-up,
 * and masking at the INTC. */
#include "hyper.h"
#include "mmio.h"

#define TSTR    0xA4490004u             /* 8-bit */
#define TCOR0   0xA4490008u             /* 32-bit */
#define TCNT0   0xA449000Cu             /* 32-bit */
#define TCR0    0xA4490010u             /* 16-bit: UNF=0x100 UNIE=0x20 TPSC[2:0] */
#define IPRA    0xA4080000u             /* 16-bit, TMU0 priority in bits 15:12 */
#define IMR4    0xA4080090u             /* 8-bit, TMU0 = 0x10 */
#define IMCR4   0xA40800D0u             /* 8-bit, write 1 to unmask */

#define SR_BL    0x10000000u
#define SR_IMASK 0x000000F0u

#define PERIOD   5000u                  /* TMU ticks between interrupts */

extern char guest_vectors[];
void trap_probe(volatile unsigned *out);
unsigned loop_tcarry(unsigned, volatile unsigned *);
unsigned loop_trapa(unsigned, volatile unsigned *), loop_alu(unsigned, volatile unsigned *), loop_bfs(unsigned, volatile unsigned *), loop_tslot(unsigned, volatile unsigned *),
         loop_ld(unsigned, volatile unsigned *), loop_ldpost(unsigned, volatile unsigned *), loop_ldself(unsigned, volatile unsigned *),
         loop_st(unsigned, volatile unsigned *), loop_stpre(unsigned, volatile unsigned *), loop_r0idx(unsigned, volatile unsigned *),
         loop_r0disp(unsigned, volatile unsigned *), loop_rmw(unsigned, volatile unsigned *), loop_pcrel(unsigned, volatile unsigned *),
         loop_jsr(unsigned, volatile unsigned *), loop_jsrslot(unsigned, volatile unsigned *), loop_pushpop(unsigned, volatile unsigned *),
         loop_ldcsr(unsigned, volatile unsigned *), loop_jsrtgt(unsigned, volatile unsigned *);
static int fails;
volatile unsigned ticks, last_intevt, bad_intevt, fatal_vec;
volatile unsigned irq_regs[18];                 /* r0-r15, spc, ssr of the interrupted code (tmu_vectors.S) */
volatile unsigned scratch[16];

static void check(const char *name, int ok)
{
    hc_puts(ok ? "PASS " : "FAIL "); hc_puts(name); hc_putc('\n');
    if (!ok) fails++;
}

static unsigned get_sr(void) { unsigned v; __asm__ volatile ("stc sr, %0" : "=r"(v)); return v; }
static void set_sr(unsigned v) { __asm__ volatile ("ldc %0, sr" :: "r"(v) : "memory"); }
static void irq_enable(void)  { set_sr(get_sr() & ~(SR_BL | SR_IMASK)); }
static void irq_disable(void) { set_sr((get_sr() & ~SR_BL) | SR_IMASK); }

/* Interrupt handler. Runs translated, with RB=1 BL=1 (tmu_vectors.S). */
void guest_irq(unsigned intevt)
{
    last_intevt = intevt;
    if (intevt != 0x400) bad_intevt++;
    mmio_w16(TCR0, mmio_r16(TCR0) & (unsigned short)~0x0100u);  /* clear UNF */
    ticks++;
}

volatile unsigned traps;
void guest_trap(void) { traps++; }

void guest_fatal(void)
{
    hc_puts("FATAL: unexpected exception, vector id "); hc_puthex(fatal_vec); hc_putc('\n');
    hc_exit(2);
}

/* Register- and store-heavy loop; interrupts may land on any instruction. */
static unsigned work(unsigned n)
{
    unsigned a = 1, b = 2, c = 3, d = 4, e = 5, f = 6, g = 7, h = 8, i;
    for (i = 0; i < n; i++) {
        a += b ^ i; b += c * 3; c ^= d + a; d += e >> 1;
        e += f | a; f ^= g + b; g += h; h ^= a;
        scratch[i & 15] = a;
        scratch[(i + 5) & 15] += h;
    }
    return a ^ b ^ c ^ d ^ e ^ f ^ g ^ h ^ scratch[3];
}

/* Restart channel 0 with another period: the tick is an instruction count,
 * so a loop of fixed length sees it at the same phases; several periods
 * sweep the interrupt over every host instruction of a construct. */
static void tmu_period(unsigned p)
{
    mmio_w8(TSTR, mmio_r8(TSTR) & (unsigned char)~1u);
    mmio_w32(TCOR0, p);
    mmio_w32(TCNT0, p);
    mmio_w8(TSTR, mmio_r8(TSTR) | 1u);
}

static void tmu_setup(void)
{
    mmio_w8(TSTR, mmio_r8(TSTR) & (unsigned char)~1u);          /* stop channel 0 */
    mmio_w16(TCR0, 0x0020);                                      /* UNIE, Pphi/4, UNF clear */
    mmio_w32(TCOR0, PERIOD);
    mmio_w32(TCNT0, PERIOD);
    mmio_w16(IPRA, (unsigned short)((mmio_r16(IPRA) & 0x0FFFu) | 0xF000u));
    mmio_w8(IMCR4, 0x10);                                        /* unmask TMU0 at the INTC */
}

void guest_main(void)
{
    unsigned t0, t1, expect, got, before, i;

    hc_puts("tmu test: VBR, TMU0 via pre-swapped MMIO, interrupts under the translator\n");
    __asm__ volatile ("ldc %0, vbr" :: "r"(guest_vectors));

    check("boot cache policy ICE/OCE/CB on, WT off", (mmio_r32(0xff00001cu) & 0x107u) == 0x105u);
    tmu_setup();
    check("TCOR0 reads back through the swap convention", mmio_r32(TCOR0) == PERIOD);
    check("TCR0 reads back (16-bit)", mmio_r16(TCR0) == 0x0020);

    mmio_w8(TSTR, mmio_r8(TSTR) | 1u);
    t0 = mmio_r32(TCNT0);
    for (i = 0; i < 200; i++) scratch[0] = i;
    t1 = mmio_r32(TCNT0);
    check("TCNT0 counts down while started", t1 < t0 && t0 <= PERIOD);

    /* interrupts still masked (IMASK=15, BL=0 from the translator's reset SR) */
    hc_puts("sr = "); hc_puthex(get_sr()); hc_putc('\n');
    irq_disable();
    for (i = 0; i < 4 * PERIOD * 4; i++) scratch[1] = i;           /* > one period */
    check("no interrupt while IMASK=15", ticks == 0);
    check("UNF pending in TCR0 meanwhile", (mmio_r16(TCR0) & 0x0100) != 0);

    irq_enable();
    /* Bound this wait by timer progress, not translated loop speed. Faster
     * chaining can finish the old empty loop before three timer periods. */
    {
        unsigned previous = mmio_r32(TCNT0), elapsed = 0;
        for (i = 0; i < 1000000 && elapsed < 10 * PERIOD && ticks < 3; ++i) {
            unsigned current = mmio_r32(TCNT0);
            elapsed += previous >= current ? previous - current : previous + PERIOD + 1 - current;
            previous = current;
        }
    }
    check("interrupts arrive with IMASK=0", ticks >= 3);
    check("INTEVT = 0x400 (TMU0)", last_intevt == 0x400 && bad_intevt == 0);
    check("SR restored by rte: BL=0, IMASK=0", (get_sr() & (SR_BL | SR_IMASK)) == 0);

    /* which registers survive trapa -> guest handler -> rte? */
    {
        static volatile unsigned out[17];
        unsigned sp, i, bad = 0;
        __asm__ volatile ("mov r15, %0" : "=r"(sp));
        out[16] = sp - 32;                          /* r15 inside trap_probe after its 8 pushes */
        irq_disable();
        trap_probe(out);
        for (i = 0; i < 15; i++) {
            unsigned e = 0x11111111u * (i + 1);
            if (out[i] != e) { bad++; hc_puts("  r"); hc_puthex(i); hc_puts(" = "); hc_puthex(out[i]); hc_puts(" expected "); hc_puthex(e); hc_putc('\n'); }
        }
        if (out[15] != 1) { bad++; hc_puts("  T lost\n"); }
        check("r0-r14 and T survive trapa -> handler -> rte", bad == 0);
    }

    /* exception + guest handler + rte at a fixed point, no interrupts: must equal the plain loop */
    irq_disable();
    expect = loop_alu(3000, scratch);
    before = traps;
    got = loop_trapa(3000, scratch);
    check("trapa into guest handler and rte, 3000 times", got == expect && traps == before + 3000);

    /* state survives interrupts landing inside translated blocks: one
     * construct per loop (tmu_loops.S), each run with interrupts off then on */
    {
        static const struct { const char *name; unsigned (*fn)(unsigned, volatile unsigned *); } loops[] = {
            { "T across indirect calls", loop_tcarry }, { "alu + bf", loop_alu }, { "bf/s", loop_bfs }, { "bt/s with dt in the slot", loop_tslot },
            { "load", loop_ld }, { "post-increment load", loop_ldpost }, { "mov.l @r6,r6", loop_ldself },
            { "store", loop_st }, { "pre-decrement store", loop_stpre }, { "r0-indexed store", loop_r0idx },
            { "r0 data store", loop_r0disp }, { "load-modify-store", loop_rmw }, { "pc-relative", loop_pcrel },
            { "jsr/rts", loop_jsr }, { "jsr with store in slot", loop_jsrslot }, { "push/pop pr and regs", loop_pushpop },
        };
        unsigned n = sizeof loops / sizeof loops[0], k;
        for (k = 0; k < n; k++) {
            irq_disable();
            expect = loops[k].fn(30000, scratch);
            if (loops[k].fn == loop_tcarry) check("T carry baseline is exact", expect == 30000);
            irq_enable();
            before = ticks;
            got = loops[k].fn(30000, scratch);
            if (got != expect) { hc_puts("  expect "); hc_puthex(expect); hc_puts(" got "); hc_puthex(got); hc_putc('\n'); }
            check(loops[k].name, got == expect && ticks > before + 1);
        }
    }
    /* a helper call per iteration: interrupts land in the helper's prologue
     * window (before it masks them), in its exit tail and in the call
     * sequence. The prologue reuses the slot the caller's scratch was
     * spilled to; a resume there must not undo that spill (2026-11-04: the
     * scratch, r1 here, took r0's value once in a few thousand ticks). */
    irq_disable();
    expect = loop_ldcsr(200000, scratch);
    irq_enable();
    before = ticks;
    got = loop_ldcsr(200000, scratch);
    if (got != expect) { hc_puts("  expect "); hc_puthex(expect); hc_puts(" got "); hc_puthex(got); hc_putc('\n'); }
    check("ldc sr helper under interrupts", got == expect && ticks > before + 50);
    if (hc_getstat(63) & 1)
        check("same-bank SR fast path actually exercised", hc_getstat(31) >= 400000);

    /* a jsr whose delay-slot store uses the target register as its swap
     * scratch: an interrupt on the scratch's restore must resume at the
     * guest's target, not at the swapped store data (2026-11-04) */
    irq_disable();
    expect = loop_jsrtgt(50000, scratch);
    before = ticks;
    {
        unsigned bad = 0, k;
        for (k = 0; k < 8; k++) {
            irq_disable();
            tmu_period(PERIOD - 1 - 7 * k);
            irq_enable();
            got = loop_jsrtgt(50000, scratch);
            if (got != expect) { bad++; hc_puts("  period "); hc_puthex(PERIOD - 1 - 7 * k); hc_puts(" expect "); hc_puthex(expect); hc_puts(" got "); hc_puthex(got); hc_putc('\n'); }
        }
        irq_disable();
        tmu_period(PERIOD);
        irq_enable();
        check("jsr with the slot store's scratch = target register", bad == 0 && ticks > before + 100);
    }

    irq_disable();
    expect = work(60000);
    irq_enable();
    before = ticks;
    got = work(60000);
    check("register/memory state intact across interrupts in a hot loop", got == expect);
    check("...and interrupts actually landed in it", ticks > before + 3);
    hc_puts("ticks so far: "); hc_puthex(ticks); hc_putc('\n');

    /* sleep is woken by the next tick and execution resumes after it (a tick
     * may also land between reading `before` and the sleep) */
    before = ticks;
    __asm__ volatile ("sleep" ::: "memory");
    check("sleep woken by TMU interrupt", ticks > before && ticks <= before + 2);

    /* mask at the INTC: no more interrupts although the timer keeps running */
    mmio_w8(IMR4, 0x10);
    before = ticks;
    for (i = 0; i < 4 * PERIOD * 4; i++) scratch[2] = i;
    check("IMR4 masks TMU0", ticks == before);
    check("UNF still set while masked", (mmio_r16(TCR0) & 0x0100) != 0);

    mmio_w8(TSTR, mmio_r8(TSTR) & (unsigned char)~1u);
    irq_disable();
    hc_puts("ticks total: "); hc_puthex(ticks); hc_putc('\n');
    if (hc_getstat(16)) check("interrupt return reuses safe interior entries", hc_getstat(15) > 0);
    hc_stats();
    hc_puts(fails ? "RESULT: FAIL\n" : "RESULT: PASS\n");
    hc_exit(fails);
}
