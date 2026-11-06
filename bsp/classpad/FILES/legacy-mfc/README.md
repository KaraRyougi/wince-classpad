# Legacy MFC runtimes for ClassPad

The four original DLLs are checked in here (binary in .gitattributes). They
came from the eMbedded Visual Tools 3.0 CD2; to re-derive them:

| Files | Source directory on EMBEDVT3_CD2 | PE machine |
|---|---|---|
| MFCCE300.DLL, OLECE300.DLL | `POCKTSDK/PROGRAM FILES/MFC/LIB/SH3` | SH3, 0x01a2 |
| MFCCE211.DLL, OLECE211.DLL | `HPC30SDK/MFC/LIB/SH4` | SH4, 0x01a6 |

Copy with lowercase destination names and check `SHA256SUMS`. Use the retail
DLLs, not the D/debug or I/localized-resource variants. Total payload: 1,313,962
bytes before image compression/alignment. Keep the 211 and 300 names unchanged.

## Build

Only `mfcce300.dll` goes into the image (2026-11-06): `platform.bib` lists it
unconditionally as a ROM **FILE** (not an SH4 MODULE), so the loader reads
the original SH3 PE header; at run time it is `\Windows\mfcce300.dll`. Its
one non-system import, `DocList.dll!DocList_Create`, comes from
`SRC\APPS\DOCLIST`. The other three DLLs stay here, unused by the image.

mfcce300 is an SH3 binary: it loads only on a kernel built with
`bsp/kernel/loader-sh3-images.patch`. Applying the patch to loader.c is not
enough; `nkmain.lib` has to be rebuilt (`build -c` in
PRIVATE\WINCEOS\COREOS\NK\KERNEL) and kern.dll relinked (Sysgen, or
`sysgen -p common nk`). `claude-build\build.bat` on the VM does both once.
The 2026-11-06 IDE build lacked it: nkmain.lib (Nov 5 03:33) predates the
patched loader.c (Nov 5 14:30) and kern.dll has no aligned 0x01A2/0x01A3
constant.

## Dependency audit (2026-11-06)

Compared every static import by name/ordinal with exports from the mounted
ClasspadUI CLASSPAD_SH4_Release directory and these four original DLLs (a
later check against the 2026-11-06 image, with DocList.dll's .def: all 1358
imports of the four resolve):

| DLL | Import providers (symbol counts) | Result |
|---|---|---|
| mfcce300.dll | coredll (317), commctrl (9), aygshell (8), doclist (1) | **Missing doclist.dll!DocList_Create** |
| olece300.dll | mfcce300 (324), coredll (179) | Direct imports resolve; transitively blocked by doclist |
| mfcce211.dll | coredll (316), commctrl (11) | All static imports resolve |
| olece211.dll | mfcce211 (324), coredll (174) | All static imports resolve |

The CD's doclist.dll is an x86 emulator binary and is not suitable. Its SH3
DOCLIST.LIB is an import library, not the implementation. A compatible native
implementation is still needed before the 300 runtime can load: that is
`SRC\APPS\DOCLIST` (DocList.dll, in platform.bib with commctrl). No fake
DocList_Create stub or binary patch has been installed. Export resolution is
not a runtime compatibility test; neither pair has been LoadLibrary-tested in
this change. Release-directory presence also does not prove inclusion in NK.bin;
verify coredll, commctrl and aygshell in the resulting ROM module table.
