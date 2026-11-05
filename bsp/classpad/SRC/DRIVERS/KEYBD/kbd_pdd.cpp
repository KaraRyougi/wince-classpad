//------------------------------------------------------------------------------
//  kbd_pdd.cpp - KEYSC matrix keyboard PDD for the Layout Manager
//
//  The Layout Manager (layoutmanager.lib, linked into this DLL) calls the
//  entries in g_rgpfnPddEntries; ours brings the SH7305 KEYSC up, requests
//  a SYSINTR for its interrupt (logical IRQ_KEYSC, intr_cfg.h) and starts an
//  IST that keeps a 96-bit image of the matrix. Every changed bit becomes a
//  key event: scan code = 16 * KIUDATA word + bit (classpad_keysc.h), key
//  down when the bit is set. The IST wakes on the interrupt and on a short
//  timeout (registry "PollMs"), because the emulator drops an edge that
//  arrives while the source is masked between the ISR and InterruptDone;
//  auto-repeat is the MDD's job.
//
//  Registry (HKLM\HARDWARE\DEVICEMAP\KEYBD): "Priority256" (IST priority,
//  default 145), "PollMs" (default 20), "Matrix" (0 = fx-CG100 keypad,
//  1 = fx-CP400 keypad; read by kbd_layout.cpp).
//------------------------------------------------------------------------------
#include <windows.h>
#include <nkintr.h>
#include <keybddbg.h>
#include <keybdpdd.h>
#include <laymgr.h>
#include "bsp_base_regs.h"
#include "intr_cfg.h"
#include "classpad_mmio.h"
#include "classpad_keysc.h"

#define KEYBD_REG_KEY           TEXT("HARDWARE\\DEVICEMAP\\KEYBD")
#define KEYBD_IST_PRIORITY      145
#define KEYBD_POLL_MS           20

#ifndef DEBUG
// The Layout Manager and the IST library carry their own zones in DEBUG
// builds; retail builds reference nothing, this keeps RETAILMSG's companion.
DBGPARAM dpCurSettings = {
    TEXT("ClasspadKbd"),
    { TEXT("OEM0"), TEXT("OEM1"), TEXT("OEM2"), TEXT("OEM3"),
      TEXT("OEM4"), TEXT("OEM5"), TEXT("API"), TEXT("PDD"),
      TEXT("InputLang"), TEXT("DeviceLayout"), TEXT("MouseData"), TEXT("ScanCodes"),
      TEXT("Init"), TEXT("Function"), TEXT("Warning"), TEXT("Error") },
    0x0
};
#endif

static UINT             g_uiPddId;
static PFN_KEYBD_EVENT  g_pfnKeybdEvent;
static DWORD            g_dwSysIntr = SYSINTR_UNDEFINED;
static HANDLE           g_hevInterrupt;
static HANDLE           g_hThread;
static UINT16           g_image[KEYSC_WORDS];       // last matrix image
static DWORD            g_events;                   // key events delivered

static DWORD RegDword(LPCTSTR name, DWORD dflt)
{
    HKEY hKey; DWORD v = dflt, type, size = sizeof v;
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, KEYBD_REG_KEY, 0, 0, &hKey) == ERROR_SUCCESS) {
        if (RegQueryValueEx(hKey, name, NULL, &type, (LPBYTE)&v, &size) != ERROR_SUCCESS || type != REG_DWORD) v = dflt;
        RegCloseKey(hKey);
    }
    return v;
}

