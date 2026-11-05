//------------------------------------------------------------------------------
//  classpad_touch.cpp - DDSI layer of touch.dll for the fx-CP400 panel
//
//  The common touch MDD (tchmdd.lib) owns the IST: it waits on its event
//  hTouchPanelEvent (the interrupt event of gIntrTouch) and calls
//  DdsiTouchPanelGetPoint. Here gIntrTouch is the contact interrupt (INTEVT
//  0x700, logical IRQ_TOUCH_PEN) that fires once when the stylus lands;
//  while the panel is pressed the samples (1000 / sample rate ms apart, each
//  reading the sensor on IIC0) are driven by this file's sample thread,
//  which sets hTouchPanelEvent itself (SetEvent on an interrupt event goes
//  through the kernel's EventModIntr, the ISR's path). The MDD's own way,
//  a timeout on the IST's wait (gdwTouchIstTimeout), is NOT used: a timed
//  wait on an interrupt event hits a CE 6.0 R3 kernel hole (WaitOneMore's
//  retry path, notes/ce/kernel-sh-notes.md section 10) whenever the
//  timeout expires between the kernel calls that set the wait up, which
//  the translator makes routine; the event then refuses every later wait.
//  The first sample without contact is the pen-up (TouchSampleValidFlag
//  without the down flag), stops the sample thread and re-enables the
//  interrupt with InterruptDone. gIntrTouchChanged is not used. Hardware
//  details and the casio-emu model: classpad_touch.h.
//
//  The panel exists on the fx-CP400 only. On the fx-CG100 (the profile
//  casio-emu runs the translator on) the IIC registers are not emulated at
//  all, so the driver decides at DdsiTouchPanelEnable whether a panel is
//  present: HKLM\HARDWARE\DEVICEMAP\TOUCH "Panel" (0/1), else the display
//  height (the fx-CP400 screen has 528 rows, the fx-CG100 216). Without a
//  panel the interrupt never fires, the IST sleeps forever and no register
//  of the bus is touched. The raw coordinates are calibrated by the MDD from
//  "CalibrationData" in the registry (platform.reg), computed from the
//  emulator's stylus mapping for the 320x528 screen.
//
//  Not verified under the translator yet: casio-emu's ClassPad profile only
//  cold-boots the real OS, so this image has only run on the fx-CG100.
//------------------------------------------------------------------------------
#include <windows.h>
#include <types.h>
#include <nkintr.h>
#include <tchddsi.h>
#include "bsp_base_regs.h"
#include "intr_cfg.h"
#include "classpad_mmio.h"
#include "classpad_touch.h"

#define TOUCH_REG_KEY               TEXT("HARDWARE\\DEVICEMAP\\TOUCH")
#define TOUCH_SAMPLE_RATE_LOW       50
#define TOUCH_SAMPLE_RATE_HIGH      100
#define TOUCH_PANEL_MIN_ROWS        500         // a display this tall is the fx-CP400 panel
#define TOUCH_IIC_POLLS             64          // DTE polls between 1 ms sleeps
#define TOUCH_IIC_ROUNDS            100         // 100 ms per byte at most
#define TOUCH_THREAD_PRIORITY       109         // the MDD's default for its IST ("Priority256")

// MDD globals (tchmain.c). gdwTouchIstTimeout is left at its INFINITE default.
extern "C" HANDLE hTouchPanelEvent;             // the IST's event
extern "C" const int MIN_CAL_COUNT = 1;         // samples before a calibration point is accepted
DWORD gIntrTouch        = SYSINTR_NOP;
DWORD gIntrTouchChanged = SYSINTR_NOP;

#ifndef DEBUG
// tchddsi.h's zones only exist in DEBUG, where the MDD defines them itself
DBGPARAM dpCurSettings = {
    TEXT("ClasspadTouch"),
    { TEXT("Samples"), TEXT("Calibrate"), TEXT("Stats"), TEXT("Thread"),
      TEXT("TipState"), TEXT("Init"), TEXT("6"), TEXT("7"),
      TEXT("8"), TEXT("Misc"), TEXT("Delays"), TEXT("Timing"),
      TEXT("Alloc"), TEXT("Function"), TEXT("Warn"), TEXT("Error") },
    0x0
};
#endif

