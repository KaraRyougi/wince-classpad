//------------------------------------------------------------------------------
//  rtc.c - real-time clock
//
//  A software clock driven by the system tick: seconds since 2000-01-01 plus
//  the millisecond tick at which it was set. The SH7305 RTC (0xA413FEC0) is
//  used for what the tick cannot do: the time spent suspended and the kernel
//  alarm. Its calendar counters are stopped when CE starts (RCR2.START = 0:
//  the ClassPad has no clock of its own; only the 64 Hz divider runs), so
//  ClasspadRtcStart starts them at boot. Until 2026-11-10 nothing did, every
//  calendar read failed and the clock stood still across a suspend.
//------------------------------------------------------------------------------
#include <bsp.h>

static UINT64 g_baseSeconds;        // seconds since 2000-01-01 00:00:00 at g_baseTick
static UINT32 g_baseTick;           // CurMSec when the clock was set
static UINT16 g_baseMillis;

static const UINT8 g_daysInMonth[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

static BOOL IsLeap(UINT32 y) { return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0); }

static void SecondsToSystemTime(UINT64 secs, UINT16 millis, LPSYSTEMTIME pst)
{
    UINT32 days = (UINT32)(secs / 86400);
    UINT32 rem  = (UINT32)(secs % 86400);
    UINT32 year = 2000, month = 0;

    pst->wHour   = (WORD)(rem / 3600);
    pst->wMinute = (WORD)((rem % 3600) / 60);
    pst->wSecond = (WORD)(rem % 60);
    pst->wMilliseconds = millis;
    pst->wDayOfWeek = (WORD)((days + 6) % 7);           // 2000-01-01 was a Saturday

    for (;;) {
        UINT32 inYear = IsLeap(year) ? 366 : 365;
        if (days < inYear) break;
        days -= inYear;
        year++;
    }
    for (month = 0; month < 12; month++) {
        UINT32 inMonth = g_daysInMonth[month] + ((month == 1 && IsLeap(year)) ? 1 : 0);
        if (days < inMonth) break;
        days -= inMonth;
    }
    pst->wYear  = (WORD)year;
    pst->wMonth = (WORD)(month + 1);
    pst->wDay   = (WORD)(days + 1);
}

static BOOL SystemTimeToSeconds(const SYSTEMTIME *pst, UINT64 *pSecs)
{
    UINT32 y, m, days = 0;
    if (pst->wYear < 2000 || pst->wMonth < 1 || pst->wMonth > 12 || pst->wDay < 1 || pst->wDay > 31 ||
        pst->wHour > 23 || pst->wMinute > 59 || pst->wSecond > 59) return FALSE;
    for (y = 2000; y < pst->wYear; y++) days += IsLeap(y) ? 366 : 365;
    for (m = 0; m + 1 < pst->wMonth; m++) days += g_daysInMonth[m] + ((m == 1 && IsLeap(pst->wYear)) ? 1 : 0);
    days += pst->wDay - 1;
    *pSecs = (UINT64)days * 86400 + pst->wHour * 3600 + pst->wMinute * 60 + pst->wSecond;
    return TRUE;
}

//------------------------------------------------------------------------------
BOOL OEMGetRealTime(LPSYSTEMTIME lpst)
{
    UINT32 elapsed = CurMSec - g_baseTick + g_baseMillis;
    SecondsToSystemTime(g_baseSeconds + elapsed / 1000, (UINT16)(elapsed % 1000), lpst);
    return TRUE;
}

//------------------------------------------------------------------------------
BOOL OEMSetRealTime(LPSYSTEMTIME lpst)
{
    UINT64 secs;
    if (!SystemTimeToSeconds(lpst, &secs)) return FALSE;
    g_baseSeconds = secs;
    g_baseMillis  = (UINT16)(lpst->wMilliseconds % 1000);
    g_baseTick    = CurMSec;
    return TRUE;
}

