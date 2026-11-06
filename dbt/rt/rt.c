/*
 * rt.c - translator runtime, C half. Target-only (big-endian SH7305, P1
 * addresses, no OS). Everything here runs with interrupts masked or BL=1.
 */
#include "rt.h"
#include "lcdcon.h"
#include "lcd_io.h"
#include "irqpins.h"
#include "profile.h"

typedef __SIZE_TYPE__ dbt_size_t;
void *memcpy(void *d, const void *s, dbt_size_t n);
void *memset(void *d, int c, dbt_size_t n);

#define HSIZE        8192                 /* hash entries, power of two */
#ifndef MAX_BLOCKS
#define MAX_BLOCKS   14400                 /* compact block headers + a metadata arena; see docs/04 */
#endif
#ifndef META_ARENA_BYTES
#define META_ARENA_BYTES (1280u * 1024u)  /* meta + spill entries of all live blocks (~320 B per block in the CE boot) */
#endif
#define EXPEVT_REG   (*(volatile sf_u32 *)0xFF000024u)
#define TRA_REG      (*(volatile sf_u32 *)0xFF000020u)
#define DEBUG_PORT   (*(volatile sf_u8 *)0xFFFFF000u)

/* layout checks against ctx_offsets.h */
#define CHECK_OFF(field, off) typedef char check_##field[(__builtin_offsetof(dbt_gctx, field) == (off)) ? 1 : -1]
CHECK_OFF(sr, CTX_SR); CHECK_OFF(pr, CTX_PR); CHECK_OFF(gbr, CTX_GBR); CHECK_OFF(mach, CTX_MACH);
CHECK_OFF(macl, CTX_MACL); CHECK_OFF(r, CTX_R0); CHECK_OFF(slots, CTX_SLOTS); CHECK_OFF(host_spc, CTX_HOSTSPC);
CHECK_OFF(ssr, CTX_SSR); CHECK_OFF(expevt, CTX_EXPEVT); CHECK_OFF(tra, CTX_TRA); CHECK_OFF(tea, CTX_TEA);
CHECK_OFF(intevt, CTX_INTEVT); CHECK_OFF(vec_off, CTX_VECOFF); CHECK_OFF(jump_to, CTX_JUMPTO);
CHECK_OFF(new_spc, CTX_NEWSPC); CHECK_OFF(new_ssr, CTX_NEWSSR);
typedef char check_size[(sizeof(dbt_gctx) == CTX_SIZE) ? 1 : -1];

dbt_gctx dbt_ctx_rt, dbt_ctx_exc;

/* freestanding helpers GCC may call */
/* Freestanding memcpy/memset. Word loops when both sides are aligned: the
 * byte loops they replace were 22 % of a CE shell boot (every instruction
 * decode zeroes an sh_insn, every translation its context and block). */
/* Keep compiler-generated struct-copy calls resolvable under LTO. */
__attribute__((used, externally_visible))
void *memcpy(void *d, const void *s, dbt_size_t n)
{
    sf_u8 *a = d; const sf_u8 *b = s;
    if ((((sf_u32)a | (sf_u32)b) & 3) == 0) {
        sf_u32 *wa = (sf_u32 *)a; const sf_u32 *wb = (const sf_u32 *)b;
        while (n >= 16) { wa[0] = wb[0]; wa[1] = wb[1]; wa[2] = wb[2]; wa[3] = wb[3]; wa += 4; wb += 4; n -= 16; }
        while (n >= 4) { *wa++ = *wb++; n -= 4; }
        a = (sf_u8 *)wa; b = (const sf_u8 *)wb;
    }
    while (n--) *a++ = *b++;
    return d;
}
__attribute__((used, externally_visible))
void *memset(void *d, int c, dbt_size_t n)
{
    sf_u8 *a = d;
    sf_u32 w = (sf_u8)c; w |= w << 8; w |= w << 16;
    while (n && ((sf_u32)a & 3)) { *a++ = (sf_u8)c; n--; }
    {
        sf_u32 *wa = (sf_u32 *)a;
        while (n >= 16) { wa[0] = w; wa[1] = w; wa[2] = w; wa[3] = w; wa += 4; n -= 16; }
        while (n >= 4) { *wa++ = w; n -= 4; }
        a = (sf_u8 *)wa;
    }
    while (n--) *a++ = (sf_u8)c;
    return d;
}

volatile sf_u32 dbt_shadow_vbr = 0;
volatile sf_u32 dbt_shadow_md = SR_MD;
dbt_stats dbt_stat;
volatile sf_u32 dbt_lookup_sr; /* saved before lookup hash/IC tests alter T */

typedef struct { sf_u32 gpc; sf_u32 host; } hent;

hent dbt_hash[HSIZE] __attribute__((aligned(32)));   /* read by stubs.S */
#define g_hash dbt_hash
static dbt_block g_blocks[MAX_BLOCKS];      /* host_base is sorted within each cache bank */
static dbt_block g_scratch;                 /* translation target; copied into g_blocks + arena on success */
static dbt_imeta g_scratch_meta[DBT_MAX_META];
static dbt_spill g_scratch_spills[DBT_MAX_BLOCK_SPILLS];
static dbt_link g_scratch_links[DBT_MAX_LINKS];
static sf_u8     g_arena[META_ARENA_BYTES] __attribute__((aligned(8)));
static sf_u32    g_arena_used;
#define SCRATCH_ARENA_NEED (DBT_MAX_META * sizeof(dbt_imeta) + DBT_MAX_BLOCK_SPILLS * sizeof(dbt_spill) + DBT_MAX_LINKS * sizeof(dbt_link) + 16)
static dbt_rt_config g_cfg;
#ifndef CACHE_BANKS
#define CACHE_BANKS 12
#endif
#define BANK_BLOCKS (MAX_BLOCKS / CACHE_BANKS)
typedef char check_bank_mask[(CACHE_BANKS <= 16) ? 1 : -1];
typedef char check_bank_blocks[(MAX_BLOCKS % CACHE_BANKS == 0) ? 1 : -1];
typedef char check_page_indices[(MAX_BLOCKS * 2 < 65535) ? 1 : -1];
static unsigned g_bank, g_bank_count[CACHE_BANKS], g_allocated, g_bank_evictions;
#ifdef DBT_INTERIOR_CACHE
#ifndef DBT_INTERIOR_ENTRY
#error DBT_INTERIOR_CACHE requires DBT_INTERIOR_ENTRY
#endif
static sf_u32 g_bank_generation[CACHE_BANKS];
#endif
#ifdef DBT_KERNEL_BANKS
typedef char check_split_banks[(DBT_KERNEL_BANKS > 0 && DBT_KERNEL_BANKS < CACHE_BANKS) ? 1 : -1];
static unsigned g_stream_bank[2];
static sf_u32 g_saved_code[CACHE_BANKS], g_saved_arena[CACHE_BANKS];
#endif
static sf_u32 g_bank_touch[CACHE_BANKS], g_access_clock;
/* Conservative destinations of patched direct links. Bits remain set until
 * the owner is replaced; false positives only cost a scan. */
static sf_u16 g_link_banks[MAX_BLOCKS];
static sf_u32 bank_code_bytes(void);
static unsigned bank_of_code(sf_u32 host)
{
    sf_u32 span = bank_code_bytes(), off = host - g_cfg.cache_base;
    return span == 262144u ? off >> 18 : off / span;
}
static void touch_code(sf_u32 host)
{
#ifdef DBT_CACHE_LRU
    g_bank_touch[bank_of_code(host)] = ++g_access_clock;
#else
    (void)host;
#endif
}
#ifdef DBT_IRQ_ENTRY_CACHE
static sf_u32 g_irq_entry_hits;
#endif
static int       g_nblocks; /* table high-water mark; holes carry binfo.dead */

static sf_u32    g_cache_used;              /* bytes */
static fpu_state g_fpu;
static int       g_translating;             /* guest pc being translated or 0 */
static sf_u32    g_translating_pc;
/* Host return address (X1) of the helper call being served in C, 0 outside
 * helpers: a helper's own guest memory access (fmov, mac) can miss the TLB
 * like the instruction it emulates would, and the fault is then delivered
 * at that instruction's guest pc, found from the return address, so the
 * kernel maps the page and the instruction runs again (the helpers read
 * before they post-increment). */
static sf_u32    g_helper_ret;

/* X1: the helper call's return address (the word after the call's delay slot). */
static sf_u32 helper_return_address(void)
{
    register sf_u32 ret __asm__("r0");
    __asm__ volatile (".word 0x009A" : "=r"(ret));              /* sts x1,r0 (DSP register code 9, as STSX in stubs.S) */
    return ret;
}

/* Guest pc of the instruction a helper serves, from its return address. */
static sf_u32 helper_guest_pc(sf_u32 ret, int *in_insn_range);
static int guest_pc_fetchable(sf_u32 gpc);
static void deliver_synthetic(sf_u32 gpc, sf_u32 expevt);
static void sample_init(const dbt_rt_config *cfg);
static int g_window_kind, g_tail_from;      /* sampling profiler: see sample_tick */
static void print_stats(void);

/* ------------------------------------------- address spaces and the TLB */
/* Blocks are keyed by virtual pc. Code below 0x80000000 is TLB-mapped and
 * can mean different things in different processes (CE gives each its own
 * ASID) or after a remap, so such blocks also record the ASID they were made
 * under and the physical address of their first instruction (from the UTLB).
 * The hash and the inline caches hold only entries valid in the current
 * ASID; switches are noticed where the guest must pass through C anyway
 * (rte, ldc sr, ldtlb) by reading PTEH, or told to us by HC_ASID. Mapping
 * changes are noticed at ldtlb (new physical page for a page that has
 * blocks) or told to us by HC_TLB_INVAL / HC_FLUSH. See docs/05. */
#define REG_PTEH  (*(volatile sf_u32 *)0xFF000000u)
#define REG_PTEL  (*(volatile sf_u32 *)0xFF000004u)
#define REG_MMUCR (*(volatile sf_u32 *)0xFF000010u)
#define IS_USER(pc)  ((pc) < 0x80000000u)
#define PHYS_UNKNOWN 0xFFFFFFFFu
#define ASID_ANY     0xFFFFu
typedef struct { sf_u8 dead, user; sf_u16 asid; sf_u32 phys; } binfo;
static binfo  g_binfo[MAX_BLOCKS];
/* Every block by guest pc, chained through the fast-path hash's index: the
 * direct-mapped g_hash holds one block per slot and loses the others to
 * collisions, and finding those by scanning the block table cost a full
 * pass over all live blocks at every translation (0.24 ms of emulated time
 * each with 4096 blocks: a third of a CE boot). CHAIN_NONE ends a chain;
 * dead blocks stay linked and are skipped. */
#define CHAIN_NONE 0xFFFFu
static sf_u16 g_chain_head[HSIZE];
static sf_u16 g_chain_next[MAX_BLOCKS];
/* User blocks for TLB invalidation. Appended during the masked block commit
 * and reset on full flush; dead blocks are skipped by the scan. */
static sf_u16 g_user_idx[MAX_BLOCKS];
/* Only ICs filled with user targets since the last switch need clearing.
 * User-target stub misses enter C even on a hash hit to register the site.
 * Normal per-site IC hits stay entirely in translated code. */
static sf_u16 g_ic_active[MAX_BLOCKS];
static sf_u8 g_ic_active_seen[MAX_BLOCKS];
/* Only hash slots that received user code since the last ASID switch. */
static sf_u16 g_user_hash[HSIZE];
static sf_u8 g_user_hash_seen[HSIZE];
static unsigned g_nuser_hash;
static int    g_nuser, g_nic; /* g_nic counts ICs filled with user targets this ASID */
/* Conservative membership filter for 1 KiB virtual pages containing user
 * code. Most ldtlb calls refill DATA, for which a block scan does no work.
 * Never clear individual bits (collisions/dead blocks only cause extra scans).
 * Two pages cover even a block straddling a 1 KiB boundary. */
static sf_u32 g_user_pages[256];
/* Two nodes per block cover first/last 1 KiB pages. Rebuilt on bank reuse.
 * With interior-entry reuse this indexes kernel blocks too; the user-page
 * filter and TLB checks still only mark/consider user code. */
static sf_u16 g_page_head[8192], g_page_next[MAX_BLOCKS * 2];
static unsigned user_page_bit(sf_u32 va)
{
    sf_u32 p = va >> 10;
    return (p ^ (p >> 12)) & 8191u;
}
static void mark_user_page(sf_u32 va)
{
    unsigned bit = user_page_bit(va);
    g_user_pages[bit >> 5] |= 1u << (bit & 31);
}
static void index_user_block(unsigned index)
{
    dbt_block *b = &g_blocks[index];
    sf_u32 first = b->guest_pc >> 10, last = (b->guest_pc + b->guest_len - 1) >> 10;
    unsigned bit = user_page_bit(b->guest_pc), node = index * 2;
    g_page_next[node] = g_page_head[bit]; g_page_head[bit] = (sf_u16)node;
    if (g_binfo[index].user) mark_user_page(b->guest_pc);
    if (first != last) {
        bit = user_page_bit(b->guest_pc + b->guest_len - 1); ++node;
        g_page_next[node] = g_page_head[bit]; g_page_head[bit] = (sf_u16)node;
        if (g_binfo[index].user) mark_user_page(b->guest_pc + b->guest_len - 1);
    }
}
static int may_have_user_code(sf_u32 vpn, unsigned shift)
{
    sf_u32 end;
    if (shift > 12) return 1;       /* large pages: retain the original scan */
    end = vpn + (1u << shift);
    for (; vpn < end; vpn += 1024) {
        unsigned bit = user_page_bit(vpn);
        if (g_user_pages[bit >> 5] & (1u << (bit & 31))) return 1;
    }
    return 0;
}
static sf_u32 g_asid;
static dbt_env   g_env;

/* ---------------------------------------------------------- console */

#ifdef DBT_TRACE_EXEC
static void watch_char(int c);
#endif
void dbt_putc(int c)
{
#ifndef DBT_NO_DEBUG_PORT
    DEBUG_PORT = (sf_u8)c;          /* casio-emu only (make DEVICE=1 leaves it out) */
#else
    (void)c;
#endif
#ifdef DBT_LCD_CONSOLE
    lcdcon_putc(c);                 /* the panel console of DEVICE=1 builds (rt/lcdcon.c) */
#endif
#ifdef DBT_TRACE_EXEC
    watch_char(c);
#endif
}
void dbt_puts(const char *s) { while (*s) dbt_putc(*s++); }
void dbt_puthex(sf_u32 v)
{
    static const char hex[] = "0123456789abcdef";
    int i;
    for (i = 28; i >= 0; i -= 4) dbt_putc(hex[(v >> i) & 0xF]);
}
void dbt_halt(const char *why)
{
    sf_u32 sr;
#ifdef DBT_LCD_CONSOLE
    lcdcon_halt(why);                    /* take the panel back, show the last lines and the exception context */
#endif
    dbt_dbgring_drain();                 /* whatever the guest logged before dying */
    dbt_puts("\n[dbt] HALT: "); dbt_puts(why); dbt_puts("\n");
    /* stay dead: with interrupts deliverable a timer tick would re-enter the
     * exception path and run the (dead) kernel's handler as if nothing happened */
    __asm__ volatile ("stc sr, %0" : "=r"(sr));
    sr |= 0x100000F0u;                   /* BL=1, IMASK=15 */
    __asm__ volatile ("ldc %0, sr" :: "r"(sr) : "memory");
    for (;;) __asm__ volatile ("sleep");
}

/* ------------------------------------------------- guest memory access */
/* Guest memory is little-endian; the host is big-endian. */

static inline sf_u32 bswap32(sf_u32 v) { return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24); }
static inline sf_u32 bswap16(sf_u32 v) { return ((v >> 8) & 0xFFu) | ((v << 8) & 0xFF00u); }

