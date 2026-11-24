/*
 * stubs.S - assembly half of the translator runtime (big-endian SH7305).
 *
 * DSP transfer registers are the spill slots: A0=7 X0=8 X1=9 Y0=10 Y1=11
 * (lds Rm,<slot> = 0x400A | m<<8 | code<<4 ; sts <slot>,Rn = 0x000A | n<<8 | code<<4).
 * They are emitted as .word so no DSP assembler support is needed.
 *
 * IL memory (4 KiB at 0xE5200000 = VBR) layout packs stubs into the gaps
 * between SH-4 exception vectors (+0x000..+0x0FF, +0x124..+0x3FF, +0x400..+0x5FF)
 * so all fast paths (TLB refill, WinCE API call 0x0E0, FPU arith, RTE, lookups,
 * helpers, exception entry) run at zero wait states.
 *
 * Window labels (dbt_*_s1.., dbt_hit_*, dbt_full_m*) are read by rt.c to
 * undo partial register saves when an exception lands inside a stub.
 */
#include "ctx_offsets.h"
#ifndef DBT_HASH_ENTRIES
#define DBT_HASH_ENTRIES 8192
#endif
#ifndef CACHE_BASE
#define CACHE_BASE 0x8CC00000
#endif
#ifndef CACHE_SIZE
#define CACHE_SIZE 0x00400000
#endif
#if defined(DBT_HASH_ASID_MIX) && !defined(DBT_HASH_ASID_TAG)
#error DBT_HASH_ASID_MIX requires tagged hash entries
#endif
#if defined(DBT_HASH_TWO_WAY) && (!defined(DBT_HASH_ASID_TAG) || defined(DBT_HASH_ASID_MIX))
#error DBT_HASH_TWO_WAY requires tagged entries without ASID mixing
#endif
#if defined(DBT_FAST_LDC_SR_MD) && !defined(DBT_FAST_LDC_SR)
#error DBT_FAST_LDC_SR_MD requires DBT_FAST_LDC_SR
#endif

#define SLOT_A0 7
#define SLOT_X0 8
#define SLOT_X1 9
#define SLOT_Y0 10
#define SLOT_Y1 11

        .macro LDSX reg, code          /* lds r\reg, slot */
        .word 0x400A | (\reg << 8) | (\code << 4)
        .endm
        .macro STSX code, reg          /* sts slot, r\reg */
        .word 0x000A | (\reg << 8) | (\code << 4)
        .endm
        .macro ICBI reg                /* icbi @r\reg (SH-4A) */
        .word 0x00E3 | (\reg << 8)
        .endm
        .macro SYNCO
        .word 0x00AB
        .endm
        .macro STCSGR reg              /* stc sgr, r\reg (SH-4A): r15 as it was at the exception */
        .word 0x003A | (\reg << 8)
        .endm

/* Save the guest context into dbt_ctx_rt with descending stores.
 * Precondition: guest r0,r1,r2 are in Y0,Y1,A0; r1 holds the guest SR;
 * r0 and r2 are free. Postcondition: r2 = &dbt_ctx_rt, r0/r1 clobbered. */
        .macro SAVE_CTX_RT sfx
        mov.l   .Lctx_rt_rend\sfx, r2
        mov.l   r15, @-r2
        mov.l   r14, @-r2
        mov.l   r13, @-r2
        mov.l   r12, @-r2
        mov.l   r11, @-r2
        mov.l   r10, @-r2
        mov.l   r9, @-r2
        mov.l   r8, @-r2
        mov.l   r7, @-r2
        mov.l   r6, @-r2
        mov.l   r5, @-r2
        mov.l   r4, @-r2
        mov.l   r3, @-r2
        STSX    SLOT_A0, 0
        mov.l   r0, @-r2               /* guest r2 */
        STSX    SLOT_Y1, 0
        mov.l   r0, @-r2               /* guest r1 */
        STSX    SLOT_Y0, 0
        mov.l   r0, @-r2               /* guest r0 */
        sts     macl, r0
        mov.l   r0, @-r2
        sts     mach, r0
        mov.l   r0, @-r2
        stc     gbr, r0
        mov.l   r0, @-r2
        sts     pr, r0
        mov.l   r0, @-r2
        mov.l   r1, @-r2               /* guest sr */
        .endm

/* Exception vector thunk (36 bytes): save bank-1 r0..r7, branch to common. */
        .macro EXC_ENTRY vec
        mov.l   1f, r15                /* r15 is in SGR; use it as the first scratch */
        mov.l   r7, @-r15              /* bank 1 -> ctx->b1[7..0] */
        mov.l   r6, @-r15
        mov.l   r5, @-r15
        mov.l   r4, @-r15
        mov.l   r3, @-r15
        mov.l   r2, @-r15
        mov.l   r1, @-r15
        mov.l   r0, @-r15
        mov     #(\vec >> 8), r0
        shll8   r0
        bra     _dbt_exc_common
        nop
        .align 2
1:      .long   _dbt_ctx_exc + CTX_B1END
        .endm

/* ================================================================== */
/* ILRAM subsection 0: starts at 0xE5200000 = VBR                     */
/* ================================================================== */
        .section .ilram, "ax", @progbits
        .subsection 0
        .balign 4
        .global _dbt_stubs_start, _dbt_vectors
_dbt_stubs_start:
_dbt_vectors:

/* -------- Gap 0: +0x000 .. +0x0FF (256 bytes) --------------------- */
/* dbt_enter(r4 = host entry): install VBR, set MD|DSP, interrupts masked */
        .global _dbt_enter
_dbt_enter:
        mov.l   .Lvectors0, r0
        ldc     r0, vbr
        mov.l   .Lsr_init0, r0
        ldc     r0, sr
        jmp     @r4
        nop

/* dbt_sync_code(r4 = addr, r5 = len): write back data, invalidate icache */
        .global _dbt_sync_code
_dbt_sync_code:
        mov     r4, r0
        add     r5, r0                 /* end */
        mov     #-32, r1
        and     r1, r4                 /* line align */
        mov     r4, r2                 /* retain first aligned line */
1:      ocbwb   @r4
        add     #32, r4
        cmp/hs  r0, r4
        bf      1b
        SYNCO                          /* dirty bytes reach RAM before I invalidation */
2:      ICBI    2
        add     #32, r2
        cmp/hs  r0, r2
        bf      2b
        SYNCO
        rts
        nop

/* hit tail: r1 = host target; guest r0,r2 in Y0,A0, r1 in Y1 */
        .global _dbt_tail_hit, _dbt_hit_a, _dbt_hit_b, _dbt_hit_c, _dbt_hit_end
_dbt_tail_hit:
_dbt_hit_a:
        STSX    SLOT_Y0, 0
_dbt_hit_b:
        STSX    SLOT_A0, 2
_dbt_hit_c:
        jmp     @r1
        STSX    SLOT_Y1, 1
_dbt_hit_end:

/* Bank-switch trampoline: entered via rte with guest's new SR. */
        .global _dbt_bank_tramp, _dbt_bt_s1, _dbt_bt_end
_dbt_bank_tramp:
        LDSX    1, SLOT_X0
_dbt_bt_s1:
        mov.l   .Lctx_sw0, r1
        ldc.l   @r1+, spc
        ldc.l   @r1+, ssr
        mov.l   @r1, r1                /* host target */
        jmp     @r1
        STSX    SLOT_X0, 1
