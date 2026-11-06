/* lcdcon.h - the translator's log on the fx-CP400 panel (DEVICE=1 builds:
 * -DDBT_LCD_CONSOLE). dbt_putc hands every byte to lcdcon_putc, which draws
 * it in the top rows of the DCS panel until the display driver takes the
 * panel over (its "CLASSPAD DDI:" sign-on in the log); dbt_halt calls
 * lcdcon_halt, which takes the panel back, redraws the last lines, the halt
 * reason and the last exception context. Nothing here is compiled without
 * the define, so the emulator path is untouched. */
#ifndef DBT_LCDCON_H
#define DBT_LCDCON_H
#ifdef DBT_LCD_CONSOLE
void lcdcon_putc(int c);
void lcdcon_halt(const char *why);
#endif
#endif