static int g_fetch16(void *c, sf_u32 pc, sf_u32 *op)
{
    (void)c;
    if (pc & 1) return 1;
    *op = bswap16(*(volatile sf_u16 *)pc);
    return 0;
}
static int g_read16le(void *c, sf_u32 a, sf_u32 *v) { (void)c; if (a & 1) return 1; *v = bswap16(*(volatile sf_u16 *)a); return 0; }
static int g_read32le(void *c, sf_u32 a, sf_u32 *v) { (void)c; if (a & 3) return 1; *v = bswap32(*(volatile sf_u32 *)a); return 0; }

static int fpu_read32(void *c, sf_u32 a, sf_u32 *v) { return g_read32le(c, a, v); }
static int fpu_write32(void *c, sf_u32 a, sf_u32 v) { (void)c; if (a & 3) return 1; *(volatile sf_u32 *)a = bswap32(v); return 0; }

#ifdef DBT_INTERIOR_HASH
#ifndef DBT_INTERIOR_ENTRY
#error DBT_INTERIOR_HASH requires DBT_INTERIOR_ENTRY
#endif
/* A block with published aliases needs all its hash destinations removed,
 * not merely the slot keyed by its start PC. One bit per owner block. */
static sf_u8 g_alias_blocks[(MAX_BLOCKS + 7) / 8];
static sf_u32 g_alias_hash_removed;
#endif

/* ------------------------------------------------------------ hashing */

/* Direct-mapped, one slot per index, overwrite on collision: the asm fast
 * path in the lookup stubs probes exactly this slot, so the C side must not
 * probe further (an entry the asm cannot see costs a C call per lookup, and
 * linear probing over keys 2 bytes apart evicted neighbours constantly).
 * Folding in the upper half (one swap.w in asm) spreads blocks that are far
 * apart. A block missing from the hash is still found in the block table
 * (find_block_by_gpc) instead of being translated again. */
static inline sf_u32 hidx(sf_u32 gpc)
{
#ifdef DBT_HASH_FOLD14
    return ((gpc >> 1) ^ (gpc >> 14)) & (HSIZE - 1);
#else
    return ((gpc >> 1) ^ (gpc >> 16)) & (HSIZE - 1);
#endif
}

static void hash_insert(sf_u32 gpc, sf_u32 host)
{
    sf_u32 sr;
    unsigned slot = hidx(gpc);
    hent *e = &g_hash[slot];
    __asm__ volatile ("stc sr, %0" : "=r"(sr));
    __asm__ volatile ("ldc %0, sr" :: "r"(sr | SR_IMASK) : "memory");
    if (IS_USER(gpc) && !g_user_hash_seen[slot]) {
        g_user_hash[g_nuser_hash++] = (sf_u16)slot;
        g_user_hash_seen[slot] = 1;
    }
    e->host = host; e->gpc = gpc;
    __asm__ volatile ("ldc %0, sr" :: "r"(sr) : "memory");
}

static sf_u32 hash_find(sf_u32 gpc)
{
    const hent *e = &g_hash[hidx(gpc)];
    return (e->host && e->gpc == gpc) ? e->host : 0;
}

/* Empty slots carry gpc = 1 (odd: never a pc): the asm fast path compares
 * only the gpc, so a cleared slot must not keep its old key. */
