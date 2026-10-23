/* bench.c - translated vs native slowdown. The kernels in bench/loops.S are
 * linked into this guest (little-endian, run under the translator) and into
 * the translator image (big-endian, run natively through HC_BENCH). Both are
 * timed with TMU1, which the runtime starts free-running at Pphi/4. */
#include "hyper.h"
#include "mmio.h"

#define TCNT1 0xA4490018u
#define ITER  20000u

extern unsigned bench_alu(unsigned, void *), bench_mem(unsigned, void *), bench_copy(unsigned, void *),
                bench_stack(unsigned, void *), bench_branch(unsigned, void *), bench_jsr(unsigned, void *),
                bench_bsr(unsigned, void *), bench_callframe(unsigned, void *);

static const struct { const char *name; unsigned (*fn)(unsigned, void *); } kernels[] = {
    { "alu        ", bench_alu },
    { "mem mixed  ", bench_mem },
    { "mem copy   ", bench_copy },
    { "stack      ", bench_stack },
    { "branch     ", bench_branch },
    { "call jsr   ", bench_jsr },
    { "call bsr   ", bench_bsr },
    { "call frame ", bench_callframe },
};

static unsigned buf[64] __attribute__((aligned(16)));

/* no libgcc in the guest: shift-subtract divide */
static unsigned udiv(unsigned a, unsigned b, unsigned *rem)
{
    unsigned q = 0, r = 0; int i;
    for (i = 31; i >= 0; i--) {
        r = (r << 1) | ((a >> i) & 1);
        if (r >= b) { r -= b; q |= 1u << i; }
    }
    if (rem) *rem = r;
    return q;
}

static void putdec(unsigned v)
{
    char s[12]; int i = 0; unsigned r;
    do { v = udiv(v, 10, &r); s[i++] = (char)('0' + r); } while (v);
    while (i) hc_putc(s[--i]);
}

static void putratio(unsigned num, unsigned den)      /* num/den with two decimals */
{
    unsigned r = den ? udiv(num * 100u + (den >> 1), den, 0) : 0, lo;
    unsigned hi = udiv(r, 100, &lo);
    putdec(hi); hc_putc('.'); hc_putc((char)('0' + udiv(lo, 10, 0))); hc_putc((char)('0' + udiv(lo, 1, 0) - 10 * udiv(lo, 10, 0)));
}

void guest_main(void)
{
    unsigned k, t0, t1, tr, nat;
    hc_puts("bench: translated vs native, "); putdec(ITER); hc_puts(" iterations, TMU1 ticks (Pphi/4)\n");
    hc_bench(0xFFFFFFFFu, 0);                           /* start the timer */
    __asm__ volatile ("ldc %0, sr" :: "r"(0x400010F0u));  /* interrupts masked */
    hc_puts("kernel        translated    native   slowdown\n");
    for (k = 0; k < sizeof kernels / sizeof kernels[0]; k++) {
        volatile unsigned reps = 2;                       /* volatile: an unrolled loop would give each pass its own return site */
        unsigned rep;
        /* the first timed pass also translates the block after this call site
         * (its return address differs from any warm-up call's), so time the
         * same call site twice and keep the second */
        for (rep = 0; rep < reps; rep++) {
            t0 = mmio_r32(TCNT1);
            kernels[k].fn(ITER, buf);
            t1 = mmio_r32(TCNT1);
        }
        tr = t0 - t1;
        nat = hc_bench(k, ITER);
        hc_puts(kernels[k].name); hc_puts("  "); putdec(tr); hc_puts("  "); putdec(nat); hc_puts("  ");
        putratio(tr, nat); hc_puts("x\n");
    }
    hc_stats();
    hc_puts("RESULT: PASS\n");
    hc_exit(0);
}
