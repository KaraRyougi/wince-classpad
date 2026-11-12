//------------------------------------------------------------------------------
//  fuguedsk.c - Casio's storage memory (\fls0) as a block device
//
//  The 24 MiB "storage memory" of Casio's OS is a FAT16 volume on Fugue, a
//  log-structured translation layer in the NOR (chip 0x1A20000-0x385FFFF).
//  classpad_fugue.h builds the map from logical clusters to flash pages once,
//  at load; this driver serves it as a disk of 512-byte sectors with an MBR
//  at sector 0, which is what MSPART and FATFS expect, so the files Casio's
//  OS (or a PC, through Casio's USB storage) put there appear in CE under
//  the profile's folder (platform.reg, [..\Profiles\ClassPadStorage]), and
//  the USB mass storage function (usbmsfn.dll, "DeviceName"="DSK1:") can
//  hand the same disk to a PC.
//
//  Reading: the flash through its P1 address like the image itself (every
//  thread runs in kernel mode, config.bib ROMFLAGS); the reader uses byte
//  accesses only, so the translator's byte swaps do not come into it.
//
//  Writing: classpad_fuguewr.h appends Fugue records the way Casio's OS
//  does. The chip cannot be programmed from here (CE executes from it), so
//  each program and erase is a hypercall to the translator (classpad_hc.h
//  ClasspadHcNor, dbt/rt/nor.c), which only accepts the storage memory. Its
//  source must be RAM at a P1 address: one page from AllocPhysMem, used
//  through its P1 alias only.
//
//  A 512-byte sector write would cost a whole 4 KiB record, and FATFS writes
//  the FAT and directories sector by sector, so writes go to a small cache of
//  clusters first (CACHE_CLUSTERS). A thread writes the dirty ones FLUSH_MS
//  after the last write; so do IOCTL_DISK_FLUSH_CACHE, closing the device and
//  a cluster being pushed out of the cache. Power lost inside that window
//  loses those sectors (the volume itself stays consistent: a record is
//  complete or absent). A cluster whose content did not change is not
//  written at all.
//
//  The driver stays read-only when: the translator has no programming
//  service (an older wince.hh3), the volume has no DETAIL record (not one
//  Casio's OS formatted), or the registry says so ("Writable"=dword:0 in the
//  driver's key). After a failed program or erase it turns read-only until
//  the next boot. Formatting from CE is refused: the volume is Casio's.
//  No volume (an erased or foreign area, no MBR): the driver does not load
//  and CE runs without the folder.
//------------------------------------------------------------------------------
#include <windows.h>
#include <types.h>
#include <devload.h>
#include <diskio.h>
#include <storemgr.h>
#include <pkfuncs.h>
#include "classpad_hc.h"
#include "classpad_fuguewr.h"
#include "classpad_fuguetrim.h"
#include "classpad_fugueioctl.h"

#define FUGUE_CLUSTERS      CLASSPAD_FUGUE_MAX_CLUSTERS(CLASSPAD_FUGUE_SECTORS)
#define FUGUE_PROFILE       TEXT("ClassPadStorage")
#define FUGUE_CHIP_OFF      (CLASSPAD_FUGUE_PA - 0x80000000u)
#define CACHE_CLUSTERS      8
#define FLUSH_MS            700
#define NO_CLUSTER          0xFFFFFFFF
#define BOUNCE_BYTES        4096        // one page

typedef struct {
    DWORD lc;                           // NO_CLUSTER = free
    DWORD stamp;                        // last use
    BOOL dirty;
    BYTE data[CLASSPAD_FUGUE_PAGE_BYTES];
} FUGUE_CACHE;

typedef struct {
    CLASSPAD_FUGUE f;
    CLASSPAD_FUGUE_WR w;
    unsigned short loc[FUGUE_CLUSTERS];
    TCHAR profile[PROFILENAMESIZE];
    CRITICAL_SECTION cs;                // the map, the cache, the flash
    BOOL writable;
    BYTE *bounce;                       // P1 alias of one physical page: what the translator programs from
    BYTE page[CLASSPAD_FUGUE_PAGE_BYTES];       // the writer's
    FUGUE_CACHE cache[CACHE_CLUSTERS];
    DWORD stamp;
    HANDLE dirtyEvent, flushThread;
    volatile BOOL stop;
    DWORD lastWrite;
    // Trim (classpad_fuguetrim.h): clusters the FAT has as free lose their records
    unsigned *ver;                      // the writer's version floors, one per logical cluster
    BYTE written[(FUGUE_CLUSTERS + 7) / 8];     // written since the driver started: never trimmed
    BOOL trimDue;                       // something was written since the last walk of the FAT
    CLASSPAD_FUGUE_TRIM trim;
} FUGUE_DISK;

