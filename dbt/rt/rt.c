/*
 * rt.c - translator runtime, C half. Target-only (big-endian SH7305, P1
 * addresses, no OS). Everything here runs with interrupts masked or BL=1.
 */
#include "rt.h"

typedef __SIZE_TYPE__ dbt_size_t;
void *memcpy(void *d, const void *s, dbt_size_t n);
void *memset(void *d, int c, dbt_size_t n);

#define HSIZE        8192                 /* hash entries, power of two */
#define MAX_BLOCKS   768                  /* ~2 MiB of metadata; see docs/04 */
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
void *memcpy(void *d, const void *s, dbt_size_t n) { sf_u8 *a = d; const sf_u8 *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int c, dbt_size_t n) { sf_u8 *a = d; while (n--) *a++ = (sf_u8)c; return d; }

volatile sf_u32 dbt_shadow_vbr = 0;
volatile sf_u32 dbt_shadow_md = SR_MD;
dbt_stats dbt_stat;

typedef struct { sf_u32 gpc; sf_u32 host; } hent;

hent dbt_hash[HSIZE] __attribute__((aligned(32)));   /* read by stubs.S */
#define g_hash dbt_hash
static dbt_block g_blocks[MAX_BLOCKS];      /* sorted by host_base (bump allocation) */
static int       g_nblocks;
static dbt_rt_config g_cfg;
static sf_u32    g_cache_used;              /* bytes */
static fpu_state g_fpu;
static int       g_translating;             /* guest pc being translated or 0 */
static sf_u32    g_translating_pc;
static dbt_env   g_env;

/* ---------------------------------------------------------- console */

void dbt_putc(int c) { DEBUG_PORT = (sf_u8)c; }
void dbt_puts(const char *s) { while (*s) dbt_putc(*s++); }
void dbt_puthex(sf_u32 v)
{
    static const char hex[] = "0123456789abcdef";
    int i;
    for (i = 28; i >= 0; i -= 4) dbt_putc(hex[(v >> i) & 0xF]);
}
void dbt_halt(const char *why)
{
    dbt_puts("\n[dbt] HALT: "); dbt_puts(why); dbt_puts("\n");
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

/* ------------------------------------------------------------ hashing */

static inline sf_u32 hidx(sf_u32 gpc) { return (gpc >> 1) & (HSIZE - 1); }

static void hash_insert(sf_u32 gpc, sf_u32 host)
{
    sf_u32 i = hidx(gpc), n;
    for (n = 0; n < 8; n++) {                      /* short linear probe, then overwrite */
        hent *e = &g_hash[(i + n) & (HSIZE - 1)];
        if (e->host == 0 || e->gpc == gpc) { e->gpc = gpc; e->host = host; return; }
    }
    g_hash[i].gpc = gpc; g_hash[i].host = host;
}

static sf_u32 hash_find(sf_u32 gpc)
{
    sf_u32 i = hidx(gpc), n;
    for (n = 0; n < 8; n++) {
        hent *e = &g_hash[(i + n) & (HSIZE - 1)];
        if (e->host == 0) return 0;
        if (e->gpc == gpc) return e->host;
    }
    return 0;
}

/* ------------------------------------------------------- block cache */

static void cache_flush_all(void)
{
    memset(g_hash, 0, sizeof(hent) * HSIZE);
    g_nblocks = 0;
    g_cache_used = 0;
    dbt_stat.flushes++;
}

void dbt_rt_init(const dbt_rt_config *cfg)
{
    g_cfg = *cfg;
    memset(&dbt_stat, 0, sizeof dbt_stat);
    cache_flush_all();
    fpuemu_reset(&g_fpu);
    memset(&g_env, 0, sizeof g_env);
    g_env.fetch16 = g_fetch16; g_env.read16le = g_read16le; g_env.read32le = g_read32le;
    g_env.rt_lookup_link = (sf_u32)&dbt_lookup_link;
    g_env.rt_lookup = (sf_u32)&dbt_lookup;
    g_env.rt_rte = (sf_u32)&dbt_rte_stub;
    g_env.rt_fpu = (sf_u32)&dbt_fpu_stub;
    g_env.rt_fallback = (sf_u32)&dbt_fallback_stub;
    g_env.rt_ldc_sr = (sf_u32)&dbt_ldc_sr_stub;
    g_env.shadow_vbr_addr = (sf_u32)&dbt_shadow_vbr;
    g_env.shadow_md_addr = (sf_u32)&dbt_shadow_md;
    g_env.max_insns = DBT_MAX_INSNS;
}

static dbt_block *translate_at(sf_u32 gpc)
{
    dbt_block *b;
    sf_u32 host;
    sf_u32 fault = 0;
    int r;
    if (g_nblocks >= MAX_BLOCKS || g_cache_used + DBT_BLOCK_WORDS * 2 > g_cfg.cache_size)
        cache_flush_all();
    b = &g_blocks[g_nblocks];
    host = g_cfg.cache_base + g_cache_used;
    g_translating = 1; g_translating_pc = gpc;
    r = dbt_translate_block(&g_env, gpc, host, (sf_u16 *)host, DBT_BLOCK_WORDS, b, &fault);
    g_translating = 0;
    if (r != DBT_OK) {
        dbt_puts("[dbt] translate error "); dbt_puthex((sf_u32)r);
        dbt_puts(" at "); dbt_puthex(gpc); dbt_puts(" fault "); dbt_puthex(fault); dbt_puts("\n");
        dbt_halt("translation failed");
    }
    g_cache_used += ((sf_u32)b->host_len + 31u) & ~31u;
    g_nblocks++;
    dbt_sync_code(host, (sf_u32)b->host_len);
    hash_insert(gpc, host);
    dbt_stat.translations++;
    dbt_stat.blocks = (sf_u32)g_nblocks;
    return b;
}

sf_u32 dbt_resolve(sf_u32 gpc, sf_u32 link_lit)
{
    sf_u32 host = hash_find(gpc);
    dbt_stat.lookups_c++;
    if (!host) host = translate_at(gpc)->host_base;
    if (link_lit) *(volatile sf_u32 *)link_lit = host;    /* literal read via the operand cache: coherent */
    return host;
}

/* Block containing a host address (blocks are sorted by host_base). */
static dbt_block *block_of_host(sf_u32 host)
{
    int lo = 0, hi = g_nblocks - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        dbt_block *b = &g_blocks[mid];
        if (host < b->host_base) hi = mid - 1;
        else if (host >= b->host_base + (sf_u32)b->host_len) lo = mid + 1;
        else return b;
    }
    return 0;
}

