/* Native, no-Flash diagnostic: isolate CE's display takeover commands.
 * Casio OS remains installed; Clear exits at every step. No CE/translator. */
#include "os.h"
#include "../rt/lcd_io.h"
#define LCD ((volatile u16 *)0xB4000000u)
#define RS ((volatile u8 *)0xA405013Cu)
static u32 lock(void)
{
    u32 sr;
    __asm__ volatile ("stc sr,%0" : "=r"(sr));
    __asm__ volatile ("ldc %0,sr" :: "r"(sr | 0xf0u) : "memory");
    return sr;
}
static void unlock(u32 sr) { __asm__ volatile ("ldc %0,sr" :: "r"(sr) : "memory"); }
static void command(unsigned c)
{
    u32 sr = lock(); dbt_lcd_select(RS, LCD, c); unlock(sr);
}
static void panel_delay(void)
{
    volatile unsigned i;
    for (i = 0; i < 12000000u; ++i) __asm__ volatile ("nop");
}
static int page(unsigned step, const char *next)
{
    ui_clear();
    ui_printf(1, "LCD takeover probe: step %u OK", step);
    ui_printf(3, "No CE, no Flash writes");
    ui_printf(5, "Next: %s", next);
    ui_printf(7, "EXE: next    Clear: exit");
    ui_printf(9, "If black, note LAST visible step.");
    ui_refresh();
    return ui_wait_exe_or_clear();
}
int hhk_main(int argc, char **argv, char **envp)
{
    u32 sr;
    unsigned i;
    if (!os_init(argc, argv, envp)) return 1;
    if (!page(0, "native window + color strip")) return 0;
    /* Same ordered native command/parameter path as HC_LCD_PUSH. Keep
     * existing panel mode; a small lower-screen strip preserves prompts. */
    sr = lock();
    dbt_lcd_select(RS, LCD, 0x2a);
    dbt_lcd_param(LCD, 0); dbt_lcd_param(LCD, 0);
    dbt_lcd_param(LCD, 1); dbt_lcd_param(LCD, 63);
    dbt_lcd_select(RS, LCD, 0x2b);
    dbt_lcd_param(LCD, 1); dbt_lcd_param(LCD, 144);
    dbt_lcd_param(LCD, 1); dbt_lcd_param(LCD, 175);
    dbt_lcd_select(RS, LCD, 0x2c);
    for (i = 0; i < 320u * 32; ++i) *LCD = (i % 320 < 160) ? 0xf800 : 0x07e0;
    dbt_lcd_sync(); unlock(sr);
    /* Do not redraw through the OS before the strip has been inspected. */
    if (!ui_wait_exe_or_clear()) return 0;
    if (!page(1, "SLPOUT (0x11)")) return 0;
    command(0x11); panel_delay();
    if (!page(2, "COLMOD (0x3a, 0x55)")) return 0;
    sr = lock(); dbt_lcd_select(RS, LCD, 0x3a); dbt_lcd_param(LCD, 0x55); unlock(sr);
    if (!page(3, "DISPON (0x29)")) return 0;
    command(0x29);
    page(4, "finished; Clear exits");
    return 0;
}