BOOL WINAPI DllMain(HANDLE hinst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls((HMODULE)hinst);
    return TRUE;
}

//------------------------------------------------------------------------------
//  The flash, for the writer
//
static int NorProgram(void *ctx, unsigned off, const unsigned char *src, unsigned len)
{
    FUGUE_DISK *d = (FUGUE_DISK *)ctx;

    if (len > BOUNCE_BYTES) return -1;
    memcpy(d->bounce, src, len);
    return ClasspadHcNor(HC_NOR_PROGRAM, FUGUE_CHIP_OFF + off, (DWORD)d->bounce, len) != 0;
}

static int NorErase(void *ctx, unsigned sector)
{
    return ClasspadHcNor(HC_NOR_ERASE, FUGUE_CHIP_OFF + sector * CLASSPAD_FUGUE_SECTOR_BYTES, 0, 0) != 0;
}

//------------------------------------------------------------------------------
//  The cluster cache. All of it under d->cs.
//
static FUGUE_CACHE *CacheFind(FUGUE_DISK *d, DWORD lc)
{
    int i;
    for (i = 0; i < CACHE_CLUSTERS; i++) {
        if (d->cache[i].lc == lc) return &d->cache[i];
    }
    return NULL;
}

//------------------------------------------------------------------------------
//  Trim. Fugue keeps a record for every cluster ever written and copies it
//  along at every reclaim, deleted files included; the FAT knows which
//  clusters hold nothing. TrimNow walks the FAT (through the cache: what has
//  not reached the flash yet counts) and has the writer drop the records of
//  the clusters it has as free, so that a reclaim gives their room back.
//  Called when a reclaim is about to happen and for the Fugue File System
//  applet's figures; a walk costs nothing in the flash.
//
//  A cluster written since the driver started is left alone, whatever the
//  FAT on the disk says: the file system (FATFS here, the computer's in mass
//  storage mode) may write a file's data before the FAT entry that claims
//  its cluster. Such a cluster is trimmed after the next boot at the
//  earliest. All of it under d->cs.
//
static int TrimRead(void *ctx, unsigned lba, unsigned char *buf)
{
    FUGUE_DISK *d = (FUGUE_DISK *)ctx;
    FUGUE_CACHE *c = CacheFind(d, lba / CLASSPAD_FUGUE_BLOCKS_PER_PAGE);

    if (lba >= d->f.blocks) return -1;
    if (c) memcpy(buf, c->data + (lba % CLASSPAD_FUGUE_BLOCKS_PER_PAGE) * CLASSPAD_FUGUE_BLOCK_BYTES, CLASSPAD_FUGUE_BLOCK_BYTES);
    else ClasspadFugueRead(&d->f, lba, 1, buf);
    return 0;
}

static void TrimUnused(void *ctx, unsigned lc)
{
    FUGUE_DISK *d = (FUGUE_DISK *)ctx;

    if (lc >= FUGUE_CLUSTERS || (d->written[lc >> 3] & (1 << (lc & 7))) || CacheFind(d, lc)) return;
    ClasspadFugueTrim(&d->w, lc);
}

static void TrimNow(FUGUE_DISK *d)
{
    unsigned before;

    if (!d->writable || !d->w.ver || !d->trimDue) return;
    d->trimDue = FALSE;
    before = d->w.trimmed;
    d->trim.ctx = d;
    d->trim.read = TrimRead;
    d->trim.unused = TrimUnused;
    ClasspadFugueTrimScan(&d->trim, d->f.blocks);
    if (d->w.trimmed != before) {
        RETAILMSG(1, (TEXT("CLASSPAD STORAGE: %u clusters the file system no longer uses dropped\r\n"), d->w.trimmed - before));
    }
}

