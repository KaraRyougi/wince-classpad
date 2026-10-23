/* lcd.c - guest that draws into a little-endian RGB565 VRAM and pushes it to
 * the fx-CG R61524 display controller through pre-swapped MMIO (16-bit data
 * bus at 0xB4000000, register/data select on Port R bit 4). The picture:
 * a colour gradient, a checkerboard, a sine wave and a scaled-up banner. */
#include "hyper.h"
#include "mmio.h"

#define LCD_W   384
#define LCD_H   216
#define DISPLAY 0xB4000000u             /* 16-bit */
#define PRDR    0xA405013Cu             /* 8-bit, bit 4 = RS */
#define TCNT1   0xA4490018u

#define RGB(r, g, b) ((unsigned short)((((r) & 0x1F) << 11) | (((g) & 0x3F) << 5) | ((b) & 0x1F)))

static unsigned short vram[LCD_H][LCD_W];      /* little-endian halfwords in guest memory */

static void lcd_select(unsigned short reg)
{
    mmio_w8(PRDR, mmio_r8(PRDR) & (unsigned char)~0x10);
    mmio_w16(DISPLAY, reg);
    mmio_w8(PRDR, mmio_r8(PRDR) | 0x10);
}
static void lcd_set(unsigned short reg, unsigned short v) { lcd_select(reg); mmio_w16(DISPLAY, v); }

/* window = the 384x216 visible area (panel columns 6..389), then stream VRAM */
static void lcd_push(void)
{
    unsigned y, x;
    lcd_set(0x210, 395 - 389);                 /* HSTART (mirrored column) */
    lcd_set(0x211, 395 - 6);                   /* HEND */
    lcd_set(0x212, 0);                         /* VSTART */
    lcd_set(0x213, LCD_H - 1);                 /* VEND */
    lcd_set(0x200, 0);                         /* HADDR */
    lcd_set(0x201, 0);                         /* VADDR */
    lcd_select(0x202);                         /* data */
    for (y = 0; y < LCD_H; y++)
        for (x = 0; x < LCD_W; x++)
            mmio_w16(DISPLAY, vram[y][x]);
}

/* 5x7 glyphs for the banner, one byte per row, MSB = left */
static const unsigned char font_H[7] = { 0x88, 0x88, 0x88, 0xF8, 0x88, 0x88, 0x88 };
static const unsigned char font_E[7] = { 0xF8, 0x80, 0x80, 0xF0, 0x80, 0x80, 0xF8 };
static const unsigned char font_L[7] = { 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0xF8 };
static const unsigned char font_O[7] = { 0x70, 0x88, 0x88, 0x88, 0x88, 0x88, 0x70 };
static const unsigned char font_C[7] = { 0x70, 0x88, 0x80, 0x80, 0x80, 0x88, 0x70 };
static const unsigned char font_W[7] = { 0x88, 0x88, 0x88, 0xA8, 0xA8, 0xD8, 0x88 };
static const unsigned char font_I[7] = { 0xF8, 0x20, 0x20, 0x20, 0x20, 0x20, 0xF8 };
static const unsigned char font_N[7] = { 0x88, 0xC8, 0xA8, 0x98, 0x88, 0x88, 0x88 };
static const unsigned char font_D[7] = { 0xF0, 0x88, 0x88, 0x88, 0x88, 0x88, 0xF0 };
static const unsigned char font_B[7] = { 0xF0, 0x88, 0x88, 0xF0, 0x88, 0x88, 0xF0 };
static const unsigned char font_T[7] = { 0xF8, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20 };

static const unsigned char *glyph(char c)
{
    switch (c) {
    case 'H': return font_H; case 'E': return font_E; case 'L': return font_L; case 'O': return font_O;
    case 'C': return font_C; case 'W': return font_W; case 'I': return font_I; case 'N': return font_N;
    case 'D': return font_D; case 'B': return font_B; case 'T': return font_T;
    default: return 0;
    }
}

static void draw_text(const char *s, unsigned x0, unsigned y0, unsigned scale, unsigned short color)
{
    for (; *s; s++, x0 += 6 * scale) {
        const unsigned char *g = glyph(*s);
        unsigned r, c, dy, dx;
        if (!g) continue;
        for (r = 0; r < 7; r++)
            for (c = 0; c < 5; c++)
                if (g[r] & (0x80 >> c))
                    for (dy = 0; dy < scale; dy++)
                        for (dx = 0; dx < scale; dx++)
                            vram[y0 + r * scale + dy][x0 + c * scale + dx] = color;
    }
}

/* 64-entry quarter-free sine table (0..255), avoids any float */
static const unsigned char sintab[64] = {
    128,140,153,165,177,188,199,209,218,226,234,240,245,250,253,255,255,255,253,250,245,240,234,226,218,209,199,188,177,165,153,140,
    128,116,103, 91, 79, 68, 57, 47, 38, 30, 22, 16, 11,  6,  3,  1,  0,  1,  3,  6, 11, 16, 22, 30, 38, 47, 57, 68, 79, 91,103,116 };

static void render(unsigned frame)
{
    unsigned x, y;
    /* gradient: red across, blue down, green from the frame counter */
    for (y = 0; y < LCD_H; y++)
        for (x = 0; x < LCD_W; x++)
            vram[y][x] = RGB(x >> 4, (frame * 2) & 0x3F, y >> 3);
    /* checkerboard in the top-right corner */
    for (y = 8; y < 72; y++)
        for (x = LCD_W - 72; x < LCD_W - 8; x++)
            vram[y][x] = (((x >> 3) ^ (y >> 3)) & 1) ? 0xFFFF : 0x0000;
    /* sine wave, 3 pixels thick */
    for (x = 0; x < LCD_W; x++) {
        unsigned s = sintab[((x >> 1) + frame) & 63];
        unsigned yy = 150 + (s * 48) / 255;
        vram[yy][x] = vram[yy + 1][x] = vram[yy + 2][x] = RGB(31, 63, 0);
    }
    /* frame: white border */
    for (x = 0; x < LCD_W; x++) vram[0][x] = vram[LCD_H - 1][x] = 0xFFFF;
    for (y = 0; y < LCD_H; y++) vram[y][0] = vram[y][LCD_W - 1] = 0xFFFF;
    draw_text("HELLO WINCE", 24, 40, 4, 0xFFFF);
    draw_text("LE ON BE DBT", 24, 84, 3, RGB(31, 63, 31));
}

void guest_main(void)
{
    unsigned frame, t0, t1;
    hc_puts("lcd test: drawing into VRAM and pushing to the R61524 through pre-swapped MMIO\n");
    __asm__ volatile ("ldc %0, sr" :: "r"(0x400010F0u));
    hc_bench(0xFFFFFFFFu, 0);                                /* start TMU1 for timing */
    for (frame = 0; frame < 24; frame++) {
        render(frame);
        t0 = mmio_r32(TCNT1);
        lcd_push();
        t1 = mmio_r32(TCNT1);
        if (frame == 0 || frame == 23) {
            hc_puts("frame "); hc_puthex(frame); hc_puts(": push "); hc_puthex(t0 - t1); hc_puts(" TMU1 ticks\n");
        }
    }
    hc_puts("sample pixel vram[100][100] = "); hc_puthex(vram[100][100]); hc_putc('\n');
    hc_stats();
    hc_puts("RESULT: PASS\n");
    hc_exit(0);
}
