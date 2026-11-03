//------------------------------------------------------------------------------
//  intr.c - SH7305 interrupt controller for the common OAL interrupt module
//
//  Vectors are (INTEVT - 0x200) / 0x20, which is also the kernel's ISR table
//  index (HookInterrupt). The INTC has per-source priority fields (IPRx) and
//  per-source mask bits (IMRx, cleared through IMCRx). The common OAL maps
//  IRQ <-> SYSINTR through 64-entry tables, so a source with a vector of 64
//  or more (KEYSC, 95) has a logical IRQ number below 64 (intr_cfg.h): the
//  ISR is hooked at the vector and translates the logical number.
//------------------------------------------------------------------------------
#include <bsp.h>

#ifndef IntrPrio
#define IntrPrio g_pNKGlobal->IntrPrio
#endif

// logical IRQ -> IMR register index and bit
typedef struct { UINT32 irq; UINT32 imr; UINT8 bit; } CLASSPAD_IRQ_MASK;

static const CLASSPAD_IRQ_MASK g_irqMask[] = {
    { IRQ_TMU0_TUNI0, 4, INTC_IMR4_TMU0 },
    { IRQ_TMU1_TUNI1, 4, INTC_IMR4_TMU1 },
    { IRQ_TMU2_TUNI2, 4, INTC_IMR4_TMU2 },
    { IRQ_KEYSC,      5, INTC_IMR5_KEYSC },
    { IRQ_TOUCH_PEN,  3, INTC_IMR3_TOUCH },
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
//  Device ISRs: level sources stay masked until the IST's InterruptDone
//  (OALIntrDoneIrqs) and the kernel gets the SYSINTR the driver requested.
//
static UINT32 DeviceIntrHandler(UINT32 imr, UINT8 bit, UINT32 irq)
{
    UINT32 sysIntr;
    MMIO_W8(INTC_IMR(imr), bit);
    sysIntr = OALIntrTranslateIrq(irq);
    return (sysIntr == SYSINTR_UNDEFINED) ? SYSINTR_NOP : sysIntr;
}

static UINT32 KeyscIntrHandler(void)
{
    return DeviceIntrHandler(5, INTC_IMR5_KEYSC, IRQ_KEYSC);
}

static UINT32 TouchIntrHandler(void)
{
    return DeviceIntrHandler(3, INTC_IMR3_TOUCH, IRQ_TOUCH_PEN);
}

//------------------------------------------------------------------------------
BOOL OALIntrInit()
{
    OALMSG(OAL_INTR && OAL_FUNC, (L"+OALIntrInit()\r\n"));

    OALIntrMapInit();

    // Mask every source the OAL knows until its driver enables it, then
    // give each its priority. The kernel masks SR.IMASK to the same level
    // while the ISR runs (IntrPrio), so IPR and IntrPrio must agree.
    MMIO_W8(INTC_IMR(4), INTC_IMR4_TMU0 | INTC_IMR4_TMU1 | INTC_IMR4_TMU2);
    MMIO_W8(INTC_IMR(5), INTC_IMR5_KEYSC);
    MMIO_W8(INTC_IMR(3), INTC_IMR3_TOUCH);
    MMIO_W16(INTC_IPRA, (MMIO_R16(INTC_IPRA) & 0x0FFF) | (IRQ_TMU0_PRIORITY << 12));
    MMIO_W16(INTC_IPRF, (MMIO_R16(INTC_IPRF) & 0x0FFF) | (IRQ_KEYSC_PRIORITY << 12));
    MMIO_W16(INTC_IPRB, (MMIO_R16(INTC_IPRB) & 0xFF0F) | (IRQ_TOUCH_PRIORITY << 4));

    if (!HookAndSetPriority(IRQ_TMU0_TUNI0, (FARPROC)OALTimerIntrHandler, IRQ_TMU0_PRIORITY)) {
        return FALSE;
    }
    if (!HookAndSetPriority(VEC_KEYSC, (FARPROC)KeyscIntrHandler, IRQ_KEYSC_PRIORITY)) {
        return FALSE;
    }
    if (!HookAndSetPriority(IRQ_TOUCH_PEN, (FARPROC)TouchIntrHandler, IRQ_TOUCH_PRIORITY)) {
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
