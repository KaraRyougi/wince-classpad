/* mmu.c - MMU on under the translator: guest code in a TLB-mapped P0 page is
 * fetched (by the translator's probe) through the UTLB entry, data accesses
 * of translated code go through the TLB, and a miss on a page without an
 * entry reaches the guest's TLB-miss handler (EXPEVT 0x040, TEA), which
 * refills with ldtlb and retries, exactly the Windows CE flow. */
#include "hyper.h"
#include "mmio.h"

#define PTEH   0xFF000000u
#define PTEL   0xFF000004u
#define MMUCR  0xFF000010u

#define V_CODE  0x00400000u            /* virtual pages (P0) */
#define V_DATA  0x00401000u
#define V_LAZY  0x00402000u            /* mapped only by the miss handler */
#define P_CODE  0x8C300000u            /* physical pages, through P1 */
#define P_DATA  0x8C301000u
#define P_LAZY  0x8C302000u
#define PTEL_FLAGS 0x17Eu              /* V | PR=11 | C | D | SH, 4 KiB */

extern char guest_vectors[];
volatile unsigned fault_pr;
extern unsigned r0_fault_load(unsigned, volatile unsigned *), r0_fault_store(unsigned, unsigned);
extern char r0_fault_load_site[], r0_fault_store_site[], r0_fault_end[];
__asm__(".text\n.align 2\n.global _r0_fault_load\n.global _r0_fault_store\n"
        ".global _r0_fault_load_site\n.global _r0_fault_store_site\n.global _r0_fault_end\n"
        "_r0_fault_load:\n mov r4,r0\n_r0_fault_load_site:\n mov.l @r0+,r1\n"
        "mov.l r1,@r5\n rts\n nop\n"
        "_r0_fault_store:\n mov r4,r0\n_r0_fault_store_site:\n mov.l r5,@-r0\n"
        "rts\n nop\n_r0_fault_end:\n");
static int fails;
static volatile unsigned faults, misses, last_vec, last_expevt, last_tea, last_spc;

static void check(const char *name, int ok)
{
    hc_puts(ok ? "PASS " : "FAIL "); hc_puts(name); hc_putc('\n');
    if (!ok) fails++;
}

static void tlb_map(unsigned v, unsigned p)
{
    mmio_w32(PTEH, v & 0xFFFFFC00u);                          /* VPN, ASID 0 */
    mmio_w32(PTEL, ((p & 0x1FFFFFFFu) & 0xFFFFFC00u) | PTEL_FLAGS);   /* 29-bit physical */
    __asm__ volatile ("ldtlb" ::: "memory");
}

unsigned guest_fault(unsigned vec, unsigned spc, unsigned expevt, unsigned tea)
{
    faults++;
    last_vec = vec; last_expevt = expevt; last_tea = tea; last_spc = spc;
    if ((expevt == 0x040 || expevt == 0x060) && (tea & 0xFFFFF000u) == V_LAZY) {
        misses++;
        tlb_map(V_LAZY, P_LAZY);
        return 0;                                            /* retry the faulting access */
    }
    hc_puts("unexpected fault vec "); hc_puthex(vec); hc_puts(" expevt "); hc_puthex(expevt);
    hc_puts(" tea "); hc_puthex(tea); hc_puts(" spc "); hc_puthex(spc); hc_putc('\n');
    hc_exit(3);
    return 0;
}

/* position-independent leaf, assembled by hand: r0 = r4 + r5 + *(r6)
 * (the load is a data access through whatever maps r6):
 * mov r5,r0 ; add r4,r0 ; mov.l @r6,r1 ; rts ; add r1,r0 */
static const unsigned short fn_code[] = { 0x6053, 0x304C, 0x6162, 0x000B, 0x301C };
/* lazy page: mov #7,r0 ; rts ; nop */
static const unsigned short lazy_code[] = { 0xE007, 0x000B, 0x0009 };

static void copy_code(unsigned dst, const unsigned short *src, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++) ((volatile unsigned short *)dst)[i] = src[i];
}

typedef unsigned (*fn2_t)(unsigned, unsigned, const unsigned *);
typedef unsigned (*fn0_t)(void);

