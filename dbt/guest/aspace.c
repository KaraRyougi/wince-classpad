/* aspace.c - translated code and address-space changes: the same virtual
 * page meaning different code in two ASIDs, a shared page reused across
 * them, a remap inside one process announced by the kernel (HC_TLB_INVAL)
 * or noticed at ldtlb, and a chained direct branch into a page that gets
 * remapped. Mirrors what Windows CE's process slots and loader do. */
#include "hyper.h"
#include "mmio.h"

#define PTEH   0xFF000000u
#define PTEL   0xFF000004u
#define MMUCR  0xFF000010u

#define V_CODE  0x00400000u
#define V_SH    0x00410000u
#define V_LAZY  0x00420000u
#define V_LAZY2 0x00430000u
#define V_X     0x00440000u            /* two pages: a bra from the first into the second */

#define P_A  0x8C300000u               /* returns 1 */
#define P_B  0x8C301000u               /* returns 2 */
#define P_C1 0x8C302000u               /* returns 3 */
#define P_C2 0x8C303000u               /* returns 4 */
#define P_S  0x8C304000u               /* returns 9 */
#define P_X  0x8C305000u               /* bra to the next page */

extern char guest_vectors[];
volatile unsigned fault_pr;
static int fails;
typedef unsigned (*fn_t)(void);
extern unsigned call_user_with_t(fn_t f, unsigned t);
__asm__(".text\n.align 2\n.global _call_user_with_t\n_call_user_with_t:\n"
        "sts.l pr,@-r15\n mov r4,r1\n mov r5,r0\n cmp/eq #1,r0\n"
        "jsr @r1\n nop\n movt r0\n lds.l @r15+,pr\n rts\n nop\n");

static void check(const char *name, int ok)
{
    hc_puts(ok ? "PASS " : "FAIL "); hc_puts(name); hc_putc('\n');
    if (!ok) fails++;
}
static unsigned get_sr(void) { unsigned v; __asm__ volatile ("stc sr, %0" : "=r"(v)); return v; }
static void set_sr(unsigned v) { __asm__ volatile ("ldc %0, sr" :: "r"(v) : "memory"); }

unsigned guest_fault(unsigned vec, unsigned spc, unsigned expevt, unsigned tea)
{
    hc_puts("unexpected fault vec "); hc_puthex(vec); hc_puts(" expevt "); hc_puthex(expevt);
    hc_puts(" tea "); hc_puthex(tea); hc_puts(" spc "); hc_puthex(spc); hc_putc('\n');
    hc_exit(3);
    return 0;
}

static void tlb_map(unsigned v, unsigned p, unsigned asid, int shared)
{
    mmio_w32(PTEH, (v & 0xFFFFFC00u) | asid);
    mmio_w32(PTEL, (p & 0x1FFFFC00u) | 0x16Cu | (shared ? 2u : 0u));    /* V | PR=11 | C | D, 4 KiB */
    __asm__ volatile ("ldtlb" ::: "memory");
}

/* the kernel's way: write the ASID, then any SR write / rte lets the translator notice */
static void set_asid_stock(unsigned asid) { mmio_w32(PTEH, asid); set_sr(get_sr()); }
/* the paravirtual way */
static void set_asid_pv(unsigned asid) { mmio_w32(PTEH, asid); hc_asid(asid); }

static unsigned __attribute__((noinline)) alternate_user_targets(unsigned count)
{
    unsigned i, sum = 0;
    for (i = 0; i < count; ++i)
        sum += ((fn_t)(V_CODE + ((i & 1) ? 0x20 : 0)))();
    return sum;
}

/* call f and report how many translations it caused. The first use also
 * translates this function's own blocks; later uses only see f's. */
static unsigned __attribute__((noinline)) call_counted(fn_t f, unsigned *ntrans)
{
    unsigned a = hc_getstat(0), r = f();
    *ntrans = hc_getstat(0) - a;
    return r;
}

static void put_ret(unsigned page, unsigned n)   /* mov #n,r0 ; rts ; nop */
{
    volatile unsigned short *c = (volatile unsigned short *)page;
    c[0] = (unsigned short)(0xE000 | n); c[1] = 0x000B; c[2] = 0x0009;
}

