/* nkxip_start.S - entry of the XIP test image: like nkimg_start.S ("ECEC" +
 * ROMHDR pointer at 0x40) but the code runs from the NOR, so .data is copied
 * from its load address in flash to RAM first, then .bss is cleared. */
        .text
        .global _start
_start:
        bra     1f
        nop
        .org    0x40
        .ascii  "ECEC"
        .long   _nk_romhdr
1:      mov.l   .Lstack, r15
        mov.l   .Ldata_lma, r1
        mov.l   .Ldata_s, r2
        mov.l   .Ldata_e, r3
2:      cmp/hs  r3, r2
        bt      3f
        mov.l   @r1+, r0
        mov.l   r0, @r2
        bra     2b
        add     #4, r2
3:      mov.l   .Lbss_s, r1
        mov.l   .Lbss_e, r2
        mov     #0, r0
4:      cmp/hs  r2, r1
        bt      5f
        mov.l   r0, @r1
        bra     4b
        add     #4, r1
5:      mov.l   .Lmain, r0
        jsr     @r0
        nop
        mov     #0, r4
        trapa   #0xF0
6:      bra     6b
        nop
        .align 2
.Ldata_lma: .long __data_lma
.Ldata_s:   .long __data_start
.Ldata_e:   .long __data_end
.Lbss_s:    .long __bss_start
.Lbss_e:    .long __bss_end
.Lstack:    .long 0x8C1F0000
.Lmain:     .long _guest_main
