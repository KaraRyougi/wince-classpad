/* apicall.c - jumps to unfetchable addresses, the way Windows CE calls its
 * kernel on SH (jsr to 0xF000xxxx), must reach the guest's general-exception
 * handler as an instruction address error with SPC = TEA = the target, and
 * the handler must be able to redirect execution (here: back to the caller
 * through PR, like the kernel's API return). */
#include "hyper.h"
#include "mmio.h"

extern char guest_vectors[];
volatile unsigned fault_pr;
static int fails;
static volatile unsigned faults, last_vec, last_spc, last_expevt, last_tea, last_pr;

static void check(const char *name, int ok)
{
    hc_puts(ok ? "PASS " : "FAIL "); hc_puts(name); hc_putc('\n');
    if (!ok) fails++;
}

unsigned guest_fault(unsigned vec, unsigned spc, unsigned expevt, unsigned tea)
{
    faults++;
    last_vec = vec; last_spc = spc; last_expevt = expevt; last_tea = tea; last_pr = fault_pr;
    return fault_pr;                        /* "API call" done: return to the caller */
}

/* jsr to `target` with r4..r7 loaded; returns r4+r5+r6+r7 as seen after the
 * call (all must survive the exception) */
static unsigned api_jsr(unsigned target)
{
    unsigned sum;
    __asm__ volatile (
        "mov    #1, r4\n\t"
        "mov    #2, r5\n\t"
        "mov    #3, r6\n\t"
        "mov    #4, r7\n\t"
        "jsr    @%1\n\t"
        "nop\n\t"
        "add    r5, r4\n\t"
        "add    r6, r4\n\t"
        "add    r7, r4\n\t"
        "mov    r4, %0\n\t"
        : "=r"(sum) : "r"(target) : "r4", "r5", "r6", "r7", "pr", "memory");
    return sum;
}

void guest_main(void)
{
    unsigned sum, before;
    hc_puts("apicall test: faults from jumps to unfetchable addresses\n");
    __asm__ volatile ("ldc %0, vbr" :: "r"(guest_vectors));

    sum = api_jsr(0xF0010000u);
    check("jsr 0xF0010000 reached the general exception handler", faults == 1 && last_vec == 1);
    check("EXPEVT = 0x0E0 (instruction address error)", last_expevt == 0x0E0);
    check("SPC = TEA = target", last_spc == 0xF0010000u && last_tea == 0xF0010000u);
    check("handler redirected to the caller, r4-r7 intact", sum == 10);

    sum = api_jsr(0xF0010000u);
    check("same call site again (inline cache miss path)", faults == 2 && sum == 10 && last_spc == 0xF0010000u);

    before = faults;
    sum = api_jsr(0x8C100001u);
    check("odd target address faults too", faults == before + 1 && last_expevt == 0x0E0 && last_tea == 0x8C100001u && sum == 10);

    sum = api_jsr(0xF000FFFCu);
    check("another API address", faults == before + 2 && last_spc == 0xF000FFFCu && sum == 10);

    hc_puts("faults: "); hc_puthex(faults); hc_putc('\n');
    hc_stats();
    hc_puts(fails ? "RESULT: FAIL\n" : "RESULT: PASS\n");
    hc_exit(fails);
}
