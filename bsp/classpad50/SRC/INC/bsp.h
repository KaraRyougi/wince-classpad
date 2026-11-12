//------------------------------------------------------------------------------
//  bsp.h - everything the CLASSPAD OAL sources include (Windows CE 5.0)
//
//  CE 5.0 links the kernel and the OAL into one nk.exe and calls the OEM
//  functions by name (nkintr.h); there is no OEMGLOBAL table. The PQOAL
//  headers of PLATFORM\COMMON\SRC\INC are used for their types and
//  prototypes only (OAL_TIMER_STATE, OAL_IOCTL_HANDLER, OALMSG...): the
//  functions behind them are this BSP's own (OALLIB\oalcommon.c), since the
//  CE 5.0 tree ships no SHx build of the common OAL libraries.
//------------------------------------------------------------------------------
#ifndef __BSP_H
#define __BSP_H

#include <windows.h>
#include <nkintr.h>
#include <pkfuncs.h>
#include <pehdr.h>        // struct info: romldr.h uses it without including pehdr.h
#include <romldr.h>
#include <oal_log.h>
#include <oal_cache.h>
#include <oal_intr.h>
#include <oal_ioctl.h>
#include <oal_power.h>
#include <oal_timer.h>
#include <oal_nkxp.h>       // the kernel variables an OAL may touch: CurMSec, dwReschedTime, curridle*, idleconv, pQueryPerformance*, pOEMUpdateRescheduleTime
#include "bsp_cfg.h"
#include "bsp_base_regs.h"
#include "image_cfg.h"
#include "ioctl_cfg.h"
#include "classpad_pwrstat.h"
#include "intr_cfg.h"
#include "classpad_mmio.h"
#include "classpad_hc.h"
#include "classpad_iic.h"

#ifdef __cplusplus
extern "C" {
#endif

// More kernel variables (PRIVATE\WINCEOS\COREOS\NK; PLATFORM\ASPEN declares
// them the same way)
extern ROMChain_t *OEMRomChain;                         // extra XIP regions (the EXT region), loader.c
extern BYTE IntrPrio[];                                 // SR.IMASK level per ISR table entry, shexcept.src

// OEM functions without a public prototype in CE 5.0
BOOL OEMIoControl(DWORD code, LPVOID pInBuf, DWORD inSize, LPVOID pOutBuf, DWORD outSize, LPDWORD pBytesReturned);
void InitClock(void);                                   // OEMInit, and the kernel after OEMPowerOff (kwin32.c CallOEMPowerOff)
void OEMIdle(DWORD dwIdleParam);
DWORD SC_GetTickCount(void);

// BSP
BOOL OALIoCtlHalPresuspend(UINT32, VOID*, UINT32, VOID*, UINT32, UINT32*);
BOOL OALIoCtlHalClasspadSuspendStats(UINT32, VOID*, UINT32, VOID*, UINT32, UINT32*);
BOOL OALIoCtlHalClasspadPanel(UINT32, VOID*, UINT32, VOID*, UINT32, UINT32*);
BOOL OALIoCtlHalClasspadSuspendOptions(UINT32, VOID*, UINT32, VOID*, UINT32, UINT32*);
BOOL OALIoCtlHalClasspadPphi(UINT32, VOID*, UINT32, VOID*, UINT32, UINT32*);
UINT32 OALClockPphiHz(void);                           // timer.c: the peripheral clock now (measured at boot, scaled by OALTimerRetune)
BOOL OALIoCtlHalPostInit(UINT32, VOID*, UINT32, VOID*, UINT32, UINT32*);
BOOL OALIoCtlHalInitRTC(UINT32, VOID*, UINT32, VOID*, UINT32, UINT32*);
BOOL OALIoCtlHalGetCacheInfo(UINT32, VOID*, UINT32, VOID*, UINT32, UINT32*);

// Clock control (ioctl.c, timer.c; register layout in classpad_clock.h)
BOOL   OALClockChanged(void);
VOID   OALTimerRetune(UINT32 mul, UINT32 div);
UINT32 OALTimerCountHz(void);
BOOL OALIoCtlHalClasspadClockGet(UINT32, VOID*, UINT32, VOID*, UINT32, UINT32*);
BOOL OALIoCtlHalClasspadClockSet(UINT32, VOID*, UINT32, VOID*, UINT32, UINT32*);

#ifdef __cplusplus
}
#endif

#endif
