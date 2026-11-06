/* launcher.c - the Windows CE launcher (wince.hhk): started by HollyHock from
 * Casio's OS, it brings the NOR window up to date from \fls0\NK.bin when
 * that file is there and differs from the flash (hhk/nkflash.c), then hands
 * the fx-CP400 to the translator image that the ELF carries at IMAGE_BASE
 * (payload.S, placed there by the launcher's own copy), which finds the CE
 * image in the NOR at NK_FLASH (docs/05).
 *
 * With OS routines available (os.c) it shows what it found and waits for
 * one EXE before touching the flash or the machine; without them it boots
 * what is in the flash at once. The payload is checked against the sum the
 * build recorded (PAYLOAD_SUM/PAYLOAD_SIZE from <build>/...-payload.h) so a
 * truncated copy on the calculator never runs.
 * HHK_EMU: the same stub as a casio-emu .g3a; the payload sits behind the
 * stub in the add-in window and hhk_takeover copies it (dbt/loader/loader.S);
 * no OS, no flashing. */
#include "util.h"
#ifndef HHK_EMU
#include "os.h"
#include "nkflash.h"
#endif

extern const u8 payload_start[], payload_end[];
extern void hhk_takeover(const void *src, void *dst, unsigned len, unsigned entry) __attribute__((noreturn));

#ifndef IMAGE_BASE
#error IMAGE_BASE (the translator link address) must come from the Makefile
#endif
#ifndef NK_FLASH
#define NK_FLASH 0x81000000u
#endif
#ifndef NK_FLASH_MAX
#define NK_FLASH_MAX 0x003E0000u
#endif
#ifndef GUEST_RAM_HI
#error GUEST_RAM_HI must come from the Makefile
#endif

#ifndef HHK_EMU
/* ROMHDR of an XIP image: pointer at image+0x44 (after "ECEC"), little-endian
 * fields dllfirst, dlllast, physfirst, physlast, nummods, ulRAMStart,
 * ulRAMFree, ulRAMEnd ... (tools/nkbin.py). 0 when there is no XIP image. */
static u32 ce_ram_end(void)
{
    const u8 *p = (const u8 *)NK_FLASH;
    u32 hdr;
    if (memcmp(p + 0x40, "ECEC", 4)) return 0;
    hdr = le32(p + 0x44);
    if (hdr < NK_FLASH || hdr > NK_FLASH + NK_FLASH_MAX - 0x60) return 0;
    return le32((const u8 *)hdr + 7 * 4);
}
#endif

static const char *ce_image_kind(void)
{
    const char *p = (const char *)NK_FLASH;
    if (!memcmp(p + 0x40, "ECEC", 4)) return "XIP image (ROMHDR) in flash";
    if (!memcmp(p, "B000FF\n", 7)) return "RAM image (.bin records) in flash";
    return "none: the embedded guest will run";
}

#ifndef HHK_EMU
static int stop(const char *msg)
{
    nk_say("%s", msg);
    nk_say("Clear or EXE: back to HollyHock");
    ui_wait_exe_or_clear();
    return 1;
}
#endif

int hhk_main(int argc, char **argv, char **envp)
{
    u32 len = (u32)(payload_end - payload_start);
    u32 sum = bytesum(payload_start, len);
    int ok = (len == PAYLOAD_SIZE && sum == PAYLOAD_SUM);
#ifdef HHK_EMU
    (void)argc; (void)argv; (void)envp;
    emu_printf("[hhk] payload %u bytes at %08x, sum %08x %s\n", len, (u32)payload_start, sum, ok ? "ok" : "MISMATCH");
    emu_printf("[hhk] CE image at %08x: %s\n", (u32)NK_FLASH, ce_image_kind());
    if (!ok) { emu_puts("[hhk] refusing to boot a damaged payload\n"); return 1; }
    emu_puts("[hhk] taking over\n");
#else
    os_init(argc, argv, envp);
    if (os_ok) {
        int file, same;
        ui_clear(); nk_row = 1;
        nk_say("Windows CE launcher (wince-classpad)");
        nk_say("OS routines: %s", os_source);
        nk_say("translator %u bytes, sum %08x %s", len, sum, ok ? "ok" : "MISMATCH");
        if (!ok) return stop("Copy wince.hhk to the calculator again.");
        nk_say("window %08x %s", (u32)NK_FLASH, nkflash_window_state());
        file = nkflash_open();
        if (file == NKF_BAD) return stop("NK.bin rejected: nothing written.");
        same = 0;
        if (file == NKF_READY) {
            nk_say("comparing NK.bin with the flash...");
            same = nkflash_compare();
            if (same < 0) { nkflash_close(); return stop("NK.bin unreadable."); }
            nk_say(same ? "NK.bin is already in the flash" : "NK.bin differs: will erase and program");
        } else {
            nk_say("no \\fls0\\NK.bin: booting what is there");
        }
        nk_say("CE image: %s", ce_image_kind());
        if (ce_ram_end() > GUEST_RAM_HI) {
            nk_say("its RAM ends at %08x, the translator", ce_ram_end());
            nk_say("starts at %08x (config.bib RAM line)", (u32)GUEST_RAM_HI);
            nkflash_close();
            return stop("Rebuild the image with the current config.bib.");
        }
        if (file == NKF_NOFILE && !memcmp((const char *)NK_FLASH, "\xff\xff\xff\xff", 4) && memcmp((const char *)NK_FLASH + 0x40, "ECEC", 4))
            nk_say("(the embedded test guest would run)");
        nk_say("EXE: %s and boot CE   Clear: cancel", (file == NKF_READY && !same) ? "flash NK.bin" : "go");
        if (!ui_wait_exe_or_clear()) { nkflash_close(); return 0; }
        if (file == NKF_READY && !same) {
            if (nkflash_write()) { nkflash_close(); return stop("Flash failed: not booting."); }
        }
        nkflash_close();
        nk_say("starting the translator (the screen stays");
        nk_say(" until the CE display driver runs)");
    } else if (!ok) {
        return 1;                       /* no way to say so: do nothing */
    }
#endif
    hhk_takeover(payload_start, (void *)IMAGE_BASE, len, IMAGE_BASE);
}
