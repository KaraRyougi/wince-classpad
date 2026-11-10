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
//  Registry, HKLM\Software\ClassPad\Battery (read again every 10 s, so a
//  settings program can change it):
//    "Type"                 dword 0 alkaline (default), 1 Ni-MH
//    "MilliVoltsPerCount"   dword, default 10
//  The level latches like Casio's: it only gets worse until the next boot.
//  Not verified on hardware: the divider scale, and whether the A/D module
//  needs more than its module-stop bit cleared after Casio's OS hands over.
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
#define MSTPCR2_ADC     0x08000000

#define BATT_REG_KEY    TEXT("Software\\ClassPad\\Battery")
#define SAMPLE_MS       2000            // a reading is reused this long
#define CONFIG_MS       10000

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

// The A/D set-up Casio's OS makes at boot (0x800813CA), with the module clock on
static void AdcInit(void)
{
    MMIO_W32(MSTPCR2, MMIO_R32(MSTPCR2) & ~MSTPCR2_ADC);
    MMIO_W16(ADC_ADCCSR, MMIO_R16(ADC_ADCCSR) & 0xDFFF);
    MMIO_W16(ADC_ADCUST, 0);
    MMIO_W16(ADC_ADCSR, ADCSR_SETUP);
    MMIO_W16(ADC_ADCUST, 0x8000);
    MMIO_W16(ADC_ADCCSR, 0x01CF);
}

// One single conversion of AN2: the 10-bit result, or -1 on a timeout
static int AdcConvert(void)
{
    UINT16 csr;
    int i, raw = -1;
    MMIO_W32(MSTPCR2, MMIO_R32(MSTPCR2) & ~MSTPCR2_ADC);
    csr = (UINT16)((MMIO_R16(ADC_ADCSR) & ~(ADCSR_ADIE | ADCSR_ADF | 0x0037)) | (ADCSR_SETUP & 0x0037));
    MMIO_W16(ADC_ADCSR, csr | ADCSR_ADST);
    for (i = 0; i < 20; i++) {              // a conversion takes well under a millisecond
        if (MMIO_R16(ADC_ADCSR) & ADCSR_ADF) { raw = MMIO_R16(ADC_ADDRC) >> 6; break; }
        Sleep(1);
    }
    csr = MMIO_R16(ADC_ADCSR);              // ADF clears by reading it set, then writing 0
    MMIO_W16(ADC_ADCSR, csr & ~(ADCSR_ADF | ADCSR_ADST | ADCSR_ADIE));
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

BOOL WINAPI BatteryPDDInitialize(LPCTSTR pszRegistryContext)
{
    UNREFERENCED_PARAMETER(pszRegistryContext);
    AdcInit();
    ReadConfig();
    RETAILMSG(1, (TEXT("CLASSPAD BATTERY: A/D channel 2, %s curve, %u mV per count\r\n"),
                  g_type == CLASSPAD_BATT_NIMH ? TEXT("Ni-MH") : TEXT("alkaline"), g_mvPerCount));
    return TRUE;
}

void WINAPI BatteryPDDDeinitialize(void) {}

// After a suspend the conversion set-up may be gone (R-standby powers the core off)
void WINAPI BatteryPDDResume(void) { AdcInit(); g_haveSample = FALSE; }

void WINAPI BatteryPDDPowerHandler(BOOL bOff) { UNREFERENCED_PARAMETER(bOff); }

BOOL WINAPI BatteryPDDGetStatus(PSYSTEM_POWER_STATUS_EX2 pstatus, PBOOL pfBatteriesChangedSinceLastCall)
{
    int level;
    if (!g_haveConfig || GetTickCount() - g_configTick >= CONFIG_MS) ReadConfig();
    memset(pstatus, 0, sizeof *pstatus);
    pstatus->ACLineStatus = AC_LINE_OFFLINE;
    pstatus->BatteryLifeTime = BATTERY_LIFE_UNKNOWN;
    pstatus->BatteryFullLifeTime = BATTERY_LIFE_UNKNOWN;
    pstatus->BackupBatteryFlag = BATTERY_FLAG_NO_BATTERY;
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
