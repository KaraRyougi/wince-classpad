/* ClassPad polled IIC0 transactions. Include after bsp_base_regs.h,
 * classpad_touch.h (classpad_iic.h) and the MMIO adapters. Same WAIT-stage
 * ordering as ClasspadDev/gint's src/touch/i2c_inth.c, but peripheral IRQs
 * remain masked at the INTC: ICIC enables WAIT and TACK status even though
 * no ISR is used.
 *
 * The caller spins on ICSR for the whole transaction (about 0.5 ms for the
 * 16-byte packet at 393 kHz) and never sleeps in it: a byte takes some 23
 * microseconds, far less than any Sleep, and under the translator one
 * interrupt per byte costs about as much as waiting for the byte.
 *
 * No unbounded bus waits, and none that depend on how fast the CPU runs:
 * the limit of a transaction is TOUCH_BUS_LIMIT_MS of the system's tick
 * count (TMU0, calibrated against the RTC; it runs whenever CE does). The
 * tick count is a kernel call, so it is asked only once every
 * TOUCH_BUS_CLOCK_EVERY status reads, and the limit starts at the first
 * such look: a transaction that goes well ends before it and never asks.
 * (Until 2026-11-12 every stage was 64 status reads, then Sleep(1), twenty
 * times: 64 reads are less than a byte on a fast CPU, so the faster the
 * CPU, the more stages fell into the Sleep.)
 *
 * ICCL/ICCH are set by TouchBusSetClock for the Pphi the OAL measured
 * (classpad_iic.h); until it is called they are Casio's.
 * Stage/status survive a reset for diagnostics. */
#ifndef CLASSPAD_TOUCH_BUS_H
#define CLASSPAD_TOUCH_BUS_H

#define TOUCH_BUS_CLOCK_EVERY   1024        /* status reads between two looks at the tick count */
#define TOUCH_BUS_LIMIT_MS      10          /* from the first look; a packet takes about 0.5 ms */
#ifndef TOUCH_BUS_NOW_MS
#define TOUCH_BUS_NOW_MS()      GetTickCount()
#endif

static unsigned g_touchBusStage, g_touchBusStatus;
static unsigned g_touchBusIccl = CLASSPAD_IIC_ICCL_STOCK, g_touchBusIcch = CLASSPAD_IIC_ICCH_STOCK;
static unsigned g_touchBusReads, g_touchBusSince;       /* status reads of this transaction; tick count at the first look, valid once reads >= CLOCK_EVERY */

static void TouchBusSetClock(unsigned pphiHz, unsigned sclHz)
{
    UINT32 l, h;
    ClasspadIicCounts(pphiHz, sclHz, &l, &h);
    g_touchBusIccl = l;
    g_touchBusIcch = h;
}

static void TouchBusReset(void)
{
    MMIO_W8(IIC0_ICCR, 0);                /* abort stale transfer, reset controller */
    MMIO_W8(IIC0_ICCR, ICCR_ICE);
    MMIO_W8(IIC0_ICCL, g_touchBusIccl);
    MMIO_W8(IIC0_ICCH, g_touchBusIcch);
    MMIO_W8(IIC0_ICSR, 0);
    MMIO_W8(IIC0_ICIC, 0x0E);            /* AL, TACK, WAIT; INTC masked */
}

/* A transaction starts: reset the controller and its time limit */
static void TouchBusBegin(void)
{
    TouchBusReset();
    g_touchBusReads = 0;
}

static int TouchBusWait(unsigned mask, unsigned expected, unsigned stage)
{
    unsigned reads = g_touchBusReads;
    g_touchBusStage = stage;
    for (;;) {
        unsigned sr = MMIO_R8(IIC0_ICSR);
        if (sr & (ICSR_AL | ICSR_TACK)) { g_touchBusStatus = sr; g_touchBusReads = reads; return 0; }
        if ((sr & mask) == expected) { g_touchBusStatus = sr; g_touchBusReads = reads; return 1; }
        if ((++reads & (TOUCH_BUS_CLOCK_EVERY - 1u)) == 0) {
            unsigned ms = (unsigned)TOUCH_BUS_NOW_MS();
            if (reads == TOUCH_BUS_CLOCK_EVERY) g_touchBusSince = ms;
            else if (ms - g_touchBusSince >= TOUCH_BUS_LIMIT_MS) { g_touchBusStatus = sr; g_touchBusReads = reads; return 0; }
        }
    }
}

