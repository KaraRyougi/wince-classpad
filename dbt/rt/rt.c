/*
 * rt.c - translator runtime, C half. Target-only (big-endian SH7305, P1
 * addresses, no OS). Everything here runs with interrupts masked or BL=1.
 */
#include "rt.h"
#include "lcdcon.h"
#include "lcd_io.h"
#include "irqpins.h"

typedef __SIZE_TYPE__ dbt_size_t;
void *memcpy(void *d, const void *s, dbt_size_t n);
void *memset(void *d, int c, dbt_size_t n);

#ifndef DBT_HASH_ENTRIES
#define DBT_HASH_ENTRIES 32768                 /* dbt/Makefile HASH_ENTRIES */
#endif
#if DBT_HASH_ENTRIES < 256 || DBT_HASH_ENTRIES > 65536 || (DBT_HASH_ENTRIES & (DBT_HASH_ENTRIES - 1))
#error DBT_HASH_ENTRIES must be a power of two between 256 and 65536
#endif
#define HSIZE DBT_HASH_ENTRIES
#if defined(DBT_HASH_TWO_WAY) && (!defined(DBT_HASH_ASID_TAG) || defined(DBT_HASH_ASID_MIX))
#error DBT_HASH_TWO_WAY requires tagged entries without ASID mixing
#endif
#if defined(DBT_HASH_ASID_MIX) && !defined(DBT_HASH_ASID_TAG)
#error DBT_HASH_ASID_MIX requires tagged hash entries
#endif
#ifndef MAX_BLOCKS
#define MAX_BLOCKS   19200                 /* compact block headers + a metadata arena; see docs/04 */
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

dbt_gctx dbt_ctx_rt DBT_XYRAM, dbt_ctx_exc DBT_XYRAM;

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
#ifdef DBT_FAST_RTE
sf_u32 dbt_rte_sr_save;                  /* rt/stubs.S rte fast path: the SR its C fallback needs */
volatile sf_u32 dbt_fast_rte_calls;      /* rtes completed by that fast path */
#endif
#ifdef DBT_STAT_IN_RAM
dbt_stats dbt_stat;            /* diagnostics: where the emulator's counter snapshot can read it */
#else
dbt_stats dbt_stat DBT_XYRAM;
#ifdef DBT_FPU_HIST
static sf_u32 g_fpu_hist[34];   /* 0-15: Fnnnmmmm[sub] arithmetic/move classes, 16-31: F??[kind]D single-operand kinds, 32: lds/sts, 33: ops run with PR=1 */
#endif
#endif
volatile sf_u32 dbt_lookup_sr; /* saved before lookup hash/IC tests alter T */

typedef struct {
    sf_u32 gpc, host;
#ifdef DBT_HASH_ASID_TAG
    sf_u32 asid; /* user keys are valid only in their publishing address space */
#endif
} hent;
#ifdef DBT_HASH_ASID_TAG
typedef char check_tagged_hash_stride[(sizeof(hent) == 12) ? 1 : -1];
#endif
#ifdef DBT_HASH_DIAG
/* -DDBT_HASH_DIAG: why a C lookup found its block outside the hash. Each
 * slot remembers what last emptied it; g_hash_diag (snapshot it with
 * HLE_DBT_COUNTER_BASE) counts per cause: 0 slot held another pc
 * (collision), 1 never filled, 2 invalidation, 3 ASID switch, 4 bank
 * probation (second chance), 5 bank recycle, 6 full flush; 7 all lookups
 * that missed the hash, 8 of those that went on to translate; 9
 * block_of_host calls, 10 of them answered by its cache; 11-13 banks full
 * by headers / code / metadata, 14-16 headers / code / metadata bytes of
 * those banks, summed (printed with the statistics). */
enum { HD_COLLIDE, HD_NEVER, HD_INVAL, HD_ASID, HD_PROBATION, HD_RECYCLE, HD_FLUSH_ALL, HD_MISSES, HD_TRANSLATED,
       HD_OWNER_CALLS, HD_OWNER_HITS };
sf_u32 g_hash_diag[17];
static sf_u8 g_hash_why[DBT_HASH_ENTRIES];
#define HASH_WHY(slot, why) (g_hash_why[slot] = (sf_u8)(why))
#else
#define HASH_WHY(slot, why) ((void)0)
#endif

/* Opt-in reuse of the launcher's first MiB after the no-return handoff.
 * crt0 switches to the private translator stack before clearing this area.
 * Do not use this layout in a launcher that expects to return to Casio OS. */
#ifdef DBT_BOOT_WORKSPACE_TABLES
#define BOOT_TABLE __attribute__((section(".lowbss")))
#else
#define BOOT_TABLE
#endif

#ifdef DBT_HASH_HIGH_RAM
/* Optional layout trade: free boot-workspace bytes for more unchanged
 * 36-byte headers; the hash instead consumes the upper runtime allocation. */
#define HASH_TABLE_SECTION
#else
#define HASH_TABLE_SECTION BOOT_TABLE
#endif
hent dbt_hash[HSIZE] HASH_TABLE_SECTION __attribute__((aligned(32)));   /* read by stubs.S */
#define g_hash dbt_hash
static dbt_block g_blocks[MAX_BLOCKS] BOOT_TABLE; /* sorted within each cache bank */
static dbt_block g_scratch DBT_XYRAM;                 /* translation target; copied into g_blocks + arena on success */
static dbt_imeta g_scratch_meta[DBT_MAX_META] DBT_XYRAM;
static dbt_spill g_scratch_spills[DBT_MAX_BLOCK_SPILLS] DBT_XYRAM;
static dbt_link g_scratch_links[DBT_MAX_LINKS] DBT_XYRAM;
static sf_u8     g_arena[META_ARENA_BYTES] __attribute__((aligned(8)));
static sf_u32    g_arena_used;
#ifdef DBT_META_ALIGN4
#define META_ALIGN_MASK 3u
typedef char check_meta_alignment[(__alignof__(dbt_imeta) <= 4 &&
    __alignof__(dbt_spill) <= 4 && __alignof__(dbt_link) <= 4) ? 1 : -1];
#else
#define META_ALIGN_MASK 7u
#endif
static sf_u32 arena_round(sf_u32 bytes) {
    return (bytes + META_ALIGN_MASK) & ~META_ALIGN_MASK;
}

#define SCRATCH_ARENA_NEED (DBT_MAX_META * sizeof(dbt_imeta) + DBT_MAX_BLOCK_SPILLS * sizeof(dbt_spill) + DBT_MAX_LINKS * sizeof(dbt_link) + 16)
static dbt_rt_config g_cfg DBT_XYRAM;
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
#endif
#if defined(DBT_INTERIOR_CACHE) || defined(DBT_HOST_OWNER_CACHE)
static sf_u32 g_bank_generation[CACHE_BANKS];
#endif
#if defined(DBT_CORE_BANKS) && !defined(DBT_KERNEL_BANKS)
#error DBT_CORE_BANKS requires DBT_KERNEL_BANKS
#endif
#ifdef DBT_CORE_BANKS
static sf_u32 g_core_translates, g_other_translates, g_core_evictions;
#endif
#ifdef DBT_KERNEL_BANKS
typedef char check_split_banks[(DBT_KERNEL_BANKS > 0 && DBT_KERNEL_BANKS < CACHE_BANKS) ? 1 : -1];
static unsigned g_stream_bank[2];
static sf_u32 g_saved_code[CACHE_BANKS], g_saved_arena[CACHE_BANKS];
#endif
static sf_u32 g_bank_touch[CACHE_BANKS], g_access_clock;
#if defined(DBT_LAZY_META) || defined(DBT_SECOND_CHANCE)
#if defined(DBT_KERNEL_BANKS) || defined(DBT_BODY_BUDGET) || defined(DBT_COMPACT_HEADER32)
#error DBT_LAZY_META / DBT_SECOND_CHANCE need one bank stream, a fixed instruction budget and the 36-byte header
#endif
/* A bank's metadata arena is filled from its start at translation (links,
 * relocation records, resident metadata) and later from its top on demand
 * (DBT_LAZY_META). The current bank's top is g_arena_used. */
static sf_u32 g_arena_top[CACHE_BANKS];
/* Per-block arena chunk, allocated at translation: links, the relocation
 * record (DBT_SECOND_CHANCE), then packed metadata and spills when they are
 * resident (packed_meta == 2). Metadata built later lives outside the chunk
 * (packed_meta == 1, meta NULL until needed). */
#define META_RESIDENT 2
#endif
#ifdef DBT_LAZY_META
static int g_lazy_owner = -1;          /* block whose metadata is in g_lazy_tmp */
#endif
#ifdef DBT_SECOND_CHANCE
#if defined(DBT_INCOMING_BITMAP) || defined(DBT_INTERIOR_HASH) || defined(DBT_INTERIOR_CACHE) || \
    defined(DBT_HOST_OWNER_CACHE) || defined(DBT_PRUNE_INDEX_REUSE) || defined(DBT_CACHE_LRU) || \
    defined(DBT_CACHE_TICK_LFU) || defined(DBT_IRQ_ENTRY_CACHE) || defined(DBT_HASH_TWO_WAY) || defined(DBT_BATCH_INVALIDATE)
#error DBT_SECOND_CHANCE is written for the default index, hash and invalidation paths
#endif
#ifndef DBT_SECOND_CHANCE_SHARE
#define DBT_SECOND_CHANCE_SHARE 2       /* survivors take at most 1/N of a bank */
#endif
/* Used since the bank was put on probation (rt.c cache_next_bank). */
static sf_u32 g_ref[(MAX_BLOCKS + 31) / 32];
static sf_u8 g_reloc_map[DBT_RELOC_BYTES];
static sf_u16 g_reloc_pool;
static sf_u32 g_survivors, g_survivor_bytes, g_probations;
static void ref_mark(unsigned i) { g_ref[i >> 5] |= 1u << (i & 31); }
static void ref_clear(unsigned i) { g_ref[i >> 5] &= ~(1u << (i & 31)); }
static int ref_test(unsigned i) { return (g_ref[i >> 5] >> (i & 31)) & 1; }
#else
#define ref_mark(i) ((void)0)
#endif
/* Conservative destinations of patched direct links. Bits remain set until
 * the owner is replaced; false positives only cost a scan. */
static sf_u16 g_link_banks[MAX_BLOCKS];
/* Host address of block i's inline-cache pair, 0 if it has none. Written
 * when a header is filled (translation, survivor move); stale for dead
 * headers, so readers check the dead flag before acting on a match. It
 * lets finish_invalidations skip blocks without an IC without touching
 * their headers. (A dense copy of each IC's target, kept by every refill
 * including the stub's, measured slower on 2026-11-07: docs/04.) */
static sf_u32 g_ic_site[MAX_BLOCKS];
#ifndef DBT_HOST_OWNER_CACHE
#ifndef DBT_OWNER_SEARCH
/* block_of_host's last answers by host address (index + 1, 0 = empty).
 * A hit counts only for a live block whose host range holds the address:
 * live blocks never overlap, so that is the block the search would find
 * (dead owners, which some callers need, still come from the search).
 * Cleared wherever headers are rewritten or reordered (full flush, bank
 * recycle). Default since 2026-11-07; -DDBT_OWNER_SEARCH: search only. */
#define DBT_OWNER_CACHE 1
static sf_u16 g_owner_cache[256];
#define OWNER_CACHE_CLEAR() memset(g_owner_cache, 0, sizeof g_owner_cache)
#endif
#endif
#ifndef OWNER_CACHE_CLEAR
#define OWNER_CACHE_CLEAR() ((void)0)
#endif
#ifdef DBT_INCOMING_BITMAP
#define SOURCE_WORDS ((MAX_BLOCKS + 31) / 32)
/* Conservative source sets: stale bits only add work. Publish before a
 * block/link becomes reachable; clear source ownership on header reuse. */
static sf_u32 g_source_links[CACHE_BANKS][SOURCE_WORDS];
static sf_u32 g_source_ic[SOURCE_WORDS];
static sf_u32 g_source_scans;
#ifdef DBT_INCOMING_FAST_SCAN
/* Nonzero input only. Avoid libgcc's out-of-line bit scan on SH4. */
static unsigned source_low_bit(sf_u32 value)
{
    static const sf_u8 positions[32] = {
        0,1,28,2,29,14,24,3,30,22,20,15,25,17,4,8,
        31,27,13,23,21,19,16,7,26,12,18,6,11,5,10,9
    };
    return positions[((value & (0u - value)) * 0x077cb531u) >> 27];
}
#endif
typedef struct { unsigned word, base, banks; sf_u32 pending; } source_scan;
static int source_next(source_scan *scan)
{
    for (;;) {
        if (scan->pending) {
#ifdef DBT_INCOMING_FAST_SCAN
            unsigned bit = source_low_bit(scan->pending);
#else
            unsigned bit = (unsigned)__builtin_ctz(scan->pending);
#endif
            scan->pending &= scan->pending - 1;
            return (int)(scan->base + bit);
        }
        if (scan->word >= SOURCE_WORDS) return -1;
        {
            unsigned bank, word = scan->word++;
            if (!word) ++g_source_scans;
            sf_u32 candidates = g_source_ic[word];
#ifdef DBT_INCOMING_FAST_SCAN
            unsigned banks = scan->banks;
            while (banks) {
                bank = source_low_bit(banks);
                banks &= banks - 1;
                candidates |= g_source_links[bank][word];
            }
#else
            for (bank = 0; bank < CACHE_BANKS; ++bank)
                if (scan->banks & (1u << bank)) candidates |= g_source_links[bank][word];
#endif
            scan->base = word * 32;
            scan->pending = candidates;
        }
    }
}
#endif

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
static fpu_state g_fpu DBT_XYRAM;
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
#if defined(DBT_CONST_DEVICE_ADDRESS) || defined(DBT_UNIFORM_STORE)
static sf_u32 g_const_device_blocks;
#endif
static void print_stats(void);
#ifdef DBT_DEV_SITES
static void print_dev_sites(void);
#endif

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
static binfo  g_binfo[MAX_BLOCKS] BOOT_TABLE;
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
/* -DDBT_USER_IC_REFILL: the lookup_ic stub refills an IC from a user hash
 * hit and lists it here itself (interrupts masked), finding the block's
 * index in the word after the IC pair (set_ic_index). Without it a user IC
 * emptied by an address-space switch stayed empty: every jsr @Rn / rts of
 * user code then took the stub's hash probe (a third of Plotter-Z's redraw,
 * docs/08). */
#ifdef DBT_USER_IC_REFILL
sf_u16 g_ic_active[MAX_BLOCKS];
sf_u8 g_ic_active_seen[MAX_BLOCKS];
int g_nic;
#else
static sf_u16 g_ic_active[MAX_BLOCKS];
static sf_u8 g_ic_active_seen[MAX_BLOCKS];
static int g_nic;
#endif
/* Only hash slots that received user code since the last ASID switch. */
#ifndef DBT_HASH_ASID_TAG
static sf_u16 g_user_hash[HSIZE];     /* untagged entries leave the hash at an ASID switch */
static sf_u8 g_user_hash_seen[HSIZE];
#endif
static unsigned g_nuser_hash;
static int    g_nuser;        /* g_nic counts ICs filled with user targets this ASID */
/* Conservative membership filter for 1 KiB virtual pages containing user
 * code. Most ldtlb calls refill DATA, for which a block scan does no work.
 * Never clear individual bits (collisions/dead blocks only cause extra scans).
 * Two pages cover even a block straddling a 1 KiB boundary. */
#ifdef DBT_FAST_LDTLB
sf_u32 dbt_user_pages[256], dbt_fast_ldtlb_calls;
#define g_user_pages dbt_user_pages
typedef char check_tlb_hooks_offset[(__builtin_offsetof(dbt_stats, tlb_hooks) == 96) ? 1 : -1];
typedef char check_tlb_skips_offset[(__builtin_offsetof(dbt_stats, tlb_scan_skips) == 104) ? 1 : -1];
#else
static sf_u32 g_user_pages[256] DBT_XYRAM;
#endif
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
#ifdef DBT_PAGE_GROUP_FILTER
    /* tlb_hook supplies a page-size-aligned VPN. For a 4 KiB page the
     * four 1 KiB hashes occupy one aligned nibble (XOR only permutes it),
     * so one bitmap read replaces four hash/probe iterations. */
    if (end <= vpn) return 0; /* preserve the original wrapping-range result */
    if (shift == 10 || shift == 12) {
        unsigned bit = user_page_bit(vpn), bits = 1;
        if (shift == 12) { bit &= ~3u; bits = 15; }
        return (g_user_pages[bit >> 5] & (bits << (bit & 31))) != 0;
    }
#endif
    for (; vpn < end; vpn += 1024) {
        unsigned bit = user_page_bit(vpn);
        if (g_user_pages[bit >> 5] & (1u << (bit & 31))) return 1;
    }
    return 0;
}
sf_u32 dbt_current_asid;
#define g_asid dbt_current_asid
#ifdef DBT_FAST_LDC_SR
volatile sf_u32 dbt_fast_ldc_sr_calls;
#define FAST_LDC_SR_CALLS dbt_fast_ldc_sr_calls
#else
#define FAST_LDC_SR_CALLS 0u
#endif
static dbt_env   g_env DBT_XYRAM;

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
#ifdef DBT_BOOT_MARQUEE
    bootmarquee_putc(c);            /* the display driver's sign-on ends the start screen's marquee */
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
#ifdef DBT_BOOT_MARQUEE
    bootmarquee_stop();
#endif
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

#ifdef DBT_LAZY_META
/* Translation reads go through these: one past the highest guest byte read
 * (code and folded PC-relative literals) decides whether the block can be
 * translated again later from its physical address alone. */
static sf_u32 g_read_hi;
static void note_read(sf_u32 a, sf_u32 n) { if (a + n > g_read_hi) g_read_hi = a + n; }
static int xl_fetch16(void *c, sf_u32 a, sf_u32 *v) { note_read(a, 2); return g_fetch16(c, a, v); }
static int xl_read16le(void *c, sf_u32 a, sf_u32 *v) { note_read(a, 2); return g_read16le(c, a, v); }
static int xl_read32le(void *c, sf_u32 a, sf_u32 *v) { note_read(a, 4); return g_read32le(c, a, v); }
/* The same guest bytes read again, through P1/P2 at the recorded physical
 * address (TLB-mapped code may have lost its UTLB entry since). */
typedef struct { sf_u32 gpc, base; } regen_ctx;
static sf_u32 regen_addr(void *c, sf_u32 a) { const regen_ctx *r = c; return r->base + (a - r->gpc); }
static int regen_fetch16(void *c, sf_u32 a, sf_u32 *v) { return g_fetch16(0, regen_addr(c, a), v); }
static int regen_read16le(void *c, sf_u32 a, sf_u32 *v) { return g_read16le(0, regen_addr(c, a), v); }
static int regen_read32le(void *c, sf_u32 a, sf_u32 *v) { return g_read32le(0, regen_addr(c, a), v); }
#endif
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

