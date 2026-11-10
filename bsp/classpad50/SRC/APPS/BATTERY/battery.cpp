// ClassPad battery status and battery type (alkaline / Ni-MH), like the
// "Battery Settings" of Casio's OS. The type is the battery driver's registry
// value HKLM\Software\ClassPad\Battery "Type" (SRC\DRIVERS\BATTDRVR), which
// the driver reads again within 10 seconds. The registry lives in RAM: the
// choice lasts until CE is started again, platform.reg holds the default.
#include <windows.h>

#define BATT_REG_KEY TEXT("Software\\ClassPad\\Battery")
#define ID_ALKALINE 100
#define ID_NIMH     101
#define ID_CLOSE    110

static TCHAR g_line[3][96];

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
                          GetSystemMetrics(SM_CXSCREEN), 290, NULL, NULL, inst, NULL);
    if (!w) return 1;
    SetForegroundWindow(w);
    MSG m; while (GetMessage(&m, NULL, 0, 0)) { TranslateMessage(&m); DispatchMessage(&m); }
    return 0;
}
