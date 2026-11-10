//------------------------------------------------------------------------------
//  battif.c - battery PDD for the Casio ClassPad fx-CP400 (docs/08)
//
//  The four AAA cells are on SH7305 A/D channel AN2. One reading is two
//  single conversions averaged, as Casio's OS does it (0x8002A340), polled
//  without the A/D interrupt; classpad_battery.h turns it into Casio's
//  levels and a percentage. Reported through the public battery MDD:
//    BatteryFlag         HIGH for Casio's levels 1..3, LOW for level 4,
//                        CRITICAL at or below Casio's power-on floor
//    BatteryLifePercent  100 in Casio's level 1, falling to 0 at the floor
//    BatteryVoltage      mV, counts * "MilliVoltsPerCount" (10: inferred
//                        from Casio's thresholds, not measured)
//    BatteryChemistry    alkaline or Ni-MH, the registry choice
//    ACLineStatus        online while a USB cable supplies power: the USB
//                        function controller's VBUS input (INTSTS0.VBSTS),
//                        which Casio's OS reads the same way (0x80016742).
//                        Casio's OS stops the controller when it is not
//                        using it, so every few seconds a thread powers
//                        it the way Casio's USB open does, reads the
//                        level and puts everything back (UsbSample): the
//                        PHY is not left powered and USBE stays off.
//  Registry, HKLM\Software\ClassPad\Battery (read again every 10 s, so a
//  settings program can change it):
//    "Type"                 dword 0 alkaline (default), 1 Ni-MH
//    "MilliVoltsPerCount"   dword, default 10
//  The level latches like Casio's: it only gets worse until the next boot.
//  Not verified on hardware: the divider scale.
//------------------------------------------------------------------------------
#include <windows.h>
#include <battimpl.h>
#include "classpad_mmio.h"
#include "classpad_battery.h"

#define ADC_ADDRC       0xA4610084      // AN2 result, bits 15..6
#define ADC_ADCSR       0xA4610088      // ADF 15, ADIE 14, ADST 13, CKS 7:6, CH 2:0
#define ADC_ADCCSR      0xA461008A
#define ADC_ADCUST      0xA461008C
#define ADCSR_ADF       0x8000
#define ADCSR_ADIE      0x4000
#define ADCSR_ADST      0x2000
#define ADCSR_SETUP     0x0042          // Casio's: clock select 1, single mode, channel 2
#define MSTPCR2         0xA4150038
#define USB_INTSTS0     0xA4D80040      // bit 7 VBSTS: the level of the VBUS input
#define USB_VBSTS       0x0080
#define USB_SYSCFG      0xA4D80000      // bit 10 SCKE: the controller's clock, needed to read its other registers
#define USB_SCKE        0x0400
#define USB_UPONCR      0xA40501D4      // 0x0600 = the USB PHY powered (gint usb.c; Casio's power-down writes 0)
#define USB_UPON        0x0600
#define CPG_USBCLKCR    0xA4150014      // bit 8 CLKSTP: the 48 MHz USB clock stopped
#define USBCLKCR_STOP   0x00000100
#define MSTPCR2_USB     0x00100000      // the USB module stopped
#define MSTPCR2_ADC     0x08000000

#define BATT_REG_KEY    TEXT("Software\\ClassPad\\Battery")
#define SAMPLE_MS       30000           // a reading is reused this long: the cells change over minutes, and every reading costs two conversions
#define CONFIG_MS       10000
#define USB_SAMPLE_MS   5000            // the USB controller is woken this often to look at VBUS
#define USB_MSELCRA     0xA4050180      // pin function select: Casio clears bits 7:6 to use USB, sets them to 01 when done
#define USB_MSELCRA_M   0x00C0
#define USB_MSELCRB     0xA4050182      // Casio clears bits 15:14 to use USB, sets them when done
#define USB_MSELCRB_M   0xC000
#define RTC_R64CNT      0xA413FEC0      // bit 0 = 64 Hz: Casio's delay counts its edges
#define USB_CLOCK_EDGES 14              // about 110 ms

