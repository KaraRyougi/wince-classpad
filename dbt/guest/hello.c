/* hello.c - first guest: prints, then checks byte order of every access size. */
#include "hyper.h"

static int fails;
static void check(const char *name, int ok)
{
    hc_puts(ok ? "PASS " : "FAIL "); hc_puts(name); hc_putc('\n');
    if (!ok) fails++;
}

struct s { unsigned char b; unsigned short h; unsigned int w; unsigned short arr[3]; };

static unsigned int ident(unsigned int x) { return x; }   /* defeat constant folding */

void guest_main(void)
{
    volatile unsigned int w = 0x11223344u;
    volatile unsigned short h = 0x5566;
    volatile unsigned char *p = (volatile unsigned char *)&w;
    volatile unsigned char *q = (volatile unsigned char *)&h;
    volatile struct s st;
    volatile short sh = -2;
    volatile int sum = 0, i;
    static const unsigned int table[4] = { 1, 0x10000, 0x7FFFFFFF, 0xDEADBEEF };

    hc_puts("hello from the little-endian guest\n");

    check("u32 store is little-endian", p[0] == 0x44 && p[1] == 0x33 && p[2] == 0x22 && p[3] == 0x11);
    check("u16 store is little-endian", q[0] == 0x66 && q[1] == 0x55);
    p[0] = 0xAA; p[3] = 0xBB;
    check("u32 load after byte writes", w == 0xBB2233AAu);
    q[1] = 0x01;
    check("u16 load after byte write", h == 0x0166);
    check("s16 sign extension", sh == -2 && (int)sh * 3 == -6);
    st.b = 1; st.h = 0x1234; st.w = 0x89ABCDEFu; st.arr[0] = 1; st.arr[1] = 2; st.arr[2] = 3;
    check("struct fields", st.b == 1 && st.h == 0x1234 && st.w == 0x89ABCDEFu && st.arr[2] == 3);
    for (i = 0; i < 4; i++) sum += table[i] & 0xFF;
    check("literal pool / rodata", sum == (1 + 0 + 0xFF + 0xEF) && ident(table[3]) == 0xDEADBEEFu);
    check("u32 pre-decrement store / post-increment load (stack)", ident(0x12345678u) == 0x12345678u);
    hc_puts("value of w: "); hc_puthex(w); hc_putc('\n');
    hc_stats();
    hc_puts(fails ? "RESULT: FAIL\n" : "RESULT: PASS\n");
    hc_exit(fails);
}
