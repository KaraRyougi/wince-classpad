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
//  InitClock (timer.c) and flushes the caches around the call; the
//  drivers' power handlers bring the panel, touch sensor and keyboard back.
//  Without R-standby (the translator did not enter it, or SR.BL is set so
//  no hypercall can be issued) the retained-RAM CPU sleep of the first
//  implementation is used, woken by Clear/ON's ordinary KEYSC interrupt.
//------------------------------------------------------------------------------
#include <bsp.h>
#include "classpad_keysc.h"
#include "classpad_power.h"
#include "classpad_pwrstat.h"
#include "classpad_lcd.h"
#include "classpad_pwrseq.h"

static ClasspadPowerState g_power;              // the CPU-sleep fallback (classpad_power.h)
extern VOID ClasspadSleepUntilInterrupt(volatile unsigned *wake);
extern VOID ClasspadRtcSuspend(void);
extern VOID ClasspadRtcResume(void);
extern BOOL ClasspadRtcAlarmArmed(void);
extern BOOL ClasspadRtcAlarmFired(void);
extern UINT32 ClasspadRtcSuspendSeconds(void);

// What the last suspend did, for the Battery program (classpad_pwrstat.h)
static CLASSPAD_SUSPEND_STATS g_stats;

static void WakeSnapshot(CLASSPAD_WAKE_SNAPSHOT *s)
{
    s->rcr1     = MMIO_R8(RTC_RCR1);
    s->rcr2     = MMIO_R8(RTC_RCR2);
    s->intreq00 = MMIO_R8(0xA4140024);          // INTREQ00: IRQ0..7 pin requests
    s->intmsk00 = MMIO_R8(0xA4140044);          // INTMSK00
    s->kiuint   = MMIO_R16(KEYSC_KIUINTREG);
    s->kiudata0 = MMIO_R16(KEYSC_KIUDATA(0));
    s->kiustate = MMIO_R16(SH7305_REG_PA_KEYSC + 0x12);
    s->cmstr    = MMIO_R16(0xA44A0000);         // CMT: CMSTR
    s->cmcsr    = MMIO_R16(0xA44A0060);         //      CMCSR
    s->rwtcsr   = MMIO_R16(0xA4520004);         // RWDT: RWTCSR
    s->r64cnt   = MMIO_R8(RTC_R64CNT);
}

//------------------------------------------------------------------------------
//  The hardware around the standby, the way Casio's OS does it (docs/08,
//  2026-11-10; OS 02.01.7003 0x800810CC and 0x80020E4C). CE's suspend used
//  to enter R-standby with every pin as it was while running and the panel
//  only asleep. The OS, before its SLEEP:
//    - sets up every port for power-off (0x800812C4: pull-ups, pin
//      functions, port control registers: g_pinsOff);
//    - clears the RTC's carry interrupt enable and periodic setting;
//    - a few more pin steps (0x80020E66.., 0x80020E9E..: after the table
//      above only PFDR bit 7 and 0xA4050162 bit 2 still change);
//    - powers the panel down (0x80038A9A): deep standby, then PHDR bit 5 low
//      (the panel's supply: the power-on raises it between the reset pulse's
//      two edges) and 0xA44C0020 bit 2 off (the module the LCD interface
//      set-up 0x8008138C starts).
//  After the wake-up the OS writes its running pin set-up again, restarts
//  that module and runs the panel's cold power-on (0x80038018: reset, power,
//  the whole register set-up). Here every register is read before it is
//  changed and written back afterwards, in reverse order, and the panel's
//  power-on is replayed from classpad_pwrseq.h up to the point where the
//  display driver's own resume takes over (SLPOUT again, its backlight
//  level, the repaint, DISPON).
//  One thing differs on purpose. The OS arms two IRQ pins as wake-up
//  sources (0x8006F7DA): IRQ1, always (0x8006F63A: priority 12, unmasked;
//  falling edge, ICR1 set by 0x80062100), whose handler 0x8006F6D8 resets
//  the USB cable state machine's variable 0x8C1F6F48 and posts an event:
//  the USB cable being plugged in; and IRQ2 (0x8006F668: priority 5), only
//  under the conditions of 0x8006F78C on the communication settings: a
//  wake-up by the serial link. CE inherits IRQ1 unmasked at priority 12 and
//  has no handler for either: an edge latched in INTREQ00 that nothing
//  clears is a pending, unmasked interrupt, and R-standby ends at once
//  every time it is entered. All eight IRQ pins are masked for the standby.
//  Only when the display driver has named the panel (IOCTL_HAL_CLASSPAD_PANEL):
//  the tables are the fx-CP400's.
//
#define PORT_PADR       0xA4050120      // bit 4: the panel's reset line
#define PORT_PFDR       0xA405012A
#define PORT_PHDR       0xA405012E      // bit 5: the panel's supply
#define PORT_X162       0xA4050162
#define LCDIF_CR4       0xA44C0004      // the LCD interface's module (16-bit)
#define LCDIF_CR6       0xA44C0006
#define LCDIF_START     0xA44C0020      // bit 2
#define ADC_ADCCSR      0xA461008A
#define ADC_ADCUST      0xA461008C
#define ADC_X8E         0xA461008E
#define INTC_INTMSK00   0xA4140044
#define INTC_INTMSKCLR00 0xA4140064

