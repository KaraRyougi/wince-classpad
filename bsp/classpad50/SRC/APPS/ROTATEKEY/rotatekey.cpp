// The service for the icon strip and the Keyboard key: the touch DDSI and the
// keyboard layout (inside GWES) only set a named event; the work happens
// here, outside GWES. Never rotate synchronously inside the touch DDSI.
//
// The strip icons act as HP Xpander's keys (the calculator Math Xpander was
// written for: HOME, MATH, MENU and ESC beside its D-pad):
//   Menu      HOME: the Windows key (VK_LWIN), the Start menu
//   Main      MATH: brings Math Xpander to the front, else starts
//             \Windows\MathXpander.exe
//   Settings  MENU: opens the foreground program's menu bar, as a tap on its
//             first menu (aygshell's menu bar, else a command bar); VK_MENU
//             (Alt) when there is none
//   Esc       ESC: VK_ESCAPE
//   Rotate    portrait <-> landscape (ChangeDisplaySettingsEx)
// Swap is this image's own: it starts the task manager (\Windows\ITaskMgr.exe,
// SRC\APPS\ITASKMGR; a second start brings the running one to the front), the
// way out when a program's close button cannot be reached. Without that
// program in the image it opens the shell's Task Manager dialog, which
// otherwise needs Alt+Tab. Resize has no event. The fx-CP400 Keyboard key shows Math
// Xpander's math keyboard (MathKeyPad, a SIP input method registered in
// platform.reg), or hides it when it is the one showing.
//
// Keys and taps go to the foreground window like a keyboard's and a stylus's
// (the strip never moves the focus: its contacts are not clicks).
#include <windows.h>
#include <commctrl.h>
#include <sipapi.h>
#include <stdlib.h>

enum { EV_ROTATE, EV_MENU, EV_MAIN, EV_ESC, EV_SETTINGS, EV_KEYBOARD, EV_SWAP, EV_COUNT };

static const LPCTSTR g_eventName[EV_COUNT] = {
    TEXT("ClassPadRotateKey"), TEXT("ClassPadIconMenu"), TEXT("ClassPadIconMain"),
    TEXT("ClassPadIconEsc"), TEXT("ClassPadIconSettings"), TEXT("ClassPadKeyboardKey"),
    TEXT("ClassPadIconSwap")
};

// MathKeyPad.dll's input method (its DllRegisterServer's CLSID)
static CLSID g_clsidMathKeyPad =
    { 0xD8183B68, 0x05D1, 0x11D4, { 0x80, 0xD9, 0x00, 0x10, 0x83, 0x02, 0x30, 0x57 } };

// GWES only broadcasts WM_SETTINGCHANGE after a rotation; it moves no window.
// Programs written for one screen shape ignore it and keep their old
// rectangle, with the close button past the new right edge. So after a
// rotation every visible top-level window that sticks out of the new screen
// is put back: one that filled the old work area (or the old screen) fills
// the new one, anything else (a dialog) keeps its size and is moved so that
// its top right corner, where the close button is, is on the screen.
struct REFIT { RECT oldScreen, oldWork, screen, work; int count, bars; };

static BOOL IsToolbarClass(LPCTSTR cls)
{
    return !wcscmp(cls, TEXT("ToolbarWindow32")) || !wcscmp(cls, TEXT("CommandBar"));
}

// Messages to another program's toolbar. A message's pointer argument is
// not carried across processes: the toolbar's thread reads and writes that
// address in ITS OWN address space. A pointer to this program's stack there
// wrote a TBBUTTON into the other program's stack area (WordPad kept
// running with it; Plotter-Z's page was not committed and it died:
// "Exception 060 ... commctrl.dll", 2026-11-14). A view of a memory-mapped
// file is at the same address in every process of CE 5.0, so everything the
// toolbar is to fill in or read lives in one such page. And a program that
// does not answer must not hang the strip: every message has a time limit.
#define BAR_MSG_MS 3000
static BYTE *g_shared;

static BYTE *Shared(void)
{
    if (!g_shared) {
        HANDLE map = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 4096, NULL);
        if (map) g_shared = (BYTE *)MapViewOfFile(map, FILE_MAP_WRITE, 0, 0, 0);
        if (!g_shared) RETAILMSG(1, (TEXT("ROTATEKEY: no shared page: %d\r\n"), GetLastError()));
    }
    return g_shared;
}

