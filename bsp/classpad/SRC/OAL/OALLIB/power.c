//------------------------------------------------------------------------------
//  power.c - OEMPowerOff: SH7305 R-standby through the translator
//
//  docs/06-deep-suspend.md. The translator's HC_STANDBY hypercall powers the
//  core off (STBCR.RSTBY + SLEEP, resume stub in RS memory at BAR) and
//  returns 1 once the core has been reset and brought back; this file decides
//  what wakes the machine and what to put back afterwards:
//    - Clear/ON through the KEYSC autonomous scan (KIUMODEREG 0x0200 with the
//      scan limited to Clear's row and column), the way the stock OS wakes;
//    - the RTC alarm when the kernel armed one (rtc.c OEMSetAlarmTime).
//  Like the OS's loop (ROM 0x80020ed2) a wake-up for any other reason goes
//  straight back to sleep. INTC, KEYSC and the CPG module stops are saved
//  and restored here; the kernel re-inits the system timer through
//  pfnInitClock (timer.c) and flushes the caches around the call; the
//  drivers' power handlers bring the panel, touch sensor and keyboard back.
//  Without R-standby (the translator did not enter it, or SR.BL is set so
//  no hypercall can be issued) the retained-RAM CPU sleep of the first
//  implementation is used, woken by Clear/ON's ordinary KEYSC interrupt.
//------------------------------------------------------------------------------
#include <bsp.h>
#include "classpad_keysc.h"
#include "classpad_power.h"

static ClasspadPowerState g_power;              // the CPU-sleep fallback (classpad_power.h)
extern VOID ClasspadSleepUntilInterrupt(volatile unsigned *wake);
extern VOID ClasspadRtcSuspend(void);
extern VOID ClasspadRtcResume(void);
extern BOOL ClasspadRtcAlarmArmed(void);
extern BOOL ClasspadRtcAlarmFired(void);

#define CP_POWER_MSTPCR2    0xA4150038u
#define STANDBY_NOT_ENTERED ((UINT32)-1)

typedef struct {
    UINT16 ipr[12];
    UINT8  imr[13];
    UINT16 kcnt, kmode, kint, kwset, kinterval, kautofix, krows, kcols;
    UINT32 mstpcr0, mstpcr2;
} CLASSPAD_STANDBY_STATE;
static CLASSPAD_STANDBY_STATE g_sb;

static BOOL ClearDown(void)
{
    return (MMIO_R16(KEYSC_KIUDATA(0)) & CP_CLEAR_BIT) != 0;
}

static void StandbySave(void)
{
    UINT32 i;
    for (i = 0; i < 12; i++) g_sb.ipr[i] = MMIO_R16(INTC_IPR(i));
    for (i = 0; i < 13; i++) g_sb.imr[i] = MMIO_R8(INTC_IMR(i));
    g_sb.kcnt      = MMIO_R16(KEYSC_KIUCNTREG);
    g_sb.kmode     = MMIO_R16(KEYSC_KIUMODEREG);
    g_sb.kint      = MMIO_R16(KEYSC_KIUINTREG) & 0xFF00;
    g_sb.kwset     = MMIO_R16(KEYSC_KIUWSETREG);
    g_sb.kinterval = MMIO_R16(KEYSC_KIUINTERVALREG);
    g_sb.kautofix  = MMIO_R16(KEYSC_KIAUTOFIXREG);
    g_sb.krows     = MMIO_R16(KEYSC_KOUTPINSET);
    g_sb.kcols     = MMIO_R16(KEYSC_KINPINSET);
    g_sb.mstpcr0   = MMIO_R32(CP_POWER_MSTPCR0);
    g_sb.mstpcr2   = MMIO_R32(CP_POWER_MSTPCR2);
}

