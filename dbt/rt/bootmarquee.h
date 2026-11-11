/* bootmarquee.h - the start screen's marquee, shared by the loader that
 * draws the start screen (dbt/hhk/ui.c, a gint program) and the translator
 * that keeps the three blue blocks moving after the hand-over (rt/bootmarquee.c,
 * DEVICE=1 builds), until the CE display driver takes the panel.
 *
 * Geometry on the 320x528 panel (docs/design/loader-luna.html drawBoot): a
 * BM_W x BM_H trough at (BM_X, BM_Y), slots of BM_CHUNK_W px every BM_STEP px
 * from (BM_X0, BM_Y0), three lit at a time sliding from the left edge out of
 * the right one: position p lights slots p, p+1, p+2 for p = -3 .. BM_SLOTS-1
 * (BM_POSITIONS in all), one step every BM_FRAME_TICKS of the RTC's 128 Hz. */
#ifndef DBT_BOOTMARQUEE_H
#define DBT_BOOTMARQUEE_H

#define BM_X            (320 / 2 - 46)
#define BM_Y            430
#define BM_W            92
#define BM_H            14
#define BM_CHUNK_W      6
#define BM_CHUNK_H      8
#define BM_STEP         8
#define BM_X0           (BM_X + 4)
#define BM_Y0           (BM_Y + 3)
#define BM_SLOTS        ((BM_W - 6 + 2) / BM_STEP)
#define BM_POSITIONS    (BM_SLOTS + 3)
#define BM_FRAME_TICKS  12                      /* about 94 ms */

/* the blue chunk's rows, top to bottom (24-bit RGB) */
#define BM_BLUE_ROWS { 0xB9CEFF, 0x86A6F6, 0x5A82F0, 0x3762E2, 0x2F5CE0, 0x4A71E6, 0x7C9BEE, 0xAFC3F6 }

/* is slot s lit at position p (p = index - 3)? */
#define BM_LIT(s, p)    ((s) >= (p) && (s) < (p) + 3)
#endif
