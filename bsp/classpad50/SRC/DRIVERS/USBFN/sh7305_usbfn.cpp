//------------------------------------------------------------------------------
//  sh7305_usbfn.cpp - USB function controller PDD for the SH7305 USB module
//
//  The PDD half of a CE 5.0 USB function controller driver: linked with the
//  public MDD (ufnmdd.lib) it becomes classpad_usbfn.dll, the "UFN" bus
//  driver that loads one function client at a time (platform.reg:
//  [HKLM\Drivers\USB\FunctionDrivers]): the mass storage function that hands
//  Casio's storage memory to a PC, or the CDC ACM serial function.
//
//  The controller (0xA4D80000) is of Renesas' USBHS family (R8A66597
//  register model). What is known of it on this SoC comes from gint's driver
//  (src/usb, include/gint/mpu/usb.h), which is the reference for every
//  sequence here, and from Casio's OS (the power-up order, battif.c).
//
//    - endpoint 0 is the DCP on the CFIFO port. SET_ADDRESS is answered by
//      the controller itself (the MDD is told through UFN_MSG_SET_ADDRESS
//      when the device state says "address"); every other request reaches
//      the MDD as a setup packet. A control transfer ends when the MDD asks
//      for the status handshake (DCPCTR.CCPL).
//    - bulk and interrupt endpoints are pipes: bulk on pipes 3, 4, 5, 1, 2
//      (physical endpoints 1-5), interrupt on pipes 6, 7 (physical endpoints
//      6, 7). OUT pipes are read through D0FIFO, IN pipes written through
//      D1FIFO; a pipe owns its FIFO port only while it is being read or
//      written, under the driver's lock.
//    - one packet per buffer (no continuous mode, no double buffer): an IN
//      transfer writes a packet and waits for the buffer-empty interrupt, an
//      OUT transfer reads a packet at each buffer-ready interrupt and ends
//      on a short packet or a full buffer.
//    - FIFO data goes through 32-bit accesses with BIGEND set, so that the
//      first byte in memory is the first on the bus. The translator swaps
//      every 16/32-bit access of this little-endian image, which makes a
//      plain copy of memory words exactly that (classpad_mmio.h); the
//      registers proper use the pre-swapped MMIO macros.
//
//  Power. Casio's OS keeps the USB clock stopped, the module in standby and
//  the PHY off, and the user does not want the PHY left on: the controller
//  is powered only while a cable is in. A thread looks at the cable pin
//  (Port U bit 1, what Casio's OS reads) every CABLE_POLL_MS; on a cable it
//  powers the controller in Casio's order, connects the pull-up and tells
//  the MDD "attach"; when VBUS goes, "detach" and everything off again. The
//  battery driver looks at the same controller for its AC status: both take
//  the named mutex USB_POWER_MUTEX, which this driver holds for as long as
//  it has the controller powered.
//  Registry (the driver's key): "CablePin"=dword:0 keeps the controller
//  powered from Start on and goes by VBUS alone (if the pin turns out not to
//  follow the cable on some unit); "Priority256" for the IST.
//
//  Interrupt: INTEVT 0xA20, logical IRQ_USB (intr_cfg.h, OAL intr.c). The
//  IST waits without a timeout (see kbd_pdd.cpp for why); the cable thread
//  also runs the service routine, so a lost edge only costs a poll period.
//
//  Not verified on the calculator when written. In casio-emu the host side
//  needs dbt/emu-patch/add_classpad_usbhost.py.
//------------------------------------------------------------------------------
#if defined(_MSC_VER) && !defined(DEBUG)
#pragma optimize("t", on)
#endif
#include <windows.h>
#include <nkintr.h>
#include <ceddk.h>
#include <devload.h>
#include <pkfuncs.h>
#include <usbfn.h>
#include "bsp_base_regs.h"
#include "intr_cfg.h"
#include "classpad_mmio.h"
#include "classpad_usbreg.h"

#define USB_POWER_MUTEX     TEXT("ClassPadUsbPower")
#define ENDPOINT_COUNT      8           // 0 = DCP, 1-5 bulk, 6-7 interrupt
#define EP0_PACKET          64
#define CABLE_POLL_MS       500
#define SERVICE_POLL_MS     100
#define IST_PRIORITY        100

static const BYTE g_pipeOfEndpoint[ENDPOINT_COUNT] = { 0, 3, 4, 5, 1, 2, 6, 7 };

typedef struct {
    BOOL initialized;
    BOOL in;                            // direction of the endpoint (endpoint 0: of the transfer in hand)
    BOOL halted;
    BYTE pipe;
    DWORD maxPacket;
    PSTransfer transfer;
} EP_STATE;

typedef struct {
    PVOID mdd;
    PFN_UFN_MDD_NOTIFY notify;
    CRITICAL_SECTION cs;                // the registers and everything below
    EP_STATE ep[ENDPOINT_COUNT];
    DWORD sysIntr;
    HANDLE intrEvent, ist, cableThread, powerMutex;
    volatile BOOL stop, resumed;
    BOOL started, powered, attached, addressTold, useCablePin;
    DWORD istPriority;
    CEDEVICE_POWER_STATE powerState;
} USBFN;

#define R16(off)        USB_R16(off)
#define W16(off, v)     USB_W16(off, v)
#define READY_SPINS     USB_READY_SPINS

static void SetPid(USBFN *u, EP_STATE *e, WORD pid)
{
    DWORD reg = e->pipe ? USB_PIPECTR(e->pipe) : USB_DCPCTR;
    int i;

    W16(reg, (WORD)((R16(reg) & ~(CTR_PID | CTR_CCPL)) | pid));
    if (pid == PID_NAK) {
        for (i = 0; i < READY_SPINS && (R16(reg) & CTR_PBUSY); i++) ;
    }
}

