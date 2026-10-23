/*
 * stubs.S - assembly half of the translator runtime (big-endian SH7305).
 *
 * DSP transfer registers are the spill slots: A0=7 X0=8 X1=9 Y0=10 Y1=11
 * (lds Rm,<slot> = 0x400A | m<<8 | code<<4 ; sts <slot>,Rn = 0x000A | n<<8 | code<<4).
 * They are emitted as .word so no DSP assembler support is needed.
 *
 * Window labels (dbt_*_s1.., dbt_hit_*, dbt_full_m*) are read by rt.c to
 * undo partial register saves when an exception lands inside a stub.
 */
#include "ctx_offsets.h"

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

/* One literal pool per region (mov.l @(disp,PC) reaches only 1 KiB). */
        .macro POOL sfx
        .align 2
.Lvectors\sfx:      .long _dbt_vectors
.Lsr_init\sfx:      .long 0x400010F0        /* MD | DSP | IMASK=15 */
.Lhmask\sfx:        .long 8191              /* HSIZE - 1, must match rt.c */
.Lhash\sfx:         .long _dbt_hash
.Limask\sfx:        .long 0x000000F0
.Lctx_rt\sfx:       .long _dbt_ctx_rt
.Lctx_rt_rend\sfx:  .long _dbt_ctx_rt + CTX_REND
.Lctx_exc\sfx:      .long _dbt_ctx_exc
.Lctx_exc_rend\sfx: .long _dbt_ctx_exc + CTX_REND
.Lstack_rt\sfx:     .long _dbt_stack_rt_top
.Lstack_exc\sfx:    .long _dbt_stack_exc_top
.Lresolve\sfx:      .long _dbt_resolve
.Lexc_c\sfx:        .long _dbt_exc_c
.Lrte_c\sfx:        .long _dbt_rte_c
.Lfpu_c\sfx:        .long _dbt_fpu_c
.Lfallback_c\sfx:   .long _dbt_fallback_c
.Lldc_sr_c\sfx:     .long _dbt_ldc_sr_c
.Lexpevt\sfx:       .long 0xFF000024
.Ltra\sfx:          .long 0xFF000020
.Ltea\sfx:          .long 0xFF00000C
.Lintevt\sfx:       .long 0xFF000028
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

        .text
        .align 2
        .global _dbt_stubs_start
_dbt_stubs_start:

/* ------------------------------------------------------------------ */
/* dbt_enter(r4 = host entry): install VBR, set MD|DSP, interrupts masked */
        .global _dbt_enter
_dbt_enter:
        mov.l   .LvectorsA, r0
        ldc     r0, vbr
        mov.l   .Lsr_initA, r0
        ldc     r0, sr
        jmp     @r4
        nop

/* ------------------------------------------------------------------ */
/* dbt_sync_code(r4 = addr, r5 = len): write back data, invalidate icache */
        .global _dbt_sync_code
_dbt_sync_code:
        mov     r4, r0
        add     r5, r0                 /* end */
        mov     #-32, r1
        and     r1, r4                 /* line align */
1:      ocbwb   @r4
        ICBI    4
        add     #32, r4
        cmp/hs  r0, r4
        bf      1b
        SYNCO
        rts
        nop

/* ------------------------------------------------------------------ */
/* Lookup stubs. In: X0 = guest target pc, X1 = link literal address (link
 * variant). r0,r1,r2 are saved to Y0,Y1,A0 (window: resume = X0). */
        .macro LOOKUP name, link, s1, s2, s3
        .global _dbt_\name
_dbt_\name:
        LDSX    0, SLOT_Y0
\s1:    LDSX    1, SLOT_Y1
\s2:    LDSX    2, SLOT_A0
\s3:    STSX    SLOT_X0, 0             /* r0 = gpc */
        .if \link
        bra     2f                     /* a link is resolved once: let C patch and chain it */
        nop
        .endif
        mov     r0, r1
        shlr    r1                     /* gpc >> 1 */
        swap.w  r0, r2                 /* low half = gpc >> 16 */
        xor     r2, r1                 /* same index as hidx() in rt.c */
        mov.l   .LhmaskA, r2
        and     r2, r1
        shll2   r1
        shll    r1                     /* index * 8 */
        mov.l   .LhashA, r2
        add     r2, r1                 /* r1 = &hash[i] */
        mov.l   @r1, r2
        cmp/eq  r0, r2
        bf      2f
        mov.l   @(4, r1), r1           /* host address */
        .if \link
        STSX    SLOT_X1, 2
        mov.l   r1, @r2                /* patch the link literal */
        .endif
        bra     _dbt_tail_hit
        nop
2:      /* miss: r0 = gpc (also in X0) */
        stc     sr, r1                 /* guest SR */
        mov.l   .LimaskA, r2
        or      r1, r2
        ldc     r2, sr                 /* interrupts masked from here on */
        SAVE_CTX_RT A
        STSX    SLOT_X0, 4             /* arg0 = gpc */
        .if \link
        STSX    SLOT_X1, 5             /* arg1 = link literal address */
        .else
        mov     #0, r5
        .endif
        mov.l   .Lstack_rtA, r15
        mov.l   .LresolveA, r0
        jsr     @r0
        nop
        bra     _dbt_tail_full_prep    /* r0 = host target */
        nop
        .endm

        LOOKUP  lookup, 0, _dbt_lk_s1, _dbt_lk_s2, _dbt_lk_s3
        LOOKUP  lookup_link, 1, _dbt_lkl_s1, _dbt_lkl_s2, _dbt_lkl_s3
        .global _dbt_lk_s1, _dbt_lk_s2, _dbt_lk_s3, _dbt_lkl_s1, _dbt_lkl_s2, _dbt_lkl_s3
        .global _dbt_lk_region_end
