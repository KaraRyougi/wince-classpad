//------------------------------------------------------------------------------
//  init.c - OEMInit for the CLASSPAD OAL
//
//  Called by the kernel from SHInit with SR = PR_B1_BK: register bank 1 and
//  exceptions blocked. Nothing here may issue a trapa (debug output goes
//  through the ring, see debug.c).
//------------------------------------------------------------------------------
#include <bsp.h>

void OEMInit()
{
    OALLogSetZones(initialOALLogZones);

    OALMSG(OAL_FUNC, (L"+OEMInit()\r\n"));
    RETAILMSG(1, (L"CLASSPAD OAL: SH7305 under the translator\r\n"));

    // No RAM-based error reporting buffer
    dwNKDrWatsonSize = 0;

    // Cache geometry for IOCTL_HAL_GET_CACHE_INFO
    OALCacheGlobalsInit();

    // INTC and the timer tick (TMU0, 1 ms)
    if (!OALIntrInit()) {
        OALMSG(OAL_ERROR, (L"ERROR: OEMInit: OALIntrInit failed\r\n"));
    }
    OALTimerInit(1, COUNT_FREQUENCY / 1000, 0);

    OALMSG(OAL_FUNC, (L"-OEMInit()\r\n"));
}
