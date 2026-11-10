//------------------------------------------------------------------------------
//  classpad_bkl.h - backlight level (docs/08, 2026-11-10)
//
//  Casio's OS has 14 levels, 0..13 (its brightness routine 0x80038280; an
//  out-of-range level counts as 7, which is also what its power-on sets).
//  The value goes to the panel's brightness register: command 0x51 on the
//  newer panel, the second parameter of register 0xB9 on the older one
//  (classpad_lcd.h LcdSetBacklight). Level 0 is special in the OS (a port
//  pin and two more registers change) and is not offered here.
//  The OS's highest value is 0x67 of 0xFF, and on the calculator that is
//  far from the panel's brightest (2026-11-10: CE ran at 0xFF before, and
//  the user finds level 13 dim), so seven more levels follow, up to 0xFF:
//  levels 1..13 are the OS's, 14..20 are beyond what the OS ever sets.
//
//  The display driver takes two escapes (ExtEscape on the screen DC):
//      DRVESC_CLASSPAD_GETBRIGHTNESS   out: one int, the level 1..20
//      DRVESC_CLASSPAD_SETBRIGHTNESS   in:  one int, the level 1..20
//  classpad_bkl.dll (SRC\APPS\BKLCPL) is the "Advanced..." button of the
//  Backlight page of Display Properties (HKCU\ControlPanel\BackLight
//  "AdvancedCPL", cplmain's screen.cpp).
//------------------------------------------------------------------------------
#ifndef __CLASSPAD_BKL_H
#define __CLASSPAD_BKL_H

#define DRVESC_CLASSPAD_GETBRIGHTNESS   100500
#define DRVESC_CLASSPAD_SETBRIGHTNESS   100501

#define CLASSPAD_BKL_LEVELS     21
#define CLASSPAD_BKL_MIN        1
#define CLASSPAD_BKL_MAX        20
#define CLASSPAD_BKL_OS_MAX     13          // the OS's own highest level
#define CLASSPAD_BKL_DEFAULT    7

static const unsigned char g_classpadBklValue[CLASSPAD_BKL_LEVELS] = {
    0x05, 0x0F, 0x14, 0x1A, 0x1F, 0x26, 0x2D, 0x34, 0x3B, 0x42, 0x49, 0x52, 0x5B, 0x67,     // the OS's 0..13
    0x78, 0x8C, 0xA2, 0xB9, 0xD0, 0xE8, 0xFF                                                // beyond the OS
};

// The level whose value is nearest to a register value
static __inline int ClasspadBklLevelOf(unsigned value)
{
    int i, best = CLASSPAD_BKL_MIN;
    unsigned bestDiff = 0xFFFF;
    for (i = CLASSPAD_BKL_MIN; i <= CLASSPAD_BKL_MAX; i++) {
        unsigned d = value > g_classpadBklValue[i] ? value - g_classpadBklValue[i] : g_classpadBklValue[i] - value;
        if (d < bestDiff) { bestDiff = d; best = i; }
    }
    return best;
}

#endif