static LRESULT BarMsg(HWND bar, UINT msg, WPARAM wParam, LPARAM lParam)
{
    DWORD result = 0;
    if (!SendMessageTimeout(bar, msg, wParam, lParam, SMTO_NORMAL, BAR_MSG_MS, &result)) return 0;
    return (LRESULT)result;
}

// A toolbar button's rectangle, through the shared page
static BOOL BarItemRect(HWND bar, int index, RECT *out)
{
    RECT *r = (RECT *)Shared();
    if (!r) return FALSE;
    SetRectEmpty(r);
    if (!BarMsg(bar, TB_GETITEMRECT, (WPARAM)index, (LPARAM)r)) return FALSE;
    *out = *r;
    return TRUE;
}

// CommandBar_AlignAdornments for a bar of another process. The library
// function sends its messages with pointers to its own stack, so it is done
// again here with the shared page, step for step (commctrl.dll of CE 5.0,
// disassembled): among the last five buttons find the close / OK / help
// buttons (command IDs WM_CLOSE, IDOK, WM_HELP); the button before the first
// of them must be the separator that CommandBar_AddAdornments put there
// (style TBSTYLE_SEP, command ID 0xCCDEADCC); its width becomes whatever is
// left of the bar's client width after the buttons behind it.
#define ADORNMENT_SEPARATOR_ID 0xCCDEADCC

static void AlignAdornments(HWND bar)
{
    BYTE *page = Shared();
    if (!page) return;
    TBBUTTON *tbb = (TBBUTTON *)(page + 64);
    TBBUTTONINFO *info = (TBBUTTONINFO *)(page + 128);
    int n = (int)BarMsg(bar, TB_BUTTONCOUNT, 0, 0), i, sep = -1, avail;
    RECT client, r;
    for (i = n - 1; i >= 0 && i >= n - 5; i--) {
        ZeroMemory(tbb, sizeof *tbb);
        if (BarMsg(bar, TB_GETBUTTON, (WPARAM)i, (LPARAM)tbb)
            && (tbb->idCommand == WM_CLOSE || tbb->idCommand == IDOK || tbb->idCommand == WM_HELP))
            sep = i - 1;
    }
    if (sep < 0 || !GetClientRect(bar, &client)) return;
    avail = client.right - client.left;
    for (i = n - 1; i > sep; i--)
        if (BarItemRect(bar, i, &r)) avail -= r.right - r.left;
    ZeroMemory(tbb, sizeof *tbb);
    if (!BarMsg(bar, TB_GETBUTTON, (WPARAM)sep, (LPARAM)tbb)
        || tbb->fsStyle != TBSTYLE_SEP || (DWORD)tbb->idCommand != ADORNMENT_SEPARATOR_ID)
        return;
    if (!BarItemRect(bar, sep, &r) || r.left >= avail) return;     // no room: as it is
    ZeroMemory(info, sizeof *info);
    info->cbSize = sizeof *info;
    info->dwMask = TBIF_SIZE;
    info->cx = (WORD)(avail - r.left);
    BarMsg(bar, TB_SETBUTTONINFO, (WPARAM)ADORNMENT_SEPARATOR_ID, (LPARAM)info);
}

