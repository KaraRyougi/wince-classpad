/* nkflash.c - nkflash.hhk: write a Windows CE image into the NOR from Casio's
 * OS, where the translator's launcher looks for it (NK_FLASH, docs/05 section
 * 1). Reads \fls0\NK.bin (Platform Builder's B000FF output, copied to the
 * calculator's storage over USB) and does what tools/nkflash.py does for the
 * emulator:
 *   - an XIP build (image start == NK_FLASH, config.bib ROMSTART): every
 *     record is programmed at its own address (P1 address - 0x80000000);
 *   - a RAM image (image start in RAM): the .bin is programmed as is at
 *     NK_FLASH, where the translator copies the records into RAM.
 * Only the window NK_FLASH .. NK_FLASH + NK_FLASH_MAX is ever touched
 * (the erased hole inside Casio's OS image, docs/03), and a sector in it is
 * erased only when it is still blank or the window already holds a CE image
 * of ours; anything else stops the program before the first erase. */
#include "util.h"
#include "os.h"
#include "norflash.h"

#ifndef NK_FLASH
#define NK_FLASH     0x81000000u
#endif
#ifndef NK_FLASH_MAX
#define NK_FLASH_MAX 0x003E0000u
#endif
#define WIN_LO (NK_FLASH - NOR_P1)
#define WIN_HI (WIN_LO + NK_FLASH_MAX)
#define CHUNK  0x10000u

static const char *paths[] = { "\\fls0\\NK.bin", "\\fls0\\nk.bin", "\\fls0\\wince\\NK.bin" };
static int row;

static void say(const char *fmt, ...)
{
    char buf[48]; va_list ap;
    va_start(ap, fmt);
    hhk_vformat(buf, 41, fmt, ap);
    va_end(ap);
    if (row > 35) { ui_clear(); row = 1; }
    ui_printf(row++, "%s", buf);
    ui_refresh();
}

