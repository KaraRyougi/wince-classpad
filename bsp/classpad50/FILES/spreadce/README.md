# SpreadCE in the EXT region

SpreadCE 2.05 for Pocket PC SH3 CE 3.0, English (Bye Design Ltd, 2012-06-27),
a shareware spreadsheet, executed in place from the EXT region like Math
Xpander. `platform.bib` lists `SpreadCE.exe` as `MODULES ... EXT P` (P: no
CPU type check, it is SH3) and `spreadce.lnk` as `FILES ... EXT`;
`platform.dat` copies the link to the Start menu.

## Which build, and where it comes from

The current SpreadCE (3.22, 2025) is a single 7 MB EXE in every SH variant,
too large for the EXT region next to Math Xpander and WordPad. 2.05 is the
last of the older line still archived: `sprep3en.zip` (920,809 bytes) from
the Wayback Machine's 2012-09-19 copy of
`http://www.byedesign.co.uk/sce/sprep3en.zip`, holding `SpreadCE.exe`
(1,492,992 bytes, SHA-256
`273d94296644ee611fa6cd78eecbe99d753bfb15b4a476f7fe01f87d67f3feac`), its
`readme.txt` and 20 `SCEHlp*.htm` help pages (not in the image). Bye
Design's own site no longer serves the 2.x files.

The binary is not in the repository. To prepare it:

    tools/sh3prep.py <unzipped>/SpreadCE.exe <VM>/PLATFORM/CLASSPAD/FILES/spreadce/SpreadCE.exe

and copy `spreadce.lnk` next to it (`tools/stage_bsp50.py` does that with the
rest of the BSP).

## What sh3prep.py does to it

`SpreadCE.exe` is linked /FIXED at 0x10000 with 1 KiB sections and no
relocations, so neither romimage (4 KiB XIP sections only) nor a section
move can take it as it is. `pe4k.reconstruct_relocs_pdata` rebuilds the
HIGHLOW fixups (25,556) from the code's own structure, since in a 1.5 MB
image an own address is also a possible instruction pair or integer:

- in `.text` only literal-pool words count: the targets of the
  `mov.l @(disp,PC)` instructions decoded from the 2,377 `.pdata` functions,
  the 264 import thunks and the leaf functions (no `.pdata` entry), traced
  along their control flow from every call, `bsr` and code pointer that
  lands outside the `.pdata` functions. Without the leaf functions (the
  first version) 7 pool pointers to `.data` stayed unmoved, among them the
  one-instruction "is this the list separator / decimal point" helpers the
  number parser uses, so `0.5` was taken for text and `=0.5` was a formula
  error (found on the emulator, 2026-11-10);
- a word into `.rdata`/`.data` is an address; a word into `.text` only when
  the target is a code entry: a `.pdata` function start, a thunk, a
  register target that reaches `jsr`/`jmp` (far branches inside a function),
  or an even target outside every `.pdata` function after the end of
  something (`rts` and its delay slot, a pool, padding: the leaf functions,
  which have no `.pdata` entry, such as the 5-word worksheet function table
  entries' handlers);
- numbers: MAKELONG-style pairs and the zlib fixed-Huffman tables
  (0x0008xxxx, 0x0007xxxx, 0x0005xxxx bit-reversed runs in `.rdata`),
  `.pdata` length words, and two odd `.rdata` words in tables of 16-bit
  pairs (`0x13F53C`, `0x145A84`, the review's overrides);
- the review (2026-11-09): every target accepted by the "after the end of
  something" rule alone (8) disassembled as a function start; every literal
  whose loaded register is not called but lands inside a function (47)
  disassembled as arithmetic or an argument constant; no rejected `.rdata`
  run of three or more points into one function except the Huffman tables.

`sh3prep.RELOC_PINS` pins the SHA-256, the fixup count and the overrides, so
another copy (the Chinese `sprep3cs`, a later version) is refused rather
than relocated unreviewed. Then the usual steps: `pe4k.realign` to 4 KiB
sections (checked against the original), and `sh3imports` sends 40
floating-point imports to SH3CRT.dll, including `_nextafter`, which SH3CRT
exports for SpreadCE. The SH3CRT IAT is not in the new `.sh3crt` section
but at the end of `.data`: CE 5.0 romimage gave an EXE's added `.sh3crt`
read-only flags in the ROM image (a DLL's stays writable), whether it came
after `.reloc` or before it, so the loader's IAT write paged in a "R/O page
from XIP module" and the first run (2026-11-10) failed with "not a valid
Windows CE application". `.data` keeps its flags in ROM; the loader fills the
IAT from the lookup table, which stays in `.sh3crt`. `.data`'s raw data grows to cover
the IAT (its zero-filled tail written out as zeros): romimage binds an EXE's
imports while building and finds an rva only inside a section's raw data.

## Known gaps

- Runs on the emulator (2026-11-10): it starts, `=1/3` and `=5/10` compute
  and print (0.333333, 0.5). The build with the leaf-function fixups (decimal
  numbers) is not run yet.
- `_jn` and `_yn` (Bessel functions) take a double but stay with the SH-4
  coredll (SH3CRT has no wrapper for them): BESSELJ/BESSELY give wrong
  results.
- `swprintf` with `%-.*G` / `%.*f` (the General and fixed number formats)
  stays with the SH-4 coredll. SHx `va_arg` reads a double as two words from
  memory with 4-byte steps (`stdlib.h`, `_M_SH`), which is how the SH-3
  caller passes it, so this should print correctly; to be checked on screen.
- The help pages are left out (570 KB); its Help menu has nothing to show.
- Shareware, unregistered (the shareware build is not time-limited).
