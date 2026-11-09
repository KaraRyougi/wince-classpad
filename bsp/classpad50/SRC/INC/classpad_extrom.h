//------------------------------------------------------------------------------
//  classpad_extrom.h - the EXT XIP region in the NOR hole (docs/03)
//
//  The second RAMIMAGE region of config.bib (EXT, CLASSPAD_EXT_ROM_PA), built
//  by makeimg next to NK and flashed on its own (\fls0\EXT.bin, dbt/hhk) into
//  the erased hole inside Casio's OS image, which the OS never writes after
//  an update. OEMInit hands it to the kernel through g_pOemGlobal->pROMChain,
//  so its modules run in place from the NOR and its files appear in \Windows
//  like the NK region's. Casio's boots leave it alone; an OS update erases it,
//  and then CE boots without it.
//
//  ClasspadExtRomFind checks what the kernel will dereference before the
//  region is chained: the signature, the ROMHDR inside the window, every
//  module's name, e32 and o32 records and section data, every file's name and
//  data. Plain C on 32-bit words, shared by the OAL, the extrom guest
//  (dbt/guest/extrom.c) and the host test (bsp/tests/test_extrom.c).
//------------------------------------------------------------------------------
#ifndef CLASSPAD_EXTROM_H
#define CLASSPAD_EXTROM_H

// The hole is 0x00EE0000-0x013E0000 in the chip (5 MiB): config.bib's EXT
// line. Only together with the NK region in the eActivity area (83860000): an
// NK image in the hole window (81000000) would overlap it.
#define CLASSPAD_EXT_ROM_PA             0x80EE0000u
#define CLASSPAD_EXT_ROM_BYTES          0x00500000u

#define CLASSPAD_EXTROM_SIGNATURE       0x43454345u     // ROM_SIGNATURE, "ECEC" in memory
#define CLASSPAD_EXTROM_ROMHDR_BYTES    84u             // sizeof(ROMHDR)
#define CLASSPAD_EXTROM_TOC_BYTES       32u             // sizeof(TOCentry)
#define CLASSPAD_EXTROM_FILE_BYTES      28u             // sizeof(FILESentry)
#define CLASSPAD_EXTROM_O32_BYTES       24u             // sizeof(o32_rom)
#define CLASSPAD_EXTROM_MAX_ENTRIES     4096u
#define CLASSPAD_EXTROM_MAX_OBJS        96u

// Reads a little-endian word of the region at address a (base <= a, a + 4 <= base + size).
#define CLASSPAD_EXTROM_W(mem, base, a) (((const unsigned *)(mem))[((a) - (base)) / 4])

// [p, p + n) inside [base, end)
#define CLASSPAD_EXTROM_IN(p, n, base, end) ((p) >= (base) && (p) <= (end) && (n) <= (end) - (p))

// The ROMHDR address of a valid EXT region claiming addresses base..base+size,
// else 0. mem is where its bytes are: (const void *)base on the device and in
// a guest, a buffer in the host test.
static unsigned ClasspadExtRomFind(const void *mem, unsigned base, unsigned size)
{
    unsigned toc, end, nmods, nfiles, i, j, t;

    if (CLASSPAD_EXTROM_W(mem, base, base + 0x40) != CLASSPAD_EXTROM_SIGNATURE) return 0;
    toc = CLASSPAD_EXTROM_W(mem, base, base + 0x44);
    if ((toc & 3) || toc < base + 0x48 || toc > base + size - CLASSPAD_EXTROM_ROMHDR_BYTES) return 0;
    end = CLASSPAD_EXTROM_W(mem, base, toc + 12);                  // physlast
    nmods = CLASSPAD_EXTROM_W(mem, base, toc + 16);                // nummods
    nfiles = CLASSPAD_EXTROM_W(mem, base, toc + 48);               // numfiles
    if (CLASSPAD_EXTROM_W(mem, base, toc + 8) != base) return 0;   // physfirst
    if (end <= toc || end > base + size) return 0;
    if (nmods > CLASSPAD_EXTROM_MAX_ENTRIES || nfiles > CLASSPAD_EXTROM_MAX_ENTRIES || nmods + nfiles == 0) return 0;
    t = toc + CLASSPAD_EXTROM_ROMHDR_BYTES;
    if (!CLASSPAD_EXTROM_IN(t, nmods * CLASSPAD_EXTROM_TOC_BYTES + nfiles * CLASSPAD_EXTROM_FILE_BYTES, base, end)) return 0;

    // TOCentry: attributes, FILETIME (2 words), size, name, e32, o32, load offset
    for (i = 0; i < nmods; i++, t += CLASSPAD_EXTROM_TOC_BYTES) {
        unsigned name = CLASSPAD_EXTROM_W(mem, base, t + 16);
        unsigned e32 = CLASSPAD_EXTROM_W(mem, base, t + 20), o32 = CLASSPAD_EXTROM_W(mem, base, t + 24);
        unsigned nobj;
        if (!CLASSPAD_EXTROM_IN(name, 1, base, end) || (e32 & 3) || (o32 & 3)) return 0;
        if (!CLASSPAD_EXTROM_IN(e32, 4, base, end)) return 0;
        nobj = CLASSPAD_EXTROM_W(mem, base, e32) & 0xFFFFu;         // e32_rom.e32_objcnt
        if (nobj == 0 || nobj > CLASSPAD_EXTROM_MAX_OBJS) return 0;
        if (!CLASSPAD_EXTROM_IN(o32, nobj * CLASSPAD_EXTROM_O32_BYTES, base, end)) return 0;
        for (j = 0; j < nobj; j++) {
            // o32_rom: vsize, rva, psize, dataptr, realaddr, flags
            unsigned o = o32 + j * CLASSPAD_EXTROM_O32_BYTES;
            unsigned psize = CLASSPAD_EXTROM_W(mem, base, o + 8), data = CLASSPAD_EXTROM_W(mem, base, o + 12);
            if (psize && !CLASSPAD_EXTROM_IN(data, psize, base, end)) return 0;
        }
    }
    // FILESentry: attributes, FILETIME (2 words), real size, stored size, name, data
    for (i = 0; i < nfiles; i++, t += CLASSPAD_EXTROM_FILE_BYTES) {
        unsigned real = CLASSPAD_EXTROM_W(mem, base, t + 12), comp = CLASSPAD_EXTROM_W(mem, base, t + 16);
        unsigned name = CLASSPAD_EXTROM_W(mem, base, t + 20), data = CLASSPAD_EXTROM_W(mem, base, t + 24);
        if (comp > real || !CLASSPAD_EXTROM_IN(name, 1, base, end) || !CLASSPAD_EXTROM_IN(data, comp, base, end)) return 0;
    }
    return toc;
}

#endif
