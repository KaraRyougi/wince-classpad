/*
 * softfloat.h - integer-only IEEE 754 single/double arithmetic with SH-4 FPU
 * semantics (NaN encoding, flush-to-zero via DN, RN/RZ rounding only).
 *
 * This file must stay free of float/double types: the kernel build is for a
 * core with no FPU and any floating-point C would recurse into the trap.
 */
#ifndef SOFTFLOAT_H
#define SOFTFLOAT_H

#ifdef _MSC_VER
typedef unsigned __int64 sf_u64;
typedef __int64          sf_i64;
typedef unsigned int     sf_u32;
typedef int              sf_i32;
#else
#include <stdint.h>
typedef uint64_t sf_u64;
typedef int64_t  sf_i64;
typedef uint32_t sf_u32;
typedef int32_t  sf_i32;
#endif

typedef sf_u32 f32;   /* raw IEEE single bits */
typedef sf_u64 f64;   /* raw IEEE double bits */

/* Rounding modes: SH-4 FPSCR.RM supports only these two. */
#define SF_RN 0
#define SF_RZ 1

/* Exception flags, in the order of the FPSCR flag/enable/cause fields
 * (bit 0 of this mask = Inexact = FPSCR bit 2 / 7 / 12). */
#define SF_FLAG_I 0x01u   /* inexact */
#define SF_FLAG_U 0x02u   /* underflow */
#define SF_FLAG_O 0x04u   /* overflow */
#define SF_FLAG_Z 0x08u   /* divide by zero */
#define SF_FLAG_V 0x10u   /* invalid operation */

typedef struct {
    int    rm;      /* SF_RN or SF_RZ */
    int    dn;      /* 1 = denormals are flushed to zero (SH-4 FPSCR.DN) */
    sf_u32 flags;   /* accumulated SF_FLAG_* */
} sf_env;

/* SH-4 default quiet NaNs. On SH-4 the fraction MSB is 0 for qNaN, 1 for sNaN
 * (the opposite of the IEEE 754-2008 convention used by x86/ARM). */
#define F32_DEFAULT_NAN 0x7FBFFFFFu
#define F64_DEFAULT_NAN 0x7FF7FFFFFFFFFFFFull

f32 f32_add (sf_env *env, f32 a, f32 b);
f32 f32_sub (sf_env *env, f32 a, f32 b);
f32 f32_mul (sf_env *env, f32 a, f32 b);
f32 f32_div (sf_env *env, f32 a, f32 b);
f32 f32_sqrt(sf_env *env, f32 a);
f32 f32_fmac(sf_env *env, f32 a, f32 b, f32 c);  /* a*b + c, single rounding */
int f32_eq  (sf_env *env, f32 a, f32 b);
int f32_gt  (sf_env *env, f32 a, f32 b);

f64 f64_add (sf_env *env, f64 a, f64 b);
f64 f64_sub (sf_env *env, f64 a, f64 b);
f64 f64_mul (sf_env *env, f64 a, f64 b);
f64 f64_div (sf_env *env, f64 a, f64 b);
f64 f64_sqrt(sf_env *env, f64 a);
int f64_eq  (sf_env *env, f64 a, f64 b);
int f64_gt  (sf_env *env, f64 a, f64 b);

f32    f32_from_i32(sf_env *env, sf_i32 v);   /* FLOAT, PR=0 */
f64    f64_from_i32(sf_env *env, sf_i32 v);   /* FLOAT, PR=1 */
sf_i32 f32_to_i32  (sf_env *env, f32 a);      /* FTRC,  PR=0 (truncate) */
sf_i32 f64_to_i32  (sf_env *env, f64 a);      /* FTRC,  PR=1 (truncate) */
f64    f32_to_f64  (sf_env *env, f32 a);      /* FCNVSD */
f32    f64_to_f32  (sf_env *env, f64 a);      /* FCNVDS */

int f32_is_nan(f32 a);
int f64_is_nan(f64 a);

#endif
