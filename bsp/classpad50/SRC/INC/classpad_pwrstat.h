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
    DWORD hardwareOff;          // 1 = pins set for power-off and the panel powered down (Casio's sequence), 0 = not done
    CLASSPAD_WAKE_SNAPSHOT first, last;     // at the first and the last unexplained wake-up
} CLASSPAD_SUSPEND_STATS;

#endif
