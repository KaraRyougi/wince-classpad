//------------------------------------------------------------------------------
//  classpad_iic.h - IIC0 registers and the SCL clock at the measured Pphi
//
//  The SH7305's IIC0 (SH-Mobile I2C, 8-bit registers at 4-byte spacing)
//  carries the fx-CP400's touch sensor; the touch driver runs its
//  transactions by polling ICSR (classpad_touch_bus.h).
//
//  Clock. SCL is Pphi / (ICCL + ICCH). Casio's OS and gint write 0x29 and
//  0x22, 75 counts, which is 393 kHz at the 29.4912 MHz Pphi of the fx-CG50
//  family; the fx-CP400 runs a faster Pphi (bsp_cfg.h) and an overclocking
//  tool raises it further, and the same two bytes then clock the sensor
//  that much faster. The OAL knows the Pphi of the moment (timer.c: measured
//  against the RTC at boot, and whatever TMU0 is retuned to when a tool
//  changes the clock while CE runs) and hands it out through
//  IOCTL_HAL_CLASSPAD_PPHI; ClasspadIicCounts scales the counts to keep SCL
//  at or below a wanted rate, never below Casio's counts. A wanted rate of 0
//  asks for Casio's counts as they are. Pphi can change at any time, so the
//  touch driver asks again at every pen-down and after a failed transaction.
//  (The overclocking tool's own IOCTLs, CLOCK_GET/SET in classpad_clock.h of
//  its branch, also report a Pphi, computed from the clock registers; this
//  one is the rate the tick really runs at.)
//------------------------------------------------------------------------------
#ifndef __CLASSPAD_IIC_H
#define __CLASSPAD_IIC_H

#define IIC0_BASE               0xA4470000                      // SH7305_REG_PA_IIC0
#define IIC0_ICDR               (IIC0_BASE + 0x00)              // data
#define IIC0_ICCR               (IIC0_BASE + 0x04)              // control
#define IIC0_ICSR               (IIC0_BASE + 0x08)              // status
#define IIC0_ICIC               (IIC0_BASE + 0x0C)              // interrupt enables
#define IIC0_ICCL               (IIC0_BASE + 0x10)              // clock low
#define IIC0_ICCH               (IIC0_BASE + 0x14)              // clock high

#define ICCR_ICE                0x80    // interface enable (clearing it resets the controller)
#define ICCR_RACK               0x40
#define ICCR_TRS                0x10    // transmit (1) / receive (0)
#define ICCR_BBSY               0x04    // with TRS: start condition
#define ICCR_SCP                0x01    // receive: keep reading (1) / last byte (0)

#define ICSR_SCLM               0x80
#define ICSR_SDAM               0x40
#define ICSR_BUSY               0x10
#define ICSR_AL                 0x08
#define ICSR_TACK               0x04    // no acknowledge from the slave
#define ICSR_WAIT               0x02
#define ICSR_DTE                0x01    // data transfer enable: next byte may be written / read

#define CLASSPAD_IIC_ICCL_STOCK 0x29
#define CLASSPAD_IIC_ICCH_STOCK 0x22
#define CLASSPAD_IIC_SCL_HZ     393216u // 29491200 / 75: what Casio's counts give at the nominal Pphi

// out: CLASSPAD_PPHI
#define IOCTL_HAL_CLASSPAD_PPHI \
    CTL_CODE(FILE_DEVICE_HAL, 2103, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct {
    UINT32 pphiHz;                      // the peripheral clock now
    UINT32 measured;                    // 1: the boot value was measured against the RTC; 0: it was the nominal 29.4912 MHz
} CLASSPAD_PPHI;

// ICCL/ICCH for SCL <= sclHz at this Pphi, in Casio's low:high proportion
// (41:34), never fewer counts than Casio's and never more than the 8 bits
// hold. At 29.4912 MHz and CLASSPAD_IIC_SCL_HZ this is exactly 0x29/0x22.
static __inline void ClasspadIicCounts(UINT32 pphiHz, UINT32 sclHz, UINT32 *iccl, UINT32 *icch)
{
    UINT32 l = CLASSPAD_IIC_ICCL_STOCK, h = CLASSPAD_IIC_ICCH_STOCK, total;
    if (pphiHz && sclHz) {
        total = pphiHz / sclHz + (pphiHz % sclHz ? 1 : 0);
        if (total > CLASSPAD_IIC_ICCL_STOCK + CLASSPAD_IIC_ICCH_STOCK) {
            l = (total * CLASSPAD_IIC_ICCL_STOCK + CLASSPAD_IIC_ICCL_STOCK + CLASSPAD_IIC_ICCH_STOCK - 1)
                / (CLASSPAD_IIC_ICCL_STOCK + CLASSPAD_IIC_ICCH_STOCK);
            h = total - l;
        }
        if (l > 255) l = 255;
        if (h > 255) h = 255;
    }
    *iccl = l;
    *icch = h;
}

#endif
