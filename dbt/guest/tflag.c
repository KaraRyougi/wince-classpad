/* T must survive indirect transfer, on cold and warm IC paths. */
#include "hyper.h"
extern unsigned call_with_t(unsigned (*fn)(void), unsigned t);
extern unsigned call_with_t_clobber(unsigned (*fn)(void), unsigned t);
extern unsigned read_t_a(void), read_t_b(void);
__asm__(".text\n.align 2\n.global _call_with_t\n_call_with_t:\n"
        "sts.l pr,@-r15\n mov r4,r1\n mov r5,r0\n cmp/eq #1,r0\n"
        "jsr @r1\n nop\n movt r0\n lds.l @r15+,pr\n rts\n nop\n"
        ".global _call_with_t_clobber\n_call_with_t_clobber:\n"
        "sts.l pr,@-r15\n mov r4,r1\n mov r5,r0\n cmp/eq #1,r0\n"
        "jsr @r1\n mov #0,r1\n movt r0\n lds.l @r15+,pr\n rts\n nop\n"
        ".global _read_t_a\n_read_t_a:\n movt r0\n rts\n nop\n"
        ".global _read_t_b\n_read_t_b:\n movt r0\n rts\n nop\n");
static int fails;
static void check(const char *name, int ok)
{
    hc_puts(ok ? "PASS " : "FAIL "); hc_puts(name); hc_putc('\n');
    if (!ok) ++fails;
}
void guest_main(void)
{
    check("cold indirect call preserves T=1", call_with_t(read_t_a, 1) == 1);
    check("warm indirect call preserves T=0", call_with_t(read_t_a, 0) == 0);
    check("cold indirect call preserves T=0", call_with_t(read_t_b, 0) == 0);
    check("warm indirect call preserves T=1", call_with_t(read_t_b, 1) == 1);
    check("X0 target cold call preserves T=1", call_with_t_clobber(read_t_a, 1) == 1);
    check("X0 target warm call preserves T=0", call_with_t_clobber(read_t_a, 0) == 0);
    check("X0 target cold call preserves T=0", call_with_t_clobber(read_t_b, 0) == 0);
    check("X0 target warm call preserves T=1", call_with_t_clobber(read_t_b, 1) == 1);
    hc_exit(fails);
}
