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
#endif