#ifdef DBT_HASH_ASID_MIX
static inline sf_u32 fast_hidx(sf_u32 gpc, sf_u32 asid)
{
    /* Block-owner chains stay PC-only. Only the direct lookup cache mixes
     * address spaces, matching the assembly fast path. */
    return (hidx(gpc) ^ (IS_USER(gpc) ? ((asid & 255u) << 5) : 0)) & (HSIZE - 1);
}
#elif defined(DBT_HASH_TWO_WAY)
#define fast_hidx(gpc, asid) (hidx(gpc) & ~1u)
#else
#define fast_hidx(gpc, asid) hidx(gpc)
#endif

static void hash_insert(sf_u32 gpc, sf_u32 host)
{
    sf_u32 sr;
    unsigned slot = fast_hidx(gpc, g_asid);
    hent *e = &g_hash[slot];
    __asm__ volatile ("stc sr, %0" : "=r"(sr));
    __asm__ volatile ("ldc %0, sr" :: "r"(sr | SR_IMASK) : "memory");
#ifdef DBT_HASH_TWO_WAY
    if (e[0].gpc == gpc && (!IS_USER(gpc) || e[0].asid == g_asid)) {
        /* Refresh in place; no extra writes on a repeated first-way key. */
    } else if (e[1].gpc == gpc && (!IS_USER(gpc) || e[1].asid == g_asid)) {
        ++e;
    } else if (e[0].host && e[0].gpc != 1) {
        /* Newest insertion occupies way zero; retain the previous key in
         * way one. No replacement metadata is written by assembly hits. */
        e[1] = e[0];
    }
#endif
#ifdef DBT_HASH_ASID_TAG
    e->asid = g_asid;
#else
    if (IS_USER(gpc) && !g_user_hash_seen[slot]) {
        g_user_hash[g_nuser_hash++] = (sf_u16)slot;
        g_user_hash_seen[slot] = 1;
    }
#endif
    e->host = host; e->gpc = gpc;
    __asm__ volatile ("ldc %0, sr" :: "r"(sr) : "memory");
}

static sf_u32 hash_find(sf_u32 gpc)
{
    const hent *e = &g_hash[fast_hidx(gpc, g_asid)];
#ifdef DBT_HASH_TWO_WAY
    unsigned way;
    for (way = 0; way < 2; ++way, ++e)
        if (e->host && e->gpc == gpc && (!IS_USER(gpc) || e->asid == g_asid)) return e->host;
    return 0;
#else
#ifdef DBT_HASH_ASID_TAG
    if (IS_USER(gpc) && e->asid != g_asid) return 0;
#endif
    return (e->host && e->gpc == gpc) ? e->host : 0;
#endif
}

/* Empty slots carry gpc = 1 (odd: never a pc): the asm fast path compares
 * only the gpc, so a cleared slot must not keep its old key. */
static sf_u8 g_asid_seen[32], g_asid_list[64];
static unsigned g_nasid_seen;
static unsigned g_pend_hash;       /* DBT_INVAL_PENDING: a dead shared owner's hash entries wait for finish_invalidations' sweep */
static void hash_remove(const dbt_block *b)
{
#ifdef DBT_HASH_ASID_MIX
    /* A physically shared owner can be cached under multiple ASIDs. Its
     * original ASID no longer enumerates the possible direct-hash slots. */
    if (g_binfo[b - g_blocks].user && g_binfo[b - g_blocks].asid == ASID_ANY) {
        unsigned i;
#ifdef DBT_INVAL_PENDING
        /* CE 5.0 keeps every DLL's code in slot 1, shared by all processes,
         * so most user blocks are ASID_ANY, and a range flush used to walk
         * the 32768-entry hash for each of them (24 % of the boot in a
         * heartbeat profile, 2026-11-09). Every entry was inserted under the
         * ASID current at the time (hash_insert), so its slot is
         * fast_hidx(pc, a) for an ASID a the guest has run under: probe
         * those (CE 5.0 runs a dozen processes). Only a guest that has used
         * too many ASIDs for that gets the one sweep per batch in
         * finish_invalidations (pend_dead knows the dead ranges). */
        if (g_nasid_seen <= 64) {
            unsigned k;
            sf_u32 lo = b->host_base, hi = lo + b->host_len;
            for (k = 0; k < g_nasid_seen; ++k) {
                hent *e = &g_hash[fast_hidx(b->guest_pc, g_asid_list[k])];
                if (e->host >= lo && e->host < hi) { e->gpc = 1; e->host = 0; }
#ifdef DBT_HASH_TWO_WAY
                if (e[1].host >= lo && e[1].host < hi) { e[1].gpc = 1; e[1].host = 0; }
#endif
            }
            return;
        }
        g_pend_hash = 1;
        return;
#endif
        for (i = 0; i < HSIZE; ++i) {
            hent *a = &g_hash[i];
            if (a->host >= b->host_base && a->host < b->host_base + b->host_len) {
                a->gpc = 1; a->host = 0;
#ifdef DBT_INTERIOR_HASH
                ++g_alias_hash_removed;
#endif
            }
        }
        return;
    }
#endif
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
            hent *a = &g_hash[fast_hidx(b->guest_pc + i, g_binfo[index].asid)];
#ifdef DBT_HASH_TWO_WAY
            if (a[1].host >= lo && a[1].host < hi) {
                a[1].gpc = 1; a[1].host = 0; ++g_alias_hash_removed;
            }
#endif
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
    hent *e = &g_hash[fast_hidx(b->guest_pc, g_binfo[b - g_blocks].asid)];
    if (e->host == b->host_base) { e->gpc = 1; e->host = 0; HASH_WHY(e - g_hash, HD_INVAL); }
#ifdef DBT_HASH_TWO_WAY
    if (e[1].host == b->host_base) { e[1].gpc = 1; e[1].host = 0; }
#endif
}

/* Physical address of virtual `va` under `asid` according to the UTLB
 * (array reads at 0xF6000000 / 0xF7000000), PHYS_UNKNOWN if unmapped. */
