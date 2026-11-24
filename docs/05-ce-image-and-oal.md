# Windows CE Image Layout & SH7305 OAL

This document describes how the translator loads Windows CE images, how the 16 MiB SDRAM is partitioned, and how the SH7305 OEM Adaptation Layer (OAL) maps Windows CE primitives onto ClassPad II hardware.

## 1. Memory Budget (16 MiB SDRAM + 64 MiB NOR)

All addresses are defined centrally in [`dbt/Makefile`](../dbt/Makefile) and propagated to C code, linker scripts, and the loader:

| Region | Address Range | Size | Purpose |
|---|---|---|---|
| **NOR — `NK` (XIP)** | `0x80EE0000`–`0x813DFFFF` | 5.0 MiB | Windows CE 5.0 core OS (kernel, OAL, GWES, shell, drivers, Plotter-Z, CEtune, PegHelp, SH3CRT, DocList) |
| **NOR — `EXT` (XIP)** | `0x83860000`–`0x83FFFFFF` | 7.625 MiB | Optional additional XIP region (Math Xpander + `mfcce300.dll`), chained via `OEMRomChain` |
| **SDRAM — Guest CE** | `0x8C000000`–`0x8C7FFFFF` | **8.0 MiB** | Windows CE RAM (first page: `BOOTARGS`; last page `0x8C7FF000`: BL=1-safe debug ring) |
| **SDRAM — Translator** | `0x8C800000`–`0x8CBFFFFF` | **4.0 MiB** | Translator code/data/BSS (28,800 block headers, 1,280 KiB metadata arena, 65,536-entry lookup hash) + 128 KiB C stack |
| **SDRAM — Code Cache** | `0x8CC00000`–`0x8CFFFFFF` | **4.0 MiB** | 16-bank translated host code cache |

Key `config.bib` choices (`bsp/classpad50/FILES/config.bib`):
- **XIP execution (`NK 80EE0000 00500000 RAMIMAGE`):** Code executes directly out of NOR flash through P1; only `.data` copy entries, heaps, and stacks consume the 8 MiB CE RAM window.
- **`FSRAMPERCENT=08080808` (3.1%):** Keeps the RAM object store / registry around ~256 KiB instead of CE's default 50% (which starved GWES and display drivers).
- **`COMPRESSION=OFF`:** Avoids running CE's software decompressor under the translator on every demand-paged module load.
- **No KITL / Debugger / Ship build:** `IMGNOKITL=1`, `IMGNODEBUGGER=1`; retail build keeps `RETAILMSG` enabled so OAL/driver sign-on strings still drive `bootmarquee` hand-off.

## 2. Image Loading (`dbt/rt/nkbin.c`, `dbt/rt/main.c`)

At startup, `dbt_main()` locates the guest in order of priority:
1. **XIP NOR image at `NK_FLASH` (`0x80EE0000`):** Validates the `"ECEC"` ROMHDR marker at `NK_FLASH + 0x40`, reads the module table to locate `nk.exe` (CE 5.0) or `kernel.dll` (CE 6.0) for selective device-aware (`kdev`) translation, registers the `EXT` ROM range (`rom2_lo/hi`) if present so code-cache flushes retain XIP ROM blocks, and jumps to `StartUp`.
2. **RAMIMAGE `B000FF` in flash or embedded blob:** Copies records into guest SDRAM and jumps to the terminator entry point (used by `nkimg` self-test).
3. **Raw little-endian binary at `0x8C100000`:** Used by standalone translator test guests (`hello`, `fpu`, `tmu`, etc.).

## 3. SH7305 OAL (`bsp/classpad50/SRC/OAL/OALLIB/`)

The reference SH-4 OAL targets the SH7750 (`0xFFDxxxxx` peripherals), whereas the SH7305 uses an SH7724-style peripheral map at `0xA4xxxxxx`. Because the translator byte-swaps all 16- and 32-bit guest memory accesses, **all OAL and driver MMIO uses pre-swapped accessors** (`classpad_mmio.h`, built on inline `swap.b`/`swap.w`), *except* P4 CCN registers (`0xFFxxxxxx`) inside `kdev`-classified kernel functions which run unswapped (`P4_R32`/`P4_W32`).

