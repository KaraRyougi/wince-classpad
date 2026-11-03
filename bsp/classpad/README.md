# PLATFORM\CLASSPAD - Casio SH7305 BSP for Windows Embedded CE 6.0 R3

Headless kernel-only BSP for the SH7305 calculators (fx-CP400, fx-CG50,
fx-CG100) running under the little-endian translator in `dbt/`. Everything
the kernel needs: image entry, 1 ms tick on TMU0, INTC glue, cache and TLB
maintenance with the translator notifications, a debug console through the
translator, a software RTC, the IOCTL table. No display, keyboard, serial or
network yet.

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
   `make -C dbt NKBIN=path/to/NK.bin run`.

## What the files are

- `SRC\OAL\OALLIB\startup.src`: `StartUp` (image entry: SR, VBR, jump to
  `KernelStart`), `OEMNMI`.
- `init.c`: `OEMInit` (runs with exceptions blocked and in register bank 1:
  no hypercalls allowed here).
- `timer.c`: TMU0 fixed 1 ms tick, `OEMIdle`, `OALStall`.
- `intr.c`: IRQ numbering (INTEVT - 0x200) / 0x20, IPR/IMR programming,
  hooks `OALTimerIntrHandler` on IRQ 16.
- `cache.c` + `cacheops.src`: `OEMCacheRangeFlush` with `ocbwb`/`ocbp`/`icbi`
  loops, MMUCR.TI, and the translator hypercalls F2 (code changed) and F9
  (TLB changed).
- `debug.c`: `OEMWriteDebugByte` -> hypercall F3 when SR.BL is clear, else
  the RAM ring at 0xACC7F000 that the translator drains.
- `ioctl.c`, `rtc.c`, `power.c`, `args.c`: IOCTL table and platform strings,
  software clock, stubs.
- `SRC\INC\classpad_mmio.h`: pre-swapped 16/32-bit register access (the
  translator swaps every 16/32-bit memory access; see `docs/04`).
- `FILES\config.bib`: NK at 0x8C101000 (autosized), RAM up to 0x8CC7F000, the
  last page of the window reserved for the debug ring.

## Rules for code in this BSP

- C89, no `float`/`double`: the SH4 compiler would emit FPU instructions and
  the kernel keeps the FPU disabled in the OAL.
- Every 16/32-bit device register access through `MMIO_R*/MMIO_W*`.
- A `trapa` (hypercall) only when `ClasspadCanTrap()` (SR.BL clear).
- Nothing here has been compiled yet: the first `build -c` in the VM will
  find the typos.
