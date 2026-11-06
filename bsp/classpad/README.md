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

## Pocket PC compatibility: DocList.dll and MFC CE 3.0

`SRC\APPS\DOCLIST` builds `DocList.dll`, a stand-in for the Pocket PC
shell's document list control. MFC CE 3.0 for Pocket PC (`MFCCE300.DLL`,
SH3, from the Pocket PC 2000 SDK's `MFC\LIB\SH3`) imports
`DocList.dll!DocList_Create` by name for `CCeDocList`. CE 6.0 has no
DocList, and the "AYGShell API Set" catalog item only adds `aygshell.dll`,
so without this DLL the loader refuses `mfcce300.dll` (and `olece300.dll`
and every program linked to them). Checked against the 2026-11-05 shell
image (`notes/ce/NK-latest.bin`): the 8 AYGSHELL ordinals MFC imports (4,
21, 34, 40, 56, 69, 83, 84), its 9 commctrl ordinals and every coredll
ordinal of both DLLs are exported there; DocList.dll was the only gap.

The control is a child window over the parent's client area: a filter
combo box (when there is more than one filter) and a report list view
(Name, Folder, Date, Size) of the files in the folder (default
`\My Documents`) and its first-level subfolders. A tap or Enter sends
`DLN_ITEMACTIVATED` with the full path (what `CCeDocListDocTemplate` opens);
the `DLM_*` messages of the SDK's `doclist.h` are handled, except the shell
services (infrared, e-mail, rename/move dialog), which answer "not done".
Delete asks for confirmation. Exports: `DocList_Create`,
`DocList_GetFirstSelectedWaveFile`, `DocList_GetNextSelectedWaveFile` (the
Pocket PC ordinals 50 to 52); not the `CDocList` C++ members. It goes into
the image with commctrl (`CE_MODULES_COMMCTRL`).

The MFC DLLs themselves are staged separately in `FILES\legacy-mfc`
(`BSP_LEGACY_MFC=1`, see its README); the 300 pair are SH3 binaries and
need `bsp/kernel/loader-sh3-images.patch`. Untested so far:
written and syntax-checked on the host only (2026-11-06), not yet built in
Platform Builder or run.

## What the files are

- `SRC\OAL\OALLIB\startup.src`: `StartUp` (image entry: SR, VBR, jump to
  `KernelStart`), `OEMNMI`.
- `init.c`: `OEMInit` (runs with exceptions blocked and in register bank 1:
  no hypercalls allowed here).
- `timer.c`: TMU0 system tick with a variable period (the kernel's
  `pfnUpdateReschedTime` hook: the timer fires at the next reschedule time,
  at most `TICK_MAX_MSEC` = 1000 ms apart, `bsp_cfg.h`), `OEMIdle`,
  `OALStall`. Every interrupt is an exception round trip through the
  translator, so the fixed 1 ms tick cost about a third of the modelled boot
  (docs/04, the 10 ms experiment). `TICK_MAX_MSEC 1` restores the fixed
  tick for comparison runs.
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

## Measuring on the device: `classpad_dbtstat.exe`

`SRC\APPS\DBTSTAT` is the profiler for the real machine, where nothing
else gets out. It reads the translator's counters through the `HC_GETSTAT`
hypercall every 2 s (`SRC\INC\classpad_hc.h` lists the indices; a `trapa`
works from user mode, the translator takes it before the kernel) and shows,
in a topmost strip at the top of the screen:

- page 0: translations, flushes, invalidations, ASID switches, inline-cache
  misses, lookups, exceptions by vector and INTEVT code, all per second;
  the tick samples' split between translated code, exit stubs, stub
  windows, translations, helpers, idle (`sleep`) and user-mode code; with
  a `PROFILE=1` translator the real-time share of each C bucket
  (translate, asid, tlb, invalidate, lcd, flush, exception delivery, rte,
  lookup: they overlap, never sum them); CCR/MMUCR/FRQCR; and the TMU0
  tick measured against the RTC (`tick/rtc` 1.000 = the clock constant in
  `bsp_cfg.h` is right for this machine);
- page 1: the twelve hottest 16 KiB buckets of guest code since the window
  started, as P1 addresses and offsets into the ROM image;
- page 2: the same for the last interval.

A tap turns the page, a tap on the `x` in the top-right corner closes it.
Start it from the shell (`\Windows\classpad_dbtstat.exe`), or for a boot
profile add `"Launch70"="classpad_dbtstat.exe"` with `"Depend70"=hex:1e,00`
to `HKLM\init` in platform.reg. Resolve the hot buckets on the Mac with
`tools/cesym.py NK.bin RELDIR <address>`. The translator for such a run is
`make -C dbt DEVICE=1 PROFILE=1 BUILD=build-dev-prof hhk`; a non-profiling
translator still gives everything but the C-time line.

