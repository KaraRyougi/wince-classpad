//------------------------------------------------------------------------------
//  classpad_fugueioctl.h - the storage driver's own ioctls (SRC\DRIVERS\FUGUEDSK)
//
//  On a handle to the disk ("DSK1:", opened for reading):
//    IOCTL_CLASSPAD_FUGUE_STATUS    out: CLASSPAD_FUGUE_INFO, the volume as it is
//    IOCTL_CLASSPAD_FUGUE_OPTIMIZE  out: CLASSPAD_FUGUE_INFO after the driver has
//                                   written its cache and reclaimed every sector
//                                   that holds superseded records
//                                   (classpad_fuguewr.h ClasspadFugueOptimize);
//                                   "reclaimed" says how many. Each reclaimed
//                                   sector is an erase of about half a second.
//    IOCTL_CLASSPAD_FUGUE_OPTIMIZE_STEP  the same, one sector per call, for a
//                                   caller that shows progress: "reclaimed" is
//                                   1, or 0 when nothing is left to reclaim.
//------------------------------------------------------------------------------
#ifndef CLASSPAD_FUGUEIOCTL_H
#define CLASSPAD_FUGUEIOCTL_H

#define IOCTL_CLASSPAD_FUGUE_STATUS     CTL_CODE(FILE_DEVICE_DISK, 0x8F0, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_CLASSPAD_FUGUE_OPTIMIZE   CTL_CODE(FILE_DEVICE_DISK, 0x8F1, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_CLASSPAD_FUGUE_OPTIMIZE_STEP CTL_CODE(FILE_DEVICE_DISK, 0x8F2, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct {
    DWORD writable;         // 0: read-only (nothing below the sector count means anything)
    DWORD sectors;          // physical sectors of the storage memory (242)
    DWORD erased;           // of them erased: room for new records
    DWORD reclaimable;      // sectors an optimise would reclaim now
    DWORD liveClusters;     // logical clusters (4 KiB) that hold data
    DWORD reclaimed;        // OPTIMIZE: sectors reclaimed by this call
    DWORD status;           // OPTIMIZE: 0, or the writer's error (classpad_fuguewr.h)
    DWORD capacityKB;       // size of the block device the file system lives on, in KiB
} CLASSPAD_FUGUE_INFO;

#endif
