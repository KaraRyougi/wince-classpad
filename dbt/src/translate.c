#include <string.h>
#include "translate.h"

#define R(n) (1u << (n))
#define SR_MD 0x40000000u
#if defined(DBT_R0_LOAD_MERGE_META) && !defined(DBT_R0_LOAD_FALLTHROUGH)
#error DBT_R0_LOAD_MERGE_META requires the load-fallthrough layout
#endif
#if defined(DBT_R0_LOAD_FALLTHROUGH) && (!defined(DBT_R0_DEVICE_TEST) || !defined(DBT_INLINE_DEVICE_READ) || defined(DBT_R0_DIRECT_DEVICE_BRANCH) || defined(DBT_R0_DEVICE_WIDE))
#error DBT_R0_LOAD_FALLTHROUGH requires narrow R0 tests and inline reads without direct-branch mode
#endif
#if defined(DBT_R0_DIRECT_DEVICE_BRANCH) && !defined(DBT_R0_DEVICE_TEST)
#error DBT_R0_DIRECT_DEVICE_BRANCH requires DBT_R0_DEVICE_TEST
#endif
#if defined(DBT_R0_DEVICE_WIDE) && !defined(DBT_R0_DEVICE_TEST)
#error DBT_R0_DEVICE_WIDE requires DBT_R0_DEVICE_TEST
#endif
#if defined(DBT_R0_DEVICE_TEST) && !defined(DBT_SHORT_DEVICE_TEST)
#error DBT_R0_DEVICE_TEST requires DBT_SHORT_DEVICE_TEST
#endif
#if defined(DBT_UNIFORM_ENTRY_BOUNDS) && (!defined(DBT_UNIFORM_STORE) || defined(DBT_CONST_DEVICE_ADDRESS))
#error DBT_UNIFORM_ENTRY_BOUNDS requires uniform stores without constant-address specialization
#endif
#if defined(DBT_CONST_P4_STORE) && !defined(DBT_CONST_DEVICE_ADDRESS)
#error DBT_CONST_P4_STORE requires constant-address tracking
#endif

/* literal tags */
#define TAG_LINK(k)   (100 + (k))   /* branch literal holding the host target (patched when linked) */
#define TAG_STUB_T(k) (200 + (k))   /* exit stub: guest target */
#define TAG_STUB_L(k) (300 + (k))   /* exit stub: address of the link literal */
#define TAG_IC_G      400           /* inline cache: cached guest target (followed by TAG_IC_H in the pool) */
#define TAG_IC_H      401           /* inline cache: its host address */
#define TAG_IC_A      402           /* address of the TAG_IC_G literal (X1 for the lookup_ic stub) */
#define TAG_RET       403           /* helper return address: host_base + offset (relocated with the block) */
#define TAG_NONE      (-1)

typedef struct {
    const dbt_env *env;
    emitter em;
    dbt_block *blk;
    sf_u32 gpc;            /* guest pc of the instruction being translated */
    dbt_imeta *cur;        /* meta entry being filled */
    int err;
    sf_u32 fault;
    int    slot_x0;          /* translating the delay slot of a branch whose target sits in X0 */
    /* Borrowed register (dbt_block.borrow): bx = -1 or a GPR no instruction
     * of the block touches, parked in A0 for the whole block. Stores swap
     * into it without a spill, and cache_reg/cache_size record that it
     * holds swapN(cache_size, r[cache_reg]) (cache_reg -1: nothing), so a
     * store of the same register again costs one instruction. */
    int    bx;
    int    cache_reg, cache_size;
    sf_u32 slot_avoid;       /* registers a delay slot may not use as scratch: the branch's
                              * target register (META_TARGET_REG reads it at a resume, before
                              * the pending spills are undone) and the kept PR scratch */
    /* instructions the pre-pass decoded, from the block's guest pc on
     * (g_pre: one translation at a time; an abandoned one never resumes) */
    int    pre_n;
#ifdef DBT_UNIFORM_STORE
    sf_u32 uniform_mask;
#ifdef DBT_UNIFORM_ENTRY_BOUNDS
    sf_u8 uniform_origin[16];
#endif
#endif
#ifdef DBT_CONST_DEVICE_ADDRESS
    sf_u32 known[16], known_mask;
#endif
} tctx;

static sh_insn g_pre[DBT_MAX_INSNS * 2 + 2];

/* DSP slots for scratch spills. Inside the delay slot of an indirect branch
 * X0 holds the target, so the slots shift: X0->X1, X1->Y1, Y1->Y0 (Y0 is
 * free during such a slot: target_to_x0 and set_pr are done with it). A0
 * belongs to the borrowed register alone. */
static int SA(const tctx *t) { return t->slot_x0 ? SLOT_X1 : SLOT_X0; }
static int SB(const tctx *t) { return t->slot_x0 ? SLOT_Y1 : SLOT_X1; }
static int SC(const tctx *t) { return t->slot_x0 ? SLOT_Y0 : SLOT_Y1; }

/* Mark the memory operation about to be emitted as the range's commit point. */
static void commit(tctx *t, int fix_kind, int fix_reg)
{
    dbt_imeta *m = t->cur;
    sf_u32 rel = (sf_u32)emit_pos(&t->em) - m->host_off;
    if (rel >= 0xFF || m->commit_rel != 0xFF) { t->err = DBT_ERR_INTERNAL; return; }
    m->commit_rel = (sf_u8)rel; m->fix_kind = (sf_u8)fix_kind; m->fix_reg = (sf_u8)fix_reg;
}

/* ------------------------------------------------------------ helpers */

/* A scratch to spill: never the borrowed register, whose spill records
 * would then interleave with the borrow's (a scratch spilled while it held
 * the swap value and restored after an exit's restore would overwrite the
 * guest value just put back). */
static int pick_scratch(const tctx *t, sf_u32 avoid)
{
    int r;
    if (t->bx >= 0) avoid |= R(t->bx);
    avoid |= t->slot_avoid;
    for (r = 1; r <= 14; r++) if (!(avoid & R(r))) return r;
    return -1;
}

static void spill(tctx *t, int gpr, int slot)
{
    dbt_block *b = t->blk;
    if (b->nspills < DBT_MAX_BLOCK_SPILLS) {
        dbt_spill *s = &b->spills[b->nspills++];
#ifndef DBT_COMPACT_SPILL6
        s->insn = (sf_u8)(t->cur - b->meta);
#endif
        s->gpr = (sf_u8)gpr; s->slot = (sf_u8)slot;
#ifndef DBT_COMPACT_SPILL6
        s->pad = 0;
#endif
        s->start = (sf_u16)emit_pos(&t->em);
        s->end = 0xFFFF;
    } else {
        t->err = DBT_ERR_OVERFLOW;
    }
    emit16(&t->em, op_lds_slot(gpr, slot));
}

static void restore(tctx *t, int gpr, int slot)
{
    dbt_block *b = t->blk;
    int i;
    for (i = b->nspills - 1; i >= 0; i--)
        if (b->spills[i].gpr == gpr && b->spills[i].slot == slot && b->spills[i].end == 0xFFFF) {
            b->spills[i].end = (sf_u16)emit_pos(&t->em);
            break;
        }
    emit16(&t->em, op_sts_slot(slot, gpr));
}

static void swap32(tctx *t, int src, int dst)
{
    emit16(&t->em, OP_SWAPB(src, dst));
    emit16(&t->em, OP_SWAPW(dst, dst));
    emit16(&t->em, OP_SWAPB(dst, dst));
}

static void swapN(tctx *t, int size, int src, int dst)
{
    if (size == 4) swap32(t, src, dst);
    else emit16(&t->em, OP_SWAPB(src, dst));
}

/* --- the borrowed register --- */

static void cache_kill(tctx *t) { t->cache_reg = -1; }

/* bx = swapN(size, r[src]), unless it holds that already */
static void bx_swap(tctx *t, int size, int src)
{
    if (t->cache_reg == src && t->cache_size == size) return;
    swapN(t, size, src, t->bx);
    t->cache_reg = src; t->cache_size = size;
}

/* Restore the borrowed register before control leaves the block. The spill
 * record stays open: on a conditional exit the block goes on along the other
 * path, and an undo after the restore is harmless (A0 keeps the value). */
static void exit_restore(tctx *t)
{
    if (t->bx >= 0) emit16(&t->em, op_sts_slot(SLOT_A0, t->bx));
    cache_kill(t);
}

/* Around a helper call: helpers want the exact guest registers (their
 * exception deliveries take the stub context as it is) and own all slots. */
static void borrow_pause(tctx *t)
{
    if (t->bx >= 0) restore(t, t->bx, SLOT_A0);
    cache_kill(t);
}
static void borrow_resume(tctx *t)
{
    if (t->bx >= 0) spill(t, t->bx, SLOT_A0);
}

/* control register <-> GPR (non-privileged ones) */
static sf_u32 op_read_cr(int cr, int bank, int rn)
{
    switch (cr) {
    case CR_MACH: return OP_STS_MACH(rn);
    case CR_MACL: return OP_STS_MACL(rn);
    case CR_PR:   return OP_STS_PR(rn);
    case CR_GBR:  return OP_STC_GBR(rn);
    case CR_SSR:  return OP_STC_SSR(rn);
    case CR_SPC:  return OP_STC_SPC(rn);
    case CR_SGR:  return OP_STC_SGR(rn);
    case CR_DBR:  return OP_STC_DBR(rn);
    case CR_RBANK: return OP_STC_RBANK(bank, rn);
    }
    return OP_NOP;
}
static sf_u32 op_write_cr(int cr, int bank, int rm)
{
    switch (cr) {
    case CR_MACH: return OP_LDS_MACH(rm);
    case CR_MACL: return OP_LDS_MACL(rm);
    case CR_PR:   return OP_LDS_PR(rm);
    case CR_GBR:  return OP_LDC_GBR(rm);
    case CR_SSR:  return OP_LDC_SSR(rm);
    case CR_SPC:  return OP_LDC_SPC(rm);
    case CR_DBR:  return OP_LDC_DBR(rm);
    case CR_RBANK: return OP_LDC_RBANK(rm, bank);
    }
    return OP_NOP;
}

/* Re-encode a store with a different data register (forms with a free data field). */
static sf_u32 store_with_data(const sh_insn *d, int data)
{
    sf_u32 n = (sf_u32)d->base << 8, m = (sf_u32)data << 4;
    switch (d->am) {
    case AM_REG:    return (0x2000u + (d->size == 1 ? 0 : d->size == 2 ? 1 : 2)) | n | m;
    case AM_PREDEC: return (0x2004u + (d->size == 1 ? 0 : d->size == 2 ? 1 : 2)) | n | m;
    case AM_R0IDX:  return (0x0004u + (d->size == 1 ? 0 : d->size == 2 ? 1 : 2)) | n | m;
    case AM_DISP:   return 0x1000u | n | m | (sf_u32)(d->disp / 4);   /* size 4 only */
    }
    return OP_NOP;
}

static int store_data_is_fixed_r0(const sh_insn *d)
{
    if (d->movca) return 1;
    if (d->am == AM_GBR) return 1;
    if (d->am == AM_DISP && d->size != 4) return 1;
    return 0;
}

/* --------------------------------------------------------- memory ops */

/* Device-aware translation (dbt_env.dev_lo/hi): is this a 32-bit access that
 * must test its address at run time? */
static int dev_candidate(const tctx *t, const sh_insn *d)
{
    sf_u32 pc;
    if (!t->env->dev_hi || d->kind != K_MEM || d->size != 4 || d->ctrl != CR_NONE || d->movca || d->am == AM_GBR) return 0;
    pc = t->gpc & 0x1FFFFFFFu;
    if (pc < (t->env->dev_lo & 0x1FFFFFFFu) || pc >= (t->env->dev_hi & 0x1FFFFFFFu)) return 0;
    if (t->env->dev_ranges) {
        /* the few functions that can reach P4 (rt/kdev.c): binary search */
        unsigned lo = 0, hi = t->env->ndev_ranges;
        while (lo < hi) {
            unsigned mid = (lo + hi) / 2;
            if (pc < t->env->dev_ranges[2 * mid]) hi = mid;
            else if (pc >= t->env->dev_ranges[2 * mid + 1]) lo = mid + 1;
            else return 1;
        }
        return 0;
    }
    return 1;
}

