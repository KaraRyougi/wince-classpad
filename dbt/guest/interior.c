/* Exercise an independently entered suffix, then invalidate its owner. */
#include "hyper.h"
extern unsigned entry_start(void), entry_middle(void), entry_late(void);
__asm__(".align 2\n.global _entry_start\n.global _entry_middle\n.global _entry_late\n"
        "_entry_start:\n mov #1,r0\n_entry_middle:\n mov #2,r1\n"
        "_entry_late:\n mov #7,r0\n rts\n nop\n");
static int fails;
#ifndef MAX_BLOCKS
#define MAX_BLOCKS 4096
#endif
static unsigned short pressure[MAX_BLOCKS * 8];
static __attribute__((noinline)) unsigned suffix(void) { return entry_middle(); }
static __attribute__((noinline)) unsigned late_suffix(void) { return entry_late(); }
static void check(const char *name, int ok)
{
    hc_puts(ok ? "PASS " : "FAIL "); hc_puts(name); hc_putc('\n');
    if (!ok) ++fails;
}
void guest_main(void)
{
    unsigned before, result, removed;
    check("initial block executes", entry_start() == 7);
    before = hc_getstat(17);
    result = suffix();
    check("interior entry produces correct value", result == 7);
    check("interior reuse actually occurred", hc_getstat(18) && hc_getstat(17) > before);
    before = hc_getstat(17);
    check("second interior entry produces correct value", late_suffix() == 7);
    check("second interior entry actually reused", hc_getstat(17) > before);
    removed = hc_getstat(22);
    *(volatile unsigned short *)(unsigned)entry_late = 0xe009;
    hc_flush((unsigned)entry_start, 10);
    check("invalidation detaches cached interior target", suffix() == 9);
    check("invalidation detaches second interior target", late_suffix() == 9);
    if (hc_getstat(21)) check("invalidation removes published hash aliases", hc_getstat(22) >= removed + 2);
    if (hc_getstat(19)) {
        unsigned i, total = 0, expected = 0, generations;
        unsigned alias = (unsigned)entry_middle;
        unsigned alias_slot = ((alias >> 1) ^ (alias >> 9)) & 255u;
        for (i = 0; i < MAX_BLOCKS * 2; ++i) {
            pressure[i * 4] = 0xe000 | (i & 127);
            pressure[i * 4 + 1] = 0x000b;
            pressure[i * 4 + 2] = pressure[i * 4 + 3] = 0x0009;
        }
        hc_flush((unsigned)pressure, sizeof pressure);
        check("owner retranslated before bank recycling", entry_start() == 9 && suffix() == 9);
        generations = hc_getstat(7);
        for (i = 0; i < MAX_BLOCKS * 2; ++i) {
            unsigned addr = (unsigned)&pressure[i * 4];
            /* Avoid replacing the alias slot merely through collisions. */
            if ((((addr >> 1) ^ (addr >> 9)) & 255u) == alias_slot) continue;
            total += ((unsigned (*)(void))addr)(); expected += i & 127;
        }
        check("cache pressure executes correct code", total == expected);
        check("all banks have been recycled", hc_getstat(7) - generations >= 12);
        check("interior target survives owner-bank recycling", suffix() == 9);
    }
    hc_exit(fails ? 1 : 0);
}