// Writes one dirty cluster to the flash. A failure other than a full volume
// ends writing for this boot.
static DWORD CacheWrite(FUGUE_DISK *d, FUGUE_CACHE *c)
{
    int r;

    if (!c->dirty) return ERROR_SUCCESS;
    if (d->w.erased < CLASSPAD_FUGUE_GC_LOW) TrimNow(d);        // a reclaim is next: let it find room
    r = ClasspadFugueWrite(&d->w, c->lc, c->data);
    if (r == CLASSPAD_FUGUE_OK) { c->dirty = FALSE; return ERROR_SUCCESS; }
    RETAILMSG(1, (TEXT("CLASSPAD STORAGE: writing cluster %u failed (%d)%s\r\n"), c->lc, r,
                  r == CLASSPAD_FUGUE_ERR_FULL ? TEXT("") : TEXT(", read-only from now on")));
    if (r == CLASSPAD_FUGUE_ERR_FULL) return ERROR_DISK_FULL;
    d->writable = FALSE;
    d->w.writable = 0;
    return ERROR_WRITE_FAULT;
}

static DWORD CacheFlush(FUGUE_DISK *d)
{
    DWORD status = ERROR_SUCCESS, s;
    int i;

    for (i = 0; i < CACHE_CLUSTERS; i++) {
        if (d->cache[i].lc == NO_CLUSTER) continue;
        s = CacheWrite(d, &d->cache[i]);
        if (s != ERROR_SUCCESS) status = s;
    }
    return status;
}

// The cache entry of cluster lc, loaded with its current content. NULL when
// the entry pushed out could not be written (*status says why).
static FUGUE_CACHE *CacheGet(FUGUE_DISK *d, DWORD lc, DWORD *status)
{
    FUGUE_CACHE *c = CacheFind(d, lc), *victim = NULL;
    const unsigned char *p;
    int i;

    if (!c) {
        for (i = 0; i < CACHE_CLUSTERS; i++) {
            c = &d->cache[i];
            if (c->lc == NO_CLUSTER) { victim = c; break; }
            if (!victim || c->stamp < victim->stamp) victim = c;
        }
        c = victim;
        if (c->lc != NO_CLUSTER) {
            *status = CacheWrite(d, c);
            if (*status != ERROR_SUCCESS) return NULL;
        }
        p = ClasspadFugueCluster(&d->f, lc);
        if (p) memcpy(c->data, p, CLASSPAD_FUGUE_PAGE_BYTES);
        else memset(c->data, 0xFF, CLASSPAD_FUGUE_PAGE_BYTES);
        c->lc = lc;
        c->dirty = FALSE;
    }
    c->stamp = ++d->stamp;
    return c;
}

static DWORD WINAPI FlushThread(LPVOID param)
{
    FUGUE_DISK *d = (FUGUE_DISK *)param;

    for (;;) {
        WaitForSingleObject(d->dirtyEvent, INFINITE);
        if (d->stop) break;
        while (!d->stop && GetTickCount() - d->lastWrite < FLUSH_MS) Sleep(FLUSH_MS / 4);    // writes still coming
        EnterCriticalSection(&d->cs);
        CacheFlush(d);
        LeaveCriticalSection(&d->cs);
    }
    return 0;
}

//------------------------------------------------------------------------------
static DWORD RegDword(HKEY key, LPCTSTR name, DWORD def)
{
    DWORD type, value, size = sizeof value;
    if (RegQueryValueEx(key, name, NULL, &type, (LPBYTE)&value, &size) != ERROR_SUCCESS || type != REG_DWORD) return def;
    return value;
}

// The profile name (the driver key's "Profile", else FUGUE_PROFILE) and
// whether the registry allows writing ("Writable", default yes).
static BOOL ReadConfig(LPCTSTR activeKey, FUGUE_DISK *d)
{
    HKEY key;
    DWORD type, size = sizeof d->profile;
    BOOL writable = TRUE;

    wcscpy(d->profile, FUGUE_PROFILE);
    key = OpenDeviceKey(activeKey);
    if (key == INVALID_HANDLE_VALUE || !key) return writable;
    if (RegQueryValueEx(key, TEXT("Profile"), NULL, &type, (LPBYTE)d->profile, &size) != ERROR_SUCCESS || type != REG_SZ) {
        wcscpy(d->profile, FUGUE_PROFILE);
    }
    d->profile[PROFILENAMESIZE - 1] = 0;
    writable = RegDword(key, TEXT("Writable"), 1) != 0;
    RegCloseKey(key);
    return writable;
}

