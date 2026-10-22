/*
 * softfloat.c - integer-only IEEE 754 arithmetic with SH-4 FPU semantics.
 *
 * Internal representation: a finite nonzero value is (sign, e, sig) with
 * sig a 64-bit integer whose bit 63 is the leading one, value = sig * 2^(e-63).
 * Bit 0 of sig doubles as a sticky bit ("something nonzero below").
 *
 * Algorithms are the textbook ones (align/add, 128-bit product, restoring
 * division, digit-by-digit square root). Correct rounding is the goal;
 * speed is tuned later in the kernel build.
 */
#include "softfloat.h"

#define CLS_ZERO   0
#define CLS_NORMAL 1
#define CLS_INF    2
#define CLS_QNAN   3
#define CLS_SNAN   4

typedef struct { int cls; int sign; int e; sf_u64 sig; } unp;

/* ---------------------------------------------------------------- bits */

static int clz64(sf_u64 x)
{
#if defined(__GNUC__) || defined(__clang__)
    return x ? __builtin_clzll(x) : 64;
#else
    int n = 0;
    if (!x) return 64;
    while (!(x & 0x8000000000000000ull)) { x <<= 1; n++; }
    return n;
#endif
}

/* Shift right by n (n may be >= 64), OR-ing the shifted-out bits into bit 0. */
static sf_u64 shr_sticky(sf_u64 x, int n)
{
    if (n <= 0) return x;
    if (n >= 64) return x ? 1u : 0u;
    {
        sf_u64 lost = x & ((1ull << n) - 1u);
        return (x >> n) | (lost ? 1u : 0u);
    }
}

/* 64x64 -> 128 */
static void mul64to128(sf_u64 a, sf_u64 b, sf_u64 *hi, sf_u64 *lo)
{
    sf_u64 a0 = a & 0xFFFFFFFFu, a1 = a >> 32;
    sf_u64 b0 = b & 0xFFFFFFFFu, b1 = b >> 32;
    sf_u64 p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    sf_u64 mid = (p00 >> 32) + (p01 & 0xFFFFFFFFu) + (p10 & 0xFFFFFFFFu);
    *lo = (p00 & 0xFFFFFFFFu) | (mid << 32);
    *hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}

/* (hi:lo) / d, where the quotient is known to fit in 64 bits. Restoring
 * division over `nbits` low bits of the numerator (the rest must be zero). */
static sf_u64 div128by64(sf_u64 hi, sf_u64 lo, sf_u64 d, int nbits, sf_u64 *rem_out)
{
    sf_u64 rem = 0, q = 0;
    int i;
    for (i = nbits - 1; i >= 0; i--) {
        sf_u64 bit = (i >= 64) ? ((hi >> (i - 64)) & 1u) : ((lo >> i) & 1u);
        sf_u64 top = rem >> 63;
        rem = (rem << 1) | bit;
        q <<= 1;
        if (top || rem >= d) { rem -= d; q |= 1u; }
    }
    *rem_out = rem;
    return q;
}

/* floor(sqrt(hi:lo)) for a 128-bit radicand; the root fits in 64 bits. */
static sf_u64 isqrt128(sf_u64 hi, sf_u64 lo, int *exact)
{
    sf_u64 rhi = 0, rlo = 0;   /* remainder, 128-bit */
    sf_u64 root = 0;
    int i;
    for (i = 63; i >= 0; i--) {
        sf_u64 two;
        sf_u64 thi, tlo;
        /* rem = rem*4 + next two bits of the radicand */
        two = (i >= 32) ? ((hi >> ((i - 32) * 2)) & 3u) : ((lo >> (i * 2)) & 3u);
        rhi = (rhi << 2) | (rlo >> 62);
        rlo = (rlo << 2) | two;
        /* trial = root*4 + 1 (fits in 128 bits) */
        thi = root >> 62;
        tlo = (root << 2) | 1u;
        if (rhi > thi || (rhi == thi && rlo >= tlo)) {
            sf_u64 nlo = rlo - tlo;
            rhi = rhi - thi - (rlo < tlo ? 1u : 0u);
            rlo = nlo;
            root = (root << 1) | 1u;
        } else {
            root <<= 1;
        }
    }
    *exact = (rhi == 0 && rlo == 0);
    return root;
}

/* ------------------------------------------------------- unpack / pack */

