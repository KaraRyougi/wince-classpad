/* test_fuguewr.c - classpad_fuguewr.h: writing Casio's storage memory.
 *
 * The flash is an array with NOR semantics: a program may only clear bits
 * (anything else fails the test), an erase sets a sector to 0xFF, and a
 * "power loss" can stop a program half way.
 *   - with a chip dump of an fx-CP400: the CRC, the slice ECCs and the whole
 *     64-byte record the writer builds are compared with every record
 *     Casio's OS wrote; then the workloads below run on a copy of that volume
 *   - always: a volume formatted here (DETAIL record, MBR, tail cluster)
 *   - workload: random cluster rewrites against a shadow copy, the volume
 *     scanned from scratch again and again (as a boot would) and compared;
 *     every logical cluster written once (a full disk still reclaims);
 *     power losses at random points, after which every acknowledged write
 *     must still be there and the writer must carry on
 *   test_fuguewr [nor-dump.bin]   (64 MiB chip image; that part is skipped when absent) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include "../classpad50/SRC/INC/classpad_fuguewr.h"
#include "../classpad50/SRC/INC/classpad_fuguetrim.h"

void (*unused_read)(const CLASSPAD_FUGUE *, unsigned, unsigned, unsigned char *) = ClasspadFugueRead;

static int fails;
#define CHECK(name, cond) do { if (!(cond)) { printf("FAIL %s\n", name); fails++; } } while (0)

#define SECTORS     CLASSPAD_FUGUE_SECTORS
#define CLUSTERS    CLASSPAD_FUGUE_MAX_CLUSTERS(SECTORS)
#define VOL_BYTES   (SECTORS * CLASSPAD_FUGUE_SECTOR_BYTES)
#define TAIL_LC     0x1837u

static unsigned char *vol;                      /* the flash */
static unsigned char *shadow;                   /* what every logical cluster must read as */
static unsigned short loc[CLUSTERS];
static unsigned ver[CLUSTERS];
static unsigned char page[CLASSPAD_FUGUE_PAGE_BYTES];
static CLASSPAD_FUGUE f;
static CLASSPAD_FUGUE_WR w;
static unsigned simErases, badPrograms;
static long crashIn = -1;                       /* bytes until the power goes, -1 = never */
static jmp_buf crashed;

static unsigned rnd_state = 12345;
static unsigned rnd(void) { rnd_state = rnd_state * 1103515245u + 12345u; return rnd_state >> 8; }

static int sim_program(void *ctx, unsigned off, const unsigned char *src, unsigned len)
{
    unsigned i;
    (void)ctx;
    if ((off | len) & 1 || off + len > VOL_BYTES || len > CLASSPAD_FUGUE_PAGE_BYTES) { badPrograms++; return -1; }
    for (i = 0; i < len; i++) {
        if (crashIn == 0) longjmp(crashed, 1);
        if (crashIn > 0) crashIn--;
        if ((vol[off + i] & src[i]) != src[i]) badPrograms++;   /* a bit would have to go from 0 to 1 */
        vol[off + i] &= src[i];
    }
    return 0;
}

static int sim_erase(void *ctx, unsigned sector)
{
    (void)ctx;
    if (sector >= SECTORS) { badPrograms++; return -1; }
    if (crashIn == 0) longjmp(crashed, 1);
    memset(vol + sector * CLASSPAD_FUGUE_SECTOR_BYTES, 0xFF, CLASSPAD_FUGUE_SECTOR_BYTES);
    simErases++;
    return 0;
}

static int mount(void)
{
    ClasspadFugueScan(&f, vol, SECTORS, loc, ver, CLUSTERS);
    if (!ClasspadFugueWrInit(&w, &f, page, 0, sim_program, sim_erase)) return 0;
    ClasspadFugueTrimInit(&w, ver);
    return 1;
}

