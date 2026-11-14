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
//  Everything is drawn with solid fills, lines and one small font. GDI is
//  slow under the translator (every call is a trap into GWES and a blit in
//  the display driver), so a change costs as few calls as it can:
//    - Refresh compares what the screen shows (g_shown) with what it should
//      show and invalidates only the cells that differ: a row's value, bar
//      and MHz figure apart from its name and buttons, one preset button, one
//      wait chip, the Apply button;
//    - Paint draws exactly the cells in the update region, not everything
//      inside its bounding box, which two far-apart cells would make the
//      whole window; the region's rectangles are fetched once (GetUpdateRgn,
//      GetRegionData) and tested here, not with a call per cell;
//    - a box is two fills, an arc one polygon, and the DC's colours are only
//      set when they change.
//  Until 2026-11-14 every step repainted the whole window, about 700 calls.
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
#include "classpad_bkl.h"             // DRVESC_CLASSPAD_PUSHFRAME

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
static TCHAR  g_bench[80], g_bench2[80];     // the benchmark's two lines
static int    g_downPreset = -1, g_downRow = -1, g_downDir, g_repeats;

//  What the window shows, cell by cell (Refresh), and painting state
typedef struct {
    unsigned cpu, cpuApplied, cpuBoot; BOOL same;   // the dial
    unsigned rowVal[6], rowHz[6]; int sel;          // the six rows
    unsigned chip[4];                               // wait chips
    unsigned presetMHz[5]; BOOL presetOn[5];
    unsigned frqcr, fllfrq;
    int keep; BOOL dirty;                           // action bar
    unsigned pphi; BOOL limits, oal;                // title
} Shown;
static Shown    g_shown;
static HRGN     g_update;                           // scratch region for GetUpdateRgn
#define UPDATE_MAX 64
static RECT     g_upd[UPDATE_MAX];                  // the update region of the WM_PAINT being served, as rectangles
static int      g_nupd;
static COLORREF g_dcBk, g_dcText;                   // colours the DC in use has now (CLR_INVALID: unknown)

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
static void SetMsg(const TCHAR *text, COLORREF color)
{
    if (!wcsncmp(g_msg, text, 39) && (g_msgColor == color || !text[0])) return;   // shown already
    wcsncpy(g_msg, text, 39); g_msg[39] = 0; g_msgColor = color;
    InvalidateBand(Y_CLOCK, Y_ROWS);
}

//  A row's cells, left to right: name and - button, value, + button, bar, MHz
#define ROW_CELLS 5
static void RowCell(int i, int cell, RECT *r)
{
    static const short left[ROW_CELLS] = { 0, 60, 109, 132, -75 };      // negative: from the right edge
    int l = left[cell] < 0 ? g_cx + left[cell] : left[cell];
    int e = cell == ROW_CELLS - 1 ? g_cx : (left[cell + 1] < 0 ? g_cx + left[cell + 1] : left[cell + 1]);
    SetR(r, l, Y_ROWS + i * H_ROW, e, Y_ROWS + (i + 1) * H_ROW);
}

