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

`platform.bib` includes these DLLs when `BSP_LEGACY_MFC=1`. They are ROM
**FILES**, retaining the original PE headers for the runtime loader (including
SH3), rather than SH4 ROMIMAGE MODULES. Their runtime location is `\Windows`.
No COM registration is added.

1. Make this repository, including the DLLs, available in Windows.
2. Open the ClasspadUI **Release** Platform Builder build window.
3. Run `tools\windows\build-legacy-mfc.bat` from this checkout. It copies the
   DLLs to the installed BSP, backs up and appends to its existing platform.bib
   if needed, then runs buildrel and makeimg. It preserves the installed BSP's
   extra application entries and config.bib ROM layout.
4. Set `BSP_LEGACY_MFC=1` in the OS Design environment settings for later builds.
5. Check makeimg's size result against the chosen flash window before using
   NK.bin. This task has not built or flashed a new image.

The existing SH3 loader patch must be present **and built into kernel.dll**.
The script checks the source marker only; that does not prove the running kernel
contains it. See `bsp/kernel/README.md` for the kernel rebuild procedure.

## Dependency audit (2026-11-06)

Compared every static import by name/ordinal with exports from the mounted
ClasspadUI CLASSPAD_SH4_Release directory and these four original DLLs:

| DLL | Import providers (symbol counts) | Result |
|---|---|---|
| mfcce300.dll | coredll (317), commctrl (9), aygshell (8), doclist (1) | **Missing doclist.dll!DocList_Create** |
| olece300.dll | mfcce300 (324), coredll (179) | Direct imports resolve; transitively blocked by doclist |
| mfcce211.dll | coredll (316), commctrl (11) | All static imports resolve |
| olece211.dll | mfcce211 (324), coredll (174) | All static imports resolve |

The CD's doclist.dll is an x86 emulator binary and is not suitable. Its SH3
DOCLIST.LIB is an import library, not the implementation. A compatible native
implementation is still needed before the 300 runtime can load. No fake
DocList_Create stub or binary patch has been installed. Export resolution is
not a runtime compatibility test; neither pair has been LoadLibrary-tested in
this change. Release-directory presence also does not prove inclusion in NK.bin;
verify coredll, commctrl and aygshell in the resulting ROM module table.
