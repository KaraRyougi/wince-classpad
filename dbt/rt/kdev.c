/*
 * kdev.c - which parts of kernel.dll need the device-aware translation.
 *
 * Every 32-bit kernel.dll access used to test its address for P4 at run
 * time (translate.c tr_mem_dev), about 2-2.5 s of a 26 s My Device load,
 * yet only 23 instruction sites in 9 functions ever reach a control
 * register (docs/04). tools/kernel_devfuncs.py picks the functions that may
 * (P4 literals, banked registers, the exception vector area) and writes
 * kernel_dev.h: per function a relocation-insensitive signature. Here they
 * are found in the image actually booted: kernel.dll is relinked with every
 * image, so addresses move while the code stays the same. A function that
 * is missing or found twice means a different kernel: then the caller keeps
 * the whole of kernel.dll device-aware, as before.
 *
 * The masking must match tools/kernel_devfuncs.py `masked`: PC-relative
 * literals become 0, bra/bsr leaving the function keep only their opcode,
 * aligned word pairs whose little-endian value is a P1/P2 address become 0.
 */
#include "rt.h"
#include "kdev.h"

typedef __SIZE_TYPE__ kdev_size_t;
void *memset(void *d, int c, kdev_size_t n);

#include "kernel_dev.h"

#define KDEV_MAX_WORDS 2048          /* the generator refuses longer functions */

static unsigned rd16(sf_u32 a)           /* guest halfword (little-endian), any source */
{
    const volatile sf_u8 *p = (const volatile sf_u8 *)a;
    return p[0] | ((unsigned)p[1] << 8);
}

static sf_u16 g_raw[KDEV_MAX_WORDS], g_out[KDEV_MAX_WORDS];
static sf_u8 g_lit[KDEV_MAX_WORDS];

/* n words at base, masked as for a function [lo, hi), into g_out */
static void kmask(sf_u32 base, unsigned n, sf_u32 lo, sf_u32 hi)
{
    unsigned i;
    memset(g_lit, 0, n);
    for (i = 0; i < n; ++i) {
        unsigned w = rd16(base + 2 * i);
        sf_u32 pc = base + 2 * i, a, j;
        g_raw[i] = (sf_u16)w;
        if ((w >> 12) == 0xD) {
            a = ((pc + 4) & ~3u) + 4 * (w & 0xFF); j = (a - base) / 2;
            if (j < n) g_lit[j] = 1;
            if (j + 1 < n) g_lit[j + 1] = 1;
        } else if ((w >> 12) == 0x9) {
            a = pc + 4 + 2 * (w & 0xFF); j = (a - base) / 2;
            if (j < n) g_lit[j] = 1;
        }
    }
    for (i = 0; i < n; ++i) {
        unsigned w = g_raw[i];
        if (g_lit[i]) w = 0;
        else if ((w >> 12) == 0xA || (w >> 12) == 0xB) {
            sf_u32 d = w & 0xFFF, t = base + 2 * i + 4 + 2 * ((d & 0x800) ? d - 0x1000 : d);
            if (t < lo || t >= hi) w &= 0xF000;
        }
        g_out[i] = (sf_u16)w;
    }
    for (i = 0; i + 1 < n; ++i)
        if (((base + 2 * i) & 3) == 0 && g_raw[i + 1] >= 0x8000 && g_raw[i + 1] < 0xC000 && !g_lit[i])
            g_out[i] = g_out[i + 1] = 0;
}

static sf_u32 crc32_words(unsigned n)      /* zlib CRC-32 of g_out as big-endian halfwords */
{
    sf_u32 crc = 0xFFFFFFFFu;
    unsigned i, k, b;
    for (i = 0; i < n; ++i)
        for (b = 0; b < 2; ++b) {
            crc ^= b ? (g_out[i] & 0xFFu) : (g_out[i] >> 8);
            for (k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
        }
    return ~crc;
}

unsigned kdev_locate(sf_u32 text, sf_u32 len, sf_u32 *ranges, unsigned max)
{
    sf_u32 at[KDEV_FUNCS], pos, end = text + len;
    unsigned hits[KDEV_FUNCS], f, n = 0, i;
    if (max < KDEV_FUNCS) return 0;
    for (f = 0; f < KDEV_FUNCS; ++f) hits[f] = 0;
    for (pos = text; pos + 8 <= end; pos += 2) {
        sf_u16 m0, m1, m2, m3;
        kmask(pos, 4, pos, pos);
        m0 = g_out[0]; m1 = g_out[1]; m2 = g_out[2]; m3 = g_out[3];
        for (f = 0; f < KDEV_FUNCS; ++f) {
            if (kdev_funcs[f].w[0] != m0 || kdev_funcs[f].w[1] != m1 || kdev_funcs[f].w[2] != m2 ||
                kdev_funcs[f].w[3] != m3 || pos + kdev_funcs[f].len > end) continue;
            kmask(pos, kdev_funcs[f].len / 2, pos, pos + kdev_funcs[f].len);
            if (crc32_words(kdev_funcs[f].len / 2) == kdev_funcs[f].crc) { ++hits[f]; at[f] = pos; }
        }
    }
    for (f = 0; f < KDEV_FUNCS; ++f)
        if (hits[f] != 1) {
            dbt_puts("[kdev] kernel function "); dbt_puthex(f); dbt_puts(" found "); dbt_puthex(hits[f]);
            dbt_puts(" times: the whole kernel.dll stays device-aware\n");
            return 0;
        }
    /* sorted, P1/P2 folded like the translator's test, adjacent ones merged */
    for (f = 0; f < KDEV_FUNCS; ++f) {
        sf_u32 lo = at[f] & 0x1FFFFFFFu, hi = lo + kdev_funcs[f].len;
        for (i = n; i > 0 && ranges[2 * (i - 1)] > lo; --i) {
            ranges[2 * i] = ranges[2 * (i - 1)]; ranges[2 * i + 1] = ranges[2 * (i - 1) + 1];
        }
        ranges[2 * i] = lo; ranges[2 * i + 1] = hi; ++n;
    }
    for (i = 1, f = 0; i < n; ++i) {
        if (ranges[2 * i] <= ranges[2 * f + 1]) {
            if (ranges[2 * i + 1] > ranges[2 * f + 1]) ranges[2 * f + 1] = ranges[2 * i + 1];
        } else { ++f; ranges[2 * f] = ranges[2 * i]; ranges[2 * f + 1] = ranges[2 * i + 1]; }
    }
    return n ? f + 1 : 0;
}
