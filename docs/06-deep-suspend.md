# Deep Suspend: SH7305 R-Standby on the fx-CP400

Windows CE suspend on the fx-CP400 uses the SH7305's **R-standby** low-power state (the same sleep mode Casio's stock OS uses when powered off), rather than ordinary CPU `sleep` (which keeps PLLs, peripheral clocks, and active SDRAM running).

## 1. What Survives vs. What Is Lost in R-Standby

| Domain | Across R-Standby |
|---|---|
| **Retained** | On-chip **RS memory** (`0xFD800000`+), external **SDRAM** (in self-refresh), **RCLK domain** (RTC, KEYSC autonomous scan, RWDT, CMT), I/O pin states, and selected backup registers (BSC/CPG/PFC). |
| **Lost (Core Reset)** | CPU general & banked registers, **`SPC`/`SSR`**, `VBR`, L1 I/D caches, UTLB/ITLB, `CCR`/`MMUCR`/`PTEH`/`TTB`, **on-chip IL memory (`0xE5200000`) and X/Y RAM (`0xE5007000`/`0xE5017000`)**, TMU, IIC0, USB, and active KEYSC matrix config. |

On wake (Clear/ON key via KEYSC autonomous scan, or hardware RTC alarm), the core resets without asserting `RESETOUT`, re-locks its PLLs, and jumps to the physical address in **`BAR` (`0xA4150040`)** in native big-endian mode with `SR = reset` (`BL=1, MD=1, RB=1, IMASK=15`). Because the guest OS is little-endian translated code, the machine-level standby/resume path lives inside the native translator (`dbt/rt/standby.S`, `dbt/rt/rt.c`), invoked by the OAL through `HC_STANDBY`.

## 2. RS Memory Layout

Casio's stock OS uses `0xFD800000`–`0xFD8003FF` and a stack below `0xFD802000`. To avoid disturbing Casio's cold-boot state, the translator uses **`0xFD801000`–`0xFD8013FF`**:
- `0xFD801000`: Save block (magic, checksum, saved `ctx`, `SPC`, `SSR`, `CCR`, `MMUCR`, `PTEH`, `TTB`, translator `VBR`/`GBR`/`SP`, and saved PFC/PLLCR registers).
- `0xFD801200`: Position-independent native resume stub pointed to by `BAR`.

## 3. Suspend & Resume Sequence

### OAL Side (`bsp/classpad50/SRC/OAL/OALLIB/power.c`)
1. Kernel calls `OEMCacheRangeFlush(0, 0, CACHE_SYNC_DISCARD | CACHE_SYNC_INSTRUCTIONS)` before `OEMPowerOff`.
2. Drivers power down peripherals:
   - **Display (`classpad_ddi.cpp`):** Sets backlight register (`0x51` on newer panels, `0xB9` on older panels) to `0`, waits ~62.5 ms (`8` ticks of 128 Hz `R64CNT`), sends `DISPOFF`, waits ~62.5 ms, then sends `SLPIN`. *(Dimming before `DISPOFF` prevents the normally-white panel from flashing white.)*
   - **Touch / Keyboard:** Powers down IIC0 sensor and saves KEYSC state.
3. OAL saves INTC/KEYSC/PFC state, configures wake sources:
   - **KEYSC autonomous scan:** `KIUMODEREG = 0x0200` limited to the Clear/ON row/column, with normal KEYSC interrupt priority set to `0`.
   - **RTC alarm:** Enabled via `OEMSetAlarmTime` (`rtc.c`, `INTEVT 0xA80`, IRQ 49) if a kernel alarm is scheduled.
4. OAL calls **`HC_STANDBY`** in a loop until the wake reason confirms Clear/ON or RTC alarm (spurious wakes re-enter standby silently). Wait loops after wake count **`R64CNT`** instead of `OALStall`, because TMU0 is reset by R-standby.
5. On return, the kernel calls `InitClock` (re-programming TMU0), re-reads the RTC, and driver `PowerHandler(FALSE)` paths wake the panel (`SLPOUT`, 120 ms wait, restore saved backlight level, full-screen repaint).

### Translator Side (`dbt/rt/standby.S`, `dbt/rt/rt.c`)

