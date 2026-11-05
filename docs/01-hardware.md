# Hardware facts: Casio SH7305 calculators

Sources: ClassPad.Dev wiki (memory map, gint ClassPad port), the6p4c's
fx-CP400 firmware write-ups, Omnimaga/Cemetech teardowns, Phoronix on the
2026-10 SH7305 Linux patches, casio-emu's own SH-4A core.

## SoC

- Renesas SH7305, SH-4A core, SH4AL-DSP variant: **no FPU**, DSP extension
  with X/Y memory. About 118 MHz on the fx-CP400 (some sources say 116),
  58 MHz stock on the fx-CG50 (community overclocks to 200+ MHz via CPG).
- **Big-endian** as strapped by Casio; byte order is a reset-time mode pin.
- Caches: 32 KiB 4-way instruction cache (casio-emu models it), operand cache
  of the same class. SH-4A cache-block instructions (ICBI/OCBI/OCBP/OCBWB).
- MMU: SH-4 style software-filled UTLB/ITLB; the calculators run in 29-bit
  address mode. Casio's OS uses the TLB only for the add-in ROM mapping at
  virtual 0x00300000; gint runs with no userspace and flat physical pointers.
- On-chip memories: IL memory 16 KiB at 0xE5200000 (instruction, zero wait),
  X/Y RAM 8 KiB each around 0xE5007000 / 0xE5017000 (gint sizes differ; verify),
  RS memory 2 KiB at 0xFD800000 retained in standby.
- Peripherals (SH7724-like register layout): CPG at 0xA4150000, INTC,
  TMU, SCIF (3-pin serial on CP400 and CG50; none on CG500), KEYSC, ADC
  (touch, battery), display DMAC at 0xB4000000, USB function at 0xA4D80000 class,
  SPU (two audio DSP cores). gint and casio-emu are the reference for all of these.

## Memory map (fx-CP400)

| Virtual | Physical | Size | What |
|---|---|---|---|
| 0x80000000 | 0x00000000 | 64 MiB | Flash, cached (P1). Casio OS executes in place here (map in docs/03). |
| 0xA0000000 | 0x00000000 | 64 MiB | Flash, uncached (P2) |
| 0x8C000000 | 0x0C000000 | 16 MiB | SDRAM (ESMT M12L128168A, 128 Mbit). Framebuffer at 0x8C000000. |
| 0x8E000000 | | | MCS region per the wiki (conflicting notes; treat as reserved) |
| 0x00000300 | | | Casio bootloader ("LV777", "CASIOABS" at 0x3380) |

fx-CG50: same layout with 8 MiB SDRAM. fx-CG100: 16 MiB SDRAM (casio-emu
allocates 16 MiB at 0x8C000000), 32 MiB NOR.

## Flash

The chip on the fx-CP400 is a Spansion custom part (board marking S99 xxxxx)
that a 2013 teardown called NAND. All software evidence says it is a linear,
16-bit, memory-mapped device that the CPU executes from (firmware patch
offsets equal ROM address minus 0x80000000; gint speaks of "flash-mapped ROM").
For our purposes it is NOR-like and XIP-capable: a 64 MiB x16 part with
uniform 128 KiB sectors and the AMD/Spansion command set (unlock 0xAAA/0x554,
32-word write buffer), as casio-emu models it from the OS's driver; the
2013 teardown's 32 MiB figure was wrong (docs/03 "The fx-CP400 flash map":
Casio's 21 MiB OS image, MCS backup, 24 MiB storage and 5.5 MiB eActivity
file systems). The fx-CG50/CG100 use a 32 MiB Spansion NOR (S29GL256 class).

## Display and input

- fx-CP400: 320x528 16-bit LCD, R61524-class controller, resistive touch via ADC.
- fx-CG50/CG100: 396x224 16-bit LCD, R61524 controller, key matrix via KEYSC.

## Debug access

- 3-pin serial (SCIF) on CP400 and CG50: the only practical KITL/debug channel.
- casio-emu: write a byte to 0xFFFFF000 to print on the host (emulator only).
