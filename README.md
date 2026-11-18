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
- 8 MiB for the dynamic translator: its own code, its block metadata, and a 3.25 MiB code cache

The Windows CE loader (`dbt/hhk/`) can be launched as a HollyHock 3 add-in, with no need to replace Casio's original operating system. A simple reset returns the calculator to its stock OS.

### 3. Patching the Xpander Binaries

Getting Windows CE running was only part of the challenge. The original Math Xpander binaries also needed several compatibility patches, which Claude helped develop.

**Section alignment and relocations.** The original Pocket PC binaries used 1 KiB section alignment, whereas the Windows CE XIP image builder requires 4 KiB alignment. Claude also had to reconstruct the relocation table of the main executable, which had been linked at a fixed address without one.

**Floating-point compatibility shim.** The original SH-3 application and the SH-4 Windows CE system libraries use different floating-point calling conventions. A small injected compatibility shim bridges these differences, allowing the original Math Xpander binaries to work with the newer system libraries.

**Higher-resolution display support.** Math Xpander was hardcoded for the HP Xpander's original 240 × 320 display. By patching a handful of hardcoded values across three DLLs, Claude widened its graphing and geometry canvases to the ClassPad II's larger 320 × 528 screen.

## Status (2026-11-10)

- The target is **Windows CE 5.0** (`bsp/classpad50`, Platform Builder 5.0).
  The project started on CE 6.0 R3 (`bsp/classpad`), which also reached the
  desktop; that BSP is kept for reference and is no longer developed
  (docs/07, docs/08).
- CE 5.0 boots to the Explorer desktop in casio-emu's ClassPad profile and on
  a real fx-CP400, with display (including rotation), keyboard, touch,
  backlight and battery drivers, and suspend/resume through the SH7305's
  R-standby.
- The image carries Pocket PC era SH3 programs next to its own: Plotter-Z
  (source in the tree), Math Xpander and SpreadCE (their binaries are not in
  the repository), a help viewer and the small DLLs they need.
- `wince.hh3`, the loader, is a gint program with a Windows XP style
  interface; it writes `NK.bin`/`EXT.bin` into the flash and starts the
  translator.
- No sound, no network. What was last verified where, and what is still open,
  is in [docs/08-ce50-port.md](docs/08-ce50-port.md).

## Layout

| Path | What |
|---|---|
| `dbt/src/` | the translator proper: SH-4 decoder, emitter, block translator |
| `dbt/rt/` | its bare-metal runtime: code cache, lookup and link stubs, exception vectors, CE image loading, standby, panel console |
| `dbt/guest/` | little-endian test guests (`GUEST=`), one per translator feature |
| `dbt/hhk/` | programs for the real fx-CP400 under HollyHock 3: `wince.hh3` (the loader and flasher) and probes |
| `dbt/loader/` | the raw `.g3a` payload casio-emu starts |
| `dbt/emu-patch/` | scripts that patch a casio-emu checkout with what the translator and the ClassPad profile need |
| `dbt/tests/`, `bsp/tests/`, `fpuemu/tests/` | host tests (`make test`) |
| `fpuemu/` | integer-only SH-4 FPU emulator, checked bit for bit against the host FPU |
| `bsp/classpad50/` | Platform Builder 5.0 BSP: OAL, display/keyboard/touch/battery drivers, applications, `.bib`/`.reg` files |
| `bsp/classpad/` | the earlier CE 6.0 R3 BSP |
| `bsp/kernel/` | small patches to the CE kernel sources |
| `tools/` | image tools (`nkbin.py`, `mkextrom.py`, `pe4k.py`, `sh3prep.py`, `nkflash.py`), analysis and tracing (`cesym.py`, `sh4trace.py`), generators for the loader's fonts and art |
| `tools/windows/` | batch files and the staging script for the Platform Builder VM |
| `docs/` | design documents and the project's dated notes |
| `notes/ce/` | text notes brought back from the Platform Builder VM |
| `loader/` | history only (see its README) |

## Documents

The documents are working notes, written as the work went and dated; older
ones describe states that later ones supersede (file names such as
`wince.hhk` and `nkflash.hhk` are now `wince.hh3`). Most are in English,
docs/07 and most of docs/08 in Chinese.

