/* launcher.c - the Windows CE launcher (wince.hhk): started by HollyHock from
 * Casio's OS, it hands the fx-CP400 to the translator image that the ELF
 * carries at IMAGE_BASE (payload.S, placed there by the launcher's own copy)
 * and lets the translator find the CE image in the NOR at NK_FLASH (docs/05).
 *
 * With OS routines available (os.c) it shows what it found and waits for EXE;
 * without them it boots at once. The payload is checked against the sum the
 * build recorded (PAYLOAD_SUM/PAYLOAD_SIZE from <build>/...-payload.h) so a
 * truncated copy on the calculator never runs.
 * HHK_EMU: the same stub as a casio-emu .g3a; the payload sits behind the
 * stub in the add-in window and hhk_takeover copies it (dbt/loader/loader.S). */
#include "util.h"
#ifndef HHK_EMU
#include "os.h"
#endif

extern const u8 payload_start[], payload_end[];
extern void hhk_takeover(const void *src, void *dst, unsigned len, unsigned entry) __attribute__((noreturn));

#ifndef IMAGE_BASE
#error IMAGE_BASE (the translator link address) must come from the Makefile
#endif
#ifndef NK_FLASH
#define NK_FLASH 0x81000000u
#endif

static const char *ce_image_kind(void)
{
    const char *p = (const char *)NK_FLASH;
    if (!memcmp(p + 0x40, "ECEC", 4)) return "XIP image (ROMHDR) in flash";
    if (!memcmp(p, "B000FF\n", 7)) return "RAM image (.bin records) in flash";
    return "none: the embedded guest will run";
}

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
        ui_clear();
        ui_printf(1, "Windows CE launcher (wince-classpad)");
        ui_printf(2, "OS routines: %s", os_source);
        ui_printf(4, "translator: %u bytes at %08x", len, (u32)payload_start);
        ui_printf(5, "payload sum %08x: %s", sum, ok ? "ok" : "MISMATCH");
        ui_printf(7, "CE image at %08x:", (u32)NK_FLASH);
        ui_printf(8, "  %s", ce_image_kind());
        if (ok) {
            ui_printf(11, "EXE: leave Casio's OS and boot CE");
            ui_printf(12, "Clear: back to HollyHock");
            ui_printf(14, "(the screen stays until the CE display");
            ui_printf(15, " driver starts; reset to return)");
        } else {
            ui_printf(11, "Copy wince.hhk to the calculator again.");
            ui_printf(12, "Clear or EXE: back to HollyHock");
        }
        ui_refresh();
        if (!ui_wait_exe_or_clear() || !ok) return ok ? 0 : 1;
    } else if (!ok) {
        return 1;                       /* no way to say so: do nothing */
    }
#endif
    hhk_takeover(payload_start, (void *)IMAGE_BASE, len, IMAGE_BASE);
}
