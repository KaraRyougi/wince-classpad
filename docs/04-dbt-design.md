# Same-ISA dynamic binary translator: little-endian SH-4 on big-endian SH7305

Status: design + core implementation in progress (2026-10-22).

## Idea

Guest and host are both SH-4. The only differences are byte order and the
missing FPU. So the translator copies almost every instruction verbatim and
rewrites four classes: memory accesses (byte swaps), FPU instructions (calls
into softfloat), branches (block linking and guest-PC bookkeeping) and a few
privileged instructions (VBR, SR, RTE). Expected cost 1.5x-3x over native,
i.e. an effective 40-70 MHz little-endian SH-4 at 118 MHz. It runs as an
ordinary big-endian program launched by a gint add-in; no hardware change,
no emulator change beyond the DSP transfer instructions.

The analogy is VMware's original x86 binary translator: same ISA, pass
through the innocent instructions, rewrite the sensitive ones.

## Memory model: byte-identical to a little-endian machine

Guest memory holds exactly the bytes a little-endian SH-4 would hold. Hence:

| Guest access | Host sequence |
|---|---|
| `mov.b` any form | unchanged |
| `mov.w @X,Rn` | load; `swap.b Rn,Rn`; `exts.w Rn,Rn` |
| `mov.w Rm,@X` | `swap.b Rm,rX`; store rX (rX = the block's borrowed register, see below; without one: spill rT, swap into rT, store, restore) |
| `mov.l @X,Rn` | load; `swap.b`; `swap.w`; `swap.b` on Rn |
| `mov.l Rm,@X` | `swap.b Rm,rX`; `swap.w rX,rX`; `swap.b rX,rX`; store rX; a second store of the same Rm reuses rX (1 instruction) |
| `mov.w/l @(disp,PC),Rn`, `mova` | resolved at translation time (guest literal, guest PC) |
| `sts.l pr,@-Rn` / `lds.l @Rm+,pr`, `stc.l`/`ldc.l` | expand to register form plus the swapped 32-bit access |
| `movca.l` | as a 32-bit store, emitted with `movca.l` |
| `mac.w`/`mac.l`, `tas.b` edge cases | C fallback interpreter (one instruction) |

Consequences accepted on purpose:
- NK.bin loads unchanged, debuggers and dumps show a normal LE image.
- Peripheral registers: 32-bit MMIO values would be swapped. All MMIO code is
  ours (kernel exception path, OAL, drivers) and uses `MMIO_RD32/WR32` macros
  that pre-swap, so the translated access lands correct. Prebuilt CE binaries
  never touch MMIO.
- 16-bit framebuffer pixels are stored LE; the display DMA reads BE. The
  display driver keeps a shadow framebuffer and converts dirty rectangles.

The alternative "XOR address" model (casio-emu's `page[j ^ 3]`) makes 32-bit
accesses free but costs an address computation on every byte and halfword
access. CE is UTF-16 everywhere, so halfword traffic is heavy; the swap model
was chosen. Revisit with real instruction-mix data from the emulator.

## Scratch registers

Translated sequences need temporaries without disturbing the 16 guest GPRs.
SR.DSP is kept set and the DSP transfer registers A0, X0, X1, Y0, Y1 are used
as a 5-slot spill area via `lds Rm,X0` / `sts X0,Rn` (one cycle each). SH-4
code never touches them. Encodings (SH-DSP): `lds Rm,DSR/A0/X0/X1/Y0/Y1` =
`0100mmmm 0110/0111/1000/1001/1010/1011 1010`, `sts` = `0000nnnn ... 1010`.
Note the DSR slot is the SH-4 FPSCR encoding, so guest `lds/sts fpscr` must
never pass through (they are routed to the FPU emulation anyway).

A translated guest instruction may spill at most two GPRs. The metadata for
that instruction records (gpr, slot) pairs so the exception handler can undo
the spill if a fault hits inside the sequence (see "Exceptions").

**Borrowed register** (2026-11-03): most blocks leave several GPRs untouched,
so the translator's pre-pass (which also decodes the block ahead and keeps
the opcodes) picks the lowest of r1-r14 that no instruction of the block
reads or writes and parks it in A0 for the whole block: `lds rX,A0` is the
block's first instruction, one spill record spans the block, and every exit
(direct jump, inline cache, `rte`) restores it first. Stores, `sts.l/stc.l`
and `lds.l/ldc.l`, and the device-aware address test then use rX without
a spill of their own, and the translator remembers which register's
byte-swapped value rX holds (killed when that register is written, at
conditional branches, and across helper calls), so repeated stores of one
register cost a single instruction each (`ZeroPage`: four `mov.l r0,@..`
per iteration, 24 host instructions before, 7 after). Helper calls pause the
borrow (restore before, park again after): helpers want the exact guest
registers, and their exception deliveries take the stub context as it is.
A0 is therefore the borrowed register's slot alone (the third scratch of a
`stc.l sr` in an indirect branch's slot moved to Y0), and no other sequence
ever picks the borrowed register as a scratch: the `jsr` fast form keeps
its PR scratch spilled across the slot and restores it in the inline
cache's delay slot, after the exit's restore, which with rX as that scratch
put the swap value back over the guest value (found as a Release boot that
reached idle in a third of the time, with the registry hashing gone). The pre-pass only
borrows when it pays: two instructions per user against the entry, two per
helper call and one for an indirect exit. `dbt_block.borrow` names the
register, -1 for none; `dbt_env.no_borrow` turns it off for tests.

Fallback if a target lacks the DSP registers: push/pop on the guest stack
(`mov.l rT,@-r15` / `mov.l @r15+,rT`), slower and only valid when r15 is a
real stack.

## Code cache and blocks

- A block starts at a guest PC and ends after the first branch (including
  its delay slot), after `rte`, `trapa`, `sleep`, `ldc Rm,sr` or at 64 guest
  instructions.
- Each block has: guest start PC, host code, a per-guest-instruction table of
  host offsets plus spill records, and link slots for its outgoing direct
  branches.
- Lookup: direct-mapped hash `(pc >> 1) & (N-1)` with tag, backed by a full
  table on miss. Direct branches are patched to the target host code once it
  exists (block chaining). Indirect branches (`jmp`, `jsr`, `rts`, `braf`,
  `bsrf`, `rte`) go through the lookup stub with the target in X0.
- Guest code reads during translation go through a probe; a TLB miss aborts
  the translation and is delivered to the guest as an instruction fetch miss
  at that PC. Translation runs with interrupts masked.
- Invalidation: the OAL's cache flush routine (which the CE kernel calls after
  writing code) issues `trapa #0xF2` (hypercall) with the range; the
  translator drops every block overlapping it. Hypercalls use TRA values
  0xF0-0xFF, which CE does not use.

## Branches and PC

Guest PC is virtual. `bsr`/`jsr`/`bsrf` write the *guest* return address into
PR (via a literal); `rts` therefore goes through the lookup stub. `braf`/`bsrf`
add the register to the guest PC+4 constant. `bt`/`bf` without delay slot
become host `bt/bf` to the linked target or an inverted skip around a long
jump. `bt/s`/`bf/s`: the delay slot always executes on SH (taken or not), so
the slot is emitted first; if the slot writes T, T is saved with `movt` and
tested afterwards.

The delay slot of a rewritten branch is emitted before the jump sequence. For
indirect branches the target is captured into X0 first (so a slot that
modifies the target register, `jmp @r0` with `mov r1,r0`, still works), and
the slot is translated with shifted scratch slots (X0->X1, X1->Y1, Y1->A0) so
its spills cannot clobber the target. A slot that needs a helper call (FPU,
`mac`) owns all five DSP slots, so for it the target is captured *after* the
slot; that requires the slot not to write the target register (helpers never
write PR, so `rts` is always fine). `jsr @r1 ; fmov.s @r1+,fr0` is rejected
as `DBT_ERR_UNSUPPORTED` (never emitted by compilers).

## FPU

`fpuemu_is_fpu_op` recognises the instruction; the translator emits a call to
a per-opcode helper with the guest instruction word as argument. Helpers
read/write the emulated FPU register file (per thread, pointed to by a
translator global the OAL updates on context switch, or simply a single file
if the guest kernel's own lazy-FPU logic is replaced by nothing). Roughly
50-100 cycles per FPU instruction instead of 500-800 by trapping.

## Privileged instructions

| Guest | Translation |
|---|---|
| `ldc Rm,vbr` / `stc vbr,Rn` | shadow VBR in translator state |
| `ldc Rm,sr` | helper: keep host MD=1 and DSP=1, record guest MD in shadow, apply BL/RB/IMASK/T etc. |
| `stc sr,Rn` | pass through, then patch MD bit from shadow |
| `rte` | stub: guest SPC -> host address lookup, force SSR.MD=1 and DSP=1 (shadow keeps guest MD), then real `rte` |
| `ldtlb`, `ldc gbr/ssr/spc/Rn_bank`, `stc ...`, cache ops, `sleep`, `trapa` | pass through |

Running the guest's user mode with real MD=1 loses inter-process memory
protection. Accepted for now; a later version can map the code cache into
guest user space with wired TLB entries.

## Exceptions and interrupts

The real VBR points to translator handlers (general 0x100, TLB miss 0x400,
interrupt 0x600). On entry:

1. Read SPC (a host address). Find the block containing it and the metadata
   range it belongs to; compute the guest resume PC (below). Undo every spill
   still open at that offset (`start < off <= end`, outermost last).
2. Set SPC = guest resume PC, SSR.MD = shadow MD. EXPEVT/INTEVT/TEA/TRA are
   real hardware registers that the guest reads with the pre-swapped MMIO
   accessors (`guest/mmio.h`).
3. Hypercall (`trapa` with TRA 0xF0-0xFF): handle in C and return.
4. Jump to the translation of guest VBR + offset.

Resume metadata per range (`dbt_imeta`):

| kind | resume PC |
|---|---|
| `META_INSN` | restart the guest instruction, unless the range's memory op has committed (see below) |
| `META_TARGET_CONST` | a branch whose slot has executed: the constant target |
| `META_TARGET_X0` | same, target parked in X0 by the branch |
| `META_COND` | after a `bt/s`/`bf/s` slot: by SSR.T, `target` if T=1 else `fallthrough` |
| `META_COND_REG` | same, but the slot wrote T, so the pre-slot T is bit 0 of a scratch register (`fix_reg`, read before the spill undo) |
| `META_RTE` | guest `rte` pending: not resumable (BL=1 code) |

A memory instruction has side effects that must not run twice (`@Rm+`,
`@-Rn`, a load into its own base register), so each range records a
*commit point*: the host offset of the memory op. An exception with SPC past
it means the op has executed and only the emitted tail (byte swaps of the
loaded value, the `lds/ldc` of a control-register load, the scratch restore)
is missing. The runtime finishes that tail in software (`fix_kind`:
`FIX_SWAP16`, `FIX_SWAP32`, `FIX_CR_BASE + cr`, applied to whatever part of
the tail has not run yet) and resumes at the following range: the next
instruction, or for a slot the branch boundary. Exceptions after a committed
`ldc.l @Rm+,sr/ssr/spc/Rn_bank` halt: such code runs with BL=1 and cannot be
interrupted.

Exceptions in a block's exit stubs (the first execution of each direct
branch) resume at the link's target, with r1 restored from Y0.

