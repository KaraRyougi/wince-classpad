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
into a RAM frame buffer, a thread pushes the dirty rows to the panel through
its 16-bit data port at 0xB4000000 (Port R bit 4 = command/data). Two panels,
`SRC\INC\classpad_lcd.h`: the fx-CP400's DCS command set (320x528; CASET
0x2A / PASET 0x2B with four parameter bytes, RAMWR 0x2C then one RGB565
pixel per 16-bit write, SLPOUT 0x11 + COLMOD 0x3A at start, DISPON 0x29
after the first push, DISPOFF/SLPIN from the power handler; what casio-emu's
fx-cp400 profile models, `src/hardware/classpad/classpad.c`) and the
fx-CG100's R61524 registers (384x216, the `lcd` guest's protocol, kept for
the emulator's fx-cg100 profile). Registry `HKLM\Drivers\Display\ClassPad`:
`Width`/`Height` (default: the panel's size; `platform.reg` says 320x528)
and `Panel` (1 = DCS, 0 = R61524; absent: a height under 500 rows means
the R61524). `SRC\APPS\HELLO` is the first window (colour bars, frame,
counter, key and touch log), launched from HKLM\init after GWES; it sizes
itself from the screen. Both are built with the BSP and go into the image
only when GWES is in the OS design (`CE_MODULES_DISPLAY`).

The DCS panel is unverified so far (2026-11-05: written against the
emulator's model, the image not yet rebuilt). On a real fx-CP400 the panel
is powered up and initialised by Casio's boot code before the OS runs; the
driver's own sequence (SLPOUT, 120 ms, COLMOD 16 bpp, DISPON) is the DCS
minimum and may need the OS's full sequence (orientation, gamma) there.

To get them: in the Catalog Items View add, under Core OS > CEBASE > Shell
and User Interface > Graphics, Windowing and Events, "Minimal GDI
Configuration", "Minimal GWES Configuration", "Minimal Input Configuration"
and "Minimal Window Manager Configuration" (plus a font, Core OS > CEBASE >
Fonts > Tahoma, so DrawText has something to draw with). Then Build >
Advanced Build Commands > Sysgen (the catalog changed), Build Current BSP
and Subprojects, Copy Files to Release Directory, Make Run-Time Image. On
the Mac `make -C dbt NKBIN=/abs/NK.bin RUN_SECS=300 nkshot` runs the image
on the emulator's fx-cp400 profile (`EMU_MODEL`, dbt/Makefile) with the
last meaningful frame dumped to `dbt/build/nk.png`; the console shows
"CLASSPAD DDI: 320x528 16bpp, DCS fx-CP400 panel, ..." and
"CLASSPAD HELLO: ..." on the way.

Rebuilding for the ClassPad panel (the 2026-11-05 change: DCS display
variant, 320x528 in `platform.reg`, `Matrix`=1; the kernel is already
patched): copy `bsp/classpad` over `PLATFORM\CLASSPAD`, then Build >
Build Current BSP and Subprojects, Copy Files to Release Directory, Make
Run-Time Image (no Sysgen: the catalog did not change). The display
driver, the registry and the keyboard layout choice are all in the BSP
and its FILES, so this rebuilds everything that changed.

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

    HLE_CB_CHORD=72,74 HLE_CB_CHORD_AT=1200000 HLE_CB_CHORD_HOLD=30000 \
        make -C dbt NKBIN=/abs/NK.bin RUN_SECS=400 nkshot

holds the keys 1 and 7 together for 30000 ticks (about 0.9 s of guest
time) from tick 1.2 million, printing `[INJECTCHORD down] 72 74` and
`[INJECTCHORD up]` on the emulator side and the driver's `scan 22 down`,
`scan 38 down`, then `scan .. up` lines and hello's `key down vk=31 sc=22`
/ `char '1'` lines on the console; `dbt/build/nk.png` shows the event log.
The single-press forms (`HLE_CB_KEY`, `HLE_CB_KEYSWEEP`, `HLE_CB_KEYSEQ`)
hold a key for only 20 ticks, under 1 ms of guest time: the interrupt
fires but the key is up again before the IST or the 20 ms poll reads the
matrix, so they show nothing with this driver. The tick count is a guide:
the window is up well before tick 1.2 million in a Release image.

The emulator's ClassPad profile is the target and `dbt/Makefile`'s default
since 2026-11-05 (`EMU_MODEL=fx-cp400`, needs `dbt/emu-patch/add_classpad_addin.py`
applied to casio-emu; the 320x528 panel, the IIC0 sensor and INTC
priorities; fx-CG100 runs are no longer part of the workflow). It also
takes a headless stylus press:

    HLE_CB_CHORD=72,74 HLE_CB_CHORD_AT=1200000 HLE_CB_CHORD_HOLD=30000 \
    HLE_CB_TOUCH=160,264 HLE_CB_TOUCH_AT=1350000 HLE_CB_TOUCH_HOLD=30000 \
        make -C dbt NKBIN=/abs/NK.bin RUN_SECS=400 nkshot

(`[INJECTTOUCH down] 160 264 at tick ...` on the emulator side). The
2026-11-05 image boots there to the same hello window and key events, but
it is built for 384x216: the touch driver decides "display 384x216 -> no
touch panel" and never touches IIC0, and that image's display driver spoke
R61524 to the ClassPad's DCS panel, so the emulated screen stayed blank and
no `nk.png` was written. The sources now carry the ClassPad geometry, the
DCS display variant and `Matrix`=1 (above); the next image built from them
should show the window at 320x528 and exercise touch with this recipe.

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