/* a blank volume with what Casio's format leaves: DETAIL, the MBR, the tail cluster */
static void format(void)
{
    unsigned l, i;
    memset(vol, 0xFF, VOL_BYTES);
    ClasspadFugueScan(&f, vol, SECTORS, loc, ver, CLUSTERS);
    ClasspadFugueWrInit(&w, &f, page, 0, sim_program, sim_erase);
    {
        static unsigned char p[CLASSPAD_FUGUE_PAGE_BYTES];
        memset(p, 0xFF, sizeof p);
        memset(p + CLASSPAD_FUGUE_DETAIL_COUNTS, 0, 3 * SECTORS);
        ClasspadFugueAppend(&w, CLASSPAD_FUGUE_KIND_DETAIL, 0xFFFFFFFE, 0xFFFF, p, 0, &l);
        memset(p, 0xFF, sizeof p);
        memset(p, 0, 512);
        p[446 + 4] = 0x0E;
        p[454] = 29;
        i = 49503; p[458] = (unsigned char)i; p[459] = (unsigned char)(i >> 8);
        p[510] = 0x55; p[511] = 0xAA;
        ClasspadFugueAppend(&w, CLASSPAD_FUGUE_KIND_DATA, 0xFFFFFFFE, 0, p, 0, &l);
        memset(p, 0xFF, sizeof p);
        memset(p + 7 * 512, 0, 512);
        memset(p + 4092, 0xCC, 4);
        ClasspadFugueAppend(&w, CLASSPAD_FUGUE_KIND_DATA, 0xFFFFFFFE, TAIL_LC, p, 0, &l);
    }
}

static void take_shadow(void)
{
    unsigned lc;
    const unsigned char *p;
    for (lc = 0; lc < CLUSTERS; lc++) {
        p = ClasspadFugueCluster(&f, lc);
        if (p) memcpy(shadow + lc * CLASSPAD_FUGUE_PAGE_BYTES, p, CLASSPAD_FUGUE_PAGE_BYTES);
        else memset(shadow + lc * CLASSPAD_FUGUE_PAGE_BYTES, 0xFF, CLASSPAD_FUGUE_PAGE_BYTES);
    }
}

/* clusters that differ from the shadow; *which gets the last one */
static unsigned differing(unsigned *which)
{
    unsigned lc, n = 0;
    const unsigned char *p;
    for (lc = 0; lc < CLUSTERS; lc++) {
        p = ClasspadFugueCluster(&f, lc);
        if (p ? memcmp(p, shadow + lc * CLASSPAD_FUGUE_PAGE_BYTES, CLASSPAD_FUGUE_PAGE_BYTES) != 0
              : !ClasspadFugueBlank(shadow + lc * CLASSPAD_FUGUE_PAGE_BYTES, CLASSPAD_FUGUE_PAGE_BYTES)) {
            n++; if (which) *which = lc;
        }
    }
    return n;
}

/* what Casio's mount needs, as far as it is known: unique FINFO words, one
 * DETAIL record whose counters add up, at most one sector with room */
static void invariants(const char *what, int exactCounts)
{
    static unsigned words[SECTORS];
    unsigned s, t, n = 0, dup = 0, partial = 0, sum = 0, slot;
    const unsigned char *p;
    char name[96];

    for (s = 0; s < SECTORS; s++) {
        if (!ClasspadFugueRecordValid(ClasspadFugueMeta(&f, s, 0), CLASSPAD_FUGUE_KIND_FINFO)) continue;
        p = ClasspadFuguePayload(&f, s, 0);
        words[n] = ClasspadFugueBe32(p);
        if (ClasspadFugueBe32(p + 4) != ~words[n] || memcmp(p, p + 8, 8) || memcmp(p, p + 16, 8)) dup++;
        for (t = 0; t < n; t++) if (words[t] == words[n]) dup++;
        n++;
        if (ClasspadFugueBlank(ClasspadFugueMeta(&f, s, 30), CLASSPAD_FUGUE_META_BYTES)) partial++;
    }
    snprintf(name, sizeof name, "%s: FINFO words unique and well formed", what); CHECK(name, dup == 0);
    snprintf(name, sizeof name, "%s: a DETAIL record is live", what); CHECK(name, w.detail != 0);
    if (w.detail) for (s = 0; s < SECTORS; s++) sum += ClasspadFugueEraseCount(&w, s);
    if (exactCounts) {
        snprintf(name, sizeof name, "%s: erase counters add up to the erases (%u vs %u)", what, sum, simErases); CHECK(name, sum == simErases);
        snprintf(name, sizeof name, "%s: one head at most (%u)", what, partial); CHECK(name, partial <= 1);
    }
    for (s = 0; s < SECTORS; s++)       /* every record the volume holds is complete: nothing half written survives a clean run */
        for (slot = 0; slot < CLASSPAD_FUGUE_RECORDS; slot++) {
            p = ClasspadFugueMeta(&f, s, slot);
            if (ClasspadFugueBlank(p, CLASSPAD_FUGUE_META_BYTES)) continue;
            if (!ClasspadFugueRecordValid(p, p[0]) && exactCounts) { snprintf(name, sizeof name, "%s: incomplete record at %u/%u", what, s, slot); CHECK(name, 0); }
        }
    snprintf(name, sizeof name, "%s: tail cluster alive", what);
    p = ClasspadFugueCluster(&f, TAIL_LC);
    CHECK(name, p && p[4092] == 0xCC && p[4095] == 0xCC);
    snprintf(name, sizeof name, "%s: no program set a bit", what); CHECK(name, badPrograms == 0);
}

