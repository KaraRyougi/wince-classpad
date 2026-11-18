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

/* The register-only moves the translator does in line (translate.c
 * tr_fpu_inline): fmov frm,frn, fldi0/1, flds/fsts, lds/sts fpscr/fpul,
 * with the FR bank switched by frchg, T kept across them, and the SZ=1
 * pair move left to the helper. */
static unsigned test_inline_moves(unsigned *out)
{
    register unsigned r0 __asm__("r0");
    register unsigned r1 __asm__("r1");
    register unsigned r2 __asm__("r2");
    register unsigned r3 __asm__("r3");
    __asm__ volatile (
        "mov.l 1f, r1\n\t"
        ".word 0x416A\n\t"      /* lds r1,fpscr : PR=0, SZ=0, FR=0 */
        ".word 0xF09D\n\t"      /* fldi1 fr0 */
        ".word 0xF18D\n\t"      /* fldi0 fr1 */
        "sett\n\t"
        ".word 0xF20C\n\t"      /* fmov fr0,fr2 (1.0) */
        "movt r2\n\t"           /* T must still be 1 */
        ".word 0xF21D\n\t"      /* flds fr2,fpul */
        ".word 0x005A\n\t"      /* sts fpul,r0 -> 0x3F800000 */
        ".word 0xFBFD\n\t"      /* frchg: bank 1 */
        ".word 0xF29D\n\t"      /* fldi1 fr2 (bank 1) */
        ".word 0xF28D\n\t"      /* fldi0 fr2 (bank 1): overwrites with 0 */
        ".word 0xF21D\n\t"      /* flds fr2,fpul -> 0 (bank 1) */
        ".word 0x015A\n\t"      /* sts fpul,r1 -> 0 */
        ".word 0xFBFD\n\t"      /* frchg: bank 0 again */
        ".word 0xF21D\n\t"      /* flds fr2,fpul -> 1.0 (bank 0 untouched) */
        ".word 0x035A\n\t"      /* sts fpul,r3 -> 0x3F800000 */
        "bra 2f\n\t"
        "nop\n\t"
        ".align 2\n"
        "1: .long 0x00040000\n"
        "2:\n\t"
        : "=r"(r0), "=r"(r1), "=r"(r2), "=r"(r3) :: "memory", "t");
    out[0] = r1; out[1] = r2; out[2] = r3;
    return r0;
}
/* SZ=1: fmov dr0,dr2 is a pair move (both words), through the helper */
static unsigned test_pair_move(unsigned *out)
{
    register unsigned r0 __asm__("r0");
    register unsigned r1 __asm__("r1");
    __asm__ volatile (
        "mov.l 1f, r1\n\t"
        ".word 0x416A\n\t"      /* lds r1,fpscr : SZ=0 */
        ".word 0xF09D\n\t"      /* fldi1 fr0 */
        ".word 0xF19D\n\t"      /* fldi1 fr1 */
        ".word 0xF28D\n\t"      /* fldi0 fr2 */
        ".word 0xF38D\n\t"      /* fldi0 fr3 */
        "mov.l 3f, r1\n\t"
        ".word 0x416A\n\t"      /* lds r1,fpscr : SZ=1 */
        ".word 0xF20C\n\t"      /* fmov dr0,dr2 (pair) */
        "mov.l 1f, r1\n\t"
        ".word 0x416A\n\t"      /* SZ=0 */
        ".word 0xF21D\n\t"      /* flds fr2,fpul */
        ".word 0x005A\n\t"      /* sts fpul,r0 -> 1.0 */
        ".word 0xF31D\n\t"      /* flds fr3,fpul */
        ".word 0x015A\n\t"      /* sts fpul,r1 -> 1.0 */
        "bra 2f\n\t"
        "nop\n\t"
        ".align 2\n"
        "1: .long 0x00040000\n"
        "3: .long 0x00140000\n" /* SZ=1 */
        "2:\n\t"
        : "=r"(r0), "=r"(r1) :: "memory", "t");
    out[0] = r1;
    return r0;
}

/* fmov.s between memory and FR registers, the forms the translator does in
 * line (tr_fpu_inline kinds 10-15): r4 = buf[8]. out[] gets the pointers
 * after the post-increment and the pre-decrement. */