static void tr_mem_plain(tctx *t, const sh_insn *d);
static void call_helper(tctx *t, sf_u32 helper, sf_u32 arg_reg, int arg_mode, sf_u32 arg_imm, sf_u32 avoid);
static void begin_meta_kind(tctx *t, int kind, sf_u32 guest_off, sf_u32 target, sf_u32 fallthrough);

#if defined(DBT_SAME_ASID_PTEH_STORE) && !defined(DBT_DYNAMIC_P4_STORE)
#error DBT_SAME_ASID_PTEH_STORE requires DBT_DYNAMIC_P4_STORE
#endif
/* Each PTEH-checked store adds ~30 words; a run of them in delay slots can
 * overflow the block buffer, and only DBT_BODY_BUDGET retries a smaller block
 * (without it the runtime halts on "translation failed"). */
#if defined(DBT_SAME_ASID_PTEH_STORE) && !defined(DBT_BODY_BUDGET)
#error DBT_SAME_ASID_PTEH_STORE requires DBT_BODY_BUDGET
#endif

#ifdef DBT_DYNAMIC_P4_STORE
static void tr_p4_store(tctx *t, const sh_insn *d)
{
    emitter *e = &t->em;
    dbt_block *b = t->blk;
    int a = pick_scratch(t, d->touched), v = pick_scratch(t, d->touched | R(a));
    int t_index, pteh, mmucr, select_fast, selected, protected_at, restore_at;
    int dispatch, skip, fast, done;
#ifdef DBT_SAME_ASID_PTEH_STORE
    int same_asid_at = -1;
#endif
    sf_u32 goff = t->gpc - b->guest_pc;
    spill(t, a, SA(t)); spill(t, v, SB(t));
    emit16(e, OP_STC_SR(v));
    if (b->nspills >= DBT_MAX_BLOCK_SPILLS) { t->err = DBT_ERR_OVERFLOW; return; }
    t_index = b->nspills++;
#ifndef DBT_COMPACT_SPILL6
    b->spills[t_index].insn = (sf_u8)(t->cur - b->meta);
#endif
    b->spills[t_index].gpr = DBT_SPILL_T;
    b->spills[t_index].slot = (sf_u8)SC(t);
#ifndef DBT_COMPACT_SPILL6
    b->spills[t_index].pad = 0;
#endif
    b->spills[t_index].start = (sf_u16)emit_pos(e);
    b->spills[t_index].end = 0xffff;
    emit16(e, op_lds_slot(v, SC(t)));
    emit16(e, OP_MOV_RR(d->base, a));
    if (d->am == AM_PREDEC) emit16(e, OP_ADD_IMM(-4, a));
    else if (d->am == AM_DISP && d->disp) emit16(e, OP_ADD_IMM(d->disp, a));
    else if (d->am == AM_R0IDX) emit16(e, OP_ADD_RR(0, a));
    emit_mov_lit(e, v, 0xFF000000u, TAG_NONE);
    emit16(e, OP_CMPEQ_RR(a, v));
    pteh = emit_pos(e); emit16(e, OP_BT(0));
    emit_mov_lit(e, v, 0xFF000010u, TAG_NONE);
    emit16(e, OP_CMPEQ_RR(a, v));
    mmucr = emit_pos(e); emit16(e, OP_BT(0));
    select_fast = emit_pos(e); emit16(e, OP_MOV_IMM(0, a));
    selected = emit_pos(e); emit16(e, OP_BRA(0)); emit16(e, OP_NOP);
#ifdef DBT_SAME_ASID_PTEH_STORE
    if (t->env->current_asid_addr) {
        same_asid_at = emit_pos(e);
        emit16(e, OP_MOV_RR(d->data, a));
        if (d->am == AM_PREDEC && d->data == d->base) emit16(e, OP_ADD_IMM(-4, a));
        emit16(e, OP_EXTUB(a, a));
        emit_mov_lit(e, v, t->env->current_asid_addr, TAG_NONE);
        emit16(e, OP_MOVL_LOAD(v, v));
        emit16(e, OP_CMPEQ_RR(a, v));
        emit16(e, OP_BT((select_fast - emit_pos(e) - 4) / 2));
    }
#endif
    protected_at = emit_pos(e); emit16(e, OP_MOV_IMM(0, a));
    restore_at = emit_pos(e);
#ifdef DBT_SAME_ASID_PTEH_STORE
    emit_patch16(e, pteh, OP_BT(((same_asid_at >= 0 ? same_asid_at : protected_at) - pteh - 4) / 2));
#else
    emit_patch16(e, pteh, OP_BT((protected_at - pteh - 4) / 2));
#endif
    emit_patch16(e, mmucr, OP_BT((protected_at - mmucr - 4) / 2));
    emit_patch16(e, selected, OP_BRA((restore_at - selected - 4) / 2));
    emit16(e, op_sts_slot(SC(t), v));
    b->spills[t_index].end = (sf_u16)emit_pos(e);
    emit16(e, OP_LDC_SR(v));
    restore(t, v, SB(t));
    dispatch = emit_pos(e); emit16(e, OP_BRAF(a));
    restore(t, a, SA(t));
    /* All scratch registers and T are canonical before either access. */
    call_helper(t, t->env->rt_fallback, 0, 2, d->op, d->touched);
    skip = emit_pos(e); emit16(e, OP_BRA(0)); emit16(e, OP_NOP);
    fast = emit_pos(e);
    if (fast - dispatch - 4 > 127) { t->err = DBT_ERR_OVERFLOW; return; }
    emit_patch16(e, select_fast, OP_MOV_IMM(fast - dispatch - 4, a));
    begin_meta_kind(t, META_INSN, goff, 0, 0);
    commit(t, FIX_NONE, 0); emit16(e, d->op);
    done = emit_pos(e);
    emit_patch16(e, skip, OP_BRA((done - skip - 4) / 2));
}
#endif

/* The effective address is tested for P4 without touching T (every shift
 * used here leaves T alone): rT ends as 0 for a P4 address and 4 otherwise,
 * and `braf rT` either falls into the `bra slow` or skips it. Layout:
 *   test ; braf rT ; sts rT (slot)
 *   bra slow ; nop
 *   fast: the usual swapped access ; bra done ; nop
 *   slow: fallback helper with the opcode, in a META range of its own so a
 *         fault in its prologue restarts the instruction (no commit there)
 *   done:
 * Cost: 15 instructions on top of the plain sequence, kernel.dll only. */
static void tr_mem_dev(tctx *t, const sh_insn *d)
{
    emitter *e = &t->em;
    int own = t->bx < 0;                  /* no borrowed register: spill one */
    int rT = own ? pick_scratch(t, d->touched) : t->bx;
    int bra_slow, bra_done, slow, done;
#ifdef DBT_R0_DIRECT_DEVICE_BRANCH
    int direct_mask = -1, direct_branch = -1;
#endif
    sf_u32 goff = t->gpc - t->blk->guest_pc;
    dbt_imeta parent = *t->cur;           /* a branch range when this is a delay slot */
#ifdef DBT_R0_DEVICE_TEST
#ifdef DBT_R0_DEVICE_WIDE
    /* The original operands are restored in BRAF's delay slot before the
     * actual access. Only indexed EA formation needs R0 concurrently. */
    if (own && d->am != AM_R0IDX && !(t->slot_avoid & R(0))) rT = 0;
#else
    if (own && !((d->touched | t->slot_avoid) & R(0))) rT = 0;
#endif
#endif
    if (own) spill(t, rT, SA(t)); else cache_kill(t);
    emit16(e, OP_MOV_RR(d->base, rT));
    if (d->am == AM_PREDEC) emit16(e, OP_ADD_IMM(-4, rT));
    else if (d->am == AM_DISP && d->disp) emit16(e, OP_ADD_IMM(d->disp, rT));
    else if (d->am == AM_R0IDX) emit16(e, OP_ADD_RR(0, rT));
#ifdef DBT_SHORT_DEVICE_TEST
    /* ((top_byte + 1) >> 8) is 1 exactly for 0xFF. All shifts here
     * preserve T; negation then gives -1 for P4 and zero otherwise. */
    emit16(e, OP_SHLR16(rT));
    emit16(e, OP_SHLR8(rT));
    emit16(e, OP_ADD_IMM(1, rT));
    emit16(e, OP_SHLR8(rT));
#ifdef DBT_R0_DEVICE_TEST
    if (rT == 0) {
#ifdef DBT_R0_LOAD_FALLTHROUGH
        if (own && !d->is_store) {
            /* P4 selects the cold native load; RAM skips its three words. */
            emit16(e, OP_ADD_IMM(-1, 0));
            emit16(e, 0xC906u); /* and #6,r0 preserves T */
        } else
#endif
#ifdef DBT_R0_DIRECT_DEVICE_BRANCH
        /* Zero for ordinary RAM, all ones for P4. Patch AND's immediate
         * to the slow-path distance, avoiding a separate BRA slow island. */
        emit16(e, OP_NEG_RR(0, 0));
        direct_mask = emit_pos(e); emit16(e, 0xC900u);
#else
        emit16(e, 0xCA01u); /* xor #1,r0 preserves T */
#endif
    }
    else
#endif
    {
        emit16(e, OP_NEG_RR(rT, rT));
        emit16(e, OP_ADD_IMM(1, rT));
    }
#else
    emit16(e, OP_NOT_RR(rT, rT));         /* top byte 0xFF -> 0x00 */
    emit16(e, OP_SHLR16(rT));
    emit16(e, OP_SHLR8(rT));              /* 0 for P4, else 1..255 */
    emit16(e, OP_NEG_RR(rT, rT));         /* 0 or negative */
    emit16(e, OP_SHLR16(rT));
    emit16(e, OP_SHLR8(rT));              /* 0 or 0xFF */
    emit16(e, OP_ADD_IMM(1, rT));
#endif
#ifndef DBT_SHORT_DEVICE_TEST
    emit16(e, OP_SHLR8(rT));              /* 0 or 1 */
#endif
#ifdef DBT_R0_DIRECT_DEVICE_BRANCH
    if (direct_mask < 0)
#endif
#ifdef DBT_R0_LOAD_FALLTHROUGH
    if (!(own && rT == 0 && !d->is_store))
#endif
    emit16(e, OP_SHLL2(rT));              /* 0 (P4: fall into bra slow) or 4 (skip it) */
#ifdef DBT_R0_DIRECT_DEVICE_BRANCH
    direct_branch = emit_pos(e);
#endif
    emit16(e, OP_BRAF(rT));
    if (own) restore(t, rT, SA(t)); else emit16(e, OP_NOP);   /* delay slot */
#ifdef DBT_R0_LOAD_FALLTHROUGH
    if (own && rT == 0 && !d->is_store) {
        /* Put the rare unswapped load first, so the usual RAM load falls
         * through without a trailing BRA/NOP. Both accesses keep their
         * own commit range, including post-increment and delay-slot loads. */
#ifdef DBT_R0_LOAD_MERGE_META
        /* The classifier and cold load restart the same instruction.
         * Share that range only while it has no earlier commit point. */
        if (parent.kind != META_INSN || parent.guest_off != goff || parent.commit_rel != 0xFF)
#endif
        begin_meta_kind(t, META_INSN, goff, 0, 0);
        commit(t, FIX_NONE, 0); emit16(e, d->op);
        bra_done = emit_pos(e); emit16(e, OP_BRA(0)); emit16(e, OP_NOP);
        begin_meta_kind(t, META_INSN, goff, 0, 0);
        tr_mem_plain(t, d);
        done = emit_pos(e);
        emit_patch16(e, bra_done, OP_BRA((done - bra_done - 4) / 2));
        cache_kill(t);
        if (parent.kind != META_INSN)
            begin_meta_kind(t, parent.kind, parent.guest_off, parent.target, parent.fallthrough);
        return;
    }
#endif
    bra_slow = emit_pos(e);
#ifdef DBT_R0_DIRECT_DEVICE_BRANCH
    if (direct_mask < 0)
#endif
    { emit16(e, OP_BRA(0)); emit16(e, OP_NOP); }
    tr_mem_plain(t, d);
    bra_done = emit_pos(e); emit16(e, OP_BRA(0)); emit16(e, OP_NOP);
    slow = emit_pos(e);
#ifdef DBT_R0_DIRECT_DEVICE_BRANCH
    if (direct_mask >= 0) {
        unsigned distance = (unsigned)(slow - direct_branch - 4);
        if (distance > 255) { t->err = DBT_ERR_OVERFLOW; return; }
        emit_patch16(e, direct_mask, 0xC900u | distance);
    } else
#endif
    emit_patch16(e, bra_slow, OP_BRA((slow - (bra_slow + 4)) / 2));
    begin_meta_kind(t, META_INSN, goff, 0, 0);
#ifdef DBT_INLINE_DEVICE_READ
    if (!d->is_store) {
        /* P4 register reads already have the guest's numeric value. They
         * neither change ASID nor invalidate mappings, so no C hook is
         * needed. Keep a commit point for post-increment recovery. */
        commit(t, FIX_NONE, 0);
        emit16(e, d->op);
    } else
#endif
#ifdef DBT_DYNAMIC_P4_STORE
    if (d->is_store) tr_p4_store(t, d);
    else
#endif
    call_helper(t, t->env->rt_fallback, 0, 2, d->op, d->touched);
    done = emit_pos(e);
    emit_patch16(e, bra_done, OP_BRA((done - (bra_done + 4)) / 2));
    cache_kill(t);                        /* the two paths join */
    if (parent.kind != META_INSN) {
        /* inside a branch: what follows (target capture, the jump) must keep
         * the branch range's resume rule, not restart the branch */
        begin_meta_kind(t, parent.kind, parent.guest_off, parent.target, parent.fallthrough);
    }
}

