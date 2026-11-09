//------------------------------------------------------------------------------
//  oalcommon.c - the PQOAL helpers the CE 6.0 BSP took from PLATFORM\COMMON
//
//  Windows CE 5.0 ships the common OAL libraries (oal_intr, oal_ioctl,
//  oal_log, oal_power, oal_timer...) in source for ARM, MIPS and x86 only;
//  nothing in the SH4 tree builds them. The BSP's OAL files were written
//  against that API (intr.c, timer.c, cache.c, power.c, ioctl.c), so this
//  file supplies the same functions, reduced to what the ClassPad uses:
//    - the log (OALMSG -> OALLog = the kernel's NKDbgPrintfW; the zone mask)
//    - the IRQ <-> SYSINTR map and the kernel's OEMInterruptEnable/Disable/
//      Done built on OALIntrEnableIrqs/DisableIrqs/DoneIrqs of intr.c
//    - OEMIoControl, driving g_oalIoCtlTable (ioctl.c, ioctl_tab.h), and the
//      generic handlers: SYSINTR requests, device info and id, processor
//      info, cache info, registry init
//    - wake sources (OALPowerWakeSource and its IOCTLs)
//    - the timer state (g_oalTimer), QueryPerformanceCounter and
//      SC_GetTickCount on top of timer.c
//    - OEMGetExtensionDRAM (none)
//  The headers are the PQOAL ones (PLATFORM\COMMON\SRC\INC), so the other
//  files read like a CE 6.0 BSP.
//------------------------------------------------------------------------------
#include <bsp.h>

//==============================================================================
//  Log
//
UINT32 g_oalLogMask = (1 << OAL_LOG_ERROR) | (1 << OAL_LOG_WARN) | (1 << OAL_LOG_INFO);

VOID OALLogSetZones(UINT32 newMask)
{
    g_oalLogMask = newMask;
}

// OALLog is NKDbgPrintfW itself in CE 5.0's oal_log.h (the kernel's printf.c
// defines it); only the serial variant is the OAL's to provide.
VOID OALLogSerial(LPCWSTR format, ...)
{
    va_list args;
    va_start(args, format);
    NKvDbgPrintfW(format, args);
    va_end(args);
}

//==============================================================================
//  IRQ <-> SYSINTR map (PLATFORM\COMMON\SRC\COMMON\INTR\BASE\map.c, one IRQ
//  per SYSINTR: every ClassPad source is a single interrupt)
//
// OAL_INTR_IRQ_UNDEFINED comes from oal_intr.h

static UINT32 g_oalSysIntr2Irq[SYSINTR_MAXIMUM];
static UINT32 g_oalIrq2SysIntr[OAL_INTR_IRQ_MAXIMUM];

VOID OALIntrMapInit(void)
{
    UINT32 i;
    for (i = 0; i < SYSINTR_MAXIMUM; i++) g_oalSysIntr2Irq[i] = OAL_INTR_IRQ_UNDEFINED;
    for (i = 0; i < OAL_INTR_IRQ_MAXIMUM; i++) g_oalIrq2SysIntr[i] = (UINT32)SYSINTR_UNDEFINED;
}

VOID OALIntrStaticTranslate(UINT32 sysIntr, UINT32 irq)
{
    if (irq < OAL_INTR_IRQ_MAXIMUM && sysIntr < SYSINTR_MAXIMUM) {
        g_oalIrq2SysIntr[irq] = sysIntr;
        g_oalSysIntr2Irq[sysIntr] = irq;
    }
}

UINT32 OALIntrTranslateIrq(UINT32 irq)
{
    return (irq < OAL_INTR_IRQ_MAXIMUM) ? g_oalIrq2SysIntr[irq] : (UINT32)SYSINTR_UNDEFINED;
}

BOOL OALIntrTranslateSysIntr(UINT32 sysIntr, UINT32 *pCount, const UINT32 **ppIrqs)
{
    if (sysIntr >= SYSINTR_MAXIMUM || g_oalSysIntr2Irq[sysIntr] == OAL_INTR_IRQ_UNDEFINED) return FALSE;
    *pCount = 1;
    *ppIrqs = &g_oalSysIntr2Irq[sysIntr];
    return TRUE;
}

