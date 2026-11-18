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
  driver's window commands and its pixels. Its rotated form (2026-11-09,
  `bsp/classpad50/SRC/INC/classpad_lcdrot.h`): r7 = 0xA5000000 | DMDO angle,
  r4 = an unrotated logical frame of the panel turned by that angle (528x320
  for 90/270), r5 = physical rows py0 | py1 << 16, r6 = physical columns
  px0 | px1 << 16; the translator sets that window and streams it in panel
  order from transposed reads of the frame, so the display driver never
  shows GPE a rotated surface (docs/08). The `dcsrot` guest checks it.
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
but a window on the CE desktop. (That window, `classpad_dbtstat.exe`, and
the opt-in `PROFILE=1` TMU1 buckets were removed later the same day; the
always-on tick sampling and counters below are still read through
`HC_GETSTAT`, and the measurements in this section are kept as the record.)

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
  histogram at the code that caused them.
- **Exception counters**: every delivery by vector (general, TLB miss,
  interrupt) and the first 16 INTEVT codes seen with their counts, so a
  storming source or an unexpected TLB-miss rate shows at once.
- **Machine state**: CCR, MMUCR, FRQCR and the RTC's seconds of day. The CE
  kernel programs CCR itself in `SHInit` (`CACHE_ENABLE | 0x80000105`:
  both caches on, P1 copy-back, no write-through), which `mdsh3.c` in the
  shared source confirms; the read-out shows what the hardware actually
  holds. The RTC seconds against GetTickCount give the real TMU clock
  (bsp_cfg.h assumes Pphi 29.4912 MHz, from casio-emu).

All of it is read with `HC_GETSTAT` (indices 64 up; the list is in
`bsp/classpad/SRC/INC/classpad_hc.h`), which works from user mode: a
`trapa` is a general exception the translator takes before the kernel sees
it. `print_stats` prints the same `[sample]` lines in the emulator (at
HC_STATS/exit).

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

#### First numbers from the hardware (2026-11-06, fx-CG500 = fx-CP400 board)

Shell image, variable-tick OAL, `DEVICE=1 PROFILE=1` translator, read 38 s
after the desktop appeared with the device idle:

| line | value | reading |
|---|---|---|
| `tick/rtc` | 1.692 (really 1.62..1.77, the RTC is read in whole seconds) | TMU0 and TMU1 count about 1.67x faster than `bsp_cfg.h` assumes: the CP400 does not run the fx-CG50 clocks. CE time, `Sleep`, every "second" in the window and in TMU1 profiles run that much too fast. `TMU1 2.00 s per 2000 ms` only says both timers share a clock. |
| FRQCR | 0x18001102 | casio-emu's ClassPad profile uses the CG50 value 0x0F011112: PLL x16 instead of x25 by the emulator's own decoding, which alone gives 1.5625, outside the measured range, so FLLFRQ differs too. Indices 384/385 now read PLLCR/FLLFRQ. |
| CCR / MMUCR | 0x105 / 0x7800F001 | as in the emulator: caches on, copy-back. |
| `irq/s` | 110, all INTEVT 0x400 | the tick really is on demand, but something sleeps every ~10 tick-ms (a polling driver thread: touch or keyboard); nothing else interrupts. |
| `tlb/s` 82, `rte/s` 373, `ldcsr/s` 2559 | per wakeup: 0.75 TLB miss, 3.4 rte, 23 ldc-sr helper calls | the kernel's wakeup path, each a C round trip. |
| `exc` 3 % | about 1150 deliveries per interval (irq + TLB + the window's own 384 hypercalls per poll) | roughly 50 tick-us (30 real us) per exception delivery in C. Cheap while idle, the dominant term in a boot with hundreds of thousands of deliveries. |
| `lcd` 2 %, `tlb` 1 % | the window's own repaint; about 120 tick-us per TLB hook | |
| `xlat max 79921 us` | one translation held interrupts for 48 real ms | index 386 now records the guest pc of the slowest translation. |
| samples | 221 per 2 s, code 99 %, idle 83 %, user 2 %, kernel-other 6 % at 0xC0425E50, hot ROM bucket 0x83E60000 | with the variable tick the samples are the wakeups, not a uniform clock: shares are indicative, the TMU1 buckets are the time measurement. |

What this changes: fix `PERIPHERAL_CLOCK_FREQ` (and the emulator's ClassPad
CPG preset) from a longer `tick/rtc` run, a few minutes narrows it to a few
tenths of a percent, or from PLLCR/FLLFRQ once read; then measure during
boot and app launches (page 1, "hot since start", and a capture), not at
idle, where there is nothing left to speed up.

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
R-standby (docs/06) also needs `dbt/emu-patch/add_sdmr.py`: the SDRAM
mode-register command the resume stub writes at 0xFEC15040 is otherwise an
invalid address outside the emulator's cold-boot mode.

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

(Removed 2026-11-06 with the rest of the opt-in profiling.)
`make -C dbt PROFILE=1 ...` reserved TMU1 (Pphi/4, interrupts off) and
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

The 2026-11-05 layout kept CE's 8 MiB guest RAM and used a 3 MiB code cache,
14400 compact block headers (now 36 bytes on target), and a 2624 KiB metadata arena; on 2026-11-06
the cache briefly shrank to 2 MiB (9600 headers, 1752 KiB arena) to give CE 11.3 MiB; CE
then went back to 8 MiB, now starting at 0x8C000000, and the translator kept the
2026-11-05 tables and took the launcher's freed MiB as a 4 MiB code cache (docs/05). Twelve disjoint
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

`DBT_PROFILE_BANK` (since removed) logged occupied block count, code bytes and metadata bytes
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


### Guarded same-mode SR helper experiment (2026-11-06)

`DBT_FAST_LDC_SR` is opt-in. After the normal helper prologue has saved
r0-r2 and masked interrupts, it checks unchanged RB, hardware PTEH ASID
against the translator's current ASID, and unchanged guest shadow MD.
Only then does it load the new SR (forcing host MD/DSP) and restore the
three borrowed registers. It does not call C or save untouched registers.
Any failed guard uses the existing full helper with the original entry SR.
The unmasked return tail has explicit exception-recovery ranges, equivalent
to the hash-hit tail; it preserves both T states and guest bank contents.
HC_GETSTAT 31 counts fast returns, 63 bit 0 identifies the build, and 26
continues counting all guest SR helper calls.

TMU regression passes under interrupts and verifies at least 400000 fast
returns. ASID/remapping and bank-switch/SPC/SSR regressions pass. The bank
test also flips guest MD twice, checks its shadow readback and verifies
both transitions bypass the fast path. On the new skin/SIP NK, the exact content-region benchmark improves from
591186 ticks / 18.041565 s to 581358 / 17.741638 s. This is about 1.7%,
not a route to the 5-second objective by itself. The flag remains opt-in
and is not in the previously delivered integration loader.

The new Windows image also changes catalog features (softkb/msim and
console added, imaging/zlib removed). Its timing cannot isolate variable
tick against the preceding normal NK. Use the same new NK for subsequent
loader comparisons. The changed shell has a different toolbar, but the
existing benchmark content region (rows 50..199) remains identical. Its
first completed load is 591186 ticks / 18.041565 modeled seconds; the
complete new frame is retained for subsequent comparisons. The embedded
NK OEMInit was checked against its matching OAL MAP timestamp 0x6aed12c1:
0x83866682 loads 1000 into r4 and calls the new timer at 0x83865c34.
Thus the new timer configuration is actually present in the image.

A current fixed-10-ms-image comparison rejected combined
`DBT_ALIAS_LOCAL_REMOVE` + `DBT_BATCH_INVALIDATE`: 372240 ticks / 11.359863 s,
versus 355860 / 10.859985 s for the integration loader, both exact frame
matches. Host sampling attributes about 1.012 s to the batch finisher,
versus 0.641 s in the baseline invalidator (64-byte symbol bins are
approximate). ASID, alias invalidation and 12-bank recycling tests pass,
but the combination stays disabled because measured latency regresses.


Host sampler now accepts `HLE_DBT_COUNTER_BASE` and
`HLE_DBT_COUNTER_WORDS` (at most 512 words inside translator RAM). It reads
backing memory only at the sampling interval's start and normal exit,
reporting first/last/modulo deltas. Start after runtime initialization and
record the exact ELF symbol/layout; these are raw words, not a stable ABI.
This permits exception and INTEVT rates over exactly the directory-load
window without polling HC_GETSTAT or changing emulated execution.


The reverse destination-bank owner bitmap experiment was also rejected:
609750 ticks / 18.608093 s versus 591186 / 18.041565 with the same skin/SIP
NK. Code modification, ASID remapping and full bank rotation tests pass,
but the scan's sampled invalidation time rises from 1.537 s to 1.881 s.
The experimental implementation was removed; its benchmark artifacts remain.

Boundary snapshots of the integration loader's g_sample (ELF address
0x8ccd2228, size 0x4f0) over the 18.041565-second load count 30227 vector
0x400 TLB exceptions (1675.41/s), 3550 vector 0x600 interrupts (196.77/s),
and zero non-hypercall vector 0x100 exceptions. All interrupt deltas are
TMU0 INTEVT 0x400; KEYSC 0xbe0 and RTC 0x700 contribute zero in this
window. The counter-enabled run exactly repeats 591186 load ticks.
Uniform guest-PC attribution also repeats this time; 249036 guest samples
are resolved against the matching new image/MAP timestamps. The largest
individual functions are GPE EmulatedBlt_Internal (~0.611 s), KCNextThread
(~0.440 s), NKHandleCall (~0.338 s), and xKCall (~0.334 s). These are
block-start samples, not instruction-exact function timings.


### Current-image My Device experiments (2026-11-06)

Using the profile-review normal NK and the same exact-directory frame matcher,
the current-source non-profile device baseline is 592278 ticks / 18.074890 s.
Alias-local removal reaches 589548 ticks / 17.991577 s: too small a difference
to establish a useful gain. These are modeled times, not hardware wall times.

An owner-filter experiment used bit 15 of the existing destination-bank mask
to flag every inline-cache owner (including assembly refills). Individual
invalidation skipped owners with neither that bit nor the affected bank bit.
ASID/remap and interior-alias/bank-recycle regressions passed. The directory
run reached 587910 ticks / 17.941589 s, but approximate invalidate_block samples
increased from 45314 to 46147. No causal improvement was demonstrated; the
experimental source change was reverted and no device Loader was replaced.
Evidence is under dbt/build-speed-profile-review/{baseline-current,alias-only,owner-filter}.

The Control Panel -> System -> close Control Panel sequence was exercised
through native stylus injection twice. System retained stale Control Panel
client pixels; selecting Memory repainted and remained responsive. This is
not proof of a deadlock or a measured freeze duration. The optional live-touch
emulator patch and physical-ROM histogram mapper support further diagnosis.


### Reclaimed launcher workspace and range-only teardown batches

`DBT_BOOT_WORKSPACE_TABLES` is opt-in. The hash, block headers and physical
mapping metadata are placed in a NOLOAD low-BSS section below 0x8C100000.
crt0 first establishes its private high stack, then clears both BSS regions;
the launcher never returns after takeover. Link assertions bound the low
section below CE RAM and preserve 128 KiB for the translator stack. The
22288-block/2880-KiB-arena/14-bank experiment has low BSS ending 0x8C0FF6C0,
high image ending 0x8CC49BC0, stack/cache boundary 0x8CC80000, and 3.5 MiB code
cache. CE retains 8 MiB. ASID, interior-entry/bank-recycle and TMU tests pass;
the actual HHK takeover path also runs the ASID guest to RESULT: PASS. Both
My Device runs match: 561702 ticks / 17.141785 modeled seconds.
No hardware result is implied and the layout is not yet shipped.

`DBT_BATCH_RANGES` requires `DBT_BATCH_INVALIDATE`. It batches only explicit
range and whole-cache guest flushes; ordinary TLB replacements retain the
immediate incoming-link cleanup. The isolated Control Panel -> System ->
close Control Panel test samples ticks 4000000..7000000 and probes the Memory
tab periodically. Both final frames show the Memory page. Approximate
invalidate_block attribution falls from 1003751 to 38357 samples (30.632050
vs 1.170563 modeled seconds). This measures function time within a fixed
post-close interval, not exact input-to-paint latency. ASID/remap and interior
alias/cache-recycle tests pass. My Device reaches 591732 ticks / 18.058228 seconds.
Evidence: build-speed-profile-review/control-isolated-{baseline,batch}.


`DBT_HASH_ASID_TAG` uses 12-byte hash entries with an ASID at offset 8.
Both C and the assembly lookup stub validate the tag for user keys; kernel
keys remain global. Switching ASID retains the hash but still clears user
monomorphic ICs. Hash invalidation and bank reuse continue to detach entries
by destination. ASID/remap, interior/bank and TMU regressions pass. The new
ASID test verifies values and fewer than 16 C IC refills over 128 kernel-only
ASID detours. First directory result: 541500 ticks / 16.525269 modeled seconds
versus 592278 ticks / 18.074890 baseline. The combined cache/batch/tag test reaches 516930 ticks / 15.775452 seconds;
its low workspace limits the table to 21546 blocks (14 x 1539). Combined
ASID, interior-entry/bank-recycle and TMU regressions all pass.
The cache-only repeat matches exactly at 561702 ticks. Cache plus range batch
(without ASID tags) reaches 536040 ticks / 16.358643 modeled seconds.
User clarified that the fixed emulator test, not hardware stopwatch time,
is authoritative for the <= 5-second objective. That objective is unmet.

The 8-kernel/6-user bank partition regresses the combined candidate to
551874 ticks / 16.841858 seconds, so it remains disabled. The host sampler
now accepts HLE_DBT_CACHE_BASE and can read block tables in reclaimed low
RAM. Defaults retain the old layout; the benchmark records this setting.
A synthetic host test verifies low-RAM block ownership, runtime/code and
sleep classification without touching emulated state. Combined-load and
Control Panel profiles are pending.

### Follow-up measurements and gpSP comparison

The combined cache/batch/ASID-tag profile exactly repeats 516930 ticks:
238579 translated-code samples, 276430 runtime samples, 1922 sleep samples,
and zero unmapped guest owners. The same Control Panel close workload reaches
the System Memory page; invalidate_block costs approximately 1.075714 seconds
in the fixed post-close window. Enabling the existing guarded FAST_LDC_SR
path reduces My Device to 503826 ticks / 15.375549 seconds; combined
regressions are pending, so it is not yet a shipping setting.

DBT_SHORT_DEVICE_TEST is a new opt-in device-address classifier. It derives
whether the effective address's top byte is FF via (top+1)>>8, then negates
and adds one to retain the original BRAF selector. It removes two host
instructions without changing T, the branch layout, or recovery metadata.
Device accesses (including branch delay slots), TMU and ASID regressions pass.
Its My Device benchmark is pending.

Local gpsp-sh4-jit gint_platform.c uses two 8-KiB on-chip banks at E5007000
and E5017000, prepares RGB565 strips and issues asynchronous DMA0 transfers
to B4000000. Its recorded smaller-YRAM-strip experiment regressed hardware
performance when XRAM was reserved for fastmem. The CE loader currently uses
neither these banks nor ILRAM/SPU RAM: DSP spill registers are separate.
Current LCD hypercalls perform synchronous native CPU writes. A proposed DMA
experiment must preserve ClassPad DCS transactions, byte order, rotation,
channel ownership and suspend completion. The current emulator dma.c performs
transfers immediately; DMA timing must be audited before treating its speed
as evidence of meeting the five-second goal.