static BOOL     g_panel;                        // sensor present (fx-CP400)
static INT      g_rate = TOUCH_SAMPLE_RATE_HIGH;
static INT      g_rateSetting = 1;              // 0 low, 1 high
static BOOL     g_prevDown;
static INT      g_lastX, g_lastY;
static LONG     g_attached;
static DWORD    g_samples, g_contacts, g_busErrors;
static HANDLE   g_hSampler;                     // sample thread
static HANDLE   g_hevSample;                    // wakes it when the stylus lands
static volatile BOOL g_sampling;                // stylus down: keep waking the IST
static volatile BOOL g_quit;

static DWORD RegDword(LPCTSTR name, DWORD dflt)
{
    HKEY hKey; DWORD v = dflt, type, size = sizeof v;
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, TOUCH_REG_KEY, 0, 0, &hKey) == ERROR_SUCCESS) {
        if (RegQueryValueEx(hKey, name, NULL, &type, (LPBYTE)&v, &size) != ERROR_SUCCESS || type != REG_DWORD) v = dflt;
        RegCloseKey(hKey);
    }
    return v;
}

//------------------------------------------------------------------------------
//  IIC0, polled (ICIC = 0: the emulator then signals DTE for every byte)
//
static BOOL IicWaitDte(void)
{
    int rounds, polls;
    for (rounds = 0; rounds < TOUCH_IIC_ROUNDS; rounds++) {
        for (polls = 0; polls < TOUCH_IIC_POLLS; polls++) {
            UINT8 sr = MMIO_R8(IIC0_ICSR);
            if (sr & ICSR_DTE) return TRUE;
            if (sr & (ICSR_TACK | ICSR_AL)) return FALSE;
        }
        Sleep(1);
    }
    return FALSE;
}

// Start condition in transmit mode, then the address byte
static BOOL IicStart(UINT8 address)
{
    MMIO_W8(IIC0_ICCR, ICCR_ICE | ICCR_TRS | ICCR_BBSY);
    if (!IicWaitDte()) return FALSE;
    MMIO_W8(IIC0_ICDR, address);
    return IicWaitDte();
}

static void IicIdle(void)
{
    MMIO_W8(IIC0_ICCR, ICCR_ICE);
}

// Command 0x84 (snapshot), repeated start, read n bytes of the packet
static BOOL TouchReadPacket(UINT8 *pkt, int n)
{
    int i;
    if (!IicStart((UINT8)(TOUCH_IIC_ADDRESS << 1))) goto fail;
    MMIO_W8(IIC0_ICDR, TOUCH_CMD_SNAPSHOT);
    if (!IicWaitDte()) goto fail;
    if (!IicStart((UINT8)((TOUCH_IIC_ADDRESS << 1) | 1))) goto fail;
    MMIO_W8(IIC0_ICCR, ICCR_ICE | ICCR_SCP);        // receive, more bytes follow
    for (i = 0; i < n; i++) {
        if (i == n - 1) MMIO_W8(IIC0_ICCR, ICCR_ICE); // last byte
        if (!IicWaitDte()) goto fail;
        pkt[i] = MMIO_R8(IIC0_ICDR);
    }
    IicIdle();
    return TRUE;
fail:
    g_busErrors++;
    if (g_busErrors <= 5) RETAILMSG(1, (TEXT("CLASSPAD TOUCH: IIC transfer failed (ICSR %02x)\r\n"), MMIO_R8(IIC0_ICSR)));
    IicIdle();
    return FALSE;
}

static BOOL PenDown(void)
{
    return (MMIO_R8(SH7305_REG_PA_PRDR) & TOUCH_PRDR_CONTACT) == 0;
}

//------------------------------------------------------------------------------
//  Sampling while pressed: wake the MDD's IST every 1000 / rate ms. Started by
//  DdsiTouchPanelGetPoint on the down transition, stopped by the pen-up.
//
static DWORD WINAPI SampleThread(LPVOID)
{
    CeSetThreadPriority(GetCurrentThread(), (int)RegDword(TEXT("Priority256"), TOUCH_THREAD_PRIORITY));
    while (!g_quit) {
        WaitForSingleObject(g_hevSample, INFINITE);     // an ordinary event: no interrupt-event wait here
        while (g_sampling && !g_quit) {
            Sleep(1000 / (DWORD)g_rate);
            SetEvent(hTouchPanelEvent);
        }
    }
    return 0;
}

