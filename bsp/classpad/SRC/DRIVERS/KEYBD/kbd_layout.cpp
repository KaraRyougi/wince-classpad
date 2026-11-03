//------------------------------------------------------------------------------
//  kbd_layout.cpp - device layouts of the KEYSC PDD: scan code -> virtual key
//
//  The Layout Manager finds the device layout of a PDD through the registry
//  (HKLM\System\CurrentControlSet\Control\Layouts\<HKL>\<PDD name> = DLL)
//  and calls the export named after the PDD (both "ClassPad" and
//  "ClassPad_00000409" are exported). Two keypads sit on the same KEYSC:
//  the fx-CG100 (what casio-emu runs the translator on) and the fx-CP400;
//  HKLM\HARDWARE\DEVICEMAP\KEYBD "Matrix" picks the table (0 / 1).
//
//  Key positions are casio-emu's setKeydown codes (row = code % 10,
//  column = code / 10 - 1) from the skins src/gui/skin/cg100/skin_cg100.h and
//  classpad_keys.inc; scan code = 8 * row + column (classpad_keysc.h). The
//  virtual keys are a choice: digits and operators as themselves, OK/EXE as
//  Return, D-pad as the arrows, DEL as Backspace, and the named keys on the
//  function keys (see the tables).
//------------------------------------------------------------------------------
#include <windows.h>
#include <keybddr.h>
#include <laymgr.h>
#include <devicelayout.h>
#include "bsp_base_regs.h"
#include "classpad_keysc.h"

typedef struct { UINT8 code; UINT8 vk; } KEYSC_KEY;

// fx-CG100 keypad
static const KEYSC_KEY g_keysCg100[] = {
    { 79, VK_F12 },         // ON
    { 69, VK_LWIN },        // HOME (main menu)
    { 59, VK_HOME },        // |<- (begin)
    { 49, VK_UP },
    { 39, VK_END },         // ->| (end)
    { 29, VK_PRIOR },       // page up
    { 78, VK_APPS },        // SETTINGS
    { 68, VK_ESCAPE },      // BACK
    { 58, VK_LEFT },
    { 48, VK_RETURN },      // OK (D-pad centre)
    { 38, VK_RIGHT },
    { 28, VK_NEXT },        // page down
    { 77, VK_SHIFT },       // SHIFT
    { 67, VK_CAPITAL },     // ALPHA
    { 57, VK_F2 },          // VAR
    { 47, VK_DOWN },
    { 37, VK_F3 },          // CATALOG
    { 27, VK_F4 },          // TOOLS
    { 76, 'X' },            // x,theta,T
    { 66, VK_F7 },          // fraction
    { 56, VK_F8 },          // square root
    { 46, VK_F11 },         // x^y
    { 36, VK_F9 },          // x^2
    { 26, VK_F10 },         // exp
    { 75, VK_COMMA },
    { 65, 'S' },            // sin
    { 55, 'C' },            // cos
    { 45, 'T' },            // tan
    { 35, VK_LBRACKET },    // (
    { 25, VK_RBRACKET },    // )
    { 74, '7' }, { 64, '8' }, { 54, '9' },
    { 44, VK_BACK },        // DEL
    { 10, VK_CANCEL },      // AC
    { 73, '4' }, { 63, '5' }, { 53, '6' },
    { 43, VK_MULTIPLY },
    { 33, VK_DIVIDE },
    { 72, '1' }, { 62, '2' }, { 52, '3' },
    { 42, VK_ADD },
    { 32, VK_SUBTRACT },
    { 71, '0' },
    { 61, VK_PERIOD },
    { 51, VK_F5 },          // x10^n
    { 41, VK_F6 },          // FORMAT
    { 31, VK_RETURN },      // EXE
};

