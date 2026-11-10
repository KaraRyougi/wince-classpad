//------------------------------------------------------------------------------
//  classpad_pwrstat.h - what the last suspend did (docs/08, 2026-11-10)
//
//  A night in suspend emptied four cells: about 100 mA, the current of a
//  running machine, not of R-standby. There is no debug output on the
//  calculator, so OEMPowerOff (OAL power.c) counts what happens and the
//  Battery program shows it: how long the suspend lasted by the RTC, how
//  often the core was powered off, and how many of the wake-ups were for no
//  accepted reason (the OAL then goes straight back to standby, like the
//  stock OS's loop at 0x80020ED2; a machine that does this all night is
//  awake all night), with the wake-source registers at the first and the
//  last of those.
//      KernelIoControl(IOCTL_HAL_CLASSPAD_SUSPEND_STATS, NULL, 0, &stats, sizeof stats, &n)
//------------------------------------------------------------------------------
#ifndef __CLASSPAD_PWRSTAT_H
#define __CLASSPAD_PWRSTAT_H

#define IOCTL_HAL_CLASSPAD_SUSPEND_STATS \
    CTL_CODE(FILE_DEVICE_HAL, 2100, METHOD_BUFFERED, FILE_ANY_ACCESS)

// The display driver tells the OAL which panel it found, so that
// OEMPowerOff can power it down and bring it back the way Casio's OS does
// (OAL power.c, classpad_pwrseq.h). in: one DWORD, CLASSPAD_PANEL_*.
#define IOCTL_HAL_CLASSPAD_PANEL \
    CTL_CODE(FILE_DEVICE_HAL, 2101, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define CLASSPAD_PANEL_NONE     0       // not an fx-CP400 panel: the OAL leaves pins and panel alone
#define CLASSPAD_PANEL_NEW      1       // ID 0x16 / 0x52
#define CLASSPAD_PANEL_OLD      2       // any other ID

// Which of the power-off steps copied from Casio's OS a suspend performs.
// All off by default: with them the calculator did not come back from the
// standby (2026-11-10) and which step is to blame is not known, so each can
// be switched on by itself from the Battery program and tried.
//   in: one DWORD, the new mask (optional); out: one DWORD, the mask in force (optional)
// The same mask is the registry value HKLM\Software\ClassPad\Suspend
// "Options", where the touch driver reads its bit.
#define IOCTL_HAL_CLASSPAD_SUSPEND_OPTIONS \
    CTL_CODE(FILE_DEVICE_HAL, 2102, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define CLASSPAD_SUSPEND_OPT_PINS       0x01    // the OS's pin table (minus the memory standby's two entries), PFDR bit 7, PUDR bit 2, RCR1
#define CLASSPAD_SUSPEND_OPT_PANEL      0x02    // panel deep standby, its supply and dot clock off; cold power-on after the wake-up
#define CLASSPAD_SUSPEND_OPT_ADC        0x04    // the A/D converter's power-off set-up
#define CLASSPAD_SUSPEND_OPT_TOUCH      0x08    // touch sensor powered down (the touch driver's part)
#define CLASSPAD_SUSPEND_OPT_PROBE      0x10    // at an unexplained wake-up also read INTREQ00, INTMSK00, CMT and RWDT
#define CLASSPAD_SUSPEND_REG_KEY        TEXT("Software\\ClassPad\\Suspend")
#define CLASSPAD_SUSPEND_REG_VALUE      TEXT("Options")

#define CLASSPAD_SUSPEND_PATH_NONE      0       // no suspend yet
#define CLASSPAD_SUSPEND_PATH_RSTANDBY  1       // the core was powered off
#define CLASSPAD_SUSPEND_PATH_CPUSLEEP  2       // the fallback: CPU sleep, clocks and SDRAM running

typedef struct {
    BYTE  rcr1, rcr2;           // RTC: AF/CF and their enables; PEF, PES, START
    BYTE  intreq00, intmsk00;   // IRQ pins: requests and masks
    WORD  kiuint, kiudata0;     // KEYSC: event flags and enables; the first data word
    WORD  kiustate, cmstr;      // KEYSC scan state; CMT start register
    WORD  cmcsr, rwtcsr;        // CMT control/status; RCLK watchdog control/status
    DWORD r64cnt;               // RTC 64 Hz counter when the snapshot was taken
} CLASSPAD_WAKE_SNAPSHOT;

typedef struct {
    DWORD size;                 // sizeof this structure
    DWORD suspends;             // OEMPowerOff calls since boot
    DWORD path;                 // CLASSPAD_SUSPEND_PATH_* of the last one
    DWORD seconds;              // how long it lasted by the RTC calendar; 0xFFFFFFFF = not known
    DWORD entries;              // R-standby entries
    DWORD unexplained;          // wake-ups without an accepted reason
    DWORD wokeByAlarm;          // 1 = the RTC alarm ended it, 0 = Clear/ON
    DWORD hardwareOff;          // CLASSPAD_SUSPEND_OPT_* of the steps the OAL performed
    CLASSPAD_WAKE_SNAPSHOT first, last;     // at the first and the last unexplained wake-up
} CLASSPAD_SUSPEND_STATS;

#endif