static void fill(unsigned char *d, unsigned lc, unsigned gen)
{
    unsigned i, x = lc * 2654435761u + gen * 40503u + 1;
    unsigned kind = rnd() % 8;
    if (kind == 0) { memset(d, 0, CLASSPAD_FUGUE_PAGE_BYTES); return; }
    if (kind == 1) { memset(d, 0xFF, CLASSPAD_FUGUE_PAGE_BYTES); d[rnd() % CLASSPAD_FUGUE_PAGE_BYTES] = (unsigned char)x; return; }
    for (i = 0; i < CLASSPAD_FUGUE_PAGE_BYTES; i++) { x = x * 1664525u + 1013904223u; d[i] = (unsigned char)(x >> 24); }
}

static void workload(const char *what, unsigned writes, unsigned span)
{
    static unsigned char d[CLASSPAD_FUGUE_PAGE_BYTES];
    unsigned i, lc, limit = f.blocks / CLASSPAD_FUGUE_BLOCKS_PER_PAGE, r, errs = 0;
    char name[96];

    if (span > limit) span = limit;
    for (i = 0; i < writes; i++) {
        lc = rnd() % span;
        if (lc == 0) continue;                  /* the MBR stays */
        fill(d, lc, i);
        r = ClasspadFugueWrite(&w, lc, d);
        if (r != CLASSPAD_FUGUE_OK) { errs++; continue; }
        memcpy(shadow + lc * CLASSPAD_FUGUE_PAGE_BYTES, d, CLASSPAD_FUGUE_PAGE_BYTES);
        if (i % 997 == 0) {                     /* a boot in between: everything from the flash alone */
            CHECK("remount stays writable", mount());
        }
    }
    snprintf(name, sizeof name, "%s: %u writes, no errors (%u)", what, writes, errs); CHECK(name, errs == 0);
    snprintf(name, sizeof name, "%s: live map matches the shadow", what); CHECK(name, differing(0) == 0);
    CHECK("remount", mount());
    snprintf(name, sizeof name, "%s: remounted volume matches the shadow", what); CHECK(name, differing(0) == 0);
    invariants(what, 1);
}

/* Trim: clusters the file system gave up. They read as 0xFF at once, their
 * records are not copied by a reclaim, a mount may bring old content back
 * (nobody cares), and a cluster written after its trim reads as written
 * whatever is still in the flash: at once, after a reclaim, after a mount. */
