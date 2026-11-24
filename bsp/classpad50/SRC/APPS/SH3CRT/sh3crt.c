//------------------------------------------------------------------------------
//  sh3crt.c - SH3CRT.dll: the floating-point runtime of SH-3 binaries on SH-4
//
//  Pocket PC SH-3 code (Math Xpander, MFC CE 3.0) has no FPU instructions:
//  the compiler calls coredll's software floating-point helpers (_adds,
//  _addd, _dtoi, ...) and passes doubles to C library functions (sin, pow,
//  atof, ...) in general registers. CE 6.0's SH-4 coredll exports the same
//  names, but built for the SH-4 FPU convention (arguments in fr4/dr4,
//  results in fr0/dr0), so an SH-3 caller gets garbage: Math Xpander's math
//  keyboard computes which key a tap hit with _adds/_stod/_addd/_dtoi and
//  never hits one.
//
//  These are the same functions with the SH-3 convention, written with
//  integer, pointer and bit-pattern parameters only, so the SH-4 compiler's
//  (unchanged) integer convention matches the SH-3 caller exactly; the work
//  is done by the SH-4 FPU and coredll. The SH-3 convention, read from the
//  callers in the Math Xpander and MFC binaries:
//    float          32-bit pattern in a general register, result in r0
//    double helper  by address: _addd(&r, &a, &b), _negd(&r, &a),
//                   _eqd(&a, &b) -> int, _dtoi(&a) -> int, _dtos(&a) ->
//                   float, _stod(&r, f), _itod(&r, i), _i64tod(&r, &i64),
//                   _dtoi64(&r64, &a)
//    C function     double arguments by value as two words (low word first)
//                   in the next argument slots (r4..r7, then the stack);
//                   a double result through a hidden pointer in r4, which is
//                   also returned in r0
//  tools/sh3imports.py points the SH-3 modules' imports of these names at
//  SH3CRT.dll (stage.py does it for every SH-3 module in platform.bib).
//------------------------------------------------------------------------------
#if defined(_MSC_VER) && !defined(DEBUG)
#pragma optimize("t", on)
#endif
#include <windows.h>
#include <math.h>
#include <float.h>
#include <stdlib.h>
#include "sh3softfloat.h"

typedef unsigned int U32;
typedef unsigned __int64 U64;

typedef union { double d; U32 w[2]; } DBITS;

static double D(U32 lo, U32 hi) { DBITS u; u.w[0] = lo; u.w[1] = hi; return u.d; }
static __inline U64 RD(const double *p)
{
    const U32 *w = (const U32 *)p;
    return (U64)w[0] | ((U64)w[1] << 32);
}
static __inline double *WD(double *r, U64 v)
{
    U32 *w = (U32 *)r;
    w[0] = (U32)v;
    w[1] = (U32)(v >> 32);
    return r;
}

BOOL WINAPI DllMain(HANDLE hInst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls((HMODULE)hInst);
    return TRUE;
}

// ---------------------------------------------------------------- float (pure integer: zero FPU traps)
U32 sh3_adds(U32 a, U32 b) { return sf_f32_add(a, b); }
U32 sh3_subs(U32 a, U32 b) { return sf_f32_sub(a, b); }
U32 sh3_muls(U32 a, U32 b) { return sf_f32_mul(a, b); }
U32 sh3_divs(U32 a, U32 b) { return sf_f32_div(a, b); }
U32 sh3_negs(U32 a)        { return a ^ SF_F32_SIGN; }
int sh3_eqs(U32 a, U32 b)  { return sf_f32_eq(a, b); }
int sh3_nes(U32 a, U32 b)  { return !sf_f32_eq(a, b); }
int sh3_lts(U32 a, U32 b)  { return sf_f32_lt(a, b); }
int sh3_les(U32 a, U32 b)  { return sf_f32_le(a, b); }
int sh3_gts(U32 a, U32 b)  { return sf_f32_lt(b, a); }
int sh3_ges(U32 a, U32 b)  { return sf_f32_le(b, a); }
U32 sh3_itos(int i)        { return sf_i32_to_f32(i); }
U32 sh3_utos(U32 u)        { return sf_u32_to_f32(u); }
int sh3_stoi(U32 a)        { return sf_f32_to_i32(a); }
U32 sh3_stou(U32 a)        { return sf_f32_to_u32(a); }

