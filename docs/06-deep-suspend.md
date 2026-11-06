# Deep suspend: R-standby for Windows CE on the fx-CP400 (2026-11-06)

Research note. Nothing here is implemented yet; the last section is the plan.

Sources: Renesas SH7724 hardware manual REJ09B0560-0100 section 18.4.6
(`~/Downloads/clemu/refs/SH7724_reference.md`, the text version), the stock
fx-CP400 OS 02.01.7003 disassembled from `~/Downloads/nor-fx-cp400.bin`
(`sh-elf-objdump -b binary -m sh4 -EB --adjust-vma=0x80000000`), casio-emu's
R-standby model (`src/hardware/power/power.c`, `docs/classpad-experimental.txt`),
the CE 6 kernel sources (`kwin32.c`, `SH/mdsh3.c`, `SH/shexcept.src`).

## 1. What the OAL does today, and why it is not "off"

`bsp/classpad/SRC/OAL/OALLIB/power.c` + `SRC/INC/classpad_power.h`:
`OEMPowerOff` saves the INTC masks, TMU and KEYSC state, masks every interrupt,
reprograms KEYSC to scan only the Clear/ON row and column with its interrupt
enabled, clears STBCR's STBY/RSTBY/USTBY bits (ordinary SLEEP) and executes
`sleep` with BL=1 until Clear/ON is released and pressed again. Display and
touch are put to sleep by their drivers' power handlers (`classpad_ddi.cpp`
`PowerHandler`: SLPIN on off, SLPOUT on resume; `classpad_touch.cpp`
`DdsiTouchPanelPowerHandler`; `kbd_pdd.cpp` `ClassPad_PowerHandler`).

That is the SH7305 *sleep* mode: the CPU clock stops, every peripheral clock,
the PLL/FLL and the SDRAM keep running. The stock OS switches the calculator
off with *R-standby*: the core area (CPU, caches, TLB, TMU, most peripherals)
is powered down, only the RCLK domain (RTC, KEYSC, RWDT, CMT), the I/O area,
the on-chip RS memory and a few backup registers stay alive, and the SDRAM sits
in self-refresh. Validated so far only in casio-emu (two cycles in the power
test image); never on the board, and never measured for current.

## 2. Hardware facts (SH7724 manual 18.4.6, SH7305 = same family)

