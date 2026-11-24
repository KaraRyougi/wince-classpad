//------------------------------------------------------------------------------
//  kbd_pdd.cpp - KEYSC matrix keyboard PDD for the Layout Manager
//
//  The Layout Manager (layoutmanager.lib, linked into this DLL) calls the
//  entries in g_rgpfnPddEntries; ours brings the SH7305 KEYSC up and starts
//  one thread, the input tick, that reads the 96-bit matrix image every
//  "PollMs" and turns every changed bit into a key event: scan code = 16 *
//  KIUDATA word + bit (classpad_keysc.h), key down when the bit is set.
//  Auto-repeat is the MDD's job.
//
//  Polling only, as gint's keysc driver does it (a timer tick that reads the
//  six data words; no KEYSC interrupt). Until 2026-11-12 an IST scanned on
//  the KEYSC interrupt and a second thread rescanned every 20 ms because an
//  edge can be lost while the source is masked between the ISR and
//  InterruptDone; the scan compares whole images, so the poll alone finds
//  everything and the interrupt only added an ISR, a thread wake-up and an
//  InterruptDone per key edge. The SYSINTR of IRQ_KEYSC is still requested
//  and registered as a wake source: the OAL's suspend uses the KEYSC
//  interrupt for Clear/ON (power.c), with its own unmasking; it is never
//  InterruptInitialize'd, so it stays masked while CE runs.
//
//  The same tick serves the touch driver (one timer wake-up for both, as
//  gint samples the touch panel from its keyboard tick): touch.dll registers
//  a function through the exported ClasspadInputTickRegister and it is
//  called after every scan, in this thread. It must not block.
//
//  Registry (HKLM\HARDWARE\DEVICEMAP\KEYBD): "Priority256" (priority of
//  the tick thread, default 145), "PollMs" (the tick, default 8, at least 5), "Matrix" (0 = fx-CG100
//  keypad, 1 = fx-CP400 keypad; read by kbd_layout.cpp).
//
//  SHIFT then Clear/ON (or Clear/ON with SHIFT held) suspends, as on the
//  Casio OS: the Clear press is not delivered as a key, the scan only asks
//  for the suspend, and the thread that ran it calls SetSystemPowerState
//  (what Start > Suspend does) once it has left the scan lock. fx-CP400
//  matrix only: SHIFT is casio key 81, Clear/ON key 20 (kbd_layout.cpp).
//------------------------------------------------------------------------------
#include <windows.h>
#include <pm.h>
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
#define KEYBD_POLL_MS           8           // gint scans at 128 Hz; its author found the KEYSC data wrong (keys stuck down) when read below about 64 Hz

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
static HANDLE           g_hThread;                  // the input tick
static CRITICAL_SECTION g_csTick;                   // the registered tick function: called and changed under it
static void           (*g_pfnInputTick)(void);      // touch.dll's part of the tick
static UINT16           g_image[KEYSC_WORDS];       // last logical matrix image (byte-swapped)
static UINT16           g_rawImage[KEYSC_WORDS];    // last raw unswapped KIUDATA words for zero-swap idle scans
static volatile BOOL    g_suppressClear;
static DWORD            g_events;                   // key events delivered
static BOOL             g_cp400;                    // fx-CP400 matrix (registry "Matrix" = 1)
static BOOL             g_shiftArmed;               // SHIFT pressed, no other key since
static volatile BOOL    g_suspendRequest;           // SHIFT + Clear/ON seen by the scan

#define KEYSC_SHIFT_SCAN    KEYSC_CODE_TO_SCAN(81)  // fx-CP400 SHIFT: word 0 bit 15
#define KEYSC_CLEAR_SCAN    KEYSC_CODE_TO_SCAN(20)  // fx-CP400 Clear/ON: word 0 bit 1

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