Current platform.bib and shared release ce.bib specify SHK for classpad_ddi,
kbdmouse and k.touch.dll (with a separate SH touch.dll). Thus kernel driver
placement already exists; it does not remove translation overhead.

DBT_LCD_DMA_STRIPS now implements an opt-in native presenter using two full
8-KiB X/Y banks. It converts little-endian guest RGB565 while the previous
bank is in flight, uses DMA0 auto-request / source increment / fixed LCD /
32-byte blocks without interrupts, and waits before returning to CE. It
preserves channel/operation/module-stop state, declines a busy or error-state
channel, and halts on a bounded wait failure rather than silently losing
pixels. The ClassPad DCS transaction remains unchanged. Initial dcs guest
CPU/DMA final frames are byte-identical; an additional lcd_dma guest tests
odd pixel tails, short CPU fallback and the actual DMA launch count. Full CE
rotation and timing validation remain pending; this path is not shipped.
HC_GETSTAT 400 reports capability and 401 reports launched strip count.
These IDs avoid the existing 32..58 profile buckets and 128..383 histogram.
The combined FAST_LDC_SR TMU, ASID and interior/bank regressions all passed.

The lcd_dma guest passes with exactly 36 DMA launches. Its full frame,
51-pixel region (48 DMA + 3 CPU tail) and seven-pixel CPU fallback produce
byte-identical CPU/DMA frames. Full CE functional run is pending. Separately,
SHORT_DEVICE_TEST alone reaches 507648 ticks / 15.492188 seconds on the
combined cache/batch/tag candidate; FAST_LDC_SR plus this classifier is pending.

DBT_META_ALIGN4 experimentally rounds runtime metadata allocations to four
bytes rather than eight. Compile-time assertions require imeta, spills and
links to need at most four-byte alignment; packed auxiliary targets are
already four-byte aligned. Bank boundaries remain eight-byte aligned. The
purpose is to reduce per-block padding and avoid premature metadata eviction;
ASID/interior/TMU and full load measurements are pending.

The combined FAST_LDC_SR + SHORT_DEVICE_TEST candidate reaches 495090 ticks /
15.108948 modeled seconds with exact directory-frame matching. Initial DMA
full CE also matches the directory frame, but is slower at 506556 ticks /
15.458862 seconds even with instantaneous DMA emulation; no speedup is claimed.
Inspection found byte-at-a-time buffer conversion. The DMA path now prepares
aligned pairs with four native swap instructions and one 32-bit store; its
color/edge regression is pending. META_ALIGN4's ASID, interior/bank-recycling
and TMU regressions all pass; its directory benchmark is running.

Paired DMA conversion passes the exact CPU reference frame and the 36-strip
edge test. Full CE four-orientation display/touch testing is running.

The four-byte metadata layout reaches 493452 ticks / 15.058960 modeled
seconds, only about 0.05 s below the eight-byte layout. The new opt-in
HLE_DBT_SAMPLE_OPS host sampler records generated-code opcode frequencies
without changing emulated state. tools/test_dbt_sampler.py verifies low-RAM
block ownership, code/runtime/sleep classification and both halfword positions
in a backing-memory word. The current fastest binary is being sampled again
to distinguish emitted instruction costs; no new speed claim follows from
the diagnostic alone.

The paired-DMA CE rotation harness passes all four display geometries and
physical touch positions, returning to portrait with zero failures. The
24-instruction candidate reaches 498912 ticks and regresses versus the
32-instruction candidate, so both shorter caps remain disabled.

The FAST_LDC_SR/SHORT_DEVICE_TEST profile exactly repeats 495090 ticks.
It records 12024 synthetic instruction-address faults during the directory
load. DBT_EARLY_FETCH_FAULT checks unexecutable targets immediately after
ASID synchronization in lookup_or_translate, before hash/interior/block
searches. It uses the existing synthetic exception and saved context, with
the same EXPEVT 0x0E0; the original translate_at guard remains as a backstop.
API-call, register-bank, MMU and TMU regressions and a load benchmark are
running. No speedup claim is made before those results.

Opcode sampling exactly repeats 495090 ticks, with 227653 generated-code
samples. swap.b contributes 26578 and swap.w 12839 samples (~1.20 modeled
seconds together); shlr8/shlr16/shll2/neg/braf also remain prominent. These
are residency samples, not execution counts. Unrecognized DSP words stay
explicit in tools/analyze_dbt_opcodes.py rather than receiving guessed names.
EARLY_FETCH_FAULT passes apicall, bank, MMU and TMU regressions.

DBT_CONST_DEVICE_ADDRESS records values emitted by PC-relative constants
within a block, eliding the device classifier only for a provably non-FF
effective address. Memory definitions discard overwritten values; other
operations and branches conservatively discard all values. Specialized blocks
are excluded from both arbitrary interior-entry and IRQ-entry publication.
The marker fits existing header padding. Host tests pass 185 checks (175
without the option), including overwrite/branch invalidation and interior
rejection. Target regressions and full CE timing are pending.

EARLY_FETCH_FAULT reaches 496182 ticks / 15.142273 modeled seconds, slower
than its 493452-tick baseline. It remains disabled despite passing regressions.

CONST_DEVICE_ADDRESS target devmem, TMU, interior and ASID regressions pass.
A new TMU loop materializes a scratch pointer inside each iteration, then
compares 60000 read/modify/write iterations with interrupts off and on. It
requires both actual interrupts and a rising specialized-block counter to
avoid a vacuous pass. HC_GETSTAT 402/403 expose this opt-in capability/count.
DMA diagnostics moved to 400/401 after finding that 57/58 overlap existing
lookup profile fields; neither experimental ABI had been shipped.

The explicit constant-address IRQ proof passes (baseline checksum, identical
interrupted result, actual ticks and nonzero specialization count). Initial
whole-block entry rejection reaches 500550 ticks / 15.275574 s, regressing
the 493452-tick baseline. The follow-up marker now records the halfword offset
after the last dependent access: both normal and IRQ interior caches may reuse
entries at or beyond that boundary. Earlier entries remain rejected. Host
tests pass 188 checks, including the safe suffix and unsafe access itself;
target TMU/interior and timing tests are pending.

META_ALIGN4 repeats exactly at 493452 ticks / 15.058960 seconds. The refined
constant-address suffix-entry rule passes both the explicit TMU specialized
loop and interior/cache-recycling regressions. DBT_ASM_FETCH_FAULT separately
checks unexecutable targets only on an assembly lookup miss, after full guest
context capture and interrupt masking, then uses HC_SYNTH with an internal
flag to count the fault and synchronize ASID. Guest delivery still uses the
existing EXPEVT/TEA/SPC path. Its API/bank/MMU/TMU and full CE tests are running.

The constant-address safe-suffix variant reaches 496728 ticks (15.158936 s),
still slower than the baseline, so constant specialization remains disabled.
The assembly fetch-fault path passes API-call, bank, MMU and TMU regressions;
its directory timing is pending. DBT_CODE_ALIGN4 now separately tests four-byte
code-block padding. Literal pools require four-byte alignment; cache-bank
boundaries and synchronization remain unchanged. The 32-byte dead bitmap is
conservative when neighbours share a bit: it may detach additional links but
must never miss a dead destination. ASID/interior/TMU and timing are pending.

The assembly early synthetic-fetch-fault experiment passes apicall, bank,
MMU and TMU regression guests, but the frozen My Device benchmark regresses
to 501096 ticks (15.292236 seconds). It remains disabled. Code-alignment-4
passes ASID isolation, interior reuse/bank recycling and TMU interrupt tests;
its loading benchmark is still running. A current-layout 64-instruction
block experiment is also running; earlier block-cap comparisons used a
different configuration and are not substituted for this measurement.

Code-alignment-4 also finishes at 501096 ticks (15.292236 seconds),
matching the full directory frame. It is rejected against the 493452-tick
metadata-alignment-4 baseline; reducing padding alone is not a gain.

DBT_DECODE_TEMPLATE is an optional fixed-size decoder initialization
experiment. All 21 decoded fields match the baseline for all 65536 opcodes
(tools/test_decode_template.py); 175 translator checks pass. Target assembly
uses direct halfword/byte stores instead of the freestanding memset call.
This is not a speed claim: the frozen-image benchmark is running.

Measurement audit: the 493452-tick baseline binary predates recent optional
experiments. A current-source control with all new experiments disabled is
now running. The two 501096-tick runs are slower than the historical best,
but attributing the entire difference to their individual flags requires
this additional control; neither is adopted meanwhile.

The decoder-template first run matches at 489630 ticks (14.942322 seconds).
A repeat is running, as is the current-source baseline. The 64-instruction
run matches at 501096 ticks (15.292236 seconds). Local-alias removal with
range-only batching passes current ASID/interior recycling regressions; its
full CE timing is pending. The existing fast-short guest samples were
resolved against timestamp-matching CE MAPs: kernel.dll accounts for 4.73938
modeled seconds, led by scheduling, calls, locks and address-space handling;
classpad_ddi contributes 0.781525 seconds but its MAP mismatch prevents
function-level claims. Evidence: fast-short-profile/ce-functions.json.

Current-source control completes at 497820 ticks (15.192261 seconds).
The decoder-template first run saves 8190 ticks (0.249939 seconds) against
that control. The assembly-fault, code-alignment and cap64 experiments each
regress by 3276 ticks, so none is adopted. Optional constant-address header
state and DMA/constant-address diagnostic switch cases are now compiled only
when their experiments are enabled; disabled IDs retain the existing zero
fallback. Default/template/constant-address host suites pass 175/175/188
checks. A clean-template timing run is pending; no cleanup speedup claimed.

Local-alias removal with range-only batching completes at 499458 ticks
(15.242249 seconds), versus current-source control 497820. Even isolated
from full TLB batching it does not improve this workload; remains disabled.

Decoder-template repeat matches exactly at 489630 ticks (14.942322 s).
DBT_DECODE_ALIGN4 optionally strengthens sh_insn alignment from 2 to 4
without changing its 28-byte size. Exhaustive opcode comparison and 175
translator checks pass; target disassembly shows seven word initialization
stores instead of fourteen halfword stores. Its full CE benchmark is
running against the clean-template build; no measured speedup claimed yet.

The clean-template run completes at 485808 ticks (14.825684 seconds),
matching the directory reference. A repeat is running. This removes only
disabled experimental header/diagnostic code; the CE image and emulator
clock settings remain unchanged. Display review confirms coalesced event
notification already exists; the driver still bounds native pushes to four
rows to allow pending guest interrupts between strips. No unmeasured driver
change is included in this translator timing.

Decoder alignment4 first run matches at 459600 ticks (14.025879 seconds), saving 26208 ticks against clean-template.
The TMU regression passes; ASID/interior and timing repeat remain pending.

Decoder alignment4 completes all target regressions: TMU interrupts, ASID
isolation/remapping and interior entry/bank recycling, all zero guest exit
codes. Host exhaustive decoding and 175 translator checks also pass.

Clean-template repeat confirms 485808 ticks. The aligned-decoder candidate
now also has an isolated DBT_HASH_FOLD14 comparison running: the normal
8192-slot hash ignores guest address bits 14/15, while fold14 includes them
at the cost of two additional rotates in the assembly lookup. ASID/interior
regressions are running; no conflict-rate or speed improvement assumed.

Aligned-decoder repeat confirms exactly 459600 ticks (14.025879 seconds).
Fold14 ASID/interior regressions pass; timing pending. DBT_BODY_BUDGET=680
experiments with longer bodies inside the unchanged 1-KiB output buffer.
Overflow attempts remain unpublished and retry at half the instruction cap
using a private environment. Early overflow guards prevent reading rejected
literal/jump slots. Capacity tests pass 6420 attempts / 4372 bounded retries
under ASan/UBSan; 175 translator checks pass. Default 560-byte budget remains.

Fold14 completes with exact directory matching at 471612 ticks
(14.392456 seconds), slower than the 459600-tick
aligned-decoder baseline. Its ASID/interior regressions pass, but it is not
adopted. Body-budget680 timing and target regressions are running.

Body-budget680 passes TMU, ASID/remapping and interior/bank-recycling target
regressions. DBT_PRUNE_LINK_BANKS is a separate experiment: after all incoming
direct links to a fully recycled bank are detached under IRQ masking, clear
that destination bit in each surviving owner. Future link_direct calls set
it again. Partial-block invalidation retains its conservative mask. Current
timing and ASID/interior target tests are pending.

Body-budget680 matches the directory at 485262 ticks
(14.809021 seconds), slower than the verified 459600-tick
candidate. It remains disabled. Early overflow guards are retained as a
correctness fix; this run does not isolate their individual timing effect.

Pruning recycled destination-bank bits passes interior/recycling and ASID
regressions; timing pending. DBT_DECODE_FPU_GATE rejects non-0xxx/4xxx/Fxxx
opcode groups before the FPU subdecoder. Exhaustive 65536-opcode comparison
across all 21 fields and 175 translator checks pass. Target build succeeds;
timing pending. A new host-only profile of the unchanged 459600-tick binary
is also running (decode-align4-profile).

DBT_CACHE_TICK_LFU experiments with cache replacement based only on actual
TMU0 samples landing in generated instruction/exit ranges. It reuses the
existing bank-touch array, saturates counts, decays all counts at each bank
selection, prefers empty banks and breaks ties in FIFO order. C lookup
frequency does not contribute. The current bank remains excluded. It is
mutually exclusive with C-side LRU and split kernel/user banks. Target build
succeeds; timing and TMU/ASID/interior regressions are running.

Host-only aligned-decoder profiling repeats exactly 459600 ticks with
zero unmapped guest samples. Of 459601 samples, 208233 are generated code,
249559 runtime and 1809 sleep. Approximate decoder time is now 0.331421 s;
exception C remains 1.137054 s, translation 1.028473 s and invalidation
0.720032 s. Prune-links finishes at 472158 ticks (14.409119 s), so is not
adopted. A current-source control isolates the newer overflow guards.

DBT_CONST_P4_STORE reuses constant tracking but disables the earlier normal-
RAM specialization. Only literal-proven P4 stores other than PTEH/MMUCR
execute directly, unswapped. Entry restrictions retain the address proof;
unknown addresses and address-space hooks keep the helper. Capability 404
identifies this variant; 402 remains reserved for normal-address proof.
192 host checks pass, including PTEH/MMUCR exclusion and predecrement/disp
effective addresses. Added devmem C-bypass and TMU interrupted-loop proof
tests; target regressions and frozen-image timing are running.

Overflow-guard current-source control completes at 485808 ticks
(14.825684 seconds), versus historical aligned-decoder 459600. Accordingly
prune-links (472158) and FPU gate (478710) improve this current-source
control, although neither beats the historical best. Body680 is 485262.
Tick-LFU passes all target regressions but regresses to 514746 ticks
(15.708801 seconds); it is not adopted. Attribution is updated rather than
assigning all timing changes to each flag independently.

P4-store target regressions pass devmem/TMU/ASID/interior, including
explicit proof of specialization, unswapped store values, no C helper for
the literal-store function and interrupts during the specialized loop.
The overflow guard is now inside emit_flush_lits, protecting all callers;
translation checks once after flushing, before reading jump records. No
more instructions are emitted after that check, so the final duplicate
check was removed. ASan/UBSan passes 6118 normal-budget and 6420 extended-
budget capacity attempts; 175 translator checks pass. Timing is pending.

