# wince-classpad

## Intro

In the early 2000s, HP, Texas Instruments, and Casio all converged on a remarkably similar vision for the next generation of graphing calculators: a large, portrait-oriented grayscale resistive touchscreen, a stylus, and a minimalist keypad for basic arithmetic and navigation.

HP announced the Windows CE-based HP Xpander, powered by a Hitachi SH-3 processor, in 2000, only to cancel it just months before mass production. The company eventually released its [Math Xpander](https://www.saltire.com/xpander.html) software for Pocket PCs free of charge.

Texas Instruments had even more ambitious plans, developing an entire family of touchscreen prototypes: the TI PLT SHH1, FHH1, and SU1. All three ran Inferno OS on TI's own OMAP 1510 ARM processor, before the company quietly canceled the entire project in early 2004. (I happen to own a few TI PLT SHH1 prototypes, likely built for demonstrations at TI's 2002 T³ conference. They are pretty slow.)

Only Casio actually brought the concept to market, releasing the SH-3 based ClassPad 300 in 2003. More than two decades later, its successors, the ClassPad II fx-CP400 and fx-CG500, remain the only graphing calculators available with this distinctive form factor. Casio has also used SuperH processors in a wide range of other products, including other graphing calculators and electronic dictionaries.

The shared CPU heritage between HP Xpander and Casio opened up an intriguing possibility: bringing HP's canceled Xpander project back to life on a Casio ClassPad. With the help of Claude Fable 5.1 and 5.2, I ported Windows CE 5.0 to the ClassPad II, successfully running the original 2001 SH-3 Math Xpander binaries without access to the source code or recompilation. (I initially managed to boot Windows CE 6.0 all the way to the desktop, but its performance was unacceptable, and it left only a few hundred KiB of free RAM. Windows CE 5.0 proved to be a much better fit.)

## The Obstacles

The biggest obstacle was endianness.

Windows CE expects little-endian operation, and much of the OS consists of precompiled binaries. Unfortunately, Casio hardwired the ClassPad II's SH7305 processor to operate in big-endian mode, with no software-selectable way to switch it.

To make matters worse, there were three additional hardware constraints:

- **No hardware FPU:** The SH7305 lacks a hardware floating-point unit, whereas standard Windows CE SH-4 binaries expect one.
- **Only 16 MiB of RAM:** Both Windows CE and the compatibility layer had to fit within an extremely tight memory budget.
- **Stock OS preservation:** Casio's original calculator OS had to remain intact and fully bootable.

## The Solutions

### 1. Same-ISA Dynamic Binary Translation

Since the guest OS and the host processor share the SuperH instruction set architecture, Claude built a lightweight dynamic binary translator (`dbt/`) to bridge the endianness gap.

Unlike conventional cross-architecture emulation, most instructions can execute without modification. The translator only needs to handle a few specific cases on the fly:

- **Memory accesses:** Rewriting 16-bit and 32-bit loads and stores with byte swapping to preserve the guest's little-endian memory layout.
- **Floating-point operations:** Redirecting FPU instructions to a software floating-point emulator (`fpuemu/`).
- **Control flow and privileged state:** Translating branches and a small number of privileged instructions.

Pure arithmetic executes at native speed, while memory-intensive operations suffer a roughly 2–4x slowdown.

Claude also implemented the Windows CE OEM Adaptation Layer (OAL) and hardware drivers for the ClassPad's display, touchscreen, keypad, battery, and power management (`bsp/`).

### 2. Squeezing Windows CE into Flash and RAM

With just 16 MiB of physical RAM, memory conservation was critical.

To minimize RAM usage, the Windows CE system image runs using Execute-in-Place (XIP) directly from the ClassPad's NOR flash, rather than being copied into RAM. The Windows CE kernel, shell, and drivers fit into an unused 5 MiB gap within the stock OS flash region, while Math Xpander resides in the flash area of Casio's eActivity file system, which it overwrites (in other words, destroys). This comes at a cost: every time the stock Casio OS boots, it reformats that area, wiping Math Xpander. (The loader can always re-flash it from a file in the main storage.)

The entire 16 MiB RAM budget is divided equally:

- 8 MiB for Windows CE
- 8 MiB for the dynamic translator: its own code, its block tables, and a 4 MiB code cache