static DWORD g_type = CLASSPAD_BATT_ALKALINE, g_mvPerCount = 10;
static DWORD g_configTick, g_sampleTick;
static BOOL  g_haveConfig, g_haveSample;
static UINT  g_raw;
static int   g_latched, g_logged = -1;

static void ReadConfig(void)
{
    HKEY key;
    DWORD v, type, size, was = g_type;
    g_type = CLASSPAD_BATT_ALKALINE; g_mvPerCount = 10;
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, BATT_REG_KEY, 0, 0, &key) == ERROR_SUCCESS) {
        size = sizeof v;
        if (RegQueryValueEx(key, TEXT("Type"), NULL, &type, (LPBYTE)&v, &size) == ERROR_SUCCESS && type == REG_DWORD)
            g_type = v ? CLASSPAD_BATT_NIMH : CLASSPAD_BATT_ALKALINE;
        size = sizeof v;
        if (RegQueryValueEx(key, TEXT("MilliVoltsPerCount"), NULL, &type, (LPBYTE)&v, &size) == ERROR_SUCCESS && type == REG_DWORD && v >= 1 && v <= 100)
            g_mvPerCount = v;
        RegCloseKey(key);
    }
    if (g_haveConfig && g_type != was) { g_latched = 0; g_logged = -1; }    // another curve: its levels start over
    g_configTick = GetTickCount(); g_haveConfig = TRUE;
}

static void WriteDiagValue(LPCTSTR name, LPCTSTR text)
{
    HKEY key; DWORD disp;
    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, BATT_REG_KEY, 0, NULL, 0, 0, NULL, &key, &disp) == ERROR_SUCCESS) {
        RegSetValueEx(key, name, 0, REG_SZ, (const BYTE *)text, (lstrlen(text) + 1) * sizeof(TCHAR));
        RegCloseKey(key);
    }
}
static void WriteDiag(LPCTSTR text) { WriteDiagValue(TEXT("Diag"), text); }

// One single conversion of AN2: the 10-bit result, or -1 on a timeout.
// The register sequence is the one Casio's battery read makes before every
// conversion (OS 02.01.7003, 0x8002A710 then 0x8002A6CC): ADCCSR bit 13
// off, ADCUST = 0, ADCSR = 0x42, then start. The first version of this
// driver made the OS's boot-time set-up instead (0x800813CA: ADCUST = 0x8000,
// ADCCSR = 0x1CF, which belongs to the OS's other, multi-channel use of the
// converter) and never saw a conversion end on the calculator (2026-11-10),
// though casio-emu, which models only ADCSR, did. Casio also sets ADIE and
// waits for the interrupt; this polls ADF instead and leaves ADIE off.
// What the registers held goes to the registry value "Diag" (the Battery
// program shows it): there is no debug output to read on the device.
static int AdcConvert(void)
{
    UINT16 csr0, csr1 = 0, csr2;
    int i, raw = -1;
    TCHAR diag[96];
    MMIO_W32(MSTPCR2, MMIO_R32(MSTPCR2) & ~MSTPCR2_ADC);
    csr0 = MMIO_R16(ADC_ADCSR);
    MMIO_W16(ADC_ADCCSR, MMIO_R16(ADC_ADCCSR) & 0xDFFF);
    MMIO_W16(ADC_ADCUST, 0);
    MMIO_W16(ADC_ADCSR, MMIO_R16(ADC_ADCSR) & ~ADCSR_ADST);
    MMIO_W16(ADC_ADCSR, ADCSR_SETUP);
    MMIO_W16(ADC_ADCSR, MMIO_R16(ADC_ADCSR) | ADCSR_ADST);
    for (i = 0; i < 20; i++) {              // a conversion takes well under a millisecond
        csr1 = MMIO_R16(ADC_ADCSR);
        if (csr1 & ADCSR_ADF) { raw = MMIO_R16(ADC_ADDRC) >> 6; break; }
        Sleep(1);
    }
    csr2 = MMIO_R16(ADC_ADCSR);             // ADF clears by reading it set, then writing 0
    MMIO_W16(ADC_ADCSR, csr2 & ~(ADCSR_ADF | ADCSR_ADST | ADCSR_ADIE));
    if (raw < 0 || !g_haveSample) {
        wsprintf(diag, TEXT("%s csr %04X>%04X>%04X ccsr %04X cust %04X mstp %08X c %04X"),
                 raw < 0 ? TEXT("timeout") : TEXT("ok"), csr0, csr1, csr2,
                 MMIO_R16(ADC_ADCCSR), MMIO_R16(ADC_ADCUST), MMIO_R32(MSTPCR2), MMIO_R16(ADC_ADDRC));
        WriteDiag(diag);
        RETAILMSG(1, (TEXT("CLASSPAD BATTERY: %s\r\n"), diag));
    }
    return raw;
}

