/*
 * fpuemu.c - SH-4 FPU instruction decode and execute on top of softfloat.c.
 * See fpuemu.h for the contract and docs/02-fpu-emulation.md for the design.
 */
#include "fpuemu.h"

#if defined(_MSC_VER)
#define FPU_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define FPU_NOINLINE __attribute__((noinline))
#else
#define FPU_NOINLINE
#endif

static FPUEMU_INLINE sf_u32 *fr_bank(fpu_state *st) { return &st->fr[(st->fpscr & FPSCR_FR) ? 16 : 0]; }
static FPUEMU_INLINE sf_u32 *xf_bank(fpu_state *st) { return &st->fr[(st->fpscr & FPSCR_FR) ? 0 : 16]; }

/* For SZ=1 moves: register field n selects DR(n) when even, XD(n-1) when odd.
 * Returns a pointer to the upper word of the pair. */
static FPUEMU_INLINE sf_u32 *pair_ptr(fpu_state *st, int n)
{
    return (n & 1) ? xf_bank(st) + (n & ~1) : fr_bank(st) + (n & ~1);
}

static FPUEMU_INLINE f64 get_dr(const sf_u32 *p) { return ((f64)p[0] << 32) | p[1]; }
static FPUEMU_INLINE void set_dr(sf_u32 *p, f64 v) { p[0] = (sf_u32)(v >> 32); p[1] = (sf_u32)v; }

static FPUEMU_INLINE void env_from_fpscr(const fpu_state *st, sf_env *env)
{
    sf_u32 fpscr = st->fpscr;
    env->rm = (fpscr & FPSCR_RM_MASK) == 0 ? SF_RN : SF_RZ;
    env->dn = (fpscr & FPSCR_DN) ? 1 : 0;
    env->flags = 0;
}

/* Record exception flags. Returns FPU_EXCEPTION if any raised flag is enabled;
 * the caller must then NOT write the result. */
static FPUEMU_INLINE int finish(fpu_state *st, const sf_env *env)
{
    sf_u32 fl = env->flags & 0x1Fu;
    sf_u32 fpscr = st->fpscr;
    if (fl == 0) {
        if (fpscr & FPSCR_CAUSE_MASK)
            st->fpscr = fpscr & ~FPSCR_CAUSE_MASK;
        return FPU_OK;
    }
    fpscr = (fpscr & ~FPSCR_CAUSE_MASK) | (fl << FPSCR_CAUSE_SHIFT);
    if (fl & ((fpscr >> FPSCR_ENABLE_SHIFT) & 0x1Fu)) {
        st->fpscr = fpscr;
        return FPU_EXCEPTION;
    }
    st->fpscr = fpscr | (fl << FPSCR_FLAG_SHIFT);
    return FPU_OK;
}

void fpuemu_reset(fpu_state *st)
{
    int i;
    st->fpscr = FPSCR_RESET;
    st->fpul = 0;
    for (i = 0; i < 32; i++) st->fr[i] = 0;
}

/* ---- memory helpers: 32-bit and 64-bit (pair) loads/stores ---- */

static int load_pair(const fpu_mem *mem, sf_u32 addr, sf_u32 *hi, sf_u32 *lo)
{
    if (mem->read32(mem->ctx, addr, hi)) return FPU_FAULT;
    if (mem->read32(mem->ctx, addr + 4, lo)) return FPU_FAULT;
    return FPU_OK;
}
static int store_pair(const fpu_mem *mem, sf_u32 addr, sf_u32 hi, sf_u32 lo)
{
    if (mem->write32(mem->ctx, addr, hi)) return FPU_FAULT;
    if (mem->write32(mem->ctx, addr + 4, lo)) return FPU_FAULT;
    return FPU_OK;
}