_dbt_bt_end:

/* Lightweight FPU arithmetic & 2-op batch stub: saves only caller-saved regs
 * (r3-r7, pr, mach, macl, r15, sr; r0-r2 are in Y0/Y1/A0). Falls back to
 * _dbt_hfpu_masked on SR.FD == 1 or FPU_EXCEPTION / FPU_UNIMPL. */
        .global _dbt_fpu_arith_stub, _dbt_hfa_s1, _dbt_hfa_s2, _dbt_hfa_s3, _dbt_hfa_masked
        .global _dbt_fafast_a, _dbt_fafast_b, _dbt_fafast_c, _dbt_fafast_end
_dbt_fpu_arith_stub:
        LDSX    0, SLOT_Y0
_dbt_hfa_s1:
        LDSX    1, SLOT_Y1
_dbt_hfa_s2:
        LDSX    2, SLOT_A0
_dbt_hfa_s3:
        stc     sr, r1
        mov.l   .Limask0, r2
        or      r1, r2
        ldc     r2, sr
_dbt_hfa_masked:
        mov     r1, r0
        shll16  r0
        cmp/pz  r0
        bf      .Lfpu_arith_fd
        mov.l   .Lstack_rt0, r2
        mov.l   r15, @-r2
        mov     r2, r15
        mov.l   r7, @-r15
        mov.l   r6, @-r15
        mov.l   r5, @-r15
        mov.l   r4, @-r15
        mov.l   r3, @-r15
        sts.l   pr, @-r15
        sts.l   mach, @-r15
        sts.l   macl, @-r15
        mov.l   r1, @-r15              /* saved guest SR at @(0, r15) */
        mov     r15, r4                /* arg0 = &sr */
        STSX    SLOT_X0, 5             /* arg1 = op0 or (op1<<16)|op0 */
        mov.l   .Lfpu_arith_c0, r0
        jsr     @r0
        nop
        tst     r0, r0
        bf      .Lfpu_arith_err
        mov.l   @r15+, r0              /* updated guest SR (T bit) */
        lds.l   @r15+, macl
        lds.l   @r15+, mach
        lds.l   @r15+, pr
        mov.l   @r15+, r3
        mov.l   @r15+, r4
        mov.l   @r15+, r5
        mov.l   @r15+, r6
        mov.l   @r15+, r7
        mov.l   @r15, r15
        STSX    SLOT_X1, 1             /* return host address */
        ldc     r0, sr                 /* guest SR & interrupts back */
_dbt_fafast_a:
        STSX    SLOT_Y0, 0
_dbt_fafast_b:
        STSX    SLOT_A0, 2
_dbt_fafast_c:
        jmp     @r1
        STSX    SLOT_Y1, 1
_dbt_fafast_end:
.Lfpu_arith_err:
        LDSX    0, SLOT_X0             /* fallback arg (op0, op0|0x10000, or op1|0x20000) */
        mov.l   @r15+, r1              /* guest SR (with any op0 T update) */
        lds.l   @r15+, macl
        lds.l   @r15+, mach
        lds.l   @r15+, pr
        mov.l   @r15+, r3
        mov.l   @r15+, r4
        mov.l   @r15+, r5
        mov.l   @r15+, r6
        mov.l   @r15+, r7
        bra     _dbt_hfpu_masked
        mov.l   @r15, r15
.Lfpu_arith_fd:
        STSX    SLOT_X0, 0
        cmp/pz  r0                     /* single op (<0x80000000) keeps bit 16 (in_slot) */
        bt      1f
        extu.w  r0, r0                 /* 2-op batch: strip op1 so op0 faults cleanly */
        LDSX    0, SLOT_X0
1:      bra     _dbt_hfpu_masked
        nop

        .align 2
.Lvectors0:     .long _dbt_vectors
.Lsr_init0:     .long 0x400010F0       /* MD | DSP | IMASK=15 */
.Lctx_sw0:      .long _dbt_ctx_rt + CTX_SWSPC
.Limask0:       .long 0x000000F0
.Lstack_rt0:    .long _dbt_stack_rt_top
.Lfpu_arith_c0: .long _dbt_fpu_arith_c

/* -------- Vector +0x100 & Gap 1: +0x100 .. +0x3FF (768 bytes) ----- */
        .org    _dbt_vectors + 0x100
        EXC_ENTRY 0x100

/* Guest rte. Entered after the guest's delay slot with the guest SR. */
        .align 2
        .global _dbt_rte_stub
_dbt_rte_stub:
        LDSX    0, SLOT_Y0
        LDSX    1, SLOT_Y1
        LDSX    2, SLOT_A0
        stc     sr, r1
        mov.l   .LimaskC, r2
        or      r1, r2
        ldc     r2, sr
#ifdef DBT_FAST_RTE
        mov.l   .Lrte_srsaveC, r0
        mov.l   r1, @r0
        mov.l   .Lrte_trackedC, r0
        mov.l   @r0, r0
        tst     r0, r0
        bf      7f                     /* PTEH tracked by dev_mem: skip uncached P4 read */
        mov.l   .Lrte_ptehC, r0
        mov.l   @r0, r0
        extu.b  r0, r0
        mov.l   .Lrte_asidC, r2
        mov.l   @r2, r2
        cmp/eq  r0, r2
        bf      8f
7:      stc     spc, r0                /* r0 = guest target */
        mov     r0, r1
        shlr    r1
        swap.w  r0, r2
        xor     r2, r1                 /* hidx() */
        cmp/pz  r0
        bf      5f                     /* kernel: global */
        mov.l   .Lasid_maskC, r2
        mov.l   @r2, r2
        tst     r2, r0                 /* T = 1 iff ASID-scoped */
        bf      5f                     /* CE 5.0 Slots 1-63: global */
#ifdef DBT_HASH_ASID_MIX
        mov.l   .Lrte_asidC, r2
        mov.l   @r2, r2
        extu.b  r2, r2
        shll2   r2
        shll2   r2
        shll    r2
        xor     r2, r1
#endif
        mov.l   .LhmaskC, r2
        and     r2, r1
        shll2   r1
        shll    r1                     /* 8-byte packed hent: index * 8 */
        mov.l   .LhashC, r2
        add     r2, r1
        mov.l   @r1, r2
        cmp/eq  r0, r2
        bf      8f
        mov.l   @(4, r1), r1           /* host_asid */
        tst     r1, r1
        bt      8f
#ifdef DBT_HASH_ASID_TAG
        mov     r1, r2
        shlr16  r2
        shlr8   r2                     /* stored ASID */
        mov.l   .Lrte_asidC, r0
        mov.l   @r0, r0
        cmp/eq  r0, r2
        bf      8f
        shll8   r1
        shlr8   r1
        mov.l   .Lcache_hiC, r2        /* restore top byte 0x8C */
        or      r2, r1
#endif
        bra     4f
        nop
5:      mov.l   .LhmaskC, r2
        and     r2, r1
        shll2   r1
        shll    r1
        mov.l   .LhashC, r2
        add     r2, r1
        mov.l   @r1, r2
        cmp/eq  r0, r2
        bf      8f
        mov.l   @(4, r1), r1           /* full host addr */
        tst     r1, r1
        bt      8f
