/* SH7305 clock chain (CPG) and the two wait-state fields that follow the bus
 * clock, shared by the OAL (which owns the registers) and cetune.exe (which
 * only computes). Method and tables after Xtune/CPtune4 (CalcLoverHK,
 * Sentaro21): FLL x450..2047 of 32768 Hz (halved when SELXM = 1), PLL
 * x1..64, then four dividers 1/2..1/64 for Iphi, Sphi, Bphi, Pphi.
 *
 *   FRQCR  0xA4150000  KICK 31, STC 29:24, IFC 23:20, SFC 15:12, BFC 11:8, PFC 3:0
 *   PLLCR  0xA4150024  bit 14 = PLL on
 *   FLLFRQ 0xA4150050  SELXM 15:14, FLF 10:0
 *   LSTATS 0xA4150060  nonzero while a frequency change settles
 *   CS0WCR 0xFEC10024  WR 10:7  (flash read waits)
 *   CS3WCR 0xFEC1002C  TRC 1:0, A3CL 8:7  (SDRAM)
 *
 * Plain C, no floating point (the FPU is emulated). The register sequence
 * at the end is compiled only where the MMIO accessors exist (the OAL and
 * the host test). */
#ifndef CLASSPAD_CLOCK_H
#define CLASSPAD_CLOCK_H

#define CPCLK_FRQCR     0xA4150000u
#define CPCLK_PLLCR     0xA4150024u
#define CPCLK_FLLFRQ    0xA4150050u
#define CPCLK_LSTATS    0xA4150060u
#define CPCLK_CS0WCR    0xFEC10024u
#define CPCLK_CS3WCR    0xFEC1002Cu

#define CPCLK_FRQCR_KICK    0x80000000u
#define CPCLK_FRQCR_FIELDS  0x3FF0FF0Fu     /* STC, IFC, SFC, BFC, PFC */
#define CPCLK_FLLFRQ_FIELDS 0x0000C7FFu     /* SELXM, FLF */
#define CPCLK_PLLCR_ON      0x00004000u
#define CPCLK_WR_MASK       0x00000780u
#define CPCLK_WR_SHIFT      7
#define CPCLK_TRC_MASK      0x00000003u
#define CPCLK_WR_SAFE       11              /* 18 waits while the clock moves */
#define CPCLK_TRC_SAFE      3               /* 9 cycles */

#define CPCLK_FLL_MIN   450u
#define CPCLK_FLL_MAX   2047u
#define CPCLK_PLL_MAX   64u
#define CPCLK_PPHI_MIN  1000000u            /* the 1 ms tick needs Pphi/4000 counts */

/* KernelIoControl codes (OAL\OALLIB\ioctl.c; 2100..2102 are in
 * classpad_pwrstat.h). GET: output ClasspadClockInfo. SET: input
 * ClasspadClockRegs; the OAL takes FLLFRQ, FRQCR, CS0WCR.WR and CS3WCR.TRC
 * from it, switches with interrupts off and rescales the TMU0 tick. */
#ifdef CTL_CODE
#define IOCTL_HAL_CLASSPAD_CLOCK_GET CTL_CODE(FILE_DEVICE_HAL, 2103, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_HAL_CLASSPAD_CLOCK_SET CTL_CODE(FILE_DEVICE_HAL, 2104, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

typedef struct { unsigned fllfrq, frqcr, cs0wcr, cs3wcr; } ClasspadClockRegs;
/* pphi: the peripheral clock as the OAL keeps time by it, in Hz: four times
 * the TMU0 count rate measured against the RTC at boot, scaled by every
 * change since. The frequencies computed from the registers are nominal. */
typedef struct { ClasspadClockRegs cur, boot; unsigned pllcr, pphi; } ClasspadClockInfo;

/* index 0..5 everywhere: FLL, PLL, Iphi, Sphi, Bphi, Pphi */
typedef struct { unsigned fll, selxm, pll, div[4]; } ClasspadClockChain;

/* Xtune defaults for the fx-CP400: step limits (index 1..5) and red zones */
static const unsigned g_cpclkLimit[6] = { 0, 800000000u, 275000000u, 180000000u, 134000000u, 50000000u };
static const unsigned g_cpclkRed[6]   = { 67000000u, 750000000u, 250000000u, 150000000u, 120000000u, 48000000u };
static const unsigned g_cpclkScale[6] = { 67100000u, 1000000000u, 350000000u, 200000000u, 200000000u, 60000000u };

static __inline void ClockDecode(const ClasspadClockRegs *r, ClasspadClockChain *c)
{
    c->fll    = r->fllfrq & 0x7FFu;
    c->selxm  = ((r->fllfrq >> 14) & 3u) == 1u;
    c->pll    = ((r->frqcr >> 24) & 63u) + 1u;
    c->div[0] = 2u << ((r->frqcr >> 20) & 15u);
    c->div[1] = 2u << ((r->frqcr >> 12) & 15u);
    c->div[2] = 2u << ((r->frqcr >> 8) & 15u);
    c->div[3] = 2u << (r->frqcr & 15u);
}

