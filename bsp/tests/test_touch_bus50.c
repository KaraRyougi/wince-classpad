/* The CE 5.0 touch driver's polled IIC0 transactions (bsp/classpad50
 * classpad_touch_bus.h) against a scripted bus: register ordering, full
 * packet length, last-byte NACK, the real-time limit (the tick count, not
 * a count of reads), next-read recovery; and the ICCL/ICCH
 * scaling of classpad_iic.h. Not a sensor emulator. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t UINT32;
#define FILE_DEVICE_HAL 0x101
#define METHOD_BUFFERED 0
#define FILE_ANY_ACCESS 0
#define CTL_CODE(t, f, m, a) (((t) << 16) | ((a) << 14) | ((f) << 2) | (m))
#include "../classpad50/SRC/INC/bsp_base_regs.h"
#include "../classpad50/SRC/INC/classpad_touch.h"

typedef struct { unsigned address, value; int write; } Op;
static Op ops[8192];
static unsigned count, cursor;
static int stuck = -1;                  /* ICSR once the script is used up, -1: reading past it is an error */
static unsigned now_ms = 0xFFFFFFF0u, ms_per_look = 4, looks;  /* the tick count (about to wrap), and what each look at it finds passed */
static unsigned now(void) { looks++; return now_ms += ms_per_look; }
#define TOUCH_BUS_NOW_MS() now()

static void expect(int write, unsigned a, unsigned v) { assert(count < 8192); ops[count++] = (Op){a, v, write}; }
static unsigned read8(unsigned a)
{
    if (cursor == count && stuck >= 0 && a == IIC0_ICSR) return (unsigned)stuck;
    assert(cursor < count && !ops[cursor].write && ops[cursor].address == a);
    return ops[cursor++].value;
}
static unsigned aborts;                 /* ICCR = 0 written after the script ran into the stuck status */
static void write8(unsigned a, unsigned v)
{
    if (cursor == count && stuck >= 0 && a == IIC0_ICCR && !(v & 255)) { aborts++; return; }
    assert(cursor < count && ops[cursor].write && ops[cursor].address == a && ops[cursor].value == (v & 255));
    cursor++;
}
#define MMIO_R8 read8
#define MMIO_W8 write8
#include "../classpad50/SRC/INC/classpad_touch_bus.h"

#define W(a, v) expect(1, a, v)
#define R(a, v) expect(0, a, v)
#define SR(v) R(IIC0_ICSR, v)
static void reset_script(void) { count = cursor = 0; stuck = -1; looks = 0; ms_per_look = 4; aborts = 0; }
static void reset_bus(void)
{
    W(IIC0_ICCR, 0); W(IIC0_ICCR, 0x80); W(IIC0_ICCL, g_touchBusIccl); W(IIC0_ICCH, g_touchBusIcch);
    W(IIC0_ICSR, 0); W(IIC0_ICIC, 0x0e);
}
static void release_wait(void) { SR(0x12); W(IIC0_ICSR, 0x10); }
static void packet_script(unsigned reg, unsigned n)
{
    unsigned i;
    reset_bus(); SR(0); W(IIC0_ICCR, 0x94); SR(1); W(IIC0_ICDR, 0x80); SR(0x12);
    W(IIC0_ICDR, reg); release_wait(); SR(0x12);
    W(IIC0_ICCR, 0x94); release_wait(); SR(0x11); W(IIC0_ICDR, 0x81); SR(0x12);
    W(IIC0_ICCR, 0x81); release_wait();
    for (i = 0; i < n; i++) {
        SR(0x12); if (i == n - 1) W(IIC0_ICCR, 0xc0);
        release_wait(); SR(0x11); R(IIC0_ICDR, i + 1);
    }
    SR(0); W(IIC0_ICCR, 0);
}
static void counts(UINT32 p, UINT32 hz, UINT32 wl, UINT32 wh)
{
    UINT32 l, h;
    ClasspadIicCounts(p, hz, &l, &h);
    if (l != wl || h != wh) { printf("counts(%u, %u) = %u/%u, want %u/%u\n", p, hz, l, h, wl, wh); assert(0); }
    if (p && hz && hz <= CLASSPAD_IIC_SCL_HZ && l < 255 && h < 255) assert(p / (l + h) <= hz);
}

