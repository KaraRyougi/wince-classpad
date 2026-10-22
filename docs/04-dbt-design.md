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

The delay slot of a rewritten branch is emitted before the jump sequence; if
the branch target register is modified by the slot (`jmp @r0` with `mov r1,r0`
in the slot) the target is copied to a scratch first.

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

1. Read SPC (a host address). Find the block containing it and the guest
   instruction it belongs to. Undo any pending spill for that instruction.
2. Set SPC = guest PC of that instruction, SSR.MD = shadow MD. EXPEVT/INTEVT/
   TEA/TRA are real hardware registers that the guest reads with the
   pre-swapped MMIO macros.
3. Hypercall (`trapa` with TRA 0xF0-0xFF): handle in C and return.
4. Jump to the translation of guest VBR + offset.

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