static unsigned test_fmov_forms(unsigned *buf, unsigned *out)
{
    register unsigned *r4 __asm__("r4") = buf;
    register unsigned r0 __asm__("r0");
    register unsigned r1 __asm__("r1");
    register unsigned r2 __asm__("r2");
    register unsigned r3 __asm__("r3");
    __asm__ volatile (
        "mov r4, r1\n\t"
        "sett\n\t"
        ".word 0xF119\n\t"      /* fmov.s @r1+,fr1   : buf[0] */
        ".word 0xF219\n\t"      /* fmov.s @r1+,fr2   : buf[1], r1 = buf+2 */
        "movt r3\n\t"           /* T kept: 1 */
        "mov r4, r2\n\t"
        "add #16, r2\n\t"
        ".word 0xF21B\n\t"      /* fmov.s fr1,@-r2   : buf[3] = buf[0], r2 = buf+3 */
        ".word 0xF22A\n\t"      /* fmov.s fr2,@r2    : buf[3] = buf[1] */
        "mov #20, r0\n\t"
        ".word 0xF417\n\t"      /* fmov.s fr1,@(r0,r4): buf[5] = buf[0] */
        "mov #4, r0\n\t"
        ".word 0xF346\n\t"      /* fmov.s @(r0,r4),fr3: buf[1] */
        ".word 0xFBFD\n\t"      /* frchg: bank 1 */
        ".word 0xF148\n\t"      /* fmov.s @r4,fr1 (bank 1): buf[0] */
        "mov #24, r0\n\t"
        ".word 0xF417\n\t"      /* fmov.s fr1,@(r0,r4): buf[6] = buf[0], from bank 1 */
        ".word 0xFBFD\n\t"      /* frchg: bank 0 */
        "mov #28, r0\n\t"
        ".word 0xF437\n\t"      /* fmov.s fr3,@(r0,r4): buf[7] = buf[1] */
        ".word 0xF31D\n\t"      /* flds fr3,fpul */
        ".word 0x005A"          /* sts fpul,r0 */
        : "=r"(r0), "=r"(r1), "=r"(r2), "=r"(r3) : "r"(r4) : "memory", "t");
    out[0] = r1; out[1] = r2; out[2] = r3;
    return r0;
}

/* SZ=1: the same opcodes move pairs, through the helper (the in-line
 * path's slow range). r4 = buf[4]: dr0 <- buf[0..1], post-increment by 8,
 * then dr0 -> @-r2. */
static unsigned test_fmov_pair_mem(unsigned *buf, unsigned *out)
{
    register unsigned *r4 __asm__("r4") = buf;
    register unsigned r0 __asm__("r0");
    register unsigned r1 __asm__("r1");
    register unsigned r2 __asm__("r2");
    __asm__ volatile (
        "mov.l 3f, r1\n\t"
        ".word 0x416A\n\t"      /* lds r1,fpscr : SZ=1 */
        "mov r4, r1\n\t"
        ".word 0xF019\n\t"      /* fmov @r1+,dr0 : buf[0], buf[1]; r1 = buf+2 */
        "mov r4, r2\n\t"
        "add #16, r2\n\t"
        ".word 0xF20B\n\t"      /* fmov dr0,@-r2 : buf[2], buf[3]; r2 = buf+2 */
        "mov.l 1f, r0\n\t"
        ".word 0x406A\n\t"      /* SZ=0 */
        ".word 0xF01D\n\t"      /* flds fr0,fpul */
        ".word 0x005A\n\t"      /* sts fpul,r0 */
        "bra 2f\n\t"
        "nop\n\t"
        ".align 2\n"
        "1: .long 0x00040000\n"
        "3: .long 0x00140000\n"
        "2:\n\t"
        : "=r"(r0), "=r"(r1), "=r"(r2) : "r"(r4) : "memory", "t");
    out[0] = r1; out[1] = r2;
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
    {
        unsigned o[3] = { 0, 0, 0 };
        v = test_inline_moves(o);
        check("fmov/flds/fldi in line, banks, T kept", v == 0x3F800000u && o[0] == 0 && o[1] == 1 && o[2] == 0x3F800000u);
        v = test_pair_move(o);
        check("fmov pair with SZ=1", v == 0x3F800000u && o[0] == 0x3F800000u);
        {
            unsigned b[8] = { 0x40490FDBu, 0xC0000000u, 0, 0, 0, 0, 0, 0 }, p[3] = { 0, 0, 0 };
            set_fpscr(0x00040000u);
            v = test_fmov_forms(b, p);
            check("fmov.s @rm+ twice: pointer +8, T kept", p[0] == (unsigned)(b + 2) && p[2] == 1);
            check("fmov.s frm,@-rn then frm,@rn", p[1] == (unsigned)(b + 3) && b[3] == 0xC0000000u);
            check("fmov.s indexed store and load", b[5] == 0x40490FDBu && v == 0xC0000000u && b[7] == 0xC0000000u);
            check("fmov.s memory forms in bank 1", b[6] == 0x40490FDBu);
            check("fmov.s left the other words alone", b[0] == 0x40490FDBu && b[1] == 0xC0000000u && b[2] == 0 && b[4] == 0);
        }
        {
            unsigned b[4] = { 0x12345678u, 0x9ABCDEF0u, 0, 0 }, p[2] = { 0, 0 };
            v = test_fmov_pair_mem(b, p);
            check("fmov pair from/to memory with SZ=1 (helper)", v == 0x12345678u && p[0] == (unsigned)(b + 2) && p[1] == (unsigned)(b + 2)
                  && b[2] == 0x12345678u && b[3] == 0x9ABCDEF0u);
        }
        check("lds fpscr masks to 22 bits", set_fpscr(0xFFFFFFFFu) == 0x003FFFFFu);
        set_fpscr(0x00040000u);
    }
    hc_stats();
    hc_puts(fails ? "RESULT: FAIL\n" : "RESULT: PASS\n");
    hc_exit(fails);
}