static void hash_remove(const dbt_block *b)
{
#ifdef DBT_INTERIOR_HASH
    unsigned index = (unsigned)(b - g_blocks);
    if (g_alias_blocks[index >> 3] & (1u << (index & 7))) {
        unsigned i;
        sf_u32 lo = b->host_base, hi = lo + b->host_len;
#ifdef DBT_ALIAS_LOCAL_REMOVE
        /* Published entries are instruction PCs inside this owner. Probe
         * their possible slots instead of scanning the whole hash table.
         * Keep the destination check: a collision may now belong elsewhere. */
        for (i = 0; i < b->guest_len; i += 2) {
            hent *a = &g_hash[hidx(b->guest_pc + i)];
#else
        for (i = 0; i < HSIZE; ++i) {
            hent *a = &g_hash[i];
#endif
            if (a->host >= lo && a->host < hi) {
                a->gpc = 1; a->host = 0; ++g_alias_hash_removed;
            }
        }
        return;
    }
#endif
    hent *e = &g_hash[hidx(b->guest_pc)];
    if (e->host == b->host_base) { e->gpc = 1; e->host = 0; }
}

/* Physical address of virtual `va` under `asid` according to the UTLB
 * (array reads at 0xF6000000 / 0xF7000000), PHYS_UNKNOWN if unmapped. */
static sf_u32 utlb_translate(sf_u32 va, sf_u32 asid)
{
    static const sf_u8 shift_of_sz[4] = { 10, 12, 16, 20 };
    int e;
    if (!(REG_MMUCR & 1)) return va & 0x1FFFFFFFu;
#ifdef DBT_UTLB_HINT
    /* A hint caches only a hardware slot number, never a translation.
     * Re-read its VPN, ASID, validity and physical mapping on every use;
     * direct UTLB-array writes therefore cannot leave stale physical data. */
    {
        static sf_u8 hint[64];
        unsigned h = ((va >> 10) ^ asid) & 63u;
        sf_u32 a, d, sz, mask;
        e = hint[h];
        a = *(volatile sf_u32 *)(0xF6000000u | ((sf_u32)e << 8));
        d = *(volatile sf_u32 *)(0xF7000000u | ((sf_u32)e << 8));
        sz = ((d >> 4) & 1) | ((d >> 6) & 2);
        mask = ~((1u << shift_of_sz[sz]) - 1);
        if ((a & d & 0x100) && (a & 0xFFFFFC00u & mask) == (va & mask) &&
            ((d & 2) || (a & 0xff) == asid))
            return (d & 0x1FFFFC00u & mask) | (va & ~mask);
        for (e = 0; e < 64; ++e) {
            a = *(volatile sf_u32 *)(0xF6000000u | ((sf_u32)e << 8));
            d = *(volatile sf_u32 *)(0xF7000000u | ((sf_u32)e << 8));
            sz = ((d >> 4) & 1) | ((d >> 6) & 2);
            mask = ~((1u << shift_of_sz[sz]) - 1);
            if (!(a & d & 0x100) || (a & 0xFFFFFC00u & mask) != (va & mask) ||
                (!(d & 2) && (a & 0xff) != asid)) continue;
            hint[h] = (sf_u8)e;
            return (d & 0x1FFFFC00u & mask) | (va & ~mask);
        }
        return PHYS_UNKNOWN;
    }
#else
    for (e = 0; e < 64; e++) {
        sf_u32 a = *(volatile sf_u32 *)(0xF6000000u | ((sf_u32)e << 8));
        sf_u32 d = *(volatile sf_u32 *)(0xF7000000u | ((sf_u32)e << 8));
        sf_u32 sz = ((d >> 4) & 1) | ((d >> 6) & 2), mask = ~((1u << shift_of_sz[sz]) - 1);
        if (!(a & 0x100) || !(d & 0x100)) continue;                       /* V */
        if ((a & 0xFFFFFC00u & mask) != (va & mask)) continue;
        if (!(d & 2) && (a & 0xFF) != asid) continue;                     /* not shared: ASID must match */
        return (d & 0x1FFFFC00u & mask) | (va & ~mask);
    }
    return PHYS_UNKNOWN;
#endif
}

static void reset_ic(dbt_block *o)
{
    volatile sf_u32 *ic = (volatile sf_u32 *)(o->host_base + (sf_u32)o->ic_off);
    ic[0] = 1; ic[1] = 0;
}

/* Undo the chaining of link k of block o and point its literal back at the exit stub. */
static void unchain(dbt_block *o, int k)
{
    sf_u16 *w = (sf_u16 *)(o->host_base + (sf_u32)o->links[k].jump_off);
    w[0] = o->links[k].w0; w[1] = o->links[k].w1; w[2] = o->links[k].w2;
    if (o->links[k].cond) w[-1] = o->links[k].skip_w;
    *(volatile sf_u32 *)(o->host_base + (sf_u32)o->links[k].lit_off) = o->host_base + (sf_u32)o->links[k].stub_off;
    dbt_sync_code(o->host_base + (sf_u32)o->links[k].jump_off - 2, 10);
    dbt_stat.unchained++;
}

#ifdef DBT_BATCH_INVALIDATE
#ifndef CACHE_SIZE
#define CACHE_SIZE 0x00300000u
#endif
/* Blocks begin on 32-byte boundaries, so no live neighbour shares a bit.
 * The bitmap is cleared before a code bank is reused. */
static sf_u8 g_dead_code[(CACHE_SIZE + 255u) / 256u];
static unsigned g_dead_banks;
static int is_dead_host(sf_u32 host)
{
    sf_u32 off = host - g_cfg.cache_base;
    return off < g_cfg.cache_size &&
        (g_dead_code[off >> 8] & (1u << ((off >> 5) & 7)));
}
#endif

/* Make block b unreachable: hash entry, inline caches and chained jumps that
 * lead into it. Its cache memory stays dead until the next full flush. */
static void invalidate_block(dbt_block *b)
{
    PROFILE_BEGIN;
    int i, k;
    sf_u32 lo = b->host_base, hi = b->host_base + (sf_u32)b->host_len;
    unsigned bank_mask = 1u << bank_of_code(lo);
    g_binfo[b - g_blocks].dead = 1;
    hash_remove(b);
#ifdef DBT_BATCH_INVALIDATE
    for (i = (int)((lo - g_cfg.cache_base) >> 5);
         (sf_u32)i * 32 < hi - g_cfg.cache_base; ++i)
        g_dead_code[i >> 3] |= (sf_u8)(1u << (i & 7));
    g_dead_banks |= bank_mask;
    (void)k;
#else
    for (i = 0; i < g_nblocks; i++) {
        dbt_block *o = &g_blocks[i];
        if (g_binfo[i].dead) continue;
        if (o->ic_off >= 0) {
            sf_u32 h = *(volatile sf_u32 *)(o->host_base + (sf_u32)o->ic_off + 4);
            if (h >= lo && h < hi) reset_ic(o);
        }
        if (!(g_link_banks[i] & bank_mask)) continue;
        for (k = 0; k < o->nlinks; k++) {
            sf_u32 v = *(volatile sf_u32 *)(o->host_base + (sf_u32)o->links[k].lit_off);
            if (v >= lo && v < hi) unchain(o, k);
        }
    }
#endif
    dbt_stat.invalidations++;
    PROFILE_END(PROF_INVALIDATE);
}

/* Complete a batch before returning to guest execution. Multiple blocks
 * can die in one TLB/range flush; scan their incoming links only once. */
static void finish_invalidations(void)
{
#ifdef DBT_BATCH_INVALIDATE
    PROFILE_BEGIN;
    int i, k;
    if (!g_dead_banks) return;
    for (i = 0; i < g_nblocks; ++i) {
        dbt_block *b = &g_blocks[i];
        if (g_binfo[i].dead) continue;
        if (b->ic_off >= 0 && is_dead_host(*(volatile sf_u32 *)(b->host_base + b->ic_off + 4)))
            reset_ic(b);
        if (!(g_link_banks[i] & g_dead_banks)) continue;
        for (k = 0; k < b->nlinks; ++k)
            if (is_dead_host(*(volatile sf_u32 *)(b->host_base + b->links[k].lit_off)))
                unchain(b, k);
    }
    g_dead_banks = 0;
    PROFILE_END(PROF_INVALIDATE);
#endif
}

/* Drop every live block overlapping guest range [lo, hi); user_only skips P1-P3 code. */
static void invalidate_range(sf_u32 lo, sf_u32 hi, int user_only)
{
    int i;
    for (i = 0; i < g_nblocks; i++) {
        dbt_block *b = &g_blocks[i];
        if (g_binfo[i].dead || (user_only && !g_binfo[i].user)) continue;
        if (b->guest_pc < hi && b->guest_pc + b->guest_len > lo) invalidate_block(b);
    }
    finish_invalidations();
}

/* The address space changed: user blocks of other ASIDs leave the hash, and
 * inline caches aimed at user-space targets forget them. Blocks stay for
 * when their ASID returns (or for reuse through a shared physical page). */
static void switch_asid(sf_u32 asid)
{
    int j;
    PROFILE_BEGIN;
    if (asid == g_asid) { dbt_stat.asid_unchanged++; return; }
    g_asid = asid;
    dbt_stat.asid_switches++;
    for (j = 0; j < (int)g_nuser_hash; j++) {
        unsigned slot = g_user_hash[j];
        hent *e = &g_hash[slot];
        if (IS_USER(e->gpc)) { e->gpc = 1; e->host = 0; }
        g_user_hash_seen[slot] = 0;
    }
    g_nuser_hash = 0;
    for (j = 0; j < g_nic; j++) {
        unsigned index = g_ic_active[j];
        dbt_block *b = &g_blocks[index];
        volatile sf_u32 *ic = (volatile sf_u32 *)(b->host_base + (sf_u32)b->ic_off);
        if (IS_USER(ic[0])) { ic[0] = 1; ic[1] = 0; }
        g_ic_active_seen[index] = 0;
    }
    g_nic = 0;
    PROFILE_END(PROF_ASID);
}

static void note_asid(void)
{
    sf_u32 a = REG_PTEH & 0xFF;
    if (a != g_asid) switch_asid(a);
}

/* ldtlb executed by the guest: PTEH/PTEL describe the new entry. A page that
 * has blocks and now maps to another physical page loses them. */
static void tlb_check_block(unsigned i, sf_u32 vpn, sf_u32 mask,
    sf_u32 ptel, sf_u32 asid, sf_u32 shared)
{
    dbt_block *b;
    if (g_binfo[i].dead || !g_binfo[i].user) return;
    b = &g_blocks[i];
    if (b->guest_pc >= vpn + ~mask + 1 || b->guest_pc + b->guest_len <= vpn) return;
    if (!shared && g_binfo[i].asid != ASID_ANY && g_binfo[i].asid != asid) return;
    if (!(ptel & 0x100) || g_binfo[i].phys == PHYS_UNKNOWN ||
        (g_binfo[i].phys & mask) != (ptel & 0x1FFFFC00u & mask)) invalidate_block(b);
}

static void tlb_hook(void)
{
    PROFILE_BEGIN;
    static const sf_u8 shift_of_sz[4] = { 10, 12, 16, 20 };
    sf_u32 pteh = REG_PTEH, ptel = REG_PTEL;
    sf_u32 sz = ((ptel >> 4) & 1) | ((ptel >> 6) & 2), mask = ~((1u << shift_of_sz[sz]) - 1);
    sf_u32 vpn = pteh & 0xFFFFFC00u & mask, asid = pteh & 0xFF, shared = (ptel >> 1) & 1;
    int i, j;
    dbt_stat.tlb_hooks++;
    note_asid();
    if (!may_have_user_code(vpn, shift_of_sz[sz])) {
        dbt_stat.tlb_scan_skips++;
        __asm__ volatile ("ldtlb" ::: "memory");
        PROFILE_END(PROF_TLB);
        return;
    }
    if (shift_of_sz[sz] <= 12) {
        sf_u32 page, end = vpn + ~mask + 1;
        for (page = vpn; page < end; page += 1024) {
            unsigned node;
            for (node = g_page_head[user_page_bit(page)]; node != CHAIN_NONE; node = g_page_next[node])
                tlb_check_block(node >> 1, vpn, mask, ptel, asid, shared);
        }
    } else {
        for (j = 0; j < g_nuser; ++j) {
            i = g_user_idx[j];
            tlb_check_block((unsigned)i, vpn, mask, ptel, asid, shared);
        }
    }
    finish_invalidations();
    __asm__ volatile ("ldtlb" ::: "memory");
    PROFILE_END(PROF_TLB);
}

/* A live block for gpc usable in the current address space. */
static dbt_block *find_block_by_gpc(sf_u32 gpc)
{
    int i;
    sf_u32 phys = 0; int have_phys = 0;
    for (i = g_chain_head[hidx(gpc)]; i != CHAIN_NONE; i = g_chain_next[i]) {
        dbt_block *b = &g_blocks[i];
        if (g_binfo[i].dead || b->guest_pc != gpc) continue;
        if (!g_binfo[i].user || g_binfo[i].asid == g_asid) { touch_code(b->host_base); return b; }
        if (!have_phys) { phys = utlb_translate(gpc, g_asid); have_phys = 1; }
        if (phys != PHYS_UNKNOWN && g_binfo[i].phys == phys &&
            (gpc >> 10) == ((gpc + b->guest_len - 1) >> 10)) {
            /* This block can now be reached from more than its original
             * ASID. A remap in either process must invalidate it. Restrict
             * reuse to one minimum-size page; the first physical address
             * alone does not validate a second page in the new process. */
            g_binfo[i].asid = ASID_ANY;
            touch_code(b->host_base); return b;
        }
    }
    return 0;
}

#ifdef DBT_INTERIOR_ENTRY
static sf_u32 g_interior_hits;
#ifdef DBT_INTERIOR_HASH
static void publish_entry_hash(sf_u32 gpc, sf_u32 host, unsigned owner)
{
    sf_u32 sr;
    __asm__ volatile ("stc sr, %0" : "=r"(sr));
    __asm__ volatile ("ldc %0, sr" :: "r"(sr | SR_IMASK) : "memory");
    g_alias_blocks[owner >> 3] |= (sf_u8)(1u << (owner & 7));
    hash_insert(gpc, host);
    __asm__ volatile ("ldc %0, sr" :: "r"(sr) : "memory");
}
#endif
#ifdef DBT_INTERIOR_CACHE
static struct { sf_u32 gpc, host, asid, generation, index; } g_entry_cache[256];
static sf_u32 g_entry_cache_hits;
static unsigned entry_slot(sf_u32 gpc) { return ((gpc >> 1) ^ (gpc >> 9)) & 255u; }
static sf_u32 entry_asid(sf_u32 gpc) { return IS_USER(gpc) ? g_asid : ASID_ANY; }
static sf_u32 find_cached_entry(sf_u32 gpc)
{
    unsigned slot = entry_slot(gpc);
    if (g_entry_cache[slot].host && g_entry_cache[slot].gpc == gpc &&
        g_entry_cache[slot].asid == entry_asid(gpc) &&
        g_entry_cache[slot].generation == g_bank_generation[bank_of_code(g_entry_cache[slot].host)] &&
        !g_binfo[g_entry_cache[slot].index].dead) {
        ++g_interior_hits; ++g_entry_cache_hits;
        touch_code(g_entry_cache[slot].host);
#ifdef DBT_INTERIOR_HASH
        publish_entry_hash(gpc, g_entry_cache[slot].host, g_entry_cache[slot].index);
#endif
        return g_entry_cache[slot].host;
    }
    return 0;
}
#endif
/* ICs and direct links invalidate by destination host range. Optional
 * hash publication additionally tracks owners for single-block invalidation. */
static sf_u32 find_interior_entry(sf_u32 gpc)
{
    unsigned node;
    for (node = g_page_head[user_page_bit(gpc)]; node != CHAIN_NONE; node = g_page_next[node]) {
        unsigned i = node >> 1;
        dbt_block *b = &g_blocks[i];
        int off;
        if (g_binfo[i].dead || (g_binfo[i].user && g_binfo[i].asid != g_asid)) continue;
        off = dbt_block_entry(b, gpc);
        if (off >= 0) {
            ++g_interior_hits;
#ifdef DBT_INTERIOR_CACHE
            {
                unsigned slot = entry_slot(gpc);
                /* An interrupt may abandon C. Make the slot unreachable
                 * until every key/owner field describes the new target. */
                g_entry_cache[slot].host = 0;
                __asm__ volatile ("" ::: "memory");
                g_entry_cache[slot].gpc = gpc;
                g_entry_cache[slot].asid = entry_asid(gpc);
                g_entry_cache[slot].generation = g_bank_generation[bank_of_code(b->host_base)];
                g_entry_cache[slot].index = i;
                __asm__ volatile ("" ::: "memory");
                g_entry_cache[slot].host = b->host_base + (sf_u32)off;
            }
#endif
            touch_code(b->host_base);
#ifdef DBT_INTERIOR_HASH
            publish_entry_hash(gpc, b->host_base + (sf_u32)off, i);
#endif
            return b->host_base + (sf_u32)off;
        }
    }
    return 0;
}
#endif

/* ------------------------------------------------------- block cache */

static void cache_flush_all(void)
{
    PROFILE_BEGIN;
#ifdef DBT_TRACE_FLUSH
    sf_u32 cache_was = g_cache_used, arena_was = g_arena_used;
#endif
    sf_u32 i, sr;
    /* Interrupts land in the translator's C code and abandon it (the guest
     * resumes at the pc being translated). Clearing 8192 hash slots takes
     * about one tick, so an unmasked flush was restarted by every tick and
     * never finished: the Release boot livelocked at its first full flush
     * once the chain heads made the loop a little longer. Mask IMASK for
     * the flush (one tick of latency, once per cache fill). */
    __asm__ volatile ("stc sr, %0" : "=r"(sr));
    __asm__ volatile ("ldc %0, sr" :: "r"(sr | 0xF0u) : "memory");
    g_nblocks = 0; g_bank = 0; g_allocated = 0;
#ifdef DBT_INTERIOR_HASH
    memset(g_alias_blocks, 0, sizeof g_alias_blocks);
#endif
#ifdef DBT_INTERIOR_CACHE
    for (i = 0; i < CACHE_BANKS; ++i) ++g_bank_generation[i];
#endif
#ifdef DBT_KERNEL_BANKS
    g_stream_bank[0] = 0; g_stream_bank[1] = DBT_KERNEL_BANKS;
    for (i = 0; i < CACHE_BANKS; ++i) {
        g_saved_code[i] = i * ((g_cfg.cache_size / CACHE_BANKS) & ~31u);
        g_saved_arena[i] = i * ((META_ARENA_BYTES / CACHE_BANKS) & ~7u);
    }
#endif
#ifdef DBT_BATCH_INVALIDATE
    memset(g_dead_code, 0, sizeof g_dead_code); g_dead_banks = 0;
#endif
    memset(g_bank_count, 0, sizeof g_bank_count);
    memset(g_bank_touch, 0, sizeof g_bank_touch); g_access_clock = 0;
    for (i = 0; i < MAX_BLOCKS; ++i) g_binfo[i].dead = 1;
    g_nuser = 0; g_nic = 0; g_nuser_hash = 0;
    memset(g_user_hash_seen, 0, sizeof g_user_hash_seen);
    memset(g_ic_active_seen, 0, sizeof g_ic_active_seen);
    memset(g_user_pages, 0, sizeof g_user_pages);
    memset(g_page_head, 0xFF, sizeof g_page_head);
    g_cache_used = 0;
    g_arena_used = 0;
    for (i = 0; i < HSIZE; i++) { g_hash[i].gpc = 1; g_hash[i].host = 0; g_chain_head[i] = CHAIN_NONE; }
    dbt_stat.flushes++;
    PROFILE_END(PROF_FLUSH);
    __asm__ volatile ("ldc %0, sr" :: "r"(sr) : "memory");
#ifdef DBT_TRACE_FLUSH
    if (dbt_stat.flushes > 1) {
        dbt_puts("[dbt] full flush "); dbt_puthex(dbt_stat.flushes); dbt_puts(" cache="); dbt_puthex(cache_was);
        dbt_puts(" arena="); dbt_puthex(arena_was); dbt_puts(": "); print_stats();
    }
#endif
}

/* Recycle one disjoint code/metadata bank. Surviving host code never moves.
 * Incoming direct chains and ICs are detached before bytes are reused; the
 * guest-PC chains are rebuilt so a recycled table index cannot form a cycle. */
static sf_u32 bank_code_bytes(void) { return (g_cfg.cache_size / CACHE_BANKS) & ~31u; }
static sf_u32 bank_arena_bytes(void) { return (META_ARENA_BYTES / CACHE_BANKS) & ~7u; }
static void cache_next_bank(void)
{
    PROFILE_BEGIN;
    sf_u32 sr, i, lo, hi;
#ifdef DBT_KERNEL_BANKS
    unsigned stream = g_bank >= DBT_KERNEL_BANKS;
    unsigned bank = stream ? DBT_KERNEL_BANKS + (g_bank - DBT_KERNEL_BANKS + 1) % (CACHE_BANKS - DBT_KERNEL_BANKS)
                           : (g_bank + 1) % DBT_KERNEL_BANKS;
#else
    unsigned bank = (g_bank + 1) % CACHE_BANKS;
#endif
    unsigned first, count;
    sf_u32 oldest = 0;
    (void)oldest;
#if defined(DBT_CACHE_LRU) && !defined(DBT_KERNEL_BANKS)
    /* C lookup/refill and exception recovery sample actual code use. Protect
     * frequently reused banks instead of evicting hot kernel code by age alone. */
    for (i = 0; i < CACHE_BANKS; ++i) {
        sf_u32 age;
        if (i == g_bank) continue;
        if (!g_bank_count[i]) { bank = i; break; }
        age = g_access_clock - g_bank_touch[i];
        if (age >= oldest) { oldest = age; bank = i; }
    }
#endif
    first = bank * BANK_BLOCKS; count = g_bank_count[bank];
    __asm__ volatile ("stc sr, %0" : "=r"(sr));
    __asm__ volatile ("ldc %0, sr" :: "r"(sr | SR_IMASK) : "memory");
    lo = g_cfg.cache_base + bank * bank_code_bytes();
    hi = lo + bank_code_bytes();
    if (count) {
#ifdef DBT_INTERIOR_CACHE
        ++g_bank_generation[bank];
#endif
        g_nic = 0;
        memset(g_ic_active_seen, 0, sizeof g_ic_active_seen);
        for (i = first; i < first + count; ++i) g_binfo[i].dead = 1;
        for (i = 0; i < (unsigned)g_nblocks; ++i) {
            dbt_block *b = &g_blocks[i];
            int k;
            if (g_binfo[i].dead) continue;
            if (b->ic_off >= 0) {
                volatile sf_u32 *ic = (volatile sf_u32 *)(b->host_base + (sf_u32)b->ic_off);
                if (ic[1] >= lo && ic[1] < hi) reset_ic(b);
                else if (ic[0] != 1 && IS_USER(ic[0])) {
                    g_ic_active[g_nic++] = (sf_u16)i;
                    g_ic_active_seen[i] = 1;
                }
            }
            if (!(g_link_banks[i] & (1u << bank))) continue;
            for (k = 0; k < b->nlinks; ++k) {
                sf_u32 v = *(volatile sf_u32 *)(b->host_base + (sf_u32)b->links[k].lit_off);
                if (v >= lo && v < hi) unchain(b, k);
            }
        }
        g_nuser = 0; g_nuser_hash = 0;
        memset(g_user_hash_seen, 0, sizeof g_user_hash_seen);
        memset(g_user_pages, 0, sizeof g_user_pages);
        memset(g_page_head, 0xFF, sizeof g_page_head);
        for (i = 0; i < HSIZE; ++i) {
            hent *e = &g_hash[i];
            if (e->host >= lo && e->host < hi) { e->gpc = 1; e->host = 0; }
            else if (e->host && e->gpc != 1 && IS_USER(e->gpc)) {
                g_user_hash[g_nuser_hash++] = (sf_u16)i;
                g_user_hash_seen[i] = 1;
            }
            g_chain_head[i] = CHAIN_NONE;
        }
        for (i = 0; i < (unsigned)g_nblocks; ++i) {
            dbt_block *b = &g_blocks[i];
            unsigned h;
            if (g_binfo[i].dead) continue;
            h = hidx(b->guest_pc);
            g_chain_next[i] = g_chain_head[h]; g_chain_head[h] = (sf_u16)i;
#ifdef DBT_INTERIOR_ENTRY
            index_user_block(i);
#endif
            if (g_binfo[i].user) {
                g_user_idx[g_nuser++] = (sf_u16)i;
#ifndef DBT_INTERIOR_ENTRY
                index_user_block(i);
#endif
            }
        }
        g_allocated -= count;
        g_bank_evictions++;
        dbt_stat.flushes++; /* generation guard: callers must not patch a recycled site */
        PROFILE_END(PROF_FLUSH);
    }
#ifdef DBT_BATCH_INVALIDATE
    if (((lo - g_cfg.cache_base) & 255u) == 0 && ((hi - lo) & 255u) == 0)
        memset(g_dead_code + (lo - g_cfg.cache_base) / 256, 0, (hi - lo) / 256);
    else for (i = (lo - g_cfg.cache_base) >> 5; i < (hi - g_cfg.cache_base) >> 5; ++i)
        g_dead_code[i >> 3] &= (sf_u8)~(1u << (i & 7));
#endif
    g_bank_count[bank] = 0; g_bank = bank;
#ifdef DBT_KERNEL_BANKS
    g_stream_bank[stream] = bank;
#endif
    g_cache_used = bank * bank_code_bytes();
    g_arena_used = bank * bank_arena_bytes();
    __asm__ volatile ("ldc %0, sr" :: "r"(sr) : "memory");
}

/* ---------------------------------------------------------- debug ring
 * An exception-free console for guest code that cannot trap: the CE kernel
 * calls the OAL's OEMWriteDebugByte with BL=1 during SHInit (and trapa with
 * BL=1 is a manual reset). The guest appends bytes to a ring in the last
 * 4 KiB page of its RAM window and the translator prints them whenever it is
 * entered (lookup miss, hypercall, halt). Layout, guest little-endian words,
 * accessed uncached (P2) on both sides:
 *   +0  wr     bytes appended by the guest (monotonic)
 *   +4  rd     bytes printed, written back by the translator
 *   +8  magic  0x47424452 ('RBGD'), set here at init
 *   +16 ..     storage: byte i of the stream lives at 16 + i % DBGRING_BYTES */
#define DBGRING_BYTES (4096 - 16)
#define DBGRING_MAGIC 0x47424452u
static volatile sf_u32 *g_dbgring;
static sf_u32 g_dbgring_rd;

static void dbgring_init(void)
{
    g_dbgring = (volatile sf_u32 *)((g_cfg.guest_ram_hi - 0x1000) | 0x20000000u);
    g_dbgring[0] = 0; g_dbgring[1] = 0; g_dbgring[2] = bswap32(DBGRING_MAGIC);
    g_dbgring_rd = 0;
}

void dbt_dbgring_drain(void)
{
    sf_u32 wr;
    volatile sf_u8 *bytes;
    if (!g_dbgring) return;
    wr = bswap32(g_dbgring[0]);
    if (wr == g_dbgring_rd) return;
    bytes = (volatile sf_u8 *)g_dbgring + 16;
    if (wr - g_dbgring_rd > DBGRING_BYTES) g_dbgring_rd = wr - DBGRING_BYTES;   /* overrun: the oldest bytes are gone */
    while (g_dbgring_rd != wr) {
        dbt_putc(bytes[g_dbgring_rd % DBGRING_BYTES]);
        g_dbgring_rd++;
        dbt_stat.dbgring_bytes++;
    }
    g_dbgring[1] = bswap32(g_dbgring_rd);
}

/* ------------------------------------------------------------ exec trace
 * -DDBT_TRACE_EXEC: translated blocks record their guest pc in a ring (see
 * trace_prologue in translate.c); the last entries are printed when the
 * guest's debug output contains DBT_WATCH (default "DEBUGCHK"), at a halt,
 * and on request. Guest pcs only; tools/cesym.py maps them to symbols. */
#ifdef DBT_TRACE_EXEC
#ifndef DBT_WATCH
#define DBT_WATCH "DEBUGCHK"
#endif
static sf_u32 g_exec_ctl;                                   /* byte index into the ring */
static sf_u8 g_exec_ring[65536] __attribute__((aligned(4)));
static const char g_watch[] = DBT_WATCH;
static unsigned g_watch_pos;

void dbt_exec_dump(unsigned n)
{
    unsigned i, idx = g_exec_ctl;
    dbt_puts("[dbt] exec trace, oldest first ("); dbt_puthex(n); dbt_puts(" entries, ctl "); dbt_puthex(idx); dbt_puts("):\n");
    for (i = n; i > 0; i--) {
        sf_u32 at = (idx - 4u * i) & 0xFFFFu;
        sf_u32 pc = *(volatile sf_u32 *)(g_exec_ring + at);
        dbt_puts("[dbt] exec "); dbt_puthex(pc); dbt_puts("\n");
    }
}

static void watch_char(int c)
{
    static int dumping;
    if (dumping) return;                                    /* the dump itself goes through dbt_putc */
    if ((char)c == g_watch[g_watch_pos]) {
        if (!g_watch[++g_watch_pos]) {
            g_watch_pos = 0;
            dumping = 1;
            dbt_puts("\n[dbt] watch string seen\n");
            dbt_exec_dump(512);
            dumping = 0;
        }
    } else g_watch_pos = ((char)c == g_watch[0]) ? 1 : 0;
}
#endif

void dbt_rt_init(const dbt_rt_config *cfg)
{
    g_cfg = *cfg;
    memset(&dbt_stat, 0, sizeof dbt_stat);
    dbgring_init();
    profile_init();
    sample_init(cfg);
    cache_flush_all();
    fpuemu_reset(&g_fpu);
    memset(&g_env, 0, sizeof g_env);
    g_env.fetch16 = g_fetch16; g_env.read16le = g_read16le; g_env.read32le = g_read32le;
    g_env.rt_lookup_link = (sf_u32)&dbt_lookup_link;
    g_env.rt_lookup = (sf_u32)&dbt_lookup;
    g_env.rt_lookup_ic = (sf_u32)&dbt_lookup_ic;
    g_env.rt_rte = (sf_u32)&dbt_rte_stub;
    g_env.rt_fpu = (sf_u32)&dbt_fpu_stub;
    g_env.rt_fallback = (sf_u32)&dbt_fallback_stub;
    g_env.rt_ldc_sr = (sf_u32)&dbt_ldc_sr_stub;
    g_env.shadow_vbr_addr = (sf_u32)&dbt_shadow_vbr;
    g_env.shadow_md_addr = (sf_u32)&dbt_shadow_md;
#ifdef DBT_RUNTIME_MAX_INSNS
    g_env.max_insns = DBT_RUNTIME_MAX_INSNS;
#else
    g_env.max_insns = DBT_MAX_INSNS;
#endif
#ifdef DBT_NO_BORROW
    g_env.no_borrow = 1;                 /* experiments: translate without the borrowed register */
#endif
    g_env.dev_lo = cfg->dev_lo; g_env.dev_hi = cfg->dev_hi;
#ifdef DBT_TRACE_EXEC
    g_env.exec_ctl = (sf_u32)&g_exec_ctl; g_env.exec_ring = (sf_u32)g_exec_ring;   /* after the memset above */
#endif
}

static dbt_block *translate_at(sf_u32 gpc)
{
    PROFILE_BEGIN;
    dbt_block *b;
    sf_u32 host;
    sf_u32 fault = 0, publish_sr;
    int r;
    unsigned index;
#ifdef DBT_KERNEL_BANKS
    /* Keep transient user translations from evicting the kernel working set
     * (and vice versa). Each stream retains its own active bank and cursor. */
    g_saved_code[g_bank] = g_cache_used; g_saved_arena[g_bank] = g_arena_used;
    g_bank = g_stream_bank[IS_USER(gpc) ? 1 : 0];
    g_cache_used = g_saved_code[g_bank]; g_arena_used = g_saved_arena[g_bank];
#endif
    if (!guest_pc_fetchable(gpc)) { dbt_stat.synth_exceptions++; deliver_synthetic(gpc, 0x0E0); }
    if (g_bank_count[g_bank] >= BANK_BLOCKS ||
        g_cache_used + DBT_BLOCK_WORDS * 2 > (g_bank + 1) * bank_code_bytes() ||
        g_arena_used + SCRATCH_ARENA_NEED > (g_bank + 1) * bank_arena_bytes()) {
#ifdef DBT_PROFILE_BANK
        dbt_puts("[bank] blocks="); dbt_puthex(g_bank_count[g_bank]);
        dbt_puts(" code="); dbt_puthex(g_cache_used - g_bank * bank_code_bytes());
        dbt_puts(" meta="); dbt_puthex(g_arena_used - g_bank * bank_arena_bytes()); dbt_puts("\n");
#endif
        cache_next_bank();
    }
    host = g_cfg.cache_base + g_cache_used;
    g_scratch.meta = g_scratch_meta; g_scratch.spills = g_scratch_spills; g_scratch.links = g_scratch_links;
    if (IS_USER(gpc) || gpc >= 0xC0000000u) {
        /* TLB-mapped code: the probe may miss, which with BL=1 is a reset */
        sf_u32 sr;
        __asm__ volatile ("stc sr, %0" : "=r"(sr));
        if (sr & SR_BL) {
            dbt_puts("[dbt] WARNING: translating mapped code "); dbt_puthex(gpc);
            dbt_puts(" with BL=1 (a TLB miss here resets the CPU)\n");
        }
    }
    g_translating = 1; g_translating_pc = gpc;
    r = dbt_translate_block(&g_env, gpc, host, (sf_u16 *)host, DBT_BLOCK_WORDS, &g_scratch, &fault);
    g_translating = 0;
    index = g_bank * BANK_BLOCKS + g_bank_count[g_bank];
    b = &g_blocks[index];
    if (r == DBT_OK) {
        /* commit: block header into the table, used metadata into the arena */
        sf_u32 nm, ns = (sf_u32)g_scratch.nspills * sizeof(dbt_spill);
        *b = g_scratch;
        b->meta = (dbt_imeta *)(g_arena + g_arena_used);
        nm = dbt_pack_meta(b->meta, &g_scratch); b->packed_meta = 1;
        g_arena_used += (nm + 7) & ~7u;
        b->spills = (dbt_spill *)(g_arena + g_arena_used); memcpy(b->spills, g_scratch_spills, ns); g_arena_used += (ns + 7) & ~7u;
        b->links = (dbt_link *)(g_arena + g_arena_used);
        memcpy(b->links, g_scratch_links, (sf_u32)b->nlinks * sizeof(dbt_link));
        g_arena_used += ((sf_u32)b->nlinks * sizeof(dbt_link) + 7) & ~7u;
        dbt_stat.arena_bytes = g_arena_used;
    }
    if (r != DBT_OK) {
        dbt_puts("[dbt] translate error "); dbt_puthex((sf_u32)r);
        dbt_puts(" at "); dbt_puthex(gpc); dbt_puts(" fault "); dbt_puthex(fault); dbt_puts("\n");
        dbt_halt("translation failed");
    }
    g_cache_used += ((sf_u32)b->host_len + 31u) & ~31u;
    dbt_sync_code(host, (sf_u32)b->host_len);     /* before anything can find the block: see below */
    __asm__ volatile ("stc sr, %0" : "=r"(publish_sr));
    __asm__ volatile ("ldc %0, sr" :: "r"(publish_sr | SR_IMASK) : "memory");
    {
        binfo *bi = &g_binfo[index];
        g_link_banks[index] = 0;
#ifdef DBT_INTERIOR_HASH
        g_alias_blocks[index >> 3] &= (sf_u8)~(1u << (index & 7));
#endif
        bi->dead = 0; bi->user = (sf_u8)IS_USER(gpc);
        bi->asid = bi->user ? (sf_u16)g_asid : (sf_u16)ASID_ANY;
        /* P1/P2 code is its own physical address; user pcs and the kernel's
         * TLB-mapped range (0xC0000000 up: XIP DLL mappings, kernel
         * VirtualAlloc) go through the UTLB. With pc & 0x1FFFFFFF a kernel
         * VA whose low bits fell inside the ROM's physical range was taken
         * for ROM and survived whole-cache flushes, the only sync the
         * kernel does when it reuses such a page for new code. */
        bi->phys = (bi->user || gpc >= 0xC0000000u) ? utlb_translate(gpc, g_asid) : (gpc & 0x1FFFFFFFu);
        g_chain_next[index] = g_chain_head[hidx(gpc)];
#ifdef DBT_INTERIOR_ENTRY
        index_user_block(index);
#endif
        if (bi->user) {
#ifndef DBT_INTERIOR_ENTRY
            index_user_block(index);
#endif
            if (g_nuser < MAX_BLOCKS) g_user_idx[g_nuser++] = (sf_u16)index;
        }

    }
    /* Publish the block and its IC address atomically. Otherwise an abandoned
     * append could retain an IC address in scratch code later overwritten by
     * a different block. Code is synced before publication; masking lasts
     * only for the mapping/index commit, never decoding or code generation. */
    g_bank_count[g_bank]++; g_allocated++;
    if (g_nblocks <= (int)index) g_nblocks = (int)index + 1;
    g_chain_head[hidx(gpc)] = (sf_u16)index;
    hash_insert(gpc, host);
    __asm__ volatile ("ldc %0, sr" :: "r"(publish_sr) : "memory");
#if defined(DBT_TRACE_LINK) || defined(DBT_TRACE_BLOCKS)
    /* -DDBT_TRACE_BLOCKS (EXTRA_CFLAGS) prints every block translated: the
     * cheapest way to follow a guest that dies without a word */
    dbt_puts("[dbt] translate gpc="); dbt_puthex(gpc); dbt_puts(" host="); dbt_puthex(host);
    dbt_puts(" len="); dbt_puthex((sf_u32)b->guest_len); dbt_puts(" end="); dbt_puthex((sf_u32)b->end_reason); dbt_puts("\n");
#endif
    dbt_stat.translations++;
    dbt_stat.blocks = g_allocated;
    PROFILE_END(PROF_TRANSLATE);
    return b;
}

static sf_u32 lookup_or_translate(sf_u32 gpc)
{
    sf_u32 host;
    note_asid();
    host = hash_find(gpc);
#ifdef DBT_INTERIOR_CACHE
    if (!host) host = find_cached_entry(gpc);
#endif
    if (!host) {
        dbt_block *b = find_block_by_gpc(gpc);     /* evicted from the hash by a collision */
        if (b) { dbt_stat.hash_evicted++; hash_insert(gpc, b->host_base); host = b->host_base; }
        else {
#ifdef DBT_INTERIOR_ENTRY
            host = find_interior_entry(gpc);
#endif
            if (!host) host = translate_at(gpc)->host_base;
        }
    }
    touch_code(host);
    return host;
}

static dbt_block *block_of_host(sf_u32 host);

sf_u32 dbt_resolve_ic(sf_u32 gpc, sf_u32 ic_addr)
{
    sf_u32 gen = dbt_stat.flushes;
    sf_u32 host = lookup_or_translate(gpc);
    volatile sf_u32 *ic = (volatile sf_u32 *)ic_addr;
    dbt_stat.lookups_c++;
    dbt_stat.ic_misses_c++;
    if (dbt_stat.flushes == gen) {
        if (IS_USER(gpc)) {
            dbt_block *owner = block_of_host(ic_addr);
            if (owner) {
                unsigned index = (unsigned)(owner - g_blocks);
                if (!g_ic_active_seen[index]) {
                    g_ic_active[g_nic++] = (sf_u16)index;
                    g_ic_active_seen[index] = 1;
                }
            }
        }
        ic[0] = 1; ic[1] = host; ic[0] = gpc;
    }   /* data: read by mov.l @(disp,PC) through the operand cache; not after a flush (the cache is gone) */
    g_tail_from = 1;
    return host;
}

/* Block containing a host address (blocks are sorted by host_base). */
static dbt_block *block_of_host(sf_u32 host)
{
    unsigned bank;
    int lo, hi;
    if (host < g_cfg.cache_base || host >= g_cfg.cache_base + CACHE_BANKS * bank_code_bytes()) return 0;
    bank = bank_of_code(host);
    lo = (int)(bank * BANK_BLOCKS); hi = lo + (int)g_bank_count[bank] - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        dbt_block *b = &g_blocks[mid];
        if (host < b->host_base) hi = mid - 1;
        else if (host >= b->host_base + (sf_u32)b->host_len) lo = mid + 1;
        else { touch_code(host); return b; }
    }
    return 0;
}