static void tr_mem(tctx *t, const sh_insn *d)
{
    if (d->ctrl == CR_NONE && dev_candidate(t, d)) {
#ifdef DBT_CONST_DEVICE_ADDRESS
        if (d->base >= 0 && (t->known_mask & R(d->base)) && d->am != AM_R0IDX) {
            sf_u32 address = t->known[d->base];
            if (d->am == AM_PREDEC) address -= 4;
            else if (d->am == AM_DISP) address += d->disp;
#ifdef DBT_CONST_P4_STORE
            /* Only PTEH and MMUCR have software side effects in dev_mem.
             * The address proof is the same literal already emitted for
             * this block; entry restrictions prevent bypassing that load. */
            if (d->is_store && (address >> 24) == 0xFFu &&
                address != 0xFF000000u && address != 0xFF000010u) {
                t->blk->const_specialized = (sf_u8)((t->gpc - t->blk->guest_pc) / 2 + 1);
                commit(t, FIX_NONE, 0);
                emit16(&t->em, d->op);
                return;
            }
#else
            if ((address >> 24) != 0xFFu) {
                t->blk->const_specialized = (sf_u8)((t->gpc - t->blk->guest_pc) / 2 + 1);
                tr_mem_plain(t, d);
                return;
            }
#endif
        }
#endif
        tr_mem_dev(t, d); return;
    }
    tr_mem_plain(t, d);
}

static void tr_mem_plain(tctx *t, const sh_insn *d)
{
    emitter *e = &t->em;
#ifdef DBT_UNIFORM_STORE
    /* Byte-uniform values (e.g. zero/-1) are endian invariant. Do not bypass
     * device dispatch or a predecrement which changes the data register. */
    if (d->is_store && d->ctrl == CR_NONE && d->size > 1 &&
        (t->uniform_mask & R(d->data)) && !dev_candidate(t, d) &&
        !(d->am == AM_PREDEC && d->data == d->base)) {
        commit(t, FIX_NONE, 0);
        emit16(e, d->op);
#ifdef DBT_UNIFORM_ENTRY_BOUNDS
        if (!t->blk->const_specialized || t->uniform_origin[d->data] < t->blk->uniform_entry_start)
            t->blk->uniform_entry_start = t->uniform_origin[d->data];
#endif
        t->blk->const_specialized = (sf_u8)((t->gpc - t->blk->guest_pc) / 2 + 1);
        return;
    }
#endif
    if (d->ctrl != CR_NONE) {
        /* sts.l/stc.l CR,@-Rn  or  lds.l/ldc.l @Rm+,CR */
        int own = t->bx < 0;
        int rT = own ? pick_scratch(t, R(d->base)) : t->bx;
        if (own) spill(t, rT, SA(t)); else cache_kill(t);
        if (d->is_store) {
            emit16(e, op_read_cr(d->ctrl, d->ctrl_bank, rT));
            swap32(t, rT, rT);
            commit(t, FIX_NONE, 0);
            emit16(e, OP_MOVL_PREDEC(rT, d->base));
        } else {
            commit(t, FIX_CR_BASE + d->ctrl, rT);
            emit16(e, OP_MOVL_POSTINC(d->base, rT));
            swap32(t, rT, rT);
            emit16(e, op_write_cr(d->ctrl, d->ctrl_bank, rT));
        }
        if (own) restore(t, rT, SA(t));
        return;
    }
    if (d->size == 1) { emit16(e, d->op); return; }
    if (!d->is_store) {
        commit(t, d->size == 2 ? FIX_SWAP16 : FIX_SWAP32, d->data);
        emit16(e, d->op);
        if (d->size == 2) { emit16(e, OP_SWAPB(d->data, d->data)); emit16(e, OP_EXTSW(d->data, d->data)); }
        else swap32(t, d->data, d->data);
        return;
    }
    /* stores: everything before the store itself is undone by the spill
     * records, so an exception before the commit point restarts cleanly */
    if (!store_data_is_fixed_r0(d) && t->bx >= 0) {
        /* borrowed register: no spill; the swapped value stays cached */
        int rT = t->bx;
        if (d->am == AM_PREDEC && d->data == d->base) {
            cache_kill(t);
            emit16(e, OP_MOV_RR(d->data, rT));
            emit16(e, OP_ADD_IMM(-d->size, rT));
            swapN(t, d->size, rT, rT);
        } else {
            bx_swap(t, d->size, d->data);
        }
        commit(t, FIX_NONE, 0);
        emit16(e, store_with_data(d, rT));
        return;
    }
    if (!store_data_is_fixed_r0(d)) {
        int rT = pick_scratch(t, d->touched);
        spill(t, rT, SA(t));
        if (d->am == AM_PREDEC && d->data == d->base) {
            /* SH stores the decremented register value */
            emit16(e, OP_MOV_RR(d->data, rT));
            emit16(e, OP_ADD_IMM(-d->size, rT));
            swapN(t, d->size, rT, rT);
        } else {
            swapN(t, d->size, d->data, rT);
        }
        commit(t, FIX_NONE, 0);
        emit16(e, store_with_data(d, rT));
        restore(t, rT, SA(t));
        return;
    }
    /* data fixed to r0 */
    if (d->am == AM_GBR || d->base != 0) {
        spill(t, 0, SA(t));
        swapN(t, d->size, 0, 0);
        commit(t, FIX_NONE, 0);
        emit16(e, d->op);
        restore(t, 0, SA(t));
        return;
    }
    /* base is r0 too: materialize the address in a scratch */
    if (d->movca) {
        int rA = pick_scratch(t, R(0));
        spill(t, rA, SA(t));
        emit16(e, OP_MOV_RR(0, rA));
        spill(t, 0, SB(t));                 /* r0 is swapped in place: keep it restorable */
        swap32(t, 0, 0);
        commit(t, FIX_NONE, 0);
        emit16(e, OP_MOVCA(rA));
        restore(t, 0, SB(t));
        restore(t, rA, SA(t));
        return;
    }
    {
        int rA = pick_scratch(t, R(0));
        int rV = pick_scratch(t, R(0) | R(rA));
        sh_insn s2;
        spill(t, rA, SA(t));
        emit16(e, OP_MOV_RR(0, rA));
        if (d->disp) emit16(e, OP_ADD_IMM(d->disp, rA));
        spill(t, rV, SB(t));
        swapN(t, d->size, 0, rV);
        s2 = *d; s2.am = AM_REG; s2.base = rA;
        commit(t, FIX_NONE, 0);
        emit16(e, store_with_data(&s2, rV));
        restore(t, rV, SB(t));
        restore(t, rA, SA(t));
    }
}

/* ----------------------------------------------------------- pc-relative */

static void tr_pcrel(tctx *t, const sh_insn *d)
{
    sf_u32 val = 0, addr;
    switch (d->pcrel) {
    case PC_MOVW:
        addr = t->gpc + 4 + (sf_u32)d->disp;
        if (t->env->read16le(t->env->ctx, addr, &val)) { t->err = DBT_ERR_READ; t->fault = addr; return; }
        if (val & 0x8000) val |= 0xFFFF0000u;
        break;
    case PC_MOVL:
        addr = ((t->gpc + 4) & ~3u) + (sf_u32)d->disp;
        if (t->env->read32le(t->env->ctx, addr, &val)) { t->err = DBT_ERR_READ; t->fault = addr; return; }
        break;
    default: /* mova */
        val = ((t->gpc + 4) & ~3u) + (sf_u32)d->disp;
        break;
    }
    emit_mov_imm(&t->em, d->pc_reg, val);
#ifdef DBT_UNIFORM_STORE
    t->uniform_mask &= ~R(d->pc_reg);
    if (val == (val & 255u) * 0x01010101u) {
        t->uniform_mask |= R(d->pc_reg);
#ifdef DBT_UNIFORM_ENTRY_BOUNDS
        t->uniform_origin[d->pc_reg] = (sf_u8)((t->gpc - t->blk->guest_pc) / 2);
#endif
    }
#endif
#ifdef DBT_CONST_DEVICE_ADDRESS
    t->known[d->pc_reg] = val;
    t->known_mask |= R(d->pc_reg);
#endif
}

/* ------------------------------------------------------------- helper calls */

/* X0 = arg, X1 = return address (host), jump to helper. rT is spilled in Y0
 * and restored in the jump's delay slot. */
/* arg_mode: 0 = X0 already holds the argument, 1 = X0 <- register arg_reg,
 * 2 = X0 <- immediate arg_imm */
static void call_helper(tctx *t, sf_u32 helper, sf_u32 arg_reg, int arg_mode, sf_u32 arg_imm, sf_u32 avoid)
{
    emitter *e = &t->em;
    int rT = pick_scratch(t, avoid);
    int ret_lit;
    borrow_pause(t);
    if (arg_mode == 1) emit16(e, op_lds_slot((int)arg_reg, SLOT_X0));
    spill(t, rT, SLOT_Y0);
    if (arg_mode == 2) { emit_mov_imm(e, rT, arg_imm); emit16(e, op_lds_slot(rT, SLOT_X0)); }
    ret_lit = emit_mov_lit(e, rT, 0, TAG_RET);               /* return address, patched below */
    emit16(e, op_lds_slot(rT, SLOT_X1));
    emit_mov_lit(e, rT, helper, TAG_NONE);
    emit16(e, OP_JMP(rT));
    /* The call is the range's commit point (at its delay slot): an exception
     * resuming at the return address, where the guest instruction is
     * complete (an interrupt in the helper's exit tail), goes on with the
     * next range instead of running the helper again, while the helper
     * prologue window, which resumes at X1 - 2, still restarts it. Needed
     * since the borrow's re-park below put a word of this range at the
     * return address. ldc.l @rm+,sr has committed at its load already. */
    if (t->cur->commit_rel == 0xFF) commit(t, FIX_NONE, 0);
    restore(t, rT, SLOT_Y0);
    if (ret_lit >= 0) e->lits[ret_lit].value = t->blk->host_base + (sf_u32)emit_pos(e);
    borrow_resume(t);
}

/* ------------------------------------------------------------- branches */

/* Jump to a guest target known at translation time, through a link literal:
 * the 5-word sequence of dbt_chain_direct, the borrowed register's restore
 * (or a nop) first. */
