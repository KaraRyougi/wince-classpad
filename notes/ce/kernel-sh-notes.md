# CE 6.0 R3 kernel, SH4 sources: what the translator and the OAL must honour

Read from `PRIVATE\WINCEOS\COREOS\NK` (copied to ~/Downloads/PRIVATE, not in
the repo): `KERNEL/SH/shexcept.src` (2597 lines, the whole exception, API-call,
interrupt and scheduler glue), `KERNEL/SH/mdsh3.c` (SHInit, exception
classification, FPU switching), `KERNEL/SH/vmshx.c` (page tables, ASID),
`KERNEL/SH/shfloat.c` (software completion of FPU exceptions),
`KERNEL/SH/intrlock.src`, `LDR/SH/shstart.src` (KernelStart),
`OEMMAIN/oemglobal.c` (the OEMGLOBAL table the OAL fills), `INC/ksshx.h`,
`INC/nkshx.h`, `INC/vmshx.h`, `KERNEL/SH/shx.h`.

## 1. Exception entry and register bank 1 (settles the docs/05 question)

- VBR = `ExceptionBase` in kernel.dll. Vectors: +0x100 `APICall` (every
  general exception), +0x400 `TLBMissHandler`, +0x600
  `InterruptExceptionHandler`. All three start in bank 1 with BL=1, as the
  hardware leaves them.
- **Bank 1 holds live kernel state for the life of the system:**
  - `r4_bank1` = pCurThread (`ldc r14, r4_bank` at every thread switch,
    `SetCPUGlobals`);
  - `r5_bank1` = pointer just past `CtxPsr` of the current thread's CONTEXT;
    the entry code stores the interrupted registers with `stc rN_user, @-r5`
    and the dispatch tail (`geh65`) re-establishes it with
    `ldc @r5+, SPC; ldc @r5+, SSR; rte`;
  - `r7_bank1` = `SH3CTL_BASE` 0xFF000000, set once in `NKStartup`, never
    reloaded; every CCN register access in assembly is `@(off, r7)`.
  - `r0-r3`, `r6` of bank 1 are scratch and are written before being read in
    all three handlers.
- The translator's exception stubs therefore may not use bank 1 as free
  scratch. Fixed today: the vector thunks save bank 1 first (r15 is the only
  free register, the hardware keeps a copy in SGR), the C side swaps the
  banks when the guest itself was in bank 1, and the exit restores bank 1
  last (`bank` guest, 14 checks). When delivering an exception to the guest
  handler, `r1_bank1` carries the jump target; every kernel handler writes
  r1 before reading it.
- SR values the kernel uses: `PR_B1_BK` 0x70008000 (MD, RB, BL, FD),
  `PR_B0_IE` 0x40008000, `PR_B0_IM` 0x400080F0, `PR_B0_BK` 0x50008000. FD
  (bit 15) is set everywhere: the FPU is off until a thread touches it.
- Every `rte` in the kernel has `nop` in its delay slot (geh65,
  TLBMissHandler, ieh55, ieh70, CaptureContext x2, RtnToUMode).
- SPC/SSR are always reloaded explicitly before `rte` (`ldc @r5+, SPC`,
  `ldc r13, SPC`, `ldc r3, SPC`...). The kernel never expects SPC/SSR to
  survive a nested exception, which matters because after the translator
  resumes from one of its own exceptions (lookup miss, hypercall) the
  hardware SPC/SSR hold host values.
- Nested general exceptions (cNest != 1) build a fake thread on the kernel
  stack (geh33). Interrupts run on `KStack` with SR = PR_B0_IE | prio<<4.
- **OEMInit runs in bank 1 with BL=1**: `NKStartup` calls `SHInit` with
  SR = PR_B1_BK and SHInit calls `OEMInitDebugSerial`, `OEMWriteDebugString`
  (sign-on) and `OEMInit` before the kernel ever lowers BL. A `trapa` with
  BL=1 is a manual reset on SH-4A (casio-emu models this), so the OAL's
  debug output cannot be a hypercall there: it goes through a RAM ring the
  translator drains (`CLASSPAD_DBG_RING`, docs/05).

## 2. API calls (PSL trap convention)