P4-store first timing matches at 483078 ticks
(14.742371 seconds). This improves the 485808-tick
overflow control by 2730 ticks, but does not beat the historical best.


Guard cleanup completes at 484716 ticks / 14.792358 modeled seconds, exact
reference frame. This does not beat the historical 459600-tick build.

DBT_DYNAMIC_P4_STORE experiments with runtime discrimination of P4 stores:
PTEH/MMUCR retain C hooks; other FFxxxxxx stores execute unswapped native
instructions. The classifier saves guest T in a tracked DSP spill and closes
all scratch ranges before the access. This avoids relying on a preceding
literal/address proof. BODY_BUDGET=560 enables bounded retries for the larger
emission without changing the normal block budget. Capability 405 distinguishes
this loader. Tests verify actual helper bypass, T=0/1 preservation, normal RAM
byte order, protected-register hooks, delay slots and predecrement, plus stores
interrupted by TMU. Host recovery checks: 182 PASS; sanitizer capacity checks:
6416 attempts / 4368 retries PASS. Target devmem/tmu pass; ASID/interior and
same-image directory timing are in progress. Feature remains opt-in.

Dynamic P4 target regressions now all pass: devmem, TMU, ASID switching and
interior-entry/bank reuse. The native-store metadata check additionally verifies
that no scratch/T recovery remains pending at the actual store. Default opcode
sequence tests must be run without SHORT_DEVICE_TEST, because their expected
classifier bytes intentionally describe the original classifier; target tests
use the complete candidate flags including SHORT_DEVICE_TEST.

Dynamic P4 full CE completes with exact reference frame at 484170 ticks /
14.77569580078125 modeled seconds. Same-source guard-clean control is 484716
/ 14.7923583984375: just 546 ticks (0.0166626 s) saved, far below the gap to
5 seconds and slower than the historical 459600-tick best. Do not enable this
feature in the delivered loader on this evidence. The extra generated path
has not produced a useful end-to-end improvement despite verified helper bypass.


DBT_BATCH_HASH_REMOVE defers whole-table alias removal inside an existing
range-invalidation batch. The dead-host bitmap already identifies every dead
entry; finish_invalidations walks the hash table once before guest execution
can resume. Non-batched TLB replacement retains immediate hash removal, as do
owners with no interior aliases. The bitmap's conservative 32-byte granularity
can evict a live neighbour's hash entry but cannot retain a dead destination.
Full cache reset clears pending work; bank reuse already clears dead-host bits.
Capability 406 and scan counter 407 allow tests to prove the deferred path ran.
Tests pass for interior entry, ASID changes and TMU. A new 16-owner/32-suffix
case asserts exactly one cleanup scan, removal of all aliases, and execution
of new instructions after modification; subsequent full-bank recycling also
passes. Same-image My Device benchmark is in progress; feature remains opt-in.

Batch hash removal reaches the exact reference frame at 486354 ticks /
14.842346191 modeled seconds. Against guard-clean's 484716 ticks,
this is 1638 ticks (0.049987793 seconds) slower. The explicit multi-owner test
proves the scan reduction works, but it does not improve this end-to-end load;
keep disabled. Remaining incoming-link invalidation still visits all blocks,
including those with no IC or relevant destination bank. A transposed source
bitmap is a candidate for reducing that scan without skipping live links; it
has not been implemented or measured yet.


DBT_INCOMING_BITMAP adds conservative source sets for each destination bank
and for blocks containing an IC. Immediate and batched invalidation iterate
only their union instead of visiting every block header. Source bits clear on
header reuse, IC ownership is published with the block, and direct-link bits
are set while link patching is masked. Dead owners may retain bits until reuse;
these are safe false positives. A full cache reset clears all sets. Bank-recycle
scanning itself retains the original implementation for this experiment.
It adds 15 * 674 * 4 = 40440 bytes at 21546 blocks / 14 banks. Image end is
8cc514c0, low BSS ends at 8c0ff740; linker still reserves the existing stack,
and CE retains its 8 MiB window. Capability 408 and counter 409 prove execution.
Target interior/aspace/TMU regressions pass. Audited interior/aspace builds
with DBT_VERIFY_INCOMING_BITMAP compare every live IC and relevant direct-link
bank against the bitmap before cleanup; both pass, including bank recycling.
Same-image My Device timing is running. Feature remains opt-in.

Incoming-bitmap CE measurement matches the reference frame at 510924
ticks / 15.592163086 modeled seconds, slower than guard-clean
by 0.799804688 seconds. Keep disabled despite passing
correctness and complete-source audits. Reducing the candidate set did not
outweigh the bitmap maintenance/iteration and resulting execution changes in
this workload; no speedup is claimed. The historical best remains 459600 ticks.


DBT_UNIFORM_STORE tracks endian-invariant register values proven by immediate
0/-1, a copied proven register, or a byte-uniform PC-relative constant. Plain
16/32-bit stores can emit the original instruction without swap/scratch spill.
Overwrites clear the proof; branches/traps/helpers discard it. Device-aware
stores retain all dispatch/hooks, and predecrement with data==base is excluded
because decrement changes the value. Entry-dependent blocks use the existing
const_specialized floor for both regular interior reuse and IRQ entry caching.
The block-start path executes its proof-producing instructions normally.
196 host checks pass, covering overwritten registers, loads, branches, device
accesses, predecrement and suffix entry. Target interior/aspace/TMU pass. New
explicit tests prove specialization executes, arbitrary middle entry with a
nonuniform value retains correct little-endian bytes, and repeated zero/-1
stores survive timer interrupts. Capability 410/counter 411 expose the feature.
CE timing is in progress. Image end 8cc472a0, low BSS 8c0ff740; CE memory unchanged.


