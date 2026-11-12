//------------------------------------------------------------------------------
//  cetune.c - CEtune, a clock tuner for Windows CE on the SH7305 calculators,
//  after Xtune/CPtune4 (CalcLoverHK, Sentaro21). The clock rules and tables
//  are in INC\classpad_clock.h; this file is the window.
//
//  The program never touches a register: it reads the clock with
//  IOCTL_HAL_CLASSPAD_CLOCK_GET and hands a whole setting to
//  IOCTL_HAL_CLASSPAD_CLOCK_SET, where the OAL switches the clock and
//  rescales the TMU0 tick to the new Pphi (Windows CE 5.0 BSP). Unlike Xtune a step only edits the
//  target; Apply writes it, and the old setting comes back by itself unless
//  Keep is pressed within ten seconds (a setting that breaks touch input or
//  the tick must not need touch input to undo).
//
//  Everything is drawn with solid fills, lines and one small font, and only
//  the bands that changed are repainted: GDI is slow under the translator.
//  No floating point: the FPU is emulated.
//
//  Without the two IOCTLs (an older kernel) the window still opens, marked
//  DEMO, and Apply changes nothing outside the program.
//------------------------------------------------------------------------------
#include <windows.h>
#include <winioctl.h>
#ifndef FILE_DEVICE_HAL
#define FILE_DEVICE_HAL 0x00000101
#endif
#include "classpad_clock.h"

BOOL KernelIoControl(DWORD code, LPVOID in, DWORD inSize, LPVOID out, DWORD outSize, LPDWORD returned);

// palette (all exact in RGB565 terms that matter: flat, no blending)
#define C_BG    RGB(20, 23, 28)
#define C_PANEL RGB(30, 35, 43)
#define C_BLACK RGB(12, 14, 17)
#define C_LINE  RGB(52, 60, 72)
#define C_TEXT  RGB(215, 221, 230)
#define C_DIM   RGB(133, 144, 160)
#define C_AMBER RGB(255, 176, 0)
#define C_GREEN RGB(90, 209, 122)
#define C_RED   RGB(255, 75, 62)
#define C_CYAN  RGB(73, 198, 229)

// layout: fixed bands from the top, the action bar at the bottom
#define Y_GAUGE   22
#define H_GAUGE   132
#define Y_CLOCK   154
#define Y_ROWS    170
#define H_ROW     22
#define Y_WAIT    306
#define Y_CHIPS   322
#define Y_PRESET  345
#define Y_PBTNS   361
#define Y_REGS    396
#define Y_BENCH   412
#define Y_END     428
#define H_ACTION  38

#define GAUGE_MAX_KHZ 350000u       // dial range
#define IDI_CETUNE    101           // cetune.rc: Ptune's icon
#define KEEP_SECONDS  10
#define TIMER_KEEP    1
#define TIMER_HOLD    2
#define TIMER_REPEAT  3

static const TCHAR *g_rowName[6] = { TEXT("FLL"), TEXT("PLL"), TEXT("IFC"), TEXT("SFC"), TEXT("BFC"), TEXT("PFC") };

static HWND   g_hWnd;
static HFONT  g_font;
static HBRUSH g_brAmber, g_brRed, g_brTrack;
static HPEN   g_penNeedle, g_penGhost, g_penTick;
static int    g_cx, g_cy;

static BOOL   g_haveOal;                    // the OAL answers the clock IOCTLs
static ClasspadClockRegs  g_cur, g_boot;    // applied now, found at boot
static unsigned           g_pllOn, g_pphi;    // g_pphi: Pphi as the OAL measured it, Hz
static ClasspadClockChain g_tg;             // what Apply would write
static ClasspadClockChain g_preset[5];      // F1 = boot (fixed), F2..F5 stored
static int    g_sel = 1;
static BOOL   g_limits = TRUE;
static TCHAR  g_msg[40];                    // right of the CLOCK header
static COLORREF g_msgColor;
static int    g_keepLeft;                   // > 0: Apply waits for Keep
static ClasspadClockRegs g_prev;            // what Revert restores
static TCHAR  g_bench[48];
static int    g_downPreset = -1, g_downRow = -1, g_downDir, g_repeats;

//------------------------------------------------------------------------------
//  Clock state
//
static BOOL SameChain(const ClasspadClockChain *a, const ClasspadClockChain *b)
{
    return memcmp(a, b, sizeof *a) == 0;
}

//  Registers for the target chain. The wait fields follow the bus clock
//  (Xtune's tables), except that a chain equal to the applied or the boot
//  chain keeps the waits that setting actually has.
static void TargetRegs(ClasspadClockRegs *r)
{
    ClasspadClockChain c;
    unsigned hz[6];

    *r = g_cur;
    ClockDecode(&g_cur, &c);
    if (SameChain(&c, &g_tg)) return;
    ClockEncode(&g_tg, r);
    ClockDecode(&g_boot, &c);
    if (SameChain(&c, &g_tg)) {
        r->cs0wcr = (r->cs0wcr & ~CPCLK_WR_MASK) | (g_boot.cs0wcr & CPCLK_WR_MASK);
        r->cs3wcr = (r->cs3wcr & ~CPCLK_TRC_MASK) | (g_boot.cs3wcr & CPCLK_TRC_MASK);
        return;
    }
    ClockHz(&g_tg, 1, hz);
    ClockAutoWait(r, hz[4]);
}

static BOOL Dirty(void)
{
    ClasspadClockRegs r;
    TargetRegs(&r);
    return memcmp(&r, &g_cur, sizeof r) != 0;
}

static void ReadClock(void)
{
    ClasspadClockInfo info;
    DWORD got = 0;

    g_haveOal = KernelIoControl(IOCTL_HAL_CLASSPAD_CLOCK_GET, NULL, 0, &info, sizeof info, &got);
    if (g_haveOal) {
        g_cur = info.cur; g_boot = info.boot; g_pllOn = info.pllcr & CPCLK_PLLCR_ON; g_pphi = info.pphi;
    } else if (g_boot.frqcr == 0) {
        // demo: the fx-CG50's stock setting
        g_boot.fllfrq = 0x00004384; g_boot.frqcr = 0x0F011112; g_boot.cs0wcr = 0x000003C0; g_boot.cs3wcr = 0x000024D1;
        g_cur = g_boot; g_pllOn = 1;
    }
}

