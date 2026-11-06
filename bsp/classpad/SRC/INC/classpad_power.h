/* Retained-RAM sleep. No BAR/reset entry or cache/MMU power gating.
 * Include after bsp_base_regs.h, classpad_keysc.h and MMIO adapters.
 * Used by the OAL and the translated suspend regression guest. */
#ifndef CLASSPAD_POWER_H
#define CLASSPAD_POWER_H
#define CP_POWER_STBCR  0xA4150020u
#define CP_POWER_MSTPCR0 0xA4150030u
#define CP_CLEAR_ROW 0x0001u
#define CP_CLEAR_COL 0x0002u
#define CP_CLEAR_BIT 0x0002u /* basic key 20: row 0, column 1 */
typedef struct {
    unsigned char masks[13], tstr;
    unsigned short count, mode, rows, cols, keyint;
    unsigned stbcr, mstp, tcor, tcnt;
    unsigned short tcr;
    volatile unsigned active, armed, wake;
} ClasspadPowerState;

static unsigned ClasspadClearDown(void)
{
    return (MMIO_R16(KEYSC_KIUDATA(0)) & CP_CLEAR_BIT) != 0;
}

static void ClasspadPowerPrepare(ClasspadPowerState *p, unsigned countsPerMSec)
{
    unsigned i;
    p->active = 0; p->wake = 0; p->armed = !ClasspadClearDown();
    p->stbcr = MMIO_R32(CP_POWER_STBCR);
    p->mstp = MMIO_R32(CP_POWER_MSTPCR0);
    p->tstr = MMIO_R8(TMU_TSTR);
    p->tcor = MMIO_R32(TMU_TCOR0);
    p->tcnt = MMIO_R32(TMU_TCNT0);
    p->tcr = MMIO_R16(TMU_TCR0);
    p->count = MMIO_R16(KEYSC_KIUCNTREG);
    p->mode = MMIO_R16(KEYSC_KIUMODEREG);
    p->rows = MMIO_R16(KEYSC_KOUTPINSET);
    p->cols = MMIO_R16(KEYSC_KINPINSET);
    p->keyint = MMIO_R16(KEYSC_KIUINTREG) & 0xFF00;
    for (i = 0; i < 13; ++i) {
        p->masks[i] = MMIO_R8(INTC_IMR(i));
        MMIO_W8(INTC_IMR(i), 0xFF);
    }
    MMIO_W8(TMU_TSTR, 0); /* no periodic timer wake; preserve all counters */
    MMIO_W32(CP_POWER_STBCR, p->stbcr & ~0xB0u); /* ordinary SLEEP */
    MMIO_W32(CP_POWER_MSTPCR0, p->mstp & ~0x8040u); /* KEYSC and TMU stay clocked */
    MMIO_W16(KEYSC_KIUCNTREG, 0);
    MMIO_W16(KEYSC_KOUTPINSET, CP_CLEAR_ROW);
    MMIO_W16(KEYSC_KINPINSET, CP_CLEAR_COL);
    MMIO_W16(KEYSC_KIUMODEREG, KEYSC_MODE_VALUE);
    MMIO_W16(KEYSC_KIUINTREG, KEYSC_INT_ENABLE | KEYSC_INT_CLEAR);
    p->active = 1;
    MMIO_W8(INTC_IMCR(5), INTC_IMR5_KEYSC);
    MMIO_W16(KEYSC_KIUCNTREG, KEYSC_CNT_SCAN_START);
    /* Keep a low-frequency recovery scan independent of KEYSC IRQ delivery.
     * This interrupt never schedules CE or exits suspend on a non-Clear key. */
    MMIO_W16(TMU_TCR0, TMU_TCR_UNIE);
    MMIO_W32(TMU_TCOR0, countsPerMSec * 20u - 1u);
    MMIO_W32(TMU_TCNT0, countsPerMSec * 20u - 1u);
    MMIO_W8(INTC_IMCR(4), INTC_IMR4_TMU0);
    MMIO_W8(TMU_TSTR, TMU_TSTR_STR0);
}

static void ClasspadPowerKeyInterrupt(ClasspadPowerState *p)
{
    unsigned down = ClasspadClearDown();
    MMIO_W16(KEYSC_KIUINTREG, KEYSC_INT_ENABLE | (MMIO_R16(KEYSC_KIUINTREG) & 0xFF));
    if (!down) p->armed = 1;
    else if (p->armed) p->wake = 1;
}

static void ClasspadPowerTimerInterrupt(ClasspadPowerState *p)
{
    MMIO_W16(TMU_TCR0, MMIO_R16(TMU_TCR0) & ~TMU_TCR_UNF);
    ClasspadPowerKeyInterrupt(p);
    /* Start the next scan; the current completed image was read above. */
    if (!p->wake) MMIO_W16(KEYSC_KIUCNTREG, KEYSC_CNT_SCAN_START);
}

static void ClasspadPowerRestore(ClasspadPowerState *p)
{
    unsigned i;
    MMIO_W8(INTC_IMR(5), INTC_IMR5_KEYSC);
    MMIO_W8(INTC_IMR(4), INTC_IMR4_TMU0);
    MMIO_W8(TMU_TSTR, 0);
    p->active = 0;
    MMIO_W32(TMU_TCOR0, p->tcor);
    MMIO_W32(TMU_TCNT0, p->tcnt);
    MMIO_W16(TMU_TCR0, p->tcr);
    MMIO_W16(KEYSC_KIUCNTREG, 0);
    MMIO_W16(KEYSC_KOUTPINSET, p->rows);
    MMIO_W16(KEYSC_KINPINSET, p->cols);
    MMIO_W16(KEYSC_KIUMODEREG, p->mode);
    MMIO_W16(KEYSC_KIUINTREG, p->keyint | KEYSC_INT_CLEAR);
    MMIO_W16(KEYSC_KIUCNTREG, p->count);
    MMIO_W32(CP_POWER_MSTPCR0, p->mstp);
    MMIO_W32(CP_POWER_STBCR, p->stbcr);
    MMIO_W8(TMU_TSTR, p->tstr);
    for (i = 0; i < 13; ++i) {
        MMIO_W8(INTC_IMR(i), 0xFF);
        MMIO_W8(INTC_IMCR(i), (unsigned char)~p->masks[i]);
    }
}
#endif