/* cond: the word just emitted is the inverted bt/bf skip over this jump */
static void jump_direct(tctx *t, sf_u32 guest_target, sf_u32 avoid, int cond)
{
    emitter *e = &t->em;
    dbt_block *b = t->blk;
    int rT = pick_scratch(t, avoid);
    int k = b->nlinks;
    if (k >= DBT_MAX_LINKS) { t->err = DBT_ERR_OVERFLOW; return; }
    b->links[k].guest_target = guest_target;
    b->links[k].lit_off = 0xFFFF;
    b->links[k].jump_off = (sf_u16)emit_pos(e);
    b->links[k].cond = (sf_u8)cond;
    b->links[k].backward = (sf_u8)(guest_target <= t->gpc);
    b->nlinks++;
    if (t->bx >= 0) exit_restore(t); else {
#ifndef DBT_EXIT_NOP_TAIL
        emit16(e, OP_NOP);
#endif
        cache_kill(t);
    }
    spill(t, rT, SLOT_X0);
    emit_mov_lit(e, rT, 0, TAG_LINK(k));                     /* host target; initially the exit stub */
    emit16(e, OP_JMP(rT));
    restore(t, rT, SLOT_X0);
#ifdef DBT_EXIT_NOP_TAIL
    /* Keep the five-word link patch window, but do not execute its padding
     * on the no-borrow far-exit path. Conditional skips remain unchanged. */
    if (t->bx < 0) emit16(e, OP_NOP);
#endif
}

/* Jump to the guest address already placed in X0. */
/* Shared miss tail of the inline caches: X0 must hold the guest target.
 * rT is a scratch already spilled to Y0 (its record is closed here). */
static void ic_miss_tail(tctx *t, int rT)
{
    emitter *e = &t->em;
    emit_mov_lit(e, rT, 0, TAG_IC_A);                    /* patched after the pool is placed */
    emit16(e, op_lds_slot(rT, SLOT_X1));
    emit_mov_lit(e, rT, t->env->rt_lookup_ic, TAG_NONE);
    emit16(e, OP_JMP(rT));
    restore(t, rT, SLOT_Y0);
}

/* IC comparisons must not change the guest's T bit. X1 is free until the
 * miss tail fills it with the IC address. Track T like a scratch save so
 * an interrupt between cmp/eq and ldc sr also sees the original value. */
static void ic_save_t(tctx *t, int r)
{
    dbt_block *b = t->blk;
    emit16(&t->em, OP_STC_SR(r));
    if (b->nspills < DBT_MAX_BLOCK_SPILLS) {
        dbt_spill *s = &b->spills[b->nspills++];
#ifndef DBT_COMPACT_SPILL6
        s->insn = (sf_u8)(t->cur - b->meta);
#endif
        s->gpr = DBT_SPILL_T; s->slot = SLOT_X1;
#ifndef DBT_COMPACT_SPILL6
        s->pad = 0;
#endif
        s->start = (sf_u16)emit_pos(&t->em); s->end = 0xffff;
    } else t->err = DBT_ERR_OVERFLOW;
    emit16(&t->em, op_lds_slot(r, SLOT_X1));
}
static void ic_restore_t(tctx *t, int r, int close)
{
    int i;
    emit16(&t->em, op_sts_slot(SLOT_X1, r));
    if (close) for (i = t->blk->nspills - 1; i >= 0; --i) {
        dbt_spill *s = &t->blk->spills[i];
        if (s->gpr == DBT_SPILL_T && s->end == 0xffff) {
            s->end = (sf_u16)emit_pos(&t->em); break;
        }
    }
#ifdef DBT_IC_T_ONLY
    /* Only CMP/EQ changed SR. Shift the saved SR's low bit into T; the
     * scratch value is dead, and all other SR bits stay untouched. */
    emit16(&t->em, OP_SHLR(r));
#else
    emit16(&t->em, OP_LDC_SR(r));
#endif
}

/* Inline cache comparing guest register `reg` directly (not written between
 * the branch and here); one scratch, which set_pr may have left spilled in
 * Y0 (rT_kept) or which is spilled here. X1 temporarily holds saved SR. */
static void jump_indirect_ic_reg(tctx *t, int reg, int rT_kept)
{
    emitter *e = &t->em;
    int rT = rT_kept >= 0 ? rT_kept : pick_scratch(t, R(reg));
    if (rT_kept < 0) spill(t, rT, SLOT_Y0);
    ic_save_t(t, rT);
    emit_mov_lit(e, rT, 1, TAG_IC_G);
    emit16(e, OP_CMPEQ_RR(reg, rT));
    emit16(e, OP_BF(4));
    ic_restore_t(t, rT, 0);
    emit_mov_lit(e, rT, 0, TAG_IC_H);
    emit16(e, OP_JMP(rT));
    emit16(e, op_sts_slot(SLOT_Y0, rT));
    /* miss */
    ic_restore_t(t, rT, 1);
    emit16(e, op_lds_slot(reg, SLOT_X0));                /* park the target for the stub */
    ic_miss_tail(t, rT);
}

/* Inline cache for rts: PR is the target; X1 temporarily holds saved SR. */
static void jump_indirect_ic_pr(tctx *t)
{
    emitter *e = &t->em;
    int rT = pick_scratch(t, 0), rU = pick_scratch(t, R(rT));
    spill(t, rT, SLOT_Y0);
    emit16(e, OP_STS_PR(rT));
    spill(t, rU, SLOT_Y1);
    ic_save_t(t, rU);
    emit_mov_lit(e, rU, 1, TAG_IC_G);
    emit16(e, OP_CMPEQ_RR(rT, rU));
    emit16(e, OP_BF(5));
    ic_restore_t(t, rU, 0);
    emit16(e, op_sts_slot(SLOT_Y0, rT));
    emit_mov_lit(e, rU, 0, TAG_IC_H);
    emit16(e, OP_JMP(rU));
    emit16(e, op_sts_slot(SLOT_Y1, rU));
    /* miss */
    ic_restore_t(t, rU, 1);
    restore(t, rU, SLOT_Y1);
    emit16(e, op_lds_slot(rT, SLOT_X0));
    ic_miss_tail(t, rT);
}

/* Indirect branch with a per-site inline cache. X0 holds the guest target.
 * Hit: preserve T, compare it with the cached guest pc and jump to the
 * cached host address. Miss: dbt_lookup_ic with X1 = address of the literal
 * pair, which the stub refills. Both literals are data, patched through the
 * operand cache. The hit-path restores are not recorded: the spill records
 * end at the miss-path restores, which cover both paths (an undo of a
 * register already restored is harmless). The whole sequence lies in the
 * META_TARGET_X0 range, so an exception anywhere in it resumes at X0. */
static void jump_indirect_ic(tctx *t)
{
    emitter *e = &t->em;
    int rT = pick_scratch(t, 0), rU = pick_scratch(t, R(rT));
    spill(t, rT, SLOT_Y0);                               /* w0 */
    emit16(e, op_sts_slot(SLOT_X0, rT));                 /* w1 rT = guest target */
    spill(t, rU, SLOT_Y1);                               /* w2 */
    ic_save_t(t, rU);
    emit_mov_lit(e, rU, 1, TAG_IC_G);                    /* w3 cached guest pc (1: odd, never matches) */
    emit16(e, OP_CMPEQ_RR(rT, rU));                      /* w4 */
    emit16(e, OP_BF(5));
    ic_restore_t(t, rU, 0);
    emit16(e, op_sts_slot(SLOT_Y0, rT));                 /* w6 */
    emit_mov_lit(e, rU, 0, TAG_IC_H);                    /* w7 cached host address */
    emit16(e, OP_JMP(rU));                               /* w8 */
    emit16(e, op_sts_slot(SLOT_Y1, rU));                 /* w9 */
    /* miss */
    ic_restore_t(t, rU, 1);
    restore(t, rU, SLOT_Y1);                             /* w10 */
    emit_mov_lit(e, rT, 0, TAG_IC_A);                    /* w11 patched after the pool is placed */
    emit16(e, op_lds_slot(rT, SLOT_X1));                 /* w12 */
    emit_mov_lit(e, rT, t->env->rt_lookup_ic, TAG_NONE); /* w13 */
    emit16(e, OP_JMP(rT));                               /* w14 */
    restore(t, rT, SLOT_Y0);                             /* w15 */
}

/* X0 = Rn (or PR when reg < 0), without disturbing guest registers. */
static void target_to_x0(tctx *t, int reg, int from_pr, sf_u32 add_const, sf_u32 avoid)
{
    emitter *e = &t->em;
    if (!from_pr && add_const == 0) { emit16(e, op_lds_slot(reg, SLOT_X0)); return; }
    {
        int rT = pick_scratch(t, avoid);
        spill(t, rT, SLOT_Y0);
        if (from_pr) emit16(e, OP_STS_PR(rT));
        else { emit_mov_imm(e, rT, add_const); emit16(e, OP_ADD_RR(reg, rT)); }
        emit16(e, op_lds_slot(rT, SLOT_X0));
        restore(t, rT, SLOT_Y0);
    }
}

/* PR = guest return address. With keep, the scratch stays spilled in Y0 for
 * the caller to reuse (and restore); returns it. */
static int set_pr(tctx *t, sf_u32 value, sf_u32 avoid, int keep)
{
    emitter *e = &t->em;
    int rT = pick_scratch(t, avoid);
    spill(t, rT, SLOT_Y0);
    emit_mov_imm(e, rT, value);
    emit16(e, OP_LDS_PR(rT));
    if (!keep) restore(t, rT, SLOT_Y0);
    return rT;
}

/* Instructions translated as a helper call own all five DSP slots. */
static int is_helper(const tctx *t, const sh_insn *s) { return s && (s->kind == K_FPU || s->kind == K_FALLBACK || dev_candidate(t, s)); }
static int slot_writes_pr(const sh_insn *s)
{
    if (!s) return 0;
    if (s->kind == K_PASS && (s->op & 0xF0FFu) == 0x402Au) return 1;           /* lds rm,pr */
    if (s->kind == K_MEM && s->ctrl == CR_PR && !s->is_store) return 1;        /* lds.l @rm+,pr */
    return 0;
}

static void tr_insn(tctx *t, const sh_insn *d, int in_slot);
static void begin_meta_kind(tctx *t, int kind, sf_u32 guest_off, sf_u32 target, sf_u32 fallthrough);

/* Target capture and delay slot of an indirect branch. Normally the target
 * goes to X0 first and the slot is translated with shifted scratch slots.
 * A slot that needs a helper call owns all five DSP slots, so for it the
 * target is captured afterwards; that needs the slot not to write the
 * target register (helpers never write PR). */
static void indirect_slot(tctx *t, const sh_insn *slot, int reg, int from_pr, sf_u32 add_const,
                          sf_u32 avoid, sf_u32 target_regs)
{
    int helper_slot = is_helper(t, slot);
    if (helper_slot) {
        if (slot->defs & target_regs) { t->err = DBT_ERR_UNSUPPORTED; return; }
        tr_insn(t, slot, 1);
        target_to_x0(t, reg, from_pr, add_const, avoid);
        return;
    }
    target_to_x0(t, reg, from_pr, add_const, avoid);
    if (slot) { t->slot_x0 = 1; tr_insn(t, slot, 1); t->slot_x0 = 0; }
}

