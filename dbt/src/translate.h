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
    sf_u32 rt_fpu_arith;     /* lightweight stub for pure FPU arith (1 op or 2 packed ops in X0); 0 = use rt_fpu */
    sf_u32 fpu_fr, fpu_fpscr, fpu_fpul;  /* fpuemu's register file (fr[32]: bank 1 at +64 bytes), FPSCR and FPUL, for the in-line FPU moves (tr_fpu_inline); 0 = none */
    sf_u32 rt_fallback;      /* X0 = opcode, X1 = return address */
    sf_u32 rt_ldc_sr;        /* X0 = new SR, X1 = return address */
    sf_u32 shadow_vbr_addr;  /* host word holding the guest VBR */
    sf_u32 shadow_md_addr;   /* host word holding guest SR.MD (0 or 0x40000000) */
    int max_insns;           /* <= DBT_MAX_INSNS; 0 = default */
    /* Blocks whose guest pc (P1/P2 folded) lies in [dev_lo, dev_hi) are
     * translated device-aware: every 32-bit load/store tests its effective
     * address for P4 (top byte 0xFF, the on-chip control registers) at run
     * time and performs such accesses unswapped through rt_fallback. For
     * code that reaches device registers directly, i.e. the CE kernel.dll;
     * our own code uses pre-swapped accessors instead. 0,0 = off. */
    sf_u32 dev_lo, dev_hi;
    /* Optional: only these [lo, hi) pairs (P1/P2 folded, sorted) inside
     * [dev_lo, dev_hi) are device-aware; NULL = the whole range. */
    const sf_u32 *dev_ranges;
    unsigned ndev_ranges;
    /* Execution trace (-DDBT_TRACE_EXEC): every block starts by appending
     * its guest pc to a 64 KiB ring. exec_ctl = address of the byte index
     * (16-bit wrap), exec_ring = the ring. 0 = off. */
    sf_u32 exec_ctl, exec_ring;
    /* 1: never borrow a register (see dbt_block.borrow); for tests */
#ifdef DBT_SAME_ASID_PTEH_STORE
    sf_u32 current_asid_addr; /* native runtime word; never a guest address */
#endif
    int no_borrow;
    /* 1: skip populating out->meta[] and out->spills[] (for DBT_LAZY_META
     * initial translation; emits bit-identical host code so on-demand rebuild
     * with no_meta = 0 reproduces exact offsets). */
    int no_meta;
    /* Optional relocation output (moving a block, rt.c DBT_SECOND_CHANCE):
     * the pool's byte offset, and one bit per 4-byte pool word that holds
     * an address inside the block. NULL = not wanted. */
    sf_u8 *reloc_map;        /* DBT_RELOC_BYTES */
    sf_u16 *reloc_pool;
} dbt_env;
#define DBT_RELOC_BYTES ((EMIT_MAX_LITS + 7) / 8)

enum { DBT_SPILL_T = 16 }; /* saved SR.T, not a GPR index */

typedef struct {
#ifndef DBT_COMPACT_SPILL6
    sf_u8  insn;             /* legacy diagnostic field; not used for recovery */
#endif
    sf_u8  gpr, slot;        /* guest GPR or DBT_SPILL_T saved in DSP slot */
#ifndef DBT_COMPACT_SPILL6
    sf_u8  pad;
#endif
    sf_u16 start, end;       /* host byte offsets of the lds (start) and sts (end) */
} dbt_spill;
#ifdef DBT_COMPACT_SPILL6
typedef char check_compact_spill[(sizeof(dbt_spill) == 6) ? 1 : -1];
#endif

