/* standby.S - SH7305 R-standby for the translator (docs/06-deep-suspend.md)
 *
 * dbt_standby_sleep(r4 = save block in RS memory) saves the caller's context
 * and the CPU control registers in the block, writes the caches back, puts
 * the SDRAM into self-refresh the way the stock OS does, sets BAR to the
 * resume stub and executes SLEEP with STBCR.RSTBY set. The core is powered
 * off; on a wake-up (KEYSC autonomous scan, RTC) it restarts at BAR with SR
 * reset, no caches, no TLB, and the resume stub (copied into RS memory by
 * rt.c, position independent, its last longword is the block address)
 * brings the SDRAM controller, caches, MMU registers and VBR back and jumps
 * to dbt_standby_resume_entry, which returns from dbt_standby_sleep with
 * r0 = 1. If SLEEP falls through (no R-standby: the emulator in another
 * profile) everything is undone and 0 is returned.
 * Register values and their order are the stock OS's (docs/06 section 3).
 *
 * The memory-standby step is OS 0x8002095e instruction for instruction:
 * RTCSR = 0xA55A0000, SDCR (0xFEC10044) = 0, then PLLCR (0xA4150024) bit 1,
 * PULCRBSC (0xA40501C3) |= 0x12 and PHCR (0xA405010E) &= 0xCFFF. The first
 * version (2026-11-06) read the PLLCR write as "SDCR |= 2": with SDCR = 2,
 * the refresh timer off and PLLCR untouched, the SDRAM was not put into
 * self-refresh and lost its contents while the core was off; the resume
 * stub (in RS memory) ran, jumped into the decayed SDRAM with SR.BL = 1,
 * and the first exception there reset the machine into the Casio OS. The
 * emulator does not model SDRAM refresh, so its test passed. */

        .macro SYNCO
        .word   0x00AB
        .endm

#define BLK_MAGIC   0x52535442          /* "RSTB" */
#define B_MAGIC     0x00
#define B_R8        0x04                /* r8..r14 at 0x04..0x1C */
#define B_R15       0x20
#define B_PR        0x24
#define B_GBR       0x28
#define B_SR        0x2C
#define B_VBR       0x30
#define B_CCR       0x34
#define B_MMUCR     0x38
#define B_PTEH      0x3C
#define B_HI        0x40                /* second base: displacements are 4 bits */
#define B_PTEL      0x00
#define B_TTB       0x04
#define B_RESUME    0x08
#define B_STATUS    0x0C
#define B_MSTPCR0   0x10
#define B_STBCR     0x14
#define B_SDCR      0x18
#define B_RTCSR     0x1C
#define B_PLLCR     0x20
#define B_PULCR     0x24                /* PULCRBSC, a byte */
#define B_PHCR      0x28                /* PHCR, a word */
#define B_SIZE      0x80

        .text
        .align  2
        .global _dbt_standby_sleep
_dbt_standby_sleep:
        /* 1. the caller's context and the control registers */
        mov.l   r8, @(B_R8 + 0, r4)
        mov.l   r9, @(B_R8 + 4, r4)
        mov.l   r10, @(B_R8 + 8, r4)
        mov.l   r11, @(B_R8 + 12, r4)
        mov.l   r12, @(B_R8 + 16, r4)
        mov.l   r13, @(B_R8 + 20, r4)
        mov.l   r14, @(B_R8 + 24, r4)
        mov.l   r15, @(B_R15, r4)
        sts     pr, r0
        mov.l   r0, @(B_PR, r4)
        stc     gbr, r0
        mov.l   r0, @(B_GBR, r4)
        stc     sr, r0
        mov.l   r0, @(B_SR, r4)
        stc     vbr, r0
        mov.l   r0, @(B_VBR, r4)
        mov.l   .Lccr, r1
        mov.l   @r1, r0
        mov.l   r0, @(B_CCR, r4)
        mov.l   .Lmmucr, r1
        mov.l   @r1, r0
        mov.l   r0, @(B_MMUCR, r4)
        mov.l   .Lpteh, r1
        mov.l   @r1, r0
        mov.l   r0, @(B_PTEH, r4)
        mov     r4, r5
        add     #B_HI, r5
        mov.l   .Lptel, r1
        mov.l   @r1, r0
        mov.l   r0, @(B_PTEL, r5)
        mov.l   .Lttb, r1
        mov.l   @r1, r0
        mov.l   r0, @(B_TTB, r5)
        mov.l   .Lresume, r0
        mov.l   r0, @(B_RESUME, r5)
        mov     #0, r0
        mov.l   r0, @(B_STATUS, r5)
        mov.l   .Lmstpcr0, r1
        mov.l   @r1, r0
        mov.l   r0, @(B_MSTPCR0, r5)
        mov.l   .Lrtc_keysc_on, r2
        and     r2, r0
        mov.l   r0, @r1                 /* RTC and KEYSC keep their clock */
        mov.l   .Lstbcr, r1
        mov.l   @r1, r0
        mov.l   r0, @(B_STBCR, r5)
        mov.l   .Lsdcr, r1
        mov.l   @r1, r0
        mov.l   r0, @(B_SDCR, r5)
        mov.l   .Lrtcsr, r1
        mov.l   @r1, r0
        mov.l   r0, @(B_RTCSR, r5)
        mov.l   .Lpllcr, r1
        mov.l   @r1, r0
        mov.l   r0, @(B_PLLCR, r5)
        mov.l   .Lpulcrbsc, r1
        mov.b   @r1, r0
        extu.b  r0, r0
        mov.l   r0, @(B_PULCR, r5)
        mov.l   .Lphcr, r1
        mov.w   @r1, r0
        extu.w  r0, r0
        mov.l   r0, @(B_PHCR, r5)
        mov.l   .Lmagic, r0
        mov.l   r0, @(B_MAGIC, r4)
        mov     r4, r8                  /* the block, in a register that survives the bank switch */
        /* 2. the rest runs uncached (P2): CCR is written from here on */
        mov.l   .Ltail, r0
        mov.l   .Lp2, r1
        or      r1, r0
        jmp     @r0
        nop
        .align  2
