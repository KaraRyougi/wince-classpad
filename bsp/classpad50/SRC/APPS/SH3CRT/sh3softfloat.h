//------------------------------------------------------------------------------
//  sh3softfloat.h - pure-integer IEEE-754 single/double soft-float for SH3CRT
//
//  SH3CRT.dll serves Pocket PC SH-3 binaries (Math Xpander, MFC CE 3.0) on
//  SH-4 WinCE 5.0 running under the translator. Implementing the core IEEE-754
//  operations with integer arithmetic avoids SH-4 FPU instructions altogether,
//  eliminating DBT FPU-emulation helper calls and FPU-disable traps.
//
//  Strict C89 (MSVC shcl /W3 /WX compatible), zero 64-bit hardware divisions,
//  round-to-nearest-even (RNE). Verified on the host by bsp/tests/test_sh3crt.
//------------------------------------------------------------------------------
#ifndef __SH3SOFTFLOAT_H
#define __SH3SOFTFLOAT_H

#ifdef _MSC_VER
typedef unsigned int       SF_U32;
typedef int                SF_I32;
typedef unsigned __int64   SF_U64;
typedef __int64            SF_I64;
#define SF_U64_C(x)        (x##ui64)
#define SF_INLINE          static __inline
#else
#include <stdint.h>
typedef uint32_t           SF_U32;
typedef int32_t            SF_I32;
typedef uint64_t           SF_U64;
typedef int64_t            SF_I64;
#define SF_U64_C(x)        (x##ULL)
#define SF_INLINE          static inline
#endif

#define SF_F32_SIGN        0x80000000u
#define SF_F32_QNAN        0x7FC00000u
#define SF_F32_INF         0x7F800000u
#define SF_F64_SIGN        SF_U64_C(0x8000000000000000)
#define SF_F64_QNAN        SF_U64_C(0x7FF8000000000000)
#define SF_F64_INF         SF_U64_C(0x7FF0000000000000)

SF_INLINE int sf_clz32(SF_U32 x)
{
    int n = 0;
    if (!x) return 32;
    if (!(x & 0xFFFF0000u)) { n += 16; x <<= 16; }
    if (!(x & 0xFF000000u)) { n += 8;  x <<= 8;  }
    if (!(x & 0xF0000000u)) { n += 4;  x <<= 4;  }
    if (!(x & 0xC0000000u)) { n += 2;  x <<= 2;  }
    if (!(x & 0x80000000u)) { n += 1; }
    return n;
}

SF_INLINE int sf_clz64(SF_U64 x)
{
    SF_U32 hi = (SF_U32)(x >> 32);
    return hi ? sf_clz32(hi) : 32 + sf_clz32((SF_U32)x);
}

SF_INLINE SF_U32 sf_rshift_sticky32(SF_U32 x, int n)
{
    if (n <= 0) return x;
    if (n >= 32) return x != 0;
    return (x >> n) | ((x & ((1u << n) - 1u)) != 0);
}

SF_INLINE SF_U64 sf_rshift_sticky64(SF_U64 x, int n)
{
    if (n <= 0) return x;
    if (n >= 64) return x != 0;
    return (x >> n) | ((x & ((SF_U64_C(1) << n) - SF_U64_C(1))) != 0);
}

SF_INLINE void sf_mul64x64(SF_U64 a, SF_U64 b, SF_U64 *hi, SF_U64 *lo)
{
    SF_U32 a0 = (SF_U32)a, a1 = (SF_U32)(a >> 32);
    SF_U32 b0 = (SF_U32)b, b1 = (SF_U32)(b >> 32);
    SF_U64 p00 = (SF_U64)a0 * b0;
    SF_U64 p01 = (SF_U64)a0 * b1;
    SF_U64 p10 = (SF_U64)a1 * b0;
    SF_U64 p11 = (SF_U64)a1 * b1;
    SF_U64 mid = (p00 >> 32) + (SF_U32)p01 + (SF_U32)p10;
    *lo = (p00 & SF_U64_C(0xFFFFFFFF)) | (mid << 32);
    *hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}

