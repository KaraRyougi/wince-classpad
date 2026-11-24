/*
 * test_host.c - compare the integer-only soft-float against the host FPU,
 * and exercise the SH-4 instruction decoder with a fake memory.
 *
 * Build/run: make test   (from the repo root)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fenv.h>
#include <stdint.h>
#include "softfloat.h"
#include "fpuemu.h"

#pragma STDC FENV_ACCESS ON

static int failures = 0, checks = 0;
static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t next64(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

static float    u2f(uint32_t u) { float f;  memcpy(&f, &u, 4); return f; }
static uint32_t f2u(float f)    { uint32_t u; memcpy(&u, &f, 4); return u; }
static double   u2d(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }
static uint64_t d2u(double d)   { uint64_t u; memcpy(&u, &d, 8); return u; }

static int host_flags(void)
{
    int e = fetestexcept(FE_ALL_EXCEPT), f = 0;
    if (e & FE_INEXACT)   f |= SF_FLAG_I;
    if (e & FE_OVERFLOW)  f |= SF_FLAG_O;
    if (e & FE_DIVBYZERO) f |= SF_FLAG_Z;
    if (e & FE_INVALID)   f |= SF_FLAG_V;
    return f;
}
#define CMP_FLAGS (SF_FLAG_I | SF_FLAG_O | SF_FLAG_Z | SF_FLAG_V)

static void fail(const char *what, int rm, uint64_t a, uint64_t b, uint64_t got, uint64_t exp, int gf, int ef)
{
    failures++;
    if (failures <= 25)
        printf("FAIL %-8s rm=%d a=%016llx b=%016llx got=%016llx exp=%016llx flags got=%02x exp=%02x\n",
               what, rm, (unsigned long long)a, (unsigned long long)b,
               (unsigned long long)got, (unsigned long long)exp, gf, ef);
}

/* ---- operand generators ---- */
static const uint32_t spec32[] = {
    0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x7F800000, 0xFF800000,
    0x7F7FFFFF, 0xFF7FFFFF, 0x00800000, 0x80800000, 0x00000001, 0x80000001,
    0x007FFFFF, 0x3F000000, 0x40000000, 0x4B000000, 0x4F000000, 0xCF000000,
    0x7FBFFFFF, 0x7FC00000, 0x7F800001, 0x3EAAAAAB, 0x33800000, 0x4EFFFFFF,
    0x4F7FFFFF, 0xCF7FFFFF, 0x5F800000, 0x1F800000,
};
static const uint64_t spec64[] = {
    0x0000000000000000ull, 0x8000000000000000ull, 0x3FF0000000000000ull, 0xBFF0000000000000ull,
    0x7FF0000000000000ull, 0xFFF0000000000000ull, 0x7FEFFFFFFFFFFFFFull, 0xFFEFFFFFFFFFFFFFull,
    0x0010000000000000ull, 0x8010000000000000ull, 0x0000000000000001ull, 0x000FFFFFFFFFFFFFull,
    0x3FE0000000000000ull, 0x4000000000000000ull, 0x41E0000000000000ull, 0xC1E0000000000000ull,
    0x7FF7FFFFFFFFFFFFull, 0x7FF8000000000000ull, 0x7FF0000000000001ull, 0x3FD5555555555555ull,
    0x41DFFFFFFFC00000ull, 0x41DFFFFFFFFFFFFFull, 0xC1E0000000200000ull, 0x4330000000000000ull,
};

static uint32_t rand32(int i)
{
    uint64_t r = next64();
    switch (i % 6) {
    case 0: return (uint32_t)r;                                  /* any bits */
    case 1: return (uint32_t)r & 0x7FFFFFFF;                     /* positive */
    case 2: return ((uint32_t)r & 0x807FFFFF) | ((0x70u + (r >> 40) % 0x20) << 23); /* near 1.0 */
    case 3: return spec32[(r >> 8) % (sizeof spec32 / sizeof *spec32)];
    case 4: return (uint32_t)r & 0x807FFFFF;                     /* denormal / zero */
    default: return ((uint32_t)r & 0x807FFFFF) | ((0x7Eu + (r >> 40) % 4) << 23);  /* huge */
    }
}
static uint64_t rand64(int i)
{
    uint64_t r = next64();
    switch (i % 6) {
    case 0: return r;
    case 1: return r & 0x7FFFFFFFFFFFFFFFull;
    case 2: return (r & 0x800FFFFFFFFFFFFFull) | ((0x3F0ull + (next64() % 0x20)) << 52);
    case 3: return spec64[(r >> 8) % (sizeof spec64 / sizeof *spec64)];
    case 4: return r & 0x800FFFFFFFFFFFFFull;
    default: return (r & 0x800FFFFFFFFFFFFFull) | ((0x7FCull + (next64() % 4)) << 52);
    }
}