- **Entry**: SR.MD=BL=RB=1; halt the peripheral modules; save what must survive
  in external memory that is held (SDRAM in self-refresh) or in RS memory; write
  back, invalidate and disable the caches; flush and disable the TLB; STBCR:
  RSTBY=1, STBY=USTBY=0; BAR = first instruction to run after wake-up ("RS
  memory or area 0", i.e. the RS memory or the NOR flash); `sleep`.
- **Wake**: NMI, IRQ, CMT, KEYSC or RTC interrupt, or a reset. The core is
  reset (only the core: RESETOUT is not asserted), the PLLs re-lock with the
  settling time ensured by the LSI, execution starts at BAR with SR reset
  (BL=1, MD=1, RB=1, IMASK=F); the wake interrupt stays pending until software
  clears BL. "Interrupts are accepted in R-standby even when BL=1."
- **Held across R-standby**: RS memory, and the BSC, DBSC, INTC, CPG, PFC and
  I/O port registers ("saved when waking up"). Everything else in the core
  area is lost: CPU registers, VBR, caches, UTLB/ITLB, TMU, CCR, MMUCR, PTEH,
  TTB, IL/X/Y on-chip RAM, KEYSC scan configuration, IIC, USB, LCDC state.
- **KEYSC** can cancel software standby, R-standby and U-standby by a key input
  (level) interrupt; the enable bit lives in the KEYSC mode register.
- **RS memory**: on SH7724 2 KiB at P4 0xFD800000. The CP400 OS uses addresses
  up to 0xFD802000 (a stack) and casio-emu maps 16 KiB there, so the SH7305
  part has more; treat 0xFD800000..0xFD803FFF as available, see section 5.
- **BAR** (0xA4150040), **STBCR** (0xA4150020): STBY=bit 7, RSTBY=bit 5,
  USTBY=bit 4. The OS also sets bit 0 (writes 0x21); the SH7724 manual calls
  that bit reserved. Copy it.
- **RWDT** (RCLK watchdog, 0xA4520000 RWTCNT / 0xA4520004 RWTCSR): written with
  the 0x5A / 0xA5 key byte in the upper half of a 16-bit write.

## 3. What the stock OS does (02.01.7003, addresses in P1)

### 3.1 Before standby (standby loop entry, 0x80020e5e)

| step | code | registers | meaning |
|---|---|---|---|
| 1 | 0x80020e64 | VBR = (table) - 0x100 | a vector table for the standby period |
| 2 | 0x80020e66, 0x80020bc0 | PFC 0xA4050110 &= 0xCFFF; 0xA4050112 &= ~3 | pin functions for the wake path |
| 3 | 0x80070e78 | KIUMODEREG (0xA44B0010) = 0; call 0x80070020(1); KIUMODEREG = **0x0200** | KEYSC autonomous standby scan: this is the key-input wake enable |
| 4 | 0x8006fcc4 | (OS bookkeeping) | |
| 5 | 0x80020bcc | IPR at 0xA4080028 &= 0x0FFF | KEYSC interrupt priority 0: the normal KEYSC IRQ is *not* the wake path, the autonomous scan is |
| 6 | 0x80020e8e | INTEVT (0xFF000028) = 0 | |
| 7 | 0x80072856, 0x8002095e | see 3.2 | SDRAM self-refresh, ports |
| 8 | 0x80020be0 | INTC IPR/IMR registers copied to RS memory 0xFD80001C.. | the OS does not trust "INTC is held" |
| 9 | 0x80020e9e.. | PFC 0xA405012A |= 0x80; 0xA405010A = (&0x3FFF)|0x4000; 0xA4050162 |= 4; 0xA4050142 = (&0xFFCF)|0x10 | more pin setup (Clear column input) |
| 10 | loop 0x80020ed2 | wake reason = 0; 0x80080df0(0); 0x80080dde; **SR = 0x70000000**; **STBCR = 0x21**; call 0x80085e74 | |

### 3.2 SDRAM into self-refresh (0x8002095e)

```
FEC10048 = 0xA55A0000      ; keyed companion register
FEC10044 = 0
FEC10044 |= 2              ; SDRAM controller: self-refresh / standby request
A40501C3 |= 0x12           ; PFC byte
A405010E &= 0xCFFF         ; PFC word
```

0xFEC10044/48/4C/50 do not exist on the SH7724 (its BSC ends at CS6BWCR
0xFEC10040 and uses the DBSC for DDR); they are the SH7305's SDR-SDRAM
controller, undocumented. The values below are what the OS writes; copy them
verbatim.

### 3.3 The standby stub (0x80085e74)

```
BAR (A4150040) = 0x80085ec2           ; the nop after the sleep
*(FD8003A4) = r15                     ; caller's stack pointer
r15 = FD80031C + 124                  ; save area in RS memory
push spc, ssr, sr, pr, mach, macl, gbr, vbr, r0..r7_bank, r0..r14
sleep                                 ; 0x80085ec0, R-standby
```

### 3.4 After wake (BAR = 0x80085ec2, SR = reset)

```
sr &= ~0x20000000                     ; RB=0: back to bank 0, BL stays 1
STBCR &= ~0x20                        ; clear RSTBY
BAR = 0xA0000000
RWTCSR (A4520004) = 0xA507            ; watchdog: key A5, value 07
r15 = FD80031C; pop r14..r0, banks, vbr, gbr, macl, mach, pr, sr, ssr, spc
r15 = *(FD8003A4); rts                ; back into the loop at 0x80020ee6
```

Note what the OS does *not* do here: no SDRAM or cache work before returning.
The stack it returns to is in SDRAM, so the SDRAM must already be readable
(the controller exits self-refresh on the first access, or bit 1 of FEC10044
is not self-refresh at all; either way, copying the OS order is safe).

### 3.5 Wake reason and the loop (0x80020ee6..0x80020f00)

`0x80070eb8` reads KIUSTATEREG bit 0 and the key image (KIUDATA), cycles
KIUMODEREG through 0 and 0x0200, and returns non-zero when Clear is down;
`0x8006f886` checks the other sources (INTC 0xA4140024, ports 0xA4050134/
0xA4050162: RTC alarm, USB). If `(reason & 7) == 0` the loop goes back to
step 10 and sleeps again. So a wake-up that is not Clear or an expected event
costs a core restart but no visible change.

### 3.6 Full restore (0x80020f02..: 0x80020d48, 0x80020df0, 0x800204a8)

```
INTC IPR/IMR restored from RS memory FD80001C.. ; BSC CS4BCR..CS6BWCR (FEC10010.., FEC10030..) restored
r15 = FD802000                        ; stack in RS memory for this part
SR = 0x700000F0
0x80080dcc(INTEVT), 0x80080df0(0xCA02), 0x80080d6a   ; OS bookkeeping in RS memory
synco; CCR |= 0x808 (ICI|OCI); icbi; CCR = 0; icbi    ; caches invalidated and OFF
MMUCR = 4 (TI)                                       ; TLB flushed
0x8002069c                                           ; INTC re-init (A4080000 IPR, A4080080 IMR, A4140010/1C)
EXPEVT = 0x20; INTEVT = 0
0x800204a8:
  SR = 0x700000F0
  0x80020916
  0x800209c4:                                        ; clocks, SDRAM out of self-refresh
     PLLCR (A4150024) &= ~6
     FEC10044 = 0
     PFC: A405010A &= 0xC003; A4050162 |= 0x10; A4050142 |= 0x100; A4050195 &= 0xC1;
          A405019E &= 0xF7; A4050194 &= 0xFE; A40501C3 &= 0xDB & 0xED; A405012E &= 0xBF;
          A405010E = (&0xCFFF)|0x1000
     wait until RTC R64CNT (A413FEC0) advanced by > 2   ; about 47 ms, PLL/SDRAM settle
     FEC10048 = 0xA55A0000; FEC10044 = 0x0A09; FEC1004C = FEC10050 = 0xA55A00E3;
     FEC15040 = 0x0040 (16-bit); FEC10048 = 0xA55A0088   ; SDRAM controller re-init
  r15 = 0x8C2361D4                                   ; stack back in SDRAM
  0x80020734; 0x8002046e (MMUCR re-enable)
  VBR = 0x80021824; SR = 0x500000F0                  ; BL=0: the pending wake interrupt is now taken
  0x8006fa54                                         ; interrupt dispatch
```

`0x8002041c` (BSC CMNCR/CSnBCR/CSnWCR = 0x02010013, 0x36DA0400, 0x76DB4400,
0x36DB0400, 0x03C0, 0x24D1, 0x0340, 0x0540; PFC 0xA4050184..; RWDT 0x5A..,
0xA507) is the cold-boot bus setup, also reachable from this path.

## 4. Design for CE under the translator

The OAL is translated LE code running on the translator's exception vectors;
after an R-standby wake the CPU runs native big-endian code at BAR with no
caches, no TLB and reset VBR. So the machine-level part belongs to the
translator (native, in SDRAM at the fixed layout address), and the OAL only
decides *when* and *what wakes us*.

### 4.1 OAL side (`OEMPowerOff`)

1. The kernel has already run `OEMCacheRangeFlush(0,0,CACHE_SYNC_DISCARD|
   CACHE_SYNC_INSTRUCTIONS)` (kwin32.c `CallOEMPowerOff`, SHx only) and will
   run it again plus `OEMInitClock()` after we return. `OEMInitClock` must
   therefore exist in the OAL and redo `OALTimerInit` (TMU is lost); today it
   is only reached at boot.
2. Save the OAL-owned state as now (INTC priorities and masks, KEYSC, CPG
   MSTPCR/FRQCR values, PFC words the drivers touched), set the wake sources:
   KEYSC autonomous scan (KIUMODEREG = 0x0200 with KOUT/KIN limited to the
   Clear row/column, as the OS does), and the RTC alarm when the kernel has one
   (`SetKernelAlarm` -> `OEMSetAlarmTime` in rtc.c; RCR1.AIE and its INTC
   enable are in the held domain).
3. Call the new hypercall **HC_STANDBY** (trapa, r4 = flags). It returns only
   when the translator has brought the core back (section 4.2). Then check the
   wake reason exactly like the OS loop: Clear down (KIUSTATEREG/KIUDATA), RTC
   alarm flag; if neither, call HC_STANDBY again. On a real wake restore the
   OAL state, `g_oalWakeSource`, and return; the kernel re-inits the clock and
   the drivers' power handlers bring the panel (SLPOUT/DISPON), touch IIC and
   keyboard back.

### 4.2 Translator side (`HC_STANDBY`, native)

Entry, with the guest interrupts already masked:

1. Store the guest context (`ctx`, already in SDRAM), the guest's CCR, MMUCR,
   PTEH, PTEL, TTB, the shadow VBR, SR and the translator's own SP/VBR/GBR in
   a save block in RS memory (section 5), together with a magic word and a
   checksum. Write the native resume stub (a few dozen instructions, position
   independent) into RS memory and point BAR at it.
2. `ocbwb`/CCR write-back of everything dirty (the kernel's flush covered the
   guest's writes; the translator wrote `ctx` and the block since), then
   CCR = ICI|OCI, CCR = 0. MMUCR = TI (flush), AT=0.
3. STBCR = 0x21, SR = 0x70000000 (MD, RB, BL), SDRAM into self-refresh as in
   3.2 (FEC10048 = 0xA55A0000, FEC10044 = 0, FEC10044 |= 2), KEYSC priority
   0, INTEVT = 0, `sleep`. Keep the exact OS order; the SDRAM step last.

Resume stub at BAR (native, in RS memory, SR = reset, SDRAM untouched yet):

4. SR = 0x700000F0 (bank 0, BL=1), STBCR &= ~0x20, BAR = 0xA0000000,
   RWTCSR = 0xA507.
5. FEC10044 = 0, wait > 2 ticks of R64CNT (0xA413FEC0), then the SDRAM
   controller re-init words from 3.6 (0xA55A0000 / 0x0A09 / 0xA55A00E3 x2 /
   FEC15040 = 0x40 / 0xA55A0088). Stack stays in RS memory until this is done.
6. Verify the save block magic/checksum; on failure fall through to the
   normal translator entry (a reboot into CE) rather than guessing.
7. CCR = ICI|OCI then the kernel's value (0x105, read from the save block),
   VBR = the translator's vector table, GBR/SP from the save block, MMUCR =
   TLB_FLUSH|TLB_ENABLE, TTB and PTEH from the save block (the kernel keeps no
   wired entries, `mdsh3.c` sets `MMUCR = TLB_FLUSH|TLB_ENABLE` and the miss
   handler in `shexcept.src` refills from TTB with `ldtlb`; the translator's own
   TLB hooks see the misses as usual).
8. Jump to the C resume routine: restore the translator's interrupt-mask
   bookkeeping, mark the code cache as valid (it is in SDRAM, nothing was
   lost), return `RT_ACT_RESUME` from the hypercall with r0 = wake status.
   The guest continues after its `trapa` with BL=0, interrupts still masked by
   the OAL.

The code cache, block tables and all translator data are in SDRAM and
survive; the translator uses no IL/X/Y memory. The UTLB is empty, which the
translator already handles after a full TLB flush.

### 4.3 Kernel side

- CE time: `CurMSec` does not advance in suspend (documented CE behaviour);
  the kernel re-reads the RTC after resume (`OEMGetRealTime`) and refreshes
  the kernel alarm.
- `OEMInitClock` after return: `OALTimerInit(TICK_MAX_MSEC, ...)` again.
- The variable tick's `pfnUpdateReschedTime` pointer survives (it is in RAM).
- Drivers: the DDI driver's resume path already sends SLPOUT and re-pushes the
  frame; verify that it also re-sends the panel init sequence the LCD
  controller needs after a core reset of the display DMAC at 0xB4000000 (the
  panel itself kept its power; the SoC-side LCD interface state is lost).

## 5. RS memory layout

The OS uses 0xFD80001C.. (INTC image), 0xFD80031C..0xFD8003BC (register save),
0xFD8003A4/A8/AC (stack pointer and two magic words its reset path checks) and
a stack below 0xFD802000. Do not touch any of those: the Casio OS must still
cold-boot after a reset. Use **0xFD801000..0xFD8013FF**: 0x000 magic + length +
checksum, 0x010 register block, 0x200 the resume stub. Confirm the real size of
the SH7305 RS memory on the board first (read/write test from the translator:
casio-emu maps 16 KiB but the manual's part has 2 KiB).

## 6. Open questions and risks

- **FEC10044 semantics** (self-refresh vs power-down of the SDRAM clock): copy
  the OS; if the first board test hangs at the first SDRAM access after wake,
  move the self-refresh exit (FEC10044 = 0) before any SDRAM read, which is
  already the plan.
- **STBCR bit 0** and **FEC15040**: unknown meaning; copied.
- **LCD/DMAC**: the display path after wake has never been exercised; the
  panel sleeps through SLPIN and keeps its own settings, the SoC side must be
  re-initialised by the DDI driver's resume path.
- **USB**: the OS treats a cable as a wake reason (0x8006f886); ignore for now.
- **Current draw**: with the core off this should match the stock OS's "off"
  (weeks on a battery); the only way to know is a measurement on the board.
- **Emulator fidelity**: casio-emu models R-standby for the ClassPad: KEYSC
  wake needs KIUMODEREG = 0x0200 and a key outside the idle state, RTC wake a
  pending enabled RTC request, then it restarts at BAR with SR = 0x700000F0
  and swaps the register banks. It does not model the SDRAM controller or the
  cache loss, so a passing emulator run proves the control flow and the save/
  restore, not the SDRAM sequence.

## 7. Plan

1. `dbt/guest/standby.c`: a bare guest (no CE) that saves a pattern in SDRAM,
   requests HC_STANDBY with the Clear wake, is woken by the emulator's injected
   Clear press and checks the pattern, the TMU1 profile clock and the RTC. Run
   under casio-emu; this exercises steps 1-8 and the RS memory layout.
2. Translator: HC_STANDBY, the save block, the resume stub (`rt/standby.S`),
   `dbt_resume_c()`; `DEVICE=1` only (the emulator build keeps it too for the
   guest test).
3. OAL: `OEMPowerOff` rewritten on top of the hypercall, `OEMInitClock`,
   RTC-alarm wake, `OALIoCtlHalPresuspend` unchanged; the kernel's own
   `OEMCacheRangeFlush` calls stay.
4. Board: first with the LCD left on (no DDI power handler) to see the resume
   path work at all, then with the drivers' handlers, then measure the current
   with the panel off.