DWORD DSK_Init(DWORD context)
{
    FUGUE_DISK *d;
    unsigned *ver;
    unsigned blocks;
    ULONG pa = 0;
    BOOL allowed;
    LPCTSTR why = TEXT("");
    int i;

    d = (FUGUE_DISK *)LocalAlloc(LPTR, sizeof *d);
    ver = (unsigned *)LocalAlloc(LMEM_FIXED, FUGUE_CLUSTERS * sizeof *ver);    // versions: only while scanning
    if (!d || !ver) {
        RETAILMSG(1, (TEXT("CLASSPAD STORAGE: out of memory\r\n")));
        if (d) LocalFree(d);
        if (ver) LocalFree(ver);
        return 0;
    }
    blocks = ClasspadFugueScan(&d->f, (const void *)CLASSPAD_FUGUE_PA, CLASSPAD_FUGUE_SECTORS, d->loc, ver, FUGUE_CLUSTERS);
    if (!blocks) {
        LocalFree(ver);
        RETAILMSG(1, (TEXT("CLASSPAD STORAGE: no volume in Casio's storage memory (%u records, %u rejected), not mounted\r\n"),
                      d->f.records, d->f.rejected));
        LocalFree(d);
        return 0;
    }
    allowed = ReadConfig((LPCTSTR)context, d);
    InitializeCriticalSection(&d->cs);
    for (i = 0; i < CACHE_CLUSTERS; i++) d->cache[i].lc = NO_CLUSTER;

    // Writable when everything it takes is there
    if (!allowed) why = TEXT(" (registry)");
    else if (!(ClasspadHcGetStat(62) & HC_FEATURE_NOR) || ClasspadHcNor(HC_NOR_PROBE, 0, 0, 0) != HC_NOR_MAGIC) why = TEXT(" (the translator cannot program the flash)");
    else if (!AllocPhysMem(BOUNCE_BYTES, PAGE_READWRITE, 0, 0, &pa) || !pa) why = TEXT(" (no memory)");
    else {
        d->bounce = (BYTE *)(pa | 0x80000000);
        if (!ClasspadFugueWrInit(&d->w, &d->f, d->page, d, NorProgram, NorErase)) why = TEXT(" (no DETAIL record: not a volume of Casio's OS)");
        else {
            d->dirtyEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
            d->flushThread = d->dirtyEvent ? CreateThread(NULL, 0, FlushThread, d, 0, NULL) : NULL;
            if (d->flushThread) d->writable = TRUE;
            else why = TEXT(" (no flush thread)");
        }
    }
    if (d->writable) {                  // the versions stay: the writer needs them to trim
        ClasspadFugueTrimInit(&d->w, ver);
        d->ver = ver;
        d->trimDue = TRUE;
    } else {
        LocalFree(ver);
    }
    RETAILMSG(1, (TEXT("CLASSPAD STORAGE: %u sectors, %u clusters in %u records (%u rejected), %s%s, %u erased sectors, profile %s\r\n"),
                  blocks, d->f.live, d->f.records, d->f.rejected, d->writable ? TEXT("writable") : TEXT("read-only"), why,
                  d->writable ? d->w.erased : 0, d->profile));
    return (DWORD)d;
}

BOOL DSK_Deinit(DWORD context)
{
    FUGUE_DISK *d = (FUGUE_DISK *)context;

    if (!d) return TRUE;
    if (d->flushThread) {
        d->stop = TRUE;
        SetEvent(d->dirtyEvent);
        WaitForSingleObject(d->flushThread, INFINITE);
        CloseHandle(d->flushThread);
    }
    EnterCriticalSection(&d->cs);
    if (d->writable) CacheFlush(d);
    LeaveCriticalSection(&d->cs);
    if (d->dirtyEvent) CloseHandle(d->dirtyEvent);
    DeleteCriticalSection(&d->cs);
    if (d->ver) LocalFree(d->ver);
    LocalFree(d);                       // the bounce page stays: it has no handle to free through its P1 alias
    return TRUE;
}

// A read-only disk refuses write access: the Storage Manager then opens the
// device for reading and marks the store read-only (CStore::OpenDisk).
DWORD DSK_Open(DWORD context, DWORD access, DWORD share)
{
    FUGUE_DISK *d = (FUGUE_DISK *)context;

    if (d && (access & GENERIC_WRITE) && !d->writable) {
        SetLastError(ERROR_ACCESS_DENIED);
        return 0;
    }
    return context;
}

