/* RS is on the peripheral bus, LCD data on the external bus. Volatile
 * preserves compiler ordering; SYNCO completes writes across both buses.
 * Match the ClasspadDev/gint R61523 select/window transaction ordering. */
#ifndef DBT_LCD_IO_H
#define DBT_LCD_IO_H
static inline void dbt_lcd_sync(void)
{
    __asm__ volatile ("synco" ::: "memory");
}
static inline void dbt_lcd_select(volatile unsigned char *rs,
                                  volatile unsigned short *port, unsigned cmd)
{
    dbt_lcd_sync();                    /* finish preceding pixel/parameter writes */
    *rs = (unsigned char)(*rs & ~0x10u);
    dbt_lcd_sync();
    *port = (unsigned short)cmd;
    dbt_lcd_sync();
    *rs = (unsigned char)(*rs | 0x10u);
    dbt_lcd_sync();
}
static inline void dbt_lcd_param(volatile unsigned short *port, unsigned value)
{
    *port = (unsigned short)value;
    dbt_lcd_sync();
}
/* The fx-CP400 was built with two kinds of panel. Casio's OS tells them
 * apart by the ID read (command 0xDA, one dummy read, then the ID; OS
 * 02.01.7003 0x8006407A): 0x16 and 0x52 are the panels whose first visible
 * column is GRAM column 0; on every other ID LCD_SetDrawingBounds
 * (0x80038A10) adds 40 to both ends of the column window. Rows are the same.
 * Read once per translation unit that draws. */
#define DBT_LCD_OLD_PANEL_COL0 40u
static inline unsigned dbt_lcd_col0(volatile unsigned char *rs, volatile unsigned short *port)
{
    static unsigned col0 = ~0u;
    if (col0 == ~0u) {
        unsigned id;
        dbt_lcd_select(rs, port, 0xDA);
        id = *port;                         /* dummy */
        dbt_lcd_sync();
        id = *port;
        dbt_lcd_sync();
        col0 = (id == 0x16u || id == 0x52u) ? 0u : DBT_LCD_OLD_PANEL_COL0;
    }
    return col0;
}
#endif