UINT32 OALIntrRequestSysIntr(UINT32 count, const UINT32 *pIrqs, UINT32 flags)
{
    UINT32 irq, sysIntr;

    if (count != 1 || pIrqs == NULL) return (UINT32)SYSINTR_UNDEFINED;
    irq = pIrqs[0];
    if (irq >= OAL_INTR_IRQ_MAXIMUM) return (UINT32)SYSINTR_UNDEFINED;

    // An IRQ that already has a SYSINTR: hand out the same one when the
    // caller allows a translation (the old IOCTL_HAL_TRANSLATE_IRQ form, or
    // a static mapping it asked for); a dynamic request for a taken IRQ fails
    sysIntr = g_oalIrq2SysIntr[irq];
    if (sysIntr != (UINT32)SYSINTR_UNDEFINED) {
        if (flags & (OAL_INTR_TRANSLATE | OAL_INTR_STATIC | OAL_INTR_FORCE_STATIC)) return sysIntr;
        return (UINT32)SYSINTR_UNDEFINED;
    }
    for (sysIntr = SYSINTR_FIRMWARE; sysIntr < SYSINTR_MAXIMUM; sysIntr++) {
        if (g_oalSysIntr2Irq[sysIntr] == OAL_INTR_IRQ_UNDEFINED) break;
    }
    if (sysIntr >= SYSINTR_MAXIMUM) {
        OALMSG(OAL_ERROR, (L"ERROR: OALIntrRequestSysIntr: no free SYSINTR for IRQ %d\r\n", irq));
        return (UINT32)SYSINTR_UNDEFINED;
    }
    g_oalSysIntr2Irq[sysIntr] = irq;
    g_oalIrq2SysIntr[irq] = sysIntr;
    return sysIntr;
}

BOOL OALIntrReleaseSysIntr(UINT32 sysIntr)
{
    UINT32 irq;
    if (sysIntr < SYSINTR_FIRMWARE || sysIntr >= SYSINTR_MAXIMUM) return FALSE;
    irq = g_oalSysIntr2Irq[sysIntr];
    if (irq == OAL_INTR_IRQ_UNDEFINED) return FALSE;
    g_oalSysIntr2Irq[sysIntr] = OAL_INTR_IRQ_UNDEFINED;
    g_oalIrq2SysIntr[irq] = (UINT32)SYSINTR_UNDEFINED;
    return TRUE;
}

//------------------------------------------------------------------------------
//  The kernel's interrupt entry points (nkintr.h): called from
//  InterruptInitialize/InterruptDisable/InterruptDone with the system not
//  preemptible
//
BOOL OEMInterruptEnable(DWORD sysIntr, LPVOID pvData, DWORD cbData)
{
    UINT32 count;
    const UINT32 *pIrqs;

    if (!OALIntrTranslateSysIntr(sysIntr, &count, &pIrqs)) {
        OALMSG(OAL_WARN, (L"WARN: OEMInterruptEnable: SYSINTR %d has no IRQ\r\n", sysIntr));
        return FALSE;
    }
    return OALIntrEnableIrqs(count, pIrqs);
}

void OEMInterruptDisable(DWORD sysIntr)
{
    UINT32 count;
    const UINT32 *pIrqs;
    if (OALIntrTranslateSysIntr(sysIntr, &count, &pIrqs)) OALIntrDisableIrqs(count, pIrqs);
}

void OEMInterruptDone(DWORD sysIntr)
{
    UINT32 count;
    const UINT32 *pIrqs;
    if (OALIntrTranslateSysIntr(sysIntr, &count, &pIrqs)) OALIntrDoneIrqs(count, pIrqs);
}

//------------------------------------------------------------------------------
//  IOCTL_HAL_REQUEST_SYSINTR / IOCTL_HAL_TRANSLATE_IRQ: the old form passes
//  one IRQ number; the new one {-1, flags, count, irq...}
//
BOOL OALIoCtlHalRequestSysIntr(UINT32 code, VOID *pInpBuffer, UINT32 inpSize, VOID *pOutBuffer, UINT32 outSize, UINT32 *pOutSize)
{
    UINT32 *pIn = (UINT32 *)pInpBuffer, sysIntr, count, flags;
    const UINT32 *pIrqs;

    if (pIn == NULL || inpSize < sizeof(UINT32) || pOutBuffer == NULL || outSize < sizeof(UINT32)) return FALSE;
    if (inpSize > sizeof(UINT32) && pIn[0] == (UINT32)-1) {
        if (inpSize < 3 * sizeof(UINT32)) return FALSE;
        flags = pIn[1]; count = pIn[2]; pIrqs = &pIn[3];
        if (inpSize < (3 + count) * sizeof(UINT32)) return FALSE;
    } else {
        flags = OAL_INTR_TRANSLATE; count = 1; pIrqs = pIn;
    }
    sysIntr = OALIntrRequestSysIntr(count, pIrqs, flags);
    if (sysIntr == (UINT32)SYSINTR_UNDEFINED) return FALSE;
    *(UINT32 *)pOutBuffer = sysIntr;
    if (pOutSize) *pOutSize = sizeof(UINT32);
    return TRUE;
}