A helper call commits its range at the call (the jmp's delay slot): the
return address now carries the borrow's re-park, a word of the helper's own
range, so without the commit an interrupt in the helper's exit tail
(resumed at the return address) would run the helper instruction again:
`fmov @Rm+`, `mac`, a device access. Committed, it goes on with the next
range; the helper prologue window resumes at X1 - 2, before the commit, and
restarts it as before. For `braf`/`bsrf` with a helper slot the range after
the slot is `META_TARGET_REG` with the PC offset in `target` (resume =
Rn + PC + 4), since X0 is loaded only after the slot.

The borrowed register is just another spill record, open from the block's
first instruction to its end: an exception anywhere in the block puts the
guest value back from A0. The record is not closed at the exits' restores
(the block goes on along the other path of a conditional exit), and an undo
after a restore has already run is harmless since A0 keeps the value. The
only thing that must never meet a borrow is a helper's own exception
delivery, hence the pause around helper calls.

The real `rte` in the stubs has a `nop` slot: SH-4 switches the register
bank before the slot executes, so an `sts A0,r2` there writes the *guest's*
r2 (found the hard way with the TMU test).

If SPC is inside translator C code (translation in progress), the exception
can only be a TLB miss from the code probe; longjmp out and deliver as above.
The same for a helper's own guest memory access (`fmov`, `mac`, the
device-aware fallback): `dbt_fpu_c`/`dbt_fallback_c` note their return
address (X1) in `g_helper_ret`, and a fault with SPC in C while it is set
is delivered at the instruction the helper serves (found through the
return address like the FPU-disable exception), with the hardware's
EXPEVT/TEA and the exact stub context: the kernel maps the page and the
instruction runs again (the helpers read before they post-increment).
First hit by GWES's font metrics touching an unmapped page of gwes.dll
(2026-11-04); counted as `probe_faults=a/b`, b being helper faults.

## Memory layout on the device

Reserve the top of RAM for the translator: code cache (1 MiB), tables and
stacks. The tables are 4096 block headers (0.7 MiB), a 1.25 MiB arena holding
each block's resume metadata and spill records (about 240 bytes per block in
practice; the translator writes a fixed-size scratch block and the runtime
copies the used entries), the 64 KiB hash and ~60 KiB of code: 1.3 MiB in
all, so the translator needs about 3.5 MiB with the cache. Guest RAM is told
to end below it (`MainMemoryEndAddress`). On 16 MiB units (fx-CP400,
fx-CG100) that leaves about 12 MiB for CE; 8 MiB units run headless only.
The layout (docs/05, defined in `dbt/Makefile`: `GUEST_RAM_HI`,
`IMAGE_BASE`, `CACHE_*`, `BLOCKS`, `ARENA_KB`) gives the translator 5 MiB
(8192 block headers, a 2.5 MiB arena) plus a 2 MiB cache and the guest
8 MiB since 2026-11-05: the Standard Shell boot hit the 4096-block cap 28
times before POSTINIT (115k translations) with the cache only 0.9 MiB
full, so the block cap, not the cache bytes, was the limit. Until then the
translator had 2.5 MiB plus a 1 MiB cache and the guest 11.5 MiB
(2026-11-03: the cache was 2 MiB and never filled past 0.6 MiB before the
2048-block cap; the room went to 4096 blocks). A layout change rebuilds
guests and image (a stamp file) and needs config.bib's RAM line to follow.

## Test plan

Guest tests are little-endian bare-metal SH-4 programs (`sh-elf-gcc -ml
-m4-nofpu`) in `dbt/guest/`, loaded by the host add-in and run under the
translator in casio-emu (headless, output through the 0xFFFFF000 debug port
via hypercall 0xF1):

1. hello: hypercall print.
2. mem: mixed-width loads/stores, literal pools, pr save/restore, structs.
3. exc: guest VBR, `trapa`, `rte`, SPC/TRA values, nested exception.
4. irq: TMU interrupt delivered to the guest handler.
5. fpu: single/double arithmetic through the helpers.
6. perf: instruction-mix benchmark to measure the slowdown.

## Runtime conventions (dbt/rt)

- `dbt_lookup` / `dbt_lookup_link` / `dbt_lookup_ic`: X0 = guest target,
  X1 = link literal address (link) or inline-cache literal pair (ic). Direct-mapped hash of 8192 entries (`dbt_hash`, read by the asm
  fast path); on a miss the stub masks interrupts, saves the guest context
  into `dbt_ctx_rt` and calls `dbt_resolve`, which translates and patches the
  link literal (a data write; PC-relative loads go through the operand cache).
  A block the direct-mapped hash lost to a collision is found through a
  second index chained by the same hash slot (`g_chain_head/next`); until
  2026-11-03 this was a linear scan of the block table at every miss, which
  made each translation cost about 0.24 ms of emulated time with 4096
  blocks (a third of a CE boot) and grow with the block cap. Rule for
  every table the translator's C code updates: an interrupt can land in
  that C code, the guest handler runs and returns to the pc being
  translated, and the C context is abandoned (`where=3` in the exception
  trace), so updates are ordered so that an abandoned one leaves a block
  nobody finds (translated again later), never a half-published entry: the
  host code is synced to the instruction cache first, the block is
  allocated (`g_nblocks++`) before its chain head is written, the hash entry
  comes last. Work that must not be restarted from scratch runs with IMASK
  raised: the full flush clears 8192 hash slots, about one tick, and an
  unmasked flush was abandoned by every tick and never finished (a livelock
  at the first cache fill of the Release boot).
  The patch is skipped when the translation flushed the whole cache
  (`dbt_stat.flushes` changed under the lookup: 4096 blocks or 1 MiB
  reached): the literal belongs to a block that no longer exists, and the
  write would land in whatever the fresh cache has put there. The same
  guard covers inline-cache refills. Found with the CE kernel, where the
  cache fills once per boot and the stale patch corrupted a freshly
  translated coredll block (a heap free with a wrong pointer, two minutes
  later).
- Helpers (`dbt_fpu_stub`, `dbt_fallback_stub`, `dbt_ldc_sr_stub`): X0 =
  argument, X1 = return address. They save the full context, mask interrupts,
  call C, restore and jump back through the shared `dbt_tail_full`.
- `dbt_rte_stub`: resolves the guest SPC, forces MD and DSP in SSR, keeps the
  guest MD in `dbt_shadow_md`, then executes the real `rte`.
- Exceptions: VBR = `dbt_vectors`. The hardware enters the vectors with
  RB=1, so the current r0-r7 are bank 1, and bank 1 belongs to the guest
  too: the CE kernel keeps pCurThread, its context pointer and SH3CTL_BASE
  in r4/r5/r7_bank1 for the life of the system (notes/ce/kernel-sh-notes.md).
  The vector thunks therefore save bank 1 first (`ctx->b1`), using r15 as the
  only free register (the hardware copied it to SGR), then bank 0 (via
  `stc Rn_bank`), the DSP slots and SPC/SSR/EXPEVT/TRA/TEA/INTEVT into
  `dbt_ctx_exc`, and call `dbt_exc_c`, which swaps the two bank images when
  the guest itself ran in bank 1 so that C always sees the guest's current
  registers in `ctx->r`. Hypercalls (`trapa` 0xF0-0xFF) return with `rte`;
  everything else is delivered to the guest handler with SPC = guest resume
  pc, SSR.MD = shadow MD, bank 1 restored except r1, which carries the host
  address of the handler (a scratch register for any SH exception handler).
  Verified by the `bank` guest. After a resume the hardware SPC/SSR hold
  host values; a guest that nests an exception without saving SPC/SSR first
  reads those, which the CE kernel never does.
- A guest `ldc rm, sr` that switches register banks cannot be finished with
  `ldc` + `jmp`: after the switch no register of the new bank is free for the
  jump (the kernel's PSL entry passes arguments through bank-0 r1-r3 across
  its switch). `dbt_ldc_sr_c` reports the flip and the stub returns through
  `rte` with SPC = `dbt_bank_tramp`, which reloads the guest's own SPC/SSR
  from the context (`ldc.l @r1+`, one register borrowed and parked in X0)
  and jumps to the target: the interrupt entry pushes SPC/SSR after its
  switch to bank 0. `bank` guest, 19 checks.
- A guest `rte` whose target is not translated yet does not translate it in
  the rte stub: that would probe the target under the SR in force at the
  rte, and the CE kernel dispatches threads with BL=1, where a TLB miss on a
  mapped target is a manual reset. `dbt_rte_c` only consults the hash; on a
  miss the rte lands in `dbt_rte_tramp`, which resolves the target in the new
  context (full context save, so T comes back from SR). `mmu` guest, last
  check.
- Debug ring: an exception-free console at `GUEST_RAM_HI - 4 KiB`
  (uncached P2 alias, words little-endian: write count, read count, magic
  0x47424452, 4080 bytes of storage). The guest appends bytes and the
  translator prints them at every entry (C lookup, hypercall, halt) and
  updates the read count. For code that runs with BL=1 and cannot `trapa`
  (the CE kernel's SHInit/OEMInit path; a trapa with BL=1 is a manual
  reset). The `hello` guest exercises it.
- An illegal instruction inside translated code is reported with host
  address, word, block and guest pc before it is delivered: it is almost
  always a translator bug.
- Execution trace (`EXTRA_CFLAGS=-DDBT_TRACE_EXEC`): every block starts
  with 13 instructions that append its guest pc to a 64 KiB ring; the last
  512 entries are printed when the guest's debug output contains
  `DBT_WATCH` (default `DEBUGCHK`, the CE kernel's assertion message).
  `tools/cesym.py NK.bin RELDIR -` turns the pcs into module!symbol+offset
  using the image's module table, the o32 section tables and the build's
  .map files (kernel-mode DLLs run at their fixed-up VM base, 0xC00xxxxx).
- Device-aware blocks (`dbt_env.dev_lo/dev_hi`, set from the ROMHDR module
  table to the union of kernel.dll's code sections; the o32 table, not
  load..load+vsize: ROMIMAGE put the Release kernel.dll's text 16 KiB below
  its load address, and the exception vectors in it read EXPEVT/TEA
  through r7_bank swapped until the range was fixed): code that reaches the on-chip control
  registers directly cannot use pre-swapped accessors, so every 32-bit
  load/store in such a block tests its effective address for P4 (top byte
  0xFF) at run time with 15 extra instructions that leave T alone (the test
  ends in `braf` on a 0/4 displacement), takes the usual swapped sequence
  otherwise, and performs a P4 access unswapped through the fallback helper.
  The helper also observes PTEH stores (ASID switch) and MMUCR.TI (user
  translations dropped), which stands in for the kernel-side FA/F9
  hypercalls. A device access in a delay slot is handled like a helper
  slot. Test hypercall FC turns the range on for a running guest (`devmem`
  guest, 17 checks). Cost is paid in kernel.dll only; our OAL keeps the
  pre-swapped convention. The slow path is a META range of its own, so
  `resume_pc` continues after a committed access at the next range of a
  different guest instruction, and inside a branch's delay slot it is
  followed by a range that restores the branch's resume rule. `make
  DEVALL=1 GUEST=x run` translates a whole test guest device-aware; the
  pre-swapped MMIO reads of exc/tmu/apicall/bank/mmu/aspace are then wrong
  by design, but hello/fpu/bench and the postinc/predec/slot loops of tmu
  must still pass.
- Faults while translating: a fault raised by the translator's code probe
  (TLB miss on a mapped page, any vector) is delivered to the guest at the
  pc being translated, with the guest registers taken from `dbt_ctx_rt`
  (the stub saved them). A guest pc that no CPU could fetch (odd, or in P4
  from 0xE0000000, where a data read would *not* fault) never reaches the
  probe: `translate_at` raises `trapa #0xF8` (HC_SYNTH, r4 = pc, r5 =
  EXPEVT 0x0E0) and `dbt_exc_c` writes EXPEVT/TEA and delivers an
  instruction address error through vector 0x100. This is what Windows CE's
  SH API-call convention (jumps to 0xF000xxxx) needs.