//------------------------------------------------------------------------------
//  Drawing primitives
//
static void Fill(HDC hdc, int l, int t, int r, int b, COLORREF c)
{
    RECT rc;
    SetR(&rc, l, t, r, b);
    if (c != g_dcBk) { SetBkColor(hdc, c); g_dcBk = c; }
    ExtTextOut(hdc, 0, 0, ETO_OPAQUE, &rc, NULL, 0, NULL);
}
//  a one-pixel edge around a filled inside: two fills
static void Box(HDC hdc, const RECT *r, COLORREF edge, COLORREF inside)
{
    Fill(hdc, r->left, r->top, r->right, r->bottom, edge);
    if (inside != edge) Fill(hdc, r->left + 1, r->top + 1, r->right - 1, r->bottom - 1, inside);
}
static void Text(HDC hdc, int l, int t, int r, int b, const TCHAR *s, COLORREF c, UINT align)
{
    RECT rc;
    SetR(&rc, l, t, r, b);
    if (c != g_dcText) { SetTextColor(hdc, c); g_dcText = c; }
    DrawText(hdc, s, -1, &rc, align | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
}
static void Line(HDC hdc, int x0, int y0, int x1, int y1)
{
    POINT p[2];
    p[0].x = x0; p[0].y = y0; p[1].x = x1; p[1].y = y1;
    Polyline(hdc, p, 2);
}
//  style: 0 plain, 1 lit (amber outline), 2 primary (amber fill), 3 warning (red outline)
static void Button(HDC hdc, const RECT *r, const TCHAR *label, int style)
{
    COLORREF edge = style == 1 ? C_AMBER : style == 3 ? C_RED : style == 2 ? C_AMBER : C_LINE;
    COLORREF text = style == 1 ? C_AMBER : style == 3 ? C_RED : style == 2 ? C_BG : C_TEXT;
    Box(hdc, r, edge, style == 2 ? C_AMBER : C_PANEL);
    if (label) Text(hdc, r->left, r->top, r->right, r->bottom, label, text, DT_CENTER);
}
//  Is any of this rectangle in the update region being painted?
static BOOL In(const RECT *r)
{
    int i;
    for (i = 0; i < g_nupd; i++)
        if (r->left < g_upd[i].right && r->right > g_upd[i].left && r->top < g_upd[i].bottom && r->bottom > g_upd[i].top) return TRUE;
    return FALSE;
}
static BOOL InBand(int top, int bottom) { RECT r; SetR(&r, 0, top, g_cx, bottom); return In(&r); }
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
//  A piece of the ring as one polygon: along the outer edge in 6 degree
//  steps, back along the inner edge through the same angles
static void DialArc(HDC hdc, int ox, int oy, int from, int to, HBRUSH brush)
{
    POINT q[2 * (180 / 6 + 2)];
    int deg[180 / 6 + 2], n = 0, i, a;
    if (to <= from) return;
    for (a = from; a < to; a += 6) deg[n++] = a;
    deg[n++] = to;
    for (i = 0; i < n; i++) DialPoint(ox, oy, 95, deg[i], &q[i]);
    for (i = 0; i < n; i++) DialPoint(ox, oy, 89, deg[n - 1 - i], &q[n + i]);
    SelectObject(hdc, brush);
    Polygon(hdc, q, 2 * n);
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
        Line(hdc, p.x, p.y, q.x, q.y);
        DialPoint(ox, oy, 114, d, &p);
        wsprintf(text, TEXT("%u"), v);
        Text(hdc, p.x - 14, p.y - 7, p.x + 14, p.y + 7, text, C_DIM, DT_CENTER);
    }

    // applied setting as a dashed needle while the target differs
    if (!SameChain(&cur, &g_tg)) {
        DialPoint(ox, oy, 80, DialDeg(curHz[2]), &p);
        SelectObject(hdc, g_penGhost);
        Line(hdc, ox, oy, p.x, p.y);
    }
    DialPoint(ox, oy, 82, deg, &p);
    SelectObject(hdc, g_penNeedle);
    Line(hdc, ox, oy, p.x, p.y);
    SelectObject(hdc, oldPen); SelectObject(hdc, oldBrush);
    Fill(hdc, ox - 4, oy - 4, ox + 4, oy + 4, C_TEXT);

    // readout: up to three digits, a point, two digits
    SetR(&box, ox - 56, Y_GAUGE + 62, ox + 56, Y_GAUGE + 102);
    Box(hdc, &box, C_LINE, C_BLACK);
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
    RECT row, r, cell;
    TCHAR text[24];
    BOOL sel = i == g_sel, hot;
    COLORREF bg = sel ? C_PANEL : C_BG;
    int barL = 132, barR = g_cx - 75, mid;

    ClockHz(&g_tg, g_pllOn, hz);
    hot = hz[i] > g_cpclkRed[i];
    RowRect(i, &row);
    mid = (row.top + row.bottom) / 2;

    RowCell(i, 0, &cell);                           // name and -
    if (In(&cell)) {
        Fill(hdc, cell.left, cell.top, cell.right, cell.bottom, bg);
        if (sel) Fill(hdc, 0, row.top, 2, row.bottom, C_AMBER);
        Text(hdc, 6, row.top, 38, row.bottom, g_rowName[i], sel ? C_AMBER : C_DIM, DT_LEFT);
        MinusRect(i, &r); Button(hdc, &r, TEXT("-"), 0);
    }
    RowCell(i, 1, &cell);                           // value
    if (In(&cell)) {
        if (i == 0)      wsprintf(text, TEXT("x%u"), g_tg.fll);
        else if (i == 1) wsprintf(text, TEXT("x%u"), g_tg.pll);
        else             wsprintf(text, TEXT("1/%u"), g_tg.div[i - 2]);
        Fill(hdc, cell.left, cell.top, cell.right, cell.bottom, bg);
        Text(hdc, 60, row.top, 108, row.bottom, text, C_TEXT, DT_CENTER);
    }
    RowCell(i, 2, &cell);                           // +
    if (In(&cell)) {
        Fill(hdc, cell.left, cell.top, cell.right, cell.bottom, bg);
        PlusRect(i, &r); Button(hdc, &r, TEXT("+"), 0);
    }
    RowCell(i, 3, &cell);                           // bar against the scale of this clock, red-zone mark on top
    if (In(&cell)) {
        Fill(hdc, cell.left, cell.top, cell.right, cell.bottom, bg);
        SetR(&r, barL, mid - 4, barR, mid + 4);
        Box(hdc, &r, C_LINE, C_BLACK);
        w = (unsigned)(barR - barL - 2) * (hz[i] / 1000u) / (g_cpclkScale[i] / 1000u);
        if (w > (unsigned)(barR - barL - 2)) w = (unsigned)(barR - barL - 2);
        if (w) Fill(hdc, barL + 1, mid - 3, barL + 1 + (int)w, mid + 3, hot ? C_RED : C_AMBER);
        w = (unsigned)(barR - barL - 2) * (g_cpclkRed[i] / 1000u) / (g_cpclkScale[i] / 1000u);
        Fill(hdc, barL + 1 + (int)w, mid - 6, barL + 2 + (int)w, mid + 6, C_RED);
    }
    RowCell(i, 4, &cell);                           // MHz
    if (In(&cell)) {
        FormatMHz(text, hz[i]);
        Fill(hdc, cell.left, cell.top, cell.right, cell.bottom, bg);
        Text(hdc, g_cx - 72, row.top, g_cx - 6, row.bottom, text, hot ? C_RED : C_TEXT, DT_RIGHT);
    }
}