// The bars across the top of a window after its width changed. A command
// bar follows its parent only when its program tells it to, which programs
// written for one screen shape do not do: it gets the client width and its
// adornments (close, OK, help) go to the right end. Command bands (a rebar:
// WordPad) only need the new width, the bands arrange themselves. Returns
// the number of bars changed.
static int RefitBars(HWND hwnd)
{
    RECT client, r; POINT origin = { 0, 0 }; TCHAR cls[32];
    int changed = 0;
    if (!GetClientRect(hwnd, &client) || client.right <= 0) return 0;
    ClientToScreen(hwnd, &origin);
    for (HWND c = GetWindow(hwnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
        if (!IsWindowVisible(c) || !GetClassName(c, cls, 32) || !GetWindowRect(c, &r)) continue;
        BOOL bar = IsToolbarClass(cls), bands = !wcscmp(cls, TEXT("ReBarWindow32"));
        if (!bar && !bands) continue;
        if (r.left - origin.x > 2 || r.top - origin.y > 2) continue;       // only a bar that starts at the top left corner
        if (r.right - r.left == client.right && !bar) continue;
        if (r.right - r.left != client.right) {
            SetWindowPos(c, NULL, 0, 0, client.right, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            changed++;
        }
        if (bar) AlignAdornments(c);
    }
    return changed;
}

static BOOL Covers(const RECT *r, const RECT *area)
{
    return r->left <= area->left && r->top <= area->top && r->right >= area->right && r->bottom >= area->bottom;
}

static BOOL CALLBACK RefitWindow(HWND hwnd, LPARAM lParam)
{
    REFIT *f = (REFIT *)lParam;
    RECT r; TCHAR cls[32];
    if (!IsWindowVisible(hwnd) || !GetWindowRect(hwnd, &r) || r.right <= r.left || r.bottom <= r.top) return TRUE;
    // the shell and the SIP place themselves
    if (GetClassName(hwnd, cls, 32) && (!wcscmp(cls, TEXT("HHTaskBar")) || !wcscmp(cls, TEXT("DesktopExplorerWindow"))
        || !wcscmp(cls, TEXT("SipWndClass")) || !wcscmp(cls, TEXT("SipBackDropWndClass"))))
        return TRUE;
    if (!(r.left >= f->screen.left && r.top >= f->screen.top && r.right <= f->screen.right && r.bottom <= f->screen.bottom)) {
        int x = r.left, y = r.top, w = r.right - r.left, h = r.bottom - r.top;
        if (Covers(&r, &f->oldWork)) {
            const RECT *to = Covers(&r, &f->oldScreen) ? &f->screen : &f->work;
            x = to->left; y = to->top; w = to->right - to->left; h = to->bottom - to->top;
        } else {
            if (x + w > f->work.right) x = f->work.right - w;
            if (x < f->work.left && w <= f->work.right - f->work.left) x = f->work.left;
            if (y + h > f->work.bottom) y = f->work.bottom - h;
            if (y < f->work.top) y = f->work.top;
        }
        SetWindowPos(hwnd, NULL, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
        f->count++;
    }
    // its bars, whether this moved the window or the program did itself
    f->bars += RefitBars(hwnd);
    return TRUE;
}

static void RefitWindows(const RECT *oldScreen, const RECT *oldWork)
{
    REFIT f; int i;
    f.oldScreen = *oldScreen; f.oldWork = *oldWork; f.count = 0; f.bars = 0;
    SetRect(&f.screen, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    // Let the programs that do follow the rotation move first, and wait for
    // the taskbar to set the new work area (the old one does not fit the
    // turned screen).
    Sleep(500);
    for (i = 0; i < 30; i++) {
        if (SystemParametersInfo(SPI_GETWORKAREA, 0, &f.work, 0) && f.work.right > f.work.left && f.work.bottom > f.work.top
            && f.work.right <= f.screen.right && f.work.bottom <= f.screen.bottom)
            break;
        Sleep(100);
    }
    if (i == 30) f.work = f.screen;
    EnumWindows(RefitWindow, (LPARAM)&f);
    RETAILMSG(1, (TEXT("ROTATEKEY: %d windows put back on the screen, %d bars given the new width\r\n"), f.count, f.bars));
}

static void Rotate(void)
{
    RECT oldScreen, oldWork;
    SetRect(&oldScreen, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    if (!SystemParametersInfo(SPI_GETWORKAREA, 0, &oldWork, 0)) oldWork = oldScreen;
    DEVMODE dm; ZeroMemory(&dm, sizeof dm); dm.dmSize = sizeof dm;
    dm.dmFields = DM_DISPLAYORIENTATION;
    LONG rc = ChangeDisplaySettingsEx(NULL, &dm, NULL, CDS_TEST, NULL);
    if (rc != DISP_CHANGE_SUCCESSFUL) {
        RETAILMSG(1, (TEXT("ROTATEKEY: query failed %d\r\n"), rc));
        return;
    }
    dm.dmDisplayOrientation = (dm.dmDisplayOrientation == DMDO_90 || dm.dmDisplayOrientation == DMDO_270) ? DMDO_0 : DMDO_90;
    rc = ChangeDisplaySettingsEx(NULL, &dm, NULL, 0, NULL);
    RETAILMSG(1, (TEXT("ROTATEKEY: mode=%d result=%d size=%dx%d\r\n"), dm.dmDisplayOrientation, rc,
        GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)));
    if (rc == DISP_CHANGE_SUCCESSFUL) RefitWindows(&oldScreen, &oldWork);
}

static void Key(BYTE vk)
{
    keybd_event(vk, 0, 0, 0);
    keybd_event(vk, 0, KEYEVENTF_KEYUP, 0);
}

// Math Xpander's frame: a visible top-level window with "Xpander" in its title.
static BOOL CALLBACK FindXpander(HWND hwnd, LPARAM lParam)
{
    TCHAR title[64];
    if (GetParent(hwnd) == NULL && IsWindowVisible(hwnd) && GetWindowText(hwnd, title, 64) > 0
        && wcsstr(title, TEXT("Xpander")) != NULL) {
        *(HWND *)lParam = hwnd;
        return FALSE;
    }
    return TRUE;
}

static void MathXpander(void)
{
    HWND hwnd = NULL;
    EnumWindows(FindXpander, (LPARAM)&hwnd);
    if (hwnd) {
        SetForegroundWindow(hwnd);
        RETAILMSG(1, (TEXT("ROTATEKEY: Math Xpander to the front\r\n")));
        return;
    }
    PROCESS_INFORMATION pi;
    if (CreateProcess(TEXT("\\Windows\\MathXpander.exe"), NULL, NULL, NULL, FALSE, 0, NULL, NULL, NULL, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        RETAILMSG(1, (TEXT("ROTATEKEY: Math Xpander started\r\n")));
    } else {
        RETAILMSG(1, (TEXT("ROTATEKEY: cannot start Math Xpander: %d\r\n"), GetLastError()));
    }
}

// ITaskMgr; else the Task Manager dialog of the shell's taskbar (explorer's
// IDM_TASKBAR_TASKMAN, the command of its Alt+Tab and of the taskbar's menu).
static void TaskManager(void)
{
    PROCESS_INFORMATION pi;
    if (CreateProcess(TEXT("\\Windows\\ITaskMgr.exe"), NULL, NULL, NULL, FALSE, 0, NULL, NULL, NULL, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        RETAILMSG(1, (TEXT("ROTATEKEY: task manager started\r\n")));
        return;
    }
    DWORD err = GetLastError();
    HWND taskbar = FindWindow(TEXT("HHTaskBar"), NULL);
    if (taskbar) PostMessage(taskbar, WM_COMMAND, 3302, 0);
    RETAILMSG(1, (TEXT("ROTATEKEY: cannot start ITaskMgr: %d, shell task manager %s\r\n"), err,
        taskbar ? TEXT("opened") : TEXT("not found")));
}

// A stylus tap at the centre of a toolbar's first button, in screen coordinates.
static BOOL TapFirstButton(HWND tb)
{
    RECT r;
    if (!tb || !IsWindowVisible(tb) || BarMsg(tb, TB_BUTTONCOUNT, 0, 0) <= 0) return FALSE;
    if (!BarItemRect(tb, 0, &r)) return FALSE;                  // through the shared page: tb is another program's
    POINT p = { (r.left + r.right) / 2, (r.top + r.bottom) / 2 };
    ClientToScreen(tb, &p);
    int cx = GetSystemMetrics(SM_CXSCREEN), cy = GetSystemMetrics(SM_CYSCREEN);
    if (p.x < 0 || p.y < 0 || p.x >= cx || p.y >= cy || cx < 2 || cy < 2) return FALSE;
    DWORD x = (DWORD)p.x * 65535 / (cx - 1), y = (DWORD)p.y * 65535 / (cy - 1);
    mouse_event(MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE | MOUSEEVENTF_LEFTDOWN, x, y, 0, 0);
    mouse_event(MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_LEFTUP, x, y, 0, 0);
    RETAILMSG(1, (TEXT("ROTATEKEY: menu bar tapped at %d,%d\r\n"), p.x, p.y));
    return TRUE;
}

// A toolbar among w and its descendants (a command bar is one).
static HWND FindToolbar(HWND w, int depth)
{
    TCHAR cls[32];
    for (HWND c = GetWindow(w, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
        if (GetClassName(c, cls, 32) && IsToolbarClass(cls)
            && IsWindowVisible(c) && BarMsg(c, TB_BUTTONCOUNT, 0, 0) > 0)
            return c;
        if (depth > 0) {
            HWND t = FindToolbar(c, depth - 1);
            if (t) return t;
        }
    }
    return NULL;
}

typedef HWND (*PFN_SHFINDMENUBAR)(HWND);

static void OpenMenuBar(void)
{
    static PFN_SHFINDMENUBAR pfnFind;
    static BOOL tried;
    HWND top = GetForegroundWindow(), mb = NULL;
    if (!tried) {
        HMODULE h = LoadLibrary(TEXT("aygshell.dll"));
        if (h) pfnFind = (PFN_SHFINDMENUBAR)GetProcAddress(h, TEXT("SHFindMenuBar"));
        tried = TRUE;
    }
    while (top && GetParent(top)) top = GetParent(top);
    if (top && pfnFind) mb = pfnFind(top);
    if (mb && (TapFirstButton(mb) || TapFirstButton(FindToolbar(mb, 1)))) return;
    if (top && TapFirstButton(FindToolbar(top, 2))) return;
    RETAILMSG(1, (TEXT("ROTATEKEY: no menu bar, sending Alt\r\n")));
    Key(VK_MENU);
}

// Show MathKeyPad, or hide the SIP when MathKeyPad is what it shows.
static void MathKeyboard(void)
{
    SIPINFO si; CLSID cur;
    ZeroMemory(&si, sizeof si); si.cbSize = sizeof si;
    BOOL on = SipGetInfo(&si) && (si.fdwFlags & SIPF_ON);
    BOOL mine = SipGetCurrentIM(&cur) && !memcmp(&cur, &g_clsidMathKeyPad, sizeof cur);
    if (on && mine) {
        SipShowIM(SIPF_OFF);
        RETAILMSG(1, (TEXT("ROTATEKEY: math keyboard hidden\r\n")));
        return;
    }
    if (!mine && !SipSetCurrentIM(&g_clsidMathKeyPad))
        RETAILMSG(1, (TEXT("ROTATEKEY: MathKeyPad not available: %d\r\n"), GetLastError()));
    SipShowIM(SIPF_ON);
    RETAILMSG(1, (TEXT("ROTATEKEY: math keyboard shown\r\n")));
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPTSTR command, int)
{
    HANDLE events[EV_COUNT];
    int i;
    HANDLE singleton = CreateMutex(NULL, FALSE, TEXT("ClassPadRotateKeyService"));
    if (!singleton) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(singleton); return 0; }
    for (i = 0; i < EV_COUNT; i++) {
        events[i] = CreateEvent(NULL, FALSE, FALSE, g_eventName[i]);
        if (!events[i]) {
            while (i-- > 0) CloseHandle(events[i]);
            CloseHandle(singleton); return 1;
        }
    }
    if (command && *command) SignalStarted((DWORD)_wtol(command));
    for (;;) {
        DWORD rc = WaitForMultipleObjects(EV_COUNT, events, FALSE, INFINITE);
        if (rc >= WAIT_OBJECT_0 + EV_COUNT) break;
        switch (rc - WAIT_OBJECT_0) {
        case EV_ROTATE:   Rotate(); break;
        case EV_MENU:     Key(VK_LWIN); break;
        case EV_MAIN:     MathXpander(); break;
        case EV_ESC:      Key(VK_ESCAPE); break;
        case EV_SETTINGS: OpenMenuBar(); break;
        case EV_KEYBOARD: MathKeyboard(); break;
        case EV_SWAP:     TaskManager(); break;
        }
    }
    for (i = 0; i < EV_COUNT; i++) CloseHandle(events[i]);
    CloseHandle(singleton); return 1;
}