static __inline unsigned ClockDivCode(unsigned div)
{
    unsigned code = 0;
    while (div > 2u) { div >>= 1; code++; }
    return code;
}

/* Rewrites only the fields this header owns; KICK is left clear. */
static __inline void ClockEncode(const ClasspadClockChain *c, ClasspadClockRegs *r)
{
    r->fllfrq = (r->fllfrq & ~CPCLK_FLLFRQ_FIELDS) | (c->selxm ? 0x4000u : 0u) | (c->fll & 0x7FFu);
    r->frqcr  = (r->frqcr & ~(CPCLK_FRQCR_FIELDS | CPCLK_FRQCR_KICK))
              | ((c->pll - 1u) << 24) | (ClockDivCode(c->div[0]) << 20) | (ClockDivCode(c->div[1]) << 12)
              | (ClockDivCode(c->div[2]) << 8) | ClockDivCode(c->div[3]);
}

/* hz[0] = FLL output, hz[1] = PLL output, hz[2..5] = Iphi, Sphi, Bphi, Pphi.
 * 32768 * 2047 * 64 still fits 32 bits. */
static __inline void ClockHz(const ClasspadClockChain *c, unsigned pllOn, unsigned hz[6])
{
    unsigned i;
    hz[0] = (32768u * c->fll) >> (c->selxm ? 1 : 0);
    hz[1] = pllOn ? hz[0] * c->pll : hz[0];
    for (i = 0; i < 4; i++) hz[2 + i] = hz[1] / c->div[i];
}

/* Largest allowed ratio div[later] / div[earlier] (Xtune divs_ratio) */
static __inline unsigned ClockMaxRatio(unsigned later, unsigned earlier)
{
    static const unsigned char ratio[3][3] = { { 2, 0, 0 }, { 8, 4, 0 }, { 16, 8, 8 } };
    return ratio[later - 1][earlier];
}

/* What the OAL insists on, whatever the limits switch says. */
static __inline int ClockValid(const ClasspadClockRegs *r, unsigned pllOn)
{
    ClasspadClockChain c;
    unsigned hz[6], i, j;
    if (((r->frqcr >> 20) & 15u) > 5u || ((r->frqcr >> 12) & 15u) > 5u ||
        ((r->frqcr >> 8) & 15u) > 5u || (r->frqcr & 15u) > 5u) return 0;
    ClockDecode(r, &c);
    if (c.fll < CPCLK_FLL_MIN) return 0;
    for (i = 1; i < 4; i++)
        for (j = 0; j < i; j++)
            if (c.div[i] < c.div[j] || c.div[i] / c.div[j] > ClockMaxRatio(i, j)) return 0;
    ClockHz(&c, pllOn, hz);
    return hz[5] >= CPCLK_PPHI_MIN;
}

/* 0 = within limits, else the index (1..5) of the first clock above its limit */
static __inline int ClockOverLimit(const ClasspadClockChain *c, const unsigned *limit)
{
    unsigned hz[6], i;
    ClockHz(c, 1, hz);
    for (i = 1; i < 6; i++) if (hz[i] > limit[i]) return (int)i;
    return 0;
}

/* One step of row sel (0..5) in direction dir (+1 = faster). big: FLL moves
 * by 50 instead of 1. limit = NULL lifts the limits (Xtune's unlocked mode).
 * Returns 0 when the chain changed, -1 at the end of the range, or the index
 * of the clock whose limit refused the step. Dividers move their neighbours
 * as in Xtune: equal ones follow, the ratio table drags the rest; Pphi steps
 * down by itself when a multiplier pushes it over its limit and back up when
 * there is room. */
static __inline int ClockStep(ClasspadClockChain *chain, int sel, int dir, int big, const unsigned *limit)
{
    ClasspadClockChain c = *chain;
    unsigned hz[6];
    int i, over;

    if (sel == 0) {
        unsigned step = big ? 50u : 1u;
        if (dir > 0) { if (c.fll >= CPCLK_FLL_MAX) return -1; c.fll = c.fll + step > CPCLK_FLL_MAX ? CPCLK_FLL_MAX : c.fll + step; }
        else         { if (c.fll <= CPCLK_FLL_MIN) return -1; c.fll = c.fll < CPCLK_FLL_MIN + step ? CPCLK_FLL_MIN : c.fll - step; }
    } else if (sel == 1) {
        if (dir > 0) { if (c.pll >= CPCLK_PLL_MAX) return -1; c.pll++; }
        else         { if (c.pll <= 1u) return -1; c.pll--; }
    } else {
        int k = sel - 2;
        if (dir > 0) {
            if (c.div[k] <= 2u) return -1;
            for (i = k - 1; i >= 0; i--) if (c.div[i] == c.div[k]) c.div[i] >>= 1;
            c.div[k] >>= 1;
            for (i = k + 1; i < 4; i++) if (c.div[i] / c.div[k] > ClockMaxRatio((unsigned)i, (unsigned)k)) c.div[i] >>= 1;
        } else {
            if (c.div[k] >= 64u) return -1;
            for (i = k + 1; i < 4; i++) if (c.div[i] == c.div[k]) c.div[i] <<= 1;
            c.div[k] <<= 1;
            for (i = k - 1; i >= 0; i--) if (c.div[k] / c.div[i] > ClockMaxRatio((unsigned)k, (unsigned)i)) c.div[i] <<= 1;
        }
    }

    if (limit && sel < 2) {
        ClockHz(&c, 1, hz);
        if (dir > 0) {
            // only while the ratio table still allows a slower Pphi
            while (hz[5] > limit[5] && c.div[3] < 64u && (c.div[3] << 1) / c.div[0] <= ClockMaxRatio(3, 0)
                   && (c.div[3] << 1) / c.div[1] <= ClockMaxRatio(3, 1) && (c.div[3] << 1) / c.div[2] <= ClockMaxRatio(3, 2)) {
                c.div[3] <<= 1; hz[5] >>= 1;
            }
        } else if (hz[5] * 2u <= limit[5] && c.div[3] > c.div[2]) {
            c.div[3] >>= 1;
        }
    }
    if (limit && dir > 0 && (over = ClockOverLimit(&c, limit)) != 0) return over;
    *chain = c;
    return 0;
}

