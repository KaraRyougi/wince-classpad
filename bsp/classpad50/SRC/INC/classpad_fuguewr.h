//------------------------------------------------------------------------------
//  classpad_fuguewr.h - writing Casio's storage memory (\fls0)
//
//  The writer on top of classpad_fugue.h's reader: it appends records the way
//  Casio's OS does, so that the OS finds a volume it could have written
//  itself. Format facts (checked against every record of an fx-CP400 chip
//  dump, bsp/tests/test_fugue.c; the write and reclaim rules come from
//  fx-CG100 volumes written by Casio's OS, ~/Dev/cg100-flash-gc):
//
//    - record bytes +8..+9: CRC-16/CCITT-FALSE of the 4 KiB payload, big-endian;
//      +10..+33: eight 3-byte ECCs, one per 512-byte slice of the payload;
//      then "ZZZ" and the record's own ECC (which covers the CRC and the
//      slice ECCs).
//    - a rewrite of a logical cluster is a new record with the live version
//      minus one (first record: 0xFFFFFFFE) in the next free slot of the one
//      "head" sector. The old record is never touched.
//    - a sector is opened by a FINFO record in slot 0 (version 0xFFFFFFFF,
//      cluster 0xFFFF, payload: a word and its complement three times; the
//      word is one below the lowest on the volume).
//    - reclaim: the live DATA records of the victim sector are copied to the
//      head (version minus one), then a DETAIL record (kind 0x88, its own
//      version chain; payload bytes 0..1 = the victim, +0x200 + 3 * s = the
//      24-bit erase count of sector s) and only then the erase. An erased
//      sector carries no header.
//    - the DETAIL record, logical clusters 0 and 3 and the tail cluster
//      (0x1837 on the fx-CP400, outside the partition) must stay alive: they
//      are ordinary live records here and move with reclaim like any other.
//
//  Order inside one record: payload, record bytes 2..63, record bytes 0..1.
//  A power loss before the last step leaves a slot the reader skips (kind
//  0xFF) and the writer steps over (not blank), so a write is either
//  complete or absent; the previous copy of the cluster stays live.
//
//  The flash is written through two callbacks, so the same code runs in the
//  CE driver (translator hypercalls), in a host test (an array with NOR
//  semantics) and anywhere else. Reads go through CLASSPAD_FUGUE.mem.
//  Byte accesses only, like the reader.
//
//  Not known about Casio's fx-CP400 OS (the dump has never seen an erase):
//  whether it checks the payload CRC and slice ECCs, how it treats a
//  half-written slot, and whether its reclaim matches the fx-CG100's.
//------------------------------------------------------------------------------
#ifndef CLASSPAD_FUGUEWR_H
#define CLASSPAD_FUGUEWR_H

#include "classpad_fugue.h"

#define CLASSPAD_FUGUE_KIND_DETAIL      0x88u
#define CLASSPAD_FUGUE_RECORDS          31u             // records 0..30 of a sector
#define CLASSPAD_FUGUE_DETAIL_COUNTS    0x200u          // erase counters in a DETAIL payload
#define CLASSPAD_FUGUE_NO_SECTOR        0xFFFFu
// Erased sectors: below GC_LOW a write reclaims one sector, below RESERVE it
// reclaims until the reserve is back. A user write never opens the last
// erased sector: reclaim needs it for its copies.
#define CLASSPAD_FUGUE_RESERVE          2u
#define CLASSPAD_FUGUE_GC_LOW           8u
// A victim must free more slots than its copies and the DETAIL record take
#define CLASSPAD_FUGUE_VICTIM_LIVE      28u
#define CLASSPAD_FUGUE_MAX_SECTORS      CLASSPAD_FUGUE_SECTORS

// ClasspadFugueWrite results
#define CLASSPAD_FUGUE_OK               0
#define CLASSPAD_FUGUE_ERR_READONLY     1       // no DETAIL record or no volume: the writer never starts
#define CLASSPAD_FUGUE_ERR_FULL         2       // no erased sector left to open and nothing to reclaim
#define CLASSPAD_FUGUE_ERR_FLASH        3       // program or erase failed
#define CLASSPAD_FUGUE_ERR_RANGE        4

