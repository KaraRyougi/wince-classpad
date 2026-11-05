# Boot and image notes

## CE image format

ROMIMAGE produces `NK.bin` (B000FF): image start/length, then records of
(address, length, checksum, data) and a terminator whose length field is the
entry point. `tools/nkbin.py` dumps it, locates the ROMHDR via the "ECEC"
signature at image start + 0x40 and prints the module table. `--flat` writes
the equivalent flat memory image (what a loader must place at `image start`).

## Where the image lives

- **RAM image** (the first images): all records are written to SDRAM. 16 MiB
  is enough for a headless kernel (about 2 MiB) but a GWES image plus the
  RAM file system ran out of pages (docs/05).
- **XIP image** (current, in the emulator and on the device): `ROMSTART` in
  `config.bib` points into the NOR at 0x81000000 (chip offset 16 MiB); code
  executes from flash, RAM holds data and the object store. casio-emu places
  the flat image with `HLE_OSROM` (`tools/nkflash.py`); on the device
  `nkflash.hhk` programs it (below). The window is 0x81000000-0x813DFFFF,
  3.875 MiB (`NK_FLASH`/`NK_FLASH_MAX` in `dbt/Makefile`, `ROMSIZE` in
  `config.bib`); the next section says why.

## The fx-CP400 flash map (2026-11-05)

The chip is **64 MiB** (P1 0x80000000-0x83FFFFFF, P2 0xA0000000-0xA3FFFFFF),
not the 32 MiB the earlier notes assumed: casio-emu's ClassPad profile models
it so, the OS writes beyond 32 MiB, and Casio's advertised capacities (24 MB
storage + 5.5 MB eActivity + a 21 MiB OS) only fit a 64 MiB part. The layout
below is what OS 02.01.7003 produces when it formats a blank chip in casio-emu
(`~/Downloads/nor-fx-cp400.bin`), compared with the pristine OS update image:

| chip offset | what |
|---|---|
| 0x0000000-0x14FFFFF | Casio's OS update image (21 MiB, written whole by the updater): code and data to 0x0EE0000; **erased 0x0EE0000-0x13E0000** (5 MiB); data 0x13E0000-0x14A0000; erased 0x14A0000-0x14E0000; 0x14E0000-0x14FFFFF end of image (`LY777D` region tag at 0x14FFEB0, mirrored from 0x300; the OS build date at 0x14FFFE0, HollyHock 3's "safe guard") |
| 0x1500000-0x1A1FFFF | MCS (main memory) backup: `CASIOMEMDATA` sectors at 0x1580000 and 0x15A0000, `CASIOMCSDATA` from 0x1600000; the rest erased but the OS's to use |
| 0x1A20000-0x385FFFF | "storage memory": Fugue FAT16 file system (`\fls0\`, 24 MiB logical; log-structured, so it spreads over the whole range) |
| 0x3860000-0x3FFFFFF | eActivity area: Fugue FAT12 (5.5 MiB logical) |

The only part of the chip Casio's OS never touches after an OS update is the
erased hole inside its own image, 0x0EE0000-0x13E0000. Offset 16 MiB lies in
it, so `NK_FLASH` stays 0x81000000 and the usable window ends at 0x13E0000:
3.875 MiB, enough for the 2.5 MiB GWES image. Consequences:

- an OS update rewrites the whole 21 MiB image and erases the window: flash
  NK again afterwards. HollyHock's patches sit below 0x920000 and in the
  last sector of the image, never in the window;
- another OS version may lay its image out differently. `nkflash.hhk`
  therefore erases a sector only when it reads blank (all 0xFF) or the window
  already holds a CE image (ROMHDR `ECEC` at +0x40 or a `B000FF` header), and
  stops before the first erase otherwise;
- the evidence is from the emulated OS (format, first boot, file operations)
  and the pristine image; the hole was never written. Real-device confirmation
  (dump the window from HollyHock before the first flash) is still worth
  doing;
- `docs/01` and the memory table in `docs/05` carry the corrected sizes.

## Boot paths

- **Emulator**: casio-emu loads `dbt/build/dbt-<guest>.g3a`, a raw payload
  at 0x00300000 (`dbt/loader/loader.S` copies the translator image to its
  link address 0x8CC80000, syncs the caches and jumps). The CE image comes
  from the emulated NOR (`make -C dbt NKBIN=/abs/NK.bin run`). `make -C dbt
  hhkrun` runs the device stub below the same way, and `make -C dbt
  flashtest` runs the NOR programming code against the emulator's chip model
  (`HLE_FLASH_CMD=1`).
- **Device, HollyHock 3** (`dbt/hhk/`, `make -C dbt hhk`): two HollyHock
  programs, plain big-endian SH ELF files the launcher copies to their
  addresses and calls as a subroutine (HollyHock 2 copies every ALLOC section,
  HollyHock 3's programs are linked the same way, at 0x8CFF0000):
  - `wince.hhk` (also copied as `wince.hh3`): the stub (`hhk/launcher.c`,
    `start.S`) in HollyHock's program window 0x8CFF0000-0x8D000000 and the
    translator image `dbt-<guest>.bin` as a second loadable region at its own
    link address 0x8CC80000, so the launcher's copy puts it where `loader.S`
    would. The stub checks the copy against the byte sum recorded at build
    time, says what it found (translator size, whether the NOR window holds
    an XIP ROMHDR, a RAM image or nothing) and waits for EXE (Clear returns
    to HollyHock); then `hhk/takeover.S` masks interrupts (IMASK=15, RB=0),
    quiets the OS's interrupt sources (TMU stopped, all INTC priorities 0 and
    sources masked, IRQ pins masked, RTC interrupts off), turns the MMU off
    with the TLB flushed, syncs the caches over the image and jumps to it.
    From there the path is the emulator's: `crt0.S`, `dbt_main` finds the
    XIP image at `NK_FLASH`, the kernel starts. Without OS routines (below)
    the stub boots at once, silently.
  - `nkflash.hhk`: writes `\fls0\NK.bin` (Platform Builder's output, copied
    to the calculator's USB storage) into the window: an XIP build has each
    record programmed at its own address (P1 address - 0x80000000), a RAM
    image goes as is to `NK_FLASH`; the same decision `tools/nkflash.py`
    makes for the emulator. The window check above runs before the first
    erase; records outside the window or an image whose start is not
    `NK_FLASH` are refused. Programming uses the chip's AMD command set
    through P2 (unlock at 0xAAA/0x554, sector erase, 32-word write buffer,
    data polling, read-back verify: `hhk/norflash.c`), with interrupts masked
    around every operation since the OS's handlers live in the chip.
  - OS routines (`hhk/os.c`): the text output and file access come from
    Casio's OS. HollyHock 3 programs receive a symbol table from the launcher
    (`HHK_SYMBOL_TABLE=<hex>` in the environment, a table that starts with
    the OS build date at 0x814FFFE0); the stub takes it when present and
    otherwise uses the built-in addresses of OS 02.01.7002.0001 (the version
    HollyHock 3 supports), only after the OS identifies itself as that
    version at 0x80020028. Key input is read from the KEYSC registers
    directly, as HollyHock's `getKey()` does.
- **Strapped device** (route B, not pursued): a little-endian bootloader at
  physical 0 after flipping the endian strap; `docs/00-plan.md`.

### Procedure (first hardware attempt)

1. Build the image in the VM with the current `config.bib` (ROMSIZE 3.875
   MiB) and verify it in the emulator: `make -C dbt NKBIN=/abs/NK.bin
   RUN_SECS=400 nkshot`.
2. Build the programs: `make -C dbt DEVICE=1 BUILD=build-dev hhk` (DEVICE=1
   leaves the emulator-only debug port out of the translator; `hhk-check`
   confirms the `.hhk` carries the same translator bytes as the `.g3a` of
   that build and loads inside the HollyHock window). Copy
   `dbt/build-dev/wince.hhk`, `dbt/build-dev/nkflash.hhk` and `NK.bin` to the
   root of the calculator's storage (`\fls0\`). If the launcher lists only
   `.hh3` files, use the `.hh3` copies.
3. Run `nkflash.hhk` from the HollyHock launcher: it reports the window,
   the image range and whether the window is blank, then erases and programs
   on EXE (about a minute for 2.5 MiB) and ends with "ROMHDR marker: ECEC
   found".
4. Run `wince.hhk`: it should report "XIP image (ROMHDR) in flash"; EXE
   hands the machine over. Nothing is visible until the display driver
   starts (minutes under the translator: the Release GWES image takes about
   15 s of emulated time, so expect several real minutes); a RESET (back of
   the calculator) returns to Casio's OS, whose RAM state is gone but whose
   flash is untouched outside the window.

What can still go wrong on hardware, in the order worth checking: the
HollyHock launcher crashing while copying the translator into the heap top
at 0x8CC80000 (then stage the payload through `File_Read` into `Mem_Malloc`
memory instead: `hhk/os.c` already has both); `hhk_takeover` leaving a
peripheral the kernel does not expect (USB, SCIF, DMAC are left alone); the
translator's assumptions about SH7305 registers that casio-emu models
loosely (`docs/04` "Limits"); the window not being free on that OS version
(`nkflash.hhk` refuses).

## SH4 OAL startup checklist

- Entry from the loader with MMU off (SH4 kernel code runs in P1/P2),
  caches invalidated, interrupts masked.
- `OEMAddressTable` is not used on SH4; the kernel maps RAM at the P1 address.
- Timer tick: TMU channel 0 at the SH7305 address (gint has it).
- Interrupt dispatch: SH-4A INTC (INTEVT codes, IPR/IMR at 0xA4080000 class),
  mapped to SYSINTR ids in `OEMInterruptHandler`.
- Debug serial: SCIF; KITL over the same port.
- Memory: `MainMemoryEndAddress` = 0x8CFFFFFF on 16 MiB units.