4:      ldc     r1, spc
        stc     ssr, r0
        mov.l   .Lrte_mdmaskC, r2
        and     r0, r2
        mov.l   .Lrte_mdC, r1
        mov.l   r2, @r1                /* dbt_shadow_md = SSR & MD */
        mov.l   .Lrte_forceC, r2
        or      r2, r0
        ldc     r0, ssr                /* host runs privileged with DSP */
        mov.l   .Lrte_countC, r1
        mov.l   @r1, r0
        add     #1, r0
        mov.l   r0, @r1
        STSX    SLOT_Y0, 0
        STSX    SLOT_Y1, 1
        STSX    SLOT_A0, 2
        rte
        nop
8:      mov.l   .Lrte_srsaveC, r0
        mov.l   @r0, r1
#endif
        SAVE_CTX_RT C
        stc     spc, r4                /* arg0 = guest spc */
        mov.l   .Lctx_rtC, r5
        add     #CTX_SSR, r5           /* arg1 = &ctx->ssr */
        stc     ssr, r0
        mov.l   r0, @r5
        mov.l   .Lstack_rtC, r15
        mov.l   .Lrte_cC, r0
        jsr     @r0
        nop
        mov.l   .Lctx_rtC, r2
        mov     r2, r1
        add     #CTX_SSR, r1
        mov.l   @r1, r1
        ldc     r1, ssr
        ldc     r0, spc
        mov.l   @(CTX_PR, r2), r0
        lds     r0, pr
        mov.l   @(CTX_GBR, r2), r0
        ldc     r0, gbr
        mov.l   @(CTX_MACH, r2), r0
        lds     r0, mach
        mov.l   @(CTX_MACL, r2), r0
        lds     r0, macl
        mov.l   @(CTX_R0 + 0, r2), r0
        LDSX    0, SLOT_Y0
        mov.l   @(CTX_R0 + 4, r2), r1
        mov.l   @(CTX_R0 + 12, r2), r3
        mov.l   @(CTX_R0 + 16, r2), r4
        mov.l   @(CTX_R0 + 20, r2), r5
        mov.l   @(CTX_R0 + 8, r2), r0
        LDSX    0, SLOT_A0
        add     #(CTX_R0 + 24), r2
        mov.l   @(0, r2), r6
        mov.l   @(4, r2), r7
        mov.l   @(8, r2), r8
        mov.l   @(12, r2), r9
        mov.l   @(16, r2), r10
        mov.l   @(20, r2), r11
        mov.l   @(24, r2), r12
        mov.l   @(28, r2), r13
        mov.l   @(32, r2), r14
        mov.l   @(36, r2), r15
        STSX    SLOT_A0, 2
        STSX    SLOT_Y0, 0
        rte
        nop

/* RTE trampoline (target not translated yet at rte): entered in new context. */
        .global _dbt_rte_tramp, _dbt_rt_s1, _dbt_rt_s2, _dbt_rt_s3, _dbt_rt_masked, _dbt_rte_tramp_end
_dbt_rte_tramp:
        LDSX    0, SLOT_Y0
_dbt_rt_s1:
        LDSX    1, SLOT_Y1
_dbt_rt_s2:
        LDSX    2, SLOT_A0
_dbt_rt_s3:
        stc     sr, r1
        mov.l   .LimaskC, r2
        or      r1, r2
        ldc     r2, sr
_dbt_rt_masked:
        SAVE_CTX_RT C
        mov     r2, r4
        add     #100, r4
        add     #(CTX_SWTARGET - 100), r4
        mov.l   @r4, r4
        mov     #0, r5
        mov.l   .Lstack_rtC, r15
        mov.l   .LresolveC, r0
        jsr     @r0
        nop
        bra     _dbt_tail_full_prep
        nop
_dbt_rte_tramp_end:

        .align 2
.LimaskC:       .long 0x000000F0
.Lctx_rtC:      .long _dbt_ctx_rt
.Lctx_rt_rendC: .long _dbt_ctx_rt + CTX_REND
.Lstack_rtC:    .long _dbt_stack_rt_top
.Lrte_cC:       .long _dbt_rte_c
.LresolveC:     .long _dbt_resolve
#ifdef DBT_FAST_RTE
.LhmaskC:       .long (DBT_HASH_ENTRIES - 1)
.LhashC:        .long _dbt_hash
.Lasid_maskC:   .long _dbt_asid_va_mask
.Lcache_hiC:    .long (CACHE_BASE & 0xFF000000)
.Lrte_trackedC: .long _dbt_pteh_tracked
.Lrte_ptehC:    .long 0xFF000000
.Lrte_asidC:    .long _dbt_current_asid
.Lrte_mdC:      .long _dbt_shadow_md
.Lrte_mdmaskC:  .long 0x40000000
.Lrte_forceC:   .long 0x40001000
.Lrte_srsaveC:  .long _dbt_rte_sr_save
.Lrte_countC:   .long _dbt_fast_rte_calls
#endif

/* -------- Vector +0x400 & Gap 2: +0x400 .. +0x5FF (512 bytes) ----- */
/* Direct ILRAM CE 5.0 UTLB miss refill (SectionTable -> SECTION -> MEMBLOCK
 * -> aPages + alk&aky, 4 KiB non-code page). Saves only bank-1 r0..r3 into
 * XYRAM dbt_tlbf_save[4]; falls back to full EXC_ENTRY 0x400 on any edge case. */
        .org    _dbt_vectors + 0x400
#ifdef DBT_TLB_REFILL
        mov.l   .Ltlbf_save4, r15
        mov.l   r0, @r15
        mov.l   r1, @(4, r15)
        mov.l   r2, @(8, r15)
        mov.l   r3, @(12, r15)
        mov.l   .Ltlbf_en4, r0
        mov.l   @r0, r0
        mov.l   .Ltlbf_shvbr4, r1
        mov.l   @r1, r1
        cmp/eq  r1, r0
        bf      .Ltlbf_slow            /* not CE5 handler, VBR changed, or nested exc */
        stc     ssr, r0
        shll2   r0
        cmp/pz  r0
        bf      .Ltlbf_slow            /* guest was in bank 1 (RB=1) */
        stc     spc, r0
        mov.l   .Ltlbf_cbase4, r1
        sub     r1, r0
        mov.l   .Ltlbf_csize4, r1
        cmp/hs  r1, r0
        bt      .Ltlbf_slow            /* host SPC outside code cache */
        mov.l   .Ltlbf_ccn4, r3        /* 0xFF000000 */
        mov.l   @(0x24, r3), r0        /* EXPEVT */
        cmp/eq  #0x40, r0
        bt      1f
        cmp/eq  #0x60, r0
        bf      .Ltlbf_slow
