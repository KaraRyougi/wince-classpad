/*
 * rt.h - translator runtime: block cache, lookup, exception delivery,
 * hypercalls, FPU/fallback helpers. The C half; dbt/rt/stubs.S is the
 * assembly half and defines the register conventions documented here.
 *
 * Conventions shared with stubs.S:
 *   X0 = guest target PC on entry to dbt_lookup / dbt_lookup_link
 *   X1 = address of the link literal (dbt_lookup_link) or return address (helpers)
 *   Y0 / Y1 / A0 = free for the stubs' own spills
 */
#ifndef DBT_RT_H
#define DBT_RT_H

#include "../src/translate.h"
#include "../../fpuemu/src/fpuemu.h"

#define SR_MD   0x40000000u
#define SR_RB   0x20000000u
#define SR_BL   0x10000000u
#define SR_DSP  0x00001000u
#define SR_IMASK 0x000000F0u

/* Guest register context saved by the stubs when calling C. Layout is fixed
 * (see ctx_offsets.h); the stubs store it with descending @-Rn stores. */
typedef struct {
    sf_u32 sr, pr, gbr, mach, macl;
    sf_u32 r[16];              /* bank-0 view of r0-r7, r8-r15 */
    sf_u32 slots[SLOT_COUNT];  /* A0 X0 X1 Y0 Y1 as saved by the exception path */
    /* exception path only */
    sf_u32 host_spc, ssr, expevt, tra, tea, intevt;
    sf_u32 vec_off;            /* 0x100, 0x400 or 0x600 */
    /* results for the asm side */
    sf_u32 jump_to;            /* host address to jump to (RT_ACT_JUMP) */
    sf_u32 new_spc, new_ssr;   /* for RT_ACT_JUMP from the exception path */
    /* exception path: r0-r7 of the bank the guest was NOT running in (bank 1
     * for a bank-0 guest). Saved first thing by the vector thunks, restored
     * last: the CE kernel keeps pCurThread and SH3CTL_BASE in r4/r5/r7_bank1
     * across all code, so the translator may not use bank 1 as scratch. */
    sf_u32 b1[8];
    /* a guest `ldc rm, sr` that switches register banks returns through rte
     * into dbt_bank_tramp, which reloads the guest's SPC/SSR from here (the
     * kernel's interrupt entry saves them after its bank switch) and jumps
     * to sw_target */
    sf_u32 sw_spc, sw_ssr, sw_target;
} dbt_gctx;

enum { RT_ACT_RESUME = 0, RT_ACT_JUMP = 1, RT_ACT_HALT = 2 };

/* Runtime configuration: memory the translator may use (P1 addresses). */
typedef struct {
    sf_u32 cache_base, cache_size;     /* code cache */
    sf_u32 guest_ram_lo, guest_ram_hi; /* what the guest may use (for sanity checks only) */
    sf_u32 dev_lo, dev_hi;             /* device-aware translation range (kernel.dll), 0,0 = none */
} dbt_rt_config;

void   dbt_rt_init(const dbt_rt_config *cfg);
/* Find or translate the block at guest pc; returns its host address. If
 * link_lit is nonzero the literal at that address is patched to the result. */
sf_u32 dbt_resolve(sf_u32 gpc, sf_u32 link_lit);
sf_u32 dbt_resolve_ic(sf_u32 gpc, sf_u32 ic_addr);     /* inline-cache miss: refill {gpc, host} at ic_addr */
/* Exception from translated code; fills ctx->jump_to/new_spc/new_ssr. */
int    dbt_exc_c(dbt_gctx *ctx);
/* Helpers called from the stubs with the guest context: FPU instruction,
 * mac fallback, and ldc rm,sr (writes ctx->sr with the SR to really load). */
void   dbt_fpu_c(dbt_gctx *ctx, sf_u32 opcode);
void   dbt_fallback_c(dbt_gctx *ctx, sf_u32 opcode);
int    dbt_ldc_sr_c(dbt_gctx *ctx, sf_u32 newsr);   /* returns 1 when the new SR switches register banks */
/* Guest rte: returns host address for the guest SPC; updates *ssr for the real rte. */
sf_u32 dbt_rte_c(sf_u32 guest_spc, sf_u32 *ssr);
/* Drop every block overlapping [lo, hi) of guest code (after code writes). */
void   dbt_invalidate(sf_u32 lo, sf_u32 hi);
/* Print what the guest appended to the debug ring (docs/04). */
void   dbt_dbgring_drain(void);

