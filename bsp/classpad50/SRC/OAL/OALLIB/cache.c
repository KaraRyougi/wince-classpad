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
    // A range at least as large as the cache is cheaper as a walk of the
    // address array (1024 stores) than line by line; the kernel's "whole
    // cache" calls (no range) are the common case during boot.
    BOOL wholeD = all || len >= g_oalCacheInfo.L1DSize;
    BOOL wholeI = all || len >= g_oalCacheInfo.L1ISize;

    if (dwFlags & (CACHE_SYNC_DISCARD | CACHE_SYNC_WRITEBACK)) {
        if (wholeD) ClasspadOCacheFlushAll();                // write-back and invalidate
        else if (dwFlags & CACHE_SYNC_DISCARD) ClasspadCachePurge(addr, len);
        else ClasspadCacheWriteback(addr, len);
    }
    if (dwFlags & CACHE_SYNC_INSTRUCTIONS) {
        if (wholeD) {
            if (!(dwFlags & (CACHE_SYNC_DISCARD | CACHE_SYNC_WRITEBACK))) ClasspadOCacheFlushAll();
        } else {
            ClasspadCacheWriteback(addr, len);              // code written through the D-cache
        }
        if (wholeI) ClasspadICacheInvalidateAll();
        else ClasspadICacheInvalidate(addr, len);
        if (ClasspadCanTrap()) ClasspadHcFlush(all ? 0 : (DWORD)pAddr, all ? 0 : dwLength);
    }
    if (dwFlags & CACHE_SYNC_FLUSH_TLB) {
        // MMUCR is a P4 register and this code is in nk.exe, which the
        // translator translates device-aware: plain access (classpad_mmio.h)
        P4_W32(CCN_MMUCR, P4_R32(CCN_MMUCR) | MMUCR_TI);
        if (ClasspadCanTrap()) ClasspadHcTlbInval(all ? 0 : (DWORD)pAddr, all ? 0 : dwLength);
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
