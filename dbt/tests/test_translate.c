/*
 * test_translate.c - host unit tests for the LE->BE block translator.
 * Guest code is written little-endian into a fake memory, translated, and
 * the emitted host opcode sequences are compared with expected ones.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "sh4dec.h"
#include "sh4emit.h"
#include "translate.h"

static uint8_t gmem[4096];
#define GBASE 0x8C001000u

static int fetch16(void *c, sf_u32 pc, sf_u32 *op)
{
    sf_u32 o = pc - GBASE; (void)c;
    if (o + 1 >= sizeof gmem) return 1;
    *op = gmem[o] | (gmem[o + 1] << 8);
    return 0;
}
static int read16le(void *c, sf_u32 a, sf_u32 *v) { return fetch16(c, a, v); }
static int read32le(void *c, sf_u32 a, sf_u32 *v)
{
    sf_u32 o = a - GBASE; (void)c;
    if (o + 3 >= sizeof gmem) return 1;
    *v = gmem[o] | (gmem[o + 1] << 8) | (gmem[o + 2] << 16) | ((sf_u32)gmem[o + 3] << 24);
    return 0;
}

static void put16(sf_u32 addr, sf_u32 v) { gmem[addr - GBASE] = v & 0xFF; gmem[addr - GBASE + 1] = (v >> 8) & 0xFF; }
static void put32(sf_u32 addr, sf_u32 v) { put16(addr, v & 0xFFFF); put16(addr + 2, v >> 16); }

static dbt_env env;
static sf_u16 hbuf[DBT_BLOCK_WORDS];
static dbt_block blk;
static dbt_imeta blk_meta[DBT_MAX_META];
static dbt_spill blk_spills[DBT_MAX_BLOCK_SPILLS];
#define HBASE 0x8CC00000u

static int failures = 0, checks = 0;

static const char *opname(sf_u32 op)
{
    static char b[32];
    sprintf(b, "%04x", op);
    return b;
}

/* Translate `n` guest opcodes placed at GBASE; returns status. */
static int translate(const sf_u32 *ops, int n, int max_insns)
{
    int i;
    memset(gmem, 0, sizeof gmem);
    for (i = 0; i < n; i++) put16(GBASE + i * 2, ops[i]);
    env.max_insns = max_insns;
    blk.meta = blk_meta; blk.spills = blk_spills;
    return dbt_translate_block(&env, GBASE, HBASE, hbuf, DBT_BLOCK_WORDS, &blk, 0);
}

/* Compare host words [from, from+n) with expected. */
static void expect_seq(const char *name, int from_words, const sf_u32 *exp, int n)
{
    int i, ok = 1;
    checks++;
    for (i = 0; i < n; i++) if (hbuf[from_words + i] != exp[i]) ok = 0;
    if (!ok) {
        failures++;
        printf("FAIL %s\n  got:", name);
        for (i = 0; i < n; i++) printf(" %s", opname(hbuf[from_words + i]));
        printf("\n  exp:");
        for (i = 0; i < n; i++) printf(" %04x", exp[i]);
        printf("\n");
    }
}

static void expect(const char *name, int cond)
{
    checks++;
    if (!cond) { failures++; printf("FAIL %s\n", name); }
}

static sf_u32 lit_at(int byte_off) { return ((sf_u32)hbuf[byte_off / 2] << 16) | hbuf[byte_off / 2 + 1]; }

/* Decode a mov.l @(disp,PC),Rn at host byte offset `at` and return the literal value. */
static sf_u32 lit_of_movl(int at)
{
    sf_u32 op = hbuf[at / 2];
    int disp = op & 0xFF;
    int addr = ((at + 4) & ~3) + disp * 4;
    return lit_at(addr);
}

#define R_(n)      (1u << (n))
#define LDS_X0(r)  op_lds_slot(r, SLOT_X0)
#define STS_X0(r)  op_sts_slot(SLOT_X0, r)
#define LDS_X1(r)  op_lds_slot(r, SLOT_X1)
#define STS_X1(r)  op_sts_slot(SLOT_X1, r)
#define LDS_Y0(r)  op_lds_slot(r, SLOT_Y0)
#define STS_Y0(r)  op_sts_slot(SLOT_Y0, r)
#define LDS_A0(r)  op_lds_slot(r, SLOT_A0)
#define STS_A0(r)  op_sts_slot(SLOT_A0, r)

