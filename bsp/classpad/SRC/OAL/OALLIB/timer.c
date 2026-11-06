//------------------------------------------------------------------------------
//  timer.c - system tick on TMU0 with a variable period ("tickless")
//
//  TMU0 counts Pphi/4 (7372800 Hz) down and reloads TCOR0 on underflow;
//  the underflow interrupts through INTEVT 0x400. Register layout and the
//  pre-swapped access convention were verified by the translator's tmu guest.
//
//  Under the translator every interrupt is expensive (the exception path
//  recovers the guest pc from the host pc, undoes open spills, resolves the
//  handler and later the rte, several hundred host instructions before the
//  kernel's own ISR work), so the tick runs only when the kernel needs one:
//  the kernel passes its next wake-up time to pfnUpdateReschedTime whenever
//  it changes (variable tick, PLATFORM\COMMON\SRC\COMMON\TIMER\VARTICK) and
//  the ISR programs the period to the next reschedule time, at most
//  TICK_MAX_MSEC. CurMSec then advances by the actual period of each tick
//  and OEMGetTickCount (oal_timer.lib counts.c) adds the counts since the
//  tick, so GetTickCount keeps its 1 ms resolution. With TICK_MAX_MSEC 1
//  the same code is a fixed 1 ms tick (bsp_cfg.h).
//
//  Invariant kept by every write of TCNT0: counts elapsed since the last
//  tick = actualCountsPerSysTick - 1 - TCNT0. A period is always programmed
//  relative to the last tick (the ISR measures its own latency from the
//  reloaded counter), so no time is lost between periods.
//------------------------------------------------------------------------------
#include <bsp.h>

static UINT32 g_maxCounts;      // TCOR0 + 1: the reload period, TICK_MAX_MSEC worth of counts

//------------------------------------------------------------------------------
//  Program the current period: `msec` ms counted from the last tick, of
//  which `elapsed` counts have already passed. The underflow is kept at
//  least countsMargin ahead by lengthening the period in whole ms, so
//  actualCountsPerSysTick stays a whole number of ms. Interrupts masked, or
//  more than 1 ms before the programmed underflow (OALTimerUpdateRescheduleTime).
//
static void SetPeriod(UINT32 msec, UINT32 elapsed)
{
    UINT32 counts;

    if (msec > g_oalTimer.msecPerSysTick) msec = g_oalTimer.msecPerSysTick;
    counts = msec * g_oalTimer.countsPerMSec;
    if (counts < elapsed + g_oalTimer.countsMargin) {
        msec   = (elapsed + g_oalTimer.countsMargin + g_oalTimer.countsPerMSec - 1) / g_oalTimer.countsPerMSec;
        counts = msec * g_oalTimer.countsPerMSec;
    }
    g_oalTimer.actualMSecPerSysTick   = msec;
    g_oalTimer.actualCountsPerSysTick = counts;
    MMIO_W32(TMU_TCNT0, counts - elapsed - 1);
}

//------------------------------------------------------------------------------
BOOL OALTimerInit(UINT32 msecPerSysTick, UINT32 countsPerMSec, UINT32 countsMargin)
{
    OALMSG(OAL_FUNC, (L"+OALTimerInit(%d, %d, %d)\r\n", msecPerSysTick, countsPerMSec, countsMargin));

    if (msecPerSysTick == 0) msecPerSysTick = 1;
    if (msecPerSysTick > 0x7FFFFFFF / countsPerMSec) msecPerSysTick = 0x7FFFFFFF / countsPerMSec;

    g_oalTimer.countsPerMSec          = countsPerMSec;
    g_oalTimer.countsMargin           = countsMargin;
    g_oalTimer.maxPeriodMSec          = msecPerSysTick;
    g_oalTimer.msecPerSysTick         = msecPerSysTick;       // the longest period: the kernel asks for shorter ones
    g_oalTimer.countsPerSysTick       = countsPerMSec * msecPerSysTick;
    g_oalTimer.actualMSecPerSysTick   = 1;                    // the first period is 1 ms; the ISR takes it from there
    g_oalTimer.actualCountsPerSysTick = countsPerMSec;
    g_oalTimer.curCounts              = 0;
    g_maxCounts                       = g_oalTimer.countsPerSysTick;

    idleconv = countsPerMSec;
    curridlehigh = curridlelow = 0;

    pQueryPerformanceFrequency = OALTimerQueryPerformanceFrequency;
    pQueryPerformanceCounter   = OALTimerQueryPerformanceCounter;
    // Variable tick: the kernel reports its next wake-up time (kcalls.c
    // KCNextThread, SleepOneMore) and uses OEMGetTickCount for its tick reads.
    if (msecPerSysTick > 1) g_pOemGlobal->pfnUpdateReschedTime = OALTimerUpdateRescheduleTime;

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
//  Timer ISR: runs in bank 0 with SR.IMASK = IRQ_TMU0_PRIORITY
//
UINT32 OALTimerIntrHandler()
{
    UINT32 sysIntr = SYSINTR_NOP, late, msec;

    // Acknowledge: clear UNF (the counter has reloaded TCOR0 and counts on)
    MMIO_W16(TMU_TCR0, MMIO_R16(TMU_TCR0) & ~TMU_TCR_UNF);

    // Counts since the underflow = our latency: the next period starts at
    // the tick, not now, so it is already elapsed time of that period.
    late = g_maxCounts - 1 - MMIO_R32(TMU_TCNT0);

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
    SetPeriod(msec, late);
    return sysIntr;
}

//------------------------------------------------------------------------------
//  The kernel's next wake-up time changed (variable tick only). Same logic
//  as the common VARTICK library: leave it to the ISR when a tick is less
//  than 1 ms away (it cannot interrupt the reprogramming then).
//
VOID OALTimerUpdateRescheduleTime(DWORD time)
{
    UINT32 baseMSec = CurMSec, diffMSec;
    INT32  elapsed, toTick;

    if (time == baseMSec + g_oalTimer.actualMSecPerSysTick) return;    // already set up

    elapsed = OALTimerCountsSinceSysTick();
    toTick  = (INT32)g_oalTimer.actualCountsPerSysTick - elapsed;
    if (baseMSec != CurMSec || toTick < (INT32)g_oalTimer.countsPerMSec) return;

    diffMSec = time - baseMSec;
    if ((INT32)diffMSec < 0) diffMSec = 0;        // a thread on its way to sleep was preempted: tick as soon as allowed
    SetPeriod(diffMSec, (UINT32)elapsed);
}

//------------------------------------------------------------------------------
//  Counts elapsed since the last tick (OEMGetTickCount, QPC, OEMIdle)
//
INT32 OALTimerCountsSinceSysTick()
{
    INT32 v = (INT32)(g_oalTimer.actualCountsPerSysTick - 1 - MMIO_R32(TMU_TCNT0));
    // The counter underflowed and reloaded while interrupts were masked (the
    // ISR has not run): the period is over, do not go backwards.
    if (v < 0) v = (INT32)g_oalTimer.actualCountsPerSysTick - 1;
    return v;
}

//------------------------------------------------------------------------------
//  OEMIdle: entered with interrupts blocked (SR.BL=1). sleep accepts
//  interrupts regardless of BL (the kernel's SH idle code advances SPC past
//  the sleep); the TMU keeps running and the ISR has run when sleep returns.
//
void OEMIdle(DWORD idleParam)
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
