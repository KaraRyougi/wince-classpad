#include <string.h>
#include "sh4dec.h"
#include "../../fpuemu/src/fpuemu.h"

#define R(n) (1u << (n))

static void mem(sh_insn *d, int size, int store, int am, int base, int data, int disp)
{
    d->kind = K_MEM; d->size = size; d->is_store = store; d->am = am;
    d->base = base; d->data = data; d->disp = disp;
    if (am != AM_GBR) d->touched |= R(base);
    if (am == AM_R0IDX) d->touched |= R(0);
    if (am == AM_POSTINC || am == AM_PREDEC) d->defs |= R(base);
    d->touched |= R(data);
    if (!store) d->defs |= R(data);
}

static void ctrl_mem(sh_insn *d, int cr, int bank, int store, int reg)
{
    /* sts.l/stc.l CR,@-Rn  or  lds.l/ldc.l @Rm+,CR */
    d->size = 4; d->is_store = store; d->am = store ? AM_PREDEC : AM_POSTINC;
    d->base = reg; d->data = -1; d->ctrl = cr; d->ctrl_bank = bank;
    d->touched |= R(reg); d->defs |= R(reg);
    d->kind = (cr == CR_SR || cr == CR_VBR) ? K_PRIV : K_MEM;
    d->priv_mem = (d->kind == K_PRIV);
}

static void br(sh_insn *d, int type, int disp, int reg, int slot)
{
    d->kind = K_BRANCH; d->br = type; d->br_disp = disp; d->br_reg = reg; d->has_slot = slot;
    if (reg >= 0) d->touched |= R(reg);
}

static int simm8(sf_u32 op) { int v = op & 0xFF; return v & 0x80 ? v - 0x100 : v; }
static int simm12(sf_u32 op) { int v = op & 0xFFF; return v & 0x800 ? v - 0x1000 : v; }