DBT_UNIFORM_ENTRY_BOUNDS is a separate experiment preserving safe prefix
entries. It tracks the original definition of each uniform register (copies
inherit their source's definition) and allows entry at/before the earliest
required definition, or after the last dependent store. Entries in between
remain rejected by both normal interior lookup and IRQ entry caching. It is
incompatible with CONST_DEVICE_ADDRESS because that feature does not record
origins. 203 host checks pass, including entry at the definition, rejection at
a copy/store and reuse of the safe suffix. Target interior passes; TMU is still
running. Separate CE binary built, not yet benchmarked.

Uniform-store floor-only experiment reaches the exact reference frame at
490722 ticks / 14.975646973 modeled seconds,
0.183288574 slower than guard-clean. Keep disabled.
The separate prefix-bounds version also passes explicit interior-entry and
interrupted-store target tests. Its CE measurement is now running as
uniform-bounds (session 11408); no speedup is claimed before it completes.

Uniform prefix bounds completes with exact reference frame at 487446
ticks / 14.875671387 seconds. It improves on the floor-only
experiment but remains 0.083312988 seconds slower than
guard-clean, so neither uniform-store variant is enabled.

DBT_CORE_BANKS changes the existing split allocator's hot-stream classifier
from all kernel-mode PCs to just the identified kernel.dll device-aware range
(non-user PC, same alias folding as device accesses). Four of fourteen banks
are reserved for this core; GWES, display/filesystem DLLs and user code share
the other ten. No guest memory, clocks or benchmark input changes. Capability
412 and counters 413/414/415 report core translations, other translations and
core-bank evictions. Interior/aspace/TMU regressions pass. Explicit tests show
both streams used, cold pressure recycling the other banks without evicting
the core, and core code surviving timer interrupts. CE timing is running as
core-banks/session 70566. Feature remains opt-in.

Core-only 4/10-bank split reaches the exact reference frame at 503280
ticks / 15.358886719 modeled seconds, slower than guard-clean
by 0.566528320 seconds. Keep disabled. Protecting a
core does not automatically improve the whole workload; other translations
lose capacity and bank switching adds work. These are hypotheses for the
regression, not separately measured causal contributions.

Rechecking historical aligned-best host opcode samples: of 208233 generated
code samples, 36518 are swap.b/swap.w, 9914 NOP, and the hottest individual
opcodes include DSP spill/restore (418a:10894, 018a:10461) and the device
classifier (4119:9901, 7101:9604, 611b:4850, 4108:4840, 4129:4831).
This points to per-access P4 classification as another generated-code cost,
not only byte swapping. A potential T-preserving shorter classifier can use
XOR #1 when R0 is available as the saved scratch; this is not implemented yet.


DBT_R0_DEVICE_TEST chooses saved R0 for the short P4 classifier when the
instruction and delay-slot restrictions leave it available. XOR #1 replaces
NEG/ADD #1; shifts and XOR preserve guest T. The dispatch layout and restore
in BRAF's delay slot are unchanged. Host sequence tests now cover original,
short and R0 classifier layouts explicitly (180 checks pass). Target devmem,
interior, ASID and TMU pass; added tests preserve live R0, an R0 indirect-call
target, and R0/T during interrupted RAM/P4 accesses. Capability is 416.

DBT_R0_DEVICE_WIDE permits R0 operands too: the original R0 is restored before
the actual memory instruction. It still excludes R0-indexed address formation
and delay-slot protected registers. 182 host checks and explicit R0 base/data
RAM/P4 tests pass. Sanitizer capacity tests: 6088 attempts / 4040 retries pass.
The wide TMU test uses R0 as the address throughout the interrupted loop.
Capability 417 selects that test. Separate CE binary built; first candidate
benchmark r0-device/session 96691 is running. Both features remain opt-in.

R0 classifier first full CE run matches the exact reference frame at
473250 ticks / 14.442443848 modeled seconds. This
saves 0.349914551 seconds over guard-clean's 484716
ticks, but remains slower than the historical aligned-best 459600 ticks.
Repeat not yet performed; do not claim a new best or a delivered build.
Wide R0 operand tests and explicit RAM/P4 interrupted loops pass; the TMU
binary was built after the test switched to the R0-address loop. Wide
interior/aspace tests and separate CE timing are now running.

Wide R0's additional interior/aspace regressions also pass (session 25048).
The exact ELF's profiling layout is blocks=8c042150, counts=8cc0dc5c,
stat=8cc45e44, 1539 headers per bank and 32 stats words. A profile repeat can
use these with the existing host-only opcode sampler without rebuilding the
translator. Historical generated-code samples include 1850 stack-postincrement
loads and 1937 stack-predecrement stores. Grouping adjacent stack operations
may amortize address classification, but must guard region-boundary crossings,
preserve partial-progress exception recovery, and reject unsafe middle entry.
No grouped-stack implementation or speed claim exists yet.

Wide R0 completes with the exact frame at 480348 ticks /
14.659057617 modeled seconds. This is slower than narrow
R0's 473250 ticks, despite broader instruction-count savings. Keep the wide
variant disabled. A host-only profile repeat of the exact narrow binary is
now running as r0-device-profile, with identical block/count/stat addresses
to the wide ELF. Additional MMU tests exercise R0 postincrement loads and
predecrement stores that incur a data TLB miss, checking retry PC, address
adjustment and stored/loaded value; these are running as build-r0-wide-mmu.

Wide-R0 MMU regression passes, including the new data-fault checks: exactly
one miss, correct retry SPC, R0 adjusted exactly once, and correct value.
Narrow-R0 profile repeat remains live in session 44843.

Narrow R0 profile repeat exactly reproduces 473250 ticks, with zero unmapped
owners. Samples: generated 219539, runtime 251898, sleep 1814. Counter deltas:
18123 translations, 11 cache generations, 21366 ASID changes, 40231 C IC misses,
34244 hash-table evictions. The leading approximate runtime costs remain
exception handling 1.1295 s, translation 1.0226 s, invalidation 0.6942 s and
IC stub 0.5191 s. Both generated execution and runtime work remain material.

DBT_META_WORD_INIT initializes each 16-byte recovery record with four full
word writes rather than memset plus field overwrites. Compile-time size,
offset and alignment assertions protect the layout; explicit host endian
packing is validated on the little-endian host and big-endian target. Uses
a may_alias word type under GCC/Clang. Record format and recovery rules do not
change. 180 host checks and devmem/interior/aspace/TMU pass. The combined
narrow-R0 + metadata initializer CE benchmark is running as meta-word,
session 29298. Feature remains opt-in pending actual end-to-end results.

Metadata word initialization matches the exact directory frame at
472704 ticks / 14.425781250 seconds, saving only
0.016662598 seconds against the repeated narrow-R0
control. This is not a material reduction toward five seconds and not a new
historical best; keep opt-in. No new loader has been delivered.

Re-evaluating the existing 512-entry DECODE_CACHE with aligned 28-byte decoder
records and narrow-R0 classifier gives an exact frame at 477618 ticks
/ 14.575744629 seconds, still slower than the 473250-tick
control. Exhaustive decoder testing now includes repeated hits, forced hash
collisions (opcode xor 0x204) and high-bit normalization for all 65536 opcodes
and all 21 decoded fields. Target devmem/interior/aspace/TMU pass.

DBT_DECODE_REF lets the main translation loop consume a cached record directly
rather than copying it on every hit. The pointer is valid only until the next
decode; a delayed branch is copied before decoding its potentially colliding
slot. The public copy-out decoder remains available for other callers and the
old cache implementation is unchanged when REF is off. Two explicit branch/
legal-MAC-slot hash-collision tests pass. 184 host checks, exhaustive decoder
checks, 6088 sanitizer capacity attempts/4040 retries, and target devmem/
interior/aspace/TMU all pass. Turning the experiment off produces a byte-for-byte
identical translator .bin to the narrow-R0 control (SHA256 38528dbbd52f2e63d4f90692f5fe7843fed4eccd8b08fd2c32ed0b6b94ee9587).
Full CE decode-ref timing is running; no speedup claimed yet.


DBT_DECODE_INTERLEAVED combines the opcode tag and 28-byte decoded record in
one 32-byte entry, with the array aligned to 32 bytes. It requires DECODE_REF;
the total 512-entry capacity remains 16 KiB. This replaces separate tag and
record indexing with one power-of-two stride and places each pair in one
32-byte cache line. Miss publication retains the private temporary and the
zero-tag/barrier protocol. Exhaustive decoding, repeated/colliding hits, high
bit normalization, and 184 host recovery tests pass. Target devmem/interior
pass, aspace/TMU are running. Array address 8cc46e80, image end 8cc4ae80 and
low BSS 8c0ff740 fit the existing layout without reducing CE RAM. Full binary
built, waiting for the preceding decode-ref timing to finish before benchmarking.

Decode-reference CE timing matches the exact directory frame at 476526 ticks /
14.54241943359375 modeled seconds, versus 473250 / 14.44244384765625 for
the narrow-R0 control. Removing the cache-record copy has not produced a net
end-to-end gain; this option remains disabled. The interleaved 32-byte cache
entry variant passes exhaustive decoding with forced collisions and all four
target regressions (devmem, interior, aspace, TMU); its fixed-image CE timing
is now running.

DBT_FAST_LDC_SR_MD is a new disabled-by-default experiment: after the existing
RB and ASID guards pass, update dbt_shadow_md directly while interrupts are
masked instead of falling back solely because guest MD changes. Physical SR
still forces MD and DSP; the existing fast-return recovery labels are intact.
New TMU coverage alternates virtual MD, checks SR readback, and restores SR
through the memory operand with its load tail masked. Validation is pending.
An initial unmasked ldc.l variant halts with "exception after a committed
privileged load" in both the candidate and unmodified fast-SR control; this is
an existing recovery limitation, not fixed or covered by masking the new test.

Interleaved decode-cache timing: 474342 ticks / 14.47576904296875 modeled seconds, exact directory frame. This remains slower than the narrow-R0 control (473250 ticks), so the option stays disabled. The SR-MD candidate passes the expanded TMU test; remaining checks are in progress.

SR-MD expanded TMU passes in both candidate and default control; candidate aspace and interior regressions also pass. The hello image is built and the frozen-image end-to-end benchmark has started; no performance gain is yet established.

A new 16-instruction-cap control uses the current aligned decoder, metadata
layout and narrow R0 classifier, with SR-MD disabled. Earlier shorter-cap
results were negative on older combinations; this measures the interaction
with the current emitted code, without changing CE memory or timer settings.
Expanded TMU, aspace and interior target regressions pass; the CE hello image
is built, awaiting serial timing after the SR-MD run.

SR-MD end-to-end result: 476526 ticks / 14.54241943359375 modeled seconds,
exact directory match. This is slower than narrow-R0 (473250 ticks), despite
the functional tests passing, so the option remains disabled. No performance
gain is inferred from bypassing a C helper alone.

DBT_REUSE_RESUME_META optionally computes the noncommitted resume PC from
the already unpacked metadata used for the commit test. It still evaluates
register/PR/T-dependent targets against the current exception context. The
committed path and spill restoration are unchanged. Target regression and
disabled-feature binary comparison are in progress; no speed gain claimed.

Resume-metadata reuse passes TMU, ASID and interior-entry target regressions.
With the option off, the original function body is preserved under the
preprocessor and the hello binary is byte-for-byte identical to narrow-R0
(SHA256 38528dbbd52f2e63d4f90692f5fe7843fed4eccd8b08fd2c32ed0b6b94ee9587).
The candidate hello is built; end-to-end timing is still pending.

Current narrow-R0 16-instruction cap matches the exact frame at 505464 ticks / 15.425537109375 modeled seconds, slower than the 32-instruction control (473250 ticks). Keep the 32-instruction cap.

DBT_HASH_ENTRIES parameterizes the C and assembly lookup masks together,
default 8192, power-of-two checked with a 65536 upper limit for 16-bit slot
tracking. The 16384-entry experiment uses 18816 block headers (1344 per bank)
to fit the existing low workspace, without borrowing CE RAM or reducing the
3.5 MiB translation cache / 2880 KiB metadata arena. An 8192-entry control
uses the same 18816 headers, isolating header pressure from hash capacity.
The initial target image ends at 8cc491e0, low workspace at 8c0fa200; linker
assertions retain the 128 KiB stack reserve. TMU passes, other target tests
and full CE timing are pending. No speedup is assumed from collision counts.

Resume-metadata reuse matches the exact frame at 476526 ticks / 14.54241943359375 seconds. It does not beat the 473250-tick control and remains disabled. Hash16k passes all target regressions; default hash parameters preserve the baseline binary exactly. Paired CE timing starts with 8192 entries / 18816 headers, followed by 16384 entries / 18816 headers.

DBT_PRUNE_INDEX_REUSE experiments with splicing dead headers out of the
existing guest-PC/page chains during bank recycling, instead of recomputing
indices for every survivor. It preserves live-node order, removes dead nodes
before their table slots are reused, and retains conservative user-page bits
(false positives only). IC/direct-link invalidation is unchanged. The option
is disabled by default and preserves the baseline binary when disabled.
DBT_VERIFY_PRUNED_INDEX separately checks no dead nodes, duplicates/cycles or
missing live headers, including second-page nodes. It is test-only. TMU
passes; ASID/interior tests with and without the audit are running.

Pruned-index TMU/aspace/interior regressions pass. The independent node
audit also passes ASID/remap and repeated bank recycling in the interior
test. CE binary is built; end-to-end timing remains pending.

The 8192-entry / 18816-header control matches the exact directory frame at 479802 ticks / 14.64239501953125 seconds. The paired 16384-entry run has started; no hash-capacity conclusion yet.

Coverage audit: the original aspace test had zero bank evictions; only the
interior test exercised the structural verifier (14 evictions). The aspace
test now keeps a user entry active throughout two table capacities of P1
code pressure, checks its return value periodically, then replaces its
physical mapping. Both the audited pruned-index build and default control
are running this combined regression.

The expanded user-mapping/recycling test passes in both default control
and audited pruned-index candidate, including remapping after pressure.
No stale/cyclic/missing-index audit failure occurred.

The 16384-entry / 18816-header configuration matches at 482532 ticks / 14.7257080078125 seconds, slower than its 8192-entry same-header control (479802 ticks). Both are slower than the 21546-header baseline (473250 ticks). Keep default hash capacity; start the independent pruned-index benchmark.

DBT_NO_TICK_SAMPLING measures the overhead of the always-on native tick
classifier and exception-vector counters. It does not alter timer programming,
IRQ delivery, IRQ-entry recovery/cache, or scheduler behavior. ABI capability
bit 0 at HC_GETSTAT 62 is cleared; ABI signature/version and optional TMU1
C-time profiling capability remain accurate. Default behavior remains sampled.
Target tests and timing are pending; no performance gain is assumed.

Pruned-index first timing matches at 470520 ticks / 14.359130859375 seconds,
2730 ticks (0.08331298828125 s) below narrow-R0. A host-only sampled repeat
will check reproducibility and runtime costs; this is not yet a selected
shipping change. No-tick-sampling passes all target regressions and awaits
a separate timing run with the ordinary emulator.

Pruned-index host-only sampled repeat exactly matches 470520 ticks with the
same binary. Runtime samples fall from 251898 to 249058, generated-code
samples change 219539 to 219655, and both reports have zero unmapped owners.
Translations increase 18123 to 18244 while cache-generation changes remain
11; the measured 0.577% improvement is not a reduction in retranslations.
Evidence: prune-index-profile/comparison.json. No-tick-sampling timing is
now running independently.

No-tick-sampling matches at 473796 ticks / 14.4591064453125 seconds, versus
473250 for narrow-R0: no end-to-end gain, keep sampling enabled.
DBT_SAME_ASID_PTEH_STORE extends the dynamic P4 store experiment: when the
actual stored value's low byte equals the runtime ASID, PTEH can be written
natively without C bookkeeping. Changed ASIDs and MMUCR retain C handling.
The T bit and scratch GPRs use existing recovery spills, with a fresh native
store commit range. Predecrement of a self-storing base adjusts the compared
value. The optional native ASID pointer is absent from default dbt_env.
Explicit device tests verify native bypass, T=0/1, predecrement, and changed
ASID C bookkeeping. 187 host checks and 6310 ASan/UBSan capacity attempts
(4262 bounded retries) pass. Target devmem/TMU/aspace pass; interior pending.
A same-flags dynamic-store control without the PTEH extension is being built.

Same-ASID PTEH interior regression also passes. A dedicated TMU loop now
performs 20000 native same-ASID stores with T=1, requires actual IRQs, checks
the final PTEH value and checksum, and verifies zero device-C-helper calls.
It is running; the independent dynamic-store control CE timing has started.

Dedicated same-ASID PTEH IRQ loop passes: checksum/T, final register value,
actual interrupts and zero helper calls all verified. Default narrow-R0
hello binary remains byte-identical with the PTEH option disabled.

Added and passed a self-predecrement PTEH proof: mov.l r4,@-r4 with
r4=PTEH+4 must compare/store ASID zero from the decremented address, preserve
the decremented register, and avoid C. This guards against comparing the
original low byte (4), which would needlessly select the slow path.

The same-flags dynamic-P4-store control (without same-ASID PTEH bypass)
matches at 479256 ticks / 14.625732421875 seconds. It is slower than the 473250-tick narrow-R0 baseline; the PTEH candidate must beat both comparisons to be a speed candidate.

Same-ASID PTEH bypass matches at 471066 ticks / 14.37579345703125 seconds, versus 479256 for its dynamic-store control and 473250 for narrow-R0. Initial gain is not yet repeated; it does not beat pruned-index (470520) or the historical 459600-tick binary.

Host-only device-store residency sampling now reads saved r[0..15] and X0
from exact dbt_ctx_rt symbols while PC lies within the exact fallback symbol
range. It supports plain/disp/R0-indexed stores; predecrement is deliberately
excluded because the saved base may already have changed. Synthetic tests
verify PTEH/MMUCR/TEA classification and no RAM writes. Timing JSON retains
all three diagnostic addresses. A frozen-baseline validation run starts next.

The pruned-index + same-ASID PTEH combination passes devmem, TMU, aspace
and interior tests, including the native PTEH IRQ loop and user mapping
across repeated bank recycling. Its CE binary is built; timing pending.

First device-store sampler repeats baseline timing and aggregate samples
exactly (473250 ticks), but every address sample is unknown. Inspection
shows SAVE_CTX_RT does not populate the saved DSP slots. The address
histogram is INVALID; it must not support any MMIO bottleneck conclusion.
Corrected sampler reads live cpu.reg.x0, which holds the helper opcode
through masked C execution. Synthetic tests now model that actual source.
A real-device-access guest integration check is required before CE sampling.

Live-X0 sampler integration passes against the baseline TMU guest: 472 TEA
store residency samples, zero unknowns, all guest checks and exit PASS.
Corrected CE baseline is now running, with an exact-473250-tick gate before
the serial pruned-index/PTEH combination timing. The invalid original
histogram and original emulator binary are retained for audit.

Corrected device sampling exactly repeats 473250 ticks and every aggregate
sample total. Fallback residency identifies PTEH 1875, 0xff000034 1281,
PTEL 1230, TTB 715 samples; 7635 remain outside classified stores. These
are residency samples, not access counts. Disassembly confirms ldtlb handling
is inlined into fallback. Counters show 27847 TLB hooks and 22257 scan skips
(79.9%): a guarded no-code-page ldtlb fast path is a plausible next target,
but unclassified residency alone does not prove all of it belongs to ldtlb.
The pruned-index/PTEH combination benchmark is now running.

Pruned-index + PTEH initial timing matches at 469428 ticks / 14.3258056640625 seconds. It improves on narrow-R0 (473250), pruned-index (470520) and PTEH-only (471066), but not the historical 459600-tick binary. A host-sampled repeat is next.

Pruned-index/PTEH sampled repeat exactly matches 469428 ticks, with zero
unmapped guest owners. Approximate fallback residency falls from 12736 to
8891 samples; generated-code residency rises, so the net gain remains only
3822 ticks versus narrow-R0. No five-second result is claimed.

DBT_PAGE_GROUP_FILTER replaces four 1-KiB bitmap probes for an aligned
4-KiB VPN with one aligned-nibble lookup. The hash XOR only permutes the
four low bits; the old wrapping-range result is preserved. An extracted-C
UBSan test compares 95625216 aligned VPN/bitmap cases with the scalar
implementation. MMU, TMU, ASID/bank recycling and interior regressions pass.
The combined candidate is undergoing the same frozen-image directory test.

Page-group first timing matches the directory frame at 467244 ticks / 14.2591552734375 seconds, versus 469428 for the same combination without it. Repeat pending; still above both the historical best and the five-second target.

DBT_SPILL_START_SEARCH optionally binary-searches the monotone spill-start
array before the reverse pending-spill scan. It excludes saves not yet
executed while preserving inclusive restore endpoints and nested undo order.
All append sites set start from the forward emitter position. Host tests
compare every translated offset and output limit against the scalar scan,
plus synthetic tables of every capacity, equal starts, overlapping and open
ranges. ASan/UBSan: 183 checks pass. Target regressions are running; no
performance claim until the frozen CE benchmark completes.

Page-group repeat exactly matches 467244 ticks / 14.2591552734375 seconds. Spill-start search passes MMU, TMU, ASID/bank recycling and interior target regressions; its full CE timing now starts against that repeated control.

DBT_INCOMING_FAST_SCAN revisits the previously rejected incoming bitmap:
source_next uses an inline nonzero De Bruijn bit index instead of libgcc
ctz, and visits only set destination-bank bits instead of all 14 banks for
every word. Publication/ownership/clearing rules are unchanged. Extracted-C
ASan/UBSan checks compare 10434808 ordered iterator results across every
bank mask, plus all 16-bit low/high-half inputs. Audited target tests are
running; separately built timing binaries omit DBT_VERIFY_INCOMING_BITMAP.
A same-bitmap control will isolate this iterator change from bitmap overhead.

Spill-start search matches at 473796 ticks / 14.4591064453125 seconds, slower than the 467244-tick page-group control. Keep it disabled; passing recovery correctness does not establish a speed benefit.

DBT_R0_DIRECT_DEVICE_BRANCH optionally replaces R0 selector XOR/shift with
NEG/AND-immediate. Ordinary addresses produce zero displacement directly to
the RAM sequence; P4 produces the patched exact slow-path displacement.
This removes the BRA-slow island (four bytes), and its executed branch for
P4, while retaining T and the original spill/restore delay slot. Other
scratch registers retain the old layout. The distance must fit eight bits;
otherwise translation returns bounded overflow. Host checks validate all
256 high-byte selector outcomes and exact path entries; 187 checks pass.
ASan/UBSan capacity: 6380 attempts, 4332 bounded retries. Target regressions
are running; no speed claim. The incoming-iterator paired benchmark remains
separate and serial.

R0 direct device branch passes devmem, TMU, ASID/bank recycling and interior target regressions. Default flag-off narrow-R0 binary still exactly matches SHA256 38528dbbd52f2e63d4f90692f5fe7843fed4eccd8b08fd2c32ed0b6b94ee9587. Incoming-bitmap control matches at 489630 ticks / 14.94232177734375 seconds; fast iterator timing is running.

Current pruned-index/PTEH guest samples resolve against 86 timestamp-matching release MAPs. Kernel.dll accounts for 152185 samples (4.644318 modeled seconds), led by KCNextThread, xKCall and NKHandleCall; no single copy routine dominates. classpad_ddi contributes 25630 samples (0.782166 seconds) but its MAP does not match, so no driver function attribution is made. Evidence: prune-pteh-profile/ce-functions.json with NK/MAP hashes.

Fast incoming iterator matches at 503826 ticks / 15.37554931640625 seconds, versus same-bitmap control 489630 and no-bitmap page-group 467244. Keep both bitmap variants disabled. Reducing iterator operations did not improve end-to-end behavior. The independent R0 direct-device-branch benchmark starts next.

R0 direct device branch matches at 471066 ticks / 14.37579345703125 seconds, slower than the 467244-tick page-group control. Keep disabled. Display review confirms the timed wake also handles resumePending; removing it without a replacement wake source is invalid. Next investigation: a masked same-ASID, no-code-page LDTLB path that avoids full context save, with original C bookkeeping retained for all uncertain cases. Current bitmap uses 8192 1-KiB hash bits and supports one aligned-nibble test for 4-KiB pages; any assembly predicate must agree with the C filter and preserve counters, SR/T, return-window recovery and all mapping invalidations.

DBT_FAST_LDTLB adds a masked helper-stub bypass for LDTLB with unchanged
ASID, 4-KiB PTEL size, and an empty code-page bitmap nibble. It reuses the
existing bitmap, not a cached physical translation. Only r0-r2 are borrowed;
X0 temporarily carries original SR after opcode/ASID/size guards. Failed
bitmap checks restore SR and opcode before SAVE_CTX_RT. Success executes
LDTLB, increments native/hook/skip counters, restores SR, and uses explicitly
registered exception-recovery windows for the three GPR restores and jump.
Stats offsets are compile-time checked. Capability 419 / native count 420.
Default builds retain the original helper path.

MMU/TMU/aspace/interior regressions pass. Corrected MMU fixture PTEL flags
from 0x16e (1 KiB despite its comment) to 0x17e (4 KiB), then verified actual
fast execution and forced C fallback on an already translated code page.
A new 20000-iteration TMU loop executes 40000 LDTLBs with alternating T and
r0/r1/r2 sentinels. Every operation takes the native path, timer interrupts
occur, checksum/T and hook/skip counters match exactly. 1-KiB fallback also
passes. Expanded ASID/64-KiB/1-MiB guards and frozen-image CE timing are running.

Expanded native-LDTLB guard test passes 1-KiB/64-KiB/1-MiB fallback and
changed-ASID bookkeeping. The first ASID fixture was invalid: GETSTAT/lookup
between the raw PTEH store and LDTLB could notice the change first. A compact
assembly write+LDTLB sequence removes that interference; counters prove C
handling. Corrected 4-KiB MMU fixture also passes the unoptimized control.
The aspace fixture had the same 1-KiB/comment mismatch; retain its original
coverage and add GUEST_4K_PAGES for a separate full 4-KiB remap/recycling run.

Fast-LDTLB 4-KiB aspace remapping/interior/recycling stress passes. Initial frozen-image CE timing matches at 455232 ticks / 13.892578125 seconds, versus page-group control 467244 and historical 459600. This is a 12012-tick (0.3665771484375-second) gain over the current-source control. A host-sampled repeat of the exact same binary is running; the five-second objective remains unachieved.

Fast-LDTLB return-window host check extracts the real stub_window recovery block and verifies r0/r1/r2 undo, unchanged r3-r15/SR, exact next guest PC and outside-window rejection at each restore/jump boundary for T=0/1. It also checks assembly restore ordering; ASan/UBSan pass. This complements the real TMU interrupt loop, not a claim of hardware validation. Feature-off narrow-R0 rebuild remains byte-identical to its recorded SHA256.

Fast-LDTLB host-sampled repeat exactly matches 455232 ticks / 13.892578125
seconds, with 455233 total samples (216269 generated code, 237146 runtime,
1818 sleep), zero unmapped guest owners. Counters still show 18731 new
translations and 12 bank generations; C fallback calls are 26858 versus
27251 total TLB hooks and 21501 no-code skips. Runtime remains distributed
among exception reconstruction, translation, invalidation and lookup.

DBT_IC_T_ONLY experiments with restoring only T in generated inline-cache
comparisons: STS saved-SR into scratch, then SHLR puts its bit 0 into T.
CMP/EQ is the only SR mutation in that interval; scratch is discarded. Other
SR fields remain intact, unlike a full LDC SR write. Spill endpoints and
instruction counts are unchanged, and default builds retain LDC SR. Host
sequence/recovery checks and target IRQ/ASID/recycling tests are running.

IC T-only restore passes 187 ASan/UBSan host checks and MMU/TMU/aspace/interior target regressions, including T across indirect calls and the expanded fast-LDTLB guard/IRQ suite. Frozen-image full CE timing starts against the repeated 455232-tick fast-LDTLB control.

IC T-only restore matches at exactly 455232 ticks / 13.892578125 seconds,
the same as the repeated full-SR-restore control. No emulator speed gain
is established; it remains optional and is not selected for the next base.

DBT_COMPACT_HEADER32 reduces the target block header from 36 to 32 bytes:
guest length becomes a checked byte (maximum 130), packed-meta/end-reason
share one flags byte, and host_len remains at target offset 22. Target size
and offset assertions guard the layout; entry-dependent specialization is
explicitly unsupported in this experiment. ASan/UBSan host suite passes
187 checks. Build/test both 21546 headers and 23702 headers (1693 per bank)
to separate layout effects from capacity. CE RAM and code/metadata arenas
are unchanged. Host sampler now accepts an explicit 32/36-byte header
stride; synthetic owner lookup tests cover a second block at each stride.

Header32 same-count (21546 blocks) timing matches at 470520 ticks /
14.359130859375 seconds, slower than the 455232-tick fast-LDTLB control.
The 23702-block variant is still running; it must beat the original
36-byte control, not merely the slower 32-byte same-count result.
Host-only sampler can now dump each bank's header count/capacity and last
code/metadata payload ends (excluding trailing alignment), using explicit
arena base/span and 32/36-byte header stride. Synthetic fixtures verify both
count offsets and unchanged owner attribution. It reads backing RAM only
at exit, with no guest MMIO calls or state writes. Full timing-invariance
validation of the newly built calcemu-bank-sample remains pending.

Header32 with 23702 blocks matches at 454686 ticks / 13.87591552734375
seconds. This saves only 546 ticks / 0.01666259765625 seconds versus the
455232-tick original-header control; treat it as provisional until repeated.
New sampler validation will first reproduce the original-header control,
then repeat the larger compact-header binary with its explicit 32-byte stride.

DBT_COMPACT_SPILL6 independently removes the unused spill.insn field and
padding, reducing spill records from 8 to 6 bytes while retaining GPR/slot
and start/end endpoints. Runtime recovery never reads insn; the retained
legacy field only supported a host sequence assertion. Flag-off assignment
order remains unchanged. ASan/UBSan host suite passes 187 checks; target
MMU/TMU/address-space/cache-recycling regressions are running. This is based
on fast-LDTLB without compact headers, keeping the experiment isolated.

Spill6 completes all target MMU/TMU/aspace/interior regressions, including full interrupt recovery, and 6310 ASan/UBSan capacity attempts (4262 bounded retries). Feature-off narrow-R0 binary retains its recorded SHA256. CE timing is pending behind the serial cache-occupancy validation runs.

New bank sampler exactly reproduces 455232 ticks and every aggregate/runtime-attribution sample of the prior fast-LDTLB profile; zero unmapped guest owners. End snapshot has nine banks at 1539-header capacity, three code payload ends above 261120 of 262144 bytes, one near the metadata limit and one partly filled bank. Payload ends exclude alignment; this is capacity evidence, not a direct trace of each eviction reason. Evidence: fast-ldtlb-bank-profile/banks.json. Larger-header repeat is still running.

Capacity follow-up: cache15 combines compact header32/spill6 with 23700
headers, 15 x 256-KiB code banks (base 8cc40000), and a 2624-KiB metadata
arena. It exchanges 256 KiB of metadata reservation for one code bank,
keeping the same physical RAM ceiling and CE's 8-MiB window. Motivation is
the validated baseline snapshot's simultaneous header/code pressure; no
speed claim until tests and frozen-image timing. Builds/regressions running.

Larger header32 bank-sampled repeat exactly matches 454686 ticks, zero unmapped owners. Snapshot now has two header-full banks, seven near code capacity, four near metadata capacity and one partly filled bank; pressure shifted rather than disappearing. Cache15 completes 187 host checks, 6310 capacity attempts and all MMU/TMU/aspace/interior target regressions. Image end 8cc0fba0 stays below stack/cache boundary 8cc40000 with the required reserve; low BSS ends 8c0ff720, below CE RAM. Next serial timings: spill6-only and cache15 combination.

Spill6-only matches the frozen directory frame at 459600 ticks /
14.02587890625 seconds, slower than the repeated 455232-tick original-spill
fast-LDTLB control. Do not select spill compression alone; cache15 still
tests whether spending its space savings on another code bank helps.

Paired-conversion LCD DMA is now rebuilt on the repeated fast-LDTLB base,
without compact headers/spills. The edge guest passes with 36 DMA launches
and a zero exit code; full CE timing is queued after cache15. This isolates
the current conversion from the older bytewise DMA performance result.

Cache15 frozen-image timing matches at 464514 ticks / 14.175842285156 seconds.

DBT_IRQ_ENTRY_HASH replaces the 16-entry FIFO alias scan with a direct
16-slot PC hash, keeping the same entry storage. Repeated insertion avoids
metadata scanning only for the same live block, ASID and flush generation.
Misses/collisions fall back to normal resolution; aliases remain separate
from block-start hashes. Extracted real insertion/lookup code passes
ASan/UBSan fixtures for ASID, generation, dead blocks, collisions, reuse,
borrow/spill refusal and linear boundaries. Feature-off narrow-R0 binary
is unchanged (38528dbb...). Target regressions and full timing are pending.

IRQ-entry hash passes MMU, TMU, address-space and interior/bank-recycling
regressions, including actual IRQ reuse (866 recorded alias hits in TMU).
The hello binary is built; frozen-image timing is queued after DMA.

Paired-conversion DMA on fast-LDTLB matches at 447042 ticks / 13.642639160156 seconds;
control is 455232 ticks. Emulator DMA is instantaneous, not hardware timing.

The current paired DMA result saves 8190 ticks / 0.24993896484375 seconds
against its repeated fast-LDTLB CPU baseline. This is a provisional gain,
not the older byte-conversion result. Exact-ELF bank-sampled repeat is
prepared with the original 36-byte headers and 14-bank layout, to follow
the independent IRQ-entry hash timing. No hardware DMA-speed claim.

DBT_LAZY_RESUME_META skips fix-up metadata unpacking in apply_resume_state
when the memory operation has not committed. Spill restoration and both
resume counters remain unchanged; committed loads keep their original fix-up
path. 320 ASan/UBSan differential cases compare actual eager/lazy bodies
with real packed/unpacked metadata and spill scans across tail positions,
T restoration, GPR and supported control-register fixes. Feature-off binary
remains identical to narrow-R0. Target regressions are running.

IRQ-entry hash matches at 461238 ticks / 14.075866699219 seconds against the repeated
455232-tick fast-LDTLB control. DMA repeat follows with its exact ELF.

Lazy resume metadata passes MMU/TMU/aspace/interior regressions. Its hello
image is built and isolated timing is queued after the paired-DMA repeat.
IRQ-entry hash regresses to 461238 ticks / 14.07586669921875 seconds versus
455232, so it is not selected or combined into DMA.

DBT_LCD_DMA_UNROLL asks GCC to unroll the aligned RGB565 conversion loop
fourfold. Disassembly confirms four pair conversions per loop branch;
byte swaps, volatile buffer writes, DMA state and tail handling are unchanged.
The edge guest records 36 launches and its entire PPM is byte-identical to
the current paired-DMA control, including odd tails and short fallback.
The hello binary is built; full CE timing is pending.

Paired-DMA bank-sampled repeat: 447042 ticks / 13.642639160156 seconds, zero unmapped
guest samples. Initial timing was 447042 ticks. Evidence: fast-dma-bank-profile.

Paired-DMA repeat is exact at 447042 ticks, with 210335 generated-code,
234869 runtime and 1839 sleep samples; no unmapped guest owners. Control
has 216269 generated-code, 237146 runtime and 1818 sleep samples. The
current repeated best is 13.64263916015625 seconds, still above five.

Lazy resume metadata matches at 468336 ticks / 14.292480468750 seconds against its
455232-tick CPU control. DMA-unroll timing follows against 447042 ticks.

DBT_R0_LOAD_FALLTHROUGH reorders narrow-R0 device-aware loads with inline
P4 reads. ADD -1 / AND 6 selects the six-byte cold-load/BRA/NOP island for
P4 or skips it for RAM. The swapped RAM sequence falls through, eliminating
its trailing BRA/NOP. Stores, R0 operands and indexed addressing retain
the prior path. Separate cold/hot commit metadata preserves exception
reconstruction. All 256 top-byte selectors, 188 emission/recovery checks
and 5968 sanitized capacity attempts (3920 bounded retries) pass. Target
RAM/P4 interrupted loops and explicit non-R0 delay-slot/postincrement
reads pass; remaining target checks and timing are pending.

RAM-load fallthrough completes devmem/TMU/aspace/interior regressions.
The expanded delay-slot/postincrement fixtures also pass on the unchanged
fast-LDTLB control. Default narrow-R0 image hash remains 38528dbb... .
Frozen-image CE timing is queued after the independent DMA-unroll run.

DMA unroll matches at 463968 ticks / 14.159179687500 seconds against its repeated
447042-tick paired-DMA control. RAM-load fallthrough timing follows.

Expanded fallthrough IRQ proof passes in candidate and fast-LDTLB control:
10,000 iterations each of RAM/P4 postincrement plus BF/S delay-slot loads,
with checksum, updated-address and actual interrupt-count checks. Runtime
source and measured hello images are unchanged by these test-only additions.
DMA-unroll regresses to 463968 ticks (14.1591796875 s), so keep the repeated
447042-tick non-unrolled DMA implementation.

Fast-DMA final bank snapshot has nine 1539-header-full banks, like the
CPU control; saved as fast-dma-bank-profile/banks.json (not an eviction
reason trace). Test DMA plus compact-header32/23702 headers, both individually
repeated improvements, retaining 14 code banks, 2880 KiB metadata and CE RAM.
Combined target regressions run before timing; no additive gain assumed.

RAM-load fallthrough matches at 461784 ticks / 14.092529296875 seconds against its
455232-tick fast-LDTLB control.

Load-fallthrough regresses to 461784 ticks / 14.092529296875 seconds.
Follow-up DBT_R0_LOAD_MERGE_META shares the classifier/cold-read metadata
only when the parent is META_INSN with the same guest offset and no prior
commit. Otherwise retain a separate record. The hot RAM load still has its
own byte-swap commit. This avoids one six-byte packed record for eligible
loads; it is a capacity/lookup hypothesis, not a measured cause of regression.
5968 sanitized capacity attempts pass; explicit range and target IRQ proofs
are running before a new timing.

DMA/header32 combination passes MMU/TMU/aspace/interior/lcd_dma target
regressions and link-layout checks. Image end 0x8cc4ef00, low BSS end 0x8c0ff770; CE RAM
and 128-KiB stack reserve remain intact. Serial full CE timing starts.
Merge-meta host checks now pass 190 assertions; disabled-feature image
remains byte-identical to narrow-R0.

Load-merge completes devmem/TMU/aspace/interior target regressions, including
the expanded conditional-slot/postincrement IRQ checks for RAM and P4.
Visual inspection of fast-dma/final.png confirms root directory entries,
toolbar and taskbar are fully painted, complementing the exact ROI matcher.

DMA/header32 matches at 461238 ticks / 14.075866699219 seconds against the repeated
447042-tick DMA control.

Load-merge matches at 459600 ticks / 14.02587890625 seconds: faster than
unmerged 461784 but slower than fast-LDTLB control 455232. Keep disabled.

DBT_HASH_ASID_MIX separates direct user hash indices using ASID<<5; kernel
keys and block-owner chains remain PC-only. C insertion/find/removal and
assembly probes agree; shared-physical ASID_ANY owners clear every matching
host-range entry. 1M C/ASM index comparisons and removal fixtures pass under
ASan/UBSan with/without local alias removal. First target MMU/TMU passed;
aspace values/remaps passed but its old interior-cache-hit assertion failed
because direct-hash retention bypasses that cache. Capability 421 selects
a zero-retranslation/unchanged-interior-hit assertion; old-mode assertion
is retained. Full target regressions rerun before any speed claim.

ASID-mix aspace now passes, including shared-owner remap, retained direct
interior alias without retranslation, and user remapping after bank recycling.
Unmixed control still passes its original interior-cache-hit assertion.
Default narrow-R0 image remains identical (38528dbb...). MMU/TMU rerun pass;
final interior regression and hello build are still in progress.

ASID-mix completes all target regressions and the hello build. Frozen-image
My Device timing starts against the repeated 455232-tick fast-LDTLB control.

ASID-mixed direct hash completes at 471066 ticks / 14.37579345703125 seconds with exact directory-frame matching and exit code zero. This is slower than the 455232-tick CPU control; keep disabled. Evidence: build-speed-profile-review/asid-mix/timing.json.

The optional DBT_HASH_TWO_WAY experiment retains 8192 total hash entries but probes two adjacent ways per bucket in both C and assembly. MMU, TMU, address-space and interior-entry target regressions pass. tools/test_two_way_hash.py extracts the actual C insertion, lookup and invalidation bodies (substituting only host-incompatible SR assembly) and passes ASan/UBSan with both full-scan and local alias removal: colliding PCs, ASID isolation, refresh/replacement, shared-owner removal, both-way interior aliases, range boundaries and global kernel keys. Complete CE timing is running; no speed claim yet.

Two-way forced-collision target regression passes: P_A/P_S share the folded bucket, alternate 4096 times with correct values and fewer than 16 C refills, and preserve T. Default feature-off hello binary still exactly matches 38528dbb... baseline. A separate DBT_INTERIOR_RANGE_FIRST experiment rejects page-chain candidates outside their guest PC range before loading binfo or calling metadata entry validation; all original candidate checks remain. Tests and timing pending.

Two-way hash completes at 457962 ticks / 13.97589111328125 seconds with exact reference matching. Slower than the 455232-tick single-way CPU control; keep disabled despite passing collision and target regressions. DBT_NATIVE_FETCH separately inlines only the normal translator fetch path on native big-endian SH: same volatile 16-bit mapped load and byte swap, same odd-address rejection, no speculative fetch. Prepass/reference-fetch callbacks are unchanged. Runtime MMU/translation recovery tests pending.

Interior-range-first completes MMU/TMU/aspace/interior target regressions and its hello build. Frozen-image My Device timing starts against the 455232-tick CPU control; no clock or guest-memory changes.

Native-fetch completes MMU/TMU/aspace/interior regressions and its CE hello build. New cross-page code-probe regression passes with both the native-fetch candidate and callback control: translating two mapped NOPs probes the initially unmapped next page, takes exactly one miss, preserves entry SPC/missing-page TEA, refills and returns correctly. Logs: /tmp/build-native-fetch-cross-mmu-run.log and /tmp/build-fetch-cross-control-mmu-run.log. CE timing awaits the running interior-range measurement.

Native-fetch code audit: the main dbt_translate_block constprop function shrinks from 0x3ba2 to 0x3a4c bytes (342 bytes); this is code-size evidence, not a timing result. Feature-off hello remains byte-identical to narrow-R0 (38528dbbd52f2e63d4f90692f5fe7843fed4eccd8b08fd2c32ed0b6b94ee9587).

Interior-range-first completes at 463968 ticks / 14.1591796875 seconds with exact frame matching. Slower than the 455232-tick CPU control; keep disabled. Native-fetch frozen-image CE timing follows as a separate experiment.

Host-only allocation-pressure trace added to add_dbt_sample.py: interpreter compares the exact ELF translate_at entry PC and reads backing-RAM bank/count/code/arena offsets. At the best unified 14-bank layout it reports the same three preallocation predicates (1539 headers, 1024 code bytes reserved, 2768 scratch metadata bytes reserved). No emulated memory API, guest state write or clock change. Synthetic tests verify equality boundaries, reason bits 1/2/4, start gating, invalid bank rejection and unchanged backing RAM. Dedicated emulator builds successfully; exact-binary fast-DMA repeat is prepared, pending native-fetch timing completion.

Native-fetch completes at 460146 ticks / 14.04254150390625 seconds with exact directory-frame matching, slower than 455232-tick CPU control despite smaller translator code. Keep disabled. The exact best fast-DMA binary now runs under host-only fill-pressure tracing; verify the repeated 447042-tick timing before attributing capacity evidence.

Next generated-code candidate identified during audit: no-borrow direct exits execute a leading NOP before spill/load/jump/restore. Moving the padding after the jump delay slot could remove one executed instruction on unchained/far exits while retaining the five-word patch window. Not implemented: this requires updating short-link no_restore selection, saved unlink words, exact recovery boundaries and sequence tests together. Wait for allocation-pressure evidence before choosing the next candidate.

DBT_EXIT_NOP_TAIL implements the no-borrow exit experiment: emit spill/literal-load/jump/restore then unreachable NOP, keeping the five-word patch window and conditional skip distances. link_direct marks no-borrow chains no_restore so a short-chain delay slot never inherits the now-first scratch save. Borrowed exits remain unchanged. Sequence expectations updated for the opt-in layout; initial 183 ASan/UBSan checks pass, target MMU/TMU/aspace/interior tests running. Explicit pending-spill boundary checks added; rerun pending. No CE speed claim.

Exit-NOP-tail rerun passes 186 ASan/UBSan host checks, including save/restore delay-slot spill boundaries and unreachable padding. MMU and TMU target regressions pass. Feature-off hello rebuild completes; hash checked against the narrow-R0 baseline.

Fast-DMA exact-binary fill-trace repeat matches 447042 ticks / 13.64263916015625 seconds, exact frame, exit zero. There are 13 pressure observations: first two share identical bank-5 occupancy only five ticks apart, so do not count both as completed recycling. Collapsing consecutive identical occupancy yields 12 groups: 9 header-limited, 2 code-limited, 1 metadata-limited; flush/generation delta is also 12. Most header-limited banks leave roughly 16–34 KiB of code unused. Raw and grouped evidence is in fast-dma-fill-trace/fill-pressure.json. This confirms header capacity is the most common pressure trigger, not that increasing headers necessarily improves total latency (the earlier header32+DMA combination regressed).

Based on measured header pressure, DBT_HASH_HIGH_RAM tests a layout-only alternative to compact-header bitfields: retain 36-byte headers, move the existing 96-KiB tagged hash out of .lowbss, increase headers 21546 to 23702 (1693/bank), reduce metadata arena 2880 to 2720 KiB to cover relocated hash and larger side tables. Same 14 code banks, same CE memory and DMA path. Linker bounds and target regressions must pass; no speed claim.

Exit-NOP-tail completes all four target regressions and CE build; callback/default host suite passes 180 checks. Feature-off binary matches 38528dbb... . Frozen-image CE timing starts.

DBT_LCD_DMA_STRIPS was removed on 2026-11-06 (with the lcd_dma guest and
HC_GETSTAT 400/401). The display driver pushes at most four rows per trap
(1280-2112 pixels), below one 4096-pixel strip, so CE never overlapped CPU
conversion with DMA: each push converted, started DMA and waited. Its only
speed evidence was the emulator, whose DMA completes instantly.

### Shell working set (2026-11-06)

Question: the Standard Shell is very slow while Solitaire runs normally.
`-DDBT_TRACE_XLATE` (rt.c) prints one `[xl] gpc asid phys guest_len` line per
translation. One My Device run (HEAD of main, 3 MiB / 14400 / 2624 KiB, normal
power NK bec974fc...) classified every translation between the second release
and the matched frame:

| | count |
| --- | ---: |
| translations | 48185 |
| of those: same (gpc, ASID) translated earlier, evicted since | 37595 (78%) |
| first translation of that code | 10569 |
| same code under another ASID | 21 |
| distinct blocks | 27964 |
| bank recycles (12 banks) | 40 |
| PROFILE translate share of TMU1 time | ~40% (trace output inflates it) |

Blocks translated 1/2/3/4 times inside that one load: 14226/8400/4239/1090.
The most retranslated modules are gwes (11560 of 14295), kernel.dll
(6495/7446), coredll (3086/3499), k.coredll, commctrl, mgtt_o (fonts),
ceshell, classpad_ddi, filesys and fsdmgr. Opening one folder touches about
twice as much code as the cache holds, and FIFO banks evict GWES/kernel
code that is in constant use. A game's working set fits and stops
translating after its first frames (inferred, not traced).

Re-translating the 27944 blocks offline with the same translator sources
(host harness over the flattened NOR, kernel.dll device-aware) gives the
footprint per guest byte:

| | non-kernel blocks | kernel.dll (device-aware) |
| --- | ---: | ---: |
| blocks | 24172 | 3772 |
| guest bytes per block | 22.9 | 24.9 |
| host bytes per block (code + literals) | 141 (84 + 57) | 366 (266 + 100) |
| metadata + spills + links per block | 139 | 223 |
| host + metadata per guest byte | 12.2 | 23.6 |

Plus 36 bytes of header and ~25 bytes of side tables per block, and 32-byte
code alignment. Across all blocks, 37% of host bytes are literal pools and
another ~19% are prologues, exits and IC sequences; memory operations are
the largest instruction share (6.7 host bytes per guest byte, three swaps
per 32-bit access), and a device-aware 32-bit access is ~30 host words.
Overlapping blocks (entries into the middle of existing code) cover the
455 KB of distinct guest code 1.43 times (648 KB). The shell's folder-open
working set would need roughly 9 MB of cache, headers and arena.

Cache-size sweep, same NK and launch tick, ram-layout sources, main's RAM
window (all matched the frame):

| code cache / headers / arena | load ticks | modeled s |
| --- | ---: | ---: |
| 2 MiB / 9600 / 1752 KiB (ram-layout default) | 1111524 | 33.921 |
| 3 MiB / 14400 / 2624 KiB (main default) | 895854 | 27.339 |
| 3.1875 MiB / 17280 / 2528 KiB | 867462 | 26.473 |

Capacity below the working set gives diminishing returns (FIFO over a
cyclic working set larger than the cache): the 11.3 MiB CE layout cost
24% on this load, and more capacity alone will not reach the target. The
layout therefore returns to 8 MiB of CE RAM (0x8C000000-0x8C7FFFFF) and
gives the reclaimed first MiB to the translator as code cache: 4 MiB cache,
14400 headers, 2624 KiB arena (495a3cf; the 3.625 MiB / 20160 / 2944 KiB
split sized from the per-block ratios above was slower, see the 8 MiB table
below). The larger lever is density: metadata is half the footprint,
literals a third of the code.

### Metadata on demand, second chance (2026-11-06)

Two cache policies, both measured on the same My Device load (NK built for
the 9 MiB window, so the translator has 7 MiB; exact frame match in every
run).

**Second chance** (`SECOND_CHANCE=1`, `-DDBT_SECOND_CHANCE`). After a bank is
recycled, the next bank to be recycled goes on probation: hash entries,
inline caches and direct chains into it are dropped (`detach_bank`), so its
blocks are reached only through the C lookup, which marks every block it
finds (`find_block_by_gpc`); exceptions and timer ticks inside a block mark
it too. When that bank's turn comes, the marked blocks (at most
1/`DBT_SECOND_CHANCE_SHARE` of the bank, default 1/2) are moved to its start
in order, so headers stay sorted by host address (`keep_survivors`), and the
bank fills after them. Translated code holds absolute addresses of itself in its
literal pool (helper return addresses, the exit stubs' link-literal
addresses, the IC pair's address; `TAG_RET`, `TAG_STUB_L`, `TAG_IC_A`), so
the translator now reports which pool words those are (`dbt_env.reloc_map`)
and each block keeps a relocation record (pool offset plus one bit per pool
word, ~4 bytes) after its links. A moved block is rebased, fully
unchained (original jump words, link literals back at its own stubs) and
its IC reset; incoming links come back through the normal lookup. With
14400 headers about 20% of a bank's blocks survive (234 of 1200 per
probation, 213 bytes of code each), and the whole run (boot included)
translates about a fifth less: its last 8192-translation stats checkpoint
before the frame is 106k against the baseline's 131k.