typedef struct {
    CLASSPAD_FUGUE *f;                  // the reader's map, kept current by every write
    void *ctx;
    // off: byte offset in the volume (even), len even and at most 4 KiB, src in RAM. 0 = programmed and verified.
    int (*program)(void *ctx, unsigned off, const unsigned char *src, unsigned len);
    // sector: physical sector of the volume. 0 = erased and verified blank.
    int (*erase)(void *ctx, unsigned sector);
    unsigned char *page;                // 4 KiB of RAM: reclaim copies, FINFO and DETAIL payloads
    unsigned char used[CLASSPAD_FUGUE_MAX_SECTORS];    // records in the sector, 0 = erased (no header)
    unsigned char live[CLASSPAD_FUGUE_MAX_SECTORS];    // live DATA records plus the live DETAIL
    unsigned char clean[CLASSPAD_FUGUE_MAX_SECTORS];   // erased and verified by this writer since the scan: known blank
    unsigned char headClean;            // current head was freshly verified blank on open: skip per-slot blank scan
    unsigned head;                      // the sector being appended to, CLASSPAD_FUGUE_NO_SECTOR = none
    unsigned erased;                    // erased sectors
    unsigned minWord;                   // lowest FINFO word on the volume
    unsigned detail;                    // the live DETAIL record: 1 + sector * 32 + slot, 0 = none
    unsigned detailVer;
    unsigned writable;
    unsigned records, reclaims, erases, skipped;       // statistics: records written, sectors reclaimed, erases, slots stepped over
    // Trim (ClasspadFugueTrimInit): per logical cluster the lowest version
    // number any record of it in the flash can have, 0xFFFFFFFF = none. 0: no trim.
    unsigned *ver;
    unsigned trimmed;                   // clusters dropped by ClasspadFugueTrim
} CLASSPAD_FUGUE_WR;

static unsigned ClasspadFugueBe32(const unsigned char *p)
{
    return (unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3];
}

// CRC-16/CCITT-FALSE (0x1021, initial 0xFFFF) of n bytes
// Per byte value: the CRC step, the column parity it contributes to an ECC,
// and whether it has an odd number of bits. Filled at the first use (the
// same values whoever fills them, so two threads doing it at once is fine).
// A record's CRC and slice ECCs cover 4 KiB each: bit by bit that was most
// of the time an optimise took under the translator.
static unsigned short g_fugueCrcTab[256];
static unsigned char g_fugueTabsReady;

static void ClasspadFugueTables(void)
{
    unsigned b, j, crc;

    if (g_fugueTabsReady) return;
    ClasspadFugueEccTables();
    for (b = 0; b < 256; b++) {
        crc = b << 8;
        for (j = 0; j < 8; j++) crc = (crc & 0x8000) ? ((crc << 1) ^ 0x1021) & 0xFFFF : (crc << 1) & 0xFFFF;
        g_fugueCrcTab[b] = (unsigned short)crc;
    }
    g_fugueTabsReady = 1;
}

static unsigned ClasspadFugueCrc(const unsigned char *p, unsigned n)
{
    unsigned crc = 0xFFFF, i;

    ClasspadFugueTables();
    for (i = 0; i < n; i++) crc = ((crc << 8) ^ g_fugueCrcTab[(crc >> 8) ^ p[i]]) & 0xFFFF;
    return crc;
}

// The 3 ECC bytes of one 512-byte slice of a payload into e[]: the record
// ECC's construction with a 9-bit byte index, the two ninth bits below the
// column parity in byte 0, the high nibble byte before the low one.
static void ClasspadFugueSliceEcc(const unsigned char *p, unsigned char *e)
{
    unsigned r1 = 0x1FF, r4 = 0xFF, r7 = 0x1FF, i, j, b, s1, s7, k;

    ClasspadFugueTables();
    for (i = 0; i < 512; i++) {
        b = p[i];
        r4 ^= g_fugueColTab[b];
        if (g_fugueOddTab[b]) { r1 ^= i; r7 ^= ~i & 0x1FF; }
    }
    e[0] = (unsigned char)(((r4 & 0x3F) << 2) | ((r1 >> 8 & 1) << 1) | (r7 >> 8 & 1));
    for (k = 0; k < 2; k++) {           // k = 0: bits 4..7 of the index, k = 1: bits 0..3
        s7 = 0; s1 = 0;
        for (j = 0; j < 4; j++) {
            if (r7 & (1u << (4 * (1 - k) + j))) s7 |= 1u << (2 * j);
            if (r1 & (1u << (4 * (1 - k) + j))) s1 |= 2u << (2 * j);
        }
        e[1 + k] = (unsigned char)((0xAA | s7) & (0x55 | s1));
    }
}