static SF_U32 sf_f32_pack_rne(SF_U32 sign, int exp, SF_U32 sig)
{
    SF_U32 low;
    if (sig == 0) return sign;
    if (exp <= 0) {
        sig = sf_rshift_sticky32(sig, 1 - exp);
        exp = 0;
    }
    low = sig & 0x7Fu;
    sig >>= 7;
    if (low > 0x40u || (low == 0x40u && (sig & 1u))) {
        sig++;
        if (sig == 0x01000000u) {
            sig >>= 1;
            exp++;
        } else if (exp == 0 && sig == 0x00800000u) {
            exp = 1;
        }
    }
    if (exp >= 0xFF) return sign | SF_F32_INF;
    if (exp == 0) return sign | (sig & 0x007FFFFFu);
    return sign | ((SF_U32)exp << 23) | (sig & 0x007FFFFFu);
}

static SF_U64 sf_f64_pack_rne(SF_U64 sign, int exp, SF_U64 sig)
{
    SF_U32 low;
    if (sig == 0) return sign;
    if (exp <= 0) {
        sig = sf_rshift_sticky64(sig, 1 - exp);
        exp = 0;
    }
    low = (SF_U32)(sig & SF_U64_C(0x3FF));
    sig >>= 10;
    if (low > 0x200u || (low == 0x200u && (sig & SF_U64_C(1)))) {
        sig++;
        if (sig == (SF_U64_C(1) << 53)) {
            sig >>= 1;
            exp++;
        } else if (exp == 0 && sig == (SF_U64_C(1) << 52)) {
            exp = 1;
        }
    }
    if (exp >= 0x7FF) return sign | SF_F64_INF;
    if (exp == 0) return sign | (sig & SF_U64_C(0x000FFFFFFFFFFFFF));
    return sign | ((SF_U64)exp << 52) | (sig & SF_U64_C(0x000FFFFFFFFFFFFF));
}

static int sf_f32_isnan(SF_U32 a) { return (a & 0x7FFFFFFFu) > SF_F32_INF; }
static int sf_f64_isnan(SF_U64 a) { return (a & ~SF_F64_SIGN) > SF_F64_INF; }

static SF_U32 sf_f32_add_core(SF_U32 a, SF_U32 b)
{
    SF_U32 sa = a & SF_F32_SIGN, sb = b & SF_F32_SIGN;
    int ea = (int)((a >> 23) & 0xFFu), eb = (int)((b >> 23) & 0xFFu);
    SF_U32 fa = a & 0x007FFFFFu, fb = b & 0x007FFFFFu;
    SF_U32 siga, sigb, sig;
    int exp, diff, sh;

    if (ea == 0xFF || eb == 0xFF) {
        if (sf_f32_isnan(a)) return a | 0x00400000u;
        if (sf_f32_isnan(b)) return b | 0x00400000u;
        if (ea == 0xFF && eb == 0xFF && sa != sb) return SF_F32_QNAN;
        return ea == 0xFF ? a : b;
    }
    if (ea == 0) { if (fa == 0) { if (eb == 0 && fb == 0) return (sa == sb) ? sa : 0u; return b; } ea = 1; siga = fa << 7; }
    else siga = (fa | 0x00800000u) << 7;
    if (eb == 0) { if (fb == 0) return a; eb = 1; sigb = fb << 7; }
    else sigb = (fb | 0x00800000u) << 7;

    if (ea < eb || (ea == eb && siga < sigb)) {
        SF_U32 ts = sa; int te = ea; SF_U32 tg = siga;
        sa = sb; ea = eb; siga = sigb;
        sb = ts; eb = te; sigb = tg;
    }
    diff = ea - eb;
    sigb = sf_rshift_sticky32(sigb, diff);
    exp = ea;
    if (sa == sb) {
        sig = siga + sigb;
        if (sig & 0x80000000u) {
            sig = (sig >> 1) | (sig & 1u);
            exp++;
        } else if (exp == 1 && !(sig & 0x40000000u)) {
            return sa | (sig >> 7);
        }
        return sf_f32_pack_rne(sa, exp, sig);
    }
    sig = siga - sigb;
    if (sig == 0) return 0u;
    sh = sf_clz32(sig) - 1;
    if (sh > 0) { sig <<= sh; exp -= sh; }
    return sf_f32_pack_rne(sa, exp, sig);
}

