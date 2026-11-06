// Retained-RAM sleep with Clear/ON as the sole wake key.
#include <bsp.h>
#include "classpad_keysc.h"
#include "classpad_power.h"

static ClasspadPowerState g_power;
extern VOID ClasspadSleepUntilInterrupt(volatile unsigned *wake);
extern VOID ClasspadRtcSuspend(void);
extern VOID ClasspadRtcResume(void);

BOOL ClasspadPowerKeyIsr(void)
{
    if (!g_power.active) return FALSE;
    ClasspadPowerKeyInterrupt(&g_power);
    return TRUE; /* no keyboard IST or scheduler while the system is suspended */
}

BOOL ClasspadPowerTimerIsr(void)
{
    if (!g_power.active) return FALSE;
    ClasspadPowerTimerInterrupt(&g_power);
    return TRUE;
}

BOOL OALIoCtlHalPresuspend(UINT32 code, VOID *in, UINT32 insize,
    VOID *out, UINT32 outsize, UINT32 *returned)
{
    UINT32 key = OALIntrTranslateIrq(IRQ_KEYSC);
    if (returned) *returned = 0;
    if (key == SYSINTR_UNDEFINED || !OALPowerWakeSource(key)) {
        NKSetLastError(ERROR_NOT_READY);
        return FALSE;
    }
    return TRUE;
}

VOID OEMPowerOff(void)
{
    UINT32 key = OALIntrTranslateIrq(IRQ_KEYSC);
    if (key == SYSINTR_UNDEFINED || !OALPowerWakeSource(key)) return;
    RETAILMSG(1, (L"CLASSPAD POWER: suspend, Clear/ON wake only\r\n"));
    g_oalWakeSource = SYSWAKE_UNKNOWN;
    ClasspadRtcSuspend();
    ClasspadPowerPrepare(&g_power, g_oalTimer.countsPerMSec);
    while (!g_power.wake) ClasspadSleepUntilInterrupt(&g_power.wake);
    ClasspadPowerRestore(&g_power);
    ClasspadRtcResume();
    g_oalWakeSource = key;
    RETAILMSG(1, (L"CLASSPAD POWER: resumed by Clear/ON\r\n"));
}
