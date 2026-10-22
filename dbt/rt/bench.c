/* bench.c - native side of the benchmark hypercall: runs the same loop
 * kernels (bench/loops.S, assembled big-endian) directly on the host and
 * returns the TMU1 ticks they took. TMU1 free-runs at Pphi/4; in casio-emu
 * one tick is four executed instructions, so the ratio translated/native is
 * an instruction-count ratio there. */
#include "rt.h"

#define TSTR   (*(volatile sf_u8  *)0xA4490004u)
#define TCOR1  (*(volatile sf_u32 *)0xA4490014u)
#define TCNT1  (*(volatile sf_u32 *)0xA4490018u)
#define TCR1   (*(volatile sf_u16 *)0xA449001Cu)

typedef sf_u32 (*bench_fn)(sf_u32 n, void *buf);
extern sf_u32 bench_alu(sf_u32, void *), bench_mem(sf_u32, void *), bench_copy(sf_u32, void *),
              bench_stack(sf_u32, void *), bench_branch(sf_u32, void *), bench_jsr(sf_u32, void *),
              bench_bsr(sf_u32, void *), bench_callframe(sf_u32, void *);

static const bench_fn kernels[] = { bench_alu, bench_mem, bench_copy, bench_stack,
                                    bench_branch, bench_jsr, bench_bsr, bench_callframe };
static sf_u32 bench_buf[64] __attribute__((aligned(16)));

void dbt_bench_timer_init(void)
{
    TSTR &= (sf_u8)~2u;
    TCR1 = 0;                       /* Pphi/4, no interrupt */
    TCOR1 = 0xFFFFFFFFu;
    TCNT1 = 0xFFFFFFFFu;
    TSTR |= 2u;
}

/* HC_BENCH: r4 = kernel index (0xFFFFFFFF: init timer), r5 = iterations.
 * Returns ticks for a native run (one warm-up call first, like the guest). */
sf_u32 dbt_bench_native(sf_u32 which, sf_u32 n)
{
    sf_u32 t0, t1;
    if (which == 0xFFFFFFFFu) { dbt_bench_timer_init(); return 0; }
    if (which >= sizeof kernels / sizeof kernels[0]) return 0;
    kernels[which](16, bench_buf);
    t0 = TCNT1;
    kernels[which](n, bench_buf);
    t1 = TCNT1;
    return t0 - t1;
}
