# Windows CE 6.0 R3 image: loading, build environment, SH7305 OAL

Status 2026-10-22: the translator runtime loads CE `.bin` images (records,
ROMHDR, entry) and the loader is tested with a synthetic image; no CE code
has run yet. This document fixes the image layout, the Platform Builder
setup and the OAL work that comes next.

## 1. Image loading (implemented)

`dbt/rt/nkbin.c` parses the ROMIMAGE `.bin` format (`B000FF\n`, image
start/length, records `{addr, len, bytesum}`, terminator `{0, entry, 0}`),
copies every record byte-for-byte into guest RAM (the guest memory image is
byte-identical to a little-endian machine, so no swapping), then reads the
ROMHDR through the `ECEC` marker at image start + 0x40 and prints CPU type,
physical range, module count, RAM range and kernel flags. Records outside
the guest window `0x8C100000..0x8CC7FFFF` are rejected; a RAM range that
leaves the window or a non-SH4 CPU type is reported as a warning.

Where the image comes from (`dbt/rt/main.c`):

1. **Flash**, at `NK_FLASH` (default P1 `0x81000000`, NOR offset 16 MiB).
   This is the layout the real device will use (the CE image lives in the
   32 MiB XIP flash; Casio's OS occupies the first few MiB). In casio-emu
   the add-in window is only 2 MiB, so a CE image cannot travel inside the
   `.g3a`; instead `HLE_OSROM=<file> HLE_OSROM_PAD=<offset>` places a file
   into the emulated NOR, and `tools/nkflash.py` (run by the Makefile for
   `make -C dbt NKBIN=/abs/NK.bin run`) decides what and where:
   - an **XIP** build (`config.bib` since 2026-11-03: `NK 81000000 ...
     RAMIMAGE`, the only image type ROMIMAGE knows, with
     `ROMSTART=81000000`) has its image start in the NOR's P1
     range; the `.bin` is flattened to the bytes the NOR holds and written
     at `start - 0x80000000`. The runtime finds the `ECEC` marker at
     `NK_FLASH + 0x40`, reads the ROMHDR (RAM range, kernel.dll's code
     range, nk.exe's entry) and jumps: the kernel executes in place, only
     its RAM range (the whole guest window) is in RAM, nothing is copied.
     Translation reads guest code from the NOR through P1; the translated
     host code lives in the RAM code cache as before.
   - a **RAMIMAGE** build (image start inside the guest window, the first
     images) is written as is at the 16 MiB offset; the runtime copies its
     records into RAM and takes the entry from the terminator record.
2. The **embedded blob** when it is a `.bin` (small test images).
3. Otherwise the blob is a raw little-endian binary at `0x8C100000`
   (the hello/exc/fpu/tmu/bench/lcd guests).

`tools/mkbin.py` wraps a flat binary as a `.bin` (256-byte records in the
test, all-zero runs skipped like ROMIMAGE does); `make -C dbt GUEST=nkimg
nkrun` builds a guest that carries a ROMHDR like ROMIMAGE output, wraps it,
runs it through the copy path and checks `.data`, `.bss`, the marker and
the ROMHDR fields; `make -C dbt GUEST=nkxip nkrun` does the same for the
XIP path with a guest linked at the NOR address (`guest/guest_xip.ld`:
code and rodata in flash, data and bss in RAM, the start code copying
`.data` as the kernel's copy entries do) and checks that it runs from
flash. casio-emu copies flash files in 32-bit units and scrambles a
trailing partial word, so `nkflash.py` pads the file to a multiple of 4
bytes before handing it to `HLE_OSROM`.

### Memory budget on a 16 MiB device

| range | use | size |
|---|---|---|
| 0x8C000000-0x8C0FFFFF | left to the launcher / Casio OS workspace in the emulator | 1 MiB |
| 0x81000000-0x817FFFFF | NOR: the CE image, executed in place (XIP); on the device the first 16 MiB of the chip hold Casio's OS | 8 MiB |
| 0x8C100000-0x8CC7FFFF | guest: CE RAM (the whole window; a RAMIMAGE build puts its image here too) | 11.5 MiB |
| 0x8CC80000-0x8CEFFFFF | translator text/data/bss (~2.3 MiB: 4096 block headers, 1.25 MiB metadata arena, hash 64 KiB) + C stack | 2.5 MiB |
| 0x8CF00000-0x8CFFFFFF | code cache | 1 MiB |

The layout is defined once in `dbt/Makefile` (`GUEST_RAM_*`, `IMAGE_BASE`,
`CACHE_*`) and reaches the C code, the launcher and the linker script from
there; the linker script asserts that the image leaves 128 KiB for its stack.

A headless CE 6.0 kernel image is 2-4 MiB, so a RAMIMAGE left 7.5-9.5 MiB
of CE RAM, half of which the RAM file system took by default: the first
GWES image ran out of pages before the display driver loaded. The image is
XIP since 2026-11-03 (above), giving CE the whole 11.5 MiB window. The block
table was compacted on 2026-10-22 (fixed per-block arrays became a shared
metadata arena: bss 2.3 MiB -> 1.2 MiB while the block limit rose from 768
to 2048) and the translator region shrunk from 3 MiB to 1.5 MiB. The other
lever is building the OS as an **XIP image** executing in place from flash
(ROMSTART in the `0x81xxxxxx` P1 flash range), which the translator already
supports since guest code may live anywhere it can fetch from; only the
copy entries (`.data`) and the kernel's RAM then use guest RAM.

## 2. Platform Builder 6.0 R3 environment (to set up)

Windows CE 6.0 is the last release with SH-4 support and the only one whose
tools are still obtainable, so everything is built there.

- **Host**: a Windows 7 32-bit (or XP SP3) VM; 40 GB disk, 4 GB RAM. 64-bit
  hosts work but the VS2005 and PB6 installers need the documented
  compatibility steps, so the 32-bit VM is the low-risk choice.
- **Install order** (each step before the next, with reboots): Visual
  Studio 2005 Professional, VS2005 SP1, Platform Builder 6.0 (install DVD;
  pick the **SH4** processor and the **Shared Source** kernel sources),
  CE 6.0 SP1, CE 6.0 R2, CE 6.0 R3, then the cumulative QFEs (monthly
  updates through 2013 exist; the last ones fix the kernel). Media and a
  product key come from an MSDN subscription or the old 180-day evaluation
  kit; sourcing them is the user's part.
- **Check first after installing** (`%_WINCEROOT%\PRIVATE\WINCEOS\COREOS\NK\KERNEL\SH`):
  the SH4 exception entry and interrupt path, for the two assumptions the
  translator makes: that no persistent kernel state lives in bank-1
  registers across code that can take exceptions (see docs/04 limits), and
  that the kernel's own `rte` slots and SR writes fit our `ldc sr`/`rte`
  interception. Also note the API-call convention: CE on SH4 calls the
  kernel by jumping to invalid addresses (`0xF000xxxx`), relying on the
  address-error exception; the translator delivers that fault from its code
  probe with the right EXPEVT and SPC, which the `exc` guest covers only for
  `trapa`, so a guest test for a jump to an unmapped address is due.
- **Reference BSP**: start from the SH4 sample BSP shipped in `PLATFORM`
  (the Solution Engine / Aspen-class board), clone it as `PLATFORM\CLASSPAD`,
  keep its kernel glue, replace the SoC-specific parts listed below.
- **OS design**: start headless (Core OS -> "Headless" / minimal kernel,
  no GWES, no device.exe extras), Release build, `IMGNOKITL=1`,
  `IMGNODEBUGGER=1`, `IMGNOSHELL=0` once a console exists. Add
  `IMGRAM32=1`-style memory settings as needed. Build output:
  `OSDesigns\<name>\RelDir\CLASSPAD_SH4_Release\NK.bin`.
- **config.bib** (XIP, the current one in `bsp/classpad/FILES`):
  ```
  MEMORY
      NK      81000000  00800000  RAMIMAGE
      RAM     8C101000  00B7E000  RAM
  CONFIG
      ROMSTART=81000000  ROMWIDTH=32  ROMSIZE=00800000
      AUTOSIZE=OFF  KERNELFIXUPS=ON  PROFILE=OFF  COMPRESSION=OFF
      FSRAMPERCENT=20202020
  ```
  (the first images were a RAMIMAGE inside the guest window with
  AUTOSIZE=ON; the loader still accepts those)
  (`AUTOSIZE` lets ROMIMAGE move the RAM start up to the real image end.)
  Second iteration (done 2026-11-04): `NK 81000000 ... RAMIMAGE` with `ROMSTART=81000000` and RAM at `8C101000`.
- **Debug output**: `OEMInitDebugSerial` does nothing, `OEMWriteDebugByte`
  issues the translator's `trapa #0xF3` hypercall (one byte in r4) when
  SR.BL is clear. The kernel calls `OEMInitDebugSerial`, the sign-on string
  and `OEMInit` from `SHInit` with SR = PR_B1_BK (bank 1, BL=1), where a
  trapa is a manual reset; those bytes go to the debug ring in the last page
  of the guest window (0xACC7F000, docs/04), which the translator prints at
  its next entry. `OEMReadDebugByte` returns `OEM_DEBUG_READ_NODATA`.

## 3. SH7305 OAL (written: `bsp/classpad`, not yet compiled)

The SH4 sample OAL targets the SH7750 (on-chip modules at `0xFFDxxxxx`);
the SH7305 is an SH-4A SoC with the SH7724-style peripheral map at
`0xA4xxxxxx`. Everything the OAL touches must use the **pre-swapped MMIO
convention** (`guest/mmio.h` in this repo: 16/32-bit register values are
byte-swapped by the guest, bytes are not), since the translator swaps every
16/32-bit load and store.

| OAL piece | sample BSP (SH7750) | ClassPad / fx-CG (SH7305) |
|---|---|---|
| `StartUp` (image entry) | sets SR, clears BSS, jumps to `KernelStart` | same; SR writes go through the translator's `ldc sr` helper, VBR to the shadow VBR. Must not rely on reset-time MMU state: start with MMUCR=0, flush TLB |
| System timer (1 ms tick) | TMU0 at `0xFFD80000` | TMU0 at `0xA4490004` (TSTR), `0xA4490008` TCOR0, `0xA449000C` TCNT0, `0xA4490010` TCR0; input Pphi/4; Pphi ~29.49 MHz -> TCOR0 = 7372. Interrupt INTEVT `0x400`, IPRA bits 15:12, unmask IMR4 bit 4 (`IMCR4 = 0x10` at `0xA40800D0`). Verified by the `tmu` guest. |
| INTC (IRQ -> SYSINTR) | IPR at `0xFFD00004`, INTEVT codes | IPRA..IPRL at `0xA4080000`+4n (16-bit; the first OAL draft had +2n, which only TMU0 in IPRA survived), IMR0..12 at `0xA4080080`+4n, IMCR at `0xA40800C0`+4n (gint `intc.h`); `OEMInterruptEnable/Disable/Done` toggle the IMR bits; INTEVT at `0xFF000028`. The kernel's interrupt handler reads INTEVT and dispatches through the OAL's `ISR` table. |
| `OEMIdle` | `sleep` | `sleep` works under the translator (tmu guest) |
| RTC (`OEMGetRealTime`) | SH7750 RTC at `0xFFC80000` | SH7305 RTC at `0xA413FEC0` (gint `rtc.h`), BCD registers; emulator models it |
| Cache (`OEMCacheRangeFlush`) | SH4 `ocbwb`/`ocbp`/`icbi` | same instructions pass through, **plus** hypercall `trapa #0xF2` (flush range) so the translator drops translated blocks for code the kernel just loaded or modified. Without this, a DLL loaded over the address of an earlier one runs stale translations. |
| Debug serial | SCIF | hypercall console (above); SCIF later, USB CDC much later |
| Power (`OEMPowerOff`) | board specific | stub |
| `OEMIoControl` | board info, device ID, UUID | minimal: `IOCTL_HAL_GET_DEVICE_INFO` with a fixed name, `IOCTL_HAL_INIT_RTC` |
| Memory map | `OEMAddressTable` (ARM/MIPS style) not used on SH4; RAM from config.bib | `MainMemoryEndAddress` from config.bib; no extension DRAM |
| Display | none headless | later: the display driver pushes a 16-bit frame buffer to the R61524 (`lcd` guest shows the protocol); ClassPad's 320x528 panel to be investigated |
| Keyboard, touch | none headless | written 2026-11-03: `SRC\DRIVERS\KEYBD` (Layout Manager + KEYSC matrix PDD, `kbdmouse.dll`) and `SRC\DRIVERS\TOUCH` (touch MDD + the fx-CP400 sensor on IIC0, `touch.dll`); the panel is an I2C sensor in casio-emu, not an ADC input. Details in `bsp/classpad/README.md` |

Translator-side items the OAL work depends on:

- ~~Deliver TLB misses and address errors from the translator's code probe~~
  done (`apicall` and `mmu` guests): a probe TLB miss is delivered with the
  hardware EXPEVT and TEA at the guest pc; an unfetchable pc (odd, or P4
  such as the API-call addresses 0xF000xxxx) gets a synthesized instruction
  address error, since real hardware would not fault the translator's data
  read there.
- ~~Guest code in TLB-mapped regions (P0/U0)~~ fetch and data paths done
  (`mmu` guest: code and data through `ldtlb` entries, miss -> handler ->
  refill -> retry).
- ~~Virtual address reuse~~ done (`aspace` guest, design in docs/04
  "Address spaces"): user-space blocks carry ASID and physical page, the
  fast caches hold only the current ASID's entries, `ldtlb` is intercepted,
  invalidation is precise (hash, inline caches, chained jumps). The kernel
  side of the contract, to add where the shared-source kernel switches
  address spaces and invalidates TLB entries (and in `OEMCacheRangeFlush`):
  `trapa #0xFA` with r4 = new ASID after writing PTEH, `trapa #0xF9` with
  r4/r5 = virtual range (r5 = 0: everything) after removing TLB entries or
  changing a mapping, `trapa #0xF2` with the range for code the loader wrote.
  Without them the translator still notices switches at `rte`/`ldc sr` and
  remaps at `ldtlb`, but a block entered from the hash between a TLB
  invalidation and the refill would run stale code.
- FPU: the kernel saves and restores FPSCR/FPUL/FR registers on context
  switch with ordinary FPU instructions, all of which go through the helper
  into the single `fpuemu` state, so per-thread FPU state works without OAL
  help. The default FPSCR the kernel installs (`0x00040001`, RZ, DN) is
  what the emulator sees most of the time.
- SR.MD is virtual: user processes run with MD=1 on the hardware (shadow
  MD=0). Privileged-instruction traps and page protection by mode do not
  fire; acceptable for bring-up, to be revisited for user/kernel isolation.

What the kernel sources settled (2026-11-02, notes/ce/kernel-sh-notes.md):

- ~~Bank 1~~: the kernel does keep live state in r4/r5/r7_bank1. The
  translator's exception stubs now preserve bank 1 (`bank` guest).
- F2 and F9 both live in `OEMCacheRangeFlush` (the kernel routes code
  writes and TLB invalidation through it); FA is not needed as a kernel
  edit (the translator reads PTEH at `rte`/`ldc sr`, and SHSetASID's store
  will be seen by the device-aware translation below). No kernel.dll change
  for the hypercall contract.
- **kernel.dll touches the CCN registers directly** (EXPEVT, INTEVT, TEA,
  TRA, PTEH/PTEL/PTEA, TTB, MMUCR, CCR; table in the notes). Through the
  swapped memory path those values arrive byte-reversed, so API calls would
  never be recognised and ASIDs would never reach the hardware. ~~Next
  translator item~~ done 2026-11-02: device-aware translation for blocks
  inside kernel.dll's address range (from the ROMHDR module table, docs/04
  "Runtime conventions"): 32-bit memory instructions test the top byte of
  the effective address and take an unswapped helper for 0xFF, which also
  observes PTEH/MMUCR writes (`devmem` guest). The loader prints the range
  it found ("[nk] kernel.dll ... (device-aware translation)").
- **SR.FD is emulated** for the kernel's lazy FPU switch: the FPU helper
  checks the guest SR and, with FD=1, raises EXPEVT 0x800 (0x820 in a
  delay slot, SPC = the branch) through the translator's synthetic
  exception instead of executing; the kernel's SwitchFPUOwner then saves
  and restores the emulated registers with ordinary FPU instructions and
  clears FD (`fpudis` guest, docs/04).
- The OAL's debug output cannot trap while BL=1 (SHInit/OEMInit): debug ring.
- The stock HARP image cannot be loaded as-is (records at 0x8C001000, RAM to
  0x90000000): `config.bib` of the ClassPad BSP places NK at 0x8C101000 and
  RAM up to 0x8CC7F000.

## 4. Order of work

1. ~~Install the VM and Platform Builder; build the stock SH4 BSP image once
   (no hardware needed) to prove the toolchain. Read the kernel's SH4
   exception sources and settle the bank-1 question.~~ Done 2026-11-02
   (HARP image built; notes/ce/).
2. ~~Guest tests for the two translator gaps~~ done 2026-10-22 (`apicall`,
   `mmu`); address-space handling done (`aspace`).
3. `PLATFORM\CLASSPAD`: ~~StartUp, OEMInit with TMU0 tick and INTC, hypercall
   debug output, cache flush hook. Headless OS design, RAMIMAGE config.bib~~
   written and compiled (bsp/classpad, 2026-11-02; CRLF files, cacheops.src).
   The ClassPad NK.bin (Debug configuration) boots under the translator in
   casio-emu (with dbt/emu-patch/add_ccn_regs.py applied): SHInit, OEMInit,
   KernelInit, the first thread, LoaderInit (ROM decompression), FSDMGR, the
   ROM file system and the boot registry, boot phases 0 and 1 within 90 s
   of emulated time, then the system and user hives, "FILESYS: Launching
   apps" and "This device has booted 1 times" (2026-11-03, about 7 minutes
   of Debug-build boot; nothing to launch in a kernel-only image, the
   kernel then idles). Fixed on the way: bank-switching `ldc sr` (SPC/SSR
   through a trampoline), `rte` into untranslated mapped code with BL=1
   (resolved after the rte), committed-resume across the device slow path,
   HC_FLUSH's range, and a link-literal patch after a full code-cache flush
   (showed as a coredll RHeapFree DEBUGCHK). The Release image
   (COMPRESSION=OFF, 1.1 MB) boots to OEMIdle within 2 minutes once the
   device-aware range was taken from kernel.dll's code sections instead of
   load..load+vsize (ROMIMAGE had put the Release kernel's text, with the
   exception vectors, below its load address). A retail kernel prints
   nothing, so the OAL marks POSTINIT and the first idle. SR.FD is
   emulated (2026-11-03). Still open: display.
4. `make -C dbt NKBIN=NK.bin run`: expect the kernel banner on stdout,
   then the first scheduler tick. Fix what breaks.
5. Memory: ~~compact block metadata; move the translator region down and
   widen the guest window~~ (done, 11.5 MiB guest window); ~~move to XIP
   in flash~~ (done 2026-11-04, the first XIP CE image pending a VM build).
6. Display driver (written 2026-11-03: `bsp/classpad/SRC/DRIVERS/DISPLAY`,
   GPE with a RAM frame buffer and a push thread to the R61524; a hello
   window app; registry and bib entries; `make nkshot` for a screenshot of
   a CE image). First GWES image (2026-11-03, `ClasspadUI`, 27 modules):
   the kernel initialises, then GWES dies in `WBITMAP::WBITMAP` with a null
   `Driver_t`: the kernel refused the driver's 65 KB data copy for low
   memory, the RAM file system having half of the 9 MiB RAM (block trace
   in `notes/ce/stock-run.txt`); `config.bib` now gives it 12.5%
   (`FSRAMPERCENT=20202020`), to be verified with a remade image.
7. Keyboard and touch (written 2026-11-03, not yet compiled):
   `SRC\DRIVERS\KEYBD` is the Layout Manager MDD with a PDD that programs
   the SH7305 KEYSC like the fx-CG100 OS does, requests a SYSINTR for its
   interrupt (INTEVT 0xBE0 is vector 95, beyond the common OAL's 64-entry
   IRQ map, so the OAL hooks the vector and the driver uses logical IRQ 48)
   and diffs the six KIUDATA words in an IST woken by the interrupt and a
   20 ms poll (the emulator drops an edge raised while the source is
   masked). Scan code = 8 * row + column; two device layouts (fx-CG100 and
   fx-CP400 keypads, registry `Matrix`). `SRC\DRIVERS\TOUCH` is the touch
   MDD with a DDSI layer for the fx-CP400 panel as casio-emu models it: a
   sensor on IIC0 (address 0x40, command 0x84 returns x, y, contact), the
   contact line on Port R bit 5 and INTEVT 0x700 on the down edge; samples
   while pressed come from the MDD's IST timeout; `CalibrationData` in
   platform.reg is computed from the emulator's stylus mapping for 320x528.
   The touch driver only touches the bus when a panel is present (registry
   `Panel`, else a display with 528 rows), because the fx-CG100 profile
   does not emulate IIC0. casio-emu cannot run the translator image on its
   ClassPad profile (that profile only cold-boots the real OS from a 64 MiB
   NOR image, `src/gui/sdl/runtime_paths.c`), so the keyboard is verified
   on the fx-CG100 with the emulator's key injection
   (`HLE_CB_KEYSWEEP`, `bsp/classpad/README.md`) and the touch driver stays
   unverified until the ClassPad profile can start an add-in or the real
   device runs. `hello.c` logs key and touch events on screen and on the
   console. The OS design needs "Touch Screen (Stylus)" (SYSGEN_TOUCH) for
   touch.dll; the keyboard MDD comes with Minimal Input.
