//------------------------------------------------------------------------------
//  timer.c - 1 ms system tick on TMU0 (fixed tick, count-down timer)
//
//  TMU0 counts Pphi/4 (7372800 Hz) from TCOR0 down to 0 and reloads;
//  underflow interrupts through INTEVT 0x400. Register layout and the
//  pre-swapped access convention were verified by the translator's tmu guest.
//------------------------------------------------------------------------------
#include <bsp.h>

//------------------------------------------------------------------------------
BOOL OALTimerInit(UINT32 msecPerSysTick, UINT32 countsPerMSec, UINT32 countsMargin)
{
    OALMSG(OAL_FUNC, (L"+OALTimerInit(%d, %d, %d)\r\n", msecPerSysTick, countsPerMSec, countsMargin));

    g_oalTimer.countsPerMSec          = countsPerMSec;
    g_oalTimer.countsMargin           = countsMargin;
    g_oalTimer.maxPeriodMSec          = 0xFFFFFFFF / countsPerMSec;
    g_oalTimer.msecPerSysTick         = msecPerSysTick;
    g_oalTimer.countsPerSysTick       = countsPerMSec * msecPerSysTick;
    g_oalTimer.actualMSecPerSysTick   = msecPerSysTick;
    g_oalTimer.actualCountsPerSysTick = g_oalTimer.countsPerSysTick;
    g_oalTimer.curCounts              = 0;

    idleconv = countsPerMSec;
    curridlehigh = curridlelow = 0;

    pQueryPerformanceFrequency = OALTimerQueryPerformanceFrequency;
    pQueryPerformanceCounter   = OALTimerQueryPerformanceCounter;

    // Stop channel 0, program it, clear a stale underflow, unmask, start
    MMIO_W8(TMU_TSTR, MMIO_R8(TMU_TSTR) & ~TMU_TSTR_STR0);
    MMIO_W16(TMU_TCR0, TMU_TCR_UNIE | TIMER_INPUT_DIVISOR_CODE);
    MMIO_W32(TMU_TCOR0, g_oalTimer.countsPerSysTick - 1);
    MMIO_W32(TMU_TCNT0, g_oalTimer.countsPerSysTick - 1);
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
    // Acknowledge: clear UNF (the counter has already reloaded)
    MMIO_W16(TMU_TCR0, MMIO_R16(TMU_TCR0) & ~TMU_TCR_UNF);

    CurMSec              += g_oalTimer.actualMSecPerSysTick;
    g_oalTimer.curCounts += g_oalTimer.actualCountsPerSysTick;

    if ((INT32)(CurMSec - dwReschedTime) >= 0) return SYSINTR_RESCHED;
    return SYSINTR_NOP;
}

//------------------------------------------------------------------------------
//  Counts elapsed since the last tick (used by OEMGetTickCount and QPC)
//
INT32 OALTimerCountsSinceSysTick()
{
    return (INT32)(g_oalTimer.actualCountsPerSysTick - 1 - MMIO_R32(TMU_TCNT0));
}

//------------------------------------------------------------------------------
//  OEMIdle: entered with interrupts blocked (SR.BL=1). sleep accepts
//  interrupts regardless of BL; the TMU keeps running.
//
void OEMIdle(DWORD idleParam)
{
    UINT32 before, after, idle;
    ULARGE_INTEGER currIdle;

    currIdle.LowPart  = curridlelow;
    currIdle.HighPart = curridlehigh;

    before = MMIO_R32(TMU_TCNT0);
    __asm("sleep");
    after = MMIO_R32(TMU_TCNT0);

    // fixed tick: at most one period passed in sleep
    idle = (before >= after) ? (before - after) : (before + g_oalTimer.actualCountsPerSysTick - after);
    currIdle.QuadPart += idle;
    curridlelow  = currIdle.LowPart;
    curridlehigh = currIdle.HighPart;
}

//------------------------------------------------------------------------------
//  Busy wait in microseconds (timer must be running)
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
        else if (base < cur)  delta = g_oalTimer.actualCountsPerSysTick - cur + base;
        else                  delta = 0;
        elapsed += delta;
        base = cur;
    }
}
