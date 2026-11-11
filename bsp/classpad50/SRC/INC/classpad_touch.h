//------------------------------------------------------------------------------
//  classpad_touch.h - the fx-CP400 touch panel as casio-emu models it
//
//  The resistive panel is read through a sensor on IIC0 (I2C address 0x40),
//  not through the ADC: casio-emu src/hardware/classpad/iic.c reproduces the
//  register subset the ClassPad OS 02.01.7003 uses (OS code at 0x80089edc)
//  and the packet format of ClasspadDev/gint src/touch. Nothing of this
//  exists on the fx-CG100 (the IIC registers are not even defined in that
//  emulator profile), so the driver only touches the bus when the panel is
//  present (registry "Panel", else a 528-row display).
//
//  IIC0 (SH-Mobile I2C, 8-bit registers at 4-byte spacing):
//    ICDR  data, ICCR control (ICE, TRS, BBSY/start, SCP), ICSR status
//    (BUSY, AL, TACK, WAIT, DTE), ICIC status enables, ICCL/ICCH clock.
//    The production transaction polls WAIT/DTE with peripheral IRQs masked
//    at the INTC; ICIC=0x0E keeps WAIT/TACK status functional.
//
//  Sensor packet (command 0x84, 16 bytes, big-endian words):
//  x1,y1,z1,gh,x2,y2,z2,dm. Contact is PRDR, not z1. x/y are left-aligned by
//  four; the uncalibrated point handed to the touch MDD is the 12-bit value.
//  casio-emu maps the stylus to raw = 0x20B + x * 0x9B6 / 256 (x 0..319)
//  and 0xF4 + y * 0x66F / 256 (y 0..575; rows 528..575 are the printed icon
//  strip under the LCD). The registry CalibrationData in platform.reg is
//  computed from these for the 320x528 screen.
//
//  Contact: Port R bit 5 reads 0 while the panel is pressed and INTEVT 0x700
//  (mask IMR3 bit 4, priority IPRB bits 7:4) is raised on the down edge. The
//  sampler polls PRDR before the first contact too: the 0x700 routing is
//  modeled by casio-emu but is not a prerequisite for receiving input.
//  Hardware reference: ClasspadDev/gint src/touch/{driver,i2c,i2c_inth,touch}.c.
//  End-to-end device validation is still required.
//------------------------------------------------------------------------------
#ifndef __CLASSPAD_TOUCH_H
#define __CLASSPAD_TOUCH_H

#define IIC0_ICDR               (SH7305_REG_PA_IIC0 + 0x00)     // data
#define IIC0_ICCR               (SH7305_REG_PA_IIC0 + 0x04)     // control
#define IIC0_ICSR               (SH7305_REG_PA_IIC0 + 0x08)     // status
#define IIC0_ICIC               (SH7305_REG_PA_IIC0 + 0x0C)     // interrupt enables
#define IIC0_ICCL               (SH7305_REG_PA_IIC0 + 0x10)     // clock low
#define IIC0_ICCH               (SH7305_REG_PA_IIC0 + 0x14)     // clock high

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

#define TOUCH_IIC_ADDRESS       0x40
#define TOUCH_CMD_SNAPSHOT      0x84
#define TOUCH_PACKET_BYTES      16      // x1,y1,z1,gh,x2,y2,z2,dm (BE words)

#define TOUCH_MSTPCR2           0xA4150038
#define TOUCH_IIC_STOP          (1u << 11)
#define TOUCH_PJCR              0xA4050110
#define TOUCH_PRCR              0xA405011C

#define TOUCH_PRDR_CONTACT      0x20    // Port R bit 5, 0 = pressed

// The sensor's power, as Casio's OS switches it (OS 02.01.7003): its
// power-down 0x8008AA1C clears 0xA405019E bit 5 and sets PHDR bit 4; its
// power-up 0x8008AA5C clears PHDR bit 4, waits 20 ms, sends the one-byte
// command 0x02 (0x8008A9FC -> 0x8008A7CC(2)) and sets 0xA405019E bit 5.
// The OS's power-off routine 0x800813A0 sets PHDR bit 4 (and waits 40 ms)
// before the standby; its touch task powers the sensor up again afterwards.
#define TOUCH_PHDR              0xA405012E
#define TOUCH_PHDR_OFF          0x10    // PH4: 1 = sensor off
#define TOUCH_X19E              0xA405019E
#define TOUCH_X19E_ON           0x20
#define TOUCH_CMD_START         0x02
#define TOUCH_POWER_UP_MS       20

// What the OS's touch start-up (0x8008AB30) does after that power-up, and
// the sensor needs before it reports anything: seven or eight one-byte
// registers written (0x8008A690: start, address, register, value, stop),
// then the status register polled until bit 0 is clear, the command 0xA8,
// the status polled until bit 1 is set (10 ms a poll, 20 polls, 40 on the
// hardware variant below), the whole pass a second time from the power-down,
// and at the end the command 0x80 (0x8008A9DC). The values: a table at
// 0x808161E8 (bytes 0, 1, 2, 11, 12) for registers 0x00, 0x08, 0x10, 0x58,
// 0x60; two words of the OS's RAM (0x8C1BE978, 0x8C1BE97C) for 0x18 and
// 0x20, which CE cannot know: the driver reads all of them back from the
// sensor while it still has the OS's set-up (classpad_touch.cpp) and
// writes back what it read. Register 0x34 = 1 only on the hardware variant
// that reads Port Q bit 0 low (0x800640DE).
#define TOUCH_REG_STATUS        0x68
#define TOUCH_STATUS_BUSY       0x01
#define TOUCH_STATUS_DONE       0x02
#define TOUCH_CMD_CALIBRATE     0xA8
#define TOUCH_CMD_RUN           0x80
#define TOUCH_CFG_REGS          { 0x00, 0x08, 0x10, 0x34, 0x18, 0x20, 0x58, 0x60 }
#define TOUCH_CFG_OS_VALUES     { 0x20, 0xA6, 0x5A, 0x01, 0x00, 0x00, 0x52, 0x77 }   // 0x18 and 0x20: not in the ROM
#define TOUCH_CFG_OS_KNOWN      0xC7    // bit n: entry n of the two lists is the ROM's (not 0x34, 0x18, 0x20)
#define TOUCH_CFG_N             8
#define TOUCH_CFG_IDX_X34       3
#define TOUCH_CFG_IDX_X18       4
#define TOUCH_CFG_IDX_X20       5
#define TOUCH_VARIANT_PORT      0xA405013A      // bit 0 low: the variant with register 0x34 and the longer polls
#define TOUCH_POLL_MS           10
#define TOUCH_POLLS             20
#define TOUCH_POLLS_VARIANT     40

// casio-emu's stylus to raw mapping (classpadSetTouch) for the registry
// calibration data and the hello window's annotation
#define TOUCH_RAW_X0            0x20B
#define TOUCH_RAW_XSPAN         0x9B6
#define TOUCH_RAW_Y0            0xF4
#define TOUCH_RAW_YSPAN         0x66F
#define TOUCH_PANEL_WIDTH       320
#define TOUCH_PANEL_HEIGHT      576     // LCD rows 0..527 + icon strip 528..575

#endif
