//------------------------------------------------------------------------------
//  storcpl.cpp - "Fugue File System" in the Control Panel (classpad_storage.cpl)
//
//  Casio's storage memory (the \Storage folder) lives in a log-structured
//  layer on the flash: every change is a new record, and the records it
//  supersedes stay until their flash sector is reclaimed. The storage driver
//  (SRC\DRIVERS\FUGUEDSK) reclaims by itself when free sectors run low; this
//  applet shows where the volume stands and has the button that tidies it at
//  once: "Optimize", every sector that holds old copies reclaimed, as many
//  sectors free as the content allows (classpad_fuguewr.h
//  ClasspadFugueOptimize; what "Optimize" in Casio's own storage menu is for).
//
//  The dialog asks the driver through its ioctls (classpad_fugueioctl.h)
//  when it opens and after an optimise; nothing runs in between and nothing
//  runs when the dialog is not open. The optimise goes one sector per call to
//  the driver, so that a progress bar can move between them, as Casio's does;
//  each sector is an erase of about half a second during which the device
//  does not respond.
//
//  Built as classpad_storage.dll; platform.bib puts it into the image under
//  the name classpad_storage.cpl.
//------------------------------------------------------------------------------
#include <windows.h>
#include <winioctl.h>
#include <commctrl.h>
#include <cpl.h>
#include "classpad_fugueioctl.h"
#include "storcpl.h"

#define DISK_NAME   TEXT("DSK1:")
#define TITLE       TEXT("Fugue File System")
#define PROFILE_KEY TEXT("System\\StorageManager\\Profiles\\ClassPadStorage")

// The icon is the shell's own for a storage volume, the one the Storage
// folder has in My Device: ceshell.dll's IDI_STORAGECARD (its resource.h).
#define CESHELL_STORAGE_ICON    0x1503

static HINSTANCE g_instance;

static HICON StorageIcon(void)
{
    static HMODULE shell;
    HICON icon = NULL;

    if (!shell) shell = LoadLibraryEx(TEXT("ceshell.dll"), NULL, LOAD_LIBRARY_AS_DATAFILE);
    if (shell) icon = LoadIcon(shell, MAKEINTRESOURCE(CESHELL_STORAGE_ICON));
    return icon;                        // none: the Control Panel shows the entry without a picture
}

BOOL WINAPI DllEntry(HANDLE instance, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_instance = (HINSTANCE)instance;
        DisableThreadLibraryCalls((HMODULE)instance);
    }
    return TRUE;
}

static BOOL Ask(DWORD code, CLASSPAD_FUGUE_INFO *info)
{
    DWORD returned;
    BOOL ok;
    HANDLE disk = CreateFile(DISK_NAME, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);

    memset(info, 0, sizeof *info);
    if (disk == INVALID_HANDLE_VALUE) return FALSE;
    ok = DeviceIoControl(disk, code, NULL, 0, info, sizeof *info, &returned, NULL);
    CloseHandle(disk);
    return ok;
}

// "24.2" for 24816 KiB: MiB with one decimal (wsprintf has no %f)
static void MiB(DWORD kb, LPTSTR out)
{
    DWORD tenths = (kb * 10 + 512) / 1024;
    wsprintf(out, TEXT("%u.%u"), tenths / 10, tenths % 10);
}

// "Free Space: 3.2 MiB out of 24.2 MiB" into line[]: the FAT file system's
// free space, as the folder's Properties would say. Not the translation
// layer's count of clusters without a record: Fugue never learns that a file
// was deleted, so on a volume that has been used every cluster has a record
// and that figure sits near zero whatever the files take. Empty when the
// folder is not there (the volume handed to a computer over USB).
static void FreeSpaceLine(LPTSTR line)
{
    TCHAR path[80], folder[64], a[16], b[16];
    ULARGE_INTEGER avail, total, free;
    DWORD type, size = sizeof folder;
    HKEY key;

    line[0] = 0;
    _tcscpy(folder, TEXT("Storage"));
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, PROFILE_KEY, 0, 0, &key) == ERROR_SUCCESS) {
        if (RegQueryValueEx(key, TEXT("Folder"), NULL, &type, (LPBYTE)folder, &size) != ERROR_SUCCESS || type != REG_SZ) _tcscpy(folder, TEXT("Storage"));
        folder[sizeof folder / sizeof folder[0] - 1] = 0;
        RegCloseKey(key);
    }
    wsprintf(path, TEXT("\\%s"), folder);
    if (!GetDiskFreeSpaceEx(path, &avail, &total, &free)) return;
    MiB((DWORD)(free.QuadPart >> 10), a);
    MiB((DWORD)(total.QuadPart >> 10), b);
    wsprintf(line, TEXT("Free Space: %s MiB out of %s MiB\r\n"), a, b);
}