static unp unpack32(const sf_env *env, f32 a)
{
    unp u;
    sf_u32 ef = (a >> 23) & 0xFF, f = a & 0x7FFFFF;
    u.sign = (int)(a >> 31);
    u.e = 0; u.sig = 0;
    if (ef == 0xFF) {
        if (f == 0) u.cls = CLS_INF;
        else u.cls = (f & 0x400000) ? CLS_SNAN : CLS_QNAN;
    } else if (ef == 0) {
        if (f == 0 || env->dn) u.cls = CLS_ZERO;
        else {
            int k = clz64((sf_u64)f) - 40;      /* bring bit 23 up */
            u.cls = CLS_NORMAL;
            u.sig = ((sf_u64)f << k) << 40;
            u.e = -126 - k;
        }
    } else {
        u.cls = CLS_NORMAL;
        u.sig = ((sf_u64)(f | 0x800000u)) << 40;
        u.e = (int)ef - 127;
    }
    return u;
}

static unp unpack64(const sf_env *env, f64 a)
{
    unp u;
    sf_u32 ef = (sf_u32)((a >> 52) & 0x7FF);
    sf_u64 f = a & 0xFFFFFFFFFFFFFull;
    u.sign = (int)(a >> 63);
    u.e = 0; u.sig = 0;
    if (ef == 0x7FF) {
        if (f == 0) u.cls = CLS_INF;
        else u.cls = (f & 0x8000000000000ull) ? CLS_SNAN : CLS_QNAN;
    } else if (ef == 0) {
        if (f == 0 || env->dn) u.cls = CLS_ZERO;
        else {
            int k = clz64(f) - 11;              /* bring bit 52 up */
            u.cls = CLS_NORMAL;
            u.sig = (f << k) << 11;
            u.e = -1022 - k;
        }
    } else {
        u.cls = CLS_NORMAL;
        u.sig = (f | 0x10000000000000ull) << 11;
        u.e = (int)ef - 1023;
    }
    return u;
}

/* Round a normalized (sign, e, sig) to `prec` bits and encode. Returns the
 * raw bits without the sign; caller ORs the sign in. */
static sf_u64 round_pack(sf_env *env, int sign, int e, sf_u64 sig,
                         int prec, int emin, int emax, int bias)
{
    int shift = 64 - prec;
    sf_u64 half = 1ull << (shift - 1);
    sf_u64 mask = (1ull << shift) - 1u;
    sf_u64 kept, rem;
    int inexact;
    int denorm = 0;
    (void)sign;

    if (sig == 0) return 0;

    if (e > emax) {
        env->flags |= SF_FLAG_O | SF_FLAG_I;
        if (env->rm == SF_RZ)
            return ((sf_u64)(emax + bias) << (prec - 1)) | ((1ull << (prec - 1)) - 1u);
        return (sf_u64)(emax + bias + 1) << (prec - 1);        /* inf */
    }

    if (e < emin) {
        if (env->dn) {
            env->flags |= SF_FLAG_U | SF_FLAG_I;
            return 0;                                            /* flush */
        }
        sig = shr_sticky(sig, emin - e);
        denorm = 1;
    }

    rem = sig & mask;
    kept = sig >> shift;
    inexact = (rem != 0);
    if (env->rm == SF_RN) {
        if (rem > half || (rem == half && (kept & 1u))) kept++;
    }
    if (inexact) env->flags |= SF_FLAG_I;

    if (denorm) {
        if (inexact) env->flags |= SF_FLAG_U;
        /* kept == 2^(prec-1) here means it rounded up to the smallest
         * normal, whose encoding is exactly that bit pattern. */
        return kept;
    }

    if (kept >> prec) {              /* carry out of rounding */
        kept >>= 1;
        e++;
        if (e > emax) {
            env->flags |= SF_FLAG_O;
            if (env->rm == SF_RZ)
                return ((sf_u64)(emax + bias) << (prec - 1)) | ((1ull << (prec - 1)) - 1u);
            return (sf_u64)(emax + bias + 1) << (prec - 1);
        }
    }
    return ((sf_u64)(e + bias) << (prec - 1)) | (kept & ((1ull << (prec - 1)) - 1u));
}

static f32 pack32(sf_env *env, int sign, int e, sf_u64 sig)
{
    return ((sf_u32)sign << 31) | (sf_u32)round_pack(env, sign, e, sig, 24, -126, 127, 127);
}
static f64 pack64(sf_env *env, int sign, int e, sf_u64 sig)
{
    return ((sf_u64)sign << 63) | round_pack(env, sign, e, sig, 53, -1022, 1023, 1023);
}

