/* tmu_vectors.S - guest vector table for the TMU interrupt test.
 * 0x600: hardware interrupt. Entered (as on real SH-4) with MD=1 RB=1 BL=1:
 * r0-r7 are bank 1 and free, SPC/SSR hold the interrupted context. We save
 * pr/mach/macl, call C with INTEVT, restore, rte.
 * 0x100/0x400: unexpected here; drop back to RB=0 and report from C. */
        .text
        .balign 4096
        .global _guest_vectors
_guest_vectors:
        .org    0x100
        /* trapa (guest-side, TRA < 0xF0): count and return, like an interrupt */
        sts.l   pr, @-r15
        mov.l   .Ltrap, r1
        jsr     @r1
        nop
        lds.l   @r15+, pr
        rte
        nop
        .align 2
.Ltrap:      .long _guest_trap
        .org    0x400
        bra     9f
        mov     #4, r0
        .org    0x600
        sts.l   pr, @-r15
        sts.l   mach, @-r15
        sts.l   macl, @-r15
        /* diagnostics: snapshot the interrupted context (bank 0 = guest r0-r7) */
        mov.l   .Lirq_regs, r1
        stc     r0_bank, r0
        mov.l   r0, @(0, r1)
        stc     r1_bank, r0
        mov.l   r0, @(4, r1)
        stc     r2_bank, r0
        mov.l   r0, @(8, r1)
        stc     r3_bank, r0
        mov.l   r0, @(12, r1)
        stc     r4_bank, r0
        mov.l   r0, @(16, r1)
        stc     r5_bank, r0
        mov.l   r0, @(20, r1)
        stc     r6_bank, r0
        mov.l   r0, @(24, r1)
        stc     r7_bank, r0
        mov.l   r0, @(28, r1)
        mov.l   r8, @(32, r1)
        mov.l   r9, @(36, r1)
        mov.l   r10, @(40, r1)
        mov.l   r11, @(44, r1)
        mov.l   r12, @(48, r1)
        mov.l   r13, @(52, r1)
        mov.l   r14, @(56, r1)
        mov.l   r15, @(60, r1)
        add     #64, r1
        stc     spc, r0
        mov.l   r0, @(0, r1)
        stc     ssr, r0
        mov.l   r0, @(4, r1)
        mov.l   .Lintevt, r4
        mov.l   @r4, r4
        swap.b  r4, r4                 /* post-swap the MMIO read (guest/mmio.h) */
        swap.w  r4, r4
        swap.b  r4, r4
        mov.l   .Lirq, r1
        jsr     @r1
        nop
        lds.l   @r15+, macl
        lds.l   @r15+, mach
        lds.l   @r15+, pr
        rte
        nop

9:      /* r0 = vector id. Record it, leave RB=1/BL=1, mask interrupts, go to C. */
        mov.l   .Lfatal_vec, r1
        mov.l   r0, @r1
        stc     sr, r1
        mov.l   .Lsr_clear, r2
        and     r2, r1
        mov.l   .Lsr_set, r2
        or      r2, r1
        ldc     r1, sr                 /* RB=0 BL=0 IMASK=15 */
        mov.l   .Lfatal, r1
        jmp     @r1
        nop

        .align 2
.Lintevt:    .long 0xFF000028
.Lirq_regs:  .long _irq_regs
.Lirq:       .long _guest_irq
.Lfatal_vec: .long _fatal_vec
.Lfatal:     .long _guest_fatal
.Lsr_clear:  .long ~0x30000000          /* clear RB, BL */
.Lsr_set:    .long 0x000000F0           /* IMASK = 15 */
