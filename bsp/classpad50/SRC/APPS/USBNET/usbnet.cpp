//------------------------------------------------------------------------------
//  usbnet.cpp - classpad_usbnet.exe: "USB Network", the network over the USB cable
//
//  One step from "cable in" to a PPP connection with the computer at the
//  other end (which runs pppd on the CDC ACM port, tools/usbppp.sh):
//    1. the USB function becomes the serial port (CDC_ACM) if it is not
//       (the USB function driver's IOCTL_UFN_CHANGE_CURRENT_CLIENT; the
//       default for the next boot is left alone),
//    2. the serial port shows up as a direct-connection RAS device (Unimodem
//       gives it the function's FriendlyName); this waits for it,
//    3. a RAS entry "USB" is written for that device: PPP, TCP/IP, address
//       and name servers from the other end, no user name or password,
//    4. rnaapp.exe dials it (-p: no prompt, -m: minimise when connected).
//       The connection then has its icon in the taskbar, where it is also
//       ended,
//    5. the Telnet server is loaded for as long as rnaapp.exe runs, which is
//       as long as the connection is there: this program waits for it to
//       end and unloads the server again. At boot the server is not loaded
//       (platform.reg: IsEnabled 0), so nothing listens without the link.
//
//  CE's direct connection opens with the text "CLIENT" and waits for
//  "CLIENTSERVER" before PPP starts: the script on the computer answers it.
//------------------------------------------------------------------------------
#include <windows.h>
#include <windev.h>
#include <ras.h>
#include <raserror.h>
#include <usbfnioctl.h>
#include <service.h>
#include "classpad_ufn.h"

#define CLIENT_SERIAL   TEXT("CDC_ACM")
#define SERIAL_KEY      TEXT("Drivers\\USB\\FunctionDrivers\\CDC_ACM")
#define ENTRY_NAME      TEXT("USB")
#define TITLE           TEXT("USB Network")
#define MAX_DEVICES     16
#define DEVICE_WAIT_MS  8000
#define TELNET_KEY      TEXT("COMM\\TELNETD")
#define TELNET_SERVICE  TEXT("TELNETD")
#define TELNET_DEVICE   TEXT("TEL0:")
#define OWNER_MUTEX     TEXT("ClasspadUsbNetTelnet")

static void Fail(LPCTSTR what)
{
    MessageBox(NULL, what, TITLE, MB_OK | MB_ICONEXCLAMATION | MB_TOPMOST);
}

// The serial function, if the driver runs another one
static BOOL SelectSerial(void)
{
    UFN_CLIENT_INFO current;
    UFN_CLIENT_NAME wanted;
    DWORD returned;
    BOOL ok = TRUE;
    HANDLE ufn = ClasspadOpenUfnBus();

    if (ufn == INVALID_HANDLE_VALUE) return FALSE;
    memset(&current, 0, sizeof current);
    ok = DeviceIoControl(ufn, IOCTL_UFN_GET_CURRENT_CLIENT, NULL, 0, &current, sizeof current, &returned, NULL);
    if (ok && _tcsicmp(current.szName, CLIENT_SERIAL)) {
        memset(&wanted, 0, sizeof wanted);
        _tcscpy(wanted.szName, CLIENT_SERIAL);
        ok = DeviceIoControl(ufn, IOCTL_UFN_CHANGE_CURRENT_CLIENT, &wanted, sizeof wanted, NULL, 0, &returned, NULL);
    }
    CloseHandle(ufn);
    return ok;
}

// The direct-connection device that is the USB serial port, into name[].
// Unimodem names a device by the FriendlyName of its driver's registry key,
// so that is what is looked for: the name the CDC ACM function's key has
// now, whatever it says. With no such device but exactly one direct
// connection in the list, that one is it (the image has no other serial
// port).
static BOOL FindDevice(LPTSTR name)
{
    static RASDEVINFO devices[MAX_DEVICES];
    TCHAR friendly[RAS_MaxDeviceName + 1];
    DWORD size, count, i, type, direct = 0, only = 0;
    HKEY key;

    friendly[0] = 0;
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, SERIAL_KEY, 0, 0, &key) == ERROR_SUCCESS) {
        size = sizeof friendly - sizeof(TCHAR);
        if (RegQueryValueEx(key, TEXT("FriendlyName"), NULL, &type, (LPBYTE)friendly, &size) != ERROR_SUCCESS || type != REG_SZ) friendly[0] = 0;
        friendly[RAS_MaxDeviceName] = 0;
        RegCloseKey(key);
    }

    size = sizeof devices;
    count = 0;
    devices[0].dwSize = sizeof(RASDEVINFO);
    if (RasEnumDevices(devices, &size, &count)) return FALSE;
    for (i = 0; i < count && i < MAX_DEVICES; i++) {
        if (_tcsicmp(devices[i].szDeviceType, RASDT_Direct)) continue;
        if (friendly[0] && _tcsstr(devices[i].szDeviceName, friendly)) {
            _tcscpy(name, devices[i].szDeviceName);
            return TRUE;
        }
        direct++;
        only = i;
    }
    if (direct != 1) return FALSE;
    _tcscpy(name, devices[only].szDeviceName);
    return TRUE;
}

