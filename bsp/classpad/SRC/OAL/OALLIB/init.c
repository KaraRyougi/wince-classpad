//------------------------------------------------------------------------------
//  init.c - OEMInit for the CLASSPAD OAL
//
//  Called by the kernel from SHInit with SR = PR_B1_BK: register bank 1 and
//  exceptions blocked. Nothing here may issue a trapa (debug output goes
//  through the ring, see debug.c).
//------------------------------------------------------------------------------
#include <bsp.h>
#include <romldr.h>
#include "classpad_extrom.h"

// The EXT region (classpad_extrom.h): the kernel puts the NK region after it
// in its ROM chain (KernelFindMemory), which runs after OEMInit.
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
    g_pOemGlobal->pROMChain = &g_extRom;
    RETAILMSG(1, (L"CLASSPAD OAL: EXT ROM region %08x..%08x, %u files\r\n",
                  g_extRom.pTOC->physfirst, g_extRom.pTOC->physlast, g_extRom.pTOC->numfiles));
}

void OEMInit()
{
    OALLogSetZones(initialOALLogZones);

    OALMSG(OAL_FUNC, (L"+OEMInit()\r\n"));
    RETAILMSG(1, (L"CLASSPAD OAL: SH7305 under the translator\r\n"));

    // No RAM-based error reporting buffer
    dwNKDrWatsonSize = 0;

    // Cache geometry for IOCTL_HAL_GET_CACHE_INFO
    OALCacheGlobalsInit();

    // INTC and the timer tick (TMU0, variable period up to TICK_MAX_MSEC; bsp_cfg.h)
    if (!OALIntrInit()) {
        OALMSG(OAL_ERROR, (L"ERROR: OEMInit: OALIntrInit failed\r\n"));
    }
    OALTimerInit(TICK_MAX_MSEC, COUNT_FREQUENCY / 1000, TICK_MARGIN_COUNTS);

    // Files in the NOR hole (Math Xpander and the like), if flashed
    OALExtRomInit();

    OALMSG(OAL_FUNC, (L"-OEMInit()\r\n"));
}
