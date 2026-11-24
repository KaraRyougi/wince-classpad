# Windows CE 5.0 Port & Real-Hardware Bring-Up (`bsp/classpad50/`)

Windows CE 5.0 is the active OS target for `wince-classpad`, built in Platform Builder 5.0 under `bsp/classpad50/`. This document summarizes the CE 5.0 BSP architecture, drivers, applications, Pocket PC SH-3 compatibility layer, and real fx-CP400 hardware lessons.

---

## 1. Kernel & Image Architecture

### Kernel Differences vs. CE 6.0
- **Combined `nk.exe`:** CE 5.0 links kernel + OAL into a single `nk.exe` (no `kernel.dll` or `OEMGLOBAL`). `dbt/rt/nkbin.c` detects this automatically and applies `kdev_funcs50` (`dbt/rt/kernel_dev50.h`) to `nk.exe`'s text.
- **No `WaitOneMore` bug:** The CE 6.0 timed-wait interrupt-event proxy leak does not exist in CE 5.0 (`schedule.c`).
- **SH-3 PE loader patch (`bsp/kernel/ce50-loader-sh3-images.patch`):** Allows loading SH-3 (`0x1A2`/`0x1A3`) file-based executables (ROM `MODULES` with flag `P` already skip the CPU check).
- **Kernel-mode drivers (`ROMFLAGS=00`):** Keeps GWES and drivers in kernel mode so fixed P2 MMIO pointers work directly.
- **OAL common layer (`oalcommon.c`):** Because PB 5.0 omits `PLATFORM\COMMON` libraries for SH-4, `oalcommon.c` provides SYSINTR mapping, `OEMIoControl` dispatch, standard HAL IOCTLs, and QPC/`SC_GetTickCount`.

