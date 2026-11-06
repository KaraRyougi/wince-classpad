/* ClassPad polled IIC0 transactions. Include after classpad_touch.h and the
 * MMIO/Sleep adapters. Same WAIT-stage ordering as ClasspadDev/gint's
 * src/touch/i2c_inth.c, but peripheral IRQs remain masked at the INTC.
 * ICIC enables WAIT and TACK status even though no ISR is used.
 * Stage/status survive a reset for diagnostics. No unbounded bus waits. */
#ifndef CLASSPAD_TOUCH_BUS_H
#define CLASSPAD_TOUCH_BUS_H
static unsigned g_touchBusStage, g_touchBusStatus;

static void TouchBusReset(void)
{
    MMIO_W8(IIC0_ICCR, 0);                /* abort stale transfer, reset controller */
    MMIO_W8(IIC0_ICCR, ICCR_ICE);
    MMIO_W8(IIC0_ICCL, 0x29);            /* Casio/gint clock at stock Pphi */
    MMIO_W8(IIC0_ICCH, 0x22);
    MMIO_W8(IIC0_ICSR, 0);
    MMIO_W8(IIC0_ICIC, 0x0E);            /* AL, TACK, WAIT; INTC masked */
}

static int TouchBusWait(unsigned mask, unsigned expected, unsigned stage)
{
    unsigned round, poll;
    g_touchBusStage = stage;
    for (round = 0; round < 20; ++round) {
        for (poll = 0; poll < 64; ++poll) {
            unsigned sr = MMIO_R8(IIC0_ICSR);
            g_touchBusStatus = sr;
            if (sr & (ICSR_AL | ICSR_TACK)) return 0;
            if ((sr & mask) == expected) return 1;
        }
        Sleep(1);
    }
    return 0;
}

static void TouchBusReleaseWait(void)
{
    MMIO_W8(IIC0_ICSR, MMIO_R8(IIC0_ICSR) & ~ICSR_WAIT);
}

static int TouchBusRead(unsigned char *pkt, int n)
{
    int i;
    if (!pkt || n < 1) return 0;
    TouchBusReset();
    if (!TouchBusWait(ICSR_BUSY, 0, 1)) goto fail;
    MMIO_W8(IIC0_ICCR, ICCR_ICE | ICCR_TRS | ICCR_BBSY);
    if (!TouchBusWait(ICSR_DTE, ICSR_DTE, 2)) goto fail;
    MMIO_W8(IIC0_ICDR, TOUCH_IIC_ADDRESS << 1);
    if (!TouchBusWait(ICSR_WAIT, ICSR_WAIT, 3)) goto fail;
    MMIO_W8(IIC0_ICDR, TOUCH_CMD_SNAPSHOT);
    TouchBusReleaseWait();
    if (!TouchBusWait(ICSR_WAIT, ICSR_WAIT, 4)) goto fail;
    MMIO_W8(IIC0_ICCR, ICCR_ICE | ICCR_TRS | ICCR_BBSY);
    TouchBusReleaseWait();
    if (!TouchBusWait(ICSR_DTE, ICSR_DTE, 5)) goto fail;
    MMIO_W8(IIC0_ICDR, (TOUCH_IIC_ADDRESS << 1) | 1);
    if (!TouchBusWait(ICSR_WAIT, ICSR_WAIT, 6)) goto fail;
    MMIO_W8(IIC0_ICCR, ICCR_ICE | ICCR_SCP);
    TouchBusReleaseWait();
    for (i = 0; i < n; ++i) {
        if (!TouchBusWait(ICSR_WAIT, ICSR_WAIT, 7)) goto fail;
        if (i == n - 1) MMIO_W8(IIC0_ICCR, ICCR_ICE | ICCR_RACK); /* NACK+STOP */
        TouchBusReleaseWait();
        if (!TouchBusWait(ICSR_DTE, ICSR_DTE, 8)) goto fail;
        pkt[i] = MMIO_R8(IIC0_ICDR);
    }
    if (!TouchBusWait(ICSR_BUSY, 0, 9)) goto fail;
    MMIO_W8(IIC0_ICCR, 0);
    return 1;
fail:
    MMIO_W8(IIC0_ICCR, 0);                /* next sample starts from a known state */
    return 0;
}
#endif
