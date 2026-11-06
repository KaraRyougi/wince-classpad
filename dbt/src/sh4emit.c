#include "sh4emit.h"

void emit_init(emitter *e, sf_u16 *buf, int cap)
{
    e->buf = buf; e->cap = cap; e->len = 0; e->overflow = 0; e->nlits = 0;
}

void emit16(emitter *e, sf_u32 op)
{
    if (e->len >= e->cap) { e->overflow = 1; return; }
    e->buf[e->len++] = (sf_u16)op;
}

int emit_pos(const emitter *e) { return e->len * 2; }

void emit_patch16(emitter *e, int byte_off, sf_u32 op)
{
    if (byte_off / 2 < e->cap) e->buf[byte_off / 2] = (sf_u16)op;
}

int emit_mov_lit(emitter *e, int rn, sf_u32 value, int tag)
{
    int idx = e->nlits;
    if (idx >= EMIT_MAX_LITS) { e->overflow = 1; return -1; }
    e->lits[idx].at = emit_pos(e);
    e->lits[idx].value = value;
    e->lits[idx].tag = tag;
    e->nlits++;
    emit16(e, OP_MOVL_PC(0, rn));      /* displacement patched at flush */
    return idx;
}

void emit_mov_imm(emitter *e, int rn, sf_u32 value)
{
    sf_i32 v = (sf_i32)value;
    if (v >= -128 && v <= 127) emit16(e, OP_MOV_IMM(v, rn));
    else emit_mov_lit(e, rn, value, -1);
}

int emit_lits_bytes(const emitter *e) { return e->nlits * 4 + 2; }

int emit_flush_lits(emitter *e)
{
    int i, start;
    if (e->nlits == 0) return emit_pos(e);
    if (emit_pos(e) & 2) emit16(e, OP_NOP);          /* align pool to 4 */
    start = emit_pos(e);
    for (i = 0; i < e->nlits; i++) {
        int at = e->lits[i].at;
        int pool = emit_pos(e), shared = 0;
#ifdef DBT_DEDUP_LITERALS
        int j;
        /* Only untagged constants are immutable. Link and IC literals must
         * retain separate storage even when their initial values match. */
        if (e->lits[i].tag == -1) for (j = 0; j < i; ++j) {
            if (e->lits[j].tag == -1 && e->lits[j].value == e->lits[i].value) {
                pool = e->lits[j].at; shared = 1; break;
            }
        }
#endif
        /* mov.l @(disp,PC),Rn: address = (PC_of_insn + 4) & ~3 + disp*4 */
        int base = (at + 4) & ~3;
        int disp = (pool - base) / 4;
        if (disp < 0 || disp > 255) e->overflow = 1;
        emit_patch16(e, at, OP_MOVL_PC(disp, (e->buf[at / 2] >> 8) & 0xF));
        if (!shared) {
            emit16(e, e->lits[i].value >> 16);
            emit16(e, e->lits[i].value & 0xFFFFu);
        }
        e->lits[i].at = pool;      /* now records the literal's own offset */
    }
    e->nlits = -e->nlits;          /* mark flushed; count kept for lookups */
    return start;
}

int emit_lit_offset(const emitter *e, int tag)
{
    int n = e->nlits < 0 ? -e->nlits : e->nlits, i;
    if (e->nlits >= 0) return -1;
    for (i = 0; i < n; i++) if (e->lits[i].tag == tag) return e->lits[i].at;
    return -1;
}

/* lds Rm,{A0,X0,X1,Y0,Y1} = 0100mmmm 0111/1000/1001/1010/1011 1010 */
static const sf_u32 slot_code[SLOT_COUNT] = { 0x7, 0x8, 0x9, 0xA, 0xB };

sf_u32 op_lds_slot(int rm, int slot) { return 0x400Au | ((sf_u32)rm << 8) | (slot_code[slot] << 4); }
sf_u32 op_sts_slot(int slot, int rn) { return 0x000Au | ((sf_u32)rn << 8) | (slot_code[slot] << 4); }