static SF_U32 sf_f32_add(SF_U32 a, SF_U32 b) { return sf_f32_add_core(a, b); }
static SF_U32 sf_f32_sub(SF_U32 a, SF_U32 b) { return sf_f32_add_core(a, b ^ SF_F32_SIGN); }

static SF_U32 sf_f32_mul(SF_U32 a, SF_U32 b)
{
    SF_U32 sign = (a ^ b) & SF_F32_SIGN;
    int ea = (int)((a >> 23) & 0xFFu), eb = (int)((b >> 23) & 0xFFu), exp, sh;
    SF_U32 fa = a & 0x007FFFFFu, fb = b & 0x007FFFFFu;
    SF_U64 prod;
    SF_U32 sig;

    if (ea == 0xFF || eb == 0xFF) {
        if (sf_f32_isnan(a)) return a | 0x00400000u;
        if (sf_f32_isnan(b)) return b | 0x00400000u;
        if ((ea == 0 && fa == 0) || (eb == 0 && fb == 0)) return SF_F32_QNAN;
        return sign | SF_F32_INF;
    }
    if (ea == 0) { if (fa == 0) return sign; sh = sf_clz32(fa) - 8; fa <<= sh; ea = 1 - sh; }
    else fa |= 0x00800000u;
    if (eb == 0) { if (fb == 0) return sign; sh = sf_clz32(fb) - 8; fb <<= sh; eb = 1 - sh; }
    else fb |= 0x00800000u;

    exp = ea + eb - 127;
    prod = (SF_U64)fa * fb; /* 47 or 48 bits */
    if (prod & (SF_U64_C(1) << 47)) {
        sig = (SF_U32)((prod >> 17) | ((prod & SF_U64_C(0x1FFFF)) != 0));
        exp++;
    } else {
        sig = (SF_U32)((prod >> 16) | ((prod & SF_U64_C(0xFFFF)) != 0));
    }
    return sf_f32_pack_rne(sign, exp, sig);
}

static SF_U32 sf_f32_div(SF_U32 a, SF_U32 b)
{
    SF_U32 sign = (a ^ b) & SF_F32_SIGN;
    int ea = (int)((a >> 23) & 0xFFu), eb = (int)((b >> 23) & 0xFFu), exp, sh, i;
    SF_U32 fa = a & 0x007FFFFFu, fb = b & 0x007FFFFFu;
    SF_U32 rem, q = 0;

    if (ea == 0xFF || eb == 0xFF) {
        if (sf_f32_isnan(a)) return a | 0x00400000u;
        if (sf_f32_isnan(b)) return b | 0x00400000u;
        if (ea == 0xFF && eb == 0xFF) return SF_F32_QNAN;
        return ea == 0xFF ? (sign | SF_F32_INF) : sign;
    }
    if (eb == 0 && fb == 0) {
        if (ea == 0 && fa == 0) return SF_F32_QNAN;
        return sign | SF_F32_INF;
    }
    if (ea == 0 && fa == 0) return sign;
    if (ea == 0) { sh = sf_clz32(fa) - 8; fa <<= sh; ea = 1 - sh; } else fa |= 0x00800000u;
    if (eb == 0) { sh = sf_clz32(fb) - 8; fb <<= sh; eb = 1 - sh; } else fb |= 0x00800000u;

    exp = ea - eb + 127;
    if (fa < fb) { fa <<= 1; exp--; }
    rem = fa;
    for (i = 25; i >= 0; i--) {
        if (rem >= fb) { rem -= fb; q |= 1u << i; }
        rem <<= 1;
    }
    q = (q << 5) | (rem != 0);
    return sf_f32_pack_rne(sign, exp, q);
}

