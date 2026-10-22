/* start.S - guest entry (little-endian) */
        .text
        .global _start
_start:
        mov.l   .Lstack, r15
        mov.l   .Lmain, r0
        jsr     @r0
        nop
        mov     #0, r4
        trapa   #0xF0                  /* exit */
1:      bra     1b
        nop
        .align 2
.Lstack: .long 0x8C1F0000
.Lmain:  .long _guest_main
