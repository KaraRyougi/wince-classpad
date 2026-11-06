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
#include "../classpad/SRC/INC/bsp_base_regs.h"
static struct {
    UINT32 countsPerMSec, countsMargin, maxPeriodMSec, msecPerSysTick;
    UINT32 countsPerSysTick, actualMSecPerSysTick, actualCountsPerSysTick;
    UINT64 curCounts;
} g_oalTimer;
static UINT32 CurMSec, dwReschedTime, idleconv, curridlehigh, curridlelow;
static void OALTimerQueryPerformanceFrequency(void) {}
static void OALTimerQueryPerformanceCounter(void) {}
static void (*pQueryPerformanceFrequency)(void), (*pQueryPerformanceCounter)(void);
typedef struct { void (*pfnUpdateReschedTime)(DWORD); void (*pfnInitClock)(void); } OEM_STATE;
static OEM_STATE oem, *g_pOemGlobal = &oem;
static UINT32 tcnt, tcor, tcr, tstr, programming_delay;
static int reload_on_read, reload_before_write;
static int suspended, power_calls, irq_enabled;
static BOOL INTERRUPTS_ENABLE(BOOL enabled) { BOOL old=irq_enabled; irq_enabled=enabled; return old; }
static BOOL ClasspadPowerTimerIsr(void) { ++power_calls; return suspended; }
static UINT32 readreg(UINT32 addr) {
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
#include "../classpad/SRC/OAL/OALLIB/timer.c"
static int failures;
#define CHECK(name, expr) do { if (!(expr)) { fprintf(stderr,"FAIL %s\n", name); ++failures; } } while (0)
static void init(void) {
    memset(&g_oalTimer,0,sizeof g_oalTimer); CurMSec=100; dwReschedTime=120;
    tcnt=tcor=tcr=tstr=0; suspended=0; power_calls=0;
    OALTimerInit(1000,7372,4096);
}
int main(void) {
    init();
    CHECK("variable-tick callback", oem.pfnUpdateReschedTime == OALTimerUpdateRescheduleTime);
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
    printf("timer regression: %d failures\n", failures);
    return failures ? 1 : 0;
}