static void EnableBits(DWORD reg, WORD bit, BOOL on)
{
    WORD v = R16(reg), nv = (WORD)(on ? (v | bit) : (v & ~bit));
    if (nv != v) W16(reg, nv);
}

// Ends the transfer of an endpoint and tells the MDD. Called with the lock held.
static void Complete(USBFN *u, EP_STATE *e, DWORD error)
{
    PSTransfer t = e->transfer;
    WORD bit = (WORD)(1 << e->pipe);

    if (!t) return;
    e->transfer = NULL;
    EnableBits(USB_BRDYENB, bit, FALSE);
    if (e->in) EnableBits(USB_BEMPENB, bit, FALSE);
    t->dwUsbError = error;
    u->notify(u->mdd, UFN_MSG_TRANSFER_COMPLETE, (DWORD)t);
}

//------------------------------------------------------------------------------
//  IN: fill available FIFO plane(s) (both planes on double-buffered bulk pipes).
//  While more packets remain, BRDY wakes us as soon as one plane frees; once
//  the final packet is queued, BEMP fires when the last plane drains.
//
static void SendPacket(USBFN *u, EP_STATE *e)
{
    PSTransfer t = e->transfer;
    DWORD port = e->pipe ? USB_D1FIFO : USB_CFIFO, sel = e->pipe ? USB_D1FIFOSEL : USB_CFIFOSEL;
    DWORD ctr = e->pipe ? USB_D1FIFOCTR : USB_CFIFOCTR;
    DWORD perms;
    WORD bit = (WORD)(1 << e->pipe);
    BOOL first = TRUE, ok = TRUE;
    DWORD lastN = 0;

    W16(sel, (WORD)(FIFOSEL_MBW32 | FIFOSEL_BIGEND | (e->pipe ? e->pipe : FIFOSEL_ISEL)));
    do {
        DWORD left, n;
        if (first) {
            if (!UsbFifoReady(ctr)) {          // previous packet has not left yet
                EnableBits(e->pipe ? USB_BRDYENB : USB_BEMPENB, bit, TRUE);
                SetPid(u, e, PID_BUF);
                return;
            }
            first = FALSE;
        } else {
            if (!(R16(USB_PIPECTR(e->pipe)) & CTR_BSTS) || !UsbFifoReady(ctr)) {
                EnableBits(USB_BRDYENB, bit, TRUE);
                return;
            }
        }
        left = t->cbBuffer - t->cbTransferred;
        n = left < e->maxPacket ? left : e->maxPacket;
        lastN = n;
        perms = GetCurrentPermissions();
        SetProcPermissions(t->dwCallerPermissions);
        __try {
            UsbFifoWrite(port, (const BYTE *)t->pvBuffer + t->cbTransferred, n);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = FALSE;
        }
        SetProcPermissions(perms);
        if (!ok) {
            W16(ctr, FIFOCTR_BCLR);
            Complete(u, e, UFN_CLIENT_BUFFER_ERROR);
            return;
        }
        t->cbTransferred += n;
        if (n < e->maxPacket) W16(ctr, FIFOCTR_BVAL);   // short (or empty) packet committed by hand
        SetPid(u, e, PID_BUF);
    } while (e->pipe && e->pipe <= 5 && t->cbTransferred < t->cbBuffer);

    if (!e->pipe && lastN < e->maxPacket) {
        // the last packet of a control read: gint found the buffer-empty
        // interrupt unreliable for a hand-committed DCP packet, and nothing
        // waits for it: the status stage follows on the MDD's handshake
        Complete(u, e, UFN_NO_ERROR);
        return;
    }
    if (e->pipe && t->cbTransferred < t->cbBuffer) {
        EnableBits(USB_BRDYENB, bit, TRUE);
    } else {
        if (e->pipe) EnableBits(USB_BRDYENB, bit, FALSE);
        EnableBits(USB_BEMPENB, bit, TRUE);
    }
}

static void OnBufferEmpty(USBFN *u, EP_STATE *e)
{
    PSTransfer t = e->transfer;

    if (!t || !e->in) return;
    if (t->cbTransferred < t->cbBuffer) SendPacket(u, e);
    else Complete(u, e, UFN_NO_ERROR);
}

//------------------------------------------------------------------------------
//  OUT: drain ready FIFO plane(s) into the transfer. Ends on a short packet or
//  a full buffer; what does not fit stays in the FIFO for the next transfer.
//
static void ReceivePacket(USBFN *u, EP_STATE *e)
{
    PSTransfer t = e->transfer;
    DWORD port = e->pipe ? USB_D0FIFO : USB_CFIFO, sel = e->pipe ? USB_D0FIFOSEL : USB_CFIFOSEL;
    DWORD ctr = e->pipe ? USB_D0FIFOCTR : USB_CFIFOCTR;
    DWORD have, room, n, perms;
    BOOL first = TRUE, ok = TRUE;

    W16(sel, (WORD)(FIFOSEL_MBW32 | FIFOSEL_BIGEND | e->pipe));
    for (;;) {
        if (first) {
            if (!UsbFifoReady(ctr)) return;    // nothing there yet: BRDY comes back here
            first = FALSE;
        } else {
            if (!(R16(USB_PIPECTR(e->pipe)) & CTR_BSTS) || !UsbFifoReady(ctr)) return;
        }
        have = R16(ctr) & FIFOCTR_DTLN;
        room = t->cbBuffer - t->cbTransferred;
        n = have < room ? have : room;
        perms = GetCurrentPermissions();
        SetProcPermissions(t->dwCallerPermissions);
        __try {
            UsbFifoRead(port, (BYTE *)t->pvBuffer + t->cbTransferred, n);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = FALSE;
        }
        SetProcPermissions(perms);
        if (!ok) {
            W16(ctr, FIFOCTR_BCLR);
            Complete(u, e, UFN_CLIENT_BUFFER_ERROR);
            return;
        }
        t->cbTransferred += n;
        if (!have) W16(ctr, FIFOCTR_BCLR);              // an empty packet is taken by clearing the buffer
        if (t->cbTransferred == t->cbBuffer || have < e->maxPacket || have % e->maxPacket) {
            Complete(u, e, UFN_NO_ERROR);
            return;
        }
        SetPid(u, e, PID_BUF);                          // the next packet
        if (!e->pipe || e->pipe > 5) return;
    }
}

