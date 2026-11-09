// Optional validation image only: two PM suspend/resume cycles after boot.
#include <windows.h>
#include <pm.h>
#include <dbgapi.h>
#include <stdlib.h>
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPWSTR command, int)
{
    DWORD result;
    if (command && *command) SignalStarted((DWORD)_wtol(command));
    Sleep(60000);
    OutputDebugStringW(L"CLASSPAD POWERTEST: request 1\r\n");
    result = SetSystemPowerState(NULL, POWER_STATE_SUSPEND, POWER_FORCE);
    NKDbgPrintfW(L"CLASSPAD POWERTEST: returned 1 result=%u\r\n", result);
    Sleep(10000);
    OutputDebugStringW(L"CLASSPAD POWERTEST: request 2\r\n");
    result = SetSystemPowerState(NULL, POWER_STATE_SUSPEND, POWER_FORCE);
    NKDbgPrintfW(L"CLASSPAD POWERTEST: returned 2 result=%u\r\n", result);
    MessageBoxW(NULL, result ? L"Suspend failed: see debug log" : L"Two suspend/resume calls returned. Check log for Clear/ON wake.", L"Power test", MB_OK);
    return 0;
}