| Document | Subject |
|---|---|
| [00-plan](docs/00-plan.md) | why a translator: the endianness blocker, the routes considered, milestones |
| [01-hardware](docs/01-hardware.md) | SH7305 and calculator hardware facts |
| [02-fpu-emulation](docs/02-fpu-emulation.md) | the FPU emulator's design |
| [03-boot-and-images](docs/03-boot-and-images.md) | CE image format, where the image lives in flash, boot paths |
| [04-dbt-design](docs/04-dbt-design.md) | the translator: design, performance work, measurements |
| [05-ce-image-and-oal](docs/05-ce-image-and-oal.md) | the CE image and the SH7305 OAL |
| [06-deep-suspend](docs/06-deep-suspend.md) | R-standby: suspend and resume |
| [07-ce42-port-lessons](docs/07-ce42-port-lessons.md) | what the CE 6.0 port taught, as a checklist for an older CE |
| [08-ce50-port](docs/08-ce50-port.md) | the CE 5.0 port, first boots, feedback from the calculator |
| [09-platform-builder-steps](docs/09-platform-builder-steps.md) | first session in the Platform Builder 6.0 VM |
| [design/loader-luna.html](docs/design/loader-luna.html) | the loader's interface mockup |

## Prerequisites

- macOS or Linux with clang or gcc and python3: host tests and `tools/`.
- fxSDK's `sh-elf-gcc` (`giteapc install Lephenixnoir/fxsdk Lephenixnoir/sh-elf-gcc`)
  for everything under `dbt/`; gint (`giteapc install Lephenixnoir/gint`,
  with its fx-CP400 library) for `wince.hh3`.
- casio-emu with the scripts in `dbt/emu-patch/` applied, for running in the
  emulator. `dbt/Makefile` looks for it in `~/Dev/casio-emu`; set `EMU=` to
  point elsewhere.
- Windows (a VM is fine) with Platform Builder 5.0 for `bsp/classpad50/`
  (Visual Studio 2005 and Platform Builder 6.0 R3 for `bsp/classpad/`).
  Windows CE itself is not in this repository; the image (`NK.bin`,
  `EXT.bin`) is built there.
- An fx-CP400 with HollyHock 3 for the real thing.

## Building and running

```sh
make test          # host tests: FPU emulator, translator, BSP pieces that compile on the host
make -C dbt run    # build launcher+translator+guest, run in casio-emu (GUEST=hello|exc|fpu|tmu|bench|lcd|apicall|mmu|aspace|bank|devmem|tlbdata|flush|fpudis|...)
make -C dbt GUEST=lcd screenshot   # same, dumping the emulated LCD to dbt/build/dbt-lcd.png
make -C dbt NKBIN=path/NK.bin RUN_SECS=300 run  # run a Windows CE .bin image from emulated flash for 5 minutes (docs/05)
python3 tools/cesym.py NK.bin RELDIR 8c186010  # guest address -> module!symbol (needs the build's .map files)
make -C dbt GUEST=nkimg nkrun      # loader self-test with a synthetic CE-format image (copied into RAM)
make -C dbt GUEST=nkxip nkrun      # the same for an image executing in place from the emulated NOR (XIP)
make -C dbt BUILD=build-x ...      # a private build directory per run: runs (and emulators) can go in parallel
make -C dbt DEVICE=1 BUILD=build-dev hhk   # wince.hh3 for a real fx-CP400 with HollyHock 3 (docs/03), checked against the .g3a
make -C dbt hhk-preview            # the loader's screens rendered on the host
make -C dbt hhkrun                 # the device stub (hhk/takeover.S) running the current guest in casio-emu
make -C dbt flashtest              # hhk/norflash.c against casio-emu's NOR command model
python3 tools/nkbin.py path/to/NK.bin
```

The Platform Builder side is described in
[bsp/classpad50/README.md](bsp/classpad50/README.md).

## Third-party material

Some files in this repository come from other people or are derived from
their work (Microsoft runtime DLLs and artwork, Plotter-Z, fonts). They are
listed with their origin in [THIRD-PARTY.md](THIRD-PARTY.md).