/* FMOV family (op = 1111nnnnmmmm0110 .. 1100). */
static FPU_NOINLINE int exec_fmov(fpu_state *st, sf_u32 gpr[16], sf_u32 op, const fpu_mem *mem)
{
    int n = (op >> 8) & 0xF, m = (op >> 4) & 0xF, sub = op & 0xF;
    int sz = (st->fpscr & FPSCR_SZ) != 0;
    sf_u32 *FR = fr_bank(st);
    sf_u32 addr, hi, lo;
    int r;

    if (!sz) {
        switch (sub) {
        case 0x6: addr = gpr[0] + gpr[m];
                  if (mem->read32(mem->ctx, addr, &lo)) return FPU_FAULT;
                  FR[n] = lo; return FPU_OK;
        case 0x7: addr = gpr[0] + gpr[n];
                  if (mem->write32(mem->ctx, addr, FR[m])) return FPU_FAULT;
                  return FPU_OK;
        case 0x8: if (mem->read32(mem->ctx, gpr[m], &lo)) return FPU_FAULT;
                  FR[n] = lo; return FPU_OK;
        case 0x9: if (mem->read32(mem->ctx, gpr[m], &lo)) return FPU_FAULT;
                  FR[n] = lo; gpr[m] += 4; return FPU_OK;
        case 0xA: if (mem->write32(mem->ctx, gpr[n], FR[m])) return FPU_FAULT;
                  return FPU_OK;
        case 0xB: addr = gpr[n] - 4;
                  if (mem->write32(mem->ctx, addr, FR[m])) return FPU_FAULT;
                  gpr[n] = addr; return FPU_OK;
        case 0xC: FR[n] = FR[m]; return FPU_OK;
        }
    } else {
        sf_u32 *dst = pair_ptr(st, n), *src = pair_ptr(st, m);
        switch (sub) {
        case 0x6: addr = gpr[0] + gpr[m];
                  if ((r = load_pair(mem, addr, &hi, &lo)) != FPU_OK) return r;
                  dst[0] = hi; dst[1] = lo; return FPU_OK;
        case 0x7: addr = gpr[0] + gpr[n];
                  return store_pair(mem, addr, src[0], src[1]);
        case 0x8: if ((r = load_pair(mem, gpr[m], &hi, &lo)) != FPU_OK) return r;
                  dst[0] = hi; dst[1] = lo; return FPU_OK;
        case 0x9: if ((r = load_pair(mem, gpr[m], &hi, &lo)) != FPU_OK) return r;
                  dst[0] = hi; dst[1] = lo; gpr[m] += 8; return FPU_OK;
        case 0xA: return store_pair(mem, gpr[n], src[0], src[1]);
        case 0xB: addr = gpr[n] - 8;
                  if ((r = store_pair(mem, addr, src[0], src[1])) != FPU_OK) return r;
                  gpr[n] = addr; return FPU_OK;
        case 0xC: dst[0] = src[0]; dst[1] = src[1]; return FPU_OK;
        }
    }
    return FPU_UNIMPL;
}

/* 0x0 and 0x4 lds/sts (.l) fpul/fpscr group. */
static FPU_NOINLINE int exec_04(fpu_state *st, sf_u32 gpr[16], sf_u32 op, const fpu_mem *mem)
{
    int n = (op >> 8) & 0xF;
    sf_u32 v;
    if ((op >> 12) == 0x0u) {
        if ((op & 0xF0FFu) == 0x005Au) { gpr[n] = st->fpul;  return FPU_OK; }   /* sts fpul,rn */
        if ((op & 0xF0FFu) == 0x006Au) { gpr[n] = st->fpscr; return FPU_OK; }   /* sts fpscr,rn */
        return FPU_NOT_FPU;
    }
    switch (op & 0xF0FFu) {
    case 0x405Au: st->fpul = gpr[n]; return FPU_OK;                             /* lds rm,fpul */
    case 0x406Au: st->fpscr = gpr[n] & FPSCR_MASK; return FPU_OK;               /* lds rm,fpscr */
    case 0x4056u: if (mem->read32(mem->ctx, gpr[n], &v)) return FPU_FAULT;      /* lds.l @rm+,fpul */
                  st->fpul = v; gpr[n] += 4; return FPU_OK;
    case 0x4066u: if (mem->read32(mem->ctx, gpr[n], &v)) return FPU_FAULT;      /* lds.l @rm+,fpscr */
                  st->fpscr = v & FPSCR_MASK; gpr[n] += 4; return FPU_OK;
    case 0x4052u: if (mem->write32(mem->ctx, gpr[n] - 4, st->fpul)) return FPU_FAULT;  /* sts.l fpul,@-rn */
                  gpr[n] -= 4; return FPU_OK;
    case 0x4062u: if (mem->write32(mem->ctx, gpr[n] - 4, st->fpscr)) return FPU_FAULT; /* sts.l fpscr,@-rn */
                  gpr[n] -= 4; return FPU_OK;
    }
    return FPU_NOT_FPU;
}