1:      mov.l   @(0x0C, r3), r1        /* r1 = TEA */
        cmp/pz  r1
        bf      .Ltlbf_slow            /* TEA >= 0x80000000 */
        /* Verify hardware ASID matches dbt_current_asid, keeping PTEH in r0 */
        mov.l   @r3, r0                /* r0 = PTEH */
        extu.b  r0, r2
        mov.l   .Ltlbf_asid4, r15
        mov.l   @r15, r15
        cmp/eq  r15, r2
        bf      .Ltlbf_slow
        /* Verify 4 KiB page has NO translated user code (dbt_user_pages nibble == 0); r1 keeps TEA */
        shlr8   r0
        shlr2   r0                     /* p = PTEH >> 10 */
        mov     r0, r2
        shlr8   r2
        shlr2   r2
        shlr2   r2
        xor     r2, r0
        mov.l   .Ltlbf_hmask4, r2      /* 0x1FFC */
        and     r2, r0
        mov     #31, r2
        and     r0, r2                 /* bit shift in word */
        mov     #-5, r15
        shld    r15, r0
        shll2   r0                     /* byte offset into dbt_user_pages */
        mov.l   .Ltlbf_pages4, r15
        mov.l   @(r0, r15), r0
        mov     #15, r15
        shld    r2, r15
        tst     r15, r0
        bf      .Ltlbf_slow            /* possible user code page -> C path */
        /* Walk CE 5.0 SectionTable -> SECTION -> MEMBLOCK -> aPages (r1 = TEA) */
        mov.l   @(0x08, r3), r2        /* r2 = TTB (SectionTable) */
        mov     r2, r0
        rotl    r0
        rotl    r0
        and     #15, r0
        cmp/eq  #2, r0                 /* p12_word(TTB) */
        bf      .Ltlbf_slow
        mov     r1, r0
        shlr16  r0
        shlr8   r0
        shlr    r0                     /* slot = TEA >> 25 */
        shll2   r0
        mov.l   @(r0, r2), r2          /* LE SectionTable[slot] */
        swap.b  r2, r2
        swap.w  r2, r2
        swap.b  r2, r2                 /* r2 = sect */
        mov     r2, r0
        rotl    r0
        rotl    r0
        and     #15, r0
        cmp/eq  #2, r0                 /* p12_word(sect) */
        bf      .Ltlbf_slow
        mov     r1, r0
        shlr16  r0
        mov.w   .Ltlbf_511, r15
        and     r15, r0                /* (TEA >> 16) & 0x1FF */
        shll2   r0
        mov.l   @(r0, r2), r2          /* LE sect[blk_idx] */
        swap.b  r2, r2
        swap.w  r2, r2
        swap.b  r2, r2                 /* r2 = blk (MEMBLOCK *) */
        mov     r2, r0
        rotl    r0
        rotl    r0
        and     #15, r0
        cmp/eq  #2, r0                 /* p12_word(blk): rejects NULL/RESERVED */
        bf      .Ltlbf_slow
        /* Access lock check: (blk->alk & pCurThread->aky) != 0; swap invariant! */
        mov     r4, r0                 /* r4_bank1 = pCurThread */
        rotl    r0
        rotl    r0
        and     #15, r0
        cmp/eq  #2, r0                 /* p12_word(pCurThread) */
        bf      .Ltlbf_slow
        mov.l   @(0x14, r4), r0        /* LE pCurThread->aky */
        mov.l   @r2, r15               /* LE blk->alk */
        tst     r15, r0
        bt      .Ltlbf_slow            /* access key mismatch -> tlb50 */
        /* Read entry = bswap32(blk->aPages[(TEA >> 12) & 15]) */
        mov     r1, r0
        shlr8   r0
        shlr2   r0
        and     #0x3C, r0
        add     #12, r0
        mov.l   @(r0, r2), r2          /* LE entry */
        swap.b  r2, r2
        swap.w  r2, r2
        swap.b  r2, r2
        mov     r2, r0
        tst     #1, r0                 /* bit 0 = CE5 valid bit */
        bt      .Ltlbf_slow
        add     #-1, r2                /* pte = entry & ~1 */
        mov     r2, r0
        and     #0x90, r0
        cmp/eq  #0x10, r0              /* 4 KiB page only */
        bf      .Ltlbf_slow
        /* PTEA = (pte >> 29) | (((pte >> 9) & 1) << 3) */
        mov     r2, r0
        rotl    r0
        rotl    r0
        rotl    r0
        and     #7, r0                 /* pte >> 29 */
        mov     r2, r1
        shlr8   r1
        shlr    r1
        shlr    r1                     /* T = bit 9 of pte */
        bf      2f
        or      #8, r0
2:      mov.l   r0, @(0x34, r3)        /* PTEA */
        mov.l   r2, @(0x04, r3)        /* PTEL */
        ldtlb
        bra     .Ltlbf_done
        nop
.Ltlbf_slow:
        mov.l   .Ltlbf_save4, r15
        mov.l   @r15, r0
        mov.l   @(4, r15), r1
        mov.l   @(8, r15), r2
        mov.l   @(12, r15), r3
        EXC_ENTRY 0x400
.Ltlbf_done:
        /* Update dbt_stat: tlb_hooks(+96), tlb_scan_skips(+104), tlb_refills(+128), exceptions(+16) */
        mov.l   .Ltlbf_stat4, r2       /* &_dbt_stat + 96 */
        mov.l   @r2, r0
        add     #1, r0
        mov.l   r0, @r2                /* tlb_hooks++ */
        mov.l   @(8, r2), r0
        add     #1, r0
        mov.l   r0, @(8, r2)           /* tlb_scan_skips++ */
        mov.l   @(32, r2), r0
        add     #1, r0
        mov.l   r0, @(32, r2)          /* tlb_refills++ */
        add     #-80, r2
        mov.l   @r2, r0
        add     #1, r0
        mov.l   r0, @r2                /* exceptions++ */
        /* Restore bank-1 r0..r3 and r15 from SGR, then rte directly! */
        mov.l   .Ltlbf_save4, r15
        mov.l   @r15, r0
        mov.l   @(4, r15), r1
        mov.l   @(8, r15), r2
        mov.l   @(12, r15), r3
        STCSGR  15
        rte
        nop
.Ltlbf_511:     .word 0x01FF
        .align 2
.Ltlbf_save4:   .long _dbt_tlbf_save
.Ltlbf_en4:     .long _dbt_tlbf5_en
.Ltlbf_shvbr4:  .long _dbt_shadow_vbr
.Ltlbf_cbase4:  .long CACHE_BASE
.Ltlbf_csize4:  .long CACHE_SIZE
.Ltlbf_ccn4:    .long 0xFF000000
.Ltlbf_asid4:   .long _dbt_current_asid
.Ltlbf_hmask4:  .long 0x1FFC
.Ltlbf_pages4:  .long _dbt_user_pages
.Ltlbf_stat4:   .long _dbt_stat + 96
#else
        EXC_ENTRY 0x400
#endif

/* -------- Vector +0x600: Interrupts + common exception handler ---- */
        .org    _dbt_vectors + 0x600
        EXC_ENTRY 0x600

        .align 2
