/* tlbrefill_vectors.S - vectors for the tlbrefill guest. VBR + 0x400 holds
 * Windows CE 6's SH TLBMissHandler word for word (shexcept.src, as in the
 * ClassPad kernel.dll), with its literal pool at the same offsets, so the
 * translator recognises it and refills UTLB misses itself (rt.c
 * tlb_refill). Its failure exit (TLBMissError) maps the page to P_ERR and
 * counts. 0x100 and 0x600 are unexpected: they rte into tlbr_unexpected.
 * Bank 1 r7 must hold SH3CTL_BASE (0xFF000000), as under CE. */
        .text
        .balign 4096
        .global _guest_vectors
_guest_vectors:
        .org    0x100
        bra     _vec_unexpected
        nop
        .org    0x400
_tlb_miss_handler:
        .short  0x5273, 0x5072, 0xe6ff, 0xd12a, 0x3212, 0x8b0e, 0x0032, 0x4029
        .short  0x4019, 0xc940, 0x8840, 0x8905, 0x4211, 0x8b2a, 0xe6df, 0x5079
        .short  0x8840, 0x8b26, 0xd023, 0x6002, 0x500b, 0x933e, 0x6123, 0x4119
        .short  0x4109, 0x6213, 0x2239, 0x4119, 0x4109, 0x2139, 0x301c, 0x6002
        .short  0x4011, 0x8916, 0x302c, 0x6002, 0x9230, 0x2209, 0x4215, 0x8b10
        .short  0x2069, 0x6103, 0xe3f7, 0x413d, 0xe301, 0x2138, 0xe3ec, 0x413d
        .short  0x8900, 0x7108, 0x171d, 0x1701, 0x0038, 0x0009, 0x0009, 0x002b
        .short  0x0009
        bra     _tlbr_error             /* +0x72: TLBMissError */
        nop
        .org    0x400 + 0xAA
        .short  0x0FFC                  /* index mask */
        .short  0x0100                  /* PTE valid bit */
        .org    0x400 + 0xB0
        .long   0x70000000              /* VM_SHARED_HEAP_BASE */
        .long   _fake_pprcNK            /* &g_pprcNK */

        .org    0x600
        bra     _vec_unexpected
        nop

/* TLBMissError: count, map the page (PTEH holds it) to P_ERR, retry */
        .align 2
_tlbr_error:
        mov.l   .Lerrors, r0
        mov.l   @r0, r1
        add     #1, r1
        mov.l   r1, @r0
        mov.l   .Lperr, r1
        mov.l   r1, @(4, r7)            /* PTEL (device-aware page: no swap) */
        ldtlb
        nop
        rte
        nop
        .align 2
.Lerrors: .long _tlbr_errors
.Lperr:   .long 0x0C30B17C              /* P_ERR 0x8C30B000, V PR=11 4K C D */

_vec_unexpected:
        stc     spc, r0
        mov.l   .Lunexp_spc, r1
        mov.l   r0, @r1
        mov.l   .Lunexp_fn, r0
        ldc     r0, spc
        rte
        nop
        .align 2
.Lunexp_spc: .long _tlbr_unexpected_spc
.Lunexp_fn:  .long _tlbr_unexpected
