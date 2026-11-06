/* tmu_loops.S - hot loops of one construct each, run with interrupts on and
 * off by tmu.c to localize resume bugs. r4 = iterations, r5 = &scratch[0]
 * (16 words), returns a checksum in r0. Only r0-r7 and r15 are used. */
        .text
        .macro  HEAD
        mov     #1, r1
        mov     #2, r2
        mov     #3, r3
        mov     #0, r7
        mov     #5, r6
        .endm
        .macro  BODY
        add     r1, r2
        xor     r2, r3
        add     r3, r1
        rotl    r6
        add     #1, r7
        .endm
        .macro  TAIL
        mov     r1, r0
        xor     r2, r0
        xor     r3, r0
        xor     r6, r0
        xor     r7, r0
        rts
        nop
        .endm

        .global _loop_alu
_loop_alu:
        HEAD
1:      BODY
        dt      r4
        bf      1b
        TAIL

        .global _loop_trapa                    /* same as loop_alu plus a trapa each iteration */
_loop_trapa:
        HEAD
1:      BODY
        trapa   #5
        dt      r4
        bf      1b
        TAIL

        .global _loop_ldcsr                    /* a helper call (ldc rm,sr) each iteration */
_loop_ldcsr:
        HEAD
1:      BODY
        stc     sr, r0
        ldc     r0, sr
        dt      r4
        bf      1b
        TAIL

        .global _loop_bfs
_loop_bfs:
        HEAD
1:      BODY
        dt      r4
        bf/s    1b
        xor     r1, r6
        TAIL

        .global _loop_tslot                    /* bt/s with a T-writing slot */
_loop_tslot:
        HEAD
1:      BODY
        cmp/pl  r4
        bt/s    1b
        dt      r4
        TAIL

        .global _loop_ld
_loop_ld:
        HEAD
        mov.l   r1, @r5
1:      BODY
        mov.l   @r5, r6                        /* plain load */
        add     r6, r1
        dt      r4
        bf      1b
        TAIL

        .global _loop_ldpost
_loop_ldpost:
        HEAD
        mov.l   r1, @r5
1:      BODY
        mov.l   @r5+, r6                       /* post-increment load */
        add     #-4, r5
        add     r6, r1
        dt      r4
        bf      1b
        TAIL

        .global _loop_ldself                   /* mov.l @r6,r6 */
_loop_ldself:
        HEAD
        mov.l   r5, @r5                        /* scratch[0] = &scratch */
1:      BODY
        mov     r5, r6
        mov.l   @r6, r6
        xor     r6, r1
        dt      r4
        bf      1b
        TAIL

        .global _loop_st
_loop_st:
        HEAD
1:      BODY
        mov.l   r1, @r5                        /* plain store */
        mov.l   @r5, r6
        dt      r4
        bf      1b
        TAIL

        .global _loop_stpre
_loop_stpre:
        HEAD
        add     #8, r5
1:      BODY
        mov.l   r1, @-r5                       /* pre-decrement store */
        mov.l   @r5+, r6
        xor     r6, r2
        dt      r4
        bf      1b
        TAIL

        .global _loop_r0idx
_loop_r0idx:
        HEAD
1:      BODY
        mov     r7, r0
        and     #15, r0
        shll2   r0
        mov.l   r1, @(r0, r5)                  /* r0-indexed store */
        mov.l   @(r0, r5), r6
        dt      r4
        bf      1b
        TAIL

        .global _loop_r0disp                   /* r0 as data: mov.l r0,@(disp,rn) */
_loop_r0disp:
        HEAD
1:      BODY
        mov     r1, r0
        mov.l   r0, @(4, r5)
        mov.l   @(4, r5), r6
        dt      r4
        bf      1b
        TAIL

        .global _loop_rmw
_loop_rmw:
        HEAD
        mov.l   r1, @r5
1:      BODY
        mov.l   @r5, r6
        add     r1, r6
        mov.l   r6, @r5
        dt      r4
        bf      1b
        mov.l   @r5, r6
        xor     r6, r1
        TAIL

        .global _loop_pcrel
_loop_pcrel:
        HEAD
1:      BODY
        mov.l   2f, r6
        xor     r6, r1
        mova    2f, r0
        xor     r0, r2
        dt      r4
        bf      1b
        TAIL
        .align 2
2:      .long   0x12345678

        .global _loop_jsr
_loop_jsr:
        sts.l   pr, @-r15
        HEAD
1:      BODY
        mov.l   .Lsub, r6
        jsr     @r6
        nop
        dt      r4
        bf      1b
        lds.l   @r15+, pr
        TAIL

        .global _loop_jsrslot                  /* store in the slot of jsr */
_loop_jsrslot:
        sts.l   pr, @-r15
        HEAD
1:      BODY
        mov.l   .Lsub, r6
        jsr     @r6
        mov.l   r1, @r5
        mov.l   @r5, r6
        xor     r6, r3
        dt      r4
        bf      1b
        lds.l   @r15+, pr
        TAIL

        .global _loop_jsrtgt                   /* jsr @r3 whose slot store swaps through r3, its lowest free scratch */
_loop_jsrtgt:
        sts.l   pr, @-r15
        mov.l   r8, @-r15
        HEAD
        mov     r5, r8
1:      BODY
        mov.l   r3, @(8, r8)
        mov.l   r2, @(12, r8)
        mov.l   .Lleaf, r3
        bra     2f                             /* block boundary: the jsr block scores 0, no borrowed register */
        mov     r8, r2
2:      jsr     @r3
        mov.l   r1, @r2                        /* data r1, base r2: the swap scratch is r3, the target */
        mov.l   @(8, r8), r3
        mov.l   @(12, r8), r2
        dt      r4
        bf      1b
        mov.l   @r15+, r8
        lds.l   @r15+, pr
        TAIL
        .align 2
.Lleaf: .long   _loop_leaf
_loop_leaf:
        rts
        nop

        .global _loop_pushpop                  /* sts.l/lds.l pr, mov.l @-r15 / @r15+ */
_loop_pushpop:
        HEAD
1:      BODY
        mov.l   r1, @-r15
        sts.l   pr, @-r15
        mov.l   r2, @-r15
        mov.l   @r15+, r6
        lds.l   @r15+, pr
        mov.l   @r15+, r6
        xor     r6, r3
        dt      r4
        bf      1b
        TAIL

/* trap_probe(out): r0-r14 = 0x11111111*k+0x100, trapa #5, store r0-r14 to out[0..14], T to out[15] */
        .global _trap_probe
_trap_probe:
        mov.l   r8, @-r15
        mov.l   r9, @-r15
        mov.l   r10, @-r15
        mov.l   r11, @-r15
        mov.l   r12, @-r15
        mov.l   r13, @-r15
        mov.l   r14, @-r15
        mov.l   r4, @-r15                      /* out */
        mov.l   .Lp0, r0
        mov     r0, r1
        add     r0, r1
        mov     r1, r2
        add     r0, r2
        mov     r2, r3
        add     r0, r3
        mov     r3, r4
        add     r0, r4
        mov     r4, r5
        add     r0, r5
        mov     r5, r6
        add     r0, r6
        mov     r6, r7
        add     r0, r7
        mov     r7, r8
        add     r0, r8
        mov     r8, r9
        add     r0, r9
        mov     r9, r10
        add     r0, r10
        mov     r10, r11
        add     r0, r11
        mov     r11, r12
        add     r0, r12
        mov     r12, r13
        add     r0, r13
        mov     r13, r14
        add     r0, r14
        sett
        trapa   #5
        mov.l   @r15, r15                      /* r15 = out (stack pointer value saved separately below) */
        mov.l   r0, @(0, r15)
        mov.l   r1, @(4, r15)
        mov.l   r2, @(8, r15)
        mov.l   r3, @(12, r15)
        mov.l   r4, @(16, r15)
        mov.l   r5, @(20, r15)
        mov.l   r6, @(24, r15)
        mov.l   r7, @(28, r15)
        mov.l   r8, @(32, r15)
        mov.l   r9, @(36, r15)
        mov.l   r10, @(40, r15)
        mov.l   r11, @(44, r15)
        mov.l   r12, @(48, r15)
        mov.l   r13, @(52, r15)
        mov.l   r14, @(56, r15)
        movt    r0
        mov.l   r0, @(60, r15)
        add     #64, r15
        mov.l   @r15, r15                      /* out[16] holds the saved stack pointer */
        add     #4, r15
        mov.l   @r15+, r14
        mov.l   @r15+, r13
        mov.l   @r15+, r12
        mov.l   @r15+, r11
        mov.l   @r15+, r10
        mov.l   @r15+, r9
        mov.l   @r15+, r8
        rts
        nop
        .align 2
.Lp0:   .long   0x11111111

        .align 2
.Lsub:  .long   _loop_sub
        .global _loop_sub
_loop_sub:
        add     #3, r1
        rts
        xor     r1, r2

/* Carry both T values through an indirect call while IRQs sweep the IC. */
        .align 2
        .global _loop_tcarry
_loop_tcarry:
        sts.l   pr,@-r15
        mov     r4,r6
        mov     #0,r7
        mov.l   .Ltreader,r1