//------------------------------------------------------------------------------
//  Panel detection: registry, else the display geometry
//
static BOOL DetectPanel(void)
{
    DWORD panel = RegDword(TEXT("Panel"), 0xFFFFFFFF);
    DEVMODE dm;
    if (panel != 0xFFFFFFFF) {
        RETAILMSG(1, (TEXT("CLASSPAD TOUCH: registry Panel=%d\r\n"), panel));
        return panel != 0;
    }
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    if (!EnumDisplaySettings(NULL, ENUM_CURRENT_SETTINGS, &dm)) {
        RETAILMSG(1, (TEXT("CLASSPAD TOUCH: EnumDisplaySettings failed, no panel assumed\r\n")));
        return FALSE;
    }
    RETAILMSG(1, (TEXT("CLASSPAD TOUCH: display %dx%d -> %s\r\n"), dm.dmPelsWidth, dm.dmPelsHeight,
                  dm.dmPelsHeight >= TOUCH_PANEL_MIN_ROWS ? TEXT("fx-CP400 panel") : TEXT("no touch panel")));
    return dm.dmPelsHeight >= TOUCH_PANEL_MIN_ROWS;
}

//------------------------------------------------------------------------------
//  DDSI
//
static BOOL CalibrationPoint(TPDC_CALIBRATION_POINT *p)
{
    INT w = p->cDisplayWidth, h = p->cDisplayHeight, rx = w / 20, ry = h / 20;
    switch (p->PointNumber) {
    case 0: p->CalibrationX = w / 2;      p->CalibrationY = h / 2;      break;
    case 1: p->CalibrationX = rx * 2;     p->CalibrationY = ry * 2;     break;
    case 2: p->CalibrationX = rx * 2;     p->CalibrationY = h - ry * 2; break;
    case 3: p->CalibrationX = w - rx * 2; p->CalibrationY = h - ry * 2; break;
    case 4: p->CalibrationX = w - rx * 2; p->CalibrationY = ry * 2;     break;
    default:
        p->CalibrationX = w / 2; p->CalibrationY = h / 2;
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    return TRUE;
}

BOOL DdsiTouchPanelGetDeviceCaps(INT iIndex, LPVOID lpOutput)
{
    if (lpOutput == NULL) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    switch (iIndex) {
    case TPDC_SAMPLE_RATE_ID: {
        TPDC_SAMPLE_RATE *r = (TPDC_SAMPLE_RATE *)lpOutput;
        r->SamplesPerSecondLow = TOUCH_SAMPLE_RATE_LOW;
        r->SamplesPerSecondHigh = TOUCH_SAMPLE_RATE_HIGH;
        r->CurrentSampleRateSetting = g_rateSetting;
        return TRUE;
    }
    case TPDC_CALIBRATION_POINT_COUNT_ID: {
        TPDC_CALIBRATION_POINT_COUNT *c = (TPDC_CALIBRATION_POINT_COUNT *)lpOutput;
        c->flags = 0;
        c->cCalibrationPoints = 5;
        return TRUE;
    }
    case TPDC_CALIBRATION_POINT_ID:
        return CalibrationPoint((TPDC_CALIBRATION_POINT *)lpOutput);
    default:
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
}

BOOL DdsiTouchPanelSetMode(INT iIndex, LPVOID lpInput)
{
    switch (iIndex) {
    case TPSM_SAMPLERATE_LOW_ID:  g_rate = TOUCH_SAMPLE_RATE_LOW;  g_rateSetting = 0; return TRUE;
    case TPSM_SAMPLERATE_HIGH_ID: g_rate = TOUCH_SAMPLE_RATE_HIGH; g_rateSetting = 1; return TRUE;
    default: SetLastError(ERROR_INVALID_PARAMETER); return FALSE;
    }
}

BOOL DdsiTouchPanelEnable(void)
{
    DWORD irq = IRQ_TOUCH_PEN;
    g_panel = DetectPanel();
    if (!KernelIoControl(IOCTL_HAL_REQUEST_SYSINTR, &irq, sizeof irq, &gIntrTouch, sizeof gIntrTouch, NULL)) {
        RETAILMSG(1, (TEXT("CLASSPAD TOUCH: no SYSINTR for IRQ %d\r\n"), irq));
        gIntrTouch = SYSINTR_UNDEFINED;
        return FALSE;
    }
    gIntrTouchChanged = SYSINTR_NOP;
    g_prevDown = FALSE;
    g_sampling = FALSE;
    if (g_panel) {
        MMIO_W8(IIC0_ICIC, 0);                      // polled
        IicIdle();
        if (!g_hSampler) {
            g_quit = FALSE;
            g_hevSample = CreateEvent(NULL, FALSE, FALSE, NULL);
            if (g_hevSample) g_hSampler = CreateThread(NULL, 0, SampleThread, NULL, 0, NULL);
            if (!g_hSampler) RETAILMSG(1, (TEXT("CLASSPAD TOUCH: no sample thread (%d), pen-up will not be seen\r\n"), GetLastError()));
        }
    }
    RETAILMSG(1, (TEXT("CLASSPAD TOUCH: enabled, SYSINTR %d, %s\r\n"), gIntrTouch,
                  g_panel ? TEXT("IIC0 sensor") : TEXT("no panel (interrupt never fires)")));
    return TRUE;
}

void DdsiTouchPanelDisable(void)
{
    RETAILMSG(1, (TEXT("CLASSPAD TOUCH: disabled after %d samples, %d contacts\r\n"), g_samples, g_contacts));
    if (g_hSampler) {
        g_quit = TRUE; g_sampling = FALSE;
        SetEvent(g_hevSample);
        WaitForSingleObject(g_hSampler, 500);
        CloseHandle(g_hSampler); g_hSampler = NULL;
        CloseHandle(g_hevSample); g_hevSample = NULL;
    }
    if (gIntrTouch != SYSINTR_NOP && gIntrTouch != SYSINTR_UNDEFINED) {
        KernelIoControl(IOCTL_HAL_RELEASE_SYSINTR, &gIntrTouch, sizeof gIntrTouch, NULL, 0, NULL);
        gIntrTouch = SYSINTR_NOP;
    }
}

LONG DdsiTouchPanelAttach(void)
{
    return ++g_attached;
}

LONG DdsiTouchPanelDetach(void)
{
    return --g_attached;
}

void DdsiTouchPanelPowerHandler(BOOL bOff)
{
    // nothing to switch; called with interrupts off, no messages
    (void)bOff;
}

void DdsiTouchPanelGetPoint(P_TOUCH_PANEL_SAMPLE_FLAGS pTipState, INT *pUncalX, INT *pUncalY)
{
    BOOL down = FALSE;
    g_samples++;
    *pTipState = TouchSampleIgnore;
    *pUncalX = g_lastX;
    *pUncalY = g_lastY;

    if (g_panel && PenDown()) {
        UINT8 pkt[TOUCH_PACKET_BYTES];
        if (TouchReadPacket(pkt, TOUCH_PACKET_BYTES)) {
            UINT contact = (pkt[4] << 8) | pkt[5];
            if (contact & TOUCH_CONTACT_BIT) {
                down = TRUE;
                g_lastX = (INT)(((pkt[0] << 8) | pkt[1]) >> 4);
                g_lastY = (INT)(((pkt[2] << 8) | pkt[3]) >> 4);
            }
        }
    }

    if (down) {
        *pUncalX = g_lastX;
        *pUncalY = g_lastY;
        *pTipState = TouchSampleValidFlag | TouchSampleDownFlag;
        if (!g_prevDown) {
            g_contacts++;
            if (g_contacts <= 20 || (g_contacts % 50) == 0)
                RETAILMSG(1, (TEXT("CLASSPAD TOUCH: down raw (%d,%d) contact %d\r\n"), g_lastX, g_lastY, g_contacts));
            g_sampling = TRUE;                      // the sample thread wakes the IST from here on
            if (g_hevSample) SetEvent(g_hevSample);
        }
    } else {
        if (g_prevDown) {
            *pTipState = TouchSampleValidFlag;      // pen up at the last point
            if (g_contacts <= 20 || (g_contacts % 50) == 0)
                RETAILMSG(1, (TEXT("CLASSPAD TOUCH: up raw (%d,%d) after %d samples\r\n"), g_lastX, g_lastY, g_samples));
        }
        g_sampling = FALSE;                         // a stray wakeup after this lands here again: harmless
        if (gIntrTouch != SYSINTR_NOP && gIntrTouch != SYSINTR_UNDEFINED) InterruptDone(gIntrTouch);
    }
    g_prevDown = down;
}