/* How to compute the guest resume PC for an exception inside this range. */
enum { META_INSN = 0,        /* guest_pc + guest_off (restart the instruction) */
       META_TARGET_CONST,    /* a branch whose slot has executed: resume at target */
       META_TARGET_X0,       /* same, target is in the X0 slot */
       META_TARGET_REG,      /* same, target is guest register fix_reg (not written in the range) + `target` (braf/bsrf: PC+4) */
       META_TARGET_PR,       /* same, target is the guest PR (rts) */
       META_COND,            /* bt/s, bf/s after the slot: SSR.T ? target : fallthrough */
       META_COND_REG,        /* same, but the deciding T bit is bit 0 of r[fix_reg] (slot wrote T) */
       META_RTE,
       META_LINEAR };       /* copied instructions: guest offset advances with host offset */           /* guest rte pending: not resumable */

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
        sf_u16 lit_off;          /* host offset of the link literal (patched by the runtime) */
        sf_u16 stub_off;         /* host offset of the exit stub */
        sf_u16 jump_off;         /* host offset of the 4-word jump sequence (lds rT,X0 ...) */
        sf_u16 w0, w1, w2, skip_w; /* original words 0-2 of the sequence (and the skip), to undo chaining */
        sf_u8 cond;              /* the word before jump_off is the inverted bt/bf skip */
        sf_u8 backward;          /* guest target is at or before the branch (loop back edge) */
        sf_u32 guest_target;     /* 20 bytes, stored only for actual links */
} dbt_link;

/* A translated block. `meta`, `spills` and `links` point at storage the caller
 * provides before translation (at least DBT_MAX_META / DBT_MAX_BLOCK_SPILLS
 * entries, and DBT_MAX_LINKS links); the runtime translates into a scratch
 * block and then copies the used entries into a compact arena, so the block table stays small. */
/* 36-byte target header: bounded offsets/counts do not need full words. */
typedef struct {
    sf_u32 guest_pc, host_base;
    dbt_imeta *meta;         /* nmeta + 1 entries, including the sentinel */
    dbt_spill *spills;
    dbt_link *links;
#ifdef DBT_COMPACT_HEADER32
#if defined(DBT_CONST_DEVICE_ADDRESS) || defined(DBT_UNIFORM_STORE)
#error DBT_COMPACT_HEADER32 requires no entry-dependent specialization
#endif
    sf_u16 code_len, host_len; /* host_len remains at target offset 22 */
    short ic_off;
    sf_u8 guest_len, nmeta;
    sf_u8 packed_meta : 1;
    sf_u8 end_reason : 3;
    sf_u8 reserved_flags : 4;
    sf_u8 nspills, nlinks;
#else
    sf_u16 guest_len, host_len, code_len;
    short ic_off;           /* inline cache pair offset, or -1 */
    sf_u8 nmeta, packed_meta, nspills, nlinks, end_reason;
#endif
    /* A GPR untouched by the block, parked in A0 and restored at exits;
     * -1 means none. Used for byte-swapped stores and their cached value. */
    signed char borrow;
#if defined(DBT_CONST_DEVICE_ADDRESS) || defined(DBT_UNIFORM_STORE)
    sf_u8 const_specialized; /* halfword offset after last entry-dependent access */
#endif
#ifdef DBT_UNIFORM_ENTRY_BOUNDS
    sf_u8 uniform_entry_start; /* earliest definition required by native stores */
#endif
} dbt_block;
#ifdef DBT_COMPACT_HEADER32
typedef char check_compact_guest_len[((DBT_MAX_INSNS * 2 + 2) <= 255) ? 1 : -1];
typedef char check_compact_target_header[(sizeof(void *) != 4 || sizeof(dbt_block) == 32) ? 1 : -1];
typedef char check_compact_host_len_offset[(sizeof(void *) != 4 || __builtin_offsetof(dbt_block, host_len) == 22) ? 1 : -1];
#endif
typedef char check_meta_count[(DBT_MAX_META <= 255) ? 1 : -1];
typedef char check_spill_count[(DBT_MAX_BLOCK_SPILLS <= 255) ? 1 : -1];
typedef char check_host_offset[(DBT_BLOCK_WORDS * 2 <= 32767) ? 1 : -1];

/* Runtime metadata: ordinary instructions need only six bytes.
 * guest_half holds a halfword offset, or an index into the aligned branch
 * target tail. kind_reg packs the 4-bit recovery kind and 4-bit GPR. */