static int nan32(uint32_t u) { return (u & 0x7F800000) == 0x7F800000 && (u & 0x7FFFFF); }
static int nan64(uint64_t u) { return (u & 0x7FF0000000000000ull) == 0x7FF0000000000000ull && (u & 0xFFFFFFFFFFFFFull); }

/* Host "nearby" generator: b close to a, to exercise cancellation */
static uint32_t near32(uint32_t a) { uint64_t r = next64(); return (a ^ 0x80000000u) + (uint32_t)((int)(r % 7) - 3); }
static uint64_t near64(uint64_t a) { uint64_t r = next64(); return (a ^ 0x8000000000000000ull) + (uint64_t)((int)(r % 7) - 3); }

static void test_binary32(const char *name, int which, int n)
{
    int i, rm;
    for (rm = 0; rm < 2; rm++) {
        sf_env env; env.rm = rm ? SF_RZ : SF_RN; env.dn = 0;
        fesetround(rm ? FE_TOWARDZERO : FE_TONEAREST);
        for (i = 0; i < n; i++) {
            uint32_t a = rand32(i), b = (i % 5 == 4) ? near32(a) : rand32(i * 7 + 1);
            volatile float fa = u2f(a), fb = u2f(b), fr = 0;
            uint32_t got = 0, exp;
            int ef, gf;
            env.flags = 0;
            feclearexcept(FE_ALL_EXCEPT);
            switch (which) {
            case 0: fr = fa + fb; got = f32_add(&env, a, b); break;
            case 1: fr = fa - fb; got = f32_sub(&env, a, b); break;
            case 2: fr = fa * fb; got = f32_mul(&env, a, b); break;
            case 3: fr = fa / fb; got = f32_div(&env, a, b); break;
            }
            ef = host_flags(); gf = (int)env.flags;
            exp = f2u(fr);
            checks++;
            if (nan32(a) || nan32(b) || nan32(exp) || nan32(got)) {
                /* SH-4 and IEEE-2008 disagree on which NaNs are signaling: compare NaN-ness only */
                if (nan32(exp) != nan32(got)) fail(name, rm, a, b, got, exp, gf, ef);
            } else if (got != exp || ((ef ^ gf) & CMP_FLAGS)) {
                fail(name, rm, a, b, got, exp, gf, ef);
            }
        }
    }
}

static void test_binary64(const char *name, int which, int n)
{
    int i, rm;
    for (rm = 0; rm < 2; rm++) {
        sf_env env; env.rm = rm ? SF_RZ : SF_RN; env.dn = 0;
        fesetround(rm ? FE_TOWARDZERO : FE_TONEAREST);
        for (i = 0; i < n; i++) {
            uint64_t a = rand64(i), b = (i % 5 == 4) ? near64(a) : rand64(i * 7 + 1);
            volatile double da = u2d(a), db = u2d(b), dr = 0;
            uint64_t got = 0, exp;
            int ef, gf;
            env.flags = 0;
            feclearexcept(FE_ALL_EXCEPT);
            switch (which) {
            case 0: dr = da + db; got = f64_add(&env, a, b); break;
            case 1: dr = da - db; got = f64_sub(&env, a, b); break;
            case 2: dr = da * db; got = f64_mul(&env, a, b); break;
            case 3: dr = da / db; got = f64_div(&env, a, b); break;
            }
            ef = host_flags(); gf = (int)env.flags;
            exp = d2u(dr);
            checks++;
            if (nan64(a) || nan64(b) || nan64(exp) || nan64(got)) {
                if (nan64(exp) != nan64(got)) fail(name, rm, a, b, got, exp, gf, ef);
            } else if (got != exp || ((ef ^ gf) & CMP_FLAGS)) {
                fail(name, rm, a, b, got, exp, gf, ef);
            }
        }
    }
}