//------------------------------------------------------------------------------
//  Controller programming: the sequence of the fx-CG100 OS 2.00 (every row
//  and column enabled, interrupt mode, continuous scan).
//
static void KeyscInit(void)
{
    UINT16 v;
    MMIO_W16(KEYSC_KIUCNTREG, 0);
    Sleep(1);
    MMIO_W16(KEYSC_KIUINTREG, KEYSC_INT_CLEAR);
    MMIO_W16(KEYSC_KIUINTREG, KEYSC_INT_ENABLE);
    MMIO_W16(KEYSC_KIAUTOFIXREG, KEYSC_AUTOFIX_VALUE);
    MMIO_W16(KEYSC_KIUINTERVALREG, KEYSC_INTERVAL_VALUE);
    MMIO_W16(KEYSC_KIUWSETREG, KEYSC_WSET_VALUE);
    MMIO_W16(KEYSC_KOUTPINSET, KEYSC_KOUT_ALL);
    MMIO_W16(KEYSC_KINPINSET, KEYSC_KIN_ALL);
    MMIO_W16(KEYSC_KIUMODEREG, KEYSC_MODE_VALUE);
    MMIO_W16(KEYSC_KIUCNTREG, KEYSC_CNT_SCAN_START);
    v = MMIO_R16(KEYSC_KIUCNTREG);
    RETAILMSG(1, (TEXT("CLASSPAD KEYBD: KEYSC up, KIUCNTREG %04x KOUT %04x KIN %04x\r\n"),
                  v, MMIO_R16(KEYSC_KOUTPINSET), MMIO_R16(KEYSC_KINPINSET)));
}

static void KeyscStop(void)
{
    MMIO_W16(KEYSC_KIUCNTREG, 0);
}

// Acknowledge the interrupt: write the status low byte back with the enable
// bits (the OS reads the low byte and merges the high bits).
static void KeyscAck(void)
{
    UINT16 v = MMIO_R16(KEYSC_KIUINTREG);
    MMIO_W16(KEYSC_KIUINTREG, (UINT16)(KEYSC_INT_ENABLE | (v & 0x00FF)));
}

//------------------------------------------------------------------------------
//  Read the matrix and deliver one event per changed bit. Returns the number
//  of events.
//
static UINT KeyscScan(void)
{
    UINT16 now[KEYSC_WORDS];
    UINT w, b, n = 0;
    for (w = 0; w < KEYSC_WORDS; w++) now[w] = MMIO_R16(KEYSC_KIUDATA(w));
    for (w = 0; w < KEYSC_WORDS; w++) {
        UINT16 changed = (UINT16)(now[w] ^ g_image[w]);
        if (!changed) continue;
        for (b = 0; b < 16; b++) {
            if (changed & (1 << b)) {
                BOOL down = (now[w] & (1 << b)) != 0;
                UINT32 sc = KEYSC_SCANCODE(w, b);
                g_events++;
                if (g_events <= 40 || (g_events % 100) == 0)
                    RETAILMSG(1, (TEXT("CLASSPAD KEYBD: scan %d %s (word %d bit %d)\r\n"), sc, down ? TEXT("down") : TEXT("up"), w, b));
                if (g_pfnKeybdEvent) g_pfnKeybdEvent(g_uiPddId, sc, down);
                n++;
            }
        }
        g_image[w] = now[w];
    }
    return n;
}

