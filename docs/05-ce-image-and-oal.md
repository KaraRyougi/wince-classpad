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
   This is the layout the real device uses (the fx-CP400 chip is 64 MiB;
   offset 16 MiB is inside the erased hole of Casio's 21 MiB OS image and the
   window runs to offset 0x13E0000, docs/03; `nkflash.hhk` programs it from
   Casio's OS, `wince.hhk` starts the translator: `dbt/hhk`). In casio-emu
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
| 0x81000000-0x813DFFFF | NOR: the CE image, executed in place (XIP); on the device this is the erased hole inside Casio's OS image (docs/03), `ROMSIZE` in config.bib | 3.875 MiB |
| 0x8C000000-0x8CB4FFFF | guest: CE RAM (the whole window, BOOTARGS page first, debug ring last; a RAMIMAGE build puts its image here too). Until 2026-11-06 the first MiB was left to the launcher / Casio OS workspace (Casio's VRAM on the device) and CE had 0x8C100000-0x8C8FFFFF, 8 MiB; nothing uses that MiB after the no-return handoff | 11.3125 MiB |
| 0x8CB50000-0x8CDFFFFF | translator text/data/bss (2.46 MiB: 9600 block headers, 1752 KiB metadata arena, hash 64 KiB) + 128 KiB C stack | 2.6875 MiB |
| 0x8CE00000-0x8CFFFFFF | code cache (3 MiB with 14400 headers and a 2624 KiB arena from 2026-11-05 to 2026-11-06) | 2 MiB |

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
| System timer | TMU0 at `0xFFD80000`, fixed 1 ms tick | TMU0 at `0xA4490004` (TSTR), `0xA4490008` TCOR0, `0xA449000C` TCNT0, `0xA4490010` TCR0; input Pphi/4; Pphi ~29.49 MHz -> 7372 counts per ms. Interrupt INTEVT `0x400`, IPRA bits 15:12, unmask IMR4 bit 4 (`IMCR4 = 0x10` at `0xA40800D0`). Verified by the `tmu` guest. **Variable tick** since 2026-11-06 (`timer.c`): the OAL registers `pfnUpdateReschedTime`, the kernel tells it the next wake-up time and TCNT0 is programmed to that, at most `TICK_MAX_MSEC` (1000) apart; CurMSec advances by the actual period, `OEMGetTickCount` adds the counts since the tick. Under the translator every interrupt costs an exception round trip, and the 1 ms tick was about a third of the modelled boot time (the 10 ms experiment in docs/04). `TICK_MAX_MSEC 1` gives the old fixed tick back. |
| INTC (IRQ -> SYSINTR) | IPR at `0xFFD00004`, INTEVT codes | IPRA..IPRL at `0xA4080000`+4n (16-bit; the first OAL draft had +2n, which only TMU0 in IPRA survived), IMR0..12 at `0xA4080080`+4n, IMCR at `0xA40800C0`+4n (gint `intc.h`); `OEMInterruptEnable/Disable/Done` toggle the IMR bits; INTEVT at `0xFF000028`. The kernel's interrupt handler reads INTEVT and dispatches through the OAL's `ISR` table. |
| `OEMIdle` | `sleep` | `sleep` works under the translator (tmu guest) |
| RTC (`OEMGetRealTime`) | SH7750 RTC at `0xFFC80000` | SH7305 RTC at `0xA413FEC0` (gint `rtc.h`), BCD registers; emulator models it |
| Cache (`OEMCacheRangeFlush`) | SH4 `ocbwb`/`ocbp`/`icbi` | same instructions pass through, **plus** hypercall `trapa #0xF2` (flush range) so the translator drops translated blocks for code the kernel just loaded or modified. Without this, a DLL loaded over the address of an earlier one runs stale translations. |
| Debug serial | SCIF | hypercall console (above); SCIF later, USB CDC much later |
| Power (`OEMPowerOff`) | board specific | SH7305 R-standby through the translator's `HC_STANDBY` hypercall (`power.c`, docs/06-deep-suspend.md): core powered off, wake by Clear/ON (KEYSC autonomous scan) or the RTC alarm (`rtc.c` `OEMSetAlarmTime`), the kernel's `pfnInitClock` re-programs TMU0 afterwards; the retained-RAM CPU `sleep` of the first implementation is the fallback. Translator side passes `make -C dbt run GUEST=standby`; the OAL side is unbuilt. |
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
   in `notes/ce/stock-run.txt`); `config.bib` gives it 3.1% now (12.5% until
   2026-11-06; it holds the registry, so not zero) and the
   image is XIP. **Verified 2026-11-04** with the XIP GWES image: GWES
   loads the driver (`DisplayInit`, 384x216 frame buffer), the hello
   window appears in the emulator's screenshot (colour bars, orange
   frame), its timer ticks and the driver keeps pushing dirty rows; the
   keyboard driver is up with its interrupt, the touch driver finds no
   panel on the fx-CG100 profile. Three translator bugs fell out of it:
   faults in helper C code are delivered to the guest, the inline-cache
   refill race, and the helper prologue window undoing a spill the call
   had already restored (docs/04; found with a driver build that logs
   its first 48 blits). The "missing" grey field and text were the hello
   app's own doing: `Rectangle` filled its interior with the stock white
   brush over the grey, and the text was white on it; `hello.c` now
   selects the hollow brush (a rebuild of the image shows it). Every blit
   the window makes is a solid fill or a 1bpp text blit through GPE's
   emulated paths. The keyboard IST's timed wait failing after the first paint
   (the interrupt event still listing the thread's previous proxy, error
   6 ERROR_INVALID_HANDLE) is a CE 6.0 R3 kernel hole, not a translator
   bug: WaitOneMore's timed-out retry path returns without unlinking an
   interrupt-event proxy (`notes/ce/kernel-sh-notes.md` section 10,
   `bsp/kernel/kcalls-waitonemore.patch`). Neither driver waits on its
   interrupt event with a timeout any more: the keyboard rescans from a
   poll thread, the touch DDSI drives the sampling while pressed from its
   own thread by setting the MDD's event (the MDD's `gdwTouchIstTimeout`
   stays INFINITE). The kernel patch is verified (2026-11-05, rebuilt kernel,
   write watch on the event: the hole fired once at the first paint, the
   patched exit path unlinked the proxy and the IST went on; the window
   ticked for the whole 600 s run). With all of it rebuilt (same day) the
   keyboard IST runs without a failure and the window shows bars, grey
   field, frame and text for the whole run (`notes/ce/hello-fix3.png`);
   the touch sample thread is not exercised on the fx-CG100 profile.
   The ClassPad image (same day, section 5 items done) runs on the
   ClassPad profile with the 320x528 DCS panel showing, the fx-CP400 key
   table (press and release in the right order, one auto-repeat during a
   held key), and the IIC0 touch driver delivering calibrated points
   (`notes/ce/hello-cp400.png`, `notes/ce/stock-run.txt`). The Standard
   Shell image (same day: Standard Shell, AYGShell, imaging codecs,
   Solitaire, QVGA portrait resources; hello no longer launched at boot)
   boots to the CE 6.0 desktop with taskbar in about 10 minutes of wall
   time on the ClassPad profile (`notes/ce/desktop-cp400.png`); with the
   runtime speedups of the same evening (docs/04), the 8192-block layout and
   the LCD push hypercall it is about 5 minutes. It is 6.95 MB and does not
   fit the real device's 3.875 MiB NOR window.
