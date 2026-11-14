/* nkflash.c - write Windows CE images into the NOR from Casio's OS, where
 * the translator's launcher and the OAL look for them (docs/03, docs/05).
 * Two windows, each fed by a B000FF file on the calculator's storage:
 *   - NK (NK_FLASH .. + NK_FLASH_MAX, \fls0\NK.bin): the image the translator
 *     boots. An XIP build (image start == NK_FLASH) has every record
 *     programmed at its own address (P1 address - 0x80000000); a RAM image
 *     is programmed as is at NK_FLASH and copied into RAM by the translator.
 *   - EXT (EXT_FLASH .. + EXT_FLASH_MAX, \fls0\EXT.bin): the second XIP
 *     region (Math Xpander), which the OAL chains to the kernel
 *     (classpad_extrom.h).
 * Since 2026-11-09 NK is in the hole inside Casio's OS image, which Casio's
 * boots leave alone (written once, found identical afterwards), and EXT in
 * the eActivity area, which Casio's OS reformats, so EXT is the one usually
 * rewritten before a CE session (dbt/Makefile NK_WINDOW; the images before
 * that date had it the other way round). A window is only ever touched
 * inside its own bounds, and a sector in it is erased only when it is still
 * blank, the window already holds a CE image of ours ("ECEC" at +0x40 or a
 * B000FF header) or the window is the eActivity area (*_WINDOW_EACT);
 * anything else stops before the first erase (nk_refused), unless the
 * caller sets nk_force after asking the user. The sectors are erased from
 * the last to the first, so the first sector, the one with the marker that
 * says the window is ours, goes last: an erase that is cut off leaves a
 * window the next start still recognises and erases again.
 *
 * Used by wince.hh3 (hhk/loader.c), a gint program: it calls nkflash_open /
 * compare / write per window and follows nk_progress; nk_say's lines are
 * not shown, the last one stays in nk_last for the error screen. Files are
 * read through gint's file system (POSIX open/read/lseek on /NK.bin =
 * \fls0\NK.bin), and through hhk/cez.c: a file packed by tools/cepack.py
 * (same name, about half the size) reads as the B000FF file it was made
 * from, unpacked a block at a time. */
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include "util.h"
#include "norflash.h"
#include "nkflash.h"
#include "cez.h"

#ifndef NK_FLASH
#define NK_FLASH     0x80EE0000u
#endif
#ifndef NK_FLASH_MAX
#define NK_FLASH_MAX 0x00500000u
#endif
#define CHUNK  0x8000u
#define HDR    15u

/* gint's file system takes POSIX paths in \fls0, the storage memory */
static const char *const nk_paths[] = { "/NK.bin", "/nk.bin", "/wince/NK.bin", 0 };
#ifdef EXT_FLASH
static const char *const ext_paths[] = { "/EXT.bin", "/ext.bin", "/wince/EXT.bin", 0 };
#endif

#ifdef NK_WINDOW_EACT
#define NK_EACT 1
#else
#define NK_EACT 0
#endif
#ifdef EXT_WINDOW_EACT
#define EXT_EACT 1
#else
#define EXT_EACT 0
#endif

const nkwindow nk_windows[] = {
    { "NK", NK_FLASH, NK_FLASH_MAX, nk_paths, NK_EACT },
#ifdef EXT_FLASH
    { "EXT", EXT_FLASH, EXT_FLASH_MAX, ext_paths, EXT_EACT },
#endif
};
const int nk_nwindows = sizeof nk_windows / sizeof nk_windows[0];

char nk_last[48];
volatile int nk_abort;
void (*nk_progress)(int what, u32 done, u32 total, u32 addr);
#define PROGRESS(w, d, t, a) do { if (nk_progress) nk_progress(w, d, t, a); } while (0)

