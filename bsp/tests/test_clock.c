/* classpad_clock.h: field packing, frequencies, Xtune's stepping rules, the
 * wait tables and the order of the register writes in ClockApply. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct { unsigned address, value; int write; } Op;
static Op ops[64];
static unsigned count, cursor;
static void expect(int write, unsigned a, unsigned v) { assert(count < 64); ops[count].address = a; ops[count].value = v; ops[count++].write = write; }
static unsigned read32(unsigned a) { assert(cursor < count && !ops[cursor].write && ops[cursor].address == a); return ops[cursor++].value; }
static void write32(unsigned a, unsigned v) {
    if (!(cursor < count && ops[cursor].write && ops[cursor].address == a && ops[cursor].value == v)) {
        printf("unexpected write %08x = %08x at step %u\n", a, v, cursor); assert(0);
    }
    cursor++;
}
#define MMIO_R32 read32
#define MMIO_W32 write32
#include "../classpad50/SRC/INC/classpad_clock.h"
#define R(a, v) expect(0, a, v)
#define W(a, v) expect(1, a, v)

/* stock fx-CG50: FLL x900 halved, PLL x16, 1/2 1/4 1/4 1/8 */
static const ClasspadClockRegs stock = { 0x00004384, 0x0F011112, 0x000003C0, 0x000024D1 };

