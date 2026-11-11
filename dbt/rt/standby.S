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

/* make STANDBY_DIAG=1: the resume path paints a stripe per stage on the
 * panel (docs/06 section 8). STAGE n, colour-literal: rows 66*(n-1).. in
 * that colour, then about 0.5 s. Keeps r1-r5; clobbers r0, r6-r15, macl. */
        .macro STAGE n, col
#ifdef DBT_STANDBY_DIAG
        mov     #\n, r7
        mov.l   \col, r6
        bsr     .Ls_stripe
        nop
#endif
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
#define B_SPC       0x2C                /* SPC and SSR: core registers, lost in R-standby */
#define B_SSR       0x30
#define B_EV0       0x34                /* wake evidence, read first thing in the resume stub: */
#define B_EV1       0x38                /* EV0 = RSECCNT<<24 | R64CNT<<16 | NMIFCR, EV1 = INTREQ00<<24 | RCR1<<16 | KIUINTREG */
#define B_EV2       0x3C                /* EV2 = SDCR<<16 | PLLCR (low words) */
#define B_SIZE      0x80
/* third base, block + 0x80: the standby loop of the resume stub */
#define B2_COUNT    0x00                /* wake-ups answered with another SLEEP */
#define B2_LOOP     0x04                /* rt.c: non-zero = the loop is on */
#define B2_EV0      0x08                /* EV0 and EV1 of the first such wake-up */
#define B2_EV1      0x0C
#define B2_EV2      0x10
#define B2_VAR      0x14                /* rt.c: which of the OS's memory-standby steps follow the self-refresh:
                                         * bit 0 PLLCR bit 1 (CKO off), bit 1 the bus pulls, bit 2 PH6 released */

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
        mov.l   .Lmstp_or, r2
        mov.l   @r2, r2
        or      r2, r0                  /* rt.c's wake-source search: module clocks to stop as well */
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
        /* SPC and SSR: the caller (the exception path's RESUME exit in
         * stubs.S) returns to the guest with rte on the live registers */
        stc     spc, r0
        mov.l   r0, @(B_SPC, r5)
        stc     ssr, r0
        mov.l   r0, @(B_SSR, r5)
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
        /* the SDRAM's self-refresh and the SLEEP run from RS memory: the
         * tail in the resume stub's blob (.Ls_sleep_tail, see there) */
        mov.l   .Lrs_tail, r0
        jmp     @r0
        nop
        .align  2
.Lfell_through:
        /* SLEEP fell through: no R-standby on this machine. Undo in reverse order. */
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
.Lrs_tail:      .long 0xFD801200 + (.Ls_sleep_tail - _dbt_standby_stub)
.Lmstp_or:      .long _dbt_standby_mstp_or

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
        /* the wake evidence, before anything else touches the machine */
        mov.l   .Ls_block, r4
        mov     r4, r5
        add     #B_HI, r5
        mov.l   .Ls_r64cnt, r1
        mov.b   @r1, r0
        extu.b  r0, r2
        mov.b   @(2, r1), r0            /* RSECCNT */
        extu.b  r0, r0
        shll8   r0
        or      r0, r2
        shll16  r2
        mov.l   .Ls_nmifcr, r1
        mov.w   @r1, r0
        extu.w  r0, r0
        or      r0, r2
        mov.l   r2, @(B_EV0, r5)
        mov.l   .Ls_intreq00, r1
        mov.b   @r1, r0
        extu.b  r0, r2
        shll8   r2
        mov.l   .Ls_rcr1, r1
        mov.b   @r1, r0
        extu.b  r0, r0
        or      r0, r2
        shll16  r2
        mov.l   .Ls_kiuintreg, r1
        mov.w   @r1, r0
        extu.w  r0, r0
        or      r0, r2
        mov.l   r2, @(B_EV1, r5)
        mov.l   .Ls_sdcr, r1
        mov.l   @r1, r0
        shll16  r0
        mov.l   .Ls_pllcr, r1
        mov.l   @r1, r2
        extu.w  r2, r2
        or      r2, r0
        mov.l   r0, @(B_EV2, r5)
        /* The OS's standby is a loop (02.01.7003 0x80020ED2): after every
         * wake-up it asks, from ROM with its stack in RS memory, whether
         * Clear is down (0x80070EB8) or a wake pin is requesting
         * (0x8006F886), and if not it executes SLEEP again; the SDRAM stays
         * in self-refresh throughout. The same here, before anything of the
         * machine is brought back: the reasons to go on are the RTC alarm
         * flag, Clear down, or a KEYSC event flag (the OAL's own test, bsp
         * power.c RStandbyOnce). Otherwise: KEYSC as the OS leaves it
         * (mode left and entered again unless KIUSTATEREG bit 0 is set,
         * KIUINTREG |= 0x7F), RWTCSR, BAR, STBCR = 0x21, SR, SLEEP. */
        mov     r5, r6
        add     #0x40, r6
        mov.l   @(B2_LOOP, r6), r0
        tst     r0, r0
        bt      .Ls_resume
        mov.l   .Lq_rcr1, r1
        mov.b   @r1, r0
        tst     #1, r0
        bf      .Ls_resume              /* RTC alarm */
        mov.l   .Lq_keysc, r1
        mov.w   @r1, r0                 /* KIUDATA0 */
        tst     #2, r0
        bf      .Ls_resume              /* Clear is down */
        mov.w   @(0x14, r1), r0         /* KIUINTREG */
        tst     #0xFF, r0
        bf      .Ls_resume              /* a key event since the flags were cleared */
        mov.l   @(B2_COUNT, r6), r0
        tst     r0, r0
        bf      7f
        mov.l   @(B_EV0, r5), r2        /* the first one: keep its evidence */
        mov.l   r2, @(B2_EV0, r6)
        mov.l   @(B_EV1, r5), r2
        mov.l   r2, @(B2_EV1, r6)
        mov.l   @(B_EV2, r5), r2
        mov.l   r2, @(B2_EV2, r6)
