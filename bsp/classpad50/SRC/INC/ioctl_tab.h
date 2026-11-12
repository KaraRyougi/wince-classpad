//------------------------------------------------------------------------------
//  ioctl_tab.h - OAL IOCTL table (included by ioctl.c), Windows CE 5.0 set.
//  The handlers are in OALLIB\oalcommon.c unless a BSP file defines them.
//------------------------------------------------------------------------------
// IOCTL CODE,                          Flags       Handler Function
//------------------------------------------------------------------------------
{ IOCTL_HAL_TRANSLATE_IRQ,                  0,  OALIoCtlHalRequestSysIntr   },
{ IOCTL_HAL_REQUEST_SYSINTR,                0,  OALIoCtlHalRequestSysIntr   },
{ IOCTL_HAL_RELEASE_SYSINTR,                0,  OALIoCtlHalReleaseSysIntr   },
{ IOCTL_HAL_REQUEST_IRQ,                    0,  OALIoCtlHalRequestIrq       },
{ IOCTL_HAL_INITREGISTRY,                   0,  OALIoCtlHalInitRegistry     },
{ IOCTL_HAL_INIT_RTC,                       0,  OALIoCtlHalInitRTC          },
{ IOCTL_HAL_POSTINIT,                       0,  OALIoCtlHalPostInit         },
{ IOCTL_HAL_REBOOT,                         0,  OALIoCtlHalReboot           },
{ IOCTL_HAL_GET_CACHE_INFO,                 0,  OALIoCtlHalGetCacheInfo     },
{ IOCTL_HAL_GET_DEVICEID,                   0,  OALIoCtlHalGetDeviceId      },
{ IOCTL_HAL_GET_DEVICE_INFO,                0,  OALIoCtlHalGetDeviceInfo    },
{ IOCTL_HAL_GET_UUID,                       0,  OALIoCtlHalGetUUID          },
{ IOCTL_PROCESSOR_INFORMATION,              0,  OALIoCtlProcessorInfo       },
{ IOCTL_HAL_PRESUSPEND,                     0,  OALIoCtlHalPresuspend       },
{ IOCTL_HAL_CLASSPAD_SUSPEND_STATS,         0,  OALIoCtlHalClasspadSuspendStats },
{ IOCTL_HAL_CLASSPAD_PANEL,                 0,  OALIoCtlHalClasspadPanel    },
{ IOCTL_HAL_CLASSPAD_SUSPEND_OPTIONS,       0,  OALIoCtlHalClasspadSuspendOptions },
{ IOCTL_HAL_CLASSPAD_CLOCK_GET,             0,  OALIoCtlHalClasspadClockGet },
{ IOCTL_HAL_CLASSPAD_CLOCK_SET,             0,  OALIoCtlHalClasspadClockSet },
{ IOCTL_HAL_ENABLE_WAKE,                    0,  OALIoCtlHalEnableWake       },
{ IOCTL_HAL_DISABLE_WAKE,                   0,  OALIoCtlHalDisableWake      },
{ IOCTL_HAL_GET_WAKE_SOURCE,                0,  OALIoCtlHalGetWakeSource    },
// Required Termination
{ 0,                                        0,  NULL                        }