#ifdef DBT_INVAL_DIAG
static sf_u32 g_utlb_calls;
#endif
static sf_u32 utlb_translate(sf_u32 va, sf_u32 asid)
{
    static const sf_u8 shift_of_sz[4] = { 10, 12, 16, 20 };
    int e;
#ifdef DBT_INVAL_DIAG
    ++g_utlb_calls;
#endif
    if (!(REG_MMUCR & 1)) return va & 0x1FFFFFFFu;
#ifndef DBT_UTLB_SCAN
    /* A hint caches only a hardware slot number, never a translation
     * (DBT_UTLB_HINT until 2026-11-07; -DDBT_UTLB_SCAN: always scan all 64).
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

/* The word after a block's IC pair: its index, for the stub's own refill. */
static void set_ic_index(unsigned index)
{
#ifdef DBT_USER_IC_REFILL
    if (g_ic_site[index]) ((volatile sf_u32 *)g_ic_site[index])[2] = index;
#else
    (void)index;
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

#if defined(DBT_BATCH_RANGES) && !defined(DBT_BATCH_INVALIDATE)
#error DBT_BATCH_RANGES requires DBT_BATCH_INVALIDATE
#endif
#if !defined(DBT_BATCH_INVALIDATE) && !defined(DBT_INVAL_IMMEDIATE)
/* Deferred detach (default since 2026-11-07; -DDBT_INVAL_IMMEDIATE for the
 * old per-block scan). invalidate_block only records the dead host range;
 * finish_invalidations, which every caller runs before the guest continues,
 * detaches the ICs and direct links into all recorded ranges in one pass
 * over the headers. A range or TLB flush killing k blocks used to make k
 * passes over all of them. Up to PEND_RANGES ranges are compared directly,
 * more go to a 32-byte-granule map that is cleared again afterwards (a
 * granule shared with a live neighbour only detaches a few of its links,
 * which the next lookup restores). Nothing outlives the batch, so bank
 * recycling and second chance need not know about it. */
#define DBT_INVAL_PENDING 1
#ifndef DBT_EAGER_DETACH
#define DBT_LAZY_DETACH 1        /* -DDBT_EAGER_DETACH: detach after every kill, as before 2026-11-09 */
#endif
#define PEND_RANGES 4                  /* beyond this the 32-byte map: pend_dead runs for every IC and link target in the span */
static sf_u32 g_pend_lo[PEND_RANGES], g_pend_len[PEND_RANGES];
static unsigned g_npend, g_pend_over, g_pend_banks;
static sf_u32 g_pend_min, g_pend_span;
static sf_u8 g_pend_map[(CACHE_SIZE + 255u) / 256u];
static void pend_mark(sf_u32 lo, sf_u32 len)
{
    sf_u32 i = (lo - g_cfg.cache_base) >> 5, end = (lo + len - g_cfg.cache_base + 31u) >> 5;
    for (; i < end; ++i) g_pend_map[i >> 3] |= (sf_u8)(1u << (i & 7));
}
static void pend_add(sf_u32 lo, sf_u32 hi, unsigned bank_mask)
{
    unsigned k;
    if (!g_npend) { g_pend_min = lo; g_pend_span = hi - lo; }
    else {
        sf_u32 max = g_pend_min + g_pend_span;
        if (lo < g_pend_min) g_pend_min = lo;
        if (hi > max) max = hi;
        g_pend_span = max - g_pend_min;
    }
    g_pend_banks |= bank_mask;
    if (g_npend < PEND_RANGES) { g_pend_lo[g_npend] = lo; g_pend_len[g_npend] = hi - lo; }
    else {
        if (!g_pend_over)
            for (k = 0; k < PEND_RANGES; ++k) pend_mark(g_pend_lo[k], g_pend_len[k]);
        g_pend_over = 1;
        pend_mark(lo, hi - lo);
    }
    ++g_npend;
}
static int pend_dead(sf_u32 host)
{
    unsigned k;
    if (host - g_pend_min >= g_pend_span) return 0;
    if (g_pend_over) {
        sf_u32 off = host - g_cfg.cache_base;
        return (g_pend_map[off >> 8] >> ((off >> 5) & 7)) & 1;
    }
    for (k = 0; k < g_npend; ++k) if (host - g_pend_lo[k] < g_pend_len[k]) return 1;
    return 0;
}
#endif
#if defined(DBT_BATCH_HASH_REMOVE) && (!defined(DBT_BATCH_INVALIDATE) || !defined(DBT_INTERIOR_HASH))
#error DBT_BATCH_HASH_REMOVE requires DBT_BATCH_INVALIDATE and DBT_INTERIOR_HASH
#endif
#ifdef DBT_BATCH_HASH_REMOVE
static int g_batch_hash_pending;
static sf_u32 g_batch_hash_scans;
#endif
#ifdef DBT_BATCH_INVALIDATE
#ifdef DBT_BATCH_RANGES
/* Range/module teardown batches many removals. A single TLB replacement
 * retains the immediate path, without a whole-cache batch completion. */
static int g_range_batch;
#define SHOULD_BATCH_INVALIDATION g_range_batch
#else
#define SHOULD_BATCH_INVALIDATION 1
#endif
#ifndef CACHE_SIZE
#define CACHE_SIZE 0x00400000u
#endif
/* One bit conservatively covers 32 code bytes. With compact alignment a
 * live neighbour may share a bit: detaching its incoming links is harmless,
 * and normal lookup can refill them. Never omit a dead destination. The
 * bitmap is cleared before a code bank is reused. */
static sf_u8 g_dead_code[(CACHE_SIZE + 255u) / 256u];
static unsigned g_dead_banks;
static int is_dead_host(sf_u32 host)
{
    sf_u32 off = host - g_cfg.cache_base;
    return off < g_cfg.cache_size &&
        (g_dead_code[off >> 8] & (1u << ((off >> 5) & 7)));
}
#endif

#ifdef DBT_VERIFY_INCOMING_BITMAP
#ifndef DBT_INCOMING_BITMAP
#error DBT_VERIFY_INCOMING_BITMAP requires DBT_INCOMING_BITMAP
#endif
static void verify_sources(unsigned banks)
{
    int i;
    for (i = 0; i < g_nblocks; ++i) {
        unsigned bank, word = (unsigned)i >> 5;
        sf_u32 bit = 1u << (i & 31);
        if (g_binfo[i].dead) continue;
        if (g_blocks[i].ic_off >= 0 && !(g_source_ic[word] & bit))
            dbt_halt("missing IC source bit");
        for (bank = 0; bank < CACHE_BANKS; ++bank)
            if ((banks & g_link_banks[i] & (1u << bank)) && !(g_source_links[bank][word] & bit))
                dbt_halt("missing direct-link source bit");
    }
}
#endif

/* Make block b unreachable: hash entry, inline caches and chained jumps that
 * lead into it. Its cache memory stays dead until the next full flush.
 * DBT_LAZY_DETACH (default since 2026-11-09): the ICs and links are not
 * detached here, only recorded (pend_add); the block's first 28 bytes
 * become the sequence a block's exit stub uses to enter the plain lookup
 * stub (X0 = the block's guest pc, X1 = 0, r1 through Y0, all guest
 * registers intact), so a stale link, IC or hash entry that still leads
 * into it costs one hash lookup and lands on the live translation. No
 * exception is involved: a trap would reset the CPU when the guest runs
 * with SR.BL set, as the kernel does during init. The two passes over all
 * block headers (finish_invalidations) then run once per bank recycle and
 * full flush, before dead memory is reused, instead of once per ldtlb or
 * TLB flush that killed something (13% of the shell's boot). Blocks are
 * allocated in 32-byte units, so the 28 bytes fit whatever the block's
 * length (not with DBT_CODE_ALIGN4). */
#ifdef DBT_INVAL_DIAG
/* -DDBT_INVAL_DIAG: calls and blocks killed per origin (0 range, 1 all but ROM, 2 TLB, 3 revalidated user blocks) */
static unsigned g_inval_origin;
static sf_u32 g_inval_calls[4], g_inval_kills[4], g_inval_multi[4];
static sf_u32 g_abr_kills[6];           /* all-but-ROM kills: phys unknown, RAM, other; pc slot 0, slots 1+, kernel */
#define INVAL_CALL(o) do { g_inval_origin = (o); ++g_inval_calls[o]; g_inval_before = g_inval_kills[o]; } while (0)
#define INVAL_END(o) do { if (g_inval_kills[o] - g_inval_before > 1) ++g_inval_multi[o]; } while (0)
static sf_u32 g_inval_before;
#else
#define INVAL_CALL(o) ((void)0)
#define INVAL_END(o) ((void)0)
#endif
static void invalidate_block(dbt_block *b)
{
    int i, k;
#ifdef DBT_INVAL_DIAG
    ++g_inval_kills[g_inval_origin];
#endif
    sf_u32 lo = b->host_base, hi = b->host_base + (sf_u32)b->host_len;
    unsigned bank_mask = 1u << bank_of_code(lo);
#ifdef DBT_INCOMING_BITMAP
    source_scan scan = {0, 0, bank_mask, 0};
#endif
    g_binfo[b - g_blocks].dead = 1;
#ifdef DBT_BATCH_HASH_REMOVE
    /* Aliases otherwise force a whole hash-table walk for every dead owner.
     * Defer those walks until this range's dead destinations are all known.
     * No guest lookup may run between marking and finish_invalidations(). */
    if (SHOULD_BATCH_INVALIDATION &&
        (g_alias_blocks[(b - g_blocks) >> 3] & (1u << ((b - g_blocks) & 7))))
        g_batch_hash_pending = 1;
    else
#endif
    hash_remove(b);
#ifdef DBT_BATCH_INVALIDATE
    if (SHOULD_BATCH_INVALIDATION) {
        for (i = (int)((lo - g_cfg.cache_base) >> 5);
             (sf_u32)i * 32 < hi - g_cfg.cache_base; ++i)
            g_dead_code[i >> 3] |= (sf_u8)(1u << (i & 7));
        g_dead_banks |= bank_mask;
    } else
#endif
#ifdef DBT_INVAL_PENDING
    pend_add(lo, hi, bank_mask);
#ifdef DBT_LAZY_DETACH
    {
        volatile sf_u16 *c = (volatile sf_u16 *)lo;
        volatile sf_u32 *l = (volatile sf_u32 *)(lo + 16);
        l[0] = b->guest_pc; l[1] = 0; l[2] = g_env.rt_lookup;
        c[1] = 0xD103; c[2] = 0x418A;          /* mov.l @(3,pc), r1 (= guest pc); lds r1, X0 */
        c[3] = 0xD103; c[4] = 0x419A;          /* mov.l @(3,pc), r1 (= 0);        lds r1, X1 */
        c[5] = 0xD103; c[6] = 0x412B;          /* mov.l @(3,pc), r1 (= lookup);   jmp @r1 */
        c[7] = 0x01AA;                         /*   sts Y0, r1 (delay slot: r1 back) */
        c[0] = 0x41AA;                         /* lds r1, Y0: written last, the entry is live until then */
        dbt_sync_code(lo, 28);
    }
#endif
    (void)i; (void)k;
#else
    {
#ifdef DBT_INCOMING_BITMAP
#ifdef DBT_VERIFY_INCOMING_BITMAP
        verify_sources(bank_mask);
#endif
        while ((i = source_next(&scan)) >= 0) {
#else
        for (i = 0; i < g_nblocks; i++) {
#endif
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
    }
#endif
    dbt_stat.invalidations++;
}

/* Complete a batch before returning to guest execution. Multiple blocks
 * can die in one TLB/range flush; scan their incoming links only once. */
static void finish_invalidations(void)
{
#ifdef DBT_INVAL_PENDING
    int i, k, n = g_nblocks;           /* local: the calls below would force a reload per block */
    sf_u32 min = g_pend_min, span = g_pend_span;
    unsigned banks = g_pend_banks;
    if (!g_npend) return;
    /* two tight passes: IC targets from the dense site array, then links
     * of the blocks whose link mask meets a dead bank */
    for (i = 0; i < n; ++i) {
        sf_u32 a = g_ic_site[i], h;
        if (!a) continue;
        h = *(volatile sf_u32 *)(a + 4);
        if (h - min < span && !g_binfo[i].dead && pend_dead(h)) reset_ic(&g_blocks[i]);
    }
    for (i = 0; i < n; ++i) {
        dbt_block *o;
        if (!(g_link_banks[i] & banks) || g_binfo[i].dead) continue;
        o = &g_blocks[i];
        for (k = 0; k < o->nlinks; ++k) {
            sf_u32 v = *(volatile sf_u32 *)(o->host_base + (sf_u32)o->links[k].lit_off);
            if (v - min < span && pend_dead(v)) unchain(o, k);
        }
    }
    if (g_pend_hash) {
        /* hash entries of the shared owners that died in this batch (hash_remove) */
        for (i = 0; i < HSIZE; ++i) {
            hent *e = &g_hash[i];
            if (e->host - min < span && pend_dead(e->host)) { e->gpc = 1; e->host = 0; }
        }
        g_pend_hash = 0;
    }
    if (g_pend_over) {
        sf_u32 a = (g_pend_min - g_cfg.cache_base) >> 8, b = (g_pend_min + g_pend_span - g_cfg.cache_base + 255u) >> 8;
        memset(g_pend_map + a, 0, b - a);
    }
    g_npend = 0; g_pend_over = 0; g_pend_banks = 0;
#endif
#ifdef DBT_BATCH_INVALIDATE
    int i, k;
#ifdef DBT_INCOMING_BITMAP
    source_scan scan = {0, 0, g_dead_banks, 0};
#endif
    if (!g_dead_banks) return;
#ifdef DBT_BATCH_HASH_REMOVE
    if (g_batch_hash_pending) {
        for (i = 0; i < HSIZE; ++i) {
            hent *e = &g_hash[i];
            if (e->host && is_dead_host(e->host)) {
                e->gpc = 1; e->host = 0; ++g_alias_hash_removed;
            }
        }
        ++g_batch_hash_scans;
        g_batch_hash_pending = 0;
    }
#endif
#ifdef DBT_INCOMING_BITMAP
#ifdef DBT_VERIFY_INCOMING_BITMAP
    verify_sources(g_dead_banks);
#endif
    while ((i = source_next(&scan)) >= 0) {
#else
    for (i = 0; i < g_nblocks; ++i) {
#endif
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
#endif
}

/* After killing blocks: detach now, or (DBT_LAZY_DETACH) leave it to the
 * next bank recycle or full flush; until then the dead entries redirect. */
static void finish_or_defer(void)
{
#ifndef DBT_LAZY_DETACH
    finish_invalidations();
#endif
}

/* Drop every live block overlapping guest range [lo, hi); user_only skips P1-P3 code. */
static void invalidate_range(sf_u32 lo, sf_u32 hi, int user_only)
{
    int i;
#ifdef DBT_BATCH_RANGES
    g_range_batch = 1;
#endif
    INVAL_CALL(0);
#ifndef DBT_INVAL_RANGE_SCAN
    /* User blocks only (CE's TLB flushes): those are in the page index
     * under their first and last 1 KiB page (blocks are shorter than 1 KiB,
     * so one of the two lies in the range's pages) and in g_user_idx, so
     * neither needs a pass over every header. Default since 2026-11-07;
     * -DDBT_INVAL_RANGE_SCAN for the full scan. */
    if (user_only && hi > lo) {
        if (hi - lo <= 256u * 1024u) {
            sf_u32 page;
            for (page = lo & ~0x3FFu; page < hi; page += 0x400u) {
                unsigned node, bit = user_page_bit(page);
                if (!(g_user_pages[bit >> 5] & (1u << (bit & 31)))) continue;
                for (node = g_page_head[bit]; node != CHAIN_NONE; node = g_page_next[node]) {
                    unsigned j = node >> 1;
                    dbt_block *b = &g_blocks[j];
                    if (g_binfo[j].dead || !g_binfo[j].user) continue;
                    if (b->guest_pc < hi && b->guest_pc + b->guest_len > lo) invalidate_block(b);
                }
            }
        } else {
            int j;
            for (j = 0; j < g_nuser; ++j) {
                dbt_block *b = &g_blocks[g_user_idx[j]];
                if (g_binfo[g_user_idx[j]].dead) continue;
                if (b->guest_pc < hi && b->guest_pc + b->guest_len > lo) invalidate_block(b);
            }
        }
    } else
#endif
    for (i = 0; i < g_nblocks; i++) {
        dbt_block *b = &g_blocks[i];
        if (g_binfo[i].dead || (user_only && !g_binfo[i].user)) continue;
        if (b->guest_pc < hi && b->guest_pc + b->guest_len > lo) invalidate_block(b);
    }
    INVAL_END(0);
    finish_or_defer();
#ifdef DBT_BATCH_RANGES
    g_range_batch = 0;
#endif
}

/* On a switch, untagged user hash entries leave the table. Tagged entries
 * stay but both C and assembly require the current ASID on every user hit.
 * Monomorphic ICs still clear: their generated hit paths have no ASID tag.
 * Blocks stay for return to an ASID or validated shared-physical reuse. */
static sf_u8 g_asid_seen[32] = { 1 }, g_asid_list[64];   /* every ASID the guest has run under (0 from reset): the slots a shared block's hash entries can be in */
static unsigned g_nasid_seen = 1;
static void switch_asid(sf_u32 asid)
{
    int j;
    if (asid == g_asid) { dbt_stat.asid_unchanged++; return; }
    g_asid = asid;
    if (!(g_asid_seen[(asid & 255u) >> 3] & (1u << (asid & 7)))) {
        g_asid_seen[(asid & 255u) >> 3] |= (sf_u8)(1u << (asid & 7));
        if (g_nasid_seen < 64) g_asid_list[g_nasid_seen] = (sf_u8)asid;
        ++g_nasid_seen;
    }
    dbt_stat.asid_switches++;
#ifndef DBT_HASH_ASID_TAG
    for (j = 0; j < (int)g_nuser_hash; j++) {
        unsigned slot = g_user_hash[j];
        hent *e = &g_hash[slot];
        if (IS_USER(e->gpc)) { e->gpc = 1; e->host = 0; HASH_WHY(slot, HD_ASID); }
        g_user_hash_seen[slot] = 0;
    }
    g_nuser_hash = 0;
#endif
    for (j = 0; j < g_nic; j++) {
        unsigned index = g_ic_active[j];
        dbt_block *b = &g_blocks[index];
        volatile sf_u32 *ic = (volatile sf_u32 *)(b->host_base + (sf_u32)b->ic_off);
        if (IS_USER(ic[0])) { ic[0] = 1; ic[1] = 0; }
        g_ic_active_seen[index] = 0;
    }
    g_nic = 0;
}

static void note_asid(void)
{
    sf_u32 a = REG_PTEH & 0xFF;
    if (a != g_asid) switch_asid(a);
}

/* ldtlb executed by the guest: PTEH/PTEL describe the new entry. A page that
 * has blocks and now maps to another physical page loses them. */
static int tlb_drops_block(unsigned i, sf_u32 vpn, sf_u32 mask,
    sf_u32 ptel, sf_u32 asid, sf_u32 shared)
{
    const dbt_block *b;
    if (g_binfo[i].dead || !g_binfo[i].user) return 0;
    b = &g_blocks[i];
    if (b->guest_pc >= vpn + ~mask + 1 || b->guest_pc + b->guest_len <= vpn) return 0;
    if (!shared && g_binfo[i].asid != ASID_ANY && g_binfo[i].asid != asid) return 0;
    return !(ptel & 0x100) || g_binfo[i].phys == PHYS_UNKNOWN ||
        (g_binfo[i].phys & mask) != (ptel & 0x1FFFFC00u & mask);
}
static void tlb_check_block(unsigned i, sf_u32 vpn, sf_u32 mask,
    sf_u32 ptel, sf_u32 asid, sf_u32 shared)
{
    if (tlb_drops_block(i, vpn, mask, ptel, asid, shared)) invalidate_block(&g_blocks[i]);
}

static void tlb_hook(void)
{
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
        return;
    }
    INVAL_CALL(2);
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
    INVAL_END(2);
    finish_or_defer();
    __asm__ volatile ("ldtlb" ::: "memory");
}

#ifdef DBT_TLB_REFILL
/* The user code pages (CE 5.0 revalidation, tlb_reval5 below). One entry per
 * (4 KiB page, ASID) that holds a user block: the physical page it was
 * translated from. A TLB flush then costs one page walk per code page
 * (hundreds) instead of a pass over every user block (up to 19200; 11% of
 * the shell's boot in a heartbeat profile), and only a page whose mapping
 * changed has its blocks looked at, through the page chains. Slot 0 is
 * per process (key carries the ASID), slots 1-63 are shared (0xFF). Open
 * addressing; key 0 empty, 1 deleted; PHYS_UNKNOWN means "treat as
 * changed". Full (3/4) or an unencodable key: g_cpage_over, and the TLB
 * flush falls back to the per-block pass until the next full flush. Two
 * tables so that a bank recycle can copy the live entries into the other. */
#define CPAGE_N 2048
static sf_u32 g_cpage_tab[2][2][CPAGE_N];  /* [table][0 keys / 1 phys] */
static unsigned g_cpage_cur, g_ncpage, g_cpage_over;
#define CPAGE_KEY  (g_cpage_tab[g_cpage_cur][0])
#define CPAGE_PHYS (g_cpage_tab[g_cpage_cur][1])
static sf_u32 cpage_key(sf_u32 va, sf_u32 asid) { return (va & ~0xFFFu) | ((va >> 25) ? 0xFFu : (asid & 0xFFu)); }
static unsigned cpage_slot(sf_u32 key) { return ((key >> 12) * 2654435761u >> 21) & (CPAGE_N - 1); }
static int cpage_find(sf_u32 key)
{
    unsigned i, n;
    for (i = cpage_slot(key), n = 0; n < CPAGE_N; i = (i + 1) & (CPAGE_N - 1), ++n) {
        if (CPAGE_KEY[i] == key) return (int)i;
        if (CPAGE_KEY[i] == 0) return -1;
    }
    return -1;
}
/* weak: an existing entry keeps its physical page (a second page whose
 * UTLB entry is gone by now must not spoil a known one) */
static void cpage_set(sf_u32 va, sf_u32 asid, sf_u32 phys, int weak)
{
    sf_u32 key = cpage_key(va, asid);
    unsigned i, n, del = CPAGE_N;
    if (g_cpage_over) return;
    if (key < 2) { g_cpage_over = 1; return; }
    for (i = cpage_slot(key), n = 0; n < CPAGE_N; i = (i + 1) & (CPAGE_N - 1), ++n) {
        if (CPAGE_KEY[i] == key) {
            if (!weak && CPAGE_PHYS[i] != phys) CPAGE_PHYS[i] = PHYS_UNKNOWN;    /* two mappings seen: the next flush drops the page's blocks */
            return;
        }
        if (CPAGE_KEY[i] == 1) { if (del == CPAGE_N) del = i; }
        else if (CPAGE_KEY[i] == 0) break;
    }
    if (del != CPAGE_N) i = del;
    else if (g_ncpage >= CPAGE_N / 4 * 3) { g_cpage_over = 1; return; }
    else ++g_ncpage;
    CPAGE_KEY[i] = key; CPAGE_PHYS[i] = phys;
}
/* the user blocks touching this page (either end), under asid8 (0xFF: any) */
static void cpage_kill(sf_u32 page, sf_u32 asid8)
{
    sf_u32 sub;
    for (sub = page; sub < page + 0x1000u; sub += 0x400u) {
        unsigned node;
        for (node = g_page_head[user_page_bit(sub)]; node != CHAIN_NONE; node = g_page_next[node]) {
            unsigned i = node >> 1;
            dbt_block *b = &g_blocks[i];
            if (g_binfo[i].dead || !g_binfo[i].user) continue;
            if (b->guest_pc >= page + 0x1000u || b->guest_pc + b->guest_len <= page) continue;
            if (asid8 != 0xFFu && g_binfo[i].asid != asid8 && g_binfo[i].asid != ASID_ANY) continue;
            invalidate_block(b);
        }
    }
}
/* a user block has been committed: its page(s) */
static void cpage_note(unsigned index)
{
    const dbt_block *b = &g_blocks[index];
    sf_u32 asid = g_binfo[index].asid, p1 = g_binfo[index].phys;
    sf_u32 lo = b->guest_pc & ~0xFFFu, hi = (b->guest_pc + b->guest_len - 1) & ~0xFFFu;
    cpage_set(lo, asid, p1 == PHYS_UNKNOWN ? PHYS_UNKNOWN : p1 & ~0xFFFu, 0);
    if (hi != lo) {
        sf_u32 p2 = utlb_translate(hi, asid == ASID_ANY ? g_asid : asid);
        cpage_set(hi, asid, p2 == PHYS_UNKNOWN ? PHYS_UNKNOWN : p2 & ~0xFFFu, p2 == PHYS_UNKNOWN);
    }
}
static void cpage_reset(void)
{
    memset(g_cpage_tab[g_cpage_cur][0], 0, sizeof g_cpage_tab[0][0]);
    g_ncpage = 0; g_cpage_over = 0;
}
/* after a bank recycle: keep the entries of the live user blocks only */
static void cpage_gc(void)
{
    unsigned from = g_cpage_cur, i, j;
    if (g_cpage_over) return;
    g_cpage_cur ^= 1;
    cpage_reset();
    for (j = 0; j < (unsigned)g_nuser; ++j) {
        unsigned index = g_user_idx[j];
        const dbt_block *b = &g_blocks[index];
        sf_u32 lo = b->guest_pc & ~0xFFFu, hi = (b->guest_pc + b->guest_len - 1) & ~0xFFFu, pg;
        if (g_binfo[index].dead) continue;
        for (pg = lo; ; pg = hi) {
            sf_u32 key = cpage_key(pg, g_binfo[index].asid), phys = PHYS_UNKNOWN;
            g_cpage_cur = from;
            i = (unsigned)cpage_find(key);
            if (i < CPAGE_N) phys = g_cpage_tab[from][1][i];
            g_cpage_cur = from ^ 1;
            cpage_set(pg, g_binfo[index].asid, phys, 1);
            if (pg == hi) break;
        }
        /* a block shared across ASIDs in slot 0 was also entered under the
         * other process (find_block_by_gpc); that entry is lost here and the
         * block is dropped at the next flush, as before the table existed */
    }
}
#endif

/* A live block for gpc usable in the current address space. */
static dbt_block *find_block_by_gpc(sf_u32 gpc)
{
    int i;
    sf_u32 phys = 0; int have_phys = 0;
    for (i = g_chain_head[hidx(gpc)]; i != CHAIN_NONE; i = g_chain_next[i]) {
        dbt_block *b = &g_blocks[i];
        if (g_binfo[i].dead || b->guest_pc != gpc) continue;
        if (!g_binfo[i].user || g_binfo[i].asid == g_asid) { touch_code(b->host_base); ref_mark((unsigned)i); return b; }
        if (!have_phys) { phys = utlb_translate(gpc, g_asid); have_phys = 1; }
        if (phys != PHYS_UNKNOWN && g_binfo[i].phys == phys &&
            (gpc >> 10) == ((gpc + b->guest_len - 1) >> 10)) {
            /* This block can now be reached from more than its original
             * ASID. A remap in either process must invalidate it. Restrict
             * reuse to one minimum-size page; the first physical address
             * alone does not validate a second page in the new process. */
            g_binfo[i].asid = ASID_ANY;
#ifdef DBT_TLB_REFILL
            if (gpc < 0x02000000u) cpage_set(gpc & ~0xFFFu, g_asid, phys & ~0xFFFu, 0);   /* slot 0: this process's mapping too */
#endif
            touch_code(b->host_base); ref_mark((unsigned)i); return b;
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
#ifdef DBT_INTERIOR_RANGE_FIRST
        /* Page chains contain unrelated blocks. Reject by guest range
         * before fetching owner state or unpacking entry metadata. Keep
         * dbt_block_entry's full validation for candidates in range. */
        if (gpc <= b->guest_pc || gpc >= b->guest_pc + b->guest_len) continue;
#endif
        if (g_binfo[i].dead || (g_binfo[i].user && g_binfo[i].asid != g_asid)) continue;
#ifdef DBT_LAZY_META
        if (gpc <= b->guest_pc || gpc >= b->guest_pc + b->guest_len) continue;
        ensure_meta(b);
#endif
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
#ifdef DBT_TRACE_FLUSH
    sf_u32 cache_was = g_cache_used, arena_was = g_arena_used;
#endif
    sf_u32 i, sr;
#ifdef DBT_INVAL_PENDING
    finish_invalidations();        /* pending ranges and the dead map must not outlive the cache they describe */
#endif
    /* Interrupts land in the translator's C code and abandon it (the guest
     * resumes at the pc being translated). Clearing 8192 hash slots takes
     * about one tick, so an unmasked flush was restarted by every tick and
     * never finished: the Release boot livelocked at its first full flush
     * once the chain heads made the loop a little longer. Mask IMASK for
     * the flush (one tick of latency, once per cache fill). */
    __asm__ volatile ("stc sr, %0" : "=r"(sr));
    __asm__ volatile ("ldc %0, sr" :: "r"(sr | 0xF0u) : "memory");
    OWNER_CACHE_CLEAR();
    g_nblocks = 0; g_bank = 0; g_allocated = 0;
#ifdef DBT_INCOMING_BITMAP
    memset(g_source_links, 0, sizeof g_source_links);
    memset(g_source_ic, 0, sizeof g_source_ic);
#endif
#ifdef DBT_INTERIOR_HASH
    memset(g_alias_blocks, 0, sizeof g_alias_blocks);
#endif
#if defined(DBT_INTERIOR_CACHE) || defined(DBT_HOST_OWNER_CACHE)
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
#ifdef DBT_BATCH_HASH_REMOVE
    g_batch_hash_pending = 0;
#endif
#endif
    memset(g_bank_count, 0, sizeof g_bank_count);
    memset(g_bank_touch, 0, sizeof g_bank_touch); g_access_clock = 0;
    for (i = 0; i < MAX_BLOCKS; ++i) g_binfo[i].dead = 1;
    g_nuser = 0; g_nic = 0; g_nuser_hash = 0;
#ifndef DBT_HASH_ASID_TAG
    memset(g_user_hash_seen, 0, sizeof g_user_hash_seen);
#endif
    memset(g_ic_active_seen, 0, sizeof g_ic_active_seen);
    memset(g_user_pages, 0, sizeof g_user_pages);
    memset(g_page_head, 0xFF, sizeof g_page_head);
#ifdef DBT_TLB_REFILL
    cpage_reset();
#endif
    g_cache_used = 0;
    g_arena_used = 0;
#if defined(DBT_LAZY_META) || defined(DBT_SECOND_CHANCE)
    for (i = 0; i < CACHE_BANKS; ++i) g_arena_top[i] = i * ((META_ARENA_BYTES / CACHE_BANKS) & ~7u);
#endif
#ifdef DBT_SECOND_CHANCE
    memset(g_ref, 0, sizeof g_ref);
#endif
#ifdef DBT_LAZY_META
    g_lazy_owner = -1;
#endif
    for (i = 0; i < HSIZE; i++) { g_hash[i].gpc = 1; g_hash[i].host = 0; g_chain_head[i] = CHAIN_NONE; HASH_WHY(i, HD_FLUSH_ALL); }
    dbt_stat.flushes++;
    __asm__ volatile ("ldc %0, sr" :: "r"(sr) : "memory");
#ifdef DBT_TRACE_FLUSH
    if (dbt_stat.flushes > 1) {
        dbt_puts("[dbt] full flush "); dbt_puthex(dbt_stat.flushes); dbt_puts(" cache="); dbt_puthex(cache_was);
        dbt_puts(" arena="); dbt_puthex(arena_was); dbt_puts(": "); print_stats();
    }
#endif
}

#ifdef DBT_PRUNE_INDEX_REUSE
/* Remove dead headers before their indices can be reused. Keeping live
 * nodes in their original order preserves newest-first lookup semantics. */
static void prune_index_chain(sf_u16 *head, sf_u16 *next, unsigned shift)
{
    while (*head != CHAIN_NONE) {
        unsigned node = *head;
        if (g_binfo[node >> shift].dead) *head = next[node];
        else head = &next[node];
    }
}
#ifdef DBT_VERIFY_PRUNED_INDEX
/* Test-only audit: no stale node/cycle and every surviving header indexed.
 * Two independent walks keep the verifier separate from the splice logic. */
static void verify_pruned_index(sf_u16 *heads, unsigned count, sf_u16 *next, unsigned shift)
{
    sf_u8 seen[(MAX_BLOCKS * 2 + 7) / 8];
    unsigned h, i;
    memset(seen, 0, sizeof seen);
    for (h = 0; h < count; ++h) {
        unsigned node;
        for (node = heads[h]; node != CHAIN_NONE; node = next[node]) {
            if (node >= ((unsigned)MAX_BLOCKS << shift) || g_binfo[node >> shift].dead)
                dbt_halt("pruned index stale node");
            if (seen[node >> 3] & (1u << (node & 7))) dbt_halt("pruned index cycle/duplicate");
            seen[node >> 3] |= 1u << (node & 7);
        }
    }
    for (i = 0; i < (unsigned)g_nblocks; ++i) {
        unsigned node = i << shift;
        dbt_block *b = &g_blocks[i];
        if (g_binfo[i].dead) continue;
#ifndef DBT_INTERIOR_ENTRY
        if (shift && !g_binfo[i].user) continue;
#endif
        if (!(seen[node >> 3] & (1u << (node & 7)))) dbt_halt("pruned index missing header");
        if (shift && (b->guest_pc >> 10) != ((b->guest_pc + b->guest_len - 1) >> 10)) {
            ++node;
            if (!(seen[node >> 3] & (1u << (node & 7)))) dbt_halt("pruned index missing second page");
        }
    }
}
#endif
#endif

/* Recycle one disjoint code/metadata bank. Surviving host code never moves.
 * Incoming direct chains and ICs are detached before bytes are reused; the
 * guest-PC chains are rebuilt so a recycled table index cannot form a cycle. */
static sf_u32 bank_code_bytes(void) { return (g_cfg.cache_size / CACHE_BANKS) & ~31u; }
static sf_u32 bank_arena_bytes(void) { return (META_ARENA_BYTES / CACHE_BANKS) & ~7u; }
#ifdef DBT_CODE_ALIGN4
#define CODE_ROUND(n) (((n) + 3u) & ~3u)
#else
#define CODE_ROUND(n) (((n) + 31u) & ~31u)
#endif

#if defined(DBT_LAZY_META) || defined(DBT_SECOND_CHANCE)
#ifdef DBT_SECOND_CHANCE
/* Relocation record after the links: pool offset (big-endian u16), then
 * one bit per pool word holding an address inside the block. */
static sf_u8 *reloc_rec(const dbt_block *b) { return (sf_u8 *)b->links + (sf_u32)b->nlinks * sizeof(dbt_link); }
static unsigned reloc_pool(const sf_u8 *r) { return ((unsigned)r[0] << 8) | r[1]; }
static sf_u32 reloc_len_at(sf_u32 host_len, unsigned pool) { return 2 + ((host_len - pool) / 4 + 7) / 8; }
#endif
/* Bytes of the block's chunk (from b->links), rounded. */
static sf_u32 chunk_bytes(const dbt_block *b)
{
    sf_u32 n;
    if (b->packed_meta == META_RESIDENT)
        return (sf_u32)((const sf_u8 *)b->spills - (const sf_u8 *)b->links) +
               arena_round((sf_u32)b->nspills * sizeof(dbt_spill));
    n = (sf_u32)b->nlinks * sizeof(dbt_link);
#ifdef DBT_SECOND_CHANCE
    n += reloc_len_at(b->host_len, reloc_pool(reloc_rec(b)));
#endif
    return arena_round(n);
}
#endif

#ifdef DBT_LAZY_META
static sf_u32 g_lazy_regens, g_lazy_kept, g_lazy_transient, g_lazy_resident;
/* Can the block be translated again later from its physical address with
 * the same result? Its guest bytes [gpc, hi) must be one physically
 * contiguous range we can read without the TLB, and must not change while
 * the block lives: only the ROM image qualifies (RAM code can be rewritten
 * before the guest flushes it). DBT_LAZY_META_ALL drops the ROM test, for
 * the RAM-resident test guests. */
static int lazy_eligible(sf_u32 gpc, sf_u32 phys, sf_u32 hi)
{
    sf_u32 last = hi - 1;
    if (hi <= gpc || phys == PHYS_UNKNOWN) return 0;
    if (gpc >= 0x80000000u && gpc < 0xC0000000u) {
        if ((gpc ^ last) & 0xE0000000u) return 0;                 /* stays in P1 or P2 */
    } else {
        sf_u32 page;                                              /* every further 1 KiB page must follow */
        for (page = (gpc | 0x3FFu) + 1; page - 1 < last; page += 0x400u)
            if (utlb_translate(page, g_asid) != phys + (page - gpc)) return 0;
    }
#ifndef DBT_LAZY_META_ALL
    if (!g_cfg.rom_hi || phys < g_cfg.rom_lo || phys + (hi - gpc) > g_cfg.rom_hi) return 0;
#endif
    return 1;
}

/* Guest pcs whose blocks needed metadata in an earlier life (hashed, never
 * cleared: a stale bit only costs arena). Their next translation keeps it
 * resident instead of paying a rebuild again: 78% of the shell's
 * translations redo evicted code. */
#define META_HINT_BITS 65536u
static sf_u32 g_meta_hint[META_HINT_BITS / 32];
static unsigned meta_hint_bit(sf_u32 gpc) { return ((gpc >> 1) ^ (gpc >> 17)) & (META_HINT_BITS - 1); }
static int meta_hinted(sf_u32 gpc) { unsigned i = meta_hint_bit(gpc); return (g_meta_hint[i >> 5] >> (i & 31)) & 1; }

static dbt_block g_regen;
static sf_u16 g_regen_code[DBT_BLOCK_WORDS] __attribute__((aligned(4)));
#define LAZY_META_BYTES (DBT_MAX_META * sizeof(dbt_imeta) + 8)
static sf_u8 g_lazy_pack[LAZY_META_BYTES] __attribute__((aligned(8)));
/* Overflow home when the block's bank arena is full: one block at a time. */
static sf_u8 g_lazy_tmp[LAZY_META_BYTES + DBT_MAX_BLOCK_SPILLS * sizeof(dbt_spill)] __attribute__((aligned(8)));
static void regen_diverged(const dbt_block *b, const char *what)
{
    dbt_puts("[dbt] metadata rebuild of block "); dbt_puthex(b->guest_pc); dbt_puts(" at ");
    dbt_puthex(b->host_base); dbt_puts(": "); dbt_puts(what); dbt_puts("\n");
    dbt_halt("metadata rebuild diverged");
}

/* Recovery metadata of a block translated without it: translate the guest
 * code again at the block's own host address into a scratch buffer, check
 * the copy against the live code (outside the link windows chaining
 * rewrites) and keep the metadata in the block's bank arena, or in the
 * one-block overflow buffer. Interrupt-safe: an interrupt abandons the
 * rebuild before the masked commit. */
static void ensure_meta(const dbt_block *cb)
{
    dbt_block *b = (dbt_block *)cb;
    unsigned index = (unsigned)(b - g_blocks), bank;
    regen_ctx rc;
    dbt_env env;
    sf_u32 fault = 0, sr, nm, ns, need, top;
    const sf_u16 *live = (const sf_u16 *)b->host_base;
    sf_u8 *dst;
    int r, k;
    unsigned w;
    if (b->meta) return;
    rc.gpc = b->guest_pc;
    rc.base = (b->guest_pc >= 0x80000000u && b->guest_pc < 0xC0000000u) ? b->guest_pc : (0x80000000u | g_binfo[index].phys);
    env = g_env;
    env.ctx = &rc; env.fetch16 = regen_fetch16; env.read16le = regen_read16le; env.read32le = regen_read32le;
#ifdef DBT_SECOND_CHANCE
    env.reloc_map = 0;
#endif
    g_regen.meta = g_scratch_meta; g_regen.spills = g_scratch_spills; g_regen.links = g_scratch_links;
    r = dbt_translate_block(&env, b->guest_pc, b->host_base, g_regen_code, DBT_BLOCK_WORDS, &g_regen, &fault);
    ++g_lazy_regens;
    { unsigned i = meta_hint_bit(b->guest_pc); g_meta_hint[i >> 5] |= 1u << (i & 31); }
    if (r != DBT_OK) regen_diverged(b, "translation failed");
    if (g_regen.guest_len != b->guest_len || g_regen.host_len != b->host_len || g_regen.code_len != b->code_len ||
        g_regen.nmeta != b->nmeta || g_regen.nspills != b->nspills || g_regen.nlinks != b->nlinks ||
        g_regen.ic_off != b->ic_off || g_regen.borrow != b->borrow || g_regen.end_reason != b->end_reason)
        regen_diverged(b, "different shape");
    for (k = 0; k < b->nlinks; ++k)
        if (g_scratch_links[k].jump_off != b->links[k].jump_off || g_scratch_links[k].lit_off != b->links[k].lit_off ||
            g_scratch_links[k].stub_off != b->links[k].stub_off)
            regen_diverged(b, "different links");
    for (w = 0; w < (unsigned)b->code_len / 2; ++w) {
        unsigned byte = w * 2;
        for (k = 0; k < b->nlinks; ++k)
            if (byte + 2 >= b->links[k].jump_off && byte < (unsigned)b->links[k].jump_off + 6) break;
        if (k == b->nlinks && g_regen_code[w] != live[w]) regen_diverged(b, "different code");
    }
    nm = dbt_pack_meta(g_lazy_pack, &g_regen);
    ns = (sf_u32)g_regen.nspills * sizeof(dbt_spill);
    need = arena_round(nm) + arena_round(ns);
    bank = bank_of_code(b->host_base);
    __asm__ volatile ("stc sr, %0" : "=r"(sr));
    __asm__ volatile ("ldc %0, sr" :: "r"(sr | SR_IMASK) : "memory");
    top = bank == g_bank ? g_arena_used : g_arena_top[bank];
    if (top + need <= (bank + 1) * bank_arena_bytes()) {
        dst = g_arena + top;
        if (bank == g_bank) g_arena_used = top + need; else g_arena_top[bank] = top + need;
        ++g_lazy_kept;
    } else {
        if (g_lazy_owner >= 0 && g_blocks[g_lazy_owner].meta == (dbt_imeta *)g_lazy_tmp)
            g_blocks[g_lazy_owner].meta = 0;
        dst = g_lazy_tmp; g_lazy_owner = (int)index;
        ++g_lazy_transient;
    }
    memcpy(dst, g_lazy_pack, nm);
    memcpy(dst + arena_round(nm), g_scratch_spills, ns);
    b->spills = (dbt_spill *)(dst + arena_round(nm));
    b->packed_meta = 1;
    b->meta = (dbt_imeta *)dst;
    __asm__ volatile ("ldc %0, sr" :: "r"(sr) : "memory");
}
#else
#define ensure_meta(b) ((void)0)
#endif

#ifdef DBT_SECOND_CHANCE
static void copy_down(sf_u8 *d, const sf_u8 *s, sf_u32 n)
{
    /* d <= s: a forward copy never reads a byte it has already overwritten */
    if (d != s) memcpy(d, s, n);
}

/* Put a bank on probation: nothing reaches its blocks except through the C
 * lookup (hash entries, inline caches and direct chains into it are
 * dropped), which marks each block it finds. Called with IRQs masked. */
static void detach_bank(unsigned bank)
{
    sf_u32 lo = g_cfg.cache_base + bank * bank_code_bytes(), hi = lo + bank_code_bytes();
    unsigned i, first = bank * BANK_BLOCKS, count = g_bank_count[bank];
    if (!count) return;
    ++g_probations;
    for (i = first; i < first + count; ++i) ref_clear(i);
    for (i = 0; i < (unsigned)g_nblocks; ++i) {
        dbt_block *b = &g_blocks[i];
        int k;
        if (g_binfo[i].dead) continue;
        if (b->ic_off >= 0) {
            volatile sf_u32 *ic = (volatile sf_u32 *)(b->host_base + (sf_u32)b->ic_off);
            if (ic[1] >= lo && ic[1] < hi) reset_ic(b);
        }
        if (!(g_link_banks[i] & (1u << bank))) continue;
        for (k = 0; k < b->nlinks; ++k) {
            sf_u32 v = *(volatile sf_u32 *)(b->host_base + (sf_u32)b->links[k].lit_off);
            if (v >= lo && v < hi) unchain(b, k);
        }
        g_link_banks[i] &= (sf_u16)~(1u << bank);     /* no link into the bank is left */
    }
    for (i = 0; i < HSIZE; ++i)
        if (g_hash[i].host >= lo && g_hash[i].host < hi) { g_hash[i].gpc = 1; g_hash[i].host = 0; HASH_WHY(i, HD_PROBATION); }
}

/* Move the blocks of the recycled bank that were used while it was on
 * probation to its start, in order (headers stay sorted by host address),
 * at most 1/DBT_SECOND_CHANCE_SHARE of the bank. All of the bank's blocks
 * are already dead and nothing outside points into it any more. A moved
 * block is rebased (relocation record), unchained and its IC reset; its
 * metadata built on demand is dropped (outside the chunk). Returns the
 * number kept; *code and *arena get the bytes they use. IRQs masked. */
static unsigned keep_survivors(unsigned bank, unsigned count, sf_u32 *code, sf_u32 *arena, const unsigned char *live)
{
    unsigned first = bank * BANK_BLOCKS, i, n = 0;
    sf_u32 lo = g_cfg.cache_base + bank * bank_code_bytes();
    sf_u32 code_max = bank_code_bytes() / DBT_SECOND_CHANCE_SHARE, arena_max = bank_arena_bytes() / DBT_SECOND_CHANCE_SHARE;
    sf_u32 c = 0, a = 0;
    for (i = first; i < first + count; ++i) {
        dbt_block nb, *b = &g_blocks[i];
        unsigned j = first + n;
        sf_u32 len, clen, base, delta, adelta;
        sf_u8 *rec;
        unsigned pool, words, w;
        int k;
        if (!live[i - first] || !ref_test(i)) continue;
        len = CODE_ROUND((sf_u32)b->host_len); clen = chunk_bytes(b);
        if (c + len > code_max || a + clen > arena_max || n >= BANK_BLOCKS / DBT_SECOND_CHANCE_SHARE) break;
        nb = *b;
        base = lo + c; delta = base - b->host_base;
        copy_down((sf_u8 *)base, (const sf_u8 *)b->host_base, b->host_len);
        adelta = (sf_u32)(g_arena + bank * bank_arena_bytes() + a) - (sf_u32)b->links;
        copy_down(g_arena + bank * bank_arena_bytes() + a, (const sf_u8 *)b->links, clen);
        nb.host_base = base;
        nb.links = (dbt_link *)((sf_u8 *)nb.links + adelta);
        if (nb.packed_meta == META_RESIDENT) {
            nb.meta = (dbt_imeta *)((sf_u8 *)nb.meta + adelta);
            nb.spills = (dbt_spill *)((sf_u8 *)nb.spills + adelta);
        } else {
            nb.meta = 0; nb.spills = 0;
#ifdef DBT_LAZY_META
            if (g_lazy_owner == (int)i) g_lazy_owner = -1;
#endif
        }
        g_blocks[j] = nb;
        g_binfo[j] = g_binfo[i]; g_binfo[j].dead = 0;
        g_link_banks[j] = 0;
        g_ic_site[j] = nb.ic_off >= 0 ? base + (sf_u32)nb.ic_off : 0;
        ref_clear(j);
        /* rebase the pool words that hold addresses inside the block */
        rec = reloc_rec(&nb); pool = reloc_pool(rec); words = (nb.host_len - pool) / 4;
        for (w = 0; w < words; ++w)
            if (rec[2 + (w >> 3)] & (1u << (w & 7))) *(volatile sf_u32 *)(base + pool + 4 * w) += delta;
        /* unchain: original jump words, link literal back at the own stub */
        for (k = 0; k < nb.nlinks; ++k) {
            sf_u16 *wd = (sf_u16 *)(base + (sf_u32)nb.links[k].jump_off);
            wd[0] = nb.links[k].w0; wd[1] = nb.links[k].w1; wd[2] = nb.links[k].w2;
            if (nb.links[k].cond) wd[-1] = nb.links[k].skip_w;
            *(volatile sf_u32 *)(base + (sf_u32)nb.links[k].lit_off) = base + (sf_u32)nb.links[k].stub_off;
        }
        if (nb.ic_off >= 0) reset_ic(&g_blocks[j]);
        set_ic_index(j);
        dbt_sync_code(base, (sf_u32)nb.host_len);
        c += len; a += clen; ++n;
        g_survivor_bytes += nb.host_len;
    }
    g_survivors += n;
    *code = c; *arena = a;
    return n;
}
#endif
static void cache_next_bank(void)
{
    sf_u32 sr, i, lo, hi;
#ifdef DBT_INVAL_PENDING
    finish_invalidations();        /* dead ranges must be detached before their memory is reused */
#endif
#ifdef DBT_KERNEL_BANKS
    unsigned stream = g_bank >= DBT_KERNEL_BANKS;
    unsigned bank = stream ? DBT_KERNEL_BANKS + (g_bank - DBT_KERNEL_BANKS + 1) % (CACHE_BANKS - DBT_KERNEL_BANKS)
                           : (g_bank + 1) % DBT_KERNEL_BANKS;
#else
    unsigned bank = (g_bank + 1) % CACHE_BANKS;
#endif
    unsigned first, count, kept = 0;
    sf_u32 oldest = 0, kept_code = 0, kept_arena = 0;
    (void)oldest;
#ifdef DBT_SECOND_CHANCE
    static unsigned char live[BANK_BLOCKS];
#endif
#ifdef DBT_CACHE_TICK_LFU
#if defined(DBT_CACHE_LRU) || defined(DBT_KERNEL_BANKS)
#error DBT_CACHE_TICK_LFU requires the unified cache without C-lookup LRU
#endif
    {
        sf_u32 least = 0xFFFFFFFFu;
        /* Start in FIFO order to avoid a low-index preference on ties.
         * Only actual timer samples contribute; a cold translation does
         * not gain protection merely by missing the lookup cache. */
        for (i = 1; i < CACHE_BANKS; ++i) {
            unsigned candidate = (g_bank + i) % CACHE_BANKS;
            if (!g_bank_count[candidate]) { bank = candidate; break; }
            if (g_bank_touch[candidate] < least) {
                least = g_bank_touch[candidate]; bank = candidate;
            }
        }
        for (i = 0; i < CACHE_BANKS; ++i) g_bank_touch[i] >>= 1;
        g_bank_touch[bank] = 0;
    }
#endif
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
    OWNER_CACHE_CLEAR();
    lo = g_cfg.cache_base + bank * bank_code_bytes();
    hi = lo + bank_code_bytes();
    if (count) {
#ifdef DBT_CORE_BANKS
        if (bank < DBT_KERNEL_BANKS) ++g_core_evictions;
#endif
#if defined(DBT_INTERIOR_CACHE) || defined(DBT_HOST_OWNER_CACHE)
        ++g_bank_generation[bank];
#endif
        g_nic = 0;
        memset(g_ic_active_seen, 0, sizeof g_ic_active_seen);
#ifdef DBT_SECOND_CHANCE
        for (i = first; i < first + count; ++i) live[i - first] = !g_binfo[i].dead;
#endif
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
#ifdef DBT_PRUNE_LINK_BANKS
            /* The entire destination bank is gone, and IRQs are masked.
             * link_direct will set this bit again if a later fill links
             * into its replacement. Partial-block invalidation cannot do
             * this, since other destinations in the same bank may live. */
            g_link_banks[i] &= (sf_u16)~(1u << bank);
#endif
        }
#ifdef DBT_SECOND_CHANCE
        /* Nothing outside points into the bank now: move the blocks used
         * during its probation to its start. The index rebuild below then
         * includes them; their hash entries come back at the next C lookup. */
        kept = keep_survivors(bank, count, &kept_code, &kept_arena, live);
#endif
        g_nuser = 0; g_nuser_hash = 0;
#ifndef DBT_HASH_ASID_TAG
        memset(g_user_hash_seen, 0, sizeof g_user_hash_seen);
#endif
#ifdef DBT_PRUNE_INDEX_REUSE
        /* User-page bits are conservative: stale bits only cause extra
         * TLB checks, never a missed invalidation. */
        for (i = 0; i < sizeof g_page_head / sizeof g_page_head[0]; ++i)
            prune_index_chain(&g_page_head[i], g_page_next, 1);
#else
        memset(g_user_pages, 0, sizeof g_user_pages);
        memset(g_page_head, 0xFF, sizeof g_page_head);
#endif
        for (i = 0; i < HSIZE; ++i) {
            hent *e = &g_hash[i];
            if (e->host >= lo && e->host < hi) { e->gpc = 1; e->host = 0; HASH_WHY(i, HD_RECYCLE); }
#ifndef DBT_HASH_ASID_TAG
            else if (e->host && e->gpc != 1 && IS_USER(e->gpc)) {
                g_user_hash[g_nuser_hash++] = (sf_u16)i;
                g_user_hash_seen[i] = 1;
            }
#endif
#ifdef DBT_PRUNE_INDEX_REUSE
            prune_index_chain(&g_chain_head[i], g_chain_next, 0);
#else
            g_chain_head[i] = CHAIN_NONE;
#endif
        }
        for (i = 0; i < (unsigned)g_nblocks; ++i) {
            dbt_block *b = &g_blocks[i];
#ifdef DBT_PRUNE_INDEX_REUSE
            (void)b;
#endif
            if (g_binfo[i].dead) continue;
#ifndef DBT_PRUNE_INDEX_REUSE
            unsigned h = hidx(b->guest_pc);
            g_chain_next[i] = g_chain_head[h]; g_chain_head[h] = (sf_u16)i;
#ifdef DBT_INTERIOR_ENTRY
            index_user_block(i);
#endif
#endif
            if (g_binfo[i].user) {
                g_user_idx[g_nuser++] = (sf_u16)i;
#if !defined(DBT_INTERIOR_ENTRY) && !defined(DBT_PRUNE_INDEX_REUSE)
                index_user_block(i);
#endif
            }
        }
#if defined(DBT_PRUNE_INDEX_REUSE) && defined(DBT_VERIFY_PRUNED_INDEX)
        verify_pruned_index(g_chain_head, HSIZE, g_chain_next, 0);
        verify_pruned_index(g_page_head, 8192, g_page_next, 1);
#endif
#ifdef DBT_TLB_REFILL
        cpage_gc();
#endif
        g_allocated -= count - kept;
        g_bank_evictions++;
        dbt_stat.flushes++; /* generation guard: callers must not patch a recycled site */
    }
#ifdef DBT_BATCH_INVALIDATE
    if (((lo - g_cfg.cache_base) & 255u) == 0 && ((hi - lo) & 255u) == 0)
        memset(g_dead_code + (lo - g_cfg.cache_base) / 256, 0, (hi - lo) / 256);
    else for (i = (lo - g_cfg.cache_base) >> 5; i < (hi - g_cfg.cache_base) >> 5; ++i)
        g_dead_code[i >> 3] &= (sf_u8)~(1u << (i & 7));
#endif
#if defined(DBT_LAZY_META) || defined(DBT_SECOND_CHANCE)
    g_arena_top[g_bank] = g_arena_used;
#endif
    g_bank_count[bank] = kept; g_bank = bank;
#ifdef DBT_KERNEL_BANKS
    g_stream_bank[stream] = bank;
#endif
    g_cache_used = bank * bank_code_bytes() + kept_code;
    g_arena_used = bank * bank_arena_bytes() + kept_arena;
#ifdef DBT_SECOND_CHANCE
    detach_bank((bank + 1) % CACHE_BANKS);      /* the next one to be recycled */
#endif
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
    g_env.fpu_fr = (sf_u32)g_fpu.fr; g_env.fpu_fpscr = (sf_u32)&g_fpu.fpscr; g_env.fpu_fpul = (sf_u32)&g_fpu.fpul;
    g_env.rt_fallback = (sf_u32)&dbt_fallback_stub;
    g_env.rt_ldc_sr = (sf_u32)&dbt_ldc_sr_stub;
    g_env.shadow_vbr_addr = (sf_u32)&dbt_shadow_vbr;
    g_env.shadow_md_addr = (sf_u32)&dbt_shadow_md;
#ifdef DBT_SAME_ASID_PTEH_STORE
    g_env.current_asid_addr = (sf_u32)&dbt_current_asid;
#endif
#ifdef DBT_RUNTIME_MAX_INSNS
    g_env.max_insns = DBT_RUNTIME_MAX_INSNS;
#else
    g_env.max_insns = DBT_MAX_INSNS;
#endif
#ifdef DBT_NO_BORROW
    g_env.no_borrow = 1;                 /* experiments: translate without the borrowed register */
#endif
    g_env.dev_lo = cfg->dev_lo; g_env.dev_hi = cfg->dev_hi;
    g_env.dev_ranges = cfg->ndev_ranges ? cfg->dev_ranges : 0; g_env.ndev_ranges = cfg->ndev_ranges;
#ifdef DBT_LAZY_META
    g_env.fetch16 = xl_fetch16; g_env.read16le = xl_read16le; g_env.read32le = xl_read32le;
#endif
#ifdef DBT_SECOND_CHANCE
    g_env.reloc_map = g_reloc_map; g_env.reloc_pool = &g_reloc_pool;
#endif
#ifdef DBT_TRACE_EXEC
    g_env.exec_ctl = (sf_u32)&g_exec_ctl; g_env.exec_ring = (sf_u32)g_exec_ring;   /* after the memset above */
#endif
}

static dbt_block *translate_at(sf_u32 gpc)
{
    dbt_block *b;
    sf_u32 host;
    sf_u32 fault = 0, publish_sr;
    int r;
    unsigned index;
#ifdef DBT_KERNEL_BANKS
    /* Keep transient user translations from evicting the kernel working set
     * (and vice versa). Each stream retains its own active bank and cursor. */
    g_saved_code[g_bank] = g_cache_used; g_saved_arena[g_bank] = g_arena_used;
#ifdef DBT_CORE_BANKS
    /* Reserve the hot stream for the identified kernel image, not all
     * kernel-mode DLLs (GWES/display/filesystem can exceed that stream). */
    {
        sf_u32 pc = gpc & 0x1fffffffu;
        int core = !IS_USER(gpc) && g_env.dev_hi &&
            pc >= (g_env.dev_lo & 0x1fffffffu) && pc < (g_env.dev_hi & 0x1fffffffu);
        g_bank = g_stream_bank[core ? 0 : 1];
        if (core) ++g_core_translates; else ++g_other_translates;
    }
#else
    g_bank = g_stream_bank[IS_USER(gpc) ? 1 : 0];
#endif
    g_cache_used = g_saved_code[g_bank]; g_arena_used = g_saved_arena[g_bank];
#endif
    if (!guest_pc_fetchable(gpc)) { dbt_stat.synth_exceptions++; deliver_synthetic(gpc, 0x0E0); }
    if (g_bank_count[g_bank] >= BANK_BLOCKS ||
        g_cache_used + DBT_BLOCK_WORDS * 2 > (g_bank + 1) * bank_code_bytes() ||
        g_arena_used + SCRATCH_ARENA_NEED > (g_bank + 1) * bank_arena_bytes()) {
#ifdef DBT_HASH_DIAG
        /* why banks fill: 11 headers, 12 code, 13 metadata (several may hold) */
        if (g_bank_count[g_bank] >= BANK_BLOCKS) ++g_hash_diag[11];
        if (g_cache_used + DBT_BLOCK_WORDS * 2 > (g_bank + 1) * bank_code_bytes()) ++g_hash_diag[12];
        if (g_arena_used + SCRATCH_ARENA_NEED > (g_bank + 1) * bank_arena_bytes()) ++g_hash_diag[13];
        /* 14-16: headers, code bytes and metadata bytes of the full bank, summed */
        g_hash_diag[14] += g_bank_count[g_bank];
        g_hash_diag[15] += g_cache_used - g_bank * bank_code_bytes();
        g_hash_diag[16] += g_arena_used - g_bank * bank_arena_bytes();
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
#ifdef DBT_LAZY_META
    g_read_hi = gpc;
#endif
#ifdef DBT_BODY_BUDGET
    {
        /* Keep failed speculative output unpublished. A private environment
         * avoids changing the global limit if an interrupt abandons a fill. */
        dbt_env attempt = g_env;
        do {
            r = dbt_translate_block(&attempt, gpc, host, (sf_u16 *)host, DBT_BLOCK_WORDS, &g_scratch, &fault);
            if (r != DBT_ERR_OVERFLOW || attempt.max_insns <= 1) break;
            attempt.max_insns = (attempt.max_insns + 1) / 2;
        } while (1);
    }
#else
    r = dbt_translate_block(&g_env, gpc, host, (sf_u16 *)host, DBT_BLOCK_WORDS, &g_scratch, &fault);
#endif
    g_translating = 0;
    index = g_bank * BANK_BLOCKS + g_bank_count[g_bank];
    b = &g_blocks[index];
    if (r == DBT_OK) {
        /* commit: block header into the table, used metadata into the arena */
        sf_u32 nm, ns = (sf_u32)g_scratch.nspills * sizeof(dbt_spill);
        *b = g_scratch;
#if defined(DBT_CONST_DEVICE_ADDRESS) || defined(DBT_UNIFORM_STORE)
        if (b->const_specialized) ++g_const_device_blocks;
#endif
#if defined(DBT_LAZY_META) || defined(DBT_SECOND_CHANCE)
        {
            /* one chunk: links, relocation record, resident metadata */
            sf_u32 used = (sf_u32)b->nlinks * sizeof(dbt_link);
            int resident = 1;
            b->links = (dbt_link *)(g_arena + g_arena_used);
            memcpy(b->links, g_scratch_links, used);
#ifdef DBT_SECOND_CHANCE
            {
                sf_u8 *rec = (sf_u8 *)b->links + used;
                sf_u32 n = reloc_len_at(b->host_len, g_reloc_pool);
                rec[0] = (sf_u8)(g_reloc_pool >> 8); rec[1] = (sf_u8)g_reloc_pool;
                memcpy(rec + 2, g_reloc_map, n - 2);
                used += n;
            }
#endif
            g_arena_used += arena_round(used);
#ifdef DBT_LAZY_META
            resident = meta_hinted(gpc) || !lazy_eligible(gpc, (IS_USER(gpc) || gpc >= 0xC0000000u) ? utlb_translate(gpc, g_asid) : (gpc & 0x1FFFFFFFu), g_read_hi);
            if (resident) ++g_lazy_resident;
#endif
            b->meta = 0; b->spills = 0; b->packed_meta = 1;
            if (resident) {
                b->meta = (dbt_imeta *)(g_arena + g_arena_used);
                nm = dbt_pack_meta(b->meta, &g_scratch);
                g_arena_used += arena_round(nm);
                b->spills = (dbt_spill *)(g_arena + g_arena_used); memcpy(b->spills, g_scratch_spills, ns); g_arena_used += arena_round(ns);
                b->packed_meta = META_RESIDENT;
            }
        }
#else
        b->meta = (dbt_imeta *)(g_arena + g_arena_used);
        nm = dbt_pack_meta(b->meta, &g_scratch); b->packed_meta = 1;
        g_arena_used += arena_round(nm);
        b->spills = (dbt_spill *)(g_arena + g_arena_used); memcpy(b->spills, g_scratch_spills, ns); g_arena_used += arena_round(ns);
        b->links = (dbt_link *)(g_arena + g_arena_used);
        memcpy(b->links, g_scratch_links, (sf_u32)b->nlinks * sizeof(dbt_link));
        g_arena_used += arena_round((sf_u32)b->nlinks * sizeof(dbt_link));
#endif
        dbt_stat.arena_bytes = g_arena_used;
    }
    if (r != DBT_OK) {
        dbt_puts("[dbt] translate error "); dbt_puthex((sf_u32)r);
        dbt_puts(" at "); dbt_puthex(gpc); dbt_puts(" fault "); dbt_puthex(fault); dbt_puts("\n");
        dbt_halt("translation failed");
    }
#ifdef DBT_CODE_ALIGN4
    g_cache_used += ((sf_u32)b->host_len + 3u) & ~3u;
#else
    g_cache_used += ((sf_u32)b->host_len + 31u) & ~31u;
#endif
    dbt_sync_code(host, (sf_u32)b->host_len);     /* before anything can find the block: see below */
    __asm__ volatile ("stc sr, %0" : "=r"(publish_sr));
    __asm__ volatile ("ldc %0, sr" :: "r"(publish_sr | SR_IMASK) : "memory");
    {
        binfo *bi = &g_binfo[index];
        g_link_banks[index] = 0;
        g_ic_site[index] = b->ic_off >= 0 ? host + (sf_u32)b->ic_off : 0;
        set_ic_index(index);
#ifdef DBT_INCOMING_BITMAP
        {
            unsigned bank, word = index >> 5;
            sf_u32 bit = 1u << (index & 31);
            for (bank = 0; bank < CACHE_BANKS; ++bank) g_source_links[bank][word] &= ~bit;
            if (b->ic_off >= 0) g_source_ic[word] |= bit;
            else g_source_ic[word] &= ~bit;
        }
#endif
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
#ifdef DBT_TLB_REFILL
            cpage_note(index);
#endif
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
#ifdef DBT_TRACE_XLATE
    /* -DDBT_TRACE_XLATE: one line per translation with the key the cache
     * uses (gpc, ASID for user blocks, physical address) for working-set
     * analysis: cold vs evicted-and-retranslated vs per-ASID duplicates */
    dbt_puts("[xl] "); dbt_puthex(gpc); dbt_putc(' ');
    dbt_puthex((sf_u32)g_binfo[index].asid); dbt_putc(' ');
    dbt_puthex(g_binfo[index].phys); dbt_putc(' ');
    dbt_puthex((sf_u32)b->guest_len); dbt_putc('\n');
#endif
#if defined(DBT_TRACE_LINK) || defined(DBT_TRACE_BLOCKS)
    /* -DDBT_TRACE_BLOCKS (EXTRA_CFLAGS) prints every block translated: the
     * cheapest way to follow a guest that dies without a word */
    dbt_puts("[dbt] translate gpc="); dbt_puthex(gpc); dbt_puts(" host="); dbt_puthex(host);
    dbt_puts(" len="); dbt_puthex((sf_u32)b->guest_len); dbt_puts(" hostlen="); dbt_puthex((sf_u32)b->host_len);
    dbt_puts(" end="); dbt_puthex((sf_u32)b->end_reason); dbt_puts("\n");
#endif
    dbt_stat.translations++;
    dbt_stat.blocks = g_allocated;
#ifdef DBT_STATS_EVERY
    /* -DDBT_STATS_EVERY=n: the counters every n translations, for runs
     * that never reach HC_STATS (the benchmark ends at a frame match) */
    if (dbt_stat.translations % (DBT_STATS_EVERY) == 0) print_stats();
#endif
    return b;
}

static sf_u32 lookup_or_translate(sf_u32 gpc)
{
    sf_u32 host;
    note_asid();
#ifdef DBT_EARLY_FETCH_FAULT
    /* CE's SH system-call targets are instruction-address faults, never
     * code-cache candidates. Preserve the same saved guest context and
     * synthetic vector used by translate_at, without futile cache scans. */
    if (!guest_pc_fetchable(gpc)) {
        dbt_stat.synth_exceptions++;
        deliver_synthetic(gpc, 0x0E0);
    }
#endif
    host = hash_find(gpc);
#ifdef DBT_INTERIOR_CACHE
    if (!host) host = find_cached_entry(gpc);
#endif
    if (!host) {
        dbt_block *b = find_block_by_gpc(gpc);     /* evicted from the hash by a collision */
#ifdef DBT_HASH_DIAG
        {
            unsigned slot = fast_hidx(gpc, g_asid);
            ++g_hash_diag[HD_MISSES];
            if (!b) ++g_hash_diag[HD_TRANSLATED];
            else if (g_hash[slot].gpc != 1) ++g_hash_diag[HD_COLLIDE];
            else ++g_hash_diag[g_hash_why[slot]];
        }
#endif
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

#ifdef DBT_HOST_OWNER_CACHE
/* Ownership survives guest invalidation, but not bank reuse. A matching
 * generation and explicit host range keep recycled headers out of hits. */
static struct { dbt_block *block; sf_u32 generation; } g_host_owner[256];
#endif

/* Block containing a host address (blocks are sorted by host_base). */
static dbt_block *block_of_host(sf_u32 host)
{
    unsigned bank;
    int lo, hi;
    if (host < g_cfg.cache_base || host >= g_cfg.cache_base + CACHE_BANKS * bank_code_bytes()) return 0;
    bank = bank_of_code(host);
#ifdef DBT_HOST_OWNER_CACHE
    unsigned slot = (host >> 4) & 255u;
    dbt_block *cached = g_host_owner[slot].block;
    if (cached && g_host_owner[slot].generation == g_bank_generation[bank] &&
        host >= cached->host_base && host < cached->host_base + (sf_u32)cached->host_len) {
        touch_code(host);
        return cached;
    }
#endif
#ifdef DBT_HASH_DIAG
    ++g_hash_diag[HD_OWNER_CALLS];
#endif
#ifdef DBT_OWNER_CACHE
    unsigned slot = (host >> 5) & 255u, c = g_owner_cache[slot];
    if (c) {
        dbt_block *b = &g_blocks[c - 1];
        if (!g_binfo[c - 1].dead && host - b->host_base < (sf_u32)b->host_len) {
#ifdef DBT_HASH_DIAG
            ++g_hash_diag[HD_OWNER_HITS];
#endif
            touch_code(host); return b;
        }
    }
#endif
    lo = (int)(bank * BANK_BLOCKS); hi = lo + (int)g_bank_count[bank] - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        dbt_block *b = &g_blocks[mid];
        if (host < b->host_base) hi = mid - 1;
        else if (host >= b->host_base + (sf_u32)b->host_len) lo = mid + 1;
        else {
#ifdef DBT_HOST_OWNER_CACHE
            g_host_owner[slot].block = b;
            g_host_owner[slot].generation = g_bank_generation[bank];
#endif
#ifdef DBT_OWNER_CACHE
            g_owner_cache[slot] = (sf_u16)(mid + 1);
#endif
            touch_code(host); return b;
        }
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
#ifdef DBT_INCOMING_BITMAP
    {
        unsigned index = (unsigned)(b - g_blocks);
        g_source_links[bank_of_code(host)][index >> 5] |= 1u << (index & 31);
    }
#endif
    for (k = 0; k < b->nlinks; k++) {
        if (b->host_base + (sf_u32)b->links[k].lit_off == link_lit) {
            sf_u32 jaddr = b->host_base + (sf_u32)b->links[k].jump_off, from;
            /* a block's back edge to itself enters past the borrowed
             * register's park (its first word) and skips the restore: the
             * register stays borrowed across iterations. Not under the
             * execution trace, whose prologue comes first. */
            int self = b->borrow >= 0 && host == b->host_base && !g_env.exec_ring;
            int no_restore = self;
#ifdef DBT_EXIT_NOP_TAIL
            no_restore |= b->borrow < 0;
#endif
            int n = dbt_chain_direct((sf_u16 *)jaddr, jaddr, self ? host + 2 : host, b->links[k].cond, b->links[k].backward, no_restore, &from);
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
    g_tail_from = 1;
    return host;
}

void dbt_invalidate(sf_u32 lo, sf_u32 hi)
{
    invalidate_range(lo, hi, 0);
}

#ifdef DBT_TLB_REFILL
static int tlb_reval5(void);           /* below, with tlb_refill5 */
#endif
/* "Every user mapping may have changed": MMUCR.TI written by the guest, a
 * range-less HC_TLB_INVAL, a wake from R-standby. CE 5.0's page tables say
 * what did change (tlb_reval5 below, when its refill handler is recognised);
 * otherwise every user block goes. */
#ifdef DBT_TLB_REFILL
static sf_u32 g_umc_mark;          /* the event count after the last revalidation: a repeat with nothing in between is skipped */
#define UMC_EVENTS() (dbt_stat.exceptions + dbt_stat.translations + dbt_stat.tlb_hooks + dbt_stat.invalidations)
#endif
static void user_mappings_changed(int in_exception)
{
#ifdef DBT_TLB_REFILL
    /* The OAL writes TI and then traps HC_TLB_INVAL for the same change:
     * the trap is the only event between the two (in_exception: it has
     * been counted), so the second walk would find what the first did. */
    if (UMC_EVENTS() - (sf_u32)in_exception == g_umc_mark) { dbt_stat.tlb_reval_skips++; return; }
    if (tlb_reval5()) { g_umc_mark = UMC_EVENTS(); return; }
#endif
    invalidate_range(0, 0x80000000u, 1);
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
#ifdef DBT_BATCH_RANGES
    g_range_batch = 1;
#endif
    INVAL_CALL(1);
    for (i = 0; i < g_nblocks; i++) {
        dbt_block *b = &g_blocks[i];
        sf_u32 pa;
        if (g_binfo[i].dead) continue;
        pa = (b->guest_pc >= 0x80000000u && b->guest_pc < 0xC0000000u) ? (b->guest_pc & 0x1FFFFFFFu) : g_binfo[i].phys;
        if (pa != PHYS_UNKNOWN && ((g_cfg.rom_hi && pa >= g_cfg.rom_lo && pa + b->guest_len <= g_cfg.rom_hi) ||
                                   (g_cfg.rom2_hi && pa >= g_cfg.rom2_lo && pa + b->guest_len <= g_cfg.rom2_hi))) { kept++; continue; }
#ifdef DBT_INVAL_DIAG
        /* what the whole-cache flush drops: physical unknown / RAM / elsewhere, and by the pc's region */
        ++g_abr_kills[pa == PHYS_UNKNOWN ? 0 : (pa >= 0x0C000000u && pa < 0x0E000000u) ? 1 : 2];
        ++g_abr_kills[3 + (b->guest_pc >= 0x80000000u ? 2 : b->guest_pc >= 0x02000000u ? 1 : 0)];
#endif
        invalidate_block(b);
    }
    INVAL_END(1);
    finish_or_defer();
#ifdef DBT_BATCH_RANGES
    g_range_batch = 0;
#endif
    dbt_stat.rom_kept += (sf_u32)kept;
}

/* ------------------------------------------------------------ hypercalls */

#include "../../bsp/classpad50/SRC/INC/classpad_lcdrot.h"
#define LCD_PANEL_W 320u                /* the fx-CP400 panel, portrait (HC_LCD_PUSH rotated form) */
#define LCD_PANEL_H 528u
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
                                         * 13-14 and 23-29 more counters, 64-116 tick samples / exceptions / machine registers, 128-383 the code histogram:
                                         * the complete list is kept in bsp/classpad/SRC/INC/classpad_hc.h) */
#define HC_DEVRANGE 0xFC                /* tests: translate [r4, r5) device-aware from now on (drops all translations at the next lookup) */
#define HC_LCD_BLIT 0xFD                /* r4 = pixels as the guest stores them (P1/P2 address), r5 = pixel count, r6 = 16-bit LCD data port: written natively */
#define HC_LCD_PUSH 0xFE                /* DCS panel: r4 = pixels (P1/P2), r5 = first row, r6 = rows, r7 = width: CASET/PASET/RAMWR and the pixels in one go; r7 = CLASSPAD_ROT_PUSH_TAG | angle: the rotated form of classpad_lcdrot.h */
#define HC_STANDBY 0xFF                 /* SH7305 R-standby (docs/06): r4 = flags (unused) -> r0 = 1 after a wake-up, 0 if the machine did not enter it */

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
 * machine can be profiled with nothing but HC_GETSTAT (indices 64 up): the
 * share of ticks in translated guest
 * code, in the lookup/helper stub windows, inside a translation or a helper,
 * in the kernel's idle `sleep`; and a 256-bucket histogram of guest code by
 * physical address over the ROM image (kernel and XIP DLLs alike: a user pc
 * is translated through the UTLB), to symbolise with tools/cesym.py. A tick
 * that lands in masked translator C (lookups, hypercalls, the exception path)
 * is delivered at the re-enabling instruction and so counts as guest code at
 * the pc that entered the stub. Every exception is also counted by vector
 * and INTEVT code, so a storming source shows up. Counters wrap modulo 2^32: take deltas. */
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

#ifndef DBT_NO_TICK_SAMPLING
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

#endif /* DBT_NO_TICK_SAMPLING */

/* Machine state worth a look on the device (HC_GETSTAT 80-83) */
#define REG_CCR    (*(volatile sf_u32 *)0xFF00001Cu)
#define REG_FRQCR  (*(volatile sf_u32 *)0xA4150000u)
#define REG_PLLCR  (*(volatile sf_u32 *)0xA4150024u)
#define REG_FLLFRQ (*(volatile sf_u32 *)0xA4150050u)
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
    case 384: return REG_PLLCR;         /* with FRQCR: the clock tree, to settle Pphi (see bsp_cfg.h) */
    case 385: return REG_FLLFRQ;
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
    dbt_puts(" pllcr="); dbt_puthex(REG_PLLCR); dbt_puts(" fllfrq="); dbt_puthex(REG_FLLFRQ);
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
    dbt_puts(" fpudis="); dbt_puthex(dbt_stat.fpu_disabled); dbt_puts("/"); dbt_puthex(dbt_stat.fpu_illegal);
    dbt_puts(" hcflush="); dbt_puthex(dbt_stat.hc_flush); dbt_puts("/"); dbt_puthex(dbt_stat.hc_flush_all); dbt_puts(" romkept="); dbt_puthex(dbt_stat.rom_kept);
    dbt_puts(" ldcsr="); dbt_puthex(dbt_stat.ldc_sr_calls + FAST_LDC_SR_CALLS); dbt_puts(" fallback="); dbt_puthex(dbt_stat.fallback_calls); dbt_puts(" rte="); dbt_puthex(dbt_stat.rte_calls);
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
#ifdef DBT_LAZY_META
    dbt_puts("\n[dbt] meta rebuilt="); dbt_puthex(g_lazy_regens); dbt_puts(" kept="); dbt_puthex(g_lazy_kept);
    dbt_puts(" overflow="); dbt_puthex(g_lazy_transient); dbt_puts(" resident="); dbt_puthex(g_lazy_resident);
#endif
#ifdef DBT_SECOND_CHANCE
    dbt_puts(" probations="); dbt_puthex(g_probations); dbt_puts(" survivors="); dbt_puthex(g_survivors);
    dbt_puts(" survivor_bytes="); dbt_puthex(g_survivor_bytes);
#endif
#ifdef DBT_TLB_REFILL
#ifdef DBT_FPU_HIST
    { int k; dbt_puts("\n[dbt] fpu hist:"); for (k = 0; k < 34; ++k) { dbt_puts(" "); dbt_puthex(g_fpu_hist[k]); } }
#endif
    dbt_puts(" tlb_refills="); dbt_puthex(dbt_stat.tlb_refills); dbt_puts(" tlb_revals="); dbt_puthex(dbt_stat.tlb_revals); dbt_puts("/"); dbt_puthex(dbt_stat.tlb_reval_skips); dbt_puts(" cpages="); dbt_puthex(g_ncpage); dbt_puts(g_cpage_over ? "!" : "");
#endif
#ifdef DBT_FAST_RTE
    dbt_puts(" fast_rte="); dbt_puthex(dbt_fast_rte_calls);
#endif
#ifdef DBT_HASH_DIAG
    {
        int o;
        dbt_puts("\n[dbt] hashdiag:");
        for (o = 0; o < 17; ++o) { dbt_puts(" "); dbt_puthex(g_hash_diag[o]); }
    }
#endif
#ifdef DBT_INVAL_DIAG
    {
        int o;
        dbt_puts("\n[dbt] inval calls/kills/multi:");
        for (o = 0; o < 4; ++o) {
            dbt_puts(" "); dbt_puthex(g_inval_calls[o]); dbt_puts("/"); dbt_puthex(g_inval_kills[o]);
            dbt_puts("/"); dbt_puthex(g_inval_multi[o]);
        }
        dbt_puts(" utlb="); dbt_puthex(g_utlb_calls);
        dbt_puts(" abr_kills:"); for (o = 0; o < 6; ++o) { dbt_puts(" "); dbt_puthex(g_abr_kills[o]); }
    }
#endif
    dbt_puts("\n");
    print_samples();
    print_trace();
#ifdef DBT_DEV_SITES
    print_dev_sites();
#endif
#ifdef DBT_CE_TIME_PROBE
    /* Diagnostic only: pointer location and dwCurMSec offset verified from
     * the frozen benchmark NK's OALTimerIntrHandler, not a platform ABI. */
    {
        sf_u32 nk = bswap32(*(volatile sf_u32 *)DBT_CE_TIME_PROBE);
        sf_u32 pa = (nk >= 0x80000000u && nk < 0xc0000000u) ? nk & 0x1fffffffu : utlb_translate(nk, g_asid);
        if (pa >= 0x0c100000u && pa < 0x0c8fff00u) {
            dbt_puts("[ce-time] nkglobal="); dbt_puthex(nk);
            dbt_puts(" msec="); dbt_puthex(bswap32(*(volatile sf_u32 *)(0x80000000u + pa + 0xc8)));
            dbt_puts("\n");
        }
    }
#endif
}

/* HC_LCD_BLIT / HC_LCD_PUSH: n little-endian 16-bit pixels from guest memory to the panel's data port */
static void lcd_pixels(const sf_u8 *s, sf_u32 n, volatile sf_u16 *port)
{
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
}

/* R-standby (docs/06-deep-suspend.md, rt/standby.S). The save block and the
 * resume stub live in the SH7305's RS memory, which keeps its contents while
 * the core is powered off; the stock OS uses 0xFD800000..0xFD8003FF and a
 * stack below 0xFD802000, so this block sits at 0xFD801000. */
#define RS_BLOCK  0xFD801000u
#define RS_STUB   0xFD801200u
extern sf_u32 dbt_standby_sleep(sf_u32 *block);

/* On-chip memories (rt/image.ld): the stubs run from IL memory, small hot
 * data lives in XRAM+YRAM. Neither survives R-standby (docs/06: only RS
 * memory does), so dbt_standby keeps a copy of the data and reloads both. */
extern sf_u32 lilram[] __asm__("_lilram"), rilram[] __asm__("_rilram"), eilram[] __asm__("_eilram"),
              rxyram[] __asm__("_rxyram"), exyram[] __asm__("_exyram");
static sf_u32 g_xyram_save[16384 / 4];
void dbt_onchip_load(void)
{
    volatile sf_u32 *d = rilram;
    const sf_u32 *s = lilram;
    sf_u32 n = (sf_u32)(eilram - rilram), i;
    for (i = 0; i < n; i++) d[i] = s[i];                 /* 32-bit stores, as gint's regcpy */
    d = rxyram;
    n = (sf_u32)(exyram - rxyram);
    for (i = 0; i < n; i++) d[i] = 0;
    __asm__ volatile ("synco\n\ticbi @%0" :: "r"(rilram) : "memory");     /* SH7724 10.5.3 */
}
static void onchip_save(void)
{
    sf_u32 n = (sf_u32)(exyram - rxyram), i;
    for (i = 0; i < n; i++) g_xyram_save[i] = ((volatile sf_u32 *)rxyram)[i];
}
static void onchip_restore(void)
{
    volatile sf_u32 *d = rilram;
    sf_u32 n = (sf_u32)(eilram - rilram), i;
    for (i = 0; i < n; i++) d[i] = lilram[i];
    d = rxyram;
    n = (sf_u32)(exyram - rxyram);
    for (i = 0; i < n; i++) d[i] = g_xyram_save[i];
    /* SH7724 10.5.3 IL memory coherency: SYNCO, then ICBI, before branching
     * to instructions written into IL memory */
    __asm__ volatile ("synco\n\ticbi @%0" :: "r"(rilram) : "memory");
}
extern const sf_u32 dbt_standby_stub[], dbt_standby_stub_end[];
static sf_u32 g_standbys, g_standby_wakes;
#ifdef DBT_STANDBY_DIAG
/* make STANDBY_DIAG=1 (docs/06 section 8): stripes 5-8 of the resume
 * path; standby.S paints 1-4. Rows 66*(stage-1).. full width, ~0.5 s each. */
static sf_u32 g_diag_woke;
static void diag_stripe(sf_u32 stage, sf_u32 colour)
{
    volatile sf_u8 *prdr = (volatile sf_u8 *)0xA405013Cu, *r64 = (volatile sf_u8 *)0xA413FEC0u;
    volatile sf_u16 *port = (volatile sf_u16 *)0xB4000000u;
    sf_u32 y0 = 66 * (stage - 1), y1 = y0 + 65, n, t;
    dbt_lcd_select(prdr, port, 0x2A);
    dbt_lcd_param(port, 0); dbt_lcd_param(port, 0); dbt_lcd_param(port, 1); dbt_lcd_param(port, 0x3F);
    dbt_lcd_select(prdr, port, 0x2B);
    dbt_lcd_param(port, y0 >> 8); dbt_lcd_param(port, y0 & 0xFF); dbt_lcd_param(port, y1 >> 8); dbt_lcd_param(port, y1 & 0xFF);
    dbt_lcd_select(prdr, port, 0x2C);
    for (n = 320 * 66; n; n--) *port = (sf_u16)colour;
    dbt_lcd_sync();
    t = *r64;
    while (((*r64 - t) & 63) < 32) ;
}
#endif
static sf_u32 dbt_standby(void)
{
    sf_u32 *stub = (sf_u32 *)RS_STUB, *block = (sf_u32 *)RS_BLOCK;
    sf_u32 n = (sf_u32)(dbt_standby_stub_end - dbt_standby_stub), i, r;
    for (i = 0; i < n; i++) stub[i] = dbt_standby_stub[i];
    stub[n - 1] = RS_BLOCK;                     /* the stub's last longword: where the block is */
    g_standbys++;
    dbt_puts("[dbt] R-standby\n");
    dbt_dbgring_drain();
    onchip_save();                              /* last: nothing below writes XYRAM before the sleep */
    r = dbt_standby_sleep(block);
#ifdef DBT_STANDBY_DIAG
    /* 5: the translator's C code in SDRAM runs again; blue, or yellow if the
     * RTC alarm flag (RCR1.AF) is set, i.e. an alarm ended the standby */
    if (r) diag_stripe(5, (*(volatile sf_u8 *)0xA413FEDCu & 1) ? 0xFFE0 : 0x001F);
#endif
    if (r) onchip_restore();                    /* before anything returns into the IL-memory stubs */
#ifdef DBT_STANDBY_DIAG
    /* 6: IL memory and XYRAM reloaded and read back: white = both match,
     * orange = IL memory differs, purple = XYRAM differs, black = both */
    if (r) {
        sf_u32 il_bad = 0, xy_bad = 0, k, m;
        m = (sf_u32)(eilram - rilram);
        for (k = 0; k < m; k++) if (((volatile sf_u32 *)rilram)[k] != lilram[k]) il_bad = 1;
        m = (sf_u32)(exyram - rxyram);
        for (k = 0; k < m; k++) if (((volatile sf_u32 *)rxyram)[k] != g_xyram_save[k]) xy_bad = 1;
        diag_stripe(6, il_bad ? (xy_bad ? 0x0000 : 0xFD20) : (xy_bad ? 0x780F : 0xFFFF));
    }
#endif
    /* back, with the core reset if r == 1: the stub restored caches, MMU
     * registers and VBR; the rest of the machine is the guest's business */
    *(volatile sf_u32 *)0xA4150030u = block[(0x40 + 0x10) / 4];     /* MSTPCR0 */
    *(volatile sf_u32 *)0xA4150020u = block[(0x40 + 0x14) / 4];     /* STBCR */
    if (r) {
        g_standby_wakes++;
        user_mappings_changed(0);               /* the UTLB is empty; the page tables survived */
    }
    dbt_puts(r ? "[dbt] R-standby wake\n" : "[dbt] R-standby not entered\n");
#ifdef DBT_STANDBY_DIAG
    if (r) { diag_stripe(7, 0x5D1C); g_diag_woke = 1; }    /* 7, steel blue: returning into the IL-memory stubs */
#endif
    return r;
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
        else {
#ifdef DBT_INVAL_DIAG
            sf_u32 before = g_inval_kills[0];
#endif
            dbt_invalidate(ctx->r[4], ctx->r[4] + ctx->r[5]);
#ifdef DBT_INVAL_DIAG
            if (g_inval_kills[0] - before > 8) {
                dbt_puts("[f2] "); dbt_puthex(ctx->r[4]); dbt_puts(" +"); dbt_puthex(ctx->r[5]);
                dbt_puts(" kills "); dbt_puthex(g_inval_kills[0] - before); dbt_puts(" pr "); dbt_puthex(ctx->pr); dbt_puts("\n");
            }
#endif
        }
        return RT_ACT_RESUME;
    case HC_STATS: print_stats(); return RT_ACT_RESUME;
    case HC_STANDBY: ctx->r[0] = dbt_standby(); return RT_ACT_RESUME;
    case HC_BENCH: ctx->r[0] = dbt_bench_native(ctx->r[4], ctx->r[5]); return RT_ACT_RESUME;
    case HC_TLB_INVAL:
        if (ctx->r[5] == 0) user_mappings_changed(1);
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
        if ((ctx->r[7] & CLASSPAD_ROT_PUSH_MASK) == CLASSPAD_ROT_PUSH_TAG) {
            /* Rotated form (classpad_lcdrot.h): the driver's frame is an
             * unrotated logical surface of the panel turned by the angle;
             * stream the physical window r6 (columns px0 | px1 << 16) x r5
             * (rows py0 | py1 << 16), end-exclusive, in panel order from
             * transposed reads of the buffer at r4. */
            sf_u32 rot = ctx->r[7] & 0xFFu, py0 = ctx->r[5] & 0xFFFFu, py1 = ctx->r[5] >> 16;
            sf_u32 px0 = ctx->r[6] & 0xFFFFu, px1 = ctx->r[6] >> 16, px, py;
            const sf_u8 *base = (const sf_u8 *)ctx->r[4];
            if (px0 >= px1 || py0 >= py1 || px1 > LCD_PANEL_W || py1 > LCD_PANEL_H) return RT_ACT_RESUME;
            DCS_CMD(0x2A); DCS_PARAM(px0 >> 8); DCS_PARAM(px0 & 0xFF); DCS_PARAM((px1 - 1) >> 8); DCS_PARAM((px1 - 1) & 0xFF);
            DCS_CMD(0x2B); DCS_PARAM(py0 >> 8); DCS_PARAM(py0 & 0xFF); DCS_PARAM((py1 - 1) >> 8); DCS_PARAM((py1 - 1) & 0xFF);
            DCS_CMD(0x2C);
            for (py = py0; py < py1; py++)
                for (px = px0; px < px1; px++) {
                    const sf_u8 *s = base + ClasspadRotOffset(rot, LCD_PANEL_W, LCD_PANEL_H, px, py);
                    *port = (sf_u16)(s[0] | ((sf_u16)s[1] << 8));
                }
            dbt_stat.lcd_pixels += (px1 - px0) * (py1 - py0);
            dbt_lcd_sync();
            return RT_ACT_RESUME;
        }
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
#ifdef DBT_USER_IC_HASH_HIT
#ifdef DBT_USER_IC_REFILL
        case 30: v = 3; break;          /* bit 1: user hash hits refill the IC */
#else
        case 30: v = 1; break;
#endif
#endif
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
        case 26: v = dbt_stat.ldc_sr_calls + FAST_LDC_SR_CALLS; break;
        case 31: v = FAST_LDC_SR_CALLS; break;
#ifdef DBT_FAST_LDC_SR
#ifdef DBT_FAST_LDC_SR_MD
        case 63: v = 3; break;          /* bit 1: MD changes stay on the fast path */
#else
        case 63: v = 1; break;
#endif
#endif
        case 27: v = dbt_stat.lookups_c; break;
        case 28: v = dbt_stat.tlb_hooks; break;
        case 29: v = g_bank_evictions; break;
#ifdef DBT_HASH_ASID_MIX
        case 421: v = 1; break;
#endif
#ifdef DBT_HASH_TWO_WAY
        case 422: v = 1; break;
#endif
        // Capability handshake: legacy loaders return zero for unknown IDs.
#ifdef DBT_CONST_DEVICE_ADDRESS
        case 402:
#ifndef DBT_CONST_P4_STORE
            v = 1;
#endif
            break;
        case 403:
            v = g_const_device_blocks;
            break;
#ifdef DBT_CONST_P4_STORE
        case 404: v = 1; break;
#endif
#endif
#ifdef DBT_FAST_LDTLB
        case 419: v = 1; break;
        case 420: v = dbt_fast_ldtlb_calls; break;
#endif
#ifdef DBT_SAME_ASID_PTEH_STORE
        case 418: v = 1; break;
#endif
#ifdef DBT_R0_DEVICE_WIDE
        case 417: v = 1; break;
#endif
#ifdef DBT_R0_DEVICE_TEST
        case 416: v = 1; break;
#endif
#ifdef DBT_CORE_BANKS
        case 412: v = 1; break;
        case 413: v = g_core_translates; break;
        case 414: v = g_other_translates; break;
        case 415: v = g_core_evictions; break;
#endif
#ifdef DBT_UNIFORM_STORE
        case 410: v = 1; break;
        case 411: v = g_const_device_blocks; break;
#endif
#ifdef DBT_INCOMING_BITMAP
        case 408: v = 1; break;
        case 409: v = g_source_scans; break;
#endif
#ifdef DBT_BATCH_HASH_REMOVE
        case 406: v = 1; break;
        case 407: v = g_batch_hash_scans; break;
#endif
#ifdef DBT_DYNAMIC_P4_STORE
        case 405: v = 1; break;
#endif
        case 59:
#ifdef DBT_HASH_ASID_TAG
            v = 1;
#endif
            break;
        case 60: v = 0x44425453u; break; /* DBTS */
#ifdef DBT_FAST_RTE
        case 432: v = dbt_fast_rte_calls; break;
        case 433: v = 1; break;          /* DBT_FAST_RTE built in */
#endif
#ifdef DBT_TLB_REFILL
        case 430: v = dbt_stat.tlb_refills; break;
        case 431: v = 1; break;          /* TLB_REFILL built in */
        case 434: v = dbt_stat.tlb_revals; break;
#endif
        case 61: v = 1; break;         /* statistics ABI version */
        case 62:
#ifndef DBT_NO_TICK_SAMPLING
            v = 1u;
#endif
            break;

        default: v = ctx->r[4] >= 64 ? sample_stat(ctx->r[4]) : 0u; break;
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
#ifdef DBT_REUSE_RESUME_META
static sf_u32 meta_decoded_pc(const dbt_block *b, const dbt_imeta *m, const dbt_gctx *ctx)
{
#else
static sf_u32 meta_start_pc(const dbt_block *b, int idx, const dbt_gctx *ctx)
{
    dbt_imeta meta;
    const dbt_imeta *m = &meta;
    dbt_get_meta(b, idx, &meta);
#endif
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

#ifdef DBT_REUSE_RESUME_META
static sf_u32 meta_start_pc(const dbt_block *b, int idx, const dbt_gctx *ctx)
{
    dbt_imeta meta;
    dbt_get_meta(b, idx, &meta);
    return meta_decoded_pc(b, &meta, ctx);
}
#endif

/* Guest pc to resume at for an exception at host offset `off` of block b
 * (off < code_len). *committed is set when the range's memory operation has
 * already executed: the guest instruction then completes in software
 * (apply_resume_state) and execution resumes at the following range. */
static sf_u32 resume_pc(const dbt_block *b, sf_u32 off, const dbt_gctx *ctx, int *idx_out, int *committed)
{
    int idx;
    ensure_meta(b);
    idx = dbt_block_find(b, off);
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
#ifdef DBT_REUSE_RESUME_META
    /* The metadata just used for the commit test is still valid. Context
     * register and T-dependent targets must still be evaluated each time. */
    return meta_decoded_pc(b, m, ctx) +
#else
    return meta_start_pc(b, idx, ctx) +
#endif
        (m->kind == META_LINEAR ? off - m->host_off : 0);
}

#ifdef DBT_IRQ_ENTRY_CACHE
/* Interior entries are safe only at a guest instruction boundary with no
 * borrowed register or pending scratch save. Never publish these aliases
 * in the normal hash, whose invalidation keys are block starts. */
static struct { sf_u32 gpc, host, asid, generation, index; } g_irq_entry[16];
#ifdef DBT_IRQ_ENTRY_HASH
static unsigned irq_entry_slot(sf_u32 gpc)
{
    return ((gpc >> 1) ^ (gpc >> 9)) & 15;
}
#else
static unsigned g_irq_entry_next;
#endif
static void remember_irq_entry(dbt_block *b, sf_u32 gpc)
{
    unsigned i, index;
#if defined(DBT_CONST_DEVICE_ADDRESS) || defined(DBT_UNIFORM_STORE)
    if (b && b->const_specialized && gpc < b->guest_pc + 2u * b->const_specialized
#ifdef DBT_UNIFORM_ENTRY_BOUNDS
        && gpc > b->guest_pc + 2u * b->uniform_entry_start
#endif
       ) return;
#endif
    if (!b || b->borrow >= 0 || gpc <= b->guest_pc || gpc >= b->guest_pc + b->guest_len) return;
    index = (unsigned)(b - g_blocks);
    if (g_binfo[index].dead) return;
    ensure_meta(b);
#ifdef DBT_IRQ_ENTRY_HASH
    /* Reuse only an entry from this still-live block and address space.
     * Collisions merely replace aliases; they never publish a block-start
     * hash entry or bypass the spill-free boundary checks below. */
    {
        unsigned slot = irq_entry_slot(gpc);
        if (g_irq_entry[slot].host && g_irq_entry[slot].gpc == gpc &&
            g_irq_entry[slot].index == index && g_irq_entry[slot].asid == g_asid &&
            g_irq_entry[slot].generation == dbt_stat.flushes) return;
    }
#endif
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
#ifdef DBT_IRQ_ENTRY_HASH
            unsigned slot = irq_entry_slot(gpc);
#else
            unsigned slot = g_irq_entry_next++ & 15;
#endif
            g_irq_entry[slot].gpc = gpc; g_irq_entry[slot].host = b->host_base + off;
            g_irq_entry[slot].asid = g_asid; g_irq_entry[slot].generation = dbt_stat.flushes;
            g_irq_entry[slot].index = index;
        }
        return;
    }
}
static sf_u32 find_irq_entry(sf_u32 gpc)
{
#ifdef DBT_IRQ_ENTRY_HASH
    {
        unsigned slot = irq_entry_slot(gpc);
#else
    unsigned n;
    for (n = 0; n < 16; ++n) {
        unsigned slot = (g_irq_entry_next - 1 - n) & 15;
#endif
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
#ifdef DBT_LAZY_RESUME_META
    /* Restarting an instruction only undoes spills. Its fix-up metadata
     * is needed solely after the memory operation has committed. */
    if (committed) dbt_get_meta(b, idx, &meta);
#else
    dbt_get_meta(b, idx, &meta);
#endif
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
#ifdef DBT_FAST_LDTLB
    /* LDTLB is committed; SR and return target are live, GPR restores pending. */
    if (IN(spc, dbt_tlbfast_a, dbt_tlbfast_end)) {
        sf_u32 target = ctx->r[1];
        g_window_kind = 2;
        if (!AFTER(spc, dbt_tlbfast_a)) UNDO(ctx, 0, SLOT_Y0);
        if (!AFTER(spc, dbt_tlbfast_b)) UNDO(ctx, 2, SLOT_A0);
        UNDO(ctx, 1, SLOT_Y1);
        *guest_pc = guest_pc_of_host(target, ctx);
        return 1;
    }
#endif
#ifdef DBT_FAST_LDC_SR
    /* Same-bank SR helper fast return: the new SR is already live. */
    if (IN(spc, dbt_srfast_a, dbt_srfast_end)) {
        sf_u32 target = ctx->r[1];
        g_window_kind = 2;
        if (!AFTER(spc, dbt_srfast_a)) UNDO(ctx, 0, SLOT_Y0);
        if (!AFTER(spc, dbt_srfast_b)) UNDO(ctx, 2, SLOT_A0);
        UNDO(ctx, 1, SLOT_Y1);
        *guest_pc = guest_pc_of_host(target, ctx);
        return 1;
    }
#endif
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
#ifdef DBT_STANDBY_DIAG
    /* 8: the first exception after a wake-up reached C through the IL-memory
     * stubs: magenta = a hypercall from CE, grey = HC_STANDBY again (the OAL
     * refused the wake reason), brown = any other exception or interrupt */
    if (g_diag_woke) {
        sf_u32 hc = (ctx->vec_off == 0x100 && ctx->expevt == 0x160) ? ((ctx->tra >> 2) & 0xFF) : 0;
        g_diag_woke = 0;
        diag_stripe(8, hc == HC_STANDBY ? 0x8410 : hc >= 0xF0 ? 0xF81F : 0xA145);
    }
#endif
    bank_normalize(ctx);
    act = exc_c_body(ctx);
    bank_normalize(ctx);
    return act;
}

#ifdef DBT_TLB_REFILL
/* The CE kernel's SH TLB refill (shexcept.src TLBMissHandler, at VBR +
 * 0x400) done here instead of in translated guest code; CE 6.0's walk first,
 * CE 5.0's (tlbr5_code, tlb_refill5) below. Delivering a UTLB miss to the
 * guest cost an exception round trip, the handler's control-register
 * accesses through the C fallback (TEA, TTB, EXPEVT, PTEA, PTEL), an ldtlb
 * and an rte in C: ~32k times per My Device load. The walk below is the
 * handler's: page directory (the process's from TTB, the kernel's from
 * g_pprcNK->ppdir for the shared heap and kernel addresses), page table,
 * valid bit 0x100, read-only mask for user-mode reads of the shared heap,
 * PTEA from PTEL bits 9 and 29-31. On success the translator's ldtlb hook
 * loads the entry and the faulting host instruction runs again. Anything
 * else (a failing walk, memory not readable through P1/P2, a fault outside
 * the code cache, a new mapping that would drop the faulting block) changes
 * nothing and the guest handler runs as before. It is used only while the
 * code at VBR + 0x400 is exactly this handler (checked once per VBR; the
 * literals, shared-heap base and &g_pprcNK, come from its pool). */
#define REG_TTB  (*(volatile sf_u32 *)0xFF000008u)
#define REG_PTEA (*(volatile sf_u32 *)0xFF000034u)
static const sf_u16 tlbr_code[57] = {          /* TLBMissHandler up to its bra TLBMissError */
    0x5273, 0x5072, 0xe6ff, 0xd12a, 0x3212, 0x8b0e, 0x0032, 0x4029,
    0x4019, 0xc940, 0x8840, 0x8905, 0x4211, 0x8b2a, 0xe6df, 0x5079,
    0x8840, 0x8b26, 0xd023, 0x6002, 0x500b, 0x933e, 0x6123, 0x4119,
    0x4109, 0x6213, 0x2239, 0x4119, 0x4109, 0x2139, 0x301c, 0x6002,
    0x4011, 0x8916, 0x302c, 0x6002, 0x9230, 0x2209, 0x4215, 0x8b10,
    0x2069, 0x6103, 0xe3f7, 0x413d, 0xe301, 0x2138, 0xe3ec, 0x413d,
    0x8900, 0x7108, 0x171d, 0x1701, 0x0038, 0x0009, 0x0009, 0x002b,
    0x0009
};
/* CE 5.0's handler (PRIVATE\WINCEOS\COREOS\NK\KERNEL\SH\shexcept.src, SH4,
 * no NKPROF), from its first instruction to its rte: TEA, TTB -> SectionTable
 * [TEA >> 25] -> SECTION[(TEA >> 16) & 0x1FF] -> MEMBLOCK (a kernel pointer;
 * NULL_BLOCK 0 and RESERVED_BLOCK 1 are "unmapped") -> aPages[(TEA >> 12) &
 * 15]: an entry whose bit 0 (the WT bit, used as "valid") is set, allowed to
 * the current thread when its access key (pCurThread->aky, r4_bank1 + 0x14)
 * shares a bit with the block's lock (MEMBLOCK + 0). Then PTEA from bits 9
 * and 29-31, PTEL = entry & ~1, ldtlb, rte. Its one PC-relative literal is
 * BLOCK_MASK (0x1FF, mov.w); it has no fixups, so the words are compared as
 * they are. Anything the handler would refuse (addresses >= 2 GiB, an
 * unmapped block, an invalid entry, an access-key mismatch: the exception
 * paths tlb50/55/60) is left to the guest. */
static const sf_u16 tlbr5_code[46] = {
    0x5173, 0x5072, 0x4111, 0x8b3b, 0x4119, 0x6213, 0x4129, 0x4101,
    0x4108, 0x001e, 0x6123, 0x932e, 0x4119, 0x2139, 0x4108, 0x011e,
    0x5345, 0x4111, 0x8d53, 0x6023, 0x4009, 0xc93c, 0x700c, 0x5210,
    0x001e, 0xc801, 0x894b, 0x2328, 0x8910, 0x6103, 0x70ff, 0xe3f7,
    0x413d, 0xe301, 0x2138, 0xe3ec, 0x413d, 0x8900, 0x7108, 0x171d,
    0x1701, 0x0038, 0x0009, 0x0009, 0x002b, 0x0009
};
static sf_u32 g_tlbr_vbr = 1, g_tlbr_heap, g_tlbr_nkprc;    /* VBR last checked (1: none), literals */
static int g_tlbr_ok;                                        /* 0 none, 6 CE 6.0's handler, 5 CE 5.0's */
static int p12_word(sf_u32 a) { return a >= 0x80000000u && a < 0xC0000000u && !(a & 3); }
static sf_u32 rd32le(sf_u32 a) { return bswap32(*(volatile sf_u32 *)a); }
static int tlbr5_check(sf_u32 h)
{
    unsigned i;
    for (i = 0; i < sizeof tlbr5_code / sizeof tlbr5_code[0]; ++i)
        if (bswap16(*(volatile sf_u16 *)(h + 2 * i)) != tlbr5_code[i]) return 0;
    if (bswap16(*(volatile sf_u16 *)(h + 0x16 + 4 + 2 * (tlbr5_code[0x16 / 2] & 0xFF))) != 0x01FF) return 0;   /* BLOCK_MASK */
    dbt_puts("[dbt] TLB refill in the translator: CE 5.0 handler (SectionTable walk)\n");
    return 5;
}
static void tlbr_check(sf_u32 vbr)
{
    sf_u32 h = vbr + 0x400, a;
    unsigned i;
    g_tlbr_vbr = vbr; g_tlbr_ok = 0;
    if (!p12_word(h) || !p12_word(h + 0x200)) return;
    for (i = 0; i < sizeof tlbr_code / sizeof tlbr_code[0]; ++i)
        if (bswap16(*(volatile sf_u16 *)(h + 2 * i)) != tlbr_code[i]) { g_tlbr_ok = tlbr5_check(h); return; }
    a = ((h + 0x06 + 4) & ~3u) + 4 * (tlbr_code[0x06 / 2] & 0xFF); g_tlbr_heap = rd32le(a);   /* VM_SHARED_HEAP_BASE */
    a = ((h + 0x24 + 4) & ~3u) + 4 * (tlbr_code[0x24 / 2] & 0xFF); g_tlbr_nkprc = rd32le(a);  /* &g_pprcNK */
    if (bswap16(*(volatile sf_u16 *)(h + 0x2A + 4 + 2 * (tlbr_code[0x2A / 2] & 0xFF))) != 0x0FFC ||
        bswap16(*(volatile sf_u16 *)(h + 0x48 + 4 + 2 * (tlbr_code[0x48 / 2] & 0xFF))) != 0x0100 ||
        !p12_word(g_tlbr_nkprc)) return;
    g_tlbr_ok = 6;
    dbt_puts("[dbt] TLB refill in the translator: CE 6.0 handler, shared heap "); dbt_puthex(g_tlbr_heap);
    dbt_puts(", g_pprcNK at "); dbt_puthex(g_tlbr_nkprc); dbt_puts("\n");
}
/* the new entry must leave the faulting block alive (it runs again) */
static int tlbr_load(const dbt_gctx *ctx, sf_u32 pte)
{
    static const sf_u8 shift_of_sz[4] = { 10, 12, 16, 20 };
    sf_u32 pteh, sz, pmask, vpn;
    dbt_block *b = block_of_host(ctx->host_spc);
    if (!b) return 0;
    pteh = REG_PTEH;
    sz = ((pte >> 4) & 1) | ((pte >> 6) & 2); pmask = ~((1u << shift_of_sz[sz]) - 1);
    vpn = pteh & 0xFFFFFC00u & pmask;
    if (tlb_drops_block((unsigned)(b - g_blocks), vpn, pmask, pte, pteh & 0xFF, (pte >> 1) & 1)) return 0;
    REG_PTEA = (pte >> 29) | (((pte >> 9) & 1) << 3);
    REG_PTEL = pte;
    tlb_hook();                                          /* ASID, stale blocks, ldtlb */
    return 1;
}
static int tlb_refill5(const dbt_gctx *ctx)
{
    sf_u32 tea = ctx->tea, ttb, sect, blk, entry, a, th;
    if (tea & 0x80000000u) return 0;                     /* tlb55: the guest's error path */
    ttb = REG_TTB;
    a = ttb + ((tea >> 25) << 2);
    if (!p12_word(ttb) || !p12_word(a)) return 0;
    sect = rd32le(a);                                    /* SectionTable[tea >> 25] */
    a = sect + (((tea >> 16) & 0x1FFu) << 2);
    if (!p12_word(sect) || !p12_word(a)) return 0;
    blk = rd32le(a);                                     /* MEMBLOCK *, or NULL_BLOCK / RESERVED_BLOCK */
    if (!(blk & 0x80000000u)) return 0;                  /* tlb60 */
    a = blk + 12 + ((tea >> 12) & 15) * 4;               /* aPages[page] */
    if (!p12_word(blk) || !p12_word(a)) return 0;
    entry = rd32le(a);
    if (!(entry & 1)) return 0;                          /* not valid: tlb60 */
    th = ctx->b1[4];                                     /* pCurThread: r4_bank1 (bank_normalize ran) */
    if (!p12_word(th) || !p12_word(th + 0x14)) return 0;
    if (!(rd32le(blk) & rd32le(th + 0x14))) return 0;    /* access lock vs the thread's key: tlb50 */
    return tlbr_load(ctx, entry & ~1u);
}

/* MMUCR.TI, or HC_TLB_INVAL without a range. CE 5.0's kernel never gives one: its
 * InvalidateRange(addr, len) is OEMCacheRangeFlush(0, 0, CACHE_SYNC_FLUSH_TLB)
 * (INC\kernel.h, "there might be aliasing somewhere"), so every decommit,
 * VirtualProtect, stack scavenge and page-out meant "all user mappings may
 * have changed" and cost every user block: 216 such calls dropped 77k blocks
 * in the first 132 s of a boot, most of the translation churn. The page
 * tables say what actually changed: a user block whose pc still maps, under
 * its ASID, to the physical page it was translated from (binfo.phys, from
 * the UTLB at translation time) is as valid as before the call; one whose
 * page is gone or moved is dropped. Slot 0 is the current process's section
 * (SectionTable[0], swapped in by SetCPUASID with the ASID) or, for another
 * process's blocks, SectionTable[procnum + 1] (CE 5.0's ASID is the procnum;
 * NK's section is NKSection, not in the table: its blocks are dropped unless
 * current). Slots 1-63 are the same for every process. Blocks reachable from
 * several ASIDs (ASID_ANY) in slot 0 and blocks crossing a page are dropped
 * rather than argued about. Content changes are not this path's business:
 * the kernel announces them with HC_FLUSH. Only with the CE 5.0 refill
 * handler recognised (g_tlbr_ok == 5): the walk is that handler's. */
static sf_u32 walk5_phys(sf_u32 va, sf_u32 asid, sf_u32 *mask_out)
{
    static const sf_u8 shift_of_sz[4] = { 10, 12, 16, 20 };
    sf_u32 ttb = REG_TTB, a, sect, blk, entry, sz, mask, slot = va >> 25;
    if (va & 0x80000000u) return PHYS_UNKNOWN;
    if (!slot && asid != (REG_PTEH & 0xFF)) {
        if (!asid || asid >= 0x3F) return PHYS_UNKNOWN;
        slot = asid + 1;
    }
    a = ttb + (slot << 2);
    if (!p12_word(ttb) || !p12_word(a)) return PHYS_UNKNOWN;
    sect = rd32le(a);
    a = sect + (((va >> 16) & 0x1FFu) << 2);
    if (!p12_word(sect) || !p12_word(a)) return PHYS_UNKNOWN;
    blk = rd32le(a);
    if (!(blk & 0x80000000u)) return PHYS_UNKNOWN;
    a = blk + 12 + ((va >> 12) & 15) * 4;
    if (!p12_word(blk) || !p12_word(a)) return PHYS_UNKNOWN;
    entry = rd32le(a);
    if (!(entry & 1)) return PHYS_UNKNOWN;
    sz = ((entry >> 4) & 1) | ((entry >> 6) & 2);
    mask = ~((1u << shift_of_sz[sz]) - 1);
    *mask_out = mask;
    return (entry & 0x1FFFFC00u & mask) | (va & ~mask);
}
static int tlb_reval5(void)
{
    unsigned i;
    int j;
    if (g_tlbr_ok != 5) return 0;
#ifdef DBT_BATCH_RANGES
    g_range_batch = 1;
#endif
    INVAL_CALL(3);
    if (!g_cpage_over) {
        /* one walk per code page; only a changed page's blocks are visited */
        for (i = 0; i < CPAGE_N; ++i) {
            sf_u32 key = CPAGE_KEY[i], page, asid8, p, mask;
            if (key < 2) continue;
            page = key & ~0xFFFu; asid8 = key & 0xFFu;
            p = walk5_phys(page, asid8, &mask);
            if (p != PHYS_UNKNOWN) p &= ~0xFFFu;
            if (p == CPAGE_PHYS[i]) continue;
            cpage_kill(page, asid8);
            if (p == PHYS_UNKNOWN) CPAGE_KEY[i] = 1; else CPAGE_PHYS[i] = p;
        }
    } else {
        /* the table overflowed: every user block, one walk per (page, ASID) */
        static sf_u32 memo_key[256], memo_phys[256], memo_mask[256], memo_gen[256], gen;
        ++gen;
        for (j = 0; j < g_nuser; ++j) {
            unsigned h;
            dbt_block *b;
            sf_u32 p, mask = 0, key;
            i = g_user_idx[j]; b = &g_blocks[i];
            if (g_binfo[i].dead || g_binfo[i].phys == PHYS_UNKNOWN) { if (!g_binfo[i].dead) invalidate_block(b); continue; }
            key = (b->guest_pc & ~0xFFFu) | (g_binfo[i].asid & 0xFFu);
            h = ((key >> 12) ^ (key >> 20) ^ key) & 255u;
            if (memo_gen[h] == gen && memo_key[h] == key) { p = memo_phys[h]; mask = memo_mask[h]; }
            else {
                p = walk5_phys(b->guest_pc, g_binfo[i].asid, &mask);
                if (p != PHYS_UNKNOWN) p &= mask;
                memo_gen[h] = gen; memo_key[h] = key; memo_phys[h] = p; memo_mask[h] = mask;
            }
            if (p == PHYS_UNKNOWN || p != (g_binfo[i].phys & mask) ||
                (b->guest_pc & mask) != ((b->guest_pc + b->guest_len - 1) & mask)) invalidate_block(b);
        }
    }
    INVAL_END(3);
    finish_or_defer();
#ifdef DBT_BATCH_RANGES
    g_range_batch = 0;
#endif
    dbt_stat.tlb_revals++;
    return 1;
}
static int tlb_refill(const dbt_gctx *ctx)
{
    sf_u32 tea = ctx->tea, mask = 0xFFFFFFFFu, dir, pde, pte, a;
    if (dbt_shadow_vbr != g_tlbr_vbr) tlbr_check(dbt_shadow_vbr);
    if (!g_tlbr_ok || ctx->host_spc - g_cfg.cache_base >= g_cfg.cache_size) return 0;
    if (ctx->expevt != 0x040 && ctx->expevt != 0x060) return 0;
    if (g_tlbr_ok == 5) return tlb_refill5(ctx);
    dir = REG_TTB;
    if (tea >= g_tlbr_heap) {
        if (!(dbt_shadow_md & SR_MD)) {                  /* user mode: shared heap, reads only */
            if (tea >= 0x80000000u || ctx->expevt != 0x040) return 0;
            mask = 0xFFFFFFDFu;
        }
        a = rd32le(g_tlbr_nkprc) + 44;                   /* &g_pprcNK->ppdir */
        if (!p12_word(a)) return 0;
        dir = rd32le(a);
    }
    a = dir + ((tea >> 20) & 0xFFCu);
    if (!p12_word(dir) || !p12_word(a)) return 0;
    pde = rd32le(a);
    a = pde + ((tea >> 10) & 0xFFCu);
    if (!p12_word(pde) || !p12_word(a)) return 0;     /* a page table outside P1/P2 is the guest's business */
    pte = rd32le(a);
    if (!(pte & 0x100)) return 0;
    return tlbr_load(ctx, pte & mask);
}
#endif

static int exc_c_body(dbt_gctx *ctx)
{
    dbt_block *b = 0;                   /* set on the in-block paths only */
    sf_u32 guest_pc, handler;
    int act, where = 0;                 /* where as trace_exc: 1 exit stub, 2 block, 3 stub window, 4 translating, 5 helper */
    dbt_stat.exceptions++;
#ifdef DBT_TLB_REFILL
    if (ctx->vec_off == 0x400 && !g_exc_depth && tlb_refill(ctx)) { dbt_stat.tlb_refills++; return RT_ACT_RESUME; }
#endif

    if (ctx->vec_off == 0x100 && ctx->expevt == 0x160) {
        sf_u32 nr = (ctx->tra >> 2) & 0xFF;
        if (nr == HC_SYNTH) {
            /* the translator asks for an exception at guest pc r4 with EXPEVT r5
             * (stub context: the guest registers are in dbt_ctx_rt) */
            sf_u32 expevt = ctx->r[5];
#ifdef DBT_ASM_FETCH_FAULT
            if (expevt & 0x10000u) {
                if (g_exc_depth) dbt_halt("guest exception handler not fetchable");
                ++dbt_stat.synth_exceptions;
                note_asid();
                expevt &= 0xFFFFu;
            }
#endif
            guest_pc = ctx->r[4];
            take_stub_context(ctx);
            REG_EXPEVT = expevt;
            REG_TEA = guest_pc;
            ctx->expevt = expevt; ctx->tea = guest_pc;
            ctx->vec_off = 0x100;
            goto deliver;
        }
#ifdef DBT_WATCH_GPC
        if (nr == 0xEE) {
            /* the trapa the translator put at DBT_WATCH_GPC (translate.c):
             * registers, and r4 read as a NUL-terminated UTF-16 string */
            static unsigned n;
            sf_u32 a = ctx->r[4], pa, i;
            if (n++ < 400) {
                dbt_puts("[watch] r4="); dbt_puthex(ctx->r[4]); dbt_puts(" r5="); dbt_puthex(ctx->r[5]);
                dbt_puts(" r6="); dbt_puthex(ctx->r[6]); dbt_puts(" pr="); dbt_puthex(ctx->pr); dbt_puts(" \"");
                pa = (a >= 0x80000000u && a < 0xC0000000u) ? (a & 0x1FFFFFFFu) : utlb_translate(a, REG_PTEH & 0xFF);
                if (pa != PHYS_UNKNOWN && !(a & 1))
                    for (i = 0; i < 64; ++i) {
                        unsigned c = *(volatile sf_u8 *)(0x80000000u + pa + 2 * i) | ((unsigned)*(volatile sf_u8 *)(0x80000000u + pa + 2 * i + 1) << 8);
                        if (!c || ((pa + 2 * i) & 0xFFF) == 0xFFE) break;      /* stop at the page's end: the next page may be unmapped */
                        dbt_putc(c >= 32 && c < 127 ? (char)c : '?');
                    }
                dbt_puts("\"\n");
            }
            return RT_ACT_RESUME;
        }
#endif
        if (nr >= 0xF0) return hypercall(ctx, nr);
    }
#ifndef DBT_NO_TICK_SAMPLING
    sample_exception(ctx);
#endif

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
            ref_mark((unsigned)(b - g_blocks));     /* exceptions and timer ticks are use too */
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
    if (ctx->vec_off == 0x600 && ctx->intevt == 0x400) {
#ifdef DBT_CACHE_TICK_LFU
        if (where == 1 || where == 2) {
            unsigned bank = bank_of_code(b->host_base);
            if (g_bank_touch[bank] != 0xFFFFFFFFu) ++g_bank_touch[bank];
        }
#endif
#ifndef DBT_NO_TICK_SAMPLING
        sample_tick(guest_pc, where);
#endif
#ifdef DBT_BOOT_MARQUEE
        bootmarquee_tick();         /* the start screen's blocks keep moving until CE draws (rt/bootmarquee.c) */
#endif
    }
#ifdef DBT_IRQ_ENTRY_CACHE
    if (ctx->vec_off == 0x600) remember_irq_entry(b, guest_pc);
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
    dbt_stat.rte_calls++;
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
    sf_u32 opcode = arg & 0xFFFFu, hret;
    g_helper_ret = hret = helper_return_address();
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
#ifdef DBT_FPU_HIST
    /* -DDBT_FPU_HIST: which FPU instructions run (by their opcode's class), in the stats */
    {
        unsigned k = (opcode & 0xF000u) == 0xF000u ? ((opcode & 0xFu) == 0xDu ? 16 + ((opcode >> 4) & 0xFu) : (opcode & 0xFu)) : 32;
        g_fpu_hist[k]++;
        if (g_fpu.fpscr & FPSCR_PR) g_fpu_hist[33]++;
    }
#endif
    r = fpuemu_exec(&g_fpu, ctx->r, &ctx->sr, opcode, &mem);
    g_helper_ret = 0;
    if (r != FPU_OK) {
        /* Not a reason to stop the machine (2026-11-09: a halt here froze the
         * calculator on an instruction the emulator merely did not know). An
         * opcode we do not implement is delivered as the hardware's general
         * illegal instruction (0x180, 0x1A0 in a delay slot): the kernel
         * raises an exception in the offending thread and the rest of the
         * system goes on. An enabled FPU exception (FPSCR cause set, result
         * not written) is the FPU exception 0x120. A faulting memory
         * operand (FPU_FAULT) was already delivered by the helper's fault
         * path before fpuemu_exec returned. */
        static unsigned printed;
        int insn;
        sf_u32 gpc = helper_guest_pc(hret, &insn);
        if (printed < 8) {
            ++printed;
            dbt_puts("[dbt] fpu op "); dbt_puthex(opcode); dbt_puts(" status "); dbt_puthex((sf_u32)r);
            dbt_puts(" at guest "); dbt_puthex(gpc); dbt_puts(" fpscr "); dbt_puthex(g_fpu.fpscr);
            dbt_puts(r == FPU_EXCEPTION ? ": FPU exception to the guest\n" : ": illegal instruction to the guest\n");
        }
        if (!insn) dbt_halt("fpu helper outside an instruction range");
        dbt_stat.fpu_illegal++;
        deliver_synthetic(gpc, r == FPU_EXCEPTION ? 0x120 : (arg & 0x10000u) ? 0x1A0 : 0x180);
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
        else if (ea == 0xFF000010u && (v & 4)) user_mappings_changed(0);             /* MMUCR.TI */
    } else {
        v = *(volatile sf_u32 *)ea;
        if (d->am == AM_POSTINC) ctx->r[d->base] += 4;
        ctx->r[d->data] = v;                                /* after the increment: @Rm+ with m == n keeps the load */
    }
}

#ifdef DBT_DEV_SITES
/* -DDBT_DEV_SITES: guest pcs whose device-aware access really reached P4
 * (the fallback's dev_mem), with counts; printed with the stats. Shows
 * which kernel.dll code needs the run-time test at all. */
static struct { sf_u32 pc, n; } g_dev_site[256];
static sf_u32 g_dev_site_other;
static void dev_site_note(sf_u32 pc, const dbt_gctx *ctx, const sh_insn *d)
{
    unsigned i;
    sf_u32 ea = ctx->r[d->base];
    if (d->am == AM_DISP) ea += (sf_u32)d->disp;
    else if (d->am == AM_R0IDX) ea += ctx->r[0];
    else if (d->am == AM_PREDEC) ea -= 4;
    if ((ea >> 24) != 0xFFu) return;
    for (i = 0; i < 256; ++i) {
        if (!g_dev_site[i].n) g_dev_site[i].pc = pc;
        if (g_dev_site[i].pc == pc) { ++g_dev_site[i].n; return; }
    }
    ++g_dev_site_other;
}
static void print_dev_sites(void)
{
    unsigned i;
    dbt_puts("[devsites] other="); dbt_puthex(g_dev_site_other); dbt_puts("\n");
    for (i = 0; i < 256 && g_dev_site[i].n; ++i) {
        dbt_puts("[devsite] "); dbt_puthex(g_dev_site[i].pc); dbt_puts(" "); dbt_puthex(g_dev_site[i].n); dbt_puts("\n");
    }
}
#endif

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
        if (d.kind == K_MEM) {
#ifdef DBT_DEV_SITES
            dev_site_note(helper_guest_pc(g_helper_ret, &(int){0}), ctx, &d);
#endif
            dev_mem(ctx, &d); g_helper_ret = 0; return;
        }
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
    ensure_meta(b);
    off = ret - 4 - b->host_base;
    idx = dbt_block_find(b, off);
    if (idx < 0) dbt_halt("helper return address outside the block's code");
    *in_insn_range = dbt_meta_kind(b, idx) == META_INSN;
    return meta_start_pc(b, idx, &dbt_ctx_rt);
}