7:      add     #1, r0
        mov.l   r0, @(B2_COUNT, r6)
        mov.w   @(0x12, r1), r0         /* KIUSTATEREG */
        tst     #1, r0
        bf      8f
        mov     #0, r0
        mov.w   r0, @(0x10, r1)         /* KIUMODEREG = 0 */
        mov     #100, r2
9:      dt      r2
        bf      9b
        mov     #2, r0
        shll8   r0
        mov.w   r0, @(0x10, r1)         /* KIUMODEREG = 0x0200 */
8:      mov.w   @(0x14, r1), r0
        or      #0x7F, r0
        mov.w   r0, @(0x14, r1)
        mov.l   .Lq_rwtcsr, r1
        mov.l   .Lq_a507, r0
        mov.w   r0, @r1
        /* the SDRAM in self-refresh whatever the wake-up did to SDCR */
        mov.l   @(B_SDCR, r5), r0
        mov.l   .Lq_fdff, r2
        and     r2, r0
        mov     #4, r2
        shll8   r2
        or      r2, r0
        mov.l   .Lq_sdcr, r1
        mov.l   r0, @r1
        mov.l   .Lq_bar, r1
        mov.l   .Lq_self, r0
        mov.l   r0, @r1
        mov.l   .Lq_stbcr, r1
        mov     #0x21, r0
        mov.l   r0, @r1
        mov.l   .Lq_sr_standby, r0
        ldc     r0, sr                  /* MD | RB | BL, IMASK = 0: r0-r7 are bank 1 from here */
        sleep
        nop
        /* fell through (no R-standby after all): the ordinary resume */
        mov.l   .Lq_sr, r0
        ldc     r0, sr
        bra     .Ls_resume
        nop
        .align  2
.Lq_rcr1:       .long 0xA413FEDC
.Lq_keysc:      .long 0xA44B0000
.Lq_rwtcsr:     .long 0xA4520004
.Lq_a507:       .long 0x0000A507
.Lq_bar:        .long 0xA4150040
.Lq_self:       .long 0xFD801200        /* this stub (rt.c RS_STUB) */
.Lq_stbcr:      .long 0xA4150020
.Lq_sr_standby: .long 0x70000000
.Lq_sr:         .long 0x500000F0
.Lq_fdff:       .long 0xFFFFFDFF
.Lq_sdcr:       .long 0xFEC10044
.Ls_resume:
        mov.l   .Ls_stbcr, r1
        mov.l   @r1, r0
        mov.l   .Ls_no_rstby, r2
        and     r2, r0
        mov.l   r0, @r1
        mov.l   .Ls_bar, r1
        mov.l   .Ls_reset, r0
        mov.l   r0, @r1
        mov.l   .Ls_rwtcsr, r1
        mov.l   .Ls_a507, r0            /* a long: the pool is too far for mov.w */
        mov.w   r0, @r1
        STAGE   1, .Ls_red              /* the core restarted at BAR */
        /* the save block (RS memory, no SDRAM needed): without it, cold restart */
        mov.l   .Ls_block, r4
        mov.l   @(B_MAGIC, r4), r0
        mov.l   .Ls_magic, r1
        cmp/eq  r1, r0
        bt      3f
        mov.l   .Ls_reset, r0
        jmp     @r0
        nop
