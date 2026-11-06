/* Opt-in hardware diagnostics. Counters are cumulative; pair snapshots and
 * divide deltas by elapsed TMU1 ticks / measured timer frequency. PROFILE=1
 * owns TMU1, stops across suspend, and perturbs execution: not a speed build. */
#ifndef DBT_HW_DIAG_H
#define DBT_HW_DIAG_H
#ifdef DBT_HW_DIAG
#ifndef DBT_PROFILE
#error DBT_HW_DIAG requires PROFILE=1 for the elapsed-time clock
#endif
static struct {
    sf_u32 vec100, vec400, vec600, other, internal_trap;
    sf_u32 irq[128], irq_other, p0_ptel[4];
} g_hw;
static void hw_diag_exception(const dbt_gctx *ctx)
{
    if (ctx->vec_off == 0x100) {
        ++g_hw.vec100;
        if (ctx->expevt == 0x160 && (ctx->tra >> 2) >= 0xf0) ++g_hw.internal_trap;
    } else if (ctx->vec_off == 0x400) ++g_hw.vec400;
    else if (ctx->vec_off == 0x600) {
        ++g_hw.vec600;
        if (!(ctx->intevt & 31u) && ctx->intevt < 4096u) ++g_hw.irq[ctx->intevt >> 5];
        else ++g_hw.irq_other;
    } else ++g_hw.other;
}
static void hw_diag_ptel(sf_u32 vpn, sf_u32 ptel)
{
    if (vpn < 0x80000000u && (ptel & 0x100u))
        ++g_hw.p0_ptel[((ptel & 8u) ? 2 : 0) | (ptel & 1u)];
}
static void hw_diag_print(void)
{
    unsigned i;
    dbt_puts("[hw] ticks="); dbt_puthex(profile_stat(12));
    dbt_puts(" ccr="); dbt_puthex(*(volatile sf_u32 *)0xff00001cu);
    dbt_puts(" vec100="); dbt_puthex(g_hw.vec100);
    dbt_puts(" vec400="); dbt_puthex(g_hw.vec400);
    dbt_puts(" vec600="); dbt_puthex(g_hw.vec600);
    dbt_puts(" internal_trap="); dbt_puthex(g_hw.internal_trap);
    dbt_puts(" other="); dbt_puthex(g_hw.other); dbt_puts("\n");
    for (i = 0; i < 128; ++i) if (g_hw.irq[i]) {
        dbt_puts("[hw irq] intevt="); dbt_puthex(i << 5);
        dbt_puts(" count="); dbt_puthex(g_hw.irq[i]); dbt_puts("\n");
    }
    dbt_puts("[hw p0-ldtlb] c0wt0="); dbt_puthex(g_hw.p0_ptel[0]);
    dbt_puts(" c0wt1="); dbt_puthex(g_hw.p0_ptel[1]);
    dbt_puts(" c1wt0="); dbt_puthex(g_hw.p0_ptel[2]);
    dbt_puts(" c1wt1="); dbt_puthex(g_hw.p0_ptel[3]);
    dbt_puts(" irq_other="); dbt_puthex(g_hw.irq_other); dbt_puts("\n");
}
#else
#define hw_diag_exception(ctx) ((void)0)
#define hw_diag_ptel(vpn, ptel) ((void)0)
#define hw_diag_print() ((void)0)
#endif
#endif
