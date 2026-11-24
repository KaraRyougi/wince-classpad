/* test_lcdrot.c - the rotation mapping of classpad_lcdrot.h against GPE's
 * convention (GPESurf::GetPtr in gpe.cpp, the one the earlier rotated-surface
 * driver used and GWES's touch rotation agrees with), and the physical window
 * of a logical row range against a brute-force search. */
#include <stdio.h>
#include <stdlib.h>
#include "../classpad50/SRC/INC/classpad_lcdrot.h"

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

/* gpe.cpp GPESurf::GetPtr for a 16 bpp surface after SetRotation(logical
 * width, logical height, rot): m_ScreenHeight is the logical width and
 * m_ScreenWidth the logical height for 90/270 (both the physical size the
 * other way round), so with the panel wp x hp:
 *    90: row = hp - 1 - x, column = y
 *   180: row = hp - 1 - y, column = wp - 1 - x
 *   270: row = x,          column = wp - 1 - y */
static void gpe_physical(unsigned rot, unsigned wp, unsigned hp, unsigned x, unsigned y, unsigned *px, unsigned *py)
{
    switch (rot) {
    case 1:  *py = hp - 1 - x; *px = y;          break;
    case 2:  *py = hp - 1 - y; *px = wp - 1 - x; break;
    case 4:  *py = x;          *px = wp - 1 - y; break;
    default: *py = y;          *px = x;          break;
    }
}

static void check_panel(unsigned wp, unsigned hp)
{
    static const unsigned rots[] = { 0, 1, 2, 4 };
    unsigned r, lx, ly, px, py;
    for (r = 0; r < 4; r++) {
        unsigned rot = rots[r], lw = ClasspadRotLogicalWidth(rot, wp, hp), lh = ClasspadRotLogicalHeight(rot, wp, hp);
        unsigned char *seen = calloc(wp * hp, 1);
        CHECK(lw * lh == wp * hp, "rot %u: logical %ux%u", rot, lw, lh);
        /* every logical pixel lands where GPE put it, and the physical pixel reads it back */
        for (ly = 0; ly < lh; ly++)
            for (lx = 0; lx < lw; lx++) {
                unsigned off;
                gpe_physical(rot, wp, hp, lx, ly, &px, &py);
                CHECK(px < wp && py < hp, "rot %u: logical %u,%u maps outside the panel (%u,%u)", rot, lx, ly, px, py);
                off = ClasspadRotOffset(rot, wp, hp, px, py);
                CHECK(off == ly * lw * 2 + lx * 2, "rot %u: physical %u,%u reads offset %u, logical %u,%u expects %u", rot, px, py, off, lx, ly, ly * lw * 2 + lx * 2);
                if (px < wp && py < hp) seen[py * wp + px]++;
            }
        for (py = 0; py < hp; py++)
            for (px = 0; px < wp; px++)
                CHECK(seen[py * wp + px] == 1, "rot %u: physical %u,%u covered %u times", rot, px, py, seen[py * wp + px]);
        /* the window of a row range is exactly the physical pixels whose source row is in the range */
        {
            unsigned ranges[][2] = { { 0, 1 }, { 0, lh }, { 3, 7 }, { lh - 1, lh }, { lh / 2, lh / 2 + 5 } };
            unsigned i;
            for (i = 0; i < sizeof ranges / sizeof ranges[0]; i++) {
                unsigned top = ranges[i][0], bottom = ranges[i][1] > lh ? lh : ranges[i][1], x0, x1, y0, y1, n = 0;
                if (top >= bottom) continue;
                ClasspadRotWindow(rot, wp, hp, top, bottom, &x0, &x1, &y0, &y1);
                CHECK(x0 < x1 && x1 <= wp && y0 < y1 && y1 <= hp, "rot %u rows %u..%u: window %u..%u x %u..%u", rot, top, bottom, x0, x1, y0, y1);
                for (py = 0; py < hp; py++)
                    for (px = 0; px < wp; px++) {
                        unsigned src_row = ClasspadRotOffset(rot, wp, hp, px, py) / (lw * 2);
                        int in_range = src_row >= top && src_row < bottom;
                        int in_window = px >= x0 && px < x1 && py >= y0 && py < y1;
                        CHECK(in_range == in_window, "rot %u rows %u..%u: physical %u,%u (source row %u) in window %d", rot, top, bottom, px, py, src_row, in_window);
                        n += in_window;
                    }
                CHECK(n == (bottom - top) * lw, "rot %u rows %u..%u: window has %u pixels, rows hold %u", rot, top, bottom, n, (bottom - top) * lw);
            }
        }
        /* 2D dirty rectangle mapping: physical window covers exactly the pixels whose logical (lx, ly) lies in [left, right) x [top, bottom) */
        {
            unsigned rects[][4] = {
                { 0, 0, lw, lh },
                { 0, 0, 1, 1 },
                { lw - 1, lh - 1, lw, lh },
                { 1, 2, lw > 4 ? 4 : lw, lh > 5 ? 5 : lh },
                { lw / 3, lh / 4, lw * 2 / 3 + 1, lh * 3 / 4 + 1 }
            };
            unsigned i;
            for (i = 0; i < sizeof rects / sizeof rects[0]; i++) {
                unsigned l = rects[i][0], t = rects[i][1], rr = rects[i][2], b = rects[i][3];
                unsigned x0, x1, y0, y1, n = 0;
                if (rr > lw) rr = lw;
                if (b > lh) b = lh;
                if (l >= rr || t >= b) continue;
                ClasspadRotRect(rot, wp, hp, l, t, rr, b, &x0, &x1, &y0, &y1);
                CHECK(x0 < x1 && x1 <= wp && y0 < y1 && y1 <= hp, "rot %u rect %u,%u..%u,%u: window %u..%u x %u..%u", rot, l, t, rr, b, x0, x1, y0, y1);
                for (py = 0; py < hp; py++)
                    for (px = 0; px < wp; px++) {
                        unsigned off = ClasspadRotOffset(rot, wp, hp, px, py) / 2;
                        unsigned src_row = off / lw, src_col = off % lw;
                        int in_rect = src_col >= l && src_col < rr && src_row >= t && src_row < b;
                        int in_window = px >= x0 && px < x1 && py >= y0 && py < y1;
                        CHECK(in_rect == in_window, "rot %u rect %u,%u..%u,%u: physical %u,%u (logical %u,%u) in window %d", rot, l, t, rr, b, px, py, src_col, src_row, in_window);
                        n += in_window;
                    }
                CHECK(n == (rr - l) * (b - t), "rot %u rect %u,%u..%u,%u: window has %u pixels, rect holds %u", rot, l, t, rr, b, n, (rr - l) * (b - t));
            }
        }
        free(seen);
    }
}

int main(void)
{
    check_panel(320, 528);      /* the fx-CP400 */
    check_panel(7, 5);          /* a small odd one */
    printf("lcdrot: %d failures\n", fails);
    return fails != 0;
}
