#include <string.h>
#include "translate.h"

#define R(n) (1u << (n))
#define SR_MD 0x40000000u

/* literal tags */
#define TAG_LINK(k)   (100 + (k))   /* branch literal holding the host target (patched when linked) */
#define TAG_STUB_T(k) (200 + (k))   /* exit stub: guest target */
#define TAG_STUB_L(k) (300 + (k))   /* exit stub: address of the link literal */
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
} tctx;

/* DSP slots for scratch spills. Inside the delay slot of an indirect branch
 * X0 holds the target, so the slots shift: X0->X1, X1->Y1, Y1->A0. */
static int SA(const tctx *t) { return t->slot_x0 ? SLOT_X1 : SLOT_X0; }
static int SB(const tctx *t) { return t->slot_x0 ? SLOT_Y1 : SLOT_X1; }
static int SC(const tctx *t) { return t->slot_x0 ? SLOT_A0 : SLOT_Y1; }

/* Mark the memory operation about to be emitted as the range's commit point. */
static void commit(tctx *t, int fix_kind, int fix_reg)
{
    dbt_imeta *m = t->cur;
    sf_u32 rel = (sf_u32)emit_pos(&t->em) - m->host_off;
    if (rel >= 0xFF || m->commit_rel != 0xFF) { t->err = DBT_ERR_INTERNAL; return; }
    m->commit_rel = (sf_u8)rel; m->fix_kind = (sf_u8)fix_kind; m->fix_reg = (sf_u8)fix_reg;
}

/* ------------------------------------------------------------ helpers */

static int pick_scratch(sf_u32 avoid)
{
    int r;
    for (r = 1; r <= 14; r++) if (!(avoid & R(r))) return r;
    return -1;
}

