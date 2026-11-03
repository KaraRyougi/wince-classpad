//------------------------------------------------------------------------------
//  ioctl.c - OEMIoControl table and platform strings
//------------------------------------------------------------------------------
#include <bsp.h>

LPCWSTR g_oalIoCtlPlatformType = IOCTL_PLATFORM_TYPE;
LPCWSTR g_oalIoCtlPlatformOEM  = IOCTL_PLATFORM_OEM;

LPCWSTR g_oalIoCtlProcessorVendor = IOCTL_PROCESSOR_VENDOR;
LPCWSTR g_oalIoCtlProcessorName   = IOCTL_PROCESSOR_NAME;
LPCWSTR g_oalIoCtlProcessorCore   = IOCTL_PROCESSOR_CORE;

UINT32 g_oalIoCtlInstructionSet = IOCTL_PROCESSOR_INSTRUCTION_SET;
UINT32 g_oalIoCtlClockSpeed = IOCTL_PROCESSOR_CLOCK_SPEED;

//------------------------------------------------------------------------------
//  Reboot: under the translator, end the run; the exit code 0x7E marks it.
//
BOOL OALIoCtlHalReboot(
    UINT32 code, VOID *pInpBuffer, UINT32 inpSize, VOID *pOutBuffer,
    UINT32 outSize, UINT32 *pOutSize
) {
    if (ClasspadCanTrap()) ClasspadHcExit(0x7E);
    for (;;) __asm("sleep");
    return FALSE;
}

//------------------------------------------------------------------------------
const OAL_IOCTL_HANDLER g_oalIoCtlTable[] = {
#include "ioctl_tab.h"
};