- `FIRST_METHOD` 0xFFFFFE01, `SYSCALL_RETURN` 0xFFFFFDFF, `API_MAX` =
  FIRST_METHOD - 0x7FFF*2 = 0xFFFEFE03. A call is a `jsr`/`jmp` to an odd
  address A >= API_MAX; iMethod = (A - FIRST_METHOD) >> 1 (negative for
  returns: -1 = trap return, -30 = PerformCallBack).
- Detection in `APICall`: EXPEVT == 0x0E0 (address error on load/ifetch),
  SPC odd and >= API_MAX -> `SyscallHandler` with r0_bank1 = iMethod,
  r2_bank1 = SSR, r4_bank1 = pCurThread; the API arguments are the bank-0
  r4-r7. Anything else: EXPEVT|0x20 == 0x60 -> TLB miss, else general.
- The translator already synthesizes EXPEVT 0x0E0 with SPC = the odd target
  and TEA = target for unfetchable pcs (`apicall` guest). The non-API address
  error path uses TEA (`IsKModeAddr(info)`), so TEA must be set: it is.
- `MDHandleHardwareException` classifies by cause >> 5: 2/3 TLB miss, 4
  initial page write, 5/6 protection, 7/8 address error, 9 FPU exception
  (EXPEVT 0x120), 0x40/0x41 FPU disable (EXPEVT 0x800/0x820) ->
  `SwitchFPUOwner`, 11 breakpoint (trapa), 12/13 reserved/slot illegal,
  15 user break.

## 3. ASID switching (hypercall FA site)

- `SHSetASID(dwASID)` = `mov.l r4, @(MMUPTEH, r7)`: a plain 32-bit store to
  0xFF000000. Callers in vmshx.c: `MDSwitchActive` (thread/process switch,
  from the scheduler's `SetCPUASID`), `MDSwitchVM`, `MDSetCPUASID`.
- ASIDs 1..0x7F for processes, `KERNEL_ASID` 0x80 OR'd with the current VM's
  ASID while the kernel is the active process; a process whose ASID ran out
  (0) forces a full TLB flush on each switch (`OEMCacheRangeFlush(NULL, 0,
  CACHE_SYNC_FLUSH_TLB)`).
- `MMUTTB` is software-only on SH-4: written by `MDSwitchVM`, read by the
  TLB miss handler to find the page directory.
- Translator: the FA hypercall is not inserted into the kernel. The
  translator already notices PTEH at `rte`/`ldc sr`; with the device-aware
  translation of kernel.dll (section 7) it sees the PTEH store itself.

## 4. TLB

- `TLBMissHandler` (+0x400): TEA -> page directory (`MMUTTB`) -> page table
  -> entry; writes `MMUPTEA` (assistance bits from PTEL bit 9 and bits
  29-31), `MMUPTEL`, `ldtlb`, `rte`. PTEH is what the hardware loaded. User
  mode may read the shared heap (>= 0x70000000) through the kernel page
  directory with the write bit cleared; kernel addresses from user mode
  fault.
- `LoadKPage` (SHInit): PTEH = 0x5800 | ASID, PTEL = KData page | 0x15A
  (user read-only, 4 KiB, cacheable, shared, valid), `ldtlb`: the kernel data
  page is visible to every process at 0x5800 and the interlocked API code
  (`InterlockedAPIs`, 8-byte aligned, restartable by masking the PC) is
  copied to 0x5B80-0x5C00 of it. User code executes from a shared page:
  the translator's SH=1 handling (`aspace` guest) covers it.
- TLB invalidation goes through the OAL: `InvalidatePages` ->
  `OEMCacheRangeFlush(addr, len, CACHE_SYNC_FLUSH_TLB)`, plus the ASID-0 case
  above and `NKSetStoreQueueBase` (CACHE_SYNC_ALL). **Hypercall F9 lives in
  our `OEMCacheRangeFlush`; no kernel change.**
- SHInit MMU setup: `MMUTEA = 0; MMUTTB = ppdir; MMUPTEH = 0; MMUCR =
  TLB_FLUSH|TLB_ENABLE`. Page directory: section entries for P1/P2
  (`pte[0x200..0x300]` = phys | valid), `pte[0]` -> a page table whose entry
  5 is the KPage.
- PTEL bits used: valid 0x100, 4 KiB 0x10, cacheable 8, dirty 4, shared 2,
  write-through 1, protection 0x60 (0 kernel RO, 0x20 kernel RW, 0x40 user
  RO, 0x60 user RW).

## 5. Cache (hypercall F2 site)

- `OEMCacheRangeFlush(pAddr, len, flags)` is the only cache entry point the
  kernel calls: `CACHE_SYNC_INSTRUCTIONS` after it writes code (loader,
  JIT), `CACHE_SYNC_DISCARD/WRITEBACK` for DMA-style users, the TLB flags
  above. **Hypercall F2 (drop translations for the range) lives here.**
- `SHInit` writes CCR directly three times (0, CACHE_FLUSH 0x808,
  CACHE_ENABLE | 0x80000105) and reads it back for the "CCR=... INTEVT=..."
  message. `DoSetRAMMode` (SetRAMMode API, unused by default) writes the
  cache address array at 0xF4001000 and toggles CCR bit 0x20. The SH7305 CCR
  layout differs from the SH7750R's (no EMODE bit 31; SH-4A: OCE 1, WT 2,
  CB 4, OCI 8, ICE 0x100, ICI 0x800); the stores are byte-swapped by the
  translator as well (section 7).

