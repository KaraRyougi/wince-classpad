/*
 * fpuemu.h - SH-4 FPU instruction emulator for an SH-4A core without FPU
 * (SH4AL-DSP, e.g. SH7305). Intended to run inside the kernel's illegal
 * instruction handler; this core is OS-agnostic and endian-neutral.
 *
 * The caller owns PC handling: on entry the trap handler knows the address
 * of the faulting instruction; after FPU_OK it advances PC by 2 (or, for an
 * instruction in a delay slot, emulates the branch too). See docs/02.
 */
#ifndef FPUEMU_H
#define FPUEMU_H

#include "softfloat.h"

#if defined(_MSC_VER)
#define FPUEMU_INLINE __inline
#elif defined(__GNUC__) || defined(__clang__)
#define FPUEMU_INLINE inline
#else
#define FPUEMU_INLINE
#endif

typedef struct {
    sf_u32 fpscr;    /* +0: single-insn @(0, st) on SH-4 */
    sf_u32 fpul;     /* +4: single-insn @(4, st) on SH-4 */
    sf_u32 fr[32];   /* +8: fr[0..15] = bank 0, fr[16..31] = bank 1. FPSCR.FR
                        selects which bank is FR0-FR15; the other is XF0-XF15. */
} fpu_state;

/* Memory callbacks return 0 on success, nonzero on fault (TLB miss, protection).
 * On a fault the emulator leaves all state untouched so the instruction can be
 * re-executed after the fault is resolved. */
typedef struct {
    int  (*read32)(void *ctx, sf_u32 addr, sf_u32 *val);
    int  (*write32)(void *ctx, sf_u32 addr, sf_u32 val);
    int  (*fetch16)(void *ctx, sf_u32 addr, sf_u32 *op);   /* fpuemu_run only */
    void *ctx;
} fpu_mem;

#define FPU_OK        0
#define FPU_NOT_FPU   1   /* opcode is not an FPU/FPUL/FPSCR instruction */
#define FPU_UNIMPL    2   /* FPU opcode we do not emulate (fipr/ftrv/fsca/fsrra, reserved) */
#define FPU_FAULT     3   /* memory callback reported a fault; no state changed */
#define FPU_EXCEPTION 4   /* an enabled FPU exception; FPSCR.cause set, result not written */

#define FPSCR_MASK        0x003FFFFFu
#define FPSCR_RM_MASK     0x00000003u
#define FPSCR_FLAG_SHIFT  2
#define FPSCR_ENABLE_SHIFT 7
#define FPSCR_CAUSE_SHIFT 12
#define FPSCR_CAUSE_MASK  (0x3Fu << FPSCR_CAUSE_SHIFT)
#define FPSCR_DN          (1u << 18)
#define FPSCR_PR          (1u << 19)
#define FPSCR_SZ          (1u << 20)
#define FPSCR_FR          (1u << 21)

/* SH-4 reset value of FPSCR */
#define FPSCR_RESET       0x00040001u

/* 1 if `op` (16-bit) is an instruction this emulator is responsible for. */
static FPUEMU_INLINE int fpuemu_is_fpu_op(sf_u32 op)
{
    sf_u32 top = (op >> 12) & 0xFu;
    if (top == 0xFu) return (op & 0xFu) != 0xFu;
    if (top == 0x4u) {
        sf_u32 lo = op & 0xFFu;
        return lo == 0x5Au || lo == 0x6Au || lo == 0x56u ||
               lo == 0x66u || lo == 0x52u || lo == 0x62u;
    }
    if (top == 0x0u) {
        sf_u32 lo = op & 0xFFu;
        return lo == 0x5Au || lo == 0x6Au;
    }
    return 0;
}

/* 1 if `op` (16-bit) is a pure FPU op that does not touch GPRs or memory
 * (arithmetic, comparisons, single-op/conv, reg-reg fmov, fmac, fschg, frchg). */
static FPUEMU_INLINE int fpuemu_is_arith_op(sf_u32 op)
{
    if ((op & 0xF000u) != 0xF000u) return 0;
    {
        sf_u32 sub = op & 0xFu;
        return sub <= 0x5u || sub == 0xCu || sub == 0xDu || sub == 0xEu;
    }
}

/* Execute one instruction. gpr[16] are R0-R15 (may be modified by @Rm+ / @-Rn
 * addressing), *sr is the status register (T bit updated by FCMP). */
int fpuemu_exec(fpu_state *st, sf_u32 gpr[16], sf_u32 *sr, sf_u32 op, const fpu_mem *mem);

/* Fast path for pure FPU instructions (fpuemu_is_arith_op(op) == 1): touches
 * only `st` and `*sr`, never reads/writes GPRs or guest memory. */
int fpuemu_exec_arith(fpu_state *st, sf_u32 *sr, sf_u32 op);

/* Execute `count` pre-decoded pure FPU opcodes (all satisfying fpuemu_is_arith_op).
 * Stops on the first non-FPU_OK status and writes the number completed to *done. */
int fpuemu_exec_batch(fpu_state *st, sf_u32 *sr, const sf_u16 *ops, int count, int *done);

/* Execute consecutive FPU instructions starting at *pc until a non-FPU
 * instruction, an error, or max_insns. *pc is left at the first instruction
 * not executed; *executed receives the count. Returns the last status
 * (FPU_OK or FPU_NOT_FPU when it stopped normally). */
int fpuemu_run(fpu_state *st, sf_u32 gpr[16], sf_u32 *sr, sf_u32 *pc,
               const fpu_mem *mem, int max_insns, int *executed);

void fpuemu_reset(fpu_state *st);

#endif
