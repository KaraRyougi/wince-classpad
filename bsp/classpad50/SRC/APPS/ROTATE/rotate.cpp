// ClassPad display orientation control and an opt-in emulator touch test.
#include <windows.h>
#include <stdarg.h>

static HWND g_window;
static BOOL g_test;
static int g_phase, g_failures;
static DWORD g_angles[] = { DMDO_0, DMDO_90, DMDO_180, DMDO_270 };
static const TCHAR *g_labels[] = { TEXT("Portrait (0)"), TEXT("Landscape (90)"), TEXT("Portrait (180)"), TEXT("Landscape (270)") };
static TCHAR g_status[128];

static void Log(LPCTSTR fmt, ...)
{
    TCHAR buf[256]; va_list ap;
    va_start(ap, fmt); wvsprintf(buf, fmt, ap); va_end(ap);
    OutputDebugString(buf);
}
static void FitWindow(HWND w)
{
    SetWindowPos(w, HWND_TOP, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), SWP_SHOWWINDOW);
}
static BOOL Rotate(HWND w, DWORD angle)
{
    DEVMODE dm; ZeroMemory(&dm, sizeof dm); dm.dmSize = sizeof dm;
    if (!EnumDisplaySettings(NULL, ENUM_CURRENT_SETTINGS, &dm)) {
        ++g_failures; Log(TEXT("ROTATION EnumDisplaySettings FAIL %d\r\n"),GetLastError());
        return FALSE;
    }
    dm.dmFields = DM_DISPLAYORIENTATION; dm.dmDisplayOrientation = angle;
    LONG rc = ChangeDisplaySettingsEx(NULL, &dm, NULL, 0, NULL);
    // CE's EnumDisplaySettings enumerates cached modes; query live rotation
    // through CDS_TEST and live geometry through USER/GDI instead.
    ZeroMemory(&dm, sizeof dm); dm.dmSize = sizeof dm;
    dm.dmFields = DM_DISPLAYORIENTATION;
    LONG query = ChangeDisplaySettingsEx(NULL, &dm, NULL, CDS_TEST, NULL);
    int width = GetSystemMetrics(SM_CXSCREEN), height = GetSystemMetrics(SM_CYSCREEN);
    HDC screen = GetDC(NULL);
    int capsWidth = screen ? GetDeviceCaps(screen, HORZRES) : 0;
    int capsHeight = screen ? GetDeviceCaps(screen, VERTRES) : 0;
    if (screen) ReleaseDC(NULL, screen);
    BOOL sideways = angle == DMDO_90 || angle == DMDO_270;
    BOOL ok = rc == DISP_CHANGE_SUCCESSFUL && query == DISP_CHANGE_SUCCESSFUL
        && dm.dmDisplayOrientation == angle && width == (sideways ? 528 : 320)
        && height == (sideways ? 320 : 528) && capsWidth == width && capsHeight == height;
    wsprintf(g_status, TEXT("%dx%d, orientation %d, %s"), width, height, angle, ok ? TEXT("OK") : TEXT("FAILED"));
    Log(TEXT("ROTATION mode=%d current=%d result=%d query=%d geometry=%dx%d caps=%dx%d %s\r\n"),
        angle, dm.dmDisplayOrientation, rc, query, width, height, capsWidth, capsHeight, ok ? TEXT("PASS") : TEXT("FAIL"));
    if (!ok) ++g_failures;
    FitWindow(w); InvalidateRect(w, NULL, TRUE);
    return ok;
}
static LRESULT CALLBACK WindowProc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        if (!g_test) {
            for (int i = 0; i < 4; ++i)
                CreateWindow(TEXT("BUTTON"), g_labels[i], WS_CHILD | WS_VISIBLE, 48, 72+i*42, 220, 36, w, (HMENU)(100+i), NULL, NULL);
            CreateWindow(TEXT("BUTTON"), TEXT("Close"), WS_CHILD | WS_VISIBLE, 48, 246, 220, 32, w, (HMENU)110, NULL, NULL);
        }
        PostMessage(w, WM_APP, 0, 0); return 0;
    case WM_APP:
        Rotate(w, DMDO_0); return 0;
    case WM_SETTINGCHANGE:
        FitWindow(w); InvalidateRect(w, NULL, TRUE); return 0;
    case WM_COMMAND:
        if (LOWORD(wp) >= 100 && LOWORD(wp) < 104) Rotate(w, g_angles[LOWORD(wp)-100]);
        if (LOWORD(wp) == 110) DestroyWindow(w);
        return 0;
    case WM_LBUTTONUP:
        if (g_test && g_phase < 4) {
            int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
            BOOL ok = x >= 34 && x <= 46 && y >= 34 && y <= 46;
            Log(TEXT("ROTATION touch phase=%d x=%d y=%d %s\r\n"), g_phase, x, y, ok ? TEXT("PASS") : TEXT("FAIL"));
            if (!ok) ++g_failures;
            ++g_phase;
            Rotate(w, g_phase < 4 ? g_angles[g_phase] : DMDO_0);
            if (g_phase == 4) {
                wsprintf(g_status, TEXT("Rotation + touch: %s (%d failures)"), g_failures ? TEXT("FAIL") : TEXT("PASS"), g_failures);
                Log(TEXT("ROTATION RESULT: %s failures=%d\r\n"), g_failures ? TEXT("FAIL") : TEXT("PASS"), g_failures);
                InvalidateRect(w, NULL, TRUE);
            }
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(w, &ps); RECT r; GetClientRect(w, &r);
        FillRect(dc, &r, (HBRUSH)GetStockObject(WHITE_BRUSH));
        SetBkMode(dc, TRANSPARENT);
        ExtTextOut(dc, 8, 8, 0, NULL, g_status, lstrlen(g_status), NULL);
        if (g_test) {
            RECT target = {24,24,57,57}; HBRUSH b = CreateSolidBrush(RGB(0,0,255));
            FillRect(dc, &target, b); DeleteObject(b);
            COLORREF pixel = GetPixel(dc, 40, 40);
            if (pixel != RGB(0,0,255)) { ++g_failures; Log(TEXT("ROTATION pixel FAIL %08x\r\n"),pixel); }
            MoveToEx(dc, 12, 80, NULL); LineTo(dc, r.right-12, r.bottom-12);
            ExtTextOut(dc, 8, 60, 0, NULL, TEXT("Tap the blue square"), 19, NULL);
        }
        EndPaint(w, &ps); return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProc(w, msg, wp, lp);
}
int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPTSTR, int)
{
    HKEY key; DWORD value=0, size=sizeof value, type;
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, TEXT("Software\\ClassPad\\RotationTest"),0,0,&key)==ERROR_SUCCESS) {
        if (RegQueryValueEx(key,TEXT("Enabled"),NULL,&type,(LPBYTE)&value,&size)==ERROR_SUCCESS && type==REG_DWORD) g_test=value!=0;
        RegCloseKey(key);
    }
    WNDCLASS wc; ZeroMemory(&wc,sizeof wc); wc.lpfnWndProc=WindowProc; wc.hInstance=inst; wc.lpszClassName=TEXT("ClasspadRotation");
    RegisterClass(&wc);
    g_window=CreateWindow(wc.lpszClassName,TEXT("Screen orientation"),WS_POPUP|WS_VISIBLE,0,0,320,528,NULL,NULL,inst,NULL);
    if (!g_window) return 1;
    SetForegroundWindow(g_window);
    MSG m; while(GetMessage(&m,NULL,0,0)){TranslateMessage(&m);DispatchMessage(&m);} return 0;
}