void dbt_invalidate(sf_u32 lo, sf_u32 hi)
{
    int i;
    dbt_stat.invalidations++;
    /* v1: any overlap flushes everything; a precise version unlinks blocks */
    for (i = 0; i < g_nblocks; i++) {
        dbt_block *b = &g_blocks[i];
        if (b->guest_pc < hi && b->guest_pc + b->guest_len > lo) { cache_flush_all(); return; }
    }
}

/* ------------------------------------------------------------ hypercalls */

#define HC_EXIT   0xF0
#define HC_PUTS   0xF1
#define HC_FLUSH  0xF2
#define HC_PUTC   0xF3
#define HC_PUTHEX 0xF4
#define HC_STATS  0xF5
#define HC_BENCH  0xF6

static void print_trace(void);

static void print_stats(void)
{
    dbt_puts("[dbt] blocks="); dbt_puthex(dbt_stat.blocks);
    dbt_puts(" translations="); dbt_puthex(dbt_stat.translations);
    dbt_puts(" flushes="); dbt_puthex(dbt_stat.flushes);
    dbt_puts(" lookups_c="); dbt_puthex(dbt_stat.lookups_c);
    dbt_puts(" exceptions="); dbt_puthex(dbt_stat.exceptions);
    dbt_puts(" fpu="); dbt_puthex(dbt_stat.fpu_ops);
    dbt_puts(" resumes="); dbt_puthex(dbt_stat.resumes);
    dbt_puts("/"); dbt_puthex(dbt_stat.resumes_committed);
    dbt_puts("\n");
    print_trace();
}

static int hypercall(dbt_gctx *ctx, sf_u32 nr)
{
    dbt_stat.hypercalls++;
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
    case HC_FLUSH: dbt_invalidate(ctx->r[4], ctx->r[5]); return RT_ACT_RESUME;
    case HC_STATS: print_stats(); return RT_ACT_RESUME;
    case HC_BENCH: ctx->r[0] = dbt_bench_native(ctx->r[4], ctx->r[5]); return RT_ACT_RESUME;
    }
    dbt_puts("[dbt] unknown hypercall "); dbt_puthex(nr); dbt_puts("\n");
    return RT_ACT_RESUME;
}