#define F32_INF(s)  (((sf_u32)(s) << 31) | 0x7F800000u)
#define F64_INF(s)  (((sf_u64)(s) << 63) | 0x7FF0000000000000ull)
#define F32_ZERO(s) ((sf_u32)(s) << 31)
#define F64_ZERO(s) ((sf_u64)(s) << 63)

int f32_is_nan(f32 a) { return ((a >> 23) & 0xFF) == 0xFF && (a & 0x7FFFFF) != 0; }
int f64_is_nan(f64 a) { return ((a >> 52) & 0x7FF) == 0x7FF && (a & 0xFFFFFFFFFFFFFull) != 0; }

/* NaN propagation: any sNaN -> invalid + default qNaN; else return the qNaN
 * operand (first operand preferred). Returns 1 if a NaN result was produced. */
static int nan_result32(sf_env *env, const unp *a, const unp *b, f32 ra, f32 rb, f32 *out)
{
    int an = a->cls >= CLS_QNAN, bn = b && b->cls >= CLS_QNAN;
    if (!an && !bn) return 0;
    if (a->cls == CLS_SNAN || (b && b->cls == CLS_SNAN)) {
        env->flags |= SF_FLAG_V;
        *out = F32_DEFAULT_NAN;
    } else {
        *out = an ? ra : rb;
    }
    return 1;
}
static int nan_result64(sf_env *env, const unp *a, const unp *b, f64 ra, f64 rb, f64 *out)
{
    int an = a->cls >= CLS_QNAN, bn = b && b->cls >= CLS_QNAN;
    if (!an && !bn) return 0;
    if (a->cls == CLS_SNAN || (b && b->cls == CLS_SNAN)) {
        env->flags |= SF_FLAG_V;
        *out = F64_DEFAULT_NAN;
    } else {
        *out = an ? ra : rb;
    }
    return 1;
}

/* ------------------------------------------------------------ add core */

/* Both operands finite and nonzero, normalized. Result normalized with the
 * sticky bit in bit 0; *rsig == 0 means an exact zero. */
static void add_core(int sa, int ea, sf_u64 siga, int sb, int eb, sf_u64 sigb,
                     int *rs, int *re, sf_u64 *rsig)
{
    sf_u64 a, b, r;
    int e, s;
    /* make a the operand with the larger exponent (or magnitude) */
    if (ea < eb || (ea == eb && siga < sigb)) {
        int t; sf_u64 ts;
        t = sa; sa = sb; sb = t;
        t = ea; ea = eb; eb = t;
        ts = siga; siga = sigb; sigb = ts;
    }
    /* leave one bit of headroom for the carry */
    a = siga >> 1;
    b = shr_sticky(sigb >> 1, ea - eb);
    e = ea + 1;   /* value = a * 2^(e-63) with a's leading bit at 62 */
    if (sa == sb) {
        r = a + b;
        s = sa;
    } else {
        r = a - b;    /* a >= b by construction */
        s = sa;
        if (r == 0) { *rs = 0; *re = 0; *rsig = 0; return; }
    }
    {
        int k = clz64(r);
        r <<= k;
        e -= k;
    }
    *rs = s; *re = e; *rsig = r;
}

/* ------------------------------------------------------------- single */

static f32 f32_addsub(sf_env *env, f32 a, f32 b, int negate_b)
{
    unp ua = unpack32(env, a), ub = unpack32(env, b);
    f32 out;
    if (negate_b) { ub.sign ^= 1; b ^= 0x80000000u; }
    if (nan_result32(env, &ua, &ub, a, b, &out)) return out;
    if (ua.cls == CLS_INF && ub.cls == CLS_INF) {
        if (ua.sign != ub.sign) { env->flags |= SF_FLAG_V; return F32_DEFAULT_NAN; }
        return F32_INF(ua.sign);
    }
    if (ua.cls == CLS_INF) return F32_INF(ua.sign);
    if (ub.cls == CLS_INF) return F32_INF(ub.sign);
    if (ua.cls == CLS_ZERO && ub.cls == CLS_ZERO)
        return F32_ZERO(ua.sign & ub.sign);
    if (ua.cls == CLS_ZERO) return pack32(env, ub.sign, ub.e, ub.sig);
    if (ub.cls == CLS_ZERO) return pack32(env, ua.sign, ua.e, ua.sig);
    {
        int rs, re; sf_u64 rsig;
        add_core(ua.sign, ua.e, ua.sig, ub.sign, ub.e, ub.sig, &rs, &re, &rsig);
        if (rsig == 0) return F32_ZERO(0);
        return pack32(env, rs, re, rsig);
    }
}