.Lstandby_tail:
        /* operand cache: write back and invalidate every line (4 ways x 256 entries) */
        mov.l   .Loc_array, r1
        mov.l   .Loc_lines, r2
        mov     #0, r0
1:      mov.l   r0, @r1
        add     #32, r1
        dt      r2
        bf      1b
        SYNCO
        mov.l   .Lccr, r1
        mov.l   .Lcc_invalidate, r0
        mov.l   r0, @r1
        SYNCO
        mov     #0, r0
        mov.l   r0, @r1                 /* caches off */
        SYNCO
        /* BAR = the resume stub in RS memory, STBCR = RSTBY (+ bit 0 as the OS writes it) */
        mov.l   .Lbar, r1
        mov.l   .Lstub_addr, r0
        mov.l   r0, @r1
        mov.l   .Lstbcr, r1
        mov     #0x21, r0
        mov.l   r0, @r1
        /* INTEVT = 0 (OS 0x80020e8e) */
        mov.l   .Lintevt, r1
        mov     #0, r0
        mov.l   r0, @r1
        /* memory standby, OS 0x8002095e verbatim: refresh timer and SDCR
         * off, PLLCR bit 1, bus pull-ups, PH6. The OS keeps using its SDRAM
         * stack after this until SLEEP, so the few fetches below are fine. */
        mov.l   .Lsdcr, r1
        mov.l   .La55a0000, r0
        mov.l   r0, @(4, r1)            /* RTCSR 0xFEC10048 */
        mov     #0, r0
        mov.l   r0, @r1                 /* SDCR 0xFEC10044 */
        mov.l   .Lpllcr, r1
        mov.l   @r1, r0
        or      #2, r0
        mov.l   r0, @r1
        mov.l   .Lpulcrbsc, r1
        mov.b   @r1, r0
        or      #0x12, r0
        mov.b   r0, @r1
        mov.l   .Lphcr, r1
        mov.w   @r1, r0
        mov.l   .Lcfff, r2
        and     r2, r0
        mov.w   r0, @r1
        /* SR = MD|RB|BL with IMASK = 0 like the OS (0x80020ede): an RTC
         * alarm can only end the standby if its priority is above IMASK.
         * r0-r7 are bank 1 from here */
        mov.l   .Lsr_standby, r0
        ldc     r0, sr
        sleep
        /* fell through: no R-standby on this machine. Undo in reverse order. */
        mov     r8, r2
        add     #B_HI, r2
        mov.l   .Lphcr, r1
        mov.l   @(B_PHCR, r2), r0
        mov.w   r0, @r1
        mov.l   .Lpulcrbsc, r1
        mov.l   @(B_PULCR, r2), r0
        mov.b   r0, @r1
        mov.l   .Lpllcr, r1
        mov.l   @(B_PLLCR, r2), r0
        mov.l   r0, @r1
        mov.l   .Lsdcr, r1
        mov     #0, r0
        mov.l   r0, @r1
        mov.l   @(B_SDCR, r2), r0
        mov.l   r0, @r1
        mov.l   .Lrtcsr, r1
        mov.l   @(B_RTCSR, r2), r0
        mov.l   r0, @r1
        mov.l   .Lstbcr, r1
        mov.l   @(B_STBCR, r2), r0
        mov.l   r0, @r1
        mov.l   .Lmstpcr0, r1
        mov.l   @(B_MSTPCR0, r2), r0
        mov.l   r0, @r1
        mov.l   .Lbar, r1
        mov.l   .Lreset_vector, r0
        mov.l   r0, @r1
        mov.l   .Lccr, r1
        mov.l   .Lcc_invalidate, r0
        mov.l   r0, @r1
        SYNCO
        mov.l   @(B_CCR, r8), r0
        mov.l   r0, @r1
        SYNCO
        mov.l   @(B_SR, r8), r9
        mov     r8, r10
        ldc     r9, sr                  /* the caller's SR: bank 0 again */
        mov.l   @(B_R8 + 0, r10), r8
        mov.l   @(B_R8 + 4, r10), r9
        mov.l   @(B_R8 + 8, r10), r10
        mov     #0, r0
        rts
        nop

        .align  2