static void tr_branch(tctx *t, const sh_insn *d, const sh_insn *slot)
{
    emitter *e = &t->em;
    sf_u32 next = t->gpc + 4;                     /* PC+4 semantics */
    sf_u32 avoid = slot ? slot->touched : 0;
    sf_u32 target = t->gpc + 4 + (sf_u32)d->br_disp;
    sf_u32 goff = t->gpc - t->blk->guest_pc;

    /* Order for every delayed branch: PR write, target capture (to X0),
     * delay slot, then a resume boundary, then the jump. An exception in the
     * slot restarts the whole branch (idempotent up to that point); an
     * exception after the boundary resumes at the target. */
    switch (d->br) {
    case BR_BRA:
        if (slot) tr_insn(t, slot, 1);
        begin_meta_kind(t, META_TARGET_CONST, goff, target, 0);
        jump_direct(t, target, 0, 0);
        return;
    case BR_BSR:
        set_pr(t, next, avoid, 0);
        if (slot) tr_insn(t, slot, 1);
        begin_meta_kind(t, META_TARGET_CONST, goff, target, 0);
        jump_direct(t, target, 0, 0);
        return;
    case BR_BRAF:
    case BR_BSRF:
        if (d->br == BR_BSRF) set_pr(t, next, avoid | R(d->br_reg), 0);
        if (is_helper(t, slot)) {
            /* a helper slot owns every DSP slot, so the target reaches X0
             * only after it; from the slot on the resume rule is the
             * register itself plus PC+4 (it is not written by the slot),
             * valid before X0 is loaded (an interrupt in the helper's exit
             * tail resumes at the committed call's next range) */
            if (slot->defs & R(d->br_reg)) { t->err = DBT_ERR_UNSUPPORTED; return; }
            t->slot_avoid = R(d->br_reg);
            tr_insn(t, slot, 1);
            t->slot_avoid = 0;
            begin_meta_kind(t, META_TARGET_REG, goff, next, 0);
            t->cur->fix_reg = (sf_u8)d->br_reg;
            target_to_x0(t, d->br_reg, 0, next, avoid | R(d->br_reg));
        } else {
            indirect_slot(t, slot, d->br_reg, 0, next, avoid | R(d->br_reg), R(d->br_reg));
            begin_meta_kind(t, META_TARGET_X0, goff, 0, 0);
        }
        exit_restore(t);
        jump_indirect_ic(t);
        return;
    case BR_JMP:
    case BR_JSR:
        if (slot && (slot->defs & R(d->br_reg))) {
            /* slot writes the target register: park the target in X0 first */
            if (d->br == BR_JSR) set_pr(t, next, avoid | R(d->br_reg), 0);
            indirect_slot(t, slot, d->br_reg, 0, 0, 0, R(d->br_reg));
            begin_meta_kind(t, META_TARGET_X0, goff, 0, 0);
            exit_restore(t);
            jump_indirect_ic(t);
        } else {
            /* fast form: the inline cache compares the register itself. The
             * PR scratch stays spilled across the slot unless the slot is a
             * helper call (helpers own Y0). */
            int rT = -1;
            if (d->br == BR_JSR) rT = set_pr(t, next, avoid | R(d->br_reg), !is_helper(t, slot));
            /* the slot's scratches: not the target register (a resume after
             * the slot's committed store reads it, before the undo: the CE
             * kernel's `jsr @r3; mov.l r1,@(12,r2)` sent a call to the
             * swapped store data, 2026-11-04) nor the kept PR scratch */
            t->slot_avoid = R(d->br_reg) | (rT >= 0 ? R(rT) : 0);
            if (slot) tr_insn(t, slot, 1);
            t->slot_avoid = 0;
            begin_meta_kind(t, META_TARGET_REG, goff, 0, 0);
            t->cur->fix_reg = (sf_u8)d->br_reg;
            exit_restore(t);
            /* a helper slot owned Y0: the PR scratch was restored before it
             * and the inline cache spills its own (it used to take rT as
             * still spilled and reloaded it from Y0, the helper's leftovers,
             * on the slow path of a device store) */
            jump_indirect_ic_reg(t, d->br_reg, is_helper(t, slot) ? -1 : rT);
        }
        return;
    case BR_RTS:
        if (slot_writes_pr(slot)) {
            indirect_slot(t, slot, -1, 1, 0, avoid, 0);
            begin_meta_kind(t, META_TARGET_X0, goff, 0, 0);
            exit_restore(t);
            jump_indirect_ic(t);
        } else {
            if (slot) tr_insn(t, slot, 1);
            begin_meta_kind(t, META_TARGET_PR, goff, 0, 0);
            exit_restore(t);
            jump_indirect_ic_pr(t);
        }
        return;
    case BR_RTE: {
        int rT;
        if (slot) tr_insn(t, slot, 1);
        begin_meta_kind(t, META_RTE, goff, 0, 0);
        exit_restore(t);
        rT = pick_scratch(t, 0);
        spill(t, rT, SLOT_Y0);
        emit_mov_lit(e, rT, t->env->rt_rte, TAG_NONE);
        emit16(e, OP_JMP(rT));
        restore(t, rT, SLOT_Y0);
        return;
    }
    case BR_BT:
    case BR_BF:
        /* inverted conditional over the 5-word direct jump */
        emit16(e, d->br == BR_BT ? OP_BF(4) : OP_BT(4));
        jump_direct(t, target, 0, 1);
        return;
    case BR_BTS:
    case BR_BFS:
        {
            /* resume pcs by T value: bt/s goes to target on T=1, bf/s on T=0 */
            sf_u32 pc_t1 = d->br == BR_BTS ? target : next;
            sf_u32 pc_t0 = d->br == BR_BTS ? next : target;
            if (!slot || !slot->writes_t) {
                if (slot) tr_insn(t, slot, 1);
                begin_meta_kind(t, META_COND, goff, pc_t1, pc_t0);
                emit16(e, d->br == BR_BTS ? OP_BF(4) : OP_BT(4));
                jump_direct(t, target, 0, 1);
                return;
            } else {
                /* slot writes T: branch on the old T (kept in rA), leave the
                 * slot's T visible. Each path after the split has its own
                 * resume range since rA is restored inside it. */
                int rA = pick_scratch(t, avoid);
                int rB = pick_scratch(t, avoid | R(rA));
                spill(t, rA, SLOT_Y0);
                emit16(e, OP_MOVT(rA));
                tr_insn(t, slot, 1);
                begin_meta_kind(t, META_COND_REG, goff, pc_t1, pc_t0);
                t->cur->fix_reg = (sf_u8)rA;
                spill(t, rB, SLOT_Y1);
                emit16(e, OP_MOVT(rB));
                emit16(e, OP_TST_RR(rA, rA));            /* T = (old T == 0) */
                emit16(e, d->br == BR_BTS ? OP_BF(4) : OP_BT(4));
                /* not taken */
                begin_meta_kind(t, META_TARGET_CONST, goff, next, 0);
                emit16(e, OP_SHLR(rB));                  /* T = slot's T */
                emit16(e, op_sts_slot(SLOT_Y1, rB));
                emit16(e, op_sts_slot(SLOT_Y0, rA));
                emit16(e, OP_BRA(8));                    /* over: shlr, 2 restores, 5-word jump */
                emit16(e, OP_NOP);
                /* taken */
                begin_meta_kind(t, META_TARGET_CONST, goff, target, 0);
                emit16(e, OP_SHLR(rB));
                restore(t, rB, SLOT_Y1);
                restore(t, rA, SLOT_Y0);
                jump_direct(t, target, 0, 0);
                return;
            }
        }
    }
}

/* ------------------------------------------------------------- privileged */

static void tr_priv(tctx *t, const sh_insn *d)
{
    emitter *e = &t->em;
    if (d->ctrl == CR_VBR) {
        if (d->priv_mem) {
            int rT = pick_scratch(t, R(d->base)), rU = pick_scratch(t, R(d->base) | R(rT));
            if (d->is_store) {                                  /* stc.l vbr,@-rn */
                spill(t, rT, SA(t));
                emit_mov_lit(e, rT, t->env->shadow_vbr_addr, TAG_NONE);
                emit16(e, OP_MOVL_LOAD(rT, rT));
                swap32(t, rT, rT);
                commit(t, FIX_NONE, 0);
                emit16(e, OP_MOVL_PREDEC(rT, d->base));
                restore(t, rT, SA(t));
            } else {                                            /* ldc.l @rm+,vbr */
                spill(t, rT, SA(t)); spill(t, rU, SB(t));
                commit(t, FIX_CR_BASE + CR_VBR, rT);
                emit16(e, OP_MOVL_POSTINC(d->base, rT));
                swap32(t, rT, rT);
                emit_mov_lit(e, rU, t->env->shadow_vbr_addr, TAG_NONE);
                emit16(e, OP_MOVL_STORE(rT, rU));
                restore(t, rU, SB(t)); restore(t, rT, SA(t));
            }
        } else if ((d->op & 0xF0FF) == 0x402E) {                /* ldc rm,vbr */
            int rm = (d->op >> 8) & 0xF, rT = pick_scratch(t, R(rm));
            spill(t, rT, SA(t));
            emit_mov_lit(e, rT, t->env->shadow_vbr_addr, TAG_NONE);
            emit16(e, OP_MOVL_STORE(rm, rT));
            restore(t, rT, SA(t));
        } else {                                                /* stc vbr,rn */
            int rn = (d->op >> 8) & 0xF;
            emit_mov_lit(e, rn, t->env->shadow_vbr_addr, TAG_NONE);
            emit16(e, OP_MOVL_LOAD(rn, rn));
        }
        return;
    }
    /* CR_SR */
    if (d->priv_mem && !d->is_store) {                          /* ldc.l @rm+,sr */
        int rT = pick_scratch(t, R(d->base));
        spill(t, rT, SLOT_X1);
        commit(t, FIX_CR_BASE + CR_SR, rT);                     /* after this point: not resumable (BL=1 code) */
        emit16(e, OP_MOVL_POSTINC(d->base, rT));
        swap32(t, rT, rT);
        emit16(e, op_lds_slot(rT, SLOT_X0));                    /* X0 = new SR */
        restore(t, rT, SLOT_X1);
        call_helper(t, t->env->rt_ldc_sr, 0, 0, 0, R(d->base));
        return;
    }
    if (!d->priv_mem && (d->op & 0xF0FF) == 0x400E) {           /* ldc rm,sr */
        call_helper(t, t->env->rt_ldc_sr, (sf_u32)((d->op >> 8) & 0xF), 1, 0, R((d->op >> 8) & 0xF));
        return;
    }
    {
        /* stc sr,rn / stc.l sr,@-rn : read SR, patch MD from shadow */
        int rn = d->priv_mem ? -1 : (int)((d->op >> 8) & 0xF);
        int dst = d->priv_mem ? pick_scratch(t, R(d->base)) : rn;
        int rT = pick_scratch(t, R(dst) | (d->priv_mem ? R(d->base) : 0));
#ifdef DBT_SR_ONE_SCRATCH
        /* Reuse one scratch: clear native MD, then merge guest MD.
         * Neither AND nor OR modifies SR.T. */
        if (d->priv_mem) spill(t, dst, SC(t));
        spill(t, rT, SA(t));
        emit16(e, OP_STC_SR(dst));
        emit_mov_lit(e, rT, ~SR_MD, TAG_NONE);
        emit16(e, 0x2009u | ((sf_u32)dst << 8) | ((sf_u32)rT << 4));
        emit_mov_lit(e, rT, t->env->shadow_md_addr, TAG_NONE);
        emit16(e, OP_MOVL_LOAD(rT, rT));
        emit16(e, 0x200Bu | ((sf_u32)dst << 8) | ((sf_u32)rT << 4));
        if (d->priv_mem) { swap32(t, dst, dst); commit(t, FIX_NONE, 0); emit16(e, OP_MOVL_PREDEC(dst, d->base)); }
        restore(t, rT, SA(t));
#else
        int rU = pick_scratch(t, R(dst) | R(rT) | (d->priv_mem ? R(d->base) : 0));
        if (d->priv_mem) spill(t, dst, SC(t));
        spill(t, rT, SA(t)); spill(t, rU, SB(t));
        emit16(e, OP_STC_SR(dst));
        emit_mov_lit(e, rT, t->env->shadow_md_addr, TAG_NONE);
        emit16(e, OP_MOVL_LOAD(rT, rT));
        emit_mov_lit(e, rU, ~SR_MD, TAG_NONE);
        emit16(e, 0x2009u | ((sf_u32)dst << 8) | ((sf_u32)rU << 4));   /* and rU,dst */
        emit16(e, 0x200Bu | ((sf_u32)dst << 8) | ((sf_u32)rT << 4));   /* or rT,dst */
        if (d->priv_mem) { swap32(t, dst, dst); commit(t, FIX_NONE, 0); emit16(e, OP_MOVL_PREDEC(dst, d->base)); }
        restore(t, rU, SB(t)); restore(t, rT, SA(t));
#endif
        if (d->priv_mem) restore(t, dst, SC(t));
    }
}

/* --------------------------------------------------------- exec trace */

/* Block entry record for the execution trace: ring[idx] = guest pc,
 * idx = (idx + 4) & 0xFFFF. Part of the first instruction's range, so an
 * exception inside just repeats the record. 13 instructions. */