What to read first: `irq/s` far above the tick rate or an INTEVT code the
OAL does not use means a storming source; `tlb/s` in the thousands means
the UTLB working set; a block share near 100 % with a flat histogram means
the translated code itself; `xlat/s` in the hundreds with `flush/s` > 0
means the code cache recycling.

## Rules for code in this BSP

- C89, no `float`/`double`: the SH4 compiler would emit FPU instructions and
  the kernel keeps the FPU disabled in the OAL.
- Every 16/32-bit device register access through `MMIO_R*/MMIO_W*`.
- A `trapa` (hypercall) only when `ClasspadCanTrap()` (SR.BL clear).
- The touch and display drivers were rebuilt in the ClasspadUI SH4 Release
  environment during the validation below.


## Touch correction and validation (2026-11-05)

The production IIC transaction is in `SRC/INC/classpad_touch_bus.h` and is
shared by the `dbt/guest/touch.c` test. It follows WAIT-stage transitions,
sets RACK/NACK before the last byte, reads all 16 bytes of command 0x84,
and bounds every wait. Errors reset the bus and the next sample retries.
The driver explicitly enables the IIC module clock and configures PJ4/5,
PR5 and the Casio IIC clock divisors. Peripheral IIC interrupts stay masked
at INTC while ICIC enables the status/WAIT handshake.

The third packet word is z1, not a 0x8000 contact flag. PRDR bit 5 determines
contact. A separate thread polls for the first down as well as movement/up;
only the MDD reads IIC. This avoids relying on the emulator-derived 0x700
contact interrupt routing on hardware. A transient bus failure does not emit
a false pen-up. The stop event wakes and joins the sampler before teardown.

References: ClasspadDev/gint `src/touch/driver.c`, `i2c.c`, `i2c_inth.c`,
`touch.c`, and `include/gint/drivers/touch.h` on its dev branch. The emulator
fills z1 with 0x8000, which hid the former contact-bit mistake. Real-device
coordinates, bus clocks and calibration still require a device check.

Validation commands (fxSDK compiler must be on PATH):

```sh
make test
HLE_CB_TOUCH=160,264 HLE_CB_TOUCH_AT=6000 HLE_CB_TOUCH_HOLD=10000000 \
  make -C dbt GUEST=touch BUILD=build-touch-fix RUN_SECS=10 run
make -C dbt GUEST=aspace PROFILE=1 BUILD=build-aspace-review run
```

The host bus test covers full packets, last-byte NACK, arbitration loss,
NACK errors, bounded timeout and subsequent recovery. The translated guest
reads 32 consecutive production transactions and checks stable coordinates.
The aspace, tlbdata, devmem, tmu and flush guests pass after the runtime changes.

For the current ClasspadUI Release environment, the staged build script lives
at `C:\WINCE600\codex-touch-review\build.bat`. Previous changed files are
under its `backup` directory. This update preserves the remote eActivity ROM
layout in config.bib; pair its NK.bin with a `NK_WINDOW=eact` translator.
The earlier touch-only build disabled automatic panel sleep while wake-up
was not implemented. The power update below supersedes that workaround;
it does not establish the cause of the originally reported black screen.

The rebuilt image (SHA256
`608a262214e706b5cdfd37dc79dad05f08b262765ca25c9aa07e9766de914c07`)
booted in the emulator. Two injected taps produced down/up events (20
samples total) through the new driver with no reported IIC errors and opened
My Device. Its directory icons finished loading; a black screen was not
reproduced in this run. The screenshot and paired eActivity translator are
in the ignored `dbt/build-touch-validation/` directory. Hardware validation
is still pending; emulator sensor behavior cannot establish hardware success.

## Suspend/resume validation

The power update saves interrupt, timer and keypad state, suspends the touch
bus and display, and enters CPU sleep with retained RAM. Only Clear/ON
(matrix code 20) wakes the system; its wake press is consumed. The normal
registry enables idle/suspend timeouts again. Actual device wake reliability
and power consumption still need hardware validation.

The successful Windows build was copied from codex-power-review and checked:
normal NK.bin SHA256 bec974fca710d06f1832f1d665fa9696ac33d6342facec3ef2521590413b73f5;
test NK-power-test.bin SHA256 a1b740dc70da9b00952c2066fcea6641c6ead0c80f8113d6637d013e6a40e34a.
The test image automatically requests two suspend/resume cycles. In the
emulator, both cycles remained asleep through EXE and touch, resumed only
on Clear/ON, and returned success (result=0). Both suspended screenshots
were entirely black; after the second wake the desktop displayed the test
completion dialog. The ignored dbt/build-power-confirm directory contains
the images, run log, screenshots and binary/environment provenance.
This validates the translated emulator path, not physical wake or battery drain.