static DWORD g_panelKind = CLASSPAD_PANEL_NONE;
static UINT32 g_pinSaved[sizeof g_pinsOff / sizeof g_pinsOff[0]];
static struct {
    UINT8 pfdr, phdr, x162, lcdStart, lcdCr6, intmsk;
    UINT16 lcdCr4, adcX8e;
} g_hw;

BOOL OALIoCtlHalClasspadPanel(UINT32 code, VOID *in, UINT32 insize,
    VOID *out, UINT32 outsize, UINT32 *returned)
{
    if (returned) *returned = 0;
    if (!in || insize < sizeof(DWORD)) return FALSE;
    g_panelKind = *(DWORD *)in;
    RETAILMSG(1, (L"CLASSPAD POWER: panel kind %u from the display driver\r\n", g_panelKind));
    return TRUE;
}

// Milliseconds on the RTC's 128 Hz counter: nothing else counts here
// (interrupts are off, TMU0 is reset by the standby). Bounded in reads.
static void WaitMs(UINT32 ms)
{
    UINT32 ticks = (ms * 128 + 999) / 1000 + 1, last = MMIO_R8(RTC_R64CNT) & 0x7F, now, n = 0, reads;
    for (reads = 0; n < ticks && reads < 40000000; reads++) {
        now = MMIO_R8(RTC_R64CNT) & 0x7F;
        n += (now - last) & 0x7F;
        last = now;
    }
}

static UINT32 PinRead(UINT32 addr, UINT8 size)
{
    return size == 1 ? MMIO_R8(addr) : size == 2 ? MMIO_R16(addr) : MMIO_R32(addr);
}
static void PinWrite(UINT32 addr, UINT8 size, UINT32 v)
{
    if (size == 1) MMIO_W8(addr, v); else if (size == 2) MMIO_W16(addr, v); else MMIO_W32(addr, v);
}

static void LcdPlay(const UINT16 *s, UINT32 count)
{
    UINT32 i;
    for (i = 0; i < count; i++) {
        UINT16 c = s[i];
        if (c == LCDSEQ_RESET_LO)       MMIO_W8(PORT_PADR, MMIO_R8(PORT_PADR) & ~0x10);
        else if (c == LCDSEQ_POWER_ON)  MMIO_W8(PORT_PHDR, MMIO_R8(PORT_PHDR) | 0x20);
        else if (c == LCDSEQ_RESET_HI)  MMIO_W8(PORT_PADR, MMIO_R8(PORT_PADR) | 0x10);
        else if (c & LCDSEQ_DELAY)      WaitMs(c & 0x0FFF);
        else if (c & LCDSEQ_CMD)        LcdCommand((UCHAR)c);
        else                            LcdParam((UCHAR)c);
    }
}