/* ------------------------------------------------------------ exceptions */

/* Guest resume pc for an exception at host offset `off` inside block b. */
/* Guest pc at the start of meta range idx (idx may be the sentinel). */
static sf_u32 meta_start_pc(const dbt_block *b, int idx, const dbt_gctx *ctx)
{
    const dbt_imeta *m = &b->meta[idx];
    switch (m->kind) {
    case META_INSN:         return b->guest_pc + m->guest_off;
    case META_TARGET_CONST: return m->target;
    case META_TARGET_X0:    return ctx->slots[SLOT_X0];
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
    const dbt_imeta *m;
    *idx_out = idx;
    *committed = 0;
    if (idx < 0) dbt_halt("resume_pc outside translated code");
    m = &b->meta[idx];
    if (m->kind == META_INSN && m->commit_rel != 0xFF && off > (sf_u32)m->host_off + m->commit_rel) {
        *committed = 1;
        return meta_start_pc(b, idx + 1, ctx);
    }
    return meta_start_pc(b, idx, ctx);
}

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
    const dbt_imeta *m = &b->meta[idx];
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
    for (i = 0; i < n; i++) ctx->r[sp[i].gpr] = ctx->slots[sp[i].slot];
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
        if (IN(spc, dbt_lookup, dbt_lookup_link)) {
            if (AFTER(spc, dbt_lk_s1)) UNDO(ctx, 0, SLOT_Y0);
            if (AFTER(spc, dbt_lk_s2)) UNDO(ctx, 1, SLOT_Y1);
            if (AFTER(spc, dbt_lk_s3)) UNDO(ctx, 2, SLOT_A0);
        } else {
            if (AFTER(spc, dbt_lkl_s1)) UNDO(ctx, 0, SLOT_Y0);
            if (AFTER(spc, dbt_lkl_s2)) UNDO(ctx, 1, SLOT_Y1);
            if (AFTER(spc, dbt_lkl_s3)) UNDO(ctx, 2, SLOT_A0);
        }
        *guest_pc = ctx->slots[SLOT_X0];
        return 1;
    }
    /* hit tail: r1 = host target */
    if (IN(spc, dbt_hit_a, dbt_hit_end)) {
        sf_u32 target = ctx->r[1];
        if (!AFTER(spc, dbt_hit_a)) UNDO(ctx, 0, SLOT_Y0);
        if (!AFTER(spc, dbt_hit_b)) UNDO(ctx, 2, SLOT_A0);
        UNDO(ctx, 1, SLOT_Y1);
        *guest_pc = guest_pc_of_host(target, ctx);
        return 1;
    }
    /* full tail (miss path and helper exits): r1 = host target, r3 in X1 */
    if (IN(spc, dbt_full_m1, dbt_full_end)) {
        sf_u32 target = ctx->r[1];
        if (!AFTER(spc, dbt_full_m1)) UNDO(ctx, 3, SLOT_X1);
        if (!AFTER(spc, dbt_full_m2)) UNDO(ctx, 0, SLOT_Y0);
        if (!AFTER(spc, dbt_full_m3)) UNDO(ctx, 2, SLOT_A0);
        UNDO(ctx, 1, SLOT_Y1);
        *guest_pc = guest_pc_of_host(target, ctx);
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
                dbt_block *b = block_of_host(ret - 2);
                int idx, committed;
                sf_u32 off;
                if (AFTER(spc, h[i].s1)) UNDO(ctx, 0, SLOT_Y0);
                if (AFTER(spc, h[i].s2)) UNDO(ctx, 1, SLOT_Y1);
                if (AFTER(spc, h[i].s3)) UNDO(ctx, 2, SLOT_A0);
                if (!b) dbt_halt("helper return address not in a block");
                off = ret - 2 - b->host_base;
                *guest_pc = resume_pc(b, off, ctx, &idx, &committed);
                apply_resume_state(b, idx, off, committed, ctx);
                return 1;
            }
        }
    }
    dbt_puts("[dbt] exception in stub at "); dbt_puthex(spc); dbt_puts("\n");
    dbt_halt("exception in a non-resumable stub region");
}