static BOOL WriteClock(const ClasspadClockRegs *r)
{
    if (!g_haveOal) { g_cur = *r; return TRUE; }
    if (!KernelIoControl(IOCTL_HAL_CLASSPAD_CLOCK_SET, (LPVOID)r, sizeof *r, NULL, 0, NULL)) return FALSE;
    ReadClock();
    return TRUE;
}

static void DefaultPresets(void)
{
    ClasspadClockChain c;
    unsigned target;
    int k;

    ClockDecode(&g_boot, &c);
    g_preset[0] = c;
    // F2: the CPU clock one divider step down. F3 and F4: the PLL multiplier
    // raised by a quarter and by a half, stepped like the + button so that
    // Pphi follows and the limits stop it. F5: free, starts as F1.
    g_preset[1] = c; ClockStep(&g_preset[1], 2, -1, 0, g_cpclkLimit);
    for (k = 2; k <= 3; k++) {
        g_preset[k] = c;
        target = c.pll + c.pll * (unsigned)(k - 1) / 4u;
        while (g_preset[k].pll < target && ClockStep(&g_preset[k], 1, 1, 0, g_cpclkLimit) == 0) { }
    }
    g_preset[4] = c;
}

//------------------------------------------------------------------------------
//  Presets file: Casio's storage memory (\Storage) when it is mounted and
//  writable, else the object store's root
//
typedef struct { DWORD magic, version; ClasspadClockChain preset[4]; } CetuneFile;
#define CETUNE_MAGIC 0x4E544543     // "CETN"

static const TCHAR *g_savePath[2] = { TEXT("\\Storage\\cetune.sav"), TEXT("\\cetune.sav") };

