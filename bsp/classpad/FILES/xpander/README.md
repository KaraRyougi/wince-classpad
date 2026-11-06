# Math Xpander in the EXT region

Math Xpander 1.0 (Saltire Software, 2001), the Pocket PC 2000 SH3 build
(`Xpander.PPC300_10003.cab`), executed in place from the EXT region: the
second XIP region of `config.bib`, in the erased hole inside Casio's OS image
(P1 `0x80EE0000`, 5 MiB; docs/03 "The EXT region"). `platform.bib` lists its
24 SH3 binaries as `MODULES ... EXT P` (P: no CPU type check, they are SH3)
and its help pages, keypad bitmaps and `mathxpander.lnk` as `FILES ... EXT`.
makeimg then writes `EXT.bin` next to `NK.bin`.

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
workbooks (`platform.reg`). Not done: registering `MathKeyPad.dll`, the
keypad input method (the cab ran `regsvrce.exe` on it).
