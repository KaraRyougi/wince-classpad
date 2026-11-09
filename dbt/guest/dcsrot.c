/* dcsrot.c - HC_LCD_PUSH's rotated form (classpad_lcdrot.h, docs/08
 * "rotation below GPE"): a landscape (DMDO_90) logical frame of 528x320
 * pixels, pushed as two bands of logical rows, which the translator turns
 * into the portrait panel. make GUEST=dcsrot screenshot -> dbt/build/dbt-dcsrot.png:
 * with logical (lx, ly) at physical (ly, 527 - lx), the logical quadrants
 * red (top left), green (top right), blue (bottom left), white (bottom
 * right) appear as physical bottom left red, top left green, bottom right
 * blue, top right white, a black 16-pixel marker near the logical top-left
 * corner at physical x 8..24, y 503..519, and a black frame all round. */
#include "hyper.h"
#include "mmio.h"
#include "../../bsp/classpad50/SRC/INC/classpad_lcdrot.h"

#define PORT 0xB4000000u
#define PRDR 0xA405013Cu
#define WP 320                          /* the panel */
#define HP 528
#define ROT CLASSPAD_ROT_90
#define LW HP                           /* logical: landscape */
#define LH WP

static unsigned short frame[LH][LW];

static void cmd(unsigned c)
{
    mmio_w8(PRDR, mmio_r8(PRDR) & ~0x10u);
    mmio_w16(PORT, (unsigned short)c);
    mmio_w8(PRDR, mmio_r8(PRDR) | 0x10u);
}
static void delay(void) { volatile unsigned i; for (i = 0; i < 200000; i++) ; }

static void push_rows(unsigned top, unsigned bottom)
{
    unsigned x0, x1, y0, y1;
    ClasspadRotWindow(ROT, WP, HP, top, bottom, &x0, &x1, &y0, &y1);
    hc_lcd_push(&frame[0][0], y0 | (y1 << 16), x0 | (x1 << 16), CLASSPAD_ROT_PUSH_TAG | ROT);
}

void guest_main(void)
{
    unsigned lx, ly;
    hc_puts("dcsrot test: landscape frame through HC_LCD_PUSH's rotated form\n");
    cmd(0x11); delay();                       /* SLPOUT */
    cmd(0x3A); mmio_w16(PORT, 0x55);          /* COLMOD 16 bpp */
    for (ly = 0; ly < LH; ly++)
        for (lx = 0; lx < LW; lx++) {
            unsigned short c = (ly < LH / 2) ? ((lx < LW / 2) ? 0xF800 : 0x07E0) : ((lx < LW / 2) ? 0x001F : 0xFFFF);
            if (lx < 2 || lx >= LW - 2 || ly < 2 || ly >= LH - 2) c = 0x0000;          /* frame */
            if (lx >= 8 && lx < 24 && ly >= 8 && ly < 24) c = 0x0000;                  /* marker */
            frame[ly][lx] = c;
        }
    push_rows(0, LH / 2);                     /* two bands: two physical column windows */
    push_rows(LH / 2, LH);
    cmd(0x29);                                /* DISPON */
    hc_puts("dcsrot: pushed\n");
    delay(); delay();
    hc_exit(0);
}