static void trim_test(const char *what)
{
    static unsigned char d[CLASSPAD_FUGUE_PAGE_BYTES];
    static unsigned char state[CLUSTERS];       /* 1 trimmed, 2 trimmed and written again */
    unsigned lc, limit = f.blocks / CLASSPAD_FUGUE_BLOCKS_PER_PAGE, n = 0, bad = 0, before, after, pass, wrong = 0, back = 0;
    const unsigned char *p;
    int status;
    char name[96];

    memset(state, 0, sizeof state);
    before = ClasspadFugueReclaimable(&w);
    for (lc = 1; lc < limit; lc++) {
        if (lc == TAIL_LC || !f.loc[lc] || rnd() % 3) continue;
        if (!ClasspadFugueTrim(&w, lc)) { bad++; continue; }
        if (ClasspadFugueCluster(&f, lc)) bad++;
        state[lc] = 1; n++;
    }
    after = ClasspadFugueReclaimable(&w);
    snprintf(name, sizeof name, "%s: trim drops %u clusters (%u refused or still readable)", what, n, bad); CHECK(name, n > 100 && bad == 0);
    snprintf(name, sizeof name, "%s: trim makes sectors reclaimable (%u -> %u)", what, before, after); CHECK(name, after > before);

    for (pass = 0; pass < 3; pass++) {
        /* pass 0: right after the trim; 1: after an optimise; 2: after a mount */
        if (pass == 1) {
            unsigned got = ClasspadFugueOptimize(&w, &status);
            snprintf(name, sizeof name, "%s: optimise after trim (%u sectors)", what, got); CHECK(name, status == CLASSPAD_FUGUE_OK && got >= after);
        }
        if (pass == 2) CHECK("remount after trim", mount());
        for (lc = 1; lc < limit; lc++) {
            if (state[lc] != 1 || rnd() % 4) continue;
            fill(d, lc, 777 + pass);
            if (ClasspadFugueWrite(&w, lc, d) != CLASSPAD_FUGUE_OK) { wrong++; continue; }
            memcpy(shadow + lc * CLASSPAD_FUGUE_PAGE_BYTES, d, CLASSPAD_FUGUE_PAGE_BYTES);
            state[lc] = 2;
        }
    }
    CHECK("remount at the end of trim", mount());
    for (lc = 0; lc < limit; lc++) {
        p = ClasspadFugueCluster(&f, lc);
        if (state[lc] == 1) { if (p) back++; continue; }       /* trimmed and left: anything goes */
        if (p ? memcmp(p, shadow + lc * CLASSPAD_FUGUE_PAGE_BYTES, CLASSPAD_FUGUE_PAGE_BYTES) != 0
              : !ClasspadFugueBlank(shadow + lc * CLASSPAD_FUGUE_PAGE_BYTES, CLASSPAD_FUGUE_PAGE_BYTES)) wrong++;
    }
    snprintf(name, sizeof name, "%s: everything else, and what was written after its trim, reads right (%u wrong)", what, wrong); CHECK(name, wrong == 0);
    invariants(what, 1);
    printf("test_fuguewr: %s: %u clusters trimmed, %u sectors reclaimable before and %u after, %u erased sectors now, %u old copies back after the mount\n",
           what, n, before, after, w.erased, back);
    take_shadow();                              /* what came back is the content from here on */
}

static void everything(const char *what)
{
    static unsigned char d[CLASSPAD_FUGUE_PAGE_BYTES];
    unsigned lc, limit = f.blocks / CLASSPAD_FUGUE_BLOCKS_PER_PAGE, errs = 0, pass;
    char name[96];

    for (pass = 0; pass < 2; pass++)            /* a full disk, then all of it again */
        for (lc = 1; lc < limit; lc++) {
            unsigned i, x = lc + pass * 77777u;
            for (i = 0; i < CLASSPAD_FUGUE_PAGE_BYTES; i++) { x = x * 1664525u + 1013904223u; d[i] = (unsigned char)(x >> 24); }
            if (ClasspadFugueWrite(&w, lc, d) != CLASSPAD_FUGUE_OK) { errs++; continue; }
            memcpy(shadow + lc * CLASSPAD_FUGUE_PAGE_BYTES, d, CLASSPAD_FUGUE_PAGE_BYTES);
        }
    snprintf(name, sizeof name, "%s: every cluster written twice (%u errors)", what, errs); CHECK(name, errs == 0);
    CHECK("remount", mount());
    snprintf(name, sizeof name, "%s: full volume matches the shadow", what); CHECK(name, differing(0) == 0);
    snprintf(name, sizeof name, "%s: the reserve holds (%u erased)", what, w.erased); CHECK(name, w.erased >= 1);
    invariants(what, 1);
}

