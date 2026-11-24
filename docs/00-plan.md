# Project Plan & Architecture Overview

This document explains why `wince-classpad` uses a dynamic binary translator, which alternative approaches were considered, and how the project evolved from its initial prototype to Windows CE 5.0 on real fx-CP400 hardware.

## The Fundamental Blocker: Endianness

Windows Embedded CE for SuperH supports **little-endian only**. Every Casio SH7305 graphing calculator (fx-CP400, fx-CG500, fx-CG50, fx-CG100) runs in **big-endian** mode: Casio's OS, the ROM bootloader, `gint` (`-mb`), and `casio-emu` all assume big-endian execution.

On SuperH, endianness is latched from a hardware mode pin at power-on reset — there is no software switch, and instruction words themselves follow the latched byte order. Consequently, unmodified little-endian Windows CE binaries cannot run natively on a stock Casio calculator through kernel work alone.

## Routes Evaluated

| Route | Description | Trade-offs |
|---|---|---|
| **A. Emulator-only** | Add little-endian mode to `casio-emu` and run WinCE natively there | No real-hardware path |
| **B. Hardware strap mod** | Flip the endian mode resistor on a sacrificial calculator and pre-flash a little-endian bootloader | High brick risk, permanently breaks Casio OS, impractical on epoxy-potted fx-CP400 SoCs |
| **C. Port Linux instead** | Big-endian SH-4A Linux with `-m4al` needs no FPU emulation | Loses Win32 / Pocket PC software compatibility (no Math Xpander) |
| **D. Same-ISA DBT *(chosen)*** | Run little-endian SH-4 Windows CE on the unmodified big-endian SH7305 via a bare-metal dynamic binary translator | 2–4× overhead on memory-heavy code; OAL/drivers pre-swap MMIO; translator owns VBR and DSP scratch registers |

Route D requires zero hardware modification, preserves Casio's stock OS completely, and runs identically in `casio-emu` and on real fx-CP400 hardware.

## Milestones Achieved

1. **Translator core & FPU emulator (`dbt/`, `fpuemu/`)** — Bit-exact integer SH-4 FPU emulator, SH-4 decoder/emitter/block translator with byte-swapped memory accesses, branch chaining, inline caches, and exception/interrupt delivery (`hello`, `exc`, `fpu`, `tmu`, `lcd`, `apicall`, `mmu`, `aspace` test guests passing).
2. **CE 6.0 R3 bring-up (`bsp/classpad/`)** — Built the initial SH7305 OAL, display/keyboard/touch drivers, and XIP flash boot; reached the Windows CE 6.0 desktop in `casio-emu` and on real hardware. High RAM footprint and per-process ASID overhead led to pivoting to Windows CE 5.0 (see [07-ce42-port-lessons.md](07-ce42-port-lessons.md)).
3. **Windows CE 5.0 port (`bsp/classpad50/`)** — Rebuilt the OAL and drivers for Platform Builder 5.0; moved core `NK.bin` into the persistent 5 MiB hole inside Casio's OS flash region and `EXT.bin` (Math Xpander + MFC) into the eActivity area.
4. **Full hardware & app ecosystem** — Added DCS panel support (older and newer fx-CP400 panels), 4-way screen rotation, 20-step backlight control, IIC0 touchscreen, keypad, Fugue flash storage (`\Storage`), USB Function (Mass Storage, CDC ACM serial, PPP USB Network + Telnet), battery gauge, SH7305 R-standby deep suspend/resume, CEtune overclocking, Plotter-Z, PegHelp, and SH-3 Pocket PC compatibility (`SH3CRT.dll`, `DocList.dll`, `pe4k.py`).
5. **Performance & reliability pass** — Added lazy recovery metadata, 16-bank second-chance code cache, ASID-tagged lookup hash, `kdev` selective P4 checks, fast privileged stubs, inline FPU translation, DMAC0 LCD pushes, 2D dirty-rect GPE fast paths, and compressed `.cez` image flashing in `wince.hh3`.

## Where to Read Next

- [01-hardware.md](01-hardware.md) — SH7305 SoC, memory map, NOR flash, and peripherals.
- [02-fpu-emulation.md](02-fpu-emulation.md) — Software SH-4 FPU design and rounding/NaN semantics.
- [03-boot-and-images.md](03-boot-and-images.md) — Flash map, XIP layout, `wince.hh3` loader, and `.cez` packing.
- [04-dbt-design.md](04-dbt-design.md) — Dynamic binary translator internals and performance architecture.
- [05-ce-image-and-oal.md](05-ce-image-and-oal.md) — Windows CE image loading, RAM budget, and SH7305 OAL design.
- [06-deep-suspend.md](06-deep-suspend.md) — SH7305 R-standby suspend/resume sequence.
- [07-ce42-port-lessons.md](07-ce42-port-lessons.md) — Engineering lessons carried from CE 6.0 R3 to CE 5.0.
- [08-ce50-port.md](08-ce50-port.md) — Windows CE 5.0 BSP, real-hardware bring-up, and Pocket PC SH-3 app compatibility.
- [09-platform-builder-steps.md](09-platform-builder-steps.md) — Setting up and working with the Platform Builder Windows VM.