_dbt_exc_common:
        mov.l   .Lctx_exc_rendD, r2
        STCSGR  1
        mov.l   r1, @-r2
        mov.l   r14, @-r2
        mov.l   r13, @-r2
        mov.l   r12, @-r2
        mov.l   r11, @-r2
        mov.l   r10, @-r2
        mov.l   r9, @-r2
        mov.l   r8, @-r2
        stc     r7_bank, r1
        mov.l   r1, @-r2
        stc     r6_bank, r1
        mov.l   r1, @-r2
        stc     r5_bank, r1
        mov.l   r1, @-r2
        stc     r4_bank, r1
        mov.l   r1, @-r2
        stc     r3_bank, r1
        mov.l   r1, @-r2
        stc     r2_bank, r1
        mov.l   r1, @-r2
        stc     r1_bank, r1
        mov.l   r1, @-r2
        stc     r0_bank, r1
        mov.l   r1, @-r2
        sts     macl, r1
        mov.l   r1, @-r2
        sts     mach, r1
        mov.l   r1, @-r2
        stc     gbr, r1
        mov.l   r1, @-r2
        sts     pr, r1
        mov.l   r1, @-r2
        stc     ssr, r1
        mov.l   r1, @-r2
        mov     r2, r3
        add     #CTX_SLOTS, r3
        STSX    SLOT_A0, 1
        mov.l   r1, @(0, r3)
        STSX    SLOT_X0, 1
        mov.l   r1, @(4, r3)
        STSX    SLOT_X1, 1
        mov.l   r1, @(8, r3)
        STSX    SLOT_Y0, 1
        mov.l   r1, @(12, r3)
        STSX    SLOT_Y1, 1
        mov.l   r1, @(16, r3)
        stc     spc, r1
        mov.l   r1, @(CTX_HOSTSPC - CTX_SLOTS, r3)
        stc     ssr, r1
        mov.l   r1, @(CTX_SSR - CTX_SLOTS, r3)
        mov.l   .LccnD, r1             /* 0xFF000000: read EXPEVT/TRA/TEA/INTEVT via one base */
        mov.l   @(0x24, r1), r15
        mov.l   r15, @(CTX_EXPEVT - CTX_SLOTS, r3)
        mov.l   @(0x20, r1), r15
        mov.l   r15, @(CTX_TRA - CTX_SLOTS, r3)
        mov.l   @(0x0C, r1), r15
        mov.l   r15, @(CTX_TEA - CTX_SLOTS, r3)
        mov.l   @(0x28, r1), r15
        mov.l   r15, @(CTX_INTEVT - CTX_SLOTS, r3)
        mov.l   r0, @(CTX_VECOFF - CTX_SLOTS, r3)
        mov     r2, r4
        mov.l   .Lstack_excD, r15
        mov.l   .Lexc_cD, r1
        jsr     @r1
        nop
        mov.l   .Lctx_excD, r2
        mov     r2, r3
        add     #CTX_SLOTS, r3
        cmp/eq  #1, r0
        bt      .Lexc_jump
        mov.l   @(0, r3), r1
        LDSX    1, SLOT_A0
        mov.l   @(4, r3), r1
        LDSX    1, SLOT_X0
        mov.l   @(8, r3), r1
        LDSX    1, SLOT_X1
        mov.l   @(12, r3), r1
        LDSX    1, SLOT_Y0
        mov.l   @(16, r3), r1
        LDSX    1, SLOT_Y1
        bra     .Lexc_restore_common
        nop
.Lexc_jump:
        mov.l   @(CTX_NEWSPC - CTX_SLOTS, r3), r1
        ldc     r1, spc
        mov.l   @(CTX_NEWSSR - CTX_SLOTS, r3), r1
        ldc     r1, ssr
.Lexc_restore_common:
        mov.l   @(CTX_PR, r2), r1
        lds     r1, pr
        mov.l   @(CTX_GBR, r2), r1
        ldc     r1, gbr
        mov.l   @(CTX_MACH, r2), r1
        lds     r1, mach
        mov.l   @(CTX_MACL, r2), r1
        lds     r1, macl
        mov.l   @(CTX_R0 + 0, r2), r1
        ldc     r1, r0_bank
        mov.l   @(CTX_R0 + 4, r2), r1
        ldc     r1, r1_bank
        mov.l   @(CTX_R0 + 8, r2), r1
        ldc     r1, r2_bank
        mov.l   @(CTX_R0 + 12, r2), r1
        ldc     r1, r3_bank
        mov.l   @(CTX_R0 + 16, r2), r1
        ldc     r1, r4_bank
        mov.l   @(CTX_R0 + 20, r2), r1
        ldc     r1, r5_bank
        mov.l   @(CTX_R0 + 24, r2), r1
        ldc     r1, r6_bank
        mov.l   @(CTX_R0 + 28, r2), r1
        ldc     r1, r7_bank
        mov     r2, r3
        add     #(CTX_R0 + 32), r3
        mov.l   @(0, r3), r8
        mov.l   @(4, r3), r9
        mov.l   @(8, r3), r10
        mov.l   @(12, r3), r11
        mov.l   @(16, r3), r12
        mov.l   @(20, r3), r13
        mov.l   @(24, r3), r14
        mov.l   @(28, r3), r15
        mov     r2, r3
        add     #CTX_SLOTS, r3
        cmp/eq  #1, r0
        bt      .Lexc_do_jump
        add     #(CTX_B1 - CTX_SLOTS), r3
        mov.l   @(0, r3), r0
        mov.l   @(4, r3), r1
        mov.l   @(8, r3), r2
        mov.l   @(16, r3), r4
        mov.l   @(20, r3), r5
        mov.l   @(24, r3), r6
        mov.l   @(28, r3), r7
        mov.l   @(12, r3), r3
        rte
        nop
.Lexc_do_jump:
        mov.l   @(CTX_JUMPTO - CTX_SLOTS, r3), r1
        add     #(CTX_B1 - CTX_SLOTS), r3
        mov.l   @(0, r3), r0
        mov.l   @(8, r3), r2
        mov.l   @(16, r3), r4
        mov.l   @(20, r3), r5
        mov.l   @(24, r3), r6
        mov.l   @(28, r3), r7
        mov.l   @(12, r3), r3
        jmp     @r1
        nop

        .align 2
.Lctx_excD:      .long _dbt_ctx_exc
.Lctx_exc_rendD: .long _dbt_ctx_exc + CTX_REND
.Lstack_excD:    .long _dbt_stack_exc_top
.Lexc_cD:        .long _dbt_exc_c
.LccnD:          .long 0xFF000000

/* ================================================================== */
/* ILRAM subsection 1: lookup stubs, full tails, helper stubs         */
/* ================================================================== */
        .subsection 1
        .align 2

/* Lookup stubs. In: X0 = guest target pc, X1 = link literal (link) or IC pair (ic). */
        .macro LOOKUP name, link, ic, cfunc, s1, s2, s3, ts
        .global _dbt_\name
_dbt_\name:
        LDSX    0, SLOT_Y0
\s1:    LDSX    1, SLOT_Y1
\s2:    LDSX    2, SLOT_A0
\s3:    STSX    SLOT_X0, 0             /* r0 = gpc */
        stc     sr, r1
        mov.l   .Llookup_srA, r2
\ts:    mov.l   r1, @r2
        .if (\link) == 0
        mov     r0, r1
        shlr    r1                     /* gpc >> 1 */
        swap.w  r0, r2                 /* low half = gpc >> 16 */
#ifdef DBT_HASH_FOLD14
        rotl    r2
        rotl    r2
#endif
        xor     r2, r1                 /* hidx() */
        cmp/pz  r0                     /* T=1 if user (<0x80000000), T=0 if kernel */
        bf      8f                     /* kernel: global fast path */
        mov.l   .Lasid_maskA, r2
        mov.l   @r2, r2
        tst     r2, r0                 /* T=1 iff ASID-scoped */
        bf      8f                     /* CE 5.0 Slots 1-63: global fast path */
        /* --- ASID-scoped path --- */
#ifdef DBT_HASH_ASID_MIX
        mov.l   .Lhash_asidA, r2
        mov.l   @r2, r2
        extu.b  r2, r2
        shll2   r2
        shll2   r2
        shll    r2                     /* (ASID & 255) << 5 */
        xor     r2, r1
