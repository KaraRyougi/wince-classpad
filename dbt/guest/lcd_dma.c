/* lcd_dma.c - CPU/DMA equivalence, full strips, odd tail and short fallback.
 * Based on dcs.c - the fx-CP400 panel through the translator's push hypercall
 * (HC_LCD_PUSH, docs/04): wake the DCS controller the way the display driver
 * does (pre-swapped 16-bit stores, RS on Port R bit 4), draw a picture into
 * a RAM frame and push it in two parts, the second with its own window.
 * make GUEST=dcs screenshot (fx-cp400 profile) -> dbt/build/dbt-dcs.png:
 * colour bars over a grey field with a white frame, the top 96 rows black
 * (what the panel console would keep). */
#include "hyper.h"
#include "mmio.h"

#define PORT 0xB4000000u
#define PRDR 0xA405013Cu
#define W 320
#define H 528

static unsigned short frame[H][W];

static void cmd(unsigned c)
{
    mmio_w8(PRDR, mmio_r8(PRDR) & ~0x10u);
    mmio_w16(PORT, (unsigned short)c);
    mmio_w8(PRDR, mmio_r8(PRDR) | 0x10u);
}
static void delay(void) { volatile unsigned i; for (i = 0; i < 200000; i++) ; }

void guest_main(void)
{
    int x, y;
    hc_puts("dcs test: frame pushed through HC_LCD_PUSH\n");
    cmd(0x11); delay();                       /* SLPOUT */
    cmd(0x3A); mmio_w16(PORT, 0x55);          /* COLMOD 16 bpp */
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            unsigned short c = 0x630C;                      /* grey */
            if (y < 96) c = 0x0000;                         /* the console's rows */
            else if (y < 200) c = (x < 80) ? 0xF800 : (x < 160) ? 0x07E0 : (x < 240) ? 0x001F : 0xFFFF;
            if (x == 0 || x == W - 1 || y == 96 || y == H - 1) c = 0xFFFF;
            frame[y][x] = c;
        }
    hc_lcd_push(&frame[96][0], 96, 200 - 96, W);           /* the bars */
    hc_lcd_push(&frame[200][0], 200, H - 200, W);          /* the rest, its own window */
    /* Width 17 x 3 = 51 pixels tests three 32-byte DMA blocks plus
     * three CPU tail pixels. A seven-pixel update uses the CPU fallback. */
    hc_lcd_push(&frame[100][0], 220, 3, 17);
    hc_lcd_push(&frame[100][0], 224, 1, 7);
    if (hc_getstat(400)) {
        unsigned strips = hc_getstat(401);
        hc_puts("DMA strips: "); hc_puthex(strips); hc_puts("\n");
        if (strips != 36) { hc_puts("FAIL: DMA strip count\n"); hc_exit(1); }
    }
    cmd(0x29);                                /* DISPON */
    hc_puts("RESULT: PASS\n");
    hc_exit(0);
}