### Two-Pass XIP Image Build (`NK.bin` + `EXT.bin`)
CE 5.0's `romimage` cannot lay out two non-contiguous XIP regions in a single pass. Our build hooks solve this cleanly:
1. `PostFmergeBib.bat` strips unused catalog baggage (`romexclude.txt`: `netbios.dll`, `serial.dll`, stock `usbmsfn.dll`, `.wav`, `classpad_rotate.exe`), moves `extmove.txt` components (WordPad, Solitaire/FreeCell, TCP/IP/PPP/Telnet stack) into `EXT` ahead of SH-3 modules (so `romimage` keeps EXT's CPU type as SH-4), and splits `EXT`-region entries into `ext.modules.bib` / `ext.files.bib`.
2. `romimage` builds **`NK.bin`** (`0x80EE0000`, 5 MiB persistent NOR hole, ~4.8 MiB) with DLL addresses in `0x01F00000`–`0x02000000` (data) / `0x02400000`–`0x04000000` (code).
3. `PostRomImage.bat` combines `ext.bib.hdr` with the split lists and invokes `romimage` a second time to build **`EXT.bin`** (`0x83860000`, 7.625 MiB eActivity area) with disjoint DLL ranges (`0x01400000`–`0x01F00000` / `0x02100000`–`0x02400000`), then moves `EXT.bin` back into the release directory.

`NK.bin` alone is a complete, bootable system (OS, shell, drivers, Plotter-Z, CEtune, PegHelp, ITaskMgr, SH3CRT, DocList). `EXT.bin` holds Math Xpander, `mfcce300.dll`, and SpreadCE; if Casio's OS reformats the eActivity area, `OEMInit` cleanly skips chaining `EXT` and CE still boots normally.

---

## 2. Drivers (`bsp/classpad50/SRC/DRIVERS/`)

### Display (`DISPLAY/classpad_ddi.cpp`)
- **Panel auto-detection (`classpad_lcd.h`):** Reads panel ID via DCS `0xDA` (`LcdReadId`). Newer panels (`0x16`/`0x52`) start at GRAM column `0`; older fx-CP400 panels add `m_col0 = 40` to every `CASET` window.
- **Backlight:** Uses DCS `0x51` (newer panels) or `0xB9` (older panels) with a 20-step brightness table (`classpad_bkl.h`), exposed via `DrvEscape` to `Control Panel → Display → Backlight → Advanced...` (`SRC/APPS/BKLCPL`).
- **4-way rotation (`classpad_lcdrot.h`):** Implements `DRVESC_GETSCREENROTATION` (matching CE 5.0 GWES's `cjOut = 0` convention) and `DRVESC_SETSCREENROTATION`. Relays out the logical surface unrotated and transposes during `PushRect` / `HC_LCD_PUSH` to avoid GPE's buggy rotated-blit paths. `classpad_rotatekey.exe` automatically refits off-screen top-level windows and realigns command bar adornments (`CommandBar_AlignAdornments`) after rotation.
- **Fast blits & dirty tracking:** Implements `SolidFill16`, `FastCopyBlt16`, and inline RGB555→RGB565 `ConvertedCopyBlt16`; tracks 2D dirty rectangles (`DirtyRect`) and uses P1 cached frame addresses (`m_frameP1Safe`) when VA and PA share SH7305 8 KiB cache colour, avoiding unnecessary `CacheRangeFlush` calls.
- **PushThread priority (`250`):** Runs at priority `250` (`THREAD_PRIORITY_ABOVE_NORMAL`) — above normal apps (`251`) and below driver ISTs — so CPU-bound busy loops such as Explorer's `ListView_DragSelect` marquee selection (`PeekMessageW` loop) and Plotter-Z's `RecalcCartesian` progress bar still push frames to the LCD at up to 20 Hz (`DISP_PUSH_MS = 50`).

### Input (`KEYBD/`, `TOUCH/`)
- **Shared polling tick:** `kbdmouse.dll` runs a single 8 ms polling tick (`KeyscTick`) that scans the KEYSC matrix (skipping byte-swaps when raw `KIUDATA` words are unchanged) and invokes `touch.dll`'s `InputTick`, avoiding redundant wakeups.
- **Touch (`classpad_touch.cpp`):** Reads the fx-CP400 IIC0 touch controller (`0x40`, command `0x84`) when `PRDR` bit 5 indicates contact, maps the bottom 7 faceplate icons to named events (`Menu`, `Main`, `Settings`, `Esc`, `Rotate`, `Swap`, `Keyboard`), and preserves Casio OS's initial sensor setup across suspend/resume.
- **Keyboard (`kbd_pdd.cpp`):** Full fx-CP400 key layout (`Matrix = 1`), mapping `= . , - /` directly and expanding `+ * ( ) ^` via pseudo-VKs for Math Xpander compatibility.

### Storage (`FUGUEDSK/`, `APPS/STORCPL/`)
- Mounts Casio's 24.1 MiB Fugue FAT16 storage flash (`0x1A20000`–`0x385FFFF`) read/write as `\Storage` (`DSK1:`), including log-structured wear-leveling, ECC, and sector reclamation.
- `Control Panel → Fugue File System` (`classpad_storage.cpl`) displays free sectors, free FAT space, reclaimable sectors, and provides one-click **Optimize**.

### USB Function (`USBFN/sh7305_usbfn.cpp`, `APPS/USBCPL/`, `APPS/USBNET/`)
- SH7305 USB peripheral controller driver supporting:
  - **USB Mass Storage** (exposes `\Storage` to a host PC)
  - **USB Serial (CDC ACM)**
  - **USB Network** (PPP over serial + Telnet server)
- Switchable via `Control Panel → USB Connection` and `Start → Programs → USB Network`.

### Battery & Power (`BATTDRVR/`, `APPS/BATTERY/`)
- Reads battery voltage via ADC (`PollInterval = 30 s`) and exposes status to CE's Power Control Panel and `Start → Programs → Battery`.
- Deep suspend via SH7305 R-standby ([06-deep-suspend.md](06-deep-suspend.md)); `wince.hh3` calls `gint_world_switch()` before `takeover.S` so Casio OS's hardware/sensor state is restored before handing off to CE (fixing a post-suspend black screen introduced by running takeover directly from `gint`'s world).

### Audio
- Intentionally omitted: no `waveapi.dll`, no audio driver, no `.wav` files, and `platform.reg` explicitly zeroes GWES key/screen click settings so zero CPU/RAM is spent on sound.

---

## 3. Applications & Pocket PC SH-3 Compatibility (`bsp/classpad50/SRC/APPS/`)

| Application | Location | Notes |
|---|---|---|
| **Math Xpander** *(3rd-party)* | `EXT.bin` (`FILES/xpander/`) | 2001 HP Pocket PC SH-3 CAS/graphing suite. Realigned to 4 KiB with reconstructed relocations (`tools/pe4k.py`), patched for 320×528 full-screen canvases, and linked to `SH3CRT.dll`, `mfcce300.dll`, and `DocList.dll`. |
| **SpreadCE** *(3rd-party)* | `EXT.bin` (`FILES/spreadce/`) | Pocket PC SH-3 spreadsheet prepared via `tools/sh3prep.py` (25,556 reconstructed `.pdata` fixups + `SH3CRT` IAT redirection in `.data`). |
| **Plotter-Z** | `SRC/APPS/PLOTTERZ/` | Open-source 3D function plotter (fixed-point rendering, partial-rect progress bar updates). |
| **CEtune** | `SRC/APPS/CETUNE/` | Xtune-inspired SH7305 clock tuner (FLL/PLL/dividers + CS0/CS3 waits via OAL IOCTLs, 10 s safe revert, presets saved to `\Storage\cetune.sav`, automatic `OALTimerRetune`). |
| **ITaskMgr** | `SRC/APPS/ITASKMGR/` | Process/window manager and CPU/memory graph adapted to read `GetIdleTime()` so `OEMIdle` still sleeps. Triggered from `Programs → Task Manager` or the faceplate `Swap` icon. |
| **PegHelp** | `SRC/APPS/PEGHELP/` | Lightweight GDI HTML help viewer (no IE dependency) for Math Xpander help pages (`test_peghelp` checks all 217 links). |
| **SH3CRT & DocList** | `SRC/APPS/SH3CRT/`, `DOCLIST/` | Integer-fast-path SH-3 floating-point CRT shim (`sh3softfloat.h`, verified by `test_sh3crt`) and Pocket PC `DocList_Create` ordinal-50 implementation. |

---

## 4. Notable Hardware Quirks Solved

1. **`DRVESC_GETSCREENROTATION` size check:** CE 5.0 GWES calls it with `cjOut = 0`; removing CE 6.0's `cjOut >= 4` check fixed rotation returning `DISP_CHANGE_BADMODE`.
2. **Older vs. newer DCS panels:** Panels whose `RDID` (`0xDA`) is neither `0x16` nor `0x52` require `+40` column offset across every `CASET` site (DDI, `HC_LCD_PUSH`, `bootmarquee`, `lcdcon`).
3. **Display `PushThread` priority (`250` vs `252`):** Lowering `PushThread` to `252` (below `THREAD_PRIORITY_NORMAL = 251`) starved screen refreshes during Explorer's `ListView_DragSelect` marquee spin loop and Plotter-Z's recalculation loop; restoring `250` fixed both.
4. **Boot marquee requiring `DEVICE=1`:** Building `wince.hh3` without `DEVICE=1` omits `-DDBT_BOOT_MARQUEE`, `-DDBT_NO_DEBUG_PORT`, and `-DDBT_IRQPIN_GUARD`, freezing the boot progress bar right after loader handover. Always build `wince.hh3` with `make -C dbt DEVICE=1 BUILD=build-dev hhk`.
5. **GWES animations (`platform.reg`):** Disabling `HKLM\SYSTEM\GWE\Animate` and `Menu\AniType` avoids unnecessary intermediate repaints during window/menu open and close.