_dbt_lk_region_end:

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

/* Restore the guest context from dbt_ctx_rt and jump to r0 (host target).
 * Leaves r1 = target, r3 = SR to load; guest r0,r1,r2,r3 in Y0,Y1,A0,X1. */
        .global _dbt_tail_full_prep
_dbt_tail_full_prep:
        mov     r0, r1
        mov.l   .Lctx_rtA, r2
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
        .global _dbt_tail_full, _dbt_full_m1, _dbt_full_m2, _dbt_full_m3, _dbt_full_m4, _dbt_full_end
_dbt_tail_full:
        ldc     r3, sr                 /* guest SR back (interrupts may now arrive) */
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
        POOL    A

/* ------------------------------------------------------------------ */
/* Helper stubs. In: X0 = argument (opcode / new SR), X1 = return address. */
        .macro HELPER name, cfunc, s1, s2, s3, masked
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
        SAVE_CTX_RT B
        mov     r2, r4                 /* arg0 = ctx */
        STSX    SLOT_X0, 5             /* arg1 = opcode / value */
        mov.l   .Lstack_rtB, r15
        mov.l   \cfunc, r0
        jsr     @r0
        nop
        STSX    SLOT_X1, 0             /* r0 = return address */
        bra     _dbt_tail_full_prep
        nop
        .endm

        HELPER  fpu_stub, .Lfpu_cB, _dbt_hfpu_s1, _dbt_hfpu_s2, _dbt_hfpu_s3, _dbt_hfpu_masked
        HELPER  fallback_stub, .Lfallback_cB, _dbt_hfb_s1, _dbt_hfb_s2, _dbt_hfb_s3, _dbt_hfb_masked
        HELPER  ldc_sr_stub, .Lldc_sr_cB, _dbt_hsr_s1, _dbt_hsr_s2, _dbt_hsr_s3, _dbt_hsr_masked
        .global _dbt_hfpu_s1, _dbt_hfpu_s2, _dbt_hfpu_s3, _dbt_hfpu_masked
        .global _dbt_hfb_s1, _dbt_hfb_s2, _dbt_hfb_s3, _dbt_hfb_masked
        .global _dbt_hsr_s1, _dbt_hsr_s2, _dbt_hsr_s3, _dbt_hsr_masked
        POOL    B

/* ------------------------------------------------------------------ */
/* Guest rte. Entered after the guest's delay slot with the guest SR (BL is
 * normally set here, so no window handling). SPC/SSR hold guest values. */
        .global _dbt_rte_stub
_dbt_rte_stub:
        LDSX    0, SLOT_Y0
        LDSX    1, SLOT_Y1
        LDSX    2, SLOT_A0
        stc     sr, r1
        mov.l   .LimaskC, r2
        or      r1, r2
        ldc     r2, sr
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
        /* r0 = host target; new SSR in ctx->ssr */
        mov.l   .Lctx_rtC, r2
        mov     r2, r1
        add     #CTX_SSR, r1
        mov.l   @r1, r1
        ldc     r1, ssr
        ldc     r0, spc
        /* restore guest registers (SR is restored by rte itself) */
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
        STSX    SLOT_A0, 2             /* r2 last: it was the base pointer */
        STSX    SLOT_Y0, 0
        rte
        nop                            /* never touch r0-r7 in an rte slot: the bank has switched */
        POOL    C

/* ------------------------------------------------------------------ */
/* Exception vectors. VBR = _dbt_vectors. Entered with MD=1 RB=1 BL=1, so
 * r0-r7 are bank 1 and free for us. */
        .balign 4096
        .global _dbt_vectors
_dbt_vectors:
        .org    _dbt_vectors + 0x100
        mov     #1, r0
        shll8   r0
        bra     _dbt_exc_common
        nop
        .org    _dbt_vectors + 0x400
        mov     #4, r0
        shll8   r0
        bra     _dbt_exc_common
        nop
        .org    _dbt_vectors + 0x600
        mov     #6, r0
        shll8   r0
        bra     _dbt_exc_common
        nop

        .align 2
_dbt_exc_common:
        /* r0 = vector offset. Save guest state into dbt_ctx_exc. */
        mov.l   .Lctx_exc_rendD, r2
        mov.l   r15, @-r2
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
        mov.l   r1, @-r2               /* ctx->sr = SSR (guest SR before the exception) */
        /* r2 = &dbt_ctx_exc */
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
        mov.l   .LexpevtD, r1
        mov.l   @r1, r1
        mov.l   r1, @(CTX_EXPEVT - CTX_SLOTS, r3)
        mov.l   .LtraD, r1
        mov.l   @r1, r1
        mov.l   r1, @(CTX_TRA - CTX_SLOTS, r3)
        mov.l   .LteaD, r1
        mov.l   @r1, r1
        mov.l   r1, @(CTX_TEA - CTX_SLOTS, r3)
        mov.l   .LintevtD, r1
        mov.l   @r1, r1
        mov.l   r1, @(CTX_INTEVT - CTX_SLOTS, r3)
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
        /* RESUME: everything back as it was, including the DSP slots */
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
        cmp/eq  #1, r0
        bt      .Lexc_do_jump
        rte
        nop
.Lexc_do_jump:
        mov     r2, r1
        add     #CTX_SLOTS, r1
        mov.l   @(CTX_JUMPTO - CTX_SLOTS, r1), r1
        jmp     @r1
        nop

        .align 2
        POOL    D
        .global _dbt_stubs_end
_dbt_stubs_end:

        .section .bss
        .balign 16
        .global _dbt_stack_rt_top, _dbt_stack_exc_top
_dbt_stack_rt:  .space 16384
_dbt_stack_rt_top:
_dbt_stack_exc: .space 16384
_dbt_stack_exc_top:
