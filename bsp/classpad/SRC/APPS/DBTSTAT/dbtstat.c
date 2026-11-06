//------------------------------------------------------------------------------
//  dbtstat.c - the translator's counters on the screen: a profiler for runs
//  on the real machine, where nothing else gets out.
//
//  Every 2 s the window reads the translator's statistics through the
//  HC_GETSTAT hypercall (classpad_hc.h lists the indices; a trapa works from
//  user mode) and shows rates over the last interval and shares of the tick
//  samples: how much of the time goes to translated guest code, to the
//  stub windows, to translations, to idle; exceptions per second by vector
//  and INTEVT code; with a PROFILE=1 translator the TMU1 time of each C
//  bucket as a percentage of real time; and the hottest buckets of guest
//  code by physical address (resolve them with tools/cesym.py).
//
//  Page 0: rates. Page 1: hot code since the window started. Page 2: hot
//  code in the last interval. Tap the window to turn the page; tap the
//  top-right corner ("x") to close it. Start it from the shell
//  (\Windows\classpad_dbtstat.exe) or as a Launch entry in platform.reg.
//------------------------------------------------------------------------------
#include <windows.h>
#include "classpad_hc.h"

#define POLL_MS         2000
#define NSTAT           HC_STAT_N
#define LINES           15
#define LINE_H          13
#define TMU1_HZ         7372800

static DWORD g_now[NSTAT], g_prev[NSTAT], g_first[NSTAT];
static DWORD g_tickNow, g_tickPrev, g_tickFirst;
static int   g_page, g_polls;
static HFONT g_font;

static void Snapshot(void)
{
    int i;
    memcpy(g_prev, g_now, sizeof g_now);
    g_tickPrev = g_tickNow;
    for (i = 0; i < 128; i++) g_now[i] = ClasspadHcGetStat(i);
    for (i = HC_STAT_HIST; i < NSTAT; i++) g_now[i] = ClasspadHcGetStat(i);
    g_tickNow = GetTickCount();
    if (g_polls++ == 0) { memcpy(g_first, g_now, sizeof g_now); memcpy(g_prev, g_now, sizeof g_now); g_tickFirst = g_tickPrev = g_tickNow; }
}

static DWORD D(int i)  { return g_now[i] - g_prev[i]; }       // delta over the last interval
static DWORD DF(int i) { return g_now[i] - g_first[i]; }      // delta since the window started

// per second over the last interval (ms is the interval length)
static DWORD Rate(int i, DWORD ms) { return ms ? (DWORD)(((ULONGLONG)D(i) * 1000) / ms) : 0; }
static DWORD Pct(DWORD part, DWORD whole) { return whole ? (DWORD)(((ULONGLONG)part * 100) / whole) : 0; }

typedef struct { int line; TCHAR text[LINES][96]; } PAGE;

static void Add(PAGE *p, LPCTSTR fmt, ...)
{
    va_list ap;
    if (p->line >= LINES) return;
    va_start(ap, fmt);
    wvsprintf(p->text[p->line++], fmt, ap);
    va_end(ap);
}

// the n hottest histogram buckets of f(i) (delta function), largest first
static int Hot(DWORD (*f)(int), int *out, int n)
{
    int i, j, k, count = 0;
    for (i = 0; i < HC_STAT_HIST_N; i++) {
        DWORD v = f(HC_STAT_HIST + i);
        if (!v) continue;
        for (j = 0; j < count && f(HC_STAT_HIST + out[j]) >= v; j++) ;
        if (j >= n) continue;
        for (k = (count < n ? count : n - 1); k > j; k--) out[k] = out[k - 1];
        out[j] = i;
        if (count < n) count++;
    }
    return count;
}

static void HotPage(PAGE *p, DWORD (*f)(int), DWORD total, LPCTSTR title)
{
    int top[12], n, i;
    DWORD base = g_now[HC_STAT_HIST_BASE], shift = g_now[HC_STAT_HIST_BASE + 1];
    Add(p, TEXT("%s  %u samples, %u KB buckets, base %08X"), title, total, (1u << shift) >> 10, base);
    n = Hot(f, top, 12);
    for (i = 0; i < n; i++) {
        DWORD pa = base + ((DWORD)top[i] << shift), v = f(HC_STAT_HIST + top[i]);
        Add(p, TEXT("  %08X  ROM+%06X  %6u  %2u%%"), pa | 0x80000000, pa - base, v, Pct(v, total));
    }
    if (!n) Add(p, TEXT("  (no samples in the ROM image yet)"));
}

