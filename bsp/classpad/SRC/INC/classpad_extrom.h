//------------------------------------------------------------------------------
//  classpad_extrom.h - the EXT ROM region in the NOR hole (docs/03)
//
//  A files-only CE ROM region (tools/mkextrom.py) at CLASSPAD_EXT_ROM_PA, in
//  the erased hole inside Casio's OS image that the OS never writes after an
//  update. OEMInit hands it to the kernel through g_pOemGlobal->pROMChain, so
//  its files appear in \Windows next to the NK region's. It is flashed on its
//  own (\fls0\EXT.bin, dbt/hhk) and survives Casio's boots, unlike an NK image
//  in the eActivity area; an OS update erases it, and then CE boots without it.
//
//  ClasspadExtRomFind checks everything the kernel will dereference before the
//  region is chained: the signature, the ROMHDR inside the window, no modules
//  (a module's e32/o32 records are not checked here), every file's name and
//  data inside the region. Plain C on 32-bit words, shared by the OAL, the
//  extrom guest (dbt/guest/extrom.c) and the host test (bsp/tests).
//------------------------------------------------------------------------------
#ifndef CLASSPAD_EXTROM_H
#define CLASSPAD_EXTROM_H

// The hole is 0x00EE0000-0x013E0000 in the chip (5 MiB). Only used together
// with an NK image in the eActivity area (NK_WINDOW=eact): an NK image in the
// hole window (0x81000000) overlaps it, and then nothing is found here.
#define CLASSPAD_EXT_ROM_PA             0x80EE0000u
#define CLASSPAD_EXT_ROM_BYTES          0x00500000u

#define CLASSPAD_EXTROM_SIGNATURE       0x43454345u     // ROM_SIGNATURE, "ECEC" in memory
#define CLASSPAD_EXTROM_ROMHDR_BYTES    84u             // sizeof(ROMHDR)
#define CLASSPAD_EXTROM_FILE_BYTES      28u             // sizeof(FILESentry)
#define CLASSPAD_EXTROM_MAX_FILES       4096u

// Reads a little-endian word of the region at address a (base <= a, a + 4 <= base + size).
#define CLASSPAD_EXTROM_W(mem, base, a) (((const unsigned *)(mem))[((a) - (base)) / 4])

// The ROMHDR address of a valid EXT region claiming addresses base..base+size,
// else 0. mem is where its bytes are: (const void *)base on the device and in
// a guest, a buffer in the host test.
static unsigned ClasspadExtRomFind(const void *mem, unsigned base, unsigned size)
{
    unsigned toc, end, n, i, f;

    if (CLASSPAD_EXTROM_W(mem, base, base + 0x40) != CLASSPAD_EXTROM_SIGNATURE) return 0;
    toc = CLASSPAD_EXTROM_W(mem, base, base + 0x44);
    if ((toc & 3) || toc < base + 0x48 || toc > base + size - CLASSPAD_EXTROM_ROMHDR_BYTES) return 0;
    end = CLASSPAD_EXTROM_W(mem, base, toc + 12);                  // physlast
    n = CLASSPAD_EXTROM_W(mem, base, toc + 48);                    // numfiles
    if (CLASSPAD_EXTROM_W(mem, base, toc + 8) != base) return 0;   // physfirst
    if (end <= toc || end > base + size) return 0;
    if (CLASSPAD_EXTROM_W(mem, base, toc + 16) != 0) return 0;     // nummods: files only
    if (n == 0 || n > CLASSPAD_EXTROM_MAX_FILES) return 0;
    if (toc + CLASSPAD_EXTROM_ROMHDR_BYTES + n * CLASSPAD_EXTROM_FILE_BYTES > end) return 0;
    f = toc + CLASSPAD_EXTROM_ROMHDR_BYTES;
    for (i = 0; i < n; i++, f += CLASSPAD_EXTROM_FILE_BYTES) {
        // FILESentry: attributes, FILETIME (2 words), real size, stored size, name, data
        unsigned real = CLASSPAD_EXTROM_W(mem, base, f + 12), comp = CLASSPAD_EXTROM_W(mem, base, f + 16);
        unsigned name = CLASSPAD_EXTROM_W(mem, base, f + 20), data = CLASSPAD_EXTROM_W(mem, base, f + 24);
        if (real != comp) return 0;                 // stored uncompressed (mkextrom.py)
        if (name < base || name >= end || data < base || data > end || comp > end - data) return 0;
    }
    return toc;
}

#endif