7. Keyboard and touch (written 2026-11-03, not yet compiled):
   `SRC\DRIVERS\KEYBD` is the Layout Manager MDD with a PDD that programs
   the SH7305 KEYSC like the fx-CG100 OS does, requests a SYSINTR for its
   interrupt (INTEVT 0xBE0 is vector 95, beyond the common OAL's 64-entry
   IRQ map, so the OAL hooks the vector and the driver uses logical IRQ 48)
   and diffs the six KIUDATA words in an IST woken by the interrupt and a
   20 ms poll thread (the emulator drops an edge raised while the source is
   masked). Scan code = 8 * row + column; two device layouts (fx-CG100 and
   fx-CP400 keypads, registry `Matrix`). `SRC\DRIVERS\TOUCH` is the touch
   MDD with a DDSI layer for the fx-CP400 panel as casio-emu models it: a
   sensor on IIC0 (address 0x40, command 0x84 returns x, y, contact), the
   contact line on Port R bit 5 and INTEVT 0x700 on the down edge; samples
   while pressed come from a DDSI thread that sets the MDD's event at the
   sample rate; `CalibrationData` in
   platform.reg is computed from the emulator's stylus mapping for 320x528.
   The touch driver only touches the bus when a panel is present (registry
   `Panel`, else a display with 528 rows), because the fx-CG100 profile
   does not emulate IIC0. The keyboard is verified on the fx-CG100 with the
   emulator's key injection (`HLE_CB_CHORD`, `bsp/classpad/README.md`).
   casio-emu's ClassPad profile used to refuse an add-in (cold boot of the
   real OS only); since 2026-11-05 `dbt/emu-patch/add_classpad_addin.py`
   lets it run the translator image (`make -C dbt EMU_MODEL=fx-cp400 ...`,
   section 5), so the touch driver can be exercised once the image is built
   for the ClassPad panel (section 5 lists what that takes). `hello.c` logs
   key and touch events on screen and on the console. The OS design needs
   "Touch Screen (Stylus)" (SYSGEN_TOUCH) for touch.dll; the keyboard MDD
   comes with Minimal Input.

