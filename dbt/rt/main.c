/*
 * main.c - translator image entry: find the guest, load it, run it.
 * Memory layout (fx-CG100 / fx-CP400 class, 16 MiB RAM) is defined in
 * dbt/Makefile and passed in as GUEST_RAM_LO/HI, IMAGE_BASE, CACHE_BASE/SIZE:
 *   0x8C100000 .. 0x8CC7FFFF  guest RAM, 11.5 MiB (test guests linked at 0x8C100000)
 *   0x8CC80000 .. 0x8CDFFFFF  this image (text/data/bss) + C stack
 *   0x8CE00000 .. 0x8CFFFFFF  code cache
 * Guest sources, in order of preference:
 *   1. a Windows CE .bin image in flash at NK_FLASH (P1 alias of the NOR;
 *      casio-emu: HLE_OSROM=... HLE_OSROM_PAD=<offset>, tools/nkflash.py
 *      places it), records copied into guest RAM, entry from the terminator
 *      record (a RAMIMAGE build);
 *   2. a flat ROMIMAGE at NK_FLASH: the kernel executes in place from the
 *      NOR, only its RAM range is in the guest window (XIP: nothing copied);
 *   3. the embedded blob, either a .bin (same loader) or a raw little-endian
 *      binary linked at GUEST_LOAD.
 */
#include "rt.h"
#include "nkbin.h"

#ifndef GUEST_RAM_LO
#define GUEST_RAM_LO 0x8C100000u
#define GUEST_RAM_HI 0x8CC80000u
#define CACHE_BASE   0x8CE00000u
#define CACHE_SIZE   0x00200000u
#endif
#define GUEST_LOAD   GUEST_RAM_LO
#ifndef NK_FLASH
#define NK_FLASH     0x81000000u          /* NOR offset 16 MiB (dbt/Makefile NK_FLASH) */
#endif
#ifndef NK_FLASH_MAX
#define NK_FLASH_MAX 0x003E0000u          /* the window on the device: the erased hole in Casio's OS image ends at offset 0x13E0000 (docs/03) */
#endif

extern const unsigned char guest_blob_start[], guest_blob_end[];   /* asm: _guest_blob_* */

static const char *nk_err(int r)
{
    switch (r) {
    case NKBIN_ERR_MAGIC: return "bad magic";
    case NKBIN_ERR_TRUNCATED: return "truncated";
    case NKBIN_ERR_RANGE: return "record outside guest RAM";
    case NKBIN_ERR_CHECKSUM: return "record checksum mismatch";
    case NKBIN_ERR_NOENTRY: return "no entry point";
    default: return "?";
    }
}

static sf_u32 load_nk(const sf_u8 *src, sf_u32 maxlen, const char *where, nkbin_info *out)
{
    nkbin_info info;
    int r = nkbin_load(src, maxlen, GUEST_LOAD, GUEST_RAM_HI, &info);
    dbt_puts("[nk] loading .bin from "); dbt_puts(where); dbt_puts("\n");
    if (r != NKBIN_OK) {
        nkbin_print(&info);
        dbt_puts("[nk] load failed: "); dbt_puts(nk_err(r)); dbt_puts(" at source offset "); dbt_puthex(info.fail_off);
        dbt_puts("\n[nk] bytes there:");
        { sf_u32 i; for (i = 0; i < 16; i++) { dbt_puts(" "); dbt_puthex(src[info.fail_off + i]); } }
        dbt_puts("\n");
        dbt_halt("image load failed");
    }
    nkbin_print(&info);
    if (info.romhdr) {
        if (info.cpu_type != 0x01A6) dbt_puts("[nk] WARNING: ROMHDR CPU type is not SH4 (0x01A6)\n");
        if (info.ram_end > GUEST_RAM_HI || info.ram_start < GUEST_LOAD)
            dbt_puts("[nk] WARNING: ROMHDR RAM range leaves the guest window; the kernel would overwrite the translator\n");
    }
    dbt_sync_code(info.start, info.length);
    *out = info;
    return info.entry;
}

void dbt_main(void)
{
    dbt_rt_config cfg;
    nkbin_info info;
    sf_u32 n = (sf_u32)(guest_blob_end - guest_blob_start);
    sf_u32 host, entry;
    memset(&info, 0, sizeof info);

    dbt_puts("[dbt] translator image up, guest blob "); dbt_puthex(n); dbt_puts(" bytes\n");

    if (nkbin_is_image((const sf_u8 *)NK_FLASH)) {
        entry = load_nk((const sf_u8 *)NK_FLASH, NK_FLASH_MAX, "flash", &info);
    } else if (nkbin_is_xip(NK_FLASH, NK_FLASH, NK_FLASH + NK_FLASH_MAX)) {
        int r = nkbin_xip(NK_FLASH, NK_FLASH, NK_FLASH + NK_FLASH_MAX, &info);
        if (r != NKBIN_OK) dbt_halt("XIP image: ROMHDR unreadable");
        nkbin_print(&info);
        if (info.cpu_type != 0x01A6) dbt_puts("[nk] WARNING: ROMHDR CPU type is not SH4 (0x01A6)\n");
        if (info.ram_end > GUEST_RAM_HI || info.ram_start < GUEST_LOAD)
            dbt_puts("[nk] WARNING: ROMHDR RAM range leaves the guest window; the kernel would overwrite the translator\n");
        if (info.physlast > NK_FLASH + NK_FLASH_MAX)
            dbt_puts("[nk] WARNING: the ROM image runs past the readable flash span\n");
        entry = info.entry;
    } else if (n >= 7 && nkbin_is_image(guest_blob_start)) {
        entry = load_nk(guest_blob_start, n, "embedded blob", &info);
    } else {
        memcpy((void *)GUEST_LOAD, guest_blob_start, n);
        dbt_sync_code(GUEST_LOAD, n);             /* guest code is data to us, but keep caches coherent */
        entry = GUEST_LOAD;
    }

    cfg.cache_base = CACHE_BASE; cfg.cache_size = CACHE_SIZE;
    cfg.guest_ram_lo = GUEST_LOAD; cfg.guest_ram_hi = GUEST_RAM_HI;
    cfg.dev_lo = info.kernel_lo; cfg.dev_hi = info.kernel_hi;      /* kernel.dll reaches the CCN registers directly */
    cfg.rom_lo = info.physfirst & 0x1FFFFFFFu; cfg.rom_hi = info.physlast & 0x1FFFFFFFu;
#ifdef DBT_NO_ROMKEEP
    cfg.rom_lo = cfg.rom_hi = 0;         /* experiments: whole-cache flushes drop the ROM blocks too */
#endif   /* XIP code in it never changes */
    dbt_rt_init(&cfg);
    dbt_shadow_vbr = 0;
    dbt_shadow_md = SR_MD;

    host = dbt_resolve(entry, 0);
    dbt_puts("[dbt] entering guest at "); dbt_puthex(entry); dbt_puts(" -> "); dbt_puthex(host); dbt_puts("\n");
    dbt_enter(host);
}