/* Wait fields for a bus clock: the lowest setting whose measured ceiling,
 * less a 5 % margin, still covers bphi (Xtune's fx-CP400 tables). */
static __inline unsigned ClockBestRomWR(unsigned bphi)
{
    static const unsigned char mhz[10] = { 13, 23, 33, 43, 53, 62, 72, 92, 110, 130 };
    unsigned i;
    for (i = 0; i < 10; i++) if (bphi <= mhz[i] * 950000u) return i;
    return 10;
}
static __inline unsigned ClockBestTRC(unsigned bphi)
{
    static const unsigned char mhz[3] = { 80, 105, 127 };
    unsigned i;
    for (i = 0; i < 3; i++) if (bphi <= mhz[i] * 950000u) return i;
    return 3;
}
static __inline unsigned ClockWRCycles(unsigned code)
{
    static const unsigned char wait[16] = { 0, 1, 2, 3, 4, 5, 6, 8, 10, 12, 14, 18, 24, 24, 24, 24 };
    return wait[code & 15u];
}
static __inline unsigned ClockTRCCycles(unsigned code)
{
    static const unsigned char cycles[4] = { 3, 4, 6, 9 };
    return cycles[code & 3u];
}
static __inline void ClockAutoWait(ClasspadClockRegs *r, unsigned bphi)
{
    r->cs0wcr = (r->cs0wcr & ~CPCLK_WR_MASK) | (ClockBestRomWR(bphi) << CPCLK_WR_SHIFT);
    r->cs3wcr = (r->cs3wcr & ~CPCLK_TRC_MASK) | ClockBestTRC(bphi);
}

#ifdef MMIO_W32
static __inline void ClockRead(ClasspadClockRegs *r)
{
    r->fllfrq = MMIO_R32(CPCLK_FLLFRQ);
    r->frqcr  = MMIO_R32(CPCLK_FRQCR) & ~CPCLK_FRQCR_KICK;
    r->cs0wcr = MMIO_R32(CPCLK_CS0WCR);
    r->cs3wcr = MMIO_R32(CPCLK_CS3WCR);
}

/* Interrupts must be off. Slowest waits first, then the frequency, then the
 * requested waits: the bus is never faster than its wait setting allows.
 * Only the fields this header owns are taken from *want. */
static __inline void ClockApply(const ClasspadClockRegs *want)
{
    unsigned cs0 = MMIO_R32(CPCLK_CS0WCR), cs3 = MMIO_R32(CPCLK_CS3WCR), i;

    MMIO_W32(CPCLK_CS0WCR, (cs0 & ~CPCLK_WR_MASK) | (CPCLK_WR_SAFE << CPCLK_WR_SHIFT));
    MMIO_W32(CPCLK_CS3WCR, cs3 | CPCLK_TRC_SAFE);
    MMIO_W32(CPCLK_FLLFRQ, (MMIO_R32(CPCLK_FLLFRQ) & ~CPCLK_FLLFRQ_FIELDS) | (want->fllfrq & CPCLK_FLLFRQ_FIELDS));
    MMIO_W32(CPCLK_FRQCR, (MMIO_R32(CPCLK_FRQCR) & ~(CPCLK_FRQCR_FIELDS | CPCLK_FRQCR_KICK))
                          | (want->frqcr & CPCLK_FRQCR_FIELDS) | CPCLK_FRQCR_KICK);
    for (i = 0; i < 1000000u && MMIO_R32(CPCLK_LSTATS) != 0; i++) { }
    MMIO_W32(CPCLK_CS0WCR, (cs0 & ~CPCLK_WR_MASK) | (want->cs0wcr & CPCLK_WR_MASK));
    MMIO_W32(CPCLK_CS3WCR, (cs3 & ~CPCLK_TRC_MASK) | (want->cs3wcr & CPCLK_TRC_MASK));
}
#endif

#endif