static BOOL Sample(void)
{
    int a, b;
    if (g_haveSample && GetTickCount() - g_sampleTick < SAMPLE_MS) return TRUE;
    a = AdcConvert();
    b = a < 0 ? -1 : AdcConvert();
    if (a < 0 || b < 0) return g_haveSample;        // keep the last good reading
    g_raw = (UINT)(a + b) / 2;
    g_sampleTick = GetTickCount(); g_haveSample = TRUE;
    return TRUE;
}

// A USB cable is supplying power: the controller's VBUS input level.
//
// The first version only read INTSTS0 and never saw a cable on the
// calculator (2026-11-10). Casio's OS keeps the controller stopped while
// nothing uses it, and a stopped module reads 0. Its USB open (OS
// 02.01.7003, 0x8000CFD4) powers the controller (0x800089E8), reads VBSTS
// once (0x80016742) and gives up with error 3 when it is 0; the power-down
// is 0x80008A82. UsbSample does the same every USB_SAMPLE_MS, in Casio's
// order:
//     MSELCRA &= ~0x00C0, MSELCRB &= ~0xC000     (pin functions)
//     USBCLKCR &= ~0x100                         (the USB clock runs)
//     14 edges of RTC R64CNT bit 0               (about 110 ms)
//     MSTPCR2 &= ~bit 20                         (the module runs)
//     UPONCR = 0x0600                            (the PHY)
//     read INTSTS0
// and back, in the power-down's order: SYSCFG, UPONCR, MSTPCR2, USBCLKCR,
// MSELCRB, MSELCRA, each to exactly what it held before. A controller that
// was already running is read as it is and left alone. USBE is never set,
// so nothing shows up on the bus.
// Left out of Casio's sequence: the interrupt controller (masks and
// priorities: no USB interrupt is used here) and its reads and zero writes
// of 0xFE00807C / 0xFE00808C (unidentified registers; gint's USB driver
// works on this calculator without them).
// One step more than Casio: when VBSTS reads 0, SYSCFG.SCKE is set and the
// register read again (the controller's manual wants SCKE for every
// register but SYSCFG; Casio reads without it). Both readings go to the
// registry value "UsbDiag" with what the registers held: there is no debug
// output to read on the device. Works on the calculator (2026-11-10: the
// Battery program shows "USB power connected" with a cable in); which of
// the two readings saw it was not recorded.
//
// The 110 ms make this a thread of its own, so nobody asking for the
// battery status waits. Everything is guarded: where the controller is not
// there to read (casio-emu's ClassPad profile has none: every read raised an
// exception in device.exe) the thread ends and the driver reports battery
// power.
static volatile BOOL g_usbOn;
static TCHAR g_usbDiag[96];

static void UsbClockDelay(void)
{
    UINT8 last = (UINT8)(MMIO_R8(RTC_R64CNT) & 1), now;
    DWORD start = GetTickCount();
    int edges = 0;
    while (edges < USB_CLOCK_EDGES && GetTickCount() - start < 300) {  // the RTC not counting: 300 ms at most
        Sleep(1);
        now = (UINT8)(MMIO_R8(RTC_R64CNT) & 1);
        if (now != last) { last = now; edges++; }
    }
}

