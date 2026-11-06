//------------------------------------------------------------------------------
//  bsp_cfg.h - clocks and timer constants of the SH7305 calculators
//------------------------------------------------------------------------------
#ifndef __BSP_CFG_H
#define __BSP_CFG_H

#define BSP_DEVICE_PREFIX           "CLASSPAD"

// Peripheral clock Pphi as measured in casio-emu and on the fx-CG50 family:
// 29.4912 MHz (the TMU counts Pphi/4 at the smallest prescaler).
// NOT the fx-CP400's: on the board the TMU counts about 1.67x faster than
// this (classpad_dbtstat.exe tick/rtc 1.69 over 38 s on 2026-11-06, FRQCR
// 0x18001102), so CE time runs that much too fast until this is corrected
// from a longer tick/rtc run or from FRQCR/PLLCR/FLLFRQ (HC_GETSTAT 82/384/385).
#define PERIPHERAL_CLOCK_FREQ       29491200
#define TIMER_INPUT_DIVISOR         4
#define TIMER_INPUT_DIVISOR_CODE    0x0000          // TCR.TPSC = Pphi/4
#define COUNT_FREQUENCY             (PERIPHERAL_CLOCK_FREQ / TIMER_INPUT_DIVISOR)   // 7372800 counts/s

// System tick (OALLIB\timer.c): the longest period between two timer
// interrupts. 1 = a fixed 1 ms tick (what the image had until 2026-11-06);
// anything larger is the variable tick, where the timer only fires at the
// kernel's next reschedule time, at most this many ms apart. TICK_MARGIN is
// the least distance, in counts, a reprogrammed underflow is set ahead of
// now: the ISR latency under the translator (an exception round trip plus a
// possible translation) must fit in it. 4096 counts = 0.56 ms.
#define TICK_MAX_MSEC               1000
#define TICK_MARGIN_COUNTS          4096

#endif
