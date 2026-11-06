/* nkflash.c - write a Windows CE image into the NOR from Casio's OS, where
 * the translator's launcher looks for it (NK_FLASH, docs/05 section 1).
 * Reads \fls0\NK.bin (Platform Builder's B000FF output, copied to the
 * calculator's storage over USB) and does what tools/nkflash.py does for the
 * emulator:
 *   - an XIP build (image start == NK_FLASH, config.bib ROMSTART): every
 *     record is programmed at its own address (P1 address - 0x80000000);
 *   - a RAM image (image start in RAM): the .bin is programmed as is at
 *     NK_FLASH, where the translator copies the records into RAM.
 * Only the window NK_FLASH .. NK_FLASH + NK_FLASH_MAX is ever touched
 * (the erased hole inside Casio's OS image, docs/03), and a sector in it is
 * erased only when it is still blank, the window already holds a CE image
 * of ours or (NK_WINDOW=eact) the eActivity volume; anything else stops
 * before the first erase.
 *
 * Two users: wince.hhk (hhk/launcher.c) calls nkflash_open / compare /
 * write to bring the flash up to date before it boots, and nkflash.hhk
 * (NKFLASH_STANDALONE) is the same as a tool of its own. */
#include "util.h"
#include "os.h"
#include "norflash.h"
#include "nkflash.h"

#ifndef NK_FLASH
#define NK_FLASH     0x81000000u
#endif
#ifndef NK_FLASH_MAX
#define NK_FLASH_MAX 0x003E0000u
#endif
#define WIN_LO (NK_FLASH - NOR_P1)
#define WIN_HI (WIN_LO + NK_FLASH_MAX)
#define CHUNK  0x10000u
#define HDR    15u

static const char *paths[] = { "\\fls0\\NK.bin", "\\fls0\\nk.bin", "\\fls0\\wince\\NK.bin" };
int nk_row = 1;