BOOL OALIoCtlHalReleaseSysIntr(UINT32 code, VOID *pInpBuffer, UINT32 inpSize, VOID *pOutBuffer, UINT32 outSize, UINT32 *pOutSize)
{
    if (pInpBuffer == NULL || inpSize < sizeof(UINT32)) return FALSE;
    return OALIntrReleaseSysIntr(*(UINT32 *)pInpBuffer);
}

BOOL OALIoCtlHalRequestIrq(UINT32 code, VOID *pInpBuffer, UINT32 inpSize, VOID *pOutBuffer, UINT32 outSize, UINT32 *pOutSize)
{
    // No bus-enumerated devices (intr.c OALIntrRequestIrqs)
    return FALSE;
}

//==============================================================================
//  OEMIoControl: the table in ioctl.c
//
BOOL OEMIoControl(DWORD code, LPVOID pInBuf, DWORD inSize, LPVOID pOutBuf, DWORD outSize, LPDWORD pBytesReturned)
{
    UINT32 i;
    for (i = 0; g_oalIoCtlTable[i].pfnHandler != NULL; i++) {
        if (g_oalIoCtlTable[i].code == code) {
            return g_oalIoCtlTable[i].pfnHandler(code, pInBuf, inSize, pOutBuf, outSize, (UINT32 *)pBytesReturned);
        }
    }
    OALMSG(OAL_WARN, (L"WARN: OEMIoControl: unsupported code 0x%x\r\n", code));
    return FALSE;
}

static UINT32 WStrLen(LPCWSTR s) { UINT32 n = 0; while (s[n]) n++; return n; }

//------------------------------------------------------------------------------
//  IOCTL_HAL_GET_DEVICE_INFO: SPI_GETPLATFORMTYPE / SPI_GETOEMINFO strings
//
static BOOL CopyString(LPCWSTR s, VOID *pOut, UINT32 outSize, UINT32 *pOutSize)
{
    UINT32 bytes = (WStrLen(s) + 1) * sizeof(WCHAR);
    if (pOutSize) *pOutSize = bytes;
    if (pOut == NULL || outSize < bytes) return FALSE;
    memcpy(pOut, s, bytes);
    return TRUE;
}

BOOL OALIoCtlHalGetDeviceInfo(UINT32 code, VOID *pInpBuffer, UINT32 inpSize, VOID *pOutBuffer, UINT32 outSize, UINT32 *pOutSize)
{
    if (pInpBuffer == NULL || inpSize < sizeof(UINT32)) return FALSE;
    switch (*(UINT32 *)pInpBuffer) {
    case SPI_GETPLATFORMTYPE: return CopyString(g_oalIoCtlPlatformType, pOutBuffer, outSize, pOutSize);
    case SPI_GETOEMINFO:      return CopyString(g_oalIoCtlPlatformOEM, pOutBuffer, outSize, pOutSize);
    default:                  return FALSE;
    }
}

//------------------------------------------------------------------------------
//  IOCTL_HAL_GET_DEVICEID: DEVICE_ID with the platform type (Unicode) as the
//  preset id and a fixed byte string as the platform id, like the common
//  deviceid.c (which takes the latter from the boot arguments; there are none)
//
BOOL OALIoCtlHalGetDeviceId(UINT32 code, VOID *pInpBuffer, UINT32 inpSize, VOID *pOutBuffer, UINT32 outSize, UINT32 *pOutSize)
{
    static const char devId[] = "CLASSPAD-SH7305";
    DEVICE_ID *pId = (DEVICE_ID *)pOutBuffer;
    UINT32 len1 = (WStrLen(g_oalIoCtlPlatformType) + 1) * sizeof(WCHAR), len2 = sizeof devId;
    UINT32 size = sizeof(DEVICE_ID) + len1 + len2;

    if (pOutSize) *pOutSize = size;
    if (pId == NULL || outSize < sizeof(DEVICE_ID)) return FALSE;
    pId->dwSize = size;
    if (outSize < size) return FALSE;
    pId->dwPresetIDOffset = sizeof(DEVICE_ID);
    pId->dwPresetIDBytes = len1;
    memcpy((UINT8 *)pId + pId->dwPresetIDOffset, g_oalIoCtlPlatformType, len1);
    pId->dwPlatformIDOffset = pId->dwPresetIDOffset + len1;
    pId->dwPlatformIDBytes = len2;
    memcpy((UINT8 *)pId + pId->dwPlatformIDOffset, devId, len2);
    return TRUE;
}

