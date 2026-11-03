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
        stc.l   spc, @-r15             /* so the handler may clear BL (and print) */
        stc.l   ssr, @-r15
        sts     pr, r1
        mov.l   .Lfault_pr, r2
        mov.l   r1, @r2
        stc     spc, r5
        stc     sr, r1
        mov.l   .Lnotbl, r2
        and     r2, r1
        ldc     r1, sr                 /* BL=0: hypercalls allowed in the handler */
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
        mov     r0, r3
        stc     sr, r1
        mov.l   .Lbl, r2
        or      r2, r1
        ldc     r1, sr                 /* BL=1 again before touching SPC/SSR */
        mov.l   @r15+, r2              /* saved SSR, masked with fault_ssr_and (a C handler clears FD) */
        mov.l   .Lssr_and, r1
        mov.l   @r1, r1
        and     r1, r2
        ldc     r2, ssr
        ldc.l   @r15+, spc
        tst     r3, r3
        bt      2f
        ldc     r3, spc
2:      lds.l   @r15+, macl
        lds.l   @r15+, mach
        lds.l   @r15+, pr
        rte
        nop
        .align 2
.Lexpevt:   .long 0xFF000024
.Ltea:      .long 0xFF00000C
.Lhandler:  .long _guest_fault
.Lssr_and:  .long _fault_ssr_and
.Lfault_pr: .long _fault_pr
.Lnotbl:    .long 0xEFFFFFFF
.Lbl:       .long 0x10000000
        .data
        .align 2
        .global _fault_ssr_and
_fault_ssr_and: .long 0xFFFFFFFF