static void test_unary(int n)
{
    int i, rm;
    for (rm = 0; rm < 2; rm++) {
        sf_env env; env.rm = rm ? SF_RZ : SF_RN; env.dn = 0;
        fesetround(rm ? FE_TOWARDZERO : FE_TONEAREST);
        for (i = 0; i < n; i++) {
            uint32_t a = rand32(i);
            uint64_t d = rand64(i);
            volatile float fa = u2f(a), fr;
            volatile double da = u2d(d), dr;
            uint32_t got32, exp32; uint64_t got64, exp64;
            int ef, gf;

            /* sqrt */
            env.flags = 0; feclearexcept(FE_ALL_EXCEPT);
            fr = sqrtf(fa); exp32 = f2u(fr); ef = host_flags();
            got32 = f32_sqrt(&env, a); gf = (int)env.flags; checks++;
            if (nan32(exp32) || nan32(got32)) { if (!(nan32(exp32) && nan32(got32))) fail("sqrt32", rm, a, 0, got32, exp32, gf, ef); }
            else if (got32 != exp32 || ((ef ^ gf) & CMP_FLAGS)) fail("sqrt32", rm, a, 0, got32, exp32, gf, ef);

            env.flags = 0; feclearexcept(FE_ALL_EXCEPT);
            dr = sqrt(da); exp64 = d2u(dr); ef = host_flags();
            got64 = f64_sqrt(&env, d); gf = (int)env.flags; checks++;
            if (nan64(exp64) || nan64(got64)) { if (!(nan64(exp64) && nan64(got64))) fail("sqrt64", rm, d, 0, got64, exp64, gf, ef); }
            else if (got64 != exp64 || ((ef ^ gf) & CMP_FLAGS)) fail("sqrt64", rm, d, 0, got64, exp64, gf, ef);

            /* f32 -> f64 (exact) and f64 -> f32 (rounded) */
            env.flags = 0; feclearexcept(FE_ALL_EXCEPT);
            dr = (double)fa; exp64 = d2u(dr); ef = host_flags();
            got64 = f32_to_f64(&env, a); gf = (int)env.flags; checks++;
            if (nan32(a)) { if (!nan64(got64)) fail("cvt_sd", rm, a, 0, got64, exp64, gf, ef); }
            else if (got64 != exp64) fail("cvt_sd", rm, a, 0, got64, exp64, gf, ef);

            env.flags = 0; feclearexcept(FE_ALL_EXCEPT);
            fr = (float)da; exp32 = f2u(fr); ef = host_flags();
            got32 = f64_to_f32(&env, d); gf = (int)env.flags; checks++;
            if (nan64(d)) { if (!nan32(got32)) fail("cvt_ds", rm, d, 0, got32, exp32, gf, ef); }
            else if (got32 != exp32 || ((ef ^ gf) & (SF_FLAG_I | SF_FLAG_O))) fail("cvt_ds", rm, d, 0, got32, exp32, gf, ef);

            /* int -> float */
            {
                int32_t iv = (int32_t)(uint32_t)next64();
                if (i % 4 == 0) iv = (int32_t)(next64() % 2000) - 1000;
                env.flags = 0; feclearexcept(FE_ALL_EXCEPT);
                fr = (float)iv; exp32 = f2u(fr); ef = host_flags();
                got32 = f32_from_i32(&env, iv); gf = (int)env.flags; checks++;
                if (got32 != exp32 || ((ef ^ gf) & SF_FLAG_I)) fail("float32", rm, (uint32_t)iv, 0, got32, exp32, gf, ef);
                env.flags = 0;
                got64 = f64_from_i32(&env, iv); exp64 = d2u((double)iv); checks++;
                if (got64 != exp64 || env.flags) fail("float64", rm, (uint32_t)iv, 0, got64, exp64, (int)env.flags, 0);
            }

            /* float -> int (truncate, saturate) */
            {
                int32_t got, exp; int inv;
                double v = (double)fa;
                if (nan32(a)) { exp = 0x7FFFFFFF; inv = 1; }
                else if (v >= 2147483648.0) { exp = 0x7FFFFFFF; inv = 1; }
                else if (v < -2147483648.0) { exp = (int32_t)0x80000000; inv = 1; }
                else { exp = (int32_t)v; inv = 0; }
                env.flags = 0;
                got = f32_to_i32(&env, a); checks++;
                if (got != exp || ((env.flags & SF_FLAG_V) != 0) != inv) fail("ftrc32", rm, a, 0, (uint32_t)got, (uint32_t)exp, (int)env.flags, inv ? SF_FLAG_V : 0);

                v = da;
                if (nan64(d)) { exp = 0x7FFFFFFF; inv = 1; }
                else if (v >= 2147483648.0) { exp = 0x7FFFFFFF; inv = 1; }
                else if (v < -2147483648.0) { exp = (int32_t)0x80000000; inv = 1; }
                else { exp = (int32_t)v; inv = 0; }
                env.flags = 0;
                got = f64_to_i32(&env, d); checks++;
                if (got != exp || ((env.flags & SF_FLAG_V) != 0) != inv) fail("ftrc64", rm, d, 0, (uint32_t)got, (uint32_t)exp, (int)env.flags, inv ? SF_FLAG_V : 0);
            }
        }
    }
}

