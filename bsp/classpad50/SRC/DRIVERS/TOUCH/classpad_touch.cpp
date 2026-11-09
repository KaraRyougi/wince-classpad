//------------------------------------------------------------------------------
//  classpad_touch.cpp - DDSI layer of touch.dll for the fx-CP400 panel
//
//  The MDD owns the interrupt event and calls GetPoint. A separate sampler
//  polls the contact pin even before the first down, so missing/unverified
//  pen IRQ routing cannot prevent all input. Only the MDD reads IIC0.
//  Keep the MDD wait infinite (CE WaitOneMore interrupt-event timeout bug).
//  The sampler waits on an ordinary stop event with a timeout instead.
//  ClassPad command 0x84 returns x1,y1,z1,gh,x2,y2,z2,dm, NOT contact flags.
//  Contact comes from Port R bit 5; calibration is supplied by the MDD.
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
static BOOL     g_stripContact;
static INT      g_iconArmed = -1;               // icon under the pen since the down

// The icon strip below the native 320x528 LCD, y=528..575, in physical sensor
// coordinates (independent of desktop rotation): Settings, Menu, Main,
// Resize, Swap, Rotate, Esc, 320/7 pixels each (casio-emu's panel and
// CASIO's fx-CP400 guide). A release on the icon the pen went down on sets
// its named event; classpad_rotatekey.exe acts on it outside GWES. Resize and
// Swap have none.
#define STRIP_X(px)     (0x20B + (px)*0x9B6/256)
#define STRIP_Y(py)     (0xF4 + (py)*0x66F/256)
#define STRIP_ICONS     7
static const INT     g_iconLeft[STRIP_ICONS + 1] = { 0, 46, 91, 137, 183, 229, 274, 320 };
static const LPCTSTR g_iconEventName[STRIP_ICONS] = {
    TEXT("ClassPadIconSettings"), TEXT("ClassPadIconMenu"), TEXT("ClassPadIconMain"),
    NULL, NULL, TEXT("ClassPadRotateKey"), TEXT("ClassPadIconEsc")
};
static const LPCTSTR g_iconName[STRIP_ICONS] = {
    TEXT("Settings"), TEXT("Menu"), TEXT("Main"), TEXT("Resize"), TEXT("Swap"), TEXT("Rotate"), TEXT("Esc")
};
static HANDLE   g_iconEvent[STRIP_ICONS];

static INT IconAt(INT x, INT y)
{
    INT i;
    if (y < STRIP_Y(528) || y >= STRIP_Y(576)) return -1;
    for (i = 0; i < STRIP_ICONS; i++)
        if (x >= STRIP_X(g_iconLeft[i]) && x < STRIP_X(g_iconLeft[i + 1])) return i;
    return -1;
}

static void IconEventsOpen(void)
{
    INT i;
    for (i = 0; i < STRIP_ICONS; i++)
        if (g_iconEventName[i] && !g_iconEvent[i]) g_iconEvent[i] = CreateEvent(NULL, FALSE, FALSE, g_iconEventName[i]);
}

static void IconEventsClose(void)
{
    INT i;
    for (i = 0; i < STRIP_ICONS; i++)
        if (g_iconEvent[i]) { CloseHandle(g_iconEvent[i]); g_iconEvent[i] = NULL; }
}
static INT      g_lastX, g_lastY;
static LONG     g_attached;
static DWORD    g_samples, g_contacts, g_busErrors;
static HANDLE   g_hSampler;                     // sample thread
static HANDLE   g_hevSample;                    // manual-reset shutdown event
static volatile BOOL g_sampling;                // stylus down: keep waking the IST
static volatile BOOL g_quit, g_suspended;

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
//  Polled IIC state machine, shared with the host protocol regression test.
#include "classpad_touch_bus.h"

static BOOL TouchReadPacket(UINT8 *pkt, int n)
{
    if (TouchBusRead(pkt, n)) return TRUE;
    g_busErrors++;
    if (g_busErrors <= 5 || (g_busErrors % 100) == 0)
        RETAILMSG(1, (TEXT("CLASSPAD TOUCH: IIC error %d stage=%d status=%02x\r\n"),
                      g_busErrors, g_touchBusStage, g_touchBusStatus));
    return FALSE;
}

static BOOL PenDown(void)
{
    return (MMIO_R8(SH7305_REG_PA_PRDR) & TOUCH_PRDR_CONTACT) == 0;
}