/* Link a direct jump: patch its literal and, when the target is in branch
 * range, rewrite the jump sequence into native branches (chaining). The
 * block owning the literal may have been flushed by the translation. */
static void link_direct(sf_u32 link_lit, sf_u32 host)
{
    dbt_block *b = block_of_host(link_lit);
    int k;
    *(volatile sf_u32 *)link_lit = host;                 /* literal read via the operand cache: coherent */
    if (!b) return;
    g_link_banks[b - g_blocks] |= (sf_u16)(1u << bank_of_code(host));
    for (k = 0; k < b->nlinks; k++) {
        if (b->host_base + (sf_u32)b->links[k].lit_off == link_lit) {
            sf_u32 jaddr = b->host_base + (sf_u32)b->links[k].jump_off, from;
            /* a block's back edge to itself enters past the borrowed
             * register's park (its first word) and skips the restore: the
             * register stays borrowed across iterations. Not under the
             * execution trace, whose prologue comes first. */
            int self = b->borrow >= 0 && host == b->host_base && !g_env.exec_ring;
            int n = dbt_chain_direct((sf_u16 *)jaddr, jaddr, self ? host + 2 : host, b->links[k].cond, b->links[k].backward, self, &from);
            if (n) { dbt_sync_code(from, (sf_u32)n); dbt_stat.chained++; }
#ifdef DBT_TRACE_LINK
            dbt_puts("[dbt] link blk="); dbt_puthex(b->guest_pc); dbt_puts(" jump="); dbt_puthex(jaddr);
            dbt_puts(" -> "); dbt_puthex(host); dbt_puts(" cond="); dbt_puthex(b->links[k].cond);
            dbt_puts(" back="); dbt_puthex(b->links[k].backward); dbt_puts(" n="); dbt_puthex((sf_u32)n); dbt_puts("\n");
#endif
            return;
        }
    }
}

static sf_u32 g_devrange_lo, g_devrange_hi;
static int g_devrange_pending;

sf_u32 dbt_resolve(sf_u32 gpc, sf_u32 link_lit)
{
    sf_u32 host, gen;
    PROFILE_BEGIN;
    dbt_dbgring_drain();
    if (g_devrange_pending) {
        /* HC_DEVRANGE: retranslate everything with the new range; the link
         * literal belongs to a block that is gone, so it is not patched */
        g_devrange_pending = 0;
        g_env.dev_lo = g_devrange_lo; g_env.dev_hi = g_devrange_hi;
        cache_flush_all();
        link_lit = 0;
    }
    gen = dbt_stat.flushes;
    host = lookup_or_translate(gpc);
    dbt_stat.lookups_c++;
    /* a full flush inside the lookup freed the block holding the link
     * literal (and maybe reused its memory for the new one): leave it alone */
    if (link_lit && dbt_stat.flushes == gen) link_direct(link_lit, host);
    PROFILE_END(PROF_LOOKUP);
    g_tail_from = 1;
    return host;
}

void dbt_invalidate(sf_u32 lo, sf_u32 hi)
{
    invalidate_range(lo, hi, 0);
}

/* HC_FLUSH with no range: the kernel synchronised the whole instruction
 * cache (InitModule, CreateNewProcHelper) without saying what it wrote.
 * Blocks whose code lies in the ROM image (XIP: executed in place from the
 * NK.bin records, directly mapped or through a kernel-mode DLL's fixed-up
 * TLB mapping) cannot be stale, the image is never written; dropping them
 * cost a retranslation of all hot code at every module load. Everything
 * else goes. */
static void invalidate_all_but_rom(void)
{
    int i, kept = 0;
    for (i = 0; i < g_nblocks; i++) {
        dbt_block *b = &g_blocks[i];
        sf_u32 pa;
        if (g_binfo[i].dead) continue;
        pa = (b->guest_pc >= 0x80000000u && b->guest_pc < 0xC0000000u) ? (b->guest_pc & 0x1FFFFFFFu) : g_binfo[i].phys;
        if (g_cfg.rom_hi && pa != PHYS_UNKNOWN && pa >= g_cfg.rom_lo && pa + b->guest_len <= g_cfg.rom_hi) { kept++; continue; }
        invalidate_block(b);
    }
    finish_invalidations();
    dbt_stat.rom_kept += (sf_u32)kept;
}

