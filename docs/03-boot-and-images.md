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
    time, then (since 2026-11-05 the flasher is built in) looks for
    `\fls0\NK.bin`: when the file is there it is compared with the window
    record by record and, if it differs, erased and programmed after the
    one EXE the stub asks for; without the file the stub boots what the
    window holds. It says what it found (translator size, window state, CE
    image kind) and waits for that EXE (Clear returns to HollyHock); then
    `hhk/takeover.S` masks interrupts (IMASK=15, RB=0),
    quiets the OS's interrupt sources (TMU stopped, all INTC priorities 0 and
    sources masked, IRQ pins masked, RTC interrupts off), turns the MMU off
    with the TLB flushed, syncs the caches over the image and jumps to it.
    From there the path is the emulator's: `crt0.S`, `dbt_main` finds the
    XIP image at `NK_FLASH`, the kernel starts. Without OS routines (below)
    the stub boots at once, silently. A `DEVICE=1` translator also writes
    its log on the panel (`rt/lcdcon.c`: the top 96 rows, 40 columns of a
    4x8 font drawn 2x, scrolling; the DCS commands the display driver uses)
    from the takeover until the driver's "CLASSPAD DDI:" sign-on, and
    `dbt_halt` takes the panel back to show the last lines, the halt reason
    and the last exception context, then masks interrupts (BL=1) before
    sleeping so a dead kernel is not "rebooted" by the next tick.
    With `"Console"=dword:1` under the display driver's registry key the
    driver signs on with "console kept", never pushes the top 96 rows and
    pushes atomically (`HC_LCD_PUSH`), so the log stays visible beside the
    running desktop: the way to read driver messages on the device.
  - `nkflash.hhk`: the same flasher as a tool of its own (flash without
    booting). It writes `\fls0\NK.bin` (Platform Builder's output, copied
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
3. Run `wince.hhk` from the HollyHock launcher: it reports the window, the
   image in `NK.bin` and whether the flash already holds it, then on EXE
   erases and programs if needed (about a minute for 2.5 MiB, "ROMHDR
   marker: ECEC found") and hands the machine over. `nkflash.hhk` does the
   flashing alone when that is wanted.
4. Nothing is visible until the display driver
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

## The EXT region in the hole (2026-11-06)

With the NK image in the eActivity area (`NK_WINDOW=eact`), the hole
0x0EE0000-0x13E0000 is free, all 5 MiB of it from its real start (the NK
window used 3.875 MiB from 16 MiB). It holds a second, files-only ROM region
(`bsp/classpad/EXT`, `tools/mkextrom.py`): "ECEC" at +0x40, a ROMHDR with no
modules, FILESentry records, names and the files uncompressed. `OEMInit`
validates it (`classpad_extrom.h`) and sets `g_pOemGlobal->pROMChain`;
`KernelFindMemory` (after `OEMInit` on SH, `mdsh3.c`) appends the NK region,
and the loader and the ROM file system walk the chain, so the files appear in
`\Windows`. `wince.hhk` / `nkflash.hhk` flash `\fls0\EXT.bin` there next to
`NK.bin` (window table in `hhk/nkflash.c`), only when it differs.

Is the hole safe to write? Casio's OS checksum covers it: a byte sum over
[0x20000, 0x14FFEB0) + [0x14FFEF0, 0x14FFFF8), stored big-endian at 0x14FFFF8
(0xce1e234b on OS 02.01.7003; casio-emu `docs/classpad-experimental.txt`), so
any data there breaks the stored sum, and it cannot be restored from inside
the hole (erased bytes are already the maximum). In casio-emu's cold boot,
which runs Casio's boot code from the reset vector, it does not matter: with
1 MiB written into the hole (sum 0xc626234b) the boot reaches the same
Language screen as the pristine NOR, frame for frame, and after 1M ticks no
sector of the chip has changed, the hole included (emulator only; other
paths such as the OS updater may still check the sum). An OS update rewrites
the image and erases the region; CE then boots without it ("no EXT ROM
region" from the OAL). Before relying on it on hardware: dump the hole from
HollyHock, write one sector, RESET and power-cycle, check that Casio's OS
boots and the sector survives.

## The eActivity area as an alternative window (experiment, 2026-11-05)

The Standard Shell image is 6.95 MB (`notes/ce/NK-shell1.bin`, not
committed) and does not fit the 3.875 MiB hole, so the eActivity area
0x3860000-0x3FFFFFF (7.625 MiB raw, a Fugue FAT12 volume of 5.5 MiB logical)
was tried in casio-emu's ClassPad cold boot (OS 02.01.7003, the NOR behind
the flash map above; stylus and key injection by
`dbt/emu-patch/add_classpad_touchseq.py`, a patched OS accepted with
`HLE_CLASSPAD_NOVERIFY=1` from `add_classpad_noverify.py`; the harness that
drives the first-boot wizard, opens eActivity and creates a folder is in the
hand-over note). Results:

1. **Foreign data over the whole area** (a recognisable pattern in every
   page): at boot the OS erases all 61 sectors and reformats the volume
   before the first dialog; it also rewrites four sectors of the MCS backup
   area. Nothing survives.
2. **Valid volume header kept** (the two formatted sectors at 0x3860000 and
   0x3880000: Fugue header, FAT12 boot sector, FATs, root directory) with
   the pattern behind it: the boot, the menu and opening the eActivity
   application leave every sector alone. The first write to the volume
   (creating a folder from the save dialog) rewrote pages of the header
   sector and **erased ten of the pattern sectors** (0x38A0000-0x39C0000):
   Fugue is log-structured and reclaims "free" physical sectors by erasing
   them as it needs them, whether or not the FAT lists their clusters as
   free. So an image behind a valid header lives exactly until the user
   saves an eActivity, then it is gone. Not acceptable as the only copy.
3. **Shrinking or disabling the volume by an OS patch**: the volume table at
   OS offset 0x6473E0 (`... 000000F2 0000003D ...`: the storage volume's 242
   and the eActivity volume's 61 sectors; the device descriptor size
   0x025E0000 = 303 sectors from 0x1A20000 appears at OS offsets 0x7B280,
   0x1731C0, 0x61D240, 0x9F1224, 0x9F122C; the eActivity start is the
   literal 0x01E40000 at 0x48695C and 0x03860000 at 0x4A9958, in the mount
   code near P1 0x804A98xx) was patched one word at a time: a sector count
   of 0, 6 or 8 ends in "DRV0 INITIALIZE Format Error!" with all 61 sectors
   erased; 12 formats the same 11542-sector volume and still erases all 61;
   the device size reduced to 0x1F00000 (with either count) gives the format
   error again. The erase extent and the logical size come from the Fugue
   mount/format code itself, so the patch needs that code disassembled
   (start at the function holding 0x4A9958). Its size is unknown; it is not
   a one-word change.
4. **Translator side** (done, default unchanged): `make -C dbt
   NK_WINDOW=eact ...` sets `NK_FLASH` 0x83860000 and `NK_FLASH_MAX`
   0x7A0000 for the translator, `wince.hhk`, `nkflash.hhk` and the `nkxip`
   test guest (which passes at that address: `make NK_WINDOW=eact
   GUEST=nkxip nkrun`); `tools/nkflash.py` accepts P1 addresses up to the
   64 MiB chip. `config.bib` carries the alternative `ROMSTART=83860000
   ROMSIZE=007A0000` as a comment: an image built with it is needed before
   that window can be exercised with CE in the emulator. The shell image
   as built (ROMSTART 0x81000000) boots in casio-emu with the window check
   relaxed, `make -C dbt NK_FLASH_MAX=0x00800000 NKBIN=/abs/NK-shell1.bin
   RUN_SECS=900 nkshot` (POSTINIT, hello window; the emulator's NOR has no
   OS image, so the overlap with the real chip's OS data at 0x13E0000 does
   not show).

Why the eActivity volume is 5.5 MiB in 7.625 MiB raw: not a FAT12 limit
(8 sectors per cluster allow 16 MiB; the volume has 1440 clusters) but the
OS's fixed geometry (11542 logical sectors; the storage volume is FAT16 with
49503 sectors because its 6188 clusters exceed FAT12's 4084) and Fugue's
overhead: it writes 4 KiB pages with a 512-byte header (the formatted
sectors show data only at 4 KiB multiples) and keeps spare sectors for
wear levelling, more of them in proportion on the small volume (storage
24.17 of 30.25 MiB = 80 %, eActivity 5.64 of 7.625 MiB = 74 %).

Conclusion: the eActivity area holds a CE image safely only with an OS
patch that stops Fugue from using it, which is disassembly work still to
do; without it the image survives until the first eActivity save. For a
7 MiB image the alternatives are a smaller image (compression on, fewer
components) or that patch.

### Using the eActivity area without a patch: reflash after every return (2026-11-05)

The reformat in result 1 above is silent: the boot continues normally, the
OS just spends the sector erases, and the only other writes are the MCS
backup log advancing (a new `CASIOMCSDATA` record, the sector sequence
tags toggled: a normal main-memory commit, not a reset). So the area can
be used **without any OS patch** by giving the image up every time Casio's
OS boots:

1. keep `NK.bin` as a file in `\fls0\` (the 24 MiB storage volume);
2. `make -C dbt NK_WINDOW=eact DEVICE=1 BUILD=build-eact hhk` builds
   `wince.hhk` and `nkflash.hhk` for `NK_FLASH` 0x83860000 (image built with
   `ROMSTART=83860000 ROMSIZE=007A0000`); with `NK_WINDOW=eact` the flasher
   also treats a Fugue volume in the window (the `Fugue FAT File System`
   header at +0x2020) as erasable, so it works on a freshly reformatted
   area;
3. from HollyHock: `wince.hhk` (it flashes `NK.bin` first, about a minute
   for 7 MB: 56 sector erases plus programming, then boots);
4. after a RESET, Casio's OS finds the volume invalid, erases the 61 sectors
   and formats it again: the image is gone, the eActivity application works
   as before (empty), and the next CE session starts at step 3.

What it costs: one reflash per CE session (the 7 MB copy over USB only once,
the file stays), 61 erase cycles per Casio boot after a CE session (NOR
endurance is 100k cycles), and every eActivity the user keeps must live
elsewhere (they are erased with the volume). Unverified on hardware like
the rest; the emulator run that established the behaviour is the foreign
data boot in result 1.
