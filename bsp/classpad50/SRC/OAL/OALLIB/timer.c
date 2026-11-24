//------------------------------------------------------------------------------
//  timer.c - system tick on TMU0 with a variable period ("tickless"), CE 5.0
//
//  TMU0 counts Pphi/4 (7372800 Hz at the stock clock; InitClock measures the
//  real rate against the RTC) down and reloads TCOR0 on underflow;
//  the underflow interrupts through INTEVT 0x400. Register layout and the
//  pre-swapped access convention were verified by the translator's tmu guest.
//
//  Under the translator every interrupt is expensive (the exception path
//  recovers the guest pc from the host pc, undoes open spills, resolves the
//  handler and later the rte, several hundred host instructions before the
//  kernel's own ISR work), so the tick runs only when the kernel needs one:
//  the kernel passes its next wake-up time to pOEMUpdateRescheduleTime
//  whenever it changes (CE 5.0's variable tick, as PLATFORM\ASPEN\KERNEL\HAL\
//  vartick.c does it) and the ISR programs the period to the next reschedule
//  time, at most TICK_MAX_MSEC. CurMSec then advances by the actual period of
//  each tick and SC_GetTickCount (oalcommon.c) adds the counts since the
//  tick, so GetTickCount keeps its 1 ms resolution. With TICK_MAX_MSEC 1 the
//  same code is a fixed 1 ms tick (bsp_cfg.h).
//
//  CE 5.0 contract (nkintr.h, kwin32.c): InitClock() programs the timer and
//  is called by OEMInit and by the kernel right after OEMPowerOff (the TMU
//  lost its registers in R-standby); OEMIdle(DWORD) sleeps until the next
//  interrupt and accounts the idle time in curridlelow/high (GetIdleTime);
//  SC_GetTickCount is the kernel's GetTickCount.
//
//  Invariant kept by every write of TCNT0: counts elapsed since the last
//  tick = actualCountsPerSysTick - 1 - TCNT0. A period is always programmed
//  relative to the last tick (the ISR measures its own latency from the
//  reloaded counter). Reprogramming briefly stops TMU0 for a coherent update;
//  that stopped interval is not counted and still needs device drift measurement.
//------------------------------------------------------------------------------
#if defined(_MSC_VER) && !defined(DEBUG)
#pragma optimize("t", on)
#endif
#ifndef CLASSPAD_TIMER_HOST_TEST
#include <bsp.h>
#endif

static UINT32 g_maxCounts;      // TCOR0 + 1: the reload period, TICK_MAX_MSEC worth of counts
static UINT32 g_invCountsPerMSec; // floor(2^32 / countsPerMSec), or 0xFFFFFFFF when countsPerMSec == 1

// Exact unsigned counts / g_oalTimer.countsPerMSec via 32x32->64 multiply
// (avoids software division on SH-4 in SC_GetTickCount and SetPeriod).
UINT32 OALTimerCountsToMSec(UINT32 counts)
{
    UINT32 d = g_oalTimer.countsPerMSec;
    UINT32 q;
    if (d == 0) return 0;
    q = (UINT32)(((UINT64)counts * g_invCountsPerMSec) >> 32);
    if (counts - q * d >= d) q++;
    return q;
}