/* ------------------------------------------------------------ hypercalls */

#define HC_EXIT   0xF0
#define HC_PUTS   0xF1
#define HC_FLUSH  0xF2
#define HC_PUTC   0xF3
#define HC_PUTHEX 0xF4
#define HC_STATS  0xF5
#define HC_BENCH  0xF6
#define HC_DUMP   0xF7                  /* r4 = host address, r5 = number of 16-bit words to print */
#define HC_SYNTH  0xF8                  /* translator-internal: deliver a synthetic exception (r4 = guest pc, r5 = EXPEVT) */
#define HC_TLB_INVAL 0xF9               /* kernel: user mappings in [r4, r4+r5) changed (r5 = 0: all) */
#define HC_ASID   0xFA                  /* kernel: address space switched to ASID r4 */
#define HC_GETSTAT 0xFB                 /* r4 = index -> r0 (0 translations, 1 blocks, 2 invalidations, 3 asid switches, 4 ic misses, 5 debug-ring bytes, 6 device accesses, 7 full flushes, 8 FPU-disable exceptions, 9/10 device-aware range lo/hi,
                                         * 11-14 profile.h, 23-29 more counters, 32+3b profile bucket b, 64-116 tick samples / exceptions / machine registers, 128-383 the code histogram:
                                         * the complete list is kept in bsp/classpad/SRC/INC/classpad_hc.h) */
#define HC_DEVRANGE 0xFC                /* tests: translate [r4, r5) device-aware from now on (drops all translations at the next lookup) */
#define HC_LCD_BLIT 0xFD                /* r4 = pixels as the guest stores them (P1/P2 address), r5 = pixel count, r6 = 16-bit LCD data port: written natively */
#define HC_LCD_PUSH 0xFE                /* DCS panel: r4 = pixels (P1/P2), r5 = first row, r6 = rows, r7 = width: CASET/PASET/RAMWR and the pixels in one go */

#define REG_EXPEVT (*(volatile sf_u32 *)0xFF000024u)
#define REG_TEA    (*(volatile sf_u32 *)0xFF00000Cu)

static int g_exc_depth;                 /* > 0 while dbt_exc_c runs (resolving a guest handler) */

/* A guest pc the CPU could not fetch from: odd, or in P4 (control space,
 * 0xE0000000 up). Real hardware raises an instruction address error there;
 * the translator's data-read probe would not (P4 reads return cache/TLB
 * arrays), so the check is made here. Windows CE's API calls rely on it:
 * they jump to 0xF000xxxx addresses and the kernel's handler decodes SPC. */
static int guest_pc_fetchable(sf_u32 gpc) { return !(gpc & 1) && gpc < 0xE0000000u; }

/* Called from the translator (stub context, guest registers in dbt_ctx_rt):
 * raise the synthetic exception through our own vector. Never returns. */
static void deliver_synthetic(sf_u32 gpc, sf_u32 expevt)
{
    if (g_exc_depth) {
        dbt_puts("[dbt] guest exception handler at "); dbt_puthex(gpc); dbt_puts(" is not fetchable\n");
        dbt_halt("guest VBR unusable");
    }
    register sf_u32 r4 __asm__("r4") = gpc;
    register sf_u32 r5 __asm__("r5") = expevt;
    __asm__ volatile ("trapa #0xF8" :: "r"(r4), "r"(r5) : "memory");
    for (;;) ;
}

static void print_trace(void);

/* ------------------------------------------------------------ sampling profiler
 * Every system tick (vector 0x600, INTEVT 0x400 = TMU0) classifies the host
 * pc it interrupted and the guest pc it resumes at, so a run on the real
 * machine can be profiled with nothing but HC_GETSTAT (indices 64 up, read by
 * bsp/classpad/SRC/APPS/DBTSTAT): the share of ticks in translated guest
 * code, in the lookup/helper stub windows, inside a translation or a helper,
 * in the kernel's idle `sleep`; and a 256-bucket histogram of guest code by
 * physical address over the ROM image (kernel and XIP DLLs alike: a user pc
 * is translated through the UTLB), to symbolise with tools/cesym.py. A tick
 * that lands in masked translator C (lookups, hypercalls, the exception path)
 * is delivered at the re-enabling instruction and so counts as guest code at
 * the pc that entered the stub; the PROFILE=1 TMU1 buckets measure that
 * time. Every exception is also counted by vector and INTEVT code, so a
 * storming source shows up. Counters wrap modulo 2^32: take deltas. */
#define SAMPLE_HIST     256
#define SAMPLE_INTEVTS  16
#define SAMPLE_KOTHER   5
static struct {
    sf_u32 total, in_block, exit_stub, stub_window, translating, helper;
    sf_u32 idle;                  /* the resume pc is a `sleep` in P1/P2 code: OEMIdle */
    sf_u32 user, user_ram;        /* TLB-mapped pcs; of those, pages outside the ROM image (RAM-loaded code) */
    sf_u32 kernel_other;          /* P1/P2 pcs outside the histogram, and P4 */
    sf_u32 hist_base, hist_shift; /* physical base and log2 of the bucket size */
    sf_u32 hist[SAMPLE_HIST];
    sf_u32 exc_general, exc_tlb, exc_irq;     /* every exception by vector (hypercalls excluded) */
    sf_u32 intevt_n;
    struct { sf_u32 code, count; } intevt[SAMPLE_INTEVTS];
    /* the stub-window ticks split by what they stand for (see sample_tick) */
    sf_u32 win_entry, win_hit, win_tail_lookup, win_tail_ldcsr, win_tail_helper, win_tramp;
    sf_u32 kother_last[SAMPLE_KOTHER], kother_idx;   /* the last kernel pcs outside the histogram */
} g_sample;
/* Set by stub_window: 1 lookup entry (before the mask), 2 hit tail, 3 full
 * tail (after C, or a helper's exit), 4 rte trampoline, 5 bank trampoline,
 * 6 helper prologue. The full tail is where a tick deferred by the mask of
 * translator C lands (the instruction after the re-enabling ldc), and
 * g_tail_from says whose C it was: 1 lookup/translation, 2 the ldc-sr
 * helper (= kernel code that ran with interrupts disabled), 3 fpu/fallback. */

static void sample_init(const dbt_rt_config *cfg)
{
    sf_u32 lo, hi, shift = 10;
    memset(&g_sample, 0, sizeof g_sample);
    if (cfg->rom_hi > cfg->rom_lo) { lo = cfg->rom_lo; hi = cfg->rom_hi; }
    else { lo = cfg->guest_ram_lo & 0x1FFFFFFFu; hi = cfg->guest_ram_hi & 0x1FFFFFFFu; }
    while (((hi - lo) >> shift) >= SAMPLE_HIST) shift++;
    g_sample.hist_base = lo; g_sample.hist_shift = shift;
}

static void sample_exception(const dbt_gctx *ctx)
{
    unsigned i;
    if (ctx->vec_off == 0x100) { g_sample.exc_general++; return; }
    if (ctx->vec_off == 0x400) { g_sample.exc_tlb++; return; }
    g_sample.exc_irq++;
    for (i = 0; i < g_sample.intevt_n; i++)
        if (g_sample.intevt[i].code == ctx->intevt) { g_sample.intevt[i].count++; return; }
    if (i < SAMPLE_INTEVTS) { g_sample.intevt[i].code = ctx->intevt; g_sample.intevt[i].count = 1; g_sample.intevt_n = i + 1; }
}

/* where: 1 exit stub, 2 inside a block, 3 stub window, 4 translating, 5 helper (as trace_exc) */
static void sample_tick(sf_u32 gpc, int where)
{
    sf_u32 phys;
    g_sample.total++;
    switch (where) {
    case 1: g_sample.exit_stub++; break;
    case 2: g_sample.in_block++; break;
    case 3:
        g_sample.stub_window++;
        switch (g_window_kind) {
        case 1: case 6: g_sample.win_entry++; break;
        case 2: g_sample.win_hit++; break;
        case 3:
            if (g_tail_from == 1) g_sample.win_tail_lookup++;
            else if (g_tail_from == 2) g_sample.win_tail_ldcsr++;
            else g_sample.win_tail_helper++;
            break;
        default: g_sample.win_tramp++; break;
        }
        break;
    case 4: g_sample.translating++; return;           /* the pc being translated says nothing about time */
    case 5: g_sample.helper++; break;
    default: break;
    }
    if (IS_USER(gpc) || (gpc >= 0xC0000000u && gpc < 0xE0000000u)) {
        /* TLB-mapped: user modules, and the kernel's P3 pages (the KPage's
         * interlocked APIs at 0xC010Bxxx showed up here); XIP code lands in
         * the ROM histogram either way */
        if (IS_USER(gpc)) g_sample.user++;
        phys = utlb_translate(gpc, g_asid);
    } else if (gpc < 0xC0000000u) {
        phys = gpc & 0x1FFFFFFFu;
        /* OEMIdle's `sleep`: the resume pc is the sleep itself or, past its
         * commit point, the instruction after it. The block was fetched from
         * there, so the reads are safe; LE code: swapped halfwords. */
        if (where == 2 && !(gpc & 1) &&
            (bswap16(*(volatile sf_u16 *)gpc) == 0x001Bu ||
             ((gpc & 0xFFFu) && bswap16(*(volatile sf_u16 *)(gpc - 2)) == 0x001Bu))) {
            g_sample.idle++;
            return;
        }
    } else { phys = PHYS_UNKNOWN; }
    phys -= g_sample.hist_base;
    if ((phys >> g_sample.hist_shift) < SAMPLE_HIST) g_sample.hist[phys >> g_sample.hist_shift]++;
    else if (IS_USER(gpc)) g_sample.user_ram++;
    else {
        g_sample.kernel_other++;
        g_sample.kother_last[g_sample.kother_idx++ % SAMPLE_KOTHER] = gpc;
    }
}

/* Machine state worth a look on the device (HC_GETSTAT 80-83) */
#define REG_CCR    (*(volatile sf_u32 *)0xFF00001Cu)
#define REG_FRQCR  (*(volatile sf_u32 *)0xA4150000u)
static sf_u32 rtc_seconds_of_day(void)
{
    /* SH7305 RTC, BCD counters (bsp rtc.c reads the same registers) */
    sf_u32 s = *(volatile sf_u8 *)0xA413FEC2u, m = *(volatile sf_u8 *)0xA413FEC4u, h = *(volatile sf_u8 *)0xA413FEC6u;
#define FROM_BCD(v) (((v) & 15u) + (((v) >> 4) & 15u) * 10u)
    return FROM_BCD(h) * 3600u + FROM_BCD(m) * 60u + FROM_BCD(s);
#undef FROM_BCD
}

static sf_u32 sample_stat(sf_u32 index)
{
    switch (index) {
    case 64: return g_sample.total;
    case 65: return g_sample.in_block;
    case 66: return g_sample.exit_stub;
    case 67: return g_sample.stub_window;
    case 68: return g_sample.translating;
    case 69: return g_sample.helper;
    case 70: return g_sample.idle;
    case 71: return g_sample.user;
    case 72: return g_sample.user_ram;
    case 73: return g_sample.kernel_other;
    case 74: return g_sample.hist_base;
    case 75: return g_sample.hist_shift;
    case 76: return SAMPLE_HIST;
    case 77: return g_sample.exc_general;
    case 78: return g_sample.exc_tlb;
    case 79: return g_sample.exc_irq;
    case 80: return REG_CCR;
    case 81: return REG_MMUCR;
    case 82: return REG_FRQCR;
    case 83: return rtc_seconds_of_day();
    case 84: return g_sample.intevt_n;
    case 117: return g_sample.win_entry;
    case 118: return g_sample.win_tail_lookup;
    case 119: return g_sample.win_tail_ldcsr;
    case 120: return g_sample.win_tail_helper;
    case 121: return g_sample.win_hit;
    case 122: return g_sample.win_tramp;
    default:
        if (index >= 85 && index < 85 + 2 * SAMPLE_INTEVTS) {
            unsigned k = (index - 85) / 2;
            return (index - 85) & 1 ? g_sample.intevt[k].count : g_sample.intevt[k].code;
        }
        if (index >= 123 && index < 123 + SAMPLE_KOTHER) return g_sample.kother_last[index - 123];
        if (index >= 128 && index < 128 + SAMPLE_HIST) return g_sample.hist[index - 128];
        return 0;
    }
}

static void print_samples(void)
{
    unsigned i, top[8], n = 0;
    if (!g_sample.total && !g_sample.exc_irq) return;
    dbt_puts("[sample] ticks="); dbt_puthex(g_sample.total);
    dbt_puts(" block="); dbt_puthex(g_sample.in_block);
    dbt_puts(" exit="); dbt_puthex(g_sample.exit_stub);
    dbt_puts(" window="); dbt_puthex(g_sample.stub_window);
    dbt_puts(" xlat="); dbt_puthex(g_sample.translating);
    dbt_puts(" helper="); dbt_puthex(g_sample.helper);
    dbt_puts(" idle="); dbt_puthex(g_sample.idle);
    dbt_puts(" user="); dbt_puthex(g_sample.user); dbt_puts("/"); dbt_puthex(g_sample.user_ram);
    dbt_puts(" kother="); dbt_puthex(g_sample.kernel_other);
    for (i = 0; i < SAMPLE_KOTHER && i < g_sample.kother_idx; i++) { dbt_puts(i ? "," : "@"); dbt_puthex(g_sample.kother_last[i]); }
    dbt_puts("\n[sample] window: entry="); dbt_puthex(g_sample.win_entry);
    dbt_puts(" hit="); dbt_puthex(g_sample.win_hit);
    dbt_puts(" after-lookup-C="); dbt_puthex(g_sample.win_tail_lookup);
    dbt_puts(" after-ldcsr="); dbt_puthex(g_sample.win_tail_ldcsr);
    dbt_puts(" after-helper="); dbt_puthex(g_sample.win_tail_helper);
    dbt_puts(" tramp="); dbt_puthex(g_sample.win_tramp);
    dbt_puts("\n[sample] exc gen="); dbt_puthex(g_sample.exc_general);
    dbt_puts(" tlb="); dbt_puthex(g_sample.exc_tlb);
    dbt_puts(" irq="); dbt_puthex(g_sample.exc_irq);
    for (i = 0; i < g_sample.intevt_n; i++) {
        dbt_puts(" "); dbt_puthex(g_sample.intevt[i].code); dbt_puts(":"); dbt_puthex(g_sample.intevt[i].count);
    }
    dbt_puts("\n[sample] ccr="); dbt_puthex(REG_CCR); dbt_puts(" mmucr="); dbt_puthex(REG_MMUCR);
    dbt_puts(" frqcr="); dbt_puthex(REG_FRQCR);
    /* the eight hottest buckets, physical and as the P1 address */
    for (i = 0; i < SAMPLE_HIST; i++) {
        unsigned j, k;
        if (!g_sample.hist[i]) continue;
        for (j = 0; j < n && g_sample.hist[top[j]] >= g_sample.hist[i]; j++) ;
        if (j >= 8) continue;
        for (k = (n < 8 ? n : 7); k > j; k--) top[k] = top[k - 1];
        top[j] = i;
        if (n < 8) n++;
    }
    dbt_puts("\n[sample] hot:");
    for (i = 0; i < n; i++) {
        sf_u32 pa = g_sample.hist_base + ((sf_u32)top[i] << g_sample.hist_shift);
        dbt_puts(" "); dbt_puthex(pa | 0x80000000u); dbt_puts("="); dbt_puthex(g_sample.hist[top[i]]);
    }
    dbt_puts(" (bucket "); dbt_puthex(1u << g_sample.hist_shift); dbt_puts(")\n");
}

