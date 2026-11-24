# Boot Flow, Flash Map & Image Formats

This document covers the fx-CP400 NOR flash layout, how Windows CE 5.0 images (`NK.bin` and `EXT.bin`) are packaged and flashed, and how `wince.hh3` hands control over to the dynamic binary translator.

## 1. fx-CP400 64 MiB NOR Flash Map

The fx-CP400 uses a 64 MiB linear x16 NOR flash (`P1 0x80000000`–`0x83FFFFFF`, `P2 0xA0000000`–`0xA3FFFFFF`, 512 × 128 KiB sectors). Under Casio OS 02.01.7003, the chip is partitioned as follows:

| Chip Offset | P1 Address | Size | Casio OS Use | Windows CE Use |
|---|---|---|---|---|
| `0x0000000`–`0x0EDFFFF` | `0x80000000`–`0x80EDFFFF` | 14.875 MiB | Casio OS code & data (HollyHock patches sit below `0x920000`) | Untouched |
| **`0x0EE0000`–`0x13DFFFF`** | **`0x80EE0000`–`0x813DFFFF`** | **5.0 MiB** | **Unused erased hole inside OS image** | **`NK.bin` (XIP Core OS — persistent)** |
| `0x13E0000`–`0x14FFFFF` | `0x813E0000`–`0x814FFFFF` | 1.125 MiB | Casio OS tail data, region tag (`LY777D`), OS build date | Untouched |
| `0x1500000`–`0x1A1FFFF` | `0x81500000`–`0x81A1FFFF` | 5.125 MiB | MCS main-memory backup | Untouched |
| `0x1A20000`–`0x385FFFF` | `0x81A20000`–`0x8385FFFF` | 30.25 MiB | Storage memory (`\fls0`, Fugue FAT16, 24.1 MiB logical) | Mounted read/write as `\Storage` (`FUGUEDSK`) |
| **`0x3860000`–`0x3FFFFFF`** | **`0x83860000`–`0x83FFFFFF`** | **7.625 MiB** | **eActivity volume (Fugue FAT12, 5.5 MiB logical)** | **`EXT.bin` (XIP extra apps — Math Xpander, MFC)** |

### Persistence Rules

- **`NK.bin` in the 5 MiB hole (`0x80EE0000`):** Casio OS 02.01.7003 never writes to this hole during normal operation and does not verify the whole-OS checksum at boot (only the factory diagnostics menu checks it). `NK.bin` holds the complete CE 5.0 kernel, GWES, shell, drivers, Plotter-Z, CEtune, PegHelp, SH3CRT, and DocList — so CE boots cleanly even without `EXT.bin`. Only a Casio OS firmware update erases this window.
- **`EXT.bin` in the eActivity area (`0x83860000`):** Whenever Casio's stock OS boots after `EXT.bin` was written, Casio's Fugue driver silently reformats the 61 eActivity sectors. Because `EXT.bin` stays stored as a file in `\fls0\`, `wince.hh3` detects the mismatch and offers to re-flash `EXT.bin` before booting CE (or boot CE directly without Math Xpander).
- **Safety check before erasing:** `hhk/nkflash.c` refuses to erase any sector unless the window is blank (`0xFF`), already holds a CE image (`ECEC` ROMHDR at `+0x40` or `B000FF` header), or (for the `EXT` window only) holds a Casio Fugue eActivity header.

## 2. Image Formats & Compression (`.cez`)

Platform Builder's `romimage` outputs standard Windows CE `B000FF` `.bin` files (`NK.bin` and `EXT.bin`): a list of `(address, length, checksum, data)` records followed by an entry-point terminator, with a ROMHDR marked by `"ECEC"` at `image_start + 0x40`.

To cut USB transfer time and `\fls0\` storage usage roughly in half (~6 MB instead of ~12 MB), images can be packed with `tools/cepack.py`:

```sh
python3 tools/cepack.py -o dbt/build/cez /path/to/NK.bin /path/to/EXT.bin
```

`cepack.py` wraps the image in the **`CEZ2`** container (independent 32 KiB raw deflate blocks plus an embedded `B000FF` record table with per-record CRC-32s and a whole-file CRC-32) under the same `NK.bin`/`EXT.bin` filenames. `wince.hh3` (`hhk/cez.c`, `hhk/nkflash.c`) checks flash equivalence via CRC-32 without decompressing, streams both raw `B000FF` and `CEZ2` transparently during flash/verify, and erases sectors **last-to-first** (preserving the first-sector `ECEC` marker if interrupted mid-erase). Verify packed images on the host with `dbt/build/ceztest`.

## 3. Real-Device Boot (`wince.hh3`)

`wince.hh3` (`dbt/hhk/`, built via `make -C dbt DEVICE=1 BUILD=build-dev hhk`) is a `gint` HollyHock 3 ELF add-in:

- **Memory placement:** Loader code executes in gint's VRAM backup area (`0x8C052800`); the big-endian translator binary (`dbt-hello.bin`) is linked as a separate loadable section pinned at `0x8C900000` (`HHK_PAYLOAD`).
- **UI (`hhk/ui.c`):** Windows XP Luna-style menu offering four actions:
  1. Flash Core OS (`NK.bin`)
  2. Flash Additional Applications (`EXT.bin`)
  3. Flash Both
  4. Boot Windows CE Directly
  
  While the menu is open, the loader incrementally compares `\fls0\NK.bin` and `\fls0\EXT.bin` against flash in 64 KiB slices (keeping keys and touch responsive) and skips writing any region that already matches.
- **NOR flashing (`hhk/norflash.c`):** Programs XIP records directly to `P1_addr - 0x80000000` through P2 (`0xA0000000+`) using the AMD/Spansion unlock sequence (`0xAAA`/`0x554`), 128 KiB sector erase, 32-word write buffer, and read-back verification, with interrupts masked during each flash command.
- **Takeover (`hhk/takeover.S`):** Once the user starts CE, the loader fades in the Windows CE 5.0 boot screen, masks interrupts (`IMASK=15, RB=0`), quiets all Casio OS interrupt sources (TMU, INTC, IRQ pins, RTC), disables and flushes the MMU/TLB, copies the translator payload from `0x8C900000` to `IMAGE_BASE` (`0x8C800000`), syncs caches, and jumps to `crt0.S`.
- **Boot marquee (`dbt/rt/bootmarquee.c`):** In `DEVICE=1` builds (`MARQUEE=1`, default), the translator animates the three blue progress blocks on every TMU0 tick (timed via the 128 Hz RTC `R64CNT`) until the CE display driver prints `"CLASSPAD DDI: "` on startup.
- **Diagnostic build flags (off by default):**
  - `CONSOLE=1`: enables an on-screen 96-row debug console (`rt/lcdcon.c`).
  - `FLASHLOG=1`: logs boot output to the last sector of the NK window (`rt/nor.c`), saved to `\fls0\WINCE.LOG` on the next `wince.hh3` launch.

## 4. Emulator Boot (`casio-emu`)

In `casio-emu`, the add-in window is limited to 2 MiB, so the translator travels in `dbt/build/dbt-hello.g3a` (`dbt/loader/loader.S` relocates it to `0x8C800000`), while `tools/nkflash.py` flattens `NK.bin`, `EXT.bin`, and an optional Casio Fugue storage image (`--fugue`) into a single NOR blob loaded via `HLE_OSROM` / `HLE_OSROM_PAD`:

```sh
make -C dbt NKBIN=/path/to/NK.bin EXTBIN=/path/to/EXT.bin RUN_SECS=300 run
```
