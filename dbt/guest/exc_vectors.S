/* exc_vectors.S - guest exception vector table (little-endian guest).
 * The guest sets VBR to _guest_vectors; the translator maps the real
 * exceptions onto it. Each handler saves scratch registers and calls C. */
        .text
        .balign 4096
        .global _guest_vectors
_guest_vectors:
        .org    0x100
        bra     1f
        mov     #1, r0
        .org    0x400
        bra     1f
        mov     #4, r0
        .org    0x600
        bra     1f
        mov     #6, r0
1:      /* r0 = vector id (bank 1, RB=1). Save context on the guest stack. */
        mov.l   r1, @-r15
        mov.l   r2, @-r15
        mov.l   r3, @-r15
        mov.l   r4, @-r15
        mov.l   r5, @-r15
        mov.l   r6, @-r15
        mov.l   r7, @-r15
        sts.l   pr, @-r15
        stc.l   spc, @-r15
        stc.l   ssr, @-r15
        mov     r0, r4                 /* arg0 = vector id */
        stc     spc, r5                /* arg1 = spc */
        mov.l   .Lexpevt, r6
        mov.l   @r6, r6
        swap.b  r6, r6                 /* pre-swapped MMIO read: see docs/04 */
        swap.w  r6, r6
        swap.b  r6, r6                 /* arg2 = EXPEVT */
        mov.l   .Ltra, r7
        mov.l   @r7, r7
        swap.b  r7, r7
        swap.w  r7, r7
        swap.b  r7, r7                 /* arg3 = TRA */
        mov.l   .Lhandler, r1
        jsr     @r1
        nop
        /* r0 = new SPC (or 0 to keep) */
        ldc.l   @r15+, ssr
        ldc.l   @r15+, spc
        tst     r0, r0
        bt      2f
        ldc     r0, spc
2:      lds.l   @r15+, pr
        mov.l   @r15+, r7
        mov.l   @r15+, r6
        mov.l   @r15+, r5
        mov.l   @r15+, r4
        mov.l   @r15+, r3
        mov.l   @r15+, r2
        mov.l   @r15+, r1
        rte
        nop
        .align 2
.Lexpevt:  .long 0xFF000024
.Ltra:     .long 0xFF000020
.Lhandler: .long _guest_exc_handler
