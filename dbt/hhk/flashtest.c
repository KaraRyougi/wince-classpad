/* flashtest.c - casio-emu check of norflash.c against the emulator's NOR
 * model (HLE_FLASH_CMD=1 enables its AMD command decoder): erase the first
 * window sector, program a pattern across 64-byte lines and a sector
 * boundary, read it back, and show that programming never touches bytes
 * outside the range. Prints FLASHTEST PASS or FAIL on the debug port. */
#include "util.h"
#include "norflash.h"

#ifndef NK_FLASH
#define NK_FLASH 0x81000000u
#endif
#define WIN (NK_FLASH - NOR_P1)

static int fails;
static void check(const char *what, int ok)
{
    emu_printf("[flashtest] %s: %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

int hhk_main(int argc, char **argv, char **envp)
{
    static u8 pat[0x3000];
    u32 i, off;
    const volatile u8 *p1 = (const volatile u8 *)NK_FLASH;
    (void)argc; (void)argv; (void)envp;
    for (i = 0; i < sizeof pat; i++) pat[i] = (u8)(i * 7 + (i >> 8));

    check("window blank", nor_range_erased(WIN, 2 * NOR_SECTOR));
    check("erase sector 0", nor_erase_sector(WIN) == 0);
    check("erase sector 1", nor_erase_sector(WIN + NOR_SECTOR) == 0);
    /* a) odd line offset, 3 lines */
    check("program 0x10+0xC0", nor_program(WIN + 0x10, pat, 0xC0) == 0);
    check("readback a", !memcmp((const void *)(NK_FLASH + 0x10), pat, 0xC0));
    check("untouched before a", p1[0x0E] == 0xFF && p1[0x0F] == 0xFF);
    check("untouched after a", p1[0xD0] == 0xFF && p1[0xD1] == 0xFF);
    /* b) across the sector boundary */
    off = WIN + NOR_SECTOR - 0x1000;
    check("program across sector", nor_program(off, pat, sizeof pat) == 0);
    check("readback b", !memcmp((const void *)(NOR_P1 + off), pat, sizeof pat));
    /* c) programming over data is refused by the verify (bits only clear) */
    check("reprogram refused", nor_program(WIN + 0x10, pat + 1, 0x40) != 0);
    /* d) erase clears it all again */
    check("erase again", nor_erase_sector(WIN) == 0 && nor_erase_sector(WIN + NOR_SECTOR) == 0);
    check("window blank again", nor_range_erased(WIN, 2 * NOR_SECTOR));
    emu_printf("FLASHTEST %s\n", fails ? "FAIL" : "PASS");
    return fails;
}
