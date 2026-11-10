//------------------------------------------------------------------------------
//  peghelp.c - PegHelp.exe for CE 6.0 without Internet Explorer: the Pocket PC
//  help viewer that Pocket PC programs start with
//      CreateProcess(L"PegHelp.exe", L"file:math1.htm#findroot", ...)
//  (Math Xpander's Workbook.dll and Graphics.dll do exactly that and ignore
//  the result). CE 6.0's own PegHelp is IE's htmlhelp.exe, which needs the
//  whole IE engine; this one renders the simple HTML of help files with
//  hpcore.c and GDI, and needs only coredll.
//
//  Command line: [file:|help:]name.htm[#anchor] (or ?anchor). A name without
//  a path is looked up next to the current page, then in \Windows. A second
//  start hands its command line to the running viewer (WM_COPYDATA) and exits.
//
//  mainhelp.htm (what explorer's Start > Help asks for) and help:contents are
//  no files: as in CE's own viewer, the page is built from the shortcuts in
//  \Windows\Help (hp_contents_page, hpcore.h).
//
//  Window: Back and Contents buttons (Contents = the page's Htm-Help meta, or
//  the first page), the page below. Tap a link to follow it, drag to scroll;
//  Up/Down, Left/Right (page), Home/End scroll; Esc/Backspace go back, or
//  close the viewer at the first page.
//------------------------------------------------------------------------------
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "hpcore.h"

#define HELP_CLASS      TEXT("PegHelp")
#define VIEW_CLASS      TEXT("PegHelpView")
#define ID_BACK         101
#define ID_CONTENTS     102
#define ID_VIEW         103
#define BAR_H           26
#define LINE_STEP       16
#define DRAG_SLOP       6
#define MAX_HISTORY     32
#define MAX_URL         (MAX_PATH + 64)
#define MAX_IMAGES      16
#define MAX_FILE        (4 * 1024 * 1024)
#define COPYDATA_URL    0x48474550      // 'PEGH'
#define WM_PH_LINK      (WM_USER + 1)   // wParam = link (token index)
#define WM_PH_BACK      (WM_USER + 2)   // wParam = 1: close at the first page
#ifndef WM_DISPLAYCHANGE
#define WM_DISPLAYCHANGE 0x007E         // only CE 6.0's windowsx.h uses it
#endif

typedef struct {
    TCHAR path[MAX_PATH];
    int scroll;
} HISTORY;

typedef struct {
    TCHAR path[MAX_PATH];
    HBITMAP bm;
    int w, h;
} IMAGE;

static HINSTANCE g_hinst;
static HWND g_main, g_view, g_back, g_contents;
static HDC g_mdc, g_idc;                // measuring DC, image source DC
static HFONT g_font[HP_FONT_COUNT];
static int g_fh[HP_FONT_COUNT], g_fa[HP_FONT_COUNT];
static short g_cw[HP_FONT_COUNT][256];  // advance widths, -1 = not measured
static HBRUSH g_rule;

static hp_doc *g_doc;
static TCHAR g_path[MAX_PATH];          // current file, "" = a message page
static TCHAR g_dir[MAX_PATH];           // its folder, with the backslash
static TCHAR g_first[MAX_URL];          // first page (Contents fallback)
static int g_height, g_scroll, g_layoutWidth;
static HISTORY g_hist[MAX_HISTORY];
static int g_nhist;

static HDC g_bdc;                       // back buffer
static HBITMAP g_bbm, g_bbmOld;
static int g_bw, g_bh;

static BOOL g_down, g_dragging;
static int g_downY, g_downScroll, g_pressLink = -1;

static IMAGE g_img[MAX_IMAGES];
static int g_nimg;

static BOOL Navigate(LPCTSTR url, int n, BOOL push);

//------------------------------------------------------------------------------
// strings (CE 6.0's coredll has no lstrcpyn)

static int Len(LPCTSTR s)
{
    int n = 0;
    while (s[n])
        n++;
    return n;
}

static void CopyN(LPTSTR dst, int cap, LPCTSTR src, int n)
{
    int i;
    if (cap <= 0)
        return;
    if (n > cap - 1)
        n = cap - 1;
    for (i = 0; i < n; i++)
        dst[i] = src[i];
    dst[n] = 0;
}