int dbt_exc_c(dbt_gctx *ctx)
{
    dbt_block *b;
    sf_u32 guest_pc, handler;
    dbt_stat.exceptions++;

    if (ctx->vec_off == 0x100 && ctx->expevt == 0x160) {
        sf_u32 nr = (ctx->tra >> 2) & 0xFF;
        if (nr >= 0xF0) return hypercall(ctx, nr);
    }

    if (stub_window(ctx, &guest_pc)) {
        trace_exc(ctx->host_spc, 0, 0, 0, 0, guest_pc, ctx->vec_off, 3);
    } else {
        b = block_of_host(ctx->host_spc);
        if (!b) {
            if (g_translating && ctx->vec_off == 0x400) {
                /* code probe faulted while translating: the guest state is the
                 * one saved by the stub that called the translator */
                int i;
                guest_pc = g_translating_pc;
                g_translating = 0;
                for (i = 0; i < 16; i++) ctx->r[i] = dbt_ctx_rt.r[i];
                ctx->sr = dbt_ctx_rt.sr; ctx->pr = dbt_ctx_rt.pr; ctx->gbr = dbt_ctx_rt.gbr;
                ctx->mach = dbt_ctx_rt.mach; ctx->macl = dbt_ctx_rt.macl;
                ctx->ssr = dbt_ctx_rt.sr;
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
                trace_exc(ctx->host_spc, b->guest_pc, off, 0xEE, 0, guest_pc, ctx->vec_off, 1);
            } else {
                int idx, committed;
                guest_pc = resume_pc(b, off, ctx, &idx, &committed);
                apply_resume_state(b, idx, off, committed, ctx);
                trace_exc(ctx->host_spc, b->guest_pc, off, b->meta[idx].kind, (sf_u32)committed, guest_pc, ctx->vec_off, 2);
            }
        }
    }

    /* deliver to the guest handler */
    ctx->new_spc = guest_pc;
    ctx->new_ssr = (ctx->ssr & ~SR_MD) | (dbt_shadow_md & SR_MD);
    dbt_shadow_md = SR_MD;                          /* guest is now in privileged handler mode */
    handler = dbt_shadow_vbr + ctx->vec_off;
    ctx->jump_to = dbt_resolve(handler, 0);
    return RT_ACT_JUMP;
}

sf_u32 dbt_rte_c(sf_u32 guest_spc, sf_u32 *ssr)
{
    sf_u32 host = dbt_resolve(guest_spc, 0);
    dbt_shadow_md = *ssr & SR_MD;
    *ssr = (*ssr | SR_MD | SR_DSP);
    return host;
}

void dbt_ldc_sr_c(dbt_gctx *ctx, sf_u32 newsr)
{
    dbt_shadow_md = newsr & SR_MD;
    ctx->sr = newsr | SR_MD | SR_DSP;
}

/* -------------------------------------------------------------- helpers */

void dbt_fpu_c(dbt_gctx *ctx, sf_u32 opcode)
{
    fpu_mem mem;
    int r;
    mem.read32 = fpu_read32; mem.write32 = fpu_write32; mem.fetch16 = 0; mem.ctx = 0;
    dbt_stat.fpu_ops++;
    r = fpuemu_exec(&g_fpu, ctx->r, &ctx->sr, opcode, &mem);
    if (r != FPU_OK) {
        dbt_puts("[dbt] fpu op "); dbt_puthex(opcode); dbt_puts(" status "); dbt_puthex((sf_u32)r); dbt_puts("\n");
        if (r != FPU_EXCEPTION) dbt_halt("fpu emulation failed");
        /* TODO: raise the guest FPU exception (vector 0x100, EXPEVT 0x800/0x820) */
    }
}

/* mac.w @rm+,@rn+ and mac.l @rm+,@rn+ with saturation per SR.S ignored (S=0). */
void dbt_fallback_c(dbt_gctx *ctx, sf_u32 opcode)
{
    int n = (opcode >> 8) & 0xF, m = (opcode >> 4) & 0xF;
    sf_u64 acc = ((sf_u64)ctx->mach << 32) | ctx->macl;
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
}