3:      STAGE   2, .Ls_yellow           /* the save block is intact */
        mov     r4, r5
        add     #B_HI, r5
        /* PLLCR bits 1-2 back to their saved state (OS 0x800209c4), the pins
         * back, the settling wait. The SDRAM stays in self-refresh until
         * SDCR is written below. */
        mov.l   .Ls_pllcr, r1
        mov.l   @r1, r0
        mov     #-7, r2                 /* ~6 */
        and     r2, r0
        mov.l   @(B_PLLCR, r5), r3
        mov     #6, r2
        and     r2, r3
        or      r3, r0
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
        /* SDRAM controller back to its running state (OS 0x80020a42); the
         * SDCR write (RMODE 0) ends the self-refresh */
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
        STAGE   3, .Ls_green            /* SDRAM controller running again */
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
        STAGE   4, .Ls_cyan             /* about to jump into the translator in SDRAM */
        mov.l   @(B_RESUME, r5), r0
        jmp     @r0
        nop

/* The way down, entered from _dbt_standby_sleep with the caches off, BAR and
 * STBCR set, r8 = the save block; it runs in RS memory because the SDRAM is
 * not to be touched from its first instruction on.
 *
 * The OS's "memory standby" (0x8002095E: RTCSR = 0xA55A0000, SDCR = 0,
 * PLLCR bit 1, PULCRBSC |= 0x12, PH6 released), which this copied until
 * 2026-11-10, does not keep the SDRAM's contents: no refresh, no clock.
 * The OS does not need them. After its standby loop it initialises the
 * SDRAM from cold (0x800209C4: wait, SDCR = 0xA09, mode register set) and
 * restarts its main loop on a new stack (0x800204A8); it never returns into
 * what was running. With every standby over in 0.1 s the cells held; in
 * the first 25 s one some did not (resets, freezes), after a minute or two
 * the translator's own code was gone (reset at the jump back into SDRAM).
 *
 * What keeps an SDRAM alive is self-refresh, and the OS has that too, for
 * its clock changes (0x80020F1C / 0x80020F3C): SDCR bit 9 (power-down)
 * clear and bit 10 (RMODE) set on the way in, the reverse on the way out.
 * That is done here. Of the OS's other steps, each only if its bit in
 * B2_VAR is set (rt.c tries them one by one): CKO off, the bus pulls, PH6. */
        .align  2
.Ls_sleep_tail:
        mov     r8, r6
        add     #0x40, r6
        add     #0x40, r6               /* block + 0x80 */
        mov.l   @(B2_VAR, r6), r7
        mov.l   .Lt_sdcr, r1
        mov.l   @r1, r0
        mov.l   .Lt_fdff, r2
        and     r2, r0
        mov     #4, r2
        shll8   r2
        or      r2, r0
        mov.l   r0, @r1                 /* self-refresh */
        mov     r7, r0
        tst     #1, r0
        bt      1f
        mov.l   .Lt_pllcr, r1
        mov.l   @r1, r0
        or      #2, r0
        mov.l   r0, @r1
1:      mov     r7, r0
        tst     #2, r0
        bt      2f
        mov.l   .Lt_pulcrbsc, r1
        mov.b   @r1, r0
        or      #0x12, r0
        mov.b   r0, @r1
2:      mov     r7, r0
        tst     #4, r0
        bt      3f
        mov.l   .Lt_phcr, r1
        mov.w   @r1, r0
        mov.l   .Lt_cfff, r2
        and     r2, r0
        mov.w   r0, @r1
3:      mov.l   .Lt_r64cnt, r1          /* more than one step of R64CNT */
        mov.b   @r1, r2
        extu.b  r2, r2
        mov     #1, r3
4:      mov.b   @r1, r0
        extu.b  r0, r0
        sub     r2, r0
        and     #63, r0
        cmp/gt  r3, r0
        bf      4b
        /* SR = MD|RB|BL with IMASK = 0 like the OS (0x80020EDE); r0-r7 are bank 1 after it */
        mov.l   .Lt_sr_standby, r0
        ldc     r0, sr
        sleep
        nop
        mov.l   .Lt_undo, r0            /* fell through: back into the SDRAM (P2), which undoes all this */
        jmp     @r0
        nop
        .align  2
.Lt_sdcr:       .long 0xFEC10044
.Lt_fdff:       .long 0xFFFFFDFF
.Lt_pllcr:      .long 0xA4150024
.Lt_pulcrbsc:   .long 0xA40501C3
.Lt_phcr:       .long 0xA405010E
.Lt_cfff:       .long 0xFFFFCFFF
.Lt_r64cnt:     .long 0xA413FEC0
.Lt_sr_standby: .long 0x70000000
.Lt_undo:       .long .Lfell_through + 0x20000000