// ------------------------------------------------------- double helpers (pure integer: zero FPU traps)
double *sh3_addd(double *r, const double *a, const double *b) { return WD(r, sf_f64_add(RD(a), RD(b))); }
double *sh3_subd(double *r, const double *a, const double *b) { return WD(r, sf_f64_sub(RD(a), RD(b))); }
double *sh3_muld(double *r, const double *a, const double *b) { return WD(r, sf_f64_mul(RD(a), RD(b))); }
double *sh3_divd(double *r, const double *a, const double *b) { return WD(r, sf_f64_div(RD(a), RD(b))); }
double *sh3_negd(double *r, const double *a)                  { return WD(r, RD(a) ^ SF_F64_SIGN); }
int sh3_eqd(const double *a, const double *b) { return sf_f64_eq(RD(a), RD(b)); }
int sh3_ned(const double *a, const double *b) { return !sf_f64_eq(RD(a), RD(b)); }
int sh3_ltd(const double *a, const double *b) { return sf_f64_lt(RD(a), RD(b)); }
int sh3_led(const double *a, const double *b) { return sf_f64_le(RD(a), RD(b)); }
int sh3_gtd(const double *a, const double *b) { return sf_f64_lt(RD(b), RD(a)); }
int sh3_ged(const double *a, const double *b) { return sf_f64_le(RD(b), RD(a)); }
int sh3_dtoi(const double *a)                 { return sf_f64_to_i32(RD(a)); }
U32 sh3_dtou(const double *a)                 { return sf_f64_to_u32(RD(a)); }
U32 sh3_dtos(const double *a)                 { return sf_f64_to_f32(RD(a)); }
double *sh3_stod(double *r, U32 f)            { return WD(r, sf_f32_to_f64(f)); }
double *sh3_itod(double *r, int i)            { return WD(r, sf_i32_to_f64(i)); }
double *sh3_utod(double *r, U32 u)            { return WD(r, sf_u32_to_f64(u)); }
double *sh3_i64tod(double *r, const __int64 *i) { return WD(r, sf_i64_to_f64(*i)); }
double *sh3_ui64tod(double *r, const U64 *u)   { return WD(r, sf_u64_to_f64(*u)); }
__int64 *sh3_dtoi64(__int64 *r, const double *a) { *r = sf_f64_to_i64(RD(a)); return r; }
U64 *sh3_dtoui64(U64 *r, const double *a)     { *r = sf_f64_to_u64(RD(a)); return r; }

// ---------------------------------------------------- C library, double
#define ONE(name) double *sh3_##name(double *r, U32 lo, U32 hi) { *r = name(D(lo, hi)); return r; }
#define TWO(name) double *sh3_##name(double *r, U32 lo, U32 hi, U32 lo2, U32 hi2) { *r = name(D(lo, hi), D(lo2, hi2)); return r; }
ONE(sin) ONE(cos) ONE(tan) ONE(asin) ONE(acos) ONE(atan)
ONE(sinh) ONE(cosh) ONE(tanh)
ONE(exp) ONE(log) ONE(log10)
TWO(atan2) TWO(pow) TWO(fmod) TWO(_hypot) TWO(_nextafter)

double *sh3_sqrt(double *r, U32 lo, U32 hi)  { return WD(r, sf_f64_sqrt((U64)lo | ((U64)hi << 32))); }
double *sh3_floor(double *r, U32 lo, U32 hi) { return WD(r, sf_f64_floor((U64)lo | ((U64)hi << 32))); }
double *sh3_ceil(double *r, U32 lo, U32 hi)  { return WD(r, sf_f64_ceil((U64)lo | ((U64)hi << 32))); }
double *sh3_fabs(double *r, U32 lo, U32 hi)  { U32 *w = (U32 *)r; w[0] = lo; w[1] = hi & 0x7FFFFFFFu; return r; }

double *sh3_atof(double *r, const char *s)                  { *r = atof(s); return r; }
double *sh3_wcstod(double *r, const wchar_t *s, wchar_t **e) { *r = wcstod(s, e); return r; }
int sh3__isnan(U32 lo, U32 hi)   { return ((hi & 0x7FF00000u) == 0x7FF00000u) && (((hi & 0x000FFFFFu) | lo) != 0); }
int sh3__finite(U32 lo, U32 hi)  { (void)lo; return (hi & 0x7FF00000u) != 0x7FF00000u; }
int sh3__fpclass(U32 lo, U32 hi) { return _fpclass(D(lo, hi)); }