int main(void)
{
    memset(&env, 0, sizeof env);
    env.fetch16 = fetch16; env.read16le = read16le; env.read32le = read32le;
    env.rt_lookup_link = 0x8CF00000; env.rt_lookup = 0x8CF00100; env.rt_rte = 0x8CF00200; env.rt_lookup_ic = 0x8CF00600;
    env.rt_fpu = 0x8CF00300; env.rt_fallback = 0x8CF00400; env.rt_ldc_sr = 0x8CF00500;
    env.shadow_vbr_addr = 0x8CF01000; env.shadow_md_addr = 0x8CF01004;
    env.no_borrow = 1;      /* the spill paths first; the borrowed register has its own section below */

    /* 1. encodings of the DSP slot transfers */
    expect("lds r1,x0", LDS_X0(1) == 0x418A);
    expect("sts x0,r1", STS_X0(1) == 0x018A);
    expect("lds r3,y0", LDS_Y0(3) == 0x43AA);

    /* 1b. decoder: GPR operands of the FPU indexed forms (the helper's scratch must avoid them) */
    { sh_insn d;
      sh4_decode(0xF127, &d);   /* fmov.s fr2,@(r0,r1) */
      expect("dec fmov @(r0,rn)", d.kind == K_FPU && d.touched == (R_(0) | R_(1)) && d.defs == 0);
      sh4_decode(0xF216, &d);   /* fmov.s @(r0,r1),fr2 */
      expect("dec fmov @(r0,rm)", d.kind == K_FPU && d.touched == (R_(0) | R_(1)));
      sh4_decode(0xF219, &d);   /* fmov.s @r1+,fr2 */
      expect("dec fmov @rm+", d.touched == R_(1) && d.defs == R_(1));
    }

    /* 2. pass-through ALU block of 3 insns, then fall-through jump */
    { sf_u32 ops[] = { 0x3C0C /* add r0,r12 */, 0xE105 /* mov #5,r1 */, 0x2208 /* tst r0,r2 */ };
      sf_u32 exp[] = { 0x3C0C, 0xE105, 0x2208 };
      int r = translate(ops, 3, 3);
      expect("alu status", r == DBT_OK);
      expect_seq("alu pass", 0, exp, 3);
      expect("alu meta", blk.nmeta == 3 && blk.meta[1].host_off == 2 && blk.meta[2].guest_off == 4 && blk.meta[3].host_off == 6 && blk.meta[3].guest_off == 6);
      /* fall-through: nop (no borrowed register) ; lds r1,x0 ; mov.l lit,r1 ; jmp @r1 ; sts x0,r1 */
      expect("alu tail", hbuf[3] == OP_NOP && hbuf[4] == LDS_X0(1) && (hbuf[5] & 0xFF00) == 0xD100 && hbuf[6] == OP_JMP(1) && hbuf[7] == STS_X0(1));
      expect("alu link", blk.nlinks == 1 && blk.links[0].guest_target == GBASE + 6);
      /* the link literal initially points at the exit stub, which loads the guest target */
      { sf_u32 stub = lit_of_movl(10); int so = stub - HBASE;
        expect("alu stub addr", so == blk.code_len);
        expect("alu stub code", hbuf[so / 2] == LDS_Y0(1) && hbuf[so / 2 + 2] == LDS_X0(1) && hbuf[so / 2 + 4] == LDS_X1(1) && hbuf[so / 2 + 6] == OP_JMP(1) && hbuf[so / 2 + 7] == STS_Y0(1));
        expect("alu stub target", lit_of_movl(so + 2) == GBASE + 6);
        expect("alu stub linkaddr", lit_of_movl(so + 6) == HBASE + (sf_u32)blk.links[0].lit_off);
        expect("alu stub lookup", lit_of_movl(so + 10) == env.rt_lookup_link);
      }
    }

    /* 3. loads: mov.l @r1,r2 ; mov.w @r3+,r4 ; mov.b @r5,r6 */
    { sf_u32 ops[] = { 0x6212, 0x6435, 0x6650 };
      sf_u32 exp[] = { 0x6212, OP_SWAPB(2,2), OP_SWAPW(2,2), OP_SWAPB(2,2),
                       0x6435, OP_SWAPB(4,4), OP_EXTSW(4,4),
                       0x6650 };
      translate(ops, 3, 3);
      expect_seq("loads", 0, exp, 8);
    }

    /* 4. stores with free data field: mov.w r3,@r4 ; mov.l r0,@(8,r0) ; mov.l r5,@-r5 */
    { sf_u32 ops[] = { 0x2431, 0x1002, 0x2556 };
      sf_u32 exp[] = { LDS_X0(1), OP_SWAPB(3,1), 0x2411, STS_X0(1),
                       LDS_X0(1), OP_SWAPB(0,1), OP_SWAPW(1,1), OP_SWAPB(1,1), 0x1012, STS_X0(1),
                       LDS_X0(1), OP_MOV_RR(5,1), OP_ADD_IMM(-4,1), OP_SWAPB(1,1), OP_SWAPW(1,1), OP_SWAPB(1,1), 0x2516, STS_X0(1) };
      translate(ops, 3, 3);
      expect_seq("stores", 0, exp, 18);
      /* spill bookkeeping for the first store: lds at 0, sts at 6 */
      expect("spill rec", blk.nspills == 4 && blk.spills[0].insn == 0 && blk.spills[0].gpr == 1 && blk.spills[0].start == 0 && blk.spills[0].end == 6);
      { dbt_spill sp[4];
        expect("pending@store", dbt_block_pending_spills(&blk, 0, 4, sp, 4) == 1 && sp[0].slot == SLOT_X0);
        expect("pending@lds", dbt_block_pending_spills(&blk, 0, 0, sp, 4) == 0);
        expect("pending@after", dbt_block_pending_spills(&blk, 0, 8, sp, 4) == 0);
        expect("find", dbt_block_find(&blk, 9) == 1 && dbt_block_find(&blk, 0) == 0 && dbt_block_find(&blk, 20) == 2);
      }
    }

    /* 5. r0-fixed stores: mov.w r0,@(4,r2) ; mov.l r0,@(8,gbr) ; mov.w r0,@(2,r0) ; movca.l r0,@r3 */
    { sf_u32 ops[] = { 0x8122, 0xC202, 0x8101, 0x03C3 };
      sf_u32 exp[] = { LDS_X0(0), OP_SWAPB(0,0), 0x8122, STS_X0(0),
                       LDS_X0(0), OP_SWAPB(0,0), OP_SWAPW(0,0), OP_SWAPB(0,0), 0xC202, STS_X0(0),
                       LDS_X0(1), OP_MOV_RR(0,1), OP_ADD_IMM(2,1), LDS_X1(2), OP_SWAPB(0,2), 0x2121, STS_X1(2), STS_X0(1),
                       LDS_X0(0), OP_SWAPB(0,0), OP_SWAPW(0,0), OP_SWAPB(0,0), 0x03C3, STS_X0(0) };
      translate(ops, 4, 4);
      expect_seq("r0 stores", 0, exp, 24);
    }

    /* 6. control-register memory forms: sts.l pr,@-r15 ; lds.l @r15+,pr ; stc.l gbr,@-r14 */
    { sf_u32 ops[] = { 0x4F22, 0x4F26, 0x4E13 };
      sf_u32 exp[] = { LDS_X0(1), OP_STS_PR(1), OP_SWAPB(1,1), OP_SWAPW(1,1), OP_SWAPB(1,1), OP_MOVL_PREDEC(1,15), STS_X0(1),
                       LDS_X0(1), OP_MOVL_POSTINC(15,1), OP_SWAPB(1,1), OP_SWAPW(1,1), OP_SWAPB(1,1), OP_LDS_PR(1), STS_X0(1),
                       LDS_X0(1), OP_STC_GBR(1), OP_SWAPB(1,1), OP_SWAPW(1,1), OP_SWAPB(1,1), OP_MOVL_PREDEC(1,14), STS_X0(1) };
      translate(ops, 3, 3);
      expect_seq("ctrl mem", 0, exp, 21);
    }

    /* 7. PC-relative: mov.l @(1,pc),r5 at GBASE (literal at GBASE+8) ; mov.w @(2,pc),r6 ; mova @(1,pc),r0 */
    { sf_u32 ops[] = { 0xD501, 0x9604, 0xC701 };
      int r;
      memset(gmem, 0, sizeof gmem);
      put16(GBASE, ops[0]); put16(GBASE + 2, ops[1]); put16(GBASE + 4, ops[2]);
      put32(GBASE + 8, 0x12345678);            /* mov.l literal at ((pc+4)&~3)+4 */
      put16(GBASE + 2 + 4 + 8, 0xFFF0);        /* mov.w literal at pc+4+8 = GBASE+14 */
      env.max_insns = 3;
      r = dbt_translate_block(&env, GBASE, HBASE, hbuf, DBT_BLOCK_WORDS, &blk, 0);
      expect("pcrel status", r == DBT_OK);
      expect("pcrel movl", (hbuf[0] & 0xFF00) == 0xD500 && lit_of_movl(0) == 0x12345678);
      expect("pcrel movw", hbuf[1] == OP_MOV_IMM(-16, 6));
      expect("pcrel mova", (hbuf[2] & 0xFF00) == 0xD000 && lit_of_movl(4) == ((GBASE + 8) & ~3u) + 4);
    }

    /* 8. bra with a slot: bra +8 ; nop   -> nop ; direct jump (nop, lds r1,x0 ...) to GBASE+4+8 */
    { sf_u32 ops[] = { 0xA004, 0x0009 };
      sf_u32 exp[] = { 0x0009, OP_NOP, LDS_X0(1) };
      translate(ops, 2, 4);
      expect_seq("bra", 0, exp, 3);
      expect("bra link", blk.nlinks == 1 && blk.links[0].guest_target == GBASE + 12 && blk.end_reason == END_BRANCH && blk.guest_len == 4);
      expect("bra meta", blk.nmeta == 2 && blk.meta[1].kind == META_TARGET_CONST && blk.meta[1].target == GBASE + 12 && blk.meta[1].host_off == 2 && blk.meta[2].guest_off == 4);
    }

    /* 9. bsr: PR = GBASE+4, slot executes after PR write */
    { sf_u32 ops[] = { 0xB010, 0x6213 /* mov r1,r2 */ };
      sf_u32 exp[] = { LDS_Y0(3), 0xD300 /* mov.l lit,r3 (disp patched) */, OP_LDS_PR(3), STS_Y0(3), 0x6213, OP_NOP, LDS_X0(1) };
      translate(ops, 2, 4);
      exp[1] = hbuf[1];   /* displacement is pool-dependent; check the literal instead */
      expect_seq("bsr", 0, exp, 7);
      expect("bsr pr", lit_of_movl(2) == GBASE + 4);
    }

    /* 10. rts ; nop -> slot, then the PR inline cache: lds r1,y0; sts pr,r1; lds r2,y1; mov.l g,r2;
     * cmp/eq r1,r2; bf +3; sts y0,r1; mov.l h,r2; jmp @r2; sts y1,r2; miss: sts y1,r2; lds r1,x0;
     * mov.l a,r1; lds r1,x1; mov.l lookup_ic,r1; jmp @r1; sts y0,r1 */
    { sf_u32 ops[] = { 0x000B, 0x0009 };
      sf_u32 exp[] = { 0x0009, LDS_Y0(1), OP_STS_PR(1), op_lds_slot(2, SLOT_Y1), 0, OP_CMPEQ_RR(1, 2), OP_BF(3), STS_Y0(1), 0,
                       OP_JMP(2), op_sts_slot(SLOT_Y1, 2), op_sts_slot(SLOT_Y1, 2), LDS_X0(1), 0, LDS_X1(1), 0, OP_JMP(1), STS_Y0(1) };
      translate(ops, 2, 4);
      exp[4] = hbuf[4]; exp[8] = hbuf[8]; exp[13] = hbuf[13]; exp[15] = hbuf[15];
      expect_seq("rts", 0, exp, 18);
      expect("rts meta", blk.meta[1].kind == META_TARGET_PR && blk.meta[1].host_off == 2);
      expect("rts ic literals", blk.ic_off > 0 && lit_of_movl(8) == 1 && lit_of_movl(16) == 0 && lit_of_movl(26) == HBASE + (sf_u32)blk.ic_off
             && lit_of_movl(30) == env.rt_lookup_ic && hbuf[blk.ic_off / 2 + 1] == 1 && hbuf[blk.ic_off / 2 + 2] == 0);
      { dbt_spill sp[4]; int n = dbt_block_pending_spills(&blk, 1, 18, sp, 4);    /* at the jmp of the hit path */
        expect("rts ic pending", n == 2); }
    }

    /* 11. jmp @r2 with slot that redefines r2: target captured first */
    { sf_u32 ops[] = { 0x422B, 0x6233 /* mov r3,r2 */ };
      sf_u32 exp[] = { LDS_X0(2), 0x6233, LDS_Y0(1) };
      translate(ops, 2, 4);
      expect_seq("jmp clobber", 0, exp, 3);
      expect("jmp meta", blk.nmeta == 2 && blk.meta[1].kind == META_TARGET_X0 && blk.meta[1].host_off == 4);
    }

    /* 12. jsr @r4 ; nop : PR (scratch kept), slot, inline cache comparing r4 directly */
    { sf_u32 ops[] = { 0x440B, 0x0009 };
      /* lds r1,y0; mov.l ret,r1; lds r1,pr; nop; mov.l g,r1; cmp/eq r4,r1; bf +2; mov.l h,r1; jmp @r1; sts y0,r1;
       * miss: lds r4,x0; mov.l a,r1; lds r1,x1; mov.l lookup_ic,r1; jmp @r1; sts y0,r1 */
      sf_u32 exp[] = { LDS_Y0(1), 0, OP_LDS_PR(1), 0x0009, 0, OP_CMPEQ_RR(4, 1), OP_BF(2), 0, OP_JMP(1), STS_Y0(1),
                       LDS_X0(4), 0, LDS_X1(1), 0, OP_JMP(1), STS_Y0(1) };
      translate(ops, 2, 4);
      exp[1] = hbuf[1]; exp[4] = hbuf[4]; exp[7] = hbuf[7]; exp[11] = hbuf[11]; exp[13] = hbuf[13];
      expect_seq("jsr", 0, exp, 16);
      expect("jsr pr", lit_of_movl(2) == GBASE + 4 && lit_of_movl(8) == 1 && lit_of_movl(14) == 0 && lit_of_movl(26) == env.rt_lookup_ic);
      expect("jsr meta", blk.meta[1].kind == META_TARGET_REG && blk.meta[1].fix_reg == 4 && blk.meta[1].host_off == 8);
      /* r1 (PR scratch) is spilled from 0 to its miss-path restore at 30 */
      expect("jsr kept scratch", blk.nspills == 1 && blk.spills[0].gpr == 1 && blk.spills[0].slot == SLOT_Y0 && blk.spills[0].start == 0 && blk.spills[0].end == 30);
    }
    /* 12b. jmp @r3 ; nop : no PR, 7-instruction hit path */
    { sf_u32 ops[] = { 0x432B, 0x0009 };
      translate(ops, 2, 4);
      /* nop; lds r1,y0; mov.l g,r1; cmp/eq r3,r1; bf +2; mov.l h,r1; jmp @r1; sts y0,r1; miss: lds r3,x0; ... */
      expect("jmp fast", hbuf[0] == 0x0009 && hbuf[1] == LDS_Y0(1) && hbuf[3] == OP_CMPEQ_RR(3, 1) && hbuf[4] == OP_BF(2)
             && hbuf[6] == OP_JMP(1) && hbuf[7] == STS_Y0(1) && hbuf[8] == LDS_X0(3) && hbuf[10] == LDS_X1(1)
             && hbuf[12] == OP_JMP(1) && hbuf[13] == STS_Y0(1) && blk.meta[1].kind == META_TARGET_REG && blk.meta[1].fix_reg == 3);
    }

    /* 13. bt +4 (no slot): bf over the jump, block continues */
    { sf_u32 ops[] = { 0x8902, 0x7101 /* add #1,r1 */ };
      sf_u32 exp[] = { OP_BF(4), OP_NOP, LDS_X0(1), 0, OP_JMP(1), STS_X0(1), 0x7101 };
      translate(ops, 2, 2);
      exp[3] = hbuf[3];
      expect_seq("bt", 0, exp, 7);
      expect("bt links", blk.nlinks == 2 && blk.links[0].guest_target == GBASE + 8 && blk.links[1].guest_target == GBASE + 4);
    }

    /* 14. bt/s with a T-writing slot: bt/s +6 ; cmp/eq r1,r2 */
    { sf_u32 ops[] = { 0x8D03, 0x3120 };
      sf_u32 exp[] = { LDS_Y0(3), OP_MOVT(3), 0x3120, op_lds_slot(4, SLOT_Y1), OP_MOVT(4), OP_TST_RR(3,3), OP_BF(4),
                       OP_SHLR(4), op_sts_slot(SLOT_Y1, 4), STS_Y0(3), OP_BRA(8), OP_NOP,
                       OP_SHLR(4), op_sts_slot(SLOT_Y1, 4), STS_Y0(3), OP_NOP, LDS_X0(1) };
      translate(ops, 2, 4);
      expect_seq("bt/s T", 0, exp, 17);
      /* bf(4) from offset 12 lands on offset 12+4+8 = 24 = taken path */
      expect("bt/s bf target", 12 + 4 + 4 * 2 == 24 && hbuf[12] == OP_SHLR(4));
      /* bra(8) at offset 20 lands at 20+4+16 = 40, right after the 5-word jump at 30..39 */
      expect("bt/s bra target", 20 + 4 + 8 * 2 == 40 && blk.meta[4].host_off == 40 && blk.meta[4].kind == META_INSN
             && blk.meta[1].kind == META_COND_REG && blk.meta[1].fix_reg == 3);
    }

    /* 14b. bt/s +6 ; nop : slot, COND boundary, inverted conditional, jump */
    { sf_u32 ops[] = { 0x8D03, 0x0009 };
      translate(ops, 2, 4);
      expect("bt/s cond", hbuf[0] == 0x0009 && hbuf[1] == OP_BF(4) && blk.meta[1].kind == META_COND
             && blk.meta[1].target == GBASE + 4 + 6 && blk.meta[1].fallthrough == GBASE + 4 && blk.meta[1].host_off == 2);
    }

    /* 15. FPU: fadd fr1,fr2 -> helper call with opcode in X0, return address in X1 */
    { sf_u32 ops[] = { 0xF120, 0x0009 };
      translate(ops, 2, 2);
      expect("fpu seq", hbuf[0] == LDS_Y0(1) && (hbuf[1] & 0xFF00) == 0xD100 && hbuf[2] == LDS_X0(1)
                        && (hbuf[3] & 0xFF00) == 0xD100 && hbuf[4] == LDS_X1(1) && (hbuf[5] & 0xFF00) == 0xD100
                        && hbuf[6] == OP_JMP(1) && hbuf[7] == STS_Y0(1) && hbuf[8] == 0x0009);
      expect("fpu lits", lit_of_movl(2) == 0xF120 && lit_of_movl(6) == HBASE + 16 && lit_of_movl(10) == env.rt_fpu);
      /* the call commits the range at the jmp's delay slot (word 7): a resume at the return
       * address (word 8) is "committed" and goes on with the next instruction */
      expect("fpu commit", blk.meta[0].commit_rel == 14 && blk.meta[0].fix_kind == FIX_NONE && blk.meta[1].host_off == 16);
    }

    /* 16. privileged: ldc r3,vbr ; stc vbr,r4 ; ldc r5,sr (ends block) */
    { sf_u32 ops[] = { 0x432E, 0x0422, 0x450E };
      sf_u32 exp[] = { LDS_X0(1), 0, OP_MOVL_STORE(3,1), STS_X0(1), 0, OP_MOVL_LOAD(4,4), LDS_X0(5), LDS_Y0(1) };
      translate(ops, 3, 4);
      exp[1] = hbuf[1]; exp[4] = hbuf[4];
      expect_seq("priv", 0, exp, 8);
      expect("priv lits", lit_of_movl(2) == env.shadow_vbr_addr && lit_of_movl(8) == env.shadow_vbr_addr);
      expect("priv end", blk.end_reason == END_PRIV && blk.guest_len == 6);
    }

    /* 17. trapa ends the block and is followed by a jump to the next pc */
    { sf_u32 ops[] = { 0xC3F1, 0x0009 };
      translate(ops, 2, 4);
      expect("trapa", hbuf[0] == 0xC3F1 && hbuf[1] == OP_NOP && hbuf[2] == LDS_X0(1) && blk.end_reason == END_TRAP && blk.nlinks == 1 && blk.links[0].guest_target == GBASE + 2);
      expect("trapa sentinel", blk.nmeta == 1 && blk.meta[1].host_off == 2 && blk.meta[1].guest_off == 2 && dbt_block_find(&blk, 4) == 1 && dbt_block_find(&blk, 0) == 0);
    }

    /* 18. fetch fault at the second instruction */
    { sf_u32 ops[] = { 0x0009 };
      int r; sf_u32 fa = 0;
      memset(gmem, 0, sizeof gmem); put16(GBASE + 4092, ops[0]); put16(GBASE + 4094, ops[0]);
      env.max_insns = 4;
      blk.meta = blk_meta; blk.spills = blk_spills;
      r = dbt_translate_block(&env, GBASE + 4092, HBASE, hbuf, DBT_BLOCK_WORDS, &blk, &fa);
      expect("fault", r == DBT_ERR_FETCH && fa == GBASE + 4096 && blk.guest_len == 4);
    }

    /* 19. commit points: mov.l @r1+,r2 ; mov.w @r3,r3 ; mov.l r2,@-r15 ; lds.l @r15+,pr */
    { sf_u32 ops[] = { 0x6216, 0x6331, 0x2F26, 0x4F26 };
      translate(ops, 4, 4);
      expect("commit load32", blk.meta[0].commit_rel == 0 && blk.meta[0].fix_kind == FIX_SWAP32 && blk.meta[0].fix_reg == 2);
      expect("commit load16", blk.meta[1].commit_rel == 0 && blk.meta[1].fix_kind == FIX_SWAP16 && blk.meta[1].fix_reg == 3);
      /* store: lds (2) + three swaps (6) then the store at +8 */
      expect("commit store", blk.meta[2].commit_rel == 8 && blk.meta[2].fix_kind == FIX_NONE);
      expect("commit lds.l pr", blk.meta[3].commit_rel == 2 && blk.meta[3].fix_kind == FIX_CR_BASE + CR_PR && blk.meta[3].fix_reg == 1);
      expect("sentinel no commit", blk.meta[4].commit_rel == 0xFF && blk.meta[4].kind == META_INSN);
    }

    /* 20. jsr @r1 ; mov.l @r15+,r1 : the slot writes the target register, so the
     * target is parked in X0 first and the slot's scratch slots shift */
    { sf_u32 ops[] = { 0x410B, 0x61F6 };
      translate(ops, 2, 2);
      /* set_pr (4 words: scratch r2 since r1 is the target), lds r1,x0, then the load (commit, swaps) */
      expect("jsr slot target first", hbuf[4] == LDS_X0(1) && hbuf[5] == 0x61F6);
      expect("jsr slot commit", blk.meta[0].commit_rel == 10 && blk.meta[0].fix_kind == FIX_SWAP32 && blk.meta[0].fix_reg == 1
             && blk.meta[1].kind == META_TARGET_X0 && blk.meta[1].host_off == 18);
    }
    /* 20b. jsr @r1 ; mov.l r4,@-r15 : slot does not write r1: fast form, slot keeps the normal spill slots,
     * and the PR scratch (r2) stays spilled in Y0 across the slot */
    { sf_u32 ops[] = { 0x410B, 0x2F46 };
      translate(ops, 2, 2);
      /* the store's scratch is r3: neither the target register r1 (an interrupt on its
       * restoring sts resumes at "the target" = r1 before the undo) nor the kept PR scratch r2 */
      expect("jsr fast slot store", hbuf[0] == LDS_Y0(2) && hbuf[2] == OP_LDS_PR(2) && hbuf[3] == LDS_X0(3) && hbuf[7] == 0x2F36 && hbuf[8] == STS_X0(3)
             && blk.meta[1].kind == META_TARGET_REG && blk.meta[1].fix_reg == 1 && blk.meta[1].host_off == 18);
      { dbt_spill sp[4]; int n = dbt_block_pending_spills(&blk, 0, 14, sp, 4);   /* at the store: r3 (X0) and r2 (Y0) open, latest first */
        expect("jsr fast pending", n == 2 && sp[0].gpr == 3 && sp[0].slot == SLOT_X0 && sp[1].gpr == 2 && sp[1].slot == SLOT_Y0); }
    }

    /* 20c. jsr @r3 ; mov.l r1,@(12,r2) (kernel.dll SwitchActiveProcess): the store's lowest free register
     * would be r3, the target; it must take r5 (r4 is the PR scratch) */
    { sf_u32 ops[] = { 0x430B, 0x1213 };
      int i, r3_spilled = 0;
      translate(ops, 2, 2);
      for (i = 0; i < blk.nspills; i++) if (blk.spills[i].gpr == 3) r3_spilled = 1;
      expect("jsr slot avoids the target register", !r3_spilled && blk.nspills == 2 && blk.spills[0].gpr == 4 && blk.spills[1].gpr == 5
             && blk.meta[1].kind == META_TARGET_REG && blk.meta[1].fix_reg == 3);
    }

    /* 21. rts ; fmov.s @r4+,fr0 : helper call first, target capture afterwards */
    { sf_u32 ops[] = { 0x000B, 0xF049 };
      int i, jmp_at = -1, stspr_at = -1;
      translate(ops, 2, 2);
      for (i = 0; i < blk.code_len / 2; i++) {
          if (jmp_at < 0 && (hbuf[i] & 0xF0FF) == 0x402B) jmp_at = i;          /* jmp @rT (helper) */
          if (stspr_at < 0 && (hbuf[i] & 0xF0FF) == 0x002A) stspr_at = i;      /* sts pr,rT */
      }
      expect("rts helper slot order", jmp_at >= 0 && stspr_at > jmp_at && hbuf[stspr_at + 1] == op_lds_slot(2, SLOT_Y1));
      /* the helper call commits the branch range at its jmp's delay slot: an interrupt in the
       * helper's exit tail resumes at the PR range, not by running the slot again */
      expect("rts helper slot commit", blk.meta[0].commit_rel == (jmp_at + 1) * 2 && blk.meta[1].kind == META_TARGET_PR);
    }

    /* 21b. braf r3 ; fmov.s @r4+,fr0 : helper slot, resume = r3 + PC+4 from the slot on, X0 loaded after */
    { sf_u32 ops[] = { 0x0323, 0xF049 };
      int i, x0_at = -1;
      translate(ops, 2, 2);
      for (i = blk.meta[1].host_off / 2; i < blk.code_len / 2; i++) if (hbuf[i] == LDS_X0(1)) { x0_at = i; break; }
      /* the helper call (words 0-7), then the register range starts with the X0 capture: lds r1,y0; mov.l; add r3,r1; lds r1,x0 */
      expect("braf helper slot", blk.meta[1].kind == META_TARGET_REG && blk.meta[1].fix_reg == 3 && blk.meta[1].target == GBASE + 4
             && blk.meta[1].host_off == 16 && x0_at == 11 && hbuf[10] == OP_ADD_RR(3, 1) && blk.meta[0].commit_rel == 14);
    }

    /* 22. jsr @r1 ; fmov.s @r1+,fr0 : helper slot writing the target register is rejected */
    { sf_u32 ops[] = { 0x410B, 0xF019 };
      expect("helper slot clobbers target", translate(ops, 2, 2) == DBT_ERR_UNSUPPORTED);
    }

    /* 24. bf/s +1 ; add #1,r3 : T=1 falls through, T=0 takes the branch */
    { sf_u32 ops[] = { 0x8F01, 0x7301 };
      translate(ops, 2, 2);
      expect("bf/s cond order", blk.meta[1].kind == META_COND && blk.meta[1].target == GBASE + 4 && blk.meta[1].fallthrough == GBASE + 6);
    }

    /* 25. bt/s +2 ; dt r1 : slot writes T; old T kept in rA, each path resumable */
    { sf_u32 ops[] = { 0x8D02, 0x4110 };
      translate(ops, 2, 2);
      expect("bt/s dt ranges", blk.nmeta >= 4 && blk.meta[1].kind == META_COND_REG && blk.meta[1].host_off == 6
             && blk.meta[1].fix_reg == 2 && blk.meta[1].target == GBASE + 8 && blk.meta[1].fallthrough == GBASE + 4
             && blk.meta[2].kind == META_TARGET_CONST && blk.meta[2].host_off == 14 && blk.meta[2].target == GBASE + 4
             && blk.meta[3].kind == META_TARGET_CONST && blk.meta[3].host_off == 24 && blk.meta[3].target == GBASE + 8);
      expect("bt/s dt words", hbuf[0] == LDS_Y0(2) && hbuf[1] == OP_MOVT(2) && hbuf[2] == 0x4110 && hbuf[7] == OP_SHLR(3));
      /* rA's spill is open from host 0 to its restore in the taken path: pending at the not-taken sts too */
      { dbt_spill sp[4]; int n = dbt_block_pending_spills(&blk, 2, 18, sp, 4);
        expect("bt/s dt pending", n == 2 && sp[1].gpr == 2 && sp[1].slot == SLOT_Y0 && sp[0].gpr == 3 && sp[0].slot == SLOT_Y1);
      }
    }

    /* 26. branch chaining: form A within bra range, out of range, form B back edge.
     * The 5-word sequence: [0] restore or nop, [1] lds r1,x0, [2] mov.l, [3] jmp, [4] sts x0,r1 */
    { sf_u16 code[8]; sf_u32 from = 0; int n;
      memset(code, 0, sizeof code);
      code[2] = OP_NOP; code[3] = LDS_X0(1); code[4] = 0xD100; code[5] = OP_JMP(1); code[6] = STS_X0(1);
      n = dbt_chain_direct(&code[2], HBASE + 0x104, HBASE + 0x000, 0, 0, 0, &from);  /* d = (0 - 0x108)/2 = -132 */
      expect("chain A", n == 4 && from == HBASE + 0x104 && code[2] == (0xA000 | ((-132) & 0xFFF)) && code[3] == 0x0009 && code[4] == 0xD100);
      code[2] = STS_A0(5); code[3] = LDS_X0(1);                                        /* a borrowed r5: its restore moves into the slot */
      n = dbt_chain_direct(&code[2], HBASE + 0x104, HBASE + 0x000, 0, 0, 0, &from);
      expect("chain A restore", n == 4 && code[2] == (0xA000 | ((-132) & 0xFFF)) && code[3] == STS_A0(5) && code[4] == 0xD100);
      code[2] = STS_A0(5); code[3] = LDS_X0(1);                                        /* self loop: the restore is dropped */
      n = dbt_chain_direct(&code[2], HBASE + 0x104, HBASE + 0x000, 0, 0, 1, &from);
      expect("chain A self", n == 4 && code[2] == (0xA000 | ((-132) & 0xFFF)) && code[3] == 0x0009 && code[4] == 0xD100);
      code[2] = OP_NOP; code[3] = LDS_X0(1);
      n = dbt_chain_direct(&code[2], HBASE + 0x104, HBASE + 0x104 + 4 + 2 * 2048, 0, 0, 0, &from);
      expect("chain out of range", n == 0 && code[2] == OP_NOP && code[3] == LDS_X0(1));
      n = dbt_chain_direct(&code[2], HBASE + 0x104, HBASE + 0x104 + 4 + 2 * 2047, 0, 0, 0, &from);
      expect("chain A max", n == 4 && code[2] == 0xA7FF);
      code[1] = OP_BF(4); code[2] = STS_A0(5); code[3] = LDS_X0(1); code[4] = 0xD100;  /* bt target: inverted skip is bf */
      n = dbt_chain_direct(&code[2], HBASE + 0x104, HBASE + 0x0F0, 1, 1, 0, &from);   /* d8 = (0xF0 - 0x106)/2 = -11 */
      expect("chain B", n == 8 && from == HBASE + 0x102 && code[1] == (0x8D00 | ((-11) & 0xFF)) /* bt/s */ && code[2] == STS_A0(5)
             && code[3] == 0xA002 && code[4] == 0x0009 && code[5] == OP_JMP(1));
      code[1] = OP_BF(4); code[2] = OP_NOP; code[3] = LDS_X0(1); code[4] = 0xD100;     /* no restore: plain bt, bra over 4 words */
      n = dbt_chain_direct(&code[2], HBASE + 0x104, HBASE + 0x0F0, 1, 1, 0, &from);
      expect("chain B plain", n == 6 && from == HBASE + 0x102 && code[1] == (0x8900 | ((-11) & 0xFF)) && code[2] == 0xA003
             && code[3] == 0x0009 && code[4] == 0xD100);
      code[1] = OP_BT(4); code[2] = OP_NOP; code[3] = LDS_X0(1); code[4] = 0xD100;     /* bf target, too far for disp8: form A */
      code[1] = OP_BF(4); code[2] = STS_A0(5); code[3] = LDS_X0(1); code[4] = 0xD100;  /* self loop with a restore: plain form B */
      n = dbt_chain_direct(&code[2], HBASE + 0x104, HBASE + 0x0F0, 1, 1, 1, &from);
      expect("chain B self", n == 6 && code[1] == (0x8900 | ((-11) & 0xFF)) && code[2] == 0xA003 && code[3] == 0x0009 && code[4] == 0xD100);
      code[1] = OP_BT(4); code[2] = OP_NOP; code[3] = LDS_X0(1); code[4] = 0xD100;
      n = dbt_chain_direct(&code[2], HBASE + 0x104, HBASE + 0x104 - 300, 1, 1, 0, &from);
      expect("chain B too far -> A", n == 4 && code[1] == OP_BT(4) && code[2] == (0xA000 | ((-152) & 0xFFF)) && code[3] == OP_NOP);
    }

    /* 27. link records: bt +4 is conditional and forward; a back edge is backward */
    { sf_u32 ops[] = { 0x8902, 0x7101 };
      translate(ops, 2, 2);
      expect("link cond fwd", blk.links[0].cond == 1 && blk.links[0].backward == 0 && blk.links[0].jump_off == 2);
    }
    { sf_u32 ops[] = { 0x7101, 0x8BFD };                                              /* add ; bf -2 (to the add) */
      translate(ops, 2, 2);
      expect("link cond back", blk.nlinks >= 1 && blk.links[0].cond == 1 && blk.links[0].backward == 1 && blk.links[0].guest_target == GBASE);
    }

    /* 23. exit stub offsets are recorded */
    { sf_u32 ops[] = { 0xA000 };
      translate(ops, 1, 1);
      expect("stub_off", blk.nlinks == 1 && blk.links[0].stub_off == blk.code_len);
      expect("link words", blk.links[0].w0 == OP_NOP && blk.links[0].w1 == LDS_X0(1)
             && blk.links[0].w2 == hbuf[blk.links[0].jump_off / 2 + 2] && (blk.links[0].w2 & 0xF000) == 0xD000 && blk.links[0].skip_w == 0);
    }

    /* device-aware blocks: a 32-bit load tests its address for P4 without
     * touching T, then takes the swapped path or the fallback helper */
    {
        static const sf_u32 ops[] = { 0x5211 };                      /* mov.l @(4,r1),r2 */
        static const sf_u32 exp[] = {
            0x438A,                     /* lds r3,x0 (scratch) */
            0x6313, 0x7304,             /* mov r1,r3 ; add #4,r3 : effective address */
            0x6337, 0x4329, 0x4319,     /* not ; shlr16 ; shlr8 : 0 for 0xFF.. */
            0x633B, 0x4329, 0x4319,     /* neg ; shlr16 ; shlr8 : 0 or 0xFF */
            0x7301, 0x4319, 0x4308,     /* add #1 ; shlr8 ; shll2 : 0 or 4 */
            0x0323, 0x038A,             /* braf r3 ; sts x0,r3 */
            0xA000 | 0,                 /* bra slow (patched) */
            0x0009,
            0x5211, 0x6228, 0x6229, 0x6228,   /* fast: the swapped load */
        };
        env.dev_lo = GBASE; env.dev_hi = GBASE + 0x1000;
        expect("dev load translates", translate(ops, 1, 0) == DBT_OK);
        expect_seq("dev load test sequence", 0, exp, 14);
        expect_seq("dev load fast path", 16, exp + 16, 4);
        expect("dev bra slow skips the fast path", (hbuf[14] & 0xF000) == 0xA000 && (hbuf[14] & 0xFFF) == 6);   /* over 4 fast words + bra/nop */
        {
            int w, found = 0;
            for (w = 22; w < 40 && w < blk.code_len / 2; w++)
                if ((hbuf[w] & 0xF000) == 0xD000 && lit_of_movl(2 * w) == env.rt_fallback) found = 1;
            expect("dev slow path calls the fallback helper", found);
        }
        env.dev_lo = env.dev_hi = 0;
        expect("plain load again without the range", translate(ops, 1, 0) == DBT_OK && blk.code_len < 22);
    }


    /* ---- the borrowed register: a GPR no instruction of the block touches,
     * parked in A0 for the whole block, the stores' swap scratch ---- */
    env.no_borrow = 0;

    /* 30. stores: mov.l r0,@r4 ; mov.l r0,@(4,r4) ; mov.w r5,@r4 -> r1 borrowed; the second store
     * of r0 reuses the swapped value (one instruction); then the fall-through restores r1 first */
    { sf_u32 ops[] = { 0x2402, 0x1401, 0x2451 };
      sf_u32 exp[] = { LDS_A0(1), OP_SWAPB(0,1), OP_SWAPW(1,1), OP_SWAPB(1,1), 0x2412,
                       0x1411,
                       OP_SWAPB(5,1), 0x2411,
                       STS_A0(1), LDS_X0(2), 0, OP_JMP(2), STS_X0(2) };   /* scratches avoid the borrowed r1 */
      translate(ops, 3, 3);
      exp[10] = hbuf[10];
      expect("borrow stores", blk.borrow == 1);
      expect_seq("borrow stores seq", 0, exp, 13);
      expect("borrow record", blk.nspills == 2 && blk.spills[0].gpr == 1 && blk.spills[0].slot == SLOT_A0 && blk.spills[0].start == 0 && blk.spills[0].end == 0xFFFF);
      expect("borrow commits", blk.meta[0].commit_rel == 8 && blk.meta[1].commit_rel == 0 && blk.meta[2].commit_rel == 2);
      { dbt_spill sp[4];
        expect("borrow pending", dbt_block_pending_spills(&blk, 0, 4, sp, 4) == 1 && sp[0].gpr == 1 && sp[0].slot == SLOT_A0
               && dbt_block_pending_spills(&blk, 0, 0, sp, 4) == 0
               && dbt_block_pending_spills(&blk, 3, 20, sp, 4) == 2);      /* after the exit restore: still listed, harmless */
      }
      expect("borrow link words", blk.links[0].w0 == STS_A0(1) && blk.links[0].w1 == LDS_X0(2) && (blk.links[0].w2 & 0xF000) == 0xD000);
    }

    /* 31. the cache dies with its register and at conditional branches:
     * mov.l r0,@r4 ; add #1,r0 ; mov.l r0,@r4 ; bt +2 ; mov.l r0,@r4 */
    { sf_u32 ops[] = { 0x2402, 0x7001, 0x2402, 0x8901, 0x2402 };
      translate(ops, 5, 5);
      expect("borrow cache kill def", hbuf[5] == 0x7001 && hbuf[6] == OP_SWAPB(0,1) && hbuf[9] == 0x2412);
      /* bf +4 ; sts a0,r1 ; lds r2,x0 ; mov.l ; jmp ; sts x0,r2 ; swap.b ... */
      expect("borrow cond exit", hbuf[10] == OP_BF(4) && hbuf[11] == STS_A0(1) && hbuf[12] == LDS_X0(2) && hbuf[14] == OP_JMP(2)
             && hbuf[15] == STS_X0(2) && hbuf[16] == OP_SWAPB(0,1) && hbuf[19] == 0x2412);
      expect("borrow cond link", blk.links[0].cond == 1 && blk.links[0].jump_off == 22 && blk.links[0].w0 == STS_A0(1) && blk.links[0].skip_w == OP_BF(4));
    }

    /* 32. a helper call pauses the borrow: mov.l r0,@r4 ; fadd fr1,fr2 ; mov.l r0,@r4 */
    { sf_u32 ops[] = { 0x2402, 0xF120, 0x2402 };
      int i, resume = -1;
      translate(ops, 3, 3);
      expect("borrow helper", blk.borrow == 1 && hbuf[5] == STS_A0(1) && hbuf[6] == LDS_Y0(2));
      for (i = 7; i < blk.code_len / 2; i++) if (hbuf[i] == LDS_A0(1)) { resume = i; break; }
      expect("borrow helper resume", resume > 0 && hbuf[resume - 1] == STS_Y0(2) && hbuf[resume + 1] == OP_SWAPB(0,1) && hbuf[resume + 4] == 0x2412);
      expect("borrow helper records", blk.nspills == 4 && blk.spills[0].slot == SLOT_A0 && blk.spills[0].end == 10
             && blk.spills[1].slot == SLOT_Y0 && blk.spills[2].slot == SLOT_A0 && blk.spills[2].start == resume * 2 && blk.spills[2].end == 0xFFFF);
      { dbt_spill sp[4];
        expect("borrow helper pending", dbt_block_pending_spills(&blk, 0, 20, sp, 4) == 1 && sp[0].slot == SLOT_Y0
               && dbt_block_pending_spills(&blk, 0, (sf_u32)resume * 2 + 4, sp, 4) == 1 && sp[0].slot == SLOT_A0);
      }
      expect("borrow helper return", lit_of_movl(18) == HBASE + (sf_u32)resume * 2);
    }

    /* 33. not worth it: one store and a helper; nothing to borrow for; every register touched */
    { sf_u32 ops[] = { 0x2402, 0xF120 };
      translate(ops, 2, 2);
      expect("borrow score helper", blk.borrow == -1 && hbuf[0] == LDS_X0(1));
    }
    { sf_u32 ops[] = { 0x7001, 0x6212 };
      translate(ops, 2, 2);
      expect("borrow score none", blk.borrow == -1 && hbuf[0] == 0x7001);
    }
    { sf_u32 ops[] = { 0x2402, 0x3C1C, 0x3D2C, 0x3E3C, 0x3F4C, 0x3B5C, 0x3A6C, 0x397C, 0x388C };   /* add pairs touching r1..r15 */
      translate(ops, 9, 9);
      expect("borrow all touched", blk.borrow == -1 && hbuf[0] == LDS_X0(1));
    }

    /* 34. indirect exit restores before the inline cache: mov.l r0,@r4 ; mov.l r5,@r4 ; rts ; nop */
    { sf_u32 ops[] = { 0x2402, 0x2452, 0x000B, 0x0009 };
      translate(ops, 4, 4);
      expect("borrow rts", blk.borrow == 1 && hbuf[9] == 0x0009 && hbuf[10] == STS_A0(1) && hbuf[11] == LDS_Y0(2) && hbuf[12] == OP_STS_PR(2)
             && blk.meta[3].kind == META_TARGET_PR && blk.meta[3].host_off == 20);
    }

    /* 34b. jsr @r4 ; mov.l r0,@r5 ; (next block) : the PR scratch kept across the slot is restored by the
     * inline cache after the exit restore, so it must not be the borrowed register (r1 here) */
    { sf_u32 ops[] = { 0x2502, 0x2502, 0x440B, 0x2502 };
      int i, bad = 0;
      translate(ops, 4, 4);
      /* park; swaps; store; store (cached); lds r2,y0; mov.l; lds r2,pr; store (cached); sts a0,r1; cache... */
      expect("borrow jsr kept scratch", blk.borrow == 1 && hbuf[5] == 0x2512 && hbuf[6] == LDS_Y0(2) && hbuf[8] == OP_LDS_PR(2)
             && hbuf[9] == 0x2512 && hbuf[10] == STS_A0(1) && hbuf[12] == OP_CMPEQ_RR(4, 2));
      for (i = 0; i < blk.code_len / 2; i++) if (hbuf[i] == LDS_Y0(1) || hbuf[i] == LDS_X0(1) || hbuf[i] == op_lds_slot(1, SLOT_Y1) || hbuf[i] == LDS_X1(1)) bad = 1;
      expect("borrow never spilled elsewhere", !bad);
    }

    /* 34c. a trapa (hypercall or the guest's) keeps the borrow: the exception path restores the slots */
    { sf_u32 ops[] = { 0x2402, 0x2452, 0xC3F3 };                   /* mov.l r0,@r4 ; mov.l r5,@r4 ; trapa #0xF3 */
      translate(ops, 3, 3);
      expect("borrow trapa", blk.borrow == 1 && hbuf[9] == 0xC3F3 && hbuf[10] == STS_A0(1) && blk.spills[0].end == 0xFFFF);
    }

    /* 35. control-register forms and the pre-decrement of the data register itself:
     * sts.l pr,@-r15 ; mov.l r14,@-r15 ; mov.l r4,@-r4 */
    { sf_u32 ops[] = { 0x4F22, 0x2FE6, 0x2446 };
      sf_u32 exp[] = { LDS_A0(1), OP_STS_PR(1), OP_SWAPB(1,1), OP_SWAPW(1,1), OP_SWAPB(1,1), OP_MOVL_PREDEC(1,15),
                       OP_SWAPB(14,1), OP_SWAPW(1,1), OP_SWAPB(1,1), OP_MOVL_PREDEC(1,15),
                       OP_MOV_RR(4,1), OP_ADD_IMM(-4,1), OP_SWAPB(1,1), OP_SWAPW(1,1), OP_SWAPB(1,1), 0x2416 };
      translate(ops, 3, 3);
      expect_seq("borrow ctrl", 0, exp, 16);
      expect("borrow ctrl commits", blk.meta[0].commit_rel == 10 && blk.meta[1].commit_rel == 6 && blk.meta[2].commit_rel == 10);
    }

    /* 36. device-aware access with a borrowed register: the P4 test needs no spill, a nop fills the slot */
    { sf_u32 ops[] = { 0x5211 };                                   /* mov.l @(4,r1),r2 */
      env.dev_lo = GBASE; env.dev_hi = GBASE + 0x1000;
      translate(ops, 1, 0);
      expect("borrow dev", blk.borrow == 3 && hbuf[0] == LDS_A0(3) && hbuf[1] == OP_MOV_RR(1,3) && hbuf[2] == OP_ADD_IMM(4,3)
             && hbuf[12] == OP_BRAF(3) && hbuf[13] == OP_NOP && (hbuf[14] & 0xF000) == 0xA000 && hbuf[16] == 0x5211);
      env.dev_lo = env.dev_hi = 0;
    }

    /* 37. the stc.l sr,@-rn slot of an indirect branch used A0 as its third slot: now Y0 */
    { sf_u32 ops[] = { 0x422B, 0x4203 };                           /* jmp @r2 ; stc.l sr,@-r2 */
      int i, a0 = 0;
      translate(ops, 2, 2);
      for (i = 0; i < blk.code_len / 2; i++) if ((hbuf[i] & 0xF0FF) == 0x407A || (hbuf[i] & 0xF0FF) == 0x007A) a0 = 1;
      expect("slot_x0 third slot is y0", blk.borrow == -1 && !a0);
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
