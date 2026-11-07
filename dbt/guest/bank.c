/* bank.c - register bank test. The CE kernel keeps pCurThread, the context
 * save pointer and SH3CTL_BASE in r4/r5/r7 of bank 1 for the whole life of the
 * system; the translator's exception path, hypercalls, lookups and helpers
 * must therefore never clobber bank 1 (nor bank 0 when the guest is in bank 1).
 *
 * bank_log[]: 0 entries, 1 entries with r4/r5/r7_bank1 intact, 2 EXPEVT,
 * 3 SPC, 4..6 bank-0 r0/r3/r6 at entry, 7 entries whose bank-1 hypercall left
 * both banks intact. */
#include "hyper.h"

extern char guest_vectors[];
unsigned bank_log[8];
static int fails;

#define MAGIC4 0x44444401u
#define MAGIC5 0x55555502u
#define MAGIC7 0xFF000000u

static void check(const char *name, int ok)
{
    hc_puts(ok ? "PASS " : "FAIL "); hc_puts(name); hc_putc('\n');
    if (!ok) fails++;
}

static void set_bank1(unsigned v4, unsigned v5, unsigned v7)
{
    __asm__ volatile ("ldc %0, r4_bank" :: "r"(v4));
    __asm__ volatile ("ldc %0, r5_bank" :: "r"(v5));
    __asm__ volatile ("ldc %0, r7_bank" :: "r"(v7));
}

static int bank1_intact(void)
{
    unsigned v4, v5, v7;
    __asm__ volatile ("stc r4_bank, %0" : "=r"(v4));
    __asm__ volatile ("stc r5_bank, %0" : "=r"(v5));
    __asm__ volatile ("stc r7_bank, %0" : "=r"(v7));
    return v4 == MAGIC4 && v5 == MAGIC5 && v7 == MAGIC7;
}