/* 1111nnnnmmmm1101 group: single-operand and conversion instructions. */
static int exec_fxxd(fpu_state *st, sf_u32 op)
{
    int n = (op >> 8) & 0xF, kind = (op >> 4) & 0xF;
    sf_u32 *FR = fr_bank(st);
    sf_env env;
    int r;

    switch (kind) {
    case 0x0: FR[n] = st->fpul; return FPU_OK;                 /* fsts fpul,frn */
    case 0x1: st->fpul = FR[n]; return FPU_OK;                 /* flds frn,fpul */
    case 0x2:                                                  /* float fpul,frn/drn */
        env_from_fpscr(st, &env);
        if (!(st->fpscr & FPSCR_PR)) {
            f32 v = f32_from_i32(&env, (sf_i32)st->fpul);
            if ((r = finish(st, &env)) != FPU_OK) return r;
            FR[n] = v;
            return FPU_OK;
        }
        if (n & 1) return FPU_UNIMPL;
        {
            f64 v = f64_from_i32(&env, (sf_i32)st->fpul);
            if ((r = finish(st, &env)) != FPU_OK) return r;
            set_dr(FR + n, v);
            return FPU_OK;
        }
    case 0x3:                                                  /* ftrc frn/drn,fpul */
        env_from_fpscr(st, &env);
        if (!(st->fpscr & FPSCR_PR)) {
            sf_i32 v = f32_to_i32(&env, FR[n]);
            if ((r = finish(st, &env)) != FPU_OK) return r;
            st->fpul = (sf_u32)v;
            return FPU_OK;
        }
        if (n & 1) return FPU_UNIMPL;
        {
            sf_i32 v = f64_to_i32(&env, get_dr(FR + n));
            if ((r = finish(st, &env)) != FPU_OK) return r;
            st->fpul = (sf_u32)v;
            return FPU_OK;
        }
    /* fneg/fabs touch the sign bit of FRn whatever PR says: with PR = 1 and
     * an even n that is DRn's sign; the CE 5.0 SH-4 compiler also emits them
     * on odd registers in double mode (Plotter-Z's recalculation stopped at
     * fneg fr3, 0xF34D, 2026-11-09), which the hardware takes the same way. */
    case 0x4:                                                  /* fneg */
        FR[n] ^= 0x80000000u; return FPU_OK;
    case 0x5:                                                  /* fabs */
        FR[n] &= 0x7FFFFFFFu; return FPU_OK;
    case 0x6:                                                  /* fsqrt */
        env_from_fpscr(st, &env);
        if (!(st->fpscr & FPSCR_PR)) {
            f32 v = f32_sqrt(&env, FR[n]);
            if ((r = finish(st, &env)) != FPU_OK) return r;
            FR[n] = v;
            return FPU_OK;
        }
        if (n & 1) return FPU_UNIMPL;
        {
            f64 v = f64_sqrt(&env, get_dr(FR + n));
            if ((r = finish(st, &env)) != FPU_OK) return r;
            set_dr(FR + n, v);
            return FPU_OK;
        }
    case 0x7: return FPU_UNIMPL;                               /* fsrra (approximate) */
    case 0x8: FR[n] = 0; return FPU_OK;                        /* fldi0 */
    case 0x9: FR[n] = 0x3F800000u; return FPU_OK;              /* fldi1 */
    case 0xA:                                                  /* fcnvsd fpul,drn */
        if (!(st->fpscr & FPSCR_PR) || (n & 1)) return FPU_UNIMPL;
        env_from_fpscr(st, &env);
        {
            f64 v = f32_to_f64(&env, st->fpul);
            if ((r = finish(st, &env)) != FPU_OK) return r;
            set_dr(FR + n, v);
            return FPU_OK;
        }
    case 0xB:                                                  /* fcnvds drm,fpul */
        if (!(st->fpscr & FPSCR_PR) || (n & 1)) return FPU_UNIMPL;
        env_from_fpscr(st, &env);
        {
            f32 v = f64_to_f32(&env, get_dr(FR + n));
            if ((r = finish(st, &env)) != FPU_OK) return r;
            st->fpul = v;
            return FPU_OK;
        }
    case 0xE: return FPU_UNIMPL;                               /* fipr */
    case 0xF:
        if (op == 0xF3FDu) { st->fpscr ^= FPSCR_SZ; return FPU_OK; }   /* fschg */
        if (op == 0xFBFDu) { st->fpscr ^= FPSCR_FR; return FPU_OK; }   /* frchg */
        return FPU_UNIMPL;                                     /* ftrv, fsca */
    default:
        return FPU_UNIMPL;
    }
}