static void OnBufferReady(USBFN *u, EP_STATE *e)
{
    if (!e->transfer) return;
    if (e->in) SendPacket(u, e);
    else ReceivePacket(u, e);
}

//------------------------------------------------------------------------------
//  The controller's events. Called with the lock held, from the IST and from
//  the cable thread.
//
static void OnSetup(USBFN *u)
{
    USB_DEVICE_REQUEST udr;
    EP_STATE *e = &u->ep[0];
    WORD req = R16(USB_USBREQ);

    udr.bmRequestType = (UCHAR)req;
    udr.bRequest = (UCHAR)(req >> 8);
    udr.wValue = R16(USB_USBVAL);
    udr.wIndex = R16(USB_USBINDX);
    udr.wLength = R16(USB_USBLENG);
    W16(USB_INTSTS0, (WORD)~INTSTS0_VALID);
    if (e->transfer) Complete(u, e, UFN_CANCELED_ERROR);        // the host gave up on the previous request
    W16(USB_CFIFOCTR, FIFOCTR_BCLR);
    // SET_ADDRESS never gets here on the chip; where it does (casio-emu's
    // host hands it over like any request) the MDD answers it and must not
    // be told a second time from the device state
    if (udr.bmRequestType == 0 && udr.bRequest == USB_REQUEST_SET_ADDRESS) u->addressTold = TRUE;
    u->notify(u->mdd, UFN_MSG_SETUP_PACKET, (DWORD)&udr);
}

static void OnDeviceState(USBFN *u, WORD sts)
{
    DWORD speed;

    switch (INTSTS0_DVSQ(sts)) {
    case 1:                             // default: a bus reset
        u->addressTold = FALSE;
        if (!u->attached) { u->attached = TRUE; u->notify(u->mdd, UFN_MSG_BUS_EVENTS, UFN_ATTACH); }
        u->notify(u->mdd, UFN_MSG_BUS_EVENTS, UFN_RESET);
        speed = (R16(USB_DVSTCTR) & DVSTCTR_RHST) == 3 ? BS_HIGH_SPEED : BS_FULL_SPEED;
        u->notify(u->mdd, UFN_MSG_BUS_SPEED, speed);
        break;
    case 2:                             // address: the controller took SET_ADDRESS
        if (!u->addressTold) {
            u->addressTold = TRUE;
            u->notify(u->mdd, UFN_MSG_SET_ADDRESS, R16(USB_USBADDR) & 0x7F);
        }
        break;
    case 4: case 5: case 6: case 7:
        u->notify(u->mdd, UFN_MSG_BUS_EVENTS, UFN_SUSPEND);
        break;
    }
}

static void Service(USBFN *u)
{
    WORD sts, pipes;
    int i, round;

    if (!u->powered) return;
    for (round = 0; round < 16; round++) {
        sts = R16(USB_INTSTS0);
        if (!(sts & R16(USB_INTENB0) & (INT_VBINT | INT_RESM | INT_DVST | INT_CTRT | INT_BEMP | INT_BRDY))) break;
        if (sts & INT_VBINT) W16(USB_INTSTS0, (WORD)~INT_VBINT);        // the cable thread reads the level
        if (sts & INT_RESM) {
            W16(USB_INTSTS0, (WORD)~INT_RESM);
            u->notify(u->mdd, UFN_MSG_BUS_EVENTS, UFN_RESUME);
        }
        if (sts & INT_DVST) {
            W16(USB_INTSTS0, (WORD)~INT_DVST);
            OnDeviceState(u, sts);
        }
        if (sts & INT_CTRT) {
            W16(USB_INTSTS0, (WORD)~INT_CTRT);
            if (sts & INTSTS0_VALID) OnSetup(u);
        }
        if (sts & INT_BEMP) {
            pipes = R16(USB_BEMPSTS) & R16(USB_BEMPENB);
            W16(USB_BEMPSTS, (WORD)~pipes);
            for (i = 0; i < ENDPOINT_COUNT; i++) {
                if (pipes & (1 << u->ep[i].pipe)) OnBufferEmpty(u, &u->ep[i]);
            }
        }
        if (sts & INT_BRDY) {
            pipes = R16(USB_BRDYSTS) & R16(USB_BRDYENB);
            W16(USB_BRDYSTS, (WORD)~pipes);
            for (i = 0; i < ENDPOINT_COUNT; i++) {
                if (pipes & (1 << u->ep[i].pipe)) OnBufferReady(u, &u->ep[i]);
            }
        }
    }
}