//------------------------------------------------------------------------------
//  Poll for the first down as well as movement/up. The sampler never reads
//  IIC: the MDD serializes all transfers. A failed packet is retried while
//  contact stays low, even if no down has been accepted yet.
static DWORD WINAPI SampleThread(LPVOID)
{
    CeSetThreadPriority(GetCurrentThread(), (int)RegDword(TEXT("Priority256"), TOUCH_THREAD_PRIORITY));
    while (!g_quit) {
        DWORD rc = WaitForSingleObject(g_hevSample, 1000 / (DWORD)g_rate);
        if (rc != WAIT_TIMEOUT || g_quit) break;
        if (!g_suspended && hTouchPanelEvent && (PenDown() || g_sampling)) SetEvent(hTouchPanelEvent);
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
    BOOL tallPanel = dm.dmPelsHeight >= TOUCH_PANEL_MIN_ROWS || dm.dmPelsWidth >= TOUCH_PANEL_MIN_ROWS;
    RETAILMSG(1, (TEXT("CLASSPAD TOUCH: display %dx%d -> %s\r\n"), dm.dmPelsWidth, dm.dmPelsHeight,
                  tallPanel ? TEXT("fx-CP400 panel") : TEXT("no touch panel")));
    return tallPanel;
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
    g_stripContact = FALSE;
    g_iconArmed = -1;
    IconEventsOpen();
    g_sampling = FALSE;
    if (g_panel) {
        // Own the bus and pins instead of depending on the Casio OS state.
        MMIO_W32(TOUCH_MSTPCR2, MMIO_R32(TOUCH_MSTPCR2) & ~TOUCH_IIC_STOP);
        MMIO_W16(TOUCH_PJCR, MMIO_R16(TOUCH_PJCR) & ~0x0F00); // PJ4/5: IIC
        MMIO_W16(TOUCH_PRCR, (MMIO_R16(TOUCH_PRCR) & ~0x0C00) | 0x0C00); // PR5 input
        MMIO_W8(INTC_IMR(7), 0xF0);                 // IIC IRQs masked; status is polled
        TouchBusReset();
        if (!g_hSampler) {
            g_quit = FALSE;
            g_hevSample = CreateEvent(NULL, TRUE, FALSE, NULL);
            if (g_hevSample) g_hSampler = CreateThread(NULL, 0, SampleThread, NULL, 0, NULL);
            if (!g_hSampler) {
                if (g_hevSample) { CloseHandle(g_hevSample); g_hevSample = NULL; }
                KernelIoControl(IOCTL_HAL_RELEASE_SYSINTR, &gIntrTouch, sizeof gIntrTouch, NULL, 0, NULL);
                gIntrTouch = SYSINTR_UNDEFINED;
                return FALSE;
            }
        }
    }
    RETAILMSG(1, (TEXT("CLASSPAD TOUCH: enabled, SYSINTR %d, %s\r\n"), gIntrTouch,
                  g_panel ? TEXT("IIC0 sensor") : TEXT("no panel (interrupt never fires)")));
    return TRUE;
}

void DdsiTouchPanelDisable(void)
{
    RETAILMSG(1, (TEXT("CLASSPAD TOUCH: disabled after %d samples, %d contacts\r\n"), g_samples, g_contacts));
    IconEventsClose();
    if (g_hSampler) {
        g_quit = TRUE; g_sampling = FALSE;
        SetEvent(g_hevSample);
        WaitForSingleObject(g_hSampler, INFINITE);
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
    g_suspended = bOff;
    g_iconArmed = -1;
    if (!g_panel) return;
    if (bOff) MMIO_W8(IIC0_ICCR, 0);
    else { TouchBusReset(); g_sampling = TRUE; }
}

void DdsiTouchPanelGetPoint(P_TOUCH_PANEL_SAMPLE_FLAGS pTipState, INT *pUncalX, INT *pUncalY)
{
    BOOL down = FALSE;
    g_samples++;
    *pTipState = TouchSampleIgnore;
    *pUncalX = g_lastX;
    *pUncalY = g_lastY;

    if (g_suspended) return;
    if (g_panel && PenDown()) {
        UINT8 pkt[TOUCH_PACKET_BYTES];
        if (TouchReadPacket(pkt, TOUCH_PACKET_BYTES)) {
            if (PenDown()) {                       // z1 is a measurement, not a flag
                down = TRUE;
                g_lastX = (INT)(((pkt[0] << 8) | pkt[1]) >> 4);
                g_lastY = (INT)(((pkt[2] << 8) | pkt[3]) >> 4);
            }
        } else {
            // A bus error is not a pen-up. Keep the previous contact and
            // retry on the next poll; only the contact pin ends a stroke.
            if (PenDown()) return;
        }
    }

    if (down && !g_prevDown) {
        g_stripContact = g_lastY >= STRIP_Y(528);
        g_iconArmed = g_stripContact ? IconAt(g_lastX, g_lastY) : -1;
    }
    if (g_stripContact) {
        if (down && IconAt(g_lastX, g_lastY) != g_iconArmed) g_iconArmed = -1;   // slid off: cancel
        if (!down) {
            if (g_iconArmed >= 0) {
                if (g_iconEvent[g_iconArmed]) SetEvent(g_iconEvent[g_iconArmed]);
                RETAILMSG(1, (TEXT("CLASSPAD TOUCH: %s released\r\n"), g_iconName[g_iconArmed]));
            }
            g_stripContact = FALSE;
            g_iconArmed = -1;
            if (gIntrTouch != SYSINTR_NOP && gIntrTouch != SYSINTR_UNDEFINED) InterruptDone(gIntrTouch);
        }
        g_sampling = down;
        g_prevDown = down;
        return; // Never clamp an icon-strip contact into a desktop click.
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