static int sf_f32_eq(SF_U32 a, SF_U32 b)
{
    if (sf_f32_isnan(a) || sf_f32_isnan(b)) return 0;
    return a == b || ((a | b) & 0x7FFFFFFFu) == 0;
}

static int sf_f32_lt(SF_U32 a, SF_U32 b)
{
    SF_U32 sa, sb;
    if (sf_f32_isnan(a) || sf_f32_isnan(b)) return 0;
    if (((a | b) & 0x7FFFFFFFu) == 0) return 0;
    sa = a & SF_F32_SIGN; sb = b & SF_F32_SIGN;
    if (sa != sb) return sa != 0;
    return sa ? (a > b) : (a < b);
}

static int sf_f32_le(SF_U32 a, SF_U32 b)
{
    if (sf_f32_isnan(a) || sf_f32_isnan(b)) return 0;
    return sf_f32_eq(a, b) || sf_f32_lt(a, b);
}

static SF_U32 sf_u32_to_f32(SF_U32 u)
{
    int sh;
    if (u == 0) return 0u;
    sh = sf_clz32(u);
    /* align MSB to bit 30 for sf_f32_pack_rne */
    if (sh == 0) return sf_f32_pack_rne(0u, 127 + 31, (u >> 1) | (u & 1u));
    return sf_f32_pack_rne(0u, 127 + 31 - sh, u << (sh - 1));
}

static SF_U32 sf_i32_to_f32(SF_I32 i)
{
    SF_U32 sign = (i < 0) ? SF_F32_SIGN : 0u;
    SF_U32 u = (i < 0) ? (SF_U32)(0u - (SF_U32)i) : (SF_U32)i;
    return sign | sf_u32_to_f32(u);
}

static SF_I32 sf_f32_to_i32(SF_U32 a)
{
    SF_U32 sign = a & SF_F32_SIGN;
    int exp = (int)((a >> 23) & 0xFFu) - 127;
    SF_U32 sig = (a & 0x007FFFFFu) | 0x00800000u;
    SF_U32 mag;

    if (exp < 0) return 0;
    if (exp >= 31) {
        if (sf_f32_isnan(a)) return (SF_I32)0x80000000u;
        return sign ? (SF_I32)0x80000000u : 0x7FFFFFFF;
    }
    mag = (exp >= 23) ? (sig << (exp - 23)) : (sig >> (23 - exp));
    return sign ? -(SF_I32)mag : (SF_I32)mag;
}

static SF_U32 sf_f32_to_u32(SF_U32 a)
{
    SF_U32 sign = a & SF_F32_SIGN;
    int exp = (int)((a >> 23) & 0xFFu) - 127;
    SF_U32 sig = (a & 0x007FFFFFu) | 0x00800000u;

    if (sign || exp < 0 || sf_f32_isnan(a)) return 0u;
    if (exp >= 32) return 0xFFFFFFFFu;
    return (exp >= 23) ? (sig << (exp - 23)) : (sig >> (23 - exp));
}

