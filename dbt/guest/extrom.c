/* extrom.c - the EXT region seen from a translated guest (docs/03): the test
 * region of guest/extrom.txt is put at CLASSPAD_EXT_ROM_PA in the emulated
 * NOR (make GUEST=extrom run), found with the OAL's own check
 * (classpad_extrom.h) through little-endian loads of the big-endian NOR, and
 * its module and files read back the way the kernel's loader and ROM file
 * code would. */
#include "hyper.h"
#include "../../bsp/classpad50/SRC/INC/classpad_extrom.h"

static const char expect[] = "wince-classpad EXT region test\n";
static int fails;

static void check(const char *name, int ok)
{
    hc_puts(ok ? "PASS " : "FAIL "); hc_puts(name); hc_putc('\n');
    if (!ok) fails++;
}

static int same(const char *a, const char *b, unsigned n)
{
    while (n--) if (*a++ != *b++) return 0;
    return 1;
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) a++, b++;
    return *a == *b;
}

void guest_main(void)
{
    unsigned toc = ClasspadExtRomFind((const void *)CLASSPAD_EXT_ROM_PA, CLASSPAD_EXT_ROM_PA, CLASSPAD_EXT_ROM_BYTES);
    const unsigned *h, *f;
    check("EXT region found at CLASSPAD_EXT_ROM_PA", toc != 0);
    if (!toc) hc_exit(1);
    h = (const unsigned *)toc;
    check("ROMHDR: one module, two files, physfirst = the region base", h[4] == 1 && h[12] == 2 && h[2] == CLASSPAD_EXT_ROM_PA);
    f = h + CLASSPAD_EXTROM_ROMHDR_BYTES / 4;
    /* TOCentry: the module's name, e32 (one object), o32 over its bytes */
    check("module extrom.dll with one section", streq((const char *)f[4], "extrom.dll")
          && (*(const unsigned short *)f[5]) == 1 && ((const unsigned *)f[6])[2] == sizeof expect - 1);
    check("the module's section holds the test text", same((const char *)((const unsigned *)f[6])[3], expect, sizeof expect - 1));
    f += CLASSPAD_EXTROM_TOC_BYTES / 4;
    check("file 0 is extrom-data.txt, read-only", streq((const char *)f[5], "extrom-data.txt") && f[0] == 1);
    check("file 0 holds the test text", f[3] == sizeof expect - 1 && same((const char *)f[6], expect, sizeof expect - 1));
    f += CLASSPAD_EXTROM_FILE_BYTES / 4;
    check("file 1 keeps its spaces, system+hidden+read-only", streq((const char *)f[5], "Name With Spaces.lnk") && f[0] == 7);
    check("file 1 holds the same text", f[3] == sizeof expect - 1 && same((const char *)f[6], expect, sizeof expect - 1));
    check("erased flash is not a region", !ClasspadExtRomFind((const void *)0x81300000u, 0x81300000u, 0x10000u));
    /* the same bytes claimed at another base: its pointers fall outside */
    check("a region is only valid at its own base", !ClasspadExtRomFind((const void *)CLASSPAD_EXT_ROM_PA, CLASSPAD_EXT_ROM_PA + 0x20000u, 0x10000u));
    if (!fails) hc_puts("RESULT: PASS\n");
    hc_exit(fails ? 1 : 0);
}