//------------------------------------------------------------------------------
//  IOCTL_HAL_GET_UUID: a fixed id (no hardware serial number is read yet)
//
BOOL OALIoCtlHalGetUUID(UINT32 code, VOID *pInpBuffer, UINT32 inpSize, VOID *pOutBuffer, UINT32 outSize, UINT32 *pOutSize)
{
    static const UINT8 uuid[16] = { 0x43, 0x4C, 0x41, 0x53, 0x53, 0x50, 0x41, 0x44, 0x53, 0x48, 0x37, 0x33, 0x30, 0x35, 0x00, 0x01 };
    if (pOutSize) *pOutSize = sizeof uuid;
    if (pOutBuffer == NULL || outSize < sizeof uuid) return FALSE;
    memcpy(pOutBuffer, uuid, sizeof uuid);
    return TRUE;
}

//------------------------------------------------------------------------------
//  IOCTL_PROCESSOR_INFORMATION
//
BOOL OALIoCtlProcessorInfo(UINT32 code, VOID *pInpBuffer, UINT32 inpSize, VOID *pOutBuffer, UINT32 outSize, UINT32 *pOutSize)
{
    PROCESSOR_INFO *pInfo = (PROCESSOR_INFO *)pOutBuffer;
    if (pOutSize) *pOutSize = sizeof(PROCESSOR_INFO);
    if (pInfo == NULL || outSize < sizeof(PROCESSOR_INFO)) return FALSE;
    memset(pInfo, 0, sizeof *pInfo);
    pInfo->wVersion = 1;
    CopyString(g_oalIoCtlProcessorCore, pInfo->szProcessCore, sizeof pInfo->szProcessCore, NULL);
    CopyString(g_oalIoCtlProcessorName, pInfo->szProcessorName, sizeof pInfo->szProcessorName, NULL);
    CopyString(g_oalIoCtlProcessorVendor, pInfo->szVendor, sizeof pInfo->szVendor, NULL);
    pInfo->dwInstructionSet = g_oalIoCtlInstructionSet;
    pInfo->dwClockSpeed = g_oalIoCtlClockSpeed;
    return TRUE;
}

//------------------------------------------------------------------------------
BOOL OALIoCtlHalInitRegistry(UINT32 code, VOID *pInpBuffer, UINT32 inpSize, VOID *pOutBuffer, UINT32 outSize, UINT32 *pOutSize)
{
    return TRUE;        // nothing to patch into the registry at boot
}

//==============================================================================
//  Cache info (cache.c fills g_oalCacheInfo in OALCacheGlobalsInit)
//
OAL_CACHE_INFO g_oalCacheInfo;

BOOL OALIoCtlHalGetCacheInfo(UINT32 code, VOID *pInpBuffer, UINT32 inpSize, VOID *pOutBuffer, UINT32 outSize, UINT32 *pOutSize)
{
    CacheInfo *pInfo = (CacheInfo *)pOutBuffer;
    if (pOutSize) *pOutSize = sizeof(CacheInfo);
    if (pInfo == NULL || outSize < sizeof(CacheInfo)) return FALSE;
    memset(pInfo, 0, sizeof *pInfo);
    pInfo->dwL1Flags          = g_oalCacheInfo.L1Flags;
    pInfo->dwL1ICacheSize     = g_oalCacheInfo.L1ISize;
    pInfo->dwL1ICacheLineSize = g_oalCacheInfo.L1ILineSize;
    pInfo->dwL1ICacheNumWays  = g_oalCacheInfo.L1INumWays;
    pInfo->dwL1DCacheSize     = g_oalCacheInfo.L1DSize;
    pInfo->dwL1DCacheLineSize = g_oalCacheInfo.L1DLineSize;
    pInfo->dwL1DCacheNumWays  = g_oalCacheInfo.L1DNumWays;
    pInfo->dwL2Flags          = g_oalCacheInfo.L2Flags;
    return TRUE;
}