.Lccr:          .long 0xFF00001C
.Lmmucr:        .long 0xFF000010
.Lpteh:         .long 0xFF000000
.Lptel:         .long 0xFF000004
.Lttb:          .long 0xFF000008
.Lmstpcr0:      .long 0xA4150030
.Lrtc_keysc_on: .long 0xFFFFFF9F        /* bit 5 RTC, bit 6 KEYSC */
.Lstbcr:        .long 0xA4150020
.Lbar:          .long 0xA4150040
.Lsdcr:         .long 0xFEC10044
.Lrtcsr:        .long 0xFEC10048
.La55a0000:     .long 0xA55A0000
.Lmagic:        .long BLK_MAGIC
.Lresume:       .long _dbt_standby_resume_entry
.Ltail:         .long .Lstandby_tail
.Lp2:           .long 0x20000000
.Loc_array:     .long 0xF4000000
.Loc_lines:     .long 0x400
.Lcc_invalidate: .long 0x00000808       /* ICI | OCI */
.Lstub_addr:    .long 0xFD801200        /* rt.c copies the stub there */
.Lreset_vector: .long 0xA0000000
.Lsr_standby:   .long 0x70000000        /* MD | RB | BL, IMASK=0: the OS's value */
.Lpllcr:        .long 0xA4150024
.Lpulcrbsc:     .long 0xA40501C3
.Lphcr:         .long 0xA405010E
.Lcfff:         .long 0xFFFFCFFF
.Lintevt:       .long 0xFF000028

/* ------------------------------------------------------------------ */
/* The resume stub: copied to RS memory (0xFD801200), entered at BAR with
 * SR = 0x700000F0 after the core reset. Position independent: every
 * literal is inside the blob. The last longword is the save block address,
 * written by rt.c after the copy. */
        .align  2
        .global _dbt_standby_stub
        .global _dbt_standby_stub_end
_dbt_standby_stub:
        mov.l   .Ls_sr, r0
        ldc     r0, sr                  /* MD | BL | IMASK=15, bank 0 */
        mov.l   .Ls_stbcr, r1
        mov.l   @r1, r0
        mov.l   .Ls_no_rstby, r2
        and     r2, r0
        mov.l   r0, @r1
        mov.l   .Ls_bar, r1
        mov.l   .Ls_reset, r0
        mov.l   r0, @r1
        mov.l   .Ls_rwtcsr, r1
        mov.w   .Ls_a507, r0
        mov.w   r0, @r1
        /* the save block (RS memory, no SDRAM needed): without it, cold restart */
        mov.l   .Ls_block, r4
        mov.l   @(B_MAGIC, r4), r0
        mov.l   .Ls_magic, r1
        cmp/eq  r1, r0
        bt      3f
        mov.l   .Ls_reset, r0
        jmp     @r0
        nop
3:      mov     r4, r5
        add     #B_HI, r5
        /* OS 0x800209c4: PLLCR bits 1-2 back (here: to their saved state),
         * SDCR = 0, the pins back, then the settling wait */
        mov.l   .Ls_pllcr, r1
        mov.l   @r1, r0
        mov     #-7, r2                 /* ~6 */
        and     r2, r0
        mov.l   @(B_PLLCR, r5), r3
        mov     #6, r2
        and     r2, r3
        or      r3, r0
        mov.l   r0, @r1
        mov.l   .Ls_sdcr, r1            /* SDRAM: leave self-refresh */
        mov     #0, r0
        mov.l   r0, @r1
        mov.l   .Ls_phcr, r1
        mov.l   @(B_PHCR, r5), r0
        mov.w   r0, @r1
        mov.l   .Ls_pulcrbsc, r1
        mov.l   @(B_PULCR, r5), r0
        mov.b   r0, @r1
        /* wait until the RTC's 64 Hz counter moved by more than 2 (OS 0x80020a32) */
        mov.l   .Ls_r64cnt, r1
        mov.b   @r1, r2
        extu.b  r2, r2
        mov     #2, r3