//------------------------------------------------------------------------------
//  Program `msec` ms from the last tick, using counterBase to measure
//  elapsed counts at the programming point. The underflow is kept at
//  least countsMargin ahead by lengthening the period in whole ms, so
//  actualCountsPerSysTick stays a whole number of ms. Interrupts are masked.
//
static void SetPeriod(UINT32 msec, UINT32 counterBase)
{
    UINT32 counts, elapsed;
    UINT8 start;

    if (msec > g_oalTimer.msecPerSysTick) msec = g_oalTimer.msecPerSysTick;
    counts = msec * g_oalTimer.countsPerMSec;
    // Freeze channel 0 before checking UNF or changing the software period.
    // Masking CPU interrupts alone does not stop the hardware: an underflow
    // between the last UNF read and TCNT write would be charged as the NEW
    // period (up to 1000 ms) although the OLD short period just elapsed.
    // Preserve the other TMU channels (the translator's benchmarks use TMU1).
    start = MMIO_R8(TMU_TSTR);
    MMIO_W8(TMU_TSTR, start & ~TMU_TSTR_STR0);
    (void)MMIO_R8(TMU_TSTR); // complete the stop before the status snapshot
    if (MMIO_R16(TMU_TCR0) & TMU_TCR_UNF) {
        MMIO_W8(TMU_TSTR, start);
        return; // leave the old period and pending interrupt to the ISR
    }
    elapsed = counterBase - 1 - MMIO_R32(TMU_TCNT0);
    if (counts < elapsed + g_oalTimer.countsMargin) {
        msec   = OALTimerCountsToMSec(elapsed + g_oalTimer.countsMargin + g_oalTimer.countsPerMSec - 1);
        counts = msec * g_oalTimer.countsPerMSec;
    }
    g_oalTimer.actualMSecPerSysTick   = msec;
    g_oalTimer.actualCountsPerSysTick = counts;
    MMIO_W32(TMU_TCNT0, counts - elapsed - 1);
    MMIO_W8(TMU_TSTR, start);
}

//------------------------------------------------------------------------------
BOOL OALTimerInit(UINT32 msecPerSysTick, UINT32 countsPerMSec, UINT32 countsMargin)
{
    static BOOL s_once;

    OALMSG(OAL_FUNC, (L"+OALTimerInit(%d, %d, %d)\r\n", msecPerSysTick, countsPerMSec, countsMargin));

    if (msecPerSysTick == 0) msecPerSysTick = 1;
    if (msecPerSysTick > 0x7FFFFFFF / countsPerMSec) msecPerSysTick = 0x7FFFFFFF / countsPerMSec;

    g_oalTimer.countsPerMSec          = countsPerMSec;
    g_invCountsPerMSec                = (countsPerMSec <= 1) ? 0xFFFFFFFFu : (UINT32)(((UINT64)1 << 32) / countsPerMSec);
    g_oalTimer.countsMargin           = countsMargin;
    g_oalTimer.maxPeriodMSec          = msecPerSysTick;
    g_oalTimer.msecPerSysTick         = msecPerSysTick;       // the longest period: the kernel asks for shorter ones
    g_oalTimer.countsPerSysTick       = countsPerMSec * msecPerSysTick;
    g_oalTimer.actualMSecPerSysTick   = 1;                    // the first period is 1 ms; the ISR takes it from there
    g_oalTimer.actualCountsPerSysTick = countsPerMSec;
    g_maxCounts                       = g_oalTimer.countsPerSysTick;

    if (!s_once) {
        // once per boot: the idle statistics and the kernel's function
        // pointers (a re-init after OEMPowerOff keeps CurMSec and curCounts)
        s_once = TRUE;
        g_oalTimer.curCounts = 0;
        idleconv = countsPerMSec;
        curridlehigh = curridlelow = 0;
        pQueryPerformanceFrequency = OALTimerQueryPerformanceFrequency;
        pQueryPerformanceCounter   = OALTimerQueryPerformanceCounter;
        // Variable tick: the kernel reports its next wake-up time (schedule.c
        // NextThread / SleepOneMore) and reads the tick through SC_GetTickCount.
        if (msecPerSysTick > 1) pOEMUpdateRescheduleTime = OALTimerUpdateRescheduleTime;
    }

    // Stop channel 0, program it, clear a stale underflow, unmask, start
    MMIO_W8(TMU_TSTR, MMIO_R8(TMU_TSTR) & ~TMU_TSTR_STR0);
    MMIO_W16(TMU_TCR0, TMU_TCR_UNIE | TIMER_INPUT_DIVISOR_CODE);
    MMIO_W32(TMU_TCOR0, g_maxCounts - 1);
    MMIO_W32(TMU_TCNT0, g_oalTimer.actualCountsPerSysTick - 1);
    MMIO_W8(INTC_IMCR(4), INTC_IMR4_TMU0);
    MMIO_W8(TMU_TSTR, MMIO_R8(TMU_TSTR) | TMU_TSTR_STR0);

    OALMSG(OAL_FUNC, (L"-OALTimerInit()\r\n"));
    return TRUE;
}