typedef struct {
    sf_u16 host_off;
    sf_u8 guest_half, kind_reg, commit_rel, fix_kind;
} dbt_pmeta;
typedef char check_pmeta_size[(sizeof(dbt_pmeta) == 6) ? 1 : -1];
static inline unsigned dbt_meta_aux_offset(unsigned nmeta)
{
    return ((nmeta + 1) * sizeof(dbt_pmeta) + 3u) & ~3u;
}
static inline int dbt_meta_has_targets(unsigned kind)
{
    return kind == META_TARGET_CONST || kind == META_TARGET_REG ||
           kind == META_COND || kind == META_COND_REG;
}
static inline unsigned dbt_meta_kind(const dbt_block *b, int i)
{
    return b->packed_meta ? (((const dbt_pmeta *)b->meta)[i].kind_reg & 15) : b->meta[i].kind;
}
static inline void dbt_get_meta(const dbt_block *b, int i, dbt_imeta *out)
{
    const dbt_pmeta *p;
    if (!b->packed_meta) { *out = b->meta[i]; return; }
    p = &((const dbt_pmeta *)b->meta)[i];
    out->host_off = p->host_off; out->guest_off = (sf_u16)p->guest_half * 2;
    out->kind = p->kind_reg & 15; out->commit_rel = p->commit_rel;
    out->fix_kind = p->fix_kind; out->fix_reg = p->kind_reg >> 4;
    out->target = out->fallthrough = 0;
    if (dbt_meta_has_targets(out->kind)) {
        const sf_u32 *aux = (const sf_u32 *)((const sf_u8 *)b->meta + dbt_meta_aux_offset(b->nmeta));
        out->target = aux[p->guest_half * 2]; out->fallthrough = aux[p->guest_half * 2 + 1];
    }
}
/* out needs at most (nmeta+1)*sizeof(dbt_imeta), aligned to 4 bytes. */
unsigned dbt_pack_meta(void *out, const dbt_block *b);

enum { END_BRANCH, END_TRAP, END_PRIV, END_FULL, END_ILLEGAL, END_FAULT };

/* Translate the block starting at guest_pc into buf (cap words), which will
 * be placed at host address host_base. Returns DBT_OK or an error; on
 * DBT_ERR_FETCH/READ, out->guest_len is the faulting guest address offset
 * and *fault_addr receives the address. */
int dbt_translate_block(const dbt_env *env, sf_u32 guest_pc, sf_u32 host_base,
                        sf_u16 *buf, int cap, dbt_block *out, sf_u32 *fault_addr);

/* Branch chaining, done by the runtime when it links a direct jump: rewrite
 * the 5-word jump sequence at `code` (host address `host`) into native
 * branches when `target` is in range. The sequence is
 *   [0] sts A0,rX (the borrowed register's restore) or nop
 *   [1] lds rT,X0 ; [2] mov.l lit,rT ; [3] jmp @rT ; [4] sts X0,rT
 * Form A (any direct jump within the 12-bit `bra` range): word 0 = `bra
 * target`, word 1 = the old word 0 (its delay slot). Form B (a conditional
 * back edge whose target is within the 8-bit range): the skip word before
 * the sequence becomes the real `bt/bf target` and word 0 a `bra` over the
 * rest when there is nothing to restore, so a taken loop branch costs one
 * instruction; with a restore the skip becomes `bt/s | bf/s target` with
 * word 0 as its delay slot, word 1 the `bra` and word 2 a nop. With
 * `no_restore` the restore word is left out of the rewritten forms (the
 * runtime chains a block's back edge to itself past its park, so the
 * register stays borrowed across iterations). Returns the number of
 * bytes rewritten (0: out of range) and sets *from to the first rewritten
 * address. The resume metadata stays valid: the rewritten words are either
 * range starts or delay slots, and a borrowed register's restore that has
 * not run yet is applied by the spill undo. */
int dbt_chain_direct(sf_u16 *code, sf_u32 host, sf_u32 target, int cond, int backward, int no_restore, sf_u32 *from);

/* Map a host byte offset inside a block to its guest instruction. Returns
 * the meta index or -1. */
/* Safe interior entry with canonical guest registers, or -1. No borrowed
 * register, unfinished scratch save, delay-slot entry or end sentinel. */
int dbt_block_entry(const dbt_block *b, sf_u32 guest_pc);

int dbt_block_find(const dbt_block *b, sf_u32 host_off);

/* Spills to undo at host_off: GPR <- slot, or SR.T <- slot bit 0 for
 * DBT_SPILL_T. Pending means
 * those with start < host_off <= end, in undo order (apply in sequence; an
 * outer spill of the same register comes last and wins). `idx` is unused
 * (ranges and spills are independent). Returns count, fills up to `max`. */
int dbt_block_pending_spills(const dbt_block *b, int idx, sf_u32 host_off, dbt_spill *out, int max);

#endif
