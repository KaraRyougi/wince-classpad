/* Production suspend register sequence under translated MMIO and IRQ entry. */
#include "hyper.h"
#include "mmio.h"
#include "../../bsp/classpad/SRC/INC/bsp_base_regs.h"
#include "../../bsp/classpad/SRC/INC/classpad_keysc.h"
#define MMIO_R8 mmio_r8
#define MMIO_R16 mmio_r16
#define MMIO_R32 mmio_r32
#define MMIO_W8 mmio_w8
#define MMIO_W16 mmio_w16
#define MMIO_W32 mmio_w32
#include "../../bsp/classpad/SRC/INC/classpad_power.h"
extern char guest_vectors[];
static ClasspadPowerState state;
volatile unsigned fatal_vec, irq_regs[18];
static unsigned bad_irq, wakes, timer_wakes;
void guest_trap(void) {}
void guest_fatal(void) { hc_puts("FAIL exception in suspend\n"); hc_exit(2); }
void guest_irq(unsigned event)
{
    unsigned before = state.wake;
    if (!state.active) { bad_irq++; return; }
    if (event == 0x400) ClasspadPowerTimerInterrupt(&state);
    else if (event == 0xBE0) ClasspadPowerKeyInterrupt(&state);
    else { bad_irq++; return; }
    if (!before && state.wake) { wakes++; if (event == 0x400) timer_wakes++; }
}
static void check(const char *name, int ok)
{
    hc_puts(ok ? "PASS " : "FAIL "); hc_puts(name); hc_putc('\n');
    if (!ok) hc_exit(1);
}
/* Equivalent to sleep.src: test with BL set so a request cannot run between
 * checking wake and SLEEP. The IRQ itself must be accepted during SLEEP. */
static void wait_irq(void)
{
    unsigned old, sr;
    __asm__ volatile("stc sr,%0" : "=r"(old));
    sr = (old & ~0xF0u) | 0x10000000u;
    __asm__ volatile("ldc %0,sr" :: "r"(sr) : "memory");
    if (!state.wake) __asm__ volatile("sleep");
    __asm__ volatile("ldc %0,sr" :: "r"(old) : "memory");
}
void guest_main(void)
{
    unsigned cycle, i;
    __asm__ volatile("ldc %0,vbr" :: "r"(guest_vectors));
    MMIO_W16(INTC_IPRF, 0xD000);
    MMIO_W16(INTC_IPRA, 0xC000);
    MMIO_W16(KEYSC_KOUTPINSET, KEYSC_KOUT_ALL);
    MMIO_W16(KEYSC_KINPINSET, KEYSC_KIN_ALL);
    MMIO_W16(KEYSC_KIUMODEREG, KEYSC_MODE_VALUE);
    MMIO_W16(KEYSC_KIUINTREG, KEYSC_INT_ENABLE | KEYSC_INT_CLEAR);
    MMIO_W16(KEYSC_KIUCNTREG, KEYSC_CNT_SCAN_START);
    for (i = 0; i < 13; ++i) MMIO_W8(INTC_IMR(i), 0xFF);
    MMIO_W8(TMU_TSTR, 5);
    for (cycle = 0; cycle < 2; ++cycle) {
        ClasspadPowerPrepare(&state, 7372);
        /* Inject a missing keyboard interrupt: Clear must still wake via TMU. */
        if (cycle == 0) MMIO_W8(INTC_IMR(5), INTC_IMR5_KEYSC);
        check("recovery timer running, only Clear matrix armed", MMIO_R8(TMU_TSTR) == 1 &&
            MMIO_R16(KEYSC_KOUTPINSET) == 1 && MMIO_R16(KEYSC_KINPINSET) == 2);
        hc_puts("POWER sleeping\n");
        while (!state.wake) wait_irq();
        check("Clear/ON woke suspended CPU", ClasspadClearDown() && !bad_irq);
        ClasspadPowerRestore(&state);
        check("timer, masks and matrix restored", MMIO_R8(TMU_TSTR) == 5 &&
            MMIO_R8(INTC_IMR(5)) == 0xFF && MMIO_R32(TMU_TCOR0) == state.tcor &&
            MMIO_R16(TMU_TCR0) == state.tcr && MMIO_R16(KEYSC_KOUTPINSET) == KEYSC_KOUT_ALL &&
            MMIO_R16(KEYSC_KINPINSET) == KEYSC_KIN_ALL);
        for (i = 0; i < 10000000 && ClasspadClearDown(); ++i) {}
        check("wake key released", !ClasspadClearDown());
    }
    check("two suspend/resume cycles", wakes == 2);
    check("Clear wake with KEYSC interrupt masked", timer_wakes >= 1);
    hc_puts("RESULT: PASS\n"); hc_exit(0);
}
