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
#define HC_GETSTAT      0xFB        // r4 = index -> r0 (indices below; dbt/rt/rt.c is the reference)
#define HC_LCD_BLIT     0xFD        // r4 = pixels (P1/P2 address, as the guest stores them), r5 = count, r6 = LCD data port: pushed natively
#define HC_LCD_PUSH     0xFE        // DCS panel: r4 = pixels (P1/P2), r5 = first row, r6 = rows, r7 = width; window commands + pixels in one go
#define HC_STANDBY      0xFF        // SH7305 R-standby (docs/06): r4 = flags (0) -> r0 = 1 after the wake-up, 0 if not entered

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
static __inline void ClasspadHcLcdPush(DWORD src, DWORD top, DWORD rows, DWORD width) { __asm("trapa #254", src, top, rows, width); }
static __inline DWORD ClasspadHcStandby(DWORD flags) { return __asm1("trapa #255", flags); }

// HC_GETSTAT: one 32-bit counter of the translator (all wrap modulo 2^32:
// take deltas). Works from user mode too (a trapa is a general exception the
// translator takes before the kernel sees it), which is how the DBTSTAT
// window (SRC\APPS\DBTSTAT) profiles a run on the real machine. User-mode
// callers must not use ClasspadCanTrap (stc sr is privileged); they never
// run with BL=1 anyway.
//   0 translations  1 blocks  2 invalidations  3 ASID switches  4 inline-cache
//   misses (C)  5 debug-ring bytes  6 device accesses  7 full flushes (and bank
//   recycles)  8 FPU-disable exceptions  9/10 device-aware range  11 profiling
//   build (1/0)  12 TMU1 counts elapsed (PROFILE=1 builds; 7372800/s)
//   13 same-ASID notifications  14 TLB scans skipped  23 exceptions
//   24 hypercalls  25 rte C entries  26 ldc-sr helper calls  27 lookup C
//   entries  28 TLB hooks  29 bank evictions  30 user IC hash-hit feature
//   31 same-mode SR fast returns; 26 includes both fast and C paths
//   63 bit 0: DBT_FAST_LDC_SR enabled
//   32+3b, 33+3b, 34+3b: calls, TMU1 counts, max counts of PROFILE bucket b
//   (0 translate, 1 asid, 2 tlb, 3 invalidate, 4 lcd, 5 flush, 6 exception
//   delivery, 7 rte, 8 lookup); zero in non-profiling builds
//   60 "DBTS" 0x44425453  61 stats ABI version (1)  62 features: bit 0 tick sampling, bit 1 PROFILE
//   64 tick samples  65 in translated code  66 exit stub  67 stub window
//   68 translating  69 helper  70 idle (sleep)  71 user-mode pcs  72 of those
//   not in the ROM image  73 kernel pcs outside the histogram
//   74 histogram base (physical)  75 log2 bucket bytes  76 bucket count (256)
//   77/78/79 exceptions by vector: general, TLB miss, interrupt
//   80 CCR  81 MMUCR  82 FRQCR  83 RTC seconds of day
//   84 number of INTEVT codes seen, 85+2k code k, 86+2k its count (k < 16)
//   117..122 the stub-window samples (67) split: 117 stub entry windows,
//   118 after lookup/translation C (translator time under its interrupt
//   mask), 119 after the ldc-sr helper (kernel code that ran with interrupts
//   disabled), 120 after the fpu/fallback helpers, 121 asm hash hit, 122
//   rte/bank trampolines;  123..127 the last five kernel pcs outside the histogram
//   128..383 the histogram: tick samples per bucket of guest code by physical address
//   384 PLLCR  385 FLLFRQ (with 82 FRQCR: the clock tree; the fx-CP400 does
//   not run the fx-CG50 clocks bsp_cfg.h assumes, see tick/rtc in the window)
//   386 guest pc of the slowest translation so far (PROFILE=1 builds)
#define HC_STAT_SAMPLES     64
#define HC_STAT_HIST_BASE   74
#define HC_STAT_HIST        128
#define HC_STAT_HIST_N      256
#define HC_STAT_N           387
static __inline DWORD ClasspadHcGetStat(DWORD index) { return __asm1("trapa #251", index); }

#endif