## 5. casio-emu's ClassPad profile (2026-11-05)

All runs so far used casio-emu's fx-CG100 profile (`CALCEMU_MODEL=fx-cg100`,
a 396x224 panel, the R61524 register protocol, no IIC0, peripheral
interrupts delivered only at SR.IMASK = 0). The ClassPad profile
(`fx-cp400`, `fx-cg500`; `build/generated/machine_profiles.inc` from
`machine-profiles.js`) models the 320x528 panel, the IIC0 touch sensor and
INTC priority arbitration, but `configureClasspad` in
`src/gui/sdl/runtime_paths.c` only cold-booted the verified OS from a
64 MiB NOR image and answered a `.g3a` with "ClassPad requires real-OS cold
boot". `dbt/emu-patch/add_classpad_addin.py` (applied to the local emulator
like the other patches; rerun after updating it, then `cmake --build
build`) changes three things:

- `configureClasspad`: with a `.g3a` on the command line (no `HLE_COLDBOOT`)
  the profile is selected and the NOR left erased; `HLE_NORFILE`/`HLE_OSROM`
  are not set, so `memory.c` places `HLE_OSROM` at `HLE_OSROM_PAD` exactly
  as on the fx-CG100 and loads the `.g3a` into the 2 MiB window behind the
  64 MiB NOR (0x84000000, aliased at 0x00300000, where the launcher starts).
  The CE image still lands at NOR offset 16 MiB (P1 0x81000000). Cold boot
  is unchanged.
- `display.c`: `HLE_FBDUMP` also writes the ClassPad frame (320x528 PPM,
  near-uniform frames skipped), so `make nkshot` works on this profile.
- `timers.c`: `HLE_CB_TOUCH="x,y" HLE_CB_TOUCH_AT=<tick> HLE_CB_TOUCH_HOLD=<ticks>`
  presses the stylus headlessly (x 0..319, y 0..575; rows 528..575 are the
  icon strip) through `classpadSetTouch`, like a click on the skin: sensor
  packet, Port R bit 5, INTEVT 0x700 on the down edge. It prints
  `[INJECTTOUCH down] x y at tick N` and `[INJECTTOUCH up]`.

`dbt/Makefile` has `EMU_MODEL ?= fx-cp400` (the ClassPad is the target; the
default was fx-cg100 until 2026-11-05, and `EMU_MODEL=fx-cg100` still runs
that profile for a comparison, nothing more) for `run`, `nkrun`, `nkshot`
and `screenshot`:

    HLE_CB_CHORD=72,74 HLE_CB_CHORD_AT=1200000 HLE_CB_CHORD_HOLD=30000 \
    HLE_CB_TOUCH=160,264 HLE_CB_TOUCH_AT=1350000 HLE_CB_TOUCH_HOLD=30000 \
        make -C dbt EMU_MODEL=fx-cp400 BUILD=build-cp400 \
             NKBIN=/abs/NK.bin RUN_SECS=330 nkshot

Result with `notes/ce/NK-fix3.bin` (the 2026-11-05 Release image built for
384x216): the `tmu` and `nkxip` guests pass on the ClassPad profile, the CE
image boots to "CLASSPAD OAL: POSTINIT", GWES, the display driver, the
keyboard driver (the chord gives the same `scan 22/38 down`, `key down
vk=31 sc=22`, `char '7'` lines as on the fx-CG100, at the same ticks: the
profile's tick rate is the same) and the hello window; nothing in the run
differs from the fx-CG100 run except the touch driver's panel decision. So
the ClassPad's INTC arbitration (priority against IMASK instead of
IMASK = 0) and its sleep semantics (an interrupt is accepted in `sleep`
even with BL = 1, `handleSleepInterrupt`) change nothing for the kernel and
the OAL. What the image still needed for the ClassPad, found in that run (all done and verified the same day, commits def3c89 and 6dc3d93, `notes/ce/stock-run.txt`).
**Done in the sources the same day** (items 1, 2, 4 below; the image is
rebuilt by the user in the VM: Build Current BSP and Subprojects, Copy
Files to Release Directory, Make Run-Time Image, no Sysgen; the kernel is
already patched): `classpad_lcd.h` describes both panels with per-panel
sizes and the DCS command set; `classpad_ddi.cpp` picks the panel from the
registry `Panel` or the height (under 500 rows = R61524, else DCS), sends
SLPOUT, 120 ms, COLMOD 16 bpp at start, CASET/PASET/RAMWR per push, DISPON
after the first push and DISPOFF/SLPIN from `PowerHandler`; `platform.reg`
has `Width`=0x140, `Height`=0x210 and `Matrix`=1. The R61524 path still
compiles and is chosen by `Height` < 500 or `Panel`=0. Unverified until the
rebuilt image runs; the real panel's initialisation beyond the DCS minimum
is unknown (Casio's boot code does it before the OS on the device).

1. **Display protocol.** The ClassPad LCD model (`src/hardware/classpad/
   classpad.c`) is not an R61524: it is a DCS panel on the same 16-bit port
   at 0xB4000000 with Port R bit 4 selecting command (0) or data (1). A
   command is the low byte of a 16-bit write; `0x2A`/`0x2B` take four
   parameter bytes (one per 16-bit write: start high, start low, end high,
   end low) for the column/row window; `0x2C` starts a memory write and
   every following 16-bit data write is one RGB565 pixel (a 32-bit write is
   two), advancing column-first inside the window; `0x11` (sleep out) and
   `0x29` (display on) must both be sent before anything is shown
   (`classpadFrame()` returns a blank frame until then); `0x28`/`0x10`
   blank and sleep; reading with bit 4 clear returns the last command, `0xDA`
   reads panel ID 0x16. The current DDI pushes R61524 register writes
   (0x200-0x202, 0x210-0x213) through `LcdWriteReg`, which the ClassPad
   model takes as commands 0x00-0x13: the hello window is drawn in the frame
   buffer but the emulated panel stays blank, so `nkshot` produces no
   `nk.png` on this profile. The DDI needed a panel variant (registry or
   `Height` >= 500) that speaks this protocol, and `classpad_lcd.h`'s
   384x216 ceilings raised to 320x528 (done, above).
2. **Geometry.** `platform.reg`: `Width`=0x140, `Height`=0x210 for the
   `ClassPad` display key (was 0x180 x 0xD8 = 384x216; done). `config.bib`
   needs nothing: the frame buffer is a `GPESurf` in GWES's RAM (338 KB at
   320x528), not a reserved region.
