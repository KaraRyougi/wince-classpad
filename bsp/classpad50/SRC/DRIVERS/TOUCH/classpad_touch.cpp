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
#include "classpad_pwrstat.h"

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
static volatile BOOL g_wakePending;             // after a resume: the sample thread powers the sensor up (it may sleep, the power handler may not)
static volatile BOOL g_powerSwitch;             // suspend option: power the sensor down (off by default; the sample thread reads the registry)
static volatile BOOL g_poweredDown;

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

// The sensor's set-up, read back once while it still has what Casio's OS
// wrote (DdsiTouchPanelEnable), for SensorPowerUp to write again.
static const UINT8 g_cfgReg[TOUCH_CFG_N] = TOUCH_CFG_REGS;
static const UINT8 g_cfgOs[TOUCH_CFG_N]  = TOUCH_CFG_OS_VALUES;
static UINT8 g_cfg[TOUCH_CFG_N];
static BOOL  g_cfgRead;                         // the read-back agreed with the ROM's table: g_cfg is the sensor's own

static void SensorConfigCapture(void)
{
    UINT8 v[TOUCH_CFG_N];
    int i;
    BOOL ok = TRUE;
    for (i = 0; i < TOUCH_CFG_N; i++) {
        v[i] = 0;
        if (!TouchBusReadReg(g_cfgReg[i], &v[i], 1)) ok = FALSE;
    }
    for (i = 0; i < TOUCH_CFG_N; i++)
        if ((TOUCH_CFG_OS_KNOWN & (1 << i)) && v[i] != g_cfgOs[i]) ok = FALSE;
    g_cfgRead = ok;
    for (i = 0; i < TOUCH_CFG_N; i++) g_cfg[i] = ok ? v[i] : g_cfgOs[i];
    TouchBusReset();
    RETAILMSG(1, (TEXT("CLASSPAD TOUCH: sensor set-up %02x %02x %02x %02x %02x %02x %02x %02x (%s)\r\n"),
                  v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7],
                  ok ? TEXT("as the OS wrote it") : TEXT("not readable: the ROM's table is used, 0x18 and 0x20 are left alone")));
}

static BOOL SensorPollStatus(UINT8 mask, UINT8 want, int polls)
{
    UINT8 st = (UINT8)(want ^ mask);            // as the OS starts: not there yet
    int i;
    for (i = 0; i < polls; i++) {
        if ((st & mask) == want) return TRUE;
        if (!TouchBusReadReg(TOUCH_REG_STATUS, &st, 1)) st = (UINT8)(want ^ mask);
        Sleep(TOUCH_POLL_MS);
    }
    return (st & mask) == want;
}

