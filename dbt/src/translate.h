/*
 * translate.h - basic-block translator: little-endian SH-4 guest code ->
 * big-endian SH-4 host code. Portable C; the runtime glue (stubs, exception
 * handlers) lives in dbt/rt and is target-only.
 */
#ifndef TRANSLATE_H
#define TRANSLATE_H

#include "sh4dec.h"
#include "sh4emit.h"

#define DBT_MAX_INSNS   64      /* guest instructions per block */
#define DBT_MAX_LINKS   8       /* direct-branch exits per block */
#define DBT_MAX_SPILLS  4       /* spill records per guest instruction */
#define DBT_BLOCK_WORDS 512     /* host buffer per block (1 KiB) */

enum { DBT_OK = 0, DBT_ERR_FETCH, DBT_ERR_READ, DBT_ERR_OVERFLOW, DBT_ERR_INTERNAL };

typedef struct {
    /* guest memory (little-endian). Return 0 on success, nonzero on fault. */
    int (*fetch16)(void *ctx, sf_u32 gpc, sf_u32 *op);
    int (*read16le)(void *ctx, sf_u32 addr, sf_u32 *val);
    int (*read32le)(void *ctx, sf_u32 addr, sf_u32 *val);
    void *ctx;
    /* host addresses of runtime entry points and translator state */
    sf_u32 rt_lookup_link;   /* X0 = guest target, X1 = address of the link literal */
    sf_u32 rt_lookup;        /* X0 = guest target */
    sf_u32 rt_rte;           /* guest rte: SPC is the guest PC */
    sf_u32 rt_fpu;           /* X0 = opcode, X1 = return address */
    sf_u32 rt_fallback;      /* X0 = opcode, X1 = return address */
    sf_u32 rt_ldc_sr;        /* X0 = new SR, X1 = return address */
    sf_u32 shadow_vbr_addr;  /* host word holding the guest VBR */
    sf_u32 shadow_md_addr;   /* host word holding guest SR.MD (0 or 0x40000000) */
    int max_insns;           /* <= DBT_MAX_INSNS; 0 = default */
} dbt_env;

typedef struct {
    sf_u16 gpr, slot;        /* guest register saved in DSP slot */
    sf_u16 start, end;       /* host byte offsets of the lds (start) and sts (end) */
} dbt_spill;

typedef struct {
    sf_u16 host_off;         /* host byte offset where this guest instruction's code starts */
    sf_u16 guest_off;        /* guest byte offset from block start (delay slot = its branch) */
    sf_u16 nspill;
    dbt_spill spill[DBT_MAX_SPILLS];
} dbt_imeta;

typedef struct {
    sf_u32 guest_pc;
    sf_u32 guest_len;        /* bytes of guest code consumed */
    sf_u32 host_base;        /* host address of buf[0] */
    int    host_len;         /* bytes emitted (code + stubs + pool) */
    int    code_len;         /* bytes of translated code before stubs */
    dbt_imeta meta[DBT_MAX_INSNS + 2];
    int    nmeta;            /* entries; the last is a sentinel at guest_len */
    struct { int lit_off; sf_u32 guest_target; } links[DBT_MAX_LINKS];
    int    nlinks;
    int    end_reason;       /* enum below */
} dbt_block;

enum { END_BRANCH, END_TRAP, END_PRIV, END_FULL, END_ILLEGAL, END_FAULT };

/* Translate the block starting at guest_pc into buf (cap words), which will
 * be placed at host address host_base. Returns DBT_OK or an error; on
 * DBT_ERR_FETCH/READ, out->guest_len is the faulting guest address offset
 * and *fault_addr receives the address. */
int dbt_translate_block(const dbt_env *env, sf_u32 guest_pc, sf_u32 host_base,
                        sf_u16 *buf, int cap, dbt_block *out, sf_u32 *fault_addr);

/* Map a host byte offset inside a block to its guest instruction. Returns
 * the meta index or -1. */
int dbt_block_find(const dbt_block *b, sf_u32 host_off);

/* Spills that must be undone (sts slot,gpr) if an exception hit at host_off
 * inside instruction `idx`. Returns count, fills up to DBT_MAX_SPILLS. */
int dbt_block_pending_spills(const dbt_block *b, int idx, sf_u32 host_off, dbt_spill *out);

#endif
