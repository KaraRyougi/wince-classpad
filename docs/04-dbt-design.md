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

Reserve the top of RAM for the translator: code cache, tables and stack, about
4 MiB. Guest RAM is told to end below it (`MainMemoryEndAddress`). On 16 MiB
units (fx-CP400, fx-CG100) that leaves 12 MiB for CE; 8 MiB units run headless
only.

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
- Helpers (`dbt_fpu_stub`, `dbt_fallback_stub`, `dbt_ldc_sr_stub`): X0 =
  argument, X1 = return address. They save the full context, mask interrupts,
  call C, restore and jump back through the shared `dbt_tail_full`.
- `dbt_rte_stub`: resolves the guest SPC, forces MD and DSP in SSR, keeps the
  guest MD in `dbt_shadow_md`, then executes the real `rte`.
- Exceptions: VBR = `dbt_vectors`; handlers save bank-0 registers (via
  `stc Rn_bank`, since RB=1), the DSP slots, SPC/SSR/EXPEVT/TRA/TEA/INTEVT into
  `dbt_ctx_exc` and call `dbt_exc_c`. Hypercalls (`trapa` 0xF0-0xFF) return
  with `rte`; everything else is delivered to the guest handler with
  SPC = guest resume pc, SSR.MD = shadow MD. Assumption to verify against the
  CE kernel source: bank-1 registers hold no persistent kernel state.
- Resume rules: translated code uses the block metadata (restart the
  instruction, or the branch target after a delay slot). Inside the stubs,
  window labels (`dbt_lk_s*`, `dbt_hit_*`, `dbt_full_m*`, `dbt_h*_s*`) tell
  the handler which of r0-r3 are still parked in the DSP slots.
- Known v1 limits: FPU memory operands and guest code must be in unmapped
  (P1) memory; a TLB miss inside C code halts unless it is the translator's
  own code probe. Guest code running with SR.RB=1 must not take exceptions
  or make hypercalls (the vectors save r0-r7 through `stc Rn_bank`, i.e.
  bank 0): true for hardware-faithful handlers, which keep BL=1 while RB=1,
  but to be checked against the CE kernel's exception entry.

## Performance (casio-emu, 2026-10-22)

`make -C dbt GUEST=bench run`: the kernels in `bench/loops.S` are assembled
big-endian into the translator image and little-endian into the guest; the
guest times its (translated) run with TMU1 and asks the runtime through
`HC_BENCH` to time the same kernel natively. 20000 iterations each,
interrupts masked, blocks translated by a warm-up call first. casio-emu
charges one TMU tick per four executed instructions regardless of type, so
these are *instruction-count* ratios; the per-iteration counts below were
read from the translated blocks and agree with the measurements.

| kernel | per iteration | native ticks | baseline | + branch chaining | + inline caches | host insns / guest insns (now) |
|---|---|---|---|---|---|---|
| alu | 5 ALU + `dt` + `bf` | 15382 | 27723 (1.80x) | 15390 (**1.00x**) | 15390 (**1.00x**) | 7 / 7 |
| mem mixed | 4 longword, 2 word, 2 byte accesses, 2 ALU, loop | 26369 | 81308 (3.08x) | 72519 (2.75x) | 72519 (2.75x) | 37 / 12 |
| mem copy | 2 post-increment loads, 2 stores, pointer wrap, loop | 29391 | 81033 (2.76x) | 68400 (2.33x) | 68399 (2.33x) | |
| stack | push r1, r2, PR; pop them; loop | 19777 | 90096 (4.56x) | 81309 (4.11x) | 81307 (4.11x) | 37 / 9 |
| branch | 3 data-dependent `bt`/`bf` (half taken), loop | 25270 | 47249 (1.87x) | 31869 (1.26x) | 33516 (1.33x) | |
| call jsr | `jsr @r6; nop` to a 3-instruction leaf | 17580 | 156016 (8.87x) | 156017 (8.87x) | 76914 (**4.38x**) | 35 / 8 |
| call bsr | `bsr leaf; nop` | 17580 | 105479 (6.00x) | 96690 (5.50x) | 57139 (**3.25x**) | |
| call frame | `jsr` to a function that pushes/pops r8, r9, PR | 35159 | 235117 (6.69x) | 235119 (6.69x) | 156016 (**4.44x**) | |

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
3. **Cheaper PR writes**: `jsr`/`bsr` spend 4 instructions materializing the
   guest return address; a return-address side table indexed by call site
   could remove the literal load from the hot path.
4. **Spill avoidance for stores** when a dead register is provably available
   (needs liveness across blocks; only after the above).

## Test platform

casio-emu implements only `lds rm, x0` of the DSP transfers; the patch in
`dbt/emu-patch/add_dsp_transfers.py` adds the other nine (applied to the local
casio-emu checkout on 2026-10-22; rerun it after updating the emulator, then
`node scripts/genDispatch.mjs && cmake --build build`). The emulator starts a
raw `.g3a` payload at 0x00300000; `dbt/loader` copies the translator image to
0x8CB00000 and jumps. `make -C dbt run` builds everything and runs it headless;
`GUEST=exc` / `GUEST=fpu` select the other guests.

Status 2026-10-22: all four guests pass under the emulator.

| Guest | What it covers | Result |
|---|---|---|
| `hello` | every access size and addressing mode, literal pool, stack, struct layout | PASS (8 checks) |
| `exc` | guest VBR, `trapa`, EXPEVT/TRA, SPC rewrite by the handler, `rte`, T bit across exceptions | PASS (6 checks) |
| `fpu` | raw FPU words through the helper: fpscr reset, fadd/fmul/float/fmov.s, double sub + fcmp/gt | PASS (5 checks, 0x21 helper calls) |
| `bench` | translated vs native timing of eight kernels (see "Performance") | runs |
| `tmu` | TMU0 programmed through pre-swapped MMIO, IMASK/BL and IMR masking, INTEVT, `rte` restoring SR, `sleep` wake-up; register probe across `trapa`; 15 one-construct hot loops (loads, stores of every form, `bt/s` with a T-writing slot, `jsr` with a store in the slot, push/pop, pc-relative) plus a compiled loop, each run with ~3000 interrupts landing at arbitrary host instructions and compared with an interrupt-free run | PASS (34 checks) |

The TMU guest found three resume bugs that the host tests could not: `bf/s`
boundaries had the T sense inverted, loads were restarted after they had
executed (now the commit-point metadata), and the `rte` stub restored r2 in
the delay slot after the bank switch. `make -C dbt GUEST=tmu run` is the
regression test for anything touching the exception path; build the runtime
with `-DDBT_TRACE_EXC` to print the last 16 exception landings with the stats.

The guest blob carries file contents only, so guests zero their own `.bss`
(`guest/start.S`); a CE image does that itself.