static void spill(tctx *t, int gpr, int slot)
{
    dbt_block *b = t->blk;
    if (b->nspills < DBT_MAX_BLOCK_SPILLS) {
        dbt_spill *s = &b->spills[b->nspills++];
        s->insn = (sf_u8)(t->cur - b->meta);
        s->gpr = (sf_u8)gpr; s->slot = (sf_u8)slot; s->pad = 0;
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

static void tr_mem(tctx *t, const sh_insn *d)
{
    emitter *e = &t->em;
    if (d->ctrl != CR_NONE) {
        /* sts.l/stc.l CR,@-Rn  or  lds.l/ldc.l @Rm+,CR */
        int rT = pick_scratch(R(d->base));
        spill(t, rT, SA(t));
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
        restore(t, rT, SA(t));
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
    if (!store_data_is_fixed_r0(d)) {
        int rT = pick_scratch(d->touched);
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
        int rA = pick_scratch(R(0));
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
        int rA = pick_scratch(R(0));
        int rV = pick_scratch(R(0) | R(rA));
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
}

/* ------------------------------------------------------------- helper calls */

/* X0 = arg, X1 = return address (host), jump to helper. rT is spilled in Y0
 * and restored in the jump's delay slot. */
/* arg_mode: 0 = X0 already holds the argument, 1 = X0 <- register arg_reg,
 * 2 = X0 <- immediate arg_imm */
static void call_helper(tctx *t, sf_u32 helper, sf_u32 arg_reg, int arg_mode, sf_u32 arg_imm, sf_u32 avoid)
{
    emitter *e = &t->em;
    int rT = pick_scratch(avoid);
    int ret_lit;
    if (arg_mode == 1) emit16(e, op_lds_slot((int)arg_reg, SLOT_X0));
    spill(t, rT, SLOT_Y0);
    if (arg_mode == 2) { emit_mov_imm(e, rT, arg_imm); emit16(e, op_lds_slot(rT, SLOT_X0)); }
    ret_lit = emit_mov_lit(e, rT, 0, TAG_NONE);              /* return address, patched below */
    emit16(e, op_lds_slot(rT, SLOT_X1));
    emit_mov_lit(e, rT, helper, TAG_NONE);
    emit16(e, OP_JMP(rT));
    restore(t, rT, SLOT_Y0);
    if (ret_lit >= 0) e->lits[ret_lit].value = t->blk->host_base + (sf_u32)emit_pos(e);
}

/* ------------------------------------------------------------- branches */

/* Jump to a guest target known at translation time, through a link literal. */
/* cond: the word just emitted is the inverted bt/bf skip over this jump */
static void jump_direct(tctx *t, sf_u32 guest_target, sf_u32 avoid, int cond)
{
    emitter *e = &t->em;
    dbt_block *b = t->blk;
    int rT = pick_scratch(avoid);
    int k = b->nlinks;
    if (k >= DBT_MAX_LINKS) { t->err = DBT_ERR_OVERFLOW; return; }
    b->links[k].guest_target = guest_target;
    b->links[k].lit_off = -1;
    b->links[k].jump_off = emit_pos(e);
    b->links[k].cond = (sf_u8)cond;
    b->links[k].backward = (sf_u8)(guest_target <= t->gpc);
    b->nlinks++;
    spill(t, rT, SLOT_X0);
    emit_mov_lit(e, rT, 0, TAG_LINK(k));                     /* host target; initially the exit stub */
    emit16(e, OP_JMP(rT));
    restore(t, rT, SLOT_X0);
}

/* Jump to the guest address already placed in X0. */
static void jump_lookup(tctx *t, sf_u32 avoid)
{
    emitter *e = &t->em;
    int rT = pick_scratch(avoid);
    spill(t, rT, SLOT_Y0);
    emit_mov_lit(e, rT, t->env->rt_lookup, TAG_NONE);
    emit16(e, OP_JMP(rT));
    restore(t, rT, SLOT_Y0);
}

/* X0 = Rn (or PR when reg < 0), without disturbing guest registers. */
static void target_to_x0(tctx *t, int reg, int from_pr, sf_u32 add_const, sf_u32 avoid)
{
    emitter *e = &t->em;
    if (!from_pr && add_const == 0) { emit16(e, op_lds_slot(reg, SLOT_X0)); return; }
    {
        int rT = pick_scratch(avoid);
        spill(t, rT, SLOT_Y0);
        if (from_pr) emit16(e, OP_STS_PR(rT));
        else { emit_mov_imm(e, rT, add_const); emit16(e, OP_ADD_RR(reg, rT)); }
        emit16(e, op_lds_slot(rT, SLOT_X0));
        restore(t, rT, SLOT_Y0);
    }
}

static void set_pr(tctx *t, sf_u32 value, sf_u32 avoid)
{
    emitter *e = &t->em;
    int rT = pick_scratch(avoid);
    spill(t, rT, SLOT_Y0);
    emit_mov_imm(e, rT, value);
    emit16(e, OP_LDS_PR(rT));
    restore(t, rT, SLOT_Y0);
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
    int helper_slot = slot && (slot->kind == K_FPU || slot->kind == K_FALLBACK);
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
        set_pr(t, next, avoid);
        if (slot) tr_insn(t, slot, 1);
        begin_meta_kind(t, META_TARGET_CONST, goff, target, 0);
        jump_direct(t, target, 0, 0);
        return;
    case BR_BRAF:
    case BR_BSRF:
        if (d->br == BR_BSRF) set_pr(t, next, avoid | R(d->br_reg));
        indirect_slot(t, slot, d->br_reg, 0, next, avoid | R(d->br_reg), R(d->br_reg));
        begin_meta_kind(t, META_TARGET_X0, goff, 0, 0);
        jump_lookup(t, 0);
        return;
    case BR_JMP:
    case BR_JSR:
        if (d->br == BR_JSR) set_pr(t, next, avoid | R(d->br_reg));
        indirect_slot(t, slot, d->br_reg, 0, 0, 0, R(d->br_reg));
        begin_meta_kind(t, META_TARGET_X0, goff, 0, 0);
        jump_lookup(t, 0);
        return;
    case BR_RTS:
        indirect_slot(t, slot, -1, 1, 0, avoid, 0);
        begin_meta_kind(t, META_TARGET_X0, goff, 0, 0);
        jump_lookup(t, 0);
        return;
    case BR_RTE: {
        int rT;
        if (slot) tr_insn(t, slot, 1);
        begin_meta_kind(t, META_RTE, goff, 0, 0);
        rT = pick_scratch(0);
        spill(t, rT, SLOT_Y0);
        emit_mov_lit(e, rT, t->env->rt_rte, TAG_NONE);
        emit16(e, OP_JMP(rT));
        restore(t, rT, SLOT_Y0);
        return;
    }
    case BR_BT:
    case BR_BF:
        /* inverted conditional over the 4-instruction direct jump */
        emit16(e, d->br == BR_BT ? OP_BF(3) : OP_BT(3));
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
                emit16(e, d->br == BR_BTS ? OP_BF(3) : OP_BT(3));
                jump_direct(t, target, 0, 1);
                return;
            } else {
                /* slot writes T: branch on the old T (kept in rA), leave the
                 * slot's T visible. Each path after the split has its own
                 * resume range since rA is restored inside it. */
                int rA = pick_scratch(avoid);
                int rB = pick_scratch(avoid | R(rA));
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
                emit16(e, OP_BRA(7));                    /* over: shlr, 2 restores, 4-word jump */
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
            int rT = pick_scratch(R(d->base)), rU = pick_scratch(R(d->base) | R(rT));
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
            int rm = (d->op >> 8) & 0xF, rT = pick_scratch(R(rm));
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
        int rT = pick_scratch(R(d->base));
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
        int dst = d->priv_mem ? pick_scratch(R(d->base)) : rn;
        int rT = pick_scratch(R(dst) | (d->priv_mem ? R(d->base) : 0));
        int rU = pick_scratch(R(dst) | R(rT) | (d->priv_mem ? R(d->base) : 0));
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
        if (d->priv_mem) restore(t, dst, SC(t));
    }
}

/* ------------------------------------------------------------- dispatcher */

static void tr_insn(tctx *t, const sh_insn *d, int in_slot)
{
    (void)in_slot;
    if (t->err) return;
    switch (d->kind) {
    case K_PASS: case K_TRAP: case K_ILLEGAL:
        emit16(&t->em, d->op); break;
    case K_MEM:   tr_mem(t, d); break;
    case K_PCREL: tr_pcrel(t, d); break;
    case K_FPU:   call_helper(t, t->env->rt_fpu, 0, 2, d->op, d->touched); break;
    case K_FALLBACK: call_helper(t, t->env->rt_fallback, 0, 2, d->op, d->touched); break;
    case K_PRIV:  tr_priv(t, d); break;
    case K_BRANCH: /* handled by caller */ break;
    }
}

static void begin_meta_kind(tctx *t, int kind, sf_u32 guest_off, sf_u32 target, sf_u32 fallthrough)
{
    dbt_block *b = t->blk;
    dbt_imeta *m;
    if (b->nmeta >= DBT_MAX_META - 1) { t->err = DBT_ERR_OVERFLOW; return; }
    m = &b->meta[b->nmeta++];
    memset(m, 0, sizeof *m);
    m->host_off = (sf_u16)emit_pos(&t->em);
    m->guest_off = (sf_u16)guest_off;
    m->kind = (sf_u8)kind;
    m->commit_rel = 0xFF;
    m->target = target;
    m->fallthrough = fallthrough;
    t->cur = m;
}

static void begin_meta(tctx *t, sf_u32 guest_off) { begin_meta_kind(t, META_INSN, guest_off, 0, 0); }

static int fetch(tctx *t, sf_u32 gpc, sh_insn *d)
{
    sf_u32 op;
    if (t->env->fetch16(t->env->ctx, gpc, &op)) { t->err = DBT_ERR_FETCH; t->fault = gpc; return 0; }
    sh4_decode(op, d);
    return 1;
}

int dbt_translate_block(const dbt_env *env, sf_u32 guest_pc, sf_u32 host_base,
                        sf_u16 *buf, int cap, dbt_block *out, sf_u32 *fault_addr)
{
    tctx t;
    dbt_block *b = out;
    int max = env->max_insns > 0 && env->max_insns <= DBT_MAX_INSNS ? env->max_insns : DBT_MAX_INSNS;
    int n = 0, ended = 0, k;
    sf_u32 gpc = guest_pc;

    memset(b, 0, sizeof *b);
    memset(&t, 0, sizeof t);
    t.env = env; t.blk = b;
    emit_init(&t.em, buf, cap);
    b->guest_pc = guest_pc; b->host_base = host_base; b->end_reason = END_FULL;

    while (!ended && n < max && !t.err) {
        sh_insn d, s;
        /* size budget: code + worst-case tail must stay within literal reach */
        if (emit_pos(&t.em) > 560 || emit_lits_bytes(&t.em) > 160 || b->nlinks >= DBT_MAX_LINKS - 1) break;
        t.gpc = gpc;
        if (!fetch(&t, gpc, &d)) break;
        begin_meta(&t, gpc - guest_pc);
        n++;
        if (d.kind == K_BRANCH) {
            const sh_insn *slot = 0;
            if (d.has_slot) {
                if (!fetch(&t, gpc + 2, &s)) break;
                if (!sh4_slot_legal(&s)) { /* let hardware raise slot-illegal: pass both through */
                    emit16(&t.em, d.op); emit16(&t.em, s.op);
                    gpc += 4; b->end_reason = END_ILLEGAL; ended = 1; break;
                }
                slot = &s;
            }
            tr_branch(&t, &d, slot);
            gpc += d.has_slot ? 4 : 2;
            if (d.br != BR_BT && d.br != BR_BF && d.br != BR_BTS && d.br != BR_BFS) { ended = 1; b->end_reason = END_BRANCH; }
            continue;
        }
        tr_insn(&t, &d, 0);
        gpc += 2;
        if (d.kind == K_TRAP && (d.op & 0xFF00) == 0xC300) { ended = 1; b->end_reason = END_TRAP; }
        else if (d.kind == K_PRIV && d.ctrl == CR_SR && !d.is_store) { ended = 1; b->end_reason = END_PRIV; }
        else if (d.kind == K_ILLEGAL) { ended = 1; b->end_reason = END_ILLEGAL; }
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
        b->links[k].stub_off = stub_off;
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
    emit_flush_lits(&t.em);
    for (k = 0; k < b->nlinks; k++) {
        int link_off = emit_lit_offset(&t.em, TAG_LINK(k));
        int l_off = emit_lit_offset(&t.em, TAG_STUB_L(k));
        sf_u32 v = host_base + (sf_u32)link_off;
        b->links[k].lit_off = link_off;
        if (l_off >= 0) { emit_patch16(&t.em, l_off, v >> 16); emit_patch16(&t.em, l_off + 2, v & 0xFFFFu); }
    }
    b->host_len = emit_pos(&t.em);
    if (t.em.overflow) return DBT_ERR_OVERFLOW;
    return DBT_OK;
}

int dbt_chain_direct(sf_u16 *code, sf_u32 host, sf_u32 target, int cond, int backward, sf_u32 *from)
{
    sf_i32 d = (sf_i32)(target - (host + 4));
    if (d & 1) return 0;
    d >>= 1;
    if (d < -2048 || d > 2047) return 0;
    if (cond && backward) {
        sf_i32 d8 = (sf_i32)(target - (host - 2 + 4)) >> 1;
        if (d8 >= -128 && d8 <= 127) {
            /* skip word: flip the sense (bit 9) and point it at the target */
            code[-1] = (sf_u16)(((code[-1] ^ 0x0200u) & 0xFF00u) | ((sf_u32)d8 & 0xFFu));
            code[0] = 0xA002;                  /* not taken: bra over the 3 remaining words */
            code[1] = 0x0009;
            *from = host - 2;
            return 6;
        }
    }
    code[0] = (sf_u16)(0xA000u | ((sf_u32)d & 0xFFFu));
    code[1] = 0x0009;
    *from = host;
    return 4;
}

int dbt_block_find(const dbt_block *b, sf_u32 host_off)
{
    int lo = 0, hi = b->nmeta;          /* include the sentinel range */
    if (host_off >= (sf_u32)b->code_len) return -1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (b->meta[mid].host_off <= host_off) lo = mid; else hi = mid - 1;
    }
    return lo;
}

int dbt_block_pending_spills(const dbt_block *b, int idx, sf_u32 host_off, dbt_spill *out, int max)
{
    int i, n = 0;
    (void)idx;
    for (i = b->nspills - 1; i >= 0 && n < max; i--) {       /* latest first, outermost last */
        const dbt_spill *s = &b->spills[i];
        if (s->start < host_off && host_off <= s->end)
            out[n++] = *s;
    }
    return n;
}