void guest_main(void)
{
    unsigned r;
    hc_puts("mmu test: TLB-mapped code and data pages under the translator\n");
    __asm__ volatile ("ldc %0, vbr" :: "r"(guest_vectors));

    copy_code(P_CODE, fn_code, 5);
    copy_code(P_LAZY, lazy_code, 3);
    *(volatile unsigned *)P_DATA = 100;
    hc_flush(P_CODE, 0x3000);                                /* the translator may hold nothing for these pages, but be explicit */

    /* MMU on: flush, then AT=1; map the code and data pages */
    mmio_w32(MMUCR, 0x4);                                    /* TI */
    mmio_w32(MMUCR, 0x1);                                    /* AT */
    tlb_map(V_CODE, P_CODE);
    tlb_map(V_DATA, P_DATA);
    check("MMUCR.AT reads back", (mmio_r32(MMUCR) & 1) == 1);

    r = ((fn2_t)V_CODE)(2, 3, (const unsigned *)V_DATA);     /* code and data through the TLB */
    check("call through a TLB-mapped code page", r == 105 && faults == 0);
    check("data load through a TLB-mapped page", r == 105);
    if (hc_getstat(419)) {
        unsigned fast = hc_getstat(420);
        check("4 KiB no-code LDTLB fast path executed", fast > 0);
        tlb_map(V_CODE, P_CODE);
        check("translated code page keeps LDTLB bookkeeping", hc_getstat(420) == fast);
        check("code page remains callable after guarded refill", ((fn2_t)V_CODE)(2, 3, (const unsigned *)V_DATA) == 105);
    }


    *(volatile unsigned *)V_DATA = 1000;                     /* data store through the TLB */
    check("data store through the TLB lands in the physical page", *(volatile unsigned *)P_DATA == 1000);
    r = ((fn2_t)V_CODE)(5, 5, (const unsigned *)V_DATA);
    check("second call (block cached by virtual pc)", r == 1010);

    r = ((fn0_t)V_LAZY)();                                   /* no entry: miss -> handler maps -> retry */
    check("TLB miss on code fetch reached the handler", misses == 1 && last_vec == 4);
    check("EXPEVT = 0x040, TEA = SPC = faulting pc", last_expevt == 0x040 && last_tea == V_LAZY && last_spc == V_LAZY);
    check("refill with ldtlb and retry executed the page", r == 7 && faults == 1);

    r = ((fn0_t)V_LAZY)();
    check("mapped page runs without a new fault", r == 7 && faults == 1);

    /* Translation starts in a mapped page and probes into the next,
     * initially unmapped page. Retry must preserve the original entry. */
    {
        unsigned m0 = misses, f0 = faults;
        volatile unsigned short *tail = (volatile unsigned short *)(P_DATA + 0xffc);
        tail[0] = 0x0009; tail[1] = 0x0009;
        mmio_w32(MMUCR, mmio_r32(MMUCR) | 4);
        hc_flush(V_DATA + 0xffc, 10);
        tlb_map(V_DATA, P_DATA);
        r = ((fn0_t)(V_DATA + 0xffc))();
        check("cross-page code probe refills and retries", r == 7 && misses == m0 + 1 && faults == f0 + 1);
        check("cross-page probe preserves entry SPC and missing-page TEA",
              last_spc == V_DATA + 0xffc && last_tea == V_LAZY && last_expevt == 0x040);
    }

    /* rte into a mapped page that is neither in the TLB nor translated,
     * with BL=1 at the rte (the kernel's thread dispatch does this): the
     * target must be probed after the rte, in the new context, so that the
     * TLB miss reaches the handler instead of resetting the CPU */
    mmio_w32(MMUCR, mmio_r32(MMUCR) | 0x4);                  /* TI: drop the entries, keep AT */
    {
        unsigned inv0 = hc_getstat(2), tr0 = hc_getstat(0);
        hc_flush(V_LAZY, 0x1000);
        hc_puts("flush: inval+"); hc_puthex(hc_getstat(2) - inv0); hc_puts(" mmucr "); hc_puthex(mmio_r32(MMUCR)); hc_putc('\n');
        (void)tr0;
    }
    {
        unsigned r0v = 0, m0 = misses, f0 = faults, sr_after, target = V_LAZY, tr0 = hc_getstat(0);
        __asm__ volatile (
            "mova 1f, r0\n\tlds r0, pr\n\t"                    /* the page's rts comes back to 1: */
            "stc sr, r2\n\tmov.l 2f, r3\n\tor r3, r2\n\tldc r2, sr\n\t"   /* BL=1 */
            "ldc %2, spc\n\t"
            "not r3, r3\n\tand r3, r2\n\tldc r2, ssr\n\t"      /* SSR: BL=0 */
            "rte\n\tnop\n\t"
            ".align 2\n2:\t.long 0x10000000\n"
            "1:\tmov r0, %0\n\tstc sr, %1\n\t"           /* 4-aligned after the literal */
            : "=r"(r0v), "=r"(sr_after) : "r"(target) : "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "pr", "memory");
        hc_puts("rte test: r0 "); hc_puthex(r0v); hc_puts(" misses+"); hc_puthex(misses - m0); hc_puts(" faults+"); hc_puthex(faults - f0);
        hc_puts(" sr "); hc_puthex(sr_after); hc_puts(" translations+"); hc_puthex(hc_getstat(0) - tr0); hc_putc('\n');
        check("rte with BL=1 into untranslated mapped code: miss handled after the rte",
              r0v == 7 && misses == m0 + 1 && faults == f0 + 1 && (sr_after & 0x10000000u) == 0);
    }

    if (hc_getstat(416)) {
        volatile unsigned value = 0;
        unsigned original = *(volatile unsigned *)P_LAZY, m0, address;
        hc_devrange((unsigned)r0_fault_load, (unsigned)r0_fault_end);
        mmio_w32(MMUCR, mmio_r32(MMUCR) | 4);
        m0 = misses;
        address = r0_fault_load(V_LAZY, &value);
        check("R0 postincrement TLB miss retries exactly once", address == V_LAZY+4 && value == original && misses == m0+1 && last_spc == (unsigned)r0_fault_load_site);
        mmio_w32(MMUCR, mmio_r32(MMUCR) | 4);
        m0 = misses;
        address = r0_fault_store(V_LAZY+4, 0x12345678u);
        check("R0 predecrement TLB miss retries exactly once", address == V_LAZY && *(volatile unsigned *)P_LAZY == 0x12345678u && misses == m0+1 && last_spc == (unsigned)r0_fault_store_site);
        hc_devrange(0,0);
    }

    mmio_w32(MMUCR, 0x4);                                    /* MMU off again, flush */
    check("MMU off", (mmio_r32(MMUCR) & 1) == 0);
    hc_stats();
    hc_puts(fails ? "RESULT: FAIL\n" : "RESULT: PASS\n");
    hc_exit(fails);
}
