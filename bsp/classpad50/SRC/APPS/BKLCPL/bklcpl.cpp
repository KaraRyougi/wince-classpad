//------------------------------------------------------------------------------
//  classpad_bkl.dll - the backlight level, as the "Advanced..." button of
//  the Backlight page of Display Properties (Control Panel > Display).
//
//  cplmain's backlight page (PUBLIC\WCESHELLFE\OAK\CTLPNL\CPLMAIN\screen.cpp)
//  loads the DLL named by HKCU\ControlPanel\BackLight "AdvancedCPL" and shows
//  the button when it exports BacklightAdvApplet(HWND); a TRUE return makes
//  it signal its "settings changed" event. The level goes to the display
//  driver through its escapes (classpad_bkl.h); the driver also puts it back
//  after a suspend. It is not stored anywhere else: CE starts with the level
//  Casio's OS left.
//
//  No resource script: a small window built here, modal to the page.
//------------------------------------------------------------------------------
#include <windows.h>
#include "classpad_bkl.h"

#define ID_DARKER   100
#define ID_BRIGHTER 101
#define ID_OK       102
#define WND_CLASS   TEXT("ClassPadBacklight")

static HINSTANCE g_inst;
static HWND g_text;
static int g_level, g_first;
static BOOL g_done, g_supported;

static BOOL Escape(int code, int *in, int *out)
{
    HDC dc = GetDC(NULL);
    int r;
    if (!dc) return FALSE;
    r = ExtEscape(dc, code, in ? sizeof(int) : 0, (LPCSTR)in, out ? sizeof(int) : 0, (LPSTR)out);
    ReleaseDC(NULL, dc);
    return r > 0;
}

static void ShowLevel(void)
{
    TCHAR s[48];
    if (g_supported) wsprintf(s, TEXT("Brightness: %d of %d"), g_level, CLASSPAD_BKL_MAX);
    else lstrcpy(s, TEXT("Not available on this display"));
    if (g_text) SetWindowText(g_text, s);
}

static void SetLevel(int level)
{
    if (level < CLASSPAD_BKL_MIN) level = CLASSPAD_BKL_MIN;
    if (level > CLASSPAD_BKL_MAX) level = CLASSPAD_BKL_MAX;
    if (g_supported && level != g_level && Escape(DRVESC_CLASSPAD_SETBRIGHTNESS, &level, NULL)) g_level = level;
    ShowLevel();
}

static LRESULT CALLBACK WndProc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_text = CreateWindow(TEXT("STATIC"), TEXT(""), WS_CHILD | WS_VISIBLE | SS_CENTER, 8, 12, 204, 20, w, NULL, g_inst, NULL);
        CreateWindow(TEXT("BUTTON"), TEXT("Darker"), WS_CHILD | WS_VISIBLE, 8, 40, 98, 34, w, (HMENU)ID_DARKER, g_inst, NULL);
        CreateWindow(TEXT("BUTTON"), TEXT("Brighter"), WS_CHILD | WS_VISIBLE, 114, 40, 98, 34, w, (HMENU)ID_BRIGHTER, g_inst, NULL);
        CreateWindow(TEXT("BUTTON"), TEXT("OK"), WS_CHILD | WS_VISIBLE, 60, 84, 100, 30, w, (HMENU)ID_OK, g_inst, NULL);
        ShowLevel();
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == ID_DARKER)   SetLevel(g_level - 1);
        if (LOWORD(wp) == ID_BRIGHTER) SetLevel(g_level + 1);
        if (LOWORD(wp) == ID_OK) DestroyWindow(w);
        return 0;
    case WM_CLOSE:
        DestroyWindow(w);
        return 0;
    case WM_DESTROY:
        g_text = NULL;
        g_done = TRUE;
        return 0;
    }
    return DefWindowProc(w, msg, wp, lp);
}

extern "C" BOOL BacklightAdvApplet(HWND parent)
{
    WNDCLASS wc;
    HWND w;
    MSG m;
    RECT r;
    int x, y, cx = 226, cy = 150;

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = g_inst;
    wc.hbrBackground = (HBRUSH)GetStockObject(LTGRAY_BRUSH);
    wc.lpszClassName = WND_CLASS;
    RegisterClass(&wc);                         // fails harmlessly the second time

    g_level = CLASSPAD_BKL_DEFAULT;
    g_supported = Escape(DRVESC_CLASSPAD_GETBRIGHTNESS, NULL, &g_level);
    g_first = g_level;
    g_done = FALSE;

    x = (GetSystemMetrics(SM_CXSCREEN) - cx) / 2;
    y = (GetSystemMetrics(SM_CYSCREEN) - cy) / 2;
    if (parent && GetWindowRect(parent, &r)) {
        x = r.left + ((r.right - r.left) - cx) / 2;
        y = r.top + ((r.bottom - r.top) - cy) / 2;
    }
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    w = CreateWindow(WND_CLASS, TEXT("Backlight"), WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                     x, y, cx, cy, parent, NULL, g_inst, NULL);
    if (!w) return FALSE;
    if (parent) EnableWindow(parent, FALSE);
    SetForegroundWindow(w);
    while (!g_done && GetMessage(&m, NULL, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessage(&m);
    }
    if (parent) {
        EnableWindow(parent, TRUE);
        SetForegroundWindow(parent);
    }
    return g_level != g_first;
}

extern "C" BOOL WINAPI DllEntry(HANDLE inst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) g_inst = (HINSTANCE)inst;
    return TRUE;
}
