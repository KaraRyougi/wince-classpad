//------------------------------------------------------------------------------
//  classpad_battery.h - fx-CP400 battery level from the A/D converter (docs/08)
//
//  Four AAA cells on SH7305 A/D channel AN2 (ADDRC, 0xA4610084; the 10-bit
//  result is the register >> 6). The levels are Casio's own, read out of OS
//  02.01.7003 (2026-11-09):
//    - 0x8002A340 reads AN2 twice and averages;
//    - 0x800B0354 sorts the reading into four levels with one threshold set
//      per battery type (the "Battery Settings" choice, a RAM flag at
//      0x8C1BE9F8), and a level only ever gets worse until the next boot;
//    - 0x800813EA is a lower floor (347 / 402) checked at power-on and
//      before critical operations.
//  One count is close to 10 mV of pack voltage: an inference from the
//  thresholds (flag 0: 1.19 / 1.07 / 0.96 V per cell and a 0.87 V floor, the
//  alkaline curve; flag != 0: 1.16 / 1.12 / 1.07 V and 1.00 V, Ni-MH), not a
//  measurement of the divider; the driver keeps the scale in the registry.
//
//  Plain C, shared by the battery PDD (SRC\DRIVERS\BATTDRVR) and
//  bsp/tests/test_battery.c.
//------------------------------------------------------------------------------
#ifndef CLASSPAD_BATTERY_H
#define CLASSPAD_BATTERY_H

#define CLASSPAD_BATT_ALKALINE  0
#define CLASSPAD_BATT_NIMH      1

// Casio's levels: 1 best .. 4 low; 5 here = below the floor
#define CLASSPAD_BATT_LEVEL_FLOOR 5

typedef struct {
    unsigned short level1, level2, level3;  // raw > levelN: Casio level N; else 4
    unsigned short floor;                   // raw <= floor: do not run
    unsigned short full;                    // our own 100 % point (1.5 V / 1.3 V per cell)
} CLASSPAD_BATT_CURVE;

static const CLASSPAD_BATT_CURVE g_ClasspadBattCurves[2] = {
    { 475, 429, 383, 347, 600 },            // alkaline
    { 465, 447, 429, 402, 520 },            // Ni-MH
};

// Casio's level (1..4, 5 below the floor) of one averaged reading
static int ClasspadBattLevel(unsigned raw, int type)
{
    const CLASSPAD_BATT_CURVE *c = &g_ClasspadBattCurves[type == CLASSPAD_BATT_NIMH];
    if (raw > c->level1) return 1;
    if (raw > c->level2) return 2;
    if (raw > c->level3) return 3;
    if (raw > c->floor)  return 4;
    return CLASSPAD_BATT_LEVEL_FLOOR;
}

// The level shown: like Casio's, it only gets worse (a loaded battery
// recovers a little when the load goes away; the icon must not come back).
// *latched starts at 0.
static int ClasspadBattLatch(int *latched, int level)
{
    if (level > *latched) *latched = level;
    return *latched;
}

// 0..100: linear between the curve's points, floor = 0, level3 = 25,
// level2 = 50, level1 = 75, full = 100
static unsigned ClasspadBattPercent(unsigned raw, int type)
{
    const CLASSPAD_BATT_CURVE *c = &g_ClasspadBattCurves[type == CLASSPAD_BATT_NIMH];
    unsigned lo, hi, base;
    if (raw <= c->floor) return 0;
    if (raw >= c->full)  return 100;
    if (raw > c->level1)      { lo = c->level1; hi = c->full;   base = 75; }
    else if (raw > c->level2) { lo = c->level2; hi = c->level1; base = 50; }
    else if (raw > c->level3) { lo = c->level3; hi = c->level2; base = 25; }
    else                      { lo = c->floor;  hi = c->level3; base = 0; }
    return base + (raw - lo) * 25 / (hi - lo);
}

#endif