The Windows CE loader (`dbt/hhk/`) can be launched as a HollyHock 3 add-in, with no need to replace Casio's original operating system. A simple reset returns the calculator to its stock OS.

### 3. Patching the Xpander Binaries

Getting Windows CE running was only part of the challenge. The original Math Xpander binaries also needed several compatibility patches, which Claude helped develop.

**Section alignment and relocations.** The original Pocket PC binaries used 1 KiB section alignment, whereas the Windows CE XIP image builder requires 4 KiB alignment. Claude also had to reconstruct the relocation table of the main executable, which had been linked at a fixed address without one.

**Floating-point compatibility shim.** The original SH-3 application and the SH-4 Windows CE system libraries use different floating-point calling conventions. A small injected compatibility shim bridges these differences, allowing the original Math Xpander binaries to work with the newer system libraries.

**Higher-resolution display support.** Math Xpander was hardcoded for the HP Xpander's original 240 × 320 display. By patching a handful of hardcoded values across three DLLs, Claude widened its graphing and geometry canvases to the ClassPad II's larger 320 × 528 screen.

## Layout

| Path | What |
|---|---|
| `docs/` | Plan, hardware facts, FPU emulation design, boot/image notes |
| `fpuemu/` | Integer-only SH-4 FPU emulator core + host test harness (clang) |
| `tools/nkbin.py` | Parser/inspector for CE `.bin` images (B000FF) and ROMHDR |
| `dbt/hhk/` | HollyHock 3 programs for the real fx-CP400: `wince.hh3` (a gint program) shows the loader, flashes NK.bin/EXT.bin when asked and starts the translator from Casio's OS |
| `loader/` | history only: the pre-translator loader idea (see its README) |
| `bsp/classpad/` | Platform Builder 6.0 BSP (OAL, drivers, .bib/.reg files); needs PB 6.0 on Windows |
| `bsp/classpad50/` | the same BSP for Windows CE 5.0 (the current target, docs/08); needs PB 5.0 on Windows |

## Status

See [docs/00-plan.md](docs/00-plan.md). The little-endian→big-endian
translator (`dbt/`) runs its three test guests to completion inside casio-emu
(2026-10-22). No Windows CE code has been loaded yet.

## Host prerequisites

- macOS/Linux with clang or gcc and python3 for `fpuemu` tests and `tools/`.
- fxSDK's `sh-elf-gcc` (`giteapc install Lephenixnoir/fxsdk Lephenixnoir/sh-elf-gcc`) for `dbt/`, including the HollyHock programs (no HollyHock SDK needed).
- Windows (VM is fine) with Platform Builder 5.0 for `bsp/classpad50/` (Visual Studio 2005 + Platform Builder 6.0 R3 for `bsp/classpad/`).
- casio-emu (fx-CG100 model) for first bring-up before touching hardware.

```sh
make test          # host tests: FPU emulator + translator
make -C dbt run    # build launcher+translator+guest, run in casio-emu (GUEST=hello|exc|fpu|tmu|bench|lcd|apicall|mmu|aspace|bank|devmem|tlbdata|flush|fpudis)
make -C dbt GUEST=lcd screenshot   # same, dumping the emulated LCD to dbt/build/dbt-lcd.png
make -C dbt NKBIN=path/NK.bin RUN_SECS=300 run  # run a Windows CE .bin image from emulated flash for 5 minutes (docs/05)
python3 tools/cesym.py NK.bin RELDIR 8c186010  # guest address -> module!symbol (needs the build's .map files)
make -C dbt GUEST=nkimg nkrun      # loader self-test with a synthetic CE-format image (copied into RAM)
make -C dbt GUEST=nkxip nkrun      # the same for an image executing in place from the emulated NOR (XIP)
make -C dbt BUILD=build-x ...      # a private build directory per run: runs (and emulators) can go in parallel
make -C dbt DEVICE=1 BUILD=build-dev hhk   # wince.hh3 for a real fx-CP400 with HollyHock 3 (docs/03), checked against the .g3a
make -C dbt hhkrun                 # the device stub (hhk/takeover.S) running the current guest in casio-emu
make -C dbt flashtest              # hhk/norflash.c against casio-emu's NOR command model
python3 tools/nkbin.py path/to/NK.bin
```