f32 f32_add(sf_env *env, f32 a, f32 b) { return f32_addsub(env, a, b, 0); }
f32 f32_sub(sf_env *env, f32 a, f32 b) { return f32_addsub(env, a, b, 1); }

f32 f32_mul(sf_env *env, f32 a, f32 b)
{
    unp ua = unpack32(env, a), ub = unpack32(env, b);
    int sign = ua.sign ^ ub.sign;
    f32 out;
    if (nan_result32(env, &ua, &ub, a, b, &out)) return out;
    if (ua.cls == CLS_INF || ub.cls == CLS_INF) {
        if (ua.cls == CLS_ZERO || ub.cls == CLS_ZERO) { env->flags |= SF_FLAG_V; return F32_DEFAULT_NAN; }
        return F32_INF(sign);
    }
    if (ua.cls == CLS_ZERO || ub.cls == CLS_ZERO) return F32_ZERO(sign);
    {
        sf_u64 hi, lo;
        int e;
        mul64to128(ua.sig, ub.sig, &hi, &lo);
        e = ua.e + ub.e + 1;
        if (!(hi >> 63)) { hi = (hi << 1) | (lo >> 63); lo <<= 1; e--; }
        return pack32(env, sign, e, hi | (lo ? 1u : 0u));
    }
}

f32 f32_div(sf_env *env, f32 a, f32 b)
{
    unp ua = unpack32(env, a), ub = unpack32(env, b);
    int sign = ua.sign ^ ub.sign;
    f32 out;
    if (nan_result32(env, &ua, &ub, a, b, &out)) return out;
    if (ua.cls == CLS_INF) {
        if (ub.cls == CLS_INF) { env->flags |= SF_FLAG_V; return F32_DEFAULT_NAN; }
        return F32_INF(sign);
    }
    if (ub.cls == CLS_INF) return F32_ZERO(sign);
    if (ub.cls == CLS_ZERO) {
        if (ua.cls == CLS_ZERO) { env->flags |= SF_FLAG_V; return F32_DEFAULT_NAN; }
        env->flags |= SF_FLAG_Z;
        return F32_INF(sign);
    }
    if (ua.cls == CLS_ZERO) return F32_ZERO(sign);
    {
        /* 24-bit significands: (siga>>40) << 39 fits in 64 bits, native divide */
        sf_u64 na = (ua.sig >> 40) << 39;
        sf_u64 nb = ub.sig >> 40;
        sf_u64 q = na / nb, rem = na % nb;
        int k = clz64(q);
        int e = ua.e - ub.e + 24 - k;   /* q = (ma<<39)/mb, value = q*2^(-39+ea-eb) */
        q = (q << k) | (rem ? 1u : 0u);
        return pack32(env, sign, e, q);
    }
}

f32 f32_sqrt(sf_env *env, f32 a)
{
    unp ua = unpack32(env, a);
    f32 out;
    if (nan_result32(env, &ua, 0, a, 0, &out)) return out;
    if (ua.cls == CLS_ZERO) return a;
    if (ua.sign) { env->flags |= SF_FLAG_V; return F32_DEFAULT_NAN; }
    if (ua.cls == CLS_INF) return a;
    {
        int E = ua.e - 63;
        sf_u64 hi = 0, lo = ua.sig;
        sf_u64 r;
        int exact, k, e;
        if (E & 1) { hi = lo >> 63; lo <<= 1; E--; }
        /* radicand << 60 */
        hi = (hi << 60) | (lo >> 4);
        lo <<= 60;
        r = isqrt128(hi, lo, &exact);
        k = clz64(r);
        e = E / 2 + 33 - k;
        r = (r << k) | (exact ? 0u : 1u);
        return pack32(env, 0, e, r);
    }
}