void guest_main(void)
{
    unsigned r, n, n2;
    hc_puts("aspace test: translations across ASIDs and remaps\n");
    __asm__ volatile ("ldc %0, vbr" :: "r"(guest_vectors));

    put_ret(P_A, 1); put_ret(P_B, 2); put_ret(P_C1, 3); put_ret(P_C2, 4); put_ret(P_S, 9);
    { volatile unsigned short *c = (volatile unsigned short *)P_X; c[0] = 0xA7FE; c[1] = 0x0009; }   /* bra +0xFFC ; nop */
    hc_flush(P_A, 0x6000);

    mmio_w32(MMUCR, 0x4); mmio_w32(MMUCR, 0x1);          /* flush, AT=1 */

    /* 1. same virtual page, two processes */
    set_asid_stock(1);
    tlb_map(V_CODE, P_A, 1, 0);
    r = call_counted((fn_t)V_CODE, &n);                   /* also translates call_counted itself */
    check("ASID 1: V_CODE -> page A", r == 1);
    if (hc_getstat(30)) {
        unsigned before;
        put_ret(P_A + 0x20, 5);
        hc_flush(V_CODE + 0x20, 8);
        check("alternate user targets warmed", alternate_user_targets(16) == 48);
        before = hc_getstat(4);
        r = alternate_user_targets(4096);
        n2 = hc_getstat(4) - before;
        check("alternating user hash hits return correct values", r == 12288);
        /* A monomorphic IC would otherwise enter C thousands of times.
         * Allow a few cold surrounding kernel returns, not per-call misses. */
        check("user hash hits bypass C IC refill", n2 < 16);
        check("user hash target preserves T=1", call_user_with_t((fn_t)V_CODE, 1) == 1);
        check("alternate user hash target preserves T=0", call_user_with_t((fn_t)(V_CODE + 0x20), 0) == 0);
        check("user hash target preserves T=0", call_user_with_t((fn_t)V_CODE, 0) == 0);
        check("alternate user hash target preserves T=1", call_user_with_t((fn_t)(V_CODE + 0x20), 1) == 1);
    }
    set_asid_stock(2);
    tlb_map(V_CODE, P_B, 2, 0);
    r = call_counted((fn_t)V_CODE, &n);
    check("ASID 2: same V_CODE -> page B (no stale translation)", r == 2 && n >= 1);
    set_asid_pv(1);
    r = call_counted((fn_t)V_CODE, &n);
    check("back to ASID 1: page A again", r == 1);
    check("...its block was reused, not retranslated", n == 0);
    set_asid_pv(2);
    r = call_counted((fn_t)V_CODE, &n);
    check("ASID 2 again: page B, reused", r == 2 && n == 0);

    n = hc_getstat(3);
    hc_asid(2); hc_asid(2);
    check("same ASID notification does not switch", hc_getstat(3) == n);

    /* Repeated switches exercise sparse hash tracking and IC resets at the
     * same indirect-call site, including reuse of already translated code. */
    { unsigned i;
      for (i = 0; i < 64; ++i) {
        set_asid_pv(1); r = call_counted((fn_t)V_CODE, &n);
        set_asid_pv(2); r += call_counted((fn_t)V_CODE, &n2);
        if (r != 3 || n != 0 || n2 != 0) break;
      }
      check("64 ASID round trips reuse code without stale ICs", i == 64);
    }

    /* 2. a shared page (SH=1) keeps one translation across ASIDs */
    tlb_map(V_SH, P_S, 2, 1);
    r = call_counted((fn_t)V_SH, &n);
    set_asid_stock(1);
    r += call_counted((fn_t)V_SH, &n2);
    check("shared page: same result in both ASIDs", r == 18 && n >= 1);
    check("...one translation served both", n2 == 0);

    /* 3. remap inside a process, announced by the kernel (loader): the
     * kernel drops the old TLB entry (here: TI flushes all), tells us, maps */
    tlb_map(V_LAZY, P_C1, 1, 0);
    r = call_counted((fn_t)V_LAZY, &n);
    check("V_LAZY -> page C1", r == 3);
    mmio_w32(MMUCR, 0x5);
    hc_tlb_inval(V_LAZY, 0x1000);
    tlb_map(V_LAZY, P_C2, 1, 0);
    r = call_counted((fn_t)V_LAZY, &n);
    check("V_LAZY -> page C2 after HC_TLB_INVAL", r == 4 && n >= 1);

    /* 4. remap noticed at ldtlb alone (no hypercall); a second entry for the
     * same VPN would be a TLB multiple hit, so the old one is flushed first */
    tlb_map(V_LAZY2, P_C1, 1, 0);
    r = call_counted((fn_t)V_LAZY2, &n);
    check("V_LAZY2 -> page C1", r == 3);
    mmio_w32(MMUCR, 0x5);
    tlb_map(V_LAZY2, P_C2, 1, 0);                        /* new physical page for a page with blocks */
    r = call_counted((fn_t)V_LAZY2, &n);
    check("V_LAZY2 -> page C2 after a bare ldtlb", r == 4 && n >= 1);
    mmio_w32(MMUCR, 0x5);
    n2 = hc_getstat(2);
    tlb_map(V_LAZY2, P_C2, 1, 0);                        /* same mapping again: nothing to drop */
    r = call_counted((fn_t)V_LAZY2, &n);
    check("re-loading the same mapping invalidates nothing", r == 4 && n == 0 && hc_getstat(2) == n2);

    /* 5. a chained direct branch into a page that gets remapped */
    tlb_map(V_X, P_X, 1, 0);
    tlb_map(V_X + 0x1000, P_C1, 1, 0);
    r = call_counted((fn_t)V_X, &n);
    r += call_counted((fn_t)V_X, &n);                    /* second call: the bra is chained now */
    check("bra across pages, chained", r == 6 && n == 0);
    mmio_w32(MMUCR, 0x5);
    tlb_map(V_X, P_X, 1, 0);
    tlb_map(V_X + 0x1000, P_C2, 1, 0);
    r = call_counted((fn_t)V_X, &n);
    check("second page remapped: chained bra unlinked and re-resolved", r == 4 && n >= 1);

    /* Reuse across ASIDs, then replace only the second ASID's mapping.
     * Do not use TI: that would hide missing invalidation with a full flush. */
    mmio_w32(MMUCR, 0x5);
    mmio_w32(MMUCR, 1);                /* URC=0: replace the same UTLB slot */
    tlb_map(V_CODE, P_A, 1, 0);
    r = call_counted((fn_t)V_CODE, &n);
    mmio_w32(MMUCR, 1);
    tlb_map(V_CODE, P_A, 2, 0);
    r += call_counted((fn_t)V_CODE, &n);
    check("same physical code reused by another ASID", r == 2 && n == 0);
    mmio_w32(MMUCR, 1);
    tlb_map(V_CODE, P_B, 2, 0);
    r = call_counted((fn_t)V_CODE, &n);
    check("reused code invalidated when second ASID remaps", r == 2 && n >= 1);

    if (hc_getstat(18)) {
        volatile unsigned short *a = (volatile unsigned short *)P_A;
        volatile unsigned short *b = (volatile unsigned short *)P_B;
        unsigned hits, cache_hits;
        mmio_w32(MMUCR, 0x4);
        a[0] = b[0] = 0x0009;
        a[1] = 0xe00b; b[1] = 0xe00d;
        a[2] = b[2] = 0x000b; a[3] = b[3] = 0x0009;
        hc_flush(0, 0);
        mmio_w32(MMUCR, 1); tlb_map(V_CODE, P_A, 1, 0);
        r = call_counted((fn_t)V_CODE, &n);
        hits = hc_getstat(17);
        r += call_counted((fn_t)(V_CODE + 2), &n);
        check("user interior entry reused", r == 22 && hc_getstat(17) > hits);
        mmio_w32(MMUCR, 1); tlb_map(V_CODE, P_B, 2, 0);
        r = call_counted((fn_t)(V_CODE + 2), &n);
        check("interior target is not reused across different ASIDs", r == 13);
        cache_hits = hc_getstat(20);
        mmio_w32(MMUCR, 1); tlb_map(V_CODE, P_A, 1, 0);
        r = call_counted((fn_t)(V_CODE + 2), &n);
        check("interior target returns to its original ASID", r == 11);
        if (hc_getstat(19)) check("cached interior entry reused after ASID return", hc_getstat(20) > cache_hits);
        mmio_w32(MMUCR, 1); tlb_map(V_CODE, P_B, 1, 0);
        r = call_counted((fn_t)(V_CODE + 2), &n);
        check("remap invalidates cached user interior target", r == 13);
    }

    mmio_w32(MMUCR, 0x4);
    hc_stats();
    hc_puts(fails ? "RESULT: FAIL\n" : "RESULT: PASS\n");
    hc_exit(fails);
}
