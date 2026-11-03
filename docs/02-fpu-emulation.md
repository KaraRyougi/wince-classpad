# FPU emulation design

The SH4AL-DSP core has no FPU. Every SH-4 FPU instruction (0xFxxx except
0xFxxF, plus LDS/STS to FPUL/FPSCR) raises a **general illegal instruction**
exception (EXPEVT 0x180) or **slot illegal instruction** (0x1A0) when it sits
in a delay slot. CE's prebuilt SH4 binaries contain these instructions, so the
kernel's illegal-instruction path becomes an FPU emulator.

## Core (`fpuemu/`)

- `softfloat.c`: integer-only IEEE 754 single/double with SH-4 semantics:
  RN and RZ only, FPSCR.DN flush-to-zero, SH-4 NaN encoding (fraction MSB set
  = sNaN, default qNaN 0x7FBFFFFF / 0x7FF7FFFFFFFFFFFF), SH-4 saturation
  values for FTRC. With DN=0 it does full gradual underflow in software,
  which real SH-4 hardware cannot (it raises an FPU error instead).
- `fpuemu.c`: decoder/executor. Register file is 32 words; FPSCR.FR picks the
  bank. SZ=1 moves pairs (DRn when the register field is even, XDn when odd).
  PR=1 selects double arithmetic. FCMP writes SR.T. FPSCR cause/flag/enable
  are honoured: an enabled exception returns `FPU_EXCEPTION` with cause set and
  the result not written, so the trap handler can raise the CE FPU exception.
- Not emulated (return `FPU_UNIMPL`): FIPR, FTRV, FSCA, FSRRA. The Microsoft
  compiler does not emit them without intrinsics; the handler should log and
  kill the thread if they ever show up.
- Memory access goes through callbacks that may fail; on failure no state is
  changed so the instruction can be re-executed after the page is mapped.

`make test` compares every operation bit-for-bit with the host FPU in both
rounding modes (20 000 samples per op/mode, plus special values and
cancellation cases) and runs decoder tests with a fake memory.

## Under the translator (done, 2026-11-03)

The DBT route needs none of the kernel-side steps below: every FPU
instruction is already a helper call into `fpuemu`, and the kernel's lazy
FPU switch works unchanged because the translator emulates SR.FD (the
helper raises EXPEVT 0x800/0x820 at the instruction when FD is set; the
kernel's SwitchFPUOwner then moves the emulated register file with fmov
and clears FD). docs/04 "FPU disable". The section below is the plan for
a native big-endian kernel port, kept for reference.

## Kernel integration (the native-kernel route, not needed with the DBT)

1. In the SH4 kernel's exception dispatch, before the generic illegal
   instruction path, check `fpuemu_is_fpu_op(*SPC)`.
2. Per-thread `fpu_state` lives in the thread structure (CE already reserves
   an FPU save area for SH4; reuse it). No hardware FPU save/restore remains.
3. Not in a delay slot (EXPEVT 0x180): run `fpuemu_run` from SPC with a small
   budget (16 instructions), then set SPC to the first non-FPU instruction.
   Consecutive FPU instructions are batched under one exception.
4. In a delay slot (EXPEVT 0x1A0): SPC points at the branch. Emulate the slot
   instruction with `fpuemu_exec`, then emulate the branch itself (BRA, BSR,
   BRAF, BSRF, BT/S, BF/S, JMP, JSR, RTS, RTE) to compute the new PC and PR.
5. `FPU_FAULT`: run the kernel's normal data-abort/TLB-miss handling for the
   faulting address and re-execute; `FPU_EXCEPTION`: raise STATUS_FLOAT_*.
6. CE sets FPSCR per thread; the emulated FPSCR replaces the hardware one.
   `lds rm,fpscr` / `sts fpscr,rn` are emulated too.

## Cost model and tricks

A trap costs roughly 400-800 cycles at 118 MHz, mostly exception entry,
decode and return; the arithmetic itself is 20-30 % of it. Priorities:

1. Batch consecutive FPU instructions (done in the core).
2. Put the vectors and the hot path in IL memory (VBR = 0xE5200000) and the
   emulated register file / exception scratch in X/Y RAM.
3. Fast dispatch: decide "illegal instruction" before the full context save.
4. Overclock (CPG), which helps everything uniformly.
5. Offline binary rewriting of the ROM modules to replace FPU clusters with
   calls into the soft-float routines. The big win, a separate project.

The core DSP and the SPU cannot help: the bottleneck is the exception, not
the arithmetic, and neither has a 53-bit multiply or leading-zero count.
