//------------------------------------------------------------------------------
//  classpad_lcdrot.h - screen rotation below GPE (docs/08)
//
//  The display driver keeps an unrotated logical frame buffer (logical width
//  x height, stride = logical width * 2 bytes) and the push to the panel
//  turns it: the panel is wp x hp pixels, portrait. GWES and GPE never see a
//  rotated surface, so every blit takes the same linear paths as in
//  portrait. Shared by the driver (classpad_ddi.cpp), the translator's
//  HC_LCD_PUSH rotated form (dbt/rt/rt.c) and bsp/tests/test_lcdrot.c.
//
//  Angles are CE's DMDO_* values: 0, 1 (90), 2 (180), 4 (270). The mapping
//  is the one GPE's rotated surfaces used (GPESurf::GetPtr, gpe.cpp), so
//  GWES's rotation of touch input and the earlier implementation agree with
//  it. Logical (lx, ly) shows at physical (px, py):
//     90: (px, py) = (ly, hp - 1 - lx)
//    180: (px, py) = (wp - 1 - lx, hp - 1 - ly)
//    270: (px, py) = (wp - 1 - ly, lx)
//------------------------------------------------------------------------------
#ifndef CLASSPAD_LCDROT_H
#define CLASSPAD_LCDROT_H

#ifdef _MSC_VER
#define CLASSPAD_ROT_INLINE static __inline
#else
#define CLASSPAD_ROT_INLINE static inline
#endif

#define CLASSPAD_ROT_0      0
#define CLASSPAD_ROT_90     1
#define CLASSPAD_ROT_180    2
#define CLASSPAD_ROT_270    4

// HC_LCD_PUSH rotated form (dbt/rt/rt.c): r4 = the logical buffer (P1/P2
// address of its first byte), r5 = physical rows py0 | py1 << 16, r6 =
// physical columns px0 | px1 << 16 (both end-exclusive), r7 = this tag | angle.
#define CLASSPAD_ROT_PUSH_TAG   0xA5000000u
#define CLASSPAD_ROT_PUSH_MASK  0xFF000000u

CLASSPAD_ROT_INLINE unsigned ClasspadRotLogicalWidth(unsigned rot, unsigned wp, unsigned hp)
{
    return (rot == CLASSPAD_ROT_90 || rot == CLASSPAD_ROT_270) ? hp : wp;
}

CLASSPAD_ROT_INLINE unsigned ClasspadRotLogicalHeight(unsigned rot, unsigned wp, unsigned hp)
{
    return (rot == CLASSPAD_ROT_90 || rot == CLASSPAD_ROT_270) ? wp : hp;
}

// The physical window [x0, x1) x [y0, y1) that shows the logical rows
// [top, bottom): for 90 and 270 the rows are physical columns.
CLASSPAD_ROT_INLINE void ClasspadRotWindow(unsigned rot, unsigned wp, unsigned hp, unsigned top, unsigned bottom,
                                           unsigned *x0, unsigned *x1, unsigned *y0, unsigned *y1)
{
    switch (rot) {
    case CLASSPAD_ROT_90:  *x0 = top;         *x1 = bottom;      *y0 = 0;           *y1 = hp;       break;
    case CLASSPAD_ROT_180: *x0 = 0;           *x1 = wp;          *y0 = hp - bottom; *y1 = hp - top; break;
    case CLASSPAD_ROT_270: *x0 = wp - bottom; *x1 = wp - top;    *y0 = 0;           *y1 = hp;       break;
    default:               *x0 = 0;           *x1 = wp;          *y0 = top;         *y1 = bottom;   break;
    }
}

// Byte offset in the logical buffer of the pixel shown at physical (px, py).
CLASSPAD_ROT_INLINE unsigned ClasspadRotOffset(unsigned rot, unsigned wp, unsigned hp, unsigned px, unsigned py)
{
    unsigned lx, ly, stride = ClasspadRotLogicalWidth(rot, wp, hp) * 2;
    switch (rot) {
    case CLASSPAD_ROT_90:  lx = hp - 1 - py; ly = px;          break;
    case CLASSPAD_ROT_180: lx = wp - 1 - px; ly = hp - 1 - py; break;
    case CLASSPAD_ROT_270: lx = py;          ly = wp - 1 - px; break;
    default:               lx = px;          ly = py;          break;
    }
    return ly * stride + lx * 2;
}

#endif