int main(void)
{
    ClasspadClockChain c, d;
    ClasspadClockRegs r;
    unsigned hz[6];
    int i;

    /* decode, frequencies, encode round trip */
    ClockDecode(&stock, &c);
    assert(c.fll == 900 && c.selxm == 1 && c.pll == 16);
    assert(c.div[0] == 2 && c.div[1] == 4 && c.div[2] == 4 && c.div[3] == 8);
    ClockHz(&c, 1, hz);
    assert(hz[0] == 14745600 && hz[1] == 235929600 && hz[2] == 117964800);
    assert(hz[3] == 58982400 && hz[4] == 58982400 && hz[5] == 29491200);
    ClockHz(&c, 0, hz);
    assert(hz[1] == 14745600);                      /* PLL off: no multiply */
    r = stock; r.frqcr |= 0x80000000u | 0x40000000u; /* KICK is dropped, the unowned bit kept */
    ClockEncode(&c, &r);
    assert(r.fllfrq == stock.fllfrq && r.frqcr == (stock.frqcr | 0x40000000u));
    assert(ClockValid(&stock, 1));

    /* Ptune F5: PLL x32, 1/2 1/4 1/4 1/16 */
    r.fllfrq = 0x00004384; r.frqcr = 0x1F001103;
    ClockDecode(&r, &c);
    ClockHz(&c, 1, hz);
    assert(hz[2] == 235929600 && hz[4] == 117964800 && hz[5] == 29491200);
    assert(ClockValid(&r, 1) && !ClockOverLimit(&c, g_cpclkLimit));

    /* validity: divider code, order, ratio, tick floor */
    r = stock; r.frqcr = 0x0F611112; assert(!ClockValid(&r, 1));   /* IFC code 6 */
    r = stock; r.frqcr = 0x0F201112; assert(!ClockValid(&r, 1));   /* Iphi slower than Sphi */
    r = stock; r.frqcr = 0x0F002222; assert(!ClockValid(&r, 1));   /* Sphi/Iphi ratio 4 > 2 */
    r = stock; r.frqcr = 0x00505505; assert(!ClockValid(&r, 1));   /* Pphi 230 kHz */
    r = stock; r.fllfrq = 0x00004100; assert(!ClockValid(&r, 1));  /* FLL x256 */

    /* PLL up from stock: Pphi follows at x28 (51.6 MHz > 50) by going to 1/16 */
    ClockDecode(&stock, &c);
    for (i = 17; i <= 27; i++) { assert(ClockStep(&c, 1, 1, 0, g_cpclkLimit) == 0); assert(c.pll == (unsigned)i && c.div[3] == 8); }
    assert(ClockStep(&c, 1, 1, 0, g_cpclkLimit) == 0 && c.pll == 28 && c.div[3] == 16);
    /* ... and Bphi stops the multiplier at x36: 36 * 14.7456 / 4 = 132.7, x37 = 136.4 > 134 */
    while (ClockStep(&c, 1, 1, 0, g_cpclkLimit) == 0) { }
    assert(c.pll == 36 && ClockStep(&c, 1, 1, 0, g_cpclkLimit) == 4);
    d = c; assert(ClockStep(&d, 1, 1, 0, NULL) == 0 && d.pll == 37);          /* limits off */
    /* down again: Pphi returns to 1/8 once 2 * Pphi fits, never past Bphi */
    while (c.pll > 16) assert(ClockStep(&c, 1, -1, 0, g_cpclkLimit) == 0);
    assert(c.div[3] == 8);
    while (ClockStep(&c, 1, -1, 0, g_cpclkLimit) == 0) { }
    assert(c.pll == 1 && c.div[3] == 4 && c.div[2] == 4);

    /* FLL: range ends and the big step */
    ClockDecode(&stock, &c);
    assert(ClockStep(&c, 0, 1, 1, g_cpclkLimit) == 0 && c.fll == 950);
    c.fll = 2040; assert(ClockStep(&c, 0, 1, 1, NULL) == 0 && c.fll == 2047 && ClockStep(&c, 0, 1, 0, NULL) == -1);
    c.fll = 460;  assert(ClockStep(&c, 0, -1, 1, NULL) == 0 && c.fll == 450 && ClockStep(&c, 0, -1, 0, NULL) == -1);

    /* dividers: equal neighbours follow, the ratio table drags the rest */
    ClockDecode(&stock, &c);                                    /* 2 4 4 8 */
    assert(ClockStep(&c, 5, 1, 0, g_cpclkLimit) == 5);          /* Pphi 1/4 = 59 MHz > 50 */
    assert(ClockStep(&c, 5, 1, 0, NULL) == 0 && c.div[3] == 4 && c.div[2] == 4);
    assert(ClockStep(&c, 4, -1, 0, NULL) == 0);                 /* Bphi 1/8 takes the equal Pphi along */
    assert(c.div[0] == 2 && c.div[1] == 4 && c.div[2] == 8 && c.div[3] == 8);
    assert(ClockStep(&c, 4, -1, 0, NULL) == 0);                 /* Bphi 1/16: B/S = 4 ok, B/I = 8 ok */
    assert(c.div[0] == 2 && c.div[1] == 4 && c.div[2] == 16 && c.div[3] == 16);
    assert(ClockStep(&c, 4, -1, 0, NULL) == 0);                 /* Bphi 1/32 drags Sphi and Iphi */
    assert(c.div[0] == 4 && c.div[1] == 8 && c.div[2] == 32 && c.div[3] == 32);
    assert(ClockStep(&c, 0 + 2, 1, 0, NULL) == 0);              /* Iphi 1/2 pulls Sphi (4), Bphi (16), Pphi (32) */
    assert(c.div[0] == 2 && c.div[1] == 4 && c.div[2] == 16 && c.div[3] == 32);
    assert(ClockStep(&c, 2, 1, 0, NULL) == -1);                 /* already 1/2 */
    c.div[0] = c.div[1] = c.div[2] = c.div[3] = 64;
    assert(ClockStep(&c, 5, -1, 0, NULL) == -1);
    ClockDecode(&stock, &c);
    assert(ClockStep(&c, 3, 1, 0, g_cpclkLimit) == 0 && c.div[1] == 2 && c.div[0] == 2);   /* Sphi 118 MHz */
    assert(ClockStep(&c, 4, 1, 0, g_cpclkLimit) == 0 && c.div[2] == 2);                    /* Bphi 118 MHz */
    /* every chain the stepper produces is one the OAL accepts */
    r = stock; ClockEncode(&c, &r); assert(ClockValid(&r, 1));

    /* wait tables (5 % margin) */
    assert(ClockBestRomWR(58982400) == 6 && ClockWRCycles(6) == 6);     /* 62 * 0.95 = 58.9 MHz: just misses */
    assert(ClockBestRomWR(58900000) == 5 && ClockBestRomWR(50000000) == 4);
    assert(ClockBestRomWR(117964800) == 9 && ClockWRCycles(9) == 12);
    assert(ClockBestRomWR(130000000) == 10 && ClockWRCycles(10) == 14);
    assert(ClockBestTRC(58982400) == 0 && ClockBestTRC(88473600) == 1 && ClockTRCCycles(1) == 4);
    assert(ClockBestTRC(117964800) == 2 && ClockTRCCycles(2) == 6 && ClockBestTRC(125000000) == 3);
    r = stock; ClockAutoWait(&r, 117964800);
    assert(r.cs0wcr == 0x000004C0 && r.cs3wcr == 0x000024D2);          /* = Ptune F5's CS0WCR and CS3WCR */

    /* register sequence: safe waits, FLL, FRQCR with KICK, settle, final waits */
    r.fllfrq = 0x00004384; r.frqcr = 0x1F001103; r.cs0wcr = 0xFFFFFCFF; r.cs3wcr = 0xFFFFFFFE;
    R(CPCLK_CS0WCR, 0x000003C0); R(CPCLK_CS3WCR, 0x000024D1);
    W(CPCLK_CS0WCR, 0x000005C0); W(CPCLK_CS3WCR, 0x000024D3);
    R(CPCLK_FLLFRQ, 0x00004384); W(CPCLK_FLLFRQ, 0x00004384);
    R(CPCLK_FRQCR, 0x0F011112);  W(CPCLK_FRQCR, 0x9F011113);       /* bits 19:16 and 7:4 are not ours: kept */
    R(CPCLK_LSTATS, 1); R(CPCLK_LSTATS, 1); R(CPCLK_LSTATS, 0);
    W(CPCLK_CS0WCR, 0x000004C0); W(CPCLK_CS3WCR, 0x000024D2);       /* only WR and TRC taken from the request */
    ClockApply(&r);
    assert(cursor == count);

    count = cursor = 0;
    R(CPCLK_FLLFRQ, 0x00004384); R(CPCLK_FRQCR, 0x8F011112); R(CPCLK_CS0WCR, 0x3C0); R(CPCLK_CS3WCR, 0x24D1);
    ClockRead(&r);
    assert(cursor == count && memcmp(&r, &stock, sizeof r) == 0);

    puts("test_clock: ok");
    return 0;
}
