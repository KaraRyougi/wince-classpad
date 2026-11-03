//------------------------------------------------------------------------------
//  intr_cfg.h - IRQ numbering and priorities
//
//  The kernel indexes its ISR table by (INTEVT - 0x200) / 0x20, the same
//  scheme the SH7750 BSPs use (sh4_intc.h), so IRQ numbers are INTEVT codes.
//------------------------------------------------------------------------------
#ifndef __INTR_CFG_H
#define __INTR_CFG_H

#define EXCEPTION_CODE_BASE         0x200
#define EXCEPTIONCODETOIRQ(x)       ((UINT32)(((x) - EXCEPTION_CODE_BASE) / 0x20))

#define IRQ_TMU0_TUNI0              EXCEPTIONCODETOIRQ(0x400)   // 16
#define IRQ_TMU1_TUNI1              EXCEPTIONCODETOIRQ(0x420)   // 17
#define IRQ_TMU2_TUNI2              EXCEPTIONCODETOIRQ(0x440)   // 18
// later: KEYSC 0xBE0, RTC 0x480.., USB 0xA20, ADC..

// Interrupt priorities (IPR fields; the kernel masks SR.IMASK to this while the ISR runs)
#define IRQ_TMU0_PRIORITY           0xE     // system timer
#define IRQ_TMU1_PRIORITY           0x0     // unused
#define IRQ_TMU2_PRIORITY           0x0     // unused

BOOL HookAndSetPriority(int hwIntNumber, FARPROC pfnHandler, BYTE intrPrio);

#endif