static struct {
    const nkwindow *w;
    int fd;
    u32 size, start, length, nrec, lo, hi, entry;
    int xip;
    u8 *buf;
    u32 bytes, done;                  /* bytes to compare or program, and so far */
} nk = { 0, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

#define WIN_LO (nk.w->base - NOR_P1)
#define WIN_HI (WIN_LO + nk.w->max)

void nk_say(const char *fmt, ...)
{
    char buf[48]; va_list ap;
    va_start(ap, fmt);
    hhk_vformat(buf, 41, fmt, ap);
    va_end(ap);
    memcpy(nk_last, buf, sizeof nk_last);
}

static int fail(const char *fmt, ...)
{
    char buf[48]; va_list ap;
    va_start(ap, fmt);
    hhk_vformat(buf, 41, fmt, ap);
    va_end(ap);
    nk_say("FAILED: %s", buf);
    return -1;
}

int window_has_our_image(const nkwindow *w)
{
    const char *p = (const char *)w->base;
    return !memcmp(p + 0x40, "ECEC", 4) || !memcmp(p, "B000FF\n", 7);
}

/* The window in the eActivity area (NK with NK_WINDOW=eact, EXT otherwise)
 * is the eActivity volume, which Casio's OS reformats at every boot that
 * finds it holding something else (docs/03), so everything in it is ours to
 * erase. Fugue is log-structured: on a used
 * calculator its header sector ("Fugue FAT File System" at +0x2020) can be
 * any sector of the area and the first one may well be blank (seen on the
 * first hardware run: "sector at 83880000 is in use" with the first sector
 * passing), so the name is only looked for, never required. */
static int window_is_fugue_volume(const nkwindow *w)
{
    u32 s;
    if (!w->eact) return 0;
    for (s = 0; s < w->max; s += NOR_SECTOR)
        if (!memcmp((const char *)w->base + s + 0x2020, "Fugue FAT File System", 21)) return 1;
    return 0;
}

const char *nkflash_window_state(const nkwindow *w)
{
    return window_has_our_image(w) ? "holds a CE image" :
           window_is_fugue_volume(w) ? "is the eActivity volume" :
           w->eact ? "is the eActivity area" : "is blank";
}

void nkflash_close(void)
{
    cez_close();
    if (nk.fd >= 0) close(nk.fd);
    nk.fd = -1;
    if (nk.buf) free(nk.buf);
    nk.buf = 0;
}

/* open the window's file, parse the header and walk the records (pass 1) */
int nkflash_open(const nkwindow *w)
{
    u8 hdr[HDR], rec[12];
    u32 pos, i;
    nk.w = w;
    nk.fd = -1; nk.nrec = 0; nk.lo = 0xFFFFFFFFu; nk.hi = 0; nk.entry = 0; nk.bytes = 0;
    for (i = 0; w->paths[i]; i++) {
        nk.fd = open(w->paths[i], O_RDONLY);
        if (nk.fd >= 0) { nk_say("file %s", w->paths[i]); break; }
    }
    if (nk.fd < 0) return NKF_NOFILE;
    if (cez_open(nk.fd)) { fail("the file is damaged"); goto bad; }
    nk.size = cez_size();                           /* of the image, packed or not */
    if (nk.size < HDR) { fail("file too short"); goto bad; }
    if (cez_read(hdr, HDR) || memcmp(hdr, "B000FF\n", 7)) { fail("not a B000FF image"); goto bad; }
    nk.start = le32(hdr + 7); nk.length = le32(hdr + 11);
    nk_say("image %08x + %x, %u bytes file", nk.start, nk.length, nk.size);

    nk.xip = nk.start >= w->base && nk.start < w->base + w->max;
    if (nk.xip) {
        if (nk.start != w->base) { fail("%s image does not start at %08x", w->name, w->base); goto bad; }
        if (nk.length > w->max) { fail("image larger than the window"); goto bad; }
        pos = HDR;
        /* a packed file says what its records are (cez_records): nothing is
         * unpacked to learn it. Otherwise the file is walked, record by record. */
        for (i = 0; i < cez_records(); i++) {
            u32 addr, len, crc;
            cez_record(i, &addr, &len, &crc);
            if (addr < nk.start || len > nk.length || addr - nk.start > nk.length - len) { fail("record %08x+%x outside", addr, len); goto bad; }
            if (addr - NOR_P1 < nk.lo) nk.lo = addr - NOR_P1;
            if (addr - NOR_P1 + len > nk.hi) nk.hi = addr - NOR_P1 + len;
            nk.nrec++;
            nk.bytes += len;
        }
        while (!cez_records()) {
            u32 addr, len, chk;
            if (cez_read(rec, 12)) { fail("record header at %u", pos); goto bad; }
            addr = le32(rec); len = le32(rec + 4); chk = le32(rec + 8);
            pos += 12;
            if (addr == 0 && chk == 0) { nk.entry = len; break; }
            if (addr < nk.start || len > nk.length || addr - nk.start > nk.length - len) { fail("record %08x+%x outside", addr, len); goto bad; }
            if (addr - NOR_P1 < nk.lo) nk.lo = addr - NOR_P1;
            if (addr - NOR_P1 + len > nk.hi) nk.hi = addr - NOR_P1 + len;
            nk.nrec++;
            nk.bytes += len;
            pos += len;
            if (cez_seek(pos)) { fail("record %u leaves the file", nk.nrec); goto bad; }
        }
        nk_say("%u records, %08x..%08x, entry %08x", nk.nrec, NOR_P1 + nk.lo, NOR_P1 + nk.hi, nk.entry);
        if (nk.hi > WIN_HI || nk.lo < WIN_LO) { fail("records leave the window"); goto bad; }
    } else if (w->base == NK_FLASH) {
        nk_say("RAM image: the file goes to %08x", w->base);
        nk.lo = WIN_LO; nk.hi = WIN_LO + nk.size + (nk.size & 1);
        nk.bytes = nk.size;
        if (nk.hi > WIN_HI) { fail("file larger than the window"); goto bad; }
    } else {
        fail("%s.bin is not built for %08x", w->name, w->base); goto bad;
    }
    nk.buf = malloc(CHUNK + 2);
    if (!nk.buf) { fail("no memory for the buffer"); goto bad; }
    return NKF_READY;
bad:
    nkflash_close();
    return NKF_BAD;
}

/* walk the file once more and compare it with the flash: 1 same, 0 differs, -1 error */
static int compare_stream(u32 off, u32 n)
{
    while (n) {
        u32 c = n > CHUNK ? CHUNK : n;
        if (nk_abort) return -2;
        if (cez_read(nk.buf, c)) return -1;
        if (memcmp(nk.buf, (const void *)(NOR_P1 + off), c)) return 0;
        off += c; n -= c; nk.done += c;
        PROGRESS(NKP_COMPARE, nk.done, nk.bytes, NOR_P1 + off);
    }
    return 1;
}

/* the CRC-32 of n bytes of flash at offset off against crc: 1 same, 0 differs, -2 nk_abort */
static int compare_crc(u32 off, u32 n, u32 crc)
{
    u32 have = 0;
    while (n) {
        u32 c = n > CHUNK ? CHUNK : n;
        if (nk_abort) return -2;
        have = cez_crc32(have, (const void *)(NOR_P1 + off), c);
        off += c; n -= c; nk.done += c;
        PROGRESS(NKP_COMPARE, nk.done, nk.bytes, NOR_P1 + off);
    }
    return have == crc;
}

/* A packed file carries the CRC-32 of every record (of the whole file, for
 * a RAM image): the flash is compared by CRC and the file is not unpacked
 * for it, which is what made the check of a packed image slow. The data
 * itself is still checked on its way into the flash (the records' sums). */
int nkflash_compare(void)
{
    u8 rec[12];
    u32 pos, i, crc;
    int r;
    nk.done = 0;
    if (nk.xip && cez_records()) {
        for (i = 0; i < cez_records(); i++) {
            u32 addr, len;
            cez_record(i, &addr, &len, &crc);
            r = compare_crc(addr - NOR_P1, len, crc);
            if (r != 1) return r;
        }
        return 1;
    }
    if (!nk.xip && cez_file_crc(&crc)) return compare_crc(WIN_LO, nk.size, crc);
    if (nk.xip) {
        pos = HDR;
        if (cez_seek(pos)) return -1;
        for (;;) {
            u32 addr, len, chk;
            if (cez_read(rec, 12)) return -1;
            addr = le32(rec); len = le32(rec + 4); chk = le32(rec + 8);
            pos += 12;
            if (addr == 0 && chk == 0) return 1;
            r = compare_stream(addr - NOR_P1, len);
            if (r != 1) return r;
            pos += len;
        }
    }
    if (cez_seek(0)) return -1;
    return compare_stream(WIN_LO, nk.size);
}

int nk_refused, nk_force;

/* erase the sectors covering [lo, hi) after checking each is ours to erase */
static int erase_span(u32 lo, u32 hi)
{
    u32 s, first = lo & ~(NOR_SECTOR - 1), n = (hi - first + NOR_SECTOR - 1) / NOR_SECTOR, i = 0, k;
    int ours = window_has_our_image(nk.w) || nk.w->eact || nk_force;
    nk_refused = 0;
    PROGRESS(NKP_ERASE, 0, n, NOR_P1 + first);
    /* a window that is not known to be ours must be blank; one that is needs
     * no look (reading every sector through the uncached alias took seconds
     * before the first sector showed on the screen) */
    for (s = first; s < hi && !ours; s += NOR_SECTOR) {
        if (!nor_sector_erased(s)) {
            nk_refused = 1;
            return fail("sector at %08x is in use", NOR_P1 + s);
        }
    }
    for (k = n; k-- > 0; ) {                        /* last sector first: see the top of the file */
        s = first + k * NOR_SECTOR;
        PROGRESS(NKP_ERASE, i, n, NOR_P1 + s);
        nk_say("erasing sector %u/%u at %08x", ++i, n, NOR_P1 + s);
        if (nor_erase_sector(s)) return fail("erase at %08x", NOR_P1 + s);
    }
    PROGRESS(NKP_ERASE, n, n, NOR_P1 + first);
    return 0;
}

/* stream n bytes from the file to flash offset off, returning their byte sum */
static int program_stream(u32 off, u32 n, u32 *sum)
{
    u32 done = 0;
    while (n) {
        u32 c = n > CHUNK ? CHUNK : n, w = c + (c & 1);
        if (cez_read(nk.buf, c)) return fail("read at file byte %u", done);
        *sum += bytesum(nk.buf, c);
        if (w != c) nk.buf[c] = 0xFF;               /* odd tail: pad with the erased value */
        if (nor_program(off, nk.buf, w)) return fail("program at %08x", NOR_P1 + off);
        off += c; n -= c; done += c; nk.done += c;
        PROGRESS(NKP_WRITE, nk.done, nk.bytes, NOR_P1 + off);
    }
    return 0;
}

int nkflash_write(void)
{
    u8 rec[12];
    u32 pos, i = 0;
    if (erase_span(nk.lo, nk.hi)) return -1;
    nk.done = 0;
    PROGRESS(NKP_WRITE, 0, nk.bytes, nk.w->base);
    if (nk.xip) {
        pos = HDR;
        if (cez_seek(pos)) return fail("seek");
        for (;;) {
            u32 addr, len, chk, sum = 0;
            if (cez_read(rec, 12)) return fail("record header at %u", pos);
            addr = le32(rec); len = le32(rec + 4); chk = le32(rec + 8);
            pos += 12;
            if (addr == 0 && chk == 0) break;
            nk_say("record %u/%u: %08x + %x", ++i, nk.nrec, addr, len);
            PROGRESS(NKP_RECORD, i, nk.nrec, addr);
            if (program_stream(addr - NOR_P1, len, &sum)) return -1;
            if (sum != chk) return fail("record %u checksum", i);
            pos += len;
        }
        nk_say("ROMHDR marker: %s", !memcmp((const char *)nk.w->base + 0x40, "ECEC", 4) ? "ECEC found" : "MISSING");
        if (memcmp((const char *)nk.w->base + 0x40, "ECEC", 4)) return fail("no ROMHDR at %08x", nk.w->base + 0x40);
    } else {
        u32 sum = 0;
        if (cez_seek(0)) return fail("seek");
        nk_say("programming %u bytes", nk.size);
        if (program_stream(WIN_LO, nk.size, &sum)) return -1;
    }
    return 0;
}

void nkflash_info(nkinfo *i)
{
    i->size = nk.size;
    i->bytes = nk.bytes;
    i->records = nk.xip ? nk.nrec : 0;
    i->sectors = nk.hi > nk.lo ? ((nk.hi - 1) / NOR_SECTOR - nk.lo / NOR_SECTOR + 1) : 0;
    i->xip = nk.xip;
}
