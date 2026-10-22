/* crt0.S - entry of the translator image (big-endian, runs from RAM) */
        .text
        .global _start
_start:
        mov.l   .Lstack, r15
        mov.l   .Lbss_start, r0
        mov.l   .Lbss_end, r1
        mov     #0, r2
1:      cmp/hs  r1, r0
        bt      2f
        mov.l   r2, @r0
        bra     1b
        add     #4, r0
2:      mov.l   .Lmain, r0
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