static void TouchBusReleaseWait(void)
{
    MMIO_W8(IIC0_ICSR, MMIO_R8(IIC0_ICSR) & ~ICSR_WAIT);
}

/* Read n bytes starting at a register (or after a command byte): start,
 * address + W, the register, repeated start, address + R, the bytes. The
 * OS's 0x8008A77E; its one-byte form is 0x8008A6E4. */
static int TouchBusReadReg(unsigned char reg, unsigned char *pkt, int n)
{
    int i;
    if (!pkt || n < 1) return 0;
    TouchBusBegin();
    if (!TouchBusWait(ICSR_BUSY, 0, 1)) goto fail;
    MMIO_W8(IIC0_ICCR, ICCR_ICE | ICCR_TRS | ICCR_BBSY);
    if (!TouchBusWait(ICSR_DTE, ICSR_DTE, 2)) goto fail;
    MMIO_W8(IIC0_ICDR, TOUCH_IIC_ADDRESS << 1);
    if (!TouchBusWait(ICSR_WAIT, ICSR_WAIT, 3)) goto fail;
    MMIO_W8(IIC0_ICDR, reg);
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

static int TouchBusRead(unsigned char *pkt, int n)
{
    return TouchBusReadReg(TOUCH_CMD_SNAPSHOT, pkt, n);
}

/* One register write: start, address + W, the register, the value, stop
 * (the OS's 0x8008A690). */
static int TouchBusWriteReg(unsigned char reg, unsigned char value)
{
    TouchBusBegin();
    if (!TouchBusWait(ICSR_BUSY, 0, 21)) goto fail;
    MMIO_W8(IIC0_ICCR, ICCR_ICE | ICCR_TRS | ICCR_BBSY);
    if (!TouchBusWait(ICSR_DTE, ICSR_DTE, 22)) goto fail;
    MMIO_W8(IIC0_ICDR, TOUCH_IIC_ADDRESS << 1);
    if (!TouchBusWait(ICSR_WAIT, ICSR_WAIT, 23)) goto fail;
    MMIO_W8(IIC0_ICDR, reg);
    TouchBusReleaseWait();
    if (!TouchBusWait(ICSR_WAIT, ICSR_WAIT, 24)) goto fail;
    MMIO_W8(IIC0_ICDR, value);
    TouchBusReleaseWait();
    if (!TouchBusWait(ICSR_WAIT, ICSR_WAIT, 25)) goto fail;
    MMIO_W8(IIC0_ICCR, ICCR_ICE | ICCR_TRS);
    TouchBusReleaseWait();
    if (!TouchBusWait(ICSR_BUSY, 0, 26)) goto fail;
    MMIO_W8(IIC0_ICCR, 0);
    return 1;
fail:
    MMIO_W8(IIC0_ICCR, 0);
    return 0;
}

/* One command byte and a stop: start, address + W, the byte, then ICCR =
 * ICE | TRS with BBSY clear (the stop condition) at the byte's WAIT. */
static int TouchBusCommand(unsigned char cmd)
{
    TouchBusBegin();
    if (!TouchBusWait(ICSR_BUSY, 0, 11)) goto fail;
    MMIO_W8(IIC0_ICCR, ICCR_ICE | ICCR_TRS | ICCR_BBSY);
    if (!TouchBusWait(ICSR_DTE, ICSR_DTE, 12)) goto fail;
    MMIO_W8(IIC0_ICDR, TOUCH_IIC_ADDRESS << 1);
    if (!TouchBusWait(ICSR_WAIT, ICSR_WAIT, 13)) goto fail;
    MMIO_W8(IIC0_ICDR, cmd);
    TouchBusReleaseWait();
    if (!TouchBusWait(ICSR_WAIT, ICSR_WAIT, 14)) goto fail;
    MMIO_W8(IIC0_ICCR, ICCR_ICE | ICCR_TRS);
    TouchBusReleaseWait();
    if (!TouchBusWait(ICSR_BUSY, 0, 15)) goto fail;
    MMIO_W8(IIC0_ICCR, 0);
    return 1;
fail:
    MMIO_W8(IIC0_ICCR, 0);
    return 0;
}
#endif