static void trace_prologue(tctx *t)
{
    emitter *e = &t->em;
    int rT = pick_scratch(t, 0), rU = pick_scratch(t, R(rT));
    spill(t, rT, SA(t)); spill(t, rU, SB(t));
    emit_mov_lit(e, rT, t->env->exec_ctl, TAG_NONE);
    emit16(e, OP_MOVL_LOAD(rT, rU));
    emit16(e, OP_ADD_IMM(4, rU));
    emit16(e, OP_EXTUW(rU, rU));
    emit16(e, OP_MOVL_STORE(rU, rT));
    emit_mov_lit(e, rT, t->env->exec_ring, TAG_NONE);
    emit16(e, OP_ADD_RR(rU, rT));
    emit_mov_lit(e, rU, t->blk->guest_pc, TAG_NONE);
    emit16(e, OP_MOVL_STORE(rU, rT));
    restore(t, rU, SB(t)); restore(t, rT, SA(t));
}

/* ------------------------------------------- FPU moves done in line
 * Every FPU instruction went to fpuemu through the helper stub: the whole
 * guest context saved and restored, the decode, ~650 host instructions.
 * In Plotter-Z's recalculation 60% of the FPU instructions were register
 * moves: fmov FRm,FRn (31%), lds/sts fpscr/fpul (30%, the CE compiler
 * switches PR for every double), fldi0/1, flds/fsts (docs/08 section 10).
 * Those touch no memory and raise nothing, so they are done here against
 * fpuemu's register file (env->fpu_fr/fpscr/fpul), in about 15 host
 * instructions, when SR.FD is clear (the kernel's lazy FPU context switch
 * needs the 0x800 exception, which only the helper raises) and, for fmov,
 * FPSCR.SZ is clear (pair moves stay with fpuemu). Layout:
 *   spill a, v ; save SR (T) ; FD test -> slow ; [SZ test -> slow] ;
 *   FR bank test ; body ; SR back ; restore ; bra done
 *   slow: SR back ; restore ; the helper call as before
 *   done:
 * The slow region reopens the spill records so an interrupt there still
 * finds a, v and T in their slots. -DDBT_NO_FPU_INLINE keeps the helper
 * for everything. */
#ifndef DBT_NO_FPU_INLINE
static int spill_t_save(tctx *t, int v)          /* stc sr,v ; v -> SC: SR.T saved, as tr_p4_store does */
{
    dbt_block *b = t->blk;
    int idx;
    emit16(&t->em, OP_STC_SR(v));
    if (b->nspills >= DBT_MAX_BLOCK_SPILLS) { t->err = DBT_ERR_OVERFLOW; return -1; }
    idx = b->nspills++;
#ifndef DBT_COMPACT_SPILL6
    b->spills[idx].insn = (sf_u8)(t->cur - b->meta);
    b->spills[idx].pad = 0;
#endif
    b->spills[idx].gpr = DBT_SPILL_T; b->spills[idx].slot = (sf_u8)SC(t);
    b->spills[idx].start = (sf_u16)emit_pos(&t->em); b->spills[idx].end = 0xFFFF;
    emit16(&t->em, op_lds_slot(v, SC(t)));
    return idx;
}
static void spill_reopen(tctx *t, int gpr, int slot)   /* a record only: the slot already holds the value */
{
    dbt_block *b = t->blk;
    if (b->nspills >= DBT_MAX_BLOCK_SPILLS) { t->err = DBT_ERR_OVERFLOW; return; }
#ifndef DBT_COMPACT_SPILL6
    b->spills[b->nspills].insn = (sf_u8)(t->cur - b->meta);
    b->spills[b->nspills].pad = 0;
#endif
    b->spills[b->nspills].gpr = (sf_u8)gpr; b->spills[b->nspills].slot = (sf_u8)slot;
    b->spills[b->nspills].start = (sf_u16)emit_pos(&t->em); b->spills[b->nspills].end = 0xFFFF;
    b->nspills++;
}
static void spill_t_restore(tctx *t, int v)     /* SC -> v ; ldc v,sr: T back, record closed */
{
    dbt_block *b = t->blk;
    int i;
    emit16(&t->em, op_sts_slot(SC(t), v));
    for (i = b->nspills - 1; i >= 0; i--)
        if (b->spills[i].gpr == DBT_SPILL_T && b->spills[i].slot == SC(t) && b->spills[i].end == 0xFFFF) {
            b->spills[i].end = (sf_u16)emit_pos(&t->em);
            break;
        }
    emit16(&t->em, OP_LDC_SR(v));
}

static int tr_fpu_inline(tctx *t, const sh_insn *d, int in_slot)
{
    emitter *e = &t->em;
    sf_u32 op = d->op;
    int n = (op >> 8) & 15, m = (op >> 4) & 15;
    int kind, a, v, bf_fd, bf_sz = -1, bra_done, slow, done, skip;
    if (!t->env->fpu_fr) return 0;
    switch (op & 0xF0FFu) {
    case 0x405A: kind = 1; break;                 /* lds rm,fpul */
    case 0x406A: kind = 2; break;                 /* lds rm,fpscr */
    case 0x005A: kind = 3; break;                 /* sts fpul,rn */
    case 0x006A: kind = 4; break;                 /* sts fpscr,rn */
    case 0xF08D: kind = 5; break;                 /* fldi0 frn */
    case 0xF09D: kind = 6; break;                 /* fldi1 frn */
    case 0xF00D: kind = 7; break;                 /* fsts fpul,frn */
    case 0xF01D: kind = 8; break;                 /* flds frm,fpul */
    default:
        if ((op & 0xF00Fu) == 0xF00Cu) kind = 9;  /* fmov frm,frn */
        else return 0;
    }
    if (t->blk->nspills + 6 > DBT_MAX_BLOCK_SPILLS) return 0;
    a = pick_scratch(t, d->touched);
    v = pick_scratch(t, d->touched | (a >= 0 ? R(a) : 0));
    if (a < 0 || v < 0) return 0;
    spill(t, a, SA(t)); spill(t, v, SB(t));
    if (spill_t_save(t, v) < 0) return 0;
    /* SR.FD (bit 15): set -> the helper raises the FPU-disable exception */
    emit_mov_lit(e, a, 0x8000u, TAG_NONE);
    emit16(e, OP_TST_RR(a, v));
    bf_fd = emit_pos(e); emit16(e, OP_BF(0));
    switch (kind) {
    case 1: emit_mov_lit(e, a, t->env->fpu_fpul, TAG_NONE);  emit16(e, OP_MOVL_STORE(n, a)); break;   /* lds/sts name the register in bits 8-11 */
    case 2: emit_mov_lit(e, v, 0x003FFFFFu, TAG_NONE);       /* FPSCR_MASK, as fpuemu's lds */
            emit16(e, OP_AND_RR(n, v));
            emit_mov_lit(e, a, t->env->fpu_fpscr, TAG_NONE); emit16(e, OP_MOVL_STORE(v, a)); break;
    case 3: emit_mov_lit(e, a, t->env->fpu_fpul, TAG_NONE);  emit16(e, OP_MOVL_LOAD(a, n)); break;
    case 4: emit_mov_lit(e, a, t->env->fpu_fpscr, TAG_NONE); emit16(e, OP_MOVL_LOAD(a, n)); break;
    default:
        /* the bank: FPSCR.FR (bit 21) adds 64 bytes; fmov also needs SZ (bit 20) clear */
        emit_mov_lit(e, a, t->env->fpu_fpscr, TAG_NONE); emit16(e, OP_MOVL_LOAD(a, v));
        if (kind == 9) {
            emit_mov_lit(e, a, 0x00100000u, TAG_NONE); emit16(e, OP_TST_RR(a, v));
            bf_sz = emit_pos(e); emit16(e, OP_BF(0));
        }
        emit_mov_lit(e, a, 0x00200000u, TAG_NONE); emit16(e, OP_TST_RR(a, v));   /* T = bank 0 */
        switch (kind) {
        case 5: case 6:                                                             /* fldi0/1 frn */
            emit_mov_lit(e, a, t->env->fpu_fr + 4u * n, TAG_NONE);
            skip = emit_pos(e); emit16(e, OP_BT(0)); emit16(e, OP_ADD_IMM(64, a));
            emit_patch16(e, skip, OP_BT((emit_pos(e) - skip - 4) / 2));
            if (kind == 5) emit16(e, OP_MOV_IMM(0, v)); else emit_mov_lit(e, v, 0x3F800000u, TAG_NONE);
            emit16(e, OP_MOVL_STORE(v, a));
            break;
        case 7:                                                                     /* fsts fpul,frn */
            emit_mov_lit(e, a, t->env->fpu_fr + 4u * n, TAG_NONE);
            skip = emit_pos(e); emit16(e, OP_BT(0)); emit16(e, OP_ADD_IMM(64, a));
            emit_patch16(e, skip, OP_BT((emit_pos(e) - skip - 4) / 2));
            emit_mov_lit(e, v, t->env->fpu_fpul, TAG_NONE); emit16(e, OP_MOVL_LOAD(v, v));
            emit16(e, OP_MOVL_STORE(v, a));
            break;
        case 8:                                                                     /* flds frm,fpul: the register is in bits 8-11 */
            emit_mov_lit(e, a, t->env->fpu_fr + 4u * n, TAG_NONE);
            skip = emit_pos(e); emit16(e, OP_BT(0)); emit16(e, OP_ADD_IMM(64, a));
            emit_patch16(e, skip, OP_BT((emit_pos(e) - skip - 4) / 2));
            emit16(e, OP_MOVL_LOAD(a, a));
            emit_mov_lit(e, v, t->env->fpu_fpul, TAG_NONE); emit16(e, OP_MOVL_STORE(a, v));
            break;
        default:                                                                    /* fmov frm,frn, SZ = 0 */
            emit_mov_lit(e, a, t->env->fpu_fr + 4u * m, TAG_NONE);
            emit_mov_lit(e, v, t->env->fpu_fr + 4u * n, TAG_NONE);
            skip = emit_pos(e); emit16(e, OP_BT(0)); emit16(e, OP_ADD_IMM(64, a)); emit16(e, OP_ADD_IMM(64, v));
            emit_patch16(e, skip, OP_BT((emit_pos(e) - skip - 4) / 2));
            emit16(e, OP_MOVL_LOAD(a, a)); emit16(e, OP_MOVL_STORE(a, v));
            break;
        }
        break;
    }
    spill_t_restore(t, v);
    restore(t, v, SB(t)); restore(t, a, SA(t));
    bra_done = emit_pos(e); emit16(e, OP_BRA(0)); emit16(e, OP_NOP);
    /* slow: everything as the helper expects, then the helper */
    slow = emit_pos(e);
    emit_patch16(e, bf_fd, OP_BF((slow - bf_fd - 4) / 2));
    if (bf_sz >= 0) emit_patch16(e, bf_sz, OP_BF((slow - bf_sz - 4) / 2));
    spill_reopen(t, a, SA(t)); spill_reopen(t, v, SB(t)); spill_reopen(t, DBT_SPILL_T, SC(t));
    spill_t_restore(t, v);
    restore(t, v, SB(t)); restore(t, a, SA(t));
    call_helper(t, t->env->rt_fpu, 0, 2, d->op | (in_slot ? 0x10000u : 0), d->touched);
    done = emit_pos(e);
    emit_patch16(e, bra_done, OP_BRA((done - bra_done - 4) / 2));
    return t->err == 0;
}
#endif

/* ------------------------------------------------------------- dispatcher */