//------------------------------------------------------------------------------
//  The TMU's count rate, measured. Pphi is not a constant of the board: the
//  clock generator's settings are whatever Casio's OS, or an overclocking
//  tool run before CE, left (bsp_cfg.h's 29.4912 MHz is the stock value), and
//  every time CE keeps (the tick, GetTickCount, Sleep, the performance
//  counter, OALStall) is counted in TMU0 counts. The RTC has its own 32.768
//  kHz crystal, so its 128 Hz counter R64CNT is the reference: TMU0 runs
//  free over CALIBRATE_RTC_TICKS of it (125 ms), started and stopped on a
//  count edge. Returns counts per millisecond, or `nominal` when R64CNT does
//  not count or the result is outside nominal / 8 .. * 8. (RCR2.START is not
//  consulted: the 128 Hz divider is what matters, and casio-emu runs it with
//  START clear.)
//  Interrupts are off (OEMInit); TMU0 is left stopped.
//
#define CALIBRATE_RTC_TICKS     16
#define CALIBRATE_MAX_READS     1000000         // gives up when R64CNT does not move (a count lasts 7.8 ms)

static UINT32 g_countsPerMSec;                  // 0 until measured
static UINT32 g_calibratedHz;                   // the measured count rate, 0 = nominal in use

static BOOL WaitRtcEdge(UINT8 *last)
{
    UINT32 reads;
    UINT8 now;
    for (reads = 0; reads < CALIBRATE_MAX_READS; reads++) {
        now = (UINT8)(MMIO_R8(RTC_R64CNT) & 0x7F);
        if (now != *last) { *last = now; return TRUE; }
    }
    return FALSE;
}

static UINT32 CalibrateCountsPerMSec(UINT32 nominal)
{
    UINT32 start, end, counts, perMSec, ticks = 0;
    UINT8 last, prev;

    g_calibratedHz = 0;
    MMIO_W8(TMU_TSTR, MMIO_R8(TMU_TSTR) & ~TMU_TSTR_STR0);
    MMIO_W16(TMU_TCR0, TIMER_INPUT_DIVISOR_CODE);                   // no interrupt
    MMIO_W32(TMU_TCOR0, 0xFFFFFFFF);
    MMIO_W32(TMU_TCNT0, 0xFFFFFFFF);
    MMIO_W8(TMU_TSTR, MMIO_R8(TMU_TSTR) | TMU_TSTR_STR0);
    last = (UINT8)(MMIO_R8(RTC_R64CNT) & 0x7F);
    if (!WaitRtcEdge(&last)) goto fail;
    start = MMIO_R32(TMU_TCNT0);
    while (ticks < CALIBRATE_RTC_TICKS) {
        prev = last;
        if (!WaitRtcEdge(&last)) goto fail;
        ticks += (UINT8)((last - prev) & 0x7F);                     // a slow read may see two counts at once
    }
    end = MMIO_R32(TMU_TCNT0);
    MMIO_W8(TMU_TSTR, MMIO_R8(TMU_TSTR) & ~TMU_TSTR_STR0);
    counts = start - end;                                           // TMU0 counts down
    // counts in ticks/128 s -> per ms, rounded
    perMSec = (UINT32)(((UINT64)counts * 128 + (UINT64)ticks * 500) / ((UINT64)ticks * 1000));
    if (perMSec < nominal / 8 || perMSec > nominal * 8) return nominal;
    g_calibratedHz = (UINT32)(((UINT64)counts * 128) / ticks);
    return perMSec;
fail:
    MMIO_W8(TMU_TSTR, MMIO_R8(TMU_TSTR) & ~TMU_TSTR_STR0);
    return nominal;
}

