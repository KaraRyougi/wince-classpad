/* test_extrom.c - ClasspadExtRomFind (classpad_extrom.h) against test regions
 * built by tools/mkextrom.py (the real EXT region comes from makeimg): the real thing is accepted, and every field the kernel
 * would follow is rejected when it points outside the region.
 *   test_extrom <EXT.bin>   (a B000FF file built for CLASSPAD_EXT_ROM_PA) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../classpad/SRC/INC/classpad_extrom.h"

static int fails;
#define CHECK(name, cond) do { if (!(cond)) { printf("FAIL %s\n", name); fails++; } } while (0)

static unsigned char *region;           /* CLASSPAD_EXT_ROM_BYTES, erased = 0xFF */
static unsigned char *pristine;

static unsigned rd(unsigned a) { unsigned v; memcpy(&v, region + (a - CLASSPAD_EXT_ROM_PA), 4); return v; }
static void wr(unsigned a, unsigned v) { memcpy(region + (a - CLASSPAD_EXT_ROM_PA), &v, 4); }
static unsigned find(void) { return ClasspadExtRomFind(region, CLASSPAD_EXT_ROM_PA, CLASSPAD_EXT_ROM_BYTES); }
static void reset(void) { memcpy(region, pristine, CLASSPAD_EXT_ROM_BYTES); }

int main(int argc, char **argv)
{
    FILE *f;
    long n;
    unsigned char *bin;
    unsigned pos, start, toc, mods, files, nmods, nfiles, e32, o32;
    if (argc != 2 || !(f = fopen(argv[1], "rb"))) { fprintf(stderr, "usage: test_extrom <EXT.bin>\n"); return 2; }
    fseek(f, 0, SEEK_END); n = ftell(f); rewind(f);
    bin = malloc((size_t)n);
    if (fread(bin, 1, (size_t)n, f) != (size_t)n) return 2;
    fclose(f);
    region = malloc(CLASSPAD_EXT_ROM_BYTES); pristine = malloc(CLASSPAD_EXT_ROM_BYTES);
    memset(region, 0xFF, CLASSPAD_EXT_ROM_BYTES);
    CHECK("erased flash is no region", find() == 0);
    if (memcmp(bin, "B000FF\n", 7)) { printf("FAIL not a B000FF file\n"); return 1; }
    memcpy(&start, bin + 7, 4);
    CHECK("built for CLASSPAD_EXT_ROM_PA", start == CLASSPAD_EXT_ROM_PA);
    for (pos = 15;;) {                  /* records: address, length, checksum, data */
        unsigned a, l, c, s = 0, i;
        memcpy(&a, bin + pos, 4); memcpy(&l, bin + pos + 4, 4); memcpy(&c, bin + pos + 8, 4);
        pos += 12;
        if (a == 0 && c == 0) break;
        for (i = 0; i < l; i++) s += bin[pos + i];
        CHECK("record checksum", s == c);
        memcpy(region + (a - CLASSPAD_EXT_ROM_PA), bin + pos, l);
        pos += l;
    }
    memcpy(pristine, region, CLASSPAD_EXT_ROM_BYTES);

    toc = find();
    CHECK("the built region is accepted", toc != 0);
    if (!toc) return 1;
    nmods = rd(toc + 16);
    nfiles = rd(toc + 48);
    mods = toc + CLASSPAD_EXTROM_ROMHDR_BYTES;
    files = mods + nmods * CLASSPAD_EXTROM_TOC_BYTES;
    e32 = nmods ? rd(mods + 20) : 0;
    o32 = nmods ? rd(mods + 24) : 0;
    CHECK("pTOC at +0x44 is the ROMHDR", rd(CLASSPAD_EXT_ROM_PA + 0x44) == toc);

    reset(); wr(CLASSPAD_EXT_ROM_PA + 0x40, 0x43454344); CHECK("bad signature", find() == 0);
    reset(); wr(CLASSPAD_EXT_ROM_PA + 0x44, toc + 2);    CHECK("unaligned pTOC", find() == 0);
    reset(); wr(CLASSPAD_EXT_ROM_PA + 0x44, CLASSPAD_EXT_ROM_PA + CLASSPAD_EXT_ROM_BYTES); CHECK("pTOC past the window", find() == 0);
    reset(); wr(toc + 8, CLASSPAD_EXT_ROM_PA + 0x1000);  CHECK("physfirst not the base", find() == 0);
    reset(); wr(toc + 12, CLASSPAD_EXT_ROM_PA + CLASSPAD_EXT_ROM_BYTES + 4); CHECK("physlast past the window", find() == 0);
    reset(); wr(toc + 12, toc);                          CHECK("physlast before the files", find() == 0);
    reset(); wr(toc + 16, CLASSPAD_EXTROM_MAX_ENTRIES + 1); CHECK("absurd module count", find() == 0);
    reset(); wr(toc + 16, 0); wr(toc + 48, 0);           CHECK("no modules and no files", find() == 0);
    reset(); wr(toc + 48, CLASSPAD_EXTROM_MAX_ENTRIES + 1); CHECK("absurd file count", find() == 0);
    reset(); wr(files + 16, rd(files + 12) + 1);         CHECK("stored larger than the file", find() == 0);
    if (nmods) {
        reset(); wr(mods + 16, CLASSPAD_EXT_ROM_PA - 1);  CHECK("module name below the base", find() == 0);
        reset(); wr(mods + 20, rd(toc + 12));             CHECK("e32 at physlast", find() == 0);
        reset(); wr(mods + 24, o32 + 2);                  CHECK("unaligned o32", find() == 0);
        reset(); wr(e32, rd(e32) & 0xFFFF0000u);          CHECK("module with no sections", find() == 0);
        reset(); wr(e32, (rd(e32) & 0xFFFF0000u) | 0x4000u); CHECK("module with absurd sections", find() == 0);
        reset(); wr(o32 + 12, rd(toc + 12) - 4);           CHECK("section data past physlast", find() == 0);
        reset(); wr(o32 + 8, 0); wr(o32 + 12, 0);          CHECK("empty section needs no data", find() == toc);
    }
    reset(); wr(files + 20, 0x8C000000);                 CHECK("name outside the region", find() == 0);
    reset(); wr(files + 24, rd(toc + 12) - 2);           CHECK("data running past physlast", find() == 0);
    reset(); wr(files + (nfiles - 1) * CLASSPAD_EXTROM_FILE_BYTES + 24, CLASSPAD_EXT_ROM_PA - 4);
    CHECK("last file's data below the base", find() == 0);
    reset(); CHECK("unchanged copy accepted again", find() == toc);

    printf("extrom: %u modules, %u files, ROMHDR %08x, %d failures\n", nmods, nfiles, toc, fails);
    return fails ? 1 : 0;
}