static BOOL HardwareOff(void)
{
    UINT32 i;
    if (g_panelKind != CLASSPAD_PANEL_NEW && g_panelKind != CLASSPAD_PANEL_OLD) return FALSE;

    // the IRQ pins: none of them wakes CE
    g_hw.intmsk = MMIO_R8(INTC_INTMSK00);
    MMIO_W8(INTC_INTMSK00, 0xFF);

    // the ports
    for (i = 0; i < sizeof g_pinsOff / sizeof g_pinsOff[0]; i++) {
        const CLASSPAD_PIN_OP *op = &g_pinsOff[i];
        g_pinSaved[i] = PinRead(op->addr, op->size);
        PinWrite(op->addr, op->size, (g_pinSaved[i] & op->andMask) | op->orValue);
    }
    g_hw.pfdr = MMIO_R8(PORT_PFDR); g_hw.x162 = MMIO_R8(PORT_X162);
    MMIO_W8(PORT_PFDR, g_hw.pfdr | 0x80);
    MMIO_W8(PORT_X162, g_hw.x162 | 0x04);

    // the A/D converter as the OS leaves it (0x800813A0; that routine's
    // other half, the touch sensor's power pin, is the touch driver's)
    g_hw.adcX8e = MMIO_R16(ADC_X8E);
    MMIO_W16(ADC_ADCCSR, MMIO_R16(ADC_ADCCSR) & 0xDFFF);
    MMIO_W16(ADC_ADCUST, 0x8000);
    MMIO_W16(ADC_X8E, (g_hw.adcX8e & 0x0FFF) | 0x8000);

    // the panel: the driver's power handler has sent backlight 0, DISPOFF
    // and SLPIN; the OS waits 100 (120) ms after SLPIN before the deep standby
    g_hw.lcdCr4 = MMIO_R16(LCDIF_CR4); g_hw.lcdCr6 = MMIO_R8(LCDIF_CR6); g_hw.lcdStart = MMIO_R8(LCDIF_START);
    g_hw.phdr = MMIO_R8(PORT_PHDR);
    WaitMs(120);
    if (g_panelKind == CLASSPAD_PANEL_NEW) LcdPlay(g_lcdOffNew, sizeof g_lcdOffNew / sizeof g_lcdOffNew[0]);
    else                                   LcdPlay(g_lcdOffOld, sizeof g_lcdOffOld / sizeof g_lcdOffOld[0]);
    MMIO_W8(PORT_PHDR, MMIO_R8(PORT_PHDR) & ~0x20);
    MMIO_W8(LCDIF_START, MMIO_R8(LCDIF_START) & ~0x04);

    // the RTC: no carry interrupt, flags clear; the alarm enable stays
    MMIO_W8(RTC_RCR1, MMIO_R8(RTC_RCR1) & 0x0F);
    return TRUE;
}

static void HardwareOn(void)
{
    UINT32 i;
    MMIO_W16(ADC_X8E, g_hw.adcX8e);              // the battery driver sets the rest up before every conversion
    MMIO_W8(PORT_X162, g_hw.x162);
    MMIO_W8(PORT_PFDR, g_hw.pfdr);
    for (i = sizeof g_pinsOff / sizeof g_pinsOff[0]; i-- > 0; )
        PinWrite(g_pinsOff[i].addr, g_pinsOff[i].size, g_pinSaved[i]);

    // the LCD interface's module as it was (its clock, MSTPCR2, is back: StandbyRestore), then the panel from cold
    MMIO_W16(LCDIF_CR4, g_hw.lcdCr4);
    MMIO_W8(LCDIF_CR6, g_hw.lcdCr6);
    if (g_panelKind == CLASSPAD_PANEL_NEW) LcdPlay(g_lcdOnNew, sizeof g_lcdOnNew / sizeof g_lcdOnNew[0]);
    else                                   LcdPlay(g_lcdOnOld, sizeof g_lcdOnOld / sizeof g_lcdOnOld[0]);
    MMIO_W8(LCDIF_START, g_hw.lcdStart);
    if (!(g_hw.phdr & 0x20)) MMIO_W8(PORT_PHDR, MMIO_R8(PORT_PHDR) & ~0x20);   // was off before: leave it as it was

    MMIO_W8(INTC_INTMSK00, 0xFF);
    MMIO_W8(INTC_INTMSKCLR00, (UINT8)~g_hw.intmsk);
}