//------------------------------------------------------------------------------
//  Power and the cable
//
static void Delay(DWORD ms) { Sleep(ms); }

// Casio's order (OS 02.01.7003 0x800089E8), gint's register set-up after it
static void PowerOn(USBFN *u)
{
    int i;

    WaitForSingleObject(u->powerMutex, INFINITE);       // held until PowerOff: the battery driver leaves the controller alone
    __try {
        MMIO_W16(PFC_MSELCRA, MMIO_R16(PFC_MSELCRA) & ~MSELCRA_USB_M);
        MMIO_W16(PFC_MSELCRB, MMIO_R16(PFC_MSELCRB) & ~MSELCRB_USB_M);
        MMIO_W32(CPG_USBCLKCR, MMIO_R32(CPG_USBCLKCR) & ~USBCLKCR_STOP);
        Delay(110);                                     // Casio waits 14 edges of the 64 Hz clock
        MMIO_W32(PWR_MSTPCR2, MMIO_R32(PWR_MSTPCR2) & ~MSTPCR2_USB);
        MMIO_W16(USB_UPONCR, UPONCR_ON);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // casio-emu has no such registers on the ClassPad profile; its controller needs none of this
    }
    W16(USB_SYSCFG, (WORD)(R16(USB_SYSCFG) | SYSCFG_SCKE));
    Delay(1);
    W16(USB_BUSWAIT, 5);
    W16(USB_REG_C2, 0x0020);
    W16(USB_SYSCFG, SYSCFG_SCKE);                       // function controller, pull-ups off, module off
    W16(USB_SYSCFG, SYSCFG_SCKE | SYSCFG_HSE);
    W16(USB_SYSCFG, SYSCFG_SCKE | SYSCFG_HSE | SYSCFG_USBE);

    W16(USB_CFIFOSEL, 0); W16(USB_D0FIFOSEL, 0); W16(USB_D1FIFOSEL, 0);
    for (i = 1; i <= 9; i++) {
        W16(USB_PIPECTR(i), 0);
        W16(USB_PIPESEL, (WORD)i);
        W16(USB_PIPECFG, 0);
    }
    W16(USB_PIPESEL, 0);
    W16(USB_DCPCFG, 0);
    W16(USB_DCPMAXP, EP0_PACKET);
    W16(USB_SOFCFG, (WORD)(R16(USB_SOFCFG) | SOFCFG_ENABLE));
    W16(USB_CFIFOSEL, FIFOSEL_MBW32 | FIFOSEL_BIGEND);
    W16(USB_INTENB1, 0);
    W16(USB_BRDYENB, 0); W16(USB_NRDYENB, 0); W16(USB_BEMPENB, 0);
    W16(USB_BRDYSTS, 0); W16(USB_NRDYSTS, 0); W16(USB_BEMPSTS, 0);
    W16(USB_INTSTS0, 0);
    W16(USB_INTENB0, INT_VBINT | INT_RESM | INT_DVST | INT_CTRT | INT_BEMP | INT_BRDY);
    u->powered = TRUE;
    u->addressTold = FALSE;
    // The interrupt line may have been left masked by the IST (see there):
    // now that the controller's sources are ours, it is let through again.
    if (u->intrEvent && u->sysIntr != SYSINTR_UNDEFINED) InterruptDone(u->sysIntr);
}

