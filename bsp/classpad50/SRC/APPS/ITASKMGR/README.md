# ITaskMgr

ITaskMgr 1.0 by Yuichiro Ishitani (IshiSoft), a task manager for Windows CE:
a CPU and memory graph, the process list (Terminate, heap use of the selected
process) and the window list (Switch, Close). BSD license: `LICENSE.txt`.

Source: <https://github.com/WinCEsoft/UTIL-PROCESS-ITaskMgr>, commit 90bdf61
("version 1.0"), directory `itaskmgr_src`. The newer fork
<https://github.com/datadiode/ITaskMgr> (threads, system information) targets
CE 6 and later compilers and was not used.

In the image as `\Windows\ITaskMgr.exe`: Programs > Task Manager, and the Swap
icon of the touch strip (`SRC\APPS\ROTATEKEY`). The close button ends it; Esc
hides it and leaves its icon in the taskbar's tray, a tap there shows it
again. Needs `toolhelp.dll` (`SYSGEN_TOOLHELP`, classpad.bat).

## Changes from upstream

Marked "ClassPad" in the sources.

- **CPU load from `GetIdleTime()`.** Upstream runs a thread of idle priority
  in an endless loop and takes what that thread did not get as the load. On
  this machine that thread would keep the processor from sleeping (OEMIdle,
  the tickless timer) and the translator busy for as long as the program
  runs. The idle thread, `CreateThreads` and `GetThreadTick` are gone.
- **Tray icon** updated only when it changes, not on every tick.
- **Size and place**: 320 x 300 at most, within the work area, centred at its
  top (upstream: 240 x 240); placed again on WM_SETTINGCHANGE when it no
  longer fits (rotation). The pages are laid out once at start explicitly.
- **Heap use** of the selected process: when the selection changes and every
  10 s, not every 2 s (a walk over every heap block, slow under the
  translator).
- **Window list**: WM_GETICON is sent with a 300 ms limit
  (`SendMessageTimeout`), so a program that has stopped answering does not
  stop the task manager too; the default icon is loaded at the small icon
  size (upstream passed the `SM_` indices as the size).
- WM_HELP no longer falls through into WM_TIMER; the write to a control that
  does not exist (`IDC_CPUPOWER`) is gone.
- **Build files**: `sources`/`makefile` for Platform Builder 5.0 instead of
  the eVC project; `StdAfx.h` names `tchar.h` and `shellapi.h`; `cpu.h`,
  `process.h`, `tasklist.h` without their Shift-JIS comments; `ITaskMgr.rc`
  written out in ASCII (Tahoma 8, no `newres.h`), same dialogs and IDs.

Not built yet, not run.
