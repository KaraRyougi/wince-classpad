# loader/

The device boot path lives in `dbt/hhk/` (HollyHock 3 programs) and
`dbt/loader/` (the casio-emu launcher); this directory is only history.

The original plan here was a gint/HollyHock add-in that reads NK.bin into RAM
and jumps to the kernel. That was written off because a little-endian kernel
cannot execute on the big-endian core, which is true, and the translator
(`docs/04`) is what made the add-in route viable after all: the add-in does
not run CE, it starts the translator, and the translator runs CE.

What exists now (`docs/03` "Boot paths"):

- `dbt/loader/loader.S`: the raw `.g3a` payload casio-emu starts at
  0x00300000; copies the translator image to 0x8CC80000 and jumps.
- `dbt/hhk/`: `wince.hh3`, the HollyHock 3 program (a gint program, since
  2026-11-10) that does the same from Casio's OS on a real fx-CP400: it shows
  the loader, writes `\fls0\NK.bin` and `\fls0\EXT.bin` into their NOR
  windows when asked, then hands over (the translator image rides in the ELF
  at its link address; `takeover.S` quiets the interrupt sources and jumps).
  `make -C dbt hhk` builds it and checks that it carries the same translator
  bytes as the emulator's `.g3a`.
- Route B (a little-endian boot sector behind a flipped endian strap) was
  never built and is not needed.