#ifdef DBT_STANDBY_DIAG
/* r7 = stage, r6 = RGB565 colour. Stage 1 wakes the panel first (it is in
 * SLPIN with the brightness at 0: SLPOUT, ~140 ms, DISPON, 0x51 = 0xFF).
 * r8 PRDR, r9 LCD port, r10 PR, r11 colour, r12 stage then first row,
 * r13-r15 scratch. Native writes: no byte swap (rt/lcd_io.h). */
.Ls_stripe:
        sts     pr, r10
        mov     r7, r12
        mov     r6, r11
        mov.l   .Ls_prdr, r8
        mov.l   .Ls_lcdport, r9
        mov     #1, r0
        cmp/eq  r0, r12
        bf      4f
        mov     #0x11, r0
        bsr     .Ls_cmd
        nop
        mov     #9, r0
        bsr     .Ls_wait
        nop
        mov     #0x29, r0
        bsr     .Ls_cmd
        nop
        mov     #0x51, r0
        bsr     .Ls_cmd
        nop
        mov     #-1, r0
        extu.b  r0, r0
        mov.w   r0, @r9
        SYNCO
4:      mov     r12, r0
        add     #-1, r0
        mov     #66, r13
        mulu.w  r0, r13
        sts     macl, r12               /* first row */
        mov     #0x2A, r0               /* columns 0..319 */
        bsr     .Ls_cmd
        nop
        mov     #0, r0
        mov.w   r0, @r9
        SYNCO
        mov.w   r0, @r9
        SYNCO
        mov     #1, r0
        mov.w   r0, @r9
        SYNCO
        mov     #0x3F, r0
        mov.w   r0, @r9
        SYNCO
        mov     #0x2B, r0               /* rows first..first+65 */
        bsr     .Ls_cmd
        nop
        mov     r12, r0
        shlr8   r0
        mov.w   r0, @r9
        SYNCO
        mov     r12, r0
        extu.b  r0, r0
        mov.w   r0, @r9
        SYNCO
        mov     r12, r0
        add     #65, r0
        mov     r0, r14
        shlr8   r0
        mov.w   r0, @r9
        SYNCO
        mov     r14, r0
        extu.b  r0, r0
        mov.w   r0, @r9
        SYNCO
        mov     #0x2C, r0
        bsr     .Ls_cmd
        nop
        mov.l   .Ls_npix, r13
5:      mov.w   r11, @r9
        dt      r13
        bf      5b
        SYNCO
        mov     #32, r0                 /* ~0.5 s */
        bsr     .Ls_wait
        nop
        lds     r10, pr
        rts
        nop
/* r0 = DCS command: RS low, the command, RS high. Clobbers r0, r13. */
.Ls_cmd:
        mov     r0, r13
        SYNCO
        mov.b   @r8, r0
        and     #0xEF, r0
        mov.b   r0, @r8
        SYNCO
        mov.w   r13, @r9
        SYNCO
        mov.b   @r8, r0
        or      #0x10, r0
        mov.b   r0, @r8
        SYNCO
        rts
        nop
/* r0 = ticks of the RTC's 64 Hz counter (< 64). Clobbers r0, r13-r15. */
.Ls_wait:
        mov     r0, r15
        mov.l   .Ls_r64cnt, r13
        mov.b   @r13, r14
        extu.b  r14, r14
1:      mov.b   @r13, r0
        extu.b  r0, r0
        sub     r14, r0
        and     #63, r0
        cmp/ge  r15, r0
        bf      1b
        rts
        nop
        .align  2
.Ls_prdr:       .long 0xA405013C
.Ls_lcdport:    .long 0xB4000000
.Ls_npix:       .long 320 * 66
.Ls_red:        .long 0xF800
.Ls_yellow:     .long 0xFFE0
.Ls_green:      .long 0x07E0
.Ls_cyan:       .long 0x07FF
#endif
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
.Ls_a507:       .long 0x0000A507
.Ls_nmifcr:     .long 0xA41400C0
.Ls_intreq00:   .long 0xA4140024
.Ls_rcr1:       .long 0xA413FEDC
.Ls_kiuintreg:  .long 0xA44B0014
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
        /* SPC and SSR back (SH7724 18.4.6: "save SPC and SSR before
         * executing the SLEEP instruction"). The exception path that issued
         * HC_STANDBY ends with rte on them; after the core reset they are
         * garbage, the rte went nowhere and the next exception, with SR.BL
         * set, reset the chip into the Casio OS (board test, 2026-11-06:
         * every stage up to the translator's C code passed). casio-emu keeps
         * the core registers across its R-standby. */
        mov.l   @(B_SPC, r5), r0
        ldc     r0, spc
        mov.l   @(B_SSR, r5), r0
        ldc     r0, ssr
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