**Metadata on demand** (`LAZY_META=1`, `-DDBT_LAZY_META`, opt-in). A ROM
block (its guest bytes, including folded PC-relative literals, physically
contiguous inside the image's ROM range; `lazy_eligible`) is committed
with links only. When recovery needs its metadata (an exception or tick
inside it, a helper fault, a stub window), the block is translated again
at its own host address from the recorded physical address through P1/P2
(no TLB), the copy is checked against the live code outside the link
windows, and the packed metadata is kept at the top of the block's bank
arena (`ensure_meta`). A hashed per-pc hint keeps metadata resident from
the next translation on for blocks that needed it once.
`-DDBT_LAZY_META_ALL` drops the ROM condition, for the RAM-resident test
guests (exc, tmu, tlbdata, fpudis and bank rebuild metadata there).

| build (code cache / headers / arena) | modeled s |
| --- | ---: |
| baseline, 3 MiB / 14400 / 2624 KiB | 27.356 |
| second chance (cap 1/2) | 26.606 |
| second chance (cap 1/3) | 26.623 |
| second chance (cap 1/4) | 27.306 |
| metadata on demand, no hint | 34.221 |
| metadata on demand with the hint | 28.639 |
| both | 27.039 |
| both, 4.0625 MiB / 21600 / 1408 KiB (no hint) | 34.821 |
| on demand only, same layout (no hint) | 33.571 |
| both, 3.625 MiB / 18000 / 2048 KiB | 28.089 |

Metadata on demand loses on this load. Without the hint, 48.6k rebuilds per
128k translations: about 38% of blocks take an exception or tick in their
lifetime, and 78% of translations redo evicted code, so the same blocks
rebuild in every life. The hint cuts that to 21k but the cost remains, and
the capacity it frees is worth little: capacity below the working set gives
diminishing returns (sweep above), and moving the freed arena into ~50% more headers and
a third more code made it slower still. Second chance is the default; metadata
on demand stays as an opt-in (it is the right tool only where capacity is
the binding limit and exceptions inside blocks are rare).

Device-aware accesses (the other density item): the hot path adds ~15
instructions per 32-bit kernel.dll access and the whole sequence, with its
inline slow path (helper call, three literals, its own metadata range), ~56
bytes. 4339 of the 12618 candidates in the traced kernel blocks (34%) use
r15 as base (pushes, pops, frame slots); the stack never points into P4, so
those could take the plain access.

Where the load's time goes (emulator host PC samples from the tap to the
frame, one per tick; runtime samples grouped by function family, so
attribution near function boundaries is approximate):