//==============================================================================
//  Wake sources (PLATFORM\COMMON\SRC\COMMON\POWER\COMMON\ioctl.c)
//
volatile UINT32 g_oalWakeSource = SYSWAKE_UNKNOWN;
static UINT32 g_oalWakeMask[(SYSINTR_MAXIMUM + 31) / 32];

BOOL OALPowerWakeSource(UINT32 sysIntr)
{
    if (sysIntr >= SYSINTR_MAXIMUM) return FALSE;
    return (g_oalWakeMask[sysIntr >> 5] & (1u << (sysIntr & 31))) != 0;
}

BOOL OALIoCtlHalEnableWake(UINT32 code, VOID *pInpBuffer, UINT32 inpSize, VOID *pOutBuffer, UINT32 outSize, UINT32 *pOutSize)
{
    UINT32 sysIntr;
    if (pInpBuffer == NULL || inpSize < sizeof(UINT32)) return FALSE;
    sysIntr = *(UINT32 *)pInpBuffer;
    if (sysIntr >= SYSINTR_MAXIMUM) return FALSE;
    g_oalWakeMask[sysIntr >> 5] |= 1u << (sysIntr & 31);
    return TRUE;
}

BOOL OALIoCtlHalDisableWake(UINT32 code, VOID *pInpBuffer, UINT32 inpSize, VOID *pOutBuffer, UINT32 outSize, UINT32 *pOutSize)
{
    UINT32 sysIntr;
    if (pInpBuffer == NULL || inpSize < sizeof(UINT32)) return FALSE;
    sysIntr = *(UINT32 *)pInpBuffer;
    if (sysIntr >= SYSINTR_MAXIMUM) return FALSE;
    g_oalWakeMask[sysIntr >> 5] &= ~(1u << (sysIntr & 31));
    return TRUE;
}

BOOL OALIoCtlHalGetWakeSource(UINT32 code, VOID *pInpBuffer, UINT32 inpSize, VOID *pOutBuffer, UINT32 outSize, UINT32 *pOutSize)
{
    if (pOutSize) *pOutSize = sizeof(UINT32);
    if (pOutBuffer == NULL || outSize < sizeof(UINT32)) return FALSE;
    *(UINT32 *)pOutBuffer = g_oalWakeSource;
    return TRUE;
}

//==============================================================================
//  Timer globals (PLATFORM\COMMON\SRC\COMMON\TIMER\COMMON): the state timer.c
//  keeps, the performance counter and the kernel's tick read-out. CE 5.0's
//  kernel takes GetTickCount from the OAL (SC_GetTickCount) and uses it for
//  its own wake-up arithmetic once pOEMUpdateRescheduleTime is set.
//
OAL_TIMER_STATE g_oalTimer;

BOOL OALTimerQueryPerformanceFrequency(LARGE_INTEGER *pFrequency)
{
    if (pFrequency == NULL) return FALSE;
    pFrequency->QuadPart = (LONGLONG)g_oalTimer.countsPerMSec * 1000;
    return TRUE;
}

BOOL OALTimerQueryPerformanceCounter(LARGE_INTEGER *pCounter)
{
    UINT64 base;
    INT32 offset;
    if (pCounter == NULL) return FALSE;
    // a consistent pair of the tick's count and the counts since it
    do {
        base = g_oalTimer.curCounts;
        offset = OALTimerCountsSinceSysTick();
    } while (base != g_oalTimer.curCounts);
    pCounter->QuadPart = (LONGLONG)(base + offset);
    return TRUE;
}

DWORD SC_GetTickCount(void)
{
    DWORD base;
    INT32 offset;
    do {
        base = CurMSec;
        offset = OALTimerCountsSinceSysTick();
    } while (base != CurMSec);
    return base + (offset > 0 ? (DWORD)offset / g_oalTimer.countsPerMSec : 0);
}

//==============================================================================
//  No extension DRAM: the whole window is in config.bib
//
BOOL OEMGetExtensionDRAM(LPDWORD pMemoryStart, LPDWORD pMemoryLength)
{
    return FALSE;
}