static SF_U64 sf_f64_add_core(SF_U64 a, SF_U64 b)
{
    SF_U64 sa = a & SF_F64_SIGN, sb = b & SF_F64_SIGN;
    int ea = (int)((a >> 52) & 0x7FFu), eb = (int)((b >> 52) & 0x7FFu);
    SF_U64 fa = a & SF_U64_C(0x000FFFFFFFFFFFFF), fb = b & SF_U64_C(0x000FFFFFFFFFFFFF);
    SF_U64 siga, sigb, sig;
    int exp, diff, sh;

    if (ea == 0x7FF || eb == 0x7FF) {
        if (sf_f64_isnan(a)) return a | SF_U64_C(0x0008000000000000);
        if (sf_f64_isnan(b)) return b | SF_U64_C(0x0008000000000000);
        if (ea == 0x7FF && eb == 0x7FF && sa != sb) return SF_F64_QNAN;
        return ea == 0x7FF ? a : b;
    }
    if (ea == 0) { if (fa == 0) { if (eb == 0 && fb == 0) return (sa == sb) ? sa : SF_U64_C(0); return b; } ea = 1; siga = fa << 10; }
    else siga = (fa | (SF_U64_C(1) << 52)) << 10;
    if (eb == 0) { if (fb == 0) return a; eb = 1; sigb = fb << 10; }
    else sigb = (fb | (SF_U64_C(1) << 52)) << 10;

    if (ea < eb || (ea == eb && siga < sigb)) {
        SF_U64 ts = sa; int te = ea; SF_U64 tg = siga;
        sa = sb; ea = eb; siga = sigb;
        sb = ts; eb = te; sigb = tg;
    }
    diff = ea - eb;
    sigb = sf_rshift_sticky64(sigb, diff);
    exp = ea;
    if (sa == sb) {
        sig = siga + sigb;
        if (sig & SF_F64_SIGN) {
            sig = (sig >> 1) | (sig & SF_U64_C(1));
            exp++;
        } else if (exp == 1 && !(sig & (SF_U64_C(1) << 62))) {
            return sa | (sig >> 10);
        }
        return sf_f64_pack_rne(sa, exp, sig);
    }
    sig = siga - sigb;
    if (sig == 0) return SF_U64_C(0);
    sh = sf_clz64(sig) - 1;
    if (sh > 0) { sig <<= sh; exp -= sh; }
    return sf_f64_pack_rne(sa, exp, sig);
}

static SF_U64 sf_f64_add(SF_U64 a, SF_U64 b) { return sf_f64_add_core(a, b); }
static SF_U64 sf_f64_sub(SF_U64 a, SF_U64 b) { return sf_f64_add_core(a, b ^ SF_F64_SIGN); }

static SF_U64 sf_f64_mul(SF_U64 a, SF_U64 b)
{
    SF_U64 sign = (a ^ b) & SF_F64_SIGN;
    int ea = (int)((a >> 52) & 0x7FFu), eb = (int)((b >> 52) & 0x7FFu), exp, sh;
    SF_U64 fa = a & SF_U64_C(0x000FFFFFFFFFFFFF), fb = b & SF_U64_C(0x000FFFFFFFFFFFFF);
    SF_U64 hi, lo, sig;

    if (ea == 0x7FF || eb == 0x7FF) {
        if (sf_f64_isnan(a)) return a | SF_U64_C(0x0008000000000000);
        if (sf_f64_isnan(b)) return b | SF_U64_C(0x0008000000000000);
        if ((ea == 0 && fa == 0) || (eb == 0 && fb == 0)) return SF_F64_QNAN;
        return sign | SF_F64_INF;
    }
    if (ea == 0) { if (fa == 0) return sign; sh = sf_clz64(fa) - 11; fa <<= sh; ea = 1 - sh; }
    else fa |= SF_U64_C(1) << 52;
    if (eb == 0) { if (fb == 0) return sign; sh = sf_clz64(fb) - 11; fb <<= sh; eb = 1 - sh; }
    else fb |= SF_U64_C(1) << 52;

    exp = ea + eb - 1023;
    sf_mul64x64(fa << 11, fb << 11, &hi, &lo);
    /* hi has MSB at bit 62 (prod < 2) or bit 63 (prod >= 2) */
    if (hi & SF_F64_SIGN) {
        sig = (hi >> 1) | ((hi & SF_U64_C(1)) | (lo != 0));
        exp++;
    } else {
        sig = hi | (lo != 0);
    }
    return sf_f64_pack_rne(sign, exp, sig);
}