static unsigned fib(unsigned n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

void guest_main(void)
{
    unsigned r0v, r3v, r6v, t;

    hc_puts("bank test: bank-1 state across the translator\n");
    __asm__ volatile ("ldc %0, vbr" :: "r"(guest_vectors));
    set_bank1(MAGIC4, MAGIC5, MAGIC7);
    check("ldc/stc rN_bank round trip", bank1_intact());

    /* hypercalls from bank 0 go through the exception path */
    hc_puts("hypercall from bank 0\n");
    check("bank 1 intact after hypercalls", bank1_intact());

    /* fresh code: lookups and translation with bank 1 live */
    t = fib(12);
    check("bank 1 intact after translating new code", t == 144 && bank1_intact());

    /* guest exception: handler runs in bank 1 and checks its registers */
    __asm__ volatile ("mov #0x11, r0\n\tmov #0x33, r3\n\tmov #0x66, r6\n\ttrapa #5\n\tmov r0, %0\n\tmov r3, %1\n\tmov r6, %2"
                      : "=r"(r0v), "=r"(r3v), "=r"(r6v) :: "r0", "r3", "r6", "memory");
    check("trapa reached the handler", bank_log[0] == 1 && bank_log[2] == 0x160);
    check("handler saw r4/r5/r7_bank1 intact", bank_log[1] == 1);
    check("handler saw the interrupted bank-0 registers", bank_log[4] == 0x11 && bank_log[5] == 0x33 && bank_log[6] == 0x66);
    check("bank-1 hypercall (RB=1, BL=0) kept both banks", bank_log[7] == 1);
    check("bank-0 registers back after rte", r0v == 0x11 && r3v == 0x33 && r6v == 0x66);
    check("bank 1 intact after the exception", bank1_intact());

    /* synthetic address error (API-call convention): odd target, back via PR */
    {
        unsigned ret = 0;
        __asm__ volatile ("mov.l 1f, r1\n\tjsr @r1\n\tnop\n\tmov #1, %0\n\tbra 2f\n\tnop\n\t.align 2\n1:\t.long 0xFFFFFE01\n2:\n\t"
                          : "=r"(ret) :: "r1", "pr", "memory");
        check("address error delivered (EXPEVT 0xE0, odd SPC)", bank_log[0] == 2 && bank_log[2] == 0xE0 && bank_log[3] == 0xFFFFFE01);
        check("returned through PR", ret == 1);
        check("handler saw bank 1 intact again", bank_log[1] == 2 && bank_log[7] == 2);
        check("bank 1 intact after the address error", bank1_intact());
    }

    /* the forms the trampoline uses: ldc.l @rm+,spc / ssr (no bank switch involved) */
    {
        static unsigned mem[2] = { 0x8C1BEEF4u, 0x400010F2u };
        unsigned a, b, p = (unsigned)mem;
        __asm__ volatile ("ldc.l @%2+, spc\n\tldc.l @%2+, ssr\n\tstc spc, %0\n\tstc ssr, %1"
                          : "=r"(a), "=r"(b), "+r"(p) :: "memory");
        check("ldc.l @rm+,spc/ssr load and post-increment", a == 0x8C1BEEF4u && b == 0x400010F2u && p == (unsigned)mem + 8);
    }

    /* ldc sr that switches banks (the kernel's PR_B1_BK / PR_B0_IE loads):
     * the old bank keeps its r0-r7, the new bank shows its own values, and
     * coming back finds everything as it was */
    {
        static unsigned res[12];
        register unsigned *resp __asm__("r8") = res;      /* r8: the same register in both banks */
        __asm__ volatile (
            "mov #0x21, r0\n\tmov #0x22, r1\n\tmov #0x23, r2\n\tmov #0x24, r3\n\tmov #0x26, r6\n\t"
            "stc sr, r9\n\tmov.l 1f, r10\n\tor r9, r10\n\t"
            "mov.l 3f, r11\n\tldc r11, spc\n\tmov.l 4f, r11\n\tldc r11, ssr\n\t"   /* the kernel saves these after its switch */
            "stc spc, r11\n\tmov.l r11, @(40, %0)\n\tstc ssr, r11\n\tmov.l r11, @(44, %0)\n\t"
            "ldc r10, sr\n\t"             /* RB=1, BL=1 */
            "mov r4, r11\n\tmov r5, r12\n\tmov r7, r13\n\t"
            "stc spc, r9\n\tmov.l r9, @(32, %0)\n\tstc ssr, r9\n\tmov.l r9, @(36, %0)\n\t"
            "mov.l 5f, r9\n\t"
            "ldc r9, sr\n\t"              /* back to bank 0 */
            "mov.l r0, @(0, %0)\n\tmov.l r1, @(4, %0)\n\tmov.l r2, @(8, %0)\n\tmov.l r3, @(12, %0)\n\tmov.l r6, @(16, %0)\n\t"
            "mov.l r11, @(20, %0)\n\tmov.l r12, @(24, %0)\n\tmov.l r13, @(28, %0)\n\t"
            "bra 2f\n\tnop\n\t.align 2\n1:\t.long 0x30000000\n3:\t.long 0x8C1BEEF0\n4:\t.long 0x400010F1\n5:\t.long 0x400010F0\n2:\n\t"
            :: "r"(resp) : "r0", "r1", "r2", "r3", "r6", "r9", "r10", "r11", "r12", "r13", "memory");
        check("ldc sr into bank 1 shows the parked r4/r5/r7", res[5] == MAGIC4 && res[6] == MAGIC5 && res[7] == MAGIC7);
        check("ldc sr back to bank 0 finds r0-r3/r6 intact", res[0] == 0x21 && res[1] == 0x22 && res[2] == 0x23 && res[3] == 0x24 && res[4] == 0x26);
        check("SPC/SSR survive the bank switch (read in bank 1)", res[8] == 0x8C1BEEF0u && res[9] == 0x400010F1u);
        check("bank 1 intact after the bank switches", bank1_intact());
    }

    /* Guest MD is shadowed even though the host stays privileged. Both
     * transitions must update shadow MD; they leave the same-mode SR fast
     * path unless it handles MD itself (DBT_FAST_LDC_SR_MD). */
    if (hc_getstat(63) & 1) {
        unsigned old, user, restored, fast = hc_getstat(31);
        __asm__ volatile (
            "stc sr, r0\n\tmov.l 1f, r1\n\txor r0, r1\n\t"
            "ldc r1, sr\n\tstc sr, %1\n\tldc r0, sr\n\t"
            "stc sr, %2\n\tmov r0, %0\n\tbra 2f\n\tnop\n\t"
            ".align 2\n1:\t.long 0x40000000\n2:\n"
            : "=r"(old), "=r"(user), "=r"(restored) :: "r0", "r1", "memory");
        check("SR privilege transitions update shadow MD", user == (old ^ 0x40000000u) && restored == old);
        if (hc_getstat(63) & 2)         /* DBT_FAST_LDC_SR_MD: the shadow MD is updated on the fast path */
            check("SR privilege transitions stay on the fast path", hc_getstat(31) == fast + 2);
        else
            check("SR privilege transitions take the full helper", hc_getstat(31) == fast);
    }

    /* many exceptions: nothing drifts */
    {
        int i;
        for (i = 0; i < 50; i++) __asm__ volatile ("trapa #6" ::: "memory");
        check("50 more exceptions", bank_log[0] == 52 && bank_log[1] == 52 && bank_log[7] == 52 && bank1_intact());
    }

    hc_stats();
    hc_puts(fails ? "RESULT: FAIL\n" : "RESULT: PASS\n");
    hc_exit(fails);
}
