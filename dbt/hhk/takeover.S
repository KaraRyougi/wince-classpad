/* takeover.S - leave Casio's OS and start the translator image, the way
 * casio-emu's launcher (dbt/loader/loader.S) hands the machine over.
 *
 *   void hhk_takeover(const void *src, void *dst, unsigned len, unsigned entry)
 *
 * 1. interrupts masked (SR.IMASK=15, RB forced to 0; MD stays 1), nothing
 *    else in SR touched so a fault still reaches a handler instead of a reset;
 * 2. the OS's interrupt sources are quieted so nothing is pending or
 *    unmasked when the CE kernel lowers IMASK: TMU stopped (TSTR, TCR0-2),
 *    every INTC priority 0 (IPRA-IPRL), every INTC source masked (IMR0-12),
 *    RTC interrupts off (RCR1, RCR2 keeps the clock running); the IRQ pin
 *    mask INTMSK00 stays as Casio's OS left it. The OAL unmasks what it uses (bsp intr.c);
 * 3. MMU off with the TLB flushed (MMUCR = TI): Casio's OS keeps a TLB entry
 *    for the add-in window; we and the kernel run in P1/P2;
 * 4. the image copied to its link address (skipped when src == dst, the
 *    .hhk case: the HollyHock launcher already put it there) and the caches
 *    made coherent over it (ocbwb + icbi per line, synco), as loader.S does;
 * 5. jump to the entry (crt0.S sets the stack, clears bss, calls dbt_main).
 * Runs from RAM (the HollyHock window or the add-in window), uses no stack.
 * Addresses: bsp/classpad/SRC/INC/bsp_base_regs.h, dbt/guest/tmu.c. */
        .text
        .global _hhk_takeover
_hhk_takeover:
        mov     r4, r8                  /* args into unbanked registers: a bank */
        mov     r5, r9                  /* switch below must not lose them */
        mov     r6, r10
        mov     r7, r11
        stc     sr, r0
        mov.l   .Lrb, r1
        and     r1, r0                  /* RB = 0 */
        or      #0xF0, r0               /* IMASK = 15 */
        ldc     r0, sr
        mov     r8, r4
        mov     r9, r5
        mov     r10, r6
        mov     r11, r7

        /* TMU: stop the three channels, clear underflow flags and interrupt enables */
        mov.l   .Ltstr, r1
        mov     #0, r0
        mov.b   r0, @r1
        mov.l   .Ltcr0, r1
        mov     #3, r2
1:      mov.w   r0, @r1
        add     #12, r1
        dt      r2
        bf      1b
        /* INTC: IPRA..IPRL = 0 (16-bit, 4-byte stride), IMR0..IMR12 = 0xFF (8-bit) */
        mov.l   .Lipr, r1
        mov     #12, r2
2:      mov.w   r0, @r1
        add     #4, r1
        dt      r2
        bf      2b
        mov.l   .Limr, r1
        mov     #13, r2
        mov     #-1, r3                 /* 0xFF: write 1 masks */
3:      mov.b   r3, @r1
        add     #4, r1
        dt      r2
        bf      3b
        /* INTMSK00 (IRQ pins) is left as Casio's OS set it: the touch contact
         * interrupt may be a pin interrupt the OAL cannot unmask itself (the
         * first hardware run had a desktop that took no input) */
        /* RTC: carry/alarm interrupts off, periodic interrupt off, clock running */
        mov.l   .Lrcr1, r1
        mov.b   r0, @r1
        mov     #1, r3
        mov.l   .Lrcr2, r1
        mov.b   r3, @r1
        /* MMU: AT=0, flush the TLB */
        mov.l   .Lmmucr, r1
        mov     #4, r3
        mov.l   r3, @r1

        /* copy src -> dst, len bytes (multiples of 4), unless already in place */
        cmp/eq  r4, r5
        bt      5f
        mov     r4, r0
        mov     r5, r1
        mov     r4, r2
        add     r6, r2
4:      mov.l   @r0+, r3
        mov.l   r3, @r1
        add     #4, r1
        cmp/hs  r2, r0
        bf      4b
5:      /* write back + invalidate the instruction cache over dst..dst+len */
        mov     r5, r1
        mov     r5, r0
        add     r6, r0
6:      ocbwb   @r1
        .word   0x01E3                  /* icbi @r1 */
        add     #32, r1
        cmp/hs  r0, r1
        bf      6b
        .word   0x00AB                  /* synco */
        jmp     @r7
        nop

        .align 2
.Lrb:     .long 0xDFFFFFFF              /* ~SR.RB */
.Ltstr:   .long 0xA4490004
.Ltcr0:   .long 0xA4490010              /* TCR1 = +12, TCR2 = +24 */
.Lipr:    .long 0xA4080000
.Limr:    .long 0xA4080080
.Lrcr1:   .long 0xA413FEDC
.Lrcr2:   .long 0xA413FEDE
.Lmmucr:  .long 0xFF000010
