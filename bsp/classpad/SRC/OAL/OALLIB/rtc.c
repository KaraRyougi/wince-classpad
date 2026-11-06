//------------------------------------------------------------------------------
//  rtc.c - real-time clock
//
//  A software clock driven by the system tick for now (the SH7305 RTC at
//  0xA413FEC0 comes later). Time is kept as seconds since 2000-01-01 plus the
//  millisecond tick at which it was set.
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
BOOL OEMSetAlarmTime(LPSYSTEMTIME lpst)
{
    // No alarm source yet
    return FALSE;
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
static BOOL ReadHardwareSeconds(UINT64 *seconds)
{
    SYSTEMTIME t;
    UINT8 sec;
    UINT32 attempts;
    if (!(MMIO_R8(SH7305_REG_PA_RTC + 0x1E) & 1)) return FALSE;
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