| ticks (modeled s) | baseline 27.36 s | second chance 26.61 s |
| --- | ---: | ---: |
| translation (decode, emit, memset/memcpy, pack) | 312k (9.5) | 258k (7.9) |
| translated code | 251k (7.7) | 249k (7.6) |
| invalidation and UTLB reads (`invalidate_*`, `utlb_translate`) | 118k (3.6) | 130k (4.0) |
| lookup and linking (C side) | 89k (2.7) | 101k (3.1) |
| stubs, vectors (IL memory, "other") | 65k (2.0) | 65k (2.0) |
| helpers, exceptions, LCD | 52k (1.6) | 58k (1.8) |

Translation is about a third of the load, not all of it. Second chance cut it
by 17%, and about half of that came back as probation lookups and more
invalidation work over the larger number of live blocks. `utlb_translate`
alone is 56k ticks (1.7 s), and `memset` inside translation 50k (1.5 s).
With the same 14400 headers, a 3.25 MiB cache gave 26.973 s against 3 MiB's
27.356 s, so at 3 MiB the code space, not the header count, still limits.

On the 8 MiB layout (VM image claude-rstandby-fix NK.bin 62ac46d2..., CE RAM
to 0x8C7FFFFF, translator 0x8C800000-0x8CFFFFFF; same tap and reference
frame, all matched). The frame check runs per 60 Hz frame, so load times are
quantized to 546 ticks (16.7 ms):

