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
    /* ROMHDR through the ECEC marker (the image is in guest RAM now) */
    {
        const sf_u8 *m = (const sf_u8 *)(out->start + 0x40);
        if (out->start + 0x48 <= ram_hi && out->start >= ram_lo && m[0] == 'E' && m[1] == 'C' && m[2] == 'E' && m[3] == 'C') {
            const sf_u8 *h;
            out->romhdr = le32(m + 4);
            if (out->romhdr >= ram_lo && out->romhdr + 0x60 <= ram_hi) {
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
                    for (i = 0; i < out->nummods && i < 256 && toc + 32 <= ram_hi; i++, toc += 32) {
                        const sf_u8 *te = (const sf_u8 *)toc;
                        sf_u32 name = le32(te + 16), e32 = le32(te + 20), load = le32(te + 28), k;
                        if (name < ram_lo || name + 16 > ram_hi || e32 < ram_lo || e32 + 24 > ram_hi) continue;
                        for (k = 0; k < sizeof kname; k++) if (((const char *)name)[k] != kname[k]) break;
                        if (k == sizeof kname) {
                            /* the code range is the union of the module's code
                             * sections (o32: vsize +0, realaddr +16, flags +20;
                             * IMAGE_SCN_CNT_CODE 0x20): ROMIMAGE places them
                             * anywhere, a Release kernel.dll's text starts below
                             * its load address. Fallback: load .. load+e32_vsize */
                            sf_u32 o32 = le32(te + 24), nobj = le16((const sf_u8 *)e32), j, lo = 0xFFFFFFFF, hi = 0;
                            (void)load;
                            for (j = 0; j < nobj && j < 32 && o32 >= ram_lo && o32 + 24 <= ram_hi; j++, o32 += 24) {
                                const sf_u8 *o = (const sf_u8 *)o32;
                                sf_u32 vsize = le32(o), real = le32(o + 16), flags = le32(o + 20);
                                if (!(flags & 0x20) || !vsize) continue;
                                if (real < lo) lo = real;
                                if (real + vsize > hi) hi = real + vsize;
                            }
                            if (hi) { out->kernel_lo = lo; out->kernel_hi = hi; }
                            else { out->kernel_lo = load; out->kernel_hi = load + le32((const sf_u8 *)e32 + 20); }
                            break;
                        }
                    }
                }
            }
        }
    }
    return NKBIN_OK;
}

void nkbin_print(const nkbin_info *i)
{
    dbt_puts("[nk] image "); dbt_puthex(i->start); dbt_puts(" +"); dbt_puthex(i->length);
    dbt_puts(" entry "); dbt_puthex(i->entry); dbt_puts(" records "); dbt_puthex(i->nrecords);
    dbt_puts(" bytes "); dbt_puthex(i->bytes); dbt_puts("\n");
    if (!i->romhdr) { dbt_puts("[nk] no ROMHDR (ECEC marker missing)\n"); return; }
    dbt_puts("[nk] ROMHDR "); dbt_puthex(i->romhdr); dbt_puts(" cpu "); dbt_puthex(i->cpu_type);
    dbt_puts(" phys "); dbt_puthex(i->physfirst); dbt_puts(".."); dbt_puthex(i->physlast);
    dbt_puts(" mods "); dbt_puthex(i->nummods); dbt_puts("\n[nk] RAM "); dbt_puthex(i->ram_start);
    dbt_puts(" free "); dbt_puthex(i->ram_free); dbt_puts(" end "); dbt_puthex(i->ram_end);
    dbt_puts(" kflags "); dbt_puthex(i->kernel_flags); dbt_puts("\n");
    if (i->kernel_hi) {
        dbt_puts("[nk] kernel.dll "); dbt_puthex(i->kernel_lo); dbt_puts(".."); dbt_puthex(i->kernel_hi);
        dbt_puts(" (device-aware translation)\n");
    }
}
