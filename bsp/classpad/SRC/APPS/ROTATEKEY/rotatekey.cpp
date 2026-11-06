// Runs outside GWES: never rotate synchronously inside the touch DDSI.
#include <windows.h>
#include <stdlib.h>

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPTSTR command, int)
{
    HANDLE singleton = CreateMutex(NULL, FALSE, TEXT("ClassPadRotateKeyService"));
    if (!singleton) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(singleton); return 0; }
    HANDLE event = CreateEvent(NULL, FALSE, FALSE, TEXT("ClassPadRotateKey"));
    if (!event) { CloseHandle(singleton); return 1; }
    if (command && *command) SignalStarted((DWORD)_wtol(command));
    for (;;) {
        if (WaitForSingleObject(event, INFINITE) != WAIT_OBJECT_0) break;
        DEVMODE dm; ZeroMemory(&dm, sizeof dm); dm.dmSize = sizeof dm;
        dm.dmFields = DM_DISPLAYORIENTATION;
        LONG rc = ChangeDisplaySettingsEx(NULL, &dm, NULL, CDS_TEST, NULL);
        if (rc != DISP_CHANGE_SUCCESSFUL) {
            RETAILMSG(1, (TEXT("ROTATEKEY: query failed %d\r\n"), rc));
            continue;
        }
        dm.dmDisplayOrientation = (dm.dmDisplayOrientation == DMDO_90 || dm.dmDisplayOrientation == DMDO_270) ? DMDO_0 : DMDO_90;
        rc = ChangeDisplaySettingsEx(NULL, &dm, NULL, 0, NULL);
        RETAILMSG(1, (TEXT("ROTATEKEY: mode=%d result=%d size=%dx%d\r\n"), dm.dmDisplayOrientation, rc,
            GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)));
    }
    CloseHandle(event); CloseHandle(singleton); return 1;
}