static BOOL UsbVbus(UINT16 *raw)
{
    UINT16 a, b, c;
    int i;
    *raw = MMIO_R16(USB_INTSTS0);
    for (i = 0; i < 8; i++) {           // three reads in a row that agree: the bit is not latched
        a = MMIO_R16(USB_INTSTS0) & USB_VBSTS;
        b = MMIO_R16(USB_INTSTS0) & USB_VBSTS;
        c = MMIO_R16(USB_INTSTS0) & USB_VBSTS;
        if (a == b && b == c) return a != 0;
    }
    return FALSE;
}

static BOOL UsbSample(void)
{
    UINT32 clk0, mstp0;
    UINT16 upon0, sys0, msa0, msb0, raw1, raw2 = 0;
    BOOL woke, on;
    TCHAR diag[96];
    msa0 = MMIO_R16(USB_MSELCRA); msb0 = MMIO_R16(USB_MSELCRB);
    clk0 = MMIO_R32(CPG_USBCLKCR); mstp0 = MMIO_R32(MSTPCR2);
    upon0 = MMIO_R16(USB_UPONCR);
    woke = (clk0 & USBCLKCR_STOP) || (mstp0 & MSTPCR2_USB) || upon0 != USB_UPON;
    if (woke) {
        MMIO_W16(USB_MSELCRA, msa0 & ~USB_MSELCRA_M);
        MMIO_W16(USB_MSELCRB, msb0 & ~USB_MSELCRB_M);
        MMIO_W32(CPG_USBCLKCR, clk0 & ~USBCLKCR_STOP);
        UsbClockDelay();
        MMIO_W32(MSTPCR2, MMIO_R32(MSTPCR2) & ~MSTPCR2_USB);
        MMIO_W16(USB_UPONCR, USB_UPON);
    }
    sys0 = MMIO_R16(USB_SYSCFG);
    on = UsbVbus(&raw1);
    if (!on && !(sys0 & USB_SCKE)) {
        MMIO_W16(USB_SYSCFG, (UINT16)(sys0 | USB_SCKE));
        Sleep(1);
        on = UsbVbus(&raw2);
        MMIO_W16(USB_SYSCFG, sys0);
    }
    if (woke) {
        MMIO_W16(USB_UPONCR, upon0);
        if (mstp0 & MSTPCR2_USB) MMIO_W32(MSTPCR2, MMIO_R32(MSTPCR2) | MSTPCR2_USB);
        if (clk0 & USBCLKCR_STOP) MMIO_W32(CPG_USBCLKCR, MMIO_R32(CPG_USBCLKCR) | USBCLKCR_STOP);
        MMIO_W16(USB_MSELCRB, msb0);
        MMIO_W16(USB_MSELCRA, msa0);
    }
    wsprintf(diag, TEXT("ms %04X %04X clk %X mstp %u upon %X sys %X ints %04X %04X"),
             msa0, msb0, clk0, (mstp0 & MSTPCR2_USB) != 0, upon0, sys0, raw1, raw2);
    if (lstrcmp(diag, g_usbDiag)) {     // the registry only when something changes
        lstrcpy(g_usbDiag, diag);
        WriteDiagValue(TEXT("UsbDiag"), diag);
        RETAILMSG(1, (TEXT("CLASSPAD BATTERY: USB %s\r\n"), diag));
    }
    return on;
}

static DWORD WINAPI UsbThread(LPVOID unused)
{
    UNREFERENCED_PARAMETER(unused);
    for (;;) {
        __try {
            g_usbOn = UsbSample();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_usbOn = FALSE;
            RETAILMSG(1, (TEXT("CLASSPAD BATTERY: no USB controller to read, AC line reported offline\r\n")));
            WriteDiagValue(TEXT("UsbDiag"), TEXT("no USB controller to read"));
            return 0;
        }
        Sleep(USB_SAMPLE_MS);
    }
}

static BOOL UsbPowered(void) { return g_usbOn; }