static DWORD WINAPI KeyscIst(LPVOID)
{
    DWORD irq = IRQ_KEYSC, pollMs = RegDword(TEXT("PollMs"), KEYBD_POLL_MS), wakeups = 0;
    if (pollMs < 5) pollMs = 5;

    CeSetThreadPriority(GetCurrentThread(), (int)RegDword(TEXT("Priority256"), KEYBD_IST_PRIORITY));

    if (!KernelIoControl(IOCTL_HAL_REQUEST_SYSINTR, &irq, sizeof irq, &g_dwSysIntr, sizeof g_dwSysIntr, NULL)) {
        RETAILMSG(1, (TEXT("CLASSPAD KEYBD: no SYSINTR for IRQ %d, polling only\r\n"), irq));
        g_dwSysIntr = SYSINTR_UNDEFINED;
    } else if (!InterruptInitialize(g_dwSysIntr, g_hevInterrupt, NULL, 0)) {
        RETAILMSG(1, (TEXT("CLASSPAD KEYBD: InterruptInitialize(%d) failed, polling only\r\n"), g_dwSysIntr));
        KernelIoControl(IOCTL_HAL_RELEASE_SYSINTR, &g_dwSysIntr, sizeof g_dwSysIntr, NULL, 0, NULL);
        g_dwSysIntr = SYSINTR_UNDEFINED;
    }

    KeyscInit();
    KeyscScan();        // keys already held: report them, start from the live image
    RETAILMSG(1, (TEXT("CLASSPAD KEYBD: IST running, SYSINTR %d, poll %d ms\r\n"), g_dwSysIntr, pollMs));

    for (;;) {
        DWORD rc = WaitForSingleObject(g_hevInterrupt, pollMs);
        if (rc == WAIT_FAILED) {
            // Seen under the translator (2026-11-04): the kernel refuses the wait
            // (ERROR_OPERATION_ABORTED, the interrupt event still lists this
            // thread's previous proxy). Spinning here at IST priority starved
            // GWES and the display push thread; say so once and keep polling.
            static BOOL s_said;
            if (!s_said) {
                s_said = TRUE;
                RETAILMSG(1, (TEXT("CLASSPAD KEYBD: WaitForSingleObject failed, error %d: polling only\r\n"), GetLastError()));
            }
            Sleep(pollMs);
        }
        if (rc == WAIT_OBJECT_0) {
            wakeups++;
            if (wakeups <= 10 || (wakeups % 100) == 0)
                RETAILMSG(1, (TEXT("CLASSPAD KEYBD: interrupt %d, KIUINTREG %04x\r\n"), wakeups, MMIO_R16(KEYSC_KIUINTREG)));
            KeyscAck();
        }
        KeyscScan();
        if (rc == WAIT_OBJECT_0 && g_dwSysIntr != SYSINTR_UNDEFINED) InterruptDone(g_dwSysIntr);
    }
    return 0;
}

//------------------------------------------------------------------------------
//  PDD interface
//
static void WINAPI ClassPad_PowerHandler(UINT, BOOL fTurnOff)
{
    // Called with interrupts off: no RETAILMSG, no waits
    if (fTurnOff) KeyscStop();
    else {
        MMIO_W16(KEYSC_KOUTPINSET, KEYSC_KOUT_ALL);
        MMIO_W16(KEYSC_KINPINSET, KEYSC_KIN_ALL);
        MMIO_W16(KEYSC_KIUMODEREG, KEYSC_MODE_VALUE);
        MMIO_W16(KEYSC_KIUCNTREG, KEYSC_CNT_SCAN_START);
    }
}

static void WINAPI ClassPad_ToggleLights(UINT, KEY_STATE_FLAGS)
{
    // no keyboard lights
}

static KEYBD_PDD g_classpadPdd = {
    KEYBD_PDD_CLASSPAD,
    TEXT("ClassPad"),
    ClassPad_PowerHandler,
    ClassPad_ToggleLights
};

static BOOL WINAPI ClassPad_Entry(UINT uiPddId, PFN_KEYBD_EVENT pfnKeybdEvent, PKEYBD_PDD *ppKeybdPdd)
{
    RETAILMSG(1, (TEXT("CLASSPAD KEYBD: PDD %d entry\r\n"), uiPddId));
    g_uiPddId = uiPddId;
    g_pfnKeybdEvent = pfnKeybdEvent;
    *ppKeybdPdd = &g_classpadPdd;

    if (g_hThread) return TRUE;
    g_hevInterrupt = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!g_hevInterrupt) return FALSE;
    g_hThread = CreateThread(NULL, 0, KeyscIst, NULL, 0, NULL);
    if (!g_hThread) {
        RETAILMSG(1, (TEXT("CLASSPAD KEYBD: CreateThread failed %d\r\n"), GetLastError()));
        CloseHandle(g_hevInterrupt); g_hevInterrupt = NULL;
        return FALSE;
    }
    return TRUE;
}

#ifdef DEBUG
static PFN_KEYBD_PDD_ENTRY v_pfnKeybdEntry = ClassPad_Entry;   // type check against keybdpdd.h
#endif

// The Layout Manager's PDD list (C++ linkage, like its own reference)
PFN_KEYBD_PDD_ENTRY g_rgpfnPddEntries[] = {
    ClassPad_Entry,
    NULL
};
