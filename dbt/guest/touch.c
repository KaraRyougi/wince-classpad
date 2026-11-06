/* Exercise the production IIC transaction through translated MMIO, without
 * CE or its interrupt routing. Inject a held contact with HLE_CB_TOUCH. */
#include "hyper.h"
#include "mmio.h"
#define MMIO_R8 mmio_r8
#define MMIO_W8 mmio_w8
static void Sleep(unsigned ms) { volatile unsigned n = ms * 100; while (n--) {} }
#include "../../bsp/classpad/SRC/INC/bsp_base_regs.h"
#include "../../bsp/classpad/SRC/INC/classpad_touch.h"
#include "../../bsp/classpad/SRC/INC/classpad_touch_bus.h"
void guest_main(void)
{
    unsigned char pkt[TOUCH_PACKET_BYTES];
    unsigned i, x = 0, y = 0;
    mmio_w32(TOUCH_MSTPCR2, mmio_r32(TOUCH_MSTPCR2) & ~TOUCH_IIC_STOP);
    mmio_w16(TOUCH_PJCR, mmio_r16(TOUCH_PJCR) & ~0x0F00);
    mmio_w16(TOUCH_PRCR, (mmio_r16(TOUCH_PRCR) & ~0x0C00) | 0x0C00);
    mmio_w8(INTC_IMR(7), 0xF0);
    for (i = 0; i < 1000000; ++i) if (!(mmio_r8(SH7305_REG_PA_PRDR) & TOUCH_PRDR_CONTACT)) break;
    if (i == 1000000) { hc_puts("RESULT: FAIL no contact\n"); hc_exit(1); }
    for (i = 0; i < 32; ++i) {
        if (!TouchBusRead(pkt, sizeof pkt)) {
            hc_puts("IIC failure stage/status "); hc_puthex(g_touchBusStage); hc_puthex(g_touchBusStatus);
            hc_puts("\nRESULT: FAIL\n"); hc_exit(2);
        }
        if (i && (x != ((unsigned)pkt[0] << 8 | pkt[1]) || y != ((unsigned)pkt[2] << 8 | pkt[3]))) {
            hc_puts("RESULT: FAIL unstable snapshot\n"); hc_exit(3);
        }
        x = (unsigned)pkt[0] << 8 | pkt[1]; y = (unsigned)pkt[2] << 8 | pkt[3];
    }
    hc_puts("touch raw x/y/z1 "); hc_puthex(x); hc_puthex(y); hc_puthex((unsigned)pkt[4] << 8 | pkt[5]);
    hc_puts("\nRESULT: PASS (32 production IIC transactions)\n"); hc_exit(0);
}