static void tr_insn(tctx *t, const sh_insn *d, int in_slot)
{
    if (t->err) return;
    switch (d->kind) {
    case K_PASS: case K_TRAP: case K_ILLEGAL:
        /* (a hypercall trapa resumes inside the block: the exception path
         * restores the DSP slots, so the borrow survives it) */
        emit16(&t->em, d->op); break;
    case K_MEM:   tr_mem(t, d); break;
    case K_PCREL: tr_pcrel(t, d); break;
    case K_FPU:
#ifndef DBT_NO_FPU_INLINE
        if (tr_fpu_inline(t, d, in_slot)) break;
#endif
        call_helper(t, t->env->rt_fpu, 0, 2, d->op | (in_slot ? 0x10000u : 0), d->touched); break;   /* bit 16: delay slot (FPU-disable raises 0x820 there) */
    case K_FALLBACK: call_helper(t, t->env->rt_fallback, 0, 2, d->op, d->touched); break;
    case K_PRIV:  tr_priv(t, d); break;
    case K_BRANCH: /* handled by caller */ break;
    }
#ifdef DBT_CONST_DEVICE_ADDRESS
    /* Keep only constants actually materialized by PC-relative loads.
     * Any non-memory operation conservatively drops all knowledge. */
    if (d->kind == K_MEM) t->known_mask &= ~d->defs;
    else if (d->kind != K_PCREL) t->known_mask = 0;
#endif
#ifdef DBT_UNIFORM_STORE
    if (d->kind != K_PCREL) {
        sf_u32 known = t->uniform_mask;
        int n = (d->op >> 8) & 15, m = (d->op >> 4) & 15;
        t->uniform_mask &= ~d->defs;
        if (d->kind == K_PASS) {
            if ((d->op & 0xf000) == 0xe000 &&
                ((d->op & 255) == 0 || (d->op & 255) == 255)) {
                t->uniform_mask |= R(n);
#ifdef DBT_UNIFORM_ENTRY_BOUNDS
                t->uniform_origin[n] = (sf_u8)((t->gpc - t->blk->guest_pc) / 2);
#endif
            } else if ((d->op & 0xf00f) == 0x6003 && (known & R(m))) {
                t->uniform_mask |= R(n);
#ifdef DBT_UNIFORM_ENTRY_BOUNDS
                t->uniform_origin[n] = t->uniform_origin[m];
#endif
            }
        } else if (d->kind != K_MEM) t->uniform_mask = 0;
    }
#endif
    /* the cached swap dies with its register; anything but plain code,
     * memory and pc-relative ops is a helper or trap and kills it too */
    if (t->cache_reg >= 0 && ((d->defs & R(t->cache_reg)) || (d->kind != K_PASS && d->kind != K_MEM && d->kind != K_PCREL)))
        cache_kill(t);
}

static void begin_meta_kind(tctx *t, int kind, sf_u32 guest_off, sf_u32 target, sf_u32 fallthrough)
{
    dbt_block *b = t->blk;
    dbt_imeta *m;
    if (b->nmeta >= DBT_MAX_META - 1) { t->err = DBT_ERR_OVERFLOW; return; }
    m = &b->meta[b->nmeta++];
#ifndef DBT_META_MEMSET          /* DBT_META_WORD_INIT until 2026-11-07 */
    {
        /* Write the complete recovery record once instead of clearing it
         * and overwriting most fields. Explicit host byte order keeps the
         * native SH build and host-side recovery tests identical. */
        typedef sf_u32 meta_word __attribute__((__may_alias__));
        typedef char check_meta_words[(sizeof(dbt_imeta) == 16 &&
            __builtin_offsetof(dbt_imeta, kind) == 4 &&
            __builtin_offsetof(dbt_imeta, target) == 8 &&
            __builtin_offsetof(dbt_imeta, fallthrough) == 12 &&
            __alignof__(dbt_imeta) >= 4) ? 1 : -1];
        meta_word *w = (meta_word *)m;
        sf_u32 host = (sf_u16)emit_pos(&t->em), guest = (sf_u16)guest_off;
        (void)sizeof(check_meta_words);
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        w[0] = (host << 16) | guest;
        w[1] = ((sf_u32)(sf_u8)kind << 24) | 0x00ff0000u;
#else
        w[0] = host | (guest << 16);
        w[1] = (sf_u8)kind | 0x0000ff00u;
#endif
        w[2] = target; w[3] = fallthrough;
    }
#else
    memset(m, 0, sizeof *m);
    m->host_off = (sf_u16)emit_pos(&t->em);
    m->guest_off = (sf_u16)guest_off;
    m->kind = (sf_u8)kind;
    m->commit_rel = 0xFF;
    m->target = target;
    m->fallthrough = fallthrough;
#endif
    t->cur = m;
}

static void begin_meta(tctx *t, sf_u32 guest_off) { begin_meta_kind(t, META_INSN, guest_off, 0, 0); }

/* The instruction at gpc: the pre-pass's record itself (no copy; g_pre is
 * not written again during this translation), else decoded into *d. NULL
 * on a fetch fault. */
static const sh_insn *fetch(tctx *t, sf_u32 gpc, sh_insn *d)
{
    sf_u32 op, i = (gpc - t->blk->guest_pc) / 2;
    if (i < (sf_u32)t->pre_n) return &g_pre[i];                     /* decoded by the pre-pass */
#ifdef DBT_NATIVE_FETCH
#if !defined(__sh__) || __BYTE_ORDER__ != __ORDER_BIG_ENDIAN__
#error DBT_NATIVE_FETCH requires the native big-endian SH runtime
#endif
    /* Native runtime's fetch16 reads this same mapped address. Keep the
     * alignment rejection and volatile access; MMU exceptions still reach
     * the runtime's in-progress translation recovery. No read-ahead. */
    if (gpc & 1) { t->err = DBT_ERR_FETCH; t->fault = gpc; return 0; }
    op = *(volatile sf_u16 *)gpc;
    op = ((op >> 8) | (op << 8)) & 0xffffu;
#else
    if (t->env->fetch16(t->env->ctx, gpc, &op)) { t->err = DBT_ERR_FETCH; t->fault = gpc; return 0; }
#endif
    sh4_decode(op, d);
    return d;
}

#ifdef DBT_DECODE_REF
static const sh_insn *fetch_ref(tctx *t, sf_u32 gpc)
{
    sf_u32 op, i = (gpc - t->blk->guest_pc) / 2;
    if (i < (sf_u32)t->pre_n) return &g_pre[i];
    if (t->env->fetch16(t->env->ctx, gpc, &op)) {
        t->err = DBT_ERR_FETCH; t->fault = gpc; return 0;
    }
    return sh4_decode_cached(op);
}
#endif

/* Would this instruction use the borrowed register? (stores with a free
 * data field, control-register memory forms, device-aware accesses) */
static int borrow_user(const tctx *t, const sh_insn *d)
{
    if (d->kind != K_MEM) return 0;
    if (d->ctrl != CR_NONE) return 1;
    if (d->is_store && d->size > 1 && !store_data_is_fixed_r0(d)) return 1;
    return dev_candidate(t, d);
}

/* Pre-pass: decode the block ahead under the main loop's end conditions
 * (the loop may still stop earlier, never later) to pick the borrowed
 * register, a GPR no instruction touches, when the block profits: two
 * instructions per user against the entry, two per helper call (which
 * pauses the borrow) and one for an indirect exit. The decoded
 * instructions are kept for the main loop, which decodes nothing twice
 * (the second decode cost a tenth of a CE boot). */
static void prepass(tctx *t, sf_u32 guest_pc, int max)
{
    sf_u32 gpc = guest_pc, touched = 0;
    int n = 0, score = -1, r;
    t->bx = -1; t->cache_reg = -1; t->pre_n = 0;
    /* The pre-pass exists to choose an untouched borrowed register. With
     * borrowing disabled, decode directly in the emitter instead of copying
     * every instruction through g_pre and scanning its register uses. */
    if (t->env->no_borrow) return;
    while (n < max) {
        sh_insn *d = &g_pre[t->pre_n];
        sf_u32 op;
        if (t->env->fetch16(t->env->ctx, gpc, &op)) break;
        t->pre_n++;
        sh4_decode(op, d);
        touched |= d->touched;
        /* Hypercalls are handled directly from the live register context,
         * without undoing the block's borrowed register. Their implicit
         * argument/result registers must therefore never be borrowed. */
        if (d->kind == K_TRAP && (d->op & 0xfff0u) == 0xc3f0u)
            touched |= R(0) | R(4) | R(5) | R(6) | R(7);
        t->gpc = gpc;
        n++;
        gpc += 2;
        if (d->kind == K_BRANCH) {
            if (d->has_slot) {
                sh_insn *sl = &g_pre[t->pre_n];
                if (t->env->fetch16(t->env->ctx, gpc, &op)) break;
                t->pre_n++;
                sh4_decode(op, sl);
                touched |= sl->touched;
                gpc += 2;
                if (!sh4_slot_legal(sl)) break;
                if (borrow_user(t, sl)) score += 2;
                if (sl->kind == K_FPU || sl->kind == K_FALLBACK) score -= 2;
            }
            if (d->br != BR_BT && d->br != BR_BF && d->br != BR_BTS && d->br != BR_BFS) {
                if (d->br != BR_BRA && d->br != BR_BSR) score -= 1;
                break;
            }
#ifdef DBT_BASIC_BLOCKS
            break;
#endif
            continue;
        }
        if (borrow_user(t, d)) score += 2;
        if (d->kind == K_FPU || d->kind == K_FALLBACK || (d->kind == K_PRIV && d->ctrl == CR_SR && !d->is_store)) score -= 2;
        if (d->kind == K_TRAP && (d->op & 0xFF00) == 0xC300) break;
        if (d->kind == K_PRIV && d->ctrl == CR_SR && !d->is_store) break;
        if (d->kind == K_ILLEGAL) break;
    }
    if (t->env->no_borrow || score <= 0) return;
    for (r = 1; r <= 14; r++) if (!(touched & R(r))) { t->bx = r; return; }
}