- Hypercalls: F0 exit, F1 puts, F2 flush translations for a guest range
  (`hc_flush`, for code the guest loaded or modified), F3 putc, F4 puthex,
  F5 stats, F6 bench, F7 dump host words, F8 synthetic exception (internal),
  F9 user mappings changed in a range (`hc_tlb_inval`), FA address space
  switched to an ASID (`hc_asid`), FB read a statistic (0 translations,
  1 blocks, 2 invalidations, 3 ASID switches, 4 inline-cache misses,
  5 debug-ring bytes, 6 device accesses, ... up to the tick-sample
  histogram at 128-383; the full index list is in
  `bsp/classpad/SRC/INC/classpad_hc.h`, "Measuring on the device" below),
  FC translate a range device-aware
  from now on (tests), FD push pixels to the LCD data port natively
  (`hc_lcd_blit`: r4 = pixels as the guest stores them at a P1/P2 address,
  r5 = count, r6 = port; the display driver's row push, 2026-11-05), FE the
  same for the DCS panel with CASET/PASET/RAMWR issued inside the hypercall
  (`hc_lcd_push`: r4 = pixels, r5 = first row, r6 = rows, r7 = width; the
  `dcs` guest checks it), so the panel console cannot write between the
  driver's window commands and its pixels.
- Address spaces: blocks are keyed by virtual pc. A block of TLB-mapped
  code (pc < 0x80000000) also records the ASID it was translated under and
  the physical address of its first instruction, found by scanning the UTLB
  arrays (0xF6000000/0xF7000000). The hash and the inline caches only hold
  entries valid for the current ASID: when PTEH's ASID is seen to change
  (read at `rte`, `ldc sr`, `ldtlb` and every C lookup, or told by HC_ASID),
  user blocks of other ASIDs leave the hash and inline caches aimed at
  user-space targets are reset; the blocks stay and are reused when their
  ASID returns, or in another ASID when the UTLB says the virtual pc maps to
  the same physical page (shared DLL code). `ldtlb` is a helper call: a new
  entry whose physical page differs from what a page's blocks were made from
  invalidates them. Invalidation is precise: the block's hash slot (empty
  slots carry gpc = 1, since the asm fast path compares only the pc), inline
  caches whose cached host address lies in it, and chained direct jumps into
  it (restored to their original words and exit stub). Dead blocks keep their
  cache memory until the next full flush.
  Not covered without the kernel's help: a TLB entry removed by a UTLB array
  write or MMUCR.TI and the page remapped later is noticed only at the
  `ldtlb` that installs the new mapping; a translated block for that page
  that is entered from the hash before then runs the old code. The kernel's
  TLB-invalidate and address-space-switch paths therefore issue F9/FA
  (docs/05).
- Resume rules: translated code uses the block metadata (restart the
  instruction, or the branch target after a delay slot). The resume pc is
  read before the pending scratch spills are undone (`META_COND_REG` reads
  the T value a slot saved in a scratch), so a register the pc comes from
  (`META_TARGET_REG`) must never be a scratch of the delay slot: the slot
  is translated with `slot_avoid` = the target register and the kept PR
  scratch (2026-11-04). Before that, kernel.dll `SwitchActiveProcess`'s
  `jsr @r3; mov.l r1,@(12,r2)` (no borrow in that block) swapped r1
  through r3, an interrupt on the `sts` restoring r3 found the store
  committed and resumed "at the target" = byteswap(process id):
  "Alignment Error" at PC=02002501 with RA = the jsr's return, once per
  boot at a fixed tick (`tmu` loop `loop_jsrtgt`, swept over eight timer
  periods, hangs the old runtime; unit test 20c). The helper
  prologue window (the three parks before the stub masks interrupts)
  resumes at the calling instruction, X1 - 2, but applies the block's
  spill records as of the return address X1: the call's delay slot
  `sts Y0,rT` has run, so the scratch's record (open through the slot's own
  offset) is closed, and the prologue has already put r0 into Y0. Until
  2026-11-04 the records were applied at X1 - 2 and the scratch took r0's
  value: with rT = r1 in KCall's epilogue, `mov.b @r1,r0` ran with r1 = the
  wait's return value (0 or WAIT_TIMEOUT = 102) in most boots of the
  blit-logging GWES image (`Exception 040 ... TEA=00000102` from the
  kernel, whose register dump is the KCall caller's frame; the translator's
  own null-page dump, printed for the first four data faults below 0x1000,
  showed the real pc and registers). The `tmu` test's `loop_ldcsr` sweeps
  interrupts over an `ldc rm,sr` per iteration and fails on the old
  runtime. Inside the stubs,
  window labels (`dbt_lk_s*`, `dbt_hit_*`, `dbt_full_m*`, `dbt_h*_s*`) tell
  the handler which of r0-r3 are still parked in the DSP slots.
- Known v1 limits: FPU memory operands and guest code must be in unmapped
  (P1) memory; a TLB miss inside C code halts unless it is the translator's
  own code probe. (SR.FD and the CCN registers used by kernel.dll are
  handled now: see "FPU disable" below and the device-aware blocks.)
- FPU disable (SR.FD): the FPU helper reads the guest SR saved by its stub
  and, when FD is set, raises the FPU-disable exception instead of
  executing: EXPEVT 0x800, or 0x820 for an instruction in a delay slot
  (the translator passes the slot flag in bit 16 of the helper argument),
  SPC = the instruction, or the branch owning the slot (the branch is
  re-executed, as on the SH-4 and as the translator's own slot-fault rule
  already does). The guest pc comes from the helper's return address (X1)
  through the block's instruction ranges; the registers are exact in
  dbt_ctx_rt since nothing has run. The CE kernel runs every thread with
  FD=1 and takes the FPU on this exception (SwitchFPUOwner: save the owner's
  registers with fmov, restore the new thread's, clear FD in the saved
  SR), so the emulated register file is switched by the kernel's own
  lazy-FPU logic. `fpudis` guest; getstat 8 counts them.

## Performance (casio-emu, 2026-10-22)

`make -C dbt GUEST=bench run`: the kernels in `bench/loops.S` are assembled
big-endian into the translator image and little-endian into the guest; the
guest times its (translated) run with TMU1 and asks the runtime through
`HC_BENCH` to time the same kernel natively. 20000 iterations each,
interrupts masked, blocks translated by a warm-up call first. casio-emu
charges one TMU tick per four executed instructions regardless of type, so
these are *instruction-count* ratios; the per-iteration counts below were
read from the translated blocks and agree with the measurements.

| kernel | per iteration | native ticks | baseline | + branch chaining | + inline caches | + cheaper calls | + borrowed register (2026-11-03) | host insns / guest insns (now) |
|---|---|---|---|---|---|---|---|---|
| alu | 5 ALU + `dt` + `bf` | 15382 | 27723 (1.80x) | 15390 (**1.00x**) | 15390 | 15388 (**1.00x**) | 15388 (**1.00x**) | 7 / 7 |
| mem mixed | 4 longword, 2 word, 2 byte accesses, 2 ALU, loop | 26369 | 81308 (3.08x) | 72519 (2.75x) | 72519 | 72518 (2.75x) | 63729 (**2.42x**) | 29 / 12 |
| mem copy | 2 post-increment loads, 2 stores, pointer wrap, loop | 29391 | 81033 (2.76x) | 68400 (2.33x) | 68399 | 68398 (2.33x) | 61532 (**2.09x**) | |
| stack | push r1, r2, PR; pop them; loop | 19777 | 90096 (4.56x) | 81309 (4.11x) | 81307 | 81307 (4.11x) | 63729 (**3.22x**) | 29 / 9 |
| branch | 3 data-dependent `bt`/`bf` (half taken), loop | 25270 | 47249 (1.87x) | 31869 (1.26x) | 33516 | 33515 (1.33x) | 33515 (1.33x) | |
| call jsr | `jsr @r6; nop` to a 3-instruction leaf | 17580 | 156016 (8.87x) | 156017 (8.87x) | 76914 (4.38x) | 54941 (**3.13x**) | 54941 (3.13x) | 25 / 8 |
| call bsr | `bsr leaf; nop` | 17580 | 105479 (6.00x) | 96690 (5.50x) | 57139 (3.25x) | 48349 (**2.75x**) | 48349 (2.75x) | |
| call frame | `jsr` to a function that pushes/pops r8, r9, PR | 35159 | 235117 (6.69x) | 235119 (6.69x) | 156016 (4.44x) | 134042 (**3.81x**) | 120858 (**3.44x**) | |

**Branch chaining** (done 2026-10-22, `dbt_chain_direct`): when the runtime
links a direct jump and the target is within the 12-bit `bra` range, the
first word of the jump sequence becomes `bra target` and the second its
delay slot (form A: a taken branch costs 3 instead of 5). For a conditional
*back edge* within the 8-bit range the inverted skip word itself becomes the
real `bt/bf target` and the jump's first word a `bra` over the rest (form B:
loop branches cost 1, like native). Since the borrowed register (2026-11-03)
the sequence is 5 words with the register's restore (or a nop) first: form A
moves that word into the `bra`'s delay slot, and form B, when there is a
restore, makes the skip a `bt/s | bf/s target` whose slot it is (a block
without a borrow keeps the plain `bt/bf`: casio-emu charges the slot as an
instruction, hardware would hide it in the branch bubble), so a chained exit
costs no extra instruction; the restore then also runs on the not-taken
path, which is why the swap cache dies at every conditional branch. Blocks are allocated in translation
order, so loop targets are almost always in range (`chained` in the stats
counts rewrites). Pure ALU loops now run at native speed; what remains is
memory-op expansion and the indirect-branch path. Two things had to change
with it: links are always resolved through C (the asm hash-hit shortcut in
`dbt_lookup_link` patched the literal without chaining; a link is resolved
once, so the C round trip costs nothing), and the hash became direct-mapped
with the same index function in C and asm (the C side used to linear-probe,
which the asm fast path could not see, and consecutive block starts evicted
each other; an entry missing from the hash is now recovered from the block
table instead of being retranslated).