BOOL DSK_Close(DWORD open)
{
    FUGUE_DISK *d = (FUGUE_DISK *)open;

    if (d && d->writable) {             // whoever had the disk is done with it: nothing stays in RAM
        EnterCriticalSection(&d->cs);
        CacheFlush(d);
        LeaveCriticalSection(&d->cs);
    }
    return TRUE;
}

DWORD DSK_Read(DWORD open, LPVOID buf, DWORD len) { return (DWORD)-1; }
DWORD DSK_Write(DWORD open, LPCVOID buf, DWORD len) { return (DWORD)-1; }
DWORD DSK_Seek(DWORD open, long pos, DWORD type) { return (DWORD)-1; }
void DSK_PowerDown(DWORD context) { }
void DSK_PowerUp(DWORD context) { }

// One scatter/gather request: sr_num_sec sectors from sr_start, laid end to
// end in the request's buffers. Returns the request's status.
static DWORD DoTransfer(FUGUE_DISK *d, SG_REQ *req, DWORD reqBytes, BOOL write)
{
    const unsigned char *p;
    unsigned char *buf;
    FUGUE_CACHE *c;
    DWORD i, pos, left, room, n, in, lc, status = ERROR_SUCCESS;

    if (!req->sr_num_sg || req->sr_num_sg > MAX_SG_BUF ||
        reqBytes < sizeof(SG_REQ) + sizeof(SG_BUF) * (req->sr_num_sg - 1)) return ERROR_INVALID_PARAMETER;
    if (req->sr_start >= d->f.blocks || req->sr_num_sec > d->f.blocks - req->sr_start) return ERROR_SECTOR_NOT_FOUND;
    if (write && !d->writable) return ERROR_WRITE_PROTECT;

    pos = req->sr_start * CLASSPAD_FUGUE_BLOCK_BYTES;           // byte position in the device (24 MiB: fits)
    left = req->sr_num_sec * CLASSPAD_FUGUE_BLOCK_BYTES;
    EnterCriticalSection(&d->cs);
    for (i = 0; i < req->sr_num_sg && left && status == ERROR_SUCCESS; i++) {
        room = req->sr_sglist[i].sb_len;
        buf = (unsigned char *)MapCallerPtr(req->sr_sglist[i].sb_buf, room);
        if (!buf) { status = ERROR_INVALID_PARAMETER; break; }
        if (room > left) room = left;
        left -= room;
        while (room) {                                          // a run inside one logical cluster
            lc = pos / CLASSPAD_FUGUE_PAGE_BYTES;
            in = pos % CLASSPAD_FUGUE_PAGE_BYTES;
            n = CLASSPAD_FUGUE_PAGE_BYTES - in;
            if (n > room) n = room;
            if (write) {
                c = CacheGet(d, lc, &status);
                if (!c) break;
                if (lc < FUGUE_CLUSTERS) d->written[lc >> 3] |= (BYTE)(1 << (lc & 7));
                d->trimDue = TRUE;
                if (memcmp(c->data + in, buf, n)) {
                    memcpy(c->data + in, buf, n);
                    c->dirty = TRUE;
                }
            } else if ((c = CacheFind(d, lc)) != NULL) {        // newer than the flash
                memcpy(buf, c->data + in, n);
            } else {
                p = ClasspadFugueCluster(&d->f, lc);
                if (p) memcpy(buf, p + in, n);
                else memset(buf, 0xFF, n);                      // never written: erased flash, as Casio's OS reads it
            }
            buf += n; pos += n; room -= n;
        }
    }
    if (write) {
        d->lastWrite = GetTickCount();
        SetEvent(d->dirtyEvent);
    }
    LeaveCriticalSection(&d->cs);
    if (status == ERROR_SUCCESS && left) status = ERROR_INVALID_PARAMETER;    // buffers shorter than the sectors asked for
    return status;
}

