/*
 * main.c - translator image entry: load the embedded guest, run it.
 * Memory layout (fx-CG100 / fx-CP400 class, 16 MiB RAM):
 *   0x8C100000 .. 0x8CAFFFFF  guest RAM (guest image linked at 0x8C100000)
 *   0x8CB00000 .. 0x8CDEFFFF  this image (text/data/bss) + stacks
 *   0x8CE00000 .. 0x8CFFFFFF  code cache
 */
#include "rt.h"

#define GUEST_LOAD   0x8C100000u
#define GUEST_ENTRY  0x8C100000u
#define CACHE_BASE   0x8CE00000u
#define CACHE_SIZE   0x00200000u

extern const unsigned char guest_blob_start[], guest_blob_end[];   /* asm: _guest_blob_* */

void dbt_main(void)
{
    dbt_rt_config cfg;
    sf_u32 n = (sf_u32)(guest_blob_end - guest_blob_start);
    sf_u32 host;

    dbt_puts("[dbt] translator image up, guest blob "); dbt_puthex(n); dbt_puts(" bytes\n");
    memcpy((void *)GUEST_LOAD, guest_blob_start, n);
    dbt_sync_code(GUEST_LOAD, n);                 /* guest code is data to us, but keep caches coherent */

    cfg.cache_base = CACHE_BASE; cfg.cache_size = CACHE_SIZE;
    cfg.guest_ram_lo = GUEST_LOAD; cfg.guest_ram_hi = 0x8CB00000u;
    dbt_rt_init(&cfg);
    dbt_shadow_vbr = 0;
    dbt_shadow_md = SR_MD;

    host = dbt_resolve(GUEST_ENTRY, 0);
    dbt_puts("[dbt] entering guest at "); dbt_puthex(GUEST_ENTRY); dbt_puts(" -> "); dbt_puthex(host); dbt_puts("\n");
    dbt_enter(host);
}
