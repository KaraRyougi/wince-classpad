/* irqpins.c - IRQ pin interrupts on the device (DEVICE=1: -DDBT_IRQPIN_GUARD).
 *
 * INTEVT 0x600..0x6E0 are the SH7305's IRQ0-7 pins, gated by INTMSK00
 * (0xA4140044, write 1 masks; INTMSKCLR00 0xA4140064 unmasks). The OAL
 * knows nothing about them, so a pin Casio's OS left enabled (make
 * IRQPINS=keep) that the kernel never acknowledges would be delivered for
 * ever. The first delivery of each pin is logged (the panel console shows
 * it during the boot), and a pin delivered 2000 times is masked with a
 * second line: that is the pin to look at in the OAL (bsp intr.c). */
#include "rt.h"
#include "irqpins.h"
#ifdef DBT_IRQPIN_GUARD
#define INTMSK00 (*(volatile sf_u8 *)0xA4140044u)
#define STORM    2000u
static sf_u32 g_count[8];
static sf_u8  g_seen, g_masked;

void dbt_irqpin_guard(sf_u32 intevt)
{
    sf_u32 pin;
    if (intevt < 0x600u || intevt > 0x6E0u || (intevt & 0x1Fu)) return;
    pin = (intevt - 0x600u) >> 5;
    if (!(g_seen & (1u << pin))) {
        g_seen |= (sf_u8)(1u << pin);
        dbt_puts("[dbt] IRQ pin "); dbt_putc((char)('0' + pin)); dbt_puts(" (INTEVT "); dbt_puthex(intevt);
        dbt_puts(") delivered, INTMSK00="); dbt_puthex(INTMSK00); dbt_puts("\n");
    }
    if (++g_count[pin] == STORM && !(g_masked & (1u << pin))) {
        g_masked |= (sf_u8)(1u << pin);
        INTMSK00 = (sf_u8)(1u << pin);
        dbt_puts("[dbt] IRQ pin "); dbt_putc((char)('0' + pin)); dbt_puts(" stormed: masked\n");
    }
}
#else
typedef int irqpins_not_built;
#endif
