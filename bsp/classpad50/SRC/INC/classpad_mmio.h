//------------------------------------------------------------------------------
//  classpad_mmio.h - pre-swapped device register access
//
//  The translator byte-swaps every 16-bit and 32-bit load and store so that
//  a little-endian image sees little-endian memory on the big-endian SH7305.
//  Device registers are not memory: to write register value V the program
//  must store bswap(V), and a register read returns bswap(value). Bytes are
//  untouched. This is the same convention as dbt/guest/mmio.h in the
//  wince-classpad repo (docs/04 "MMIO").
//
//  Every register address in this BSP is already a P2/P4 address (0xA4...,
//  0xFF...), so the uncached alias is the address itself; CLASSPAD_UA does
//  what the OAL's OALPAtoUA does (pa | 0xA0000000) without needing oal.h, so
//  the kernel-mode drivers hosted by GWES (display, keyboard, touch) use the
//  same macros as the OAL.
//
//  Exception, CE 5.0: the OAL is part of nk.exe, which the translator
//  translates "device-aware" (docs/04): a 16/32-bit access whose address has
//  the top byte 0xFF (the P4 control registers: MMUCR, CCR, INTEVT...) is
//  performed unswapped there, because the kernel's own code reads and writes
//  those registers directly. OAL code therefore accesses 0xFFxxxxxx with the
//  plain P4_* macros, never with MMIO_*: a pre-swapped value would be swapped
//  back by nothing. Drivers (outside nk.exe) have no business with P4.
//------------------------------------------------------------------------------
#ifndef __CLASSPAD_MMIO_H
#define __CLASSPAD_MMIO_H

#define CLASSPAD_UA(pa)     ((UINT32)(pa) | 0xA0000000)

#if defined(_MSC_VER)
extern DWORD __asm1(const char *, ...);
static __inline UINT32 classpad_bswap32(UINT32 v)
{
    v = (UINT32)__asm1("swap.b r4, r0", (DWORD)v);
    v = (UINT32)__asm1("swap.w r4, r0", (DWORD)v);
    return (UINT32)__asm1("swap.b r4, r0", (DWORD)v);
}
static __inline UINT16 classpad_bswap16(UINT16 v)
{
    return (UINT16)__asm1("swap.b r4, r0", (DWORD)v);
}
#elif defined(__GNUC__)
static __inline UINT32 classpad_bswap32(UINT32 v) { return __builtin_bswap32(v); }
static __inline UINT16 classpad_bswap16(UINT16 v) { return __builtin_bswap16(v); }
#else
static __inline UINT32 classpad_bswap32(UINT32 v)
{
    return (v >> 24) | ((v >> 8) & 0x0000FF00) | ((v << 8) & 0x00FF0000) | (v << 24);
}
static __inline UINT16 classpad_bswap16(UINT16 v)
{
    return (UINT16)((v >> 8) | (v << 8));
}
#endif

#define MMIO_R32(pa)        classpad_bswap32(*(volatile UINT32 *)CLASSPAD_UA(pa))
#define MMIO_W32(pa, v)     (*(volatile UINT32 *)CLASSPAD_UA(pa) = classpad_bswap32((UINT32)(v)))
#define MMIO_R16(pa)        classpad_bswap16(*(volatile UINT16 *)CLASSPAD_UA(pa))
#define MMIO_W16(pa, v)     (*(volatile UINT16 *)CLASSPAD_UA(pa) = classpad_bswap16((UINT16)(v)))
#define MMIO_R8(pa)         (*(volatile UINT8 *)CLASSPAD_UA(pa))
#define MMIO_W8(pa, v)      (*(volatile UINT8 *)CLASSPAD_UA(pa) = (UINT8)(v))

// P4 control registers (0xFFxxxxxx) from code inside nk.exe only (above)
#define P4_R32(pa)          (*(volatile UINT32 *)(pa))
#define P4_W32(pa, v)       (*(volatile UINT32 *)(pa) = (UINT32)(v))

#endif