// The complete 64-byte record for a 4 KiB payload into m[]
static void ClasspadFugueBuildRecord(unsigned char *m, unsigned kind, unsigned ver, unsigned lc, const unsigned char *payload)
{
    unsigned i, crc;

    for (i = 0; i < CLASSPAD_FUGUE_META_BYTES; i++) m[i] = 0xFF;
    m[0] = (unsigned char)kind;
    m[1] = 0x0F;
    m[2] = (unsigned char)(ver >> 24); m[3] = (unsigned char)(ver >> 16);
    m[4] = (unsigned char)(ver >> 8);  m[5] = (unsigned char)ver;
    m[6] = (unsigned char)(lc >> 8);   m[7] = (unsigned char)lc;
    crc = ClasspadFugueCrc(payload, CLASSPAD_FUGUE_PAGE_BYTES);
    m[8] = (unsigned char)(crc >> 8);  m[9] = (unsigned char)crc;
    for (i = 0; i < 8; i++) ClasspadFugueSliceEcc(payload + i * 512, m + 10 + 3 * i);
    m[34] = 'Z'; m[35] = 'Z'; m[36] = 'Z';
    ClasspadFugueEcc(m, m + 37);
}

static int ClasspadFugueBlank(const unsigned char *p, unsigned n)
{
    const CLASSPAD_FUGUE_WORD *q;

    for (; n && ((unsigned long)p & 3); n--, p++) if (*p != 0xFF) return 0;
    for (q = (const CLASSPAD_FUGUE_WORD *)p; n >= 4; n -= 4, q++) if (*q != 0xFFFFFFFFu) return 0;
    for (p = (const unsigned char *)q; n; n--, p++) if (*p != 0xFF) return 0;
    return 1;
}

static int ClasspadFugueEqual(const unsigned char *a, const unsigned char *b, unsigned n)
{
    if ((((unsigned long)a | (unsigned long)b) & 3) == 0) {
        const CLASSPAD_FUGUE_WORD *aw = (const CLASSPAD_FUGUE_WORD *)a;
        const CLASSPAD_FUGUE_WORD *bw = (const CLASSPAD_FUGUE_WORD *)b;
        for (; n >= 4; n -= 4) if (*aw++ != *bw++) return 0;
        a = (const unsigned char *)aw; b = (const unsigned char *)bw;
    }
    for (; n; n--) if (*a++ != *b++) return 0;
    return 1;
}

static const unsigned char *ClasspadFugueMeta(const CLASSPAD_FUGUE *f, unsigned s, unsigned slot)
{
    return f->mem + s * CLASSPAD_FUGUE_SECTOR_BYTES + CLASSPAD_FUGUE_META_OFF + slot * CLASSPAD_FUGUE_META_BYTES;
}

static const unsigned char *ClasspadFuguePayload(const CLASSPAD_FUGUE *f, unsigned s, unsigned slot)
{
    return f->mem + s * CLASSPAD_FUGUE_SECTOR_BYTES + slot * CLASSPAD_FUGUE_PAGE_BYTES;
}

// The erase count of sector s in the live DETAIL record (0 without one)
static unsigned ClasspadFugueEraseCount(const CLASSPAD_FUGUE_WR *w, unsigned s)
{
    const unsigned char *p;

    if (!w->detail) return 0;
    p = ClasspadFuguePayload(w->f, (w->detail - 1) >> 5, (w->detail - 1) & 31) + CLASSPAD_FUGUE_DETAIL_COUNTS + 3 * s;
    return (unsigned)p[0] << 16 | (unsigned)p[1] << 8 | p[2];
}