static void powerloss(const char *what, unsigned rounds)
{
    static unsigned char d[CLASSPAD_FUGUE_PAGE_BYTES];
    volatile unsigned round, lost = 0, bad = 0, inflight = 0;
    unsigned limit = f.blocks / CLASSPAD_FUGUE_BLOCKS_PER_PAGE, lc, which, n, i;
    char name[96];

    for (round = 0; round < rounds; round++) {
        crashIn = rnd() % 200000;
        if (!setjmp(crashed)) {
            for (i = 0; ; i++) {
                lc = 1 + rnd() % (limit - 1);
                fill(d, lc, round * 1000 + i);
                inflight = lc;
                if (ClasspadFugueWrite(&w, lc, d) != CLASSPAD_FUGUE_OK) { bad++; break; }
                memcpy(shadow + lc * CLASSPAD_FUGUE_PAGE_BYTES, d, CLASSPAD_FUGUE_PAGE_BYTES);
            }
        }
        crashIn = -1;
        lost++;
        if (!mount()) { bad++; break; }
        n = differing(&which);                  /* the write the power cut: old or new, nothing else */
        if (n > 1 || (n == 1 && which != inflight)) bad++;
        if (n == 1) {
            if (!memcmp(ClasspadFugueCluster(&f, which), d, CLASSPAD_FUGUE_PAGE_BYTES))
                memcpy(shadow + which * CLASSPAD_FUGUE_PAGE_BYTES, d, CLASSPAD_FUGUE_PAGE_BYTES);
            else bad++;
        }
    }
    snprintf(name, sizeof name, "%s: %u power losses, acknowledged writes all there (%u bad)", what, (unsigned)lost, (unsigned)bad); CHECK(name, bad == 0);
    invariants(what, 0);
}

static void records_of_casio(const unsigned char *v)
{
    unsigned char m[CLASSPAD_FUGUE_META_BYTES], e[3];
    unsigned s, slot, q, recs = 0, crcBad = 0, eccBad = 0, recBad = 0, nontrivial = 0;
    const unsigned char *r, *p;

    for (s = 0; s < SECTORS; s++)
        for (slot = 0; slot < CLASSPAD_FUGUE_RECORDS; slot++) {
            r = v + s * CLASSPAD_FUGUE_SECTOR_BYTES + CLASSPAD_FUGUE_META_OFF + slot * CLASSPAD_FUGUE_META_BYTES;
            p = v + s * CLASSPAD_FUGUE_SECTOR_BYTES + slot * CLASSPAD_FUGUE_PAGE_BYTES;
            if (r[0] == 0xFF) continue;
            recs++;
            if (ClasspadFugueCrc(p, CLASSPAD_FUGUE_PAGE_BYTES) != ((unsigned)r[8] << 8 | r[9])) crcBad++;
            for (q = 0; q < 8; q++) {
                ClasspadFugueSliceEcc(p + q * 512, e);
                if (memcmp(e, r + 10 + 3 * q, 3)) eccBad++;
                if (e[0] != 0xFF || e[1] != 0xFF || e[2] != 0xFF) nontrivial++;
            }
            ClasspadFugueBuildRecord(m, r[0], ClasspadFugueBe32(r + 2), (unsigned)r[6] << 8 | r[7], p);
            if (memcmp(m, r, CLASSPAD_FUGUE_META_BYTES)) recBad++;
        }
    CHECK("dump: payload CRC of every record", recs > 0 && crcBad == 0);
    CHECK("dump: slice ECC of every record", eccBad == 0);
    CHECK("dump: the writer builds every record byte for byte", recBad == 0);
    printf("test_fuguewr: %u records of Casio's OS reproduced (%u slices with a non-trivial ECC)\n", recs, nontrivial);
}

static void run(const char *what)
{
    char name[96];
    snprintf(name, sizeof name, "%s: writable", what); CHECK(name, mount());
    take_shadow();
    simErases = 0;
    {   /* erases the volume has already had */
        unsigned s; for (s = 0; s < SECTORS; s++) simErases += ClasspadFugueEraseCount(&w, s);
    }
    workload(what, 3000, 64);                   /* a few hot clusters: the FAT and a directory */
    workload(what, 20000, 100000);              /* anywhere */
    everything(what);
    workload(what, 20000, 100000);              /* churn on the full disk */
    {   /* optimise: nothing readable changes, and no sector worth reclaiming is left */
        int status;
        unsigned n = ClasspadFugueOptimize(&w, &status), s, left = 0;
        snprintf(name, sizeof name, "%s: optimise (%u sectors)", what, n); CHECK(name, status == CLASSPAD_FUGUE_OK);
        CHECK("remount after optimise", mount());
        snprintf(name, sizeof name, "%s: optimised volume matches the shadow", what); CHECK(name, differing(0) == 0);
        for (s = 0; s < SECTORS; s++) if (w.used[s] && s != w.head && w.used[s] >= 3u + w.live[s] && w.live[s] <= CLASSPAD_FUGUE_VICTIM_LIVE) left++;
        snprintf(name, sizeof name, "%s: optimise left nothing to reclaim (%u)", what, left); CHECK(name, left == 0);
        invariants(what, 1);
    }
    printf("test_fuguewr: %s: %u records written, %u sectors reclaimed, %u erased sectors left, %u slots stepped over\n",
           what, w.records, w.reclaims, w.erased, w.skipped);
    trim_test(what);
    workload(what, 5000, 100000);
    powerloss(what, 400);
    workload(what, 5000, 100000);               /* and it still works afterwards */
}