#endif
        mov.l   .LhmaskA, r2
        and     r2, r1
        shll2   r1
        shll    r1                     /* 8-byte packed hent: index * 8 */
        mov.l   .LhashA, r2
        add     r2, r1                 /* r1 = &hash[i] */
        mov.l   @r1, r2
        cmp/eq  r0, r2
        bf      2f
        mov.l   @(4, r1), r1           /* host_asid */
        tst     r1, r1
        bt      2f
#ifdef DBT_HASH_ASID_TAG
        mov     r1, r2
        shlr16  r2
        shlr8   r2                     /* stored ASID */
        mov.l   .Lhash_asidA, r0
        mov.l   @r0, r0
        cmp/eq  r0, r2
        bf      2f
        shll8   r1
        shlr8   r1
        mov.l   .Lcache_hiA, r2
        or      r2, r1
#endif
        .if \ic
#ifdef DBT_USER_IC_HASH_HIT
#ifdef DBT_USER_IC_REFILL
        /* ASID-scoped target (Slot 0): private owner has bit 31 set in ic[2] -> skip g_ic_active! */
        STSX    SLOT_X1, 2
        mov.l   @(8, r2), r0
        cmp/pz  r0
        bf      6f                     /* bit 31 set: private owner, unmasked refill! */
        /* Shared owner: mask IRQs, register in g_ic_active, refill, restore full SR */
        mov.l   .Llookup_srA, r2
        mov.l   @r2, r2
        mov.l   .LimaskA, r0
        or      r0, r2
        ldc     r2, sr
        STSX    SLOT_X1, 2
        mov     #1, r0
        mov.l   r0, @r2
        mov.l   r1, @(4, r2)
        mov.l   @(8, r2), r0           /* block index (bit 31 clear) */
        mov.l   .Lic_seenA, r1
        mov.b   @(r0, r1), r2
        tst     r2, r2
        bf      7f
        mov     #1, r2
        mov.b   r2, @(r0, r1)
        mov.l   .Lic_countA, r1
        mov.l   @r1, r2
        add     #1, r2
        mov.l   r2, @r1
        add     #-1, r2
        shll    r2
        mov.l   .Lic_activeA, r1
        add     r2, r1
        mov.w   r0, @r1
7:      STSX    SLOT_X1, 2
        STSX    SLOT_X0, 0
        mov.l   @(4, r2), r1
        mov.l   r0, @r2                /* publish: pc last */
        mov.l   .Llookup_srA, r0
        mov.l   @r0, r0
        ldc     r0, sr                 /* unmask IRQs + restore T */
        bra     _dbt_tail_hit
        nop
#else
        bra     3f
        nop
#endif
#else
        bra     6f
        nop
#endif
        .else
        bra     3f
        nop
        .endif
8:      /* --- Global path (kernel + CE 5.0 Slots 1-63) --- */
        mov.l   .LhmaskA, r2
        and     r2, r1
        shll2   r1
        shll    r1                     /* 8-byte packed hent: index * 8 */
        mov.l   .LhashA, r2
        add     r2, r1
        mov.l   @r1, r2
        cmp/eq  r0, r2
        bf      2f
        mov.l   @(4, r1), r1           /* full host addr */
        tst     r1, r1
        bt      2f
        .if \ic
6:      /* Unmasked IC refill (interrupt-safe: pc = 1, host, pc) */
        STSX    SLOT_X1, 2
        mov     #1, r0
        mov.l   r0, @r2
        mov.l   r1, @(4, r2)
        STSX    SLOT_X0, 0
        mov.l   r0, @r2
        .endif
3:      /* Unmasked hit: only SR.T changed -> 1-cycle shlr instead of ldc sr! */
        mov.l   .Llookup_srA, r0
        mov.l   @r0, r0
        shlr    r0
        bra     _dbt_tail_hit
        nop
        .endif /* (\link) == 0 */
2:      /* miss: r1 = guest SR (reload from dbt_lookup_sr if clobbered by hash probe) */
        .if (\link) == 0
        mov.l   .Llookup_srA, r2
        mov.l   @r2, r1
        .endif
        mov.l   .LimaskA, r2
        or      r1, r2
        ldc     r2, sr                 /* interrupts masked from here on */
        .if (\link) == 0
        STSX    SLOT_X0, 0
        mov.l   .Lapi_limA, r2
        cmp/hs  r2, r0
        bt      9f                     /* P4 unfetchable (WinCE API 0xF000xxxx) */
        tst     #1, r0
        bf      9f                     /* odd target */
        .endif
        SAVE_CTX_RT A
        STSX    SLOT_X0, 4             /* arg0 = gpc */
        .if \link | \ic
        STSX    SLOT_X1, 5             /* arg1 = link literal / inline cache address */
        .else
        mov     #0, r5
        .endif
        mov.l   .Lstack_rtA, r15
        mov.l   \cfunc, r0
        jsr     @r0
        nop
        bra     _dbt_tail_full_prep    /* r0 = host target */
        nop
        .if (\link) == 0
9:      bra     _dbt_api_fast
        nop
        .endif
        .endm

        LOOKUP  lookup, 0, 0, .LresolveA, _dbt_lk_s1, _dbt_lk_s2, _dbt_lk_s3, _dbt_lk_ts
        LOOKUP  lookup_link, 1, 0, .LresolveA, _dbt_lkl_s1, _dbt_lkl_s2, _dbt_lkl_s3, _dbt_lkl_ts
        LOOKUP  lookup_ic, 0, 1, .Lresolve_icA, _dbt_lki_s1, _dbt_lki_s2, _dbt_lki_s3, _dbt_lki_ts
        .global _dbt_lk_s1, _dbt_lk_s2, _dbt_lk_s3, _dbt_lkl_s1, _dbt_lkl_s2, _dbt_lkl_s3, _dbt_lki_s1, _dbt_lki_s2, _dbt_lki_s3
        .global _dbt_lk_ts, _dbt_lkl_ts, _dbt_lki_ts
        .global _dbt_lk_region_end
_dbt_lk_region_end:
        .align 2
.LhmaskA:       .long (DBT_HASH_ENTRIES - 1)
.LhashA:        .long _dbt_hash
.Lasid_maskA:   .long _dbt_asid_va_mask
.Lcache_hiA:    .long (CACHE_BASE & 0xFF000000)
.Lhash_asidA:   .long _dbt_current_asid
.Llookup_srA:   .long _dbt_lookup_sr
.LimaskA:       .long 0x000000F0
.Lctx_rt_rendA: .long _dbt_ctx_rt + CTX_REND
.Lstack_rtA:    .long _dbt_stack_rt_top
.LresolveA:     .long _dbt_resolve
.Lresolve_icA:  .long _dbt_resolve_ic
#ifdef DBT_USER_IC_REFILL
.Lic_seenA:     .long _g_ic_active_seen
.Lic_countA:    .long _g_nic
.Lic_activeA:   .long _g_ic_active
#endif
.Lapi_limA:     .long 0xE0000000

