# Same-ISA Dynamic Binary Translator (`dbt/`)

`dbt/` is a bare-metal dynamic binary translator that runs **little-endian SH-4** Windows CE on Casio's **big-endian SH7305 (`SH4AL-DSP`)** without hardware modification. Because guest and host share the SuperH instruction set, integer ALU instructions execute natively while the translator rewrites only what differs: byte-swapped memory accesses, software FPU operations, control flow, and a small set of privileged instructions.

---

## 1. Core Execution Model

### Byte-Identical Little-Endian Memory
Guest RAM and flash hold the exact byte layout a real little-endian SH-4 sees (so `NK.bin`, `EXT.bin`, PE loaders, and UTF-16 strings work unmodified):

| Guest Access | Translated Host Sequence |
|---|---|
| `mov.b` (all forms) | Unchanged (1 host instruction) |
| `mov.w @X, Rn` | Load → `swap.b Rn, Rn` → `exts.w Rn, Rn` |
| `mov.w Rm, @X` | `swap.b Rm, rX` → store `rX` |
| `mov.l @X, Rn` | Load → `swap.b` → `swap.w` → `swap.b` |
| `mov.l Rm, @X` | `swap.b`/`swap.w`/`swap.b` into `rX` → store `rX` *(reuses `rX` across consecutive stores of the same `Rm`)* |
| PC-relative `mov.w`/`mov.l`, `mova` | Resolved at translation time against guest PC and guest literal pool |
| `sts.l`/`lds.l`, `stc.l`/`ldc.l` | Expanded to register move + swapped 32-bit memory access |
| `mac.w`/`mac.l`, rare edge cases | Dispatched to single-instruction C fallback (`dbt_fallback_stub`) |

### Scratch Registers & Borrowed Register
The translator keeps `SR.DSP = 1` and uses the SH4AL-DSP transfer registers **`A0, X0, X1, Y0, Y1`** as a 5-slot, 1-cycle spill file (`lds`/`sts`). SH-4 code never touches them.
- **Block-scoped borrowed register:** A pre-pass picks the lowest unused GPR (`r1`–`r14`) in the block and parks it in `A0` once at block entry (`lds rX, A0`), restoring it at block exits and pausing the borrow around helper calls. Stores and device-address checks use `rX` without per-instruction spills, and consecutive stores of the same register reuse the already-swapped value in `rX`.

### Branches, Delay Slots & Block Linking
- A basic block ends after the first branch (including its delay slot), `rte`, `trapa`, `sleep`, `ldc Rm, sr`, or at 64 guest instructions (or earlier if code/spill/meta/literal budgets approach their limits).
- **Direct branches (`bra`, `bsr`, `bt`, `bf`, `bt/s`, `bf/s`):** Lazily patched to jump directly to the target host block once translated (**block chaining**). For `bt/s`/`bf/s`, the delay slot executes first; if the slot clobbers `T`, `T` is saved via `movt`.
- **Indirect branches (`jmp`, `jsr`, `rts`, `braf`, `bsrf`):** Use per-site **inline caches (IC)** storing `(guest_pc, host_pc)`. User-space IC misses are refilled directly from the ASID-tagged lookup hash in assembly (`DBT_USER_IC_HASH_HIT` + `DBT_USER_IC_REFILL`) before falling back to C.
- **Delay-slot isolation:** Indirect targets are captured into `X0` before executing the slot (with shifted scratch slots `X0→X1→Y1→A0`), preserving `jmp @r0 ; mov r1, r0` semantics.

---

## 2. FPU Translation & Block Budgets