3. **Touch.** With a 528-row display the driver detects the panel itself,
   or `"Panel"=dword:1` under `HKLM\HARDWARE\DEVICEMAP\TOUCH` forces it;
   with this image it printed `display 384x216 -> no touch panel` and never
   touched IIC0, so the DDSI (`classpad_touch.cpp`) is still unexercised. Do
   not force the panel in an image that will also run on the fx-CG100
   profile: that profile has no IIC0 registers and an access is a fatal
   undefined-MMIO fault.
4. **Keypad.** `"Matrix"=dword:1` under `HKLM\HARDWARE\DEVICEMAP\KEYBD`
   for the fx-CP400 key table (the emulator's key codes are the same on both
   profiles; the hello run above used the fx-CG100 layout).
5. **Undefined registers.** None were hit: the ClassPad profile defines a
   superset of the fx-CG100's registers for everything the OAL and the
   drivers touch (KEYSC with KIUSTATEREG at +0x12, Port R with the contact
   bit, IIC0, the DCS panel port).

A faceplate warning (`ClassPad photo not loaded`) under the headless SDL
driver is harmless.

## 6. Image size: what the ROM carries without needing it (2026-11-06)

NK.bin was 6.94 MB in the 7.625 MiB eActivity window. Module sizes from the
release directory (largest first): gwes 593 KB, coredll + k.coredll 825 KB,
commctrl 355 KB, tcpstk 287 KB, ceshell 265 KB, explorer 260 KB, kernel
236 KB, filesys 228 KB, shdocvw 175 KB, mgtt_o 174 KB, ole32 + oleaut32
309 KB, classpad_ddi 142 KB, cplmain 141 KB, pegcards 137 KB, wince.nls
209 KB, mfcce300 408 KB, tahoma.ttf 136 KB, hives 180 KB.

`bsp/classpad/FILES/PostFmergeBib.bat` (run by makeimg from the release
directory between fmerge and romimage) drops what `romexclude.txt` lists:
- the network stack, ~740 KB: tcpstk, ndis, afd, dhcp, netbios, cxport, ws2k,
  ws2serv, ws2instl, winsock, wspm, nspm, uiproxy, serial. The Standard Shell
  item depends on it in the catalog, but a PE import scan of the image shows
  no shell module importing it (explorer, ceshell, shdocvw, mfcce300); only
  servicesd.exe imports ws2.dll, which stays. The registry still lists NDIS,
  AFD and Ws2Serv under Drivers\BuiltIn: device manager fails to load them and
  goes on.
- the 18 standard .wav files, 62 KB: forced by the Control Panel item
  (SYSGEN_CTLPNL sets SYSGEN_AUDIO_STDWAVEFILES); there is no audio driver.

The hook must be in the release directory (buildrel or the claude-build
script copies it there); once there it applies to every makeimg until
romexclude.txt is deleted from that directory.

Also worth doing, in the OS design rather than here: only the English (US)
locale (the design had all 68 code page 1252 locales: wince.nls 209 KB), no
Solitaire (pegcards + solitare, 167 KB), and possibly Power Management
(Minimal) instead of the full PM (pm.dll 42 KB, one thread fewer).
