# PLATFORM\CLASSPAD for Windows CE 5.0 - Casio SH7305 BSP

The Windows CE 5.0 port of the ClassPad BSP (`bsp/classpad` is the CE 6.0
R3 one; `docs/08-ce50-port.md` lists what the move changed and why). Same
hardware, same translator (`dbt/`), same drivers and applications: the
SH7305 OAL rewritten against the CE 5.0 kernel interface, the build files
against Platform Builder 5.0.

Status 2026-11-09: builds in the PB 5.0 VM and **boots to the CE 5.0
desktop in casio-emu's ClassPad profile** (POSTINIT 2.3 s, display driver
14 s, desktop 29 s, Explorer taskbar 37 s of wall clock with the
translator's CE 5.0 KDEV/TLB_REFILL tables and page-table revalidation of
user blocks; `notes/ce/desktop-cp400-ce50.png`, docs/08 sections 7-8;
it was 111 s before those); a stylus double tap
opens My Device and the Main strip icon starts Math Xpander from the EXT
region (`notes/ce/xpander-cp400-ce50.png`), without the SH3 loader patch:
XIP modules are loaded without a CPU-type check. The Start menu, Plotter-Z,
the keyboard, SH3CRT's arithmetic and the real device are still to be
exercised.

## What CE 5.0 changes in this BSP (details in docs/08)

