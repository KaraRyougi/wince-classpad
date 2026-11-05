//------------------------------------------------------------------------------
//  hello.c - a full-screen window that paints colour bars, a counter and the
//  input events it receives: proves GWES, GDI, classpad_ddi.dll and the
//  keyboard and touch drivers end to end. Key events arrive as WM_KEYDOWN/
//  WM_KEYUP (virtual key in wParam, the KEYSC scan code in lParam bits
//  16..23) and WM_CHAR; the touch panel arrives as mouse messages. Every
//  event is logged on the screen (last lines) and on the debug console
//  ("CLASSPAD HELLO: key down vk=.. sc=..", "touch down x,y").
//------------------------------------------------------------------------------
#include <windows.h>

#define LOG_LINES   8

static int g_ticks;
static TCHAR g_log[LOG_LINES][64];
static int g_logCount;                  // total lines logged (ring index = count % LOG_LINES)
static int g_keyDowns, g_touchDowns;
static int g_touchX = -1, g_touchY = -1, g_touchDown;

static void Log(HWND hWnd, LPCTSTR text)
{
    TCHAR *line = g_log[g_logCount % LOG_LINES];
    wcsncpy(line, text, 63);            // CE's coredll has no lstrcpyn
    line[63] = 0;
    g_logCount++;
    RETAILMSG(1, (TEXT("CLASSPAD HELLO: %s\r\n"), text));
    InvalidateRect(hWnd, NULL, FALSE);
}

