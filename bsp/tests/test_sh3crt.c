/* test_sh3crt.c - verify sh3softfloat.h bit-for-bit against host IEEE-754 FPU */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../classpad50/SRC/APPS/SH3CRT/sh3softfloat.h"

static uint32_t f2u(float f)  { uint32_t u; memcpy(&u, &f, 4); return u; }
static float    u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint64_t d2u(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static double   u2d(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }

static int fails;
static void chk32(const char *op, uint32_t a, uint32_t b, uint32_t got, uint32_t want)
{
    if (sf_f32_isnan(got) && sf_f32_isnan(want)) return;
    if (got != want) {
        if (fails < 20) fprintf(stderr, "FAIL %s(%08x, %08x): got %08x want %08x\n", op, a, b, got, want);
        fails++;
    }
}
static void chk64(const char *op, uint64_t a, uint64_t b, uint64_t got, uint64_t want)
{
    if (sf_f64_isnan(got) && sf_f64_isnan(want)) return;
    if (got != want) {
        if (fails < 20) fprintf(stderr, "FAIL %s(%016llx, %016llx): got %016llx want %016llx\n",
            op, (unsigned long long)a, (unsigned long long)b, (unsigned long long)got, (unsigned long long)want);
        fails++;
    }
}

static uint64_t rng = 0x123456789ABCDEF0ULL;
static uint64_t next64(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

int main(void)
{
    static const uint32_t f32_edge[] = {
        0x00000000u, 0x80000000u, 0x00000001u, 0x80000001u, 0x007FFFFFu, 0x00800000u,
        0x3F000000u, 0x3F800000u, 0xBF800000u, 0x3F800001u, 0x40000000u, 0x40490FDBu,
        0x4B000000u, 0x4EFFFFFFu, 0x4F000000u, 0x4F7FFFFFu, 0x4F800000u, 0xCF000000u,
        0x7F7FFFFFu, 0xFF7FFFFFu, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0x7F800001u
    };
    static const uint64_t f64_edge[] = {
        0x0000000000000000ULL, 0x8000000000000000ULL, 0x0000000000000001ULL, 0x8000000000000001ULL,
        0x000FFFFFFFFFFFFFULL, 0x0010000000000000ULL, 0x3FE0000000000000ULL, 0x3FEFFFFFFFFFFFFFULL,
        0x3FF0000000000000ULL, 0xBFF0000000000000ULL, 0x3FF0000000000001ULL, 0x3FF8000000000000ULL,
        0xBFF8000000000000ULL, 0x4000000000000000ULL, 0x400921FB54442D18ULL, 0xC00921FB54442D18ULL,
        0x41DFFFFFFFC00000ULL, 0x41E0000000000000ULL, 0xC1E0000000000000ULL, 0x41EFFFFFFFE00000ULL,
        0x4330000000000000ULL, 0x43DFFFFFFFFFFFFFULL, 0x43E0000000000000ULL, 0xC3E0000000000000ULL,
        0x7FEFFFFFFFFFFFFFULL, 0xFFEFFFFFFFFFFFFFULL, 0x7FF0000000000000ULL, 0xFFF0000000000000ULL,
        0x7FF8000000000000ULL, 0x7FF0000000000001ULL
    };
    size_t i, j;
    int k;

    for (i = 0; i < sizeof f32_edge / sizeof f32_edge[0]; i++) {
        uint32_t a = f32_edge[i];
        float fa = u2f(a);
        chk64("f32_to_f64", a, 0, sf_f32_to_f64(a), d2u((double)fa));
        if (!isnan(fa) && fa >= -2147483648.0f && fa < 2147483648.0f) {
            if (sf_f32_to_i32(a) != (int32_t)fa) { fprintf(stderr, "FAIL f32_to_i32 %08x\n", a); fails++; }
        }
        if (!isnan(fa) && fa >= 0.0f && fa < 4294967296.0f) {
            if (sf_f32_to_u32(a) != (uint32_t)fa) { fprintf(stderr, "FAIL f32_to_u32 %08x\n", a); fails++; }
        }
        for (j = 0; j < sizeof f32_edge / sizeof f32_edge[0]; j++) {
            uint32_t b = f32_edge[j];
            float fb = u2f(b);
            chk32("f32_add", a, b, sf_f32_add(a, b), f2u(fa + fb));
            chk32("f32_sub", a, b, sf_f32_sub(a, b), f2u(fa - fb));
            chk32("f32_mul", a, b, sf_f32_mul(a, b), f2u(fa * fb));
            chk32("f32_div", a, b, sf_f32_div(a, b), f2u(fa / fb));
            if (sf_f32_eq(a, b) != (fa == fb)) fails++;
            if (sf_f32_lt(a, b) != (fa < fb)) fails++;
            if (sf_f32_le(a, b) != (fa <= fb)) fails++;
        }
    }

    for (i = 0; i < sizeof f64_edge / sizeof f64_edge[0]; i++) {
        uint64_t a = f64_edge[i];
        double da = u2d(a);
        chk32("f64_to_f32", (uint32_t)(a >> 32), (uint32_t)a, sf_f64_to_f32(a), f2u((float)da));
        chk64("f64_sqrt", a, 0, sf_f64_sqrt(a), d2u(sqrt(da)));
        chk64("f64_floor", a, 0, sf_f64_floor(a), d2u(floor(da)));
        chk64("f64_ceil", a, 0, sf_f64_ceil(a), d2u(ceil(da)));
        if (!isnan(da) && da >= -2147483648.0 && da <= 2147483647.0) {
            if (sf_f64_to_i32(a) != (int32_t)da) { fprintf(stderr, "FAIL f64_to_i32 %016llx\n", (unsigned long long)a); fails++; }
        }
        if (!isnan(da) && da >= 0.0 && da <= 4294967295.0) {
            if (sf_f64_to_u32(a) != (uint32_t)da) { fprintf(stderr, "FAIL f64_to_u32 %016llx\n", (unsigned long long)a); fails++; }
        }
        if (!isnan(da) && da >= -9223372036854775808.0 && da < 9223372036854775808.0) {
            if (sf_f64_to_i64(a) != (int64_t)da) { fprintf(stderr, "FAIL f64_to_i64 %016llx\n", (unsigned long long)a); fails++; }
        }
        if (!isnan(da) && da >= 0.0 && da < 18446744073709551616.0) {
            if (sf_f64_to_u64(a) != (uint64_t)da) { fprintf(stderr, "FAIL f64_to_u64 %016llx\n", (unsigned long long)a); fails++; }
        }
        for (j = 0; j < sizeof f64_edge / sizeof f64_edge[0]; j++) {
            uint64_t b = f64_edge[j];
            double db = u2d(b);
            chk64("f64_add", a, b, sf_f64_add(a, b), d2u(da + db));
            chk64("f64_sub", a, b, sf_f64_sub(a, b), d2u(da - db));
            chk64("f64_mul", a, b, sf_f64_mul(a, b), d2u(da * db));
            chk64("f64_div", a, b, sf_f64_div(a, b), d2u(da / db));
            if (sf_f64_eq(a, b) != (da == db)) fails++;
            if (sf_f64_lt(a, b) != (da < db)) fails++;
            if (sf_f64_le(a, b) != (da <= db)) fails++;
        }
    }

    /* Random stress: 10,000 float & double ops + integer conversions */
    for (k = 0; k < 10000; k++) {
        uint32_t a32 = (uint32_t)next64(), b32 = (uint32_t)next64();
        uint64_t a64 = next64(), b64 = next64();
        float fa = u2f(a32), fb = u2f(b32);
        double da = u2d(a64), db = u2d(b64);
        int32_t si = (int32_t)a32;
        uint32_t ui = b32;
        int64_t si64 = (int64_t)a64;
        uint64_t ui64 = b64;
        chk32("r_f32_add", a32, b32, sf_f32_add(a32, b32), f2u(fa + fb));
        chk32("r_f32_mul", a32, b32, sf_f32_mul(a32, b32), f2u(fa * fb));
        chk32("r_f32_div", a32, b32, sf_f32_div(a32, b32), f2u(fa / fb));
        chk32("r_i32_f32", (uint32_t)si, 0, sf_i32_to_f32(si), f2u((float)si));
        chk32("r_u32_f32", ui, 0, sf_u32_to_f32(ui), f2u((float)ui));
        chk64("r_f64_add", a64, b64, sf_f64_add(a64, b64), d2u(da + db));
        chk64("r_f64_mul", a64, b64, sf_f64_mul(a64, b64), d2u(da * db));
        chk64("r_f64_div", a64, b64, sf_f64_div(a64, b64), d2u(da / db));
        chk64("r_f64_sqrt", a64 & ~SF_F64_SIGN, 0, sf_f64_sqrt(a64 & ~SF_F64_SIGN), d2u(sqrt(fabs(da))));
        chk64("r_f64_floor", a64, 0, sf_f64_floor(a64), d2u(floor(da)));
        chk64("r_f64_ceil", a64, 0, sf_f64_ceil(a64), d2u(ceil(da)));
        chk32("r_f64_f32", (uint32_t)(a64 >> 32), (uint32_t)a64, sf_f64_to_f32(a64), f2u((float)da));
        chk64("r_f32_f64", a32, 0, sf_f32_to_f64(a32), d2u((double)fa));
        chk64("r_i32_f64", (uint32_t)si, 0, sf_i32_to_f64(si), d2u((double)si));
        chk64("r_u32_f64", ui, 0, sf_u32_to_f64(ui), d2u((double)ui));
        chk64("r_i64_f64", a64, 0, sf_i64_to_f64(si64), d2u((double)si64));
        chk64("r_u64_f64", b64, 0, sf_u64_to_f64(ui64), d2u((double)ui64));
    }

    printf("test_sh3crt: %d failures\n", fails);
    return fails ? 1 : 0;
}
