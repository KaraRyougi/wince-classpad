/* launcher.c - the casio-emu variant of the Windows CE loader (HHK_EMU):
 * the stub of a raw .g3a payload that casio-emu starts at 0x00300000, with
 * no OS and no flashing. The payload sits behind the stub in the add-in
 * window and hhk_takeover copies it to IMAGE_BASE (dbt/loader/loader.S
 * does the same for the plain .g3a). The payload is checked against the
 * sum the build recorded (PAYLOAD_SUM/PAYLOAD_SIZE from
 * <build>/...-payload.h). The calculator's loader, a gint program with the
 * screens and the flasher, is hhk/loader.c. */
#include "util.h"

extern const u8 payload_start[], payload_end[];
extern void hhk_takeover(const void *src, void *dst, unsigned len, unsigned entry) __attribute__((noreturn));

#ifndef IMAGE_BASE
#error IMAGE_BASE (the translator link address) must come from the Makefile
#endif
#ifndef NK_FLASH
#define NK_FLASH 0x80EE0000u
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
    (void)argc; (void)argv; (void)envp;
    emu_printf("[hhk] payload %u bytes at %08x, sum %08x %s\n", len, (u32)payload_start, sum, ok ? "ok" : "MISMATCH");
    emu_printf("[hhk] CE image at %08x: %s\n", (u32)NK_FLASH, ce_image_kind());
    if (!ok) { emu_puts("[hhk] refusing to boot a damaged payload\n"); return 1; }
    emu_puts("[hhk] taking over\n");
    hhk_takeover(payload_start, (void *)IMAGE_BASE, len, IMAGE_BASE);
}