**Inline caches** (done 2026-10-22, `jump_indirect_ic`): every indirect
branch site (`jsr`, `jmp`, `rts`, `braf`, `bsrf`) ends with a 10-instruction
hit path: compare the guest target (parked in X0) with a cached guest pc held
in a literal, and `jmp` to the cached host address held in the next literal.
A miss goes to `dbt_lookup_ic` with X1 = address of the literal pair; the stub
refills both words (data writes, read back through the operand cache) from
its asm hash probe or, failing that, from C (`dbt_resolve_ic`). The asm
refill runs with interrupts live and a handler may execute the same site,
so it writes pc = 1 (never matches), then the host, then the pc
(2026-11-04): with pc first, an interrupt between the two stores left
{new pc, old host}, a hit into the previous target or into host 0. Every
run of a given binary failed or passed deterministically (the tick lands
at the same instruction), which looked like a layout-sensitive bug; the
intermittent "Faulted in KCall" of the Debug image was this too, the RTC
edges being host-time based. Rule: a multi-word update that translated
code can read must be ordered so that no interleaving pairs new with old,
or run masked (C runs masked: the lookup and helper stubs raise IMASK
before calling it, and casio-emu's fx-CG100 profile delivers peripheral
interrupts only at IMASK=0, `interpreter.c`). The chaining patches
(`link_direct`, `unchain`) are covered by that mask; their intermediate
states would not be safe otherwise (form A's first store alone leaves
[restore, restore, mov.l, jmp @rT, sts X0,rT], the scratch's spill gone).
Both literals start as {1, 0}, so the first execution always misses. A
`jsr`/`rts` pair now costs about 29 host instructions instead of ~57 (the
remaining cost: 4 for the PR literal, 1 to park the target, 10 per inline
cache, and 4 for `rts` to move PR into X0). Blocks grew by 24 bytes, which
pushes a few conditional back edges out of the 8-bit range (`branch` 1.26x ->
1.33x); the next step there is a 2-word short form when the site's target
register is not written by the slot.

**Cheaper calls** (done 2026-10-22): when the delay slot does not write
the target register, `jsr`/`jmp` no longer park the target in X0; the
inline cache compares the register itself (`META_TARGET_REG` resumes from
it), with one scratch: `mov.l g,rT; cmp/eq rN,rT; bf; mov.l h,rT; jmp @rT;
sts Y0,rT` (6). For `jsr` the scratch is the one `set_pr` used, left
spilled across the slot (not across a helper slot: helpers own Y0), so the
PR write costs 3 instead of 4 and the cache needs no spill of its own.
`rts` runs its slot first and reads PR straight into the scratch
(`META_TARGET_PR`), 10 instructions, no X0 round trip. The old X0-parking
form remains for `braf`/`bsrf` and for slots that write the target register
or PR. A `jsr`/`rts` pair is now 3 + 6 + 10 = 19 host instructions
(was 57 before inline caches); the slot's own scratch may even be the target
register, since it is restored before the compare.

Measurement pitfalls worth remembering: a timed region must not contain the
first execution of the code *after* the call (its block gets translated
inside the region, ~2900 ticks here), and GCC unrolls a two-iteration
timing loop, giving each pass its own return site.

Where the cycles go:

- **Direct jumps** (`bra`, taken `bt`/`bf`, block fall-through): 5 host
  instructions instead of 1 (`lds rT,X0; mov.l lit,rT; jmp @rT; sts X0,rT`
  plus the inverted skip). That alone makes a pure ALU loop 1.6-1.8x.
- **Indirect branches** (`jsr`, `rts`, `jmp`): about 10 instructions in the
  block (PR literal with spill/restore, target to X0, lookup jump) plus 19
  in the lookup stub's hash-hit path, twice per call/return pair.
- **Stores**: 3 swaps + store = 4 with the borrowed register (6 with a
  spill/restore pair when the block has no free register; byte stores 1),
  and 1 for a repeated store of the same register; loads: 4 (word loads 3).
  `sts.l pr,@-r15` is 5. SH-4 has no byte-swap instruction, so 3 swaps per
  longword is the floor for a value stored once.
- FPU and `mac` go through C helpers (not measured here; hundreds of
  instructions each, as planned).

On hardware, native code stalls on memory and the translated extra work is
all register ALU, so memory-heavy ratios should come out a little lower than
here; the 3-5x code expansion will cost instruction-cache misses instead.

Helper paths (2026-11-03, `bench` kernels, 20000 iterations, TMU1 ticks;
native = the same loop run big-endian by the translator image):

| kernel | translated | native | slowdown | per call |
|---|---|---|---|---|
| `ldc sr` (no change: a helper with a full context save) | 329598 | 10988 | 30x | ~150 host instructions |
| inline-cache miss (indirect call alternating two targets) | 123050 | 19778 | 6.2x | ~56 |
| device load in device-aware code (fallback helper) | 1773203 | 8791 | 202x | ~800 |

**Borrowed register** (done 2026-11-03, "Scratch registers" above): stores
and `sts.l/lds.l` lose their spill/restore pair, repeated stores of one
register their swaps too, and a block's back edge to itself is chained past
the park (`link_direct`), so a loop keeps the register borrowed. The
translator's pre-pass that picks the register decodes every instruction
once and keeps the decoding for the main loop: decoding twice cost a tenth
of a CE boot (first measured with the double decode, the Release boot was
13% slower in spite of the cheaper stores). Costs on the way: a helper call
now commits its range at the call (the re-park word at the return address
would otherwise restart the helper on an interrupt in its exit tail), and
no scratch is ever the borrowed register. Release boot to first idle:
16231 -> 15973 ticks; the `flush` guest (6000 translations of 2-3
instruction blocks per pass) is 5% slower per pass, the pre-pass' fixed
cost per block. Most of a CE boot's block time is in loops without stores
(the OAL's line-by-line cache walk, 30% of the samples), where nothing
changes.

Optimizations, in order of payoff per effort:

1. ~~**Branch chaining**~~ done (above).
2. ~~**Inline caches for indirect branches**~~ done (above).
3. ~~**Cheaper PR writes**~~ done (above); `bsr` still pays 4 + the direct jump.
4. ~~**Spill avoidance for stores**~~ done (2026-11-03) as the borrowed
   register ("Scratch registers" above): no liveness needed, a register the
   block never touches is free for the whole block.

### Runtime hot spots found with the shell image (2026-11-05)

A heartbeat profile of the Standard Shell boot (casio-emu `HLE_HB=1`, 646
samples of the PC every 50M iterations, symbolised with `sh-elf-addr2line`
on the translator ELF) put 78 % of the boot in the translator's own C code
and 22 % in translated guest code. Inside the C code: `memset` 22 % (the
byte loop of rt.c, called for every `sh_insn` an instruction decode fills
and for every translation's context and block), `switch_asid` 17 % and the
`ldtlb` fallback 10 % (both scanned every block header, up to MAX_BLOCKS, at
every address-space switch and every TLB refill), the translation proper
3 %. Fixes: word loops for `memset`/`memcpy`; `g_user_idx` and `g_ic_idx`
list the user-space blocks and the blocks with an inline cache, so an ASID
switch and a TLB hook only visit those (appended before the block is
published, reset by a full flush; the loops re-check `binfo` so a duplicate
or a reused index is harmless). Measured on the hello image (NK-fix5,
4096 blocks, 512 KiB cache, iterations at each milestone): display driver
up 1500M -> 800M, hello window 2700M -> 1400M, tick 20 8100M -> 4050M; in
the idle window the cost per guest second fell from 270M to 107M
iterations (every 1 ms tick's `rte` used to pay a full block scan). All
13 guests pass. Still open from the same profile: the shell's working set
exceeds 4096 blocks (92 full flushes and 376 k translations in 900 s with
the old layout, measured by the HollyHock session), which the 8192-block
layout addresses, and the prepass/main-loop double decode is already
avoided (`g_pre`).

### Measuring on the device (2026-11-06)

Everything above was measured in casio-emu, whose cost model is one tick per
instruction: no caches, no SDRAM latency, no branch penalties. The real
fx-CP400 is slower than that model predicts and nothing had been measured
there, so the runtime now carries a profiler that needs no output channel
but a window on the CE desktop:

- **Tick sampling** (`sample_tick`, always on): every TMU0 interrupt
  (vector 0x600, INTEVT 0x400) classifies the interrupted host pc the way
  `trace_exc` does (inside a block, in an exit stub, in a lookup/helper stub
  window, inside a translation, inside a helper) and the guest pc it resumes
  at: the kernel's idle `sleep`, a user-mode pc (translated through the UTLB
  so XIP module code lands in the same histogram as the kernel), or a
  kernel pc. The histogram has 256 buckets over the ROM image by physical
  address (`hist_base`, `hist_shift`), 16 KiB each for the 3.875 MiB window;
  `tools/cesym.py` names them. A tick that lands in masked translator C is
  delivered at the first instruction after the re-enabling `ldc` in the
  stubs' full tail, so the stub-window samples are split by where in the
  stubs they land and by which C entry returned last (`g_tail_from`):
  after a lookup or translation (translator time), after the `ldc sr`
  helper (kernel code that ran with interrupts disabled), after an FPU or
  fallback helper, the cheap entry windows, the asm hash hit, and the
  trampolines. The guest pc of such a sample is the one that entered the
  stub (the "where=3 caveat" above), so those samples still sit in the
  histogram at the code that caused them. The PROFILE=1 buckets measure the
  same time in TMU1 counts; the two should agree.
- **Exception counters**: every delivery by vector (general, TLB miss,
  interrupt) and the first 16 INTEVT codes seen with their counts, so a
  storming source or an unexpected TLB-miss rate shows at once.
- **PROFILE=1 buckets** gained `exc` (the C side of a delivery, handler
  translation included), `rte` and `lookup` (`dbt_resolve`, translation and
  full flushes included). On the device TMU1 counts real time: 7,372,800
  per second at the stock clock, so the buckets are seconds, not model
  ticks. The build reserves TMU1, which the OAL never touches.
- **Machine state**: CCR, MMUCR, FRQCR and the RTC's seconds of day. The CE
  kernel programs CCR itself in `SHInit` (`CACHE_ENABLE | 0x80000105`:
  both caches on, P1 copy-back, no write-through), which `mdsh3.c` in the
  shared source confirms; the read-out shows what the hardware actually
  holds. The RTC seconds against GetTickCount give the real TMU clock
  (bsp_cfg.h assumes Pphi 29.4912 MHz, from casio-emu).

All of it is read with `HC_GETSTAT` (indices 64 up; the list is in
`bsp/classpad/SRC/INC/classpad_hc.h`), which works from user mode: a
`trapa` is a general exception the translator takes before the kernel sees
it. `bsp/classpad/SRC/APPS/DBTSTAT` (`classpad_dbtstat.exe`) polls it every
2 s and shows rates, sample shares, C-time percentages and the hottest code
buckets on the panel; `print_stats` prints the same `[sample]` lines in the
emulator (every 4096 rte in PROFILE builds, and at HC_STATS/exit).

First emulator numbers (power image, ClassPad profile, PROFILE=1, 360 s
wall / 270 s of guest ticks to idle): 88 % of the ticks idle once the
desktop is up; before that, translated code 3/4 and the stub windows 1/4,
of which almost all is "after lookup C" (translations under the mask); TLB
misses 0.4 per tick (45 k); of the TMU1 time, translate 8 %, exception
delivery 6 %, lookup 5 %, invalidate 4 %. The kernel pcs outside the
histogram were P3 addresses 0xC010Bxxx (the KPage's interlocked APIs), now
translated through the UTLB like user pcs.

The first thing it is for: deciding whether the device's slowness is the
translated code (block share high, histogram concentrated), the interrupt
rate (irq/s, TLB misses/s), the translator's C (PROFILE buckets) or idle
that is not idle. Until those numbers exist, the hardware-only costs the
emulator cannot show (I-cache misses from the 3-5x code expansion, literal
loads through the operand cache, `jmp @rT` penalties) are guesses.

### Profiling a CE boot (2026-11-03)

The 1 ms TMU tick is a sampling profiler: `-DDBT_TRACE_EXC` prints every
exception delivery with the guest pc it resumes at, and
`tools/cesym.py NK.bin RELDIR -` turns the `vec=00000600` lines into
module!function names (the recipe: `grep vec=00000600 | grep -o 'resume=...'
| cesym | sort | uniq -c | sort -rn`). `-DDBT_TRACE_FLUSH` prints the
statistics at every full code-cache flush, with the cache and arena fill.
Release image, 29770 samples (about 30 s of emulated time to the first
idle; the emulator runs at roughly a quarter of real time):

| Function | Samples | Note |
|---|---|---|
| nk.exe ClasspadCacheWriteback | 4761 | OEMCacheRangeFlush walking the 11.5 MiB RAM window line by line for "whole cache" calls |
| k.coredll SHATransform | 765 | registry hive hashing, plain translated code |
| kernel.dll ZeroPage | 550 | stores of zero still pay the swap |
| kernel.dll KCNextThread | 418 | |
| nk.exe ClasspadICacheInvalidate | 310 | as the write-back above |
| kernel.dll InitModule | 290 | |
| k.coredll memset / memcpy / sh3memcpy | 563 | |
| nk.exe OEMCacheRangeFlush | 177 | |

After the fixes below the same boot takes 15.5 s of emulated time (16 k
translations). Reading that profile needs one caveat: a sample whose
interrupt was taken in a stub window after a masked C call (`where=3`,
60 % of the samples then) measures the time the kernel ran with
interrupts disabled, attributed to the `ldc sr` helper that re-enabled
them: guest work, not translator overhead. The helpers themselves are
cheap (`bench` kernels, 2026-11-03): an `ldc sr` costs about 150 host
instructions, an inline-cache miss about 56, a device-register load
through the fallback about 800; at 59 k, 10 k and 24 k per boot that is
under half a second. What remains is the translated code's own slowdown
(the table above: 2.3-4.1x on memory and call-heavy code).

So 18 % of the boot was the OAL's whole-cache walks: `OEMCacheRangeFlush`
now walks the cache address arrays instead (1024 stores to 0xF4000000 /
0xF0000000 for the 32 KiB 4-way caches, `cacheops.src`), which needs
casio-emu to accept the operand-cache arrays (`dbt/emu-patch/add_oc_arrays.py`,
it models no operand cache). The code cache filled 18 times in those 30 s
(MAX_BLOCKS was 2048, now 4096; `-DMAX_BLOCKS=n -DMETA_ARENA_BYTES=n`
override the limits for experiments), 36 k translations in all, and with the
cap halved the boot took 13 % longer for 16 k more translations: about
0.24 ms of emulated time per translation, a third of the boot. The kernel's
two whole-cache instruction flushes (InitModule, CreateNewProcHelper, 8 per
boot) made the translator drop every translation; now blocks whose code lies
in the ROM image are kept (XIP, never written; `hcflush=` calls / all and
`romkept=` in the statistics). The decision uses the physical address of
the block's code: for P1/P2 code it is the pc itself, for user pcs and the
kernel's TLB-mapped range (0xC0000000 up: kernel-mode DLL mappings and
kernel `VirtualAlloc`) it comes from the UTLB at translation time (until
2026-11-04 the kernel range was also keyed by pc & 0x1FFFFFFF, so a kernel
VA whose low bits fell inside the ROM's physical range, say 0xC1xxxxxx, was
taken for ROM and a translation of a reused page survived the flush).

## Test platform

casio-emu implements only `lds rm, x0` of the DSP transfers; the patch in
`dbt/emu-patch/add_dsp_transfers.py` adds the other nine (applied to the local
casio-emu checkout on 2026-10-22; rerun it after updating the emulator, then
`node scripts/genDispatch.mjs && cmake --build build`); `add_ccn_regs.py`
adds the TTB and PTEA registers (2026-11-02) and `add_oc_arrays.py` makes the
operand-cache address and data arrays (0xF4000000/0xF5000000) accepted
(the OAL's whole-cache flush walks them); `add_classpad_addin.py`
(2026-11-05) lets the ClassPad profile (`make EMU_MODEL=fx-cp400 ...`,
320x528 panel, IIC0 touch, INTC priorities) start an add-in, adds
`HLE_FBDUMP` for its frame and the headless stylus `HLE_CB_TOUCH`
(docs/05 section 5). The emulator starts a
raw `.g3a` payload at 0x00300000; `dbt/loader` copies the translator image to
`IMAGE_BASE` (0x8CC80000, see the layout in `dbt/Makefile`) and jumps.
`make -C dbt run` builds everything and runs it headless;
`GUEST=exc` / `GUEST=fpu` select the other guests.

Status 2026-10-22: all four guests pass under the emulator.

| Guest | What it covers | Result |
|---|---|---|
| `hello` | every access size and addressing mode, literal pool, stack, struct layout | PASS (8 checks) |
| `exc` | guest VBR, `trapa`, EXPEVT/TRA, SPC rewrite by the handler, `rte`, T bit across exceptions | PASS (6 checks) |
| `fpu` | raw FPU words through the helper: fpscr reset, fadd/fmul/float/fmov.s, double sub + fcmp/gt | PASS (5 checks, 0x21 helper calls) |
| `bench` | translated vs native timing of eight kernels (see "Performance") | runs |
| `apicall` | `jsr` to 0xF0010000 / 0xF000FFFC / an odd address (the CE API-call convention): instruction address error synthesized by the translator, EXPEVT 0x0E0, SPC = TEA = target, handler redirects to the caller through PR with r4-r7 intact, repeated calls through the inline-cache miss path | PASS (7 checks) |
| `mmu` | MMUCR.AT on, code and data pages mapped with PTEH/PTEL/`ldtlb`: translated call through a TLB-mapped code page with a data load through another, store through the TLB, TLB miss on a code fetch delivered to the guest's 0x400 handler (EXPEVT 0x040, TEA = SPC = pc), refill and retry, MMU off again | PASS (10 checks) |
| `aspace` | address spaces: the same virtual page mapped to different code in ASID 1 and 2 (switch noticed at `ldc sr`, or announced with HC_ASID), blocks reused when an ASID returns and across ASIDs for a shared (SH=1) page, a remap announced by HC_TLB_INVAL, a remap noticed at `ldtlb` alone, reloading an identical mapping invalidating nothing, and a chained cross-page `bra` unlinked when its target page is remapped | PASS (14 checks) |
| `lcd` | draws into a little-endian RGB565 VRAM and pushes 24 frames to the R61524 display controller (16-bit bus at 0xB4000000, RS on Port R) through the pre-swapped MMIO accessors; `make -C dbt GUEST=lcd screenshot` captures the frame | shows the picture (docs/img/lcd-guest.png) |
| `bank` | register bank 1 across the translator: values parked in r4/r5/r7_bank1 (the CE kernel's pCurThread, context pointer, SH3CTL_BASE) survive hypercalls from bank 0, translation of new code, a trapa handled in bank 1, a hypercall issued from bank 1 with BL cleared (both banks intact), a synthetic address error returning through PR, and 50 more exceptions; the handler sees the interrupted bank-0 registers | PASS (14 checks) |
| `devmem` | device-aware translation: a value written to TEA pre-swapped reads back unswapped once the guest is translated device-aware, plain round trips, a pre-swapped accessor is now wrong, ordinary memory keeps its LE view, T survives device and memory loads, device access in `bra` and `jsr` delay slots, `@Rm+`, `@-Rn`, `@(r0,Rm)` forms, a PTEH store switches the ASID, MMUCR.TI goes through the device path, accesses counted | PASS (17 checks) |
| `tlbdata` | every memory addressing form (byte/word/long; @Rm, @Rm+, @-Rn, @(disp,Rm), @(r0,Rm), @(disp,GBR), movca, base = data, base = r0, load-modify-store) with a synchronous TLB miss on the access itself: the data page is mapped only by the miss handler and the TLB is flushed before each form, so the translated sequence faults at its memory op, the handler refills, the instruction restarts; exactly one miss per form and the little-endian result checked through the physical alias | PASS (55 checks; casio-emu adds 4 to the raw loaded word for `mov.l @Rm+,Rn` with m = n, where the SH manual increments only for m != n: accepted) |
| `fpudis` | SR.FD: FPU instruction (lds/sts fpul, sts fpscr, fsts/flds) with FD=1 raises 0x800 at the instruction, 0x820 with SPC = the branch for a slot of bra, jsr and bt/s; the handler clears FD in SSR and the instruction or the whole branch is re-executed once; FD=0 never faults | PASS (19 checks) |
| `flush` | code-cache pressure: 3000 indirect calls and a 3000-block direct chain, three passes, 9 full flushes with links and inline caches resolved across them; the sums must not change | PASS (the hazard above is not reproduced: after a flush the stale literal lies in cache memory the new block does not reuse) |
| `tmu` | TMU0 programmed through pre-swapped MMIO, IMASK/BL and IMR masking, INTEVT, `rte` restoring SR, `sleep` wake-up; register probe across `trapa`; 15 one-construct hot loops (loads, stores of every form, `bt/s` with a T-writing slot, `jsr` with a store in the slot, push/pop, pc-relative) plus a compiled loop, each run with ~3000 interrupts landing at arbitrary host instructions and compared with an interrupt-free run | PASS (34 checks) |

![lcd guest frame](img/lcd-guest.png)

The `lcd` guest is the first device driven by translated code: register
select through an 8-bit port bit (no swap), window registers and pixel data
as 16-bit writes (pre-swapped), 384x216 pixels per frame. One frame push
costs 0x2ebd6 TMU1 ticks (about 1.7 M host instructions, ~21 per pixel: a
VRAM load with swap, the pre-swap, the store with its spill, loop control).
The display layout is the fx-CG50/CG100 panel that casio-emu models; the
ClassPad's 320x528 panel and its controller are a later concern for the
display driver.

The TMU guest found three resume bugs that the host tests could not: `bf/s`
boundaries had the T sense inverted, loads were restarted after they had
executed (now the commit-point metadata), and the `rte` stub restored r2 in
the delay slot after the bank switch. `make -C dbt GUEST=tmu run` is the
regression test for anything touching the exception path; build the runtime
with `-DDBT_TRACE_EXC` to print every exception delivery as it happens (`[exc] vec= blk=+off kind= resume= where=`; where 1 = helper, 2 = translated code, 3 = outside) and the last 16 landings with the stats.

The guest blob carries file contents only, so guests zero their own `.bss`
(`guest/start.S`); a CE image does that itself.


### Touch/performance investigation (2026-11-05)

`make -C dbt PROFILE=1 ...` reserves TMU1 (Pphi/4, interrupts off) and
records calls, cumulative ticks and maximum ticks for completed translations,
ASID changes, TLB hooks, block invalidation, LCD pixel streaming and full
cache flushes. Statistics are printed by HC_STATS and every 4096 guest RTES
in profiling builds. Normal builds do not read the profiling timer.
HC_GETSTAT 11 reports whether profiling is enabled; 12 is elapsed TMU1 ticks;
13 is skipped same-ASID notifications; 14 is TLB scans skipped by the user-code
page filter. Indices 32..58 are nine triplets (calls,total,max), in the order
above plus exception delivery, rte and lookup (2026-11-06, "Measuring on the
device"). Counters wrap modulo 2^32; take deltas less than one timer wrap apart.
Buckets overlap (TLB includes any invalidation); do not sum them. Translation
only measures completed attempts, not C work abandoned on a guest fault.
Do not combine PROFILE with a guest that owns/reprograms TMU1. At the stock
clock 7,372,800 ticks are one second on hardware; emulator tick counts are
instruction-model costs, not measured hardware latency.

The shell exceeded the former 8192-block cache. Instrumented NK-shell-r2 runs
show repeated full flushes and large translation, ASID and TLB costs. A PTEH
write with an unchanged ASID now returns without walking the block/IC lists.
A conservative 8192-bit filter records 1 KiB pages containing user code;
1/4 KiB TLB refills for pages absent from this filter skip the user-block
scan. Collisions and dead blocks only cause extra scans, never omitted
invalidations; large mappings retain the original scan. Bits are published
before block visibility and reset only at full flush. The aspace guest checks
same-ASID notifications as well as remapping and cross-page chained branches.
An instrumented shell snapshot skipped 40,584 of 47,074 TLB scans (86.2%).
This is a scan count, not a claim of an 86% overall speedup.

The DCS display path now preserves the dirty bottom row and limits each native
LCD trap to four rows, returning to interruptible guest execution between
strips. The push thread enforces its delay even when drawing signals its event.
Measured emulator pixel-loop maxima went from 92,816 ticks for a full-screen
push to 706 ticks for a four-row strip. These omit trap/window setup and are
not hardware timing measurements. R61524 keeps its bottom-of-frame presentation
rule; the optional legacy Blit=2 path is not the bounded DCS default.

### Incremental cache and address-space optimization (2026-11-05)

The current layout keeps CE's 8 MiB guest RAM and uses a 3 MiB code cache,
14400 compact block headers (now 36 bytes on target), and a 2624 KiB metadata arena. Twelve disjoint
banks recycle code and metadata together. Before reuse, incoming direct
links and inline caches are detached; surviving code stays at its original
host address. FIFO is the default. `DBT_CACHE_LRU` is an experimental policy
based on C-side access samples, and was slower on the directory benchmark.
The `flushes` counter also counts bank recycling, since it guards callers
against patching sites whose storage may have been replaced.

Each block stores only its used links. Instruction recovery metadata uses
6-byte base records with extra target/fallthrough words only for branch
records. The translator still uses full records in scratch storage. Host
tests compare packed and unpacked recovery metadata and host-offset lookup.
Bank-local host-address searches account for holes left by invalidation.

ASID changes clear only hash slots and inline-cache owners recorded as
containing user targets. User IC refills pass through C to register the
owner. Kernel-target ICs remain valid. A page index narrows 1/4 KiB TLB
checks to blocks touching those pages; large mappings retain the full scan.
Index nodes are rebuilt when bank indices are reused. Direct-link bank
masks conservatively skip links that cannot target an invalidated bank;
IC targets are still checked independently.

`tools/bench_my_device.py` drives two fixed taps and waits for an exact RGB
match of rows 50 through 199 of a full directory reference. Apply
`dbt/emu-patch/add_classpad_benchmark.py` to an isolated emulator checkout
first. The reference excludes the clock but includes all directory icons
and labels. Both versions must use the same flattened NK image, reference,
emulator, and tap tick. The result records simulator ticks and host wall
time separately. The conversion of 32768 simulator ticks per modeled second
is not a measurement of physical ClassPad latency. Run serially when
comparing wall time. A failure to match is a failed benchmark.

Local fixed-image measurements (old NK SHA-256
`608a262214e706b5cdfd37dc79dad05f08b262765ca25c9aa07e9766de914c07`):

| Runtime | Load ticks after second release | Modeled seconds |
| --- | ---: | ---: |
| Original 8192-block baseline | 3196152 | 97.539 |
| Compact headers, larger cache | 1739424 | 53.083 |
| Active user IC list | 1222908 | 37.320 |
| Page index and bank recycling | 940080 | 28.689 |
| Packed metadata, FIFO banks | 847806 | 25.873 |
| Preserve surviving hash and IC entries | 842346 | 25.706 |
| Same runtime, experimental `-O3` build | 804672 | 24.556 |
| Direct-link destination-bank filter (`-O2`) | 823782 | 25.140 |

These measurements have not met the requested 10-second target. The power
test image is separate and is not substituted into this comparison.


Further experiments retain the same CE image: narrow decoded instructions
from 84 to 28 bytes, an optional 512-entry opcode decode cache
(`DBT_DECODE_CACHE`), and `-O3 -flto`. The decoder's 21 exposed fields were
compared for all 65536 opcodes against the original decoder, including
cache hits and intervening fills; they were identical. Six-byte metadata
stores guest offsets in halfwords and aligns its auxiliary target tail to
four bytes. Boundary tests cover offsets above 255 bytes, r15, maximum
record count, and both tail alignments. TMU recovery, ASID remapping and
cache-pressure guests pass with the compact structures; TMU and ASID also
pass with LTO. These flags remain experiments until directory timings and
broader runtime checks establish the selected configuration.


The additional ASID-reuse regression overwrites one UTLB slot without
MMUCR.TI: ASID 1 maps A, ASID 2 reuses A, then ASID 2 maps B. It failed
before the fix (stale A translation survived) and passes afterward. Reused
blocks now carry ASID_ANY for invalidation, and cross-ASID physical reuse
is restricted to a single minimum-size page, since one physical address
cannot validate a second page. Benchmark variants preceding this fix are
exploratory results, not validation of the final runtime.

| Additional experiment (same fixed NK) | Load ticks | Modeled seconds |
| --- | ---: | ---: |
| Six-byte metadata, 3 MiB cache, O3 | 782832 | 23.890 |
| Six-byte metadata, 3.25 MiB cache, O3 | 777372 | 23.724 |
| Compact decoded instruction structure | 776826 | 23.707 |
| Optional opcode decode cache | 785016 | 23.957 |
| LTO with opcode decode cache | 747342 | 22.807 |

The decode cache is not enabled by default: its measured overhead outweighed
its savings in this workload. LTO and the corrected shared-ASID reuse path
are being benchmarked separately before a default configuration is chosen.


Current experiments additionally support `DBT_LINEAR_META`: adjacent copied
ALU instructions share one recovery range, with guest PC advancing by the
host offset inside that range. The entry prologue, rewritten operations,
branch slots and exit sentinel retain their own recovery records. TMU
interrupt recovery passes with this encoding. The compact block header is
36 rather than 56 bytes on the 32-bit target; offset/count bounds are checked
at compile time.

`DBT_PROFILE_BANK` logs occupied block count, code bytes and metadata bytes
at bank rotation. This disproved a simple block-cap-only explanation:
different banks exhaust different resources. Larger block tables alone
consume metadata space and are not a substantial improvement.

`DBT_INLINE_DEVICE_READ` executes unswapped P4 loads directly, retaining a
commit point for post-increment recovery. P4 writes still use C so PTEH and
MMUCR effects are observed. HC_GETSTAT 6 counts C device-helper accesses;
it excludes inline reads. `DBT_DEDUP_LITERALS` shares only untagged constants
after their translation-time values are finalized; mutable link and IC
literals retain individual storage. `DBT_RUNTIME_MAX_INSNS` varies block
length without changing metadata capacity.

The device test exposed an independent borrowed-register bug: hypercalls
are intercepted directly from live registers, so borrowing an implicit
r4-r7 argument corrupts the call when no instruction in that block explicitly
writes it. The pre-pass now reserves r0/r4-r7 for F0..FF hypercalls. The
regression passes without diagnostic prints. The TMU arrival test now uses
elapsed timer ticks rather than a fixed empty-loop count, which can finish
before three periods after optimization. Its interrupt-count requirement
is unchanged.

| Experiment | Load ticks | Modeled seconds |
| --- | ---: | ---: |
| LTO with corrected shared-ASID invalidation | 762084 | 23.257 |
| Larger block table alone | 758808 | 23.157 |
| Coalesced linear metadata | 754986 | 23.040 |
| Compact header and 20160-block table | 752802 | 22.974 |
| Inline P4 reads (before hypercall-register fix) | 685644 | 20.924 |

The latest combined build and 16/32-instruction variants are still under
test. The 10-second target remains unachieved; these are emulator results,
not physical-device timing claims.


`DBT_BATCH_INVALIDATE` marks all affected code ranges in a 32-byte-granularity
bitmap, then detaches incoming IC/direct links in one pass. Bank reuse clears
its bitmap bits. ASID, data-TLB and cache-pressure regressions pass. It does
not substantially improve the measured directory time, so remains opt-in.
`DBT_KERNEL_BANKS` partitions kernel/user bank allocation; the 4/8 split
regressed and is not selected.

`DBT_BASIC_BLOCKS` ends a translation at conditional branches, creating
separate successors. Its host tests and TMU/ASID regressions pass, but its
measured latency is worse than the 16-instruction cap. Shorter blocks are
not monotonically faster: fixed headers and exits eventually dominate.

| Experiment, including hypercall-register fix | Load ticks | Modeled seconds |
| --- | ---: | ---: |
| Combined 64-instruction configuration | 700932 | 21.391 |
| 32-instruction cap | 693288 | 21.157 |
| 16-instruction cap | 492906 | 15.042 |
| 8-instruction cap | 809586 | 24.707 |
| 4-instruction cap | 1185234 | 36.170 |
| Conditional-branch basic blocks | 748980 | 22.857 |
| Batched invalidation, 32-instruction cap | 696564 | 21.257 |
| 4 kernel / 8 user banks, batched, 32-instruction cap | 1078218 | 32.905 |

`DBT_IRQ_ENTRY_CACHE` remembers up to 16 safe interior instruction entries
for RTE. It accepts only blocks without a borrowed GPR, excludes pending
scratch saves, and requires matching ASID, cache generation and a live
owner block. Interior aliases are never published in the ordinary hash.
Both linear-copy ranges and explicit instruction starts are supported;
other recovery kinds fall back to normal lookup/translation. HC_GETSTAT 15
reports hits, and 16 reports whether this experiment is enabled. The TMU
guest verifies that the path was actually exercised while preserving state;
the ASID regression also passes. Its directory timing, a no-borrow variant,
a 12-instruction cap, and an 8-instruction build with more headers remain
under measurement. None is yet a verified sub-10-second result.


The fixed-image experiments continue to show strong scheduling sensitivity:
removing redundant work can change the kernel/cache working set enough to
regress total latency, so local instruction savings alone are not accepted
as an end-to-end speed claim.

| Experiment | Load ticks | Modeled seconds |
| --- | ---: | ---: |
| Interior-entry reuse, borrowing enabled, 16 instructions | 733692 | 22.391 |
| Interior-entry reuse, borrowing disabled, 16 instructions | 476526 | 14.542 |
| 12-instruction cap | 757170 | 23.107 |
| 8 instructions, 28032 headers / 2080 KiB arena | 807402 | 24.640 |
| Skip unused literal-array clearing, borrowing enabled | 704754 | 21.507 |
| Also skip the borrow pre-pass when borrowing is disabled | 647424 | 19.758 |

The pre-pass is unnecessary when no register will be borrowed, and emitter
literal entries are fully initialized before publication through nlits.
Host tests and TMU/ASID/device regressions pass after removing those redundant
operations; their aggregate timing above remains worse than the best earlier
configuration.

A separate clock diagnostic reads the frozen image's g_pNKGlobal pointer
at 0x8c1068dc and dwCurMSec at offset 0xc8. These locations were verified by
matching OALTimerIntrHandler machine code against the old image; they are
not a stable ABI and are guarded by DBT_CE_TIME_PROBE. Its log compares CE
milliseconds with free-running TMU1. Busy periods lose elapsed time because
the OAL adds a fixed millisecond per delivered interrupt, even when multiple
hardware periods pass while interrupts are masked.

A local diagnostic flat image changes exactly one byte at 0x838658be:
OEMInit's OALTimerInit argument changes from 1 to 10 ms. The call target,
original opcode and all other image bytes were checked; the provenance
manifest is in build-tick-diagnostic/patch.json. This is explicitly a
separate BSP/tick experiment, not a same-image translator speed comparison.
No production BSP timer default or shared Windows build files have yet been
changed for this experiment. Normal production images must be rebuilt from
source before adopting such a change.

The 10 ms diagnostic completed with exact My Device reference-frame matches:

| Translator | Original 1 ms NK | Diagnostic 10 ms NK |
| --- | ---: | ---: |
| No-borrow single-pass + explicit initialization + time probe | 19.708 s | 13.143 s |
| Earlier no-borrow IRQ-entry build | 14.542 s | 13.859 s |

These are modeled emulator seconds (ticks / 32768), not calculator timings.
The 10 ms runs took 16.116 s and 17.104 s of host wall time after the second
release, respectively; overlapping emulator runs make wall-time comparisons
unsuitable for attributing a speedup. Both remain above the 10 s objective.
The diagnostic image is not installed in the Windows build tree.

For the earlier no-borrow IRQ-entry build, profile snapshots around the
My Device tap account for approximately 5.746 s in 33,477 translations,
0.046 s in 13,461 ASID notifications, 0.871 s in TLB hooks, and 0.781 s
in invalidation. These are coarse snapshot deltas, not exact click-to-frame
boundaries, and the TLB/invalidation buckets overlap. After the ASID fast
path, translation remains a larger measured cost than ASID notification
itself; effects of ASID changes on subsequent cache misses are not included
in the ASID bucket.

The newly successful power build's normal NK.bin (SHA256
bec974fca710d06f1832f1d665fa9696ac33d6342facec3ef2521590413b73f5)
was separately checked with the earlier no-borrow IRQ-entry translator.
My Device matched the full reference at tick 7997262: 485262 ticks after
the second release, 14.809 modeled seconds and
18.166 host wall seconds. This verifies loading with
the new power image, not a same-image comparison against the old baseline.
Artifacts and input hashes are under build-speed-review/power-current.

### Reusing safe interior entries

DBT_INTERIOR_ENTRY extends the page index to kernel code and lets cold
lookups/RTE find an instruction inside an existing block. dbt_block_entry
requires an even interior PC, no borrowed register, an INSN or linear
instruction boundary, and no pending scratch save. Delay slots and end
sentinels cannot be used. User entries require the owner's current ASID;
this path deliberately does not broaden cross-ASID physical-code reuse.
Aliases are never published in the start-PC hash. ICs and direct links
already detach targets by host range, and the page index is rebuilt on
bank reuse, so the owning block controls the alias lifetime.

The interior guest confirms that reuse actually occurs, then modifies the
owner and verifies an already-used call site sees the replacement code.
The aspace guest additionally checks user interior reuse, ASID isolation,
switching back, and a bare ldtlb remap without a full cache flush. TMU and
cache-recycling guests pass. Host tests cover odd/start/end/delay-slot
rejection, outstanding spills, borrowed registers and packed metadata.

Same frozen NK, no borrowing, IRQ-entry reuse, O3/LTO, 20160 headers and
2528 KiB arena:

| Configuration | Load ticks | Modeled seconds |
| --- | ---: | ---: |
| 32 instructions, no interior reuse | 612480 | 18.691 |
| 32 instructions, interior reuse | 570984 | 17.425 |
| 64 instructions, interior reuse | 590094 | 18.008 |
| 16 instructions, interior reuse | 405000 | 12.360 |

All completed runs matched the exact directory reference frame. These are
experimental configurations, not a new default or evidence of meeting the
10-second objective.

DBT_DEVICE_DISPATCH separately simplifies the P4 classifier to
(top_byte + 1) >> 8, with the device path before the ordinary-memory path.
This removes four T-neutral arithmetic/shift instructions per tested
access without changing the normal path's branch count. The ordinary path
has its own recovery record after the device path. Device tests cover T,
MMIO, normal memory, addressing modes and delay slots. When this option is
present, the TMU guest marks only its assembly stress loops device-aware,
leaving its pre-swapped MMIO handlers outside that range; interrupt state
and memory recovery pass on the new dispatch path.

With 32-instruction blocks, the compact device classifier completed in
575898 ticks (17.575 modeled seconds), versus 570984 (17.425) without it.
Thus the shorter emitted sequence has not demonstrated an end-to-end gain
in this configuration. Both borrowed and non-borrowed device regression
builds pass; the option remains experimental. The 16-instruction interior
configuration also passes its own TMU regression, including actual entry
reuse after interrupts.

The 16-instruction compact-dispatch combination also regressed: 576444
load ticks (17.592 modeled seconds) versus 405000 (12.360) without it.
The compact-dispatch code and its conditional test paths were therefore
removed from the worktree; its source snapshot is retained only under the
ignored build-speed-review/dispatch-source directory. The safe interior
entry optimization and its regression tests remain.

On the latest power-enabled normal image (SHA256 bec974fc...3b73f5),
interior16 matched the reference at tick 8115744: 603744 load ticks,
18.425 modeled seconds, and 24.175 host wall seconds. The same image's
previous IRQ-entry build took 14.809 modeled seconds. Consequently the
12.360-second result on the frozen touch image does not demonstrate an
improvement for the current image, and DBT_INTERIOR_ENTRY remains disabled
by default. The next performance work must explain this image-dependent
cache/scheduler sensitivity and verify gains on the current normal image.

### Host-side sampling and T preservation

The optional add_dbt_sample.py emulator patch samples native PC/sleep once
per timer quantum after HLE_DBT_SAMPLE. It does not modify guest memory,
registers or clocks. Repeating the latest normal image reproduced the exact
endpoints 7997262 (IRQ-entry build) and 8115744 (interior16). The samples
include one boundary tick and are approximate attribution, not instruction
cycle counts: the former had 372411 C/stub, 111187 translated-code and 1665
sleep samples; the latter 368018 C/stub, 234075 translated-code and 1652
sleep samples. Thus the regression is not mostly idle waiting, and total
C/stub occupancy barely changed despite lower translation cost.

DBT_INTERIOR_CACHE adds 256 direct-mapped safe-entry records with ASID,
owner index and per-bank generation. Only recycling the owner bank (or a
full flush) changes its generation; individual invalidation is checked via
the owner's dead flag. Host zero is published first and last with compiler
barriers around the key/owner fields. ASID-return reuse, remapping and a
small-cache full-bank recycling test pass. Before the T fix below this
option took 580812 ticks (17.725 modeled seconds) on the latest image, a
modest reduction from 18.425; it remains experimental.

Inspection then exposed a correctness defect: inline-cache cmp/eq and the
lookup stub's shifts/hash comparisons changed SR.T across guest indirect
jumps/calls/returns. The tflag guest failed cold T=1 and warm T=0 before
the fix. ICs now temporarily save SR in X1 and restore it on both paths.
A DBT_SPILL_T recovery record restores T if interrupted before restoration.
The lookup stubs separately save SR before their tests; their exception
windows recover the saved T, and both hit and C-call paths restore it.
The original T-flag cases now pass, including the X0 target form whose delay
slot overwrites the target register. TMU adds an exact 30000-result loop
carrying both T values through indirect calls under interrupts; it passes.
Earlier timing results are historical measurements of a version with this
known correctness defect and must not be selected as the final artifact.

After T preservation, the current normal NK matched the directory reference
in 665988 load ticks (20.324 modeled seconds) without interior reuse,
and 587910 ticks (17.942 seconds) with interior reuse plus the 256-entry
cache. Both use 16-instruction blocks and no borrowing. The latter took
23.846 host wall seconds in this run. This remains above the 10-second
objective. The T fix stays enabled: removing it to recover an earlier time
would reintroduce the demonstrated ISA error. Cold/warm register and X0
calls, T across return, interrupted T-carry loops, ASID remaps and bank
recycling tests pass.

### Publishing safe aliases in the fast hash

DBT_INTERIOR_HASH publishes validated interior PCs in the normal assembly
lookup hash. A bitmap records which owners have aliases. Individual block
invalidation scans and clears all hash destinations inside that owner's
host range; bank recycling already clears the entire victim host range.
ASID changes still clear user hash entries through the existing sparse list.
Owner marks and hash publication are made with interrupts masked.

The interior guest verifies hash aliases are removed on code modification,
then forces all banks to recycle and checks the target again. A negative
build using a temporary rt.c with alias removal disabled failed both stale
code and alias-removal checks; the normal source passes. ASID/remap tests
also pass. With T preservation and the current normal NK, fast alias hashing
completed in 411006 load ticks (12.543 modeled seconds), versus 587910
(17.942) with only the separate C entry cache.

DBT_UTLB_HINT is a separate candidate: it remembers one UTLB slot number per
bucket, but re-reads and checks both hardware words on every use. It caches
no physical address. VPN, page size, ASID/shared and validity checks are the
same as the full scan, which is retained on a mismatch. ASID/remap regression
passes; performance is still being measured.

The current normal-image tick diagnostic changes one byte at 0x838662ca:
OEMInit's delay-slot mov #1,r4 becomes mov #10,r4. The unique OEMInit
signature, jsr opcode, call literal 0x83865728 and counts-per-ms literal
7372 were verified before patching. Its manifest is build-tick-current/patch.json.
This is a separate BSP configuration experiment and has not changed the
production BSP source or shared Windows files.


### Current-image checks and local alias removal (2026-11-06)

The UTLB-hint profiling build reached the exact My Device reference frame
in 393534 ticks (12.009705 modeled seconds). The separate 10 ms image with
the preceding alias-hash build took 397356 ticks (12.126343 seconds).
Neither establishes a sub-10-second result.

The shipped test loader in `build-device-hh3-test/wince.hh3` uses DEVICE=1,
PROFILE=0 and the UTLB-hint configuration, without HASH_FOLD14. Its exact
translator payload was subsequently exercised via the matching .g3a against
the current normal NK image: 576444 ticks (17.591675 modeled seconds), exact
frame match. Result and screenshot: `build-speed-review/device-hh3-current/`.
This does not exercise HollyHock OS calls/flashing or prove hardware timing.
The profiling configuration's 12.01 seconds must not be attributed to this
loader. Its bytes have not changed after delivery.

`DBT_ALIAS_LOCAL_REMOVE` narrows alias invalidation from all 8192 hash slots
to the slots of every aligned guest instruction address inside the owner.
All published aliases originate in this interval; the destination host-range
check remains necessary to preserve other blocks after hash collisions.
This avoids extra reverse-index storage. Expanded interior regression checks
two distinct aliases, invalidates their modified owner, verifies both see the
new code, and recycles all cache banks. This and the ASID/remap guest pass.
Profiling and DEVICE=1/PROFILE=0 directory benchmarks are tracked separately;
no speed improvement is claimed until their exact-frame runs finish.

The local-removal profiling run completed at 572622 ticks (17.475037
modeled seconds), with an exact frame match. It regresses versus the
12.009705-second profiling baseline despite reducing the invalidation
search bound. It is not enabled in the delivered HH3. The unprofiled
device configuration must be evaluated independently.


### Hardware black screen at DDI takeover (2026-11-05)

User reports no CE desktop, backlight off precisely when the CE display
initializes, and Clear/ON does not restore it. Treat this as a display-handoff
failure, not proof of automatic suspend. The new power-state normalization
experiment is gated by DBT_BOOT_POWER_INIT and is NOT in the LCD-sync loader.

Compared with ClasspadDev/gint dev src/r61523/r61523.c, native HC_LCD_PUSH
and the debug console lacked SYNCO barriers around cross-bus RS/command
transitions. rt/lcd_io.h now completes preceding writes, RS-low, command,
and RS-high separately; HC_LCD_PUSH parameters and the final pixel batch
are also drained. No per-pixel barrier is added. Existing host regressions
and target ELF/payload checks pass; physical causality is still unproven.

The reference driver also inherits the live Casio panel configuration,
whereas our CE PanelInit unconditionally emits SLPOUT and COLMOD=0x55.
The native lcdprobe.hh3 diagnostic isolates native window/pixel writes and
these initialization commands one step at a time, outside CE and DBT. It
uses Casio UI and Clear/EXE; it does not link the flash writer or take over
the machine. Its successful build is not a hardware validation. The CE
translated LcdSelect path still lacks explicit barriers and remains under
investigation; the new native helper only covers native push/console paths.

Hardware lcdprobe result: user reached step 4 successfully. This rules out
a deterministic failure of these commands in the native Casio environment,
but does not validate their translated CE execution or scheduling. The next
controlled hardware check is build-device-lcdsync/wince.hh3 with unchanged
NK.bin; its boot-power-init experiment remains off.


### Sub-five-second work after baseline commit 9675991

Keep the normal NK SHA bec974fca710d06f1832f1d665fa9696ac33d6342facec3ef2521590413b73f5
and native LCD SYNCO barriers fixed. DEVICE=1/PROFILE=0, O3/LTO,
20160 headers, 2528 KiB metadata, 16-instruction cap, no borrowed register,
linear metadata, inline device reads, literal deduplication, IRQ/interior
entry caches, interior hash publication and UTLB slot hints form the new
comparison configuration. They are opt-in, not Makefile defaults.

`DBT_USER_IC_HASH_HIT` lets the IC miss stub probe the ASID-scoped hash for
user targets. A hit jumps directly without publishing a new IC, so no
active-owner registration is required. Hash misses still call the existing
C resolver. SR.T restoration remains unchanged. HC_GETSTAT 30 indicates
this feature. The ASID guest verifies 4096 alternating user calls avoid
per-call C refill, both T values survive alternate targets, and all existing
ASID/remap tests pass. TMU interrupt/register tests also pass.

Measured exact-frame times with the current normal NK:

| Configuration | Ticks after second release | Modeled seconds |
| --- | ---: | ---: |
| Optimizations plus LCD synchronization | 584088 | 17.824951 |
| Plus user IC hash-hit bypass | 568800 | 17.358398 |

These are emulator times, not hardware speed claims. Both runs use host-side
sampling from tick 7512000. Baseline samples: 1694 sleep, 245156 translated
code, 337239 runtime; candidate: 1666 sleep, 243663 translated code, 323472
runtime. Sampling includes the release tick and hence totals one more tick
than the load duration. The improvement is about 2.6%, not the requested
sub-five-second result. No new artifact has replaced the hardware baseline.

The host sampler can now attribute cached PCs to current 36-byte block
headers, using caller-supplied exact ELF table addresses and bank sizes.
It reads backing RAM without emulated accesses. A TMU test mapped all 36330
translated-code samples with zero unresolved owners. `analyze_dbt_samples.py`
checks category totals; `analyze_ce_samples.py` maps guest block PCs through
NK ROM sections and timestamp-matching CE MAP files. Overlapping EXE VAs
remain ambiguous, and mismatching MAPs are skipped. This machinery is for
profiling, not part of the device runtime.

The 32-instruction variant (`userhash32-sync`) matched the final frame in
564978 ticks / 17.241760 seconds. This is a small gain, still far from 5 s.
An optional `DBT_HOST_OWNER_CACHE` now caches 256 host-to-block lookups,
checking the containing range and bank generation before reuse. Both full
flush and bank recycling advance generations. The ASID regression passes;
the interior-entry pressure regression passes after 13 bank evictions.
The production-size 32-instruction timing run is `owner32-sync`.

The shared Windows MAP files have since changed and no longer match the
fixed NK timestamps. The analyzer now retains ROM section/module attribution
without accepting mismatching function symbols. Across all 243663 guest
samples, kernel.dll accounts for 173621 (5.298492 modeled seconds), the display
driver for 26886 (0.820496 s), and GWES for 18152 (0.553955 s). These totals
cover all guest samples rather than only the top 50 function rows; runtime
samples are separate. Current function names cannot be validated from the
changed shared MAPs. Keep the test NK fixed for comparisons.

The host-owner lookup cache measured 545322 ticks / 16.641907 s with the
normal image (`owner32-sync`), versus 564978 / 17.241760 s without it. Both
matched the reference. The separate 10-ms diagnostic image completed at
tick 7878234 (`owner32-tick10`); this is not a same-NK comparison.

### Physical suspend failure and recovery scan

User reports Suspend gives a white panel and Clear cannot resume, while
normal direction keys and EXE work. This invalidates the earlier emulator
success as evidence of physical suspend readiness. Normal keyboard operation
has a polling fallback; the old suspend path stopped every TMU and depended
entirely on KEYSC interrupts. The emulator injects key-edge IRQs directly and
does not prove the physical scanner produces those IRQs in this state.

The revised retained-RAM path preserves TMU0 reload/counter/control and uses
it for a 20-ms recovery scan while CE is suspended. The ISR returns NOP,
without updating CE time or scheduling threads; only Clear sets the resume
flag. It restores the previous timer and masks before returning. This is
light sleep with periodic internal CPU wakeups, not verified deep standby or
verified low power consumption. KEYSC and TMU clock gates are kept enabled.
The translated power regression masks KEYSC for the first cycle and verifies
Clear still resumes, EXE does not, two cycles complete and timer configuration
is restored. Hardware and rebuilt full NK validation remain outstanding.
The display PowerHandler still lacks backlight control; its white-panel
behavior is unresolved and must not be reported as fixed by this change.

`owner32-tick10` completed in 366234 ticks / 11.176575 modeled seconds with
an exact reference match. The same Loader on normal NK took 16.641907 s.
This supports measuring timer overhead separately; it does not justify a
production timer change without a rebuilt image and power/input checks.
A repeat with guest/native PC sampling is `owner32-tick10-sample`.

`DBT_SR_ONE_SCRATCH` reuses the MD-mask temporary for the shadow-MD load
when translating STC SR and STC.L SR. It removes one scratch spill/restore
pair, without changing the MD calculation, memory byte order, or SR.T.
The 175 host translator checks pass with the flag, as does the interrupt
recovery/T/register TMU guest. The same-normal-NK benchmark is `sr32-normal`;
no speedup is claimed before that run completes. This flag remains opt-in.

### Integration of claude/device-profiler-tickless

The test branch combines b05c23f with the current translator/cache/wake
work, keeping main and the Claude worktree unchanged. HC_GETSTAT 23 retains
the branch's exceptions counter; the user-IC optimization feature moves to
30, with its regression updated. The power recovery ISR runs before normal
timer accounting. The device stats app uses the proven CE test-app include
and library paths. Tick-sample shares are event-biased under variable tick;
PROFILE builds use independent TMU1 time for interval rates.

A production-timer register-model regression reproduced a missing-latency
bug before the fix: after underflow, CountsSinceSysTick clamped instead of
including the elapsed reloaded counter. It now reads a consistent side of
the reload and includes that latency. Reschedule updates mask interrupts,
restore the prior enable state, and SetPeriod re-reads TCNT at programming
time instead of using a stale pre-call sample. Tests cover pending reload,
reload during a read, programming delay, earlier/past deadlines and bypass
of normal timing during suspend. Fine-grained timing between the final
counter read and write still requires real-target clock-drift validation.

Host tests, TMU interrupt recovery and the ASID/remap guest pass for the
integrated runtime. A Windows rebuild and a complete tickless NK run remain
required before claiming boot, suspend, touch or sub-five-second timing.
`tools/windows/build-tickless.bat` builds OAL and the optional stats app;
the app is available under Windows and is not launched automatically.

The cache initialization now runs from P2 before BSS/C initialization,
purges the four operand-cache ways without stack spills, and enables
ICE/OCE/CB with WT clear. There is no new launcher display or prompt.
The dirty-purge/cache-control order follows the SH4 startup pattern in
https://github.com/torvalds/linux/blob/master/arch/sh/kernel/cpu/init.c;
SH4A cache geometry is described by its companion sh4/probe.c. Runtime
code synchronization now completes writeback before instruction invalidation.
Tests cover cache-off and inherited write-through entry, IRQ recovery,
ASID/remaps, and cache-bank recycling. On the existing fixed normal NK,
cachefast-normal matched My Device at tick 8055684 (543684 load ticks,
16.591919 modeled seconds). The emulator's operand-cache backing is coherent;
this does not prove physical dirty-line writeback or hardware speedup.