static SF_U64 sf_f64_div(SF_U64 a, SF_U64 b)
{
    SF_U64 sign = (a ^ b) & SF_F64_SIGN;
    int ea = (int)((a >> 52) & 0x7FFu), eb = (int)((b >> 52) & 0x7FFu), exp, sh, i;
    SF_U64 fa = a & SF_U64_C(0x000FFFFFFFFFFFFF), fb = b & SF_U64_C(0x000FFFFFFFFFFFFF);
    SF_U64 rem, q = 0;

    if (ea == 0x7FF || eb == 0x7FF) {
        if (sf_f64_isnan(a)) return a | SF_U64_C(0x0008000000000000);
        if (sf_f64_isnan(b)) return b | SF_U64_C(0x0008000000000000);
        if (ea == 0x7FF && eb == 0x7FF) return SF_F64_QNAN;
        return ea == 0x7FF ? (sign | SF_F64_INF) : sign;
    }
    if (eb == 0 && fb == 0) {
        if (ea == 0 && fa == 0) return SF_F64_QNAN;
        return sign | SF_F64_INF;
    }
    if (ea == 0 && fa == 0) return sign;
    if (ea == 0) { sh = sf_clz64(fa) - 11; fa <<= sh; ea = 1 - sh; } else fa |= SF_U64_C(1) << 52;
    if (eb == 0) { sh = sf_clz64(fb) - 11; fb <<= sh; eb = 1 - sh; } else fb |= SF_U64_C(1) << 52;

    exp = ea - eb + 1023;
    if (fa < fb) { fa <<= 1; exp--; }
    rem = fa;
    for (i = 54; i >= 0; i--) {
        if (rem >= fb) { rem -= fb; q |= SF_U64_C(1) << i; }
        rem <<= 1;
    }
    q = (q << 8) | (rem != 0);
    return sf_f64_pack_rne(sign, exp, q);
}

static SF_U64 sf_f64_sqrt(SF_U64 a)
{
    SF_U64 sign = a & SF_F64_SIGN;
    int exp = (int)((a >> 52) & 0x7FFu), sh, i;
    SF_U64 frac = a & SF_U64_C(0x000FFFFFFFFFFFFF);
    SF_U64 rem = 0, q = 0, top;

    if (exp == 0x7FF) {
        if (frac != 0) return a | SF_U64_C(0x0008000000000000);
        return sign ? SF_F64_QNAN : a;
    }
    if (exp == 0 && frac == 0) return a; /* preserve -0.0 */
    if (sign) return SF_F64_QNAN;
    if (exp == 0) { sh = sf_clz64(frac) - 11; frac <<= sh; exp = 1 - sh; }
    else frac |= SF_U64_C(1) << 52;

    exp -= 1023;
    if (exp & 1) { frac <<= 1; exp--; }
    exp = (exp >> 1) + 1023;
    /* frac is 53 or 54 bits in [1, 4)*2^52; compute 55-bit integer sqrt of frac * 2^56 */
    top = frac << 10; /* 64 bits: bits 63..10 hold frac, bits 9..0 are 0 */
    for (i = 0; i < 55; i++) {
        SF_U64 trial;
        rem = (rem << 2) | (top >> 62);
        top <<= 2;
        q <<= 1;
        trial = (q << 1) | SF_U64_C(1);
        if (rem >= trial) { rem -= trial; q |= SF_U64_C(1); }
    }
    /* q has 55 bits (bit 54 set); shift to bit 62 with sticky from rem */
    return sf_f64_pack_rne(SF_U64_C(0), exp, (q << 8) | (rem != 0));
}

static int sf_f64_eq(SF_U64 a, SF_U64 b)
{
    if (sf_f64_isnan(a) || sf_f64_isnan(b)) return 0;
    return a == b || ((a | b) & ~SF_F64_SIGN) == 0;
}