//  A strip of buttons: its background only when the left margin, which no
//  button covers, is to be painted (the whole strip is), each button when it is
static BOOL StripBackground(HDC hdc, int top, int bottom)
{
    RECT margin;
    SetR(&margin, 0, top, 6, bottom);
    if (!In(&margin)) return FALSE;
    Fill(hdc, 0, top, g_cx, bottom, C_BG);
    return TRUE;
}

static void PaintWait(HDC hdc)
{
    ClasspadClockRegs t;
    TCHAR text[16];
    RECT r;
    int i;

    TargetRegs(&t);
    if (InBand(Y_WAIT, Y_CHIPS)) Section(hdc, Y_WAIT, TEXT("WAIT STATES"), TEXT("AUTO"), C_GREEN);
    StripBackground(hdc, Y_CHIPS, Y_PRESET);
    for (i = 0; i < 4; i++) {
        ChipRect(i, &r);
        if (!In(&r)) continue;
        if (i == 0)      wsprintf(text, TEXT("roR %u"), ClockWRCycles((t.cs0wcr & CPCLK_WR_MASK) >> CPCLK_WR_SHIFT));
        else if (i == 1) wsprintf(text, TEXT("TRC %u"), ClockTRCCycles(t.cs3wcr & CPCLK_TRC_MASK));
        else if (i == 2) wsprintf(text, TEXT("CL %u"), ((t.cs3wcr >> 7) & 3u) + 1u);
        else             wcscpy(text, g_tg.selxm ? TEXT("XM 1/2") : TEXT("XM 1"));
        Button(hdc, &r, text, 0);
    }
}

