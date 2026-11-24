# Lessons from CE 6.0 R3: Moving to Windows CE 5.0

The project initially targeted **Windows CE 6.0 R3** (`bsp/classpad/`) and reached the Explorer desktop in both `casio-emu` and on real hardware. However, CE 6.0 proved a poor fit for the fx-CP400's 16 MiB RAM and 5 MiB persistent flash hole:

- **Image size:** A CE 6.0 Standard Shell image was ~6.95 MiB (forcing `NK.bin` into the volatile eActivity flash region that Casio's OS wipes on every boot).
- **Memory & translation overhead:** CE 6.0's strict kernel/user separation and per-process ASIDs caused constant address-space switches, TLB refills, `ldc sr`/`rte` traps, and left very little free RAM inside the 8 MiB guest window.

Pivoting to **Windows CE 5.0** (`bsp/classpad50/`) shrunk the core OS image to ~4.8 MiB (fitting cleanly into the persistent 5 MiB flash hole), increased free RAM, and aligned closely with the Pocket PC (CE 3.0) slot memory model used by Math Xpander.

Below is the checklist of every CE 6.0 architectural quirk and how each was resolved in CE 5.0 (see [08-ce50-port.md](08-ce50-port.md) for the full CE 5.0 implementation).

## 1. Kernel & Translator Contract

| CE 6.0 Finding | How It Mapped to CE 5.0 |
|---|---|
| **Bank 1 live kernel state:** `r4/r5/r7_bank1` hold `pCurThread`, context pointer, and `0xFF000000`. Translator exception stubs must preserve bank 1. | Identical in CE 5.0 (`shexcept.src`); bank-1 preservation kept as-is. |
| **Selective P4 (`kdev`) translation:** Kernel reads/writes CCN registers (`0xFF0000xx`) directly; swapping them breaks API calls and ASIDs. `tools/kernel_devfuncs.py` identifies P4-touching functions by signature. | CE 5.0 links kernel + OAL together into **`nk.exe`** (no separate `kernel.dll`). Translator scans `nk.exe`'s text against `kdev_funcs50` (`dbt/rt/kernel_dev50.h`). OAL code in `nk.exe` uses unswapped `P4_R32`/`P4_W32` for `0xFFxxxxxx`. |
| **API call trap range:** CE 6.0 jumps to odd addresses `>= 0xFFFEFE03` (address error `EXPEVT 0x0E0`). | CE 5.0 uses the `0xF001xxxx` trap range; the translator's generic "unfetchable PC → synthetic address error" handles both without change. |
| **TLB miss shortcut (`TLB_REFILL`):** Fast-path UTLB refill in the translator matched CE 6.0's `TLBMissHandler` page-table layout. | Updated in `dbt/rt/rt.c` to recognize and support CE 5.0's page-table structure. |
| **`WaitOneMore` interrupt-event proxy leak:** CE 6.0 QFE bug (`kcalls.c`) leaked interrupt-event proxies on timed waits (`bsp/kernel/kcalls-waitonemore.patch`). | Hole does not exist in CE 5.0; drivers still avoid timed waits on interrupt events as good practice. |
| **SH-3 PE loader rejection:** Kernel `loader.c` rejected CPU type `0x1A2`/`0x1A3` (SH-3) on SH-4 builds. | Same patch applied to CE 5.0 (`bsp/kernel/`); ROM `MODULES` also carry flag `P`. |
| **BL=1 debug output:** `SHInit`/`OEMInit` run with `SR.BL = 1`, where `trapa` resets the CPU. | Early boot bytes write to the RAM debug ring (`0xAC7FF000`); `OEMWriteDebugString` uses `HC_PUTS` once `SR.BL = 0`. |

## 2. FPU & Legacy SH-3 Compatibility

- **Software FPU (`fpuemu/`) + `SR.FD` emulation:** Carried over unchanged; CE 5.0 SH-4 binaries also emit SH-4 FPU instructions and rely on `SR.FD` for lazy FPU context switching.
- **SH-3 vs. SH-4 calling convention mismatch (`SH3CRT.dll`):** Pocket PC 2000 SH-3 binaries pass `float`/`double` arguments to CRT helpers (`_adds`, `_addd`, `sin`, `pow`, `atof`, …) in integer registers / memory, whereas SH-4 `coredll.dll` expects `fr4`/`dr4`. `SH3CRT.dll` re-exports those symbols using integer/pointer signatures, and `tools/sh3imports.py` rewrites SH-3 binary IATs to import from `SH3CRT.dll`.
- **4 KiB section alignment (`pe4k.py`):** CE 3.0 SH-3 used 1 KiB pages, whereas SH-4 CE 5.0 requires 4 KiB alignment for XIP modules. `tools/pe4k.py` realigns sections to 4 KiB and reconstructs the 222 missing relocations of `/FIXED`-linked `MathXpander.exe`.
- **MFC & DocList (`mfcce300.dll`, `DocList.dll`):** `mfcce300.dll` (from eVT 3.0) imports `DocList.dll!DocList_Create`, which CE 5.0/6.0 omit. Our clean-room `bsp/classpad50/SRC/APPS/DOCLIST` supplies the missing exports by Pocket PC ordinal (50–52).
- **Lightweight Help Viewer (`PegHelp.exe`):** Stock CE PegHelp drags in all of IE (~14 MiB). `bsp/classpad50/SRC/APPS/PEGHELP` renders the HTML subset used by Math Xpander help files using only `coredll` + GDI.

## 3. Image Size, Memory & Drivers

- **Two-region XIP layout reversed in CE 5.0:** Because CE 5.0 `NK.bin` fits inside the 5 MiB persistent hole (`0x80EE0000`), `NK.bin` moved into the hole and `EXT.bin` (Math Xpander + `mfcce300.dll`) moved to the eActivity area (`0x83860000`). CE 5.0's `romimage` cannot build two non-contiguous regions in one pass, so `PostFmergeBib.bat` + `PostRomImage.bat` run `romimage` twice with disjoint DLL ranges.
- **`FSRAMPERCENT=08080808` (3.1%) & `COMPRESSION=OFF`:** Essential in both CE 6.0 and CE 5.0.
- **No kernel/user driver split in CE 5.0:** CE 6.0's `k.touch.dll` / `K` flag model disappeared in CE 5.0; drivers run in `gwes.exe` / `device.exe` as standard CE 5.0 DLLs.
- **Hardware lessons carried forward directly:** Pre-swapped MMIO, variable-tick TMU0, INTC `+4` IPR stride, KEYSC/touch shared polling tick, DCS panel `SYNCO` ordering, backlight dimming before `DISPOFF`, R-standby `PLLCR` + `SPC`/`SSR` preservation, and `DEVICE=1` IRQ-pin guarding.
