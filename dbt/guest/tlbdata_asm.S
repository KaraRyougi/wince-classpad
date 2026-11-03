/* tlbdata_asm.S - one function per memory addressing form (little-endian
 * guest). r4 = address, r5 = value / pointer; the result comes back in r0.
 * Post/pre-increment forms return the updated register. */
        .text
        .macro F name
        .global _\name
_\name:
        .endm

        F ld_b
        mov.b   @r4, r0
        rts
        nop
        F ld_w
        mov.w   @r4, r0
        rts
        nop
        F ld_l
        mov.l   @r4, r0
        rts
        nop
        F ld_bpost                    /* r5 = &base: returns the byte, *r5 = r4 after */
        mov.b   @r4+, r0
        rts
        mov.l   r4, @r5
        F ld_wpost
        mov.w   @r4+, r0
        rts
        mov.l   r4, @r5
        F ld_lpost
        mov.l   @r4+, r0
        rts
        mov.l   r4, @r5
        F ld_bdisp
        mov.b   @(3, r4), r0
        rts
        nop
        F ld_wdisp
        mov.w   @(4, r4), r0
        rts
        nop
        F ld_ldisp
        mov.l   @(8, r4), r1
        rts
        mov     r1, r0
        F ld_br0
        mov     #5, r0
        mov.b   @(r0, r4), r0
        rts
        nop
        F ld_wr0
        mov     #4, r0
        mov.w   @(r0, r4), r0
        rts
        nop
        F ld_lr0
        mov     #8, r0
        mov.l   @(r0, r4), r0
        rts
        nop
        F ld_bgbr
        stc     gbr, r1
        ldc     r4, gbr
        mov.b   @(1, gbr), r0
        rts
        ldc     r1, gbr
        F ld_wgbr
        stc     gbr, r1
        ldc     r4, gbr
        mov.w   @(4, gbr), r0
        rts
        ldc     r1, gbr
        F ld_lgbr
        stc     gbr, r1
        ldc     r4, gbr
        mov.l   @(8, gbr), r0
        rts
        ldc     r1, gbr
        F ld_lself                    /* data register is the base register */
        mov     r4, r6
        mov.l   @r6, r6
        rts
        mov     r6, r0
        F ld_lpostself                /* @rm+ with m == n: the load wins */
        mov     r4, r6
        mov.l   @r6+, r6
        rts
        mov     r6, r0
        F ld_wsign                    /* sign extension of a negative word */
        mov.w   @(12, r4), r0
        rts
        nop

        F st_b
        mov.b   r5, @r4
        rts
        mov     r5, r0
        F st_w
        mov.w   r5, @r4
        rts
        mov     r5, r0
        F st_l
        mov.l   r5, @r4
        rts
        mov     r5, r0
        F st_bpre                     /* returns the decremented address */
        mov.b   r5, @-r4
        rts
        mov     r4, r0
        F st_wpre
        mov.w   r5, @-r4
        rts
        mov     r4, r0
        F st_lpre
        mov.l   r5, @-r4
        rts
        mov     r4, r0
        F st_lpreself                 /* mov.l rn,@-rn stores the decremented value */
        mov     r4, r6
        mov.l   r6, @-r6
        rts
        mov     r6, r0
        F st_bdisp
        mov     r5, r0
        mov.b   r0, @(3, r4)
        rts
        nop
        F st_wdisp
        mov     r5, r0
        mov.w   r0, @(4, r4)
        rts
        nop
        F st_ldisp
        mov.l   r5, @(8, r4)
        rts
        mov     r5, r0
        F st_br0
        mov     #5, r0
        mov.b   r5, @(r0, r4)
        rts
        nop
        F st_wr0
        mov     #4, r0
        mov.w   r5, @(r0, r4)
        rts
        nop
        F st_lr0
        mov     #8, r0
        mov.l   r5, @(r0, r4)
        rts
        nop
        F st_bgbr
        stc     gbr, r1
        ldc     r4, gbr
        mov     r5, r0
        mov.b   r0, @(1, gbr)
        rts
        ldc     r1, gbr
        F st_wgbr
        stc     gbr, r1
        ldc     r4, gbr
        mov     r5, r0
        mov.w   r0, @(4, gbr)
        rts
        ldc     r1, gbr
        F st_lgbr
        stc     gbr, r1
        ldc     r4, gbr
        mov     r5, r0
        mov.l   r0, @(8, gbr)
        rts
        ldc     r1, gbr
        F st_movca
        mov     r5, r0
        movca.l r0, @r4
        rts
        nop
        F st_lr0base                  /* mov.l r5,@(disp,r0): the base is r0 */
        mov     r4, r0
        mov.l   r5, @(8, r0)
        rts
        nop
        F rmw_l                       /* load-modify-store across two pages */
        mov.l   @r4, r0
        add     r5, r0
        mov.l   r0, @r4
        rts
        nop
