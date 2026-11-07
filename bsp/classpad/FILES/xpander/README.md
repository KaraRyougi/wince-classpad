# Math Xpander in the EXT region

Math Xpander 1.0 (Saltire Software, 2001), the Pocket PC 2000 SH3 build
(`Xpander.PPC300_10003.cab`), executed in place from the EXT region: the
second XIP region of `config.bib`, in the erased hole inside Casio's OS image
(P1 `0x80EE0000`, 5 MiB; docs/03 "The EXT region"). `platform.bib` lists its
23 SH3 DLLs and `MathXpander.exe` as `MODULES ... EXT P` (P: no CPU type
check, they are SH3) and its help pages, keypad bitmaps and `mathxpander.lnk`
as `FILES ... EXT`. makeimg then writes `EXT.bin` next to `NK.bin`.

The Pocket PC tools linked the binaries with 1 KiB sections, and romimage
takes only 4 KiB-aligned XIP modules ("only 4k page alignments supported at
this time"): stage.py realigns every `MODULES` entry under `FILES` with
`tools/pe4k.py` (same file layout and bytes; new section addresses, the RVAs,
every HIGHLOW fixup and the .reloc table moved with them; checked against the
original after relocating both). `MathXpander.exe` is linked /FIXED, with no
relocations, and CE 6 refuses it as a plain file too ("not a valid Windows CE
application": `loader.c` ReadO32Info takes no section that is not page
aligned). `pe4k.reconstruct_relocs` rebuilds its 222 HIGHLOW fixups (in a
/FIXED image at 0x10000 every own address is 0x0001xxxx, no SH instruction:
157 literal-pool words in .text, 43 vtable and message-map pointers in
.rdata, 2 in .data, 20 .pdata function starts; .rsrc's 0x00010000 words are
version fields), checked by hand and pinned by SHA-256 in stage.py; then it
is realigned and runs in place like the DLLs.

Only this README and `mathxpander.lnk` (pointing at
`\Windows\MathXpander.exe`; the cab's link points into
`\Program Files\Xpander`) are in the repository. The binaries and help files
come from the unpacked cab:
`tools/windows/claude-build/stage.py --xpander ~/Downloads/Xpander_SH3/files`
copies `Program Files/Xpander/*` and `Windows/*` (not `regsvrce.exe`) into
this folder of the VM package, where `platform.bib` expects them.

Outside dependencies, all in NK: `mfcce300.dll` (`FILES/legacy-mfc`, XIP),
`DocList.dll` (`SRC/APPS/DOCLIST`), coredll, aygshell, ole32. The NK side also
copies the link to the Start menu (`platform.dat`) and registers `.mwb`
workbooks (`platform.reg`). `platform.reg` also registers `MathKeyPad.dll`,
the math keyboard, as a SIP input method (what its DllRegisterServer writes;
the cab ran `regsvrce.exe` on it), and the fx-CP400 Keyboard key shows it.

Floating point: the SH-3 binaries call coredll's software floating-point
helpers and double C functions, which CE 6.0's SH-4 coredll implements with
the FPU calling convention; `stage.py` points those imports at
`SH3CRT.dll` (`SRC/APPS/SH3CRT`, the SH-3 convention) with
`tools/sh3imports.py`. Without it the math keyboard computed every tap as
"no key" (found in casio-emu, 2026-11-06), and any computed number was
suspect. Not covered: `printf`-family `%f` arguments (variadic doubles) and
any other coredll function taking or returning a float or double outside
the list in `sh3crt.def`.