BOOL OALIoCtlHalClasspadSuspendStats(UINT32 code, VOID *in, UINT32 insize,
    VOID *out, UINT32 outsize, UINT32 *returned)
{
    if (returned) *returned = 0;
    if (!out || outsize < sizeof g_stats) return FALSE;
    g_stats.size = sizeof g_stats;
    memcpy(out, &g_stats, sizeof g_stats);
    if (returned) *returned = sizeof g_stats;
    return TRUE;
}

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
    MMIO_W16(KEYSC_KIUINTREG, KEYSC_INT_CLEAR);     // the OS's order: flags, then enables
    MMIO_W16(KEYSC_KIUINTREG, KEYSC_INT_ENABLE);
    MMIO_W16(KEYSC_KIUCNTREG, KEYSC_CNT_SCAN_START);

    // The wake-up is a level: a Clear still held from before the suspend
    // would end it at once, so wait (bounded, 1 s) for its release. Then
    // clear the KEYSC event flags (the scan start above sets one) so that
    // after the wake-up they mean "a key event happened while the core was
    // off". The bound is counted on the RTC's R64CNT (128 Hz), not OALStall:
    // from the second pass of the loop in OEMPowerOff, TMU0 has been reset
    // by R-standby (the kernel re-inits it only after OEMPowerOff returns)
    // and OALStall would spin for ever on a stopped TCNT0.
    {
        UINT32 last = MMIO_R8(RTC_R64CNT) & 0x7F, now, ticks = 0;
        while (ticks < 128 && ClearDown()) {
            now = MMIO_R8(RTC_R64CNT) & 0x7F;
            ticks += (now - last) & 0x7F;
            last = now;
        }
    }
    MMIO_W16(KEYSC_KIUINTREG, KEYSC_INT_CLEAR);     // two writes: the flag byte must read 0 afterwards
    MMIO_W16(KEYSC_KIUINTREG, KEYSC_INT_ENABLE);    // whether the flags are write-1-to-clear or plain bits (casio-emu)

    if (!ClasspadHcStandby(0)) return STANDBY_NOT_ENTERED;

    // Back with the core reset. Why? The OS checks the Clear level right
    // after its restart; here the translator re-translates the resumed
    // code first, so a short press can be over by now: the KEYSC event
    // flags (key down/up since the clear above) count as the press too.
    if (ClasspadRtcAlarmFired()) return SYSINTR_RTC_ALARM;
    if (ClearDown() || (MMIO_R16(KEYSC_KIUINTREG) & 0x00FF) != 0) return keySysIntr;
    return 0;
}

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
    if (key == SYSINTR_UNDEFINED || !OALPowerWakeSource(key)) return FALSE;
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
    g_stats.suspends++;
    g_stats.path = CLASSPAD_SUSPEND_PATH_NONE;
    g_stats.entries = g_stats.unexplained = g_stats.wokeByAlarm = g_stats.hardwareOff = 0;
    g_stats.seconds = 0xFFFFFFFF;

    reason = STANDBY_NOT_ENTERED;
    if (ClasspadCanTrap()) {
        BOOL hw;
        StandbySave();
        hw = HardwareOff();
        g_stats.hardwareOff = hw;
        do {
            reason = RStandbyOnce(key);
            if (reason != STANDBY_NOT_ENTERED) {
                g_stats.path = CLASSPAD_SUSPEND_PATH_RSTANDBY;
                g_stats.entries++;
            }
            if (reason == 0) {                  // nobody asked for this wake-up: what do the wake sources say?
                if (g_stats.unexplained++ == 0) WakeSnapshot(&g_stats.first);
                WakeSnapshot(&g_stats.last);
            }
        } while (reason == 0);
        StandbyRestore();
        if (hw) HardwareOn();
    }
    if (reason == STANDBY_NOT_ENTERED) {
        g_stats.path = CLASSPAD_SUSPEND_PATH_CPUSLEEP;
        RETAILMSG(1, (L"CLASSPAD POWER: no R-standby, CPU sleep with Clear/ON wake\r\n"));
        ClasspadPowerPrepare(&g_power, g_oalTimer.countsPerMSec);
        while (!g_power.wake) ClasspadSleepUntilInterrupt(&g_power.wake);
        ClasspadPowerRestore(&g_power);
        reason = key;
    }

    ClasspadRtcResume();
    g_stats.seconds = ClasspadRtcSuspendSeconds();
    g_stats.wokeByAlarm = reason == SYSINTR_RTC_ALARM;
    g_oalWakeSource = reason;
    RETAILMSG(1, (L"CLASSPAD POWER: resumed by %s\r\n", reason == SYSINTR_RTC_ALARM ? L"the RTC alarm" : L"Clear/ON"));
}
