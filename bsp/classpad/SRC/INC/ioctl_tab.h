//------------------------------------------------------------------------------
//  ioctl_tab.h - OAL IOCTL table (included by ioctl.c). Handlers come from
//  PLATFORM\COMMON oal_ioctl.lib / oal_intr.lib unless defined in this BSP.
//------------------------------------------------------------------------------
// IOCTL CODE,                          Flags       Handler Function
//------------------------------------------------------------------------------
{ IOCTL_HAL_TRANSLATE_IRQ,                  0,  OALIoCtlHalRequestSysIntr   },
{ IOCTL_HAL_REQUEST_SYSINTR,                0,  OALIoCtlHalRequestSysIntr   },
{ IOCTL_HAL_RELEASE_SYSINTR,                0,  OALIoCtlHalReleaseSysIntr   },
{ IOCTL_HAL_REQUEST_IRQ,                    0,  OALIoCtlHalRequestIrq       },
{ IOCTL_HAL_INITREGISTRY,                   0,  OALIoCtlHalInitRegistry     },
{ IOCTL_HAL_INIT_RTC,                       0,  OALIoCtlHalInitRTC          },
{ IOCTL_HAL_REBOOT,                         0,  OALIoCtlHalReboot           },
{ IOCTL_HAL_DDK_CALL,                       0,  OALIoCtlHalDdkCall          },
{ IOCTL_HAL_GET_CACHE_INFO,                 0,  OALIoCtlHalGetCacheInfo     },
{ IOCTL_HAL_GET_DEVICEID,                   0,  OALIoCtlHalGetDeviceId      },
{ IOCTL_HAL_GET_DEVICE_INFO,                0,  OALIoCtlHalGetDeviceInfo    },
{ IOCTL_HAL_GET_UUID,                       0,  OALIoCtlHalGetUUID          },
{ IOCTL_HAL_GET_RANDOM_SEED,                0,  OALIoCtlHalGetRandomSeed    },
{ IOCTL_PROCESSOR_INFORMATION,              0,  OALIoCtlProcessorInfo       },
{ IOCTL_HAL_GETREGSECUREKEYS,               0,  OALIoCtlHalGetRegSecureKeys },
{ IOCTL_HAL_UPDATE_MODE,                    0,  OALIoCtlHalUpdateMode       },
// Required Termination
{ 0,                                        0,  NULL                        }
