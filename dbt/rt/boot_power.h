/* Establish the power state required by the guest before any native SLEEP.
 * SH7305 STBCR mode bits and MSTPCR0 gates match gint/mpu/power.h.
 * Do not touch cache, TLB, RAM, clock divisors or the standby resume address.
 * Pointer arguments let the host regression verify preservation of other bits. */
#ifndef DBT_BOOT_POWER_H
#define DBT_BOOT_POWER_H
static inline void dbt_boot_power(volatile unsigned *stbcr,
                                 volatile unsigned *mstp0)
{
    *stbcr &= ~0xB0u;       /* ordinary SLEEP, not standby/R-standby/U-standby */
    *mstp0 &= ~0x8060u;     /* TMU0 (three channels), KEYSC and RTC clocks */
}
#endif
