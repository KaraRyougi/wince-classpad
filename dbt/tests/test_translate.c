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

#define LDS_X0(r)  op_lds_slot(r, SLOT_X0)
#define STS_X0(r)  op_sts_slot(SLOT_X0, r)
#define LDS_X1(r)  op_lds_slot(r, SLOT_X1)
#define STS_X1(r)  op_sts_slot(SLOT_X1, r)
#define LDS_Y0(r)  op_lds_slot(r, SLOT_Y0)
#define STS_Y0(r)  op_sts_slot(SLOT_Y0, r)

int main(void)
{
    memset(&env, 0, sizeof env);
    env.fetch16 = fetch16; env.read16le = read16le; env.read32le = read32le;
    env.rt_lookup_link = 0x8CF00000; env.rt_lookup = 0x8CF00100; env.rt_rte = 0x8CF00200;
    env.rt_fpu = 0x8CF00300; env.rt_fallback = 0x8CF00400; env.rt_ldc_sr = 0x8CF00500;
    env.shadow_vbr_addr = 0x8CF01000; env.shadow_md_addr = 0x8CF01004;

    /* 1. encodings of the DSP slot transfers */
    expect("lds r1,x0", LDS_X0(1) == 0x418A);
    expect("sts x0,r1", STS_X0(1) == 0x018A);
    expect("lds r3,y0", LDS_Y0(3) == 0x43AA);

    /* 2. pass-through ALU block of 3 insns, then fall-through jump */
    { sf_u32 ops[] = { 0x3C0C /* add r0,r12 */, 0xE105 /* mov #5,r1 */, 0x2208 /* tst r0,r2 */ };
      sf_u32 exp[] = { 0x3C0C, 0xE105, 0x2208 };
      int r = translate(ops, 3, 3);
      expect("alu status", r == DBT_OK);
      expect_seq("alu pass", 0, exp, 3);
      expect("alu meta", blk.nmeta == 3 && blk.meta[1].host_off == 2 && blk.meta[2].guest_off == 4 && blk.meta[3].host_off == 6 && blk.meta[3].guest_off == 6);
      /* fall-through: lds r1,x0 ; mov.l lit,r1 ; jmp @r1 ; sts x0,r1 */
      expect("alu tail", hbuf[3] == LDS_X0(1) && (hbuf[4] & 0xFF00) == 0xD100 && hbuf[5] == OP_JMP(1) && hbuf[6] == STS_X0(1));
      expect("alu link", blk.nlinks == 1 && blk.links[0].guest_target == GBASE + 6);
      /* the link literal initially points at the exit stub, which loads the guest target */
      { sf_u32 stub = lit_of_movl(8); int so = stub - HBASE;
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

    /* 8. bra with a slot: bra +8 ; nop   -> nop ; direct jump to GBASE+4+8 */
    { sf_u32 ops[] = { 0xA004, 0x0009 };
      sf_u32 exp[] = { 0x0009, LDS_X0(1) };
      translate(ops, 2, 4);
      expect_seq("bra", 0, exp, 2);
      expect("bra link", blk.nlinks == 1 && blk.links[0].guest_target == GBASE + 12 && blk.end_reason == END_BRANCH && blk.guest_len == 4);
      expect("bra meta", blk.nmeta == 2 && blk.meta[1].kind == META_TARGET_CONST && blk.meta[1].target == GBASE + 12 && blk.meta[1].host_off == 2 && blk.meta[2].guest_off == 4);
    }

    /* 9. bsr: PR = GBASE+4, slot executes after PR write */
    { sf_u32 ops[] = { 0xB010, 0x6213 /* mov r1,r2 */ };
      sf_u32 exp[] = { LDS_Y0(3), 0xD300 /* mov.l lit,r3 (disp patched) */, OP_LDS_PR(3), STS_Y0(3), 0x6213, LDS_X0(1) };
      translate(ops, 2, 4);
      exp[1] = hbuf[1];   /* displacement is pool-dependent; check the literal instead */
      expect_seq("bsr", 0, exp, 6);
      expect("bsr pr", lit_of_movl(2) == GBASE + 4);
    }

    /* 10. rts ; nop  -> target to X0 before the slot, then lookup */
    { sf_u32 ops[] = { 0x000B, 0x0009 };
      sf_u32 exp[] = { LDS_Y0(1), OP_STS_PR(1), LDS_X0(1), STS_Y0(1), 0x0009, LDS_Y0(1) };
      translate(ops, 2, 4);
      expect_seq("rts", 0, exp, 6);
      expect("rts lookup", lit_of_movl(12) == env.rt_lookup && hbuf[7] == OP_JMP(1) && hbuf[8] == STS_Y0(1));
    }

    /* 11. jmp @r2 with slot that redefines r2: target captured first */
    { sf_u32 ops[] = { 0x422B, 0x6233 /* mov r3,r2 */ };
      sf_u32 exp[] = { LDS_X0(2), 0x6233, LDS_Y0(1) };
      translate(ops, 2, 4);
      expect_seq("jmp clobber", 0, exp, 3);
      expect("jmp meta", blk.nmeta == 2 && blk.meta[1].kind == META_TARGET_X0 && blk.meta[1].host_off == 4);
    }

    /* 12. jsr @r4 ; nop : PR first, then X0, then the slot */
    { sf_u32 ops[] = { 0x440B, 0x0009 };
      sf_u32 exp[] = { LDS_Y0(1), 0, OP_LDS_PR(1), STS_Y0(1), LDS_X0(4), 0x0009, LDS_Y0(1) };
      translate(ops, 2, 4);
      exp[1] = hbuf[1];
      expect_seq("jsr", 0, exp, 7);
      expect("jsr pr", lit_of_movl(2) == GBASE + 4);
    }

    /* 13. bt +4 (no slot): bf over the jump, block continues */
    { sf_u32 ops[] = { 0x8902, 0x7101 /* add #1,r1 */ };
      sf_u32 exp[] = { OP_BF(3), LDS_X0(1), 0, OP_JMP(1), STS_X0(1), 0x7101 };
      translate(ops, 2, 2);
      exp[2] = hbuf[2];
      expect_seq("bt", 0, exp, 6);
      expect("bt links", blk.nlinks == 2 && blk.links[0].guest_target == GBASE + 8 && blk.links[1].guest_target == GBASE + 4);
    }

    /* 14. bt/s with a T-writing slot: bt/s +6 ; cmp/eq r1,r2 */
    { sf_u32 ops[] = { 0x8D03, 0x3120 };
      sf_u32 exp[] = { LDS_Y0(3), OP_MOVT(3), 0x3120, op_lds_slot(4, SLOT_Y1), OP_MOVT(4), OP_TST_RR(3,3), OP_BF(4),
                       OP_SHLR(4), op_sts_slot(SLOT_Y1, 4), STS_Y0(3), OP_BRA(7), OP_NOP,
                       OP_SHLR(4), op_sts_slot(SLOT_Y1, 4), STS_Y0(3), LDS_X0(1) };
      translate(ops, 2, 4);
      expect_seq("bt/s T", 0, exp, 16);
      /* bf(4) from offset 12 lands on offset 12+4+8 = 24 = taken path */
      expect("bt/s bf target", 12 + 4 + 4 * 2 == 24 && hbuf[12] == OP_SHLR(4));
      /* bra(7) at offset 20 lands at 20+4+14 = 38, right after the 4-word jump at 30..37 */
      expect("bt/s bra target", 20 + 4 + 7 * 2 == 38 && blk.meta[4].host_off == 38 && blk.meta[4].kind == META_INSN
             && blk.meta[1].kind == META_COND_REG && blk.meta[1].fix_reg == 3);
    }

    /* 14b. bt/s +6 ; nop : slot, COND boundary, inverted conditional, jump */
    { sf_u32 ops[] = { 0x8D03, 0x0009 };
      translate(ops, 2, 4);
      expect("bt/s cond", hbuf[0] == 0x0009 && hbuf[1] == OP_BF(3) && blk.meta[1].kind == META_COND
             && blk.meta[1].target == GBASE + 4 + 6 && blk.meta[1].fallthrough == GBASE + 4 && blk.meta[1].host_off == 2);
    }

    /* 15. FPU: fadd fr1,fr2 -> helper call with opcode in X0, return address in X1 */
    { sf_u32 ops[] = { 0xF120, 0x0009 };
      translate(ops, 2, 2);
      expect("fpu seq", hbuf[0] == LDS_Y0(1) && (hbuf[1] & 0xFF00) == 0xD100 && hbuf[2] == LDS_X0(1)
                        && (hbuf[3] & 0xFF00) == 0xD100 && hbuf[4] == LDS_X1(1) && (hbuf[5] & 0xFF00) == 0xD100
                        && hbuf[6] == OP_JMP(1) && hbuf[7] == STS_Y0(1) && hbuf[8] == 0x0009);
      expect("fpu lits", lit_of_movl(2) == 0xF120 && lit_of_movl(6) == HBASE + 16 && lit_of_movl(10) == env.rt_fpu);
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
      expect("trapa", hbuf[0] == 0xC3F1 && hbuf[1] == LDS_X0(1) && blk.end_reason == END_TRAP && blk.nlinks == 1 && blk.links[0].guest_target == GBASE + 2);
      expect("trapa sentinel", blk.nmeta == 1 && blk.meta[1].host_off == 2 && blk.meta[1].guest_off == 2 && dbt_block_find(&blk, 4) == 1 && dbt_block_find(&blk, 0) == 0);
    }

    /* 18. fetch fault at the second instruction */
    { sf_u32 ops[] = { 0x0009 };
      int r; sf_u32 fa = 0;
      memset(gmem, 0, sizeof gmem); put16(GBASE + 4092, ops[0]); put16(GBASE + 4094, ops[0]);
      env.max_insns = 4;
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

    /* 20. jsr @r1 ; mov.l r4,@-r15 : the slot store must not touch X0 (the target) */
    { sf_u32 ops[] = { 0x410B, 0x2F46 };
      translate(ops, 2, 2);
      /* set_pr (4 words), lds r1,x0, then the store with its scratch in X1 */
      expect("jsr slot target first", hbuf[4] == LDS_X0(1));
      expect("jsr slot spill shifted", hbuf[5] == LDS_X1(1) && hbuf[9] == 0x2F16 && hbuf[10] == STS_X1(1));
      expect("jsr slot commit", blk.meta[0].commit_rel == 18 && blk.meta[1].kind == META_TARGET_X0 && blk.meta[1].host_off == 22);
      { dbt_spill sp[4];
        expect("jsr slot pending@store", dbt_block_pending_spills(&blk, 0, 20, sp, 4) == 1 && sp[0].slot == SLOT_X1 && sp[0].gpr == 1);
      }
    }

    /* 21. rts ; fmov.s @r4+,fr0 : helper call first, target capture afterwards */
    { sf_u32 ops[] = { 0x000B, 0xF049 };
      int i, jmp_at = -1, stspr_at = -1;
      translate(ops, 2, 2);
      for (i = 0; i < blk.code_len / 2; i++) {
          if (jmp_at < 0 && (hbuf[i] & 0xF0FF) == 0x402B) jmp_at = i;          /* jmp @rT (helper) */
          if (stspr_at < 0 && (hbuf[i] & 0xF0FF) == 0x002A) stspr_at = i;      /* sts pr,rT */
      }
      expect("rts helper slot order", jmp_at >= 0 && stspr_at > jmp_at && hbuf[stspr_at + 1] == LDS_X0((hbuf[stspr_at] >> 8) & 0xF));
      expect("rts helper slot no commit", blk.meta[0].commit_rel == 0xFF && blk.meta[1].kind == META_TARGET_X0);
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

    /* 26. branch chaining: form A within bra range, out of range, form B back edge */
    { sf_u16 code[8]; sf_u32 from = 0; int n;
      memset(code, 0, sizeof code);
      code[2] = LDS_X0(1); code[3] = 0xD100; code[4] = OP_JMP(1); code[5] = STS_X0(1);
      n = dbt_chain_direct(&code[2], HBASE + 0x104, HBASE + 0x000, 0, 0, &from);     /* d = (0 - 0x108)/2 = -132 */
      expect("chain A", n == 4 && from == HBASE + 0x104 && code[2] == (0xA000 | ((-132) & 0xFFF)) && code[3] == 0x0009 && code[4] == OP_JMP(1));
      code[2] = LDS_X0(1); code[3] = 0xD100;
      n = dbt_chain_direct(&code[2], HBASE + 0x104, HBASE + 0x104 + 4 + 2 * 2048, 0, 0, &from);
      expect("chain out of range", n == 0 && code[2] == LDS_X0(1));
      n = dbt_chain_direct(&code[2], HBASE + 0x104, HBASE + 0x104 + 4 + 2 * 2047, 0, 0, &from);
      expect("chain A max", n == 4 && code[2] == 0xA7FF);
      code[1] = OP_BF(3); code[2] = LDS_X0(1); code[3] = 0xD100;                      /* bt target: inverted skip is bf */
      n = dbt_chain_direct(&code[2], HBASE + 0x104, HBASE + 0x0F0, 1, 1, &from);      /* d8 = (0xF0 - 0x106)/2 = -11 */
      expect("chain B", n == 6 && from == HBASE + 0x102 && code[1] == (0x8900 | ((-11) & 0xFF)) && code[2] == 0xA002 && code[3] == 0x0009);
      code[1] = OP_BT(3); code[2] = LDS_X0(1); code[3] = 0xD100;                      /* bf target, too far for disp8: form A */
      n = dbt_chain_direct(&code[2], HBASE + 0x104, HBASE + 0x104 - 300, 1, 1, &from);
      expect("chain B too far -> A", n == 4 && code[1] == OP_BT(3) && code[2] == (0xA000 | ((-152) & 0xFFF)));
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
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