// The sensor back on after a suspend, the way the OS's touch start-up does
// it (0x8008AB30, classpad_touch.h): power down, power up (PH4 low, 20 ms,
// command 0x02, 0xA405019E bit 5), the set-up registers, status bit 0 clear,
// command 0xA8, status bit 1 set; two such passes, then command 0x80. Until
// 2026-11-10 this sent the 0x02 alone and the sensor came back deaf.
static void SensorPowerUp(void)
{
    BOOL variant = (MMIO_R8(TOUCH_VARIANT_PORT) & 1) == 0;
    int polls = variant ? TOUCH_POLLS_VARIANT : TOUCH_POLLS;
    int pass = 0, attempt, i;
    BOOL ok = FALSE;
    for (attempt = 0; attempt < 5 && pass < 2; attempt++) {
        TouchBusReset();
        Sleep(TOUCH_POLL_MS);
        MMIO_W8(TOUCH_X19E, MMIO_R8(TOUCH_X19E) & ~TOUCH_X19E_ON);
        MMIO_W8(TOUCH_PHDR, MMIO_R8(TOUCH_PHDR) | TOUCH_PHDR_OFF);
        Sleep(TOUCH_POLL_MS);
        MMIO_W8(TOUCH_PHDR, MMIO_R8(TOUCH_PHDR) & ~TOUCH_PHDR_OFF);
        Sleep(TOUCH_POWER_UP_MS);
        ok = TouchBusCommand(TOUCH_CMD_START);
        MMIO_W8(TOUCH_X19E, MMIO_R8(TOUCH_X19E) | TOUCH_X19E_ON);
        for (i = 0; i < TOUCH_CFG_N; i++) {
            if (i == TOUCH_CFG_IDX_X34 && !variant) continue;
            if ((i == TOUCH_CFG_IDX_X18 || i == TOUCH_CFG_IDX_X20) && !g_cfgRead) continue;
            if (!TouchBusWriteReg(g_cfgReg[i], g_cfg[i])) ok = FALSE;
        }
        if (!SensorPollStatus(TOUCH_STATUS_BUSY, 0, polls)) { ok = FALSE; continue; }
        if (!TouchBusCommand(TOUCH_CMD_CALIBRATE)) ok = FALSE;
        if (!SensorPollStatus(TOUCH_STATUS_DONE, TOUCH_STATUS_DONE, polls)) { ok = FALSE; continue; }
        Sleep(TOUCH_POLL_MS);
        pass++;
    }
    if (!TouchBusCommand(TOUCH_CMD_RUN)) ok = FALSE;
    Sleep(TOUCH_POLL_MS);
    TouchBusReset();
    RETAILMSG(1, (TEXT("CLASSPAD TOUCH: sensor powered up, %d of 2 passes in %d attempts, %s\r\n"),
                  pass, attempt, ok ? TEXT("every step acknowledged") : TEXT("not every step acknowledged")));
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
    DWORD optionPoll = 0;
    CeSetThreadPriority(GetCurrentThread(), (int)RegDword(TEXT("Priority256"), TOUCH_THREAD_PRIORITY));
    while (!g_quit) {
        DWORD rc = WaitForSingleObject(g_hevSample, 1000 / (DWORD)g_rate);
        if (rc != WAIT_TIMEOUT || g_quit) break;
        if ((++optionPoll & 63) == 0) {         // the suspend options (classpad_pwrstat.h), every 64 polls
            HKEY key; DWORD v = 0, type, size = sizeof v;
            if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, TEXT("Software\\ClassPad\\Suspend"), 0, 0, &key) == ERROR_SUCCESS) {
                if (RegQueryValueEx(key, TEXT("Options"), NULL, &type, (LPBYTE)&v, &size) != ERROR_SUCCESS || type != REG_DWORD) v = CLASSPAD_SUSPEND_OPT_DEFAULT;
                RegCloseKey(key);
            } else v = CLASSPAD_SUSPEND_OPT_DEFAULT;
            g_powerSwitch = (v & CLASSPAD_SUSPEND_OPT_TOUCH) != 0;
        }
        if (g_wakePending) {                    // resumed: nobody else uses the bus while g_suspended is set
            SensorPowerUp();
            g_wakePending = FALSE;
            g_suspended = FALSE;
            g_sampling = TRUE;
        }
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
        SensorConfigCapture();
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

// Off: the bus stopped and, with the suspend option set (off by default,
// classpad_pwrstat.h), the sensor powered down as the OS does before its
// standby. On: after a power-down the sample thread powers it up, and until
// then g_suspended keeps everybody off the bus.
void DdsiTouchPanelPowerHandler(BOOL bOff)
{
    g_iconArmed = -1;
    if (!g_panel) { g_suspended = bOff; return; }
    if (bOff) {
        g_suspended = TRUE;
        g_wakePending = FALSE;
        MMIO_W8(IIC0_ICCR, 0);
        g_poweredDown = g_powerSwitch;
        if (g_poweredDown) {
            MMIO_W8(TOUCH_X19E, MMIO_R8(TOUCH_X19E) & ~TOUCH_X19E_ON);
            MMIO_W8(TOUCH_PHDR, MMIO_R8(TOUCH_PHDR) | TOUCH_PHDR_OFF);
        }
    } else if (g_poweredDown) {
        g_wakePending = TRUE;                   // g_suspended stays set until the sample thread has powered the sensor up
    } else {
        TouchBusReset();                        // as before the power switch existed
        g_sampling = TRUE;
        g_suspended = FALSE;
    }
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
