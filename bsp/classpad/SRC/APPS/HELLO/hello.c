//------------------------------------------------------------------------------
//  hello.c - a full-screen window that paints colour bars, a frame, a counter
//  and a banner: proves GWES, GDI and classpad_ddi.dll end to end. The debug
//  output ("CLASSPAD HELLO: ...") shows the same steps on the console.
//------------------------------------------------------------------------------
#include <windows.h>

static int g_ticks;

static void Paint(HWND hWnd)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hWnd, &ps);
    RECT rc; int i, w, h;
    TCHAR text[64];
    HBRUSH hbr; HPEN hpen, hold;

    GetClientRect(hWnd, &rc);
    w = rc.right - rc.left; h = rc.bottom - rc.top;

    // eight colour bars across the top third
    for (i = 0; i < 8; i++) {
        static const COLORREF bars[8] = {
            RGB(255,255,255), RGB(255,255,0), RGB(0,255,255), RGB(0,255,0),
            RGB(255,0,255), RGB(255,0,0), RGB(0,0,255), RGB(0,0,0) };
        RECT b; b.left = i * w / 8; b.right = (i + 1) * w / 8; b.top = 0; b.bottom = h / 3;
        hbr = CreateSolidBrush(bars[i]); FillRect(hdc, &b, hbr); DeleteObject(hbr);
    }
    // grey field below with a frame and a diagonal
    { RECT b = rc; b.top = h / 3; hbr = CreateSolidBrush(RGB(96,96,96)); FillRect(hdc, &b, hbr); DeleteObject(hbr); }
    hpen = CreatePen(PS_SOLID, 3, RGB(255,160,0)); hold = (HPEN)SelectObject(hdc, hpen);
    Rectangle(hdc, 8, h / 3 + 8, w - 8, h - 8);
    MoveToEx(hdc, 8, h - 8, NULL); LineTo(hdc, w - 8, h / 3 + 8);
    SelectObject(hdc, hold); DeleteObject(hpen);
    // text (needs a font in the image; without one GDI draws nothing here)
    SetBkMode(hdc, TRANSPARENT); SetTextColor(hdc, RGB(255,255,255));
    { RECT t = rc; t.top = h / 3 + 16; DrawText(hdc, TEXT("Windows CE on the ClassPad"), -1, &t, DT_CENTER | DT_SINGLELINE); }
    wsprintf(text, TEXT("tick %d"), g_ticks);
    { RECT t = rc; t.top = h / 3 + 48; DrawText(hdc, text, -1, &t, DT_CENTER | DT_SINGLELINE); }
    EndPaint(hWnd, &ps);
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:  SetTimer(hWnd, 1, 1000, NULL); return 0;
    case WM_TIMER:   g_ticks++; InvalidateRect(hWnd, NULL, FALSE); if ((g_ticks % 10) == 0) RETAILMSG(1, (TEXT("CLASSPAD HELLO: tick %d\r\n"), g_ticks)); return 0;
    case WM_PAINT:   Paint(hWnd); return 0;
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
    while (GetMessage(&m, NULL, 0, 0)) { TranslateMessage(&m); DispatchMessage(&m); }
    return 0;
}
