/* SH7305 clock chain (CPG) and the bus/wait-state fields that follow it,
 * shared by the OAL (which owns the registers) and cetune.exe (which only
 * computes). Method, tables and Ptune4 F5 fast idle cycles after Xtune /
 * Ptune4 (CalcLoverHK, Sentaro21) and gpSP's ClassPad II port: FLL x450..2047
 * of 32768 Hz (halved when SELXM = 1), PLL x1..64, then four dividers
 * 1/2..1/64 for Iphi, Sphi, Bphi, Pphi.
 *
 *   FRQCR  0xA4150000  KICK 31, STC 29:24, IFC 23:20, SFC 15:12, BFC 11:8, PFC 3:0
 *   PLLCR  0xA4150024  bit 14 = PLL on
 *   SSCGCR 0xA4150044  bit 31 = spread spectrum on (read-only here)
 *   FLLFRQ 0xA4150050  SELXM 15:14, FLF 10:0
 *   LSTATS 0xA4150060  nonzero while a frequency change settles
 *   CS0BCR 0xFEC10004  idle waits 30:16 (flash: Ptune4 F5 0x4490xxxx)
 *   CS3BCR 0xFEC1000C  idle waits 30:16 (SDRAM: Ptune4 F5 0x0490xxxx)
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
#define CPCLK_SSCGCR    0xA4150044u
#define CPCLK_FLLFRQ    0xA4150050u
#define CPCLK_LSTATS    0xA4150060u
#define CPCLK_CS0BCR    0xFEC10004u
#define CPCLK_CS3BCR    0xFEC1000Cu
#define CPCLK_CS0WCR    0xFEC10024u
#define CPCLK_CS3WCR    0xFEC1002Cu

#define CPCLK_FRQCR_KICK        0x80000000u
#define CPCLK_FRQCR_FIELDS      0x3FF0FF0Fu     /* STC, IFC, SFC, BFC, PFC */
#define CPCLK_FLLFRQ_FIELDS     0x0000C7FFu     /* SELXM, FLF */
#define CPCLK_PLLCR_ON          0x00004000u
#define CPCLK_SSCGCR_SSEN       0x80000000u     /* spread-spectrum enable (bit 31, read-only) */
#define CPCLK_WR_MASK           0x00000780u
#define CPCLK_WR_SHIFT          7
#define CPCLK_TRC_MASK          0x00000003u
#define CPCLK_WR_SAFE           11u             /* 18 waits while the clock moves */
#define CPCLK_TRC_SAFE          3u              /* 9 cycles */

/* Idle-cycle fields (IWW, IWRWD, IWRWS, IWRRD, IWRRS, bits 30:16) of Ptune4
 * F5 / gpSP (CS0BCR = 0x44900400, CS3BCR = 0x04904400). Only the stock chain
 * keeps the calculator's own conservative idle cycles (e.g. CP400 CS3BCR
 * 0x76db4400). Everything else in CS0BCR/CS3BCR stays as stock has it. */
#define CPCLK_IDLE_MASK         0x7FFF0000u
#define CPCLK_CS0BCR_IDLE_FAST  0x44900000u
#define CPCLK_CS3BCR_IDLE_FAST  0x04900000u

#define CPCLK_FLL_MIN   450u
#define CPCLK_FLL_MAX   2047u
#define CPCLK_PLL_MAX   64u
#define CPCLK_PPHI_MIN  1000000u            /* the 1 ms tick needs Pphi/4000 counts */
#define CPCLK_F5_PLL_HZ 471859200u          /* Ptune4 F5 PLL output: 235.93 / 117.96 / 117.96 / 29.49 MHz */

/* KernelIoControl codes (OAL\OALLIB\ioctl.c; 2100..2102 are in
 * classpad_pwrstat.h, 2103 is the touch driver's IOCTL_HAL_CLASSPAD_PPHI).
 * GET: output ClasspadClockInfo. SET: input ClasspadClockRegs; the OAL takes
 * FLLFRQ, FRQCR, CS0BCR/CS3BCR idle waits, CS0WCR.WR and CS3WCR.TRC from it,
 * switches with interrupts off and rescales the TMU0 tick. */
#ifdef CTL_CODE
#define IOCTL_HAL_CLASSPAD_CLOCK_GET CTL_CODE(FILE_DEVICE_HAL, 2104, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_HAL_CLASSPAD_CLOCK_SET CTL_CODE(FILE_DEVICE_HAL, 2105, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

typedef struct {
    unsigned fllfrq, frqcr, cs0bcr, cs3bcr, cs0wcr, cs3wcr;
} ClasspadClockRegs;
/* pphi: the peripheral clock as the OAL keeps time by it, in Hz: four times
 * the TMU0 count rate measured against the RTC at boot, scaled by every
 * change since. The frequencies computed from the registers are nominal. */
