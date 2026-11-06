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

        .global _dev_nonr0_slot_load
_dev_nonr0_slot_load:
        bra     1f
        mov.l   @(12,r4),r2
1:      rts
        mov     r2,r0

        .global _dev_nonr0_postinc
_dev_nonr0_postinc:
        mov.l   @r4,r1
        mov.l   @r1+,r2
        mov.l   r1,@r4
        rts
        mov     r2,r0

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

        .align 2
        .global _dev_const_store
_dev_const_store:
        mov.l   .Lconst_tea, r1
        mov.l   r4, @r1
        rts
        nop
        .align 2
.Lconst_tea:
        .long   0xFF00000C

        .global _dev_t_after_store
_dev_t_after_store:
        tst     r6, r6
        bt      1f
        sett
        bra     2f
        nop
1:      clrt
2:      mov.l   r5, @r4
        rts
        movt    r0

        .global _dev_keep_r0
_dev_keep_r0:
        mov     #85,r0
        mov.l   @r4,r1
        rts
        nop
        .global _dev_r0_target_store
_dev_r0_target_store:
        sts.l   pr,@-r15
        mov.l   2f,r0
        jsr     @r0
        mov.l   r5,@r4
        lds.l   @r15+,pr
        rts
        nop
1:      rts
        nop
        .align 2
2:      .long 1b

        .global _dev_r0_base
_dev_r0_base:
        mov     r4,r0
        mov.l   @r0,r1
        rts
        mov     r1,r0
        .global _dev_r0_self_store
_dev_r0_self_store:
        mov     r4,r0
        mov.l   r0,@r0
        rts
        nop

        .global _dev_self_predec
_dev_self_predec:
        mov.l   r4, @-r4
        rts
        mov     r4, r0