static int sf_f64_lt(SF_U64 a, SF_U64 b)
{
    SF_U64 sa, sb;
    if (sf_f64_isnan(a) || sf_f64_isnan(b)) return 0;
    if (((a | b) & ~SF_F64_SIGN) == 0) return 0;
    sa = a & SF_F64_SIGN; sb = b & SF_F64_SIGN;
    if (sa != sb) return sa != 0;
    return sa ? (a > b) : (a < b);
}

static int sf_f64_le(SF_U64 a, SF_U64 b)
{
    if (sf_f64_isnan(a) || sf_f64_isnan(b)) return 0;
    return sf_f64_eq(a, b) || sf_f64_lt(a, b);
}

static SF_U32 sf_f64_to_f32(SF_U64 a)
{
    SF_U32 sign = (SF_U32)((a >> 32) & SF_F32_SIGN);
    int exp = (int)((a >> 52) & 0x7FFu);
    SF_U64 frac = a & SF_U64_C(0x000FFFFFFFFFFFFF);
    SF_U32 sig;

    if (exp == 0x7FF) {
        if (frac != 0) return sign | SF_F32_QNAN | (SF_U32)(frac >> 29);
        return sign | SF_F32_INF;
    }
    if (exp == 0 && frac == 0) return sign;
    exp = exp - 1023 + 127;
    sig = (SF_U32)(((frac | (SF_U64_C(1) << 52)) >> 22) | ((frac & SF_U64_C(0x3FFFFF)) != 0));
    return sf_f32_pack_rne(sign, exp, sig);
}

static SF_U64 sf_f32_to_f64(SF_U32 a)
{
    SF_U64 sign = (SF_U64)(a & SF_F32_SIGN) << 32;
    int exp = (int)((a >> 23) & 0xFFu), sh;
    SF_U32 frac = a & 0x007FFFFFu;

    if (exp == 0xFF) {
        if (frac != 0) return sign | SF_F64_QNAN | ((SF_U64)frac << 29);
        return sign | SF_F64_INF;
    }
    if (exp == 0) {
        if (frac == 0) return sign;
        sh = sf_clz32(frac) - 8;
        frac = (frac << sh) & 0x007FFFFFu;
        exp = 1 - sh;
    }
    return sign | ((SF_U64)(exp - 127 + 1023) << 52) | ((SF_U64)frac << 29);
}

static SF_U64 sf_u64_to_f64(SF_U64 u)
{
    int sh;
    if (u == 0) return SF_U64_C(0);
    sh = sf_clz64(u);
    if (sh == 0) return sf_f64_pack_rne(SF_U64_C(0), 1023 + 63, (u >> 1) | (u & SF_U64_C(1)));
    return sf_f64_pack_rne(SF_U64_C(0), 1023 + 63 - sh, u << (sh - 1));
}

static SF_U64 sf_i64_to_f64(SF_I64 i)
{
    SF_U64 sign = (i < 0) ? SF_F64_SIGN : SF_U64_C(0);
    SF_U64 u = (i < 0) ? (SF_U64_C(0) - (SF_U64)i) : (SF_U64)i;
    return sign | sf_u64_to_f64(u);
}

static SF_U64 sf_i32_to_f64(SF_I32 i) { return sf_i64_to_f64((SF_I64)i); }
static SF_U64 sf_u32_to_f64(SF_U32 u) { return sf_u64_to_f64((SF_U64)u); }

static SF_I32 sf_f64_to_i32(SF_U64 a)
{
    SF_U64 sign = a & SF_F64_SIGN;
    int exp = (int)((a >> 52) & 0x7FFu) - 1023;
    SF_U64 sig = (a & SF_U64_C(0x000FFFFFFFFFFFFF)) | (SF_U64_C(1) << 52);
    SF_U32 mag;

    if (exp < 0) return 0;
    if (exp >= 31) {
        if (sf_f64_isnan(a)) return (SF_I32)0x80000000u;
        return sign ? (SF_I32)0x80000000u : 0x7FFFFFFF;
    }
    mag = (SF_U32)(sig >> (52 - exp));
    return sign ? -(SF_I32)mag : (SF_I32)mag;
}