- **Inline FPU (`tr_fpu_inline`):** Register moves (`fmov`), single-precision memory forms (`fmov.s @rm, frn`, `@rm+`, `@(r0,rm)`, `frm, @rn`, `@-rn`, `@(r0,rn)`), `fldi0`/`fldi1`, `fabs`, `fneg`, `flds`/`fsts`, and `lds`/`sts fpul` are emitted inline directly against `g_fpu` after fast `SR.FD` and `FPSCR.SZ` checks.
- **Fast arithmetic stub (`dbt_fpu_arith_stub`):** Common single/double arithmetic and comparisons enter a lightweight assembly stub rather than full exception-style dispatch.
- **Full fallback (`dbt_fpu_stub` → `fpuemu_exec`):** Handles `SZ=1` pair moves, `FPSCR` mode changes, and exception-raising cases.
- **Lazy FPU switching (`SR.FD`):** Whenever guest `SR.FD = 1`, FPU instructions synthesize `EXPEVT 0x800` (or `0x820` in a delay slot) so Windows CE's `SwitchFPUOwner` swaps thread FPU state unchanged.
- **Dense FPU block overflow protection (`translate.c`, `sh4emit.h`):** `EMIT_MAX_LITS = 128` and per-instruction budget guards on `spill_cnt`, `meta_cnt`, `nlits`, and emitted code bytes ensure FPU-dense loops (such as Plotter-Z's expression evaluator) split cleanly into multiple blocks instead of failing translation with `DBT_ERR_OVERFLOW`.

---

## 3. Privileged Instructions, MMU & Exceptions

| Guest Instruction | Handling |
|---|---|
| `ldc`/`stc vbr` | Stored in translator shadow VBR; real `VBR` stays pointed at `dbt_vectors` in IL memory (`0xE5200000`). |
| `ldc Rm, sr` / `stc sr, Rn` | Fast assembly stub (`DBT_FAST_LDC_SR`, `DBT_FAST_LDC_SR_MD`) keeps host `MD=1, DSP=1`, tracks guest `MD` in `dbt_shadow_md`, and applies `BL`/`RB`/`IMASK`/`T`. |
| `rte` | Fast stub (`DBT_FAST_RTE`) resolves guest `SPC` via the lookup hash, forces `SSR.MD=1, DSP=1`, updates `dbt_shadow_md`, and executes native `rte` (with a `nop` slot because SH-4 switches register banks before the delay slot). |
| `ldtlb` | Fast stub (`DBT_FAST_LDTLB`) executes native `ldtlb` and invalidates only blocks whose page was remapped. |
| `trapa #0xF0`–`#0xFF` | Intercepted as translator hypercalls (flush, debug print, TLB inval, LCD push, R-standby, stats). |

### Precise Exception Recovery & Lazy Metadata
- **Bank 1 preservation:** Windows CE stores live kernel pointers in `r4/r5/r7_bank1`. `dbt_vectors` saves bank 1 first (`ctx->b1`) before bank 0, DSP slots, and control registers.
- **Commit points:** Side-effecting memory ops (`@Rm+`, `@-Rn`, helper calls) record a commit offset. An exception before commit restarts the guest instruction (undoing open spills); an exception after commit finishes the remaining byte-swap/control-register tail in software and resumes at the next guest instruction.
- **Metadata on demand (`DBT_LAZY_META`, default on):** Profiling showed fewer than 3% of translated ROM blocks ever need recovery metadata during their lifetime. ROM blocks (both `NK` and `EXT`) are therefore translated without storing per-instruction metadata in the arena; if an exception lands mid-block, `ensure_meta()` re-decodes that single block on demand. Exceptions at block entry (`host == host_base`) resolve directly from the header with zero rebuild.

### Selective Device-Aware Translation (`kdev`) & Fast TLB Refill
- **Selective P4 (`DBT_KDEV`):** Only kernel functions that access SH-4 CCN registers (`0xFF0000xx`) receive runtime unswapped P4 address tests (`DBT_SHORT_DEVICE_TEST`). `tools/kernel_devfuncs.py` generates per-image signature tables (`kernel_dev50.h` for CE 5.0 `nk.exe`, `kernel_dev.h` for CE 6.0 `kernel.dll`) with P1 address hints for instant startup verification; `kdev_locate()` falls back to scanning or whole-module device-aware mode if a kernel is relinked.
- **Translator-side TLB refill (`DBT_TLB_REFILL`):** Recognizes the exact bytes of CE 5.0's (and CE 6.0's) `TLBMissHandler` at boot and walks the guest page table (`SectionTable → SECTION → MEMBLOCK → aPages`) directly inside the translator's `0x400` handler, checking access locks (`alk`/`aky`) before issuing `ldtlb` and retrying the faulting instruction.
- **Page-table revalidation (`tlb_reval5`):** On whole-TLB invalidations (`MMUCR.TI`, rangeless `HC_TLB_INVAL`, or R-standby resume), instead of discarding all user-space blocks, the translator re-walks CE 5.0's page tables for live user blocks and drops only blocks whose physical mapping actually changed.

---

## 4. Code Cache, Layout & On-Chip Memories

Defined in [`dbt/Makefile`](../dbt/Makefile) and [`dbt/rt/image.ld`](../dbt/rt/image.ld) (8 MiB translator + code cache at `0x8C800000`–`0x8CFFFFFF`):

