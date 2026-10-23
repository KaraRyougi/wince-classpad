/* loader.S - raw .g3a payload (big-endian). casio-emu starts us at
 * 0x00300000 with r15 = 0x8C080000. Copy the translator image to its link
 * address and jump to it. IMAGE is the path of dbt.bin. */
        .text
        .global _start
_start:
        mov.l   .Lsrc, r0
        mov.l   .Ldst, r1
        mov.l   .Lend, r2
1:      mov.l   @r0+, r3
        mov.l   r3, @r1
        add     #4, r1
        cmp/hs  r2, r0
        bf      1b
        /* write back + invalidate instruction cache over the copied image */
        mov.l   .Ldst, r4
        mov.l   .Lsize, r5
        mov     r4, r0
        add     r5, r0
2:      ocbwb   @r4
        .word   0x04E3                 /* icbi @r4 */
        add     #32, r4
        cmp/hs  r0, r4
        bf      2b
        .word   0x00AB                 /* synco */
        mov.l   .Ldst, r0
        jmp     @r0
        nop
        .align 2
.Lsrc:  .long _image_start
.Lend:  .long _image_end
.Ldst:  .long IMAGE_BASE               /* from the Makefile layout */
.Lsize: .long _image_end - _image_start

        .section .rodata
        .balign 32
        .global _image_start, _image_end
_image_start:
        .incbin IMAGE
_image_end:
