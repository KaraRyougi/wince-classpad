/* loops.S - benchmark kernels, assembled twice: big-endian into the
 * translator image (native run) and little-endian into the bench guest
 * (translated run). Pure code: byte order only changes the buffer contents,
 * never the instruction count. r4 = iterations, r5 = 256-byte buffer,
 * returns r0. Only r0-r7 and r15 are used. */
        .text
        .macro  HEAD
        mov     #1, r1
        mov     #2, r2
        mov     #3, r3
        mov     #0, r7
        mov     #5, r6
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

/* ALU only: 5 ALU ops + dt + bf per iteration (baseline, expect ~1.0x) */
        .global _bench_alu
_bench_alu:
        HEAD
1:      add     r1, r2
        xor     r2, r3
        add     r3, r1
        rotl    r6
        add     #1, r7
        dt      r4
        bf      1b
        TAIL

/* memory: 4 longword, 2 word, 2 byte accesses + 2 ALU + dt + bf per iteration */
        .global _bench_mem
_bench_mem:
        HEAD
1:      mov.l   @r5, r1
        mov.l   @(4, r5), r2
        add     r2, r1
        mov.l   r1, @r5
        mov.l   r2, @(4, r5)
        mov.w   @(8, r5), r0
        mov.w   r0, @(10, r5)
        mov.b   @(12, r5), r0
        mov.b   r0, @(13, r5)
        add     #1, r7
        dt      r4
        bf      1b
        TAIL

/* memory, pointer walk: memcpy-like (post-increment loads, stores, 32 words wrap) */
        .global _bench_copy
_bench_copy:
        HEAD
        mov     r5, r6
        mov     r5, r7
        add     #64, r7
        mov     #0, r3
1:      mov.l   @r6+, r1
        mov.l   r1, @r7
        add     #4, r7
        mov.l   @r6+, r2
        mov.l   r2, @r7
        add     #4, r7
        add     #1, r3
        mov     r3, r0
        and     #7, r0
        tst     r0, r0
        bf      2f
        mov     r5, r6                 /* wrap every 8 iterations (16 words) */
        mov     r5, r7
        add     #64, r7
2:      dt      r4
        bf      1b
        mov     r3, r7
        TAIL

/* stack: push/pop 2 registers and PR per iteration (pre-decrement / post-increment) */
        .global _bench_stack
_bench_stack:
        HEAD
1:      mov.l   r1, @-r15
        mov.l   r2, @-r15
        sts.l   pr, @-r15
        lds.l   @r15+, pr
        mov.l   @r15+, r2
        mov.l   @r15+, r1
        add     #1, r7
        dt      r4
        bf      1b
        TAIL

/* branches: 3 data-dependent conditional branches (about half taken) + loop back edge */
        .global _bench_branch
_bench_branch:
        HEAD
1:      mov     r7, r0
        tst     #1, r0
        bt      2f
        add     #1, r1
2:      tst     #2, r0
        bf      3f
        add     #2, r2
3:      tst     #4, r0
        bt/s    4f
        add     #1, r7
        xor     r1, r3
4:      dt      r4
        bf      1b
        TAIL

/* calls, indirect: jsr @reg to a leaf, rts back */
        .global _bench_jsr
_bench_jsr:
        sts.l   pr, @-r15
        HEAD
        mov.l   .Lleaf, r6
1:      jsr     @r6
        nop
        add     #1, r7
        dt      r4
        bf      1b
        lds.l   @r15+, pr
        TAIL

/* calls, direct: bsr to a leaf, rts back */
        .global _bench_bsr
_bench_bsr:
        sts.l   pr, @-r15
        HEAD
1:      bsr     _bench_leaf
        nop
        add     #1, r7
        dt      r4
        bf      1b
        lds.l   @r15+, pr
        TAIL

/* calls, C-style: jsr to a function with a frame (push/pop r8,r9,pr) */
        .global _bench_callframe
_bench_callframe:
        sts.l   pr, @-r15
        HEAD
        mov.l   .Lframe, r6
1:      jsr     @r6
        nop
        add     #1, r7
        dt      r4
        bf      1b
        lds.l   @r15+, pr
        TAIL

/* empty: call overhead probe */
        .global _bench_empty
_bench_empty:
        rts
        mov     #0, r0

        .align 2
.Lleaf:  .long   _bench_leaf
.Lframe: .long   _bench_frame

        .global _bench_leaf
_bench_leaf:
        add     #3, r1
        rts
        xor     r1, r2

        .global _bench_frame
_bench_frame:
        mov.l   r8, @-r15
        mov.l   r9, @-r15
        sts.l   pr, @-r15
        mov     r1, r8
        add     r2, r8
        mov     r8, r9
        add     r9, r3
        lds.l   @r15+, pr
        mov.l   @r15+, r9
        rts
        mov.l   @r15+, r8
