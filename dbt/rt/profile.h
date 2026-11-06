/* Optional TMU1 profiling, Pphi/4 ticks (7,372,800 Hz at stock clocks).
 * Buckets overlap (e.g. TLB hook includes invalidation); never sum them.
 * Totals/elapsed wrap modulo 2^32. Compare snapshots less than one wrap
 * apart. Translation measures completed attempts only: a fault can abandon C.
 * PROFILE=1 reserves TMU1; do not use with guests that reprogram that timer. */
#ifndef DBT_PROFILE_H
#define DBT_PROFILE_H
enum { PROF_TRANSLATE, PROF_ASID, PROF_TLB, PROF_INVALIDATE, PROF_LCD, PROF_FLUSH, PROF_COUNT };
#ifdef DBT_PROFILE
static struct { sf_u32 calls, ticks, max; } g_profile[PROF_COUNT];
#define PROFILE_CLOCK (*(volatile sf_u32 *)0xA4490018u)
#define PROFILE_BEGIN sf_u32 profile_start = PROFILE_CLOCK
#define PROFILE_END(bucket) profile_end(bucket, profile_start)
static void profile_end(unsigned bucket, sf_u32 start)
{
    sf_u32 delta = start - PROFILE_CLOCK;
    g_profile[bucket].calls++;
    g_profile[bucket].ticks += delta;
    if (delta > g_profile[bucket].max) g_profile[bucket].max = delta;
}
static void profile_init(void)
{
    extern void dbt_bench_timer_init(void);
    memset(g_profile, 0, sizeof g_profile);
    dbt_bench_timer_init();
}
static sf_u32 profile_stat(sf_u32 index)
{
    if (index == 11) return 1;
    if (index == 12) return 0xFFFFFFFFu - PROFILE_CLOCK;
    if (index >= 32 && index < 32 + 3 * PROF_COUNT) {
        unsigned b = (index - 32) / 3, field = (index - 32) % 3;
        return field == 0 ? g_profile[b].calls : field == 1 ? g_profile[b].ticks : g_profile[b].max;
    }
    return 0;
}
static void profile_print(void)
{
    static const char *names[] = { "translate", "asid", "tlb", "invalidate", "lcd", "flush" };
    unsigned i;
    dbt_puts("[profile] elapsed_ticks="); dbt_puthex(profile_stat(12)); dbt_puts("\n");
    for (i = 0; i < PROF_COUNT; ++i) {
        dbt_puts("[profile] "); dbt_puts(names[i]);
        dbt_puts(" calls="); dbt_puthex(g_profile[i].calls);
        dbt_puts(" ticks="); dbt_puthex(g_profile[i].ticks);
        dbt_puts(" max="); dbt_puthex(g_profile[i].max); dbt_puts("\n");
    }
}
#else
#define PROFILE_BEGIN ((void)0)
#define PROFILE_END(bucket) ((void)0)
#define profile_init() ((void)0)
#define profile_print() ((void)0)
#define profile_stat(index) 0u
#endif
#endif
