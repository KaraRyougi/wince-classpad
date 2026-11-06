/* crt0.S - entry of the translator image (big-endian, runs from RAM) */
        .text
        .global _start
_start:
        /* Establish cache policy before any C code or BSS stores. The
         * loader has already written this image back. SH7305 has four
         * 8-KiB operand-cache ways; purge their address arrays from P2,
         * without a stack spill, before invalidating or changing CCR.
         * Clearing a tag writes back its dirty line (SH4 cache-array rule).
         * Both HollyHock and the emulator loader enter this same path. */
        stc     sr, r0
        or      #0xf0, r0
        ldc     r0, sr
        mova    .Lcache_p2, r0
        mov.l   .Lp2_bit, r1
        or      r1, r0
        jmp     @r0
        nop
        .align 2
.Lcache_p2:
        mov.l   .Lccr, r1
        mov.l   @r1, r4
        mov     r4, r0
        tst     #1, r0
        bt      .Lcache_clean
        mov.l   .Loc_begin, r2
        mov.l   .Loc_end, r3
        mov     #0, r0
.Lcache_purge:
        mov.l   r0, @r2
        add     #32, r2
        cmp/hs  r3, r2
        bf      .Lcache_purge
.Lcache_clean:
        .word   0x00ab                 /* SYNCO: finish dirty writebacks */
        mov     #-3, r0               /* clear WT; preserve unrelated bits */
        and     r0, r4
        mov.l   .Lcache_enable, r0
        or      r0, r4
        mov.l   r4, @r1
        mov.l   @r1, r0               /* ordered readback, no UI/log */
        .word   0x00ab
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        mov.l   .Lcached_entry, r0
        .word   0x00e3                 /* ICBI @r0: SH-4A instruction barrier after a CCR write */
        jmp     @r0
        nop
        .align 2
.Lcached:
        mov.l   .Lstack, r15
        mov.l   .Lbss_start, r0
        mov.l   .Lbss_end, r1
        mov     #0, r2
1:      cmp/hs  r1, r0
        bt      2f
        mov.l   r2, @r0
        bra     1b
        add     #4, r0
2:      mov.l   .Llow_start, r0
        mov.l   .Llow_end, r1
.Llow_clear:
        cmp/hs  r1, r0
        bt      .Llow_done
        mov.l   r2, @r0
        bra     .Llow_clear
        add     #4, r0
.Llow_done:
        mov.l   .Lonchip, r0
        jsr     @r0
        nop
        mov.l   .Lmain, r0
        jsr     @r0
        nop
3:      sleep
        bra     3b
        nop
        .align 2
.Lstack:     .long _image_stack_top
.Lbss_start: .long _bss_start
.Lbss_end:   .long _bss_end
.Lmain:      .long _dbt_main
.Lonchip:    .long _dbt_onchip_load
.Llow_start: .long _low_bss_start
.Llow_end:   .long _low_bss_end

.Lp2_bit:       .long 0x20000000
.Lccr:          .long 0xff00001c
.Loc_begin:     .long 0xf4000000
.Loc_end:       .long 0xf4008000
.Lcache_enable: .long 0x0000090d /* ICI|ICE|OCI|CB|OCE; stored policy 0x105 */
.Lcached_entry: .long .Lcached
