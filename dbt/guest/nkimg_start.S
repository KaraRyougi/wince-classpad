/* nkimg_start.S - entry of the synthetic CE-format test image. The first
 * 0x48 bytes mimic a ROMIMAGE layout: code at 0, "ECEC" + ROMHDR pointer at
 * 0x40 (what the loader looks for). */
        .text
        .global _start
_start:
        bra     1f
        nop
        .org    0x40
        .ascii  "ECEC"
        .long   _nk_romhdr
1:      mov.l   .Lstack, r15
        mov.l   .Lbss_s, r1
        mov.l   .Lbss_e, r2
        mov     #0, r0
2:      cmp/hs  r2, r1
        bt      3f
        mov.l   r0, @r1
        bra     2b
        add     #4, r1
3:      mov.l   .Lmain, r0
        jsr     @r0
        nop
        mov     #0, r4
        trapa   #0xF0
4:      bra     4b
        nop
        .align 2
.Lbss_s: .long __bss_start
.Lbss_e: .long __bss_end
.Lstack: .long 0x8C1F0000
.Lmain:  .long _guest_main