static void test_fmac(int n)
{
    int i, rm;
    for (rm = 0; rm < 2; rm++) {
        sf_env env; env.rm = rm ? SF_RZ : SF_RN; env.dn = 0;
        fesetround(rm ? FE_TOWARDZERO : FE_TONEAREST);
        for (i = 0; i < n; i++) {
            uint32_t a = rand32(i), b = rand32(i + 3), c = rand32(i + 5);
            volatile float fa = u2f(a), fb = u2f(b), fc = u2f(c), fr;
            uint32_t got, exp;
            if (i % 3 == 0) {   /* make c cancel the product */
                fc = -(fa * fb); c = f2u(fc);
                c = near32(c ^ 0x80000000u);
                fc = u2f(c);
            }
            env.flags = 0; feclearexcept(FE_ALL_EXCEPT);
            fr = fmaf(fa, fb, fc); exp = f2u(fr);
            got = f32_fmac(&env, a, b, c); checks++;
            if (nan32(exp) || nan32(got)) { if (!(nan32(exp) && nan32(got))) fail("fmac", rm, ((uint64_t)a << 32) | b, c, got, exp, (int)env.flags, host_flags()); }
            else if (got != exp) fail("fmac", rm, ((uint64_t)a << 32) | b, c, got, exp, (int)env.flags, host_flags());
        }
    }
}

static void test_compare(int n)
{
    int i;
    sf_env env; env.rm = SF_RN; env.dn = 0;
    for (i = 0; i < n; i++) {
        uint32_t a = rand32(i), b = (i & 1) ? near32(a) : rand32(i + 9);
        uint64_t x = rand64(i), y = (i & 1) ? near64(x) : rand64(i + 9);
        float fa = u2f(a), fb = u2f(b);
        double dx = u2d(x), dy = u2d(y);
        checks += 4;
        env.flags = 0;
        if (f32_eq(&env, a, b) != (fa == fb)) fail("eq32", 0, a, b, 0, 0, 0, 0);
        if (f32_gt(&env, a, b) != (fa > fb)) fail("gt32", 0, a, b, 0, 0, 0, 0);
        if (f64_eq(&env, x, y) != (dx == dy)) fail("eq64", 0, x, y, 0, 0, 0, 0);
        if (f64_gt(&env, x, y) != (dx > dy)) fail("gt64", 0, x, y, 0, 0, 0, 0);
    }
}