// Prepares the writer for a volume ClasspadFugueScan has mapped into f.
// page is 4 KiB of RAM the writer keeps. Returns 1 when the volume can be
// written: it has a block device and a DETAIL record. Otherwise every write
// fails with CLASSPAD_FUGUE_ERR_READONLY and the flash is left alone.
static int ClasspadFugueWrInit(CLASSPAD_FUGUE_WR *w, CLASSPAD_FUGUE *f, unsigned char *page, void *ctx,
                               int (*program)(void *, unsigned, const unsigned char *, unsigned),
                               int (*erase)(void *, unsigned))
{
    const unsigned char *m, *p;
    unsigned s, slot, n, word, v, headWord = 0, i;

    w->f = f; w->ctx = ctx; w->program = program; w->erase = erase; w->page = page;
    w->head = CLASSPAD_FUGUE_NO_SECTOR;
    w->headClean = 0;
    w->erased = 0; w->minWord = 0xFFFFFFFF; w->detail = 0; w->detailVer = 0;
    w->records = w->reclaims = w->erases = w->skipped = 0;
    w->writable = 0;
    w->ver = 0; w->trimmed = 0;
    if (f->sectors > CLASSPAD_FUGUE_MAX_SECTORS) return 0;

    for (s = 0; s < f->sectors; s++) {
        w->live[s] = 0;
        w->clean[s] = 0;
        n = 0;
        for (slot = 0; slot < CLASSPAD_FUGUE_RECORDS; slot++) {
            m = ClasspadFugueMeta(f, s, slot);
            if (!ClasspadFugueBlank(m, CLASSPAD_FUGUE_META_BYTES)) n = slot + 1;
            if (slot && ClasspadFugueRecordValid(m, CLASSPAD_FUGUE_KIND_DETAIL)) {
                v = ClasspadFugueBe32(m + 2);
                if (!w->detail || v < w->detailVer) { w->detail = 1 + s * 32 + slot; w->detailVer = v; }
            }
        }
        p = ClasspadFuguePayload(f, s, 0);
        if (!n && !ClasspadFugueBlank(p, 24)) n = 1;    // a header payload without its record: not erased
        w->used[s] = (unsigned char)n;
        if (!n) { w->erased++; continue; }
        if (!ClasspadFugueRecordValid(ClasspadFugueMeta(f, s, 0), CLASSPAD_FUGUE_KIND_FINFO)) continue;
        word = ClasspadFugueBe32(p);
        if (ClasspadFugueBe32(p + 4) != (~word & 0xFFFFFFFF)) continue;
        if (word < w->minWord) w->minWord = word;
        // the head: the most recently opened sector that still has room
        if (n < CLASSPAD_FUGUE_RECORDS && (w->head == CLASSPAD_FUGUE_NO_SECTOR || word < headWord)) {
            w->head = s; headWord = word;
        }
    }
    for (i = 0; i < f->clusters; i++) {
        if (f->loc[i]) w->live[(f->loc[i] - 1) >> 5]++;
    }
    if (w->detail) w->live[(w->detail - 1) >> 5]++;
    w->writable = f->blocks != 0 && w->detail != 0;
    return (int)w->writable;
}

// Programs n bytes (at most 4 KiB) at off, leaving out 64-byte lines that are all 0xFF
static int ClasspadFugueProgramSparse(CLASSPAD_FUGUE_WR *w, unsigned off, const unsigned char *src, unsigned n)
{
    unsigned i, len, run = 0;

    for (i = 0; i <= n; i += len) {
        len = n - i < 64 ? n - i : 64;
        if (i < n && !ClasspadFugueBlank(src + i, len)) { run += len; continue; }
        // a blank line or the end: what has gathered goes out in one call
        // (each call is a trap into the translator in the CE driver)
        if (run && w->program(w->ctx, off + i - run, src + i - run, run)) return -1;
        run = 0;
        if (i >= n) break;
    }
    return 0;
}

