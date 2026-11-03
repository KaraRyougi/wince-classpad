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
//------------------------------------------------------------------------------
#ifndef __CLASSPAD_MMIO_H
#define __CLASSPAD_MMIO_H

#define CLASSPAD_UA(pa)     ((UINT32)(pa) | 0xA0000000)

static __inline UINT32 classpad_bswap32(UINT32 v)
{
    return (v >> 24) | ((v >> 8) & 0x0000FF00) | ((v << 8) & 0x00FF0000) | (v << 24);
}
static __inline UINT16 classpad_bswap16(UINT16 v)
{
    return (UINT16)((v >> 8) | (v << 8));
}

#define MMIO_R32(pa)        classpad_bswap32(*(volatile UINT32 *)CLASSPAD_UA(pa))
#define MMIO_W32(pa, v)     (*(volatile UINT32 *)CLASSPAD_UA(pa) = classpad_bswap32((UINT32)(v)))
#define MMIO_R16(pa)        classpad_bswap16(*(volatile UINT16 *)CLASSPAD_UA(pa))
#define MMIO_W16(pa, v)     (*(volatile UINT16 *)CLASSPAD_UA(pa) = classpad_bswap16((UINT16)(v)))
#define MMIO_R8(pa)         (*(volatile UINT8 *)CLASSPAD_UA(pa))
#define MMIO_W8(pa, v)      (*(volatile UINT8 *)CLASSPAD_UA(pa) = (UINT8)(v))

#endif
