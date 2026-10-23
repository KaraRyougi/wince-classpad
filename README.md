# wince-classpad

Windows Embedded CE 6.0 R3 on the Casio SH7305 calculators
(ClassPad II fx-CP400 / fx-CG500 first; fx-CG50 and fx-CG100 share the SoC).

The SoC is an SH-4A core (SH4AL-DSP) at about 118 MHz with **no FPU**, 16 MiB
of SDRAM at 0x8C000000 and 32 MiB of memory-mapped flash at 0x80000000 (XIP).
CE 6.0 R3 is the last release with SH4 support and ships the full kernel source,
but most of the OS is prebuilt SH4 binaries that contain FPU instructions, so a
trap-and-emulate FPU lives in the kernel's illegal-instruction path.

## Layout

| Path | What |
|---|---|
| `docs/` | Plan, hardware facts, FPU emulation design, boot/image notes |
| `fpuemu/` | Integer-only SH-4 FPU emulator core + host test harness (clang) |
| `tools/nkbin.py` | Parser/inspector for CE `.bin` images (B000FF) and ROMHDR |
| `loader/` | gint add-in that loads an NK.bin into RAM and jumps to it |
| `bsp/` | Platform Builder BSP (OAL, drivers, .bib/.reg files); needs PB 6.0 on Windows |

## Status

See [docs/00-plan.md](docs/00-plan.md). The little-endian→big-endian
translator (`dbt/`) runs its three test guests to completion inside casio-emu
(2026-10-22). No Windows CE code has been loaded yet.

## Host prerequisites

- macOS/Linux with clang or gcc and python3 for `fpuemu` tests and `tools/`.
- fxSDK + gint (`giteapc install Lephenixnoir/fxsdk Lephenixnoir/sh-elf-gcc ...`) for `loader/`.
- Windows (VM is fine) with Visual Studio 2005 + Platform Builder 6.0 R3 for `bsp/`.
- casio-emu (fx-CG100 model) for first bring-up before touching hardware.

```sh
make test          # host tests: FPU emulator + translator
make -C dbt run    # build launcher+translator+guest, run in casio-emu (GUEST=hello|exc|fpu|tmu|bench|lcd|apicall|mmu|aspace)
make -C dbt GUEST=lcd screenshot   # same, dumping the emulated LCD to dbt/build/dbt-lcd.png
make -C dbt NKBIN=path/NK.bin run  # run a Windows CE .bin image from emulated flash (docs/05)
make -C dbt GUEST=nkimg nkrun      # loader self-test with a synthetic CE-format image
python3 tools/nkbin.py path/to/NK.bin
```