/* Two-operand arithmetic: fadd fsub fmul fdiv fcmp/eq fcmp/gt (sub 0..5). */
static int exec_arith(fpu_state *st, sf_u32 *sr, sf_u32 op)
{
    int n = (op >> 8) & 0xF, m = (op >> 4) & 0xF, sub = op & 0xF;
    sf_u32 *FR = fr_bank(st);
    sf_env env;
    int r;
    env_from_fpscr(st, &env);

    if (!(st->fpscr & FPSCR_PR)) {
        f32 a = FR[n], b = FR[m], v = 0;
        int t = 0;
        switch (sub) {
        case 0: v = f32_add(&env, a, b); break;
        case 1: v = f32_sub(&env, a, b); break;
        case 2: v = f32_mul(&env, a, b); break;
        case 3: v = f32_div(&env, a, b); break;
        case 4: t = f32_eq(&env, a, b); break;
        case 5: t = f32_gt(&env, a, b); break;
        }
        if ((r = finish(st, &env)) != FPU_OK) return r;
        if (sub >= 4) *sr = (*sr & ~1u) | (sf_u32)t;
        else FR[n] = v;
        return FPU_OK;
    } else {
        f64 a, b, v = 0;
        int t = 0;
        if ((n & 1) || (m & 1)) return FPU_UNIMPL;
        a = get_dr(FR + n); b = get_dr(FR + m);
        switch (sub) {
        case 0: v = f64_add(&env, a, b); break;
        case 1: v = f64_sub(&env, a, b); break;
        case 2: v = f64_mul(&env, a, b); break;
        case 3: v = f64_div(&env, a, b); break;
        case 4: t = f64_eq(&env, a, b); break;
        case 5: t = f64_gt(&env, a, b); break;
        }
        if ((r = finish(st, &env)) != FPU_OK) return r;
        if (sub >= 4) *sr = (*sr & ~1u) | (sf_u32)t;
        else set_dr(FR + n, v);
        return FPU_OK;
    }
}

int fpuemu_exec_arith(fpu_state *st, sf_u32 *sr, sf_u32 op)
{
    sf_u32 sub = op & 0xFu;
    if (sub <= 0x5u)
        return exec_arith(st, sr, op);
    if (sub == 0xDu)
        return exec_fxxd(st, op);
    if (sub == 0xCu) {
        int n = (op >> 8) & 0xF, m = (op >> 4) & 0xF;
        if (!(st->fpscr & FPSCR_SZ)) {
            sf_u32 *FR = fr_bank(st);
            FR[n] = FR[m];
        } else {
            sf_u32 *dst = pair_ptr(st, n), *src = pair_ptr(st, m);
            dst[0] = src[0];
            dst[1] = src[1];
        }
        return FPU_OK;
    }
    if (sub == 0xEu) {                                          /* fmac fr0,frm,frn */
        sf_u32 *FR = fr_bank(st);
        int n = (op >> 8) & 0xF;
        sf_env env; f32 r; int rc;
        if (st->fpscr & FPSCR_PR) return FPU_UNIMPL;
        env_from_fpscr(st, &env);
        r = f32_fmac(&env, FR[0], FR[(op >> 4) & 0xF], FR[n]);
        if ((rc = finish(st, &env)) != FPU_OK) return rc;
        FR[n] = r;
        return FPU_OK;
    }
    return FPU_NOT_FPU;
}

int fpuemu_exec_batch(fpu_state *st, sf_u32 *sr, const sf_u16 *ops, int count, int *done)
{
    int i, r = FPU_OK;
    for (i = 0; i < count; i++) {
        r = fpuemu_exec_arith(st, sr, ops[i]);
        if (r != FPU_OK) break;
    }
    if (done) *done = i;
    return r;
}

int fpuemu_exec(fpu_state *st, sf_u32 gpr[16], sf_u32 *sr, sf_u32 op, const fpu_mem *mem)
{
    sf_u32 top;
    op &= 0xFFFFu;
    top = op >> 12;
    if (top == 0xFu) {
        sf_u32 sub = op & 0xFu;
        if (sub <= 0x5u) return exec_arith(st, sr, op);
        if (sub == 0xDu) return exec_fxxd(st, op);
        if (sub <= 0xCu) return exec_fmov(st, gpr, op, mem);
        if (sub == 0xEu) return fpuemu_exec_arith(st, sr, op);
        return FPU_NOT_FPU;
    }
    if (top == 0x4u || top == 0x0u)
        return exec_04(st, gpr, op, mem);
    return FPU_NOT_FPU;
}

int fpuemu_run(fpu_state *st, sf_u32 gpr[16], sf_u32 *sr, sf_u32 *pc,
               const fpu_mem *mem, int max_insns, int *executed)
{
    int count = 0, r = FPU_OK;
    sf_u32 op;
    while (count < max_insns) {
        if (mem->fetch16(mem->ctx, *pc, &op)) { r = FPU_FAULT; break; }
        if (!fpuemu_is_fpu_op(op)) { r = count ? FPU_OK : FPU_NOT_FPU; break; }
        r = fpuemu_exec(st, gpr, sr, op, mem);
        if (r != FPU_OK) break;
        *pc += 2;
        count++;
    }
    if (executed) *executed = count;
    return r;
}
