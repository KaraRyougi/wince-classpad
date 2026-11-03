//------------------------------------------------------------------------------
//  bsp_cfg.h - clocks and timer constants of the SH7305 calculators
//------------------------------------------------------------------------------
#ifndef __BSP_CFG_H
#define __BSP_CFG_H

#define BSP_DEVICE_PREFIX           "CLASSPAD"

// Peripheral clock Pphi as measured in casio-emu and on the fx-CG50 family:
// 29.4912 MHz (the TMU counts Pphi/4 at the smallest prescaler).
#define PERIPHERAL_CLOCK_FREQ       29491200
#define TIMER_INPUT_DIVISOR         4
#define TIMER_INPUT_DIVISOR_CODE    0x0000          // TCR.TPSC = Pphi/4
#define COUNT_FREQUENCY             (PERIPHERAL_CLOCK_FREQ / TIMER_INPUT_DIVISOR)   // 7372800 counts/s

#endif