static void print_stats(void)
{
    dbt_puts("[dbt] blocks="); dbt_puthex(dbt_stat.blocks);
    dbt_puts(" translations="); dbt_puthex(dbt_stat.translations);
    dbt_puts(" flushes="); dbt_puthex(dbt_stat.flushes);
    dbt_puts(" bank_evictions="); dbt_puthex(g_bank_evictions);
#ifdef DBT_IRQ_ENTRY_CACHE
    dbt_puts(" irq_entries="); dbt_puthex(g_irq_entry_hits);
#endif
#ifdef DBT_INTERIOR_ENTRY
    dbt_puts(" interior_entries="); dbt_puthex(g_interior_hits);
#ifdef DBT_INTERIOR_CACHE
    dbt_puts(" entry_cache_hits="); dbt_puthex(g_entry_cache_hits);
#endif
#endif
    dbt_puts(" lookups_c="); dbt_puthex(dbt_stat.lookups_c);
    dbt_puts(" exceptions="); dbt_puthex(dbt_stat.exceptions);
    dbt_puts(" fpu="); dbt_puthex(dbt_stat.fpu_ops);
    dbt_puts(" fpudis="); dbt_puthex(dbt_stat.fpu_disabled);
    dbt_puts(" hcflush="); dbt_puthex(dbt_stat.hc_flush); dbt_puts("/"); dbt_puthex(dbt_stat.hc_flush_all); dbt_puts(" romkept="); dbt_puthex(dbt_stat.rom_kept);
    dbt_puts(" ldcsr="); dbt_puthex(dbt_stat.ldc_sr_calls); dbt_puts(" fallback="); dbt_puthex(dbt_stat.fallback_calls); dbt_puts(" rte="); dbt_puthex(dbt_stat.rte_calls);
    dbt_puts(" chained="); dbt_puthex(dbt_stat.chained);
    dbt_puts(" evicted="); dbt_puthex(dbt_stat.hash_evicted);
    dbt_puts(" ic_miss_c="); dbt_puthex(dbt_stat.ic_misses_c);
    dbt_puts(" probe_faults="); dbt_puthex(dbt_stat.probe_faults); dbt_puts("/"); dbt_puthex(dbt_stat.helper_faults);
    dbt_puts(" synth="); dbt_puthex(dbt_stat.synth_exceptions);
    dbt_puts("\n[dbt] inval="); dbt_puthex(dbt_stat.invalidations);
    dbt_puts(" dev="); dbt_puthex(dbt_stat.dev_accesses);
    dbt_puts(" unchained="); dbt_puthex(dbt_stat.unchained);
    dbt_puts(" asid_unchanged="); dbt_puthex(dbt_stat.asid_unchanged);
    dbt_puts(" asid_switches="); dbt_puthex(dbt_stat.asid_switches);
    dbt_puts(" tlb_scan_skips="); dbt_puthex(dbt_stat.tlb_scan_skips);
    dbt_puts(" tlb_hooks="); dbt_puthex(dbt_stat.tlb_hooks);
    dbt_puts(" arena="); dbt_puthex(dbt_stat.arena_bytes);
    dbt_puts(" lcd_pixels="); dbt_puthex(dbt_stat.lcd_pixels);
    dbt_puts(" resumes="); dbt_puthex(dbt_stat.resumes);
    dbt_puts("/"); dbt_puthex(dbt_stat.resumes_committed);
    dbt_puts("\n");
    print_samples();
    print_trace();
    profile_print();
#ifdef DBT_CE_TIME_PROBE
    /* Diagnostic only: pointer location and dwCurMSec offset verified from
     * the frozen benchmark NK's OALTimerIntrHandler, not a platform ABI. */
    {
        sf_u32 nk = bswap32(*(volatile sf_u32 *)DBT_CE_TIME_PROBE);
        sf_u32 pa = (nk >= 0x80000000u && nk < 0xc0000000u) ? nk & 0x1fffffffu : utlb_translate(nk, g_asid);
        if (pa >= 0x0c100000u && pa < 0x0c8fff00u) {
            dbt_puts("[ce-time] nkglobal="); dbt_puthex(nk);
            dbt_puts(" msec="); dbt_puthex(bswap32(*(volatile sf_u32 *)(0x80000000u + pa + 0xc8)));
#ifdef DBT_PROFILE
            dbt_puts(" clock="); dbt_puthex(0xffffffffu - PROFILE_CLOCK);
#endif
            dbt_puts("\n");
        }
    }
#endif
}

/* HC_LCD_BLIT / HC_LCD_PUSH: n little-endian 16-bit pixels from guest memory to the panel's data port */
static void lcd_pixels(const sf_u8 *s, sf_u32 n, volatile sf_u16 *port)
{
    PROFILE_BEGIN;
    dbt_stat.lcd_pixels += n;
    if (((sf_u32)s & 3) == 0) {
        const sf_u32 *w = (const sf_u32 *)s;
        for (; n >= 2; n -= 2) {
            sf_u32 v = *w++;
            *port = (sf_u16)((v >> 24) | ((v >> 8) & 0xFF00u));
            *port = (sf_u16)(((v >> 8) & 0xFFu) | ((v & 0xFFu) << 8));
        }
        s = (const sf_u8 *)w;
    }
    for (; n; n--, s += 2) *port = (sf_u16)(s[0] | ((sf_u16)s[1] << 8));
    dbt_lcd_sync();
    PROFILE_END(PROF_LCD);
}

static int hypercall(dbt_gctx *ctx, sf_u32 nr)
{
    dbt_stat.hypercalls++;
    dbt_dbgring_drain();
    switch (nr) {
    case HC_EXIT:
        dbt_puts("[dbt] guest exit code "); dbt_puthex(ctx->r[4]); dbt_puts("\n");
        print_stats();
        dbt_halt("guest exited");
    case HC_PUTS: {
        const sf_u8 *p = (const sf_u8 *)ctx->r[4];      /* bytes: no endian issue */
        while (*p) dbt_putc(*p++);
        return RT_ACT_RESUME;
    }
    case HC_PUTC: dbt_putc((int)ctx->r[4]); return RT_ACT_RESUME;
    case HC_PUTHEX: dbt_puthex(ctx->r[4]); return RT_ACT_RESUME;
    case HC_FLUSH:                                          /* r4 = address, r5 = length (0,0: everything) */
        dbt_stat.hc_flush++;
        if (ctx->r[5] == 0) { dbt_stat.hc_flush_all++; invalidate_all_but_rom(); }
        else dbt_invalidate(ctx->r[4], ctx->r[4] + ctx->r[5]);
        return RT_ACT_RESUME;
    case HC_STATS: print_stats(); return RT_ACT_RESUME;
    case HC_BENCH: ctx->r[0] = dbt_bench_native(ctx->r[4], ctx->r[5]); return RT_ACT_RESUME;
    case HC_TLB_INVAL:
        if (ctx->r[5] == 0) invalidate_range(0, 0x80000000u, 1);
        else invalidate_range(ctx->r[4], ctx->r[4] + ctx->r[5], 1);
        return RT_ACT_RESUME;
    case HC_ASID: switch_asid(ctx->r[4] & 0xFF); return RT_ACT_RESUME;
    case HC_DEVRANGE:
        g_devrange_lo = ctx->r[4]; g_devrange_hi = ctx->r[5]; g_devrange_pending = 1;
        return RT_ACT_RESUME;
    case HC_LCD_BLIT:
        /* The display driver's push: each pixel is a little-endian 16-bit
         * value in guest memory, the panel wants it as one 16-bit bus write.
         * Translated code pays a swap sequence per store (and the memory load
         * before it); here it is a byte-pair per pixel. The source must be a
         * P1/P2 address (the driver passes the P2 alias of its physical frame
         * after writing the rows back), never a TLB-mapped one. */
        lcd_pixels((const sf_u8 *)ctx->r[4], ctx->r[5], (volatile sf_u16 *)ctx->r[6]);
        return RT_ACT_RESUME;
    case HC_LCD_PUSH: {
        /* The same with the DCS window set here, so nothing of ours (the
         * panel console) can land between the driver's window commands and
         * its pixels: r5 = first row, r6 = rows, r7 = width, the data port
         * and RS pin as in bsp/classpad/SRC/INC/classpad_lcd.h. */
        volatile sf_u16 *port = (volatile sf_u16 *)0xB4000000u;
        volatile sf_u8 *prdr = (volatile sf_u8 *)0xA405013Cu;
        sf_u32 top = ctx->r[5], rows = ctx->r[6], width = ctx->r[7], x1 = width - 1, y1 = top + rows - 1;
#define DCS_CMD(c) dbt_lcd_select(prdr, port, (c))
#define DCS_PARAM(v) dbt_lcd_param(port, (v))
        DCS_CMD(0x2A); DCS_PARAM(0); DCS_PARAM(0); DCS_PARAM(x1 >> 8); DCS_PARAM(x1 & 0xFF);
        DCS_CMD(0x2B); DCS_PARAM(top >> 8); DCS_PARAM(top & 0xFF); DCS_PARAM(y1 >> 8); DCS_PARAM(y1 & 0xFF);
        DCS_CMD(0x2C);
#undef DCS_CMD
#undef DCS_PARAM
        lcd_pixels((const sf_u8 *)ctx->r[4], rows * width, port);
        return RT_ACT_RESUME;
    }
    case HC_GETSTAT: {
        sf_u32 v = 0;
        switch (ctx->r[4]) {
        case 0: v = dbt_stat.translations; break;
        case 1: v = dbt_stat.blocks; break;
        case 2: v = dbt_stat.invalidations; break;
        case 3: v = dbt_stat.asid_switches; break;
        case 4: v = dbt_stat.ic_misses_c; break;
        case 5: v = dbt_stat.dbgring_bytes; break;
        case 6: v = dbt_stat.dev_accesses; break;
        case 7: v = dbt_stat.flushes; break;
        case 8: v = dbt_stat.fpu_disabled; break;
        case 9: v = g_env.dev_lo; break;
        case 10: v = g_env.dev_hi; break;
#ifdef DBT_INTERIOR_ENTRY
        case 17: v = g_interior_hits; break;
        case 18: v = 1; break;
#endif
#ifdef DBT_INTERIOR_CACHE
        case 19: v = 1; break;
        case 20: v = g_entry_cache_hits; break;
#endif
#ifdef DBT_INTERIOR_HASH
        case 21: v = 1; break;
        case 22: v = g_alias_hash_removed; break;
#endif
        case 13: v = dbt_stat.asid_unchanged; break;
        case 14: v = dbt_stat.tlb_scan_skips; break;
#ifdef DBT_IRQ_ENTRY_CACHE
        case 15: v = g_irq_entry_hits; break;
        case 16: v = 1; break;
#endif
        case 23: v = dbt_stat.exceptions; break;
        case 24: v = dbt_stat.hypercalls; break;
        case 25: v = dbt_stat.rte_calls; break;
        case 26: v = dbt_stat.ldc_sr_calls; break;
        case 27: v = dbt_stat.lookups_c; break;
        case 28: v = dbt_stat.tlb_hooks; break;
        case 29: v = g_bank_evictions; break;
        default: v = ctx->r[4] >= 64 ? sample_stat(ctx->r[4]) : profile_stat(ctx->r[4]); break;
        }
        ctx->r[0] = v;
        return RT_ACT_RESUME;
    }
    case HC_DUMP: {
        const volatile sf_u16 *p = (const volatile sf_u16 *)(ctx->r[4] & ~1u);
        sf_u32 i;
        for (i = 0; i < ctx->r[5]; i++) {
            if ((i & 7) == 0) { dbt_puts(i ? "\n[dbt] " : "[dbt] "); dbt_puthex((sf_u32)(p + i)); dbt_puts(":"); }
            dbt_puts(" "); dbt_puthex(p[i]);
        }
        dbt_puts("\n");
        return RT_ACT_RESUME;
    }
    }
    dbt_puts("[dbt] unknown hypercall "); dbt_puthex(nr); dbt_puts("\n");
    return RT_ACT_RESUME;
}

/* ------------------------------------------------------------ exceptions */

/* Guest resume pc for an exception at host offset `off` inside block b. */
/* Guest pc at the start of meta range idx (idx may be the sentinel). */
static sf_u32 meta_start_pc(const dbt_block *b, int idx, const dbt_gctx *ctx)
{
    dbt_imeta meta;
    const dbt_imeta *m = &meta;
    dbt_get_meta(b, idx, &meta);
    switch (m->kind) {
    case META_INSN: case META_LINEAR: return b->guest_pc + m->guest_off;
    case META_TARGET_CONST: return m->target;
    case META_TARGET_X0:    return ctx->slots[SLOT_X0];
    case META_TARGET_REG:   return ctx->r[m->fix_reg] + m->target;   /* register not written in the range, never spilled; + PC offset for braf/bsrf */
    case META_TARGET_PR:    return ctx->pr;
    case META_COND:         return (ctx->ssr & 1) ? m->target : m->fallthrough;
    case META_COND_REG:     return (ctx->r[m->fix_reg] & 1) ? m->target : m->fallthrough;   /* before the undo */
    default:
        dbt_halt("exception while a guest rte is pending");
    }
}

/* Guest pc to resume at for an exception at host offset `off` of block b
 * (off < code_len). *committed is set when the range's memory operation has
 * already executed: the guest instruction then completes in software
 * (apply_resume_state) and execution resumes at the following range. */
static sf_u32 resume_pc(const dbt_block *b, sf_u32 off, const dbt_gctx *ctx, int *idx_out, int *committed)
{
    int idx = dbt_block_find(b, off);
    dbt_imeta meta;
    const dbt_imeta *m = &meta;
    *idx_out = idx;
    *committed = 0;
    if (idx < 0) dbt_halt("resume_pc outside translated code");
    dbt_get_meta(b, idx, &meta);
    if (m->kind == META_INSN && m->commit_rel != 0xFF && off > (sf_u32)m->host_off + m->commit_rel) {
        /* resume at the next guest instruction: skip further ranges of this
         * one (the device-aware slow path has a range of its own) */
        int j = idx + 1;
        while (j < b->nmeta) {
            dbt_imeta next; dbt_get_meta(b, j, &next);
            if (next.kind != META_INSN || next.guest_off != m->guest_off) break;
            ++j;
        }
        *committed = 1;
        return meta_start_pc(b, j, ctx);
    }
    return meta_start_pc(b, idx, ctx) +
        (m->kind == META_LINEAR ? off - m->host_off : 0);
}

#ifdef DBT_IRQ_ENTRY_CACHE
/* Interior entries are safe only at a guest instruction boundary with no
 * borrowed register or pending scratch save. Never publish these aliases
 * in the normal hash, whose invalidation keys are block starts. */
static struct { sf_u32 gpc, host, asid, generation, index; } g_irq_entry[16];
static unsigned g_irq_entry_next;
static void remember_irq_entry(dbt_block *b, sf_u32 gpc)
{
    unsigned i, index;
    if (!b || b->borrow >= 0 || gpc <= b->guest_pc || gpc >= b->guest_pc + b->guest_len) return;
    index = (unsigned)(b - g_blocks);
    if (g_binfo[index].dead) return;
    for (i = 0; i < b->nmeta; ++i) {
        dbt_imeta m; sf_u32 off;
        dbt_spill spill;
        dbt_get_meta(b, i, &m);
        if (m.kind == META_INSN && b->guest_pc + m.guest_off == gpc) off = m.host_off;
        else if (m.kind == META_LINEAR) {
            dbt_imeta next; dbt_get_meta(b, i + 1, &next);
            if (gpc < b->guest_pc + m.guest_off) continue;
            off = m.host_off + gpc - b->guest_pc - m.guest_off;
            if (off >= next.host_off) continue;
        } else continue;
        if (dbt_block_pending_spills(b, (int)i, off, &spill, 1)) return;
        {
            unsigned slot = g_irq_entry_next++ & 15;
            g_irq_entry[slot].gpc = gpc; g_irq_entry[slot].host = b->host_base + off;
            g_irq_entry[slot].asid = g_asid; g_irq_entry[slot].generation = dbt_stat.flushes;
            g_irq_entry[slot].index = index;
        }
        return;
    }
}
static sf_u32 find_irq_entry(sf_u32 gpc)
{
    unsigned n;
    for (n = 0; n < 16; ++n) {
        unsigned slot = (g_irq_entry_next - 1 - n) & 15;
        if (g_irq_entry[slot].host && g_irq_entry[slot].gpc == gpc &&
            g_irq_entry[slot].asid == g_asid && g_irq_entry[slot].generation == dbt_stat.flushes &&
            !g_binfo[g_irq_entry[slot].index].dead) {
            ++g_irq_entry_hits;
            return g_irq_entry[slot].host;
        }
    }
    return 0;
}
#endif