static void PowerOff(USBFN *u)
{
    W16(USB_INTENB0, 0);
    W16(USB_SYSCFG, (WORD)(R16(USB_SYSCFG) & ~SYSCFG_DPRPU));
    W16(USB_SYSCFG, (WORD)(R16(USB_SYSCFG) & ~SYSCFG_USBE));
    u->powered = FALSE;
    __try {                             // gint's order, with the values Casio's OS leaves
        MMIO_W16(USB_UPONCR, 0);
        Delay(1);                       // "crucial" in gint
        MMIO_W32(PWR_MSTPCR2, MMIO_R32(PWR_MSTPCR2) | MSTPCR2_USB);
        MMIO_W32(CPG_USBCLKCR, MMIO_R32(CPG_USBCLKCR) | USBCLKCR_STOP);
        Delay(1);
        MMIO_W16(PFC_MSELCRB, MMIO_R16(PFC_MSELCRB) | MSELCRB_USB_M);
        MMIO_W16(PFC_MSELCRA, (MMIO_R16(PFC_MSELCRA) & ~MSELCRA_USB_M) | MSELCRA_USB_OFF);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    ReleaseMutex(u->powerMutex);
}

static BOOL CablePin(void)
{
    BOOL in = TRUE;
    __try {
        in = (MMIO_R8(PORT_PUDR) & PUDR_USB_CABLE) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return in;
}

// The transfers the MDD still has out are handed back before a detach
static void CancelAll(USBFN *u)
{
    int i;
    for (i = 0; i < ENDPOINT_COUNT; i++) {
        if (u->ep[i].transfer) Complete(u, &u->ep[i], UFN_CANCELED_ERROR);
    }
}

static void Detach(USBFN *u)
{
    CancelAll(u);
    if (u->attached) {
        u->attached = FALSE;
        u->notify(u->mdd, UFN_MSG_BUS_EVENTS, UFN_DETACH);
    }
    if (u->powered) PowerOff(u);
}

static DWORD WINAPI CableThread(LPVOID param)
{
    USBFN *u = (USBFN *)param;
    BOOL cable;

    while (!u->stop) {
        EnterCriticalSection(&u->cs);
        if (u->resumed) {               // the machine was off in between: nothing in the controller is to be trusted
            u->resumed = FALSE;
            Detach(u);
        }
        cable = u->useCablePin ? CablePin() : TRUE;
        if (!u->powered) {
            if (cable) {
                PowerOn(u);
                if (R16(USB_INTSTS0) & INTSTS0_VBSTS) {
                    u->attached = TRUE;
                    u->notify(u->mdd, UFN_MSG_BUS_EVENTS, UFN_ATTACH);
                    W16(USB_SYSCFG, (WORD)(R16(USB_SYSCFG) | SYSCFG_DPRPU));    // the host sees a device from here on
                    (void)R16(USB_SYSSTS);
                } else if (u->useCablePin) {
                    PowerOff(u);        // the pin says cable, the controller says no VBUS: not this time
                }
            }
        } else if (!u->attached) {      // powered without the pin, waiting for VBUS
            if (R16(USB_INTSTS0) & INTSTS0_VBSTS) {
                u->attached = TRUE;
                u->notify(u->mdd, UFN_MSG_BUS_EVENTS, UFN_ATTACH);
                W16(USB_SYSCFG, (WORD)(R16(USB_SYSCFG) | SYSCFG_DPRPU));
                (void)R16(USB_SYSSTS);
            }
        } else if (!cable || !(R16(USB_INTSTS0) & INTSTS0_VBSTS)) {
            CancelAll(u);
            u->attached = FALSE;
            W16(USB_SYSCFG, (WORD)(R16(USB_SYSCFG) & ~SYSCFG_DPRPU));
            u->notify(u->mdd, UFN_MSG_BUS_EVENTS, UFN_DETACH);
            if (u->useCablePin) PowerOff(u);
        } else {
            Service(u);                 // an interrupt edge lost: this picks it up
        }
        LeaveCriticalSection(&u->cs);
        Sleep(u->attached ? SERVICE_POLL_MS : CABLE_POLL_MS);
    }
    EnterCriticalSection(&u->cs);       // stopping: off the bus and unpowered, by the thread that holds the power mutex
    Detach(u);
    LeaveCriticalSection(&u->cs);
    return 0;
}

static DWORD WINAPI Ist(LPVOID param)
{
    USBFN *u = (USBFN *)param;
    BOOL powered;

    CeSetThreadPriority(GetCurrentThread(), (int)u->istPriority);
    for (;;) {
        if (WaitForSingleObject(u->intrEvent, INFINITE) != WAIT_OBJECT_0 || u->stop) break;
        EnterCriticalSection(&u->cs);
        powered = u->powered;
        Service(u);
        LeaveCriticalSection(&u->cs);
        // An interrupt while the controller is off is not ours to answer: its
        // enables and pending bits are what Casio's OS (or its USB transfer
        // just before CE was started) left there, nothing here clears them,
        // and InterruptDone would let the same interrupt in again at once.
        // That was the boot that never reached the desktop on the calculator
        // (2026-11-11: 83000 USB interrupts in 16 seconds against 160 system
        // ticks, after files had been copied over USB before the start). The
        // line stays masked until PowerOn has set the controller up.
        if (powered) InterruptDone(u->sysIntr);
    }
    return 0;
}

//------------------------------------------------------------------------------
//  The PDD interface
//
static DWORD WINAPI UfnPdd_Deinit(PVOID context)
{
    USBFN *u = (USBFN *)context;

    if (u->powerMutex) CloseHandle(u->powerMutex);
    DeleteCriticalSection(&u->cs);
    LocalFree(u);
    return ERROR_SUCCESS;
}

static DWORD WINAPI UfnPdd_Start(PVOID context)
{
    USBFN *u = (USBFN *)context;
    DWORD irq = IRQ_USB;

    if (u->started) return ERROR_SUCCESS;
    u->stop = FALSE;
    u->intrEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!u->intrEvent) return GetLastError();
    u->sysIntr = (DWORD)SYSINTR_UNDEFINED;
    if (!KernelIoControl(IOCTL_HAL_REQUEST_SYSINTR, &irq, sizeof irq, &u->sysIntr, sizeof u->sysIntr, NULL)) {
        RETAILMSG(1, (TEXT("CLASSPAD USB: no SYSINTR for IRQ %d, polling only\r\n"), irq));
        u->sysIntr = (DWORD)SYSINTR_UNDEFINED;
    } else if (!InterruptInitialize(u->sysIntr, u->intrEvent, NULL, 0)) {
        RETAILMSG(1, (TEXT("CLASSPAD USB: InterruptInitialize(%d) failed, polling only\r\n"), u->sysIntr));
        KernelIoControl(IOCTL_HAL_RELEASE_SYSINTR, &u->sysIntr, sizeof u->sysIntr, NULL, 0, NULL);
        u->sysIntr = (DWORD)SYSINTR_UNDEFINED;
    }
    if (u->sysIntr != (DWORD)SYSINTR_UNDEFINED) u->ist = CreateThread(NULL, 0, Ist, u, 0, NULL);
    u->cableThread = CreateThread(NULL, 0, CableThread, u, 0, NULL);
    if (!u->cableThread) return ERROR_GEN_FAILURE;
    u->started = TRUE;
    RETAILMSG(1, (TEXT("CLASSPAD USB: started, SYSINTR %d, cable by %s\r\n"), u->sysIntr, u->useCablePin ? TEXT("pin") : TEXT("VBUS")));
    return ERROR_SUCCESS;
}

static DWORD WINAPI UfnPdd_Stop(PVOID context)
{
    USBFN *u = (USBFN *)context;

    if (!u->started) return ERROR_SUCCESS;
    u->stop = TRUE;
    WaitForSingleObject(u->cableThread, INFINITE);      // it detaches and powers the controller off on its way out
    CloseHandle(u->cableThread);
    u->cableThread = NULL;
    if (u->ist) {
        InterruptDisable(u->sysIntr);
        SetEvent(u->intrEvent);
        WaitForSingleObject(u->ist, INFINITE);
        CloseHandle(u->ist);
        u->ist = NULL;
        KernelIoControl(IOCTL_HAL_RELEASE_SYSINTR, &u->sysIntr, sizeof u->sysIntr, NULL, 0, NULL);
    }
    CloseHandle(u->intrEvent);
    u->intrEvent = NULL;
    u->started = FALSE;
    return ERROR_SUCCESS;
}

static DWORD WINAPI UfnPdd_IsConfigurationSupportable(PVOID context, UFN_BUS_SPEED speed, PUFN_CONFIGURATION config)
{
    return ERROR_SUCCESS;
}

static DWORD WINAPI UfnPdd_IsEndpointSupportable(PVOID context, DWORD endpoint, UFN_BUS_SPEED speed,
                                                 PUSB_ENDPOINT_DESCRIPTOR desc, BYTE config, BYTE iface, BYTE alt)
{
    BYTE type = desc->bmAttributes & USB_ENDPOINT_TYPE_MASK;
    WORD size = desc->wMaxPacketSize & USB_ENDPOINT_MAX_PACKET_SIZE_MASK;

    if (endpoint >= ENDPOINT_COUNT) return ERROR_INVALID_PARAMETER;
    if (endpoint == 0) {
        if (size < EP0_PACKET) return ERROR_INVALID_PARAMETER;
        desc->wMaxPacketSize = EP0_PACKET;
        return ERROR_SUCCESS;
    }
    if (type == USB_ENDPOINT_TYPE_BULK && endpoint <= 5) {
        if (size > (speed == BS_HIGH_SPEED ? 512 : 64)) desc->wMaxPacketSize = (WORD)(speed == BS_HIGH_SPEED ? 512 : 64);
        return ERROR_SUCCESS;
    }
    if (type == USB_ENDPOINT_TYPE_INTERRUPT && endpoint >= 6) {
        if (size > 64) desc->wMaxPacketSize = 64;       // pipes 6-9 have 64-byte buffers
        return ERROR_SUCCESS;
    }
    return ERROR_INVALID_PARAMETER;
}

static DWORD WINAPI UfnPdd_InitEndpoint(PVOID context, DWORD endpoint, UFN_BUS_SPEED speed, PUSB_ENDPOINT_DESCRIPTOR desc,
                                        PVOID reserved, BYTE config, BYTE iface, BYTE alt)
{
    USBFN *u = (USBFN *)context;
    EP_STATE *e;
    BYTE type = desc->bmAttributes & USB_ENDPOINT_TYPE_MASK;
    WORD cfg;

    if (endpoint >= ENDPOINT_COUNT) return ERROR_INVALID_PARAMETER;
    e = &u->ep[endpoint];
    EnterCriticalSection(&u->cs);
    e->pipe = g_pipeOfEndpoint[endpoint];
    e->maxPacket = desc->wMaxPacketSize & USB_ENDPOINT_MAX_PACKET_SIZE_MASK;
    e->in = (desc->bEndpointAddress & 0x80) != 0;
    e->halted = FALSE;
    e->transfer = NULL;
    e->initialized = TRUE;
    if (endpoint && u->powered) {
        SetPid(u, e, PID_NAK);
        cfg = (WORD)((type == USB_ENDPOINT_TYPE_BULK ? (PIPECFG_BULK | PIPECFG_DBLB) : PIPECFG_INT) | PIPECFG_SHTNAK |
                     (e->in ? PIPECFG_DIR_IN : 0) | (desc->bEndpointAddress & 0x0F));
        W16(USB_PIPESEL, e->pipe);
        W16(USB_PIPECFG, cfg);
        // buffer memory, in 64-byte blocks (128 total): 0-3 DCP, 4-7 pipes 6-9,
        // 2x512 B (16 blocks, double-buffered) for each bulk pipe 1-5 -> blocks 8..87
        if (e->pipe <= 5) W16(USB_PIPEBUF, (WORD)((7 << 10) | (8 + (e->pipe - 1) * 16)));
        W16(USB_PIPEMAXP, (WORD)e->maxPacket);
        W16(USB_PIPEPERI, 0);
        W16(USB_PIPESEL, 0);
        W16(USB_PIPECTR(e->pipe), CTR_ACLRM);           // empty buffer, DATA0
        W16(USB_PIPECTR(e->pipe), 0);
        W16(USB_PIPECTR(e->pipe), CTR_SQCLR);
    }
    LeaveCriticalSection(&u->cs);
    return ERROR_SUCCESS;
}

static DWORD WINAPI UfnPdd_DeinitEndpoint(PVOID context, DWORD endpoint)
{
    USBFN *u = (USBFN *)context;
    EP_STATE *e;

    if (endpoint >= ENDPOINT_COUNT) return ERROR_INVALID_PARAMETER;
    e = &u->ep[endpoint];
    EnterCriticalSection(&u->cs);
    if (e->transfer) Complete(u, e, UFN_CANCELED_ERROR);
    if (endpoint && u->powered && e->initialized) {
        SetPid(u, e, PID_NAK);
        W16(USB_PIPESEL, e->pipe);
        W16(USB_PIPECFG, 0);
        W16(USB_PIPESEL, 0);
    }
    e->initialized = FALSE;
    LeaveCriticalSection(&u->cs);
    return ERROR_SUCCESS;
}

static DWORD WINAPI UfnPdd_RegisterDevice(PVOID context, PCUSB_DEVICE_DESCRIPTOR hsDevice, PCUFN_CONFIGURATION hsConfig,
                                          PCUSB_CONFIGURATION_DESCRIPTOR hsConfigDesc, PCUSB_DEVICE_DESCRIPTOR fsDevice,
                                          PCUFN_CONFIGURATION fsConfig, PCUSB_CONFIGURATION_DESCRIPTOR fsConfigDesc,
                                          PCUFN_STRING_SET strings, DWORD stringSets)
{
    return ERROR_SUCCESS;               // the MDD answers the descriptor requests
}

static DWORD WINAPI UfnPdd_DeregisterDevice(PVOID context)
{
    return ERROR_SUCCESS;
}

static DWORD WINAPI UfnPdd_IssueTransfer(PVOID context, DWORD endpoint, PSTransfer transfer)
{
    USBFN *u = (USBFN *)context;
    EP_STATE *e;
    DWORD ret = ERROR_SUCCESS;
    WORD bit;

    if (endpoint >= ENDPOINT_COUNT) return ERROR_INVALID_PARAMETER;
    e = &u->ep[endpoint];
    EnterCriticalSection(&u->cs);
    if (!e->initialized || e->transfer) ret = ERROR_INVALID_PARAMETER;
    else if (!u->powered || !u->attached) ret = ERROR_NOT_READY;
    else {
        bit = (WORD)(1 << e->pipe);
        if (!endpoint) e->in = TRANSFER_IS_IN(transfer) != 0;
        e->transfer = transfer;
        if (e->in) {
            SendPacket(u, e);
        } else if (!endpoint && !transfer->cbBuffer) {
            Complete(u, e, UFN_NO_ERROR);               // a control write without data: nothing to read
        } else {
            EnableBits(USB_BRDYENB, bit, TRUE);
            SetPid(u, e, PID_BUF);
            // what a shorter transfer before this one left in the FIFO, or a packet that came in meanwhile
            W16(e->pipe ? USB_D0FIFOSEL : USB_CFIFOSEL, (WORD)(FIFOSEL_MBW32 | FIFOSEL_BIGEND | e->pipe));
            if ((R16(USB_BRDYSTS) & bit) || (R16(e->pipe ? USB_D0FIFOCTR : USB_CFIFOCTR) & FIFOCTR_DTLN)) {
                W16(USB_BRDYSTS, (WORD)~bit);
                ReceivePacket(u, e);
            }
        }
    }
    LeaveCriticalSection(&u->cs);
    return ret;
}

static DWORD WINAPI UfnPdd_AbortTransfer(PVOID context, DWORD endpoint, PSTransfer transfer)
{
    USBFN *u = (USBFN *)context;
    EP_STATE *e;

    if (endpoint >= ENDPOINT_COUNT) return ERROR_INVALID_PARAMETER;
    e = &u->ep[endpoint];
    EnterCriticalSection(&u->cs);
    if (e->transfer == transfer) {
        if (u->powered) {
            SetPid(u, e, PID_NAK);
            if (e->pipe) {
                W16(USB_PIPECTR(e->pipe), CTR_ACLRM);
                W16(USB_PIPECTR(e->pipe), 0);
            } else {
                W16(USB_CFIFOCTR, FIFOCTR_BCLR);
            }
        }
        Complete(u, e, UFN_CANCELED_ERROR);
    }
    LeaveCriticalSection(&u->cs);
    return ERROR_SUCCESS;
}

static DWORD WINAPI UfnPdd_StallEndpoint(PVOID context, DWORD endpoint)
{
    USBFN *u = (USBFN *)context;
    EP_STATE *e;

    if (endpoint >= ENDPOINT_COUNT) return ERROR_INVALID_PARAMETER;
    e = &u->ep[endpoint];
    EnterCriticalSection(&u->cs);
    if (u->powered) SetPid(u, e, PID_STALL);
    if (endpoint) e->halted = TRUE;     // endpoint 0 recovers by itself at the next setup
    LeaveCriticalSection(&u->cs);
    return ERROR_SUCCESS;
}

static DWORD WINAPI UfnPdd_ClearEndpointStall(PVOID context, DWORD endpoint)
{
    USBFN *u = (USBFN *)context;
    EP_STATE *e;

    if (endpoint >= ENDPOINT_COUNT) return ERROR_INVALID_PARAMETER;
    e = &u->ep[endpoint];
    EnterCriticalSection(&u->cs);
    if (u->powered) {
        SetPid(u, e, PID_NAK);
        if (endpoint) {
            W16(USB_PIPECTR(e->pipe), CTR_SQCLR);       // DATA0 again
            if (e->transfer) SetPid(u, e, PID_BUF);
        }
    }
    e->halted = FALSE;
    LeaveCriticalSection(&u->cs);
    return ERROR_SUCCESS;
}

static DWORD WINAPI UfnPdd_IsEndpointHalted(PVOID context, DWORD endpoint, PBOOL halted)
{
    USBFN *u = (USBFN *)context;

    if (endpoint >= ENDPOINT_COUNT || !halted) return ERROR_INVALID_PARAMETER;
    *halted = u->ep[endpoint].halted;
    return ERROR_SUCCESS;
}

// The status stage of the control transfer in hand
static DWORD WINAPI UfnPdd_SendControlStatusHandshake(PVOID context, DWORD endpoint)
{
    USBFN *u = (USBFN *)context;

    EnterCriticalSection(&u->cs);
    if (u->powered) W16(USB_DCPCTR, (WORD)((R16(USB_DCPCTR) & ~CTR_PID) | PID_BUF | CTR_CCPL));
    LeaveCriticalSection(&u->cs);
    return ERROR_SUCCESS;
}

static DWORD WINAPI UfnPdd_SetAddress(PVOID context, BYTE address)
{
    return ERROR_SUCCESS;               // the controller has done it
}

static DWORD WINAPI UfnPdd_InitiateRemoteWakeup(PVOID context)
{
    return ERROR_NOT_SUPPORTED;
}

static void WINAPI UfnPdd_PowerDown(PVOID context)
{
}

// Called with interrupts off: only a note for the cable thread
static void WINAPI UfnPdd_PowerUp(PVOID context)
{
    ((USBFN *)context)->resumed = TRUE;
}

static DWORD WINAPI UfnPdd_IOControl(PVOID context, IOCTL_SOURCE source, DWORD code, PBYTE in, DWORD inSize,
                                     PBYTE out, DWORD outSize, PDWORD actual)
{
    USBFN *u = (USBFN *)context;
    PCE_BUS_POWER_STATE ps;

    switch (code) {
    case IOCTL_BUS_GET_POWER_STATE:
        if (source != MDD_IOCTL || !in || inSize != sizeof(CE_BUS_POWER_STATE)) break;
        ps = (PCE_BUS_POWER_STATE)in;
        *ps->lpceDevicePowerState = u->powerState;
        return ERROR_SUCCESS;
    case IOCTL_BUS_SET_POWER_STATE:
        if (source != MDD_IOCTL || !in || inSize != sizeof(CE_BUS_POWER_STATE)) break;
        ps = (PCE_BUS_POWER_STATE)in;
        u->powerState = *ps->lpceDevicePowerState;      // the cable decides what is powered
        return ERROR_SUCCESS;
    }
    return ERROR_INVALID_PARAMETER;
}

static DWORD RegDword(HKEY key, LPCTSTR name, DWORD def)
{
    DWORD type, value, size = sizeof value;
    if (RegQueryValueEx(key, name, NULL, &type, (LPBYTE)&value, &size) != ERROR_SUCCESS || type != REG_DWORD) return def;
    return value;
}

extern "C" DWORD WINAPI UfnPdd_Init(LPCTSTR activeKey, PVOID mddContext, PUFN_MDD_INTERFACE_INFO mddInfo,
                                    PUFN_PDD_INTERFACE_INFO pddInfo)
{
    static const UFN_PDD_INTERFACE_INFO info = {
        UFN_PDD_INTERFACE_VERSION,
        UFN_PDD_CAPS_SUPPORTS_FULL_SPEED | UFN_PDD_CAPS_SUPPORTS_HIGH_SPEED,
        ENDPOINT_COUNT,
        NULL,
        &UfnPdd_Deinit,
        &UfnPdd_IsConfigurationSupportable,
        &UfnPdd_IsEndpointSupportable,
        &UfnPdd_InitEndpoint,
        &UfnPdd_RegisterDevice,
        &UfnPdd_DeregisterDevice,
        &UfnPdd_Start,
        &UfnPdd_Stop,
        &UfnPdd_IssueTransfer,
        &UfnPdd_AbortTransfer,
        &UfnPdd_DeinitEndpoint,
        &UfnPdd_StallEndpoint,
        &UfnPdd_ClearEndpointStall,
        &UfnPdd_SendControlStatusHandshake,
        &UfnPdd_SetAddress,
        &UfnPdd_IsEndpointHalted,
        &UfnPdd_InitiateRemoteWakeup,
        &UfnPdd_PowerDown,
        &UfnPdd_PowerUp,
        &UfnPdd_IOControl,
    };
    USBFN *u;
    HKEY key;
    int i;

    u = (USBFN *)LocalAlloc(LPTR, sizeof *u);
    if (!u) return ERROR_OUTOFMEMORY;
    u->mdd = mddContext;
    u->notify = mddInfo->pfnNotify;
    u->powerState = D0;
    u->useCablePin = TRUE;
    u->istPriority = IST_PRIORITY;
    for (i = 0; i < ENDPOINT_COUNT; i++) u->ep[i].pipe = g_pipeOfEndpoint[i];
    u->ep[0].maxPacket = EP0_PACKET;
    key = OpenDeviceKey(activeKey);
    if (key && key != INVALID_HANDLE_VALUE) {
        u->useCablePin = RegDword(key, TEXT("CablePin"), 1) != 0;
        u->istPriority = RegDword(key, TEXT("Priority256"), IST_PRIORITY);
        RegCloseKey(key);
    }
    InitializeCriticalSection(&u->cs);
    u->powerMutex = CreateMutex(NULL, FALSE, USB_POWER_MUTEX);
    if (!u->powerMutex) {
        DeleteCriticalSection(&u->cs);
        LocalFree(u);
        return ERROR_GEN_FAILURE;
    }
    memcpy(pddInfo, &info, sizeof info);
    pddInfo->pvPddContext = u;
    return ERROR_SUCCESS;
}

extern "C" BOOL WINAPI UfnPdd_DllEntry(HANDLE dll, DWORD reason, LPVOID reserved)
{
    return TRUE;
}

#ifdef DEBUG
UFN_GENERATE_DPCURSETTINGS(UFN_DEFAULT_DPCURSETTINGS_NAME, _T(""), _T(""), _T(""), _T(""), DBG_ERROR | DBG_WARNING | DBG_INIT)
#endif
