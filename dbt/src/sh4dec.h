/*
 * sh4dec.h - SH-4 instruction classifier for the LE->BE translator.
 * Every 16-bit opcode is decoded into what the translator needs to know:
 * which class it belongs to, which registers it touches, and the operands
 * of memory, PC-relative and branch instructions.
 */
#ifndef SH4DEC_H
#define SH4DEC_H

#include "../../fpuemu/src/softfloat.h"   /* sf_u32 */

enum sh_kind {
    K_PASS,       /* copy verbatim */
    K_MEM,        /* load/store: byte-order rewrite */
    K_PCREL,      /* mov.w/l @(disp,PC), mova: resolved at translate time */
    K_BRANCH,     /* control transfer */
    K_FPU,        /* FPU / FPUL / FPSCR: helper call */
    K_PRIV,       /* ldc/stc sr, vbr (incl. .l forms): helper */
    K_TRAP,       /* trapa, sleep: pass through, block boundary */
    K_FALLBACK,   /* mac.w, mac.l: C interpreter */
    K_ILLEGAL
};

enum sh_am { AM_REG, AM_POSTINC, AM_PREDEC, AM_DISP, AM_R0IDX, AM_GBR };

enum sh_cr { CR_NONE = -1, CR_MACH, CR_MACL, CR_PR, CR_GBR, CR_VBR, CR_SR,
             CR_SSR, CR_SPC, CR_SGR, CR_DBR, CR_RBANK };

enum sh_br { BR_BRA, BR_BSR, BR_BT, BR_BF, BR_BTS, BR_BFS, BR_BRAF, BR_BSRF,
             BR_JMP, BR_JSR, BR_RTS, BR_RTE };

enum sh_pc { PC_MOVW, PC_MOVL, PC_MOVA };

/* Decoding is hot during cache refills. Keep operands narrow so the
 * pre-pass copies 28 bytes per instruction instead of 84. Displacements
 * retain signed 16-bit range; register masks retain all sixteen bits. */
typedef struct {
    signed char    kind;
    sf_u16 op;
    /* K_MEM */
    signed char size;          /* 1, 2, 4 */
    signed char is_store;
    signed char am;            /* enum sh_am */
    signed char base;          /* base register (ignored for AM_GBR) */
    signed char data;          /* data register: Rn for loads, Rm for stores; r0 for fixed forms */
    short disp;          /* byte displacement, already scaled */
    signed char ctrl;          /* enum sh_cr when the data side is a control register (lds.l/sts.l/ldc.l/stc.l) */
    signed char ctrl_bank;     /* bank index for CR_RBANK */
    signed char movca;         /* movca.l r0,@rn */
    /* K_BRANCH */
    signed char br;            /* enum sh_br */
    short br_disp;       /* byte displacement relative to PC+4 */
    signed char br_reg;
    signed char has_slot;
    /* K_PCREL */
    signed char pcrel;         /* enum sh_pc */
    signed char pc_reg;
    /* K_PRIV: ctrl, plus mem fields when .l form (am/base/is_store set) */
    signed char priv_mem;
    /* generic */
    signed char writes_t;      /* writes SR.T */
    sf_u16 touched;    /* GPR bitmask read or written */
    sf_u16 defs;       /* GPR bitmask written */
} sh_insn;

void sh4_decode(sf_u32 op, sh_insn *d);

/* 1 if the instruction is legal in a delay slot */
int sh4_slot_legal(const sh_insn *d);

#endif
