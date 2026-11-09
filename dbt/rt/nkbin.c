/* nkbin.c - see nkbin.h. Freestanding; reads the source bytes one at a time
 * (the source may be flash, the destination is guest RAM). */
#include "nkbin.h"

static sf_u32 le32(const sf_u8 *p) { return p[0] | (p[1] << 8) | ((sf_u32)p[2] << 16) | ((sf_u32)p[3] << 24); }
static sf_u16 le16(const sf_u8 *p) { return (sf_u16)(p[0] | (p[1] << 8)); }

static const sf_u8 MAGIC[7] = { 'B', '0', '0', '0', 'F', 'F', '\n' };

int nkbin_is_image(const sf_u8 *p)
{
    int i;
    for (i = 0; i < 7; i++) if (p[i] != MAGIC[i]) return 0;
    return 1;
}

/* ROMHDR through the ECEC marker at start + 0x40; the image bytes lie in
 * [lo, hi) (guest RAM after a .bin load, the NOR for an XIP image). */
static void parse_rom(sf_u32 start, sf_u32 lo, sf_u32 hi, nkbin_info *out)
{
    const sf_u8 *m = (const sf_u8 *)(start + 0x40);
    if (start + 0x48 <= hi && start >= lo && m[0] == 'E' && m[1] == 'C' && m[2] == 'E' && m[3] == 'C') {
        const sf_u8 *h;
        out->romhdr = le32(m + 4);
        if (out->romhdr >= lo && out->romhdr + 0x60 <= hi) {
            h = (const sf_u8 *)out->romhdr;
            out->physfirst = le32(h + 8); out->physlast = le32(h + 12); out->nummods = le32(h + 16);
            out->ram_start = le32(h + 20); out->ram_free = le32(h + 24); out->ram_end = le32(h + 28);
            out->kernel_flags = le32(h + 52);
            out->cpu_type = le16(h + 68);
            /* module table right after the ROMHDR (84 bytes): 32-byte TOC
             * entries, name pointer at +16, e32 at +20, load address at +28 */
            {
                sf_u32 i, toc = out->romhdr + 84;
                static const char kname[] = "kernel.dll";
                static const char nkname[] = "nk.exe";
                sf_u32 nk_lo = 0, nk_hi = 0, nk_text = 0, nk_text_len = 0;
                for (i = 0; i < out->nummods && i < 256 && toc + 32 <= hi; i++, toc += 32) {
                    const sf_u8 *te = (const sf_u8 *)toc;
                    sf_u32 name = le32(te + 16), e32 = le32(te + 20), load = le32(te + 28), k, is_nk, is_kdll;
                    if (name < lo || name + 16 > hi || e32 < lo || e32 + 24 > hi) continue;
                    for (k = 0; k < sizeof nkname; k++) if (((const char *)name)[k] != nkname[k]) break;
                    is_nk = (k == sizeof nkname);
                    for (k = 0; k < sizeof kname; k++) if (((const char *)name)[k] != kname[k]) break;
                    is_kdll = (k == sizeof kname);
                    if (is_nk) out->nk_entry = load + le32((const sf_u8 *)e32 + 4);   /* e32_entryrva */
                    if (is_nk || is_kdll) {
                        /* the code range is the union of the module's code
                         * sections (o32: vsize +0, realaddr +16, flags +20;
                         * IMAGE_SCN_CNT_CODE 0x20): ROMIMAGE places them
                         * anywhere, a Release kernel.dll's text starts below
                         * its load address. Fallback: load .. load+e32_vsize */
                        sf_u32 o32 = le32(te + 24), nobj = le16((const sf_u8 *)e32), j, clo = 0xFFFFFFFF, chi = 0, tlo = 0, tlen = 0;
                        for (j = 0; j < nobj && j < 32 && o32 >= lo && o32 + 24 <= hi; j++, o32 += 24) {
                            const sf_u8 *o = (const sf_u8 *)o32;
                            sf_u32 vsize = le32(o), real = le32(o + 16), flags = le32(o + 20);
                            if (!(flags & 0x20) || !vsize) continue;
                            if (vsize > tlen) { tlo = real; tlen = vsize; }      /* the largest code section: .text */
                            if (real < clo) clo = real;
                            if (real + vsize > chi) chi = real + vsize;
                        }
                        if (!chi) { clo = load; chi = load + le32((const sf_u8 *)e32 + 20); }
                        if (is_kdll) { out->kernel_lo = clo; out->kernel_hi = chi; out->kernel_is_nk = 0; out->kernel_text = tlo; out->kernel_text_len = tlen; }
                        else { nk_lo = clo; nk_hi = chi; nk_text = tlo; nk_text_len = tlen; }
                    }
                }
                /* CE 6.0: kernel.dll is the kernel and reaches the CCN registers;
                 * nk.exe is only the OAL. CE 5.0 and earlier: there is no
                 * kernel.dll, nk.exe is kernel + OAL in one module, so the
                 * device-aware range is its code (the OAL inside it must then
                 * access P4 registers unswapped: bsp/classpad50 P4_* macros). */
                if (!out->kernel_hi && nk_hi) { out->kernel_lo = nk_lo; out->kernel_hi = nk_hi; out->kernel_is_nk = 1; out->kernel_text = nk_text; out->kernel_text_len = nk_text_len; }
            }
        }
    }
}