| code cache / headers / arena | second chance | modeled s |
| --- | --- | ---: |
| 4 MiB / 14400 / 2624 KiB (495a3cf) | off | 26.973 |
| 4 MiB / 14400 / 2624 KiB | on | 25.806 |
| 4 MiB / 16800 / 2624 KiB | on | 27.539 |
| 3.625 MiB / 20160 / 2944 KiB | on | 26.223 |

More headers is slower here even with the same or more code space: more
live blocks make every invalidation scan longer (invalidation is already
3.6 s of the load, above). 4 MiB / 14400 with second chance is the best of
these.

What the translated code spends its time on (second chance, 3 MiB layout,
26.61 s; host opcode and guest-block residency samples from the tap to the
frame, `HLE_DBT_SAMPLE_OPS`/`HLE_DBT_SAMPLE_BLOCKS`; kern.map from the VM
release directory). 248786 generated-code samples (7.59 s):

- by module: kernel.dll 73.1%, classpad_ddi 10.3% (almost all
  `GPE::EmulatedBlt_Internal`), gwes 7.1%, k.coredll 4.7%. The hot kernel
  code is CE's thread/system-call machinery: KCNextThread 6.4%, KCall 4.6%,
  NKHandleCall 4.3%, InterruptExceptionHandler 2.9%, GeneralException 2.6%,
  then CRITEnter, RunqDequeue, handle locking, waits, SetCPUASID,
  TLBMissHandler, ObjectCall, SwitchActiveProcess.
- by host opcode: shifts 17.7% (1.35 s), byte swaps 17.2% (1.31 s), DSP
  slot transfers 9.9%, nops 6.5%, braf 2.8%; the top opcodes are the
  device test (`not`, `shlr16`, `shlr8`, `neg`, `add #1`, `shll2`, `braf`,
  `bra` over the slow path). The device test is roughly 2-2.5 s of the 7.6.

Exact counts over the same window (`-DDBT_STAT_IN_RAM`, the emulator's
`HLE_DBT_COUNTER_BASE` snapshot; that build also records device sites and
took 29.39 s): 39665 translations, 11767 system calls (synthetic
instruction-address faults), 22162 ASID switches, 76302 exceptions, 35646
ldtlb hooks, 87799 rte, 391385 `ldc sr` helper calls, 239050 device
accesses through the C fallback, 305986 C lookups (160067 of them blocks
missing from the hash), 186240 IC misses.

`-DDBT_DEV_SITES` records every guest pc whose device-aware access really
reached P4. Over the whole run (boot and load) that is 23 sites in 9
kernel.dll functions: TLBMissHandler (5 sites, 305886 accesses),
InterruptExceptionHandler (1, 240925), SHSetASID (1, 75638), MDSetCPUASID
(1, 49670), APICall (1, 17513), GeneralException (2, 300), MDSwitchVM,
SHInit, LoadKPage. Every other kernel.dll access pays the test for nothing.
A literal-pool scan for P4 constants finds some of these functions
(InterruptExceptionHandler, SHSetASID, MDSetCPUASID, MDSwitchVM, SHInit) and
also non-code false positives, but misses TLBMissHandler, APICall,
GeneralException and LoadKPage, so a narrower device-aware range needs a
fixed function list for this kernel.dll, not a scan alone.

### Device-aware translation only where kernel.dll can reach P4 (2026-11-06)

`KDEV=1` (`-DDBT_KDEV`) narrows the device-aware range from all of
kernel.dll to the functions that may access P4. tools/kernel_devfuncs.py
reads an NK.bin and its kern.map and keeps a function when its own words
hold a mov.l literal with the top byte 0xFF (below 0xFFFF0000), when it
moves to or from a banked register (CE keeps the SH3CTL base, 0xFF000000,
in r5_bank1), when it builds such a value with `mov #-1` and shifts, or
when it lies in the exception vector area (ExceptionBase to
_ExceptionEndAddress: those handlers run with RB=1, where r0-r7 are the
banked registers, so their P4 base leaves no trace). That keeps 25 of 1090
functions, 5932 of 190060 code bytes: the vector-area handlers
(GeneralException, TLBMissHandler, InterruptExceptionHandler, APICall,
SHSetASID, LoadKPage, ZeroPage, ...), CaptureContext, SyscallHandler,
NKPerformCallBack, DoSetRAMMode, DumpFrame, SHInit, MDSwitchVM,
MDSetCPUASID, NKQueryInstructionSet and a few with P4-looking constants.

kernel.dll is relinked into every image (other address, the 2026-11-06
image's text is 20 bytes longer), so rt/kernel_dev.h holds a
relocation-insensitive signature per function instead of addresses: the
length, the first four words and a CRC-32 over the function, with
PC-relative literals, outward bra/bsr displacements and aligned words
holding P1/P2 addresses masked out. rt/kdev.c scans kernel.dll's text at
boot (nkbin's largest code section) and needs every function exactly once;
otherwise it prints which one and the whole of kernel.dll stays
device-aware. The translator (`dbt_env.dev_ranges`) then binary-searches
the merged ranges (12 for both images measured). `--check NK.bin SITES`
runs the same search on another image and checks recorded P4 sites
against it. The table was generated from the VM's ClasspadUI release
(NK.bin 62ac46d2..., kern.map) and locates in the older benchmark image too.

Checks: every P4 access recorded with `-DDBT_DEV_SITES` lies inside the
located functions (23 sites in each image, boot and load); host
translator test (an access outside the listed ranges is plain, inside one
it keeps the test); 14 guests pass; both images match the reference frame
and their boot logs list exactly the ranges the script predicts.

| image, layout (second chance on) | whole kernel.dll | KDEV=1 |
| --- | ---: | ---: |
| old benchmark NK, 3 MiB / 14400 / 2624 KiB | 26.606 s | 21.457 s |
| 2026-11-06 8 MiB NK, 4 MiB / 14400 / 2624 KiB | 25.806 s | 23.540 s |

Risk: a kernel.dll function outside the list that reaches P4 only through
a pointer it was given, on a path neither the rules nor the recorded runs
saw, would get swapped values. KDEV=1 is the default; `KDEV=0` restores
the old behavior. Regenerate the table when kernel.dll's sources change
(a new CE update): `tools/kernel_devfuncs.py NK.bin kern.map -o
dbt/rt/kernel_dev.h`; an unchanged kernel only moves, which the
signatures absorb.

### Translation copies, UTLB lookups and invalidation scans (2026-11-07)

Runtime profile of the My Device load (second chance + KDEV, old 9 MiB NK,
host samples from the tap to the frame): besides translation proper,
`memset` 1.10 s and `memcpy` 0.86 s, `utlb_translate` 1.18 s,
`invalidate_block` 1.24 s and `invalidate_range` 0.66 s. Their causes and
what changed (each old form stays behind a switch):

- **Per-instruction clearing and copying.** `sh4_decode` cleared a whole
  `sh_insn` with `memset` per decoded opcode, `begin_meta_kind` a 16-byte
  recovery record per instruction, and `fetch` copied each pre-decoded
  instruction out of `g_pre`. The fixed-size template initialiser
  (formerly opt-in `DBT_DECODE_TEMPLATE`; `-DDBT_DECODE_MEMSET` restores
  the old one, `tools/test_decode_template.py` compares both for all 65536
  opcodes) and the four-word record write (formerly `DBT_META_WORD_INIT`;
  `-DDBT_META_MEMSET`) are now the default, and `fetch` returns a pointer
  into `g_pre` (which nothing writes during the translation) instead of a
  copy.
- **UTLB lookups.** Every translation of user or P3 code, cross-ASID
  lookups and the tick classifier read all 64 UTLB array entries (two P4
  reads each). The slot hint (formerly `DBT_UTLB_HINT`; `-DDBT_UTLB_SCAN`
  restores the scan) remembers the slot that matched last for a hashed
  page and ASID and re-reads that slot's hardware words on every use, so
  it never returns a stale mapping; a mismatch falls back to the scan.
- **One detach pass per flush.** `invalidate_block` used to walk all
  headers for the ICs and direct links into each block it killed: a flush
  killing k blocks made k passes over 14400 headers. Over boot and load,
  the range flushes killed 2635 blocks in 7 calls, the HC_FLUSH-all
  teardowns 322 in 46, ldtlb replacements 486 in 16k hooks (counters from
  `-DDBT_INVAL_DIAG`). Now `invalidate_block` records the dead host range
  and `finish_invalidations`, which every caller already runs before the
  guest continues, detaches the ICs and links into all recorded ranges in
  one pass: up to 16 ranges compared directly, beyond that a 32-byte
  granule map that is cleared after the pass. Nothing survives the batch,
  so unlike `DBT_BATCH_INVALIDATE` it needs nothing from bank recycling
  and works with second chance. `-DDBT_INVAL_IMMEDIATE` restores the
  per-block scan.
- **User range flushes without a full scan.** CE's HC_TLB_INVAL (229 calls
  during the load, none killing anything) walked every header. User blocks
  are already in the page index under their first and last 1 KiB page
  (blocks are shorter than 1 KiB, so one of the two lies within any range
  they overlap) and in `g_user_idx`; a range of up to 256 KiB now walks
  the index chains of its pages, a larger one the user list.
  `-DDBT_INVAL_RANGE_SCAN` restores the scan. The aspace guest gained
  checks that HC_TLB_INVAL alone drops a block for a partial range on its
  page, keeps it for a range on the next page, and drops it for a
  whole-space flush; they pass on both the old and the new path.

Results, My Device load, second chance + KDEV, all exact frame matches.
On the 2026-11-06 8 MiB image (4 MiB / 14400 / 2624 KiB), changes added
one at a time:

| build | modeled s |
| --- | ---: |
| a589323 | 23.507 |
| + no per-instruction clear/copy | 21.491 |
| + UTLB slot hint | 19.858 |
| + one detach pass per flush | 19.058 |
| + user range flushes via the page index | 18.341 |

The same changes on the old benchmark image (9 MiB window, 3 MiB / 14400 /
2624 KiB, host-sampled): 21.707 s -> 13.559 s. Sampled seconds, a589323
-> now: memset 1.17 -> 0.07, memcpy 0.72 -> 0.12, utlb_translate 1.39 ->
0.43, invalidate_block + invalidate_range 1.50 -> 0.06 plus 0.69 in the
single detach pass, translated code 4.51 -> 2.50 (less waiting work in the
changed schedule; not attributable to one change).

Single runs move with the schedule: a diagnostic build of a589323 that only
adds counters and prints (`-DDBT_INVAL_DIAG -DDBT_STATS_EVERY=2000`)
finished in 21.691 s against 23.507 s. Steps under ~2 s in the table are
therefore not individually established; the sampled per-function times
are the firmer evidence. Host tests (179 checks), the decode comparison
and all 15 guests (extrom with NK_WINDOW=eact) pass.

### Lookup hash per address space, owner cache, cheaper detach pass (2026-11-07)

Load-window counters of d4091fa (old benchmark image, `-DDBT_STAT_IN_RAM`
snapshot): 35873 translations, 207160 C lookups, 107076 of them for a block
that existed but was not in the hash. `-DDBT_HASH_DIAG` records what last
emptied each hash slot and classifies those lookups: ASID switch 67006
(72%), another pc in the slot 18611, bank recycle 4899, second-chance
probation 3013. CE6 processes share virtual addresses, and untagged entries
had to leave the hash at every address-space switch.

- `HASH_TAG=1` (Makefile, default): `DBT_HASH_ASID_TAG` (12-byte entries
  with the ASID; a switch keeps them), `DBT_HASH_ASID_MIX` (user keys mix
  the ASID into the index; tagging alone put every process's code at the
  same addresses into the same slots: 276k C lookups, slower) and
  `DBT_USER_IC_HASH_HIT` (an IC miss on a user target jumps from a hash
  hit in the stub instead of refilling in C). These were measured opt-ins
  of earlier work; together they take C lookups from 278k to 148k at the
  same workload. `HASH_ENTRIES` sets the size (8192; 16384 gave 131k and
  ~0.24 s for 96 KiB more, within the run-to-run spread).
- `block_of_host` keeps its last answers in 256 slots by host address. A
  hit needs a live block whose host range holds the address; live blocks
  never overlap, so it is the block the binary search finds, and dead
  owners still come from the search. Cleared on full flush and bank
  recycle. 49% of 211k calls hit. `-DDBT_OWNER_SEARCH` disables it.
- The one-pass detach reads the IC addresses from a dense per-header array
  (`g_ic_site`, written at translation and survivor moves; stale for dead
  headers, which are checked) and the link masks in a separate loop with a
  local bound.
- The emitter's per-halfword helpers (`emit16`, `emit_pos`, slot opcodes)
  are inline in sh4emit.h; the FPU opcode gate (formerly
  `DBT_DECODE_FPU_GATE`, `-DDBT_DECODE_NO_FPU_GATE` restores it) skips the
  call into fpuemu for opcode groups that cannot be FPU instructions; the
  exhaustive decoder comparison now covers both.

Single runs vary with the system state at the tap: the same changes took
the old image from 13.71 s to 16.43 s in one run because the guest did
more work in it (`ldc sr` 97k -> 323k, ASID switches 13.5k -> 21k,
translated code 2.56 -> 4.34 s), while the translator's own C time stayed
at ~10 s. Comparisons below are therefore means over three tap ticks
(7.5M, 8.0M, 8.5M) on the 2026-11-06 8 MiB image, all exact frame matches:

| build | mean | runs |
| --- | ---: | --- |
| d4091fa | 18.30 s | 18.34 18.30 18.25 |
| owner cache, detach arrays, inline emitter, FPU gate | 16.98 s | 17.11 17.06 16.77 |
| + HASH_TAG=1 | 15.69 s | 15.69 15.86 15.52 |
| + HASH_ENTRIES=16384 | 15.45 s | 15.24 15.41 15.68 |
| final default (HASH_TAG=1, 8192, local loop bound) | 15.31 s | 14.81 15.51 15.60 |

Sampled on the old image at the same (busier) workload, items without vs
with HASH_TAG: translator C 10.26 -> 9.25 s, C lookups 278k -> 148k,
`block_of_host` 0.60 -> 0.28 s, the detach pass 0.60 -> 0.52 s. Remaining
leaders: `dbt_translate_block` 1.50 s, `lookup_or_translate` 1.12 s,
`sh4_decode` 0.74 s (36.4k translations); translated guest code 4.3 s and
the assembly stubs 1.8 s in that run. Host tests, the decoder comparison,
all 15 guests (aspace runs its 9 tagged-hash checks too) and the HHK check
pass.

### Where the guest's time goes, and CE's TLB refill in the translator (2026-11-07)

Guest-block profile of the 8 MiB image at three tap ticks (3 MiB-cache
build, so the sampler's 256 KiB banks apply; maps from the VM release
directory): 15.49 / 14.68 / 15.45 s, translated guest code 4.34 / 3.87 /
4.38 s. classpad_ddi (0.75 s, `GPE::EmulatedBlt_Internal` 0.56 s), gwes
(0.5 s) and the rest are the same in all three; the difference is
kernel.dll (2.25 / 1.86 / 2.28 s: KCall, GeneralException, KCNextThread,
NKHandleCall), i.e. how many system calls and thread switches happen. No
polling loop or background job stands out. The quiet 13.71 s run of the
old image (kernel.dll 0.86 s) was the outlier, not the rule.

`TLB_REFILL=1` (Makefile, default): a UTLB miss taken in the code cache
is refilled by the translator (rt.c `tlb_refill`) with the walk of CE6's
`TLBMissHandler` (shexcept.src): page directory from TTB, or
`g_pprcNK->ppdir` for addresses from the shared heap base up (user mode
there: reads only, PR write bit masked), page table, valid bit 0x100,
PTEA from PTEL bits 9 and 29-31; then the translator's ldtlb hook and the
faulting host instruction runs again (`RT_ACT_RESUME`). It applies only
while VBR + 0x400 holds that handler word for word (57 words up to its
`bra TLBMissError`, checked once per VBR; shared-heap base and &g_pprcNK
come from its literal pool; both measured images match). Anything else
leaves the state untouched and delivers the exception as before: a
failing walk (the guest then reaches TLBMissError itself), a directory or
table outside P1/P2, a fault outside the code cache or during nested
exception handling, and a new mapping that would drop the faulting block.

Before, each miss was an exception into C, the guest handler in
translated code with TEA/TTB/EXPEVT/PTEA/PTEL through the C fallback, an
ldtlb hook and an rte in C. Counters over the load (old image): 26234
refills; device-access fallbacks 236718 -> 100442, rte 69031 -> 41188.
New guest `tlbrefill`: CE's handler at its VBR + 0x400 with its own page
directories and fake g_pprcNK; process-directory read, write miss,
shared-heap lookup and a missing valid bit (TLBMissError path), with and
without TLB_REFILL (the handler as translated guest code gives the same
results).

| 8 MiB image, mean of taps 7.5M/8.0M/8.5M | mean | runs |
| --- | ---: | --- |
| before (HASH_TAG=1 commit) | 15.31 s | 14.81 15.51 15.60 |
| TLB_REFILL=1 | 13.56 s | 13.51 13.78 13.40 |

### Common helper calls without C (2026-11-07)

`FAST_HELPERS=1` (Makefile, default) turns on four paths that stay in
assembly or generated code:

- `DBT_INLINE_DEVICE_READ`: a control-register load in device-aware code
  runs natively and unswapped in the block (reads have no side effects);
  stores keep the C helper (PTEH changes the ASID, MMUCR.TI flushes).
- `DBT_FAST_LDC_SR` + `_MD`: `ldc sr` with unchanged register bank and
  ASID loads the new SR in the stub, updating the shadowed guest MD there
  too. The bank guest now checks for the MD variant (HC_GETSTAT 63 bit 1)
  that both privilege transitions stay on the fast path.
- `DBT_FAST_LDTLB`: ldtlb with unchanged ASID on a 4 KiB page without
  translated user code, in the stub.
- `DBT_FAST_RTE` (new): the rte stub checks that the hardware ASID is the
  one the translator tracks and probes the lookup hash for SPC (the same
  index, ASID mix and tag check as the lookup stubs); on a hit it sets the
  shadow MD from SSR, forces MD|DSP into SSR, puts the host address in SPC
  and rtes. The SR it needs for the C path is parked in `dbt_rte_sr_save`,
  so any miss continues into the unchanged C path. HC_GETSTAT 432 counts
  the fast returns (the tmu guest takes it 4317 times).

`DBT_DYNAMIC_P4_STORE` (native control-register stores except PTEH and
MMUCR) was measured too and gave nothing (13.37 -> 13.49 s), with longer
blocks and no bounded retry under second chance, so it stays off.

Load counters on the old image (TLB_REFILL on in both): `ldc sr` through
C 309362 -> 53179, rte through C 41188 -> 10179 (29493 fast), C fallback
calls 100442 -> 60874 (the remaining 58025 device accesses are stores).

| 8 MiB image, mean of taps 7.5M/8.0M/8.5M | mean | runs |
| --- | ---: | --- |
| TLB_REFILL commit | 13.56 s | 13.51 13.78 13.40 |
| + inline device reads | 13.37 s | 13.54 13.56 13.00 |
| + native P4 stores (not kept) | 13.49 s | 13.46 13.51 13.50 |
| reads + stores + fast ldc sr (MD) | 12.92 s | 12.98 13.15 12.65 |
| ... + fast ldtlb | 12.71 s | 12.96 13.03 12.13 |
| FAST_HELPERS=1 (reads, ldc sr, ldtlb, rte; no stores) | 12.61 s | 12.78 12.51 12.55 |

### Layout by what fills a bank (2026-11-07)

`-DDBT_HASH_DIAG` now also counts why a bank is full when translation
moves on (headers, code space or metadata) and what the full bank held.
My Device load, 4 MiB / 14400 / 2624 KiB: 34 banks filled, 33 by their
1200 headers and 1 by metadata; a full bank held on average 192 KiB of
its 341 KiB code space (56%) and 180 KiB of its 218 KiB metadata. A block
averages ~160 bytes of host code and ~150 bytes of metadata. So a quarter
of the code cache was idle while blocks were evicted for want of headers.

Layouts within the same 8 MiB (tap-averaged on the 2026-11-06 image, all
exact frame matches):

| code cache / headers / arena / hash entries | mean | runs |
| --- | ---: | --- |
| 4 MiB / 14400 / 2624 KiB / 8192 (ae8e567) | 12.61 s | 12.78 12.51 12.55 |
| 4 MiB / 16800 / 2624 KiB / 8192 | 12.59 s | 12.96 12.43 12.38 |
| 3.5 MiB / 18000 / 2720 KiB / 8192 | 12.11 s | 11.91 12.16 12.25 |
| 3 MiB / 18000 / 2720 KiB / 8192 | 11.73 s | 11.99 11.78 11.40 |
| 3.25 MiB / 19200 / 2900 KiB / 8192 | 11.19 s | 11.23 10.86 11.47 |
| 3 MiB / 19200 / 2900 KiB / 8192 | 10.98 s | 10.48 10.93 11.53 |
| 3.25 MiB / 20400 / 3100 KiB / 8192 | 10.86 s | 10.84 10.71 11.03 |
| 3 MiB / 20400 / 3100 KiB / 16384 | 10.70 s | 10.28 11.01 10.82 |
| 3.25 MiB / 19200 / 2900 KiB / 16384 | 10.51 s | 10.64 10.55 10.34 |
| 3.25 MiB / 20400 / 3000 KiB / 16384 | 10.48 s | 10.83 10.36 10.25 |
| **3.25 MiB / 19200 / 2900 KiB / 32768** | **10.21 s** | 9.94 10.26 10.42 |

The last one is the default since 2026-11-07 (Makefile, rt/main.c,
docs/05); at taps 9.0M/9.5M/10.0M it gives 10.41 10.38 9.76, so 10.20 s
over six. The image ends at 0x8CC970D0, 36 KiB below its 128 KiB stack
(rt/image.ld asserts the bound); a 65536-entry hash no longer fits.

Tried and dropped: a dense copy of every inline cache's target (a pool
word after the IC pair pointing into the copy, written by the stub's
refill, the C refill, resets and survivor moves), so that the
invalidation pass and the recycle scans stop reading ICs from code
memory. Over six tap ticks it was slower, 10.49 s against 10.20 s.