//------------------------------------------------------------------------------
//  Read the matrix and deliver one event per changed bit. Returns the number
//  of events.
//
static UINT KeyscScan(void)
{
    UINT16 raw[KEYSC_WORDS], now[KEYSC_WORDS], diff = 0;
    UINT w, b, n = 0;
    for (w = 0; w < KEYSC_WORDS; w++) {
        raw[w] = *(volatile UINT16 *)CLASSPAD_UA(KEYSC_KIUDATA(w));
        diff |= (UINT16)(raw[w] ^ g_rawImage[w]);
    }
    if (!diff && !g_suppressClear) return 0;
    for (w = 0; w < KEYSC_WORDS; w++) {
        if (raw[w] != g_rawImage[w] || (w == 0 && g_suppressClear)) {
            now[w] = classpad_bswap16(raw[w]);
            g_rawImage[w] = raw[w];
        } else {
            now[w] = g_image[w];
        }
    }
    if (g_suppressClear) {
        if (!(now[0] & KEYSC_CLEAR_MASK)) g_suppressClear = FALSE;
        now[0] &= ~KEYSC_CLEAR_MASK;
    }
    for (w = 0; w < KEYSC_WORDS; w++) {
        UINT16 changed = (UINT16)(now[w] ^ g_image[w]);
        if (!changed) continue;
        for (b = 0; b < 16; b++) {
            if (changed & (1 << b)) {
                BOOL down = (now[w] & (1 << b)) != 0;
                UINT32 sc = KEYSC_SCANCODE(w, b);
                if (g_cp400 && down) {
                    if (sc == KEYSC_CLEAR_SCAN && g_shiftArmed) {
                        // SHIFT + Clear/ON: suspend. Clear stays masked
                        // until it is released (the image keeps it up), like
                        // the press that wakes the machine.
                        g_shiftArmed = FALSE;
                        g_suppressClear = TRUE;
                        g_suspendRequest = TRUE;
                        now[w] = (UINT16)(now[w] & ~(1 << b));
                        RETAILMSG(1, (TEXT("CLASSPAD KEYBD: SHIFT + Clear/ON: suspend\r\n")));
                        continue;
                    }
                    g_shiftArmed = (sc == KEYSC_SHIFT_SCAN);
                }
                g_events++;
                if (g_events <= 40 || (g_events % 100) == 0)
                    RETAILMSG(1, (TEXT("CLASSPAD KEYBD: scan %d %s (word %d bit %d)\r\n"), sc, down ? TEXT("down") : TEXT("up"), w, b));
                // The third argument is fKeyUp: keybdpdd.h declares it KEY_STATE_FLAGS,
                // but the Layout Manager's KeybdEventCallback (laymgr.cpp) takes BOOL
                // fKeyUp and any non-zero value is a release. Passing the down state
                // here turned every press into a release and every release into a
                // press that auto-repeated for ever (2026-11-05 injection runs).
                if (g_pfnKeybdEvent) g_pfnKeybdEvent(g_uiPddId, sc, down ? 0 : 1);
                n++;
            }
        }
        g_image[w] = now[w];
    }
    return n;
}

// SetSystemPowerState returns only after the resume, and the power handlers
// run in between.
static void SuspendIfRequested(void)
{
    if (!g_suspendRequest) return;
    g_suspendRequest = FALSE;
    SetSystemPowerState(NULL, POWER_STATE_SUSPEND, POWER_FORCE);
}

//------------------------------------------------------------------------------
//  The other user of the tick (touch.dll): fn is called after every scan, in
//  the tick thread; NULL takes it out again, and when the call returns the
//  old function is not running and will not be called any more.
//
extern "C" BOOL ClasspadInputTickRegister(void (*fn)(void))
{
    if (!g_hThread) return FALSE;               // no tick (the PDD was never entered)
    EnterCriticalSection(&g_csTick);
    g_pfnInputTick = fn;
    LeaveCriticalSection(&g_csTick);
    return TRUE;
}

static DWORD WINAPI KeyscTick(LPVOID)
{
    DWORD irq = IRQ_KEYSC, pollMs = RegDword(TEXT("PollMs"), KEYBD_POLL_MS);
    if (pollMs < 5) pollMs = 5;

    CeSetThreadPriority(GetCurrentThread(), (int)RegDword(TEXT("Priority256"), KEYBD_IST_PRIORITY));

    // The SYSINTR only as the suspend's wake source (see the header)
    if (!KernelIoControl(IOCTL_HAL_REQUEST_SYSINTR, &irq, sizeof irq, &g_dwSysIntr, sizeof g_dwSysIntr, NULL)) {
        RETAILMSG(1, (TEXT("CLASSPAD KEYBD: no SYSINTR for IRQ %d: Clear/ON will not wake a suspend\r\n"), irq));
        g_dwSysIntr = SYSINTR_UNDEFINED;
    } else if (!KernelIoControl(IOCTL_HAL_ENABLE_WAKE, &g_dwSysIntr, sizeof g_dwSysIntr, NULL, 0, NULL))
        RETAILMSG(1, (TEXT("CLASSPAD KEYBD: wake registration failed %d\r\n"), GetLastError()));
    g_cp400 = RegDword(TEXT("Matrix"), 0) == 1;
    KeyscInit();
    KeyscScan();        // keys already held: report them, start from the live image
    RETAILMSG(1, (TEXT("CLASSPAD KEYBD: input tick every %d ms, polling only (wake SYSINTR %d)\r\n"), pollMs, g_dwSysIntr));

    for (;;) {
        Sleep(pollMs);
        KeyscScan();
        if (g_pfnInputTick) {
            EnterCriticalSection(&g_csTick);
            if (g_pfnInputTick) g_pfnInputTick();
            LeaveCriticalSection(&g_csTick);
        }
        SuspendIfRequested();
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
        g_suppressClear = TRUE;
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
    InitializeCriticalSection(&g_csTick);
    g_hThread = CreateThread(NULL, 0, KeyscTick, NULL, 0, NULL);
    if (!g_hThread) {
        RETAILMSG(1, (TEXT("CLASSPAD KEYBD: CreateThread failed %d\r\n"), GetLastError()));
        DeleteCriticalSection(&g_csTick);
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