//------------------------------------------------------------------------------
//  InitClock: the kernel's name for it (called with interrupts off from
//  OEMInit, and from CallOEMPowerOff after OEMPowerOff returns). The rate is
//  measured once per boot: a resume keeps it (the translator restores the
//  clock generator it saved, and the measurement costs 125 ms).
//
//
//  The counts kept across a rate change (the performance counter's base and
//  the idle total) are rescaled, so that counts / frequency stays continuous;
//  a program that cached QueryPerformanceFrequency keeps the old rate.
//
static UINT64 TimerRescale(UINT64 counts, UINT32 from, UINT32 to)
{
    UINT64 whole = counts / from;
    return whole * to + (counts - whole * from) * to / from;
}

static void RateChanged(UINT32 from, UINT32 to)
{
    ULARGE_INTEGER idle;

    if (from == 0 || from == to) return;
    g_oalTimer.curCounts = TimerRescale(g_oalTimer.curCounts, from, to);
    idle.LowPart = curridlelow; idle.HighPart = curridlehigh;
    idle.QuadPart = TimerRescale(idle.QuadPart, from, to);
    curridlelow = idle.LowPart; curridlehigh = idle.HighPart;
    idleconv = to;
}

void InitClock(void)
{
    // A resume that comes back with other clock settings than the OAL last
    // saw (OALClockChanged, ioctl.c) is measured again like a boot.
    BOOL moved = OALClockChanged();
    UINT32 old = g_countsPerMSec;

    if (g_countsPerMSec == 0 || moved) {
        g_countsPerMSec = CalibrateCountsPerMSec(COUNT_FREQUENCY / 1000);
        RateChanged(old, g_countsPerMSec);
        if (g_calibratedHz)
            RETAILMSG(1, (L"CLASSPAD OAL: TMU0 counts %u Hz by the RTC (nominal %u): %u counts per ms\r\n",
                          g_calibratedHz, COUNT_FREQUENCY, g_countsPerMSec));
        else
            RETAILMSG(1, (L"CLASSPAD OAL: TMU0 rate not measured (the RTC does not count, or implausible), nominal %u Hz\r\n", COUNT_FREQUENCY));
    }
    OALTimerInit(TICK_MAX_MSEC, g_countsPerMSec, TICK_MARGIN_COUNTS);
}

//------------------------------------------------------------------------------
//  IOCTL_HAL_CLASSPAD_CLOCK_SET (ioctl.c) changed Pphi by the factor mul/div:
//  the count rate follows by exactly that factor (the dividers are integers),
//  so the measured rate is scaled instead of measured again (125 ms with the
//  timer stopped). The period in flight is closed at the old rate: its whole
//  milliseconds go to CurMSec, the rest (< 1 ms) and the time the switch
//  itself took are dropped. Interrupts are off; the tick restarts with a
//  1 ms period and the ISR takes it from there.
//
VOID OALTimerRetune(UINT32 mul, UINT32 div)
{
    UINT32 old = g_oalTimer.countsPerMSec, hz, perMSec, elapsedMSec;

    if (old == 0 || mul == 0 || div == 0 || mul == div) return;
    hz = (UINT32)((UINT64)(g_calibratedHz ? g_calibratedHz : COUNT_FREQUENCY) * mul / div);
    perMSec = (hz + 500) / 1000;
    if (perMSec == 0) return;

    MMIO_W8(TMU_TSTR, MMIO_R8(TMU_TSTR) & ~TMU_TSTR_STR0);
    elapsedMSec = (UINT32)OALTimerCountsSinceSysTick() / old;
    CurMSec += elapsedMSec;
    g_oalTimer.curCounts += (UINT64)elapsedMSec * old;
    RateChanged(old, perMSec);
    g_calibratedHz  = hz;
    g_countsPerMSec = perMSec;
    OALTimerInit(TICK_MAX_MSEC, perMSec, TICK_MARGIN_COUNTS);
}