#ifdef DBT_DECODE_CACHE
static void decode_uncached(sf_u32 op, sh_insn *d)
#else
void sh4_decode(sf_u32 op, sh_insn *d)
#endif
{
    int n = (op >> 8) & 0xF, m = (op >> 4) & 0xF;
    memset(d, 0, sizeof *d);
    d->op = op & 0xFFFFu; op &= 0xFFFFu;
    d->ctrl = CR_NONE; d->data = -1; d->base = -1; d->br_reg = -1;
    d->kind = K_PASS;

    if (fpuemu_is_fpu_op(op)) {
        d->kind = K_FPU;
        /* GPR operands of FPU instructions: lds/sts and fmov address registers */
        switch (op >> 12) {
        case 0x0: d->defs |= R(n); d->touched |= R(n); break;          /* sts fpul/fpscr,rn */
        case 0x4: d->touched |= R(m); if ((op & 0xF) == 0x6 || (op & 0xF) == 0x2) d->defs |= R(m); break;
        case 0xF:
            switch (op & 0xF) {
            case 0x6: d->touched |= R(0) | R(m); break;                   /* fmov @(r0,rm),frn */
            case 0x7: d->touched |= R(0) | R(n); break;                   /* fmov frm,@(r0,rn) */
            case 0x8: case 0x9: d->touched |= R(m); if ((op & 0xF) == 0x9) d->defs |= R(m); break;
            case 0xA: case 0xB: d->touched |= R(n); if ((op & 0xF) == 0xB) d->defs |= R(n); break;
            case 0x4: case 0x5: d->writes_t = 1; break;
            }
        }
        return;
    }

    switch (op >> 12) {
    case 0x0:
        switch (op & 0xF) {
        case 0x2:
            d->defs |= R(n); d->touched |= R(n);
            if (m == 0) { d->kind = K_PRIV; d->ctrl = CR_SR; }
            else if (m == 2) { d->kind = K_PRIV; d->ctrl = CR_VBR; }
            return;                                     /* stc gbr/ssr/spc/Rm_bank/sgr/dbr,rn: pass */
        case 0x3:
            switch (m) {
            case 0x0: br(d, BR_BSRF, 0, n, 1); return;
            case 0x2: br(d, BR_BRAF, 0, n, 1); return;
            case 0x8: case 0x9: case 0xA: case 0xB: d->touched |= R(n); return;   /* pref ocbi ocbp ocbwb */
            case 0xC: mem(d, 4, 1, AM_REG, n, 0, 0); d->movca = 1; return;        /* movca.l r0,@rn */
            }
            d->kind = K_ILLEGAL; return;
        case 0x4: mem(d, 1, 1, AM_R0IDX, n, m, 0); return;
        case 0x5: mem(d, 2, 1, AM_R0IDX, n, m, 0); return;
        case 0x6: mem(d, 4, 1, AM_R0IDX, n, m, 0); return;
        case 0x7: d->touched |= R(n) | R(m); return;                            /* mul.l */
        case 0x8:
            if (op == 0x0008 || op == 0x0018) { d->writes_t = 1; return; }       /* clrt sett */
            if (op == 0x0038) { d->kind = K_FALLBACK; return; }                 /* ldtlb: helper (TLB bookkeeping) */
            if (op == 0x0028 || op == 0x0048 || op == 0x0058) return;            /* clrmac clrs sets */
            d->kind = K_ILLEGAL; return;
        case 0x9:
            if (op == 0x0009) return;                                            /* nop */
            if (op == 0x0019) { d->writes_t = 1; return; }                       /* div0u */
            if (m == 2) { d->defs |= R(n); d->touched |= R(n); return; }         /* movt */
            d->kind = K_ILLEGAL; return;
        case 0xA:
            if (m == 0 || m == 1 || m == 2) { d->defs |= R(n); d->touched |= R(n); return; } /* sts mach/macl/pr */
            if (m == 0xF) { d->defs |= R(n); d->touched |= R(n); return; }       /* stc dbr? (0000nnnn11111010) */
            d->kind = K_ILLEGAL; return;
        case 0xB:
            if (op == 0x000B) { br(d, BR_RTS, 0, -1, 1); return; }
            if (op == 0x001B) { d->kind = K_TRAP; return; }                      /* sleep */
            if (op == 0x002B) { br(d, BR_RTE, 0, -1, 1); return; }
            d->kind = K_ILLEGAL; return;
        case 0xC: mem(d, 1, 0, AM_R0IDX, m, n, 0); return;
        case 0xD: mem(d, 2, 0, AM_R0IDX, m, n, 0); return;
        case 0xE: mem(d, 4, 0, AM_R0IDX, m, n, 0); return;
        case 0xF: d->kind = K_FALLBACK; d->touched |= R(n) | R(m); d->defs |= R(n) | R(m); return; /* mac.l */
        }
        d->kind = K_ILLEGAL; return;
    case 0x1: mem(d, 4, 1, AM_DISP, n, m, (op & 0xF) * 4); return;
    case 0x2:
        switch (op & 0xF) {
        case 0x0: mem(d, 1, 1, AM_REG, n, m, 0); return;
        case 0x1: mem(d, 2, 1, AM_REG, n, m, 0); return;
        case 0x2: mem(d, 4, 1, AM_REG, n, m, 0); return;
        case 0x4: mem(d, 1, 1, AM_PREDEC, n, m, 0); return;
        case 0x5: mem(d, 2, 1, AM_PREDEC, n, m, 0); return;
        case 0x6: mem(d, 4, 1, AM_PREDEC, n, m, 0); return;
        case 0x3: d->kind = K_ILLEGAL; return;
        case 0x7: case 0x8: case 0xC: d->writes_t = 1; d->touched |= R(n) | R(m); return; /* div0s tst cmp/str */
        default: d->touched |= R(n) | R(m); d->defs |= R(n); return;          /* and xor or xtrct mulu muls */
        }
    case 0x3:
        d->touched |= R(n) | R(m);
        switch (op & 0xF) {
        case 0x0: case 0x2: case 0x3: case 0x6: case 0x7: d->writes_t = 1; return;   /* cmp/eq hs ge hi gt */
        case 0x4: d->writes_t = 1; d->defs |= R(n); return;                           /* div1 */
        case 0xA: case 0xB: case 0xE: case 0xF: d->writes_t = 1; d->defs |= R(n); return; /* subc subv addc addv */
        case 0x5: case 0xD: return;                                                   /* dmulu dmuls */
        case 0x8: case 0xC: d->defs |= R(n); return;                                  /* sub add */
        }
        d->kind = K_ILLEGAL; return;
    case 0x4:
        switch (op & 0xFF) {
        case 0x00: case 0x01: case 0x04: case 0x05: case 0x24: case 0x25:
        case 0x20: case 0x21:                                                    /* shifts/rotates with T */
            d->writes_t = 1; d->defs |= R(n); d->touched |= R(n); return;
        case 0x08: case 0x09: case 0x18: case 0x19: case 0x28: case 0x29:        /* shll2.. */
            d->defs |= R(n); d->touched |= R(n); return;
        case 0x10: d->writes_t = 1; d->defs |= R(n); d->touched |= R(n); return;  /* dt */
        case 0x11: case 0x15: d->writes_t = 1; d->touched |= R(n); return;        /* cmp/pz cmp/pl */
        case 0x1B: d->writes_t = 1; d->touched |= R(n); return;                   /* tas.b @rn (byte RMW) */
        case 0x02: ctrl_mem(d, CR_MACH, 0, 1, n); return;
        case 0x12: ctrl_mem(d, CR_MACL, 0, 1, n); return;
        case 0x22: ctrl_mem(d, CR_PR, 0, 1, n); return;
        case 0x06: ctrl_mem(d, CR_MACH, 0, 0, n); return;
        case 0x16: ctrl_mem(d, CR_MACL, 0, 0, n); return;
        case 0x26: ctrl_mem(d, CR_PR, 0, 0, n); return;
        case 0x03: ctrl_mem(d, CR_SR, 0, 1, n); return;
        case 0x13: ctrl_mem(d, CR_GBR, 0, 1, n); return;
        case 0x23: ctrl_mem(d, CR_VBR, 0, 1, n); return;
        case 0x33: ctrl_mem(d, CR_SSR, 0, 1, n); return;
        case 0x43: ctrl_mem(d, CR_SPC, 0, 1, n); return;
        case 0x32: ctrl_mem(d, CR_SGR, 0, 1, n); return;
        case 0xF2: ctrl_mem(d, CR_DBR, 0, 1, n); return;
        case 0x07: ctrl_mem(d, CR_SR, 0, 0, n); return;
        case 0x17: ctrl_mem(d, CR_GBR, 0, 0, n); return;
        case 0x27: ctrl_mem(d, CR_VBR, 0, 0, n); return;
        case 0x37: ctrl_mem(d, CR_SSR, 0, 0, n); return;
        case 0x47: ctrl_mem(d, CR_SPC, 0, 0, n); return;
        case 0xF6: ctrl_mem(d, CR_DBR, 0, 0, n); return;
        case 0x0A: case 0x1A: case 0x2A: d->touched |= R(n); return;             /* lds rm,mach/macl/pr */
        case 0x0E: d->kind = K_PRIV; d->ctrl = CR_SR; d->touched |= R(n); d->writes_t = 1; return;
        case 0x1E: d->touched |= R(n); return;                                   /* ldc rm,gbr */
        case 0x2E: d->kind = K_PRIV; d->ctrl = CR_VBR; d->touched |= R(n); return;
        case 0x3E: case 0x4E: case 0xFA: d->touched |= R(n); return;             /* ldc rm,ssr/spc/dbr */
        case 0x0B: br(d, BR_JSR, 0, n, 1); return;
        case 0x2B: br(d, BR_JMP, 0, n, 1); return;
        }
        if ((op & 0x8F) == 0x83) { ctrl_mem(d, CR_RBANK, m & 7, 1, n); return; }   /* stc.l Rm_bank,@-rn */
        if ((op & 0x8F) == 0x87) { ctrl_mem(d, CR_RBANK, m & 7, 0, n); return; }   /* ldc.l @rm+,Rn_bank */
        if ((op & 0x8F) == 0x8E) { d->touched |= R(n); return; }                     /* ldc rm,Rn_bank */
        if ((op & 0xF) == 0xC || (op & 0xF) == 0xD) { d->touched |= R(n) | R(m); d->defs |= R(n); return; } /* shad shld */
        if ((op & 0xF) == 0xF) { d->kind = K_FALLBACK; d->touched |= R(n) | R(m); d->defs |= R(n) | R(m); return; } /* mac.w */
        d->kind = K_ILLEGAL; return;
    case 0x5: mem(d, 4, 0, AM_DISP, m, n, (op & 0xF) * 4); return;
    case 0x6:
        switch (op & 0xF) {
        case 0x0: mem(d, 1, 0, AM_REG, m, n, 0); return;
        case 0x1: mem(d, 2, 0, AM_REG, m, n, 0); return;
        case 0x2: mem(d, 4, 0, AM_REG, m, n, 0); return;
        case 0x4: mem(d, 1, 0, AM_POSTINC, m, n, 0); return;
        case 0x5: mem(d, 2, 0, AM_POSTINC, m, n, 0); return;
        case 0x6: mem(d, 4, 0, AM_POSTINC, m, n, 0); return;
        case 0xA: d->writes_t = 1; /* negc */ /* fallthrough */
        default: d->touched |= R(n) | R(m); d->defs |= R(n); return;
        }
    case 0x7: d->touched |= R(n); d->defs |= R(n); return;                       /* add #imm */
    case 0x8:
        switch (n) {
        case 0x0: mem(d, 1, 1, AM_DISP, m, 0, op & 0xF); return;
        case 0x1: mem(d, 2, 1, AM_DISP, m, 0, (op & 0xF) * 2); return;
        case 0x4: mem(d, 1, 0, AM_DISP, m, 0, op & 0xF); return;
        case 0x5: mem(d, 2, 0, AM_DISP, m, 0, (op & 0xF) * 2); return;
        case 0x8: d->writes_t = 1; d->touched |= R(0); return;                  /* cmp/eq #imm,r0 */
        case 0x9: br(d, BR_BT, simm8(op) * 2, -1, 0); return;
        case 0xB: br(d, BR_BF, simm8(op) * 2, -1, 0); return;
        case 0xD: br(d, BR_BTS, simm8(op) * 2, -1, 1); return;
        case 0xF: br(d, BR_BFS, simm8(op) * 2, -1, 1); return;
        }
        d->kind = K_ILLEGAL; return;
    case 0x9: d->kind = K_PCREL; d->pcrel = PC_MOVW; d->pc_reg = n; d->disp = (op & 0xFF) * 2; d->defs |= R(n); d->touched |= R(n); return;
    case 0xA: br(d, BR_BRA, simm12(op) * 2, -1, 1); return;
    case 0xB: br(d, BR_BSR, simm12(op) * 2, -1, 1); return;
    case 0xC:
        switch (n) {
        case 0x0: mem(d, 1, 1, AM_GBR, -1, 0, op & 0xFF); return;
        case 0x1: mem(d, 2, 1, AM_GBR, -1, 0, (op & 0xFF) * 2); return;
        case 0x2: mem(d, 4, 1, AM_GBR, -1, 0, (op & 0xFF) * 4); return;
        case 0x3: d->kind = K_TRAP; return;                                     /* trapa */
        case 0x4: mem(d, 1, 0, AM_GBR, -1, 0, op & 0xFF); return;
        case 0x5: mem(d, 2, 0, AM_GBR, -1, 0, (op & 0xFF) * 2); return;
        case 0x6: mem(d, 4, 0, AM_GBR, -1, 0, (op & 0xFF) * 4); return;
        case 0x7: d->kind = K_PCREL; d->pcrel = PC_MOVA; d->pc_reg = 0; d->disp = (op & 0xFF) * 4; d->defs |= R(0); d->touched |= R(0); return;
        case 0x8: case 0xC: d->writes_t = 1; d->touched |= R(0); return;        /* tst #imm,r0 ; tst.b */
        case 0x9: case 0xA: case 0xB: d->touched |= R(0); d->defs |= R(0); return;
        case 0xD: case 0xE: case 0xF: d->touched |= R(0); return;              /* and.b xor.b or.b (byte RMW) */
        }
        d->kind = K_ILLEGAL; return;
    case 0xD: d->kind = K_PCREL; d->pcrel = PC_MOVL; d->pc_reg = n; d->disp = (op & 0xFF) * 4; d->defs |= R(n); d->touched |= R(n); return;
    case 0xE: d->defs |= R(n); d->touched |= R(n); return;                       /* mov #imm,rn */
    case 0xF: d->kind = K_ILLEGAL; return;                                       /* 0xFxxF reserved */
    }
    d->kind = K_ILLEGAL;
}

int sh4_slot_legal(const sh_insn *d)
{
    if (d->kind == K_BRANCH || d->kind == K_PCREL || d->kind == K_ILLEGAL) return 0;
    if (d->kind == K_TRAP && (d->op & 0xFF00) == 0xC300) return 0;   /* trapa */
    return 1;
}

#ifdef DBT_DECODE_CACHE
/* Decode results depend only on the opcode, never on PC or address space.
 * A zero tag keeps an interrupted/abandoned fill invisible. */
static sh_insn decode_cache[512];
static volatile sf_u32 decode_tags[512];
void sh4_decode(sf_u32 op, sh_insn *d)
{
    unsigned slot;
    op &= 0xffffu;
    slot = (op ^ (op >> 7)) & 511u;
    if (decode_tags[slot] == op + 1) { *d = decode_cache[slot]; return; }
    decode_uncached(op, d);
    decode_tags[slot] = 0;
    __asm__ volatile ("" ::: "memory");
    decode_cache[slot] = *d;
    __asm__ volatile ("" ::: "memory");
    decode_tags[slot] = op + 1;
}
#endif
