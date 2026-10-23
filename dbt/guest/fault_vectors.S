/* fault_vectors.S - guest vectors for the fault tests (apicall, mmu).
 * 0x100 (general), 0x400 (TLB miss), 0x600 (interrupt) all call
 * guest_fault(vec_id, spc, expevt, tea) with the guest PR saved in fault_pr;
 * a non-zero return value replaces SPC. Entered with RB=1: r0-r7 are free. */
        .text
        .balign 4096
        .global _guest_vectors
_guest_vectors:
        .org    0x100
        bra     1f
        mov     #1, r4
        .org    0x400
        bra     1f
        mov     #4, r4
        .org    0x600
        bra     1f
        mov     #6, r4
1:      sts.l   pr, @-r15
        sts.l   mach, @-r15
        sts.l   macl, @-r15
        sts     pr, r1
        mov.l   .Lfault_pr, r2
        mov.l   r1, @r2
        stc     spc, r5
        mov.l   .Lexpevt, r6
        mov.l   @r6, r6
        swap.b  r6, r6                 /* post-swap the MMIO reads */
        swap.w  r6, r6
        swap.b  r6, r6
        mov.l   .Ltea, r7
        mov.l   @r7, r7
        swap.b  r7, r7
        swap.w  r7, r7
        swap.b  r7, r7
        mov.l   .Lhandler, r1
        jsr     @r1
        nop
        tst     r0, r0
        bt      2f
        ldc     r0, spc
2:      lds.l   @r15+, macl
        lds.l   @r15+, mach
        lds.l   @r15+, pr
        rte
        nop
        .align 2
.Lexpevt:   .long 0xFF000024
.Ltea:      .long 0xFF00000C
.Lhandler:  .long _guest_fault
.Lfault_pr: .long _fault_pr
