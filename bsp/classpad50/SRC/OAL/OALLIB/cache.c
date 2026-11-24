//------------------------------------------------------------------------------
//  cache.c - OEMCacheRangeFlush and cache geometry
//
//  Besides the hardware operations, two things tell the translator what the
//  kernel did (docs/05 "kernel hypercall contract"):
//    CACHE_SYNC_INSTRUCTIONS  -> trapa #0xF2: drop translations of the range
//    CACHE_SYNC_FLUSH_TLB     -> trapa #0xF9: user mappings changed
//  Both only when SR.BL is clear; the translator also notices ldtlb and
//  PTEH changes by itself, so a skipped notification is not fatal.
//------------------------------------------------------------------------------
#include <bsp.h>
#include <pkfuncs.h>

extern void ClasspadCacheWriteback(DWORD addr, DWORD len);
extern void ClasspadCachePurge(DWORD addr, DWORD len);
extern void ClasspadICacheInvalidate(DWORD addr, DWORD len);
extern void ClasspadOCacheFlushAll(void);
extern void ClasspadICacheInvalidateAll(void);

void OEMCacheRangeFlush(LPVOID pAddr, DWORD dwLength, DWORD dwFlags)
{
    DWORD addr = (DWORD)pAddr, len = dwLength;
    BOOL all = (addr == 0 && len == 0);
    BOOL canTrap = ClasspadCanTrap();
    // A range at least as large as the cache is cheaper as a walk of the
    // address array (1024 stores) than line by line; the kernel's "whole
    // cache" calls (no range) are the common case during boot.
    // For writeback-only (without discard), prefer line-by-line writeback up
    // to 4x the cache size so we avoid invalidating 32 KiB of hot D-cache.
    BOOL wholeD = all || ((dwFlags & CACHE_SYNC_DISCARD)
        ? (len >= g_oalCacheInfo.L1DSize)
        : (len >= g_oalCacheInfo.L1DSize * 4));
    BOOL didD = FALSE;

    if (dwFlags & (CACHE_SYNC_DISCARD | CACHE_SYNC_WRITEBACK)) {
        if (wholeD) ClasspadOCacheFlushAll();                // write-back and invalidate
        else if (dwFlags & CACHE_SYNC_DISCARD) ClasspadCachePurge(addr, len);
        else ClasspadCacheWriteback(addr, len);
        didD = TRUE;
    }
    if (dwFlags & CACHE_SYNC_INSTRUCTIONS) {
        if (!didD) {
            if (wholeD) ClasspadOCacheFlushAll();
            else ClasspadCacheWriteback(addr, len);          // code written through the D-cache
        }
        // Under the translator the guest never executes from hardware I-cache:
        // translated host blocks live in the code cache and are synchronised by
        // the translator itself. Flushing hardware I-cache here would only
        // evict the translator's own hot code.
        if (canTrap) ClasspadHcFlush(all ? 0 : (DWORD)pAddr, all ? 0 : dwLength);
    }
    if (dwFlags & CACHE_SYNC_FLUSH_TLB) {
        // MMUCR is a P4 register and this code is in nk.exe, which the
        // translator translates device-aware: plain access (classpad_mmio.h).
        // The MMUCR_TI store already notifies the translator of a whole-TLB
        // flush synchronously inside dev_mem, so skip the redundant whole-TLB
        // hypercall and only issue HC_TLB_INVAL for range invalidations.
        P4_W32(CCN_MMUCR, P4_R32(CCN_MMUCR) | MMUCR_TI);
        if (!all && canTrap) ClasspadHcTlbInval((DWORD)pAddr, dwLength);
    }
}

//------------------------------------------------------------------------------
VOID OALCacheGlobalsInit()
{
    // SH7305: 32 KiB / 32 KiB, 4-way, 32-byte lines (to confirm on hardware)
    g_oalCacheInfo.L1Flags          = 0;
    g_oalCacheInfo.L1ISetsPerWay    = 256;
    g_oalCacheInfo.L1INumWays       = 4;
    g_oalCacheInfo.L1ILineSize      = 32;
    g_oalCacheInfo.L1ISize          = 32 * 1024;
    g_oalCacheInfo.L1DSetsPerWay    = 256;
    g_oalCacheInfo.L1DNumWays       = 4;
    g_oalCacheInfo.L1DLineSize      = 32;
    g_oalCacheInfo.L1DSize          = 32 * 1024;
    g_oalCacheInfo.L2Flags          = 0;
    g_oalCacheInfo.L2ISetsPerWay    = 0;
    g_oalCacheInfo.L2INumWays       = 0;
    g_oalCacheInfo.L2ILineSize      = 0;
    g_oalCacheInfo.L2ISize          = 0;
    g_oalCacheInfo.L2DSetsPerWay    = 0;
    g_oalCacheInfo.L2DNumWays       = 0;
    g_oalCacheInfo.L2DLineSize      = 0;
    g_oalCacheInfo.L2DSize          = 0;
}
