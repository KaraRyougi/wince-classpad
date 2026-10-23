/* nkimg.c - synthetic CE-format image: carries a ROMHDR like ROMIMAGE output
 * and checks, once loaded through the .bin loader, that its data arrived,
 * that the ROMHDR is reachable through the ECEC marker, and that .bss is
 * clear. Built into a .bin by tools/mkbin.py (make GUEST=nkimg nkrun). */
#include "hyper.h"

extern char bss_start_[] __asm__("__bss_start"), bss_end_[] __asm__("__bss_end");   /* C names get a leading underscore */

struct romhdr {                        /* ROMHDR, CE 6.0 layout */
    unsigned dllfirst, dlllast, physfirst, physlast, nummods;
    unsigned ulRAMStart, ulRAMFree, ulRAMEnd, ulCopyEntries, ulCopyOffset;
    unsigned ulProfileLen, ulProfileOffset, numfiles, ulKernelFlags, ulFSRamPercent;
    unsigned ulDrivglobStart, ulDrivglobLen;
    unsigned short usCPUType, usMiscFlags;
    unsigned pExtensions, ulTrackingStart, ulTrackingLen;
};

const struct romhdr nk_romhdr = {
    0, 0, 0x8C100000u, (unsigned)bss_start_, 0,
    (unsigned)bss_end_, (unsigned)bss_end_, 0x8CB00000u, 0, 0,
    0, 0, 0, 0, 0, 0, 0,
    0x01A6, 0,                         /* SH4 */
    0, 0, 0
};

static int fails;
static unsigned bss_probe[64];                     /* must be zero after load */
static unsigned data_probe[4] = { 0x11223344u, 0xDEADBEEFu, 1, 2 };
static const char banner[] = "nkimg: CE-format image loaded by the translator\n";

static void check(const char *name, int ok)
{
    hc_puts(ok ? "PASS " : "FAIL "); hc_puts(name); hc_putc('\n');
    if (!ok) fails++;
}

void guest_main(void)
{
    const unsigned *marker = (const unsigned *)0x8C100040u;
    const struct romhdr *h;
    unsigned i, nz = 0;
    hc_puts(banner);
    check(".data arrived intact", data_probe[0] == 0x11223344u && data_probe[1] == 0xDEADBEEFu);
    for (i = 0; i < 64; i++) nz |= bss_probe[i];
    check(".bss is zero", nz == 0);
    check("ECEC marker at image+0x40", marker[0] == 0x43454345u);    /* "ECEC" little-endian */
    h = (const struct romhdr *)marker[1];
    check("ROMHDR pointer", h == &nk_romhdr);
    check("ROMHDR fields", h->usCPUType == 0x01A6 && h->physfirst == 0x8C100000u && h->ulRAMEnd == 0x8CB00000u);
    hc_puts("physlast "); hc_puthex(h->physlast); hc_puts(" ramstart "); hc_puthex(h->ulRAMStart); hc_putc('\n');
    hc_stats();
    hc_puts(fails ? "RESULT: FAIL\n" : "RESULT: PASS\n");
    hc_exit(fails);
}
