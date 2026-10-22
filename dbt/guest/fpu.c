/* fpu.c - guest FPU test. The guest is built without FPU support, so the FPU
 * instructions are emitted as raw words; under the translator each becomes a
 * call into the softfloat helpers. */
#include "hyper.h"

static int fails;
static void check(const char *name, int ok)
{
    hc_puts(ok ? "PASS " : "FAIL "); hc_puts(name); hc_putc('\n');
    if (!ok) fails++;
}

/* r0 = FPSCR after writing v (lds r0,fpscr ; sts fpscr,r0) */
static unsigned set_fpscr(unsigned v)
{
    register unsigned r0 __asm__("r0") = v;
    __asm__ volatile (".word 0x406A\n\t.word 0x006A" : "+r"(r0) :: "memory");   /* lds r0,fpscr ; sts fpscr,r0 */
    return r0;
}

/* single: 1.0 + 1.0 -> ftrc -> 2 */
static unsigned test_add_single(void)
{
    register unsigned r0 __asm__("r0");
    __asm__ volatile (
        ".word 0xF09D\n\t"      /* fldi1 fr0 */
        ".word 0xF19D\n\t"      /* fldi1 fr1 */
        ".word 0xF100\n\t"      /* fadd fr0,fr1 */
        ".word 0xF13D\n\t"      /* ftrc fr1,fpul */
        ".word 0x005A"          /* sts fpul,r0 */
        : "=r"(r0) :: "memory");
    return r0;
}

/* float 7 -> fr2 ; fmul fr2,fr2 -> 49 -> ftrc */
static unsigned test_mul_single(void)
{
    register unsigned r0 __asm__("r0") = 7;
    __asm__ volatile (
        ".word 0x405A\n\t"      /* lds r0,fpul */
        ".word 0xF22D\n\t"      /* float fpul,fr2 */
        ".word 0xF222\n\t"      /* fmul fr2,fr2 */
        ".word 0xF23D\n\t"      /* ftrc fr2,fpul */
        ".word 0x005A"          /* sts fpul,r0 */
        : "+r"(r0) :: "memory");
    return r0;
}

/* fmov.s to/from memory: store 1.0f, read back the bits */
static unsigned test_fmov_mem(unsigned *buf)
{
    register unsigned *r4 __asm__("r4") = buf;
    register unsigned r0 __asm__("r0");
    __asm__ volatile (
        ".word 0xF39D\n\t"      /* fldi1 fr3 */
        ".word 0xF43A\n\t"      /* fmov.s fr3,@r4 */
        ".word 0xF548\n\t"      /* fmov.s @r4,fr5 */
        ".word 0xF51D\n\t"      /* flds fr5,fpul */
        ".word 0x005A"          /* sts fpul,r0 */
        : "=r"(r0) : "r"(r4) : "memory");
    return r0;
}

/* double: PR=1, 3.0 - 0.5 = 2.5 -> ftrc -> 2 ; compare 3.0 > 0.5 sets T */
static unsigned test_double(unsigned *t_out)
{
    register unsigned r0 __asm__("r0") = 3;
    register unsigned r1 __asm__("r1");
    __asm__ volatile (
        ".word 0x405A\n\t"      /* lds r0,fpul */
        "mov.l 1f, r1\n\t"
        ".word 0x416A\n\t"      /* lds r1,fpscr  (PR=1) */
        ".word 0xF02D\n\t"      /* float fpul,dr0 */
        "mov #1, r1\n\t"
        ".word 0x415A\n\t"      /* lds r1,fpul */
        ".word 0xF22D\n\t"      /* float fpul,dr2  (1.0) */
        ".word 0xF42D\n\t"      /* float fpul,dr4  (1.0) */
        ".word 0xF240\n\t"      /* fadd dr4,dr2    (2.0) */
        ".word 0xF423\n\t"      /* fdiv dr2,dr4    dr4 = 1.0/2.0 = 0.5 */
        ".word 0xF041\n\t"      /* fsub dr4,dr0    3.0 - 0.5 = 2.5 */
        ".word 0xF045\n\t"      /* fcmp/gt dr4,dr0 : 3? no: dr0 (2.5) > dr4 (0.5) -> T=1 */
        "movt r1\n\t"
        ".word 0xF03D\n\t"      /* ftrc dr0,fpul -> 2 */
        ".word 0x005A\n\t"      /* sts fpul,r0 */
        "bra 2f\n\t"
        "nop\n\t"
        ".align 2\n"
        "1: .long 0x00080000\n" /* FPSCR: PR=1, RN, DN=0 */
        "2:\n\t"
        : "+r"(r0), "=r"(r1) :: "memory", "t");
    *t_out = r1;
    return r0;
}

void guest_main(void)
{
    unsigned buf[2] = { 0, 0 };
    unsigned t = 0, v;

    hc_puts("fpu test\n");
    check("fpscr reset value", set_fpscr(0x00040001u) == 0x00040001u);
    set_fpscr(0x00040000u);                                  /* RN, DN=1, PR=0 */
    check("fadd single 1+1 -> 2", test_add_single() == 2);
    check("float/fmul single 7*7 -> 49", test_mul_single() == 49);
    v = test_fmov_mem(buf);
    check("fmov.s store/load 1.0f", v == 0x3F800000u && buf[0] == 0x3F800000u);
    set_fpscr(0x00040000u);
    v = test_double(&t);
    check("double 3.0-0.5 -> 2, fcmp/gt T=1", v == 2 && t == 1);
    hc_stats();
    hc_puts(fails ? "RESULT: FAIL\n" : "RESULT: PASS\n");
    hc_exit(fails);
}