### Runtime portrait/landscape rotation

The GPE driver implements CE's GET/SETSCREENROTATION escapes, reached via
`ChangeDisplaySettingsEx` with `DM_DISPLAYORIENTATION`. Supported values
are DMDO_0, DMDO_90, DMDO_180 and DMDO_270 (CE encodes 270 as 4).
Logical geometry alternates between 320x528 and 528x320 on the fx-CP400.
The native framebuffer allocation/stride and LCD push geometry stay
320x528; GPE renders into the rotated primary surface directly. Blit dirty
rectangles are transformed back to native rows; GPE stroke clips already
arrive in native coordinates. Resume marks the full native height dirty.
Touch panel detection accepts either dimension being 528, so starting in
landscape does not disable the sensor. Raw sensor coordinates remain in
the calibration coordinate system; rotation is performed by GWES.

Open `\Windows\classpad_rotate.exe` for orientation buttons. Closing the
program leaves the selected orientation active. The change is runtime
only; this version boots in portrait.

`tools/windows/build-rotation.bat` builds the display/touch drivers and the
control program, then generates `NK-rotation.bin` and an emulator-only
`NK-rotation-test.bin`. The latter autoruns the program's test mode and
turns off automatic sleep for the test. The batch restores the normal
registry and release NK afterward. `app` rebuilds only the control program
and images; `image` only repackages them.

`tools/test_rotation_emulator.py` injects four physical IIC contacts, each
at the panel location corresponding to logical (40,40) in the next
orientation. It checks mode dimensions, application-reported touch
coordinates, GDI pixel readback, and the blue target's physical framebuffer
location. It saves all four frames and the final return-to-portrait frame.
This test does not establish physical-device LCD or suspend/resume behavior.

The fixed Rotate icon below the LCD now toggles portrait (0) / landscape (90)
on release. Touch DDSI consumes contacts starting on the icon strip, cancels
Rotate when dragged outside its region, and signals `ClassPadRotateKey` once.
`classpad_rotatekey.exe` starts at Launch94 and waits without polling, querying
live orientation and calling ChangeDisplaySettingsEx from outside GWES. The
strip remains in physical sensor coordinates in either orientation. Manual
four-direction selection remains available in `classpad_rotate.exe`.
`python3 tools/test_rotate_key.py` tests the actual GetPoint routing with stubbed
hardware, including hold, drag cancellation, adjacent icon, bus failure and
normal desktop contacts. `test_rotation_emulator.py --soft-key` tests four
physical strip taps against the background service in the diagnostic image.

2026-11-06 emulator verification: all four manual orientations passed live
orientation query, USER/GDI dimensions, framebuffer target and calibrated touch
checks. With the Rotate service image, four IIC strip taps at physical (251,550)
produced exactly 90,0,90,0 with successful returns and matching 528x320/320x528
geometry (`ROTATION HARNESS: PASS`). Normal NK SHA256:
`04eb54538c61b387ae178c1384c8360c38435232b00b6499cc41b49cf80f8af9`.
Hardware icon-strip hit regions still require confirmation on the device.

Timer recovery (2026-11-06): hardware underflow could occur after SetPeriod's
last UNF check and before its TCNT write, while software already recorded the
new period. That charged an old short-period IRQ as up to 1000 ms. A host
register-model regression fails on the previous code and passes with TMU0
stopped during the coherent status/period/counter update. Pending UNF retains
old accounting; stale ISR calls with no UNF do not advance CurMSec. Other TMU
channel start bits are preserved. The short stopped interval is uncounted;
hardware drift still needs measurement. This is a demonstrated code defect,
not yet a confirmed explanation of the reported hardware clock/touch failure.

`tools/windows/build-timer-recovery.bat` builds OAL and makes a normal image,
a rotation regression image, and `NK-timer-stats.bin`, which starts DBTSTAT
without requiring touch or Explorer and disables idle timeouts for diagnosis.
The script restores the normal registry/image after packaging. For emulator
clock measurements use `dbt/emu-patch/enable_rtc_reference.py`, rebuild the
emulator, and set HLE_RTC_RUNNING=1: direct HLE boot otherwise leaves the RTC
stopped, making DBTSTAT's 0.000 ratio invalid. Four-direction touch/geometry
regression passes with the rebuilt timer; at 120 s app uptime the independent
emulator RTC comparison reads tick/rtc 0.995. Physical-device confirmation is
still required.
