//------------------------------------------------------------------------------
//  fuguedsk.c - Casio's storage memory (\fls0) as a block device, read-only
//
//  The 24 MiB "storage memory" of Casio's OS is a FAT16 volume on Fugue, a
//  log-structured translation layer in the NOR (chip 0x1A20000-0x385FFFF).
//  classpad_fugue.h builds the map from logical clusters to flash pages once,
//  at load; this driver serves it as a disk of 512-byte sectors with an MBR
//  at sector 0, which is what MSPART and FATFS expect, so the files Casio's
//  OS (or a PC, through Casio's USB storage) put there appear in CE under
//  the profile's folder (platform.reg, [..\Profiles\ClassPadStorage]).
//
//  Nothing is written, in this version: the map is fixed while CE runs
//  (Casio's OS is not running), DSK_Open refuses write access so the Storage
//  Manager marks the store read-only, the device reports
//  STORAGE_DEVICE_FLAG_READONLY so FATFS mounts the volume read-only, and a
//  write request fails with ERROR_WRITE_PROTECT. Writing means appending
//  Fugue records and programming the NOR, which CE executes from: a later
//  step, behind the same DoRead/DoWrite pair.
//
//  The flash is read through its P1 address like the image itself (every
//  thread runs in kernel mode, config.bib ROMFLAGS); the reader uses byte
//  accesses only, so the translator's byte swaps do not come into it.
//  No volume (an erased or foreign area, no MBR): the driver does not load
//  and CE runs without the folder.
//------------------------------------------------------------------------------
#include <windows.h>
#include <types.h>
#include <devload.h>
#include <diskio.h>
#include <storemgr.h>
#include "classpad_fugue.h"

#define FUGUE_CLUSTERS      CLASSPAD_FUGUE_MAX_CLUSTERS(CLASSPAD_FUGUE_SECTORS)
#define FUGUE_PROFILE       TEXT("ClassPadStorage")

typedef struct {
    CLASSPAD_FUGUE f;
    unsigned short loc[FUGUE_CLUSTERS];
    TCHAR profile[PROFILENAMESIZE];
} FUGUE_DISK;

BOOL WINAPI DllMain(HANDLE hinst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls((HMODULE)hinst);
    return TRUE;
}

// The profile name: the driver key's "Profile", else FUGUE_PROFILE.
static void ReadProfile(LPCTSTR activeKey, FUGUE_DISK *d)
{
    HKEY key;
    DWORD type, size = sizeof d->profile;

    wcscpy(d->profile, FUGUE_PROFILE);
    key = OpenDeviceKey(activeKey);
    if (key == INVALID_HANDLE_VALUE || !key) return;
    if (RegQueryValueEx(key, TEXT("Profile"), NULL, &type, (LPBYTE)d->profile, &size) != ERROR_SUCCESS || type != REG_SZ) {
        wcscpy(d->profile, FUGUE_PROFILE);
    }
    d->profile[PROFILENAMESIZE - 1] = 0;
    RegCloseKey(key);
}

DWORD DSK_Init(DWORD context)
{
    FUGUE_DISK *d;
    unsigned *ver;
    unsigned blocks;

    d = (FUGUE_DISK *)LocalAlloc(LPTR, sizeof *d);
    ver = (unsigned *)LocalAlloc(LMEM_FIXED, FUGUE_CLUSTERS * sizeof *ver);    // versions: only while scanning
    if (!d || !ver) {
        RETAILMSG(1, (TEXT("CLASSPAD STORAGE: out of memory\r\n")));
        if (d) LocalFree(d);
        if (ver) LocalFree(ver);
        return 0;
    }
    blocks = ClasspadFugueScan(&d->f, (const void *)CLASSPAD_FUGUE_PA, CLASSPAD_FUGUE_SECTORS, d->loc, ver, FUGUE_CLUSTERS);
    LocalFree(ver);
    if (!blocks) {
        RETAILMSG(1, (TEXT("CLASSPAD STORAGE: no volume in Casio's storage memory (%u records, %u rejected), not mounted\r\n"),
                      d->f.records, d->f.rejected));
        LocalFree(d);
        return 0;
    }
    ReadProfile((LPCTSTR)context, d);
    RETAILMSG(1, (TEXT("CLASSPAD STORAGE: %u sectors, %u clusters in %u records (%u rejected), read-only, profile %s\r\n"),
                  blocks, d->f.live, d->f.records, d->f.rejected, d->profile));
    return (DWORD)d;
}

BOOL DSK_Deinit(DWORD context)
{
    if (context) LocalFree((HLOCAL)context);
    return TRUE;
}

// Write access is refused: the Storage Manager then opens the device for
// reading and marks the store read-only (CStore::OpenDisk).
DWORD DSK_Open(DWORD context, DWORD access, DWORD share)
{
    if (access & GENERIC_WRITE) {
        SetLastError(ERROR_ACCESS_DENIED);
        return 0;
    }
    return context;
}

BOOL DSK_Close(DWORD open) { return TRUE; }
DWORD DSK_Read(DWORD open, LPVOID buf, DWORD len) { return (DWORD)-1; }
DWORD DSK_Write(DWORD open, LPCVOID buf, DWORD len) { return (DWORD)-1; }
DWORD DSK_Seek(DWORD open, long pos, DWORD type) { return (DWORD)-1; }
void DSK_PowerDown(DWORD context) { }
void DSK_PowerUp(DWORD context) { }