| Resource | Configuration | Notes |
|---|---|---|
| **Code cache** | **4.0 MiB** (`0x8CC00000`–`0x8CFFFFFF`), **16 banks** | 256 KiB per bank; second-chance eviction (`DBT_SECOND_CHANCE`) promotes hot blocks before recycling a bank |
| **Block headers** | **28,800** headers | Indexed by physical page groups (`DBT_PAGE_GROUP_FILTER`) and link-bank masks (`DBT_PRUNE_LINK_BANKS`) |
| **Metadata arena** | **1,280 KiB** | Compacted by `DBT_LAZY_META` (~35 B/ROM block) + literal deduplication (`DBT_DEDUP_LITERALS`) |
| **Lookup hash** | **65,536 entries** | Direct-mapped, ASID-tagged (`DBT_HASH_ASID_TAG`, `DBT_HASH_ASID_MIX`) with chaining on collision |
| **On-chip IL memory** | 16 KiB at `0xE5200000` | Holds `dbt_vectors` and hot assembly stubs (reloaded across R-standby) |
| **On-chip X/Y RAM** | 16 KiB at `0xE5007000`/`0xE5017000` | Holds hot runtime state (saved/restored across R-standby) |
| **Build optimization** | `-O2 -flto` | Link-time optimization across `rt/` + `src/` + `fpuemu/` |

---

## 5. Native LCD Push (`HC_LCD_PUSH` / `DMAC0`)

To eliminate translated-store overhead during display updates, `classpad_ddi.cpp` passes dirty rectangles (`PushRect`) to `HC_LCD_PUSH` in `dbt/rt/rt.c`:
- Issues DCS `CASET` (`0x2A`), `PASET` (`0x2B`), and `RAMWR` (`0x2C`) with `SYNCO`-ordered `PRDR` toggling (`dbt/rt/lcd_io.h`) and automatic older-panel column offset (`col0 = 40` vs `0`).
- Converts little-endian RGB565 guest rows (including 90°/180°/270° rotated transposes) into double-buffered 32-byte-aligned staging buffers (`g_lcd_dma[2][640]`) using native `swap.b`/`swap.w` pairs, flushing lines with `ocbwb` + `synco`.
- Streams each completed buffer to CS5A (`0x14000000`) via **SH7305 DMAC0** (32-byte bursts `TS=4` when 32-byte aligned, 2-byte transfers `TS=1` otherwise) with bounded completion waits and `DMAOR` error checks while the CPU prepares the next buffer.

---

## 6. Performance Highlights & Rejected Experiments

### Key Measured Wins
- **Baseline microbenchmarks (`GUEST=bench`):** ALU `1.0×`, branches `1.3×`, memory `2.3–2.8×`, calls `2.8–3.8×` vs. native SH-4.
- **CE 5.0 boot to Explorer taskbar (`casio-emu` wall clock):** Dropped from ~20–25 min (early translator) → 129 s (second-chance cache + ASID hash + fast helpers) → 84 s (+ CE 5.0 `kdev` + `TLB_REFILL` + `tlb_reval5`).
- **Inline `fmov.s` memory forms:** Cut Plotter-Z Ackley Function recalculation instruction count by **24%** (208M → 158M emulated instructions).
- **Lazy metadata + 65k hash layout:** Cut bank recycles by >60% (33 → 13) and total translations by ~22% over boot + Explorer usage.

### Approaches Tried and Not Adopted
- **Calls through a literal pool as direct links:** Added complexity with negligible gain over existing inline caches.
- **Deferred in-block interrupts:** Reduced metadata rebuilds, but consumed tight IL memory space and introduced unexplained touch-sampling anomalies.
- **XOR-address endianness model (`addr ^ 3`):** Avoids 32-bit swaps but penalizes 8-bit and 16-bit accesses heavily; rejected because Windows CE is UTF-16 and 16-bpp RGB565 everywhere.

## 7. Testing

- **Host unit tests (`make test`):** `dbt/tests/test_translate.c` tests decoding, byte-swap sequences, delay slots, commit points, inline FPU recovery, IC refill, and dense FPU block splitting.
- **Emulator guest suite (`make -C dbt GUEST=<name> run`):** `hello`, `exc`, `fpu`, `fpudis`, `tmu`, `bench`, `lcd`, `apicall`, `mmu`, `aspace`, `bank`, `devmem`, `tlbdata`, `flush`, `nkimg`, `nkxip`, `extrom`, `standby`, `usbfn`.