static void Paint(HWND hWnd)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hWnd, &ps);
    RECT rc; int i, w, h, top, line;
    TCHAR text[96];
    HBRUSH hbr; HPEN hpen, hold;

    GetClientRect(hWnd, &rc);
    w = rc.right - rc.left; h = rc.bottom - rc.top;

    // eight colour bars across the top sixth
    for (i = 0; i < 8; i++) {
        static const COLORREF bars[8] = {
            RGB(255,255,255), RGB(255,255,0), RGB(0,255,255), RGB(0,255,0),
            RGB(255,0,255), RGB(255,0,0), RGB(0,0,255), RGB(0,0,0) };
        RECT b; b.left = i * w / 8; b.right = (i + 1) * w / 8; b.top = 0; b.bottom = h / 6;
        hbr = CreateSolidBrush(bars[i]); FillRect(hdc, &b, hbr); DeleteObject(hbr);
    }
    // grey field below with a frame (Rectangle fills with the DC's brush, white
    // by default, which hid the field and the white text: use the hollow brush)
    { RECT b = rc; b.top = h / 6; hbr = CreateSolidBrush(RGB(96,96,96)); FillRect(hdc, &b, hbr); DeleteObject(hbr); }
    hpen = CreatePen(PS_SOLID, 3, RGB(255,160,0)); hold = (HPEN)SelectObject(hdc, hpen);
    hbr = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, 4, h / 6 + 4, w - 4, h - 4);
    SelectObject(hdc, hbr);
    SelectObject(hdc, hold); DeleteObject(hpen);

    // text (needs a font in the image; without one GDI draws nothing here)
    SetBkMode(hdc, TRANSPARENT); SetTextColor(hdc, RGB(255,255,255));
    top = h / 6 + 8; line = 14;
    { RECT t = rc; t.top = top; DrawText(hdc, TEXT("Windows CE on the ClassPad"), -1, &t, DT_CENTER | DT_SINGLELINE); }
    wsprintf(text, TEXT("tick %d   keys %d   touches %d"), g_ticks, g_keyDowns, g_touchDowns);
    { RECT t = rc; t.top = top + line; DrawText(hdc, text, -1, &t, DT_CENTER | DT_SINGLELINE); }
    { RECT t = rc; t.top = top + 2 * line; DrawText(hdc, TEXT("press keys / touch the screen"), -1, &t, DT_CENTER | DT_SINGLELINE); }

    // the event log, oldest first
    SetTextColor(hdc, RGB(255,255,160));
    for (i = 0; i < LOG_LINES; i++) {
        int idx = g_logCount - LOG_LINES + i;
        RECT t = rc;
        if (idx < 0) continue;
        t.left = 12; t.top = top + (4 + i) * line;
        DrawText(hdc, g_log[idx % LOG_LINES], -1, &t, DT_LEFT | DT_SINGLELINE);
    }

    // the last touch point: a crosshair, red while down
    if (g_touchX >= 0) {
        hpen = CreatePen(PS_SOLID, 1, g_touchDown ? RGB(255,0,0) : RGB(0,255,0));
        hold = (HPEN)SelectObject(hdc, hpen);
        MoveToEx(hdc, g_touchX - 10, g_touchY, NULL); LineTo(hdc, g_touchX + 11, g_touchY);
        MoveToEx(hdc, g_touchX, g_touchY - 10, NULL); LineTo(hdc, g_touchX, g_touchY + 11);
        SelectObject(hdc, hold); DeleteObject(hpen);
    }
    EndPaint(hWnd, &ps);
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    TCHAR text[64];
    switch (msg) {
    case WM_CREATE:  SetTimer(hWnd, 1, 1000, NULL); return 0;
    case WM_TIMER:
        g_ticks++; InvalidateRect(hWnd, NULL, FALSE);
        if ((g_ticks % 10) == 0) RETAILMSG(1, (TEXT("CLASSPAD HELLO: tick %d, %d keys, %d touches\r\n"), g_ticks, g_keyDowns, g_touchDowns));
        return 0;
    case WM_PAINT:   Paint(hWnd); return 0;
    case WM_KEYDOWN:
        if (!(lParam & 0x40000000)) g_keyDowns++;      // not an auto-repeat
        wsprintf(text, TEXT("key down vk=%02X sc=%d%s"), (UINT)wParam, (int)((lParam >> 16) & 0xFF),
                 (lParam & 0x40000000) ? TEXT(" repeat") : TEXT(""));
        Log(hWnd, text);
        return 0;
    case WM_KEYUP:
        wsprintf(text, TEXT("key up   vk=%02X sc=%d"), (UINT)wParam, (int)((lParam >> 16) & 0xFF));
        Log(hWnd, text);
        return 0;
    case WM_CHAR:
        wsprintf(text, TEXT("char %04X '%c'"), (UINT)wParam, (wParam >= 32 && wParam < 127) ? (TCHAR)wParam : TEXT('.'));
        Log(hWnd, text);
        return 0;
    case WM_LBUTTONDOWN:
        g_touchDowns++; g_touchDown = 1;
        g_touchX = (short)LOWORD(lParam); g_touchY = (short)HIWORD(lParam);
        wsprintf(text, TEXT("touch down %d,%d"), g_touchX, g_touchY);
        Log(hWnd, text);
        return 0;
    case WM_MOUSEMOVE:
        if (wParam & MK_LBUTTON) {
            g_touchX = (short)LOWORD(lParam); g_touchY = (short)HIWORD(lParam);
            InvalidateRect(hWnd, NULL, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        g_touchDown = 0;
        g_touchX = (short)LOWORD(lParam); g_touchY = (short)HIWORD(lParam);
        wsprintf(text, TEXT("touch up   %d,%d"), g_touchX, g_touchY);
        Log(hWnd, text);
        return 0;
    case WM_DESTROY: KillTimer(hWnd, 1); PostQuitMessage(0); return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPWSTR lpCmdLine, int nCmdShow)
{
    WNDCLASS wc; HWND hWnd; MSG m;
    RETAILMSG(1, (TEXT("CLASSPAD HELLO: starting\r\n")));
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = WndProc; wc.hInstance = hInstance; wc.lpszClassName = TEXT("ClasspadHello");
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    if (!RegisterClass(&wc)) { RETAILMSG(1, (TEXT("CLASSPAD HELLO: RegisterClass failed %d\r\n"), GetLastError())); return 1; }
    hWnd = CreateWindow(TEXT("ClasspadHello"), TEXT("Hello"), WS_VISIBLE | WS_POPUP,
                        0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), NULL, NULL, hInstance, NULL);
    if (!hWnd) { RETAILMSG(1, (TEXT("CLASSPAD HELLO: CreateWindow failed %d\r\n"), GetLastError())); return 1; }
    RETAILMSG(1, (TEXT("CLASSPAD HELLO: window %dx%d\r\n"), GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)));
    ShowWindow(hWnd, SW_SHOW); UpdateWindow(hWnd);
    SetForegroundWindow(hWnd); SetFocus(hWnd);     // keyboard input goes to the focus window
    while (GetMessage(&m, NULL, 0, 0)) { TranslateMessage(&m); DispatchMessage(&m); }
    return 0;
}
