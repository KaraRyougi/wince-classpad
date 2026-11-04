# Boot and image notes

## CE image format

ROMIMAGE produces `NK.bin` (B000FF): image start/length, then records of
(address, length, checksum, data) and a terminator whose length field is the
entry point. `tools/nkbin.py` dumps it, locates the ROMHDR via the "ECEC"
signature at image start + 0x40 and prints the module table. `--flat` writes
the equivalent flat memory image (what a loader must place at `image start`).

## Where the image lives

- **RAM image** (the first images): all records are written to SDRAM. 16 MiB
  is enough for a headless kernel (about 2 MiB) but a GWES image plus the
  RAM file system ran out of pages (docs/05).
- **XIP image** (current, in the emulator too): `ROMSTART` in `config.bib`
  points into the NOR at 0x81000000; code executes from flash, RAM holds
  data and the object store. casio-emu places the flat image with
  `HLE_OSROM`; on the device it has to be flashed (the first 16 MiB of the
  32 MiB chip hold Casio's OS, the offset is a parameter to revisit).

## Boot paths by route

- **Emulator (route A)**: casio-emu gets a "raw kernel" option: load the
  `--flat` image (or parse NK.bin records) into guest RAM, set
  PC = entry, SP somewhere in RAM, VBR = 0x8C000000-class scratch, SR with
  BL=1 MD=1 and interrupts masked, in **little-endian** mode. The OAL startup
  then owns the machine.
- **Strapped device (route B)**: the reset vector at physical 0 must hold our
  little-endian bootloader; it initializes the BSC/SDRAM/CPG the way Casio's
  bootloader does (reverse-engineer from the 0x300 "LV777" code), then jumps
  to the XIP kernel. It must be written to flash from a big-endian add-in
  *before* the strap is flipped, using the OS's flash routines or own CFI code.
- **A hollyhock/gint loader add-in** (the original idea) is not viable: it
  would run under the big-endian Casio OS, and nothing little-endian can
  execute on that core.

## SH4 OAL startup checklist

- Entry from the loader with MMU off (SH4 kernel code runs in P1/P2), caches
  invalidated, interrupts masked.
- `OEMAddressTable` is not used on SH4; the kernel maps RAM at the P1 address.
- Timer tick: TMU channel 0 at the SH7305 address (gint has it).
- Interrupt dispatch: SH-4A INTC (INTEVT codes, IPR/IMR at 0xA4080000 class),
  mapped to SYSINTR ids in `OEMInterruptHandler`.
- Debug serial: SCIF; KITL over the same port.
- Memory: `MainMemoryEndAddress` = 0x8CFFFFFF on 16 MiB units.
