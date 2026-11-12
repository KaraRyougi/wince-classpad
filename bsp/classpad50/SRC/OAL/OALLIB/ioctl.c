//------------------------------------------------------------------------------
//  ioctl.c - the OAL IOCTL table and platform strings (the dispatcher,
//  OEMIoControl, and the generic handlers are in oalcommon.c)
//------------------------------------------------------------------------------
#include <bsp.h>
#include "classpad_clock.h"

LPCWSTR g_oalIoCtlPlatformType = IOCTL_PLATFORM_TYPE;
LPCWSTR g_oalIoCtlPlatformOEM  = IOCTL_PLATFORM_OEM;

LPCWSTR g_oalIoCtlProcessorVendor = IOCTL_PROCESSOR_VENDOR;
LPCWSTR g_oalIoCtlProcessorName   = IOCTL_PROCESSOR_NAME;
LPCWSTR g_oalIoCtlProcessorCore   = IOCTL_PROCESSOR_CORE;

UINT32 g_oalIoCtlInstructionSet = IOCTL_PROCESSOR_INSTRUCTION_SET;
UINT32 g_oalIoCtlClockSpeed = IOCTL_PROCESSOR_CLOCK_SPEED;

//------------------------------------------------------------------------------
//  Reboot: under the translator, end the run; the exit code 0x7E marks it.
//
BOOL OALIoCtlHalReboot(
    UINT32 code, VOID *pInpBuffer, UINT32 inpSize, VOID *pOutBuffer,
    UINT32 outSize, UINT32 *pOutSize
) {
    if (ClasspadCanTrap()) ClasspadHcExit(0x7E);
    for (;;) __asm("sleep");
    return FALSE;
}

//------------------------------------------------------------------------------
//  IOCTL_HAL_POSTINIT: the kernel is up (SystemStartupFunc in schedule.c,
//  once the system has been initialised). A retail kernel prints nothing
//  itself, so the OAL marks the milestone; under the translator it is the
//  sign that the boot got past the kernel.
//
BOOL OALIoCtlHalPostInit(
    UINT32 code, VOID *pInpBuffer, UINT32 inpSize, VOID *pOutBuffer,
    UINT32 outSize, UINT32 *pOutSize
) {
    RETAILMSG(1, (L"CLASSPAD OAL: POSTINIT (kernel initialised)\r\n"));
    return TRUE;
}

//------------------------------------------------------------------------------
//  Clock control for cetune.exe (SRC\APPS\CETUNE, classpad_clock.h). The
//  program computes a setting, the OAL writes it: FLLFRQ, FRQCR and the two
//  wait fields that follow the bus clock. The OAL also owns what hangs on
//  Pphi, the TMU0 tick (timer.c). A driver that derives a rate from Pphi
//  reads it with OALClockPphi (kernel) or IOCTL_HAL_CLASSPAD_CLOCK_GET.
//
static ClasspadClockRegs s_bootClock, s_lastClock;
static BOOL s_haveClock;

static UINT32 NominalPphi(const ClasspadClockRegs *regs)
{
    ClasspadClockChain chain;
    unsigned hz[6];
    ClockDecode(regs, &chain);
    ClockHz(&chain, MMIO_R32(CPCLK_PLLCR) & CPCLK_PLLCR_ON, hz);
    return hz[5];
}

//  InitClock (timer.c) asks at boot and after every resume: are the
//  multipliers and dividers still what the OAL last saw? The first call
//  records the setting the loader left (cetune's preset F1).
BOOL OALClockChanged(void)
{
    ClasspadClockRegs now;
    BOOL changed;

    ClockRead(&now);
    if (!s_haveClock) {
        s_haveClock = TRUE;
        s_bootClock = s_lastClock = now;
        return FALSE;
    }
    changed = ((now.fllfrq ^ s_lastClock.fllfrq) & CPCLK_FLLFRQ_FIELDS) != 0
           || ((now.frqcr ^ s_lastClock.frqcr) & CPCLK_FRQCR_FIELDS) != 0;
    s_lastClock = now;
    return changed;
}

UINT32 OALClockPphi(void)
{
    return OALTimerCountHz() * TIMER_INPUT_DIVISOR;
}

BOOL OALIoCtlHalClasspadClockGet(
    UINT32 code, VOID *pInpBuffer, UINT32 inpSize, VOID *pOutBuffer,
    UINT32 outSize, UINT32 *pOutSize
) {
    ClasspadClockInfo info;

    if (pOutSize) *pOutSize = sizeof info;
    if (!pOutBuffer || outSize < sizeof info) {
        NKSetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    ClockRead(&info.cur);
    info.boot  = s_bootClock;
    info.pllcr = MMIO_R32(CPCLK_PLLCR);
    info.pphi  = OALClockPphi();
    memcpy(pOutBuffer, &info, sizeof info);
    return TRUE;
}

BOOL OALIoCtlHalClasspadClockSet(
    UINT32 code, VOID *pInpBuffer, UINT32 inpSize, VOID *pOutBuffer,
    UINT32 outSize, UINT32 *pOutSize
) {
    ClasspadClockRegs want;
    UINT32 before, after;
    BOOL enabled;

    if (pOutSize) *pOutSize = 0;
    if (!pInpBuffer || inpSize < sizeof want) {
        NKSetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    memcpy(&want, pInpBuffer, sizeof want);
    if (!ClockValid(&want, MMIO_R32(CPCLK_PLLCR) & CPCLK_PLLCR_ON)) {
        NKSetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    enabled = INTERRUPTS_ENABLE(FALSE);
    ClockRead(&s_lastClock);
    before = NominalPphi(&s_lastClock);
    ClockApply(&want);
    ClockRead(&s_lastClock);
    after = NominalPphi(&s_lastClock);
    OALTimerRetune(after, before);
    INTERRUPTS_ENABLE(enabled);

    RETAILMSG(1, (L"CLASSPAD OAL: clock FLLFRQ=%08x FRQCR=%08x CS0WCR=%08x CS3WCR=%08x, Pphi %u Hz\r\n",
                  s_lastClock.fllfrq, s_lastClock.frqcr, s_lastClock.cs0wcr, s_lastClock.cs3wcr, OALClockPphi()));
    return TRUE;
}

const OAL_IOCTL_HANDLER g_oalIoCtlTable[] = {
#include "ioctl_tab.h"
};