int main(void)
{
    unsigned char packet[16];
    unsigned i;

    /* clock: Casio's counts at the nominal Pphi, scaled above it, never fewer, 8 bits */
    counts(29491200, CLASSPAD_IIC_SCL_HZ, 0x29, 0x22);
    counts(29491200, 0, 0x29, 0x22);
    counts(49152000, 0, 0x29, 0x22);                    /* 0 = unscaled, whatever Pphi is */
    counts(49152000, CLASSPAD_IIC_SCL_HZ, 69, 56);      /* 5/3 of nominal: 125 counts */
    counts(58982400, CLASSPAD_IIC_SCL_HZ, 82, 68);      /* twice nominal: twice the counts */
    counts(14745600, CLASSPAD_IIC_SCL_HZ, 0x29, 0x22);  /* slower Pphi: Casio's counts, a slower SCL */
    counts(29491200, 1000000, 0x29, 0x22);              /* a faster SCL than Casio's counts give is not made */
    counts(49152000, 100000, 255, 223);                 /* 492 counts: low clamps at 255 */
    counts(0, CLASSPAD_IIC_SCL_HZ, 0x29, 0x22);

    /* the packet, one byte, with Casio's counts (nothing set) */
    reset_script(); packet_script(0x84, 16); assert(TouchBusRead(packet, 16)); assert(cursor == count);
    for (i = 0; i < 16; i++) assert(packet[i] == i + 1);
    reset_script(); packet_script(0x68, 1); assert(TouchBusReadReg(0x68, packet, 1)); assert(cursor == count);

    /* the scaled counts reach the registers on every reset */
    TouchBusSetClock(49152000, CLASSPAD_IIC_SCL_HZ);
    assert(g_touchBusIccl == 69 && g_touchBusIcch == 56);
    reset_script(); packet_script(0x84, 16); assert(TouchBusRead(packet, 16)); assert(cursor == count);
    TouchBusSetClock(49152000, 0);
    assert(g_touchBusIccl == 0x29 && g_touchBusIcch == 0x22);
    TouchBusSetClock(29491200, CLASSPAD_IIC_SCL_HZ);
    assert(g_touchBusIccl == 0x29 && g_touchBusIcch == 0x22);

    /* a command and a register write */
    reset_script(); reset_bus(); SR(0); W(IIC0_ICCR, 0x94); SR(1); W(IIC0_ICDR, 0x80); SR(0x12);
    W(IIC0_ICDR, 0x02); release_wait(); SR(0x12); W(IIC0_ICCR, 0x90); release_wait(); SR(0); W(IIC0_ICCR, 0);
    assert(TouchBusCommand(0x02)); assert(cursor == count);
    reset_script(); reset_bus(); SR(0); W(IIC0_ICCR, 0x94); SR(1); W(IIC0_ICDR, 0x80); SR(0x12);
    W(IIC0_ICDR, 0x58); release_wait(); SR(0x12); W(IIC0_ICDR, 0x52); release_wait(); SR(0x12);
    W(IIC0_ICCR, 0x90); release_wait(); SR(0); W(IIC0_ICCR, 0);
    assert(TouchBusWriteReg(0x58, 0x52)); assert(cursor == count);

    /* no acknowledge, arbitration lost */
    for (i = 0; i < 2; i++) {
        reset_script(); reset_bus(); SR(0); W(IIC0_ICCR, 0x94); SR(1);
        W(IIC0_ICDR, 0x80); SR(i ? ICSR_AL : ICSR_TACK); W(IIC0_ICCR, 0);
        assert(!TouchBusRead(packet, 16)); assert(cursor == count && g_touchBusStage == 3);
    }

    /* transactions that go well never ask for the time */
    reset_script(); packet_script(0x84, 16); assert(TouchBusRead(packet, 16)); assert(looks == 0);

    /* a bus that never becomes free: given up 10 ms after the first look at
     * the tick count (4 ms pass between looks: the third look after it) */
    reset_script(); reset_bus(); stuck = ICSR_BUSY;
    assert(!TouchBusRead(packet, 16));
    assert(aborts == 1 && g_touchBusStage == 1 && looks == 4 && g_touchBusReads == 4 * TOUCH_BUS_CLOCK_EVERY);
    /* a CPU four times as fast (1 ms between looks instead of 4) does about
     * four times the reads in the same 10 ms */
    reset_script(); reset_bus(); stuck = ICSR_BUSY; ms_per_look = 1;
    assert(!TouchBusRead(packet, 16));
    assert(aborts == 1 && looks == 11 && g_touchBusReads == 11 * TOUCH_BUS_CLOCK_EVERY);

    /* the limit covers the whole transaction, not each stage: a sensor that
     * stops in the middle of the packet */
    reset_script(); reset_bus(); SR(0); W(IIC0_ICCR, 0x94); SR(1); W(IIC0_ICDR, 0x80); SR(0x12);
    W(IIC0_ICDR, 0x84); release_wait();
    stuck = ICSR_BUSY;                                  /* no WAIT for the register byte */
    assert(!TouchBusRead(packet, 16));
    assert(aborts == 1 && g_touchBusStage == 4 && looks == 4);

    /* recovery */
    reset_script(); packet_script(0x84, 16); assert(TouchBusRead(packet, 16)); assert(cursor == count);
    reset_script(); assert(!TouchBusRead(packet, 0)); assert(!TouchBusRead(NULL, 16)); assert(!cursor);
    puts("touch bus 50: counts, packet, command, write, NACK, arbitration, time limit and recovery PASS");
    return 0;
}
