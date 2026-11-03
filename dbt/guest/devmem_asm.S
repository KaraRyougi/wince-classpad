/* devmem_asm.S - fixed instruction forms for the device-aware test (little-endian guest) */
        .text
        .global _dev_slot_load
_dev_slot_load:                        /* r4 = base: returns @(12,base) loaded in a bra delay slot */
        mov     r4, r1
        bra     1f
        mov.l   @(12, r1), r0
        nop
1:      rts
        nop

        .global _dev_slot_store
_dev_slot_store:                       /* r4 = base, r5 = value: store in a jsr delay slot */
        mov.l   2f, r1
        sts.l   pr, @-r15
        jsr     @r1
        mov.l   r5, @(12, r4)
        lds.l   @r15+, pr
        rts
        nop
3:      rts
        nop
        .align 2
2:      .long   3b

        .global _dev_postinc
_dev_postinc:                          /* r4 = &base: r0 = @base, base += 4 */
        mov.l   @r4, r1
        mov.l   @r1+, r0
        rts
        mov.l   r1, @r4

        .global _dev_predec
_dev_predec:                           /* r4 = &base, r5 = value: @-base = value */
        mov.l   @r4, r1
        mov.l   r5, @-r1
        rts
        mov.l   r1, @r4

        .global _dev_t_after_load
_dev_t_after_load:                     /* r4 = address, r5 = T to set: returns T after the load */
        tst     r5, r5
        bt      1f
        sett
        bra     2f
        nop
1:      clrt
2:      mov.l   @r4, r1
        rts
        movt    r0

        .global _dev_r0idx
_dev_r0idx:                            /* r4 = base, r5 = index: r0 = @(r5, base) */
        mov     r5, r0
        mov.l   @(r0, r4), r0
        rts
        nop