static sf_u32 op_swapb(sf_u32 x) { return (x & 0xFFFF0000u) | ((x & 0xFF) << 8) | ((x >> 8) & 0xFF); }
static sf_u32 op_swapw(sf_u32 x) { return (x << 16) | (x >> 16); }
static sf_u32 op_extsw(sf_u32 x) { return (x & 0x8000) ? (x | 0xFFFF0000u) : (x & 0xFFFFu); }

/* Undo the scratch spills still open at `off`, and after a committed memory
 * op finish the instruction's emitted tail that was interrupted: the byte
 * swap of the loaded value (swap.b, swap.w, swap.b / swap.b, exts.w) and,
 * for control-register loads, the ldc/lds itself. `k` tail instructions
 * have already run, so only the remaining ones are applied. The register is
 * read before the undo since control-register loads use a scratch. */
static void apply_resume_state(const dbt_block *b, int idx, sf_u32 off, int committed, dbt_gctx *ctx)
{
    dbt_imeta meta;
    const dbt_imeta *m = &meta;
    dbt_get_meta(b, idx, &meta);
    dbt_spill sp[8];
    int n, i, fix = committed ? m->fix_kind : FIX_NONE;
    sf_u32 v = 0, k = 0;
    if (fix != FIX_NONE) {
        v = ctx->r[m->fix_reg];
        k = (off - ((sf_u32)m->host_off + m->commit_rel)) / 2 - 1;
        if (fix == FIX_SWAP16) {
            if (k < 1) v = op_swapb(v);
            if (k < 2) v = op_extsw(v);
        } else {
            if (k < 1) v = op_swapb(v);
            if (k < 2) v = op_swapw(v);
            if (k < 3) v = op_swapb(v);
            if (fix >= FIX_CR_BASE && k >= 4) fix = FIX_NONE;   /* the ldc/lds already ran */
        }
    }
    n = dbt_block_pending_spills(b, idx, off, sp, 8);
    for (i = 0; i < n; i++) {
        if (sp[i].gpr == DBT_SPILL_T)
            ctx->ssr = (ctx->ssr & ~1u) | (ctx->slots[sp[i].slot] & 1u);
        else ctx->r[sp[i].gpr] = ctx->slots[sp[i].slot];
    }
    switch (fix) {
    case FIX_NONE: break;
    case FIX_SWAP16: case FIX_SWAP32: ctx->r[m->fix_reg] = v; break;
    case FIX_CR_BASE + CR_MACH: ctx->mach = v; break;
    case FIX_CR_BASE + CR_MACL: ctx->macl = v; break;
    case FIX_CR_BASE + CR_PR:   ctx->pr = v; break;
    case FIX_CR_BASE + CR_GBR:  ctx->gbr = v; break;
    case FIX_CR_BASE + CR_VBR:  dbt_shadow_vbr = v; break;
    default:
        /* ssr/spc/sr/rN_bank/sgr/dbr loads: only reachable with BL=1, where
         * no interrupt can land after the load */
        dbt_halt("exception after a committed privileged load");
    }
    dbt_stat.resumes += committed ? 0 : 1;
    dbt_stat.resumes_committed += committed ? 1 : 0;
}

/* Debug trace of the last exceptions and where they resumed; printed with
 * the stats when built with -DDBT_TRACE_EXC. */
#ifdef DBT_TRACE_EXC
#define TRACE_N 16
static struct { sf_u32 host, blk, off, kind, committed, resume, vec, where; } g_trace[TRACE_N];
static sf_u32 g_trace_n;
static void trace_exc(sf_u32 host, sf_u32 blk, sf_u32 off, sf_u32 kind, sf_u32 committed, sf_u32 resume, sf_u32 vec, sf_u32 where)
{
    unsigned i = g_trace_n++ % TRACE_N;
    dbt_puts("[exc] vec="); dbt_puthex(vec); dbt_puts(" blk="); dbt_puthex(blk); dbt_puts("+"); dbt_puthex(off);
    dbt_puts(" kind="); dbt_puthex(kind); dbt_puts(" resume="); dbt_puthex(resume); dbt_puts(" where="); dbt_puthex(where);
    if (where >= 3) { dbt_puts(" host="); dbt_puthex(host); }   /* 3: a stub window, 4: the translator's C code (its pc, for a profile) */
    dbt_puts("\n");
    g_trace[i].host = host; g_trace[i].blk = blk; g_trace[i].off = off; g_trace[i].kind = kind;
    g_trace[i].committed = committed; g_trace[i].resume = resume; g_trace[i].vec = vec; g_trace[i].where = where;
}
static void print_trace(void)
{
    unsigned i, n = g_trace_n < TRACE_N ? g_trace_n : TRACE_N;
    for (i = 0; i < n; i++) {
        unsigned k = (g_trace_n - n + i) % TRACE_N;
        dbt_puts("[dbt] exc vec="); dbt_puthex(g_trace[k].vec);
        dbt_puts(" where="); dbt_puthex(g_trace[k].where);
        dbt_puts(" host="); dbt_puthex(g_trace[k].host);
        dbt_puts(" blk="); dbt_puthex(g_trace[k].blk);
        dbt_puts(" off="); dbt_puthex(g_trace[k].off);
        dbt_puts(" kind="); dbt_puthex(g_trace[k].kind);
        dbt_puts(" commit="); dbt_puthex(g_trace[k].committed);
        dbt_puts(" resume="); dbt_puthex(g_trace[k].resume);
        dbt_puts("\n");
    }
}
#else
#define trace_exc(host, blk, off, kind, committed, resume, vec, where) ((void)0)
static void print_trace(void) {}
#endif

/* Which exit stub (link index) contains host offset `off` >= code_len. */
static int exit_stub_of(const dbt_block *b, sf_u32 off)
{
    int k;
    for (k = b->nlinks - 1; k >= 0; k--)
        if (off >= (sf_u32)b->links[k].stub_off) return k;
    return -1;
}

/* Guest pc for a host address that must be a range start (block entry or
 * instruction boundary): used when a stub is about to jump to r1. */
static sf_u32 guest_pc_of_host(sf_u32 host, const dbt_gctx *ctx)
{
    dbt_block *b = block_of_host(host);
    int idx, committed;
    if (!b) dbt_halt("stub target not in a block");
    return resume_pc(b, host - b->host_base, ctx, &idx, &committed);
}

#define IN(spc, a, b) ((spc) >= (sf_u32)(a) && (spc) < (sf_u32)(b))
#define AFTER(spc, l)  ((spc) > (sf_u32)(l))
#define UNDO(ctx, reg, slot) ((ctx)->r[reg] = (ctx)->slots[slot])

/* Exceptions whose host pc lies in the runtime stubs. Returns 1 and sets
 * *guest_pc (after applying register undos) when handled. */
static int stub_window(dbt_gctx *ctx, sf_u32 *guest_pc)
{
    sf_u32 spc = ctx->host_spc;
    if (!IN(spc, dbt_stubs_start, dbt_stubs_end)) return 0;

    /* lookup stubs before masking: r0/r1/r2 saved in order to Y0/Y1/A0; resume = X0 */
    if (IN(spc, dbt_lookup, dbt_hit_a)) {
        g_window_kind = 1;
        if (IN(spc, dbt_lookup, dbt_lookup_link)) {
            if (AFTER(spc, dbt_lk_s1)) UNDO(ctx, 0, SLOT_Y0);
            if (AFTER(spc, dbt_lk_s2)) UNDO(ctx, 1, SLOT_Y1);
            if (AFTER(spc, dbt_lk_s3)) UNDO(ctx, 2, SLOT_A0);
            if (AFTER(spc, dbt_lk_ts)) ctx->ssr = (ctx->ssr & ~1u) | (dbt_lookup_sr & 1u);
        } else if (IN(spc, dbt_lookup_link, dbt_lookup_ic)) {
            if (AFTER(spc, dbt_lkl_s1)) UNDO(ctx, 0, SLOT_Y0);
            if (AFTER(spc, dbt_lkl_s2)) UNDO(ctx, 1, SLOT_Y1);
            if (AFTER(spc, dbt_lkl_s3)) UNDO(ctx, 2, SLOT_A0);
            if (AFTER(spc, dbt_lkl_ts)) ctx->ssr = (ctx->ssr & ~1u) | (dbt_lookup_sr & 1u);
        } else {
            if (AFTER(spc, dbt_lki_s1)) UNDO(ctx, 0, SLOT_Y0);
            if (AFTER(spc, dbt_lki_s2)) UNDO(ctx, 1, SLOT_Y1);
            if (AFTER(spc, dbt_lki_s3)) UNDO(ctx, 2, SLOT_A0);
            if (AFTER(spc, dbt_lki_ts)) ctx->ssr = (ctx->ssr & ~1u) | (dbt_lookup_sr & 1u);
        }
        *guest_pc = ctx->slots[SLOT_X0];
        return 1;
    }
    /* hit tail: r1 = host target */
    if (IN(spc, dbt_hit_a, dbt_hit_end)) {
        sf_u32 target = ctx->r[1];
        g_window_kind = 2;
        if (!AFTER(spc, dbt_hit_a)) UNDO(ctx, 0, SLOT_Y0);
        if (!AFTER(spc, dbt_hit_b)) UNDO(ctx, 2, SLOT_A0);
        UNDO(ctx, 1, SLOT_Y1);
        *guest_pc = guest_pc_of_host(target, ctx);
        return 1;
    }
    /* full tail (miss path and helper exits): r1 = host target, r3 in X1 */
    if (IN(spc, dbt_full_m1, dbt_full_end)) {
        sf_u32 target = ctx->r[1];
        g_window_kind = 3;
        if (!AFTER(spc, dbt_full_m1)) UNDO(ctx, 3, SLOT_X1);
        if (!AFTER(spc, dbt_full_m2)) UNDO(ctx, 0, SLOT_Y0);
        if (!AFTER(spc, dbt_full_m3)) UNDO(ctx, 2, SLOT_A0);
        UNDO(ctx, 1, SLOT_Y1);
        *guest_pc = guest_pc_of_host(target, ctx);
        return 1;
    }
    /* rte trampoline (target not translated at the rte): resume = the target */
    if (IN(spc, dbt_rte_tramp, dbt_rt_masked)) {
        g_window_kind = 4;
        if (AFTER(spc, dbt_rt_s1)) UNDO(ctx, 0, SLOT_Y0);
        if (AFTER(spc, dbt_rt_s2)) UNDO(ctx, 1, SLOT_Y1);
        if (AFTER(spc, dbt_rt_s3)) UNDO(ctx, 2, SLOT_A0);
        *guest_pc = dbt_ctx_rt.sw_target;
        return 1;
    }
    /* bank-switch trampoline: resume = the guest instruction after the ldc */
    if (IN(spc, dbt_bank_tramp, dbt_bt_end)) {
        g_window_kind = 5;
        if (AFTER(spc, dbt_bt_s1)) UNDO(ctx, 1, SLOT_X0);
        *guest_pc = guest_pc_of_host(dbt_ctx_rt.sw_target, ctx);
        return 1;
    }
    /* helper prologues before masking: resume = the calling instruction,
     * found as the range containing (X1 - 2) */
    {
        struct { char *start, *s1, *s2, *s3, *masked; } h[3] = {
            { dbt_fpu_stub, dbt_hfpu_s1, dbt_hfpu_s2, dbt_hfpu_s3, dbt_hfpu_masked },
            { dbt_fallback_stub, dbt_hfb_s1, dbt_hfb_s2, dbt_hfb_s3, dbt_hfb_masked },
            { dbt_ldc_sr_stub, dbt_hsr_s1, dbt_hsr_s2, dbt_hsr_s3, dbt_hsr_masked },
        };
        int i;
        for (i = 0; i < 3; i++) {
            if (IN(spc, h[i].start, h[i].masked)) {
                sf_u32 ret = ctx->slots[SLOT_X1];
                g_window_kind = 6;
                dbt_block *b = block_of_host(ret - 2);
                int idx, committed;
                sf_u32 off;
                if (AFTER(spc, h[i].s1)) UNDO(ctx, 0, SLOT_Y0);
                if (AFTER(spc, h[i].s2)) UNDO(ctx, 1, SLOT_Y1);
                if (AFTER(spc, h[i].s3)) UNDO(ctx, 2, SLOT_A0);
                if (!b) dbt_halt("helper return address not in a block");
                off = ret - 2 - b->host_base;
                *guest_pc = resume_pc(b, off, ctx, &idx, &committed);
                /* The registers are as of the return address, not of the
                 * jmp's delay slot: that `sts Y0,rT` has run, so the
                 * scratch's spill record (open through its own offset) is
                 * closed, and the prologue has since put r0 into Y0.
                 * Undoing it at X1 - 2 loaded r0 into the scratch: with
                 * rT = r1 in KCall's epilogue, r1 = the wait's return value
                 * at `mov.b @r1,r0` (TEA 0 or 102) once in a few GWES boots
                 * (2026-11-04). The re-park of the borrowed register at the
                 * return address has not run either: its record starts
                 * there and is not pending at off + 2. */
                apply_resume_state(b, idx, off + 2, committed, ctx);
                return 1;
            }
        }
    }
    dbt_puts("[dbt] exception in stub at "); dbt_puthex(spc); dbt_puts("\n");
    dbt_halt("exception in a non-resumable stub region");
}

/* The guest registers of an exception raised inside the translator (stub
 * context): the stub saved them in dbt_ctx_rt before calling C. */
static void take_stub_context(dbt_gctx *ctx)
{
    int i;
    for (i = 0; i < 16; i++) ctx->r[i] = dbt_ctx_rt.r[i];
    ctx->sr = dbt_ctx_rt.sr; ctx->pr = dbt_ctx_rt.pr; ctx->gbr = dbt_ctx_rt.gbr;
    ctx->mach = dbt_ctx_rt.mach; ctx->macl = dbt_ctx_rt.macl;
    ctx->ssr = dbt_ctx_rt.sr;
}

/* The stub saves bank 0 into ctx->r[0..7] and the current bank (1) into
 * ctx->b1, by position. C code wants ctx->r to be the registers the guest
 * was using: swap the two when the guest itself ran in bank 1 (SSR.RB=1),
 * and swap back before the stub restores by position again. The CE kernel
 * never takes an exception in bank 1 (it runs there with BL=1), so this is
 * for completeness. */
static void bank_normalize(dbt_gctx *ctx)
{
    int i;
    if (ctx->ssr & SR_RB)
        for (i = 0; i < 8; i++) { sf_u32 t = ctx->r[i]; ctx->r[i] = ctx->b1[i]; ctx->b1[i] = t; }
}

static int exc_c_body(dbt_gctx *ctx);

int dbt_exc_c(dbt_gctx *ctx)
{
    int act;
    PROFILE_BEGIN;
    bank_normalize(ctx);
    act = exc_c_body(ctx);
    bank_normalize(ctx);
    PROFILE_END(PROF_EXC);
    return act;
}