// fx-CP400 keypad
static const KEYSC_KEY g_keysCp400[] = {
    { 20, VK_ESCAPE },      // Clear
    { 81, VK_SHIFT },       // Shift
    { 85, VK_APPS },        // Keyboard (soft keyboard)
    { 73, VK_LEFT }, { 84, VK_UP }, { 74, VK_DOWN }, { 83, VK_RIGHT },
    { 76, 'X' }, { 75, 'Y' }, { 63, 'Z' },
    { 86, VK_EQUAL },
    { 66, VK_LBRACKET },    // (
    { 56, VK_RBRACKET },    // )
    { 82, VK_BACK },        // Del
    { 65, '7' }, { 64, '8' }, { 62, '9' },
    { 71, VK_DIVIDE },
    { 55, '4' }, { 54, '5' }, { 52, '6' },
    { 61, VK_MULTIPLY },
    { 45, '1' }, { 44, '2' }, { 42, '3' },
    { 51, VK_SUBTRACT },
    { 35, '0' },
    { 34, VK_PERIOD },
    { 32, VK_F5 },          // EXP
    { 41, VK_ADD },
    { 72, VK_BACKQUOTE },   // ^
    { 46, VK_COMMA },
    { 36, VK_HYPHEN },      // (-)
    { 31, VK_RETURN },      // EXE
};

static UINT8 g_scanToVKey[KEYSC_SCANCODES];

static ScanCodeToVKeyData g_scvk = {
    0,                      // no prefix
    0,                      // first scan code
    KEYSC_SCANCODES - 1,    // last scan code
    g_scanToVKey
};

static ScanCodeToVKeyData *g_rgpscvk[] = { &g_scvk };

// 1:1 remapping (the Layout Manager asks for the buffer size with a NULL buffer)
static UINT WINAPI ClassPadRemapVKey(const KEYBD_EVENT *pKbdEvents, UINT cKbdEvents,
                                     KEYBD_EVENT *pRmpKbdEvents, UINT cMaxRmpKbdEvents)
{
    UINT i;
    if (pRmpKbdEvents == NULL) return cKbdEvents;
    if (cMaxRmpKbdEvents < cKbdEvents) return 0;
    for (i = 0; i < cKbdEvents; i++) pRmpKbdEvents[i] = pKbdEvents[i];
    return cKbdEvents;
}

static DEVICE_LAYOUT g_layout = {
    sizeof(DEVICE_LAYOUT),
    KEYBD_PDD_CLASSPAD,
    g_rgpscvk,
    sizeof(g_rgpscvk) / sizeof(g_rgpscvk[0]),
    ClassPadRemapVKey
};

static DWORD MatrixFromRegistry(void)
{
    HKEY hKey; DWORD v = 0, type, size = sizeof v;
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, TEXT("HARDWARE\\DEVICEMAP\\KEYBD"), 0, 0, &hKey) == ERROR_SUCCESS) {
        if (RegQueryValueEx(hKey, TEXT("Matrix"), NULL, &type, (LPBYTE)&v, &size) != ERROR_SUCCESS || type != REG_DWORD) v = 0;
        RegCloseKey(hKey);
    }
    return v;
}

static void BuildTable(const KEYSC_KEY *keys, UINT count)
{
    UINT i;
    memset(g_scanToVKey, 0, sizeof g_scanToVKey);
    for (i = 0; i < count; i++) {
        UINT32 sc = KEYSC_CODE_TO_SCAN(keys[i].code);
        if (sc < KEYSC_SCANCODES) g_scanToVKey[sc] = keys[i].vk;
    }
}

extern "C" BOOL ClassPad(PDEVICE_LAYOUT pDeviceLayout)
{
    DWORD matrix;
    if (pDeviceLayout == NULL || pDeviceLayout->dwSize != sizeof(DEVICE_LAYOUT)) {
        RETAILMSG(1, (TEXT("CLASSPAD KEYBD: device layout size mismatch\r\n")));
        return FALSE;
    }
    matrix = MatrixFromRegistry();
    if (matrix == 1) BuildTable(g_keysCp400, sizeof(g_keysCp400) / sizeof(g_keysCp400[0]));
    else             BuildTable(g_keysCg100, sizeof(g_keysCg100) / sizeof(g_keysCg100[0]));
    RETAILMSG(1, (TEXT("CLASSPAD KEYBD: device layout %s\r\n"), matrix == 1 ? TEXT("fx-CP400") : TEXT("fx-CG100")));
    *pDeviceLayout = g_layout;
    return TRUE;
}

extern "C" BOOL ClassPad_00000409(PDEVICE_LAYOUT pDeviceLayout)
{
    return ClassPad(pDeviceLayout);
}

#ifdef DEBUG
static PFN_DEVICE_LAYOUT_ENTRY v_pfnDeviceLayout = ClassPad;   // type check against devicelayout.h
#endif
