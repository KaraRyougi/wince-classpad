#include <windows.h>
#include <stdarg.h>
#include <stdlib.h>
static void Log(LPCTSTR format, ...) {
    TCHAR text[512]; va_list ap; va_start(ap, format);
    wvsprintf(text, format, ap); va_end(ap); OutputDebugString(text);
}
int WINAPI WinMain(HINSTANCE,HINSTANCE,LPTSTR command,int) {
    if(command && *command) SignalStarted((DWORD)_wtol(command));
    MEMORYSTATUS ms; ZeroMemory(&ms,sizeof ms); ms.dwLength=sizeof ms; GlobalMemoryStatus(&ms);
    Log(TEXT("OFFICETEST RAM total=%u available=%u\r\n"),ms.dwTotalPhys,ms.dwAvailPhys);
    const TCHAR *dlls[]={TEXT("coredll.dll"),TEXT("commctrl.dll"),TEXT("commdlg.dll"),TEXT("ceshell.dll"),TEXT("ole32.dll"),TEXT("imgdecmp.dll"),TEXT("office.dll"),TEXT("ppv_res.dll")};
    for(unsigned i=0;i<sizeof(dlls)/sizeof(dlls[0]);++i) {
        HMODULE m=LoadLibrary(dlls[i]); DWORD error=m?0:GetLastError();
        Log(TEXT("OFFICETEST DLL %s handle=%08x error=%u\r\n"),dlls[i],m,error);
        if(m) FreeLibrary(m);
    }
    PROCESS_INFORMATION pi; ZeroMemory(&pi,sizeof pi);
    BOOL ok=CreateProcess(TEXT("\\Windows\\ppviewer.exe"),TEXT("\\Windows\\overview.ppv"),NULL,NULL,FALSE,0,NULL,NULL,NULL,&pi);
    DWORD error=ok?0:GetLastError();
    Log(TEXT("OFFICETEST CREATE ok=%u error=%u pid=%08x\r\n"),ok,error,pi.dwProcessId);
    if(ok) {
        CloseHandle(pi.hThread); Sleep(15000);
        DWORD code=0; GetExitCodeProcess(pi.hProcess,&code);
        Log(TEXT("OFFICETEST after15s exit=%u (259=still running)\r\n"),code);
        CloseHandle(pi.hProcess);
    }
    Log(TEXT("OFFICETEST COMPLETE\r\n")); return ok?0:1;
}
