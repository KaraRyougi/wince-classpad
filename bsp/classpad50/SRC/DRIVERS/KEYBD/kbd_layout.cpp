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
//  virtual keys are a choice: digits as themselves, OK/EXE as Return, D-pad as
//  the arrows, DEL as Backspace, and the named keys on the function keys (see
//  the tables).
//
//  Operators type the ASCII characters Math Xpander's own math keyboard
//  (MathKeyPad.dll) sends: + - * / ( ) ^ = . , as a US keyboard's main block
//  types them, never the numeric keypad's VKs or [ ] `. The ones a US
//  keyboard types with Shift are pseudo VKs here (VKX_*, unassigned codes);
//  ClassPadRemapVKey turns each into Shift + the base key, unless SHIFT is
//  already held. x, y and z type the letters (VK 'X' 'Y' 'Z': lower case,
//  upper case with SHIFT).
//
//  The fx-CP400 Keyboard key types nothing: its press sets the named event
//  ClassPadKeyboardKey, and classpad_rotatekey.exe shows or hides Math
//  Xpander's math keyboard (MathKeyPad, a SIP input method) outside GWES.
//------------------------------------------------------------------------------
#include <windows.h>
#include <keybddr.h>
#include <laymgr.h>
#include <devicelayout.h>
#include "bsp_base_regs.h"
#include "classpad_keysc.h"

typedef struct { UINT8 code; UINT8 vk; } KEYSC_KEY;

// Characters a US keyboard types with Shift: pseudo VKs (0x88-0x8F are
// unassigned), expanded by ClassPadRemapVKey.
#define VKX_PLUS        0x88    // +  Shift + '='
#define VKX_STAR        0x89    // *  Shift + '8'
#define VKX_LPAREN      0x8A    // (  Shift + '9'
#define VKX_RPAREN      0x8B    // )  Shift + '0'
#define VKX_CARET       0x8C    // ^  Shift + '6'
#define VKX_KEYBOARD    0x8D    // fx-CP400 Keyboard: sets ClassPadKeyboardKey, types nothing

static UINT32 ShiftedBase(UINT32 vk)
{
    switch (vk) {
    case VKX_PLUS:   return VK_EQUAL;
    case VKX_STAR:   return '8';
    case VKX_LPAREN: return '9';
    case VKX_RPAREN: return '0';
    case VKX_CARET:  return '6';
    }
    return 0;
}

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
    { 46, VKX_CARET },      // x^y: ^
    { 36, VK_F9 },          // x^2
    { 26, VK_F10 },         // exp
    { 75, VK_COMMA },
    { 65, 'S' },            // sin
    { 55, 'C' },            // cos
    { 45, 'T' },            // tan
    { 35, VKX_LPAREN },     // (
    { 25, VKX_RPAREN },     // )
    { 74, '7' }, { 64, '8' }, { 54, '9' },
    { 44, VK_BACK },        // DEL
    { 10, VK_CANCEL },      // AC
    { 73, '4' }, { 63, '5' }, { 53, '6' },
    { 43, VKX_STAR },       // x
    { 33, VK_SLASH },       // division
    { 72, '1' }, { 62, '2' }, { 52, '3' },
    { 42, VKX_PLUS },       // +
    { 32, VK_HYPHEN },      // -
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
    { 85, VKX_KEYBOARD },   // Keyboard: Math Xpander's math keyboard
    { 73, VK_LEFT }, { 84, VK_UP }, { 74, VK_DOWN }, { 83, VK_RIGHT },
    { 76, 'X' }, { 75, 'Y' }, { 63, 'Z' },
    { 86, VK_EQUAL },       // =
    { 66, VKX_LPAREN },     // (
    { 56, VKX_RPAREN },     // )
    { 82, VK_BACK },        // Del
    { 65, '7' }, { 64, '8' }, { 62, '9' },
    { 71, VK_SLASH },       // division
    { 55, '4' }, { 54, '5' }, { 52, '6' },
    { 61, VKX_STAR },       // x
    { 45, '1' }, { 44, '2' }, { 42, '3' },
    { 51, VK_HYPHEN },      // -
    { 35, '0' },
    { 34, VK_PERIOD },      // .
    { 32, VK_F5 },          // EXP
    { 41, VKX_PLUS },       // +
    { 72, VKX_CARET },      // ^
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

static BOOL g_shiftHeld;    // the SHIFT key is down (its own events pass through)
static BOOL g_keyboardHeld; // the Keyboard key is down (auto-repeat sets no event)
static HANDLE g_keyboardEvent;

// A VKX_* event becomes Shift + its base key: Shift down before the base key
// goes down, Shift up after it comes up; an auto-repeat (down again) repeats
// both. While SHIFT itself is held no Shift events are added. Everything else
// passes through; VKX_KEYBOARD only sets its event. The Layout Manager first
// asks for the buffer size with a NULL buffer: two events per input event at
// most.
static UINT WINAPI ClassPadRemapVKey(const KEYBD_EVENT *pKbdEvents, UINT cKbdEvents,
                                     KEYBD_EVENT *pRmpKbdEvents, UINT cMaxRmpKbdEvents)
{
    UINT i, n = 0;
    if (pRmpKbdEvents == NULL) return 2 * cKbdEvents;
    for (i = 0; i < cKbdEvents; i++) {
        const KEYBD_EVENT *e = &pKbdEvents[i];
        UINT32 base = ShiftedBase(e->uiVk);
        BOOL down = (e->KeyStateFlags & KeyStateDownFlag) != 0;
        if (e->uiVk == VKX_KEYBOARD) {
            if (down && !g_keyboardHeld) {
                if (!g_keyboardEvent) g_keyboardEvent = CreateEvent(NULL, FALSE, FALSE, TEXT("ClassPadKeyboardKey"));
                if (g_keyboardEvent) SetEvent(g_keyboardEvent);
            }
            g_keyboardHeld = down;
            continue;
        }
        if (n + (base && !g_shiftHeld ? 2 : 1) > cMaxRmpKbdEvents) return 0;
        if (!base) {
            if (e->uiVk == VK_SHIFT || e->uiVk == VK_LSHIFT || e->uiVk == VK_RSHIFT) g_shiftHeld = down;
            pRmpKbdEvents[n++] = *e;
            continue;
        }
        if (down && !g_shiftHeld) {
            pRmpKbdEvents[n] = *e; pRmpKbdEvents[n++].uiVk = VK_SHIFT;
        }
        pRmpKbdEvents[n] = *e; pRmpKbdEvents[n++].uiVk = base;
        if (!down && !g_shiftHeld) {
            pRmpKbdEvents[n] = *e; pRmpKbdEvents[n++].uiVk = VK_SHIFT;
        }
    }
    return n;
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