static struct {
    int fd;
    u32 size, start, length, nrec, lo, hi, entry;
    int xip;
    u8 *buf;
} nk = { -1, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

void nk_say(const char *fmt, ...)
{
    char buf[48]; va_list ap;
    va_start(ap, fmt);
    hhk_vformat(buf, 41, fmt, ap);
    va_end(ap);
    if (nk_row > 35) { ui_clear(); nk_row = 1; }
    ui_printf(nk_row++, "%s", buf);
    ui_refresh();
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

static int read_exact(int fd, void *buf, u32 n)
{
    u8 *p = buf;
    while (n) {
        int r = os_read(fd, p, (int)(n > CHUNK ? CHUNK : n));
        if (r <= 0) return -1;
        p += r; n -= (u32)r;
    }
    return 0;
}

static int window_has_our_image(void)
{
    const char *p = (const char *)NK_FLASH;
    return !memcmp(p + 0x40, "ECEC", 4) || !memcmp(p, "B000FF\n", 7);
}

/* NK_WINDOW=eact: the window is the eActivity volume, which Casio's OS
 * reformats at every boot that finds it holding something else (docs/03), so
 * everything in it is ours to erase. Fugue is log-structured: on a used
 * calculator its header sector ("Fugue FAT File System" at +0x2020) can be
 * any sector of the area and the first one may well be blank (seen on the
 * first hardware run: "sector at 83880000 is in use" with the first sector
 * passing), so the name is only looked for, never required. */
static int window_is_fugue_volume(void)
{
#ifdef NK_WINDOW_EACT
    u32 s;
    for (s = 0; s < NK_FLASH_MAX; s += NOR_SECTOR)
        if (!memcmp((const char *)NK_FLASH + s + 0x2020, "Fugue FAT File System", 21)) return 1;
#endif
    return 0;
}

static int window_is_eact(void)
{
#ifdef NK_WINDOW_EACT
    return 1;
#else
    return 0;
#endif
}

const char *nkflash_window_state(void)
{
    return window_has_our_image() ? "holds a CE image" :
           window_is_fugue_volume() ? "is the eActivity volume" :
           window_is_eact() ? "is the eActivity area" : "is blank";
}

void nkflash_close(void)
{
    if (nk.fd >= 0) os_close(nk.fd);
    nk.fd = -1;
    if (nk.buf) os_free(nk.buf);
    nk.buf = 0;
}

/* open \fls0\NK.bin, parse the header and walk the records (pass 1) */
int nkflash_open(void)
{
    u8 hdr[HDR], rec[12];
    u32 pos, i;
    nk.fd = -1; nk.nrec = 0; nk.lo = 0xFFFFFFFFu; nk.hi = 0; nk.entry = 0;
    for (i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        nk.fd = os_open(paths[i]);
        if (nk.fd >= 0) { nk_say("file %s", paths[i]); break; }
    }
    if (nk.fd < 0) return NKF_NOFILE;
    if (os_fsize(nk.fd, &nk.size) || nk.size < HDR) { fail("file size"); goto bad; }
    if (read_exact(nk.fd, hdr, HDR) || memcmp(hdr, "B000FF\n", 7)) { fail("not a B000FF image"); goto bad; }
    nk.start = le32(hdr + 7); nk.length = le32(hdr + 11);
    nk_say("image %08x + %x, %u bytes file", nk.start, nk.length, nk.size);

    nk.xip = nk.start >= NK_FLASH && nk.start < NK_FLASH + NK_FLASH_MAX;
    if (nk.xip) {
        if (nk.start != NK_FLASH) { fail("ROMSTART is not NK_FLASH"); goto bad; }
        if (nk.length > NK_FLASH_MAX) { fail("image larger than the window"); goto bad; }
        pos = HDR;
        for (;;) {
            u32 addr, len, chk;
            if (read_exact(nk.fd, rec, 12)) { fail("record header at %u", pos); goto bad; }
            addr = le32(rec); len = le32(rec + 4); chk = le32(rec + 8);
            pos += 12;
            if (addr == 0 && chk == 0) { nk.entry = len; break; }
            if (addr < nk.start || len > nk.length || addr - nk.start > nk.length - len) { fail("record %08x+%x outside", addr, len); goto bad; }
            if (addr - NOR_P1 < nk.lo) nk.lo = addr - NOR_P1;
            if (addr - NOR_P1 + len > nk.hi) nk.hi = addr - NOR_P1 + len;
            nk.nrec++;
            pos += len;
            if (os_lseek(nk.fd, (int)pos) < 0) { fail("seek to %u", pos); goto bad; }
        }
        nk_say("%u records, %08x..%08x, entry %08x", nk.nrec, NOR_P1 + nk.lo, NOR_P1 + nk.hi, nk.entry);
        if (nk.hi > WIN_HI || nk.lo < WIN_LO) { fail("records leave the window"); goto bad; }
    } else {
        nk_say("RAM image: the file goes to %08x", NK_FLASH);
        nk.lo = WIN_LO; nk.hi = WIN_LO + nk.size + (nk.size & 1);
        if (nk.hi > WIN_HI) { fail("file larger than the window"); goto bad; }
    }
    nk.buf = os_malloc(CHUNK + 2);
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
        if (read_exact(nk.fd, nk.buf, c)) return -1;
        if (memcmp(nk.buf, (const void *)(NOR_P1 + off), c)) return 0;
        off += c; n -= c;
    }
    return 1;
}

int nkflash_compare(void)
{
    u8 rec[12];
    u32 pos;
    int r;
    if (nk.xip) {
        pos = HDR;
        if (os_lseek(nk.fd, (int)pos) < 0) return -1;
        for (;;) {
            u32 addr, len, chk;
            if (read_exact(nk.fd, rec, 12)) return -1;
            addr = le32(rec); len = le32(rec + 4); chk = le32(rec + 8);
            pos += 12;
            if (addr == 0 && chk == 0) return 1;
            r = compare_stream(addr - NOR_P1, len);
            if (r != 1) return r;
            pos += len;
        }
    }
    if (os_lseek(nk.fd, 0) < 0) return -1;
    return compare_stream(WIN_LO, nk.size);
}

/* erase the sectors covering [lo, hi) after checking each is ours to erase */
static int erase_span(u32 lo, u32 hi)
{
    u32 s, first = lo & ~(NOR_SECTOR - 1), n = 0, i = 0;
    int ours = window_has_our_image() || window_is_eact();
    for (s = first; s < hi; s += NOR_SECTOR) {
        n++;
        if (!nor_sector_erased(s) && !ours)
            return fail("sector at %08x is in use", NOR_P1 + s);
    }
    for (s = first; s < hi; s += NOR_SECTOR) {
        nk_say("erasing sector %u/%u at %08x", ++i, n, NOR_P1 + s);
        if (nor_erase_sector(s)) return fail("erase at %08x", NOR_P1 + s);
    }
    return 0;
}

/* stream n bytes from the file to flash offset off, returning their byte sum */
static int program_stream(u32 off, u32 n, u32 *sum)
{
    u32 done = 0;
    while (n) {
        u32 c = n > CHUNK ? CHUNK : n, w = c + (c & 1);
        if (read_exact(nk.fd, nk.buf, c)) return fail("read at file byte %u", done);
        *sum += bytesum(nk.buf, c);
        if (w != c) nk.buf[c] = 0xFF;               /* odd tail: pad with the erased value */
        if (nor_program(off, nk.buf, w)) return fail("program at %08x", NOR_P1 + off);
        off += c; n -= c; done += c;
        if ((done & 0x3FFFF) == 0) { ui_printf(nk_row - 1, "  %u KiB", done >> 10); ui_refresh(); }
    }
    return 0;
}

int nkflash_write(void)
{
    u8 rec[12];
    u32 pos, i = 0;
    if (erase_span(nk.lo, nk.hi)) return -1;
    if (nk.xip) {
        pos = HDR;
        if (os_lseek(nk.fd, (int)pos) < 0) return fail("seek");
        for (;;) {
            u32 addr, len, chk, sum = 0;
            if (read_exact(nk.fd, rec, 12)) return fail("record header at %u", pos);
            addr = le32(rec); len = le32(rec + 4); chk = le32(rec + 8);
            pos += 12;
            if (addr == 0 && chk == 0) break;
            nk_say("record %u/%u: %08x + %x", ++i, nk.nrec, addr, len);
            if (program_stream(addr - NOR_P1, len, &sum)) return -1;
            if (sum != chk) return fail("record %u checksum", i);
            pos += len;
        }
        nk_say("ROMHDR marker: %s", !memcmp((const char *)NK_FLASH + 0x40, "ECEC", 4) ? "ECEC found" : "MISSING");
    } else {
        u32 sum = 0;
        if (os_lseek(nk.fd, 0) < 0) return fail("seek");
        nk_say("programming %u bytes", nk.size);
        if (program_stream(WIN_LO, nk.size, &sum)) return -1;
    }
    return 0;
}

#ifdef NKFLASH_STANDALONE
int hhk_main(int argc, char **argv, char **envp)
{
    int r;
    os_init(argc, argv, envp);
    if (!os_ok) return 1;                          /* no OS routines: nothing we can do or say */
    ui_clear(); nk_row = 1;
    nk_say("CE image flasher (wince-classpad)");
    nk_say("OS routines: %s", os_source);
    nk_say("window %08x..%08x %s", NK_FLASH, NK_FLASH + NK_FLASH_MAX, nkflash_window_state());
    r = nkflash_open();
    if (r == NKF_NOFILE) { nk_say("no \\fls0\\NK.bin. Any key."); ui_wait_exe_or_clear(); return 1; }
    if (r != NKF_READY) { nk_say("nothing was written. Any key."); ui_wait_exe_or_clear(); return 1; }
    nk_say("comparing with the flash...");
    r = nkflash_compare();
    if (r == 1) { nk_say("already in flash. Any key."); ui_wait_exe_or_clear(); nkflash_close(); return 0; }
    nk_say("EXE: erase and program   Clear: cancel");
    if (!ui_wait_exe_or_clear()) { nkflash_close(); return 0; }
    r = nkflash_write();
    nkflash_close();
    nk_say(r ? "nothing more was written. Any key." : "done. Start wince.hhk to boot it. Any key.");
    ui_wait_exe_or_clear();
    return r ? 1 : 0;
}
#endif