// The Telnet server's "IsEnabled": services.exe asks the server whether it
// wants its port when it loads it, and the server answers with this value
// (and looks at it again for every connection).
static void EnableTelnet(DWORD enabled)
{
    HKEY key;

    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, TELNET_KEY, 0, 0, &key) != ERROR_SUCCESS) return;
    RegSetValueEx(key, TEXT("IsEnabled"), 0, REG_DWORD, (LPBYTE)&enabled, sizeof enabled);
    RegCloseKey(key);
}

// Loads the Telnet server into services.exe, which then listens on its port.
// An image without the server just has no Telnet: nothing is said.
static void StartTelnet(void)
{
    if (!IsAPIReady(SH_SERVICES)) return;
    EnableTelnet(1);
    if (GetServiceHandle(TELNET_DEVICE, NULL, NULL) == INVALID_HANDLE_VALUE) ActivateService(TELNET_SERVICE, 0);
}

// Unloads it: the port is closed and the sessions that are open end.
static void StopTelnet(void)
{
    HANDLE service;

    if (!IsAPIReady(SH_SERVICES)) return;
    service = GetServiceHandle(TELNET_DEVICE, NULL, NULL);
    if (service != INVALID_HANDLE_VALUE) DeregisterService(service);
    EnableTelnet(0);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPTSTR commandLine, int show)
{
    static RASENTRY entry;
    static RASDIALPARAMS params;
    TCHAR device[RAS_MaxDeviceName + 1], args[64];
    PROCESS_INFORMATION pi;
    DWORD start;
    HANDLE mutex;
    BOOL owner;

    // The network components live in the EXT region: without it the RAS
    // calls below have nobody to answer them (and end the program with an
    // exception), so this is asked first, before USB is switched over.
    if (!IsAPIReady(SH_COMM)) { Fail(TEXT("Networking is not available.")); return 1; }
    if (!SelectSerial()) { Fail(TEXT("Cannot switch USB to serial port mode.")); return 1; }

    start = GetTickCount();
    while (!FindDevice(device)) {
        if (GetTickCount() - start > DEVICE_WAIT_MS) { Fail(TEXT("The USB serial port is not available.")); return 1; }
        Sleep(250);
    }

    memset(&entry, 0, sizeof entry);
    entry.dwSize = sizeof entry;
    entry.dwfOptions = RASEO_IpHeaderCompression | RASEO_SwCompression;
    entry.dwfNetProtocols = RASNP_Ip;
    entry.dwFramingProtocol = RASFP_Ppp;
    _tcscpy(entry.szDeviceType, RASDT_Direct);
    _tcscpy(entry.szDeviceName, device);
    if (RasSetEntryProperties(NULL, ENTRY_NAME, &entry, sizeof entry, NULL, 0)) { Fail(TEXT("Cannot create the connection.")); return 1; }

    memset(&params, 0, sizeof params);
    params.dwSize = sizeof params;
    _tcscpy(params.szEntryName, ENTRY_NAME);
    RasSetEntryDialParams(NULL, &params, FALSE);        // no user name, no password

    // Only the first instance looks after the Telnet server: one started
    // while the connection is there must not take the server away when its
    // own rnaapp.exe leaves at once.
    mutex = CreateMutex(NULL, FALSE, OWNER_MUTEX);
    owner = mutex && GetLastError() != ERROR_ALREADY_EXISTS;
    if (owner) StartTelnet();

    wsprintf(args, TEXT("-p -m -e%s"), ENTRY_NAME);
    if (!CreateProcess(TEXT("rnaapp.exe"), args, NULL, NULL, FALSE, 0, NULL, NULL, NULL, &pi)) {
        if (owner) StopTelnet();
        Fail(TEXT("Cannot start the connection."));
        return 1;
    }
    CloseHandle(pi.hThread);
    if (owner) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        StopTelnet();
    }
    CloseHandle(pi.hProcess);
    return 0;
}
