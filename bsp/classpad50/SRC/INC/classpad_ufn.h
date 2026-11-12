//------------------------------------------------------------------------------
//  classpad_ufn.h - a handle for the USB function driver's client ioctls
//
//  IOCTL_UFN_GET_CURRENT_CLIENT, IOCTL_UFN_CHANGE_CURRENT_CLIENT and the
//  others of usbfnioctl.h are bus ioctls: the MDD answers them only on a
//  handle opened in the bus name space (UFN_Open looks for
//  DEVACCESS_BUSNAMESPACE; on a plain "UFN1:" handle they go to the PDD and
//  fail). The bus name is not fixed, so the device is looked up by its legacy
//  name and opened through the "$bus\..." name the device manager reports.
//------------------------------------------------------------------------------
#ifndef CLASSPAD_UFN_H
#define CLASSPAD_UFN_H

#ifndef DEVACCESS_BUSNAMESPACE
#define DEVACCESS_BUSNAMESPACE  FILE_WRITE_ATTRIBUTES   // devload.h (DDK)
#endif

// INVALID_HANDLE_VALUE when the driver is not loaded
static HANDLE ClasspadOpenUfnBus(void)
{
    DEVMGR_DEVICE_INFORMATION di;
    HANDLE find, bus;

    memset(&di, 0, sizeof di);
    di.dwSize = sizeof di;
    find = FindFirstDevice(DeviceSearchByLegacyName, TEXT("UFN*"), &di);
    if (find == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    FindClose(find);
    bus = CreateFile(di.szBusName, GENERIC_READ | GENERIC_WRITE | DEVACCESS_BUSNAMESPACE,
                     FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    return bus;
}

#endif