## 6. FPU

- `MDSetupThread`: `Psr = SR_FPU_DISABLED | mode`, `Fpscr = 0x40000` (DN).
  The first FPU instruction of a thread raises EXPEVT 0x800 (FPU disable),
  cause id 0x40 -> `SwitchFPUOwner`: `SaveFloatContext(old)` /
  `RestoreFloatContext(new)` with FD cleared, `lds #0, fpscr`, `frchg`
  (0xFBFD), 32 x `fmov.s`, `sts fpscr/fpul`; then the thread's PSR loses FD.
  `RtnToUMode`/`CaptureContext` set FD unless the thread is
  `g_CurFPUOwner`. `FPUFlushContext` on paging and Get/SetThreadContext.
- EXPEVT 0x120 (FPU exception) -> `HandleHWFloatException` recomputes the
  instruction in software from the context (shfloat.c). Only reached if the
  hardware FPU signals an enabled IEEE exception.
- **Translator work item:** emulate SR.FD. Today `fpuemu` executes every FPU
  instruction regardless of FD, so the lazy context switch never triggers
  and all threads would share one FPU register file. The FPU helper must
  check the guest SR's FD bit and deliver EXPEVT 0x800 (vector 0x100) when
  set; the kernel then clears FD for the owner. The save/restore sequences
  use `frchg`, `fmov.s @-Rn` / `@Rn+`, `lds/sts fpscr/fpul`: all through the
  helper already.

## 7. CCN (P4) register access from kernel.dll: the byte-swap problem

The translator swaps every 16/32-bit load and store. Our own code (OAL,
test guests) uses pre-swapped accessors, but kernel.dll reads and writes
the on-chip control registers directly and is not rebuilt. Sites:

| File | Access |
|---|---|
| shexcept.src `APICall` | `mov @(EXPEVT,r7)` read |
| shexcept.src `geh36` | `mov.l @(EXPEVT,r7)`, `@(MMUTEA,r7)`, `@(TRPA,r7)` reads |
| shexcept.src `TLBMissHandler` | `@(MMUTEA,r7)`, `@(MMUTTB,r7)` reads; `@(MMUPTEA,r7)`, `@(MMUPTEL,r7)` writes; `@(EXPEVT,r7)` read |
| shexcept.src `_LoadKPage` | `@(MMUPTEH,r3)` read, `@(MMUPTEH,r3)` and `@(MMUPTEL,r3)` writes |
| shexcept.src `InterruptExceptionHandler` | `@(INTEVT,r7)` read |
| shexcept.src `ieh60` | `@(TRPA,r7)`, `@(EXPEVT,r7)` writes (break button) |
| shexcept.src `_SHSetASID` | `@(MMUPTEH,r7)` write |
| shexcept.src `_DoSetRAMMode` | cache array 0xF4001000/0xF4003000 writes, `@(CCR,r1)` read-modify-write |
| mdsh3.c `SHInit` | `CCR` x3 writes + read; `MMUTEA`, `MMUTTB`, `MMUPTEH`, `MMUCR` writes |
| mdsh3.c `DumpFrame` | `MMUPTEL`, `MMUPTEH`, `MMUCR`, `MMUTTB` reads |
| vmshx.c `MDSwitchVM`, `MDSetCPUASID` | `MMUTTB` writes |