static void Build(PAGE *p)
{
    DWORD ms = g_tickNow - g_tickPrev, prof = g_now[11], el = D(12);   // el: TMU1 counts of the interval (profiling builds)
    DWORD samples = D(HC_STAT_SAMPLES);
    p->line = 0;
    if (g_page == 1) { HotPage(p, DF, DF(HC_STAT_SAMPLES), TEXT("hot since start:")); return; }
    if (g_page == 2) { HotPage(p, D, samples, TEXT("hot last interval:")); return; }

    {
        // the TMU0 tick against the RTC, over the whole run: 1000 = exact
        DWORD rtc = (g_now[83] - g_first[83] + 86400) % 86400, t = g_tickNow - g_tickFirst, ratio = rtc >= 10 ? t / rtc : 0;
        Add(p, TEXT("DBT %s  dt %u ms  tick/rtc %u.%03u  up %us"), prof ? TEXT("PROFILE") : TEXT("stats"),
            ms, ratio / 1000, ratio % 1000, t / 1000);
    }
    Add(p, TEXT("CCR %08X  MMUCR %08X  FRQCR %08X"), g_now[80], g_now[81], g_now[82]);
    Add(p, TEXT("PLLCR %08X  FLLFRQ %08X   slowest xlat at %08X"), g_now[384], g_now[385], g_now[386]);
    Add(p, TEXT("xlat/s %u  blocks %u  flush/s %u  inval/s %u"), Rate(0, ms), g_now[1], Rate(7, ms), Rate(2, ms));
    Add(p, TEXT("asid/s %u  icmiss/s %u  lookup/s %u  tlbhook/s %u"), Rate(3, ms), Rate(4, ms), Rate(27, ms), Rate(28, ms));
    Add(p, TEXT("exc/s gen %u  tlb %u  irq %u   rte/s %u  ldcsr/s %u"), Rate(77, ms), Rate(78, ms), Rate(79, ms), Rate(25, ms), Rate(26, ms));
    {
        TCHAR line[96] = TEXT("intevt/s"); int k, n = (int)g_now[84]; if (n > 16) n = 16;
        for (k = 0; k < n && k < 6; k++) {
            TCHAR item[24];
            wsprintf(item, TEXT("  %X:%u"), g_now[85 + 2 * k], Rate(86 + 2 * k, ms));
            if (lstrlen(line) + lstrlen(item) < 95) lstrcat(line, item);
        }
        Add(p, TEXT("%s"), line);
    }
    // the sample split: translated code, the cheap stub windows, and the
    // masked time delivered at the re-enabling instruction (translator C,
    // kernel interrupt-disabled regions, helpers)
    Add(p, TEXT("ticks %u: code %u%%  exit %u%%  stub-entry %u%%  hit %u%%  tramp %u%%"), samples,
        Pct(D(65), samples), Pct(D(66), samples), Pct(D(117), samples), Pct(D(121), samples), Pct(D(122), samples));
    Add(p, TEXT("  masked: translator %u%%  kernel %u%%  helpers %u%%"),
        Pct(D(118) + D(68), samples), Pct(D(119), samples), Pct(D(120) + D(69), samples));
    Add(p, TEXT("  idle %u%%  user %u%% (ram %u%%)  kernel-other %u%% @%08X"),
        Pct(D(70), samples), Pct(D(71), samples), Pct(D(72), samples), Pct(D(73), samples), g_now[123]);
    if (prof && el) {
        // percentage of real time in each C bucket (buckets overlap: never sum them)
        Add(p, TEXT("C time %%: xlat %u  asid %u  tlb %u  inval %u  lcd %u  flush %u"),
            Pct(D(33), el), Pct(D(36), el), Pct(D(39), el), Pct(D(42), el), Pct(D(45), el), Pct(D(48), el));
        Add(p, TEXT("  exc %u  rte %u  lookup %u   (xlat max %u us)"),
            Pct(D(51), el), Pct(D(54), el), Pct(D(57), el), g_now[34] / (TMU1_HZ / 1000000));
        Add(p, TEXT("  TMU1 %u.%02u s per %u ms of ticks"), el / TMU1_HZ, (el % TMU1_HZ) * 100 / TMU1_HZ, ms);
    } else {
        Add(p, TEXT("(build the translator with PROFILE=1 for the C time buckets)"));
    }
    {
        int top[4], n, i; TCHAR line[96] = TEXT("hot now:");
        n = Hot(D, top, 4);
        for (i = 0; i < n; i++) {
            TCHAR item[24];
            wsprintf(item, TEXT(" %08X:%u"), (g_now[74] + ((DWORD)top[i] << g_now[75])) | 0x80000000, D(HC_STAT_HIST + top[i]));
            if (lstrlen(line) + lstrlen(item) < 95) lstrcat(line, item);
        }
        Add(p, TEXT("%s"), line);
    }
    Add(p, TEXT("tap: next page   top-right: close"));
}

