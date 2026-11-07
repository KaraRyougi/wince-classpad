// The icon strip's service: the touch DDSI (inside GWES) only sets a named
// event when an icon is released; the work happens here, outside GWES. Never
// rotate synchronously inside the touch DDSI.
//
//   Rotate    portrait <-> landscape (ChangeDisplaySettingsEx)
//   Menu      Home: the Windows key (VK_LWIN), the Start menu, as the
//             fx-CG100's HOME key
//   Main      Math Xpander: brings its window to the front, else starts
//             \Windows\MathXpander.exe
//   Esc       VK_ESCAPE
//   Settings  the Menu key (VK_APPS), as the fx-CG100's SETTINGS key
//
// Resize and Swap have no event. Keys go to the foreground window like a
// keyboard's (the strip never moves the focus: its contacts are not clicks).
#include <windows.h>
#include <stdlib.h>

enum { ICON_ROTATE, ICON_MENU, ICON_MAIN, ICON_ESC, ICON_SETTINGS, ICON_COUNT };

static const LPCTSTR g_eventName[ICON_COUNT] = {
    TEXT("ClassPadRotateKey"), TEXT("ClassPadIconMenu"), TEXT("ClassPadIconMain"),
    TEXT("ClassPadIconEsc"), TEXT("ClassPadIconSettings")
};

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

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPTSTR command, int)
{
    HANDLE events[ICON_COUNT];
    int i;
    HANDLE singleton = CreateMutex(NULL, FALSE, TEXT("ClassPadRotateKeyService"));
    if (!singleton) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(singleton); return 0; }
    for (i = 0; i < ICON_COUNT; i++) {
        events[i] = CreateEvent(NULL, FALSE, FALSE, g_eventName[i]);
        if (!events[i]) {
            while (i-- > 0) CloseHandle(events[i]);
            CloseHandle(singleton); return 1;
        }
    }
    if (command && *command) SignalStarted((DWORD)_wtol(command));
    for (;;) {
        DWORD rc = WaitForMultipleObjects(ICON_COUNT, events, FALSE, INFINITE);
        if (rc >= WAIT_OBJECT_0 + ICON_COUNT) break;
        switch (rc - WAIT_OBJECT_0) {
        case ICON_ROTATE:   Rotate(); break;
        case ICON_MENU:     Key(VK_LWIN); break;
        case ICON_MAIN:     MathXpander(); break;
        case ICON_ESC:      Key(VK_ESCAPE); break;
        case ICON_SETTINGS: Key(VK_APPS); break;
        }
    }
    for (i = 0; i < ICON_COUNT; i++) CloseHandle(events[i]);
    CloseHandle(singleton); return 1;
}