// The volume as the driver reports it into the dialog; the button only when there is something to reclaim
static void Show(HWND dialog)
{
    CLASSPAD_FUGUE_INFO info;
    TCHAR text[400], space[80];
    BOOL can = FALSE;

    if (!Ask(IOCTL_CLASSPAD_FUGUE_STATUS, &info)) {
        _tcscpy(text, TEXT("The Fugue file system is not available."));
    } else {
        FreeSpaceLine(space);
        if (!info.writable) {
            wsprintf(text, TEXT("The Fugue file system is read-only.\r\n\r\n%s"), space);
        } else {
            wsprintf(text, TEXT("Free Flash Sectors: %u out of %u\r\n%sReclaimable Sectors: %u"),
                     info.erased, info.sectors, space, info.reclaimable);
            can = info.reclaimable != 0;
        }
    }
    SetDlgItemText(dialog, IDC_STORAGE_STATUS, text);
    EnableWindow(GetDlgItem(dialog, IDC_STORAGE_OPTIMIZE), can);
}

// One sector per call to the driver, the progress bar moved in between
static void Optimize(HWND dialog)
{
    CLASSPAD_FUGUE_INFO info;
    HWND bar = GetDlgItem(dialog, IDC_STORAGE_PROGRESS);
    TCHAR text[200];
    DWORD total, done = 0, error = 0;

    if (!Ask(IOCTL_CLASSPAD_FUGUE_STATUS, &info) || !info.writable || !info.reclaimable) { Show(dialog); return; }
    total = info.reclaimable;
    EnableWindow(GetDlgItem(dialog, IDC_STORAGE_OPTIMIZE), FALSE);
    EnableWindow(GetDlgItem(dialog, IDOK), FALSE);
    SendMessage(bar, PBM_SETRANGE, 0, MAKELPARAM(0, total));
    SendMessage(bar, PBM_SETPOS, 0, 0);
    ShowWindow(bar, SW_SHOW);
    // no wait cursor: on this screen it sits in the middle of the dialog, over
    // the text or the bar, and the bar already says that something is going on
    for (;;) {
        wsprintf(text, TEXT("Optimizing... Sector %u out of %u"), done + 1 > total ? total : done + 1, total);
        SetDlgItemText(dialog, IDC_STORAGE_STATUS, text);
        UpdateWindow(dialog);
        if (!Ask(IOCTL_CLASSPAD_FUGUE_OPTIMIZE_STEP, &info)) { error = GetLastError() ? GetLastError() : 1; break; }
        if (info.status) { error = info.status; break; }
        if (!info.reclaimed) break;
        done++;
        if (done > total) {                             // the head sector filled meanwhile and became one more
            total = done + info.reclaimable;
            SendMessage(bar, PBM_SETRANGE, 0, MAKELPARAM(0, total));
        }
        SendMessage(bar, PBM_SETPOS, done, 0);
        UpdateWindow(bar);
    }
    ShowWindow(bar, SW_HIDE);
    {   // what was tapped or typed while the device did not respond is dropped:
        // it would otherwise arrive now and, on Close, shut the dialog
        MSG msg;
        while (PeekMessage(&msg, NULL, WM_MOUSEFIRST, WM_MOUSELAST, PM_REMOVE)) ;
        while (PeekMessage(&msg, NULL, WM_KEYFIRST, WM_KEYLAST, PM_REMOVE)) ;
    }
    EnableWindow(GetDlgItem(dialog, IDOK), TRUE);
    SetFocus(GetDlgItem(dialog, IDOK));
    if (error) {
        MessageBox(dialog, TEXT("Cannot optimize the file system."), TITLE, MB_OK | MB_ICONEXCLAMATION);
    }
    Show(dialog);
}

static BOOL CALLBACK StorageProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_INITDIALOG:
        Show(dialog);
        SetFocus(GetDlgItem(dialog, IDOK));
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_STORAGE_OPTIMIZE:
            Optimize(dialog);
            return TRUE;
        case IDOK:
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
    LPNEWCPLINFO info;
    LPCPLINFO old;

    switch (message) {
    case CPL_INIT:
        return TRUE;
    case CPL_GETCOUNT:
        return 1;
    case CPL_INQUIRE:
        old = (LPCPLINFO)param2;
        old->idIcon = 0;                // the icon is not a resource of this module (CPL_NEWINQUIRE has it)
        old->idName = IDS_STORAGE_NAME;
        old->idInfo = IDS_STORAGE_INFO;
        old->lData = 0;
        return 0;
    case CPL_NEWINQUIRE:
        info = (LPNEWCPLINFO)param2;
        memset(info, 0, sizeof *info);
        info->dwSize = sizeof *info;
        info->hIcon = StorageIcon();
        LoadString(g_instance, IDS_STORAGE_NAME, info->szName, sizeof info->szName / sizeof info->szName[0]);
        LoadString(g_instance, IDS_STORAGE_INFO, info->szInfo, sizeof info->szInfo / sizeof info->szInfo[0]);
        return 0;
    case CPL_DBLCLK: {
        INITCOMMONCONTROLSEX icc;                       // the dialog has a progress bar
        icc.dwSize = sizeof icc;
        icc.dwICC = ICC_PROGRESS_CLASS;
        InitCommonControlsEx(&icc);
        DialogBox(g_instance, MAKEINTRESOURCE(IDD_STORAGE), cpl, StorageProc);
    }
        return 0;
    case CPL_STOP:
    case CPL_EXIT:
        return 0;
    }
    return 0;
}