typedef struct {
    ClasspadClockRegs cur, boot;
    unsigned pllcr, sscgcr, pphi;
} ClasspadClockInfo;

/* index 0..5 everywhere: FLL, PLL, Iphi, Sphi, Bphi, Pphi */
typedef struct { unsigned fll, selxm, pll, div[4]; } ClasspadClockChain;

/* Xtune / gpSP limits for the fx-CP400: default limits (index 1..5), hard
 * limits for unlocked mode (Xtune PLL_MAX..IO_MAX / gpSP cp_clock_hard_limit),
 * red zones (Xtune config.h.in) and bar scales. */
static const unsigned g_cpclkLimit[6]     = { 0, 800000000u, 275000000u, 180000000u, 134000000u, 50000000u };
static const unsigned g_cpclkHardLimit[6] = { 0, 1000000000u, 350000000u, 200000000u, 200000000u, 60000000u };
static const unsigned g_cpclkRed[6]       = { 17000000u, 750000000u, 250000000u, 150000000u, 120000000u, 48000000u };
static const unsigned g_cpclkScale[6]     = { 67100000u, 1000000000u, 350000000u, 200000000u, 200000000u, 60000000u };

static __inline int ClockSameChain(const ClasspadClockChain *a, const ClasspadClockChain *b)
{
    return a->fll == b->fll && a->selxm == b->selxm && a->pll == b->pll
        && a->div[0] == b->div[0] && a->div[1] == b->div[1]
        && a->div[2] == b->div[2] && a->div[3] == b->div[3];
}

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

