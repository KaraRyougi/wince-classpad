/* Execute the production variable-tick logic against a register model. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint32_t UINT32, DWORD;
typedef uint8_t UINT8;
typedef int32_t INT32;
typedef uint64_t UINT64;
typedef int BOOL;
typedef void VOID;
typedef union { struct { uint32_t LowPart, HighPart; }; uint64_t QuadPart; } ULARGE_INTEGER;
#define TRUE 1
#define FALSE 0
#define SYSINTR_NOP 0
#define SYSINTR_RESCHED 1
#define TIMER_INPUT_DIVISOR_CODE 0
#define TICK_MAX_MSEC 1000
#define OALMSG(...) ((void)0)
#define RETAILMSG(...) ((void)0)
#define __asm(...) ((void)0)
#include "../classpad50/SRC/INC/bsp_base_regs.h"
#define COUNT_FREQUENCY 7372000
#define TICK_MARGIN_COUNTS 4096
static struct {
    UINT32 countsPerMSec, countsMargin, maxPeriodMSec, msecPerSysTick;
    UINT32 countsPerSysTick, actualMSecPerSysTick, actualCountsPerSysTick;
    UINT64 curCounts;
} g_oalTimer;
static UINT32 CurMSec, dwReschedTime, idleconv, curridlehigh, curridlelow;
static void OALTimerQueryPerformanceFrequency(void) {}
static void OALTimerQueryPerformanceCounter(void) {}
static void (*pQueryPerformanceFrequency)(void), (*pQueryPerformanceCounter)(void);
static void (*pOEMUpdateRescheduleTime)(DWORD);   /* CE 5.0: the kernel variable, not an OEMGLOBAL field */
static UINT32 tcnt, tcor, tcr, tstr, programming_delay;
static int reload_on_read, reload_before_write;
static int suspended, power_calls, irq_enabled;
static BOOL INTERRUPTS_ENABLE(BOOL enabled) { BOOL old=irq_enabled; irq_enabled=enabled; return old; }
static BOOL ClasspadPowerTimerIsr(void) { ++power_calls; return suspended; }
static int clock_changed;
static BOOL OALClockChanged(void) { return clock_changed; }   /* ioctl.c: the CPG settings moved since the last look */
/* calibration model: every read of R64CNT or TCNT0 takes sim_read_us; the RTC counts 128 Hz, TMU0 sim_hz */
static UINT32 sim_hz, sim_read_us = 3, sim_rtc_start = 1, sim_rtc_dead;
static uint64_t sim_us;
static UINT32 readreg(UINT32 addr) {
    if (sim_hz) {
        if (addr == RTC_RCR2) return sim_rtc_start;
        if (addr == RTC_R64CNT) { sim_us += sim_read_us; return sim_rtc_dead ? 5 : (UINT32)((sim_us * 128 / 1000000) & 0x7F); }
        if (addr == TMU_TCNT0) { sim_us += sim_read_us; return (tstr & TMU_TSTR_STR0) ? tcnt - (UINT32)(sim_us * sim_hz / 1000000) : tcnt; }
    }
    if (addr == TMU_TCNT0) {
        if (reload_on_read) { reload_on_read=0; tcr |= TMU_TCR_UNF; tcnt=tcor-50; }
        return tcnt;
    }
    if (addr == TMU_TCOR0) return tcor;
    if (addr == TMU_TCR0) {
        if (programming_delay) { tcnt -= programming_delay; programming_delay=0; }
        return tcr;
    }
    if (addr == TMU_TSTR) return tstr;
    return 0;
}
static void writereg(UINT32 addr, UINT32 v) {
    if (addr == TMU_TCNT0) {
        // Hardware can underflow after the driver's final status read but
        // before its counter write; writing TCNT does not clear UNF.
        if (reload_before_write && (tstr & TMU_TSTR_STR0)) {
            reload_before_write=0; tcr |= TMU_TCR_UNF; tcnt=tcor;
        }
        tcnt = v;
    }
    if (addr == TMU_TCOR0) tcor = v;
    if (addr == TMU_TCR0) tcr = v;
    if (addr == TMU_TSTR) tstr = v;
}
#define MMIO_R32 readreg
#define MMIO_R16 readreg
#define MMIO_R8 readreg
#define MMIO_W32 writereg
#define MMIO_W16 writereg
#define MMIO_W8 writereg
VOID OALTimerUpdateRescheduleTime(DWORD time);
INT32 OALTimerCountsSinceSysTick(void);
#define CLASSPAD_TIMER_HOST_TEST
#include "../classpad50/SRC/OAL/OALLIB/timer.c"
static int failures;
#define CHECK(name, expr) do { if (!(expr)) { fprintf(stderr,"FAIL %s\n", name); ++failures; } } while (0)
static void init(void) {
    memset(&g_oalTimer,0,sizeof g_oalTimer); CurMSec=100; dwReschedTime=120;
    tcnt=tcor=tcr=tstr=0; suspended=0; power_calls=0;
    OALTimerInit(1000,7372,4096);
}
int main(void) {
    init();
    CHECK("variable-tick callback", pOEMUpdateRescheduleTime == OALTimerUpdateRescheduleTime);
    CHECK("initial 1ms / reload 1000ms", tcnt == 7371 && tcor == 7371999);
    tcr |= TMU_TCR_UNF; tcnt = tcor - 100;
    CHECK("pending underflow includes lateness", OALTimerCountsSinceSysTick() == 7472);
    CHECK("early IRQ does not schedule", OALTimerIntrHandler() == SYSINTR_NOP);
    CHECK("ISR advances exact previous period", CurMSec == 101 && g_oalTimer.curCounts == 7372);
    CHECK("next deadline and latency", g_oalTimer.actualMSecPerSysTick == 19 && OALTimerCountsSinceSysTick() == 100);
    tcnt = g_oalTimer.actualCountsPerSysTick - 1 - 7372;
    irq_enabled=1;
    OALTimerUpdateRescheduleTime(106);
    CHECK("callback restores interrupt state", irq_enabled == 1);
    CHECK("earlier deadline", g_oalTimer.actualMSecPerSysTick == 5 && OALTimerCountsSinceSysTick() == 7372);
    OALTimerUpdateRescheduleTime(100);
    CHECK("past deadline respects margin", tcnt + 1 >= 4096);
    init(); tcnt=0; reload_on_read=1;
    CHECK("read spanning a reload", OALTimerCountsSinceSysTick() == 7422);
    init(); tcnt=7371-100; programming_delay=200;
    SetPeriod(5,7372);
    CHECK("programming uses fresh counter", OALTimerCountsSinceSysTick() == 300);
    init(); suspended=1; tcnt=12345; tcr|=TMU_TCR_UNF;
    CHECK("suspend ISR bypasses scheduler", OALTimerIntrHandler() == SYSINTR_NOP && CurMSec == 100 && tcnt == 12345 && power_calls == 1);
    init(); tcnt=7371-100; reload_before_write=1;
    SetPeriod(1000,7372);
    CHECK("reprogram does not mislabel a pending old-period IRQ", !(tcr & TMU_TCR_UNF) || g_oalTimer.actualMSecPerSysTick == 1);
    reload_before_write=0;
    CHECK("reprogram preserves running channel", (tstr & TMU_TSTR_STR0) != 0);
    init();
    CHECK("stale IRQ does not advance time", OALTimerIntrHandler() == SYSINTR_NOP && CurMSec == 100);
    init(); tstr |= 6; tcr |= TMU_TCR_UNF; tcnt=tcor-50;
    SetPeriod(1000,7372);
    CHECK("pending IRQ retains old accounting", g_oalTimer.actualMSecPerSysTick==1 && (tcr & TMU_TCR_UNF));
    CHECK("other channels preserved", tstr==7);
    /* the rate measured against the RTC (InitClock) */
    {
        static const UINT32 rates[] = { 7372800, 3686400, 14745600, 9216000, 7000000 };
        unsigned i;
        for (i = 0; i < sizeof rates / sizeof rates[0]; i++) {
            UINT32 got, want = (rates[i] + 500) / 1000;
            sim_hz = rates[i]; sim_us = 123457; sim_rtc_start = 1; sim_rtc_dead = 0; tstr = 0;
            got = CalibrateCountsPerMSec(7372);
            if (got + 1 < want || got > want + 1) { fprintf(stderr, "FAIL calibrate %u Hz: %u counts/ms, want %u\n", rates[i], got, want); ++failures; }
            CHECK("calibration leaves TMU0 stopped", !(tstr & TMU_TSTR_STR0));
        }
        sim_read_us = 9000;                         /* reads slower than an RTC count: still right */
        sim_hz = 7372800; sim_us = 0; tstr = 0;
        { UINT32 got = CalibrateCountsPerMSec(7372); if (got < 7300 || got > 7450) { fprintf(stderr, "FAIL calibrate with slow reads: %u\n", got); ++failures; } }
        sim_read_us = 3;
        sim_rtc_dead = 1; CHECK("RTC not counting: nominal", CalibrateCountsPerMSec(7372) == 7372);
        sim_rtc_dead = 0; sim_hz = 100000000; sim_us = 0; CHECK("implausible rate: nominal", CalibrateCountsPerMSec(7372) == 7372);
        sim_hz = 0;
    }
    /* a clock change (OALTimerRetune): Pphi x 3/2, 3 ms and 100 counts into a 10 ms period */
    init(); g_calibratedHz = 7372000; g_countsPerMSec = 7372;
    g_oalTimer.curCounts = (UINT64)7372 * 4000; curridlelow = 7372u * 1000u; curridlehigh = 0;
    g_oalTimer.actualMSecPerSysTick = 10; g_oalTimer.actualCountsPerSysTick = 73720;
    tcnt = 73720 - 1 - 3 * 7372 - 100; tstr = TMU_TSTR_STR0 | 2;
    OALTimerRetune(3, 2);
    CHECK("retune: whole ms of the open period reach CurMSec", CurMSec == 103);
    CHECK("retune: new rate", g_oalTimer.countsPerMSec == 11058 && g_countsPerMSec == 11058 && OALTimerCountHz() == 11058000);
    CHECK("retune: performance counter continuous", g_oalTimer.curCounts == (UINT64)11058 * 4003);
    CHECK("retune: idle total and its unit", curridlelow == 11058u * 1000u && curridlehigh == 0 && idleconv == 11058);
    CHECK("retune: 1 ms period, 1000 ms reload, running, TMU1 untouched", tcnt == 11057 && tcor == 11058u * 1000u - 1 && tstr == (TMU_TSTR_STR0 | 2));
    OALTimerRetune(2, 3);
    CHECK("retune back", g_oalTimer.countsPerMSec == 7372 && g_oalTimer.curCounts == (UINT64)7372 * 4003 && CurMSec == 103);
    OALTimerRetune(5, 5);
    CHECK("retune by 1 changes nothing", g_oalTimer.countsPerMSec == 7372 && tcnt == 7371);
    /* a resume with other clock settings: measured again, the counts rescaled */
    g_oalTimer.curCounts = (UINT64)7372 * 5000;
    clock_changed = 1; sim_hz = 3686400; sim_us = 0; tstr = 0;
    InitClock();
    CHECK("resume with a changed clock is measured again", g_oalTimer.countsPerMSec >= 3685 && g_oalTimer.countsPerMSec <= 3687);
    CHECK("... and the counter rescaled", g_oalTimer.curCounts == (UINT64)g_oalTimer.countsPerMSec * 5000 && idleconv == g_oalTimer.countsPerMSec);
    clock_changed = 0; sim_hz = 14745600; sim_us = 0;
    InitClock();
    CHECK("resume with the same clock keeps the rate", g_oalTimer.countsPerMSec >= 3685 && g_oalTimer.countsPerMSec <= 3687);
    sim_hz = 0;
    printf("CE 5.0 timer regression: %d failures\n", failures);
    return failures ? 1 : 0;
}
