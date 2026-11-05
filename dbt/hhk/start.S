/* start.S - entry of the HollyHock program (big-endian SH-4A).
 *
 * HollyHock's launcher copies the ELF's loadable sections to their addresses
 * and calls the entry as a subroutine, on its own stack, privileged (MD=1).
 * HollyHock 3 programs are C programs started through newlib's crt, so the
 * launcher may hand over argc/argv/envp in r4-r6 (the SDK reads the symbol
 * table address from the HHK_SYMBOL_TABLE environment variable); we pass
 * them on untouched and let hhk/os.c decide whether they look real.
 * HHK_EMU: casio-emu starts the raw payload at 0x00300000 with r15 =
 * 0x8C080000 and nothing to return to. */
        .section .text.start,"ax"
        .global _start
_start:
#ifndef HHK_EMU
        mov.l   r8, @-r15
        mov.l   r9, @-r15
        mov.l   r10, @-r15
        mov.l   r11, @-r15
        mov.l   r12, @-r15
        mov.l   r13, @-r15
        mov.l   r14, @-r15
        sts.l   pr, @-r15
#else
        mov     #0, r4
        mov     #0, r5
        mov     #0, r6
#endif
        /* clear .bss (HollyHock 2 does, HollyHock 3 is not known to) */
        mov.l   .Lbss_s, r0
        mov.l   .Lbss_e, r1
        mov     #0, r2
1:      cmp/hs  r1, r0
        bt      2f
        mov.l   r2, @r0
        bra     1b
        add     #4, r0
2:      mov.l   .Lmain, r0
        jsr     @r0
        nop
#ifndef HHK_EMU
        lds.l   @r15+, pr
        mov.l   @r15+, r14
        mov.l   @r15+, r13
        mov.l   @r15+, r12
        mov.l   @r15+, r11
        mov.l   @r15+, r10
        mov.l   @r15+, r9
        rts
        mov.l   @r15+, r8
#else
3:      sleep
        bra     3b
        nop
#endif
        .align 2
.Lbss_s: .long _bss_start
.Lbss_e: .long _bss_end
.Lmain:  .long _hhk_main

#ifndef HHK_EMU
/* HollyHock 3 launcher metadata (sdk/include/appdef.h: ELF notes named
 * NAME/DESCRIPTION/AUTHOR/VERSION, type 1) and the HollyHock 2 sections. */
        .macro  HHNOTE type, str
        .section .note.hollyhock,"a",@note
        .balign 4
        .long   9f-8f, 7f-6f, 1
8:      .asciz  "\type"
9:      .balign 4
6:      .asciz  "\str"
7:      .balign 4
        .endm
        .macro  HHSECT name, str
        .section \name,"",@progbits
        .asciz  "\str"
        .endm
#ifndef HHK_APP_NAME
#define HHK_APP_NAME "Windows CE"
#define HHK_APP_DESC "flashes fls0/NK.bin if needed, then boots CE through the translator"
#endif
        HHNOTE  NAME, HHK_APP_NAME
        HHNOTE  DESCRIPTION, HHK_APP_DESC
        HHNOTE  AUTHOR, "wince-classpad"
        HHNOTE  VERSION, HHK_APP_VERSION
        HHSECT  .hollyhock_name, HHK_APP_NAME
        HHSECT  .hollyhock_description, HHK_APP_DESC
        HHSECT  .hollyhock_author, "wince-classpad"
        HHSECT  .hollyhock_version, HHK_APP_VERSION
#endif
