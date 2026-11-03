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
| `mov.w Rm,@X` | spill rT; `swap.b Rm,rT`; store rT; restore rT |
| `mov.l @X,Rn` | load; `swap.b`; `swap.w`; `swap.b` on Rn |
| `mov.l Rm,@X` | spill rT; `swap.b Rm,rT`; `swap.w rT,rT`; `swap.b rT,rT`; store rT; restore rT |
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

The real `rte` in the stubs has a `nop` slot: SH-4 switches the register
bank before the slot executes, so an `sts A0,r2` there writes the *guest's*
r2 (found the hard way with the TMU test).

If SPC is inside translator C code (translation in progress), the exception
can only be a TLB miss from the code probe; longjmp out and deliver as above.

## Memory layout on the device

Reserve the top of RAM for the translator: code cache (2 MiB), tables and
stacks. The tables are 2048 block headers (416 KiB), a 640 KiB arena holding
each block's resume metadata and spill records (about 240 bytes per block in
practice; the translator writes a fixed-size scratch block and the runtime
copies the used entries), the 64 KiB hash and ~60 KiB of code: 1.3 MiB in
all, so the translator needs about 3.5 MiB with the cache. Guest RAM is told
to end below it (`MainMemoryEndAddress`). On 16 MiB units (fx-CP400,
fx-CG100) that leaves about 12 MiB for CE; 8 MiB units run headless only.
The emulator layout (docs/05, defined in `dbt/Makefile`) gives the
translator 1.5 MiB plus the 2 MiB cache and the guest 11.5 MiB.

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
  The patch is skipped when the translation flushed the whole cache
  (`dbt_stat.flushes` changed under the lookup: 2048 blocks or 2 MiB
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
  5 debug-ring bytes, 6 device accesses), FC translate a range device-aware
  from now on (tests).
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
  instruction, or the branch target after a delay slot). Inside the stubs,
  window labels (`dbt_lk_s*`, `dbt_hit_*`, `dbt_full_m*`, `dbt_h*_s*`) tell
  the handler which of r0-r3 are still parked in the DSP slots.
- Known v1 limits: FPU memory operands and guest code must be in unmapped
  (P1) memory; a TLB miss inside C code halts unless it is the translator's
  own code probe. SR.FD is not emulated yet: FPU instructions run whatever
  FD says, so the CE kernel's lazy FPU context switch (EXPEVT 0x800) never
  triggers (notes/ce/kernel-sh-notes.md section 6). Device registers
  accessed by kernel.dll itself (CCN at 0xFF000000) are byte-swapped like
  memory; the plan is device-aware translation of kernel.dll's blocks
  (docs/05).

## Performance (casio-emu, 2026-10-22)

`make -C dbt GUEST=bench run`: the kernels in `bench/loops.S` are assembled
big-endian into the translator image and little-endian into the guest; the
guest times its (translated) run with TMU1 and asks the runtime through
`HC_BENCH` to time the same kernel natively. 20000 iterations each,
interrupts masked, blocks translated by a warm-up call first. casio-emu
charges one TMU tick per four executed instructions regardless of type, so
these are *instruction-count* ratios; the per-iteration counts below were
read from the translated blocks and agree with the measurements.

| kernel | per iteration | native ticks | baseline | + branch chaining | + inline caches | + cheaper calls | host insns / guest insns (now) |
|---|---|---|---|---|---|---|---|
| alu | 5 ALU + `dt` + `bf` | 15382 | 27723 (1.80x) | 15390 (**1.00x**) | 15390 | 15388 (**1.00x**) | 7 / 7 |
| mem mixed | 4 longword, 2 word, 2 byte accesses, 2 ALU, loop | 26369 | 81308 (3.08x) | 72519 (2.75x) | 72519 | 72518 (2.75x) | 37 / 12 |
| mem copy | 2 post-increment loads, 2 stores, pointer wrap, loop | 29391 | 81033 (2.76x) | 68400 (2.33x) | 68399 | 68398 (2.33x) | |
| stack | push r1, r2, PR; pop them; loop | 19777 | 90096 (4.56x) | 81309 (4.11x) | 81307 | 81307 (4.11x) | 37 / 9 |
| branch | 3 data-dependent `bt`/`bf` (half taken), loop | 25270 | 47249 (1.87x) | 31869 (1.26x) | 33516 | 33515 (1.33x) | |
| call jsr | `jsr @r6; nop` to a 3-instruction leaf | 17580 | 156016 (8.87x) | 156017 (8.87x) | 76914 (4.38x) | 54941 (**3.13x**) | 25 / 8 |
| call bsr | `bsr leaf; nop` | 17580 | 105479 (6.00x) | 96690 (5.50x) | 57139 (3.25x) | 48349 (**2.75x**) | |
| call frame | `jsr` to a function that pushes/pops r8, r9, PR | 35159 | 235117 (6.69x) | 235119 (6.69x) | 156016 (4.44x) | 134042 (**3.81x**) | |

**Branch chaining** (done 2026-10-22, `dbt_chain_direct`): when the runtime
links a direct jump and the target is within the 12-bit `bra` range, the
first word of the 4-word jump sequence becomes `bra target` and the second
a `nop` (form A: a taken branch costs 3 instead of 5). For a conditional
*back edge* within the 8-bit range the inverted skip word itself becomes the
real `bt/bf target` and the jump's first word a `bra` over the rest (form B:
loop branches cost 1, like native). Blocks are allocated in translation
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
its asm hash probe or, failing that, from C (`dbt_resolve_ic`). Both
literals start as {1, 0}, so the first execution always misses. A
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
- **Stores**: spill + 3 swaps + store + restore = 6 (byte stores 1); loads:
  4 (word loads 3). `sts.l pr,@-r15` is 7. SH-4 has no byte-swap
  instruction, so 3 swaps per longword is the floor; the spill/restore pair
  costs the other 2.
- FPU and `mac` go through C helpers (not measured here; hundreds of
  instructions each, as planned).

On hardware, native code stalls on memory and the translated extra work is
all register ALU, so memory-heavy ratios should come out a little lower than
here; the 3-5x code expansion will cost instruction-cache misses instead.

Optimizations, in order of payoff per effort:

1. ~~**Branch chaining**~~ done (above).
2. ~~**Inline caches for indirect branches**~~ done (above).
3. ~~**Cheaper PR writes**~~ done (above); `bsr` still pays 4 + the direct jump.
4. **Spill avoidance for stores** when a dead register is provably available
   (needs liveness across blocks; only after the above).

## Test platform

casio-emu implements only `lds rm, x0` of the DSP transfers; the patch in
`dbt/emu-patch/add_dsp_transfers.py` adds the other nine (applied to the local
casio-emu checkout on 2026-10-22; rerun it after updating the emulator, then
`node scripts/genDispatch.mjs && cmake --build build`). The emulator starts a
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
