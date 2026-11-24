/*
 * kdev.c - which parts of the kernel module need the device-aware translation.
 *
 * CE 6.0: kernel.dll (rt/kernel_dev.h). CE 5.0: nk.exe, kernel and OAL in one
 * module (rt/kernel_dev50.h, generated from the CE 5.0 kern.exe/kern.map with
 * the same script; the OAL's OEMCacheRangeFlush is among its functions). The
 * caller says which from the module table (nkbin.c kernel_is_nk).
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

#include "kernel_dev.h"          /* CE 6.0 kernel.dll: kdev_funcs[KDEV_FUNCS] */
#include "kernel_dev50.h"        /* CE 5.0 nk.exe (kernel + OAL): kdev_funcs50[KDEV_FUNCS50] */

/* both generated tables have this layout */
typedef struct { sf_u16 w[4]; sf_u32 len, crc, hint; } kdev_func_t;   /* hint: the function's P1 address in the image the table was made from, 0 = unknown */

#define KDEV_MAX_WORDS 2048          /* the generator refuses longer functions */

static unsigned rd16(sf_u32 a)           /* guest halfword (little-endian), any source */
{
    const volatile sf_u8 *p = (const volatile sf_u8 *)a;
    return p[0] | ((unsigned)p[1] << 8);
}

/* Boot-time scratch overlaid onto rt.c's g_arena (before dbt_rt_init):
 * saves 18 KiB of permanent BSS. */
static sf_u16 *g_raw, *g_out;
static sf_u8 *g_lit, *g_w0set;

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

unsigned kdev_locate(sf_u32 text, sf_u32 len, sf_u32 *ranges, unsigned max, int is_nk)
{
    const kdev_func_t *tab = is_nk ? (const kdev_func_t *)kdev_funcs50 : (const kdev_func_t *)kdev_funcs;
    const unsigned nf = is_nk ? KDEV_FUNCS50 : KDEV_FUNCS;
    const char *mod = is_nk ? "nk.exe" : "kernel.dll";
    sf_u32 at[KDEV_MAX_RANGES], pos, end = text + len;
    unsigned hits[KDEV_MAX_RANGES], f, n = 0, i, any_zero = 0, any_br = 0;
    {
        sf_u8 *scr = dbt_kdev_arena();
        g_raw = (sf_u16 *)scr;
        g_out = (sf_u16 *)(scr + KDEV_MAX_WORDS * 2u);
        g_lit = scr + KDEV_MAX_WORDS * 4u;
        g_w0set = scr + KDEV_MAX_WORDS * 5u;
    }
    if (max < nf || nf > KDEV_MAX_RANGES) return 0;
    /* The image this table was generated from, or a relink that left these
     * functions where they were (nk.exe and the translator ship together):
     * check every signature at its recorded address and skip the scan. */
    for (f = 0; f < nf; ++f) {
        sf_u32 h = tab[f].hint;
        if (!h || h < text || h + tab[f].len > end) break;
        kmask(h, 4, h, h);
        if (g_out[0] != tab[f].w[0] || g_out[1] != tab[f].w[1] || g_out[2] != tab[f].w[2] || g_out[3] != tab[f].w[3]) break;
        kmask(h, tab[f].len / 2, h, h + tab[f].len);
        if (crc32_words(tab[f].len / 2) != tab[f].crc) break;
        at[f] = h; hits[f] = 1;
    }
    if (f == nf) { dbt_puts("[kdev] "); dbt_puts(mod); dbt_puts(" functions at their recorded addresses\n"); goto found; }
    /* Cheap pre-filter on the raw first word (the 4-word masking took 7 s of
     * a CE 5.0 boot when done at every position): masked word 0 is the raw
     * word, or 0 for an aligned P1/P2 address pair (a literal inside the
     * window can only mask words 1-3), or the bare bra/bsr opcode. */
    memset(g_w0set, 0, 65536 / 8);
    for (f = 0; f < nf; ++f) {
        unsigned w0 = tab[f].w[0];
        hits[f] = 0;
        g_w0set[w0 >> 3] |= (sf_u8)(1u << (w0 & 7));
        if (w0 == 0) any_zero = 1;
        if (w0 == 0xA000 || w0 == 0xB000) any_br = 1;
    }
    for (pos = text; pos + 8 <= end; pos += 2) {
        sf_u16 m0, m1, m2, m3;
        unsigned raw0 = rd16(pos), cand = (g_w0set[raw0 >> 3] >> (raw0 & 7)) & 1;
        if (!cand && any_zero && !(pos & 3)) { unsigned raw1 = rd16(pos + 2); cand = raw1 >= 0x8000 && raw1 < 0xC000; }
        if (!cand && any_br && ((raw0 >> 12) == 0xA || (raw0 >> 12) == 0xB)) cand = (g_w0set[(raw0 & 0xF000) >> 3] >> ((raw0 & 0xF000) & 7)) & 1;
        if (!cand) continue;
        kmask(pos, 4, pos, pos);
        m0 = g_out[0]; m1 = g_out[1]; m2 = g_out[2]; m3 = g_out[3];
        for (f = 0; f < nf; ++f) {
            if (tab[f].w[0] != m0 || tab[f].w[1] != m1 || tab[f].w[2] != m2 ||
                tab[f].w[3] != m3 || pos + tab[f].len > end) continue;
            kmask(pos, tab[f].len / 2, pos, pos + tab[f].len);
            if (crc32_words(tab[f].len / 2) == tab[f].crc) { ++hits[f]; at[f] = pos; }
        }
    }
    for (f = 0; f < nf; ++f)
        if (hits[f] != 1) {
            dbt_puts("[kdev] "); dbt_puts(mod); dbt_puts(" function "); dbt_puthex(f); dbt_puts(" found "); dbt_puthex(hits[f]);
            dbt_puts(" times: the whole module stays device-aware\n");
            return 0;
        }
found:
    /* sorted, P1/P2 folded like the translator's test, adjacent ones merged */
    for (f = 0; f < nf; ++f) {
        sf_u32 lo = at[f] & 0x1FFFFFFFu, hi = lo + tab[f].len;
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