static void PaintPresets(HDC hdc)
{
    unsigned hz[6];
    TCHAR text[16];
    RECT r;
    int i;

    if (InBand(Y_PRESET, Y_PBTNS)) Section(hdc, Y_PRESET, TEXT("PRESETS"), TEXT("hold F2-F5 to store"), C_DIM);
    StripBackground(hdc, Y_PBTNS, Y_REGS);
    for (i = 0; i < 5; i++) {
        BOOL on = SameChain(&g_preset[i], &g_tg);
        PresetRect(i, &r);
        if (!In(&r)) continue;
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
    int i;

    if (InBand(g_cy - H_ACTION, g_cy - H_ACTION + 1)) {     // the line on top: the whole bar is to be painted
        Fill(hdc, 0, g_cy - H_ACTION, g_cx, g_cy - H_ACTION + 1, C_LINE);
        Fill(hdc, 0, g_cy - H_ACTION + 1, g_cx, g_cy, C_BLACK);
    }
    if (g_keepLeft > 0) {
        ActionRect(0, &r);
        if (In(&r)) { wsprintf(text, TEXT("Keep (%d)"), g_keepLeft); Button(hdc, &r, text, 2); }
        ActionRect(1, &r); ActionRect(3, &last); r.right = last.right;
        if (In(&r)) Button(hdc, &r, TEXT("Revert now"), 0);
        return;
    }
    for (i = 0; i < 4; i++) {
        ActionRect(i, &r);
        if (!In(&r)) continue;
        if (i == 0) Button(hdc, &r, dirty ? TEXT("Apply") : TEXT("Applied"), dirty ? 2 : 0);
        else        Button(hdc, &r, i == 1 ? TEXT("Bench") : i == 2 ? TEXT("MemTest") : TEXT("Save"), 0);
    }
}

//------------------------------------------------------------------------------
//  What the window shows, and what has to be painted again after a change
//
static void Snapshot(Shown *s)
{
    ClasspadClockChain c;
    ClasspadClockRegs t;
    unsigned hz[6];
    int i;

    memset(s, 0, sizeof *s);
    ClockHz(&g_tg, g_pllOn, hz);
    s->cpu = hz[2];
    for (i = 0; i < 6; i++) s->rowHz[i] = hz[i];
    s->rowVal[0] = g_tg.fll; s->rowVal[1] = g_tg.pll;
    for (i = 0; i < 4; i++) s->rowVal[2 + i] = g_tg.div[i];
    s->sel = g_sel;
    ClockDecode(&g_cur, &c);  s->same = SameChain(&c, &g_tg); ClockHz(&c, g_pllOn, hz); s->cpuApplied = hz[2];
    ClockDecode(&g_boot, &c); ClockHz(&c, g_pllOn, hz); s->cpuBoot = hz[2];
    TargetRegs(&t);
    s->chip[0] = t.cs0wcr & CPCLK_WR_MASK; s->chip[1] = t.cs3wcr & CPCLK_TRC_MASK;
    s->chip[2] = (t.cs3wcr >> 7) & 3u;     s->chip[3] = g_tg.selxm;
    s->frqcr = t.frqcr; s->fllfrq = t.fllfrq;
    for (i = 0; i < 5; i++) {
        s->presetOn[i] = SameChain(&g_preset[i], &g_tg);
        ClockHz(&g_preset[i], g_pllOn, hz);
        s->presetMHz[i] = (hz[2] + 500000u) / 1000000u;
    }
    s->keep = g_keepLeft; s->dirty = memcmp(&t, &g_cur, sizeof t) != 0;
    s->pphi = g_pphi; s->limits = g_limits; s->oal = g_haveOal;
}

//  After any change of state: invalidate the cells that no longer show it
static void Refresh(void)
{
    Shown n;
    const Shown *o = &g_shown;
    RECT r;
    int i, c;

    Snapshot(&n);
    if (n.pphi != o->pphi || n.limits != o->limits || n.oal != o->oal) InvalidateBand(0, Y_GAUGE);
    if (n.cpu != o->cpu || n.cpuApplied != o->cpuApplied || n.cpuBoot != o->cpuBoot || n.same != o->same) InvalidateBand(Y_GAUGE, Y_CLOCK);
    for (i = 0; i < 6; i++) {
        if (n.sel != o->sel && (i == n.sel || i == o->sel)) { RowRect(i, &r); InvalidateRect(g_hWnd, &r, FALSE); continue; }
        if (n.rowVal[i] != o->rowVal[i]) { RowCell(i, 1, &r); InvalidateRect(g_hWnd, &r, FALSE); }
        if (n.rowHz[i] != o->rowHz[i]) for (c = 3; c < ROW_CELLS; c++) { RowCell(i, c, &r); InvalidateRect(g_hWnd, &r, FALSE); }
    }
    for (i = 0; i < 4; i++) if (n.chip[i] != o->chip[i]) { ChipRect(i, &r); InvalidateRect(g_hWnd, &r, FALSE); }
    for (i = 0; i < 5; i++)
        if (n.presetOn[i] != o->presetOn[i] || n.presetMHz[i] != o->presetMHz[i]) { PresetRect(i, &r); InvalidateRect(g_hWnd, &r, FALSE); }
    if (n.frqcr != o->frqcr || n.fllfrq != o->fllfrq) InvalidateBand(Y_REGS, Y_BENCH);
    if ((n.keep > 0) != (o->keep > 0)) InvalidateBand(g_cy - H_ACTION, g_cy);      // the bar changes its buttons
    else if (n.keep != o->keep || n.dirty != o->dirty) { ActionRect(0, &r); InvalidateRect(g_hWnd, &r, FALSE); }
    g_shown = n;
}

static void Paint(HWND hWnd)
{
    struct { RGNDATAHEADER h; RECT r[UPDATE_MAX]; } data;
    PAINTSTRUCT ps;
    HDC hdc;
    HGDIOBJ oldFont;
    int i, kind;

    // the region's rectangles, before BeginPaint validates it; the bounding
    // box when there is no region to be had or it has too many pieces
    if (!g_update) g_update = CreateRectRgn(0, 0, 0, 0);
    kind = GetUpdateRgn(hWnd, g_update, FALSE);
    g_nupd = 0;
    if (kind != ERROR && kind != NULLREGION && GetRegionData(g_update, sizeof data, (RGNDATA *)&data) && data.h.nCount <= UPDATE_MAX)
        for (i = 0; i < (int)data.h.nCount; i++) g_upd[g_nupd++] = data.r[i];
    hdc = BeginPaint(hWnd, &ps);
    if (!g_nupd) { g_upd[0] = ps.rcPaint; g_nupd = 1; }
    oldFont = SelectObject(hdc, g_font);
    g_dcBk = g_dcText = CLR_INVALID;

    SetBkMode(hdc, TRANSPARENT);
    if (InBand(0, Y_GAUGE)) PaintTitle(hdc);
    if (InBand(Y_GAUGE, Y_CLOCK)) PaintGauge(hdc);
    if (InBand(Y_CLOCK, Y_ROWS)) Section(hdc, Y_CLOCK, TEXT("CLOCK"), g_msg[0] ? g_msg : TEXT("MHz"), g_msg[0] ? g_msgColor : C_DIM);
    for (i = 0; i < 6; i++) if (InBand(Y_ROWS + i * H_ROW, Y_ROWS + (i + 1) * H_ROW)) PaintRow(hdc, i);
    if (InBand(Y_ROWS + 6 * H_ROW, Y_WAIT)) Fill(hdc, 0, Y_ROWS + 6 * H_ROW, g_cx, Y_WAIT, C_BG);
    if (InBand(Y_WAIT, Y_PRESET)) PaintWait(hdc);
    if (InBand(Y_PRESET, Y_REGS)) PaintPresets(hdc);
    if (InBand(Y_REGS, Y_BENCH)) PaintRegs(hdc);
    if (InBand(Y_BENCH, Y_END)) Section(hdc, Y_BENCH, TEXT("BENCH"), g_bench, C_TEXT);
    if (g_cy - H_ACTION > Y_END && InBand(Y_END, g_cy - H_ACTION)) {
        Fill(hdc, 0, Y_END, g_cx, g_cy - H_ACTION, C_BG);
        if (g_bench2[0]) Text(hdc, 6, Y_END, g_cx - 6, Y_END + 16, g_bench2, C_TEXT, DT_RIGHT);
    }
    if (InBand(g_cy - H_ACTION, g_cy)) PaintActions(hdc);
    SelectObject(hdc, oldFont);
    EndPaint(hWnd, &ps);
}

//------------------------------------------------------------------------------
//  Benchmark: Xtune's (src/benchmark/benchmark.c), score for score.
//    CPU       passes of an empty counting loop in 100 ms, / 100
//    ROM, RAM, I/O   passes of 179 uncached long reads in 50 ms, of the flash
//              (0xA0150000), the SDRAM (0xAC150000) and a peripheral register
//              (0xA4150000); the loops are Xtune's own (bench.src)
//    dupdate   the whole frame to the panel, as frames per second: Xtune
//              times gint's dupdate(), here the display driver pushes its
//              frame (DRVESC_CLASSPAD_PUSHFRAME)
//    INT       10000 runs of Dhrystone 2.1, as Dhrystones per second
//  and Xtune's correction for the spread-spectrum clock generator.
//  Xtune stops a loop from a timer interrupt; here a thread of higher
//  priority clears the flag after a Sleep, and the count is scaled by the
//  time the loop really ran (the performance counter), a Sleep being only
//  good to a millisecond or two. Everything runs under the translator and
//  beside Windows CE's threads, so the scores are lower than Ptune4's on the
//  same clock; they compare one clock setting with another.
//
DWORD tick_count(volatile DWORD *flag);                     // bench.src
DWORD mem_bench(DWORD *area, volatile DWORD *flag);
int   dhrystone(int runs);                                  // dhry.c: 1 = it ended with the right values
BOOL  SetKMode(BOOL on);                                    // coredll: the uncached addresses below
BOOL  CeSetThreadPriority(HANDLE thread, int priority);
int   CeGetThreadPriority(HANDLE thread);

#define BENCH_DHRYSTONE 10000
#define BENCH_ROM   0xA0150000u
#define BENCH_RAM   0xAC150000u
#define BENCH_IO    0xA4150000u
#define BENCH_SSCGCR_SSEN ((const volatile BYTE *)0xA4150044u)     // bit 7 of the register's first byte: spread spectrum on

static volatile DWORD g_benchFlag;

static DWORD WINAPI BenchStop(LPVOID ms)
{
    Sleep((DWORD)ms);
    g_benchFlag = 0;
    return 0;
}

static DWORD MicroSeconds(const LARGE_INTEGER *from, const LARGE_INTEGER *to)
{
    LARGE_INTEGER f;
    if (!QueryPerformanceFrequency(&f) || f.QuadPart == 0) return 0;
    return (DWORD)((to->QuadPart - from->QuadPart) * 1000000 / f.QuadPart);
}

//  One timed loop: area = NULL for the counting loop. Passes in `ms`.
static DWORD BenchLoop(DWORD *area, DWORD ms)
{
    LARGE_INTEGER t0, t1;
    HANDLE stop;
    DWORD n, us;

    g_benchFlag = 1;
    stop = CreateThread(NULL, 0, BenchStop, (LPVOID)ms, CREATE_SUSPENDED, NULL);
    if (!stop) return 0;
    CeSetThreadPriority(stop, CeGetThreadPriority(GetCurrentThread()) - 1);
    ResumeThread(stop);                                     // it runs at once, as far as its Sleep
    QueryPerformanceCounter(&t0);
    n = area ? mem_bench(area, &g_benchFlag) : tick_count(&g_benchFlag);
    QueryPerformanceCounter(&t1);
    CloseHandle(stop);
    us = MicroSeconds(&t0, &t1);
    if (us) n = (DWORD)((unsigned __int64)n * (ms * 1000) / us);
    return n;
}

static void RunBench(void)
{
    static const DWORD area[3] = { BENCH_ROM, BENCH_RAM, BENCH_IO };
    DWORD data[8];
    LARGE_INTEGER t0, t1;
    ClasspadClockChain cur;
    BOOL kmode, pushed, right;
    HDC hdc;
    int i;

    wcscpy(g_bench, TEXT("running..."));
    g_bench2[0] = 0;
    InvalidateBand(Y_BENCH, g_cy - H_ACTION); UpdateWindow(g_hWnd);

    memset(data, 0, sizeof data);
    kmode = SetKMode(TRUE);
    data[0] = BenchLoop(NULL, 100) / 100;
    for (i = 0; i < 3; i++) data[1 + i] = BenchLoop((DWORD *)area[i], 50);

    hdc = GetDC(g_hWnd);
    QueryPerformanceCounter(&t0);
    pushed = ExtEscape(hdc, DRVESC_CLASSPAD_PUSHFRAME, 0, NULL, 0, NULL) > 0;
    QueryPerformanceCounter(&t1);
    ReleaseDC(g_hWnd, hdc);
    data[4] = pushed ? MicroSeconds(&t0, &t1) : 0;

    QueryPerformanceCounter(&t0);
    right = dhrystone(BENCH_DHRYSTONE);
    QueryPerformanceCounter(&t1);
    data[6] = MicroSeconds(&t0, &t1);

    // Xtune: with the spread spectrum on and PLL x3 or more, scores down and times up by 256 / PLL / 1000
    ClockDecode(&g_cur, &cur);
    if (g_haveOal && (*BENCH_SSCGCR_SSEN & 0x80) && cur.pll >= 3) {
        for (i = 0; i < 8; i++) {
            DWORD correction = data[i] * 256 / cur.pll / 1000;
            if (i <= 3) data[i] -= correction; else data[i] += correction;
        }
    }
    SetKMode(kmode);

    wsprintf(g_bench, TEXT("CPU %u  ROM %u  RAM %u  I/O %u"), data[0], data[1], data[2], data[3]);
    if (data[4]) { DWORD fps = 100000000u / data[4]; wsprintf(g_bench2, TEXT("dupdate: %u.%02u FPS   "), fps / 100, fps % 100); }
    else         wcscpy(g_bench2, TEXT("dupdate: n/a   "));
    if (!right)        wcscat(g_bench2, TEXT("INT: Dhrystone result WRONG"));
    else if (data[6])  wsprintf(g_bench2 + wcslen(g_bench2), TEXT("INT: %u Dhrystone/s"), (DWORD)((unsigned __int64)BENCH_DHRYSTONE * 1000000 / data[6]));
    else               wcscat(g_bench2, TEXT("INT: n/a"));
    InvalidateBand(Y_BENCH, g_cy - H_ACTION);
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
    Refresh();
}

static void StopKeep(void)
{
    KillTimer(g_hWnd, TIMER_KEEP);
    g_keepLeft = 0;
    Refresh();
}

static void Revert(void)
{
    StopKeep();
    if (!WriteClock(&g_prev)) SetMsg(TEXT("revert refused by the OAL"), C_RED);
    else                      SetMsg(TEXT("reverted"), C_GREEN);
    ClockDecode(&g_cur, &g_tg);
    Refresh();
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
    Refresh();
}

static void ToggleLimits(void)
{
    if (g_limits && MessageBox(g_hWnd,
            TEXT("Without limits a step can pass the tested range of the PLL, CPU, bus and I/O clocks. ")
            TEXT("A setting the machine cannot run hangs it until RESET.\r\n\r\nSwitch the limits off?"),
            TEXT("CEtune"), MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    g_limits = !g_limits;
    SetMsg(TEXT(""), C_DIM);
    Refresh();
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
        else if (g_sel != i) { g_sel = i; SetMsg(TEXT(""), C_DIM); Refresh(); }
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
        Refresh();
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
            Refresh();
        }
        g_downPreset = -1;
    }
}

static void OnTimer(WPARAM id)
{
    TCHAR text[24];

    if (id == TIMER_KEEP) {
        if (--g_keepLeft <= 0) Revert();
        else Refresh();
    } else if (id == TIMER_HOLD) {
        KillTimer(g_hWnd, TIMER_HOLD);
        if (g_downPreset > 0) {
            g_preset[g_downPreset] = g_tg;
            wsprintf(text, TEXT("stored in F%d"), g_downPreset + 1);
            SetMsg(text, C_GREEN);
            Refresh();
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
    case VK_UP:     if (g_sel > 0) { g_sel--; SetMsg(TEXT(""), C_DIM); Refresh(); } break;
    case VK_DOWN:   if (g_sel < 5) { g_sel++; SetMsg(TEXT(""), C_DIM); Refresh(); } break;
    case VK_LEFT:   Step(g_sel, -1, 0); break;
    case VK_RIGHT:  Step(g_sel, 1, 0); break;
    case VK_RETURN: if (g_keepLeft > 0) { StopKeep(); SetMsg(TEXT("kept"), C_GREEN); } else Apply(); break;
    case VK_ESCAPE: if (g_keepLeft > 0) Revert(); else { ClockDecode(&g_cur, &g_tg); SetMsg(TEXT(""), C_DIM); Refresh(); } break;
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
    Snapshot(&g_shown);                         // the first WM_PAINT draws everything as it is now
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