_dbt_api_fast:
        /* r0 = gpc (in X0), r1 = guest SR; guest r0,r1,r2 in Y0,Y1,A0; IRQs masked */
        mov.l   .Lapi_rbblE, r2        /* SR_RB | SR_BL = 0x30000000 */
        tst     r2, r1
        bf      .Lapi_slow
        mov.l   .Lapi_vbr100hE, r2
        mov.l   @r2, r2
        tst     r2, r2
        bt      .Lapi_slow
        mov.l   .Lapi_shvbrE, r2
        mov.l   @r2, r2
        mov.l   .Lapi_vbr100vE, r0
        mov.l   @r0, r0
        cmp/eq  r0, r2
        bf      .Lapi_slow
        mov.l   .Lapi_trackedE, r2
        mov.l   @r2, r2
        tst     r2, r2
        bf      1f
        mov.l   .Lapi_ccnE, r2
        mov.l   @r2, r2
        extu.b  r2, r2
        mov.l   .Lapi_asidE, r0
        mov.l   @r0, r0
        cmp/eq  r0, r2
        bf      .Lapi_slow
1:      STSX    SLOT_X0, 0             /* r0 = gpc */
        mov.l   .Lapi_ccnE, r2         /* 0xFF000000 */
        mov.l   r0, @(0x0C, r2)        /* TEA = gpc */
        ldc     r0, spc                /* SPC = gpc */
        mov     #0x70, r0
        shll    r0                     /* 0x0E0 */
        mov.l   r0, @(0x24, r2)        /* EXPEVT = 0x0E0 */
        mov.l   .Lapi_mdmaskE, r0      /* 0x40000000 */
        not     r0, r2
        and     r1, r2                 /* guest_sr & ~SR_MD */
        mov.l   .Lapi_shmdE, r0
        mov.l   @r0, r0
        or      r0, r2
        ldc     r2, ssr                /* SSR */
        mov.l   .Lapi_mdmaskE, r2
        mov.l   .Lapi_shmdE, r0
        mov.l   r2, @r0                /* dbt_shadow_md = SR_MD */
        mov.l   .Lapi_statE, r2        /* &_dbt_stat */
        mov.l   @(16, r2), r0
        add     #1, r0
        mov.l   r0, @(16, r2)          /* exceptions++ */
        add     #112, r2
        mov.l   @r2, r0
        add     #1, r0
        mov.l   r0, @r2                /* synth_exceptions++ */
        mov.l   .Lapi_excsrE, r2       /* MD | RB | BL | DSP */
        or      r2, r1
        ldc     r1, sr                 /* NOW IN BANK 1, BL=1: only bank-1 r1 used */
        STSX    SLOT_Y0, 1
        ldc     r1, r0_bank
        STSX    SLOT_Y1, 1
        ldc     r1, r1_bank
        STSX    SLOT_A0, 1
        ldc     r1, r2_bank
        mov.l   .Lapi_vbr100hE, r1
        mov.l   @r1, r1
        jmp     @r1
        nop
.Lapi_slow:
        SAVE_CTX_RT E
        STSX    SLOT_X0, 4             /* arg0 = gpc */
        mov     #0, r5
        mov.l   .Lstack_rtE, r15
        mov.l   .LresolveE, r0
        jsr     @r0
        nop
        bra     _dbt_tail_full_prep
        nop

/* Full context restore tails (after C resolve or helper). */
        .macro TAIL_PREP
        mov     r0, r1
        mov.l   .Lctx_rtE, r2
        mov.l   @(CTX_SR, r2), r3
        mov.l   @(CTX_PR, r2), r0
        lds     r0, pr
        mov.l   @(CTX_GBR, r2), r0
        ldc     r0, gbr
        mov.l   @(CTX_MACH, r2), r0
        lds     r0, mach
        mov.l   @(CTX_MACL, r2), r0
        lds     r0, macl
        mov.l   @(CTX_R0 + 0, r2), r0
        LDSX    0, SLOT_Y0
        mov.l   @(CTX_R0 + 4, r2), r0
        LDSX    0, SLOT_Y1
        mov.l   @(CTX_R0 + 8, r2), r0
        LDSX    0, SLOT_A0
        mov.l   @(CTX_R0 + 12, r2), r0
        LDSX    0, SLOT_X1
        mov.l   @(CTX_R0 + 16, r2), r4
        mov.l   @(CTX_R0 + 20, r2), r5
        add     #(CTX_R0 + 24), r2
        mov.l   @(0, r2), r6
        mov.l   @(4, r2), r7
        mov.l   @(8, r2), r8
        mov.l   @(12, r2), r9
        mov.l   @(16, r2), r10
        mov.l   @(20, r2), r11
        mov.l   @(24, r2), r12
        mov.l   @(28, r2), r13
        mov.l   @(32, r2), r14
        mov.l   @(36, r2), r15
        .endm

        .global _dbt_tail_full_prep
_dbt_tail_full_prep:
        TAIL_PREP
        .global _dbt_tail_full, _dbt_full_m1, _dbt_full_m2, _dbt_full_m3, _dbt_full_m4, _dbt_full_end
_dbt_tail_full:
        ldc     r3, sr
_dbt_full_m1:
        STSX    SLOT_X1, 3
_dbt_full_m2:
        STSX    SLOT_Y0, 0
_dbt_full_m3:
        STSX    SLOT_A0, 2
_dbt_full_m4:
        jmp     @r1
        STSX    SLOT_Y1, 1
_dbt_full_end:

        .global _dbt_tail_rte_prep
_dbt_tail_rte_prep:
        TAIL_PREP
        add     #100, r2
        add     #(CTX_SWSPC - CTX_R0 - 24 - 100), r2
        mov.l   r1, @(CTX_SWTARGET - CTX_SWSPC, r2)
        mov.l   .LtrampE, r1
        ldc     r1, spc
        ldc     r3, ssr
        STSX    SLOT_X1, 3
        STSX    SLOT_Y0, 0
        STSX    SLOT_A0, 2
        STSX    SLOT_Y1, 1
        rte
        nop

        .align 2
.Lctx_rtE:      .long _dbt_ctx_rt
.Lctx_rt_rendE: .long _dbt_ctx_rt + CTX_REND
.Lstack_rtE:    .long _dbt_stack_rt_top
.LresolveE:     .long _dbt_resolve
.LtrampE:       .long _dbt_bank_tramp
.Lapi_rbblE:    .long 0x30000000
.Lapi_vbr100hE: .long _dbt_vbr100_host
.Lapi_vbr100vE: .long _dbt_vbr100_vbr
.Lapi_shvbrE:   .long _dbt_shadow_vbr
.Lapi_trackedE: .long _dbt_pteh_tracked
.Lapi_asidE:    .long _dbt_current_asid
.Lapi_ccnE:     .long 0xFF000000
.Lapi_mdmaskE:  .long 0x40000000
.Lapi_shmdE:    .long _dbt_shadow_md
.Lapi_statE:    .long _dbt_stat
.Lapi_excsrE:   .long 0x70001000

/* ------------------------------------------------------------------ */
/* Helper stubs. In: X0 = argument (opcode / new SR), X1 = return address. */
        .macro HELPER name, cfunc, s1, s2, s3, masked, rb=0, tlb=0
        .global _dbt_\name
_dbt_\name:
        LDSX    0, SLOT_Y0
\s1:    LDSX    1, SLOT_Y1
\s2:    LDSX    2, SLOT_A0
\s3:    stc     sr, r1
        mov.l   .LimaskB, r2
        or      r1, r2
        ldc     r2, sr