BOOL WINAPI BatteryPDDInitialize(LPCTSTR pszRegistryContext)
{
    HANDLE thread;
    UNREFERENCED_PARAMETER(pszRegistryContext);
    ReadConfig();
    thread = CreateThread(NULL, 0, UsbThread, NULL, 0, NULL);
    if (thread) CloseHandle(thread);
    RETAILMSG(1, (TEXT("CLASSPAD BATTERY: A/D channel 2, %s curve, %u mV per count\r\n"),
                  g_type == CLASSPAD_BATT_NIMH ? TEXT("Ni-MH") : TEXT("alkaline"), g_mvPerCount));
    return TRUE;
}

void WINAPI BatteryPDDDeinitialize(void) {}

// After a suspend take a fresh reading (every conversion sets the converter up again)
void WINAPI BatteryPDDResume(void) { g_haveSample = FALSE; }

void WINAPI BatteryPDDPowerHandler(BOOL bOff) { UNREFERENCED_PARAMETER(bOff); }

BOOL WINAPI BatteryPDDGetStatus(PSYSTEM_POWER_STATUS_EX2 pstatus, PBOOL pfBatteriesChangedSinceLastCall)
{
    int level;
    if (!g_haveConfig || GetTickCount() - g_configTick >= CONFIG_MS) ReadConfig();
    memset(pstatus, 0, sizeof *pstatus);
    pstatus->ACLineStatus = UsbPowered() ? AC_LINE_ONLINE : AC_LINE_OFFLINE;
    pstatus->BatteryLifeTime = BATTERY_LIFE_UNKNOWN;
    pstatus->BatteryFullLifeTime = BATTERY_LIFE_UNKNOWN;
    // There is no backup battery. UNKNOWN, not NO_BATTERY: the taskbar (explorer
    // taskbar\power.cpp) answers NO_BATTERY with a "Backup Battery Very Low"
    // dialog at every boot and treats UNKNOWN as nothing to say.
    pstatus->BackupBatteryFlag = BATTERY_FLAG_UNKNOWN;
    pstatus->BackupBatteryLifePercent = BATTERY_PERCENTAGE_UNKNOWN;
    pstatus->BackupBatteryLifeTime = BATTERY_LIFE_UNKNOWN;
    pstatus->BackupBatteryFullLifeTime = BATTERY_LIFE_UNKNOWN;
    pstatus->BatteryChemistry = g_type == CLASSPAD_BATT_NIMH ? BATTERY_CHEMISTRY_NIMH : BATTERY_CHEMISTRY_ALKALINE;
    if (pfBatteriesChangedSinceLastCall) *pfBatteriesChangedSinceLastCall = FALSE;
    if (!Sample()) {
        pstatus->BatteryFlag = BATTERY_FLAG_UNKNOWN;
        pstatus->BatteryLifePercent = BATTERY_PERCENTAGE_UNKNOWN;
        return TRUE;
    }
    level = ClasspadBattLatch(&g_latched, ClasspadBattLevel(g_raw, (int)g_type));
    pstatus->BatteryFlag = level >= CLASSPAD_BATT_LEVEL_FLOOR ? BATTERY_FLAG_CRITICAL :
                           level == 4 ? BATTERY_FLAG_LOW : BATTERY_FLAG_HIGH;
    pstatus->BatteryLifePercent = (BYTE)ClasspadBattPercent(g_raw, (int)g_type);
    pstatus->BatteryVoltage = g_raw * g_mvPerCount;
    if (level != g_logged) {                // the raw count, to check the scale against Casio's display
        g_logged = level;
        RETAILMSG(1, (TEXT("CLASSPAD BATTERY: raw %u (%u mV), level %d, %u%%\r\n"),
                      g_raw, g_raw * g_mvPerCount, level, pstatus->BatteryLifePercent));
    }
    return TRUE;
}

// Three main battery levels (high, low, critical), no backup battery
LONG BatteryPDDGetLevels(void) { return MAKELONG(3, 0); }

BOOL BatteryPDDSupportsChangeNotification(void) { return FALSE; }
