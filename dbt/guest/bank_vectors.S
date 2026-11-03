/* bank_vectors.S - guest exception vectors for the bank test (little-endian guest).
 * The handlers run in bank 1, as on real hardware, and check that the values
 * the guest parked in r4/r5/r7 of bank 1 (the CE kernel's pCurThread, context
 * pointer and SH3CTL_BASE) survived whatever the translator did in between.
 * Results go to bank_log[] (see bank.c for the slot meanings). */
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
1:      /* bank 1: r0 = vector id. r4/r5/r7 must still be the magic values.
         * SPC/SSR are saved before the nested hypercall below overwrites them
         * (as any real handler must; under the translator a nested exception
         * leaves host values in SPC/SSR, see docs/04). */
        stc.l   spc, @-r15
        stc.l   ssr, @-r15
        mov.l   .Llog, r1
        mov.l   @(0, r1), r2
        add     #1, r2
        mov.l   r2, @(0, r1)           /* log[0]: entries */
        mov.l   .Lmagic4, r2
        cmp/eq  r2, r4
        movt    r3
        mov.l   .Lmagic5, r2
        cmp/eq  r2, r5
        movt    r6
        and     r6, r3
        mov.l   .Lmagic7, r2
        cmp/eq  r2, r7
        movt    r6
        and     r6, r3
        mov.l   @(4, r1), r2
        add     r3, r2
        mov.l   r2, @(4, r1)           /* log[1]: entries that saw r4/r5/r7 intact */
        mov.l   .Lexpevt, r2
        mov.l   @r2, r2
        swap.b  r2, r2
        swap.w  r2, r2
        swap.b  r2, r2                 /* pre-swapped MMIO read (docs/04) */
        mov.l   r2, @(8, r1)           /* log[2]: EXPEVT of the last entry */
        stc     spc, r2
        mov.l   r2, @(12, r1)          /* log[3]: SPC of the last entry */

        /* bank-0 registers as the hardware left them (r0..r7 of the interrupted code) */
        stc     r0_bank, r2
        mov.l   r2, @(16, r1)
        stc     r3_bank, r2
        mov.l   r2, @(20, r1)
        stc     r6_bank, r2
        mov.l   r2, @(24, r1)          /* log[4..6]: bank-0 r0, r3, r6 at entry */

        /* a hypercall from bank 1 with BL cleared (RB stays 1): the translator
         * must leave both banks alone */
        stc     sr, r2
        mov.l   .Lnotbl, r3
        and     r3, r2
        ldc     r2, sr                 /* BL = 0, RB = 1, MD = 1 */
        mov     r4, r6                 /* keep the magic */
        mov     #0x2B, r4              /* '+' */
        trapa   #0xF3                  /* hc_putc */
        mov     r6, r4
        stc     sr, r2
        mov.l   .Lbl, r3
        or      r3, r2
        ldc     r2, sr                 /* BL back on */
        mov.l   .Lmagic5, r2
        cmp/eq  r2, r5
        movt    r3
        mov.l   .Lmagic7, r2
        cmp/eq  r2, r7
        movt    r6
        and     r6, r3
        mov.l   .Lmagic4, r2
        cmp/eq  r2, r4
        movt    r6
        and     r6, r3
        stc     r0_bank, r2
        mov.l   @(16, r1), r6
        cmp/eq  r2, r6
        movt    r6
        and     r6, r3
        stc     r3_bank, r2
        mov.l   @(20, r1), r6
        cmp/eq  r2, r6
        movt    r6
        and     r6, r3
        stc     r6_bank, r2
        mov.l   @(24, r1), r6
        cmp/eq  r2, r6
        movt    r6
        and     r6, r3
        mov.l   @(28, r1), r2
        add     r3, r2
        mov.l   r2, @(28, r1)          /* log[7]: entries whose bank-1 hypercall preserved both banks */

        /* back to the interrupted code; an address error on an API-style odd
         * target returns to PR instead */
        ldc.l   @r15+, ssr
        ldc.l   @r15+, spc
        mov.l   @(8, r1), r2
        mov     #0xE0, r3
        extu.b  r3, r3
        cmp/eq  r2, r3
        bf      2f
        sts     pr, r2
        ldc     r2, spc
2:      rte
        nop

        .align 2
.Llog:     .long _bank_log
.Lmagic4:  .long 0x44444401
.Lmagic5:  .long 0x55555502
.Lmagic7:  .long 0xFF000000
.Lexpevt:  .long 0xFF000024
.Lnotbl:   .long 0xEFFFFFFF
.Lbl:      .long 0x10000000
