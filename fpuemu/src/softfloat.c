/*
 * softfloat.c - fast integer-only IEEE 754 arithmetic with SH-4 FPU semantics.
 *
 * Uses pure 32-bit paths for single precision, reciprocal/rsqrt LUT +
 * Newton-Raphson with exact remainder checks (zero 64-bit software division
 * or 128-bit restoring loops), constant-shift specialized round-and-pack
 * helpers, and a fast normal-operand path that avoids unpacking structs.
 */
#include "softfloat.h"

#if defined(_MSC_VER)
#define SF_INLINE   __inline
#define SF_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define SF_INLINE   inline
#define SF_NOINLINE __attribute__((noinline))
#else
#define SF_INLINE
#define SF_NOINLINE
#endif

#if defined(__GNUC__) || defined(__clang__)
#define SF_LIKELY(x)   __builtin_expect(!!(x), 1)
#define SF_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define SF_LIKELY(x)   (x)
#define SF_UNLIKELY(x) (x)
#endif

/* ---------------------------------------------------------------- tables */

static const sf_u8 clz8_lut[256] = {
    8,7,6,6,5,5,5,5,4,4,4,4,4,4,4,4, 3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,
    2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2, 2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

/* 2^15 / (1 + (i + 0.5) / 128), clamped to 0x7FFF */
static const sf_u16 recip128[128] = {
    0x7F80u, 0x7E84u, 0x7D8Cu, 0x7C98u, 0x7BA7u, 0x7ABAu, 0x79D0u, 0x78EAu,
    0x7808u, 0x7728u, 0x764Cu, 0x7573u, 0x749Du, 0x73CAu, 0x72FAu, 0x722Du,
    0x7162u, 0x709Bu, 0x6FD6u, 0x6F14u, 0x6E54u, 0x6D98u, 0x6CDDu, 0x6C25u,
    0x6B70u, 0x6ABCu, 0x6A0Cu, 0x695Du, 0x68B1u, 0x6807u, 0x675Eu, 0x66B9u,
    0x6615u, 0x6573u, 0x64D3u, 0x6435u, 0x6399u, 0x62FFu, 0x6267u, 0x61D1u,
    0x613Cu, 0x60A9u, 0x6018u, 0x5F89u, 0x5EFBu, 0x5E6Fu, 0x5DE4u, 0x5D5Bu,
    0x5CD4u, 0x5C4Eu, 0x5BCAu, 0x5B47u, 0x5AC5u, 0x5A45u, 0x59C6u, 0x5949u,
    0x58CDu, 0x5853u, 0x57DAu, 0x5762u, 0x56EBu, 0x5676u, 0x5601u, 0x558Eu,
    0x551Du, 0x54ACu, 0x543Du, 0x53CEu, 0x5361u, 0x52F5u, 0x528Au, 0x5220u,
    0x51B7u, 0x514Fu, 0x50E9u, 0x5083u, 0x501Eu, 0x4FBAu, 0x4F57u, 0x4EF6u,
    0x4E95u, 0x4E35u, 0x4DD5u, 0x4D77u, 0x4D1Au, 0x4CBDu, 0x4C62u, 0x4C07u,
    0x4BADu, 0x4B54u, 0x4AFCu, 0x4AA4u, 0x4A4Eu, 0x49F8u, 0x49A3u, 0x494Eu,
    0x48FBu, 0x48A8u, 0x4856u, 0x4805u, 0x47B4u, 0x4764u, 0x4715u, 0x46C6u,
    0x4678u, 0x462Bu, 0x45DEu, 0x4592u, 0x4547u, 0x44FCu, 0x44B2u, 0x4469u,
    0x4420u, 0x43D8u, 0x4390u, 0x4349u, 0x4303u, 0x42BDu, 0x4277u, 0x4233u,
    0x41EEu, 0x41ABu, 0x4168u, 0x4125u, 0x40E3u, 0x40A2u, 0x4061u, 0x4020u
};

/* 2^15 / sqrt(M): [0..63] for M in [1,2), [64..127] for M in [2,4) */
static const sf_u16 rsqrt128[128] = {
    0x7F81u, 0x7E87u, 0x7D92u, 0x7CA3u, 0x7BB9u, 0x7AD5u, 0x79F5u, 0x791Au,
    0x7843u, 0x7771u, 0x76A3u, 0x75D9u, 0x7514u, 0x7452u, 0x7393u, 0x72D9u,
    0x7221u, 0x716Eu, 0x70BDu, 0x7010u, 0x6F66u, 0x6EBEu, 0x6E1Au, 0x6D78u,
    0x6CDAu, 0x6C3Du, 0x6BA4u, 0x6B0Du, 0x6A78u, 0x69E6u, 0x6956u, 0x68C9u,
    0x683Eu, 0x67B4u, 0x672Du, 0x66A8u, 0x6625u, 0x65A4u, 0x6525u, 0x64A7u,
    0x642Cu, 0x63B2u, 0x633Au, 0x62C3u, 0x624Fu, 0x61DBu, 0x616Au, 0x60FAu,
    0x608Bu, 0x601Eu, 0x5FB2u, 0x5F48u, 0x5EDFu, 0x5E78u, 0x5E11u, 0x5DACu,
    0x5D49u, 0x5CE6u, 0x5C85u, 0x5C25u, 0x5BC6u, 0x5B68u, 0x5B0Bu, 0x5AB0u,
    0x5A28u, 0x5978u, 0x58CBu, 0x5822u, 0x577Cu, 0x56DBu, 0x563Du, 0x55A2u,
    0x550Au, 0x5475u, 0x53E4u, 0x5355u, 0x52C9u, 0x5240u, 0x51B9u, 0x5135u,
    0x50B4u, 0x5035u, 0x4FB8u, 0x4F3Du, 0x4EC5u, 0x4E4Fu, 0x4DDAu, 0x4D68u,
    0x4CF8u, 0x4C8Au, 0x4C1Du, 0x4BB2u, 0x4B49u, 0x4AE2u, 0x4A7Cu, 0x4A18u,
    0x49B6u, 0x4955u, 0x48F5u, 0x4897u, 0x483Au, 0x47DFu, 0x4785u, 0x472Cu,
    0x46D5u, 0x467Fu, 0x462Au, 0x45D6u, 0x4583u, 0x4532u, 0x44E2u, 0x4492u,
    0x4444u, 0x43F7u, 0x43ABu, 0x4360u, 0x4316u, 0x42CCu, 0x4284u, 0x423Du,
    0x41F6u, 0x41B1u, 0x416Cu, 0x4128u, 0x40E5u, 0x40A2u, 0x4061u, 0x4020u
};

/* ---------------------------------------------------------------- bits */

static SF_INLINE int clz32(sf_u32 x)
{
    int n = 0;
    if (x >= 0x00010000u) { x >>= 16; } else { n = 16; }
    if (x >= 0x00000100u) { x >>= 8;  } else { n += 8; }
    return n + (int)clz8_lut[x];
}

static SF_INLINE int clz64(sf_u64 x)
{
    sf_u32 hi = (sf_u32)(x >> 32);
    if (hi) return clz32(hi);
    return 32 + clz32((sf_u32)x);
}

/* Shift right by n (n > 0), OR-ing shifted-out bits into bit 0. */
static SF_INLINE sf_u64 shr_sticky64(sf_u64 x, int n)
{
    if (n <= 0) return x;
    if (n >= 64) return x != 0;
    return (x >> n) | ((x << (64 - n)) != 0);
}

static SF_INLINE sf_u32 shr_sticky32(sf_u32 x, int n)
{
    if (n <= 0) return x;
    if (n >= 32) return x != 0;
    return (x >> n) | ((x << (32 - n)) != 0);
}

/* 64x64 -> 128 using four 32x32->64 products (SH-4 dmulu.l) */
static SF_INLINE void mul64to128(sf_u64 a, sf_u64 b, sf_u64 *hi, sf_u64 *lo)
{
    sf_u32 a0 = (sf_u32)a, a1 = (sf_u32)(a >> 32);
    sf_u32 b0 = (sf_u32)b, b1 = (sf_u32)(b >> 32);
    sf_u64 p00 = (sf_u64)a0 * b0;
    sf_u64 p01 = (sf_u64)a0 * b1;
    sf_u64 p10 = (sf_u64)a1 * b0;
    sf_u64 p11 = (sf_u64)a1 * b1;
    sf_u64 mid = (p00 >> 32) + (sf_u32)p01 + (sf_u32)p10;
    *lo = ((sf_u64)(sf_u32)p00) | (mid << 32);
    *hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}

/* Low 64 bits of (u32 * u64), two dmulu.l instead of full 64x64 */
static SF_INLINE sf_u64 mul32x64_lo(sf_u32 a, sf_u64 b)
{
    sf_u64 lo = (sf_u64)a * (sf_u32)b;
    sf_u32 hi = (sf_u32)(lo >> 32) + a * (sf_u32)(b >> 32);
    return ((sf_u64)hi << 32) | (sf_u32)lo;
}

/* Low 64 bits of (s * s) for 64-bit s, one dmulu.l + one mul.l */
static SF_INLINE sf_u64 sqr64_lo(sf_u64 s)
{
    sf_u32 s0 = (sf_u32)s, s1 = (sf_u32)(s >> 32);
    sf_u64 lo = (sf_u64)s0 * s0;
    sf_u32 hi = (sf_u32)(lo >> 32) + 2u * (s0 * s1);
    return ((sf_u64)hi << 32) | (sf_u32)lo;
}

/* ~30-bit reciprocal of b32 in [2^31, 2^32): returns r ~= 2^62 / b32 */
static SF_INLINE sf_u32 recip32(sf_u32 b32)
{
    sf_u32 idx = (b32 >> 24) & 0x7Fu;
    sf_u32 x = (sf_u32)recip128[idx] << 16;
    sf_u32 p, e;
    p = (sf_u32)(((sf_u64)b32 * x) >> 31);
    e = 0u - p;
    x = (sf_u32)(((sf_u64)x * e) >> 31);
    p = (sf_u32)(((sf_u64)b32 * x) >> 31);
    e = 0u - p;
    return (sf_u32)(((sf_u64)x * e) >> 31);
}

/* ~29-bit inv-sqrt of a30 in [2^30, 2^32) (M = a30/2^30 in [1,4)):
 * odd = (M >= 2). Returns r ~= 2^31 / sqrt(M). */
static SF_INLINE sf_u32 rsqrt32(sf_u32 a30, int odd)
{
    sf_u32 idx = ((sf_u32)odd << 6) | ((odd ? (a30 >> 25) : (a30 >> 24)) & 0x3Fu);
    sf_u32 r = (sf_u32)rsqrt128[idx] << 16;
    sf_u32 sq, t, f;
    sq = (sf_u32)(((sf_u64)r * r) >> 32);
    t  = (sf_u32)(((sf_u64)a30 * sq) >> 29);
    f  = 0xC0000000u - (t >> 1);
    r  = (sf_u32)(((sf_u64)r * f) >> 31);
    sq = (sf_u32)(((sf_u64)r * r) >> 32);
    t  = (sf_u32)(((sf_u64)a30 * sq) >> 29);
    f  = 0xC0000000u - (t >> 1);
    return (sf_u32)(((sf_u64)r * f) >> 31);
}

#define F32_INF(s)  (((sf_u32)(s) << 31) | 0x7F800000u)
#define F64_INF(s)  (((sf_u64)(s) << 63) | 0x7FF0000000000000ull)
#define F32_ZERO(s) ((sf_u32)(s) << 31)
#define F64_ZERO(s) ((sf_u64)(s) << 63)

int f32_is_nan(f32 a) { return (a & 0x7FFFFFFFu) > 0x7F800000u; }
int f64_is_nan(f64 a) { return (a & 0x7FFFFFFFFFFFFFFFull) > 0x7FF0000000000000ull; }

/* ------------------------------------------------------- round & pack */

/* sig31 has bit 30 = leading 1, bit 6 = round, bits 5..0 = sticky;
 * eb is biased exponent (1..254 for normal). */
static SF_NOINLINE f32 round_pack32_cold(sf_env *env, sf_u32 sign, int eb, sf_u32 sig31)
{
    sf_u32 rem, kept;
    if (eb >= 255) {
        env->flags |= SF_FLAG_O | SF_FLAG_I;
        return (sign << 31) | (env->rm == SF_RZ ? 0x7F7FFFFFu : 0x7F800000u);
    }
    if (env->dn) {
        env->flags |= SF_FLAG_U | SF_FLAG_I;
        return sign << 31;
    }
    sig31 = shr_sticky32(sig31, 1 - eb);
    rem = sig31 & 0x7Fu;
    kept = sig31 >> 7;
    if (rem) {
        env->flags |= SF_FLAG_I | SF_FLAG_U;
        if (env->rm == SF_RN && (rem > 0x40u || (rem == 0x40u && (kept & 1u))))
            kept++;
    }
    return (sign << 31) | kept;
}

static SF_INLINE f32 round_pack32(sf_env *env, sf_u32 sign, int eb, sf_u32 sig31)
{
    sf_u32 rem, kept;
    if (SF_UNLIKELY((sf_u32)(eb - 1) >= 254u))
        return round_pack32_cold(env, sign, eb, sig31);
    rem = sig31 & 0x7Fu;
    kept = sig31 >> 7;
    if (rem) {
        env->flags |= SF_FLAG_I;
        if (env->rm == SF_RN && (rem > 0x40u || (rem == 0x40u && (kept & 1u)))) {
            kept++;
            if (SF_UNLIKELY(kept == 0x01000000u && eb == 254))
                env->flags |= SF_FLAG_O;
        }
    }
    return (sign << 31) | (((sf_u32)(eb - 1) << 23) + kept);
}

/* sig64 has bit 63 = leading 1, bit 10 = round, bits 9..0 = sticky;
 * eb is biased exponent (1..2046 for normal). */
static SF_NOINLINE f64 round_pack64_cold(sf_env *env, sf_u32 sign, int eb, sf_u64 sig64)
{
    sf_u32 rem;
    sf_u64 kept;
    if (eb >= 2047) {
        env->flags |= SF_FLAG_O | SF_FLAG_I;
        return ((sf_u64)sign << 63) |
               (env->rm == SF_RZ ? 0x7FEFFFFFFFFFFFFFull : 0x7FF0000000000000ull);
    }
    if (env->dn) {
        env->flags |= SF_FLAG_U | SF_FLAG_I;
        return (sf_u64)sign << 63;
    }
    sig64 = shr_sticky64(sig64, 1 - eb);
    rem = (sf_u32)sig64 & 0x7FFu;
    kept = sig64 >> 11;
    if (rem) {
        env->flags |= SF_FLAG_I | SF_FLAG_U;
        if (env->rm == SF_RN && (rem > 0x400u || (rem == 0x400u && (kept & 1u))))
            kept++;
    }
    return ((sf_u64)sign << 63) | kept;
}

static SF_INLINE f64 round_pack64(sf_env *env, sf_u32 sign, int eb, sf_u64 sig64)
{
    sf_u32 rem;
    sf_u64 kept;
    if (SF_UNLIKELY((sf_u32)(eb - 1) >= 2046u))
        return round_pack64_cold(env, sign, eb, sig64);
    rem = (sf_u32)sig64 & 0x7FFu;
    kept = sig64 >> 11;
    if (rem) {
        env->flags |= SF_FLAG_I;
        if (env->rm == SF_RN && (rem > 0x400u || (rem == 0x400u && (kept & 1u)))) {
            kept++;
            if (SF_UNLIKELY(kept == 0x0020000000000000ull && eb == 2046))
                env->flags |= SF_FLAG_O;
        }
    }
    return ((sf_u64)sign << 63) | (((sf_u64)(sf_u32)(eb - 1) << 52) + kept);
}

/* ----------------------------------------------------- NaN helpers */

static SF_INLINE f32 nan_prop32(sf_env *env, f32 a, f32 b)
{
    sf_u32 ma = a & 0x7FFFFFFFu, mb = b & 0x7FFFFFFFu;
    int an = ma > 0x7F800000u, bn = mb > 0x7F800000u;
    if ((an && (a & 0x00400000u)) || (bn && (b & 0x00400000u))) {
        env->flags |= SF_FLAG_V;
        return F32_DEFAULT_NAN;
    }
    return an ? a : b;
}

static SF_INLINE f64 nan_prop64(sf_env *env, f64 a, f64 b)
{
    sf_u64 ma = a & 0x7FFFFFFFFFFFFFFFull, mb = b & 0x7FFFFFFFFFFFFFFFull;
    int an = ma > 0x7FF0000000000000ull, bn = mb > 0x7FF0000000000000ull;
    if ((an && (a & 0x0008000000000000ull)) || (bn && (b & 0x0008000000000000ull))) {
        env->flags |= SF_FLAG_V;
        return F64_DEFAULT_NAN;
    }
    return an ? a : b;
}

/* ------------------------------------------------------------- single */

/* Core add/sub for two finite nonzero single operands (normalized: bit 30 = 1,
 * biased exponents ea, eb; caller already flipped b's sign for sub). */
static SF_INLINE f32 f32_add_norm(sf_env *env, sf_u32 sa, int ea, sf_u32 siga,
                                  sf_u32 sb, int eb, sf_u32 sigb)
{
    int diff;
    sf_u32 r;
    if (ea < eb || (ea == eb && siga < sigb)) {
        sf_u32 ts = sa; sa = sb; sb = ts;
        { int te = ea; ea = eb; eb = te; }
        ts = siga; siga = sigb; sigb = ts;
    }
    diff = ea - eb;
    if (diff > 0)
        sigb = (diff >= 31) ? 1u : ((sigb >> diff) | ((sigb << (32 - diff)) != 0));
    if (sa == sb) {
        r = siga + sigb;
        if (r & 0x80000000u) {
            r = (r >> 1) | (r & 1u);
            ea++;
        }
    } else {
        r = siga - sigb;
        if (r == 0) return 0u;
        if (!(r & 0x40000000u)) {
            int k = clz32(r) - 1;
            r <<= k;
            ea -= k;
        }
    }
    return round_pack32(env, sa, ea, r);
}

static SF_NOINLINE f32 f32_addsub_slow(sf_env *env, f32 a, f32 b)
{
    sf_u32 ma = a & 0x7FFFFFFFu, mb = b & 0x7FFFFFFFu;
    sf_u32 sa = a >> 31, sb = b >> 31;
    int ea, eb;
    sf_u32 siga, sigb;

    if (ma > 0x7F800000u || mb > 0x7F800000u)
        return nan_prop32(env, a, b);
    if (ma == 0x7F800000u) {
        if (mb == 0x7F800000u && sa != sb) {
            env->flags |= SF_FLAG_V;
            return F32_DEFAULT_NAN;
        }
        return a;
    }
    if (mb == 0x7F800000u) return b;

    if (ma < 0x00800000u) {
        if (ma == 0 || env->dn) {
            if (mb == 0 || (mb < 0x00800000u && env->dn))
                return F32_ZERO(sa & sb);
            return b;
        }
        {
            int k = clz32(ma) - 1;
            siga = ma << k;
            ea = 1 - (k - 7);
        }
    } else {
        ea = (int)(ma >> 23);
        siga = ((ma & 0x7FFFFFu) | 0x800000u) << 7;
    }

    if (mb < 0x00800000u) {
        if (mb == 0 || env->dn)
            return a;
        {
            int k = clz32(mb) - 1;
            sigb = mb << k;
            eb = 1 - (k - 7);
        }
    } else {
        eb = (int)(mb >> 23);
        sigb = ((mb & 0x7FFFFFu) | 0x800000u) << 7;
    }

    return f32_add_norm(env, sa, ea, siga, sb, eb, sigb);
}

static SF_INLINE f32 f32_add_impl(sf_env *env, f32 a, f32 b)
{
    sf_u32 ea = (a >> 23) & 0xFFu;
    sf_u32 eb = (b >> 23) & 0xFFu;
    if (SF_LIKELY(((ea - 1u) < 0xFEu) & ((eb - 1u) < 0xFEu))) {
        sf_u32 ma = a & 0x7FFFFFFFu, mb = b & 0x7FFFFFFFu;
        sf_u32 sa = a >> 31, sb = b >> 31;
        sf_u32 siga, sigb, r;
        int e, diff;
        if (ma < mb) {
            sf_u32 t = ma; ma = mb; mb = t;
            t = sa; sa = sb; sb = t;
            t = ea; ea = eb; eb = t;
        }
        siga = ((ma & 0x7FFFFFu) | 0x800000u) << 7;
        sigb = ((mb & 0x7FFFFFu) | 0x800000u) << 7;
        e = (int)ea;
        diff = e - (int)eb;
        if (diff > 0)
            sigb = (diff >= 31) ? 1u : ((sigb >> diff) | ((sigb << (32 - diff)) != 0));
        if (sa == sb) {
            r = siga + sigb;
            if (r & 0x80000000u) {
                r = (r >> 1) | (r & 1u);
                e++;
            }
        } else {
            r = siga - sigb;
            if (r == 0) return 0u;
            if (!(r & 0x40000000u)) {
                int k = clz32(r) - 1;
                r <<= k;
                e -= k;
            }
        }
        return round_pack32(env, sa, e, r);
    }
    return f32_addsub_slow(env, a, b);
}

f32 f32_add(sf_env *env, f32 a, f32 b) { return f32_add_impl(env, a, b); }
f32 f32_sub(sf_env *env, f32 a, f32 b) { return f32_add_impl(env, a, b ^ 0x80000000u); }

static SF_NOINLINE f32 f32_mul_slow(sf_env *env, f32 a, f32 b)
{
    sf_u32 ma = a & 0x7FFFFFFFu, mb = b & 0x7FFFFFFFu;
    sf_u32 sign = (a ^ b) >> 31;
    int ea, eb, e;
    sf_u32 siga, sigb, hi, lo, r;
    sf_u64 p;

    if (ma > 0x7F800000u || mb > 0x7F800000u)
        return nan_prop32(env, a, b);
    if (ma == 0x7F800000u || mb == 0x7F800000u) {
        int az = (ma == 0) || (ma < 0x00800000u && env->dn);
        int bz = (mb == 0) || (mb < 0x00800000u && env->dn);
        if (az || bz) { env->flags |= SF_FLAG_V; return F32_DEFAULT_NAN; }
        return F32_INF(sign);
    }
    if (ma < 0x00800000u) {
        if (ma == 0 || env->dn) return F32_ZERO(sign);
        { int k = clz32(ma); siga = ma << k; ea = 1 - (k - 8); }
    } else {
        ea = (int)(ma >> 23);
        siga = ((ma & 0x7FFFFFu) | 0x800000u) << 8;
    }
    if (mb < 0x00800000u) {
        if (mb == 0 || env->dn) return F32_ZERO(sign);
        { int k = clz32(mb) - 1; sigb = mb << k; eb = 1 - (k - 7); }
    } else {
        eb = (int)(mb >> 23);
        sigb = ((mb & 0x7FFFFFu) | 0x800000u) << 7;
    }
    p = (sf_u64)siga * sigb;
    hi = (sf_u32)(p >> 32);
    lo = (sf_u32)p;
    e = ea + eb - 127;
    if (hi & 0x40000000u) {
        r = hi | (lo != 0);
        e++;
    } else {
        r = (hi << 1) | (lo >> 31) | ((lo & 0x7FFFFFFFu) != 0);
    }
    return round_pack32(env, sign, e, r);
}

f32 f32_mul(sf_env *env, f32 a, f32 b)
{
    sf_u32 ea = (a >> 23) & 0xFFu;
    sf_u32 eb = (b >> 23) & 0xFFu;
    if (SF_LIKELY(((ea - 1u) < 0xFEu) & ((eb - 1u) < 0xFEu))) {
        sf_u32 sign = (a ^ b) >> 31;
        sf_u32 ma = ((a & 0x7FFFFFu) | 0x800000u) << 8;
        sf_u32 mb = ((b & 0x7FFFFFu) | 0x800000u) << 7;
        sf_u64 p = (sf_u64)ma * mb;
        sf_u32 hi = (sf_u32)(p >> 32);
        sf_u32 lo = (sf_u32)p;
        int e = (int)ea + (int)eb - 127;
        sf_u32 r;
        if (hi & 0x40000000u) {
            r = hi | (lo != 0);
            e++;
        } else {
            r = (hi << 1) | (lo >> 31) | ((lo & 0x7FFFFFFFu) != 0);
        }
        return round_pack32(env, sign, e, r);
    }
    return f32_mul_slow(env, a, b);
}

static SF_INLINE f32 f32_div_norm(sf_env *env, sf_u32 sign, int e, sf_u32 ma, sf_u32 mb)
{
    sf_u32 r, q;
    sf_i32 rem;
    if (ma < mb) { ma <<= 1; e--; }
    r = recip32(mb << 8);
    q = (sf_u32)(((sf_u64)(ma << 7) * r) >> 37);
    rem = (sf_i32)((ma << 24) - q * mb);
    while (rem < 0) { q--; rem += (sf_i32)mb; }
    while ((sf_u32)rem >= mb) { q++; rem -= (sf_i32)mb; }
    return round_pack32(env, sign, e, (q << 6) | (rem != 0));
}

static SF_NOINLINE f32 f32_div_slow(sf_env *env, f32 a, f32 b)
{
    sf_u32 ma = a & 0x7FFFFFFFu, mb = b & 0x7FFFFFFFu;
    sf_u32 sign = (a ^ b) >> 31;
    int ea, eb;
    sf_u32 siga, sigb;

    if (ma > 0x7F800000u || mb > 0x7F800000u)
        return nan_prop32(env, a, b);
    if (ma == 0x7F800000u) {
        if (mb == 0x7F800000u) { env->flags |= SF_FLAG_V; return F32_DEFAULT_NAN; }
        return F32_INF(sign);
    }
    if (mb == 0x7F800000u) return F32_ZERO(sign);
    if (mb == 0 || (mb < 0x00800000u && env->dn)) {
        if (ma == 0 || (ma < 0x00800000u && env->dn)) {
            env->flags |= SF_FLAG_V;
            return F32_DEFAULT_NAN;
        }
        env->flags |= SF_FLAG_Z;
        return F32_INF(sign);
    }
    if (ma == 0 || (ma < 0x00800000u && env->dn))
        return F32_ZERO(sign);

    if (ma < 0x00800000u) {
        int k = clz32(ma) - 8;
        siga = ma << k;
        ea = 1 - k;
    } else {
        ea = (int)(ma >> 23);
        siga = (ma & 0x7FFFFFu) | 0x800000u;
    }
    if (mb < 0x00800000u) {
        int k = clz32(mb) - 8;
        sigb = mb << k;
        eb = 1 - k;
    } else {
        eb = (int)(mb >> 23);
        sigb = (mb & 0x7FFFFFu) | 0x800000u;
    }
    return f32_div_norm(env, sign, ea - eb + 127, siga, sigb);
}

f32 f32_div(sf_env *env, f32 a, f32 b)
{
    sf_u32 ea = (a >> 23) & 0xFFu;
    sf_u32 eb = (b >> 23) & 0xFFu;
    if (SF_LIKELY(((ea - 1u) < 0xFEu) & ((eb - 1u) < 0xFEu))) {
        sf_u32 sign = (a ^ b) >> 31;
        sf_u32 ma = (a & 0x7FFFFFu) | 0x800000u;
        sf_u32 mb = (b & 0x7FFFFFu) | 0x800000u;
        return f32_div_norm(env, sign, (int)ea - (int)eb + 127, ma, mb);
    }
    return f32_div_slow(env, a, b);
}

static SF_INLINE f32 f32_sqrt_norm(sf_env *env, int e_unbiased, sf_u32 m)
{
    int odd = e_unbiased & 1;
    int res_eb = ((e_unbiased - odd) >> 1) + 127;
    sf_u32 a30, r, s;
    sf_i32 rem;
    if (odd) m <<= 1;
    a30 = m << 7;
    r = rsqrt32(a30, odd);
    s = (sf_u32)(((sf_u64)a30 * r) >> 37);
    rem = (sf_i32)((m << 25) - s * s);
    while (rem < 0) { s--; rem += (sf_i32)(2u * s + 1u); }
    while ((sf_u32)rem >= 2u * s + 1u) { rem -= (sf_i32)(2u * s + 1u); s++; }
    return round_pack32(env, 0u, res_eb, (s << 6) | (rem != 0));
}

static SF_NOINLINE f32 f32_sqrt_slow(sf_env *env, f32 a)
{
    sf_u32 ma = a & 0x7FFFFFFFu;
    if (ma > 0x7F800000u) {
        if (a & 0x00400000u) { env->flags |= SF_FLAG_V; return F32_DEFAULT_NAN; }
        return a;
    }
    if (ma == 0 || (ma < 0x00800000u && env->dn)) return a;
    if (a & 0x80000000u) { env->flags |= SF_FLAG_V; return F32_DEFAULT_NAN; }
    if (ma == 0x7F800000u) return a;
    /* positive denormal with dn == 0 */
    {
        int k = clz32(ma) - 8;
        return f32_sqrt_norm(env, -126 - k, ma << k);
    }
}

f32 f32_sqrt(sf_env *env, f32 a)
{
    if (SF_LIKELY((a - 0x00800000u) < 0x7F000000u)) {
        int e = (int)(a >> 23) - 127;
        sf_u32 m = (a & 0x7FFFFFu) | 0x800000u;
        return f32_sqrt_norm(env, e, m);
    }
    return f32_sqrt_slow(env, a);
}

/* Shared 64-bit add core for f32_fmac and f64_add/sub:
 * siga, sigb have bit 63 = leading 1; returns normalized rsig (bit 63 = 1) or 0. */
static SF_INLINE void add_core64(sf_u32 sa, int ea, sf_u64 siga,
                                 sf_u32 sb, int eb, sf_u64 sigb,
                                 sf_u32 *rs, int *re, sf_u64 *rsig)
{
    sf_u64 a, b, r;
    int e, diff;
    if (ea < eb || (ea == eb && siga < sigb)) {
        sf_u32 ts = sa; sa = sb; sb = ts;
        { int te = ea; ea = eb; eb = te; }
        { sf_u64 tg = siga; siga = sigb; sigb = tg; }
    }
    a = siga >> 1;
    diff = ea - eb;
    b = sigb >> 1;
    if (diff > 0)
        b = (diff >= 63) ? 1ull : ((b >> diff) | ((b << (64 - diff)) != 0));
    e = ea + 1;
    if (sa == sb) {
        r = a + b;
        if (!(r >> 63)) { r <<= 1; e--; }
    } else {
        r = a - b;
        if (r == 0) { *rs = 0; *re = 0; *rsig = 0; return; }
        if (r & 0x4000000000000000ull) {
            r <<= 1;
            e--;
        } else {
            int k = clz64(r);
            r <<= k;
            e -= k;
        }
    }
    *rs = sa;
    *re = e;
    *rsig = r;
}

static SF_INLINE f32 pack32_from_sig64(sf_env *env, sf_u32 sign, int eb, sf_u64 sig64)
{
    sf_u32 sig31 = (sf_u32)(sig64 >> 33) | (((sig64 & 0x1FFFFFFFFull) != 0) ? 1u : 0u);
    return round_pack32(env, sign, eb, sig31);
}

static SF_NOINLINE f32 f32_fmac_slow(sf_env *env, f32 a, f32 b, f32 c)
{
    sf_u32 ma = a & 0x7FFFFFFFu, mb = b & 0x7FFFFFFFu, mc = c & 0x7FFFFFFFu;
    sf_u32 psign = (a ^ b) >> 31, sc = c >> 31;
    int an = ma > 0x7F800000u, bn = mb > 0x7F800000u, cn = mc > 0x7F800000u;
    int az, bz, cz;
    int ea, eb, ec, pe, re;
    sf_u32 siga, sigb, rs;
    sf_u64 p, sigc, rsig;

    if (an || bn) {
        if ((an && (a & 0x00400000u)) || (bn && (b & 0x00400000u)) ||
            (cn && (c & 0x00400000u))) {
            env->flags |= SF_FLAG_V;
            return F32_DEFAULT_NAN;
        }
        return an ? a : b;
    }
    az = (ma == 0) || (ma < 0x00800000u && env->dn);
    bz = (mb == 0) || (mb < 0x00800000u && env->dn);
    cz = (mc == 0) || (mc < 0x00800000u && env->dn);
    if (cn) {
        if ((c & 0x00400000u) ||
            (ma == 0x7F800000u && bz) || (mb == 0x7F800000u && az)) {
            env->flags |= SF_FLAG_V;
            return F32_DEFAULT_NAN;
        }
        return c;
    }
    if (ma == 0x7F800000u || mb == 0x7F800000u) {
        if (az || bz) { env->flags |= SF_FLAG_V; return F32_DEFAULT_NAN; }
        if (mc == 0x7F800000u && sc != psign) { env->flags |= SF_FLAG_V; return F32_DEFAULT_NAN; }
        return F32_INF(psign);
    }
    if (mc == 0x7F800000u) return c;
    if (az || bz) {
        if (cz) return F32_ZERO(psign & sc);
        return c;
    }

    if (ma < 0x00800000u) {
        int k = clz32(ma); siga = ma << k; ea = 1 - (k - 8);
    } else {
        ea = (int)(ma >> 23); siga = ((ma & 0x7FFFFFu) | 0x800000u) << 8;
    }
    if (mb < 0x00800000u) {
        int k = clz32(mb); sigb = mb << k; eb = 1 - (k - 8);
    } else {
        eb = (int)(mb >> 23); sigb = ((mb & 0x7FFFFFu) | 0x800000u) << 8;
    }
    p = (sf_u64)siga * sigb;
    pe = ea + eb - 126;
    if (!(p >> 63)) { p <<= 1; pe--; }
    if (cz) return pack32_from_sig64(env, psign, pe, p);

    if (mc < 0x00800000u) {
        int k = clz32(mc); sigc = (sf_u64)(mc << k) << 32; ec = 1 - (k - 8);
    } else {
        ec = (int)(mc >> 23); sigc = ((sf_u64)((mc & 0x7FFFFFu) | 0x800000u)) << 40;
    }
    add_core64(psign, pe, p, sc, ec, sigc, &rs, &re, &rsig);
    if (rsig == 0) return 0u;
    return pack32_from_sig64(env, rs, re, rsig);
}

f32 f32_fmac(sf_env *env, f32 a, f32 b, f32 c)
{
    sf_u32 ea = (a >> 23) & 0xFFu;
    sf_u32 eb = (b >> 23) & 0xFFu;
    sf_u32 ec = (c >> 23) & 0xFFu;
    if (SF_LIKELY(((ea - 1u) < 0xFEu) & ((eb - 1u) < 0xFEu) & ((ec - 1u) < 0xFEu))) {
        sf_u32 psign = (a ^ b) >> 31, sc = c >> 31;
        sf_u32 ma = ((a & 0x7FFFFFu) | 0x800000u) << 8;
        sf_u32 mb = ((b & 0x7FFFFFu) | 0x800000u) << 8;
        sf_u64 p = (sf_u64)ma * mb;
        sf_u64 sigc = ((sf_u64)((c & 0x7FFFFFu) | 0x800000u)) << 40;
        int pe = (int)ea + (int)eb - 126;
        sf_u32 rs; int re; sf_u64 rsig;
        if (!(p >> 63)) { p <<= 1; pe--; }
        add_core64(psign, pe, p, sc, (int)ec, sigc, &rs, &re, &rsig);
        if (rsig == 0) return 0u;
        return pack32_from_sig64(env, rs, re, rsig);
    }
    return f32_fmac_slow(env, a, b, c);
}

int f32_eq(sf_env *env, f32 a, f32 b)
{
    sf_u32 ma = a & 0x7FFFFFFFu, mb = b & 0x7FFFFFFFu;
    if (SF_UNLIKELY(ma > 0x7F800000u || mb > 0x7F800000u)) {
        if ((ma > 0x7F800000u && (a & 0x00400000u)) ||
            (mb > 0x7F800000u && (b & 0x00400000u)))
            env->flags |= SF_FLAG_V;
        return 0;
    }
    if (env->dn) {
        if (ma < 0x00800000u) { a = 0; ma = 0; }
        if (mb < 0x00800000u) { b = 0; mb = 0; }
    }
    return (a == b) || ((ma | mb) == 0);
}

int f32_gt(sf_env *env, f32 a, f32 b)
{
    sf_u32 ma = a & 0x7FFFFFFFu, mb = b & 0x7FFFFFFFu;
    sf_i32 ia, ib;
    if (SF_UNLIKELY(ma > 0x7F800000u || mb > 0x7F800000u)) {
        env->flags |= SF_FLAG_V;
        return 0;
    }
    if (env->dn) {
        if (ma < 0x00800000u) { a = 0; ma = 0; }
        if (mb < 0x00800000u) { b = 0; mb = 0; }
    }
    if ((ma | mb) == 0) return 0;
    ia = (a & 0x80000000u) ? -(sf_i32)ma : (sf_i32)ma;
    ib = (b & 0x80000000u) ? -(sf_i32)mb : (sf_i32)mb;
    return ia > ib;
}

/* ------------------------------------------------------------- double */

static SF_NOINLINE f64 f64_addsub_slow(sf_env *env, f64 a, f64 b)
{
    sf_u64 ma = a & 0x7FFFFFFFFFFFFFFFull, mb = b & 0x7FFFFFFFFFFFFFFFull;
    sf_u32 sa = (sf_u32)(a >> 63), sb = (sf_u32)(b >> 63);
    int ea, eb, re;
    sf_u64 siga, sigb, rsig;
    sf_u32 rs;

    if (ma > 0x7FF0000000000000ull || mb > 0x7FF0000000000000ull)
        return nan_prop64(env, a, b);
    if (ma == 0x7FF0000000000000ull) {
        if (mb == 0x7FF0000000000000ull && sa != sb) {
            env->flags |= SF_FLAG_V;
            return F64_DEFAULT_NAN;
        }
        return a;
    }
    if (mb == 0x7FF0000000000000ull) return b;

    if (ma < 0x0010000000000000ull) {
        if (ma == 0 || env->dn) {
            if (mb == 0 || (mb < 0x0010000000000000ull && env->dn))
                return F64_ZERO(sa & sb);
            return b;
        }
        {
            int k = clz64(ma);
            siga = ma << k;
            ea = 1 - (k - 11);
        }
    } else {
        ea = (int)(ma >> 52);
        siga = ((ma & 0xFFFFFFFFFFFFFull) | 0x10000000000000ull) << 11;
    }

    if (mb < 0x0010000000000000ull) {
        if (mb == 0 || env->dn)
            return a;
        {
            int k = clz64(mb);
            sigb = mb << k;
            eb = 1 - (k - 11);
        }
    } else {
        eb = (int)(mb >> 52);
        sigb = ((mb & 0xFFFFFFFFFFFFFull) | 0x10000000000000ull) << 11;
    }

    add_core64(sa, ea, siga, sb, eb, sigb, &rs, &re, &rsig);
    if (rsig == 0) return 0ull;
    return round_pack64(env, rs, re, rsig);
}

static SF_INLINE f64 f64_add_impl(sf_env *env, f64 a, f64 b)
{
    sf_u32 ha = (sf_u32)(a >> 32), hb = (sf_u32)(b >> 32);
    sf_u32 ea = (ha >> 20) & 0x7FFu;
    sf_u32 eb = (hb >> 20) & 0x7FFu;
    if (SF_LIKELY(((ea - 1u) < 0x7FEu) & ((eb - 1u) < 0x7FEu))) {
        sf_u64 ma = a & 0x7FFFFFFFFFFFFFFFull;
        sf_u64 mb = b & 0x7FFFFFFFFFFFFFFFull;
        sf_u32 sa = ha >> 31, sb = hb >> 31;
        sf_u64 siga, sigb, r;
        int e, diff;
        if (ma < mb) {
            sf_u64 tg = ma; ma = mb; mb = tg;
            { sf_u32 ts = sa; sa = sb; sb = ts; }
            { sf_u32 te = ea; ea = eb; eb = te; }
        }
        siga = ((ma & 0xFFFFFFFFFFFFFull) | 0x10000000000000ull) << 10;
        sigb = ((mb & 0xFFFFFFFFFFFFFull) | 0x10000000000000ull) << 10;
        e = (int)ea + 1;
        diff = (int)ea - (int)eb;
        if (diff > 0)
            sigb = (diff >= 63) ? 1ull : ((sigb >> diff) | ((sigb << (64 - diff)) != 0));
        if (sa == sb) {
            r = siga + sigb;
            if (!(r >> 63)) { r <<= 1; e--; }
        } else {
            r = siga - sigb;
            if (r == 0) return 0ull;
            if (r & 0x4000000000000000ull) {
                r <<= 1;
                e--;
            } else {
                int k = clz64(r);
                r <<= k;
                e -= k;
            }
        }
        return round_pack64(env, sa, e, r);
    }
    return f64_addsub_slow(env, a, b);
}

f64 f64_add(sf_env *env, f64 a, f64 b) { return f64_add_impl(env, a, b); }
f64 f64_sub(sf_env *env, f64 a, f64 b) { return f64_add_impl(env, a, b ^ 0x8000000000000000ull); }

static SF_NOINLINE f64 f64_mul_slow(sf_env *env, f64 a, f64 b)
{
    sf_u64 ma = a & 0x7FFFFFFFFFFFFFFFull, mb = b & 0x7FFFFFFFFFFFFFFFull;
    sf_u32 sign = (sf_u32)((a ^ b) >> 63);
    int ea, eb, e;
    sf_u64 siga, sigb, hi, lo;

    if (ma > 0x7FF0000000000000ull || mb > 0x7FF0000000000000ull)
        return nan_prop64(env, a, b);
    if (ma == 0x7FF0000000000000ull || mb == 0x7FF0000000000000ull) {
        int az = (ma == 0) || (ma < 0x0010000000000000ull && env->dn);
        int bz = (mb == 0) || (mb < 0x0010000000000000ull && env->dn);
        if (az || bz) { env->flags |= SF_FLAG_V; return F64_DEFAULT_NAN; }
        return F64_INF(sign);
    }
    if (ma < 0x0010000000000000ull) {
        if (ma == 0 || env->dn) return F64_ZERO(sign);
        { int k = clz64(ma); siga = ma << k; ea = 1 - (k - 11); }
    } else {
        ea = (int)(ma >> 52);
        siga = ((ma & 0xFFFFFFFFFFFFFull) | 0x10000000000000ull) << 11;
    }
    if (mb < 0x0010000000000000ull) {
        if (mb == 0 || env->dn) return F64_ZERO(sign);
        { int k = clz64(mb); sigb = mb << k; eb = 1 - (k - 11); }
    } else {
        eb = (int)(mb >> 52);
        sigb = ((mb & 0xFFFFFFFFFFFFFull) | 0x10000000000000ull) << 11;
    }
    mul64to128(siga, sigb, &hi, &lo);
    e = ea + eb - 1022;
    if (!(hi >> 63)) { hi = (hi << 1) | (lo >> 63); lo <<= 1; e--; }
    return round_pack64(env, sign, e, hi | (lo != 0));
}

f64 f64_mul(sf_env *env, f64 a, f64 b)
{
    sf_u32 ha = (sf_u32)(a >> 32), hb = (sf_u32)(b >> 32);
    sf_u32 ea = (ha >> 20) & 0x7FFu;
    sf_u32 eb = (hb >> 20) & 0x7FFu;
    if (SF_LIKELY(((ea - 1u) < 0x7FEu) & ((eb - 1u) < 0x7FEu))) {
        sf_u32 sign = (ha ^ hb) >> 31;
        sf_u64 siga = ((a & 0xFFFFFFFFFFFFFull) | 0x10000000000000ull) << 11;
        sf_u64 sigb = ((b & 0xFFFFFFFFFFFFFull) | 0x10000000000000ull) << 11;
        sf_u64 hi, lo;
        int e = (int)ea + (int)eb - 1022;
        mul64to128(siga, sigb, &hi, &lo);
        if (!(hi >> 63)) { hi = (hi << 1) | (lo >> 63); lo <<= 1; e--; }
        return round_pack64(env, sign, e, hi | (lo != 0));
    }
    return f64_mul_slow(env, a, b);
}

/* 53-bit / 53-bit -> 54-bit quotient + exact sticky using two radix-2^26/27 steps */
static SF_INLINE f64 f64_div_norm(sf_env *env, sf_u32 sign, int e, sf_u64 ma, sf_u64 mb)
{
    sf_u32 r, q_hi, q_lo;
    sf_i64 rem1, rem2;
    sf_u64 q;

    if (ma < mb) { ma <<= 1; e--; }
    r = recip32((sf_u32)(mb >> 21));

    q_hi = (sf_u32)(((sf_u64)(sf_u32)(ma >> 22) * r) >> 34);
    rem1 = (sf_i64)((ma << 27) - mul32x64_lo(q_hi, mb));
    while (rem1 < 0) { q_hi--; rem1 += (sf_i64)mb; }
    while ((sf_u64)rem1 >= mb) { q_hi++; rem1 -= (sf_i64)mb; }

    q_lo = (sf_u32)(((sf_u64)(sf_u32)((sf_u64)rem1 >> 21) * r) >> 36);
    rem2 = (sf_i64)(((sf_u64)rem1 << 26) - mul32x64_lo(q_lo, mb));
    while (rem2 < 0) { q_lo--; rem2 += (sf_i64)mb; }
    while ((sf_u64)rem2 >= mb) { q_lo++; rem2 -= (sf_i64)mb; }

    q = (((sf_u64)q_hi << 26) | (sf_u64)q_lo) << 10;
    return round_pack64(env, sign, e, q | (rem2 != 0));
}

static SF_NOINLINE f64 f64_div_slow(sf_env *env, f64 a, f64 b)
{
    sf_u64 ma = a & 0x7FFFFFFFFFFFFFFFull, mb = b & 0x7FFFFFFFFFFFFFFFull;
    sf_u32 sign = (sf_u32)((a ^ b) >> 63);
    int ea, eb;
    sf_u64 siga, sigb;

    if (ma > 0x7FF0000000000000ull || mb > 0x7FF0000000000000ull)
        return nan_prop64(env, a, b);
    if (ma == 0x7FF0000000000000ull) {
        if (mb == 0x7FF0000000000000ull) { env->flags |= SF_FLAG_V; return F64_DEFAULT_NAN; }
        return F64_INF(sign);
    }
    if (mb == 0x7FF0000000000000ull) return F64_ZERO(sign);
    if (mb == 0 || (mb < 0x0010000000000000ull && env->dn)) {
        if (ma == 0 || (ma < 0x0010000000000000ull && env->dn)) {
            env->flags |= SF_FLAG_V;
            return F64_DEFAULT_NAN;
        }
        env->flags |= SF_FLAG_Z;
        return F64_INF(sign);
    }
    if (ma == 0 || (ma < 0x0010000000000000ull && env->dn))
        return F64_ZERO(sign);

    if (ma < 0x0010000000000000ull) {
        int k = clz64(ma) - 11;
        siga = ma << k;
        ea = 1 - k;
    } else {
        ea = (int)(ma >> 52);
        siga = (ma & 0xFFFFFFFFFFFFFull) | 0x10000000000000ull;
    }
    if (mb < 0x0010000000000000ull) {
        int k = clz64(mb) - 11;
        sigb = mb << k;
        eb = 1 - k;
    } else {
        eb = (int)(mb >> 52);
        sigb = (mb & 0xFFFFFFFFFFFFFull) | 0x10000000000000ull;
    }
    return f64_div_norm(env, sign, ea - eb + 1023, siga, sigb);
}

f64 f64_div(sf_env *env, f64 a, f64 b)
{
    sf_u32 ha = (sf_u32)(a >> 32), hb = (sf_u32)(b >> 32);
    sf_u32 ea = (ha >> 20) & 0x7FFu;
    sf_u32 eb = (hb >> 20) & 0x7FFu;
    if (SF_LIKELY(((ea - 1u) < 0x7FEu) & ((eb - 1u) < 0x7FEu))) {
        sf_u32 sign = (ha ^ hb) >> 31;
        sf_u64 ma = (a & 0xFFFFFFFFFFFFFull) | 0x10000000000000ull;
        sf_u64 mb = (b & 0xFFFFFFFFFFFFFull) | 0x10000000000000ull;
        return f64_div_norm(env, sign, (int)ea - (int)eb + 1023, ma, mb);
    }
    return f64_div_slow(env, a, b);
}

static SF_INLINE f64 f64_sqrt_norm(sf_env *env, int e_unbiased, sf_u64 m)
{
    int odd = e_unbiased & 1;
    int res_eb = ((e_unbiased - odd) >> 1) + 1023;
    sf_u32 a30, r, s0;
    sf_i32 d32;
    sf_i64 delta, rem;
    sf_u64 s;

    if (odd) m <<= 1;
    a30 = (sf_u32)(m >> 22);
    r = rsqrt32(a30, odd);
    s0 = (sf_u32)(((sf_u64)a30 * r) >> 34);
    /* (m << 2) - s0*s0 is exact and bounded by ~2^29, fitting in sf_i32 */
    d32 = (sf_i32)((m << 2) - (sf_u64)s0 * s0);
    delta = ((sf_i64)d32 * (sf_i64)r) >> 33;
    s = (sf_u64)(((sf_i64)s0 << 26) + delta);
    rem = (sf_i64)((m << 54) - sqr64_lo(s));
    while (rem < 0) {
        s--;
        rem += (sf_i64)((s << 1) + 1ull);
    }
    while ((sf_u64)rem >= (s << 1) + 1ull) {
        rem -= (sf_i64)((s << 1) + 1ull);
        s++;
    }
    return round_pack64(env, 0u, res_eb, (s << 10) | (rem != 0));
}

static SF_NOINLINE f64 f64_sqrt_slow(sf_env *env, f64 a)
{
    sf_u64 ma = a & 0x7FFFFFFFFFFFFFFFull;
    if (ma > 0x7FF0000000000000ull) {
        if (a & 0x0008000000000000ull) { env->flags |= SF_FLAG_V; return F64_DEFAULT_NAN; }
        return a;
    }
    if (ma == 0 || (ma < 0x0010000000000000ull && env->dn)) return a;
    if (a >> 63) { env->flags |= SF_FLAG_V; return F64_DEFAULT_NAN; }
    if (ma == 0x7FF0000000000000ull) return a;
    {
        int k = clz64(ma) - 11;
        return f64_sqrt_norm(env, -1022 - k, ma << k);
    }
}

f64 f64_sqrt(sf_env *env, f64 a)
{
    sf_u32 hi = (sf_u32)(a >> 32);
    if (SF_LIKELY((hi - 0x00100000u) < 0x7FE00000u)) {
        int e = (int)(hi >> 20) - 1023;
        sf_u64 m = (a & 0xFFFFFFFFFFFFFull) | 0x10000000000000ull;
        return f64_sqrt_norm(env, e, m);
    }
    return f64_sqrt_slow(env, a);
}

int f64_eq(sf_env *env, f64 a, f64 b)
{
    sf_u64 ma = a & 0x7FFFFFFFFFFFFFFFull, mb = b & 0x7FFFFFFFFFFFFFFFull;
    if (SF_UNLIKELY(ma > 0x7FF0000000000000ull || mb > 0x7FF0000000000000ull)) {
        if ((ma > 0x7FF0000000000000ull && (a & 0x0008000000000000ull)) ||
            (mb > 0x7FF0000000000000ull && (b & 0x0008000000000000ull)))
            env->flags |= SF_FLAG_V;
        return 0;
    }
    if (env->dn) {
        if (ma < 0x0010000000000000ull) { a = 0; ma = 0; }
        if (mb < 0x0010000000000000ull) { b = 0; mb = 0; }
    }
    return (a == b) || ((ma | mb) == 0);
}

int f64_gt(sf_env *env, f64 a, f64 b)
{
    sf_u64 ma = a & 0x7FFFFFFFFFFFFFFFull, mb = b & 0x7FFFFFFFFFFFFFFFull;
    sf_i64 ia, ib;
    if (SF_UNLIKELY(ma > 0x7FF0000000000000ull || mb > 0x7FF0000000000000ull)) {
        env->flags |= SF_FLAG_V;
        return 0;
    }
    if (env->dn) {
        if (ma < 0x0010000000000000ull) { a = 0; ma = 0; }
        if (mb < 0x0010000000000000ull) { b = 0; mb = 0; }
    }
    if ((ma | mb) == 0) return 0;
    ia = (a >> 63) ? -(sf_i64)ma : (sf_i64)ma;
    ib = (b >> 63) ? -(sf_i64)mb : (sf_i64)mb;
    return ia > ib;
}

/* -------------------------------------------------------- conversions */

f32 f32_from_i32(sf_env *env, sf_i32 v)
{
    sf_u32 sign = (sf_u32)v >> 31;
    sf_u32 mag = sign ? (0u - (sf_u32)v) : (sf_u32)v;
    int k;
    sf_u32 s;
    if (mag == 0) return 0u;
    k = clz32(mag);
    s = mag << k;
    return round_pack32(env, sign, 127 + 31 - k, (s >> 1) | (s & 1u));
}

f64 f64_from_i32(sf_env *env, sf_i32 v)
{
    sf_u32 sign = (sf_u32)v >> 31;
    sf_u32 mag = sign ? (0u - (sf_u32)v) : (sf_u32)v;
    int k;
    sf_u64 frac;
    (void)env;
    if (mag == 0) return 0ull;
    k = clz32(mag);
    frac = ((sf_u64)((mag << k) & 0x7FFFFFFFu)) << 21;
    return ((sf_u64)sign << 63) | ((sf_u64)(sf_u32)(1023 + 31 - k) << 52) | frac;
}

sf_i32 f32_to_i32(sf_env *env, f32 a)
{
    sf_u32 sign = a >> 31;
    int e = (int)((a >> 23) & 0xFFu) - 127;
    sf_u32 f = a & 0x7FFFFFu;
    sf_u32 m;
    if (e < 0) {
        if (e == -127 && (f == 0 || env->dn)) return 0;
        env->flags |= SF_FLAG_I;
        return 0;
    }
    if (e >= 31) {
        if (e == 31 && sign && f == 0) return (sf_i32)0x80000000u;
        env->flags |= SF_FLAG_V;
        if (e == 128 && f != 0) return 0x7FFFFFFF;
        return sign ? (sf_i32)0x80000000u : 0x7FFFFFFF;
    }
    m = f | 0x800000u;
    if (e >= 23) {
        m <<= (e - 23);
    } else {
        int sh = 23 - e;
        if (m << (32 - sh)) env->flags |= SF_FLAG_I;
        m >>= sh;
    }
    return sign ? (sf_i32)(0u - m) : (sf_i32)m;
}

sf_i32 f64_to_i32(sf_env *env, f64 a)
{
    sf_u32 hi = (sf_u32)(a >> 32);
    sf_u32 sign = hi >> 31;
    int e = (int)((hi >> 20) & 0x7FFu) - 1023;
    sf_u64 f = a & 0xFFFFFFFFFFFFFull;
    sf_u64 m64;
    sf_u32 mag;
    int sh;
    if (e < 0) {
        if (e == -1023 && (f == 0 || env->dn)) return 0;
        env->flags |= SF_FLAG_I;
        return 0;
    }
    if (e >= 31) {
        if (e == 31 && sign && f == 0) return (sf_i32)0x80000000u;
        env->flags |= SF_FLAG_V;
        if (e == 1024 && f != 0) return 0x7FFFFFFF;
        return sign ? (sf_i32)0x80000000u : 0x7FFFFFFF;
    }
    m64 = f | 0x10000000000000ull;
    sh = 52 - e;
    mag = (sf_u32)(m64 >> sh);
    if (m64 << (64 - sh)) env->flags |= SF_FLAG_I;
    return sign ? (sf_i32)(0u - mag) : (sf_i32)mag;
}

f64 f32_to_f64(sf_env *env, f32 a)
{
    sf_u32 sign = a >> 31;
    sf_u32 ef = (a >> 23) & 0xFFu;
    sf_u32 f = a & 0x7FFFFFu;
    if (SF_LIKELY((ef - 1u) < 0xFEu))
        return ((sf_u64)sign << 63) | ((sf_u64)(ef + (1023u - 127u)) << 52) | ((sf_u64)f << 29);
    if (ef == 0xFFu) {
        if (f == 0) return F64_INF(sign);
        if (f & 0x400000u) { env->flags |= SF_FLAG_V; return F64_DEFAULT_NAN; }
        return F64_INF(sign) | ((sf_u64)(f & 0x3FFFFFu) << 29);
    }
    if (f == 0 || env->dn) return F64_ZERO(sign);
    {
        int k = clz32(f) - 8;
        sf_u32 nf = (f << k) & 0x7FFFFFu;
        return ((sf_u64)sign << 63) | ((sf_u64)(sf_u32)(1023 - 126 - k) << 52) | ((sf_u64)nf << 29);
    }
}

f32 f64_to_f32(sf_env *env, f64 a)
{
    sf_u32 hi = (sf_u32)(a >> 32);
    sf_u32 sign = hi >> 31;
    sf_u32 ef = (hi >> 20) & 0x7FFu;
    sf_u64 f = a & 0xFFFFFFFFFFFFFull;
    if (SF_LIKELY((ef - 1u) < 0x7FEu)) {
        sf_u64 m64 = f | 0x10000000000000ull;
        sf_u32 sig31 = (sf_u32)(m64 >> 22) | (((sf_u32)m64 & 0x3FFFFFu) != 0);
        return round_pack32(env, sign, (int)ef - (1023 - 127), sig31);
    }
    if (ef == 0x7FFu) {
        if (f == 0) return F32_INF(sign);
        if (f & 0x8000000000000ull) { env->flags |= SF_FLAG_V; return F32_DEFAULT_NAN; }
        return F32_INF(sign) | (sf_u32)((a >> 29) & 0x3FFFFFu) | 0x1u;
    }
    if (f == 0 || env->dn) return F32_ZERO(sign);
    env->flags |= SF_FLAG_U | SF_FLAG_I;
    return F32_ZERO(sign);
}