Values written through the swapped path land byte-reversed in the register
(SHSetASID would put the ASID into PTEH bits 31:24, i.e. hardware ASID 0 for
everything), and values read come back reversed (EXPEVT 0x0E0 reads as
0xE0000000, so no API call would ever be recognised).

Chosen fix (translator side, no kernel rebuild): **device-aware translation
for blocks inside kernel.dll's address range** (known from the ROMHDR TOC
the loader already parses). In those blocks every 16/32-bit memory
instruction first tests the top byte of the effective address; 0xFF (P4
on-chip registers) takes a helper that performs the access unswapped and
also observes PTEH/PTEL/PTEA/MMUCR traffic, which replaces the FA/F9
hypercalls for the kernel. Cost: ~5 instructions per memory op in kernel.dll
only. Fallback if that proves slow: patch the sites above (and the `shx.h`
REG32BIT macros) and rebuild kernel.dll in the VM.

## 8. Interrupts

- `InterruptExceptionHandler`: `ExceptionTable[INTEVT >> 5]` (16 system +
  496 interrupt entries) gives the ISR, `IntrPrio[(INTEVT >> 5) - 16]` the
  IMASK level to run it at. The OAL installs ISRs with
  `HookInterrupt(irq, fn)` where irq = (INTEVT - 0x200) / 0x20, and sets
  `IntrPrio[irq]` (HARP's `HookAndSetPriority`). `dwSHxIntEventCodeLength`
  (`SH4_INTEVT_LENGTH`) bounds irq: SH7305 codes (TMU0 0x400 -> irq 16,
  KEYSC 0xBE0, RTC 0x480...) fit.
- The ISR returns a SYSINTR: NOP -> `rte`; a device SYSINTR sets a bit in
  `PendEvents1/2`; RESCHED or a pending bResched at nest level 1 ->
  `InterruptResched` -> scheduler. The kernel masks SR.IMASK to the ISR's
  priority while it runs, so IPR level and `IntrPrio` must agree.
- Sleep work-around: if `fInIdle` and the halfword at SPC is 0x001B (`sleep`)
  the handler advances SPC by 2 (a swapped 16-bit read of LE code: correct).
- `OEMIdle` is entered with SR = PR_B0_BK (BL=1) and must `sleep`; the CPU
  accepts interrupts in sleep regardless of BL. Works under the translator
  (`tmu` guest).
- `OEMNMI` (vector 0x1C0) must exist and return SYSINTR_NOP.

## 9. Boot sequence (for reading the first run's output)

`StartUp` (nk.exe, our OAL) -> `KernelStart` (nkldr: SR = PR_B1_BK, CCR =
0x808 via P2, `KernelRelocate(pTOC)`, `SetOsAxsDataBlockPointer`,
`FindKernelEntry`) -> kernel.dll `NKStartup(pKData, OEMInitGlobals)` ->
`SHInit` on the uncached alias (CCR off/flush, zero KData, exchange globals,
`InitPageDirectory`, KITL dll if present, `OEMInitDebugSerial`, sign-on
string, MMU on + KPage, interlocked code copy, CCR on, "CCR=xxxx INTEVT=xxxx"
message, `OEMInit`, `KernelFindMemory`) -> VBR = ExceptionBase, SR =
PR_B0_IE, `KernelInit` -> `resched` (first thread).

The first message from a working OAL is therefore the kernel sign-on
"Windows CE Kernel for SH4 Built on ..." followed by "CCR=...".

## 10. WaitOneMore leaves an interrupt-event proxy linked when a timed wait expires between its KCalls (kernel hole, patched)

- **Symptom** (2026-11-04/05, image with the KEYSC driver): right after the
  hello window's first paint the keyboard IST's
  `WaitForSingleObject(hevInterrupt, 20)` returns WAIT_FAILED on every call,
  `GetLastError()` = 6 (ERROR_INVALID_HANDLE), and the thread runs at
  priority 145 for ever, starving GWES. Deterministic in the emulator: six
  runs all stalled after the 1496th link of the event's `pProxList`.
- **Mechanism** (`KERNEL/kcalls.c`, QFE 2013-03-29 / KB2826824, lines
  2217-2385): `DoWaitForObjects` (schedule.c) sets `lpce = NULL` for an
  interrupt event and calls `KCall(WaitOneMore)` in a loop. The first call
  has `lpPendProxy` set: it links the proxy on `pevent->pProxList`, sets
  `lpProxy = pProx` and returns with `dwContinue` still set. The later
  calls have `lpPendProxy == 0` and `dwPendTime != INFINITE`, so they take
  the last branch: `if (!dwPendTime || (int)(GETCURRTICK() - dwPendWakeup)
  >= 0) { retval = WAIT_TIMEOUT; goto exitfunc; }` and otherwise one
  `SleepOneMore` step per KCall. `exitfunc` hands the proxies to `lpce`
  (NULL here) and clears `lpProxy`; the proxy stays on the event. The next
  wait finds `pevent->pProxList` set and goes to `waitbad`: the kernel's
  "more than one thread waiting on the same interrupt event" check, error
  6. (DoWaitForObjects has a DEBUGCHK for exactly this; retail builds drop
  it, so the hole is silent.) The three other places that end an
  interrupt-event wait special-case `!lpce && lpProxy` and do
  `pevent->pProxList = 0; pprox->pQDown = 0`: NextThread's sleep expiry
  (~1172), RunqDequeue's sleep chain (~260) and the thread-kill path
  (~982). `exitfunc` is the one that does not.
- **Trigger**: more than dwTimeout+1 ms of tick between two consecutive
  KCalls of the waiting thread. On hardware that takes a 21 ms preemption
  at a KCall boundary, so it is rare; under the translator the first
  translation of new code (the paint path) is charged as guest time, so
  it happens every run.
- **Evidence** (casio-emu `HLE_WRITEWATCH` on THREAD 8cbb92b8: lpProxy
  +0x34, dwLastError +0x38, pCrabPth +0x204; `HLE_RAMSAVE_AT` snapshot at
  tick 2e6): 1495 times link (PR 810122be, WaitOneMore) then clear by
  NextThread (PR 81010f8e); the 1496th link is cleared with PR 810122be
  (exitfunc), the event keeps `pProxList = d03ffc18`, dwLastError = 6 on
  every wait after that, pCrabPth took non-zero values from SleepOneMore
  (PR 8101251c). Snapshot: IST running, lpProxy 0, lpPendProxy d03ffc18,
  dwPendWakeup 0xe8c4 > CurMSec 0xe8af.
- **Why a driver cannot recover on its own**: the stale `pProxList` points
  at a dead slot of the thread's stack (`_proxys[]` of DoWaitForObjects).
  The next interrupt on that SYSINTR (`WakeOneThreadInterruptDelayed`) and
  `InterruptDisable` (`EventModIntr` with EVENT_SET) both dereference it as
  a PROXY and write through its `pTh`. Re-creating the event from the
  driver therefore goes through that code with garbage; the kernel fix is
  the fix. Drivers can only avoid timed waits on interrupt events.
- **Fix**: `bsp/kernel/kcalls-waitonemore.patch` adds the same unlink to
  `exitfunc`, guarded by `lpe->pProxList == pProx` (`bsp/kernel/README.md`
  for the rebuild). `kbd_pdd.cpp` waits INFINITE and rescans from a poll
  thread. The common touch MDD waits on its interrupt event with
  `gdwTouchIstTimeout`, which the DDSI used to set to the sample period
  while the stylus is down (same pattern); `classpad_touch.cpp` now leaves
  it INFINITE and wakes the IST from its own sample thread with
  `SetEvent(hTouchPanelEvent)`, which the kernel routes through
  `EventModIntr` like the ISR (syncobj.c ForceEventModify, `phdIntr`).
  So both drivers are safe on an unpatched kernel too.
- **Tools that found it**: write watches on the thread's fields and on
  `IntrEvents[SYSINTR - 8]` (KData + 0x1a0 + 4 * index), a RAM snapshot
  scanned for the thread by `bBPrio`/`bCPrio`, and the fact that the
  emulator's runs are deterministic, so counts line up between runs.

