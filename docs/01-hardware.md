# Hardware Reference: Casio fx-CP400 (SH7305)

Hardware specifications used throughout the translator (`dbt/`), loader (`dbt/hhk/`), and Windows CE BSP (`bsp/classpad50/`). Primary sources include the Renesas SH7724 hardware manual, `gint`, `casio-emu`, and disassembly of Casio OS 02.01.7003.

## Processor (Renesas SH7305)

- **Core:** SH-4A (`SH4AL-DSP` variant), **no hardware FPU**, DSP extension with X/Y RAM.
- **Endianness:** Hardwired **big-endian** via reset mode pin.
- **Clock:** ~118 MHz stock on fx-CP400 (58 MHz stock on fx-CG50); tunable via the CPG (`0xA4150000`, exposed to CE via CEtune).
- **Caches:**
  - L1 Instruction Cache: 32 KiB, 4-way set-associative, 32-byte lines (`ICBI` / address array at `0xF0000000`).
  - L1 Operand (Data) Cache: 32 KiB, 4-way set-associative, 32-byte lines, 8 KiB per way (`OCBI` / `OCBP` / `OCBWB` / address array at `0xF4000000`).
- **MMU:** Software-filled SH-4 UTLB/ITLB in 29-bit address mode (`PTEH`/`PTEL`/`PTEA`/`TTB`/`MMUCR` at `0xFF000000`–`0xFF000034`).
- **On-chip memories:**
  - **IL memory:** 16 KiB at `0xE5200000` (zero-wait instruction SRAM — holds the translator's hot stubs and exception vectors).
  - **X/Y RAM:** 8 KiB each at `0xE5007000` / `0xE5017000` (holds hot translator state; lost in R-standby and restored by `dbt_standby`).
  - **RS memory:** At `0xFD800000` (retained across R-standby; Casio OS uses `0xFD800000`–`0xFD8003FF` and a stack below `0xFD802000`; translator save block and resume stub sit at `0xFD801000`–`0xFD8013FF`).

## Physical & Virtual Memory Map (fx-CP400)

| Virtual (P1 / P2) | Physical | Size | Description |
|---|---|---|---|
| `0x80000000` / `0xA0000000` | `0x00000000` | 64 MiB | Linear x16 NOR flash (XIP-capable, 128 KiB uniform sectors) |
| `0x8C000000` / `0xAC000000` | `0x0C000000` | 16 MiB | SDR-SDRAM (`ESMT M12L128168A`, 128 Mbit) |
| `0xA4000000`–`0xA4FFFFFF` | `0x04000000`+ | — | SH7724-style on-chip peripherals (uncached P2) |
| `0xB4000000` | `0x14000000` | — | LCD panel 16-bit data port (CS5A) |
| `0xFE008000` | — | — | DMAC0 registers (`SAR0` `0xFE008020`, `DAR0` `0xFE008024`, `TCR0` `0xFE008028`, `CHCR0` `0xFE00802C`, `DMAOR` `0xFE008060`) |
| `0xFF000000` | — | — | SH-4A CCN registers (`PTEH`, `PTEL`, `TTB`, `TEA`, `MMUCR`, `CCR`, `TRA`, `EXPEVT`, `INTEVT`) |

> Note: Early teardowns misidentified the 64 MiB Spansion flash (`S99...`) as 32 MiB NAND. It is a linear 16-bit XIP NOR flash with 512 × 128 KiB sectors and the standard AMD/Spansion command set (unlock `0xAAA`/`0x554`, 32-word write buffer). See [03-boot-and-images.md](03-boot-and-images.md) for the exact partition table.

## Key Peripherals

| Peripheral | Base Address | Notes |
|---|---|---|
| **CPG / Standby** | `0xA4150000` | `FRQCR`, `STBCR` (`0xA4150020`), `PLLCR` (`0xA4150024`), `MSTPCR0` (`0xA4150030`), `BAR` (`0xA4150040`) |
| **INTC** | `0xA4080000` | `IPRA`–`IPRL` at `+4n` (16-bit), `IMR0`–`IMR12` at `0xA4080080+4n`, `IMCR` at `0xA40800C0+4n` |
| **PFC / GPIO** | `0xA4050100` | Port R data `PRDR` at `0xA405013C` (bit 4 = LCD RS command/data select, bit 5 = touch contact) |
| **TMU** | `0xA4490000` | `TSTR` (`+0x04`), `TCOR0` (`+0x08`), `TCNT0` (`+0x0C`), `TCR0` (`+0x10`); `INTEVT 0x400`, clocked at `Pphi/4` |
| **RTC** | `0xA413FEC0` | `R64CNT` (128 Hz 7-bit counter at `0xA413FEC0`), BCD date/time, hardware alarm (`INTEVT 0xA80`) |
| **KEYSC** | `0xA44B0000` | Key matrix (`KIUDATA0`–`5`, `KIUMODEREG` `+0x10`, `KIUSTATEREG` `+0x12`), `INTEVT 0xBE0`, autonomous standby wake |
| **IIC0 (Touch)** | `0xA4470000` | Resistive touch controller at I²C address `0x40` (command `0x84` returns X/Y/Z1/Z2); contact line on `PRDR` bit 5 |
| **LCD Panel** | `0xB4000000` | 320×528 16-bpp RGB565 DCS panel (R61523-class); `PRDR` bit 4 selects command (`0`) vs data (`1`). Older panels (`RDID != 0x16/0x52`) start at GRAM column 40; newer panels start at column 0. Backlight controlled via DCS `0x51` (newer) or `0xB9` (older). |
| **USB Function** | `0xA4D80000` | High/Full-speed USB peripheral controller (`sh7305_usbfn.cpp`) |
| **RWDT** | `0xA4520000` | RCLK watchdog (`RWTCNT` `+0x00`, `RWTCSR` `+0x04`, keyed 16-bit writes `0x5Axx`/`0xA5xx`) |
| **SDRAM Ctrl** | `0xFEC10044` | Undocumented SH7305 SDR-SDRAM controller (`0xFEC10044`–`0xFEC10050`, mode reg `0xFEC15040`) |
| **Debug (emu only)** | `0xFFFFF000` | Byte-write host stdout port in `casio-emu`; disabled in `DEVICE=1` builds (`-DDBT_NO_DEBUG_PORT`) |
