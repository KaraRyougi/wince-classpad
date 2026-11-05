# PLATFORM\CLASSPAD - Casio SH7305 BSP for Windows Embedded CE 6.0 R3

Headless kernel-only BSP for the SH7305 calculators (fx-CP400, fx-CG50,
fx-CG100) running under the little-endian translator in `dbt/`. Everything
the kernel needs: image entry, 1 ms tick on TMU0, INTC glue, cache and TLB
maintenance with the translator notifications, a debug console through the
translator, a software RTC, the IOCTL table. Display, keyboard and touch drivers
are written (sections below); no serial or network yet.

## Install into the Platform Builder VM

1. Copy this directory to `C:\WINCE600\PLATFORM\CLASSPAD` (the directory name
   must match `classpad.bat` and `<PlatformDirectory>` in `CATALOG`).
2. In VS2005: File > New > Project > Platform Builder for CE 6.0 > OS Design.
   The catalog should list "Casio ClassPad: SH4" under BSPs (if not: Tools >
   Platform Builder for CE 6.0 > Catalog refresh, or restart VS). Template
   Custom Device, remove everything the wizard pre-selects.
3. Project > Properties > Configuration Properties > Build Options, Release
   configuration: untick everything. In particular "Enable eboot space in
   memory" (IMGEBOOT, no bootloader here), "Enable kernel debugger" (so
   IMGNODEBUGGER=1: no kd/hd/osaxs in the image) and "Enable KITL" (so
   IMGNOKITL=1: no transport). Leave "Enable ship build" off too, it would
   compile the RETAILMSG output away. The generated PBInitEnv.bat clears the
   IMG* variables after calling `classpad.bat`, so what this page says is
   what counts.
4. Build > Advanced Build Commands > Sysgen (once), then Build Solution.
   Afterwards only `build -c` in `PLATFORM\CLASSPAD` and `makeimg` in the
   release directory are needed when OAL sources change (notes/ce/build-commands.txt).
5. Copy `NK.bin` from the flat release directory to the Mac and run
   `make -C dbt NKBIN=/absolute/path/NK.bin RUN_SECS=600 run` (the path is
   resolved inside `dbt/`; 20 s is the default alarm).

