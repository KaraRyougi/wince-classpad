# Software SH-4 FPU Emulation (`fpuemu/`)

The SH7305 (`SH4AL-DSP`) has no hardware floating-point unit, whereas Windows CE SH-4 binaries freely emit SH-4 FPU instructions (`0xFxxx`, plus `lds`/`sts` on `FPUL`/`FPSCR`). `fpuemu/` provides a portable, integer-only, endian-neutral SH-4 FPU implementation shared by the dynamic binary translator and host test suite.

## Architecture

### 1. IEEE-754 Soft-Float Core (`fpuemu/src/softfloat.c`)

Implements single- and double-precision IEEE-754 arithmetic using integer operations only, matching SH-4 hardware semantics:

- **Rounding modes:** Round-to-nearest-even (`RN`, `FPSCR.RM = 0`) and round-toward-zero (`RZ`, `FPSCR.RM = 1`).
- **Denormals:** Honours `FPSCR.DN` (flush-to-zero when `DN=1`, which is Windows CE's default `FPSCR = 0x00040001`; full software gradual underflow when `DN=0`).
- **SH-4 NaN & conversion rules:** Signaling NaN has the fraction MSB set; default quiet NaN is `0x7FBFFFFF` (single) / `0x7FF7FFFFFFFFFFFF` (double); `FTRC` saturates to SH-4 limits on overflow/NaN.
- **Fast paths:** Specialized integer fast paths for normal single/double add, sub, mul, div, sqrt, compare, and int↔float conversions keep common operations fast without branching into full exception logic.

### 2. SH-4 Instruction Executor (`fpuemu/src/fpuemu.c`, `fpuemu.h`)

- **Register file:** 32 single-precision words (`fr[32]`), banked via `FPSCR.FR` (primary bank `FR0`–`FR15`, alternate bank `XF0`–`XF15`), plus `FPUL` and `FPSCR`.
- **Modes:** `FPSCR.PR = 1` selects double-precision arithmetic (`DRn`); `FPSCR.SZ = 1` selects 64-bit pair moves (`DRn` for even, `XDn` for odd).
- **Comparisons & exceptions:** `FCMP/EQ` and `FCMP/GT` update guest `SR.T`. FPSCR cause/flag/enable bits are enforced: an enabled exception returns `FPU_EXCEPTION` without clobbering the destination register.
- **Fault safety:** Memory loads/stores execute through callbacks; on a page fault (`FPU_FAULT`), no architectural FPU state is mutated so the instruction cleanly retries after the kernel services the TLB miss.
- **Unimplemented vector ops:** `FIPR`, `FTRV`, `FSCA`, and `FSRRA` return `FPU_UNIMPL` (Microsoft's SH-4 compiler does not emit them without explicit intrinsics).

## Integration with the Translator (`dbt/`)

Under the dynamic binary translator, FPU instructions never incur hardware illegal-instruction exceptions (`EXPEVT 0x180`/`0x1A0`, ~500–800 cycles per trap). Instead:

1. **Inline translation (`dbt/src/translate.c`):** Frequent data-movement and simple ops (`fmov`, `fmov.s`, `fldi0`/`fldi1`, `fabs`, `fneg`, `flds`/`fsts`, `lds`/`sts fpul`) are translated inline directly against the `g_fpu` register file in RAM.
2. **Fast arithmetic stub (`dbt_fpu_arith_stub` in `dbt/rt/stubs.S`):** Common single/double arithmetic and compares enter a lightweight assembly stub rather than full context-save dispatch.
3. **Full fallback (`dbt_fpu_stub`):** Complex or mode-sensitive instructions call into `fpuemu_exec()`.
4. **Lazy FPU context switching (`SR.FD`):** Windows CE switches FPU ownership lazily by setting `SR.FD = 1` on context switch. Both inline FPU code and stubs check guest `SR.FD` and synthesize `EXPEVT 0x800` (or `0x820` in a delay slot, with `SPC` pointing at the branch). The kernel's `SwitchFPUOwner` then saves/restores the emulated register file using ordinary `fmov` instructions and clears `SR.FD` (verified by the `fpudis` guest test).

## Verification

`make test` builds and runs `fpuemu/tests/test_host.c`, which compares every arithmetic, conversion, and comparison operation bit-for-bit against the host CPU's IEEE-754 FPU across both rounding modes (tens of thousands of random, edge-case, subnormal, NaN, infinity, and catastrophic-cancellation inputs), plus instruction-level decoder tests with fault-injecting memory.