static void test_dn(void)
{
    sf_env env; env.rm = SF_RN; env.dn = 1;
    uint32_t r;
    /* denormal input treated as zero */
    env.flags = 0; r = f32_add(&env, 0x00000001u, 0x00000001u); checks++;
    if (r != 0) fail("dn_add", 0, 1, 1, r, 0, (int)env.flags, 0);
    /* tiny result flushed, underflow+inexact flagged */
    env.flags = 0; r = f32_mul(&env, 0x00800000u, 0x3F000000u); checks++;   /* min_normal * 0.5 */
    if (r != 0 || env.flags != (SF_FLAG_U | SF_FLAG_I)) fail("dn_mul", 0, 0x00800000u, 0x3F000000u, r, 0, (int)env.flags, SF_FLAG_U | SF_FLAG_I);
    /* sign of a flushed negative result */
    env.flags = 0; r = f32_mul(&env, 0x80800000u, 0x3F000000u); checks++;
    if (r != 0x80000000u) fail("dn_sign", 0, 0x80800000u, 0x3F000000u, r, 0x80000000u, (int)env.flags, 0);
    /* with dn=0 the same product is a denormal */
    env.dn = 0; env.flags = 0; r = f32_mul(&env, 0x00800000u, 0x3F000000u); checks++;
    if (r != 0x00400000u || env.flags != 0) fail("dn0_mul", 0, 0x00800000u, 0x3F000000u, r, 0x00400000u, (int)env.flags, 0);
}

/* ---- instruction-level tests with a fake 4 KiB memory ---- */
static uint32_t fakemem[1024];
static int rd32(void *c, sf_u32 a, sf_u32 *v) { (void)c; if (a >= 4096 || (a & 3)) return 1; *v = fakemem[a / 4]; return 0; }
static int wr32(void *c, sf_u32 a, sf_u32 v) { (void)c; if (a >= 4096 || (a & 3)) return 1; fakemem[a / 4] = v; return 0; }
static uint16_t code[64];
static int fe16(void *c, sf_u32 a, sf_u32 *op) { (void)c; if (a >= sizeof code) return 1; *op = code[a / 2]; return 0; }

static void expect(const char *what, int cond)
{
    checks++;
    if (!cond) { failures++; printf("FAIL insn: %s\n", what); }
}