static int exc_c_body(dbt_gctx *ctx)
{
    dbt_block *b = 0;                   /* set on the in-block paths only */
    sf_u32 guest_pc, handler;
    int act, where = 0;                 /* where as trace_exc: 1 exit stub, 2 block, 3 stub window, 4 translating, 5 helper */
    dbt_stat.exceptions++;

    if (ctx->vec_off == 0x100 && ctx->expevt == 0x160) {
        sf_u32 nr = (ctx->tra >> 2) & 0xFF;
        if (nr == HC_SYNTH) {
            /* the translator asks for an exception at guest pc r4 with EXPEVT r5
             * (stub context: the guest registers are in dbt_ctx_rt) */
            sf_u32 expevt = ctx->r[5];
            guest_pc = ctx->r[4];
            take_stub_context(ctx);
            REG_EXPEVT = expevt;
            REG_TEA = guest_pc;
            ctx->expevt = expevt; ctx->tea = guest_pc;
            ctx->vec_off = 0x100;
            goto deliver;
        }
        if (nr >= 0xF0) return hypercall(ctx, nr);
    }
    sample_exception(ctx);

    if (stub_window(ctx, &guest_pc)) {
        where = 3;
        trace_exc(ctx->host_spc, 0, 0, 0, 0, guest_pc, ctx->vec_off, 3);
    } else {
        b = block_of_host(ctx->host_spc);
        if (!b) {
            if (g_translating) {
                /* code probe faulted while translating (TLB miss or address
                 * error, whichever vector): the guest state is the one saved
                 * by the stub that called the translator; deliver the fault at
                 * the guest pc being translated */
                guest_pc = g_translating_pc;
                g_translating = 0;
                take_stub_context(ctx);
                dbt_stat.probe_faults++;
                where = 4;
                trace_exc(ctx->host_spc, 0, 0, 0, 0, guest_pc, ctx->vec_off, 4);   /* 4: in the translator's C code (host = its pc) */
            } else if (g_helper_ret) {
                /* a helper's guest memory access faulted (TLB miss on a page
                 * the kernel has not mapped yet, address error): deliver it
                 * at the instruction the helper serves, as the hardware
                 * would; the stub context is exact, the helper reads before
                 * it writes back */
                int insn;
                guest_pc = helper_guest_pc(g_helper_ret, &insn);
                g_helper_ret = 0;
                take_stub_context(ctx);
                dbt_stat.helper_faults++;
                where = 5;
                trace_exc(ctx->host_spc, 0, 0, 0, 0, guest_pc, ctx->vec_off, 5);   /* 5: in a helper's C code */
            } else {
                dbt_puts("[dbt] exception outside translated code: spc="); dbt_puthex(ctx->host_spc);
                dbt_puts(" expevt="); dbt_puthex(ctx->expevt); dbt_puts(" vec="); dbt_puthex(ctx->vec_off);
                dbt_puts(" tea="); dbt_puthex(ctx->tea); dbt_puts("\n");
                dbt_halt("unmapped exception");
            }
        } else {
            sf_u32 off = ctx->host_spc - b->host_base;
            if (off >= (sf_u32)b->code_len) {
                /* exit stub of link k (runs once per link, before it is patched):
                 * r1 was saved to Y0 by its first instruction; resume = the link target */
                int k = exit_stub_of(b, off);
                if (k < 0) dbt_halt("exception in a block's literal pool");
                if (off > (sf_u32)b->links[k].stub_off) ctx->r[1] = ctx->slots[SLOT_Y0];
                guest_pc = b->links[k].guest_target;
                where = 1;
                trace_exc(ctx->host_spc, b->guest_pc, off, 0xEE, 0, guest_pc, ctx->vec_off, 1);
            } else {
                int idx, committed;
                /* the pc first: META_COND_REG reads the T value a slot saved
                 * in a scratch, which the undo below takes back. The other
                 * register the pc can come from, META_TARGET_REG's, is never
                 * a scratch of its slot (translate.c slot_avoid). */
                guest_pc = resume_pc(b, off, ctx, &idx, &committed);
                apply_resume_state(b, idx, off, committed, ctx);
                where = 2;
                trace_exc(ctx->host_spc, b->guest_pc, off, dbt_meta_kind(b, idx), (sf_u32)committed, guest_pc, ctx->vec_off, 2);
                if (ctx->vec_off == 0x100 && (ctx->expevt == 0x180 || ctx->expevt == 0x1A0)) {
                    /* an illegal instruction inside translated code is almost
                     * always the translator's own bug: say where */
                    dbt_puts("[dbt] illegal instruction in translated code: host="); dbt_puthex(ctx->host_spc);
                    dbt_puts(" word="); dbt_puthex(*(volatile sf_u16 *)ctx->host_spc);
                    dbt_puts(" block="); dbt_puthex(b->guest_pc);
                    dbt_puts(" off="); dbt_puthex(off);
                    dbt_puts(" guest="); dbt_puthex(guest_pc); dbt_puts("\n");
                }
            }
        }
    }

deliver:
    if (ctx->vec_off == 0x600 && ctx->intevt == 0x400) sample_tick(guest_pc, where);
#ifdef DBT_IRQ_ENTRY_CACHE
    if (ctx->vec_off == 0x600) remember_irq_entry(b, guest_pc);
#endif
#ifdef DBT_PROFILE_PC
    if (ctx->vec_off == 0x600 && ctx->intevt == 0x400) {
        static unsigned sample;
        if ((++sample & 63u) == 0) {
            sf_u32 phys = utlb_translate(guest_pc, g_asid);
            if (!IS_USER(guest_pc) && guest_pc < 0xC0000000u) phys = guest_pc & 0x1FFFFFFFu;
            dbt_puts("[sample] gpc="); dbt_puthex(guest_pc);
            dbt_puts(" phys="); dbt_puthex(phys);
            dbt_puts(" host="); dbt_puthex(ctx->host_spc); dbt_puts("\n");
        }
    }
#endif
#ifdef DBT_IRQPIN_GUARD
    if (ctx->vec_off == 0x600) dbt_irqpin_guard(ctx->intevt);   /* device: IRQ pins the OAL does not know (rt/irqpins.c) */
#endif
    /* A data fault on the null page is almost always a wrong register in
     * translated code; the first few are described (host code and guest
     * registers) before the guest handles them. */
    if (ctx->tea < 0x1000u && (ctx->vec_off == 0x400 || ctx->vec_off == 0x100) &&
        (ctx->expevt == 0x040 || ctx->expevt == 0x060 || ctx->expevt == 0x0E0 || ctx->expevt == 0x100)) {
        static int shown;
        if (shown < 4) {
            int i;
            shown++;
            dbt_puts("[dbt] null-page fault: expevt="); dbt_puthex(ctx->expevt); dbt_puts(" tea="); dbt_puthex(ctx->tea);
            dbt_puts(" host="); dbt_puthex(ctx->host_spc); dbt_puts(" block="); dbt_puthex(b ? b->guest_pc : 0);
            dbt_puts(" guest="); dbt_puthex(guest_pc); dbt_puts("\n[dbt]  code:");
            for (i = -8; i < 8; i++) {
                sf_u32 a = ctx->host_spc + (sf_u32)(i * 2);
                dbt_puts(i == 0 ? " >" : " "); dbt_puthex(*(volatile sf_u16 *)a);
            }
            dbt_puts("\n[dbt]  regs:");
            for (i = 0; i < 16; i++) { dbt_puts(" "); dbt_puthex(ctx->r[i]); }
            dbt_puts("\n[dbt]  slots A0 X0 X1 Y0 Y1:");
            for (i = 0; i < SLOT_COUNT; i++) { dbt_puts(" "); dbt_puthex(ctx->slots[i]); }
            dbt_puts("\n");
        }
    }
    /* deliver to the guest handler */
    ctx->new_spc = guest_pc;
    ctx->new_ssr = (ctx->ssr & ~SR_MD) | (dbt_shadow_md & SR_MD);
    dbt_shadow_md = SR_MD;                          /* guest is now in privileged handler mode */
    handler = dbt_shadow_vbr + ctx->vec_off;
    g_exc_depth++;
    ctx->jump_to = dbt_resolve(handler, 0);
    g_exc_depth--;
    act = RT_ACT_JUMP;
    return act;
}

sf_u32 dbt_rte_c(sf_u32 guest_spc, sf_u32 *ssr)
{
    PROFILE_BEGIN;
    dbt_stat.rte_calls++;
#ifdef DBT_PROFILE
    if ((dbt_stat.rte_calls & 4095u) == 0) print_stats();
#endif
    sf_u32 host;
    note_asid();                       /* a context switch returns to the new thread through rte */
    dbt_dbgring_drain();
    host = hash_find(guest_spc);
#ifdef DBT_IRQ_ENTRY_CACHE
    if (!host) host = find_irq_entry(guest_spc);
#endif
#ifdef DBT_INTERIOR_CACHE
    if (!host) host = find_cached_entry(guest_spc);
#endif
    if (!host) {
        dbt_block *b = find_block_by_gpc(guest_spc);
        if (b) { dbt_stat.hash_evicted++; hash_insert(guest_spc, b->host_base); host = b->host_base; }
    }
#ifdef DBT_INTERIOR_ENTRY
    if (!host) host = find_interior_entry(guest_spc);
#endif
    if (!host) {
        /* Not translated yet. Translating here would probe the target with
         * the current SR: the CE kernel dispatches threads with BL=1, and a
         * TLB miss on a mapped target would then be a reset. The real CPU
         * fetches the target after the rte, so do the same: rte into the
         * trampoline, which looks the target up in the new context. */
        dbt_ctx_rt.sw_target = guest_spc;
        host = (sf_u32)dbt_rte_tramp;
    }
    dbt_shadow_md = *ssr & SR_MD;
    *ssr = (*ssr | SR_MD | SR_DSP);
    PROFILE_END(PROF_RTE);
    return host;
}

int dbt_ldc_sr_c(dbt_gctx *ctx, sf_u32 newsr)
{
    dbt_stat.ldc_sr_calls++;
    g_tail_from = 2;
    /* ctx->sr is the SR at the helper's entry (SAVE_CTX_RT): compare banks.
     * When RB changes the stub returns through rte, since after the switch
     * no register of either bank may be touched (the old bank keeps the
     * guest's r0-r7, the new bank is live state of the other context). */
    int flip = ((ctx->sr ^ newsr) & SR_RB) != 0;
    note_asid();
    dbt_shadow_md = newsr & SR_MD;
    ctx->sr = newsr | SR_MD | SR_DSP;
    if (flip) {
        /* still the guest's values: nothing has trapped since the helper was entered */
        __asm__ volatile ("stc spc, %0" : "=r"(ctx->sw_spc));
        __asm__ volatile ("stc ssr, %0" : "=r"(ctx->sw_ssr));
    }
    return flip;
}

/* -------------------------------------------------------------- helpers */

void dbt_fpu_c(dbt_gctx *ctx, sf_u32 arg)
{
    fpu_mem mem;
    int r;
    g_tail_from = 3;
    sf_u32 opcode = arg & 0xFFFFu;
    g_helper_ret = helper_return_address();
    if (ctx->sr & SR_FD) {
        /* SR.FD: the FPU is disabled. The CE kernel runs every thread with
         * FD=1 and gives it the FPU on this exception (lazy context switch,
         * SwitchFPUOwner): general FPU disable 0x800, or 0x820 when the
         * instruction sits in a delay slot (bit 16 of the argument, set by
         * the translator), with SPC = the instruction, or the branch whose
         * slot it is: the branch is re-executed. The guest registers are
         * exact in dbt_ctx_rt (the helper stub saved them before anything
         * ran); the call site's instruction range names the guest pc. X1
         * still holds the return address at this point. */
        int insn;
        sf_u32 gpc = helper_guest_pc(g_helper_ret, &insn);
        if (!insn) dbt_halt("fpu helper outside an instruction range");
        dbt_stat.fpu_disabled++;
        g_helper_ret = 0;
        deliver_synthetic(gpc, (arg & 0x10000u) ? 0x820 : 0x800);
    }
    mem.read32 = fpu_read32; mem.write32 = fpu_write32; mem.fetch16 = 0; mem.ctx = 0;
    dbt_stat.fpu_ops++;
    r = fpuemu_exec(&g_fpu, ctx->r, &ctx->sr, opcode, &mem);
    g_helper_ret = 0;
    if (r != FPU_OK) {
        dbt_puts("[dbt] fpu op "); dbt_puthex(opcode); dbt_puts(" status "); dbt_puthex((sf_u32)r); dbt_puts("\n");
        if (r != FPU_EXCEPTION) dbt_halt("fpu emulation failed");
        /* TODO: raise the guest FPU exception (vector 0x100, EXPEVT 0x800/0x820) */
    }
}

/* mac.w @rm+,@rn+ and mac.l @rm+,@rn+ with saturation per SR.S ignored (S=0). */
/* A 32-bit load/store whose address turned out to be in P4 (device-aware
 * blocks, docs/05): performed unswapped, so the register sees the value the
 * little-endian program meant. Writes that change the address space or the
 * TLB are observed here, which is what the kernel hypercalls FA/F9 would
 * have told us. */
static void dev_mem(dbt_gctx *ctx, const sh_insn *d)
{
    sf_u32 ea = ctx->r[d->base], v;
    if (d->am == AM_DISP) ea += (sf_u32)d->disp;
    else if (d->am == AM_R0IDX) ea += ctx->r[0];
    else if (d->am == AM_PREDEC) ea -= 4;
    dbt_stat.dev_accesses++;
    if (d->is_store) {
        if (d->am == AM_PREDEC) ctx->r[d->base] = ea;       /* the decremented value is what gets stored */
        v = ctx->r[d->data];
        *(volatile sf_u32 *)ea = v;
        if (ea == 0xFF000000u) switch_asid(v & 0xFF);                               /* PTEH: ASID */
        else if (ea == 0xFF000010u && (v & 4)) invalidate_range(0, 0x80000000u, 1); /* MMUCR.TI */
    } else {
        v = *(volatile sf_u32 *)ea;
        if (d->am == AM_POSTINC) ctx->r[d->base] += 4;
        ctx->r[d->data] = v;                                /* after the increment: @Rm+ with m == n keeps the load */
    }
}

void dbt_fallback_c(dbt_gctx *ctx, sf_u32 opcode)
{
    dbt_stat.fallback_calls++;
    g_tail_from = 3;
    int n = (opcode >> 8) & 0xF, m = (opcode >> 4) & 0xF;
    sf_u64 acc;
    if (opcode == 0x0038) { tlb_hook(); return; }           /* ldtlb */
    g_helper_ret = helper_return_address();
    if ((opcode & 0xF00F) != 0x000F && (opcode & 0xF00F) != 0x400F) {
        sh_insn d;
        sh4_decode(opcode, &d);
        if (d.kind == K_MEM) { dev_mem(ctx, &d); g_helper_ret = 0; return; }
    }
    acc = ((sf_u64)ctx->mach << 32) | ctx->macl;
    if ((opcode & 0xF00F) == 0x000F) {                       /* mac.l */
        sf_u32 a = bswap32(*(volatile sf_u32 *)ctx->r[n]), b = bswap32(*(volatile sf_u32 *)ctx->r[m]);
        ctx->r[n] += 4; ctx->r[m] += 4;
        acc += (sf_u64)((sf_i64)(sf_i32)a * (sf_i64)(sf_i32)b);
    } else {                                                 /* mac.w */
        sf_u32 a = bswap16(*(volatile sf_u16 *)ctx->r[n]), b = bswap16(*(volatile sf_u16 *)ctx->r[m]);
        ctx->r[n] += 2; ctx->r[m] += 2;
        acc += (sf_u64)((sf_i64)(sf_i32)(sf_i32)(short)a * (sf_i64)(sf_i32)(short)b);
    }
    ctx->mach = (sf_u32)(acc >> 32); ctx->macl = (sf_u32)acc;
    g_helper_ret = 0;
}

static sf_u32 helper_guest_pc(sf_u32 ret, int *in_insn_range)
{
    dbt_block *b = block_of_host(ret - 4);                      /* the jmp of the call sequence */
    sf_u32 off;
    int idx;
    if (!b) dbt_halt("helper return address not in a block");
    off = ret - 4 - b->host_base;
    idx = dbt_block_find(b, off);
    if (idx < 0) dbt_halt("helper return address outside the block's code");
    *in_insn_range = dbt_meta_kind(b, idx) == META_INSN;
    return meta_start_pc(b, idx, &dbt_ctx_rt);
}
