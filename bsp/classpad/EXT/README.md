# EXT region: files in the NOR hole

A second, files-only Windows CE ROM region in the erased hole inside Casio's
OS image (chip 0x0EE0000-0x13E0000, P1 `0x80EE0000`, 5 MiB; docs/03 "The EXT
region"). The OAL hands it to the kernel in `OEMInit` through
`g_pOemGlobal->pROMChain` (`SRC/OAL/OALLIB/init.c`, checks in
`SRC/INC/classpad_extrom.h`), so its files appear in `\Windows` next to the NK
image's and load like any ROM file (SH3 binaries through the kernel's SH3
loader patch).

It is not built by makeimg: `tools/mkextrom.py` writes it from a manifest
(`<ROM name> <source> [flags]`), on the Mac:

    make -C dbt NK_WINDOW=eact XPANDER=~/Downloads/Xpander_SH3/files extrom

gives `dbt/build/EXT.bin`. Copied to the calculator as `\fls0\EXT.bin` next to
`NK.bin`, `wince.hhk` (NK_WINDOW=eact) writes it into the hole when it differs
and boots; Casio's boots leave it there, so it is written once. The NK image
must be in the eActivity area: the hole window overlaps this region.

- `xpander.txt`: Math Xpander 1.0 (Saltire, Pocket PC 2000 SH3 cab
  `Xpander.PPC300_10003.cab`): 24 SH3 binaries, help pages, keypad bitmaps and
  `Math Xpander.lnk`, 3.8 of the 5 MiB. The binaries are not in the
  repository; `XPANDER` points at the unpacked cab. Its outside dependencies
  are in NK: `mfcce300.dll` (`FILES/legacy-mfc`), `DocList.dll`
  (`SRC/APPS/DOCLIST`), coredll, aygshell, ole32. The NK side adds the Start
  menu link (`FILES/platform.dat`) and the `.mwb` file type
  (`FILES/platform.reg`). Not done: registering the MathKeyPad input method
  (the cab ran `regsvrce.exe` on it).
- `Math Xpander.lnk`: the link, pointing at `\Windows\MathXpander.exe` (the
  cab's points into `\Program Files\Xpander`).

Tests: `make test` (`bsp/tests/test_extrom.c`: the region checks, rejection
cases) and `make -C dbt NK_WINDOW=eact GUEST=extrom run` (the region in the
emulated NOR, read by a translated guest with the OAL's code).
