//------------------------------------------------------------------------------
//  intr.c - SH7305 interrupt controller for the common OAL interrupt module
//
//  IRQ numbers are (INTEVT - 0x200) / 0x20, which is also the kernel's ISR
//  table index (HookInterrupt). The INTC has per-source priority fields
//  (IPRx) and per-source mask bits (IMRx, cleared through IMCRx).
//------------------------------------------------------------------------------
#include <bsp.h>

#ifndef IntrPrio
#define IntrPrio g_pNKGlobal->IntrPrio
#endif

// IRQ -> IMR register index and bit (TMU only for now; KEYSC, RTC, USB later)
typedef struct { UINT32 irq; UINT32 imr; UINT8 bit; } CLASSPAD_IRQ_MASK;

static const CLASSPAD_IRQ_MASK g_irqMask[] = {
    { EXCEPTIONCODETOIRQ(0x400), 4, INTC_IMR4_TMU0 },
    { EXCEPTIONCODETOIRQ(0x420), 4, INTC_IMR4_TMU1 },
    { EXCEPTIONCODETOIRQ(0x440), 4, INTC_IMR4_TMU2 },
};

static const CLASSPAD_IRQ_MASK *FindMask(UINT32 irq)
{
    UINT32 i;
    for (i = 0; i < sizeof(g_irqMask) / sizeof(g_irqMask[0]); i++) {
        if (g_irqMask[i].irq == irq) return &g_irqMask[i];
    }
    return NULL;
}

//------------------------------------------------------------------------------
BOOL HookAndSetPriority(int hwIntNumber, FARPROC pfnHandler, BYTE intrPrio)
{
    if (!HookInterrupt(hwIntNumber, pfnHandler)) {
        OALMSG(OAL_ERROR, (L"ERROR: HookAndSetPriority: HookInterrupt(%d) failed\r\n", hwIntNumber));
        return FALSE;
    }
    IntrPrio[hwIntNumber] = intrPrio;
    return TRUE;
}

//------------------------------------------------------------------------------
BOOL OALIntrInit()
{
    OALMSG(OAL_INTR && OAL_FUNC, (L"+OALIntrInit()\r\n"));

    OALIntrMapInit();

    // Mask the timer sources until the timer code enables them, then give
    // TMU0 its priority. The kernel masks SR.IMASK to the same level while
    // the ISR runs (IntrPrio).
    MMIO_W8(INTC_IMR(4), INTC_IMR4_TMU0 | INTC_IMR4_TMU1 | INTC_IMR4_TMU2);
    MMIO_W16(INTC_IPRA, (MMIO_R16(INTC_IPRA) & 0x0FFF) | (IRQ_TMU0_PRIORITY << 12));

    if (!HookAndSetPriority(IRQ_TMU0_TUNI0, (FARPROC)OALTimerIntrHandler, IRQ_TMU0_PRIORITY)) {
        return FALSE;
    }

    OALMSG(OAL_INTR && OAL_FUNC, (L"-OALIntrInit()\r\n"));
    return TRUE;
}

//------------------------------------------------------------------------------
BOOL OALIntrRequestIrqs(DEVICE_LOCATION *pDevLoc, UINT32 *pCount, UINT32 *pIrqs)
{
    // No bus-enumerated devices
    return FALSE;
}

//------------------------------------------------------------------------------
BOOL OALIntrEnableIrqs(UINT32 count, const UINT32 *pIrqs)
{
    BOOL rc = TRUE;
    UINT32 i;
    for (i = 0; i < count; i++) {
        const CLASSPAD_IRQ_MASK *m = FindMask(pIrqs[i]);
        if (m == NULL) {
            OALMSG(OAL_ERROR, (L"ERROR: OALIntrEnableIrqs: unknown IRQ %d\r\n", pIrqs[i]));
            rc = FALSE;
            continue;
        }
        MMIO_W8(INTC_IMCR(m->imr), m->bit);
    }
    return rc;
}

//------------------------------------------------------------------------------
VOID OALIntrDisableIrqs(UINT32 count, const UINT32 *pIrqs)
{
    UINT32 i;
    for (i = 0; i < count; i++) {
        const CLASSPAD_IRQ_MASK *m = FindMask(pIrqs[i]);
        if (m == NULL) {
            OALMSG(OAL_ERROR, (L"ERROR: OALIntrDisableIrqs: unknown IRQ %d\r\n", pIrqs[i]));
            continue;
        }
        MMIO_W8(INTC_IMR(m->imr), m->bit);
    }
}

//------------------------------------------------------------------------------
VOID OALIntrDoneIrqs(UINT32 count, const UINT32 *pIrqs)
{
    // Level sources: re-enable after the IST has serviced the device
    OALIntrEnableIrqs(count, pIrqs);
}
