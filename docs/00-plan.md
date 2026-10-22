# Plan and status

Last updated: 2026-10-22

## The blocker found on day one: endianness

Windows Embedded CE supports **little-endian only** (Microsoft's own CE 6.0
architecture notes say so). Every Casio SH7305 calculator runs the core in
**big-endian** mode: Casio's OS, the bootloader at flash offset 0x300, gint
(`-mb`) and casio-emu all assume it. On SuperH the byte order is latched from
a mode pin (MD5 on SH7708/SH7750; the SH-4A SoCs use the same scheme) at
power-on reset. There is no software switch, and SH instruction words are
themselves subject to the data endianness, so CE's binaries cannot run on a
big-endian core by any amount of kernel work.

Consequences:

- A **pure-software port is impossible** on an unmodified calculator.
- A port is possible only on a **hardware-modified unit** whose endian strap
  is flipped to little-endian. On such a unit Casio's OS, bootloader and every
  existing add-in stop working; the reset vector in flash must already hold
  our own little-endian boot code before the strap is flipped. The fx-CP400
  SoC is a chip-on-board under epoxy, so that mod is realistic (if at all)
  only on an fx-CG50/CG100 board where the strap resistor can be located.
- The same port can be developed and run in **casio-emu with a little-endian
  mode**, which does not exist yet but is a contained change to an emulator
  that already stores guest memory as byte-swapped longwords.

## Routes

| Route | What it gives | Cost / risk |
|---|---|---|
| A. Emulator-only | WinCE booting on an emulated SH7305 (LE mode) with a full debugger | casio-emu LE mode; no device |
| B. Strapped device | WinCE on a sacrificial fx-CG50/CG100 | find and flip the MD strap; flash own LE bootloader first; brick risk; device loses Casio OS |
| C. Drop WinCE, go Linux | Big-endian SH4A is supported, GCC `-m4al` needs no FPU emulation, mainline SH7305 work started 2026-10 | no Win32 stack; all hardware knowledge carries over |

Route A is a prerequisite for B. Everything in this repository except the
boot path is shared by A and B.

## What exists

- `fpuemu/`: SH-4 FPU trap-and-emulate core (integer-only, endian-neutral),
  verified bit-exact against the host FPU for add/sub/mul/div/sqrt/fmac/
  conversions/compares in RN and RZ, plus instruction-level decoder tests.
- `tools/nkbin.py`: CE `.bin` image inspector (records, ROMHDR, module table).
- Docs 01-03: hardware facts, FPU emulation design, boot/image notes.

## Milestones (routes A/B)

1. Decide the route. (Blocked on the user.)
2. casio-emu: little-endian mode + "raw kernel" load (write NK.bin records to
   RAM, set PC/SP/VBR, go). Needed before any CE code can be tried.
3. Platform Builder 6.0 R3 + VS2005 in a Windows VM; build a headless SH4
   image; confirm the toolchain works on a stock BSP (emulator `r2d` in QEMU
   is a sanity check that needs no Casio code).
4. OAL for SH7305: startup, INTC, TMU tick, SCIF debug serial, KITL (serial).
   Boot headless to a serial shell inside casio-emu.
5. Hook `fpuemu` into the kernel's general-illegal-instruction path
   (EXPEVT 0x180/0x1A0), including delay-slot handling and lazy per-thread
   FPU state. Count traps per module.
6. Display driver (flat framebuffer, 320x528 CP400 / 396x224 CG50-class),
   keyboard, touch. GWES up.
7. Only for route B: NOR boot sector with our own LE bootloader, flash
   layout, CFI programming code, strap mod procedure.

## Open questions

- Exact MD pin/strap on the fx-CG50 and fx-CG100 boards (teardown needed).
- Does `shcl.exe` in PB 6.0 have a no-FPU code generation switch? Only
  affects code we compile ourselves; the prebuilt OS binaries need the trap.
- CE's SH4 default FPSCR value (believed 0x00040001: RZ, DN=1); affects what
  rounding the emulator sees most of the time.