static BOOL SavePresets(void)
{
    CetuneFile f;
    HANDLE h;
    DWORD done = 0;
    int i;

    f.magic = CETUNE_MAGIC; f.version = 1;
    memcpy(f.preset, &g_preset[1], sizeof f.preset);
    for (i = 0; i < 2; i++) {
        h = CreateFile(g_savePath[i], GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;
        WriteFile(h, &f, sizeof f, &done, NULL);
        CloseHandle(h);
        if (done == sizeof f) return TRUE;
    }
    return FALSE;
}

static void LoadPresets(void)
{
    CetuneFile f;
    ClasspadClockRegs r;
    ClasspadClockChain back;
    HANDLE h;
    DWORD done;
    int i, k;

    for (i = 0; i < 2; i++) {
        h = CreateFile(g_savePath[i], GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;
        done = 0;
        ReadFile(h, &f, sizeof f, &done, NULL);
        CloseHandle(h);
        if (done != sizeof f || f.magic != CETUNE_MAGIC || f.version != 1) continue;
        for (k = 0; k < 4; k++) {
            // only chains the OAL would take
            if (f.preset[k].pll < 1 || f.preset[k].pll > CPCLK_PLL_MAX || f.preset[k].fll > CPCLK_FLL_MAX) continue;
            r = g_boot; ClockEncode(&f.preset[k], &r);
            ClockDecode(&r, &back);
            if (SameChain(&back, &f.preset[k]) && ClockValid(&r, 1)) g_preset[1 + k] = f.preset[k];
        }
        return;
    }
}

//------------------------------------------------------------------------------
//  Geometry shared by painting and hit testing
//
static void SetR(RECT *r, int l, int t, int rr, int b) { r->left = l; r->top = t; r->right = rr; r->bottom = b; }

static void RowRect(int i, RECT *r)    { SetR(r, 0, Y_ROWS + i * H_ROW, g_cx, Y_ROWS + (i + 1) * H_ROW); }
static void MinusRect(int i, RECT *r)  { SetR(r, 39, Y_ROWS + i * H_ROW + 2, 59, Y_ROWS + i * H_ROW + 20); }
static void PlusRect(int i, RECT *r)   { SetR(r, 109, Y_ROWS + i * H_ROW + 2, 129, Y_ROWS + i * H_ROW + 20); }
static void ChipRect(int i, RECT *r)   { int w = (g_cx - 24) / 4; SetR(r, 6 + i * (w + 4), Y_CHIPS, 6 + i * (w + 4) + w, Y_CHIPS + 18); }
static void PresetRect(int i, RECT *r) { int w = (g_cx - 28) / 5; SetR(r, 6 + i * (w + 4), Y_PBTNS, 6 + i * (w + 4) + w, Y_PBTNS + 30); }
static void TitleRect(int i, RECT *r)       // 0 = limits, 1 = help, 2 = close
{
    if (i == 0)      SetR(r, g_cx - 116, 2, g_cx - 50, 20);
    else if (i == 1) SetR(r, g_cx - 46, 2, g_cx - 28, 20);
    else             SetR(r, g_cx - 24, 2, g_cx - 6, 20);
}
static void ActionRect(int i, RECT *r)      // Apply, Bench, MemTest, Save; Keep = 0, Revert = 1..3
{
    int total = g_cx - 24, w0 = total * 14 / 44, w = (total - w0) / 3, x = 6;
    if (i > 0) x = 6 + w0 + 4 + (i - 1) * (w + 4);
    SetR(r, x, g_cy - H_ACTION + 6, x + (i ? w : w0), g_cy - 6);
}
static BOOL Hit(const RECT *r, int x, int y) { return x >= r->left && x < r->right && y >= r->top && y < r->bottom; }

static void InvalidateBand(int top, int bottom)
{
    RECT r;
    SetR(&r, 0, top, g_cx, bottom);
    InvalidateRect(g_hWnd, &r, FALSE);
}
static void InvalidateState(void)           // everything a target or applied change shows in
{
    InvalidateBand(0, Y_BENCH);
    InvalidateBand(g_cy - H_ACTION, g_cy);
}
static void SetMsg(const TCHAR *text, COLORREF color)
{
    wcsncpy(g_msg, text, 39); g_msg[39] = 0; g_msgColor = color;
    InvalidateBand(Y_CLOCK, Y_ROWS);
}

//------------------------------------------------------------------------------
//  Drawing primitives
//
static void Fill(HDC hdc, int l, int t, int r, int b, COLORREF c)
{
    RECT rc;
    SetR(&rc, l, t, r, b);
    SetBkColor(hdc, c);
    ExtTextOut(hdc, 0, 0, ETO_OPAQUE, &rc, NULL, 0, NULL);
}
static void Frame(HDC hdc, const RECT *r, COLORREF c)
{
    Fill(hdc, r->left, r->top, r->right, r->top + 1, c);
    Fill(hdc, r->left, r->bottom - 1, r->right, r->bottom, c);
    Fill(hdc, r->left, r->top, r->left + 1, r->bottom, c);
    Fill(hdc, r->right - 1, r->top, r->right, r->bottom, c);
}
static void Text(HDC hdc, int l, int t, int r, int b, const TCHAR *s, COLORREF c, UINT align)
{
    RECT rc;
    SetR(&rc, l, t, r, b);
    SetTextColor(hdc, c);
    DrawText(hdc, s, -1, &rc, align | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
}
//  style: 0 plain, 1 lit (amber outline), 2 primary (amber fill), 3 warning (red outline)
static void Button(HDC hdc, const RECT *r, const TCHAR *label, int style)
{
    COLORREF edge = style == 1 ? C_AMBER : style == 3 ? C_RED : style == 2 ? C_AMBER : C_LINE;
    COLORREF text = style == 1 ? C_AMBER : style == 3 ? C_RED : style == 2 ? C_BG : C_TEXT;
    Fill(hdc, r->left, r->top, r->right, r->bottom, style == 2 ? C_AMBER : C_PANEL);
    Frame(hdc, r, edge);
    if (label) Text(hdc, r->left, r->top, r->right, r->bottom, label, text, DT_CENTER);
}
static void Section(HDC hdc, int y, const TCHAR *name, const TCHAR *right, COLORREF rightColor)
{
    Fill(hdc, 0, y, g_cx, y + 1, C_LINE);
    Fill(hdc, 0, y + 1, g_cx, y + 16, C_BG);
    Text(hdc, 6, y + 1, g_cx / 2, y + 16, name, C_DIM, DT_LEFT);
    if (right) Text(hdc, 60, y + 1, g_cx - 6, y + 16, right, rightColor, DT_RIGHT);
}
//  "117.96" from Hz (rounded to 10 kHz)
static void FormatMHz(TCHAR *out, unsigned hz)
{
    unsigned v = (hz + 5000u) / 10000u;
    wsprintf(out, TEXT("%u.%02u"), v / 100u, v % 100u);
}

//  Seven-segment digit, 14 x 24, drawn with fills (no font in the image is
//  this size, and the readout must not depend on one)
static void Seg7(HDC hdc, int x, int y, int digit, COLORREF c)
{
    static const BYTE seg[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };
    BYTE m = seg[digit];
    if (m & 0x01) Fill(hdc, x + 1, y, x + 13, y + 3, c);
    if (m & 0x02) Fill(hdc, x + 11, y + 1, x + 14, y + 12, c);
    if (m & 0x04) Fill(hdc, x + 11, y + 12, x + 14, y + 23, c);
    if (m & 0x08) Fill(hdc, x + 1, y + 21, x + 13, y + 24, c);
    if (m & 0x10) Fill(hdc, x, y + 12, x + 3, y + 23, c);
    if (m & 0x20) Fill(hdc, x, y + 1, x + 3, y + 12, c);
    if (m & 0x40) Fill(hdc, x + 1, y + 10, x + 13, y + 13, c);
}

//------------------------------------------------------------------------------
//  The dial: half circle, 0 at the left, GAUGE_MAX_KHZ at the right
//
static const short g_sin[91] = {
       0,   18,   36,   54,   71,   89,  107,  125,  143,  160,  178,  195,  213,
     230,  248,  265,  282,  299,  316,  333,  350,  367,  384,  400,  416,  433,
     449,  465,  481,  496,  512,  527,  543,  558,  573,  587,  602,  616,  630,
     644,  658,  672,  685,  698,  711,  724,  737,  749,  761,  773,  784,  796,
     807,  818,  828,  839,  849,  859,  868,  878,  887,  896,  904,  912,  920,
     928,  935,  943,  949,  956,  962,  968,  974,  979,  984,  989,  994,  998,
    1002, 1005, 1008, 1011, 1014, 1016, 1018, 1020, 1022, 1023, 1023, 1024, 1024,
};

static void DialPoint(int ox, int oy, int radius, int deg, POINT *p)
{
    int c = deg <= 90 ? g_sin[90 - deg] : -g_sin[deg - 90];
    int s = deg <= 90 ? g_sin[deg] : g_sin[180 - deg];
    p->x = ox - (radius * c + (c >= 0 ? 512 : -512)) / 1024;
    p->y = oy - (radius * s + 512) / 1024;
}
static int DialDeg(unsigned hz)
{
    unsigned khz = hz / 1000u;
    if (khz > GAUGE_MAX_KHZ) khz = GAUGE_MAX_KHZ;
    return (int)(khz * 180u / GAUGE_MAX_KHZ);
}
static void DialArc(HDC hdc, int ox, int oy, int from, int to, HBRUSH brush)
{
    POINT q[4];
    int a, e;
    SelectObject(hdc, brush);
    for (a = from; a < to; a = e) {
        e = a + 6 > to ? to : a + 6;
        DialPoint(ox, oy, 89, a, &q[0]); DialPoint(ox, oy, 95, a, &q[1]);
        DialPoint(ox, oy, 95, e, &q[2]); DialPoint(ox, oy, 89, e, &q[3]);
        Polygon(hdc, q, 4);
    }
}

static void PaintGauge(HDC hdc)
{
    ClasspadClockChain cur, boot;
    unsigned hz[6], curHz[6], bootHz[6], v, ratio;
    int ox = g_cx / 2, oy = Y_GAUGE + 118, deg, red = DialDeg(g_cpclkRed[2]), i, x;
    BOOL hot;
    POINT p, q;
    RECT box;
    TCHAR text[32];
    HGDIOBJ oldPen, oldBrush;

    ClockHz(&g_tg, g_pllOn, hz);
    ClockDecode(&g_cur, &cur);   ClockHz(&cur, g_pllOn, curHz);
    ClockDecode(&g_boot, &boot); ClockHz(&boot, g_pllOn, bootHz);
    hot = hz[2] > g_cpclkRed[2];
    deg = DialDeg(hz[2]);

    Fill(hdc, 0, Y_GAUGE, g_cx, Y_GAUGE + H_GAUGE, C_BG);

    oldPen = SelectObject(hdc, GetStockObject(NULL_PEN));
    oldBrush = SelectObject(hdc, g_brTrack);
    DialArc(hdc, ox, oy, 0, deg, hot ? g_brRed : g_brAmber);
    if (deg < red) DialArc(hdc, ox, oy, deg, red, g_brTrack);
    DialArc(hdc, ox, oy, deg > red ? deg : red, 180, g_brRed);

    // scale: a tick and a number every 50 MHz
    SelectObject(hdc, g_penTick);
    for (v = 0; v <= GAUGE_MAX_KHZ / 1000u; v += 50) {
        int d = DialDeg(v * 1000000u);
        DialPoint(ox, oy, 98, d, &p); DialPoint(ox, oy, 104, d, &q);
        MoveToEx(hdc, p.x, p.y, NULL); LineTo(hdc, q.x, q.y);
        DialPoint(ox, oy, 114, d, &p);
        wsprintf(text, TEXT("%u"), v);
        Text(hdc, p.x - 14, p.y - 7, p.x + 14, p.y + 7, text, C_DIM, DT_CENTER);
    }

    // applied setting as a dashed needle while the target differs
    if (!SameChain(&cur, &g_tg)) {
        DialPoint(ox, oy, 80, DialDeg(curHz[2]), &p);
        SelectObject(hdc, g_penGhost);
        MoveToEx(hdc, ox, oy, NULL); LineTo(hdc, p.x, p.y);
    }
    DialPoint(ox, oy, 82, deg, &p);
    SelectObject(hdc, g_penNeedle);
    MoveToEx(hdc, ox, oy, NULL); LineTo(hdc, p.x, p.y);
    SelectObject(hdc, oldPen); SelectObject(hdc, oldBrush);
    Fill(hdc, ox - 4, oy - 4, ox + 4, oy + 4, C_TEXT);

    // readout: up to three digits, a point, two digits
    SetR(&box, ox - 56, Y_GAUGE + 62, ox + 56, Y_GAUGE + 102);
    Fill(hdc, box.left, box.top, box.right, box.bottom, C_BLACK);
    Frame(hdc, &box, C_LINE);
    v = (hz[2] + 5000u) / 10000u;
    if (v > 99999u) v = 99999u;
    x = box.right - 10 - 14;
    for (i = 0; i < 5; i++) {
        if (i == 2) { Fill(hdc, x + 14, box.top + 29, x + 17, box.top + 32, hot ? C_RED : C_AMBER); x -= 7; }
        if (i < 3 || v) Seg7(hdc, x, box.top + 8, (int)(v % 10u), hot ? C_RED : C_AMBER);
        v /= 10u; x -= 18;
    }

    Text(hdc, 8, Y_GAUGE + 4, 120, Y_GAUGE + 18, TEXT("CPU MHz"), C_DIM, DT_LEFT);
    ratio = bootHz[2] ? (hz[2] / 1000u) * 100u / (bootHz[2] / 1000u) : 100u;
    wsprintf(text, TEXT("x%u.%02u boot"), ratio / 100u, ratio % 100u);
    Text(hdc, g_cx - 120, Y_GAUGE + 4, g_cx - 8, Y_GAUGE + 18, text, C_GREEN, DT_RIGHT);
}

//------------------------------------------------------------------------------
//  The bands below the dial
//
static void PaintTitle(HDC hdc)
{
    RECT r;
    Fill(hdc, 0, 0, g_cx, 21, C_BLACK);
    Fill(hdc, 0, 21, g_cx, 22, C_AMBER);
    Text(hdc, 6, 0, 50, 21, TEXT("CEtune"), C_AMBER, DT_LEFT);
    if (g_haveOal) {
        // the peripheral clock the OAL keeps time by (measured against the RTC)
        TCHAR text[32], mhz[16];
        FormatMHz(mhz, g_pphi);
        wsprintf(text, TEXT("Pphi %s meas."), mhz);
        Text(hdc, 50, 0, g_cx - 120, 21, text, C_DIM, DT_LEFT);
    } else {
        Text(hdc, 50, 0, g_cx - 120, 21, TEXT("DEMO: no OAL clock"), C_RED, DT_LEFT);
    }
    TitleRect(0, &r); Button(hdc, &r, g_limits ? TEXT("LIMITS ON") : TEXT("LIMITS OFF"), g_limits ? 0 : 3);
    if (g_limits) Text(hdc, r.left, r.top, r.right, r.bottom, TEXT("LIMITS ON"), C_GREEN, DT_CENTER);
    TitleRect(1, &r); Button(hdc, &r, TEXT("?"), 0);
    TitleRect(2, &r); Button(hdc, &r, TEXT("x"), 0);
}

static void PaintRow(HDC hdc, int i)
{
    unsigned hz[6], w;
    RECT row, r;
    TCHAR text[24];
    BOOL sel = i == g_sel, hot;
    int barL = 132, barR = g_cx - 75, mid;

    ClockHz(&g_tg, g_pllOn, hz);
    hot = hz[i] > g_cpclkRed[i];
    RowRect(i, &row);
    mid = (row.top + row.bottom) / 2;
    Fill(hdc, 0, row.top, g_cx, row.bottom, sel ? C_PANEL : C_BG);
    if (sel) Fill(hdc, 0, row.top, 2, row.bottom, C_AMBER);
    Text(hdc, 6, row.top, 38, row.bottom, g_rowName[i], sel ? C_AMBER : C_DIM, DT_LEFT);
    MinusRect(i, &r); Button(hdc, &r, TEXT("-"), 0);
    PlusRect(i, &r);  Button(hdc, &r, TEXT("+"), 0);
    if (i == 0)      wsprintf(text, TEXT("x%u"), g_tg.fll);
    else if (i == 1) wsprintf(text, TEXT("x%u"), g_tg.pll);
    else             wsprintf(text, TEXT("1/%u"), g_tg.div[i - 2]);
    Text(hdc, 60, row.top, 108, row.bottom, text, C_TEXT, DT_CENTER);

    // bar against the scale of this clock, red-zone mark on top
    SetR(&r, barL, mid - 4, barR, mid + 4);
    Fill(hdc, r.left, r.top, r.right, r.bottom, C_BLACK);
    Frame(hdc, &r, C_LINE);
    w = (unsigned)(barR - barL - 2) * (hz[i] / 1000u) / (g_cpclkScale[i] / 1000u);
    if (w > (unsigned)(barR - barL - 2)) w = (unsigned)(barR - barL - 2);
    if (w) Fill(hdc, barL + 1, mid - 3, barL + 1 + (int)w, mid + 3, hot ? C_RED : C_AMBER);
    w = (unsigned)(barR - barL - 2) * (g_cpclkRed[i] / 1000u) / (g_cpclkScale[i] / 1000u);
    Fill(hdc, barL + 1 + (int)w, mid - 6, barL + 2 + (int)w, mid + 6, C_RED);

    FormatMHz(text, hz[i]);
    Text(hdc, g_cx - 72, row.top, g_cx - 6, row.bottom, text, hot ? C_RED : C_TEXT, DT_RIGHT);
}

static void PaintWait(HDC hdc)
{
    ClasspadClockRegs t;
    TCHAR text[16];
    RECT r;

    TargetRegs(&t);
    Section(hdc, Y_WAIT, TEXT("WAIT STATES"), TEXT("AUTO"), C_GREEN);
    Fill(hdc, 0, Y_CHIPS, g_cx, Y_PRESET, C_BG);
    wsprintf(text, TEXT("roR %u"), ClockWRCycles((t.cs0wcr & CPCLK_WR_MASK) >> CPCLK_WR_SHIFT));
    ChipRect(0, &r); Button(hdc, &r, text, 0);
    wsprintf(text, TEXT("TRC %u"), ClockTRCCycles(t.cs3wcr & CPCLK_TRC_MASK));
    ChipRect(1, &r); Button(hdc, &r, text, 0);
    wsprintf(text, TEXT("CL %u"), ((t.cs3wcr >> 7) & 3u) + 1u);
    ChipRect(2, &r); Button(hdc, &r, text, 0);
    ChipRect(3, &r); Button(hdc, &r, g_tg.selxm ? TEXT("XM 1/2") : TEXT("XM 1"), 0);
}

static void PaintPresets(HDC hdc)
{
    unsigned hz[6];
    TCHAR text[16];
    RECT r;
    int i;

    Section(hdc, Y_PRESET, TEXT("PRESETS"), TEXT("hold F2-F5 to store"), C_DIM);
    Fill(hdc, 0, Y_PBTNS, g_cx, Y_REGS, C_BG);
    for (i = 0; i < 5; i++) {
        BOOL on = SameChain(&g_preset[i], &g_tg);
        PresetRect(i, &r);
        Button(hdc, &r, NULL, on ? 1 : 0);
        wsprintf(text, TEXT("F%d"), i + 1);
        Text(hdc, r.left, r.top + 2, r.right, r.top + 15, text, on ? C_AMBER : C_TEXT, DT_CENTER);
        ClockHz(&g_preset[i], g_pllOn, hz);
        wsprintf(text, TEXT("%u MHz"), (hz[2] + 500000u) / 1000000u);
        Text(hdc, r.left, r.top + 15, r.right, r.bottom - 2, text, C_DIM, DT_CENTER);
    }
}

static void PaintRegs(HDC hdc)
{
    ClasspadClockRegs t;
    TCHAR text[32];

    TargetRegs(&t);
    Fill(hdc, 0, Y_REGS, g_cx, Y_REGS + 1, C_LINE);
    Fill(hdc, 0, Y_REGS + 1, g_cx, Y_BENCH, C_BG);
    wsprintf(text, TEXT("FRQCR %08X"), t.frqcr);
    Text(hdc, 6, Y_REGS + 1, g_cx / 2, Y_BENCH, text, C_CYAN, DT_LEFT);
    wsprintf(text, TEXT("FLLFRQ %08X"), t.fllfrq);
    Text(hdc, g_cx / 2, Y_REGS + 1, g_cx - 6, Y_BENCH, text, C_CYAN, DT_RIGHT);
}

static void PaintActions(HDC hdc)
{
    RECT r, last;
    TCHAR text[16];
    BOOL dirty = Dirty();

    Fill(hdc, 0, g_cy - H_ACTION, g_cx, g_cy - H_ACTION + 1, C_LINE);
    Fill(hdc, 0, g_cy - H_ACTION + 1, g_cx, g_cy, C_BLACK);
    if (g_keepLeft > 0) {
        wsprintf(text, TEXT("Keep (%d)"), g_keepLeft);
        ActionRect(0, &r); Button(hdc, &r, text, 2);
        ActionRect(1, &r); ActionRect(3, &last); r.right = last.right;
        Button(hdc, &r, TEXT("Revert now"), 0);
        return;
    }
    ActionRect(0, &r); Button(hdc, &r, dirty ? TEXT("Apply") : TEXT("Applied"), dirty ? 2 : 0);
    ActionRect(1, &r); Button(hdc, &r, TEXT("Bench"), 0);
    ActionRect(2, &r); Button(hdc, &r, TEXT("MemTest"), 0);
    ActionRect(3, &r); Button(hdc, &r, TEXT("Save"), 0);
}

static BOOL Touches(const RECT *paint, int top, int bottom) { return paint->top < bottom && paint->bottom > top; }

static void Paint(HWND hWnd)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hWnd, &ps);
    HGDIOBJ oldFont = SelectObject(hdc, g_font);
    const RECT *p = &ps.rcPaint;
    int i;

    SetBkMode(hdc, TRANSPARENT);
    if (Touches(p, 0, Y_GAUGE)) PaintTitle(hdc);
    if (Touches(p, Y_GAUGE, Y_CLOCK)) PaintGauge(hdc);
    if (Touches(p, Y_CLOCK, Y_ROWS)) Section(hdc, Y_CLOCK, TEXT("CLOCK"), g_msg[0] ? g_msg : TEXT("MHz"), g_msg[0] ? g_msgColor : C_DIM);
    for (i = 0; i < 6; i++) if (Touches(p, Y_ROWS + i * H_ROW, Y_ROWS + (i + 1) * H_ROW)) PaintRow(hdc, i);
    if (Touches(p, Y_ROWS + 6 * H_ROW, Y_WAIT)) Fill(hdc, 0, Y_ROWS + 6 * H_ROW, g_cx, Y_WAIT, C_BG);
    if (Touches(p, Y_WAIT, Y_PRESET)) PaintWait(hdc);
    if (Touches(p, Y_PRESET, Y_REGS)) PaintPresets(hdc);
    if (Touches(p, Y_REGS, Y_BENCH)) PaintRegs(hdc);
    if (Touches(p, Y_BENCH, Y_END)) Section(hdc, Y_BENCH, TEXT("BENCH"), g_bench, C_TEXT);
    if (Touches(p, Y_END, g_cy - H_ACTION) && g_cy - H_ACTION > Y_END) Fill(hdc, 0, Y_END, g_cx, g_cy - H_ACTION, C_BG);
    if (Touches(p, g_cy - H_ACTION, g_cy)) PaintActions(hdc);
    SelectObject(hdc, oldFont);
    EndPaint(hWnd, &ps);
}

//------------------------------------------------------------------------------
//  Bench and memory test. Times come from GetTickCount, so a tick that the
//  OAL failed to retune shows here as scores that do not follow the clock.
//
#define BENCH_MS 300

static void RunBench(void)
{
    static DWORD bufA[4096], bufB[4096];
    volatile DWORD sink = 0;
    DWORD start, x = 0x12345678, cpu = 0, ram = 0, gdi = 0;
    HDC hdc;
    int i;

    wcscpy(g_bench, TEXT("running..."));
    InvalidateBand(Y_BENCH, Y_END); UpdateWindow(g_hWnd);

    start = GetTickCount();
    while (GetTickCount() - start < BENCH_MS) {         // integer: xorshift and a multiply
        for (i = 0; i < 2000; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; x += x * 2654435761u; }
        sink += x; cpu++;
    }
    start = GetTickCount();
    while (GetTickCount() - start < BENCH_MS) {         // memory: 16 KB copies
        memcpy(bufB, bufA, sizeof bufA); bufA[ram & 4095] = bufB[(ram + 7) & 4095] + 1;
        ram++;
    }
    hdc = GetDC(g_hWnd);
    start = GetTickCount();
    while (GetTickCount() - start < BENCH_MS) {         // GDI: fills of the dial area
        Fill(hdc, 0, Y_GAUGE, g_cx, Y_GAUGE + H_GAUGE, (gdi & 1) ? C_PANEL : C_BG);
        gdi++;
    }
    ReleaseDC(g_hWnd, hdc);

    // CPU in thousands of rounds per second, RAM in KB/s, GDI in fills per second
    wsprintf(g_bench, TEXT("CPU %u  RAM %u  GDI %u"), cpu * 2u * 1000u / BENCH_MS, ram * 16u * 1000u / BENCH_MS, gdi * 1000u / BENCH_MS);
    InvalidateBand(Y_GAUGE, Y_CLOCK);
    InvalidateBand(Y_BENCH, Y_END);
}

//  Pattern test of a freshly committed block. It exercises SDRAM at the
//  applied timing but only the pages the kernel hands out, not all of RAM.
#define MEMTEST_BYTES (256 * 1024)

static void RunMemTest(void)
{
    static const DWORD pattern[4] = { 0x55555555, 0xAAAAAAAA, 0x00000000, 0xFFFFFFFF };
    DWORD *mem, n = MEMTEST_BYTES / sizeof(DWORD), i, bad = 0xFFFFFFFF;
    int pass;

    wcscpy(g_bench, TEXT("testing memory..."));
    InvalidateBand(Y_BENCH, Y_END); UpdateWindow(g_hWnd);

    mem = (DWORD *)VirtualAlloc(NULL, MEMTEST_BYTES, MEM_COMMIT, PAGE_READWRITE);
    if (!mem) { wcscpy(g_bench, TEXT("MEM: no memory for the test")); InvalidateBand(Y_BENCH, Y_END); return; }
    for (pass = 0; pass < 6 && bad == 0xFFFFFFFF; pass++) {
        // four fixed patterns, then the address and its complement
        for (i = 0; i < n; i++) mem[i] = pass < 4 ? pattern[pass] : pass == 4 ? i * 4u : ~(i * 4u);
        for (i = 0; i < n; i++)
            if (mem[i] != (pass < 4 ? pattern[pass] : pass == 4 ? i * 4u : ~(i * 4u))) { bad = i * 4u; break; }
    }
    VirtualFree(mem, 0, MEM_RELEASE);
    if (bad == 0xFFFFFFFF) wsprintf(g_bench, TEXT("MEM OK  %u KB x 6 patterns"), MEMTEST_BYTES / 1024);
    else                   wsprintf(g_bench, TEXT("MEM FAIL at +%05X"), bad);
    InvalidateBand(Y_BENCH, Y_END);
}

//------------------------------------------------------------------------------
//  Actions
//
static void Step(int row, int dir, int big)
{
    TCHAR text[40];
    int over;

    if (g_keepLeft > 0) return;
    g_sel = row;
    over = ClockStep(&g_tg, row, dir, big, g_limits ? g_cpclkLimit : NULL);
    if (over > 0) {
        wsprintf(text, TEXT("%s > %u MHz"), g_rowName[over], g_cpclkLimit[over] / 1000000u);
        SetMsg(text, C_RED);
    } else {
        SetMsg(TEXT(""), C_DIM);
    }
    InvalidateState();
}

static void StopKeep(void)
{
    KillTimer(g_hWnd, TIMER_KEEP);
    g_keepLeft = 0;
    InvalidateState();
}

static void Revert(void)
{
    StopKeep();
    if (!WriteClock(&g_prev)) SetMsg(TEXT("revert refused by the OAL"), C_RED);
    else                      SetMsg(TEXT("reverted"), C_GREEN);
    ClockDecode(&g_cur, &g_tg);
    InvalidateState();
}

static void Apply(void)
{
    ClasspadClockRegs want;

    if (g_keepLeft > 0 || !Dirty()) return;
    TargetRegs(&want);
    g_prev = g_cur;
    if (!WriteClock(&want)) { SetMsg(TEXT("refused by the OAL"), C_RED); return; }
    ClockDecode(&g_cur, &g_tg);
    SetMsg(TEXT("Keep, or it reverts"), C_AMBER);
    g_keepLeft = KEEP_SECONDS;
    SetTimer(g_hWnd, TIMER_KEEP, 1000, NULL);
    InvalidateState();
}

static void ToggleLimits(void)
{
    if (g_limits && MessageBox(g_hWnd,
            TEXT("Without limits a step can pass the tested range of the PLL, CPU, bus and I/O clocks. ")
            TEXT("A setting the machine cannot run hangs it until RESET.\r\n\r\nSwitch the limits off?"),
            TEXT("CEtune"), MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    g_limits = !g_limits;
    SetMsg(TEXT(""), C_DIM);
    InvalidateBand(0, Y_GAUGE);
}

static void Help(void)
{
    MessageBox(g_hWnd,
        TEXT("Rows: FLL and PLL multiply 32768 Hz, the four dividers give the CPU (IFC), SuperHyway (SFC), ")
        TEXT("bus (BFC) and I/O (PFC) clocks. Tap - or +, or hold to repeat. Red mark: red zone.\r\n\r\n")
        TEXT("Nothing changes until Apply. After Apply press Keep within 10 s, or the old setting returns.\r\n\r\n")
        TEXT("Wait states follow the bus clock. XM switches the FLL input divider.\r\n")
        TEXT("F1 is the setting found at boot; hold F2-F5 to store the target, Save writes them to a file.\r\n\r\n")
        TEXT("Keys: up/down row, left/right step, Enter apply or keep, Esc revert."),
        TEXT("CEtune"), MB_OK);
}

static void Click(int x, int y)
{
    RECT r;
    int i;

    for (i = 0; i < 3; i++) {
        TitleRect(i, &r);
        if (!Hit(&r, x, y)) continue;
        if (i == 0) ToggleLimits(); else if (i == 1) Help(); else PostMessage(g_hWnd, WM_CLOSE, 0, 0);
        return;
    }
    if (g_keepLeft > 0) {
        ActionRect(0, &r);
        if (Hit(&r, x, y)) { StopKeep(); SetMsg(TEXT("kept"), C_GREEN); return; }
        if (y >= g_cy - H_ACTION && x >= r.right) Revert();
        return;                                     // nothing else while the countdown runs
    }
    for (i = 0; i < 6; i++) {
        RowRect(i, &r);
        if (!Hit(&r, x, y)) continue;
        MinusRect(i, &r); r.left -= 3; r.top -= 2; r.bottom += 2;
        if (Hit(&r, x, y)) { g_downRow = i; g_downDir = -1; }
        PlusRect(i, &r); r.right += 3; r.top -= 2; r.bottom += 2;
        if (Hit(&r, x, y)) { g_downRow = i; g_downDir = 1; }
        if (g_downRow == i) { g_repeats = 0; Step(i, g_downDir, 0); SetTimer(g_hWnd, TIMER_REPEAT, 400, NULL); }
        else if (g_sel != i) { g_sel = i; SetMsg(TEXT(""), C_DIM); InvalidateBand(Y_ROWS, Y_WAIT); }
        return;
    }
    ChipRect(3, &r);
    if (Hit(&r, x, y)) {                            // FLL input divider
        ClasspadClockChain c = g_tg;
        int over;
        c.selxm = !c.selxm;
        over = g_limits ? ClockOverLimit(&c, g_cpclkLimit) : 0;
        if (over) { TCHAR text[40]; wsprintf(text, TEXT("%s > %u MHz"), g_rowName[over], g_cpclkLimit[over] / 1000000u); SetMsg(text, C_RED); }
        else      { g_tg = c; SetMsg(TEXT(""), C_DIM); }
        InvalidateState();
        return;
    }
    for (i = 0; i < 5; i++) {
        PresetRect(i, &r);
        if (!Hit(&r, x, y)) continue;
        g_downPreset = i;                           // load on release, store on hold
        if (i > 0) SetTimer(g_hWnd, TIMER_HOLD, 800, NULL);
        return;
    }
    for (i = 0; i < 4; i++) {
        ActionRect(i, &r);
        if (!Hit(&r, x, y)) continue;
        if (i == 0) Apply();
        else if (i == 1) RunBench();
        else if (i == 2) RunMemTest();
        else if (SavePresets()) SetMsg(TEXT("presets saved"), C_GREEN);
        else SetMsg(TEXT("cannot write the file"), C_RED);
        return;
    }
}

static void Release(int x, int y)
{
    RECT r;

    KillTimer(g_hWnd, TIMER_REPEAT); g_downRow = -1;
    if (g_downPreset >= 0) {
        KillTimer(g_hWnd, TIMER_HOLD);
        PresetRect(g_downPreset, &r);
        if (Hit(&r, x, y) && g_keepLeft == 0) {
            int over = g_limits ? ClockOverLimit(&g_preset[g_downPreset], g_cpclkLimit) : 0;
            if (over) { TCHAR text[40]; wsprintf(text, TEXT("%s > %u MHz"), g_rowName[over], g_cpclkLimit[over] / 1000000u); SetMsg(text, C_RED); }
            else      { g_tg = g_preset[g_downPreset]; SetMsg(TEXT(""), C_DIM); }
            InvalidateState();
        }
        g_downPreset = -1;
    }
}

static void OnTimer(WPARAM id)
{
    TCHAR text[24];

    if (id == TIMER_KEEP) {
        if (--g_keepLeft <= 0) Revert();
        else InvalidateBand(g_cy - H_ACTION, g_cy);
    } else if (id == TIMER_HOLD) {
        KillTimer(g_hWnd, TIMER_HOLD);
        if (g_downPreset > 0) {
            g_preset[g_downPreset] = g_tg;
            wsprintf(text, TEXT("stored in F%d"), g_downPreset + 1);
            SetMsg(text, C_GREEN);
            InvalidateBand(Y_PRESET, Y_REGS);
            g_downPreset = -1;
        }
    } else if (id == TIMER_REPEAT) {
        if (g_downRow < 0) { KillTimer(g_hWnd, TIMER_REPEAT); return; }
        if (g_repeats == 0) SetTimer(g_hWnd, TIMER_REPEAT, 150, NULL);
        g_repeats++;
        Step(g_downRow, g_downDir, g_downRow == 0 && g_repeats > 4);    // a held FLL moves by 50
    }
}

static void OnKey(WPARAM vk)
{
    switch (vk) {
    case VK_UP:     if (g_sel > 0) { g_sel--; SetMsg(TEXT(""), C_DIM); InvalidateBand(Y_ROWS, Y_WAIT); } break;
    case VK_DOWN:   if (g_sel < 5) { g_sel++; SetMsg(TEXT(""), C_DIM); InvalidateBand(Y_ROWS, Y_WAIT); } break;
    case VK_LEFT:   Step(g_sel, -1, 0); break;
    case VK_RIGHT:  Step(g_sel, 1, 0); break;
    case VK_RETURN: if (g_keepLeft > 0) { StopKeep(); SetMsg(TEXT("kept"), C_GREEN); } else Apply(); break;
    case VK_ESCAPE: if (g_keepLeft > 0) Revert(); else { ClockDecode(&g_cur, &g_tg); SetMsg(TEXT(""), C_DIM); InvalidateState(); } break;
    }
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_PAINT:       Paint(hWnd); return 0;
    case WM_ERASEBKGND:  return 1;                  // every band fills itself
    case WM_LBUTTONDOWN: SetCapture(hWnd); Click((short)LOWORD(lParam), (short)HIWORD(lParam)); return 0;
    case WM_LBUTTONUP:   ReleaseCapture(); Release((short)LOWORD(lParam), (short)HIWORD(lParam)); return 0;
    case WM_TIMER:       OnTimer(wParam); return 0;
    case WM_KEYDOWN:     OnKey(wParam); return 0;
    case WM_CLOSE:
        if (g_keepLeft > 0) Revert();               // an unconfirmed setting does not outlive the window
        DestroyWindow(hWnd);
        return 0;
    case WM_DESTROY:     PostQuitMessage(0); return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPWSTR lpCmdLine, int nCmdShow)
{
    WNDCLASS wc;
    LOGFONT lf;
    RECT work;
    MSG m;
    HWND other = FindWindow(TEXT("CEtune"), NULL);

    if (other) { SetForegroundWindow(other); return 0; }

    ReadClock();
    DefaultPresets();
    LoadPresets();
    ClockDecode(&g_cur, &g_tg);
    wcscpy(g_bench, TEXT("tap Bench or MemTest"));
    RETAILMSG(1, (TEXT("CETUNE: %s, FLLFRQ=%08x FRQCR=%08x\r\n"), g_haveOal ? TEXT("OAL clock control") : TEXT("demo (no clock IOCTL)"), g_cur.fllfrq, g_cur.frqcr));

    memset(&lf, 0, sizeof lf);
    lf.lfHeight = -11; lf.lfWeight = FW_NORMAL; lf.lfQuality = NONANTIALIASED_QUALITY;
    wcscpy(lf.lfFaceName, TEXT("Tahoma"));
    g_font      = CreateFontIndirect(&lf);
    g_brAmber   = CreateSolidBrush(C_AMBER);
    g_brRed     = CreateSolidBrush(C_RED);
    g_brTrack   = CreateSolidBrush(C_LINE);
    g_penNeedle = CreatePen(PS_SOLID, 2, C_TEXT);
    g_penGhost  = CreatePen(PS_DASH, 1, C_DIM);
    g_penTick   = CreatePen(PS_SOLID, 1, C_DIM);

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = WndProc; wc.hInstance = hInstance; wc.lpszClassName = TEXT("CEtune");
    wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_CETUNE));
    if (!RegisterClass(&wc)) return 1;

    // the work area: the shell's taskbar stays visible
    if (!SystemParametersInfo(SPI_GETWORKAREA, 0, &work, 0))
        SetR(&work, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    g_cx = work.right - work.left; g_cy = work.bottom - work.top;
    g_hWnd = CreateWindow(TEXT("CEtune"), TEXT("CEtune"), WS_VISIBLE | WS_POPUP,
                          work.left, work.top, g_cx, g_cy, NULL, NULL, hInstance, NULL);
    if (!g_hWnd) return 1;
    // the taskbar button's small icon
    SendMessage(g_hWnd, WM_SETICON, FALSE, (LPARAM)LoadImage(hInstance, MAKEINTRESOURCE(IDI_CETUNE), IMAGE_ICON, 16, 16, 0));
    ShowWindow(g_hWnd, SW_SHOW); UpdateWindow(g_hWnd);
    SetForegroundWindow(g_hWnd); SetFocus(g_hWnd);

    while (GetMessage(&m, NULL, 0, 0)) { TranslateMessage(&m); DispatchMessage(&m); }
    return 0;
}