A Release image is silent: the prebuilt retail kernel libraries carry no
message strings at all (not even the RETAILMSG ones: "Booting Windows CE
version", "This device has booted"), so after the OAL sign-on nothing
prints. The OAL marks the milestones itself: "CLASSPAD OAL: POSTINIT"
(IOCTL_HAL_POSTINIT, the kernel's first thread has finished system init)
and "CLASSPAD OAL: first idle" (OEMIdle's first call: every boot thread is
blocked). The Debug image prints the kernel's and filesys' messages as
well, at about the same boot time under the translator.

## Display: GWES image

`SRC\DRIVERS\DISPLAY` is a GPE display driver (`classpad_ddi.dll`): GDI draws
into a RAM frame buffer, a thread pushes the dirty rows to the R61524 through
its 16-bit data port (the `lcd` guest's protocol; geometry in
`SRC\INC\classpad_lcd.h`, 384x216 for the emulated fx-CG100 panel).
`SRC\APPS\HELLO` is the first window (colour bars, frame, counter), launched
from HKLM\init after GWES. Both are built with the BSP and go into the image
only when GWES is in the OS design (`CE_MODULES_DISPLAY`).

To get them: in the Catalog Items View add, under Core OS > CEBASE > Shell
and User Interface > Graphics, Windowing and Events, "Minimal GDI
Configuration", "Minimal GWES Configuration", "Minimal Input Configuration"
and "Minimal Window Manager Configuration" (plus a font, Core OS > CEBASE >
Fonts > Tahoma, so DrawText has something to draw with). Then Build >
Advanced Build Commands > Sysgen (the catalog changed), Build Current BSP
and Subprojects, Copy Files to Release Directory, Make Run-Time Image. On
the Mac `make -C dbt NKBIN=/abs/NK.bin RUN_SECS=300 nkshot` runs the image
with the emulator dumping the last meaningful frame to `dbt/build/nk.png`;
the console shows "CLASSPAD DDI: ..." and "CLASSPAD HELLO: ..." on the way.

## Keyboard and touch

`SRC\DRIVERS\KEYBD` builds `kbdmouse.dll` (file name `classpad_kbd.dll`):
the Layout Manager MDD from `layoutmanager.lib`, the US input language, a
PDD that scans the SH7305 KEYSC (`kbd_pdd.cpp`: the register sequence of
the fx-CG100 OS, an IST on the KEYSC interrupt plus a 20 ms poll thread
(same reason as for touch: no timed wait on the interrupt event), one key
event per changed bit of the six KIUDATA words, scan code = 8 * row +
column) and two device layouts (`kbd_layout.cpp`: the fx-CG100 keypad and
the fx-CP400 keypad, chosen by `HKLM\HARDWARE\DEVICEMAP\KEYBD` "Matrix",
0 or 1). The hardware facts are in `SRC\INC\classpad_keysc.h`. Nothing
beyond "Minimal Input Configuration" is needed in the catalog
(`CE_MODULES_KEYBD` is set with it).

`SRC\DRIVERS\TOUCH` builds `touch.dll` (`classpad_touch.dll`): the touch
MDD and calibration from `tchmdd.lib`/`tch_cal.lib` with a DDSI layer for
the fx-CP400 panel as casio-emu models it (`SRC\INC\classpad_touch.h`): a
sensor on IIC0, the contact line on Port R bit 5, INTEVT 0x700 when the
stylus lands, samples while pressed from a DDSI thread that sets the
IST's event every 1000 / rate ms (not from a timeout on the IST's wait: a
timed wait on an interrupt event hits a CE 6.0 R3 kernel hole, see
`notes/ce/kernel-sh-notes.md` section 10 and `bsp/kernel/`). The
registry holds a `CalibrationData` computed from the emulator's stylus
mapping, so no calibration screen is needed. The driver decides at start
whether a panel exists ("Panel" in `HKLM\HARDWARE\DEVICEMAP\TOUCH`, else
a 528-row display) and otherwise never touches the bus, which the fx-CG100
emulator profile does not implement. The catalog needs Core OS > CEBASE >
Shell and User Interface > User Interface > "Touch Screen (Stylus)"
(SYSGEN_TOUCH) and a Sysgen afterwards; without it `touch.dll` is left out
of the image (`CE_MODULES_POINTER`).

Both drivers print "CLASSPAD KEYBD: ..." / "CLASSPAD TOUCH: ..." when they
start and for the first events; `classpad_hello.exe` logs every key and
touch event on the screen and as "CLASSPAD HELLO: key down vk=.. sc=..".

Verification in casio-emu (fx-CG100 profile): the emulator injects keys
through `setKeydown` codes (row = code % 10, column = code / 10 - 1; the
digit 7 is 74, 4 is 73, 1 is 72, EXE 31, OK 48) at `updateTimers` ticks,
about 9000 ticks per second on an M-series Mac, a Release boot reaching
GWES after 150-200 s:

    HLE_CB_KEYSWEEP=72,74 HLE_CB_KEY_AT=1800000 HLE_CB_KEY_EVERY=90000 \
        make -C dbt NKBIN=/abs/NK.bin RUN_SECS=420 nkshot

presses the keys 1, 4 and 7 (the sweep walks the codes 72, 73, 74) one
after the other, 10 s apart, each printing `[INJECT] code=N` on the
emulator side and the driver's `scan` / hello's `key down` lines on the
console; `dbt/build/nk.png` shows the event log. The tick count is only a
guide: pick `HLE_CB_KEY_AT` from the `[INJECT]` time stamps of a first run. Touch cannot
be exercised this way: casio-emu's ClassPad profile only cold-boots the
real OS from a 64 MiB NOR image and refuses an add-in, so the touch driver
is written against the emulator's model but has not run yet.

## What the files are

- `SRC\OAL\OALLIB\startup.src`: `StartUp` (image entry: SR, VBR, jump to
  `KernelStart`), `OEMNMI`.
- `init.c`: `OEMInit` (runs with exceptions blocked and in register bank 1:
  no hypercalls allowed here).
- `timer.c`: TMU0 fixed 1 ms tick, `OEMIdle`, `OALStall`.
- `intr.c`: IRQ numbering (INTEVT - 0x200) / 0x20, IPR/IMR programming,
  hooks `OALTimerIntrHandler` on IRQ 16, the KEYSC ISR on vector 95
  (logical IRQ 48 for the SYSINTR map, which has 64 entries) and the touch
  contact ISR on IRQ 40; device IRQs are masked in the ISR and unmasked by
  InterruptDone.
- `cache.c` + `cacheops.src`: `OEMCacheRangeFlush` with `ocbwb`/`ocbp`/`icbi`
  loops, MMUCR.TI, and the translator hypercalls F2 (code changed) and F9
  (TLB changed).
- `debug.c`: `OEMWriteDebugByte` -> hypercall F3 when SR.BL is clear, else
  the RAM ring at 0xACC7F000 that the translator drains.
- `ioctl.c`, `rtc.c`, `power.c`, `args.c`: IOCTL table and platform strings,
  software clock, stubs.
- `SRC\INC\classpad_mmio.h`: pre-swapped 16/32-bit register access (the
  translator swaps every 16/32-bit memory access; see `docs/04`), used by
  the OAL and the drivers alike; `classpad_keysc.h`, `classpad_touch.h`:
  the KEYSC and touch sensor register maps.
- `FILES\config.bib`: NK at 0x8C101000 (autosized), RAM up to 0x8CC7F000, the
  last page of the window reserved for the debug ring.

## Rules for code in this BSP

- C89, no `float`/`double`: the SH4 compiler would emit FPU instructions and
  the kernel keeps the FPU disabled in the OAL.
- Every 16/32-bit device register access through `MMIO_R*/MMIO_W*`.
- A `trapa` (hypercall) only when `ClasspadCanTrap()` (SR.BL clear).
- Nothing here has been compiled yet: the first `build -c` in the VM will
  find the typos.