### User inline caches refilled from the hash (2026-11-09)

Found while profiling Plotter-Z on the CE 5.0 image (docs/08): a stylus
drag spent 33-41% of its heartbeat samples in `_dbt_lki_ts`, the hash
probe of the IC miss stub. Cause: with `DBT_USER_IC_HASH_HIT` a user
target found in the hash was jumped to without filling the site's IC
(filling it needs the block listed in `g_ic_active`, which was C's job),
and `switch_asid` empties the user ICs. So a user IC was filled once, by
C, when its target was first translated, and after the next address-space
switch never again: every `jsr @Rn` and `rts` of user code took the stub's
probe, about 45 host instructions. CE switches address space on every
system call, so this was the steady state of all user code.

`USER_IC_REFILL=1` (default, `-DDBT_USER_IC_REFILL`):

- the translator puts a third word after the IC pair (`TAG_IC_I`,
  `emit_lit_only`: a pool word no instruction loads); the runtime writes
  the block's index there at commit and when a survivor moves
  (`set_ic_index`);
- the stub, on a user hash hit, masks interrupts, parks the host address
  in the IC (pc still 1), lists the block in `g_ic_active` /
  `g_ic_active_seen` unless it is there already, publishes the pc and
  returns through the usual `ldc`; kernel targets keep the old unmasked
  refill.

The stubs' IL memory had 8 bytes left. The four literal pools repeated
every literal; those only one region uses are now only in that pool
(`POOL`), which freed 96 bytes; the section is at 4072 of 4096.

Measured on the 2026-11-09 CE 5.0 image, emulated instructions:

| | before | after |
|---|---:|---:|
| Plotter-Z `Redraw` to `Redraw` while dragging (old PlotterZ.exe, 150k calls per frame) | 37.4M | 17.7M |
| stub share of the drag's samples | 42-51% | 2% |
| Saddle Surface, Render tap to the final `Redraw` | 218M | 190M |
| boot to "Explorer taskbar thread started" | 874M | 852M |

All guests pass with the same output as `USER_IC_REFILL=0` (bench, tmu,
lcd and flush differ in their timing figures only); `make test` runs
`test_translate` a second time with the define.

Not done, estimated from the same run: a recalculation still spends about
60% in FPU emulation. Of the ~250k FPU instructions that reach the helper
per recalculation, about 70% are `fmov.s` to or from memory (the CE
compiler's loads, stores and register saves), each a full helper call of
about 350 instructions for one swapped 32-bit move. Translating those in
line means driving `tr_mem` from `tr_fpu_inline` with a borrowed register;
the slots and the fault recovery of that combination have not been worked
out.

### Metadata on demand by default, and the layout it allows (2026-11-10)

**Who needs a block's recovery metadata.** Counters in a scratch copy of
rt.c (one bit set per block life and cause; boot of the 2026-11-10 CE 5.0
image, My Device opened, Start > Programs, casio-emu fx-cp400):

| | block lives | share of 58.6k translations |
| --- | ---: | ---: |
| needed its metadata at all | 1638 | 2.8% |
| ... because of an interrupt (in the block or in a helper stub it called) | 1596 | |
| ... because of anything synchronous (a TLB miss the guest gets, a helper fault, FPU disable) | 61 | 0.1% |

The 38% of 2026-11-06 ("Metadata on demand, second chance") dates from
before the translator refilled the TLB itself: 15.7k misses of a boot are
now refilled without looking at the block. The first count of this round
said 9-11% and was wrong in the same direction: it included interrupts
taken at a block's entry.

**Block entries need no metadata.** An interrupt usually arrives while the
translator runs masked (a lookup in C, a translation) and is taken in the
stub tail that is about to jump to the block's first instruction, or at
that instruction. `guest_pc_of_host` and the in-block path of `exc_c_body`
now answer those from the header (`host == host_base`: the guest pc is the
block's, nothing is spilled) instead of going through `resume_pc`, which
under `LAZY_META` rebuilt the metadata for it. Rebuilds over the scenario
above: 6536 before, 1748 with this.

**An interrupt in a dead block's redirect was resumed from the dead
block's metadata** (a bug since the lazy detach of 2026-11-09, found
because `LAZY_META` halted on it: "metadata rebuild diverged", twice in
seven boots). `invalidate_block` overwrites the entry of a dead block with
eight instructions that take stale links, ICs and hash entries to the
lookup stub. `block_of_host` still finds the dead header for a host pc in
there, and the exception path then applied the block's own metadata to
offsets 2-12 of code that is no longer the block: the guest resumed up to
six instructions into the block without having run them, with r1 holding
the redirect's scratch value. Such an exception now resumes at the block's
guest pc with r1 taken back from Y0 (`DEAD_REDIRECT_CODE`), and a rebuild
for a dead block does not compare the redirect's 28 bytes. No guest covers
this; seven boots after the fix had no halt.

**`LAZY_META=1` is the default**, and `lazy_eligible` accepts the EXT
region as well as NK (Math Xpander, SpreadCE and WordPad run from there;
they would otherwise keep ~150 bytes of metadata per block in an arena now
sized for ~46).

**Layout.** With metadata on demand a block's arena share is its links and
relocation record: 34-36 bytes when a bank fills, against ~150. Within the
same 8 MiB:

| | until 2026-11-10 | now |
| --- | ---: | ---: |
| code cache | 3.25 MiB | 4.0625 MiB at 0x8CBF0000 |
| block headers | 19200 | 25680 |
| metadata arena | 2900 KiB | 1200 KiB |
| lookup hash entries | 32768 | 65536 |

Per block: ~165 bytes of code, ~46 of arena, 63 of header and side tables.
The hash was doubled rather than left at 32768 for a few more headers:
with 25680 blocks in 32768 direct-mapped slots about a third of the blocks
would be out of the hash at any time, against a sixth now (a quarter with
the old layout). `print_stats` now always says what filled the banks. Two
boots through the scenario: 24-25 banks filled, 14-15 by headers and 9-10
by code (both can hold), none by the arena; a full bank held 2102 of 2140
headers, 336 of 347 KiB of code and 75 of 100 KiB of arena. The scenario
took 13 bank recycles and 44-48k translations (stats every 4000), against
33 and 57-59k with the old layout. Rebuilds 1550-1570 (overflow buffer
19-49 times), 1000 blocks resident by the hint.

No time was measured, on the user's instruction: the figures above are
counts. The image ends near 0x8CBC9C00, 25 KiB below the stack's reserve; `make
DEVICE=1 hhk` links. All guests pass as before (`interior` fails the same
two reuse checks with and without the change); `flush` translates more
before the cache is full and needs more than the default 20 s of emulator
time when other emulators run beside it.

**What this costs.** Code that is not in a ROM region keeps its metadata
(RAM can change under a block). The arena now holds about 670 such blocks
per bank where it held 1600, so a program loaded into RAM from a storage
card or the object store gets about 8000 blocks, not 19200. Nothing in the
image runs from RAM today. `LAZY_META=0` with the old layout (the Makefile
comment has the values) is the way back; letting metadata and code share a
bank's memory would remove the fixed split.

**Deferred interrupts (prototype, not adopted).** The other consumer of
metadata, an interrupt inside a block, can be delivered at the block's
exit instead: mask interrupts (SSR.IMASK = 15), unchain the block's links,
empty its IC, point its pool literals for `dbt_lookup_link` and
`dbt_lookup_ic` at a `trapa`, and deliver the interrupt from that trap
with the guest state canonical. Blocks that read or write SR, `sleep` or
`trapa` are not deferred, nor is an interrupt inside a link's patch window
or the IC sequence. It passes all guests (the first version, which
unmasked at the exit and waited for the request to fire again, failed
seven tmu loops: the interrupt was not taken there and was deferred again
and again) and boots CE; rebuilds fell from 1748 to 685. It is not in the
tree: the remaining gain is small, it adds 8 bytes to the IL memory
(4080 of 4096) for reloading SPC and SSR on the resume path, and in two
boots one tap each got 2 and 10 touch samples where 22-37 is usual, which
was not explained. The patch is `notes/defer-irq-prototype.patch`
(against d0513e3, with the census counters).

**Test recipe.** The double tap that opens My Device, `35,40,700000,3000;
35,40,706000,3000`, only counted as a double click when the first tap was
processed late: it usually opened the folder with the old layout and
never in five boots with the new one, although the touch driver reported
both taps. `35,40,700000,1200;35,40,702400,1200` opened it in two of two.
