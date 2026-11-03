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

// INTC: priority registers (16-bit), mask registers (8-bit) and mask-clear registers
#define SH7305_REG_PA_INTC          0xA4080000
#define INTC_IPRA                   (SH7305_REG_PA_INTC + 0x00)     // TMU0 bits 15:12, TMU1 11:8, TMU2 7:4
#define INTC_IPR(n)                 (SH7305_REG_PA_INTC + 2 * (n))
#define INTC_IMR(n)                 (SH7305_REG_PA_INTC + 0x80 + 4 * (n))   // write 1 to mask
#define INTC_IMCR(n)                (SH7305_REG_PA_INTC + 0xC0 + 4 * (n))   // write 1 to unmask
#define INTC_IMR4_TMU0              0x10
#define INTC_IMR4_TMU1              0x20
#define INTC_IMR4_TMU2              0x40

// RTC (to be used by rtc.c later; BCD registers)
#define SH7305_REG_PA_RTC           0xA413FEC0

#endif
