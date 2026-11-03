//------------------------------------------------------------------------------
//  intr_cfg.h - IRQ numbering and priorities
//
//  The kernel indexes its ISR table by (INTEVT - 0x200) / 0x20, the same
//  scheme the SH7750 BSPs use (sh4_intc.h), so IRQ numbers are INTEVT codes.
//
//  The common OAL interrupt module maps IRQ <-> SYSINTR through 64-entry
//  tables (OAL_INTR_IRQ_MAXIMUM, prebuilt in PLATFORM\COMMON): a source whose
//  vector is 64 or more gets a separate logical IRQ number below 64. Drivers
//  pass the logical number to IOCTL_HAL_REQUEST_SYSINTR; the OAL hooks the
//  ISR at the vector (HookInterrupt) and masks through the logical number.
//------------------------------------------------------------------------------
#ifndef __INTR_CFG_H
#define __INTR_CFG_H

#define EXCEPTION_CODE_BASE         0x200
#define EXCEPTIONCODETOIRQ(x)       ((UINT32)(((x) - EXCEPTION_CODE_BASE) / 0x20))

// vector == logical IRQ (below 64)
#define IRQ_TMU0_TUNI0              EXCEPTIONCODETOIRQ(0x400)   // 16
#define IRQ_TMU1_TUNI1              EXCEPTIONCODETOIRQ(0x420)   // 17
#define IRQ_TMU2_TUNI2              EXCEPTIONCODETOIRQ(0x440)   // 18
#define IRQ_TOUCH_PEN               EXCEPTIONCODETOIRQ(0x700)   // 40: ClassPad touch contact (casio-emu classpad.c)

// KEYSC: INTEVT 0xBE0 is vector 95, so it gets logical IRQ 48
#define VEC_KEYSC                   EXCEPTIONCODETOIRQ(0xBE0)   // 95: HookInterrupt / IntrPrio index
#define IRQ_KEYSC                   48                          // logical: drivers, OALIntr* mask table
// later: RTC 0x480.., USB 0xA20, ADC 0x560, IIC0 0xE00-0xE60

// Interrupt priorities (IPR fields; the kernel masks SR.IMASK to this while the ISR runs)
#define IRQ_TMU0_PRIORITY           0xE     // system timer
#define IRQ_TMU1_PRIORITY           0x0     // unused
#define IRQ_TMU2_PRIORITY           0x0     // unused
#define IRQ_KEYSC_PRIORITY          0xD     // what the fx-CG100 OS programs into IPRF 15:12
#define IRQ_TOUCH_PRIORITY          0xC     // IPRB 7:4

BOOL HookAndSetPriority(int hwIntNumber, FARPROC pfnHandler, BYTE intrPrio);

#endif
