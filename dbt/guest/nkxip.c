/* nkxip.c - synthetic CE-format image executing in place from the emulated
 * NOR (make GUEST=nkxip nkrun): linked at the flash address with
 * guest_xip.ld, wrapped as a .bin by tools/mkbin.py and put back flat into
 * the flash by tools/nkflash.py, so the runtime takes the XIP path (no copy,
 * ROMHDR read from flash, entry = the image start since there is no nk.exe).
 * Checks that the code really runs from flash, that .data reached RAM
 * intact, that .bss is clear and that the ROMHDR is the one in the ROM. */
#include "hyper.h"

extern char bss_start_[] __asm__("__bss_start"), bss_end_[] __asm__("__bss_end");
extern char rom_end_[] __asm__("__rom_end");

struct romhdr {                        /* ROMHDR, CE 6.0 layout */
    unsigned dllfirst, dlllast, physfirst, physlast, nummods;
    unsigned ulRAMStart, ulRAMFree, ulRAMEnd, ulCopyEntries, ulCopyOffset;
    unsigned ulProfileLen, ulProfileOffset, numfiles, ulKernelFlags, ulFSRamPercent;
    unsigned ulDrivglobStart, ulDrivglobLen;
    unsigned short usCPUType, usMiscFlags;
    unsigned pExtensions, ulTrackingStart, ulTrackingLen;
};

#ifndef ROM_BASE
#define ROM_BASE 0x80EE0000u            /* dbt/Makefile passes NK_FLASH */
#endif
#define ROM_MASK 0xFFF00000u

/* A module table with one entry, "kernel.dll", whose single code section
 * lies below its load address (as ROMIMAGE lays out a Release kernel.dll):
 * the loader must make exactly that section device-aware, not
 * load..load+vsize. The runtime reports its range through getstat 9/10. */
struct tocentry { unsigned attrs, tlo, thi, fsize, name, e32, o32, load; };
struct e32_rom { unsigned short objcnt, imageflags; unsigned entryrva, vbase; unsigned short subsysmajor, subsysminor;
                 unsigned stackmax, vsize, sect14rva, sect14size, timestamp; unsigned unit[18]; };
struct o32_rom { unsigned vsize, rva, psize, dataptr, realaddr, flags; };
#define KCODE_LO (ROM_BASE + 0x100)
#define KCODE_HI (ROM_BASE + 0x300)
static const char kname[] = "kernel.dll";
static const struct e32_rom k_e32 = { 1, 0, 0, ROM_BASE + 0x2000, 0, 0, 0, 0x1000, 0, 0, 0, { 0 } };
static const struct o32_rom k_o32[1] = { { KCODE_HI - KCODE_LO, 0x1000, KCODE_HI - KCODE_LO, 0, KCODE_LO, 0x60000020u } };

const struct { struct romhdr h; struct tocentry toc[1]; } nk_romhdr = {
    { 0, 0, ROM_BASE, (unsigned)rom_end_, 1,
      GUEST_RAM_LO, (unsigned)bss_end_, GUEST_RAM_HI, 0, 0,
      0, 0, 0, 0, 0, 0, 0,
      0x01A6, 0,                         /* SH4 */
      0, 0, 0 },
    { { 0, 0, 0, 0, (unsigned)kname, (unsigned)&k_e32, (unsigned)k_o32, ROM_BASE + 0x2000 } }
};

static int fails;
static unsigned bss_probe[64];                     /* must be zero after start */
static unsigned data_probe[4] = { 0x11223344u, 0xDEADBEEFu, 1, 2 };
static const char banner[] = "nkxip: CE-format image executing in place from the NOR\n";

static void check(const char *name, int ok)
{
    hc_puts(ok ? "PASS " : "FAIL "); hc_puts(name); hc_putc('\n');
    if (!ok) fails++;
}

static unsigned here(void) { unsigned pc; __asm__ volatile ("mova 1f, r0\n1:\n mov r0, %0" : "=r"(pc) : : "r0"); return pc; }

void guest_main(void)
{
    const unsigned *marker = (const unsigned *)(ROM_BASE + 0x40);
    const struct romhdr *h;
    unsigned i, nz = 0, pc = here();
    hc_puts(banner);
    check("code runs from the NOR", (pc & ROM_MASK) == (ROM_BASE & ROM_MASK) && pc >= ROM_BASE);
    check(".data copied to RAM intact", data_probe[0] == 0x11223344u && data_probe[1] == 0xDEADBEEFu
          && ((unsigned)data_probe & 0xFF000000u) == 0x8C000000u);
    for (i = 0; i < 64; i++) nz |= bss_probe[i];
    check(".bss is zero", nz == 0);
    check("ECEC marker at image+0x40", marker[0] == 0x43454345u);    /* "ECEC" little-endian */
    h = (const struct romhdr *)marker[1];
    check("ROMHDR pointer", h == &nk_romhdr.h && ((unsigned)h & ROM_MASK) == (ROM_BASE & ROM_MASK));
    check("ROMHDR fields", h->usCPUType == 0x01A6 && h->physfirst == ROM_BASE && h->ulRAMEnd == GUEST_RAM_HI);
    data_probe[2] = 0x55;                                              /* RAM is writable, flash was not touched */
    check("RAM data writable", data_probe[2] == 0x55);
    check("kernel.dll code range from its sections", hc_getstat(9) == KCODE_LO && hc_getstat(10) == KCODE_HI);
    hc_puts("dev range "); hc_puthex(hc_getstat(9)); hc_puts(".."); hc_puthex(hc_getstat(10)); hc_putc('\n');
    hc_puts("physlast "); hc_puthex(h->physlast); hc_puts(" pc "); hc_puthex(pc); hc_putc('\n');
    hc_stats();
    hc_puts(fails ? "RESULT: FAIL\n" : "RESULT: PASS\n");
    hc_exit(fails);
}
