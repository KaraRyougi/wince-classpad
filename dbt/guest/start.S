/* start.S - guest entry (little-endian) */
        .text
        .global _start
_start:
        mov.l   .Lstack, r15
        mov.l   .Lbss_s, r1            /* zero .bss (the blob only carries file contents) */
        mov.l   .Lbss_e, r2
        mov     #0, r0
1:      cmp/hs  r2, r1
        bt      2f
        mov.l   r0, @r1
        bra     1b
        add     #4, r1
2:      mov.l   .Lmain, r0
        jsr     @r0
        nop
        mov     #0, r4
        trapa   #0xF0                  /* exit */
3:      bra     3b
        nop
        .align 2
.Lbss_s: .long __bss_start
.Lbss_e: .long __bss_end
.Lstack: .long 0x8C1F0000
.Lmain:  .long _guest_main