// Opens a new head: the erased sector with the lowest erase count. keep =
// erased sectors that must remain afterwards.
static int ClasspadFugueOpenHead(CLASSPAD_FUGUE_WR *w, unsigned keep)
{
    unsigned char m[CLASSPAD_FUGUE_META_BYTES];
    unsigned char p[CLASSPAD_FUGUE_PAGE_BYTES];         // not w->page: the caller's payload may be in it
    unsigned s, best = CLASSPAD_FUGUE_NO_SECTOR, bestCount = 0, c, word, i;

    if (w->erased <= keep) return CLASSPAD_FUGUE_ERR_FULL;
    for (s = 0; s < w->f->sectors; s++) {
        if (w->used[s]) continue;
        c = ClasspadFugueEraseCount(w, s);
        if (best == CLASSPAD_FUGUE_NO_SECTOR || c < bestCount) { best = s; bestCount = c; }
    }
    if (best == CLASSPAD_FUGUE_NO_SECTOR) return CLASSPAD_FUGUE_ERR_FULL;
    // payload bytes without records (an interrupted write before the last
    // erase): the sector is erased again rather than trusted
    if (!w->clean[best] && !ClasspadFugueBlank(w->f->mem + best * CLASSPAD_FUGUE_SECTOR_BYTES, CLASSPAD_FUGUE_SECTOR_BYTES)) {
        if (w->erase(w->ctx, best)) return CLASSPAD_FUGUE_ERR_FLASH;
        w->erases++;
    }
    w->clean[best] = 0;
    word = w->minWord - 1;
    for (i = 0; i < CLASSPAD_FUGUE_PAGE_BYTES; i++) p[i] = 0xFF;
    for (i = 0; i < 3; i++) {
        p[8 * i + 0] = (unsigned char)(word >> 24);  p[8 * i + 1] = (unsigned char)(word >> 16);
        p[8 * i + 2] = (unsigned char)(word >> 8);   p[8 * i + 3] = (unsigned char)word;
        p[8 * i + 4] = (unsigned char)(~word >> 24); p[8 * i + 5] = (unsigned char)(~word >> 16);
        p[8 * i + 6] = (unsigned char)(~word >> 8);  p[8 * i + 7] = (unsigned char)~word;
    }
    ClasspadFugueBuildRecord(m, CLASSPAD_FUGUE_KIND_FINFO, 0xFFFFFFFF, 0xFFFF, p);
    w->used[best] = 1;                  // whatever happens next, the sector is no longer erased
    w->erased--;
    if (ClasspadFugueProgramSparse(w, best * CLASSPAD_FUGUE_SECTOR_BYTES, p, CLASSPAD_FUGUE_PAGE_BYTES) ||
        w->program(w->ctx, best * CLASSPAD_FUGUE_SECTOR_BYTES + CLASSPAD_FUGUE_META_OFF + 2, m + 2, CLASSPAD_FUGUE_META_BYTES - 2) ||
        w->program(w->ctx, best * CLASSPAD_FUGUE_SECTOR_BYTES + CLASSPAD_FUGUE_META_OFF, m, 2)) {
        return CLASSPAD_FUGUE_ERR_FLASH;
    }
    w->minWord = word;
    w->head = best;
    w->headClean = 1;                   // remaining slots 1..30 of this freshly opened sector are known blank
    w->records++;
    return CLASSPAD_FUGUE_OK;
}

// Appends one record to the head. payload is 4 KiB of RAM. *loc gets the
// record's place (1 + sector * 32 + slot).
static int ClasspadFugueAppend(CLASSPAD_FUGUE_WR *w, unsigned kind, unsigned ver, unsigned lc,
                               const unsigned char *payload, unsigned keep, unsigned *loc)
{
    unsigned char m[CLASSPAD_FUGUE_META_BYTES];
    unsigned s, slot, base, tries = 0;
    int r;

    for (;;) {
        if (w->head == CLASSPAD_FUGUE_NO_SECTOR || w->used[w->head] >= CLASSPAD_FUGUE_RECORDS) {
            w->head = CLASSPAD_FUGUE_NO_SECTOR;
            w->headClean = 0;
            r = ClasspadFugueOpenHead(w, keep);
            if (r != CLASSPAD_FUGUE_OK) return r;
        }
        s = w->head;
        slot = w->used[s];
        base = s * CLASSPAD_FUGUE_SECTOR_BYTES;
        w->used[s]++;                   // taken, whether the record gets there or not
        if (!w->headClean &&
            (!ClasspadFugueBlank(ClasspadFugueMeta(w->f, s, slot), CLASSPAD_FUGUE_META_BYTES) ||
             !ClasspadFugueBlank(ClasspadFuguePayload(w->f, s, slot), CLASSPAD_FUGUE_PAGE_BYTES))) {
            w->skipped++;               // left by an interrupted write
            continue;
        }
        ClasspadFugueBuildRecord(m, kind, ver, lc, payload);
        if (ClasspadFugueProgramSparse(w, base + slot * CLASSPAD_FUGUE_PAGE_BYTES, payload, CLASSPAD_FUGUE_PAGE_BYTES) ||
            w->program(w->ctx, base + CLASSPAD_FUGUE_META_OFF + slot * CLASSPAD_FUGUE_META_BYTES + 2, m + 2, CLASSPAD_FUGUE_META_BYTES - 2) ||
            w->program(w->ctx, base + CLASSPAD_FUGUE_META_OFF + slot * CLASSPAD_FUGUE_META_BYTES, m, 2)) {
            w->headClean = 0;
            w->skipped++;
            if (++tries >= 3) return CLASSPAD_FUGUE_ERR_FLASH;
            continue;                   // the slot is spent; the next one
        }
        w->records++;
        *loc = 1 + s * 32 + slot;
        return CLASSPAD_FUGUE_OK;
    }
}

