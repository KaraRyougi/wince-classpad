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
// Resize and Swap have no event. The fx-CP400 Keyboard key shows Math
// Xpander's math keyboard (MathKeyPad, a SIP input method registered in
// platform.reg), or hides it when it is the one showing.
//
// Keys and taps go to the foreground window like a keyboard's and a stylus's
// (the strip never moves the focus: its contacts are not clicks).
#include <windows.h>
#include <commctrl.h>
#include <sipapi.h>
#include <stdlib.h>

enum { EV_ROTATE, EV_MENU, EV_MAIN, EV_ESC, EV_SETTINGS, EV_KEYBOARD, EV_COUNT };

static const LPCTSTR g_eventName[EV_COUNT] = {
    TEXT("ClassPadRotateKey"), TEXT("ClassPadIconMenu"), TEXT("ClassPadIconMain"),
    TEXT("ClassPadIconEsc"), TEXT("ClassPadIconSettings"), TEXT("ClassPadKeyboardKey")
};

// MathKeyPad.dll's input method (its DllRegisterServer's CLSID)
static CLSID g_clsidMathKeyPad =
    { 0xD8183B68, 0x05D1, 0x11D4, { 0x80, 0xD9, 0x00, 0x10, 0x83, 0x02, 0x30, 0x57 } };

static void Rotate(void)
{
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

// A stylus tap at the centre of a toolbar's first button, in screen coordinates.
static BOOL TapFirstButton(HWND tb)
{
    RECT r;
    if (!tb || !IsWindowVisible(tb) || SendMessage(tb, TB_BUTTONCOUNT, 0, 0) <= 0) return FALSE;
    if (!SendMessage(tb, TB_GETITEMRECT, 0, (LPARAM)&r)) return FALSE;
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
        if (GetClassName(c, cls, 32) && (!wcscmp(cls, TEXT("ToolbarWindow32")) || !wcscmp(cls, TEXT("CommandBar")))
            && IsWindowVisible(c) && SendMessage(c, TB_BUTTONCOUNT, 0, 0) > 0)
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
        }
    }
    for (i = 0; i < EV_COUNT; i++) CloseHandle(events[i]);
    CloseHandle(singleton); return 1;
}
