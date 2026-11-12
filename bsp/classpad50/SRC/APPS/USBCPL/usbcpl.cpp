//------------------------------------------------------------------------------
//  usbcpl.cpp - "USB Connection" in the Control Panel (classpad_usb.cpl)
//
//  The USB function controller driver (SRC\DRIVERS\USBFN) runs one function
//  client at a time: "Mass_Storage_Class" (Casio's storage memory as a USB
//  drive) or "CDC_ACM" (a serial port for PPP or a terminal). The applet's
//  dialog shows the two as "USB Mode" with the running one selected; OK with
//  the other selected changes the running client and the default for the
//  next boot (the MDD's IOCTL_UFN_CHANGE_CURRENT_CLIENT and
//  IOCTL_UFN_CHANGE_DEFAULT_CLIENT, on a bus handle: classpad_ufn.h). The
//  host sees the cable unplugged and plugged again as the new device, so a
//  drive still mounted there should be ejected first.
//
//  Built as classpad_usb.dll; platform.bib puts it into the image under the
//  name classpad_usb.cpl.
//------------------------------------------------------------------------------
#include <windows.h>
#include <cpl.h>
#include <usbfnioctl.h>
#include "classpad_ufn.h"
#include "usbcpl.h"

#define CLIENT_STORAGE  TEXT("Mass_Storage_Class")
#define CLIENT_SERIAL   TEXT("CDC_ACM")
#define TITLE           TEXT("USB Connection")

static HINSTANCE g_instance;

BOOL WINAPI DllEntry(HANDLE instance, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_instance = (HINSTANCE)instance;
        DisableThreadLibraryCalls((HMODULE)instance);
    }
    return TRUE;
}

// The running client's name into name[] (empty: none). FALSE: the driver does not answer.
static BOOL CurrentClient(LPTSTR name)
{
    UFN_CLIENT_INFO current;
    DWORD returned;
    BOOL ok;
    HANDLE ufn = ClasspadOpenUfnBus();

    name[0] = 0;
    if (ufn == INVALID_HANDLE_VALUE) return FALSE;
    memset(&current, 0, sizeof current);
    ok = DeviceIoControl(ufn, IOCTL_UFN_GET_CURRENT_CLIENT, NULL, 0, &current, sizeof current, &returned, NULL);
    CloseHandle(ufn);
    if (ok) _tcscpy(name, current.szName);
    return ok;
}

// previous: the client to put back when the new one does not load (the MDD
// has unloaded it by then, and USB would stay dead until the next boot)
static BOOL ChangeClient(LPCTSTR name, LPCTSTR previous)
{
    UFN_CLIENT_NAME wanted;
    DWORD returned;
    BOOL ok;
    HANDLE ufn = ClasspadOpenUfnBus();

    if (ufn == INVALID_HANDLE_VALUE) return FALSE;
    memset(&wanted, 0, sizeof wanted);
    _tcscpy(wanted.szName, name);
    ok = DeviceIoControl(ufn, IOCTL_UFN_CHANGE_CURRENT_CLIENT, &wanted, sizeof wanted, NULL, 0, &returned, NULL);
    if (ok) {
        DeviceIoControl(ufn, IOCTL_UFN_CHANGE_DEFAULT_CLIENT, &wanted, sizeof wanted, NULL, 0, &returned, NULL);
    } else if (previous[0]) {
        memset(&wanted, 0, sizeof wanted);
        _tcscpy(wanted.szName, previous);
        DeviceIoControl(ufn, IOCTL_UFN_CHANGE_CURRENT_CLIENT, &wanted, sizeof wanted, NULL, 0, &returned, NULL);
    }
    CloseHandle(ufn);
    return ok;
}

static BOOL CALLBACK ModeProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
    static TCHAR current[UFN_CLIENT_NAME_MAX_CHARS];
    LPCTSTR wanted;
    HCURSOR old;
    BOOL ok;

    switch (message) {
    case WM_INITDIALOG:
        CurrentClient(current);
        CheckRadioButton(dialog, IDC_USB_STORAGE, IDC_USB_SERIAL, _tcsicmp(current, CLIENT_SERIAL) ? IDC_USB_STORAGE : IDC_USB_SERIAL);
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDOK:
            wanted = IsDlgButtonChecked(dialog, IDC_USB_SERIAL) == BST_CHECKED ? CLIENT_SERIAL : CLIENT_STORAGE;
            if (_tcsicmp(current, wanted)) {
                old = SetCursor(LoadCursor(NULL, IDC_WAIT));
                ok = ChangeClient(wanted, current);
                SetCursor(old);
                if (!ok) {
                    MessageBox(dialog, TEXT("Cannot change the USB mode."), TITLE, MB_OK | MB_ICONEXCLAMATION);
                    return TRUE;
                }
            }
            EndDialog(dialog, 1);
            return TRUE;
        case IDCANCEL:
            EndDialog(dialog, 0);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

extern "C" LONG CALLBACK CPlApplet(HWND cpl, UINT message, LPARAM param1, LPARAM param2)
{
    TCHAR current[UFN_CLIENT_NAME_MAX_CHARS];
    LPNEWCPLINFO info;
    LPCPLINFO old;

    switch (message) {
    case CPL_INIT:
        return TRUE;
    case CPL_GETCOUNT:
        return 1;
    case CPL_INQUIRE:
        old = (LPCPLINFO)param2;
        old->idIcon = IDI_USB;
        old->idName = IDS_USB_NAME;
        old->idInfo = IDS_USB_INFO;
        old->lData = 0;
        return 0;
    case CPL_NEWINQUIRE:
        info = (LPNEWCPLINFO)param2;
        memset(info, 0, sizeof *info);
        info->dwSize = sizeof *info;
        info->hIcon = LoadIcon(g_instance, MAKEINTRESOURCE(IDI_USB));
        LoadString(g_instance, IDS_USB_NAME, info->szName, sizeof info->szName / sizeof info->szName[0]);
        LoadString(g_instance, IDS_USB_INFO, info->szInfo, sizeof info->szInfo / sizeof info->szInfo[0]);
        return 0;
    case CPL_DBLCLK:
        if (!CurrentClient(current)) MessageBox(cpl, TEXT("USB is not available."), TITLE, MB_OK | MB_ICONEXCLAMATION);
        else DialogBox(g_instance, MAKEINTRESOURCE(IDD_USB), cpl, ModeProc);
        return 0;
    case CPL_STOP:
    case CPL_EXIT:
        return 0;
    }
    return 0;
}