//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
//  Kernel alarm (SetKernelAlarm) on the hardware RTC's alarm registers. The
//  hardware RTC keeps Casio's calendar, CE's clock is g_baseSeconds + the tick,
//  so the alarm is programmed as "hardware now + (alarm - CE now)". Its
//  interrupt (INTEVT 0xA80, intr.c) is one-shot; it is also the wake-up from
//  R-standby (power.c), where the RTC keeps running in the RCLK domain.
//
static BOOL ReadHardwareSeconds(UINT64 *seconds);
static void SecondsToSystemTime(UINT64 secs, UINT16 millis, LPSYSTEMTIME pst);
static BOOL g_alarmArmed;
static UINT8 ToBcd(UINT32 v) { return (UINT8)(((v / 10) << 4) | (v % 10)); }

BOOL OEMSetAlarmTime(LPSYSTEMTIME lpst)
{
    UINT64 target, now, hwNow;
    UINT32 elapsed;
    SYSTEMTIME t;
    if (lpst == NULL) {                                     // cancel
        MMIO_W8(RTC_RCR1, 0);
        MMIO_W8(INTC_IMR(10), INTC_IMR10_RTC_ALARM);
        g_alarmArmed = FALSE;
        return TRUE;
    }
    if (!SystemTimeToSeconds(lpst, &target) || !ReadHardwareSeconds(&hwNow)) return FALSE;
    elapsed = CurMSec - g_baseTick + g_baseMillis;
    now = g_baseSeconds + elapsed / 1000;
    if (target <= now) target = now + 1;
    SecondsToSystemTime(hwNow + (target - now), 0, &t);
    MMIO_W8(RTC_RCR1, 0);                                   // AIE off, AF clear while programming
    MMIO_W8(RTC_RSECAR, (UINT8)(ToBcd(t.wSecond) | 0x80));
    MMIO_W8(RTC_RMINAR, (UINT8)(ToBcd(t.wMinute) | 0x80));
    MMIO_W8(RTC_RHRAR,  (UINT8)(ToBcd(t.wHour) | 0x80));
    MMIO_W8(RTC_RWKAR,  0);
    MMIO_W8(RTC_RDAYAR, (UINT8)(ToBcd(t.wDay) | 0x80));
    MMIO_W8(RTC_RMONAR, (UINT8)(ToBcd(t.wMonth) | 0x80));
    MMIO_W8(RTC_RCR3,   0);                                 // the year is not compared
    MMIO_W8(RTC_RCR1, RTC_RCR1_AIE);
    MMIO_W8(INTC_IMCR(10), INTC_IMR10_RTC_ALARM);
    g_alarmArmed = TRUE;
    return TRUE;
}

BOOL ClasspadRtcAlarmArmed(void) { return g_alarmArmed && (MMIO_R8(RTC_RCR1) & RTC_RCR1_AIE) != 0; }
BOOL ClasspadRtcAlarmFired(void) { return (MMIO_R8(RTC_RCR1) & RTC_RCR1_AF) != 0; }

UINT32 ClasspadRtcAlarmIsr(void)                            // intr.c, INTEVT 0xA80
{
    MMIO_W8(RTC_RCR1, 0);                                   // AF cleared, AIE off: one-shot
    MMIO_W8(INTC_IMR(10), INTC_IMR10_RTC_ALARM);
    g_alarmArmed = FALSE;
    return SYSINTR_RTC_ALARM;
}

//------------------------------------------------------------------------------
//  IOCTL_HAL_INIT_RTC: called once at boot with the default time
//
BOOL OALIoCtlHalInitRTC(
    UINT32 code, VOID *pInpBuffer, UINT32 inpSize, VOID *pOutBuffer,
    UINT32 outSize, UINT32 *pOutSize
) {
    if (pInpBuffer != NULL && inpSize >= sizeof(SYSTEMTIME)) {
        return OEMSetRealTime((LPSYSTEMTIME)pInpBuffer);
    }
    return TRUE;
}

/* The TMU tick stops during suspend. Account for elapsed calendar time
 * using the battery-backed hardware RTC, without changing Casio's clock. */