/* Rewrites only the chain fields this header owns; KICK is left clear. */
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
 * by 50 instead of 1. limit = g_cpclkLimit (normal) or g_cpclkHardLimit
 * (unlocked), or NULL for no upper limit. Returns 0 when the chain changed,
 * -1 at the end of the range (or below CPCLK_PPHI_MIN), or the index of the
 * clock whose limit refused the step. Dividers move their neighbours as in
 * Xtune: equal ones follow, the ratio table drags the rest; Pphi steps down
 * by itself when a multiplier pushes it over its limit and back up when there
 * is room. */
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
            /* only while the ratio table still allows a slower Pphi */
            while (hz[5] > limit[5] && c.div[3] < 64u && (c.div[3] << 1) / c.div[0] <= ClockMaxRatio(3, 0)
                   && (c.div[3] << 1) / c.div[1] <= ClockMaxRatio(3, 1) && (c.div[3] << 1) / c.div[2] <= ClockMaxRatio(3, 2)) {
                c.div[3] <<= 1; hz[5] >>= 1;
            }
        } else {
            while (hz[5] * 2u <= limit[5] && c.div[3] > c.div[2]) {
                c.div[3] >>= 1; hz[5] <<= 1;
            }
        }
    }
    if (dir < 0) {
        ClockHz(&c, 1, hz);
        if (hz[5] < CPCLK_PPHI_MIN) return -1;
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
    if (bphi <= 150000000u) return 10;
    if (bphi <= 175000000u) return 11;
    return 12;
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

/* Full register setting for a target chain, strictly following gpSP's
 * cp_clock_for_chain() + Ptune4 F5 fast idle cycles:
 *  - the stock chain returns stock verbatim (including stock idle cycles);
 *  - a chain with the same bus clock as stock keeps stock's WR and TRC;
 *  - a faster bus never gets shorter WR/TRC than stock, a slower bus never
 *    gets longer WR/TRC than stock;
 *  - non-stock chains use Ptune4 F5's fast flash/SDRAM idle cycles (CS0BCR
 *    0x4490xxxx, CS3BCR 0x0490xxxx). */
static __inline void ClockForChain(const ClasspadClockRegs *stock, const ClasspadClockChain *ch,
                                   unsigned pllOn, ClasspadClockRegs *out)
{
    ClasspadClockChain stockChain;
    unsigned hz[6], stockHz[6];

    *out = *stock;
    ClockDecode(stock, &stockChain);
    if (ClockSameChain(&stockChain, ch)) return;

    ClockEncode(ch, out);
    ClockHz(&stockChain, pllOn, stockHz);
    ClockHz(ch, pllOn, hz);
    if (hz[4] != stockHz[4]) {
        int faster = hz[4] > stockHz[4];
        unsigned wait = ClockBestRomWR(hz[4]);
        unsigned trc  = ClockBestTRC(hz[4]);
        unsigned stockWait = (stock->cs0wcr & CPCLK_WR_MASK) >> CPCLK_WR_SHIFT;
        unsigned stockTrc  = stock->cs3wcr & CPCLK_TRC_MASK;

        if (faster ? wait < stockWait : wait > stockWait) wait = stockWait;
        if (faster ? trc < stockTrc : trc > stockTrc) trc = stockTrc;
        out->cs0wcr = (out->cs0wcr & ~CPCLK_WR_MASK) | (wait << CPCLK_WR_SHIFT);
        out->cs3wcr = (out->cs3wcr & ~CPCLK_TRC_MASK) | trc;
    }
    out->cs0bcr = (stock->cs0bcr & ~CPCLK_IDLE_MASK) | CPCLK_CS0BCR_IDLE_FAST;
    out->cs3bcr = (stock->cs3bcr & ~CPCLK_IDLE_MASK) | CPCLK_CS3BCR_IDLE_FAST;
}

/* Ptune4 preset chain at the 471.86 MHz PLL base (F2..F5 in Ptune4.g3a and
 * cp_clock_f5() in gpSP). Keeps the calculator's own FLLFRQ when it divides
 * 471.86 MHz cleanly, otherwise falls back to FLL x900 SELXM=1/2, PLL x32. */
static __inline void ClockPtuneChain(const ClasspadClockRegs *boot,
                                     unsigned d0, unsigned d1, unsigned d2, unsigned d3,
                                     ClasspadClockChain *out)
{
    ClasspadClockChain b;
    unsigned hz[6];

    ClockDecode(boot, &b);
    ClockHz(&b, 1, hz);
    if (hz[0] && CPCLK_F5_PLL_HZ % hz[0] == 0u &&
        CPCLK_F5_PLL_HZ / hz[0] >= 1u && CPCLK_F5_PLL_HZ / hz[0] <= CPCLK_PLL_MAX) {
        out->fll   = b.fll;
        out->selxm = b.selxm;
        out->pll   = CPCLK_F5_PLL_HZ / hz[0];
    } else {
        out->fll   = 900u;
        out->selxm = 1u;
        out->pll   = 32u;
    }
    out->div[0] = d0;
    out->div[1] = d1;
    out->div[2] = d2;
    out->div[3] = d3;
}

#ifdef MMIO_W32
static __inline void ClockRead(ClasspadClockRegs *r)
{
    r->fllfrq = MMIO_R32(CPCLK_FLLFRQ);
    r->frqcr  = MMIO_R32(CPCLK_FRQCR) & ~CPCLK_FRQCR_KICK;
    r->cs0bcr = MMIO_R32(CPCLK_CS0BCR);
    r->cs3bcr = MMIO_R32(CPCLK_CS3BCR);
    r->cs0wcr = MMIO_R32(CPCLK_CS0WCR);
    r->cs3wcr = MMIO_R32(CPCLK_CS3WCR);
}

/* Interrupts must be off. Slowest waits first, then the frequency, then the
 * requested idle cycles and waits: the bus is never faster than its wait
 * setting allows (matching gpSP's cp_clock_apply_step). Only the fields this
 * header owns are taken from *want; CS5AWCR, RTCOR and SSCGCR are untouched. */
static __inline void ClockApply(const ClasspadClockRegs *want)
{
    unsigned cs0b = MMIO_R32(CPCLK_CS0BCR), cs3b = MMIO_R32(CPCLK_CS3BCR);
    unsigned cs0w = MMIO_R32(CPCLK_CS0WCR), cs3w = MMIO_R32(CPCLK_CS3WCR), i;

    MMIO_W32(CPCLK_CS0WCR, (cs0w & ~CPCLK_WR_MASK) | (CPCLK_WR_SAFE << CPCLK_WR_SHIFT));
    MMIO_W32(CPCLK_CS3WCR, cs3w | CPCLK_TRC_SAFE);
    MMIO_W32(CPCLK_FLLFRQ, (MMIO_R32(CPCLK_FLLFRQ) & ~CPCLK_FLLFRQ_FIELDS) | (want->fllfrq & CPCLK_FLLFRQ_FIELDS));
    MMIO_W32(CPCLK_FRQCR, (MMIO_R32(CPCLK_FRQCR) & ~(CPCLK_FRQCR_FIELDS | CPCLK_FRQCR_KICK))
                          | (want->frqcr & CPCLK_FRQCR_FIELDS) | CPCLK_FRQCR_KICK);
    for (i = 0; i < 1000000u && MMIO_R32(CPCLK_LSTATS) != 0; i++) { }
    MMIO_W32(CPCLK_CS0BCR, (cs0b & ~CPCLK_IDLE_MASK) | (want->cs0bcr & CPCLK_IDLE_MASK));
    MMIO_W32(CPCLK_CS3BCR, (cs3b & ~CPCLK_IDLE_MASK) | (want->cs3bcr & CPCLK_IDLE_MASK));
    MMIO_W32(CPCLK_CS0WCR, (cs0w & ~CPCLK_WR_MASK) | (want->cs0wcr & CPCLK_WR_MASK));
    MMIO_W32(CPCLK_CS3WCR, (cs3w & ~CPCLK_TRC_MASK) | (want->cs3wcr & CPCLK_TRC_MASK));
}
#endif

#endif