//  The TMU0 count rate in Hz (Pphi / 4), as measured or scaled
UINT32 OALTimerCountHz(void)
{
    return g_calibratedHz ? g_calibratedHz : COUNT_FREQUENCY;
}

//  Pphi in Hz, now, for whatever divides it (the touch driver's IIC0 clock,
//  classpad_iic.h; cetune's display): four times the TMU0 rate (TCR.TPSC =
//  Pphi/4), which is the rate measured at boot and, after a clock change
//  inside CE, what OALTimerRetune scaled it to.
UINT32 OALClockPphiHz(void)
{
    return OALTimerCountHz() * 4;
}

#ifndef CLASSPAD_TIMER_HOST_TEST
BOOL OALIoCtlHalClasspadPphi(UINT32 code, VOID *in, UINT32 insize,
    VOID *out, UINT32 outsize, UINT32 *returned)
{
    CLASSPAD_PPHI *clock = (CLASSPAD_PPHI *)out;
    if (returned) *returned = 0;
    if (!clock || outsize < sizeof *clock) return FALSE;
    clock->pphiHz   = OALClockPphiHz();
    clock->measured = g_calibratedHz != 0;
    if (returned) *returned = sizeof *clock;
    return TRUE;
}
#endif

//------------------------------------------------------------------------------
//  Timer ISR: runs in bank 0 with SR.IMASK = IRQ_TMU0_PRIORITY
//
extern BOOL ClasspadPowerTimerIsr(void);
#ifndef CLASSPAD_TIMER_HOST_TEST
extern volatile BOOL g_classpadPowerSleeping;
#endif
UINT32 OALTimerIntrHandler(void)
{
    UINT32 sysIntr = SYSINTR_NOP, msec, tcr;

#ifdef CLASSPAD_TIMER_HOST_TEST
    if (ClasspadPowerTimerIsr()) return SYSINTR_NOP;
#else
    if (g_classpadPowerSleeping && ClasspadPowerTimerIsr()) return SYSINTR_NOP;
#endif
    tcr = MMIO_R16(TMU_TCR0);
    if (!(tcr & TMU_TCR_UNF)) return SYSINTR_NOP;
    // Acknowledge: clear UNF (the counter has reloaded TCOR0 and counts on)
    MMIO_W16(TMU_TCR0, tcr & ~TMU_TCR_UNF);

    // Counts since the underflow = our latency: the next period starts at
    // the tick, not now, so it is already elapsed time of that period.

    CurMSec              += g_oalTimer.actualMSecPerSysTick;
    g_oalTimer.curCounts += g_oalTimer.actualCountsPerSysTick;

    if ((INT32)(CurMSec - dwReschedTime) >= 0) {
        // the scheduler runs and tells us the next time through
        // OALTimerUpdateRescheduleTime; until then the longest period
        sysIntr = SYSINTR_RESCHED;
        msec = g_oalTimer.msecPerSysTick;
    } else {
        msec = dwReschedTime - CurMSec;
    }
    if (msec >= g_oalTimer.msecPerSysTick && g_maxCounts > g_oalTimer.countsMargin + g_oalTimer.countsPerMSec) {
        // Hardware already reloaded TCNT0 = TCOR0 = g_maxCounts - 1 on
        // underflow, so avoid stopping/reprogramming TMU0 for a full period.
        g_oalTimer.actualMSecPerSysTick   = g_oalTimer.msecPerSysTick;
        g_oalTimer.actualCountsPerSysTick = g_maxCounts;
    } else {
        SetPeriod(msec, g_maxCounts);
    }
    return sysIntr;
}