/* a*b + c with a single rounding. The 48-bit product is exact in 64 bits. */
f32 f32_fmac(sf_env *env, f32 a, f32 b, f32 c)
{
    unp ua = unpack32(env, a), ub = unpack32(env, b), uc = unpack32(env, c);
    int psign = ua.sign ^ ub.sign;
    f32 out;
    /* NaN / invalid handling: product first, then the addend */
    if (nan_result32(env, &ua, &ub, a, b, &out)) {
        if (uc.cls == CLS_SNAN) { env->flags |= SF_FLAG_V; return F32_DEFAULT_NAN; }
        return out;
    }
    if (uc.cls >= CLS_QNAN) {
        if (uc.cls == CLS_SNAN) { env->flags |= SF_FLAG_V; return F32_DEFAULT_NAN; }
        if ((ua.cls == CLS_INF && ub.cls == CLS_ZERO) || (ub.cls == CLS_INF && ua.cls == CLS_ZERO)) {
            env->flags |= SF_FLAG_V; return F32_DEFAULT_NAN;
        }
        return c;
    }
    if (ua.cls == CLS_INF || ub.cls == CLS_INF) {
        if (ua.cls == CLS_ZERO || ub.cls == CLS_ZERO) { env->flags |= SF_FLAG_V; return F32_DEFAULT_NAN; }
        if (uc.cls == CLS_INF && uc.sign != psign) { env->flags |= SF_FLAG_V; return F32_DEFAULT_NAN; }
        return F32_INF(psign);
    }
    if (uc.cls == CLS_INF) return F32_INF(uc.sign);
    if (ua.cls == CLS_ZERO || ub.cls == CLS_ZERO) {
        if (uc.cls == CLS_ZERO) return F32_ZERO(psign & uc.sign);
        return pack32(env, uc.sign, uc.e, uc.sig);
    }
    {
        sf_u64 hi, lo;
        int pe;
        int rs, re; sf_u64 rsig;
        mul64to128(ua.sig, ub.sig, &hi, &lo);   /* lo is all zero: 24x24 bits */
        pe = ua.e + ub.e + 1;
        if (!(hi >> 63)) { hi <<= 1; pe--; }
        if (uc.cls == CLS_ZERO) return pack32(env, psign, pe, hi);
        add_core(psign, pe, hi, uc.sign, uc.e, uc.sig, &rs, &re, &rsig);
        if (rsig == 0) return F32_ZERO(0);
        return pack32(env, rs, re, rsig);
    }
}

int f32_eq(sf_env *env, f32 a, f32 b)
{
    unp ua = unpack32(env, a), ub = unpack32(env, b);
    if (ua.cls >= CLS_QNAN || ub.cls >= CLS_QNAN) {
        if (ua.cls == CLS_SNAN || ub.cls == CLS_SNAN) env->flags |= SF_FLAG_V;
        return 0;
    }
    if (ua.cls == CLS_ZERO && ub.cls == CLS_ZERO) return 1;
    return a == b;
}

int f32_gt(sf_env *env, f32 a, f32 b)
{
    unp ua = unpack32(env, a), ub = unpack32(env, b);
    sf_i32 ia, ib;
    if (ua.cls >= CLS_QNAN || ub.cls >= CLS_QNAN) { env->flags |= SF_FLAG_V; return 0; }
    if (ua.cls == CLS_ZERO && ub.cls == CLS_ZERO) return 0;
    if (ua.cls == CLS_ZERO) a = 0;    /* flushed denormal -> compare as zero */
    if (ub.cls == CLS_ZERO) b = 0;
    /* sign-magnitude to two's complement ordering */
    ia = (a & 0x80000000u) ? -(sf_i32)(a & 0x7FFFFFFFu) : (sf_i32)a;
    ib = (b & 0x80000000u) ? -(sf_i32)(b & 0x7FFFFFFFu) : (sf_i32)b;
    return ia > ib;
}

/* ------------------------------------------------------------- double */

static f64 f64_addsub(sf_env *env, f64 a, f64 b, int negate_b)
{
    unp ua = unpack64(env, a), ub = unpack64(env, b);
    f64 out;
    if (negate_b) { ub.sign ^= 1; b ^= 0x8000000000000000ull; }
    if (nan_result64(env, &ua, &ub, a, b, &out)) return out;
    if (ua.cls == CLS_INF && ub.cls == CLS_INF) {
        if (ua.sign != ub.sign) { env->flags |= SF_FLAG_V; return F64_DEFAULT_NAN; }
        return F64_INF(ua.sign);
    }
    if (ua.cls == CLS_INF) return F64_INF(ua.sign);
    if (ub.cls == CLS_INF) return F64_INF(ub.sign);
    if (ua.cls == CLS_ZERO && ub.cls == CLS_ZERO)
        return F64_ZERO(ua.sign & ub.sign);
    if (ua.cls == CLS_ZERO) return pack64(env, ub.sign, ub.e, ub.sig);
    if (ub.cls == CLS_ZERO) return pack64(env, ua.sign, ua.e, ua.sig);
    {
        int rs, re; sf_u64 rsig;
        add_core(ua.sign, ua.e, ua.sig, ub.sign, ub.e, ub.sig, &rs, &re, &rsig);
        if (rsig == 0) return F64_ZERO(0);
        return pack64(env, rs, re, rsig);
    }
}

