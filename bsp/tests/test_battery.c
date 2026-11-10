/* test_battery.c - classpad_battery.h: Casio's level thresholds (OS 02.01.7003,
 * 0x800B0354 and 0x800813EA), the one-way latch and the percentage. */
#include <stdio.h>
#include "../classpad50/SRC/INC/classpad_battery.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main(void)
{
    static const struct { unsigned raw; int alk, nimh; } t[] = {
        { 600, 1, 1 }, { 476, 1, 1 }, { 475, 2, 1 }, { 466, 2, 1 }, { 465, 2, 2 }, { 448, 2, 2 }, { 447, 2, 3 },
        { 430, 2, 3 }, { 429, 3, 4 }, { 403, 3, 4 }, { 402, 3, 5 }, { 384, 3, 5 }, { 383, 4, 5 }, { 348, 4, 5 },
        { 347, 5, 5 }, { 0, 5, 5 },
    };
    unsigned i, raw, prev;
    int type, latched = 0;
    for (i = 0; i < sizeof t / sizeof t[0]; i++) {
        CHECK(ClasspadBattLevel(t[i].raw, CLASSPAD_BATT_ALKALINE) == t[i].alk, "alkaline raw %u: level %d, want %d", t[i].raw, ClasspadBattLevel(t[i].raw, 0), t[i].alk);
        CHECK(ClasspadBattLevel(t[i].raw, CLASSPAD_BATT_NIMH) == t[i].nimh, "Ni-MH raw %u: level %d, want %d", t[i].raw, ClasspadBattLevel(t[i].raw, 1), t[i].nimh);
    }
    /* the latch never improves */
    CHECK(ClasspadBattLatch(&latched, 2) == 2, "latch 2");
    CHECK(ClasspadBattLatch(&latched, 1) == 2, "latch keeps 2 over 1");
    CHECK(ClasspadBattLatch(&latched, 4) == 4, "latch 4");
    CHECK(ClasspadBattLatch(&latched, 3) == 4, "latch keeps 4 over 3");
    /* percent: monotonic, 0 at the floor, 100 at full, 25/50/75 at the thresholds */
    for (type = 0; type < 2; type++) {
        const CLASSPAD_BATT_CURVE *c = &g_ClasspadBattCurves[type];
        prev = 0;
        for (raw = 0; raw < 1024; raw++) {
            unsigned p = ClasspadBattPercent(raw, type);
            CHECK(p <= 100 && p >= prev, "type %d raw %u: percent %u after %u", type, raw, p, prev);
            prev = p;
        }
        CHECK(ClasspadBattPercent(c->floor, type) == 0 && ClasspadBattPercent(c->level1, type) == 100 && ClasspadBattPercent(1023, type) == 100, "type %d ends", type);
        CHECK(ClasspadBattPercent(c->level3, type) == 33 && ClasspadBattPercent(c->level2, type) == 67, "type %d thresholds: %u %u", type,
              ClasspadBattPercent(c->level3, type), ClasspadBattPercent(c->level2, type));
        CHECK(ClasspadBattPercent(c->level1 - 1, type) < 100, "type %d: below level 1 is under 100", type);
    }
    printf("battery: %d failures\n", fails);
    return fails != 0;
}