1:      clrt
        jsr     @r1
        nop
        add     r0,r7
        sett
        jsr     @r1
        nop
        add     r0,r7
        dt      r6
        bf      1b
        mov     r7,r0
        lds.l   @r15+,pr
        rts
        nop
        .align 2
.Ltreader: .long _loop_treader
_loop_treader:
        movt    r0
        rts
        nop

/* Materialize a normal-memory address inside every loop iteration. The
 * first load can omit the device classifier only after that literal load. */
        .align 2
        .global _loop_constaddr, _loop_constaddr_end
_loop_constaddr:
1:      mov.l .Lconstscratch,r1
        mov.l @r1,r0
        add #1,r0
        mov.l r0,@r1
        dt r4
        bf 1b
        rts
        nop
        .align 2
.Lconstscratch: .long _scratch
_loop_constaddr_end:

/* Literal-address native P4 store, including interrupt restart/commit paths. */
        .align 2
        .global _loop_constp4, _loop_constp4_end
_loop_constp4:
1:      mov.l .Lconstp4tea,r1
        mov.l r4,@r1
        dt r4
        bf 1b
        rts
        nop
        .align 2
.Lconstp4tea: .long 0xFF00000C
_loop_constp4_end:

        .align 2
        .global _loop_uniform_store
_loop_uniform_store:
1:      mov     #0,r1
        mov.l   r1,@r5
        mov     #-1,r2
        mov.l   r2,@(4,r5)
        dt      r4
        bf      1b
        rts
        nop

        .align 2
        .global _loop_r0_device
        .global _loop_r0_device_end
_loop_r0_device:
        mov     #85,r0
1:      mov.l   @r5,r2
        add     #1,r2
        mov.l   r2,@r5
        dt      r4
        bf      1b
        rts
        nop
_loop_r0_device_end:

        .align 2
        .global _loop_device_read_slots
        .global _loop_device_read_slots_end
_loop_device_read_slots:
        mov     #0,r1
1:      mov     r5,r6
        mov.l   @r6+,r2
        add     r2,r1
        dt      r4
        bf/s    1b
        mov.l   @r5,r3
        xor     r3,r1
        xor     r6,r1
        rts
        mov     r1,r0
_loop_device_read_slots_end:

        .align 2
        .global _loop_r0_operand
        .global _loop_r0_operand_end
_loop_r0_operand:
        mov     r5,r0
1:      mov.l   @r0,r2
        add     #1,r2
        mov.l   r2,@r0
        dt      r4
        bf      1b
        mov     #85,r0
        rts
        nop
_loop_r0_operand_end:

/* Both SR operand forms change virtual MD without changing RB or ASID.
 * Every readback must reflect the new guest mode, including across IRQs. */
        .global _loop_ldcsr_md
_loop_ldcsr_md:
        stc     sr, r3
        mov     #1, r2
        shll16  r2
        shll8   r2
        shll2   r2
        shll2   r2
        shll2   r2
        mov     r3, r1
        xor     r2, r1
        mov     #0, r6
1:      ldc     r1, sr
        stc     sr, r0
        xor     r1, r0
        or      r0, r6
        /* The existing translator requires the privileged memory-load
         * tail to be masked. Re-enable IRQs via the restored SR. */
        mov     r1, r0
        or      #0xf0, r0
        ldc     r0, sr
        mov.l   r3, @-r15
        ldc.l   @r15+, sr
        stc     sr, r0
        xor     r3, r0
        or      r0, r6
        dt      r4
        bf      1b
        rts
        mov     r6, r0

        .align 2
        .global _loop_same_asid_pteh, _loop_same_asid_pteh_end
_loop_same_asid_pteh:
        mov.l   2f, r1
        mov     #0, r0
1:      sett
        mov.l   r5, @r1
        movt    r2
        add     r2, r0
        dt      r4
        bf      1b
        rts
        nop
        .align 2
2:      .long 0xff000000
_loop_same_asid_pteh_end:

        .align 2
        .global _loop_fast_ldtlb
_loop_fast_ldtlb:
        mov     #0x5a, r0
        mov     #0x6b, r1
        mov     #0x7c, r2
        mov     #0, r6
1:      sett
        ldtlb
        movt    r3
        add     r3, r6
        clrt
        ldtlb
        movt    r3
        add     r3, r6
        dt      r4
        bf      1b
        add     r0, r6
        add     r1, r6
        add     r2, r6
        rts
        mov     r6, r0

        .align 2
        .global _ldtlb_change_asid
_ldtlb_change_asid:
        mov.l   1f, r1
        mov.l   r4, @r1                 /* caller pre-swaps raw MMIO value */
        ldtlb
        rts
        nop
        .align 2
1:      .long 0xff000000