// The volume for IOCTL_CLASSPAD_FUGUE_STATUS / _OPTIMIZE. Under d->cs.
static void FugueInfo(FUGUE_DISK *d, CLASSPAD_FUGUE_INFO *info)
{
    memset(info, 0, sizeof *info);
    info->writable = d->writable;
    info->sectors = d->f.sectors;
    info->liveClusters = d->f.live;
    info->capacityKB = d->f.blocks / 2;
    if (!d->writable) return;
    info->erased = d->w.erased;
    info->reclaimable = ClasspadFugueReclaimable(&d->w);
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
    CLASSPAD_FUGUE_INFO *fi;
    SG_REQ *req;
    DWORD status = ERROR_SUCCESS;
    int r;

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
            status = DoTransfer(d, req, inSize, code == DISK_IOCTL_WRITE || code == IOCTL_DISK_WRITE);
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

        case IOCTL_DISK_DEVICE_INFO:                            // read-only here is what makes FATFS mount read-only
            sdi = (STORAGEDEVICEINFO *)in;
            if (!sdi || inSize != sizeof(STORAGEDEVICEINFO)) { status = ERROR_INVALID_PARAMETER; break; }
            sdi->cbSize = sizeof(STORAGEDEVICEINFO);
            wcscpy(sdi->szProfile, d->profile);
            sdi->dwDeviceClass = STORAGE_DEVICE_CLASS_BLOCK;
            sdi->dwDeviceType = STORAGE_DEVICE_TYPE_FLASH;
            sdi->dwDeviceFlags = d->writable ? STORAGE_DEVICE_FLAG_READWRITE : STORAGE_DEVICE_FLAG_READONLY;
            if (returned) *returned = sizeof(STORAGEDEVICEINFO);
            break;

        case IOCTL_DISK_FLUSH_CACHE:
            if (d->writable) {
                EnterCriticalSection(&d->cs);
                status = CacheFlush(d);
                LeaveCriticalSection(&d->cs);
            }
            break;

        case IOCTL_CLASSPAD_FUGUE_STATUS:                       // classpad_fugueioctl.h
        case IOCTL_CLASSPAD_FUGUE_OPTIMIZE:
        case IOCTL_CLASSPAD_FUGUE_OPTIMIZE_STEP:
            fi = (CLASSPAD_FUGUE_INFO *)out;
            if (!fi || outSize != sizeof(CLASSPAD_FUGUE_INFO)) { status = ERROR_INVALID_PARAMETER; break; }
            EnterCriticalSection(&d->cs);
            TrimNow(d);                                         // what the FAT has as free counts as reclaimable
            if (code != IOCTL_CLASSPAD_FUGUE_STATUS && d->writable) {
                unsigned n;
                r = CLASSPAD_FUGUE_OK;
                CacheFlush(d);                                  // what is still in RAM first: its old copies become reclaimable
                if (!d->writable) n = 0;
                else if (code == IOCTL_CLASSPAD_FUGUE_OPTIMIZE_STEP) n = ClasspadFugueOptimizeStep(&d->w, &r);
                else n = ClasspadFugueOptimize(&d->w, &r);
                if (r != CLASSPAD_FUGUE_OK && r != CLASSPAD_FUGUE_ERR_FULL) {
                    RETAILMSG(1, (TEXT("CLASSPAD STORAGE: optimise failed (%d) after %u sectors, read-only from now on\r\n"), r, n));
                    d->writable = FALSE;
                    d->w.writable = 0;
                }
                FugueInfo(d, fi);
                fi->reclaimed = n;
                fi->status = (r == CLASSPAD_FUGUE_ERR_FULL) ? 0 : (DWORD)r;
                if (n) RETAILMSG(1, (TEXT("CLASSPAD STORAGE: optimised, %u sectors reclaimed, %u erased sectors\r\n"), n, fi->erased));
            } else {
                FugueInfo(d, fi);
            }
            LeaveCriticalSection(&d->cs);
            if (returned) *returned = sizeof(CLASSPAD_FUGUE_INFO);
            break;

        case DISK_IOCTL_INITIALIZED:
        case IOCTL_DISK_INITIALIZED:
            break;

        case DISK_IOCTL_SETINFO:
        case IOCTL_DISK_SETINFO:
        case DISK_IOCTL_FORMAT_MEDIA:
        case IOCTL_DISK_FORMAT_MEDIA:                           // the volume is Casio's: never formatted from here
            status = ERROR_WRITE_PROTECT;
            break;

        default:                                                // GETNAME (the profile's Folder answers), storage id, delete sectors...
            status = ERROR_NOT_SUPPORTED;
            break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        status = ERROR_INVALID_PARAMETER;
    }

    if (status != ERROR_SUCCESS) { SetLastError(status); return FALSE; }
    return TRUE;
}