static UINT64 g_suspendRtc;
static BOOL g_suspendRtcValid;
static UINT32 FromBcd(UINT32 v)
{
    return (v & 15) + ((v >> 4) & 15) * 10 + ((v >> 8) & 15) * 100 + ((v >> 12) & 15) * 1000;
}
#define RTC_RCR2_START  0x01
#define RTC_RCR2_RESET  0x02            // the divider below one second
#define RTC_RCR2_RTCEN  0x08
#define RTC_RCR2_PES    0x70            // the periodic interrupt setting, kept as it is

// The calendar as seconds since 2000, whether it is counting or not
static BOOL ReadCalendar(UINT64 *seconds)
{
    SYSTEMTIME t;
    UINT8 sec;
    UINT32 attempts;
    for (attempts = 0; attempts < 4; ++attempts) {
        memset(&t, 0, sizeof t);
        sec = MMIO_R8(SH7305_REG_PA_RTC + 2);
        t.wSecond = (WORD)FromBcd(sec);
        t.wMinute = (WORD)FromBcd(MMIO_R8(SH7305_REG_PA_RTC + 4));
        t.wHour = (WORD)FromBcd(MMIO_R8(SH7305_REG_PA_RTC + 6));
        t.wDay = (WORD)FromBcd(MMIO_R8(SH7305_REG_PA_RTC + 10));
        t.wMonth = (WORD)FromBcd(MMIO_R8(SH7305_REG_PA_RTC + 12));
        t.wYear = (WORD)FromBcd(MMIO_R16(SH7305_REG_PA_RTC + 14));
        if (t.wYear < 100) t.wYear += 2000;
        if (sec == MMIO_R8(SH7305_REG_PA_RTC + 2)) return SystemTimeToSeconds(&t, seconds);
    }
    return FALSE;
}
static BOOL ReadHardwareSeconds(UINT64 *seconds)
{
    if (!(MMIO_R8(RTC_RCR2) & RTC_RCR2_START)) return FALSE;
    return ReadCalendar(seconds);
}

// OEMInit: the calendar counters count from here on. A calendar that holds a
// date is started as it is; one that does not (never set) starts at
// 2000-01-01 00:00:00. Only differences of it are used (suspend, alarm), so
// what it shows does not matter, and CE's own date is not taken from it.
VOID ClasspadRtcStart(void)
{
    UINT8 rcr2 = MMIO_R8(RTC_RCR2);
    UINT64 seconds;
    BOOL kept;
    if (rcr2 & RTC_RCR2_START) return;
    kept = ReadCalendar(&seconds);
    if (!kept) {
        MMIO_W8(RTC_RCR2, (UINT8)((rcr2 & RTC_RCR2_PES) | RTC_RCR2_RTCEN | RTC_RCR2_RESET));
        MMIO_W8(SH7305_REG_PA_RTC + 2, 0x00);       // second
        MMIO_W8(SH7305_REG_PA_RTC + 4, 0x00);       // minute
        MMIO_W8(SH7305_REG_PA_RTC + 6, 0x00);       // hour
        MMIO_W8(SH7305_REG_PA_RTC + 8, 6);          // day of the week: Saturday
        MMIO_W8(SH7305_REG_PA_RTC + 10, 0x01);      // day
        MMIO_W8(SH7305_REG_PA_RTC + 12, 0x01);      // month
        MMIO_W16(SH7305_REG_PA_RTC + 14, 0x2000);   // year
    }
    MMIO_W8(RTC_RCR2, (UINT8)((rcr2 & RTC_RCR2_PES) | RTC_RCR2_RTCEN | RTC_RCR2_START));
    RETAILMSG(1, (L"CLASSPAD RTC: calendar counters started (RCR2 was %02X, %s)\r\n",
                  rcr2, kept ? L"date kept" : L"set to 2000-01-01"));
}

VOID ClasspadRtcSuspend(void)
{
    g_suspendRtcValid = ReadHardwareSeconds(&g_suspendRtc);
}
VOID ClasspadRtcResume(void)
{
    UINT64 now;
    if (g_suspendRtcValid && ReadHardwareSeconds(&now) && now >= g_suspendRtc)
        g_baseSeconds += now - g_suspendRtc;
}