f64 f64_add(sf_env *env, f64 a, f64 b) { return f64_addsub(env, a, b, 0); }
f64 f64_sub(sf_env *env, f64 a, f64 b) { return f64_addsub(env, a, b, 1); }

f64 f64_mul(sf_env *env, f64 a, f64 b)
{
    unp ua = unpack64(env, a), ub = unpack64(env, b);
    int sign = ua.sign ^ ub.sign;
    f64 out;
    if (nan_result64(env, &ua, &ub, a, b, &out)) return out;
    if (ua.cls == CLS_INF || ub.cls == CLS_INF) {
        if (ua.cls == CLS_ZERO || ub.cls == CLS_ZERO) { env->flags |= SF_FLAG_V; return F64_DEFAULT_NAN; }
        return F64_INF(sign);
    }
    if (ua.cls == CLS_ZERO || ub.cls == CLS_ZERO) return F64_ZERO(sign);
    {
        sf_u64 hi, lo;
        int e;
        mul64to128(ua.sig, ub.sig, &hi, &lo);
        e = ua.e + ub.e + 1;
        if (!(hi >> 63)) { hi = (hi << 1) | (lo >> 63); lo <<= 1; e--; }
        return pack64(env, sign, e, hi | (lo ? 1u : 0u));
    }
}

f64 f64_div(sf_env *env, f64 a, f64 b)
{
    unp ua = unpack64(env, a), ub = unpack64(env, b);
    int sign = ua.sign ^ ub.sign;
    f64 out;
    if (nan_result64(env, &ua, &ub, a, b, &out)) return out;
    if (ua.cls == CLS_INF) {
        if (ub.cls == CLS_INF) { env->flags |= SF_FLAG_V; return F64_DEFAULT_NAN; }
        return F64_INF(sign);
    }
    if (ub.cls == CLS_INF) return F64_ZERO(sign);
    if (ub.cls == CLS_ZERO) {
        if (ua.cls == CLS_ZERO) { env->flags |= SF_FLAG_V; return F64_DEFAULT_NAN; }
        env->flags |= SF_FLAG_Z;
        return F64_INF(sign);
    }
    if (ua.cls == CLS_ZERO) return F64_ZERO(sign);
    {
        /* numerator = siga << 62 (126 bits), quotient in (2^61, 2^63) */
        sf_u64 hi = ua.sig >> 2, lo = ua.sig << 62;
        sf_u64 rem, q;
        int k, e;
        q = div128by64(hi, lo, ub.sig, 126, &rem);
        k = clz64(q);
        e = ua.e - ub.e + 1 - k;
        q = (q << k) | (rem ? 1u : 0u);
        return pack64(env, sign, e, q);
    }
}

f64 f64_sqrt(sf_env *env, f64 a)
{
    unp ua = unpack64(env, a);
    f64 out;
    if (nan_result64(env, &ua, 0, a, 0, &out)) return out;
    if (ua.cls == CLS_ZERO) return a;
    if (ua.sign) { env->flags |= SF_FLAG_V; return F64_DEFAULT_NAN; }
    if (ua.cls == CLS_INF) return a;
    {
        int E = ua.e - 63;
        sf_u64 hi = 0, lo = ua.sig;
        sf_u64 r;
        int exact, k, e;
        if (E & 1) { hi = lo >> 63; lo <<= 1; E--; }
        hi = (hi << 60) | (lo >> 4);
        lo <<= 60;
        r = isqrt128(hi, lo, &exact);
        k = clz64(r);
        e = E / 2 + 33 - k;
        r = (r << k) | (exact ? 0u : 1u);
        return pack64(env, 0, e, r);
    }
}