static void Paint(HWND hWnd)
{
    static PAGE page;
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hWnd, &ps);
    RECT rc; HFONT old; int i;
    HBRUSH hbr = CreateSolidBrush(RGB(0, 0, 64));

    GetClientRect(hWnd, &rc);
    FillRect(hdc, &rc, hbr); DeleteObject(hbr);
    Build(&page);
    old = (HFONT)SelectObject(hdc, g_font);
    SetBkMode(hdc, TRANSPARENT);
    for (i = 0; i < page.line; i++) {
        SetTextColor(hdc, i == 0 ? RGB(255, 220, 120) : RGB(230, 230, 230));
        ExtTextOut(hdc, 2, 2 + i * LINE_H, 0, NULL, page.text[i], lstrlen(page.text[i]), NULL);
    }
    SetTextColor(hdc, RGB(255, 120, 120));
    ExtTextOut(hdc, rc.right - 12, 2, 0, NULL, TEXT("x"), 1, NULL);     // the close corner (WM_LBUTTONDOWN)
    SelectObject(hdc, old);
    EndPaint(hWnd, &ps);
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
        Snapshot();
        SetTimer(hWnd, 1, POLL_MS, NULL);
        return 0;
    case WM_TIMER:
        Snapshot();
        InvalidateRect(hWnd, NULL, FALSE);
        return 0;
    case WM_PAINT:
        Paint(hWnd);
        return 0;
    case WM_LBUTTONDOWN: {
        RECT rc; GetClientRect(hWnd, &rc);
        if ((short)HIWORD(lParam) < LINE_H + 2 && (short)LOWORD(lParam) > rc.right - 24) { DestroyWindow(hWnd); return 0; }
        g_page = (g_page + 1) % 3;
        InvalidateRect(hWnd, NULL, FALSE);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hWnd, 1);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPWSTR lpCmdLine, int nCmdShow)
{
    WNDCLASS wc; HWND hWnd; MSG m; LOGFONT lf;
    int w = GetSystemMetrics(SM_CXSCREEN), h = LINES * LINE_H + 4;

    RETAILMSG(1, (TEXT("CLASSPAD DBTSTAT: starting\r\n")));
    memset(&lf, 0, sizeof lf);
    lf.lfHeight = -11; lf.lfWeight = FW_NORMAL; lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy(lf.lfFaceName, TEXT("Tahoma"));
    g_font = CreateFontIndirect(&lf);

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = WndProc; wc.hInstance = hInstance; wc.lpszClassName = TEXT("ClasspadDbtStat");
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    if (!RegisterClass(&wc)) { RETAILMSG(1, (TEXT("CLASSPAD DBTSTAT: RegisterClass failed %d\r\n"), GetLastError())); return 1; }
    hWnd = CreateWindowEx(WS_EX_TOPMOST, TEXT("ClasspadDbtStat"), TEXT("DBT"), WS_VISIBLE | WS_POPUP,
                          0, 0, w, h, NULL, NULL, hInstance, NULL);
    if (!hWnd) { RETAILMSG(1, (TEXT("CLASSPAD DBTSTAT: CreateWindow failed %d\r\n"), GetLastError())); return 1; }
    ShowWindow(hWnd, SW_SHOW); UpdateWindow(hWnd);
    while (GetMessage(&m, NULL, 0, 0)) { TranslateMessage(&m); DispatchMessage(&m); }
    DeleteObject(g_font);
    return 0;
}
