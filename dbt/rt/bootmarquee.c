/* bootmarquee.c - keep the start screen's marquee moving while CE boots
 * (DEVICE=1 builds, -DDBT_BOOT_MARQUEE; geometry in rt/bootmarquee.h).
 *
 * The loader (wince.hh3) leaves the start screen on the panel and hands
 * over; CE then takes minutes under the translator before its display
 * driver draws anything. On every system tick the translator delivers
 * (TMU0, rt.c "deliver:") this looks at the RTC's 128 Hz counter and, every
 * BM_FRAME_TICKS, redraws the strip inside the trough with the three blocks
 * one slot further: one CASET/PASET/RAMWR window and 8 rows of pixels, the
 * DCS sequence rt/lcdcon.c uses. It stops for good when the display
 * driver's first line "CLASSPAD DDI: " goes through dbt_putc (before the
 * driver touches the panel: a window of ours between two of its pixel
 * writes would move its cursor) and when the translator halts. */
#ifdef DBT_BOOT_MARQUEE
#include "rt.h"
#include "lcd_io.h"
#include "bootmarquee.h"

#define PORT   (*(volatile sf_u16 *)0xB4000000u)
#define PRDR   (*(volatile sf_u8 *)0xA405013Cu)
#define R64CNT (*(volatile sf_u8 *)0xA413FEC0u)     /* RTC 128 Hz counter, 7 bits */
#define RGB565(h) ((sf_u16)((((h) >> 19) & 0x1F) << 11 | (((h) >> 10) & 0x3F) << 5 | (((h) >> 3) & 0x1F)))

static int g_off;                      /* 1: the display driver has the panel (or we halted) */
static int g_match;                    /* bytes of the hand-over string matched so far */
static int g_pos;                      /* 0 .. BM_POSITIONS-1 */
static unsigned g_last, g_acc;
static int g_started;
static const char g_handover[] = "CLASSPAD DDI: ";

static void cmd(unsigned c) { dbt_lcd_select(&PRDR, &PORT, c); }
static void dat(unsigned v) { dbt_lcd_param(&PORT, v); }

static void draw(int pos)
{
    static const unsigned rows[BM_CHUNK_H] = BM_BLUE_ROWS;
    const unsigned x0 = BM_X + 2, x1 = BM_X + BM_W - 3, y0 = BM_Y0, y1 = BM_Y0 + BM_CHUNK_H - 1;
    unsigned x, r;
    cmd(0x2A); dat(x0 >> 8); dat(x0 & 0xFF); dat(x1 >> 8); dat(x1 & 0xFF);     /* CASET */
    cmd(0x2B); dat(y0 >> 8); dat(y0 & 0xFF); dat(y1 >> 8); dat(y1 & 0xFF);     /* PASET */
    cmd(0x2C);                                                                 /* RAMWR */
    for (r = 0; r < BM_CHUNK_H; r++) {
        sf_u16 c = RGB565(rows[r]);
        for (x = x0; x <= x1; x++) {
            int rel = (int)x - BM_X0, s = rel / BM_STEP;
            int lit = rel >= 0 && rel % BM_STEP < BM_CHUNK_W && s < BM_SLOTS && BM_LIT(s, pos - 3);
            PORT = lit ? c : 0x0000u;
        }
    }
    dbt_lcd_sync();
}

void bootmarquee_tick(void)
{
    unsigned now, d;
    if (g_off) return;
    now = R64CNT & 0x7Fu;
    if (!g_started) { g_started = 1; g_last = now; return; }
    d = (now - g_last) & 0x7Fu;
    g_last = now;
    g_acc += d;
    if (g_acc < BM_FRAME_TICKS) return;
    g_acc = 0;
    g_pos = (g_pos + 1) % BM_POSITIONS;
    draw(g_pos);
}

void bootmarquee_putc(int c)
{
    if (g_off) return;
    if (c == g_handover[g_match]) {
        if (!g_handover[++g_match]) g_off = 1;
    } else {
        g_match = c == g_handover[0];
    }
}

void bootmarquee_stop(void) { g_off = 1; }
#endif