/* --- the FAT walk of classpad_fuguetrim.h, on a file system made here --- */
#define DEV_LBAS 49536u
static unsigned char *dev;                      /* DEV_LBAS sectors */
static unsigned char named[DEV_LBAS / 8];
static unsigned namedCount;
static int dev_read(void *ctx, unsigned lba, unsigned char *buf)
{
    (void)ctx;
    if (lba >= DEV_LBAS) return -1;
    memcpy(buf, dev + lba * 512, 512);
    return 0;
}
static void dev_unused(void *ctx, unsigned lc) { (void)ctx; if (lc < DEV_LBAS / 8) { named[lc]++; namedCount++; } }

static void put16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void put32(unsigned char *p, unsigned v) { put16(p, v & 0xFFFF); put16(p + 2, v >> 16); }

/* a FAT16 volume at LBA start with a random half of its clusters in use;
 * mode 1: the second FAT disagrees about a few; returns what must be named */
static unsigned make_fat(unsigned start, unsigned spc, unsigned nfats, int mode, unsigned char *expect)
{
    unsigned reserved = 1, rootEntries = 512, total = DEV_LBAS - start, fatSectors, data, clusters, c, k, lc, lba, n = 0;
    unsigned char *b;
    static unsigned char used[65536];

    memset(dev, 0, (size_t)DEV_LBAS * 512);
    clusters = (total - reserved - 32) / spc;
    fatSectors = (clusters + 2 + 255) / 256;
    data = reserved + nfats * fatSectors + 32;
    clusters = (total - data) / spc;
    dev[510] = 0x55; dev[511] = 0xAA; dev[446 + 4] = 0x06; put32(dev + 454, start); put32(dev + 458, total);
    b = dev + start * 512;
    put16(b + 11, 512); b[13] = (unsigned char)spc; put16(b + 14, reserved); b[16] = (unsigned char)nfats; put16(b + 17, rootEntries);
    if (total < 65536) put16(b + 19, total); else put32(b + 32, total);
    put16(b + 22, fatSectors); b[510] = 0x55; b[511] = 0xAA;
    for (c = 2; c < clusters + 2; c++) {
        used[c] = rnd() % 2;
        for (k = 0; k < nfats; k++) put16(dev + (start + reserved + k * fatSectors) * 512 + c * 2, used[c] ? 0xFFFF : 0);
        if (mode == 1 && nfats == 2 && !used[c] && rnd() % 16 == 0) {      /* free in the first FAT only: in use */
            put16(dev + (start + reserved + fatSectors) * 512 + c * 2, 0x1234);
            used[c] = 1;
        }
    }
    memset(expect, 0, DEV_LBAS / 8);
    for (lc = 0; lc < DEV_LBAS / 8; lc++) {
        for (lba = lc * 8; lba < lc * 8 + 8; lba++) {
            if (lba < start + data || lba >= start + data + clusters * spc) break;
            if (used[(lba - start - data) / spc + 2]) break;
        }
        if (lba == lc * 8 + 8) { expect[lc] = 1; n++; }
    }
    return n;
}