// Reclaims the sector victim (not the head, at most
// CLASSPAD_FUGUE_VICTIM_LIVE live records): its live records move to the
// head, a DETAIL record announces the erase, the sector is erased.
static int ClasspadFugueReclaimSector(CLASSPAD_FUGUE_WR *w, unsigned victim)
{
    const unsigned char *m, *src;
    unsigned slot, lc, v, loc, c;
    int r;

    if (victim >= w->f->sectors || victim == w->head || !w->used[victim] ||
        w->live[victim] > CLASSPAD_FUGUE_VICTIM_LIVE) return CLASSPAD_FUGUE_ERR_FULL;

    for (slot = 1; slot < CLASSPAD_FUGUE_RECORDS; slot++) {
        m = ClasspadFugueMeta(w->f, victim, slot);
        if (!ClasspadFugueRecordValid(m, CLASSPAD_FUGUE_KIND_DATA)) continue;
        lc = (unsigned)m[6] << 8 | m[7];
        if (lc >= w->f->clusters || w->f->loc[lc] != 1 + victim * 32 + slot) continue;     // superseded
        v = ClasspadFugueBe32(m + 2);
        if (!v) return CLASSPAD_FUGUE_ERR_FLASH;        // version exhausted: unreachable in practice
        src = ClasspadFuguePayload(w->f, victim, slot);
        ClasspadFugueCopy(w->page, src, CLASSPAD_FUGUE_PAGE_BYTES);             // the flash cannot be read while it programs
        r = ClasspadFugueAppend(w, CLASSPAD_FUGUE_KIND_DATA, v - 1, lc, w->page, 0, &loc);
        if (r != CLASSPAD_FUGUE_OK) return r;
        w->f->loc[lc] = (unsigned short)loc;
        if (w->ver) w->ver[lc] = v - 1;
        w->live[victim]--;
        w->live[(loc - 1) >> 5]++;
    }

    // the DETAIL record that announces the erase, with the victim's count
    // one up; it also moves the record out of the victim if it was there
    src = ClasspadFuguePayload(w->f, (w->detail - 1) >> 5, (w->detail - 1) & 31);
    ClasspadFugueCopy(w->page, src, CLASSPAD_FUGUE_PAGE_BYTES);
    w->page[0] = (unsigned char)(victim >> 8);
    w->page[1] = (unsigned char)victim;
    c = ClasspadFugueEraseCount(w, victim);
    if (c < 0xFFFFFE) c++;
    w->page[CLASSPAD_FUGUE_DETAIL_COUNTS + 3 * victim + 0] = (unsigned char)(c >> 16);
    w->page[CLASSPAD_FUGUE_DETAIL_COUNTS + 3 * victim + 1] = (unsigned char)(c >> 8);
    w->page[CLASSPAD_FUGUE_DETAIL_COUNTS + 3 * victim + 2] = (unsigned char)c;
    if (!w->detailVer) return CLASSPAD_FUGUE_ERR_FLASH;
    r = ClasspadFugueAppend(w, CLASSPAD_FUGUE_KIND_DETAIL, w->detailVer - 1, 0xFFFF, w->page, 0, &loc);
    if (r != CLASSPAD_FUGUE_OK) return r;
    w->live[(w->detail - 1) >> 5]--;
    w->live[(loc - 1) >> 5]++;
    w->detail = loc;
    w->detailVer--;

    if (w->erase(w->ctx, victim)) return CLASSPAD_FUGUE_ERR_FLASH;
    w->erases++;
    w->reclaims++;
    w->used[victim] = 0;
    w->live[victim] = 0;
    w->clean[victim] = 1;
    w->erased++;
    return CLASSPAD_FUGUE_OK;
}

