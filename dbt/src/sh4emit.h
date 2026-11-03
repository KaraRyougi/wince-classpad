/*
 * sh4emit.h - tiny SH-4 assembler for the translator. Emits 16-bit opcodes
 * into a buffer (host word order is the caller's business) and manages a
 * literal pool for 32-bit constants.
 */
#ifndef SH4EMIT_H
#define SH4EMIT_H

#include "../../fpuemu/src/softfloat.h"

#define EMIT_MAX_LITS 64

typedef struct {
    sf_u16 *buf;        /* output buffer */
    int     cap;        /* capacity in 16-bit words */
    int     len;        /* words emitted */
    int     overflow;   /* set if cap was exceeded */
    /* literal pool: 32-bit values referenced by mov.l @(disp,PC) */
    struct { int at; sf_u32 value; int tag; } lits[EMIT_MAX_LITS];
    int     nlits;
} emitter;

void emit_init(emitter *e, sf_u16 *buf, int cap);
void emit16(emitter *e, sf_u32 op);
int  emit_pos(const emitter *e);                 /* current offset in bytes */
void emit_patch16(emitter *e, int byte_off, sf_u32 op);

/* mov.l @(disp,PC),Rn with a 32-bit literal to be placed by emit_flush_lits.
 * `tag` is returned by emit_lit_find so the caller can locate the literal's
 * byte offset after flushing (for link patching). Returns the lit index. */
int  emit_mov_lit(emitter *e, int rn, sf_u32 value, int tag);
/* mov #imm,Rn when it fits, else a literal (tag -1) */
void emit_mov_imm(emitter *e, int rn, sf_u32 value);
/* Place the literal pool here (aligned); returns byte offset of pool start.
 * Call only at a point not reached by fall-through (after a jump). */
int  emit_flush_lits(emitter *e);
/* Byte offset of the literal with the given tag, valid after flushing. -1 if none. */
int  emit_lit_offset(const emitter *e, int tag);
/* Worst-case bytes the pool will need (for block size budgeting). */
int  emit_lits_bytes(const emitter *e);

/* --- encodings (n = Rn, m = Rm) --- */
#define OP_NOP             0x0009u
#define OP_RTS             0x000Bu
#define OP_RTE             0x002Bu
#define OP_MOV_RR(m,n)     (0x6003u | ((n) << 8) | ((m) << 4))
#define OP_SWAPB(m,n)      (0x6008u | ((n) << 8) | ((m) << 4))
#define OP_SWAPW(m,n)      (0x6009u | ((n) << 8) | ((m) << 4))
#define OP_EXTSW(m,n)      (0x600Fu | ((n) << 8) | ((m) << 4))
#define OP_ADD_RR(m,n)     (0x300Cu | ((n) << 8) | ((m) << 4))
#define OP_ADD_IMM(i,n)    (0x7000u | ((n) << 8) | ((i) & 0xFF))
#define OP_MOV_IMM(i,n)    (0xE000u | ((n) << 8) | ((i) & 0xFF))
#define OP_MOVL_PC(d,n)    (0xD000u | ((n) << 8) | ((d) & 0xFF))
#define OP_MOVT(n)         (0x0029u | ((n) << 8))
#define OP_TST_RR(m,n)     (0x2008u | ((n) << 8) | ((m) << 4))
#define OP_NOT_RR(m,n)     (0x6007u | ((n) << 8) | ((m) << 4))
#define OP_NEG_RR(m,n)     (0x600Bu | ((n) << 8) | ((m) << 4))
#define OP_SHLR8(n)        (0x4019u | ((n) << 8))
#define OP_SHLR16(n)       (0x4029u | ((n) << 8))
#define OP_SHLL2(n)        (0x4008u | ((n) << 8))
#define OP_BRAF(n)         (0x0023u | ((n) << 8))
#define OP_SHLR(n)         (0x4001u | ((n) << 8))
#define OP_JMP(n)          (0x402Bu | ((n) << 8))
#define OP_CMPEQ_RR(m,n)   (0x3000u | ((n) << 8) | ((m) << 4))
#define OP_JSR(n)          (0x400Bu | ((n) << 8))
#define OP_BRA(d)          (0xA000u | ((d) & 0xFFF))
#define OP_BT(d)           (0x8900u | ((d) & 0xFF))
#define OP_BF(d)           (0x8B00u | ((d) & 0xFF))
#define OP_BTS(d)          (0x8D00u | ((d) & 0xFF))
#define OP_BFS(d)          (0x8F00u | ((d) & 0xFF))
#define OP_STS_PR(n)       (0x002Au | ((n) << 8))
#define OP_LDS_PR(m)       (0x402Au | ((m) << 8))
#define OP_STS_MACH(n)     (0x000Au | ((n) << 8))
#define OP_STS_MACL(n)     (0x001Au | ((n) << 8))
#define OP_LDS_MACH(m)     (0x400Au | ((m) << 8))
#define OP_LDS_MACL(m)     (0x401Au | ((m) << 8))
#define OP_STC_GBR(n)      (0x0012u | ((n) << 8))
#define OP_STC_SSR(n)      (0x0032u | ((n) << 8))
#define OP_STC_SPC(n)      (0x0042u | ((n) << 8))
#define OP_STC_SGR(n)      (0x003Au | ((n) << 8))
#define OP_STC_DBR(n)      (0x00FAu | ((n) << 8))
#define OP_STC_SR(n)       (0x0002u | ((n) << 8))
#define OP_STC_RBANK(b,n)  (0x0082u | ((n) << 8) | ((b) << 4))
#define OP_LDC_GBR(m)      (0x401Eu | ((m) << 8))
#define OP_LDC_SSR(m)      (0x403Eu | ((m) << 8))
#define OP_LDC_SPC(m)      (0x404Eu | ((m) << 8))
#define OP_LDC_DBR(m)      (0x40FAu | ((m) << 8))
#define OP_LDC_RBANK(m,b)  (0x408Eu | ((m) << 8) | ((b) << 4))
#define OP_MOVL_STORE(m,n) (0x2002u | ((n) << 8) | ((m) << 4))      /* mov.l rm,@rn */
#define OP_MOVL_LOAD(m,n)  (0x6002u | ((n) << 8) | ((m) << 4))      /* mov.l @rm,rn */
#define OP_MOVL_PREDEC(m,n) (0x2006u | ((n) << 8) | ((m) << 4))     /* mov.l rm,@-rn */
#define OP_MOVL_POSTINC(m,n) (0x6006u | ((n) << 8) | ((m) << 4))    /* mov.l @rm+,rn */
#define OP_MOVCA(n)        (0x00C3u | ((n) << 8))                   /* movca.l r0,@rn */

/* DSP transfer registers used as spill slots (SH-DSP encodings). */
enum { SLOT_A0 = 0, SLOT_X0, SLOT_X1, SLOT_Y0, SLOT_Y1, SLOT_COUNT };
sf_u32 op_lds_slot(int rm, int slot);   /* lds Rm,<slot> */
sf_u32 op_sts_slot(int slot, int rn);   /* sts <slot>,Rn */

#endif