static void fat_test(void)
{
    static const struct { unsigned start, spc, nfats; int mode; } cases[] = {
        { 29, 8, 2, 0 }, { 32, 8, 2, 0 }, { 29, 4, 2, 1 }, { 63, 8, 1, 0 }, { 29, 2, 2, 1 }, { 40, 1, 2, 0 },
    };
    static unsigned char expect[DEV_LBAS / 8];
    CLASSPAD_FUGUE_TRIM t;
    unsigned i, lc, want, got, wrong;
    char name[96];

    dev = malloc((size_t)DEV_LBAS * 512);
    t.ctx = 0; t.read = dev_read; t.unused = dev_unused;
    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        want = make_fat(cases[i].start, cases[i].spc, cases[i].nfats, cases[i].mode, expect);
        memset(named, 0, sizeof named); namedCount = 0;
        got = ClasspadFugueTrimScan(&t, DEV_LBAS);
        for (lc = 0, wrong = 0; lc < DEV_LBAS / 8; lc++) wrong += named[lc] != expect[lc];
        snprintf(name, sizeof name, "FAT walk %u (start %u, %u sectors per cluster, %u FATs): %u unused clusters named, %u expected, %u wrong",
                 i, cases[i].start, cases[i].spc, cases[i].nfats, got, want, wrong);
        CHECK(name, wrong == 0 && got == want && got == namedCount && (want > 100 || cases[i].spc == 1));
    }
    /* what is not this layout names nothing */
    make_fat(29, 8, 2, 0, expect);
    memset(named, 0, sizeof named); namedCount = 0;
    put16(dev + 29 * 512 + 11, 1024);
    CHECK("FAT walk: other sector size", ClasspadFugueTrimScan(&t, DEV_LBAS) == 0);
    put16(dev + 29 * 512 + 11, 512); dev[29 * 512 + 13] = 64;                   /* FAT12: too few clusters */
    CHECK("FAT walk: FAT12", ClasspadFugueTrimScan(&t, DEV_LBAS) == 0);
    dev[29 * 512 + 13] = 8;
    CHECK("FAT walk: device shorter than the volume", ClasspadFugueTrimScan(&t, DEV_LBAS - 8) == 0);
    dev[510] = 0;
    CHECK("FAT walk: no MBR", ClasspadFugueTrimScan(&t, DEV_LBAS) == 0 && namedCount == 0);
    free(dev);
}

/* the same walk on the volume mounted in f */
static int vol_read(void *ctx, unsigned lba, unsigned char *buf) { (void)ctx; ClasspadFugueRead(&f, lba, 1, buf); return 0; }
static unsigned volUnused, volUnusedLive;
static void vol_unused(void *ctx, unsigned lc) { (void)ctx; volUnused++; if (f.loc[lc]) volUnusedLive++; }

int main(int argc, char **argv)
{
    unsigned off = CLASSPAD_FUGUE_PA - 0x80000000u;
    FILE *fp;

    vol = malloc(VOL_BYTES);
    shadow = malloc((size_t)CLUSTERS * CLASSPAD_FUGUE_PAGE_BYTES);

    {   /* known values: an all-zero cluster, and a blank slice */
        static unsigned char z[CLASSPAD_FUGUE_PAGE_BYTES];
        unsigned char e[3];
        CHECK("CRC of a zero cluster", ClasspadFugueCrc(z, sizeof z) == 0xEFDF);
        ClasspadFugueSliceEcc(z, e);
        CHECK("slice ECC of zeros", e[0] == 0xFF && e[1] == 0xFF && e[2] == 0xFF);
    }

    fat_test();

    format();
    run("formatted here");

    fp = argc > 1 ? fopen(argv[1], "rb") : 0;
    if (!fp) printf("test_fuguewr: no chip dump, Casio's volume skipped\n");
    else {
        if (fseek(fp, off, SEEK_SET) || fread(vol, 1, VOL_BYTES, fp) != VOL_BYTES) { printf("FAIL %s is not a 64 MiB chip image\n", argv[1]); fails++; }
        else {
            records_of_casio(vol);
            if (mount()) {      /* Casio's own volume: how much of it the FAT has as free */
                CLASSPAD_FUGUE_TRIM t;
                t.ctx = 0; t.read = vol_read; t.unused = vol_unused;
                ClasspadFugueTrimScan(&t, f.blocks);
                printf("test_fuguewr: Casio's volume: the FAT has %u of %u logical clusters as free, %u of them hold a record (%u live in all)\n",
                       volUnused, f.blocks / CLASSPAD_FUGUE_BLOCKS_PER_PAGE, volUnusedLive, f.live);
                CHECK("Casio's volume: the FAT walk finds the file system", volUnused > 0);
            }
            rnd_state = 999;
            run("Casio's volume");
        }
        fclose(fp);
    }
    if (fails) { printf("test_fuguewr: %d FAILED\n", fails); return 1; }
    printf("test_fuguewr: ok\n");
    return 0;
}
