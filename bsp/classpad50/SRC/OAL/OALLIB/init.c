//------------------------------------------------------------------------------
//  init.c - OEMInit for the CLASSPAD OAL (Windows CE 5.0)
//
//  Called by the kernel from SH3Init (mdsh3.c) with SR = PR_B1_BK: register
//  bank 1 and exceptions blocked, from the uncached alias of the image.
//  Nothing here may issue a trapa (debug output goes through the ring, see
//  debug.c). The kernel has already relocated its data, turned the MMU and
//  the caches on (CCR = CACHE_ENABLE | OEMExtraCCR) and printed its sign-on.
//------------------------------------------------------------------------------
#include <bsp.h>
#include "classpad_extrom.h"

// Variables the kernel imports from the OAL (shexcept.src, mdsh3.c):
//  - SH4CacheLines: the number of operand-cache lines the kernel's
//    DoSetRAMMode walks in the address array at 0xF4000000 (32-byte stride).
//    SH7305: 32 KiB, 4 ways x 256 entries, contiguous way bits 14:13.
//  - OEMExtraCCR (kernel common, left 0): CCR bits to add to CACHE_ENABLE
//    (OCE | ICE | CB = 0x105, also the SH-4A layout). Copy-back, both caches.
const DWORD SH4CacheLines = 1024;

// The EXT XIP region (classpad_extrom.h): OEMRomChain is the OEM's list of
// extra regions; KernelFindMemory (loader.c), which runs after OEMInit,
// appends the NK region to it.
static ROMChain_t g_extRom;

static void OALExtRomInit(void)
{
    unsigned toc = ClasspadExtRomFind((const void *)CLASSPAD_EXT_ROM_PA, CLASSPAD_EXT_ROM_PA, CLASSPAD_EXT_ROM_BYTES);
    if (!toc) {
        RETAILMSG(1, (L"CLASSPAD OAL: no EXT ROM region at %08x\r\n", CLASSPAD_EXT_ROM_PA));
        return;
    }
    g_extRom.pNext = NULL;
    g_extRom.pTOC = (ROMHDR *)toc;
    OEMRomChain = &g_extRom;
    RETAILMSG(1, (L"CLASSPAD OAL: EXT ROM region %08x..%08x, %u modules, %u files\r\n",
                  g_extRom.pTOC->physfirst, g_extRom.pTOC->physlast, g_extRom.pTOC->nummods, g_extRom.pTOC->numfiles));
}

void OEMInit(void)
{
    OALLogSetZones((1 << OAL_LOG_ERROR) | (1 << OAL_LOG_WARN) | (1 << OAL_LOG_INFO));

    RETAILMSG(1, (L"CLASSPAD OAL: SH7305 under the translator (CE 5.0)\r\n"));

    // No RAM-based error reporting buffer
    dwNKDrWatsonSize = 0;

    // Cache geometry for IOCTL_HAL_GET_CACHE_INFO
    OALCacheGlobalsInit();

    // INTC: the ISRs and their priorities (HookInterrupt is the one kernel
    // service allowed here), then the tick on TMU0 (variable period up to
    // TICK_MAX_MSEC, bsp_cfg.h). The kernel calls InitClock again after
    // OEMPowerOff.
    if (!OALIntrInit()) {
        OALMSG(OAL_ERROR, (L"ERROR: OEMInit: OALIntrInit failed\r\n"));
    }
    InitClock();

    // Files in the NOR hole (Math Xpander and the like), if flashed
    OALExtRomInit();
}