static void test_insns(void)
{
    fpu_state st; sf_u32 gpr[16] = {0}; sf_u32 sr = 0;
    fpu_mem mem; mem.read32 = rd32; mem.write32 = wr32; mem.fetch16 = fe16; mem.ctx = 0;
    fpuemu_reset(&st);
    st.fpscr = 0;                                  /* RN, DN=0, PR=0, SZ=0, FR=0 */

    /* fldi1 fr1 ; fldi1 fr2 ; fadd fr2,fr1  -> fr1 = 2.0 */
    expect("fldi1", fpuemu_exec(&st, gpr, &sr, 0xF19D, &mem) == FPU_OK && st.fr[1] == 0x3F800000u);
    expect("fldi1b", fpuemu_exec(&st, gpr, &sr, 0xF29D, &mem) == FPU_OK);
    expect("fadd", fpuemu_exec(&st, gpr, &sr, 0xF120, &mem) == FPU_OK && st.fr[1] == 0x40000000u);

    /* fmov.s @r1,fr3 from fake memory; fmov.s fr3,@-r2 */
    fakemem[0x10 / 4] = 0x40490FDBu; gpr[1] = 0x10;
    expect("fmov.s @rm", fpuemu_exec(&st, gpr, &sr, 0xF318, &mem) == FPU_OK && st.fr[3] == 0x40490FDBu);
    gpr[2] = 0x100;
    expect("fmov.s @-rn", fpuemu_exec(&st, gpr, &sr, 0xF23B, &mem) == FPU_OK && gpr[2] == 0xFC && fakemem[0xFC / 4] == 0x40490FDBu);
    /* fmov.s @r1+,fr4 : r1 advances */
    expect("fmov.s @rm+", fpuemu_exec(&st, gpr, &sr, 0xF419, &mem) == FPU_OK && gpr[1] == 0x14 && st.fr[4] == 0x40490FDBu);
    /* fault: unaligned address leaves state untouched */
    gpr[5] = 0x13;
    expect("fault", fpuemu_exec(&st, gpr, &sr, 0xF659, &mem) == FPU_FAULT && gpr[5] == 0x13 && st.fr[6] == 0);

    /* fcmp/gt fr1(2.0) > fr3(pi)? no -> T=0 ; fcmp/gt fr3,fr1 -> T=1 */
    sr = 1;
    expect("fcmp/gt F", fpuemu_exec(&st, gpr, &sr, 0xF135, &mem) == FPU_OK && (sr & 1) == 0);
    expect("fcmp/gt T", fpuemu_exec(&st, gpr, &sr, 0xF315, &mem) == FPU_OK && (sr & 1) == 1);

    /* ftrc fr3,fpul -> 3 ; float fpul,fr5 -> 3.0 ; sts fpul,r7 */
    expect("ftrc", fpuemu_exec(&st, gpr, &sr, 0xF33D, &mem) == FPU_OK && st.fpul == 3);
    expect("float", fpuemu_exec(&st, gpr, &sr, 0xF52D, &mem) == FPU_OK && st.fr[5] == 0x40400000u);
    expect("sts fpul", fpuemu_exec(&st, gpr, &sr, 0x075A, &mem) == FPU_OK && gpr[7] == 3);

    /* lds r8,fpscr with PR=1 ; double add DR2 = DR2 + DR4 (1.5 + 2.25) */
    gpr[8] = FPSCR_PR;
    expect("lds fpscr", fpuemu_exec(&st, gpr, &sr, 0x486A, &mem) == FPU_OK && st.fpscr == FPSCR_PR);
    st.fr[2] = 0x3FF80000u; st.fr[3] = 0; st.fr[4] = 0x40020000u; st.fr[5] = 0;
    expect("fadd DR", fpuemu_exec(&st, gpr, &sr, 0xF240, &mem) == FPU_OK && st.fr[2] == 0x400E0000u && st.fr[3] == 0);
    /* odd register with PR=1 is reserved */
    expect("fadd DR odd", fpuemu_exec(&st, gpr, &sr, 0xF340, &mem) == FPU_UNIMPL);
    /* fneg/fabs flip or clear the sign bit of FRn whatever PR is, odd n included
     * (the CE 5.0 compiler emits fneg fr3 in double mode: Plotter-Z, 2026-11-09) */
    st.fr[3] = 0x40490FDBu;
    expect("fneg odd PR", fpuemu_exec(&st, gpr, &sr, 0xF34D, &mem) == FPU_OK && st.fr[3] == 0xC0490FDBu && st.fr[2] == 0x400E0000u);
    expect("fabs odd PR", fpuemu_exec(&st, gpr, &sr, 0xF35D, &mem) == FPU_OK && st.fr[3] == 0x40490FDBu);
    expect("fneg DR", fpuemu_exec(&st, gpr, &sr, 0xF24D, &mem) == FPU_OK && st.fr[2] == 0xC00E0000u && st.fr[3] == 0x40490FDBu);
    st.fr[2] = 0x400E0000u; st.fr[3] = 0;
    /* fcnvds DR2,fpul -> 3.75f ; fcnvsd back */
    expect("fcnvds", fpuemu_exec(&st, gpr, &sr, 0xF2BD, &mem) == FPU_OK && st.fpul == 0x40700000u);
    expect("fcnvsd", fpuemu_exec(&st, gpr, &sr, 0xF6AD, &mem) == FPU_OK && st.fr[6] == 0x400E0000u && st.fr[7] == 0);

    /* fschg -> SZ=1 ; fmov DR2,@r9 writes two words high first ; frchg swaps banks */
    gpr[9] = 0x200;
    expect("fschg", fpuemu_exec(&st, gpr, &sr, 0xF3FD, &mem) == FPU_OK && (st.fpscr & FPSCR_SZ));
    expect("fmov DR,@rn", fpuemu_exec(&st, gpr, &sr, 0xF92A, &mem) == FPU_OK && fakemem[0x200 / 4] == 0x400E0000u && fakemem[0x204 / 4] == 0);
    /* fmov @r9,XD0 (n=1 means XD0) */
    expect("fmov @rm,XD", fpuemu_exec(&st, gpr, &sr, 0xF198, &mem) == FPU_OK && st.fr[16] == 0x400E0000u && st.fr[17] == 0);
    expect("frchg", fpuemu_exec(&st, gpr, &sr, 0xFBFD, &mem) == FPU_OK && (st.fpscr & FPSCR_FR));
    /* after frchg, FR0 is old XF0 */
    st.fpscr &= ~FPSCR_SZ;
    expect("bank swap", fpuemu_exec(&st, gpr, &sr, 0xF80C, &mem) == FPU_OK && st.fr[24] == 0x400E0000u);  /* fmov fr0,fr8 */
    st.fpscr &= ~FPSCR_FR;

    /* enabled exception: enable Z, fdiv by zero -> FPU_EXCEPTION, result not written, cause set */
    st.fpscr = (SF_FLAG_Z << FPSCR_ENABLE_SHIFT);
    st.fr[1] = 0x3F800000u; st.fr[2] = 0;
    expect("fdiv exc", fpuemu_exec(&st, gpr, &sr, 0xF123, &mem) == FPU_EXCEPTION && st.fr[1] == 0x3F800000u
           && ((st.fpscr >> FPSCR_CAUSE_SHIFT) & 0x1F) == SF_FLAG_Z && ((st.fpscr >> FPSCR_FLAG_SHIFT) & 0x1F) == 0);
    /* disabled: result inf, flag Z sticky */
    st.fpscr = 0;
    expect("fdiv inf", fpuemu_exec(&st, gpr, &sr, 0xF123, &mem) == FPU_OK && st.fr[1] == 0x7F800000u
           && ((st.fpscr >> FPSCR_FLAG_SHIFT) & 0x1F) == SF_FLAG_Z);

    /* fmac fr0,fr1,fr2 : 2*3+1 = 7 */
    st.fpscr = 0; st.fr[0] = 0x40000000u; st.fr[1] = 0x40400000u; st.fr[2] = 0x3F800000u;
    expect("fmac", fpuemu_exec(&st, gpr, &sr, 0xF21E, &mem) == FPU_OK && st.fr[2] == 0x40E00000u);

    /* non-FPU opcodes */
    expect("not fpu", fpuemu_exec(&st, gpr, &sr, 0x6003, &mem) == FPU_NOT_FPU);
    expect("is_fpu", fpuemu_is_fpu_op(0xF123) && fpuemu_is_fpu_op(0x405A) && !fpuemu_is_fpu_op(0xF12F) && !fpuemu_is_fpu_op(0x0009));

    /* fpuemu_run: fldi1 fr1; fldi1 fr2; fadd fr2,fr1; nop  -> executes 3, stops at nop */
    {
        sf_u32 pc = 0; int n = 0;
        code[0] = 0xF19D; code[1] = 0xF29D; code[2] = 0xF120; code[3] = 0x0009;
        st.fpscr = 0;
        expect("run", fpuemu_run(&st, gpr, &sr, &pc, &mem, 16, &n) == FPU_OK && n == 3 && pc == 6 && st.fr[1] == 0x40000000u);
    }
    /* fpuemu_exec_batch: fldi1 fr1; fldi1 fr2; fadd fr2,fr1; fmul fr1,fr1 -> fr1 = 4.0 */
    {
        sf_u16 batch[4] = { 0xF19D, 0xF29D, 0xF120, 0xF112 };
        int done = 0;
        st.fpscr = 0;
        expect("batch ok", fpuemu_exec_batch(&st, &sr, batch, 4, &done) == FPU_OK && done == 4 && st.fr[1] == 0x40800000u);
    }
}

int main(void)
{
    int n = 20000;
    printf("softfloat vs host FPU (%d samples per op/mode)\n", n);
    test_binary32("add32", 0, n); test_binary32("sub32", 1, n);
    test_binary32("mul32", 2, n); test_binary32("div32", 3, n);
    test_binary64("add64", 0, n); test_binary64("sub64", 1, n);
    test_binary64("mul64", 2, n); test_binary64("div64", 3, n);
    test_unary(n);
    test_fmac(n);
    test_compare(n);
    test_dn();
    test_insns();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
