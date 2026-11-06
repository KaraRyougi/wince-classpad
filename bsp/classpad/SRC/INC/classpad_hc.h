//------------------------------------------------------------------------------
//  classpad_hc.h - hypercalls to the translator (trapa #0xF0..0xFF)
//
//  A trapa is a general exception: it must not be issued with SR.BL=1 (manual
//  reset on SH-4A). The kernel runs the OAL with BL=1 in several places
//  (SHInit -> OEMInit, OEMIdle, parts of the scheduler), so every caller
//  checks ClasspadCanTrap() first and falls back to the debug ring or to
//  doing nothing. Numbers and arguments: docs/04 "Hypercalls".
//------------------------------------------------------------------------------
#ifndef __CLASSPAD_HC_H
#define __CLASSPAD_HC_H

#define HC_EXIT         0xF0        // r4 = exit code; stops the emulator run
#define HC_PUTS         0xF1        // r4 = NUL-terminated byte string
#define HC_FLUSH        0xF2        // r4 = address, r5 = length: drop translations of code written in the range (0,0 = all)
#define HC_PUTC         0xF3        // r4 = byte
#define HC_PUTHEX       0xF4        // r4 = value
#define HC_STATS        0xF5        // print translator statistics
#define HC_TLB_INVAL    0xF9        // r4 = address, r5 = length: user mappings changed (len 0 = all)
#define HC_ASID         0xFA        // r4 = new ASID (the translator also notices PTEH itself)
#define HC_GETSTAT      0xFB        // r4 = index -> r0
#define HC_LCD_BLIT     0xFD        // r4 = pixels (P1/P2 address, as the guest stores them), r5 = count, r6 = LCD data port: pushed natively

// shcl: __asm(string, args...) places the arguments in r4, r5, ...; __asm1 returns r0
extern DWORD __asm1(const char *, ...);

#define SR_BL_BIT       0x10000000

static __inline BOOL ClasspadCanTrap(void)
{
    return (__asm1("stc sr, r0") & SR_BL_BIT) == 0;
}

static __inline void ClasspadHcPutc(BYTE ch)      { __asm("trapa #243", (DWORD)ch); }
static __inline void ClasspadHcExit(DWORD code)   { __asm("trapa #240", code); }
static __inline void ClasspadHcFlush(DWORD addr, DWORD len)     { __asm("trapa #242", addr, len); }
static __inline void ClasspadHcTlbInval(DWORD addr, DWORD len)  { __asm("trapa #249", addr, len); }
static __inline void ClasspadHcLcdBlit(DWORD src, DWORD pixels, DWORD port) { __asm("trapa #253", src, pixels, port); }

#endif
