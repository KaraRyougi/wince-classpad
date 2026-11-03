/* fpudis_asm.S - FPU instructions at known addresses for the SR.FD test.
 * Each function takes r4 and returns r0 through FPUL; the *_site symbols
 * name the instruction (or the branch owning the delay slot) that the
 * FPU-disable exception must report in SPC. Opcodes as .word: the guest
 * is built with -m4-nofpu. */
        .text
        .align 2
        .global _fpudis_lds, _fpudis_lds_site
_fpudis_lds:
_fpudis_lds_site:
        .word   0x445A                 /* lds r4,fpul */
        .word   0x005A                 /* sts fpul,r0 */
        rts
        nop

        .align 2
        .global _fpudis_bra, _fpudis_bra_site
_fpudis_bra:
_fpudis_bra_site:
        bra     1f
        .word   0x445A                 /* lds r4,fpul (delay slot) */
        mov     #-1, r0                /* not reached */
        rts
        nop
1:      .word   0x005A                 /* sts fpul,r0 */
        rts
        nop

        .align 2
        .global _fpudis_jsr, _fpudis_jsr_site
_fpudis_jsr:
        sts.l   pr, @-r15
        mov.l   .Lfn, r1
_fpudis_jsr_site:
        jsr     @r1
        .word   0x445A                 /* lds r4,fpul (delay slot of an indirect branch) */
        lds.l   @r15+, pr
        rts
        nop
        .align 2
.Lfn:   .long   .Lfnbody
.Lfnbody:
        .word   0x005A                 /* sts fpul,r0 */
        rts
        nop

        .align 2
        .global _fpudis_bts, _fpudis_bts_site
_fpudis_bts:
        sett
_fpudis_bts_site:
        bt/s    1f
        .word   0x445A                 /* lds r4,fpul (delay slot of a conditional branch) */
        mov     #-1, r0                /* not reached */
        rts
        nop
1:      .word   0x005A
        rts
        nop

        .align 2
        .global _fpudis_stsfpscr, _fpudis_stsfpscr_site
_fpudis_stsfpscr:
_fpudis_stsfpscr_site:
        .word   0x006A                 /* sts fpscr,r0 */
        rts
        nop

        .align 2
        .global _fpudis_fsts, _fpudis_fsts_site
_fpudis_fsts:                          /* r4 -> fpul -> fr0 -> fpul -> r0: four FPU instructions, the first one faults */
_fpudis_fsts_site:
        .word   0x445A                 /* lds r4,fpul */
        .word   0xF00D                 /* fsts fpul,fr0 */
        .word   0xF01D                 /* flds fr0,fpul */
        .word   0x005A                 /* sts fpul,r0 */
        rts
        nop