// Reclaims one sector, the one with the fewest live records. Returns
// CLASSPAD_FUGUE_OK, CLASSPAD_FUGUE_ERR_FULL when no sector would give
// anything back, or a flash error.
static int ClasspadFugueReclaim(CLASSPAD_FUGUE_WR *w)
{
    unsigned s, victim = CLASSPAD_FUGUE_NO_SECTOR;

    for (s = 0; s < w->f->sectors; s++) {
        if (!w->used[s] || s == w->head) continue;
        if (victim == CLASSPAD_FUGUE_NO_SECTOR || w->live[s] < w->live[victim]) victim = s;
    }
    if (victim == CLASSPAD_FUGUE_NO_SECTOR) return CLASSPAD_FUGUE_ERR_FULL;
    return ClasspadFugueReclaimSector(w, victim);
}

// Sectors an optimise would reclaim now: not the head, at least two dead
// records behind the header, few enough live ones to gain from it
static unsigned ClasspadFugueReclaimable(const CLASSPAD_FUGUE_WR *w)
{
    unsigned s, n = 0;

    if (!w->writable) return 0;
    for (s = 0; s < w->f->sectors; s++) {
        if (w->used[s] && s != w->head && w->used[s] >= 3u + w->live[s] && w->live[s] <= CLASSPAD_FUGUE_VICTIM_LIVE) n++;
    }
    return n;
}

// One step of the optimise: the reclaimable sector with the fewest live
// records. Returns 1 when a sector was reclaimed, 0 when there was none (or
// on an error: *status is CLASSPAD_FUGUE_OK or what stopped it). A caller
// that shows progress calls this in a loop.
static unsigned ClasspadFugueOptimizeStep(CLASSPAD_FUGUE_WR *w, int *status)
{
    unsigned s, best = CLASSPAD_FUGUE_NO_SECTOR;
    int r;

    *status = CLASSPAD_FUGUE_OK;
    if (!w->writable) { *status = CLASSPAD_FUGUE_ERR_READONLY; return 0; }
    for (s = 0; s < w->f->sectors; s++) {
        if (!w->used[s] || s == w->head) continue;
        if (w->used[s] < 3u + w->live[s] || w->live[s] > CLASSPAD_FUGUE_VICTIM_LIVE) continue;
        if (best == CLASSPAD_FUGUE_NO_SECTOR || w->live[s] < w->live[best]) best = s;
    }
    if (best == CLASSPAD_FUGUE_NO_SECTOR) return 0;
    r = ClasspadFugueReclaimSector(w, best);
    if (r == CLASSPAD_FUGUE_OK) return 1;
    if (r != CLASSPAD_FUGUE_ERR_FULL) *status = r;
    return 0;
}

// Optimise: reclaims every sector that holds superseded records, fewest live
// records first, until none would give anything back, so that the volume has
// as many erased sectors as its content allows (what "Optimize" in Casio's
// storage menu is for; the writer otherwise reclaims only when erased sectors
// run low). A sector is worth it when it has at least two dead records: its
// reclaim costs one slot, the DETAIL record, and leaves the previous DETAIL
// record dead behind it. Returns the number of sectors reclaimed; *status is
// CLASSPAD_FUGUE_OK or the error that stopped it.
static unsigned ClasspadFugueOptimize(CLASSPAD_FUGUE_WR *w, int *status)
{
    unsigned n = 0;

    while (ClasspadFugueOptimizeStep(w, status)) n++;
    return n;
}