//------------------------------------------------------------------------------
//  The kernel's next wake-up time changed (variable tick only). Same logic
//  as the common VARTICK library: leave it to the ISR when a tick is less
//  than 1 ms away (it cannot interrupt the reprogramming then).
//
VOID OALTimerUpdateRescheduleTime(DWORD time)
{
    BOOL enabled = INTERRUPTS_ENABLE(FALSE);
    UINT32 baseMSec = CurMSec, diffMSec;
    INT32  elapsed, toTick;

    diffMSec = time - baseMSec;
    if ((INT32)diffMSec < 0) diffMSec = 0;        // a thread on its way to sleep was preempted: tick as soon as allowed
    else if (diffMSec > g_oalTimer.msecPerSysTick) diffMSec = g_oalTimer.msecPerSysTick;
    if (diffMSec == g_oalTimer.actualMSecPerSysTick) goto done;    // already set up

    elapsed = OALTimerCountsSinceSysTick();
    toTick  = (INT32)g_oalTimer.actualCountsPerSysTick - elapsed;
    if (baseMSec != CurMSec || toTick < (INT32)g_oalTimer.countsPerMSec) goto done;

    SetPeriod(diffMSec, g_oalTimer.actualCountsPerSysTick);
done:
    INTERRUPTS_ENABLE(enabled);
}

//------------------------------------------------------------------------------
//  Counts elapsed since the last tick (SC_GetTickCount, QPC, OEMIdle)
//
INT32 OALTimerCountsSinceSysTick(void)
{
    UINT32 before, after, count;
    // Read a consistent side of the reload. The ISR has not accounted for
    // this period while UNF is pending; include both it and ISR latency.
    do {
        before = MMIO_R16(TMU_TCR0);
        count = MMIO_R32(TMU_TCNT0);
        after = MMIO_R16(TMU_TCR0);
    } while ((before ^ after) & TMU_TCR_UNF);
    if (after & TMU_TCR_UNF)
        return (INT32)(g_oalTimer.actualCountsPerSysTick + g_maxCounts - 1 - count);
    return (INT32)(g_oalTimer.actualCountsPerSysTick - 1 - count);
}

//------------------------------------------------------------------------------
//  OEMIdle: entered with interrupts blocked (SR.BL=1). sleep accepts
//  interrupts regardless of BL (the kernel's SH idle code advances SPC past
//  the sleep); the TMU keeps running and the ISR has run when sleep returns.
//
void OEMIdle(DWORD dwIdleParam)
{
    UINT32 baseMSec;
    INT32  usedCounts, idleCounts;
    ULARGE_INTEGER currIdle;

    static BOOL s_idleSeen;

    if (!s_idleSeen) {
        // first idle = every boot thread is blocked: with a retail kernel
        // the only sign that the boot has finished (goes through the ring: BL=1)
        s_idleSeen = TRUE;
        RETAILMSG(1, (L"CLASSPAD OAL: first idle\r\n"));
    }

    baseMSec   = CurMSec;
    usedCounts = OALTimerCountsSinceSysTick();
    idleCounts = (INT32)g_oalTimer.actualCountsPerSysTick;    // the period that will end the sleep

    __asm("sleep");

    if (CurMSec == baseMSec) idleCounts = OALTimerCountsSinceSysTick();   // woken by something else
    idleCounts -= usedCounts;
    if (idleCounts < 0) idleCounts = 0;

    currIdle.LowPart  = curridlelow;
    currIdle.HighPart = curridlehigh;
    currIdle.QuadPart += (UINT32)idleCounts;
    curridlelow  = currIdle.LowPart;
    curridlehigh = currIdle.HighPart;
}

//------------------------------------------------------------------------------
//  Busy wait in microseconds (timer must be running). TCNT0 wraps to TCOR0
//  after an underflow, whatever the period in force.
//
VOID OALStall(UINT32 microSec)
{
    UINT32 base, cur, delta;
    UINT64 wait, elapsed = 0;

    if (microSec == 0 || g_oalTimer.countsPerMSec == 0) return;
    wait = ((UINT64)microSec * g_oalTimer.countsPerMSec) / 1000;
    base = MMIO_R32(TMU_TCNT0);
    while (elapsed < wait) {
        cur = MMIO_R32(TMU_TCNT0);
        if (cur < base)       delta = base - cur;
        else if (base < cur)  delta = g_maxCounts - cur + base;
        else                  delta = 0;
        elapsed += delta;
        base = cur;
    }
}