| Component | File | SH7305 Implementation |
|---|---|---|
| **Startup** | `startup.src` | Initializes SR/VBR, clears `MMUCR`, flushes TLB, clears BSS, enters `KernelStart`. |
| **System Timer** | `timer.c` | TMU0 (`0xA4490004`–`0xA4490010`, `Pphi/4`, `INTEVT 0x400`, `IMR4` bit 4). Implements **variable tick** (`OALTimerUpdateRescheduleTime`, up to `TICK_MAX_MSEC = 1000` ms) to minimize timer-interrupt translator round-trips. Uses reciprocal 32×32→64 multiply in `OALTimerCountsToMSec` (avoiding software division) and skips reprogramming TMU0 when the hardware reload already matches a full period. |
| **Interrupts (INTC)** | `intr.c` | Maps SH7305 `IPRA`–`IPRL` (`0xA4080000+4n`) and `IMR`/`IMCR` (`0xA4080080+4n`/`0xA40800C0+4n`) via an O(1) IRQ lookup table. Maps vectors beyond CE's 64-entry table (KEYSC `INTEVT 0xBE0` → IRQ 48, RTC alarm `INTEVT 0xA80` → IRQ 49). Masks sources in ISR and unmasks in `OEMInterruptDone`. |
| **Cache & TLB** | `cache.c`, `cacheops.src` | Executes native `ocbwb`/`ocbp` (or address-array walk at `0xF4000000` for large ranges) and issues hypercalls `HC_FLUSH` (`trapa #0xF2`) on instruction sync and `HC_TLB_INVAL` (`trapa #0xF9`) on range TLB invalidations. Skips redundant hardware I-cache invalidation under the translator since guest code never executes from hardware I-cache. |
| **Debug Output** | `debug.c` | Batches `OEMWriteDebugString` over `HC_PUTS` when `SR.BL = 0`; falls back to the RAM debug ring (`0xAC7FF000`) during early boot (`SHInit`/`OEMInit`, when `SR.BL = 1` and `trapa` would reset the CPU). |
| **RTC & Alarm** | `rtc.c` | Reads/writes SH7305 BCD RTC (`0xA413FEC0+`) and programs the hardware alarm (`INTEVT 0xA80`) as a wake source for R-standby. |
| **Power & Suspend** | `power.c` | `OEMIdle` executes `sleep`. `OEMPowerOff` invokes `HC_STANDBY` for SH7305 R-standby (with CPU-sleep fallback); see [06-deep-suspend.md](06-deep-suspend.md). |
| **EXT ROM Chain** | `oalcommon.c`, `classpad_extrom.h` | Validates the `EXT` window at `0x83860000` (`ECEC`, ROMHDR, module/file bounds) and exposes it via `OEMRomChain` so `\Windows` transparently includes `EXT.bin` modules and files. |
| **Clock / IOCTLs** | `ioctl.c`, `classpad_clock.h` | Standard HAL IOCTLs plus custom ClassPad IOCTLs for CPG clock tuning (CEtune), panel ID/type reporting, and power statistics. |

## 4. Translator Hypercall Contract (`classpad_hc.h`)

The OAL and display driver communicate with the bare-metal translator through `trapa` hypercalls:

| Trap | Hypercall | Purpose |
|---|---|---|
| `trapa #0xF2` | `HC_FLUSH` | Invalidate translated blocks for a modified/reloaded code range |
| `trapa #0xF3` | `HC_PUTC` / `HC_PUTS` | Debug character / batched string output |
| `trapa #0xF9` | `HC_TLB_INVAL` | Notify translator of a range TLB invalidation |
| `trapa #0xFA` | `HC_ASID` | Optional explicit ASID notification (also tracked automatically via `PTEH` writes) |
| `HC_LCD_PUSH` / `HC_LCD_BLIT` | Panel transfer | Native byte-swapped + rotated DMAC0 pixel push to the DCS panel port (`0xB4000000`) |
| `HC_LCD_OP` | Panel bus op | Atomic `SYNCO`-ordered DCS command/parameter/read transactions |
| `HC_STANDBY` | Deep suspend | Native SH7305 R-standby entry and resume from RS memory |
| `HC_GETSTAT` | Profiling | Read translator counters (translations, blocks, invalidations, IC misses, ASID switches) |