// Trim. ver[] is the array ClasspadFugueScan filled for this volume (one
// entry per logical cluster); the writer keeps it from here on.
static void ClasspadFugueTrimInit(CLASSPAD_FUGUE_WR *w, unsigned *ver)
{
    unsigned i;

    for (i = 0; i < w->f->clusters; i++) if (!w->f->loc[i]) ver[i] = 0xFFFFFFFF;
    w->ver = ver;
}

// Drops logical cluster lc: the file system says it holds nothing (a deleted
// file's cluster, classpad_fuguetrim.h). It reads as 0xFF from now on, its
// record is one more dead record in its sector and goes when that sector is
// reclaimed, without being copied. Nothing is written for this. Until then
// (and for older copies in other sectors, longer) a mount finds the record
// again and the cluster comes back with old content, which is as good as
// 0xFF for a cluster nobody uses; a later write of the cluster takes a
// version below everything that may still be there (w->ver), so it wins
// whatever the next mount finds. Returns 1 when a record was dropped.
static unsigned ClasspadFugueTrim(CLASSPAD_FUGUE_WR *w, unsigned lc)
{
    unsigned loc;

    if (!w->writable || !w->ver || lc >= w->f->clusters || !(loc = w->f->loc[lc])) return 0;
    w->ver[lc] = ClasspadFugueBe32(ClasspadFugueMeta(w->f, (loc - 1) >> 5, (loc - 1) & 31) + 2);
    w->live[(loc - 1) >> 5]--;
    w->f->loc[lc] = 0;
    w->f->live--;
    w->trimmed++;
    return 1;
}

// Writes the 4 KiB of logical cluster lc (data in RAM). Nothing is written
// when the cluster already holds exactly that.
static int ClasspadFugueWrite(CLASSPAD_FUGUE_WR *w, unsigned lc, const unsigned char *data)
{
    const unsigned char *old;
    unsigned ver = 0xFFFFFFFE, loc, n = 0;
    int r;

    if (!w->writable) return CLASSPAD_FUGUE_ERR_READONLY;
    if (lc >= w->f->clusters || lc * CLASSPAD_FUGUE_BLOCKS_PER_PAGE >= w->f->blocks) return CLASSPAD_FUGUE_ERR_RANGE;

    old = ClasspadFugueCluster(w->f, lc);
    if (old) {
        if (ClasspadFugueEqual(old, data, CLASSPAD_FUGUE_PAGE_BYTES)) return CLASSPAD_FUGUE_OK;
    } else if (ClasspadFugueBlank(data, CLASSPAD_FUGUE_PAGE_BYTES)) {
        return CLASSPAD_FUGUE_OK;       // no record reads as 0xFF already
    }

    while (w->erased < CLASSPAD_FUGUE_GC_LOW && (w->erased < CLASSPAD_FUGUE_RESERVE || !n)) {
        r = ClasspadFugueReclaim(w);
        if (r == CLASSPAD_FUGUE_ERR_FULL) break;
        if (r != CLASSPAD_FUGUE_OK) return r;
        n++;
    }

    if (w->f->loc[lc]) {                // after the reclaim: it may just have moved this cluster, one version down
        ver = ClasspadFugueBe32(ClasspadFugueMeta(w->f, (w->f->loc[lc] - 1) >> 5, (w->f->loc[lc] - 1) & 31) + 2);
        if (!ver) return CLASSPAD_FUGUE_ERR_FLASH;
        ver--;
    } else if (w->ver && w->ver[lc] != 0xFFFFFFFF) {
        // trimmed: records of it may still be in the flash, and the new one must win over them
        if (!w->ver[lc]) return CLASSPAD_FUGUE_ERR_FLASH;
        ver = w->ver[lc] - 1;
    }
    r = ClasspadFugueAppend(w, CLASSPAD_FUGUE_KIND_DATA, ver, lc, data, 1, &loc);
    if (r != CLASSPAD_FUGUE_OK) return r;
    if (w->ver) w->ver[lc] = ver;
    if (w->f->loc[lc]) w->live[(w->f->loc[lc] - 1) >> 5]--;
    else w->f->live++;
    w->f->loc[lc] = (unsigned short)loc;
    w->live[(loc - 1) >> 5]++;
    return CLASSPAD_FUGUE_OK;
}

#endif