static void Append(LPTSTR dst, int cap, LPCTSTR src)
{
    int d = Len(dst), i = 0;
    while (src[i] && d + 1 < cap)
        dst[d++] = src[i++];
    dst[d] = 0;
}

static TCHAR Low(TCHAR c)
{
    return (TCHAR)((c >= 'A' && c <= 'Z') ? c + 32 : c);
}

static BOOL Prefix(LPCTSTR s, int n, LPCTSTR p)
{
    int i;
    for (i = 0; p[i]; i++)
        if (i >= n || Low(s[i]) != Low(p[i]))
            return FALSE;
    return TRUE;
}

static BOOL SameText(LPCTSTR a, LPCTSTR b)
{
    while (*a && Low(*a) == Low(*b)) {
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

//------------------------------------------------------------------------------
// files

static BOOL IsFile(LPCTSTR path)
{
    DWORD a = GetFileAttributes(path);
    return a != 0xFFFFFFFF && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// a file name from a link (not NUL-terminated) -> full path that exists
static BOOL Resolve(LPCTSTR name, int n, LPTSTR out)
{
    TCHAR rel[MAX_PATH];
    int i;
    if (n <= 0 || n >= MAX_PATH - 10)
        return FALSE;
    CopyN(rel, MAX_PATH, name, n);
    for (i = 0; rel[i]; i++)
        if (rel[i] == '/')
            rel[i] = '\\';
    if (rel[0] == '\\') {
        CopyN(out, MAX_PATH, rel, Len(rel));
        return IsFile(out);
    }
    if (g_dir[0]) {
        CopyN(out, MAX_PATH, g_dir, Len(g_dir));
        Append(out, MAX_PATH, rel);
        if (IsFile(out))
            return TRUE;
    }
    CopyN(out, MAX_PATH, TEXT("\\Windows\\"), 9);
    Append(out, MAX_PATH, rel);
    return IsFile(out);
}

static unsigned char *ReadAll(LPCTSTR path, int *size)
{
    HANDLE h = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    DWORD n, got = 0;
    unsigned char *b = NULL;
    if (h == INVALID_HANDLE_VALUE)
        return NULL;
    n = GetFileSize(h, NULL);
    if (n != 0xFFFFFFFF && n <= MAX_FILE) {
        b = (unsigned char *)malloc(n + 1);
        if (b && (!ReadFile(h, b, n, &got, NULL) || got != n)) {
            free(b);
            b = NULL;
        }
    }
    CloseHandle(h);
    *size = (int)n;
    return b;
}

//------------------------------------------------------------------------------
// fonts and the metrics hpcore asks for

static HFONT Font(int f)
{
    static const int px[4] = { 10, 13, 15, 18 };
    LOGFONT lf;
    TEXTMETRIC tm;
    int i;
    if (g_font[f])
        return g_font[f];
    memset(&lf, 0, sizeof lf);
    lf.lfHeight = -px[HP_FONT_SIZE(f)];
    lf.lfWeight = (f & HP_FONT_BOLD) ? FW_BOLD : FW_NORMAL;
    lf.lfItalic = (BYTE)((f & HP_FONT_ITALIC) ? TRUE : FALSE);
    lf.lfUnderline = (BYTE)((f & HP_FONT_UNDERLINE) ? TRUE : FALSE);
    lf.lfCharSet = DEFAULT_CHARSET;
    CopyN(lf.lfFaceName, LF_FACESIZE, TEXT("Tahoma"), 6);
    g_font[f] = CreateFontIndirect(&lf);
    if (!g_font[f])
        g_font[f] = (HFONT)GetStockObject(SYSTEM_FONT);
    SelectObject(g_mdc, g_font[f]);
    GetTextMetrics(g_mdc, &tm);
    g_fh[f] = tm.tmHeight;
    g_fa[f] = tm.tmAscent;
    for (i = 0; i < 256; i++)
        g_cw[f][i] = -1;
    return g_font[f];
}

static void CbFont(void *ctx, int f, int *h, int *a)
{
    (void)ctx;
    Font(f);
    *h = g_fh[f];
    *a = g_fa[f];
}

static int CbWidth(void *ctx, int f, const hp_char *s, int n)
{
    int i, w = 0;
    SIZE sz;
    (void)ctx;
    SelectObject(g_mdc, Font(f));
    for (i = 0; i < n; i++) {
        WCHAR c = (WCHAR)s[i];
        if (c < 256 && g_cw[f][c] >= 0) {
            w += g_cw[f][c];
            continue;
        }
        if (!GetTextExtentPoint32(g_mdc, &c, 1, &sz))
            sz.cx = 0;
        if (c < 256)
            g_cw[f][c] = (short)sz.cx;
        w += sz.cx;
    }
    return w;
}

static DWORD Get32(const unsigned char *p)
{
    return (DWORD)p[0] | ((DWORD)p[1] << 8) | ((DWORD)p[2] << 16) | ((DWORD)p[3] << 24);
}

// a BMP file (any name: Pocket PC uses .2bp for 2 bpp ones) as a DIB section
static HBITMAP LoadBmp(LPCTSTR path, int *w, int *h)
{
    int size, bpp, ncol, stride, rows;
    DWORD off, hdr, comp, used;
    unsigned char *b = ReadAll(path, &size);
    BITMAPINFO *bmi = NULL;
    HBITMAP bm = NULL;
    void *bits = NULL;
    if (!b)
        return NULL;
    if (size < 54 || b[0] != 'B' || b[1] != 'M')
        goto out;
    off = Get32(b + 10);
    hdr = Get32(b + 14);
    *w = (int)Get32(b + 18);
    *h = (int)Get32(b + 22);
    bpp = b[28] | (b[29] << 8);
    comp = Get32(b + 30);
    used = Get32(b + 46);
    if (hdr < 40 || comp != BI_RGB || *w <= 0 || *w > 2048 || *h == 0 || *h > 2048 || *h < -2048)
        goto out;
    if (bpp != 1 && bpp != 2 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32)
        goto out;
    ncol = bpp <= 8 ? (used && used <= 256 ? (int)used : 1 << bpp) : 0;
    rows = *h < 0 ? -*h : *h;
    stride = ((*w * bpp + 31) / 32) * 4;
    if (14 + hdr + (DWORD)ncol * 4 > (DWORD)size || off + (DWORD)(stride * rows) > (DWORD)size)
        goto out;
    bmi = (BITMAPINFO *)malloc(sizeof(BITMAPINFOHEADER) + 256 * sizeof(RGBQUAD));
    if (!bmi)
        goto out;
    memcpy(bmi, b + 14, sizeof(BITMAPINFOHEADER));
    bmi->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi->bmiHeader.biClrUsed = (DWORD)ncol;
    if (ncol)
        memcpy(bmi->bmiColors, b + 14 + hdr, (size_t)ncol * 4);
    bm = CreateDIBSection(g_mdc, bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (bm && bits)
        memcpy(bits, b + off, (size_t)stride * (size_t)rows);
    *h = rows;
out:
    free(bmi);
    free(b);
    return bm;
}

static IMAGE *Image(const hp_char *src, int n)
{
    TCHAR path[MAX_PATH];
    int i, w = 0, h = 0;
    HBITMAP bm;
    if (!Resolve((LPCTSTR)src, n, path))
        return NULL;
    for (i = 0; i < g_nimg; i++)
        if (SameText(g_img[i].path, path))
            return g_img[i].bm ? &g_img[i] : NULL;
    if (g_nimg >= MAX_IMAGES)
        return NULL;
    bm = LoadBmp(path, &w, &h);
    CopyN(g_img[g_nimg].path, MAX_PATH, path, Len(path));
    g_img[g_nimg].bm = bm;
    g_img[g_nimg].w = w;
    g_img[g_nimg].h = h;
    g_nimg++;
    return bm ? &g_img[g_nimg - 1] : NULL;
}

static int CbImage(void *ctx, const hp_char *src, int n, int *w, int *h)
{
    IMAGE *im = Image(src, n);
    (void)ctx;
    if (!im)
        return 0;
    *w = im->w;
    *h = im->h;
    return 1;
}

static const hp_metrics g_metrics = { NULL, CbFont, CbWidth, CbImage };

//------------------------------------------------------------------------------
// view

static int ViewHeight(void)
{
    RECT rc;
    GetClientRect(g_view, &rc);
    return rc.bottom;
}

static void UpdateScroll(void)
{
    SCROLLINFO si;
    memset(&si, 0, sizeof si);
    si.cbSize = sizeof si;
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;
    si.nMin = 0;
    si.nMax = g_height > 0 ? g_height - 1 : 0;
    si.nPage = (UINT)ViewHeight();
    si.nPos = g_scroll;
    SetScrollInfo(g_view, SB_VERT, &si, TRUE);
}

static void ScrollTo(int y)
{
    int max = g_height - ViewHeight();
    if (max < 0)
        max = 0;
    if (y > max)
        y = max;
    if (y < 0)
        y = 0;
    if (y != g_scroll) {
        g_scroll = y;
        InvalidateRect(g_view, NULL, FALSE);
    }
    UpdateScroll();
}

static void Layout(void)
{
    RECT rc;
    GetClientRect(g_view, &rc);
    g_layoutWidth = rc.right;
    g_height = g_doc ? hp_layout(g_doc, &g_metrics, rc.right) : 0;
    InvalidateRect(g_view, NULL, FALSE);
}

static void BackBuffer(HDC hdc, int w, int h)
{
    if (g_bdc && g_bw == w && g_bh == h)
        return;
    if (g_bdc) {
        SelectObject(g_bdc, g_bbmOld);
        DeleteObject(g_bbm);
        DeleteDC(g_bdc);
    }
    g_bdc = CreateCompatibleDC(hdc);
    g_bbm = CreateCompatibleBitmap(hdc, w > 0 ? w : 1, h > 0 ? h : 1);
    g_bbmOld = (HBITMAP)SelectObject(g_bdc, g_bbm);
    g_bw = w;
    g_bh = h;
}

static void Paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    RECT rc;
    HDC hdc = BeginPaint(hwnd, &ps);
    int i, n = 0;
    const hp_item *it = NULL;

    GetClientRect(hwnd, &rc);
    BackBuffer(hdc, rc.right, rc.bottom);
    FillRect(g_bdc, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));
    SetBkMode(g_bdc, TRANSPARENT);
    if (g_doc)
        it = hp_items(g_doc, &n);
    for (i = 0; i < n; i++) {
        int top = it[i].y - g_scroll, len;
        const hp_char *s;
        if (top + it[i].h < 0 || top >= rc.bottom)
            continue;
        switch (it[i].kind) {
        case HP_TEXT:
        case HP_BULLET:
            s = hp_item_text(g_doc, &it[i], &len);
            SelectObject(g_bdc, Font(it[i].font));
            if (it[i].link < 0)
                SetTextColor(g_bdc, RGB(0, 0, 0));
            else if (it[i].link == g_pressLink)
                SetTextColor(g_bdc, RGB(192, 0, 0));
            else
                SetTextColor(g_bdc, RGB(0, 0, 192));
            ExtTextOut(g_bdc, it[i].x, top, 0, NULL, (LPCWSTR)s, (UINT)len, NULL);
            break;
        case HP_RULE: {
            RECT r;
            r.left = it[i].x;
            r.top = top;
            r.right = it[i].x + it[i].w;
            r.bottom = top + 1;
            FillRect(g_bdc, &r, g_rule);
            break;
        }
        case HP_IMAGE: {
            IMAGE *im;
            s = hp_item_text(g_doc, &it[i], &len);
            im = Image(s, len);
            if (im) {
                HBITMAP old = (HBITMAP)SelectObject(g_idc, im->bm);
                BitBlt(g_bdc, it[i].x, top, it[i].w, it[i].h, g_idc, 0, 0, SRCCOPY);
                SelectObject(g_idc, old);
            }
            break;
        }
        default:
            break;
        }
    }
    BitBlt(hdc, 0, 0, rc.right, rc.bottom, g_bdc, 0, 0, SRCCOPY);
    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK ViewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
    switch (msg) {
    case WM_PAINT:
        Paint(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (g_doc && LOWORD(lp) != g_layoutWidth) {
            // keep the same part of the page in view
            int frac = g_height ? (int)((__int64)g_scroll * 10000 / g_height) : 0;
            Layout();
            g_scroll = (int)((__int64)frac * g_height / 10000);
        }
        ScrollTo(g_scroll);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_VSCROLL: {
        int page = ViewHeight() - LINE_STEP;
        switch (LOWORD(wp)) {
        case SB_LINEUP:     ScrollTo(g_scroll - LINE_STEP); break;
        case SB_LINEDOWN:   ScrollTo(g_scroll + LINE_STEP); break;
        case SB_PAGEUP:     ScrollTo(g_scroll - page); break;
        case SB_PAGEDOWN:   ScrollTo(g_scroll + page); break;
        case SB_TOP:        ScrollTo(0); break;
        case SB_BOTTOM:     ScrollTo(g_height); break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: {
            SCROLLINFO si;
            memset(&si, 0, sizeof si);
            si.cbSize = sizeof si;
            si.fMask = SIF_TRACKPOS;
            GetScrollInfo(hwnd, SB_VERT, &si);
            ScrollTo(si.nTrackPos);
            break;
        }
        default:
            break;
        }
        return 0;
    }
    case WM_KEYDOWN: {
        int page = ViewHeight() - LINE_STEP;
        switch (wp) {
        case VK_UP:     ScrollTo(g_scroll - LINE_STEP); break;
        case VK_DOWN:   ScrollTo(g_scroll + LINE_STEP); break;
        case VK_PRIOR:
        case VK_LEFT:   ScrollTo(g_scroll - page); break;
        case VK_NEXT:
        case VK_RIGHT:  ScrollTo(g_scroll + page); break;
        case VK_HOME:   ScrollTo(0); break;
        case VK_END:    ScrollTo(g_height); break;
        case VK_ESCAPE:
        case VK_BACK:   PostMessage(g_main, WM_PH_BACK, 1, 0); break;
        default:        break;
        }
        return 0;
    }
    case WM_LBUTTONDOWN:
        SetFocus(hwnd);
        SetCapture(hwnd);
        g_down = TRUE;
        g_dragging = FALSE;
        g_downY = y;
        g_downScroll = g_scroll;
        g_pressLink = g_doc ? hp_hit(g_doc, x, y + g_scroll) : -1;
        if (g_pressLink >= 0)
            InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_MOUSEMOVE:
        if (g_down) {
            int dy = y - g_downY;
            if (!g_dragging && (dy > DRAG_SLOP || dy < -DRAG_SLOP)) {
                g_dragging = TRUE;
                if (g_pressLink >= 0) {
                    g_pressLink = -1;
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            }
            if (g_dragging)
                ScrollTo(g_downScroll - dy);
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_down) {
            int link = g_pressLink;
            g_down = FALSE;
            g_pressLink = -1;
            ReleaseCapture();
            if (!g_dragging && link >= 0 && g_doc && hp_hit(g_doc, x, y + g_scroll) == link)
                PostMessage(g_main, WM_PH_LINK, (WPARAM)link, 0);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (g_down && (HWND)lp != hwnd) {
            g_down = FALSE;
            g_pressLink = -1;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    default:
        break;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

//------------------------------------------------------------------------------
// pages and navigation

static void SetDoc(hp_doc *d, LPCTSTR path)
{
    TCHAR title[80];
    const hp_char *t;
    int n, i;
    hp_free(g_doc);
    g_doc = d;
    CopyN(g_path, MAX_PATH, path, Len(path));
    CopyN(g_dir, MAX_PATH, path, Len(path));
    for (i = Len(g_dir); i > 0 && g_dir[i - 1] != '\\'; i--)
        ;
    g_dir[i] = 0;
    t = g_doc ? hp_title(g_doc, &n) : NULL;
    if (t)
        CopyN(title, 80, (LPCTSTR)t, n);
    else
        CopyN(title, 80, TEXT("Help"), 4);
    SetWindowText(g_main, title);
    g_scroll = 0;
    Layout();
}

// a page of our own: what went wrong, as HTML (UTF-8)
static void Message(LPCTSTR what, LPCTSTR name, int n)
{
    static const char head[] = "\xEF\xBB\xBF<title>Help</title><b>Help</b><hr><p>";
    unsigned char buf[600];
    int o = 0, i;
    LPCTSTR parts[2];
    int lens[2];
    parts[0] = what;
    lens[0] = Len(what);
    parts[1] = name;
    lens[1] = name ? n : 0;
    memcpy(buf, head, sizeof head - 1);
    o = sizeof head - 1;
    for (i = 0; i < 2; i++) {
        int k;
        for (k = 0; k < lens[i] && o < (int)sizeof buf - 8; k++) {
            unsigned int c = parts[i][k];
            if (c == '<' || c == '&')
                c = ' ';
            if (c < 0x80) {
                buf[o++] = (unsigned char)c;
            } else if (c < 0x800) {
                buf[o++] = (unsigned char)(0xC0 | (c >> 6));
                buf[o++] = (unsigned char)(0x80 | (c & 0x3F));
            } else {
                buf[o++] = (unsigned char)(0xE0 | (c >> 12));
                buf[o++] = (unsigned char)(0x80 | ((c >> 6) & 0x3F));
                buf[o++] = (unsigned char)(0x80 | (c & 0x3F));
            }
        }
    }
    SetDoc(hp_parse(buf, o), TEXT(""));
}

static void PushHistory(void)
{
    if (!g_doc || !g_path[0])
        return;
    if (g_nhist == MAX_HISTORY) {
        memmove(g_hist, g_hist + 1, sizeof(HISTORY) * (MAX_HISTORY - 1));
        g_nhist--;
    }
    CopyN(g_hist[g_nhist].path, MAX_PATH, g_path, Len(g_path));
    g_hist[g_nhist].scroll = g_scroll;
    g_nhist++;
}

static void UpdateButtons(void)
{
    EnableWindow(g_back, g_nhist > 0);
}

// The Help Contents page: one link per shortcut in \Windows\Help, by name.
#define CONTENTS_PATH   TEXT("\\Windows\\mainhelp.htm")
#define CONTENTS_DIR    TEXT("\\Windows\\Help\\")
#define MAX_CONTENTS    64

typedef struct {
    TCHAR name[MAX_PATH];
    TCHAR target[MAX_PATH];
} CONTENTS_ENTRY;

static BOOL IsContentsName(LPCTSTR url, int n)
{
    static const TCHAR file[] = TEXT("mainhelp.htm"), special[] = TEXT("contents");
    int i, s = 0;
    for (i = 0; i < n; i++)
        if (url[i] == '\\' || url[i] == '/')
            s = i + 1;
    if (n - s == 12 && Prefix(url + s, 12, file))
        return TRUE;
    return n == 8 && Prefix(url, 8, special);
}

static int LowCompare(LPCTSTR a, LPCTSTR b)
{
    while (*a && Low(*a) == Low(*b)) {
        a++;
        b++;
    }
    return (int)Low(*a) - (int)Low(*b);
}

static hp_doc *BuildContents(void)
{
    CONTENTS_ENTRY *e = (CONTENTS_ENTRY *)malloc(sizeof(CONTENTS_ENTRY) * MAX_CONTENTS);
    const hp_char *names[MAX_CONTENTS], *targets[MAX_CONTENTS];
    TCHAR path[MAX_PATH];
    WIN32_FIND_DATA fd;
    HANDLE h;
    hp_doc *d = NULL;
    unsigned char *b;
    int count = 0, i, k, n;

    if (!e)
        return NULL;
    CopyN(path, MAX_PATH, CONTENTS_DIR, Len(CONTENTS_DIR));
    Append(path, MAX_PATH, TEXT("*.lnk"));
    h = FindFirstFile(path, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (count == MAX_CONTENTS || (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                continue;
            CopyN(path, MAX_PATH, CONTENTS_DIR, Len(CONTENTS_DIR));
            Append(path, MAX_PATH, fd.cFileName);
            b = ReadAll(path, &n);
            if (!b)
                continue;
            if (hp_lnk_target(b, n, (hp_char *)e[count].target, MAX_PATH)) {
                n = Len(fd.cFileName);
                if (n > 4 && fd.cFileName[n - 4] == '.')
                    n -= 4;
                CopyN(e[count].name, MAX_PATH, fd.cFileName, n);
                count++;
            }
            free(b);
        } while (FindNextFile(h, &fd));
        FindClose(h);
    }
    for (i = 1; i < count; i++) {           // by name
        CONTENTS_ENTRY t = e[i];
        for (k = i; k > 0 && LowCompare(e[k - 1].name, t.name) > 0; k--)
            e[k] = e[k - 1];
        e[k] = t;
    }
    for (i = 0; i < count; i++) {
        names[i] = (const hp_char *)e[i].name;
        targets[i] = (const hp_char *)e[i].target;
    }
    b = hp_contents_page(names, targets, count, &n);
    if (b) {
        d = hp_parse(b, n);
        free(b);
    }
    free(e);
    return d;
}

static hp_doc *LoadDoc(LPCTSTR path)
{
    int n;
    unsigned char *b;
    hp_doc *d;
    if (SameText(path, CONTENTS_PATH))
        return BuildContents();
    b = ReadAll(path, &n);
    if (!b)
        return NULL;
    d = hp_parse(b, n);
    free(b);
    return d;
}

// url = [file:|help:][//]name[#anchor]; not NUL-terminated
static BOOL Navigate(LPCTSTR url, int n, BOOL push)
{
    TCHAR path[MAX_PATH];
    int f, a = -1, fn, an = 0, y;

    while (n > 0 && (url[0] == ' ' || url[0] == '"')) {
        url++;
        n--;
    }
    while (n > 0 && (url[n - 1] == ' ' || url[n - 1] == '"'))
        n--;
    if (Prefix(url, n, TEXT("file:")) || Prefix(url, n, TEXT("help:"))) {
        url += 5;
        n -= 5;
    }
    if (Prefix(url, n, TEXT("//"))) {
        url += 2;
        n -= 2;
    }
    if (Prefix(url, n, TEXT("http:")) || Prefix(url, n, TEXT("https:")) || Prefix(url, n, TEXT("mailto:"))) {
        if (push)
            PushHistory();
        Message(TEXT("This viewer shows help files only, not "), url, n);
        UpdateButtons();
        return FALSE;
    }
    for (f = 0; f < n; f++)
        if (url[f] == '#')
            break;
    if (f == n)
        for (f = 0; f < n; f++)
            if (url[f] == '?')
                break;
    fn = f;
    if (f < n) {
        a = f + 1;
        an = n - a;
    }

    if (fn == 0 && g_doc && g_path[0]) {
        // an anchor in this page
        if (push)
            PushHistory();
    } else {
        hp_doc *d = NULL;
        BOOL found = FALSE;
        if (fn > 0 && IsContentsName(url, fn)) {
            CopyN(path, MAX_PATH, CONTENTS_PATH, Len(CONTENTS_PATH));
            found = TRUE;
        } else if (fn > 0) {
            found = Resolve(url, fn, path);
        }
        if (!found || (d = LoadDoc(path)) == NULL) {
            if (push)
                PushHistory();
            Message(TEXT("Cannot open the help file "), url, fn ? fn : n);
            UpdateButtons();
            return FALSE;
        }
        if (push)
            PushHistory();
        if (SameText(path, g_path))
            hp_free(d);     // same page: keep the laid out one
        else
            SetDoc(d, path);
    }
    y = an > 0 ? hp_anchor(g_doc, (const hp_char *)(url + a), an) : 0;
    g_scroll = -1;
    ScrollTo(y > 0 ? y : 0);
    InvalidateRect(g_view, NULL, FALSE);
    UpdateButtons();
    return TRUE;
}

static void Back(BOOL closeAtFirst)
{
    HISTORY h;
    if (!g_nhist) {
        if (closeAtFirst)
            PostMessage(g_main, WM_CLOSE, 0, 0);
        return;
    }
    h = g_hist[--g_nhist];
    if (!SameText(h.path, g_path)) {
        hp_doc *d = LoadDoc(h.path);
        if (!d) {
            Message(TEXT("Cannot open the help file "), h.path, Len(h.path));
            UpdateButtons();
            return;
        }
        SetDoc(d, h.path);
    }
    g_scroll = -1;
    ScrollTo(h.scroll);
    UpdateButtons();
}

static void Contents(void)
{
    int n;
    const hp_char *c = g_doc ? hp_contents(g_doc, &n) : NULL;
    if (c)
        Navigate((LPCTSTR)c, n, TRUE);
    else if (g_first[0])
        Navigate(g_first, Len(g_first), TRUE);
}

static void FollowLink(int link)
{
    TCHAR url[MAX_URL];
    int n;
    const hp_char *h = g_doc ? hp_href(g_doc, link, &n) : NULL;
    if (!h)
        return;
    CopyN(url, MAX_URL, (LPCTSTR)h, n);     // g_doc goes away when the page changes
    Navigate(url, Len(url), TRUE);
}

//------------------------------------------------------------------------------
// main window

static void FitWorkArea(HWND hwnd)
{
    RECT rc;
    if (!SystemParametersInfo(SPI_GETWORKAREA, 0, &rc, 0))
        SetRect(&rc, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    MoveWindow(hwnd, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, TRUE);
}

static LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_back = CreateWindow(TEXT("BUTTON"), TEXT("Back"), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                              2, 2, 56, BAR_H - 4, hwnd, (HMENU)ID_BACK, g_hinst, NULL);
        g_contents = CreateWindow(TEXT("BUTTON"), TEXT("Contents"), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                  62, 2, 72, BAR_H - 4, hwnd, (HMENU)ID_CONTENTS, g_hinst, NULL);
        g_view = CreateWindow(VIEW_CLASS, TEXT(""), WS_CHILD | WS_VISIBLE | WS_VSCROLL,
                              0, BAR_H, 10, 10, hwnd, (HMENU)ID_VIEW, g_hinst, NULL);
        return g_back && g_contents && g_view ? 0 : -1;
    case WM_SIZE: {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(g_view, 0, BAR_H, w, h > BAR_H ? h - BAR_H : 0, TRUE);
        return 0;
    }
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        rc.bottom = BAR_H;
        FillRect((HDC)wp, &rc, (HBRUSH)GetStockObject(LTGRAY_BRUSH));
        return 1;
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE && g_view)
            SetFocus(g_view);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_BACK:       Back(FALSE); break;
        case ID_CONTENTS:   Contents(); break;
        default:            break;
        }
        SetFocus(g_view);
        return 0;
    case WM_PH_LINK:
        FollowLink((int)wp);
        return 0;
    case WM_PH_BACK:
        Back(wp != 0);
        return 0;
    case WM_COPYDATA: {
        const COPYDATASTRUCT *cds = (const COPYDATASTRUCT *)lp;
        if (cds && cds->dwData == COPYDATA_URL && cds->lpData) {
            int n = (int)(cds->cbData / sizeof(TCHAR));
            LPCTSTR s = (LPCTSTR)cds->lpData;
            while (n > 0 && !s[n - 1])
                n--;
            Navigate(s, n, TRUE);
            SetForegroundWindow(hwnd);
            return TRUE;
        }
        return FALSE;
    }
    case WM_SETTINGCHANGE:
        if (wp == SPI_SETWORKAREA)
            FitWorkArea(hwnd);
        return 0;
    case WM_DISPLAYCHANGE:
        FitWorkArea(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR cmd, int show)
{
    WNDCLASS wc;
    MSG msg;
    HWND other;
    int n;
    (void)hPrev;
    (void)show;

    if (!cmd)
        cmd = TEXT("");
    n = Len(cmd);

    // one viewer: a second start hands its page to the first
    other = FindWindow(HELP_CLASS, NULL);
    if (other) {
        COPYDATASTRUCT cds;
        cds.dwData = COPYDATA_URL;
        cds.cbData = (DWORD)((n + 1) * sizeof(TCHAR));
        cds.lpData = cmd;
        if (n)
            SendMessage(other, WM_COPYDATA, 0, (LPARAM)&cds);
        SetForegroundWindow(other);
        return 0;
    }

    g_hinst = hInst;
    g_mdc = CreateCompatibleDC(NULL);
    g_idc = CreateCompatibleDC(NULL);
    g_rule = CreateSolidBrush(RGB(128, 128, 128));
    if (!g_mdc || !g_idc)
        return 1;

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = MainProc;
    wc.hInstance = hInst;
    wc.lpszClassName = HELP_CLASS;
    if (!RegisterClass(&wc))
        return 1;
    wc.lpfnWndProc = ViewProc;
    wc.lpszClassName = VIEW_CLASS;
    if (!RegisterClass(&wc))
        return 1;

    g_main = CreateWindowEx(0, HELP_CLASS, TEXT("Help"), WS_VISIBLE | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, NULL, NULL, hInst, NULL);
    if (!g_main)
        return 1;
    FitWorkArea(g_main);

    CopyN(g_first, MAX_URL, cmd, n);
    if (n)
        Navigate(cmd, n, FALSE);
    else
        Message(TEXT("No help topic was given."), NULL, 0);
    UpdateButtons();
    ShowWindow(g_main, SW_SHOW);
    UpdateWindow(g_main);
    SetFocus(g_view);

    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    hp_free(g_doc);
    return (int)msg.wParam;
}