static SF_U32 sf_f64_to_u32(SF_U64 a)
{
    SF_U64 sign = a & SF_F64_SIGN;
    int exp = (int)((a >> 52) & 0x7FFu) - 1023;
    SF_U64 sig = (a & SF_U64_C(0x000FFFFFFFFFFFFF)) | (SF_U64_C(1) << 52);

    if (sign || exp < 0 || sf_f64_isnan(a)) return 0u;
    if (exp >= 32) return 0xFFFFFFFFu;
    return (SF_U32)(sig >> (52 - exp));
}

static SF_I64 sf_f64_to_i64(SF_U64 a)
{
    SF_U64 sign = a & SF_F64_SIGN;
    int exp = (int)((a >> 52) & 0x7FFu) - 1023;
    SF_U64 sig = (a & SF_U64_C(0x000FFFFFFFFFFFFF)) | (SF_U64_C(1) << 52);
    SF_U64 mag;

    if (exp < 0) return 0;
    if (exp >= 63) {
        if (sf_f64_isnan(a)) return (SF_I64)SF_F64_SIGN;
        return sign ? (SF_I64)SF_F64_SIGN : (SF_I64)SF_U64_C(0x7FFFFFFFFFFFFFFF);
    }
    mag = (exp >= 52) ? (sig << (exp - 52)) : (sig >> (52 - exp));
    return sign ? -(SF_I64)mag : (SF_I64)mag;
}

static SF_U64 sf_f64_to_u64(SF_U64 a)
{
    SF_U64 sign = a & SF_F64_SIGN;
    int exp = (int)((a >> 52) & 0x7FFu) - 1023;
    SF_U64 sig = (a & SF_U64_C(0x000FFFFFFFFFFFFF)) | (SF_U64_C(1) << 52);

    if (sign || exp < 0 || sf_f64_isnan(a)) return SF_U64_C(0);
    if (exp >= 64) return SF_U64_C(0xFFFFFFFFFFFFFFFF);
    return (exp >= 52) ? (sig << (exp - 52)) : (sig >> (52 - exp));
}

static SF_U64 sf_f64_floor(SF_U64 a)
{
    SF_U64 sign = a & SF_F64_SIGN, mask, trunc;
    int exp = (int)((a >> 52) & 0x7FFu);
    if (exp >= 1023 + 52) return sf_f64_isnan(a) ? (a | SF_U64_C(0x0008000000000000)) : a;
    if ((a & ~SF_F64_SIGN) == 0) return a;
    if (exp < 1023) return sign ? SF_U64_C(0xBFF0000000000000) : SF_U64_C(0);
    mask = (SF_U64_C(1) << (52 - (exp - 1023))) - SF_U64_C(1);
    if ((a & mask) == 0) return a;
    trunc = a & ~mask;
    if (sign) trunc += mask + SF_U64_C(1);
    return trunc;
}

static SF_U64 sf_f64_ceil(SF_U64 a)
{
    SF_U64 sign = a & SF_F64_SIGN, mask, trunc;
    int exp = (int)((a >> 52) & 0x7FFu);
    if (exp >= 1023 + 52) return sf_f64_isnan(a) ? (a | SF_U64_C(0x0008000000000000)) : a;
    if ((a & ~SF_F64_SIGN) == 0) return a;
    if (exp < 1023) return sign ? SF_F64_SIGN : SF_U64_C(0x3FF0000000000000);
    mask = (SF_U64_C(1) << (52 - (exp - 1023))) - SF_U64_C(1);
    if ((a & mask) == 0) return a;
    trunc = a & ~mask;
    if (!sign) trunc += mask + SF_U64_C(1);
    return trunc;
}

#endif