/* translator state visible to generated code */
sf_u32 dbt_bench_native(sf_u32 which, sf_u32 n);      /* rt/bench.c */
extern volatile sf_u32 dbt_shadow_vbr;
extern volatile sf_u32 dbt_shadow_md;

/* provided by stubs.S */
extern char dbt_vectors[];                          /* real VBR value */
#include "ctx_offsets.h"
void dbt_enter(sf_u32 host_entry) __attribute__((noreturn));
void dbt_sync_code(sf_u32 addr, sf_u32 len);        /* ocbwb + icbi over the range */
extern char dbt_lookup[], dbt_lookup_link[], dbt_lookup_ic[], dbt_rte_stub[], dbt_fpu_stub[], dbt_fallback_stub[], dbt_ldc_sr_stub[];
/* window labels for exception recovery inside the stubs (see stubs.S) */
extern char dbt_lk_s1[], dbt_lk_s2[], dbt_lk_s3[], dbt_lkl_s1[], dbt_lkl_s2[], dbt_lkl_s3[], dbt_lki_s1[], dbt_lki_s2[], dbt_lki_s3[], dbt_lk_region_end[];
extern char dbt_hit_a[], dbt_hit_b[], dbt_hit_c[], dbt_hit_end[];
extern char dbt_full_m1[], dbt_full_m2[], dbt_full_m3[], dbt_full_m4[], dbt_full_end[];
extern char dbt_bank_tramp[], dbt_bt_s1[], dbt_bt_end[];
extern char dbt_rte_tramp[], dbt_rt_s1[], dbt_rt_s2[], dbt_rt_s3[], dbt_rt_masked[], dbt_rte_tramp_end[];
extern char dbt_hfpu_s1[], dbt_hfpu_s2[], dbt_hfpu_s3[], dbt_hfpu_masked[];
extern char dbt_hfb_s1[], dbt_hfb_s2[], dbt_hfb_s3[], dbt_hfb_masked[];
extern char dbt_hsr_s1[], dbt_hsr_s2[], dbt_hsr_s3[], dbt_hsr_masked[];
extern char dbt_stubs_start[], dbt_stubs_end[];
extern dbt_gctx dbt_ctx_rt;      /* used by lookup-miss and helper stubs */
extern dbt_gctx dbt_ctx_exc;     /* used by the exception handlers */

void *memcpy(void *d, const void *s, __SIZE_TYPE__ n);
void *memset(void *d, int c, __SIZE_TYPE__ n);

/* debug console (emulator debug port now, SCIF later) */
void dbt_putc(int c);
void dbt_puts(const char *s);
void dbt_puthex(sf_u32 v);
void dbt_halt(const char *why) __attribute__((noreturn));

/* statistics */
typedef struct {
    sf_u32 blocks, translations, flushes, lookups_c, exceptions, hypercalls, fpu_ops, invalidations;
    sf_u32 resumes, resumes_committed;
    sf_u32 chained;                      /* direct jumps rewritten into native branches */
    sf_u32 hash_evicted;                 /* lookups that found the block only in the block table */
    sf_u32 ic_misses_c;                  /* inline-cache misses that also missed the asm hash probe */
    sf_u32 probe_faults;                 /* code-probe faults delivered to the guest (TLB misses, address errors) */
    sf_u32 unchained, asid_switches, tlb_hooks;
    sf_u32 arena_bytes;                  /* metadata arena in use */
    sf_u32 synth_exceptions;             /* address errors synthesized for unfetchable guest pcs */   /* exceptions resumed inside a block: restart / after the memory op */
    sf_u32 dbgring_bytes;          /* bytes printed from the guest debug ring */
    sf_u32 dev_accesses;           /* device-aware 32-bit accesses taken through the fallback helper */
} dbt_stats;
extern dbt_stats dbt_stat;

#endif