\masked:
#ifdef DBT_FAST_LDTLB
        .if \tlb
        STSX    SLOT_X0, 0
        cmp/eq  #0x38, r0
        bf      8f
        mov.l   .Ltlb_ptehB, r0
        mov.l   @r0, r0
        extu.b  r0, r0
        mov.l   .Ltlb_asidB, r2
        mov.l   @r2, r2
        cmp/eq  r2, r0
        bf      8f
        mov.l   .Ltlb_ptelB, r0
        mov.l   @r0, r0
        and     #0x90, r0
        cmp/eq  #0x10, r0              /* 4 KiB only */
        bf      8f
        LDSX    1, SLOT_X0             /* opcode known: reuse X0 for SR */
        mov.l   .Ltlb_ptehB, r0
        mov.l   @r0, r0
        shlr8   r0
        shlr2   r0
        mov     r0, r2
        shlr8   r2
        shlr2   r2
        shlr2   r2
        xor     r2, r0
        mov.l   .Ltlb_hashmaskB, r2
        and     r2, r0
        mov     r0, r2
        mov     #31, r1
        and     r1, r2
        mov     #-5, r1
        shld    r1, r0
        shll2   r0
        mov.l   .Ltlb_pagesB, r1
        mov.l   @(r0,r1), r0
        mov     #15, r1
        shld    r2, r1
        tst     r1, r0
        bf      7f
        ldtlb
        mov.l   .Ltlb_countB, r2
        mov.l   @r2, r0
        add     #1, r0
        mov.l   r0, @r2
        mov.l   .Ltlb_hooksB, r2       /* &_dbt_stat + 96 */
        mov.l   @r2, r0
        add     #1, r0
        mov.l   r0, @r2
        mov.l   @(8, r2), r0
        add     #1, r0
        mov.l   r0, @(8, r2)           /* tlb_scan_skips (+104) */
        STSX    SLOT_X0, 0
        STSX    SLOT_X1, 1
        ldc     r0, sr
        .global _dbt_tlbfast_a, _dbt_tlbfast_b, _dbt_tlbfast_c, _dbt_tlbfast_end
_dbt_tlbfast_a:
        STSX    SLOT_Y0, 0
_dbt_tlbfast_b:
        STSX    SLOT_A0, 2
_dbt_tlbfast_c:
        jmp     @r1
        STSX    SLOT_Y1, 1
_dbt_tlbfast_end:
7:      STSX    SLOT_X0, 1
        mov     #0x38, r0
        LDSX    0, SLOT_X0
8:
        .endif
#endif
#ifdef DBT_FAST_LDC_SR
        .if \rb
        STSX    SLOT_X0, 0
        mov     r1, r2
        xor     r0, r2
        mov.l   .Lfast_rbB, r0
        tst     r0, r2
        bf      9f
        mov.l   .Lfast_trackedB, r0
        mov.l   @r0, r0
        tst     r0, r0
        bf      8f                     /* PTEH tracked by dev_mem: skip uncached P4 read */
        mov.l   .Lfast_asidB, r0
        mov.l   @r0, r0
        mov.l   .Lfast_ptehB, r2
        mov.l   @r2, r2
        extu.b  r2, r2
        cmp/eq  r0, r2
        bf      9f
8:      STSX    SLOT_X0, 0             /* r0 = new SR; no more branches to 9: */
#ifdef DBT_FAST_LDC_SR_MD
        mov.l   .Lfast_mdmaskB, r2
        and     r0, r2
        mov.l   .Lfast_mdB, r1
        mov.l   r2, @r1
#else
        mov.l   .Lfast_mdB, r2
        mov.l   @r2, r2
        xor     r0, r2
        mov.l   .Lfast_mdmaskB, r0
        tst     r0, r2
        bf      9f
        STSX    SLOT_X0, 0
#endif
        mov.l   .Lfast_countB, r2
        mov.l   @r2, r1
        add     #1, r1
        mov.l   r1, @r2
        mov.l   .Lfast_forceB, r2
        or      r2, r0
        STSX    SLOT_X1, 1
        ldc     r0, sr
        .global _dbt_srfast_a, _dbt_srfast_b, _dbt_srfast_c, _dbt_srfast_end
_dbt_srfast_a:
        STSX    SLOT_Y0, 0
_dbt_srfast_b:
        STSX    SLOT_A0, 2
_dbt_srfast_c:
        jmp     @r1
        STSX    SLOT_Y1, 1
_dbt_srfast_end:
9:
        .endif
#endif
        SAVE_CTX_RT B
        mov     r2, r4
        STSX    SLOT_X0, 5
        mov.l   .Lstack_rtB, r15
        mov.l   \cfunc, r0
        jsr     @r0
        nop
        .if \rb
        tst     r0, r0
        STSX    SLOT_X1, 0
        bt      1f
        bra     _dbt_tail_rte_prep
        nop
1:
        .else
        STSX    SLOT_X1, 0
        .endif
        bra     _dbt_tail_full_prep
        nop
        .endm

        HELPER  fpu_stub, .Lfpu_cB, _dbt_hfpu_s1, _dbt_hfpu_s2, _dbt_hfpu_s3, _dbt_hfpu_masked
        HELPER  fallback_stub, .Lfallback_cB, _dbt_hfb_s1, _dbt_hfb_s2, _dbt_hfb_s3, _dbt_hfb_masked, 0, 1
        HELPER  ldc_sr_stub, .Lldc_sr_cB, _dbt_hsr_s1, _dbt_hsr_s2, _dbt_hsr_s3, _dbt_hsr_masked, 1
        .global _dbt_hfpu_s1, _dbt_hfpu_s2, _dbt_hfpu_s3, _dbt_hfpu_masked
        .global _dbt_hfb_s1, _dbt_hfb_s2, _dbt_hfb_s3, _dbt_hfb_masked
        .global _dbt_hsr_s1, _dbt_hsr_s2, _dbt_hsr_s3, _dbt_hsr_masked

        .align 2
.LimaskB:        .long 0x000000F0
.Lctx_rt_rendB:  .long _dbt_ctx_rt + CTX_REND
.Lstack_rtB:     .long _dbt_stack_rt_top
.Lfpu_cB:        .long _dbt_fpu_c
.Lfallback_cB:   .long _dbt_fallback_c
.Lldc_sr_cB:     .long _dbt_ldc_sr_c
#ifdef DBT_FAST_LDC_SR
.Lfast_rbB:      .long 0x20000000
.Lfast_trackedB: .long _dbt_pteh_tracked
.Lfast_asidB:    .long _dbt_current_asid
.Lfast_ptehB:    .long 0xFF000000
.Lfast_mdB:      .long _dbt_shadow_md
.Lfast_mdmaskB:  .long 0x40000000
.Lfast_forceB:   .long 0x40001000
.Lfast_countB:   .long _dbt_fast_ldc_sr_calls
#endif
#ifdef DBT_FAST_LDTLB
.Ltlb_ptehB:     .long 0xFF000000
.Ltlb_ptelB:     .long 0xFF000004
.Ltlb_asidB:     .long _dbt_current_asid
.Ltlb_pagesB:    .long _dbt_user_pages
.Ltlb_hashmaskB: .long 0x1FFC
.Ltlb_countB:    .long _dbt_fast_ldtlb_calls
.Ltlb_hooksB:    .long _dbt_stat + 96
#endif

        .align 2
        .global _dbt_stubs_end
_dbt_stubs_end:

        .section .bss
        .balign 16
        .global _dbt_stack_exc_top
_dbt_stack_exc: .space 16384
_dbt_stack_exc_top:
