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
#define DBT_BLOCK_WORDS 512     /* host buffer per block (1 KiB) */

enum { DBT_OK = 0, DBT_ERR_FETCH, DBT_ERR_READ, DBT_ERR_OVERFLOW, DBT_ERR_INTERNAL, DBT_ERR_UNSUPPORTED };

typedef struct {
    /* guest memory (little-endian). Return 0 on success, nonzero on fault. */
    int (*fetch16)(void *ctx, sf_u32 gpc, sf_u32 *op);
    int (*read16le)(void *ctx, sf_u32 addr, sf_u32 *val);
    int (*read32le)(void *ctx, sf_u32 addr, sf_u32 *val);
    void *ctx;
    /* host addresses of runtime entry points and translator state */
    sf_u32 rt_lookup_link;   /* X0 = guest target, X1 = address of the link literal */
    sf_u32 rt_lookup;        /* X0 = guest target */
    sf_u32 rt_lookup_ic;     /* X0 = guest target, X1 = address of the site's inline-cache literal pair */
    sf_u32 rt_rte;           /* guest rte: SPC is the guest PC */
    sf_u32 rt_fpu;           /* X0 = opcode, X1 = return address */
    sf_u32 rt_fallback;      /* X0 = opcode, X1 = return address */
    sf_u32 rt_ldc_sr;        /* X0 = new SR, X1 = return address */
    sf_u32 shadow_vbr_addr;  /* host word holding the guest VBR */
    sf_u32 shadow_md_addr;   /* host word holding guest SR.MD (0 or 0x40000000) */
    int max_insns;           /* <= DBT_MAX_INSNS; 0 = default */
} dbt_env;

typedef struct {
    sf_u8  insn;             /* meta index the spill belongs to */
    sf_u8  gpr, slot;        /* guest register saved in DSP slot */
    sf_u8  pad;
    sf_u16 start, end;       /* host byte offsets of the lds (start) and sts (end) */
} dbt_spill;

/* How to compute the guest resume PC for an exception inside this range. */
enum { META_INSN = 0,        /* guest_pc + guest_off (restart the instruction) */
       META_TARGET_CONST,    /* a branch whose slot has executed: resume at target */
       META_TARGET_X0,       /* same, target is in the X0 slot */
       META_TARGET_REG,      /* same, target is guest register fix_reg (not written in the range) */
       META_TARGET_PR,       /* same, target is the guest PR (rts) */
       META_COND,            /* bt/s, bf/s after the slot: SSR.T ? target : fallthrough */
       META_COND_REG,        /* same, but the deciding T bit is bit 0 of r[fix_reg] (slot wrote T) */
       META_RTE };           /* guest rte pending: not resumable */

/* Fix-up the runtime applies when an exception lands after the range's
 * memory operation has executed (host_off + commit_rel < exception offset):
 * the guest instruction is then complete except for the emitted tail, which
 * the runtime performs in software before resuming at the following range. */
enum { FIX_NONE = 0,
       FIX_SWAP16,           /* r[fix_reg] = sign-extended byte-swapped low half (mov.w load) */
       FIX_SWAP32,           /* r[fix_reg] = byte-swapped (mov.l load) */
       FIX_CR_BASE = 0x10 }; /* FIX_CR_BASE + sh_cr: control register = bswap32(r[fix_reg]) */

typedef struct {
    sf_u16 host_off;         /* host byte offset where this range starts */
    sf_u16 guest_off;        /* META_INSN: guest byte offset from block start */
    sf_u8  kind;
    sf_u8  commit_rel;       /* host bytes from host_off to the memory op; 0xFF = none */
    sf_u8  fix_kind;         /* FIX_* */
    sf_u8  fix_reg;
    sf_u32 target;           /* META_TARGET_CONST; META_COND*: resume pc when the T value is 1 */
    sf_u32 fallthrough;      /* META_COND*: resume pc when the T value is 0 */
} dbt_imeta;

#define DBT_MAX_META   (DBT_MAX_INSNS * 2 + 2)
#define DBT_MAX_BLOCK_SPILLS 64

typedef struct {
    sf_u32 guest_pc;
    sf_u32 guest_len;        /* bytes of guest code consumed */
    sf_u32 host_base;        /* host address of buf[0] */
    int    host_len;         /* bytes emitted (code + stubs + pool) */
    int    code_len;         /* bytes of translated code before stubs */
    dbt_imeta meta[DBT_MAX_META];
    int    nmeta;            /* entries; meta[nmeta] is a sentinel at guest_len */
    dbt_spill spills[DBT_MAX_BLOCK_SPILLS];
    int    nspills;
    struct {
        int lit_off;             /* host offset of the link literal (patched by the runtime) */
        int stub_off;            /* host offset of the exit stub */
        int jump_off;            /* host offset of the 4-word jump sequence (lds rT,X0 ...) */
        sf_u8 cond;              /* the word before jump_off is the inverted bt/bf skip */
        sf_u8 backward;          /* guest target is at or before the branch (loop back edge) */
        sf_u8 scratch;           /* register the jump sequence spills */
        sf_u16 w0, w1, skip_w;   /* original words of the sequence (and the skip), to undo chaining */
        sf_u32 guest_target;
    } links[DBT_MAX_LINKS];
    int    nlinks;
    int    ic_off;           /* host offset of the inline cache {guest pc, host} literal pair, or -1 */
    int    end_reason;       /* enum below */
} dbt_block;

enum { END_BRANCH, END_TRAP, END_PRIV, END_FULL, END_ILLEGAL, END_FAULT };

/* Translate the block starting at guest_pc into buf (cap words), which will
 * be placed at host address host_base. Returns DBT_OK or an error; on
 * DBT_ERR_FETCH/READ, out->guest_len is the faulting guest address offset
 * and *fault_addr receives the address. */
int dbt_translate_block(const dbt_env *env, sf_u32 guest_pc, sf_u32 host_base,
                        sf_u16 *buf, int cap, dbt_block *out, sf_u32 *fault_addr);

/* Branch chaining, done by the runtime when it links a direct jump: rewrite
 * the 4-word jump sequence at `code` (host address `host`) into native
 * branches when `target` is in range. Form A (any direct jump within the
 * 12-bit `bra` range): word 0 = `bra target`, word 1 = `nop`. Form B (a
 * conditional back edge whose target is within the 8-bit range): the skip
 * word before the sequence becomes the real `bt/bf target` and word 0 a
 * `bra` over the rest, so a taken loop branch costs one instruction. Returns
 * the number of bytes rewritten (0: out of range) and sets *from to the
 * first rewritten address. The resume metadata stays valid: the rewritten
 * words are either range starts or delay slots. */
int dbt_chain_direct(sf_u16 *code, sf_u32 host, sf_u32 target, int cond, int backward, sf_u32 *from);

/* Map a host byte offset inside a block to its guest instruction. Returns
 * the meta index or -1. */
int dbt_block_find(const dbt_block *b, sf_u32 host_off);

/* Spills that must be undone (gpr <- slot) if an exception hit at host_off:
 * those with start < host_off <= end, in undo order (apply in sequence; an
 * outer spill of the same register comes last and wins). `idx` is unused
 * (ranges and spills are independent). Returns count, fills up to `max`. */
int dbt_block_pending_spills(const dbt_block *b, int idx, sf_u32 host_off, dbt_spill *out, int max);

#endif