int nkbin_load(const sf_u8 *bin, sf_u32 maxlen, sf_u32 ram_lo, sf_u32 ram_hi, nkbin_info *out)
{
    sf_u32 off = 7 + 8;
    memset(out, 0, sizeof *out);
    if (maxlen < off || !nkbin_is_image(bin)) return NKBIN_ERR_MAGIC;
    out->start = le32(bin + 7);
    out->length = le32(bin + 11);
    for (;;) {
        sf_u32 addr, len, chk, sum = 0, i;
        out->fail_off = off;
        if (off + 12 > maxlen) return NKBIN_ERR_TRUNCATED;
        addr = le32(bin + off); len = le32(bin + off + 4); chk = le32(bin + off + 8);
        off += 12;
        if (addr == 0 && chk == 0) { out->entry = len; break; }      /* terminator: length = entry point */
        if (off + len > maxlen) return NKBIN_ERR_TRUNCATED;
        if (addr < ram_lo || addr + len > ram_hi || addr + len < addr) return NKBIN_ERR_RANGE;
        for (i = 0; i < len; i++) sum += bin[off + i];
        if (sum != chk) return NKBIN_ERR_CHECKSUM;
        memcpy((void *)addr, bin + off, len);
        out->nrecords++; out->bytes += len;
        off += len;
    }
    if (!out->entry) return NKBIN_ERR_NOENTRY;
    parse_rom(out->start, ram_lo, ram_hi, out);
    return NKBIN_OK;
}

int nkbin_is_xip(sf_u32 start, sf_u32 lo, sf_u32 hi)
{
    const sf_u8 *m = (const sf_u8 *)(start + 0x40);
    sf_u32 toc;
    if (start < lo || start + 0x48 > hi) return 0;
    if (m[0] != 'E' || m[1] != 'C' || m[2] != 'E' || m[3] != 'C') return 0;
    toc = le32(m + 4);
    return toc >= lo && toc + 0x60 <= hi;
}

int nkbin_xip(sf_u32 start, sf_u32 lo, sf_u32 hi, nkbin_info *out)
{
    memset(out, 0, sizeof *out);
    out->xip = 1;
    out->start = start;
    parse_rom(start, lo, hi, out);
    if (!out->romhdr) return NKBIN_ERR_MAGIC;
    out->length = out->physlast > out->physfirst ? out->physlast - out->physfirst : 0;
    out->entry = out->nk_entry ? out->nk_entry : out->physfirst;
    return NKBIN_OK;
}

void nkbin_print(const nkbin_info *i)
{
    dbt_puts(i->xip ? "[nk] XIP image " : "[nk] image "); dbt_puthex(i->start); dbt_puts(" +"); dbt_puthex(i->length);
    dbt_puts(" entry "); dbt_puthex(i->entry);
    if (!i->xip) { dbt_puts(" records "); dbt_puthex(i->nrecords); dbt_puts(" bytes "); dbt_puthex(i->bytes); }
    dbt_puts("\n");
    if (!i->romhdr) { dbt_puts("[nk] no ROMHDR (ECEC marker missing)\n"); return; }
    dbt_puts("[nk] ROMHDR "); dbt_puthex(i->romhdr); dbt_puts(" cpu "); dbt_puthex(i->cpu_type);
    dbt_puts(" phys "); dbt_puthex(i->physfirst); dbt_puts(".."); dbt_puthex(i->physlast);
    dbt_puts(" mods "); dbt_puthex(i->nummods); dbt_puts("\n[nk] RAM "); dbt_puthex(i->ram_start);
    dbt_puts(" free "); dbt_puthex(i->ram_free); dbt_puts(" end "); dbt_puthex(i->ram_end);
    dbt_puts(" kflags "); dbt_puthex(i->kernel_flags); dbt_puts("\n");
    if (i->kernel_hi) {
        dbt_puts(i->kernel_is_nk ? "[nk] nk.exe (kernel + OAL, CE 5.0 style) " : "[nk] kernel.dll ");
        dbt_puthex(i->kernel_lo); dbt_puts(".."); dbt_puthex(i->kernel_hi);
        dbt_puts(" (device-aware translation)\n");
    }
}