2:      mov.b   @r1, r0
        extu.b  r0, r0
        sub     r2, r0
        and     #63, r0
        cmp/gt  r3, r0
        bf      2b
        /* SDRAM controller back to its running state (OS 0x80020a42) */
        mov.l   .Ls_rtcsr, r1
        mov.l   .Ls_a55a0000, r0
        mov.l   r0, @r1
        mov.l   .Ls_sdcr, r1
        mov.l   .Ls_sdcr_run, r0
        mov.l   r0, @r1
        mov.l   .Ls_rtcnt, r1
        mov.l   .Ls_a55a00e3, r0
        mov.l   r0, @r1
        mov.l   r0, @(4, r1)
        mov.l   .Ls_fec15040, r1
        mov     #0x40, r0
        mov.w   r0, @r1
        mov.l   .Ls_rtcsr, r1
        mov.l   .Ls_a55a0088, r0
        mov.l   r0, @r1
        /* caches: invalidate, then the guest's CCR */
        mov.l   .Ls_ccr, r1
        mov.l   .Ls_cc_invalidate, r0
        mov.l   r0, @r1
        SYNCO
        mov.l   @(B_CCR, r4), r0
        mov.l   r0, @r1
        SYNCO
        /* MMU registers; the TLB stays empty and refills from TTB */
        mov.l   .Ls_pteh, r1
        mov.l   @(B_PTEH, r4), r0
        mov.l   r0, @r1
        mov.l   .Ls_ptel, r1
        mov.l   @(B_PTEL, r5), r0
        mov.l   r0, @r1
        mov.l   .Ls_ttb, r1
        mov.l   @(B_TTB, r5), r0
        mov.l   r0, @r1
        mov.l   .Ls_mmucr, r1
        mov.l   @(B_MMUCR, r4), r0
        or      #4, r0                  /* TI */
        mov.l   r0, @r1
        /* the translator's vectors, then its code in SDRAM */
        mov.l   @(B_VBR, r4), r0
        ldc     r0, vbr
        mov.l   @(B_RESUME, r5), r0
        jmp     @r0
        nop
        .align  2
.Ls_sr:         .long 0x500000F0        /* MD | BL | IMASK=15 */
.Ls_stbcr:      .long 0xA4150020
.Ls_no_rstby:   .long 0xFFFFFFDF
.Ls_bar:        .long 0xA4150040
.Ls_reset:      .long 0xA0000000
.Ls_rwtcsr:     .long 0xA4520004
.Ls_sdcr:       .long 0xFEC10044
.Ls_r64cnt:     .long 0xA413FEC0
.Ls_rtcsr:      .long 0xFEC10048
.Ls_rtcnt:      .long 0xFEC1004C        /* and RTCOR at +4 */
.Ls_fec15040:   .long 0xFEC15040
.Ls_a55a0000:   .long 0xA55A0000
.Ls_sdcr_run:   .long 0x00000A09
.Ls_a55a00e3:   .long 0xA55A00E3
.Ls_a55a0088:   .long 0xA55A0088
.Ls_magic:      .long BLK_MAGIC
.Ls_ccr:        .long 0xFF00001C
.Ls_cc_invalidate: .long 0x00000808
.Ls_pteh:       .long 0xFF000000
.Ls_ptel:       .long 0xFF000004
.Ls_ttb:        .long 0xFF000008
.Ls_mmucr:      .long 0xFF000010
.Ls_pllcr:      .long 0xA4150024
.Ls_pulcrbsc:   .long 0xA40501C3
.Ls_phcr:       .long 0xA405010E
.Ls_a507:       .word 0xA507
        .align  2
.Ls_block:      .long 0                 /* patched: the save block address */
_dbt_standby_stub_end:

/* ------------------------------------------------------------------ */
/* Back in SDRAM with caches, MMU registers and VBR restored: return from
 * dbt_standby_sleep with r0 = 1. r4 = the block. */
        .align  2
        .global _dbt_standby_resume_entry
_dbt_standby_resume_entry:
        mov     r4, r5
        add     #B_HI, r5
        mov     #1, r0
        mov.l   r0, @(B_STATUS, r5)
        mov.l   @(B_R15, r4), r15
        mov.l   @(B_PR, r4), r0
        lds     r0, pr
        mov.l   @(B_GBR, r4), r0
        ldc     r0, gbr
        mov.l   @(B_R8 + 0, r4), r8
        mov.l   @(B_R8 + 4, r4), r9
        mov.l   @(B_R8 + 8, r4), r10
        mov.l   @(B_R8 + 12, r4), r11
        mov.l   @(B_R8 + 16, r4), r12
        mov.l   @(B_R8 + 20, r4), r13
        mov.l   @(B_R8 + 24, r4), r14
        mov.l   @(B_SR, r4), r0
        ldc     r0, sr                  /* the caller's SR */
        mov     #1, r0
        rts
        nop