// One scatter/gather read: sr_num_sec sectors from sr_start, laid end to end
// into the request's buffers. Returns the request's status.
static DWORD DoRead(FUGUE_DISK *d, SG_REQ *req, DWORD reqBytes)
{
    const unsigned char *p;
    unsigned char *out;
    DWORD i, pos, left, room, n, in;

    if (!req->sr_num_sg || req->sr_num_sg > MAX_SG_BUF ||
        reqBytes < sizeof(SG_REQ) + sizeof(SG_BUF) * (req->sr_num_sg - 1)) return ERROR_INVALID_PARAMETER;
    if (req->sr_start >= d->f.blocks || req->sr_num_sec > d->f.blocks - req->sr_start) return ERROR_SECTOR_NOT_FOUND;

    pos = req->sr_start * CLASSPAD_FUGUE_BLOCK_BYTES;           // byte position in the device (24 MiB: fits)
    left = req->sr_num_sec * CLASSPAD_FUGUE_BLOCK_BYTES;
    for (i = 0; i < req->sr_num_sg && left; i++) {
        room = req->sr_sglist[i].sb_len;
        out = (unsigned char *)MapCallerPtr(req->sr_sglist[i].sb_buf, room);
        if (!out) return ERROR_INVALID_PARAMETER;
        if (room > left) room = left;
        left -= room;
        while (room) {                                          // a run inside one logical cluster
            in = pos % CLASSPAD_FUGUE_PAGE_BYTES;
            n = CLASSPAD_FUGUE_PAGE_BYTES - in;
            if (n > room) n = room;
            p = ClasspadFugueCluster(&d->f, pos / CLASSPAD_FUGUE_PAGE_BYTES);
            if (p) memcpy(out, p + in, n);
            else memset(out, 0xFF, n);                          // never written: erased flash, as Casio's OS reads it
            out += n; pos += n; room -= n;
        }
    }
    return left ? ERROR_INVALID_PARAMETER : ERROR_SUCCESS;       // buffers shorter than the sectors asked for
}

static DWORD DoWrite(FUGUE_DISK *d, SG_REQ *req, DWORD reqBytes)
{
    return ERROR_WRITE_PROTECT;
}

static void GetInfo(FUGUE_DISK *d, DISK_INFO *info)
{
    info->di_total_sectors = d->f.blocks;
    info->di_bytes_per_sect = CLASSPAD_FUGUE_BLOCK_BYTES;
    info->di_cylinders = 0;
    info->di_heads = 0;
    info->di_sectors = 0;
    info->di_flags = DISK_INFO_FLAG_MBR | DISK_INFO_FLAG_CHS_UNCERTAIN;
}

BOOL DSK_IOControl(DWORD open, DWORD code, PBYTE in, DWORD inSize, PBYTE out, DWORD outSize, PDWORD returned)
{
    FUGUE_DISK *d = (FUGUE_DISK *)open;
    STORAGEDEVICEINFO *sdi;
    SG_REQ *req;
    DWORD status = ERROR_SUCCESS;

    if (PSLGetCallerTrust() != OEM_CERTIFY_TRUST) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    if (!d) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }

    __try {
        switch (code) {
        case DISK_IOCTL_READ:
        case IOCTL_DISK_READ:
        case DISK_IOCTL_WRITE:
        case IOCTL_DISK_WRITE:
            req = (SG_REQ *)in;
            if (!req || inSize < sizeof(SG_REQ)) { status = ERROR_INVALID_PARAMETER; break; }
            status = (code == DISK_IOCTL_READ || code == IOCTL_DISK_READ) ? DoRead(d, req, inSize) : DoWrite(d, req, inSize);
            req->sr_status = status;
            break;

        case DISK_IOCTL_GETINFO:                                // the old code: DISK_INFO in the in buffer
            if (!in || inSize != sizeof(DISK_INFO)) { status = ERROR_INVALID_PARAMETER; break; }
            GetInfo(d, (DISK_INFO *)in);
            if (returned) *returned = sizeof(DISK_INFO);
            break;

        case IOCTL_DISK_GETINFO:
            if (!out || outSize != sizeof(DISK_INFO)) { status = ERROR_INVALID_PARAMETER; break; }
            GetInfo(d, (DISK_INFO *)out);
            if (returned) *returned = sizeof(DISK_INFO);
            break;

        case IOCTL_DISK_DEVICE_INFO:                            // what makes FATFS mount read-only
            sdi = (STORAGEDEVICEINFO *)in;
            if (!sdi || inSize != sizeof(STORAGEDEVICEINFO)) { status = ERROR_INVALID_PARAMETER; break; }
            sdi->cbSize = sizeof(STORAGEDEVICEINFO);
            wcscpy(sdi->szProfile, d->profile);
            sdi->dwDeviceClass = STORAGE_DEVICE_CLASS_BLOCK;
            sdi->dwDeviceType = STORAGE_DEVICE_TYPE_FLASH;
            sdi->dwDeviceFlags = STORAGE_DEVICE_FLAG_READONLY;
            if (returned) *returned = sizeof(STORAGEDEVICEINFO);
            break;

        case DISK_IOCTL_INITIALIZED:
        case IOCTL_DISK_INITIALIZED:
            break;

        case DISK_IOCTL_SETINFO:
        case IOCTL_DISK_SETINFO:
        case DISK_IOCTL_FORMAT_MEDIA:
        case IOCTL_DISK_FORMAT_MEDIA:
        case IOCTL_DISK_DELETE_SECTORS:
            status = ERROR_WRITE_PROTECT;
            break;

        default:                                                // GETNAME (the profile's Folder answers), storage id, flush...
            status = ERROR_NOT_SUPPORTED;
            break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        status = ERROR_INVALID_PARAMETER;
    }

    if (status != ERROR_SUCCESS) { SetLastError(status); return FALSE; }
    return TRUE;
}