static int fail(const char *fmt, ...)
{
    char buf[48]; va_list ap;
    va_start(ap, fmt);
    hhk_vformat(buf, 41, fmt, ap);
    va_end(ap);
    say("FAILED: %s", buf);
    say("nothing more was written. Any key.");
    ui_wait_exe_or_clear();
    return 1;
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
 * a Fugue volume there is ours to erase as well; its header names itself. */
static int window_is_fugue_volume(void)
{
#ifdef NK_WINDOW_EACT
    return !memcmp((const char *)NK_FLASH + 0x2020, "Fugue FAT File System", 21);
#else
    return 0;
#endif
}

/* erase the sectors covering [lo, hi) after checking each is ours to erase */
static int erase_span(u32 lo, u32 hi)
{
    u32 s, first = lo & ~(NOR_SECTOR - 1), n = 0, i = 0;
    int ours = window_has_our_image() || window_is_fugue_volume();
    for (s = first; s < hi; s += NOR_SECTOR) {
        n++;
        if (!nor_sector_erased(s) && !ours)
            return fail("sector at %08x is in use", NOR_P1 + s);
    }
    for (s = first; s < hi; s += NOR_SECTOR) {
        say("erasing sector %u/%u at %08x", ++i, n, NOR_P1 + s);
        if (nor_erase_sector(s)) return fail("erase at %08x", NOR_P1 + s);
    }
    return 0;
}

/* stream n bytes from the file to flash offset off, returning their byte sum */
static int program_stream(int fd, u32 off, u32 n, u8 *buf, u32 *sum)
{
    u32 done = 0;
    while (n) {
        u32 c = n > CHUNK ? CHUNK : n, w = c + (c & 1);
        if (read_exact(fd, buf, c)) return fail("read at file byte %u", done);
        *sum += bytesum(buf, c);
        if (w != c) buf[c] = 0xFF;                   /* odd tail: pad with the erased value */
        if (nor_program(off, buf, w)) return fail("program at %08x", NOR_P1 + off);
        off += c; n -= c; done += c;
        if ((done & 0x3FFFF) == 0) { ui_printf(row - 1, "  %u KiB", done >> 10); ui_refresh(); }
    }
    return 0;
}

int hhk_main(int argc, char **argv, char **envp)
{
    u8 hdr[15], rec[12], *buf;
    u32 size, start, length, nrec = 0, lo = 0xFFFFFFFFu, hi = 0, entry = 0, pos, i;
    int fd = -1, xip;
    os_init(argc, argv, envp);
    if (!os_ok) return 1;                          /* no OS routines: nothing we can do or say */
    ui_clear(); row = 1;
    say("CE image flasher (wince-classpad)");
    say("OS routines: %s", os_source);
    say("window %08x..%08x", NK_FLASH, NK_FLASH + NK_FLASH_MAX);

    for (i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        fd = os_open(paths[i]);
        if (fd >= 0) { say("file %s", paths[i]); break; }
    }
    if (fd < 0) return fail("no \\fls0\\NK.bin (%d)", fd);
    if (os_fsize(fd, &size) || size < sizeof hdr) return fail("file size");
    if (read_exact(fd, hdr, sizeof hdr) || memcmp(hdr, "B000FF\n", 7)) return fail("not a B000FF image");
    start = le32(hdr + 7); length = le32(hdr + 11);
    say("image %08x + %x, %u bytes file", start, length, size);

    xip = start >= NK_FLASH && start < NK_FLASH + NK_FLASH_MAX;
    if (xip) {
        if (start != NK_FLASH) return fail("ROMSTART is not NK_FLASH");
        if (length > NK_FLASH_MAX) return fail("image larger than the window");
        /* pass 1: walk the records, check them, find the span */
        pos = sizeof hdr;
        for (;;) {
            u32 addr, len, chk;
            if (read_exact(fd, rec, 12)) return fail("record header at %u", pos);
            addr = le32(rec); len = le32(rec + 4); chk = le32(rec + 8);
            pos += 12;
            if (addr == 0 && chk == 0) { entry = len; break; }
            if (addr < start || len > length || addr - start > length - len) return fail("record %08x+%x outside", addr, len);
            if (addr - NOR_P1 < lo) lo = addr - NOR_P1;
            if (addr - NOR_P1 + len > hi) hi = addr - NOR_P1 + len;
            nrec++;
            pos += len;
            if (os_lseek(fd, (int)pos) < 0) return fail("seek to %u", pos);
        }
        say("%u records, %08x..%08x, entry %08x", nrec, NOR_P1 + lo, NOR_P1 + hi, entry);
        if (hi > WIN_HI || lo < WIN_LO) return fail("records leave the window");
    } else {
        say("RAM image: the file goes to %08x", NK_FLASH);
        lo = WIN_LO; hi = WIN_LO + size + (size & 1);
        if (hi > WIN_HI) return fail("file larger than the window");
    }
    say("%s", window_has_our_image() ? "window holds a CE image: replace it" :
               window_is_fugue_volume() ? "window is the eActivity volume: erase it" : "window is blank");
    say("EXE: erase and program   Clear: cancel");
    if (!ui_wait_exe_or_clear()) { os_close(fd); return 0; }

    buf = os_malloc(CHUNK + 2);
    if (!buf) return fail("no memory for the buffer");
    if (erase_span(lo, hi)) return 1;

    if (xip) {
        pos = sizeof hdr; i = 0;
        if (os_lseek(fd, (int)pos) < 0) return fail("seek");
        for (;;) {
            u32 addr, len, chk, sum = 0;
            if (read_exact(fd, rec, 12)) return fail("record header at %u", pos);
            addr = le32(rec); len = le32(rec + 4); chk = le32(rec + 8);
            pos += 12;
            if (addr == 0 && chk == 0) break;
            say("record %u/%u: %08x + %x", ++i, nrec, addr, len);
            if (program_stream(fd, addr - NOR_P1, len, buf, &sum)) return 1;
            if (sum != chk) return fail("record %u checksum", i);
            pos += len;
        }
        say("ROMHDR marker: %s", !memcmp((const char *)NK_FLASH + 0x40, "ECEC", 4) ? "ECEC found" : "MISSING");
    } else {
        u32 sum = 0;
        if (os_lseek(fd, 0) < 0) return fail("seek");
        say("programming %u bytes", size);
        if (program_stream(fd, WIN_LO, size, buf, &sum)) return 1;
    }
    os_close(fd);
    os_free(buf);
    say("done. Start wince.hhk to boot it. Any key.");
    ui_wait_exe_or_clear();
    return 0;
}