int f64_eq(sf_env *env, f64 a, f64 b)
{
    unp ua = unpack64(env, a), ub = unpack64(env, b);
    if (ua.cls >= CLS_QNAN || ub.cls >= CLS_QNAN) {
        if (ua.cls == CLS_SNAN || ub.cls == CLS_SNAN) env->flags |= SF_FLAG_V;
        return 0;
    }
    if (ua.cls == CLS_ZERO && ub.cls == CLS_ZERO) return 1;
    return a == b;
}

int f64_gt(sf_env *env, f64 a, f64 b)
{
    unp ua = unpack64(env, a), ub = unpack64(env, b);
    sf_i64 ia, ib;
    if (ua.cls >= CLS_QNAN || ub.cls >= CLS_QNAN) { env->flags |= SF_FLAG_V; return 0; }
    if (ua.cls == CLS_ZERO && ub.cls == CLS_ZERO) return 0;
    if (ua.cls == CLS_ZERO) a = 0;
    if (ub.cls == CLS_ZERO) b = 0;
    ia = (a >> 63) ? -(sf_i64)(a & 0x7FFFFFFFFFFFFFFFull) : (sf_i64)a;
    ib = (b >> 63) ? -(sf_i64)(b & 0x7FFFFFFFFFFFFFFFull) : (sf_i64)b;
    return ia > ib;
}

/* -------------------------------------------------------- conversions */

f32 f32_from_i32(sf_env *env, sf_i32 v)
{
    int sign = v < 0;
    sf_u64 mag = sign ? (sf_u64)(-(sf_i64)v) : (sf_u64)v;
    int k;
    if (mag == 0) return 0;
    k = clz64(mag);
    return pack32(env, sign, 63 - k, mag << k);
}

f64 f64_from_i32(sf_env *env, sf_i32 v)
{
    int sign = v < 0;
    sf_u64 mag = sign ? (sf_u64)(-(sf_i64)v) : (sf_u64)v;
    int k;
    if (mag == 0) return 0;
    k = clz64(mag);
    return pack64(env, sign, 63 - k, mag << k);
}

static sf_i32 trunc_to_i32(sf_env *env, const unp *u)
{
    sf_u64 mag;
    if (u->cls >= CLS_QNAN) { env->flags |= SF_FLAG_V; return 0x7FFFFFFF; }
    if (u->cls == CLS_INF) { env->flags |= SF_FLAG_V; return u->sign ? (sf_i32)0x80000000u : 0x7FFFFFFF; }
    if (u->cls == CLS_ZERO) return 0;
    if (u->e < 0) { env->flags |= SF_FLAG_I; return 0; }
    if (u->e > 31 || (u->e == 31 && !(u->sign && u->sig == 0x8000000000000000ull))) {
        env->flags |= SF_FLAG_V;
        return u->sign ? (sf_i32)0x80000000u : 0x7FFFFFFF;
    }
    mag = u->sig >> (63 - u->e);
    if (u->sig & ((1ull << (63 - u->e)) - 1u)) env->flags |= SF_FLAG_I;
    return u->sign ? (sf_i32)(0u - (sf_u32)mag) : (sf_i32)mag;
}

sf_i32 f32_to_i32(sf_env *env, f32 a) { unp u = unpack32(env, a); return trunc_to_i32(env, &u); }
sf_i32 f64_to_i32(sf_env *env, f64 a) { unp u = unpack64(env, a); return trunc_to_i32(env, &u); }

f64 f32_to_f64(sf_env *env, f32 a)
{
    unp u = unpack32(env, a);
    switch (u.cls) {
    case CLS_SNAN: env->flags |= SF_FLAG_V; return F64_DEFAULT_NAN;
    case CLS_QNAN: return F64_INF(u.sign) | ((sf_u64)(a & 0x3FFFFF) << 29);
    case CLS_INF:  return F64_INF(u.sign);
    case CLS_ZERO: return F64_ZERO(u.sign);
    default:       return pack64(env, u.sign, u.e, u.sig);
    }
}

f32 f64_to_f32(sf_env *env, f64 a)
{
    unp u = unpack64(env, a);
    switch (u.cls) {
    case CLS_SNAN: env->flags |= SF_FLAG_V; return F32_DEFAULT_NAN;
    case CLS_QNAN: return F32_INF(u.sign) | (sf_u32)((a >> 29) & 0x3FFFFF) | 0x1u;
    case CLS_INF:  return F32_INF(u.sign);
    case CLS_ZERO: return F32_ZERO(u.sign);
    default:       return pack32(env, u.sign, u.e, u.sig);
    }
}