int dbt_translate_block(const dbt_env *env, sf_u32 guest_pc, sf_u32 host_base,
                        sf_u16 *buf, int cap, dbt_block *out, sf_u32 *fault_addr)
{
    tctx t;
    dbt_block *b = out;
    int max = env->max_insns > 0 && env->max_insns <= DBT_MAX_INSNS ? env->max_insns : DBT_MAX_INSNS;
    int n = 0, ended = 0, k, pool;
    sf_u32 gpc = guest_pc;

    {
        dbt_imeta *m = b->meta; dbt_spill *s = b->spills; dbt_link *l = b->links;      /* caller-provided storage survives the reset */
        memset(b, 0, sizeof *b);
        b->meta = m; b->spills = s; b->links = l;
    }
    /* Literal slots are fully written by emit_mov_lit before nlits exposes
     * them. Do not clear the 768-byte unused array on every translation. */
    t.env = env; t.blk = b; t.gpc = guest_pc; t.cur = 0;
    t.err = 0; t.fault = 0; t.slot_x0 = 0; t.slot_avoid = 0;
    t.bx = -1; t.cache_reg = -1; t.cache_size = 0; t.pre_n = 0;
#ifdef DBT_UNIFORM_STORE
    t.uniform_mask = 0;
#endif
#ifdef DBT_CONST_DEVICE_ADDRESS
    t.known_mask = 0;
#endif
    emit_init(&t.em, buf, cap);
#ifdef DBT_WATCH_GPC
    /* -DDBT_WATCH_GPC=0x...: the block starting at that guest pc begins with
     * trapa #0xEE, so every entry reaches rt.c's HC_WATCH, which prints the
     * guest registers and the string r4 points at (diagnostic). */
    if (guest_pc == (sf_u32)(DBT_WATCH_GPC)) emit16(&t.em, 0xC3EE);
#endif
    b->guest_pc = guest_pc; b->host_base = host_base; b->end_reason = END_FULL; b->ic_off = -1;
    prepass(&t, guest_pc, max);
    b->borrow = t.bx;

    while (!ended && n < max && !t.err) {
        sh_insn decoded, s;
        const sh_insn *d;
        /* size budget: code + worst-case tail must stay within literal reach */
#ifdef DBT_BODY_BUDGET
        if (emit_pos(&t.em) > DBT_BODY_BUDGET || emit_lits_bytes(&t.em) > 160 || b->nlinks >= DBT_MAX_LINKS - 1) break;
#else
        if (emit_pos(&t.em) > 560 || emit_lits_bytes(&t.em) > 160 || b->nlinks >= DBT_MAX_LINKS - 1) break;
#endif
        t.gpc = gpc;
#ifdef DBT_DECODE_REF
        d = fetch_ref(&t, gpc);
        if (!d) break;
#else
        if (!(d = fetch(&t, gpc, &decoded))) break;
#endif
#ifdef DBT_LINEAR_META
        if (n > 0 && d->kind == K_PASS) {
            dbt_imeta *prev = b->nmeta ? &b->meta[b->nmeta - 1] : 0;
            if (prev && prev->kind == META_LINEAR &&
                prev->guest_off + emit_pos(&t.em) - prev->host_off == gpc - guest_pc)
                t.cur = prev;
            else begin_meta_kind(&t, META_LINEAR, gpc - guest_pc, 0, 0);
        } else
#endif
        begin_meta(&t, gpc - guest_pc);
        if (n == 0 && env->exec_ring) trace_prologue(&t);
        if (n == 0 && t.bx >= 0) spill(&t, t.bx, SLOT_A0);        /* park the borrowed register */
        n++;
        if (d->kind == K_BRANCH) {
            const sh_insn *slot = 0;
            if (d->has_slot) {
#ifdef DBT_DECODE_REF
                /* Decoding the slot can evict the branch's cache entry. */
                decoded = *d; d = &decoded;
#endif
                if (!(slot = fetch(&t, gpc + 2, &s))) break;
                if (!sh4_slot_legal(slot)) { /* let hardware raise slot-illegal: pass both through */
                    emit16(&t.em, d->op); emit16(&t.em, slot->op);
                    gpc += 4; b->end_reason = END_ILLEGAL; ended = 1; break;
                }
            }
#ifdef DBT_CONST_DEVICE_ADDRESS
            t.known_mask = 0;
#endif
#ifdef DBT_UNIFORM_STORE
            t.uniform_mask = 0;
#endif
            tr_branch(&t, d, slot);
#ifdef DBT_UNIFORM_STORE
            t.uniform_mask = 0;
#endif
#ifdef DBT_CONST_DEVICE_ADDRESS
            t.known_mask = 0;
#endif
            gpc += d->has_slot ? 4 : 2;
            if (d->br != BR_BT && d->br != BR_BF && d->br != BR_BTS && d->br != BR_BFS) { ended = 1; b->end_reason = END_BRANCH; }
#ifdef DBT_BASIC_BLOCKS
            /* Both successors are independently cached-> Do not duplicate a
             * fall-through trace whenever another branch enters its middle. */
            if (!ended) break;
#endif
            continue;
        }
        tr_insn(&t, d, 0);
        gpc += 2;
        if (d->kind == K_TRAP && (d->op & 0xFF00) == 0xC300) { ended = 1; b->end_reason = END_TRAP; }
        else if (d->kind == K_PRIV && d->ctrl == CR_SR && !d->is_store) { ended = 1; b->end_reason = END_PRIV; }
        else if (d->kind == K_ILLEGAL) { ended = 1; b->end_reason = END_ILLEGAL; }
    }

    if (t.err == DBT_ERR_FETCH || t.err == DBT_ERR_READ) {
        if (fault_addr) *fault_addr = t.fault;
        b->guest_len = gpc - guest_pc;
        return t.err;
    }
    if (t.err) return t.err;

    b->guest_len = gpc - guest_pc;
    /* fall-through exit: also after trapa / ldc sr / illegal, where execution
     * resumes at the next guest instruction. The jump belongs to the
     * sentinel range (resume = next guest pc). */
    b->meta[b->nmeta].host_off = (sf_u16)emit_pos(&t.em);
    b->meta[b->nmeta].guest_off = (sf_u16)b->guest_len;
    b->meta[b->nmeta].kind = META_INSN;
    b->meta[b->nmeta].commit_rel = 0xFF;
    if (!ended || b->end_reason != END_BRANCH) {
        t.cur = &b->meta[b->nmeta];
        t.gpc = gpc - 2;                       /* "branch" pc for the backward flag: never backward */
        jump_direct(&t, gpc, 0, 0);
    }
    b->code_len = emit_pos(&t.em);

    /* exit stubs: one per link. Enter with all guest registers intact. */
    for (k = 0; k < b->nlinks; k++) {
        int stub_off = emit_pos(&t.em);
        int rS = 1;
        int i;
        b->links[k].stub_off = (sf_u16)stub_off;
        /* record the stub address as the initial value of the link literal */
        for (i = 0; i < t.em.nlits; i++)
            if (t.em.lits[i].tag == TAG_LINK(k)) t.em.lits[i].value = host_base + (sf_u32)stub_off;
        emit16(&t.em, op_lds_slot(rS, SLOT_Y0));
        emit_mov_lit(&t.em, rS, b->links[k].guest_target, TAG_STUB_T(k));
        emit16(&t.em, op_lds_slot(rS, SLOT_X0));
        emit_mov_lit(&t.em, rS, 0, TAG_STUB_L(k));              /* address of the link literal, patched below */
        emit16(&t.em, op_lds_slot(rS, SLOT_X1));
        emit_mov_lit(&t.em, rS, env->rt_lookup_link, TAG_NONE);
        emit16(&t.em, OP_JMP(rS));
        emit16(&t.em, op_sts_slot(SLOT_Y0, rS));
    }
    pool = emit_flush_lits(&t.em);
    /* The emitter rejects failed literal sites; reject failed jump slots
     * before inspecting them. No further instructions are emitted below. */
    if (t.em.overflow) return DBT_ERR_OVERFLOW;
    if (env->reloc_map) {
        /* Pool words holding an address inside this block (helper return
         * addresses, exit-stub link-literal addresses, the IC pair's
         * address): what a move must rebase. Link literals are left out;
         * a moved block is unchained and they are rewritten from links. */
        int nl = -t.em.nlits, i;
        memset(env->reloc_map, 0, DBT_RELOC_BYTES);
        *env->reloc_pool = (sf_u16)pool;
        for (i = 0; i < nl; i++) {
            int tag = t.em.lits[i].tag;
            if (tag == TAG_RET || tag == TAG_IC_A || (tag >= TAG_STUB_L(0) && tag < TAG_STUB_L(DBT_MAX_LINKS))) {
                unsigned w = (unsigned)(t.em.lits[i].at - pool) / 4;
                env->reloc_map[w >> 3] |= (sf_u8)(1u << (w & 7));
            }
        }
    }
    {
        int g_off = emit_lit_offset(&t.em, TAG_IC_G), a_off = emit_lit_offset(&t.em, TAG_IC_A);
        if (g_off >= 0 && a_off >= 0) {
            sf_u32 v = host_base + (sf_u32)g_off;
            b->ic_off = g_off;
            emit_patch16(&t.em, a_off, v >> 16); emit_patch16(&t.em, a_off + 2, v & 0xFFFFu);
        }
    }
    for (k = 0; k < b->nlinks; k++) {
        int link_off = emit_lit_offset(&t.em, TAG_LINK(k));
        int l_off = emit_lit_offset(&t.em, TAG_STUB_L(k));
        sf_u32 v = host_base + (sf_u32)link_off;
        b->links[k].lit_off = (sf_u16)link_off;
        if (l_off >= 0) { emit_patch16(&t.em, l_off, v >> 16); emit_patch16(&t.em, l_off + 2, v & 0xFFFFu); }
        /* original jump words, for unchaining when the target block dies */
        b->links[k].w0 = t.em.buf[b->links[k].jump_off / 2];
        b->links[k].w1 = t.em.buf[b->links[k].jump_off / 2 + 1];
        b->links[k].w2 = t.em.buf[b->links[k].jump_off / 2 + 2];
        b->links[k].skip_w = b->links[k].cond ? t.em.buf[b->links[k].jump_off / 2 - 1] : 0;
    }
    b->host_len = emit_pos(&t.em);
    return DBT_OK;
}

int dbt_chain_direct(sf_u16 *code, sf_u32 host, sf_u32 target, int cond, int backward, int no_restore, sf_u32 *from)
{
    sf_i32 d = (sf_i32)(target - (host + 4));
    if (d & 1) return 0;
    d >>= 1;
    if (d < -2048 || d > 2047) return 0;
    if (cond && backward) {
        sf_i32 d8 = (sf_i32)(target - (host - 2 + 4)) >> 1;
        if (d8 >= -128 && d8 <= 127) {
            if (code[0] == 0x0009 || no_restore) {
                /* nothing to restore: the skip word, flipped (bit 9), becomes
                 * the real branch; word 0 a bra over the 4 remaining words */
                code[-1] = (sf_u16)(((code[-1] ^ 0x0200u) & 0xFF00u) | ((sf_u32)d8 & 0xFFu));
                code[0] = 0xA003;
                code[1] = 0x0009;
                *from = host - 2;
                return 6;
            }
            /* skip word: flip the sense (bit 9), delay it (bit 10, word 0
             * becomes its slot) and point it at the target */
            code[-1] = (sf_u16)((((code[-1] ^ 0x0200u) | 0x0400u) & 0xFF00u) | ((sf_u32)d8 & 0xFFu));
            code[1] = 0xA002;                  /* not taken: bra over the 3 remaining words */
            code[2] = 0x0009;
            *from = host - 2;
            return 8;
        }
    }
    code[1] = no_restore ? 0x0009 : code[0];   /* the restore (or nop) is the delay slot */
    code[0] = (sf_u16)(0xA000u | ((sf_u32)d & 0xFFFu));
    *from = host;
    return 4;
}

unsigned dbt_pack_meta(void *out, const dbt_block *b)
{
    dbt_pmeta *p = (dbt_pmeta *)out;
    sf_u32 *aux = (sf_u32 *)((sf_u8 *)out + dbt_meta_aux_offset(b->nmeta));
    unsigned i, naux = 0;
    for (i = 0; i <= b->nmeta; ++i) {
        const dbt_imeta *m = &b->meta[i];
        p[i].host_off = m->host_off; p[i].guest_half = (sf_u8)(m->guest_off / 2);
        p[i].kind_reg = (sf_u8)(m->kind | (m->fix_reg << 4)); p[i].commit_rel = m->commit_rel;
        p[i].fix_kind = m->fix_kind;
        if (dbt_meta_has_targets(m->kind)) {
            p[i].guest_half = (sf_u8)naux;
            aux[naux * 2] = m->target; aux[naux * 2 + 1] = m->fallthrough;
            ++naux;
        }
    }
    return dbt_meta_aux_offset(b->nmeta) + naux * 8;
}

int dbt_block_find(const dbt_block *b, sf_u32 host_off)
{
    int lo = 0, hi = b->nmeta;          /* include the sentinel range */
    if (host_off >= (sf_u32)b->code_len) return -1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        sf_u32 at = b->packed_meta ? ((const dbt_pmeta *)b->meta)[mid].host_off : b->meta[mid].host_off;
        if (at <= host_off) lo = mid; else hi = mid - 1;
    }
    return lo;
}

int dbt_block_pending_spills(const dbt_block *b, int idx, sf_u32 host_off, dbt_spill *out, int max)
{
    int i, n = 0;
    (void)idx;
#ifdef DBT_SPILL_START_SEARCH
    /* Spills are appended in emission order. Exclude saves that have not
     * executed, but retain the reverse order of nested active saves. */
    {
        int lo = 0, hi = b->nspills;
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (b->spills[mid].start < host_off) lo = mid + 1;
            else hi = mid;
        }
        i = lo - 1;
    }
#else
    i = b->nspills - 1;
#endif
    for (; i >= 0 && n < max; i--) {       /* latest first, outermost last */
        const dbt_spill *s = &b->spills[i];
        if (s->start < host_off && host_off <= s->end)
            out[n++] = *s;
    }
    return n;
}

int dbt_block_entry(const dbt_block *b, sf_u32 guest_pc)
{
    unsigned i;
#if defined(DBT_CONST_DEVICE_ADDRESS) || defined(DBT_UNIFORM_STORE)
    if (b->const_specialized && guest_pc < b->guest_pc + 2u * b->const_specialized
#ifdef DBT_UNIFORM_ENTRY_BOUNDS
        && guest_pc > b->guest_pc + 2u * b->uniform_entry_start
#endif
       ) return -1;
#endif
    if (b->borrow >= 0 || guest_pc <= b->guest_pc ||
        guest_pc >= b->guest_pc + b->guest_len || (guest_pc & 1)) return -1;
    for (i = 0; i < b->nmeta; ++i) {
        dbt_imeta m;
        dbt_spill spill;
        sf_u32 off;
        dbt_get_meta(b, i, &m);
        if (m.kind == META_INSN && b->guest_pc + m.guest_off == guest_pc) off = m.host_off;
        else if (m.kind == META_LINEAR && guest_pc >= b->guest_pc + m.guest_off) {
            dbt_imeta next;
            dbt_get_meta(b, i + 1, &next);
            off = m.host_off + guest_pc - b->guest_pc - m.guest_off;
            if (off >= next.host_off) continue;
        } else continue;
        if (off >= b->code_len || dbt_block_pending_spills(b, i, off, &spill, 1)) return -1;
        return (int)off;
    }
    return -1;
}