**Entry:**
1. Copies X/Y RAM (`g_xyram_save`) to SDRAM (IL memory stubs are reloaded from SDRAM on resume via `dbt_onchip_load()`).
2. Saves translator + guest core registers — including live **`SPC` and `SSR`** — into the RS save block at `0xFD801000`, and copies the resume stub to `0xFD801200` (`BAR = 0xFD801200`).
3. Writes back and disables L1 caches (`CCR = ICI|OCI`, then `CCR = 0`), flushes and disables TLB (`MMUCR = TI`).
4. Executes true **SDRAM self-refresh** from **RS memory** (`0xFD80xxxx`) — unlike Casio OS `0x8002095E`, which turns off refresh/clock (`SDCR = 0`, `PLLCR |= 2`, `PULCRBSC`, `PH6`) because Casio cold-restarts on wake:
   ```
   SDCR (0xFEC10044) = 0x0C09   ; clear PDOWN (bit 9), set RMODE (bit 10) -> true self-refresh
   STBCR (0xA4150020) = 0x21    ; RSTBY=1 (+ OS bit 0)
   SR = 0x70000000              ; MD=1, RB=1, BL=1, IMASK=0 (allows RTC wake)
   INTEVT = 0
   sleep
   ```
   *(On wake, `standby.S` checks wake reasons — `RCR1.AF`, `KIUDATA0` Clear bit, `KIUINTREG` — inside RS memory before waking SDRAM, and re-enters `sleep` silently on spurious wakes.)*

**Resume (at `BAR = 0xFD801200`, running from RS memory with RS stack):**
1. Switches to bank 0 (`SR = 0x700000F0`), clears `STBCR.RSTBY`, resets `BAR = 0xA0000000`, resets watchdog `RWTCSR = 0xA507`.
2. Checks wake reasons in RS memory; if genuine, waits >2 ticks of `R64CNT` (~47 ms for PLL/SDRAM settle) and restores `SDCR = 0x0A09` (exiting self-refresh; BSC preserves `SDCR = 0x0C09` across R-standby).
3. Validates save block magic/checksum, restores caches (`CCR`), `MMUCR` (TLB flush + enable), `TTB`/`PTEH`, translator `VBR`/`GBR`/`SP`, and reloads on-chip IL memory and X/Y RAM (`SYNCO` + `ICBI`).
4. Restores saved **`SPC` and `SSR`** and returns `RT_ACT_RESUME` to the OAL with SDRAM and the entire translated code cache intact.

**Full hardware power-off (`CLASSPAD_SUSPEND_OPT_DEFAULT = 0x0F`):** On real hardware, the OAL and drivers also power down unused pins (Casio pin table minus `PULCRBSC`/`PH6`), panel deep standby + `PHDR` bit 5 / `0xA44C0020` bit 2 with full cold-init replay (`classpad_pwrseq.h`), ADC, and the IIC0 touch sensor (`PHDR` bit 4 / `0xA405019E` bit 5) with Casio's full startup replay on resume. `wince.hh3` calls `gint_world_switch()` before `takeover.S` so `gint` restores Casio's hardware state before CE takes over.

## 4. Real-Hardware Lessons & Diagnostics

Three critical hardware bugs were caught on the physical fx-CP400 (none of which appeared in `casio-emu`, because the emulator does not model SDRAM refresh loss or core register clearing across R-standby):

1. **SDRAM self-refresh register mix-up:** Early code wrote `0xFEC10044 |= 2` instead of `PLLCR (0xA4150024) |= 2`, causing SDRAM to lose contents while powered down and reset into Casio OS on wake.
2. **Lost `SPC`/`SSR`:** `stubs.S` returns from `HC_STANDBY` via `rte` using live `SPC`/`SSR`, which R-standby resets. Saving and restoring `SPC`/`SSR` across `dbt_standby_sleep` fixed the post-wake reset.
3. **White screen on suspend:** The fx-CP400 panel is normally-white and controlled via DCS brightness (`0x51`/`0xB9`), not `SLPIN`. Zeroing brightness 60 ms before `DISPOFF`/`SLPIN` makes the screen turn cleanly black.

**Diagnostic build:** `make -C dbt DEVICE=1 STANDBY_DIAG=1 hhk` paints 8 color-coded horizontal stripes across the screen during resume (BAR reached → magic intact → SDRAM re-init → MMU/caches restored → C code running → IL/XYRAM verified → stub return → first post-wake exception) to pinpoint any board-level resume failure. `make -C dbt GUEST=standby run` verifies the full flow in `casio-emu` (requires `dbt/emu-patch/add_sdmr.py`).
