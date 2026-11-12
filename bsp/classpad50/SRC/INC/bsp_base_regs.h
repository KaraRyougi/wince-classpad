//------------------------------------------------------------------------------
//  bsp_base_regs.h - SH7305 on-chip module addresses used by the OAL
//
//  All of these are P4/area-7 physical addresses used through the uncached
//  P2 alias (OALPAtoUA). Every 16/32-bit access must go through the
//  pre-swapped accessors in classpad_mmio.h (the translator byte-swaps all
//  16/32-bit loads and stores; see docs/04 in the wince-classpad repo).
//------------------------------------------------------------------------------
#ifndef __BSP_BASE_REGS_H
#define __BSP_BASE_REGS_H

// CCN (MMU/cache control), same layout as the SH7750
#define SH7305_REG_PA_CCN           0xFF000000
#define CCN_PTEH                    (SH7305_REG_PA_CCN + 0x00)
#define CCN_PTEL                    (SH7305_REG_PA_CCN + 0x04)
#define CCN_TTB                     (SH7305_REG_PA_CCN + 0x08)
#define CCN_TEA                     (SH7305_REG_PA_CCN + 0x0C)
#define CCN_MMUCR                   (SH7305_REG_PA_CCN + 0x10)
#define CCN_CCR                     (SH7305_REG_PA_CCN + 0x1C)
#define CCN_TRA                     (SH7305_REG_PA_CCN + 0x20)
#define CCN_EXPEVT                  (SH7305_REG_PA_CCN + 0x24)
#define CCN_INTEVT                  (SH7305_REG_PA_CCN + 0x28)

#define MMUCR_AT                    0x00000001
#define MMUCR_TI                    0x00000004
#define CCR_OCE                     0x00000001
#define CCR_WT                      0x00000002
#define CCR_CB                      0x00000004
#define CCR_OCI                     0x00000008
#define CCR_ICE                     0x00000100
#define CCR_ICI                     0x00000800

// TMU (verified under the translator by the dbt tmu guest)
#define SH7305_REG_PA_TMU           0xA4490000
#define TMU_TSTR                    (SH7305_REG_PA_TMU + 0x04)      // 8-bit
#define TMU_TCOR0                   (SH7305_REG_PA_TMU + 0x08)      // 32-bit
#define TMU_TCNT0                   (SH7305_REG_PA_TMU + 0x0C)      // 32-bit
#define TMU_TCR0                    (SH7305_REG_PA_TMU + 0x10)      // 16-bit
#define TMU_TCOR1                   (SH7305_REG_PA_TMU + 0x14)
#define TMU_TCNT1                   (SH7305_REG_PA_TMU + 0x18)
#define TMU_TCR1                    (SH7305_REG_PA_TMU + 0x1C)
#define TMU_TCOR2                   (SH7305_REG_PA_TMU + 0x20)
#define TMU_TCNT2                   (SH7305_REG_PA_TMU + 0x24)
#define TMU_TCR2                    (SH7305_REG_PA_TMU + 0x28)

#define TMU_TSTR_STR0               0x01
#define TMU_TSTR_STR1               0x02
#define TMU_TSTR_STR2               0x04
#define TMU_TCR_UNF                 0x0100
#define TMU_TCR_UNIE                0x0020
#define TMU_TCR_TPSC_D4             0x0000
#define TMU_TCR_TPSC_D16            0x0001
#define TMU_TCR_TPSC_D64            0x0002
#define TMU_TCR_TPSC_D256           0x0003

// INTC: priority registers (16-bit, IPRA..IPRL at 4-byte spacing: casio-emu
// intc.c defineRegArray, SH7724 manual), mask registers (8-bit, IMR0..IMR12)
// and mask-clear registers (IMCR0..). Writing 1 to an IMR bit masks the
// source, writing 1 to the IMCR bit unmasks it.
#define SH7305_REG_PA_INTC          0xA4080000
#define INTC_IPR(n)                 (SH7305_REG_PA_INTC + 4 * (n))
#define INTC_IPRA                   INTC_IPR(0)     // TMU0 bits 15:12, TMU1 11:8, TMU2 7:4
#define INTC_IPRB                   INTC_IPR(1)     // bits 7:4: touch contact (INTEVT 0x700)
#define INTC_IPRF                   INTC_IPR(5)     // bits 15:12: KEYSC (INTEVT 0xBE0), bits 7:4: USB (INTEVT 0xA20)
#define INTC_IPRK                   INTC_IPR(10)    // bits 15:12: RTC (alarm INTEVT 0xA80, periodic 0xAA0, carry 0xAC0)
#define INTC_IMR(n)                 (SH7305_REG_PA_INTC + 0x80 + 4 * (n))   // write 1 to mask
#define INTC_IMCR(n)                (SH7305_REG_PA_INTC + 0xC0 + 4 * (n))   // write 1 to unmask
#define INTC_IMR3_TOUCH             0x10            // ClassPad touch contact (casio-emu classpad.c)
#define INTC_IMR4_TMU0              0x10
#define INTC_IMR4_TMU1              0x20
#define INTC_IMR4_TMU2              0x40
#define INTC_IMR5_KEYSC             0x80            // KEYSC (casio-emu keyboard.c)
#define INTC_IMR10_RTC_ALARM        0x01            // RTC alarm (casio-emu rtc.c)
#define INTC_IMR9_USB               0x02            // USB function controller (INTEVT 0xA20; gint intc.c)

// KEYSC key scan controller (register map in classpad_keysc.h)
#define SH7305_REG_PA_KEYSC         0xA44B0000

// IIC0 (SH-Mobile I2C, 8-bit registers at 4-byte spacing): the ClassPad
// touch sensor sits on it (classpad_touch.h)
#define SH7305_REG_PA_IIC0          0xA4470000

// GPIO port R data register (8-bit): bit 4 = LCD register/data select,
// bit 5 = touch contact line (0 while the panel is pressed; ClassPad only)
#define SH7305_REG_PA_PRDR          0xA405013C

// RTC (8-bit BCD counters and alarm registers, SH7724 section 27; Casio's
// clock keeps running in it, rtc.c only reads the counters and uses the alarm)
#define SH7305_REG_PA_RTC           0xA413FEC0
#define RTC_R64CNT                  (SH7305_REG_PA_RTC + 0x00)
#define RTC_RSECCNT                 (SH7305_REG_PA_RTC + 0x02)
#define RTC_RSECAR                  (SH7305_REG_PA_RTC + 0x10)  // bit 7 of each alarm register: compare it
#define RTC_RMINAR                  (SH7305_REG_PA_RTC + 0x12)
#define RTC_RHRAR                   (SH7305_REG_PA_RTC + 0x14)
#define RTC_RWKAR                   (SH7305_REG_PA_RTC + 0x16)
#define RTC_RDAYAR                  (SH7305_REG_PA_RTC + 0x18)
#define RTC_RMONAR                  (SH7305_REG_PA_RTC + 0x1A)
#define RTC_RCR1                    (SH7305_REG_PA_RTC + 0x1C)  // bit 7 CF, bit 4 CIE, bit 3 AIE, bit 0 AF
#define RTC_RCR2                    (SH7305_REG_PA_RTC + 0x1E)  // bit 0 START
#define RTC_RYRAR                   (SH7305_REG_PA_RTC + 0x20)
#define RTC_RCR3                    (SH7305_REG_PA_RTC + 0x24)  // bit 7: compare the year alarm
#define RTC_RCR1_AF                 0x01
#define RTC_RCR1_AIE                0x08

#endif
