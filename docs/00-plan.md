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
| **D. Same-ISA translator (chosen)** | A little-endian SH-4 guest (WinCE) runs on the stock big-endian device under a dynamic binary translator that byte-swaps every memory access and emulates the FPU; no hardware mod | translator overhead on every load/store; all guest MMIO must be pre-swapped; the translator owns VBR and the DSP transfer registers (`docs/04-dbt-design.md`) |

Route D is what we are building. Route A (emulator LE mode) is no longer
needed: casio-emu runs the translator as an ordinary big-endian add-in.

## What exists

- `fpuemu/`: SH-4 FPU trap-and-emulate core (integer-only, endian-neutral),
  verified bit-exact against the host FPU for add/sub/mul/div/sqrt/fmac/
  conversions/compares in RN and RZ, plus instruction-level decoder tests.
- `tools/nkbin.py`: CE `.bin` image inspector (records, ROMHDR, module table).
- `dbt/`: the translator (decoder, emitter, block translator with resume
  metadata; host tests), its bare-metal runtime (code cache, lookup/link
  stubs, exception vectors, FPU/mac helpers, hypercall console), a `.g3a`
  launcher and three little-endian test guests. As of 2026-10-22 all three
  guests pass in casio-emu (with `dbt/emu-patch` applied to the emulator).
- Docs 01-04: hardware facts, FPU emulation design, boot/image notes, DBT design.

## Milestones (route D)

1. ~~Translator core + runtime; hello/exception/FPU guests pass in casio-emu.~~
   Done 2026-10-22.
2. ~~Interrupt guest: TMU through pre-swapped MMIO, interrupt through the
   guest VBR under the translator, `rte` back into translated code.~~ Done
   2026-10-22 (34 checks, including interrupts at arbitrary points of
   store/load/branch-heavy loops). Next: a guest that writes the LCD
   framebuffer.
3. ~~Measure: translated vs native slowdown.~~ Done 2026-10-22 (docs/04
   "Performance"): ALU 1.8x, memory 2.8-3.1x, stack push/pop 4.6x, branches
   1.9x, calls 6-9x; direct-jump chaining and inline caches for indirect
   branches are the identified fixes. Still to do: code-cache pressure with a
   larger guest.
4. Guest image choice (blocked on the user): Pocket PC 2000 SH3 ROM (closed,
   Jornada 540 hardware to emulate in the OAL layer) vs self-built CE 6.0 R3
   SH4 image (Platform Builder + VS2005 in a Windows VM, our own OAL).
5. CE image loading: `.bin` records to guest RAM, ROMHDR, PC/SP/VBR, entry.
   For CE 6.0: OAL for SH7305 (startup, INTC, TMU tick, debug serial via
   hypercall first), headless boot to a serial shell inside casio-emu.
6. Display driver (flat framebuffer, 320x528 CP400 / 396x224 CG50-class),
   keyboard, touch. GWES up.
7. Real hardware: run the `.g3a` on an fx-CG50/CP400; fix anything the
   emulator was lenient about (cache coherency, TLB, DSP register state).

## Open questions

- Exact MD pin/strap on the fx-CG50 and fx-CG100 boards (teardown needed).
- Does `shcl.exe` in PB 6.0 have a no-FPU code generation switch? Only
  affects code we compile ourselves; the prebuilt OS binaries need the trap.
- CE's SH4 default FPSCR value (believed 0x00040001: RZ, DN=1); affects what
  rounding the emulator sees most of the time.
