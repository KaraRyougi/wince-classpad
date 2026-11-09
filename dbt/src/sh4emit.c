#include "sh4emit.h"

void emit_init(emitter *e, sf_u16 *buf, int cap)
{
    e->buf = buf; e->cap = cap; e->len = 0; e->overflow = 0; e->nlits = 0;
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

int emit_lit_only(emitter *e, sf_u32 value, int tag)
{
    int idx = e->nlits;
    if (idx >= EMIT_MAX_LITS) { e->overflow = 1; return -1; }
    e->lits[idx].at = -1;              /* no instruction to patch */
    e->lits[idx].value = value;
    e->lits[idx].tag = tag;
    e->nlits++;
    return idx;
}

void emit_mov_imm(emitter *e, int rn, sf_u32 value)
{
    sf_i32 v = (sf_i32)value;
    if (v >= -128 && v <= 127) emit16(e, OP_MOV_IMM(v, rn));
    else emit_mov_lit(e, rn, value, -1);
}


int emit_flush_lits(emitter *e)
{
    int i, start;
    /* A failed emission may have recorded a literal at cap. Never inspect
     * that instruction slot; callers must discard the incomplete output. */
    if (e->overflow) return -1;
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
        if (at >= 0) {
            /* mov.l @(disp,PC),Rn: address = (PC_of_insn + 4) & ~3 + disp*4 */
            int base = (at + 4) & ~3;
            int disp = (pool - base) / 4;
            if (disp < 0 || disp > 255) e->overflow = 1;
            emit_patch16(e, at, OP_MOVL_PC(disp, (e->buf[at / 2] >> 8) & 0xF));
        }
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

