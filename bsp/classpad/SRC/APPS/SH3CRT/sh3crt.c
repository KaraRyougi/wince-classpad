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
#include <windows.h>
#include <math.h>
#include <float.h>
#include <stdlib.h>

typedef unsigned int U32;
typedef unsigned __int64 U64;

typedef union { double d; U32 w[2]; } DBITS;
typedef union { float f; U32 w; } FBITS;

static double D(U32 lo, U32 hi) { DBITS u; u.w[0] = lo; u.w[1] = hi; return u.d; }
static float  F(U32 bits)       { FBITS u; u.w = bits; return u.f; }
static U32    FB(float f)       { FBITS u; u.f = f; return u.w; }

BOOL WINAPI DllMain(HANDLE hInst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls((HMODULE)hInst);
    return TRUE;
}

// ---------------------------------------------------------------- float
U32 sh3_adds(U32 a, U32 b) { return FB(F(a) + F(b)); }
U32 sh3_subs(U32 a, U32 b) { return FB(F(a) - F(b)); }
U32 sh3_muls(U32 a, U32 b) { return FB(F(a) * F(b)); }
U32 sh3_divs(U32 a, U32 b) { return FB(F(a) / F(b)); }
U32 sh3_negs(U32 a)        { return FB(-F(a)); }
int sh3_eqs(U32 a, U32 b)  { return F(a) == F(b); }
int sh3_nes(U32 a, U32 b)  { return F(a) != F(b); }
int sh3_lts(U32 a, U32 b)  { return F(a) <  F(b); }
int sh3_les(U32 a, U32 b)  { return F(a) <= F(b); }
int sh3_gts(U32 a, U32 b)  { return F(a) >  F(b); }
int sh3_ges(U32 a, U32 b)  { return F(a) >= F(b); }
U32 sh3_itos(int i)        { return FB((float)i); }
U32 sh3_utos(U32 u)        { return FB((float)u); }
int sh3_stoi(U32 a)        { return (int)F(a); }
U32 sh3_stou(U32 a)        { return (U32)F(a); }

// ------------------------------------------------------- double helpers
double *sh3_addd(double *r, const double *a, const double *b) { *r = *a + *b; return r; }
double *sh3_subd(double *r, const double *a, const double *b) { *r = *a - *b; return r; }
double *sh3_muld(double *r, const double *a, const double *b) { *r = *a * *b; return r; }
double *sh3_divd(double *r, const double *a, const double *b) { *r = *a / *b; return r; }
double *sh3_negd(double *r, const double *a)                  { *r = -*a; return r; }
int sh3_eqd(const double *a, const double *b) { return *a == *b; }
int sh3_ned(const double *a, const double *b) { return *a != *b; }
int sh3_ltd(const double *a, const double *b) { return *a <  *b; }
int sh3_led(const double *a, const double *b) { return *a <= *b; }
int sh3_gtd(const double *a, const double *b) { return *a >  *b; }
int sh3_ged(const double *a, const double *b) { return *a >= *b; }
int sh3_dtoi(const double *a)                 { return (int)*a; }
U32 sh3_dtou(const double *a)                 { return (U32)*a; }
U32 sh3_dtos(const double *a)                 { return FB((float)*a); }
double *sh3_stod(double *r, U32 f)            { *r = (double)F(f); return r; }
double *sh3_itod(double *r, int i)            { *r = (double)i; return r; }
double *sh3_utod(double *r, U32 u)            { *r = (double)u; return r; }
double *sh3_i64tod(double *r, const __int64 *i) { *r = (double)*i; return r; }
double *sh3_ui64tod(double *r, const U64 *u)   { *r = (double)*u; return r; }
__int64 *sh3_dtoi64(__int64 *r, const double *a) { *r = (__int64)*a; return r; }
U64 *sh3_dtoui64(U64 *r, const double *a)     { *r = (U64)*a; return r; }

// ---------------------------------------------------- C library, double
#define ONE(name) double *sh3_##name(double *r, U32 lo, U32 hi) { *r = name(D(lo, hi)); return r; }
#define TWO(name) double *sh3_##name(double *r, U32 lo, U32 hi, U32 lo2, U32 hi2) { *r = name(D(lo, hi), D(lo2, hi2)); return r; }
ONE(sin) ONE(cos) ONE(tan) ONE(asin) ONE(acos) ONE(atan)
ONE(sinh) ONE(cosh) ONE(tanh)
ONE(exp) ONE(log) ONE(log10) ONE(sqrt) ONE(floor) ONE(ceil) ONE(fabs)
TWO(atan2) TWO(pow) TWO(fmod) TWO(_hypot) TWO(_nextafter)

double *sh3_atof(double *r, const char *s)                  { *r = atof(s); return r; }
double *sh3_wcstod(double *r, const wchar_t *s, wchar_t **e) { *r = wcstod(s, e); return r; }
int sh3__isnan(U32 lo, U32 hi)   { return _isnan(D(lo, hi)); }
int sh3__finite(U32 lo, U32 hi)  { return _finite(D(lo, hi)); }
int sh3__fpclass(U32 lo, U32 hi) { return _fpclass(D(lo, hi)); }
