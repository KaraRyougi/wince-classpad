// ClassPad battery status and battery type (alkaline / Ni-MH), like the
// "Battery Settings" of Casio's OS. The type is the battery driver's registry
// value HKLM\Software\ClassPad\Battery "Type" (SRC\DRIVERS\BATTDRVR), which
// the driver reads again within 10 seconds. The registry lives in RAM: the
// choice lasts until CE is started again, platform.reg holds the default.
#include <windows.h>
#include <winioctl.h>
#include <pkfuncs.h>
#include "classpad_pwrstat.h"

#define BATT_REG_KEY TEXT("Software\\ClassPad\\Battery")
#define ID_ALKALINE 100
#define ID_NIMH     101
#define ID_CLOSE    110

static TCHAR g_line[3][96];
static TCHAR g_susp[4][64];         // what the last suspend did (classpad_pwrstat.h)

static DWORD GetType(void)
{
    HKEY key; DWORD v = 0, type, size = sizeof v;
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, BATT_REG_KEY, 0, 0, &key) == ERROR_SUCCESS) {
        if (RegQueryValueEx(key, TEXT("Type"), NULL, &type, (LPBYTE)&v, &size) != ERROR_SUCCESS || type != REG_DWORD) v = 0;
        RegCloseKey(key);
    }
    return v ? 1 : 0;
}
static void SetType(DWORD v)
{
    HKEY key; DWORD disp;
    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, BATT_REG_KEY, 0, NULL, 0, 0, NULL, &key, &disp) == ERROR_SUCCESS) {
        RegSetValueEx(key, TEXT("Type"), 0, REG_DWORD, (const BYTE *)&v, sizeof v);
        RegCloseKey(key);
    }
}
static void SnapLine(LPTSTR out, LPCTSTR tag, const CLASSPAD_WAKE_SNAPSHOT *p)
{
    wsprintf(out, TEXT("%s %02X %02X %02X %02X %04X %04X %04X %04X %04X %04X"), tag, p->rcr1, p->rcr2, p->intreq00, p->intmsk00,
             p->kiuint, p->kiudata0, p->kiustate, p->cmstr, p->cmcsr, p->rwtcsr);
}
static void GetSuspendStats(void)
{
    CLASSPAD_SUSPEND_STATS st;
    DWORD n = 0;
    int i;
    for (i = 0; i < 4; i++) g_susp[i][0] = 0;
    memset(&st, 0, sizeof st);
    if (!KernelIoControl(IOCTL_HAL_CLASSPAD_SUSPEND_STATS, NULL, 0, &st, sizeof st, &n) || n < sizeof st) return;
    if (st.path == CLASSPAD_SUSPEND_PATH_NONE) {
        lstrcpy(g_susp[0], TEXT("Suspend: none since the start"));
        return;
    }
    if (st.seconds == 0xFFFFFFFF) wsprintf(g_susp[0], TEXT("Suspend %u: length unknown, woke by %s"), st.suspends, st.wokeByAlarm ? TEXT("alarm") : TEXT("Clear"));
    else wsprintf(g_susp[0], TEXT("Suspend %u: %u s, woke by %s"), st.suspends, st.seconds, st.wokeByAlarm ? TEXT("alarm") : TEXT("Clear"));
    if (st.path == CLASSPAD_SUSPEND_PATH_CPUSLEEP) wsprintf(g_susp[1], TEXT("CPU sleep only (standby entries %u)"), st.entries);
    else wsprintf(g_susp[1], TEXT("Standby x%u, stray wakes %u, hw off %u"), st.entries, st.unexplained, st.hardwareOff);
    if (st.unexplained) {
        SnapLine(g_susp[2], TEXT("1:"), &st.first);
        SnapLine(g_susp[3], TEXT("n:"), &st.last);
    }
}
static void Refresh(HWND w)
{
    SYSTEM_POWER_STATUS_EX2 s;
    ZeroMemory(&s, sizeof s);
    wsprintf(g_line[0], TEXT("Battery type: %s"), GetType() ? TEXT("Ni-MH") : TEXT("Alkaline"));
    if (GetSystemPowerStatusEx2(&s, sizeof s, TRUE) && s.BatteryLifePercent <= 100) {
        wsprintf(g_line[1], TEXT("%d%%   %d.%02d V"), s.BatteryLifePercent, s.BatteryVoltage / 1000, (s.BatteryVoltage % 1000) / 10);
        lstrcpy(g_line[2], s.ACLineStatus == AC_LINE_ONLINE ? TEXT("USB power connected") :
                           (s.BatteryFlag & BATTERY_FLAG_CRITICAL) ? TEXT("Critical: replace the batteries") :
                           (s.BatteryFlag & BATTERY_FLAG_LOW) ? TEXT("Low") : TEXT("OK"));
    } else {
        lstrcpy(g_line[1], TEXT("No reading from the battery driver"));
        g_line[2][0] = 0;
    }
    GetSuspendStats();
    InvalidateRect(w, NULL, TRUE);
}
static LRESULT CALLBACK WindowProc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        CreateWindow(TEXT("BUTTON"), TEXT("Alkaline batteries"), WS_CHILD | WS_VISIBLE, 48, 110, 220, 36, w, (HMENU)ID_ALKALINE, NULL, NULL);
        CreateWindow(TEXT("BUTTON"), TEXT("Ni-MH batteries"), WS_CHILD | WS_VISIBLE, 48, 152, 220, 36, w, (HMENU)ID_NIMH, NULL, NULL);
        CreateWindow(TEXT("BUTTON"), TEXT("Close"), WS_CHILD | WS_VISIBLE, 48, 204, 220, 32, w, (HMENU)ID_CLOSE, NULL, NULL);
        SetTimer(w, 1, 5000, NULL);
        Refresh(w);
        return 0;
    case WM_TIMER: Refresh(w); return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == ID_ALKALINE) { SetType(0); Refresh(w); }
        if (LOWORD(wp) == ID_NIMH)     { SetType(1); Refresh(w); }
        if (LOWORD(wp) == ID_CLOSE) DestroyWindow(w);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(w, &ps); RECT r; int i;
        GetClientRect(w, &r);
        FillRect(dc, &r, (HBRUSH)GetStockObject(WHITE_BRUSH));
        SetBkMode(dc, TRANSPARENT);
        for (i = 0; i < 3; i++) ExtTextOut(dc, 12, 16 + i * 24, 0, NULL, g_line[i], lstrlen(g_line[i]), NULL);
        for (i = 0; i < 4; i++) ExtTextOut(dc, 8, 246 + i * 17, 0, NULL, g_susp[i], lstrlen(g_susp[i]), NULL);
        EndPaint(w, &ps);
        return 0;
    }
    case WM_DESTROY: KillTimer(w, 1); PostQuitMessage(0); return 0;
    }
    return DefWindowProc(w, msg, wp, lp);
}
int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPTSTR, int)
{
    WNDCLASS wc; ZeroMemory(&wc, sizeof wc);
    wc.lpfnWndProc = WindowProc; wc.hInstance = inst; wc.lpszClassName = TEXT("ClasspadBattery");
    RegisterClass(&wc);
    HWND w = CreateWindow(wc.lpszClassName, TEXT("Battery"), WS_VISIBLE | WS_CAPTION | WS_SYSMENU, 0, 0,
                          GetSystemMetrics(SM_CXSCREEN), 346, NULL, NULL, inst, NULL);
    if (!w) return 1;
    SetForegroundWindow(w);
    MSG m; while (GetMessage(&m, NULL, 0, 0)) { TranslateMessage(&m); DispatchMessage(&m); }
    return 0;
}