static void StandbyRestore(void)
{
    UINT32 i;
    MMIO_W32(CP_POWER_MSTPCR0, g_sb.mstpcr0);
    MMIO_W32(CP_POWER_MSTPCR2, g_sb.mstpcr2);
    for (i = 0; i < 12; i++) MMIO_W16(INTC_IPR(i), g_sb.ipr[i]);
    for (i = 0; i < 13; i++) {
        MMIO_W8(INTC_IMR(i), 0xFF);
        MMIO_W8(INTC_IMCR(i), (UINT8)~g_sb.imr[i]);
    }
    MMIO_W16(KEYSC_KIUCNTREG, 0);
    MMIO_W16(KEYSC_KOUTPINSET, g_sb.krows);
    MMIO_W16(KEYSC_KINPINSET, g_sb.kcols);
    MMIO_W16(KEYSC_KIUWSETREG, g_sb.kwset);
    MMIO_W16(KEYSC_KIUINTERVALREG, g_sb.kinterval);
    MMIO_W16(KEYSC_KIAUTOFIXREG, g_sb.kautofix);
    MMIO_W16(KEYSC_KIUMODEREG, g_sb.kmode);
    MMIO_W16(KEYSC_KIUINTREG, g_sb.kint | KEYSC_INT_CLEAR);
    MMIO_W16(KEYSC_KIUCNTREG, g_sb.kcnt);
}

//------------------------------------------------------------------------------
//  One R-standby. Returns the SYSINTR of the wake source, 0 for a wake-up
//  without an accepted reason (sleep again), STANDBY_NOT_ENTERED when the
//  translator could not power the core off.
//
static UINT32 RStandbyOnce(UINT32 keySysIntr)
{
    UINT32 i;

    // Only an armed RTC alarm may reach the INTC; everything else is masked.
    for (i = 0; i < 13; i++) MMIO_W8(INTC_IMR(i), 0xFF);
    if (ClasspadRtcAlarmArmed()) MMIO_W8(INTC_IMCR(10), INTC_IMR10_RTC_ALARM);

    // KEYSC: autonomous scan of Clear's row and column (the OS's standby
    // mode); its ordinary interrupt stays masked at the INTC.
    MMIO_W16(KEYSC_KIUCNTREG, 0);
    MMIO_W16(KEYSC_KOUTPINSET, CP_CLEAR_ROW);
    MMIO_W16(KEYSC_KINPINSET, CP_CLEAR_COL);
    MMIO_W16(KEYSC_KIUMODEREG, KEYSC_MODE_VALUE);
    MMIO_W16(KEYSC_KIUINTREG, KEYSC_INT_ENABLE | KEYSC_INT_CLEAR);
    MMIO_W16(KEYSC_KIUCNTREG, KEYSC_CNT_SCAN_START);

    // The wake-up is a level: a Clear still held from before the suspend
    // would end it at once, so wait (bounded) for its release.
    for (i = 0; i < 2000 && ClearDown(); i++) OALStall(1000);

    if (!ClasspadHcStandby(0)) return STANDBY_NOT_ENTERED;

    // Back with the core reset. Why?
    if (ClearDown()) return keySysIntr;
    if (ClasspadRtcAlarmFired()) return SYSINTR_RTC_ALARM;
    return 0;
}

BOOL ClasspadPowerKeyIsr(void)
{
    if (!g_power.active) return FALSE;
    ClasspadPowerKeyInterrupt(&g_power);
    return TRUE; /* no keyboard IST or scheduler while the system is suspended */
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
    UINT32 key = OALIntrTranslateIrq(IRQ_KEYSC), reason;
    if (key == SYSINTR_UNDEFINED || !OALPowerWakeSource(key)) return;
    RETAILMSG(1, (L"CLASSPAD POWER: suspend (R-standby, Clear/ON%s wake)\r\n",
                  ClasspadRtcAlarmArmed() ? L" or RTC alarm" : L""));
    g_oalWakeSource = SYSWAKE_UNKNOWN;
    ClasspadRtcSuspend();

    reason = STANDBY_NOT_ENTERED;
    if (ClasspadCanTrap()) {
        StandbySave();
        do reason = RStandbyOnce(key); while (reason == 0);
        StandbyRestore();
    }
    if (reason == STANDBY_NOT_ENTERED) {
        RETAILMSG(1, (L"CLASSPAD POWER: no R-standby, CPU sleep with Clear/ON wake\r\n"));
        ClasspadPowerPrepare(&g_power);
        while (!g_power.wake) ClasspadSleepUntilInterrupt(&g_power.wake);
        ClasspadPowerRestore(&g_power);
        reason = key;
    }

    ClasspadRtcResume();
    g_oalWakeSource = reason;
    RETAILMSG(1, (L"CLASSPAD POWER: resumed by %s\r\n", reason == SYSINTR_RTC_ALARM ? L"the RTC alarm" : L"Clear/ON"));
}
