/* standby.c - R-standby through the translator (docs/06): the core is
 * powered off and restarted at BAR; the guest must come back exactly where it
 * was, with its SDRAM intact. Two wake-ups: the RTC alarm (set 3 s ahead),
 * then the Clear key through the KEYSC autonomous scan (the Makefile injects
 * a held Clear press, chord 20, while this guest sleeps). */
#include "hyper.h"
#include "mmio.h"
#include "../../bsp/classpad/SRC/INC/bsp_base_regs.h"
#include "../../bsp/classpad/SRC/INC/classpad_keysc.h"

#define RSECCNT  0xA413FEC2u            /* 8-bit BCD */
#define RSECAR   0xA413FED0u
#define RMINAR   0xA413FED2u
#define RHRAR    0xA413FED4u
#define RWKAR    0xA413FED6u
#define RDAYAR   0xA413FED8u
#define RMONAR   0xA413FEDAu
#define RCR1     0xA413FEDCu            /* bit 3 AIE, bit 0 AF */
#define RCR2     0xA413FEDEu            /* bit 0 START */
#define RCR3     0xA413FEE4u
#define IPRK     0xA4080028u            /* bits 15:12: RTC */
#define IMCR10   0xA40800E8u            /* bit 0: RTC alarm */
#define IMR10    0xA40800A8u
#define INTEVT_RTC_ALARM 0xA80u

#define CLEAR_ROW 0x0001u               /* classpad_power.h: Clear/ON is row 0, column 1 */
#define CLEAR_COL 0x0002u

#define PATTERN_WORDS 16384             /* 64 KiB of SDRAM checked across the power-off */

extern char guest_vectors[];
static int fails;
volatile unsigned fatal_vec, irq_regs[18];
volatile unsigned irqs, last_intevt;
static unsigned pattern[PATTERN_WORDS];

static void check(const char *name, int ok)
{
    hc_puts(ok ? "PASS " : "FAIL "); hc_puts(name); hc_putc('\n');
    if (!ok) fails++;
}

void guest_trap(void) {}
void guest_fatal(void) { hc_puts("FAIL unexpected exception in the standby guest\n"); hc_exit(2); }
void guest_irq(unsigned intevt)
{
    last_intevt = intevt;
    irqs++;
    if (intevt == INTEVT_RTC_ALARM) mmio_w8(RCR1, mmio_r8(RCR1) & 0xFEu);    /* clear AF */
}

static unsigned get_sr(void) { unsigned v; __asm__ volatile ("stc sr, %0" : "=r"(v)); return v; }
static void set_sr(unsigned v) { __asm__ volatile ("ldc %0, sr" :: "r"(v) : "memory"); }
static unsigned bcd2bin(unsigned v) { return (v & 15) + ((v >> 4) & 15) * 10; }
static unsigned bin2bcd(unsigned v) { return ((v / 10) << 4) | (v % 10); }
static unsigned seconds(void) { return bcd2bin(mmio_r8(RSECCNT)); }

static void fill(unsigned seed)
{
    unsigned i;
    for (i = 0; i < PATTERN_WORDS; i++) pattern[i] = seed ^ (i * 0x9E3779B1u);
}
static int verify(unsigned seed)
{
    unsigned i;
    for (i = 0; i < PATTERN_WORDS; i++) if (pattern[i] != (seed ^ (i * 0x9E3779B1u))) return 0;
    return 1;
}

void guest_main(void)
{
    unsigned s0, s1, r, sr;
    __asm__ volatile ("ldc %0, vbr" :: "r"(guest_vectors));
    set_sr((get_sr() & ~0x10000000u) | 0xF0u);      /* BL=0, IMASK=15 */

    hc_puts("standby: RTC alarm wake-up\n");
    s0 = seconds();
    mmio_w8(RSECAR, (unsigned char)(bin2bcd((s0 + 3) % 60) | 0x80u));
    mmio_w8(RMINAR, 0); mmio_w8(RHRAR, 0); mmio_w8(RWKAR, 0); mmio_w8(RDAYAR, 0); mmio_w8(RMONAR, 0); mmio_w8(RCR3, 0);
    mmio_w8(RCR1, 0x08);                            /* AIE, AF clear */
    mmio_w8(RCR2, mmio_r8(RCR2) | 1u);              /* counting */
    mmio_w16(IPRK, 0xF000);
    mmio_w8(IMCR10, 0x01);
    fill(0x5EEDu);
    r = hc_standby(0);
    s1 = seconds();
    check("R-standby entered and left on the RTC alarm", r == 1);
    check("SDRAM contents survived the power-off", verify(0x5EEDu));
    check("the RTC kept time: 3 s to the alarm", (s1 + 60 - s0) % 60 >= 3 && (s1 + 60 - s0) % 60 <= 5);
    check("the translator is back: hypercalls work", hc_getstat(1) != 0);
    /* the pending alarm interrupt is delivered once the guest unmasks */
    irqs = 0; last_intevt = 0;
    sr = get_sr(); set_sr(sr & ~0xF0u);
    __asm__ volatile ("nop; nop; nop; nop" ::: "memory");
    set_sr(sr);
    check("the alarm interrupt reached the guest after the wake-up", irqs == 1 && last_intevt == INTEVT_RTC_ALARM);
    mmio_w8(RCR1, 0);

    hc_puts("standby: Clear key wake-up (KEYSC autonomous scan)\n");
    mmio_w16(KEYSC_KIUCNTREG, 0);
    mmio_w16(KEYSC_KOUTPINSET, CLEAR_ROW);
    mmio_w16(KEYSC_KINPINSET, CLEAR_COL);
    mmio_w16(KEYSC_KIUMODEREG, 0x0200);
    mmio_w16(KEYSC_KIUINTREG, 0);                   /* no ordinary KEYSC interrupt */
    s0 = seconds();
    fill(0xC1EA4u);
    r = hc_standby(0);
    s1 = seconds();
    check("R-standby entered and left on the Clear key", r == 1);
    check("SDRAM contents survived the second power-off", verify(0xC1EA4u));
    check("Clear is down in the key image after the wake-up", (mmio_r16(KEYSC_KIUDATA(0)) & KEYSC_CLEAR_MASK) != 0);
    check("the RTC kept time while waiting for the key", (s1 + 60 - s0) % 60 >= 1);
    hc_puts("standby: done\n");
    hc_exit(fails ? 1 : 0);
}