- **nk.exe = kernel + OAL.** `SRC\OAL\KERN` links `nk.lib` (the OS design's
  sysgen'd kernel) with `oal.lib` into `kern.exe`, which `common.bib` puts
  into the image as `nk.exe` (`IMGNOKITL`, `IMGNODEBUGGER` in
  `classpad.bat`). No `OEMGLOBAL` table: the kernel calls `OEMInit`,
  `OEMIdle(DWORD)`, `InitClock`, `SC_GetTickCount`, `OEMIoControl`,
  `OEMCacheRangeFlush`, `OEMInterruptEnable/Disable/Done`, the debug and RTC
  functions by name (`nkintr.h`); the ISR table imports the assembly symbol
  `OEMNMI` and the constant `SH4CacheLines`; the EXT region is handed over
  through `OEMRomChain`.
- **The translator treats nk.exe as the kernel** (`dbt/rt/nkbin.c`: no
  kernel.dll in the module table, so nk.exe's code sections are the
  device-aware range). The OAL is therefore inside the device-aware range:
  its accesses to P4 control registers (MMUCR in `cache.c`) use the plain
  `P4_*` macros of `classpad_mmio.h`, not the pre-swapped `MMIO_*` ones.
- **No PLATFORM\COMMON for SH4** in CE 5.0: `OALLIB\oalcommon.c` carries the
  SYSINTR map, `OEMIoControl` and its generic handlers, the log, wake sources,
  `g_oalTimer`, QueryPerformanceCounter and `SC_GetTickCount`. The PQOAL
  headers (`PLATFORM\COMMON\SRC\INC`) are still used for the types.
- **Timer**: `InitClock()` programs TMU0 (OEMInit, and the kernel calls it
  again after `OEMPowerOff`: `kwin32.c CallOEMPowerOff`); the variable tick
  goes through `pOEMUpdateRescheduleTime` and `SC_GetTickCount`.
- **Kernel patches**: only the SH3 loader patch
  (`bsp/kernel/ce50-loader-sh3-images.patch`). The WaitOneMore
  interrupt-event hole of CE 6.0 does not exist in CE 5.0's `schedule.c`
  (its `exitfunc` unlinks the proxy); the drivers keep avoiding timed waits
  on interrupt events anyway.
- **Drivers**: the same `gpe.lib` and `tchmdd.lib` (sysgen makes them in
  `cesysgen\oak\lib`; `PUBLIC\COMMON\OAK\LIB` only has the inputs `gpe_lib`
  and `tchmain`), `touch.def` from `cesysgen\oak\inc`; no kernel-mode
  `K` flag and no `k.touch.dll` twin in `platform.bib` (CE 5.0 runs
  drivers in kernel mode as a matter of course, `ROMFLAGS` bit 1 off).
  Sources unchanged: the Layout Manager, touch DDSI and GPE interfaces are
  the same in both versions (headers diffed, only the copyright differs).
- **config.bib / EXT**: CE 5.0's romimage cannot lay out two regions that
  are not contiguous (it put NK at EXT's address and EXT after it), so the
  EXT region is built by a second romimage run: `PostFmergeBib.bat` moves
  the EXT entries out of ce.bib, `PostRomImage.bat` builds `ext.bib` from
  `ext.bib.hdr` and runs romimage again, giving `EXT.bin`. The two runs use
  disjoint DLL address ranges (CE 5.0 has data and code ranges, see
  config.bib); no `KDLL*` ranges.
- **Catalog**: `CATALOG\classpad.cec` (PB 5.0 format) instead of the
  `.pbcxml`.

## Install into the Platform Builder 5.0 VM

1. Copy this directory to `C:\WINCE500\PLATFORM\CLASSPAD` **with CRLF line
   endings** (build.exe does not parse LF-only `dirs`/`sources` files; a git
   checkout does it through `.gitattributes`, a copy from the Mac needs the
   conversion: `tools/stage_bsp50.py` writes the tree with CRLF and leaves
   the `.dll`/`.ico`/`.2BP`/`.lnk` files alone). The Math Xpander binaries
   (`FILES\xpander`) and `FILES\legacy-mfc\mfcce300.dll` must be the
   **processed** copies: 4 KiB-aligned sections, rebuilt relocations for
   MathXpander.exe, imports pointed at SH3CRT.dll (`FILES\xpander\README.md`;
   `tools/windows/claude-build/stage.py --xpander` makes them, or copy them
   from the CE 6.0 tree's `PLATFORM\CLASSPAD\FILES`). romimage stops with
   "Could not find file ...xpander\AlgebraicExpression.dll" without them.
   stage_bsp50.py keeps binaries that are already there.
2. File > Manage Catalog Items > Import > `PLATFORM\CLASSPAD\CATALOG\classpad.cec`.
3. File > New Platform: BSP "Casio ClassPad: SH4", a design template with
   the Standard Shell (or Enterprise Terminal / custom: whatever has GWES,
   the Standard Shell, AYGShell API Set, commctrl, commdlg, Touch Screen
   (Stylus), Keyboard MDD). Remove networking, audio, PC Card, USB, IE.
   Release configuration: Platform > Settings > Build Options: enable
   "Enable eboot space in memory" OFF, "Enable KITL" OFF, "Enable kernel
   debugger" OFF, "Enable ship build" OFF (a ship build compiles the
   RETAILMSG output away). Locale 0409 only.
4. Apply `bsp/kernel/ce50-loader-sh3-images.patch` to
   `PRIVATE\WINCEOS\COREOS\NK\KERNEL\loader.c` and `build -c` there (the
   SH3 modules of Math Xpander and `mfcce300.dll` need it), then Build OS >
   Sysgen (makes `nk.lib` from the rebuilt `nkmain.lib`), Build OS > Build
   and Sysgen Current BSP (PB 5.0's name for building just the BSP after a
   source change; the first time, Build and Sysgen for the whole design),
   then Build OS > Make Run-Time Image. `NK.bin` and `EXT.bin` land in the
   flat release directory. The command-line equivalent, from Build OS > Open
   Release Directory: `cd %_TARGETPLATROOT%`, `build -c`, then `makeimg`.
5. On the Mac: `make -C dbt NKBIN=/abs/NK.bin EXTBIN=/abs/EXT.bin RUN_SECS=600 nkshot`
   (images built before the 2026-11-09 region swap need `NK_WINDOW=eact`)
   runs the image on casio-emu's ClassPad profile; the console should show
   `[nk] nk.exe (kernel + OAL, CE 5.0 style) ... (device-aware translation)`,
   the kernel sign-on "Windows CE Kernel for Hitachi SH Built on ...",
   "CLASSPAD OAL: SH7305 under the translator (CE 5.0)", "POSTINIT" and the
   display driver's lines.

## Layout

| Path | What |
|---|---|
| `classpad.bat`, `sources.cmn`, `dirs`, `CESYSGEN\makefile` | PB 5.0 build environment (`BSP_NO*` keep the stock drivers out, `IMGNOKITL`/`IMGNODEBUGGER` pick `kern.exe`) |
| `CATALOG\classpad.cec` | the BSP's catalog entry |
| `SRC\OAL\OALLIB` | the OAL: `startup.src` (StartUp, OEMNMI), `init.c` (OEMInit, SH4CacheLines, the EXT region), `oalcommon.c` (PQOAL stand-ins), `intr.c` (INTC, ISRs), `timer.c` (TMU0 tick, InitClock, OEMIdle), `cache.c` + `cacheops.src`, `debug.c` (translator console and ring), `ioctl.c` (IOCTL table), `rtc.c`, `power.c` (R-standby) + `sleep.src` |
| `SRC\OAL\KERN` | `kern.exe`: nk.lib + oal.lib + the KITL stubs |
| `SRC\INC` | `bsp.h`, register maps, `classpad_mmio.h` (pre-swapped MMIO, plain P4), `classpad_hc.h` (hypercalls), `ioctl_tab.h`, `image_cfg.h`, `classpad_extrom.h` |
| `SRC\DRIVERS` | display (GPE, DCS panel), keyboard (Layout Manager + KEYSC), touch (tchmain + IIC0 sensor): the CE 6.0 sources |
| `SRC\APPS` | rotate, rotatekey, SH3CRT.dll, DocList.dll, Plotter-Z (`dirs`); hello and officetest kept but not built |
| `FILES` | `config.bib` (NK in the eActivity area, EXT in the hole), `platform.bib/reg/dat/db`, `PostFmergeBib.bat` + `romexclude.txt` (drops the network stack and the .wav files at makeimg time; the hook exists in CE 5.0's makeimg too), `legacy-mfc` (mfcce300.dll), `xpander` (README, link; binaries staged from the cab) |

For the hardware facts, the drivers' behaviour, the touch and power
validation and the Pocket PC compatibility work, `bsp/classpad/README.md`
still applies: the sources are the same files.

## Rules for code in this BSP

- C89, no `float`/`double`: the SH4 compiler would emit FPU instructions and
  the kernel keeps the FPU disabled in the OAL.
- Every 16/32-bit device register access through `MMIO_R*/MMIO_W*`;
  P4 control registers (0xFFxxxxxx) in OAL code through `P4_R32/P4_W32`
  (nk.exe is translated device-aware, so those accesses are not swapped).
- A `trapa` (hypercall) only when `ClasspadCanTrap()` (SR.BL clear): the
  kernel runs OEMInit and OEMIdle with BL=1.
- No timed waits on interrupt events in drivers (kept from CE 6.0).
