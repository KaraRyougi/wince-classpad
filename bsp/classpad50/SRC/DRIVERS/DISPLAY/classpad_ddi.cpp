//------------------------------------------------------------------------------
//  classpad_ddi.cpp - GPE display driver for the Casio ClassPad under the
//  translator.
//
//  GDI draws into a RAM primary surface (GPE's software blt and line code).
//  The controller cannot scan RAM out, so a thread pushes the rows GDI has
//  touched to the panel through its data port: every Blt/Line on the primary
//  surface widens a dirty row range, the thread pushes it at most every
//  DISP_PUSH_MS. The emulator's fx-CG100 panel presents a frame when a
//  full-width strip reaches the panel bottom, so a push always runs from the
//  first dirty row to the last row (the fx-CP400 panel shows every write).
//  Two panels, classpad_lcd.h: the fx-CP400's DCS command set (default) and
//  the fx-CG100's R61524 registers. Registry:
//      HKLM\System\GDI\Drivers        "Display"="classpad_ddi.dll"
//      HKLM\Drivers\Display\ClassPad  "Width", "Height" (optional; the
//          panel's native size otherwise), "Panel" (0 = R61524 fx-CG100,
//          1 = DCS fx-CP400; absent: a Height under 500 means the fx-CG100)
//------------------------------------------------------------------------------
#if defined(_MSC_VER) && !defined(DEBUG)
#pragma optimize("t", on)
#endif
#include <windows.h>
#include <types.h>
#include <winddi.h>
#include <gpe.h>
#include <pwingdi.h>
#include "classpad_lcd.h"
#include "classpad_hc.h"
#include "classpad_lcdrot.h"
#include <winioctl.h>
#include <pkfuncs.h>
#include "classpad_pwrstat.h"
#include "classpad_bkl.h"

#define DISP_PUSH_MS        50
#define DISP_IDLE_MS        250             // idle wait ceiling so resume (no SetEvent in PowerHandler) is noticed promptly
#define DISP_CHUNK_PIXELS   16384           // pixel budget per HC_LCD_PUSH trap so IRQs stay responsive
#define DISP_CONSOLE_ROWS   96              // rows the translator's console keeps (rt/lcdcon.c LINES * CELL_H)
#define DISP_REG_KEY        TEXT("Drivers\\Display\\ClassPad")

static ULONG g_bitMasks[] = { 0xF800, 0x07E0, 0x001F };

// gpe.lib's debug zones live in the driver. The gpe.h macro only expands in
// DEBUG builds; the retail library still links against dpCurSettings.
#ifdef DEBUG
INSTANTIATE_GPE_ZONES(0x3, "ClasspadDDI", "unused1", "unused2")
#else
DBGPARAM dpCurSettings = {
    TEXT("ClasspadDDI"),
    { TEXT("Errors"), TEXT("Warnings"), TEXT("Performance"), TEXT("Temporary tests"),
      TEXT("Enter,Exit"), TEXT("Initialize"), TEXT("Blt Calls"), TEXT("Blt Verbose"),
      TEXT("Line Calls"), TEXT("Line Verbose"), TEXT("Surface"), TEXT("Flip"),
      TEXT("Poly"), TEXT("Cursor"), TEXT("unused1"), TEXT("unused2") },
    0x3
};
#endif

// in gpe.lib (C++ linkage), declared by every GPE driver itself
BOOL APIENTRY GPEEnableDriver(ULONG engineVersion, ULONG cj, DRVENABLEDATA *data, PENGCALLBACKS engineCallbacks);

// DirectDraw HAL registration hook called from gpe.lib's DllMain; provided by
// ddgpe.lib for DirectDraw-capable drivers, an empty C++ function otherwise.
VOID RegisterDDHALAPI(void)
{
}

// The primary surface: the panel's 320x528x2 bytes, laid out as an
// unrotated surface of the logical size (320x528 in portrait, 528x320 in
// landscape; classpad_lcdrot.h). GPESurf keeps the stride protected, so a
// subclass sets it when the layout changes.
class ClasspadSurf : public GPESurf
{
public:
    ClasspadSurf(int width, int height, void *pBits, int stride, EGPEFormat format) : GPESurf(width, height, pBits, stride, format) {}
    ClasspadSurf(int width, int height, EGPEFormat format) : GPESurf(width, height, format) {}
    void Relayout(int width, int height) { m_nStrideBytes = width * 2; SetRotation(width, height, DMDO_0); }
};

class ClasspadGPE : public GPE
{
    GPEMode             m_mode;
    CRITICAL_SECTION    m_cs;
    HANDLE              m_hEvent;
    HANDLE              m_hThread;
    volatile BOOL       m_run;
    int                 m_dirtyLeft;
    int                 m_dirtyTop;
    int                 m_dirtyRight;
    int                 m_dirtyBottom;
    DWORD               m_pushes;
    int                 m_panel;            // LCD_PANEL_DCS / LCD_PANEL_R61524
    volatile BOOL       m_suspended, m_resumePending;
    volatile BOOL       m_displayOn;        // DCS: DISPON sent (after the first push)
    UCHAR               m_brightness;       // DCS: the backlight level saved at power-off
    USHORT              m_panelId;          // DCS: what RDID answered
    BOOL                m_panelNew;         // DCS: ID 0x16 / 0x52; FALSE = the older panel (classpad_lcd.h)
    unsigned            m_col0;             // DCS: first visible GRAM column (0, or 40 on the older panel)
    int                 m_level;            // DCS: backlight level 1..13 (classpad_bkl.h)
    CRITICAL_SECTION    m_csLcd;            // the panel's port: the push thread and the brightness escape
    BYTE               *m_pFrame;           // the primary surface, physically contiguous (AllocPhysMem)
    ULONG               m_framePa;          // its physical address
    BOOL                m_frameP1Safe;      // VA & PA match modulo SH7305 8 KiB cache way -> P1 cached push needs no flush
    int                 m_blit;             // registry "Blit": 1 = HC_LCD_PUSH (default), 2 = HC_LCD_BLIT with our window commands, 0 = translated stores
    int                 m_consoleRows;      // top rows left to the translator's console (registry "Console"), else 0
    ClasspadSurf       *m_pPrimary;         // m_pPrimarySurface, with Relayout
    USHORT             *m_cvt16;            // 16 -> 16 bpp colour conversion table for non-RGB555 converters (65536 entries)
    ULONG               m_cvtProbe[6];      // what the table's converter made of CVT_PROBE: identifies it

public:
    ClasspadGPE();
    virtual ~ClasspadGPE();

    virtual int     NumModes() { return 1; }
    virtual SCODE   GetModeInfo(GPEMode *pMode, int modeNo);
    virtual SCODE   SetMode(int modeId, HPALETTE *pPalette);
    virtual int     InVBlank() { return 1; }
    virtual SCODE   SetPalette(const PALETTEENTRY *, unsigned short, unsigned short) { return S_OK; }
    virtual SCODE   AllocSurface(GPESurf **ppSurf, int width, int height, EGPEFormat format, int surfaceFlags);
    virtual SCODE   SetPointerShape(GPESurf *, GPESurf *, int, int, int, int) { return S_OK; }
    virtual SCODE   MovePointer(int, int) { return S_OK; }
    virtual SCODE   Line(GPELineParms *pParms, EGPEPhase phase);
    virtual SCODE   BltPrepare(GPEBltParms *pParms);
    virtual SCODE   BltComplete(GPEBltParms *) { return S_OK; }
    virtual VOID    PowerHandler(BOOL bOff);
    virtual ULONG   DrvEscape(SURFOBJ *, ULONG, ULONG, PVOID, ULONG, PVOID);
    LONG            RotateDisplay(int angle);

    void            PanelInit();
    SCODE           WrappedLine(GPELineParms *pParms);
    SCODE           WrappedBlt(GPEBltParms *pParms);
    SCODE           SolidFill16(GPEBltParms *pParms);
    SCODE           FastCopyBlt16(GPEBltParms *pParms);
    SCODE           ConvertedCopyBlt16(GPEBltParms *pParms);
    void            SetWindow(unsigned x0, unsigned x1, unsigned y0, unsigned y1);
    void            DirtyRect(int left, int top, int right, int bottom);
    void            Dirty(int top, int bottom);
    void            PushRect(int left, int top, int right, int bottom);
    void            PushRows(int top, int bottom);
    static DWORD WINAPI PushThread(LPVOID pThis);
};

static DWORD RegDword(LPCTSTR name, DWORD dflt)
{
    HKEY hKey; DWORD v = dflt, type, size = sizeof v;
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, DISP_REG_KEY, 0, 0, &hKey) == ERROR_SUCCESS) {
        if (RegQueryValueEx(hKey, name, NULL, &type, (LPBYTE)&v, &size) != ERROR_SUCCESS || type != REG_DWORD) v = dflt;
        RegCloseKey(hKey);
    }
    return v;
}

ClasspadGPE::ClasspadGPE()
{
    // The translator's panel console (dbt/rt/lcdcon.c) stops drawing at the
    // first "CLASSPAD DDI: " in the log unless the next word is "console":
    // registry "Console"=1 keeps its log in the top rows, which are then
    // never pushed, and pushes atomically through HC_LCD_PUSH. Said before
    // the panel is touched.
    m_consoleRows = RegDword(TEXT("Console"), 0) ? DISP_CONSOLE_ROWS : 0;
    RETAILMSG(1, (m_consoleRows ? TEXT("CLASSPAD DDI: console kept\r\n") : TEXT("CLASSPAD DDI: starting\r\n")));
    int panel  = (int)RegDword(TEXT("Panel"),  0xFFFFFFFF);
    int width  = (int)RegDword(TEXT("Width"),  0);
    int height = (int)RegDword(TEXT("Height"), 0);
    int maxWidth, maxHeight;
    // The panel: the registry, else the configured height (a short display
    // is the fx-CG100 panel of the emulator's fx-cg100 profile), else the
    // ClassPad. Width/Height default to the panel's native size and cannot
    // exceed it.
    if (panel != LCD_PANEL_R61524 && panel != LCD_PANEL_DCS)
        panel = (height > 0 && height < LCD_PANEL_MIN_ROWS) ? LCD_PANEL_R61524 : LCD_PANEL_DCS;
    maxWidth  = panel == LCD_PANEL_DCS ? LCD_CP400_WIDTH  : LCD_CG100_WIDTH;
    maxHeight = panel == LCD_PANEL_DCS ? LCD_CP400_HEIGHT : LCD_CG100_HEIGHT;
    if (width <= 0 || width > maxWidth)   width  = maxWidth;
    if (height <= 0 || height > maxHeight) height = maxHeight;
    m_panel     = panel;
    m_displayOn = FALSE;
    m_suspended = m_resumePending = FALSE;
    m_brightness = LCD_BRIGHTNESS_DEFAULT;
    m_panelId = LCD_ID_NEW_A; m_panelNew = TRUE; m_col0 = 0;
    m_level = CLASSPAD_BKL_DEFAULT;
    InitializeCriticalSection(&m_csLcd);

    m_iRotate = DMDO_0;
    m_cvt16 = NULL;
    memset(m_cvtProbe, 0, sizeof m_cvtProbe);
    m_mode.modeId    = 0;
    m_mode.width     = width;
    m_mode.height    = height;
    m_mode.Bpp       = 16;
    m_mode.frequency = 60;
    m_mode.format    = gpe16Bpp;
    m_pMode          = &m_mode;
    m_nScreenWidth   = width;
    m_nScreenHeight  = height;
    m_nScreenWidthSave  = width;
    m_nScreenHeightSave = height;

    // The frame lives in physically contiguous memory so the push can hand
    // its pixels to the translator by physical address (PushRect). Align PA to
    // 8 KiB (SH7305 D-cache way size) so VA and P1 share cache colour and no
    // CacheRangeFlush is needed before HC_LCD_PUSH / HC_LCD_BLIT. "Blit"=0 in
    // the registry keeps translated stores, 2 uses HC_LCD_BLIT with our window.
    m_blit    = (int)RegDword(TEXT("Blit"), 1);
    m_framePa = 0;
    m_pFrame  = (BYTE *)AllocPhysMem(width * 2 * height, PAGE_READWRITE, 0x2000, 0, &m_framePa);
    if (!m_pFrame) m_pFrame = (BYTE *)AllocPhysMem(width * 2 * height, PAGE_READWRITE, 0, 0, &m_framePa);
    m_frameP1Safe = m_pFrame && ((((ULONG)m_pFrame ^ m_framePa) & 0x1FFFu) == 0);
    if (m_pFrame) m_pPrimary = new ClasspadSurf(width, height, m_pFrame, width * 2, gpe16Bpp);
    else          m_pPrimary = new ClasspadSurf(width, height, gpe16Bpp);
    m_pPrimarySurface = m_pPrimary;
    if (m_pPrimarySurface && m_pPrimarySurface->Buffer()) {
        m_pPrimarySurface->SetRotation(width, height, DMDO_0);
        memset(m_pPrimarySurface->Buffer(), 0, m_pPrimarySurface->Stride() * height);
    }

    if (m_panel == LCD_PANEL_DCS) {
        m_panelId  = LcdReadId();
        m_panelNew = LcdIdIsNew(m_panelId);
        m_col0     = m_panelNew ? 0 : LCD_OLD_COL0;
        RETAILMSG(1, (TEXT("CLASSPAD DDI: panel ID %04X, %s, first column %u\r\n"),
                      m_panelId, m_panelNew ? TEXT("newer panel") : TEXT("older panel"), m_col0));
        PanelInit();
        {
            // the level the OS left, as one of its 14; what cannot be read counts as level 7 and is not written
            UCHAR raw = LcdGetBacklight(m_panelNew);
            if (raw >= LCD_BRIGHTNESS_USABLE) { m_level = ClasspadBklLevelOf(raw); m_brightness = raw; }
        }
        {
            // the OAL powers this panel down and back up around the standby (power.c)
            DWORD kind = m_panelNew ? CLASSPAD_PANEL_NEW : CLASSPAD_PANEL_OLD;
            KernelIoControl(IOCTL_HAL_CLASSPAD_PANEL, &kind, sizeof kind, NULL, 0, NULL);
        }
    }

    InitializeCriticalSection(&m_cs);
    m_dirtyLeft = 0; m_dirtyRight = width;
    m_dirtyTop = 0; m_dirtyBottom = height;             // first push: the whole screen
    m_pushes = 0;
    m_run = TRUE;
    m_hEvent = CreateEvent(NULL, FALSE, TRUE, NULL);
    m_hThread = CreateThread(NULL, 0, PushThread, this, 0, NULL);
    if (m_hThread) CeSetThreadPriority(m_hThread, 250);  // above normal apps (251) so busy loops (ListView_DragSelect, Plotter-Z) still refresh, below driver ISTs

    RETAILMSG(1, (TEXT("CLASSPAD DDI: %dx%d 16bpp, %s panel, frame buffer %08x (phys %08x%s), stride %d, push %s\r\n"),
                  width, height, m_panel == LCD_PANEL_DCS ? TEXT("DCS fx-CP400") : TEXT("R61524 fx-CG100"),
                  m_pPrimarySurface ? m_pPrimarySurface->Buffer() : NULL, m_framePa,
                  m_frameP1Safe ? TEXT(" P1") : TEXT(" P2"),
                  m_pPrimarySurface ? m_pPrimarySurface->Stride() : 0,
                  (m_pFrame && m_blit) ? TEXT("by hypercall") : TEXT("by translated stores")));
}

ClasspadGPE::~ClasspadGPE()
{
    m_run = FALSE;
    if (m_hEvent) SetEvent(m_hEvent);
    if (m_hThread) { WaitForSingleObject(m_hThread, 2000); CloseHandle(m_hThread); }
    if (m_hEvent) CloseHandle(m_hEvent);
    DeleteCriticalSection(&m_cs);
    DeleteCriticalSection(&m_csLcd);
    if (m_cvt16) LocalFree(m_cvt16);
    delete m_pPrimarySurface;
    if (m_pFrame) FreePhysMem(m_pFrame);
}

SCODE ClasspadGPE::GetModeInfo(GPEMode *pMode, int modeNo)
{
    if (modeNo != 0) return E_INVALIDARG;
    *pMode = m_mode;
    return S_OK;
}

SCODE ClasspadGPE::SetMode(int modeId, HPALETTE *pPalette)
{
    if (modeId != 0) return E_INVALIDARG;
    if (pPalette)
        *pPalette = EngCreatePalette(PAL_BITFIELDS, 0, NULL, g_bitMasks[0], g_bitMasks[1], g_bitMasks[2]);
    return S_OK;
}

// GWES calls these escapes through ChangeDisplaySettingsEx. The angle is
// carried in cjIn (not a pointed-to input structure), as required by CE.
ULONG ClasspadGPE::DrvEscape(SURFOBJ *pso, ULONG code, ULONG inSize,
                            PVOID in, ULONG outSize, PVOID out)
{
    if (code == QUERYESCSUPPORT) {
        if (!in || inSize < sizeof(DWORD)) return 0;
        DWORD query = *(DWORD *)in;
        if (query == DRVESC_GETSCREENROTATION || query == DRVESC_SETSCREENROTATION) return 1;
        if ((query == DRVESC_CLASSPAD_GETBRIGHTNESS || query == DRVESC_CLASSPAD_SETBRIGHTNESS) && m_panel == LCD_PANEL_DCS) return 1;
        if (query == DRVESC_CLASSPAD_PUSHFRAME) return 1;
        return GPE::DrvEscape(pso, code, inSize, in, outSize, out);
    }
    if (code == DRVESC_GETSCREENROTATION) {
        // CE 5.0's GWES (GdiGetScreenRotation) asks with cjOut = 0 and pvOut
        // pointing at a 16-bit word it keeps: the current angle in the low
        // byte, the supported angles above it; the CE 5.0 sample drivers
        // store a DWORD there without looking at cjOut, and so do we. With
        // the CE 6.0 size check (cjOut >= 4) the query failed, GWES recorded
        // "no rotation supported" once and for all, and every
        // ChangeDisplaySettingsEx(DM_DISPLAYORIENTATION) returned
        // DISP_CHANGE_BADMODE (the Rotate strip icon did nothing, 2026-11-09).
        if (!out) return DISP_CHANGE_BADPARAM;
        *(DWORD *)out = ((DMDO_90 | DMDO_180 | DMDO_270) << 8) | m_iRotate;
        return DISP_CHANGE_SUCCESSFUL;
    }
    if (code == DRVESC_SETSCREENROTATION) return RotateDisplay((int)inSize);
    // The whole frame to the panel, synchronously (classpad_bkl.h): CEtune's dupdate timing
    if (code == DRVESC_CLASSPAD_PUSHFRAME) {
        EnterCriticalSection(&m_csLcd);
        PushRows(0, m_nScreenHeight);
        LeaveCriticalSection(&m_csLcd);
        return 1;
    }
    // Backlight level (classpad_bkl.h). The value is also what a resume puts back.
    if (code == DRVESC_CLASSPAD_GETBRIGHTNESS && m_panel == LCD_PANEL_DCS) {
        if (!out || outSize < sizeof(int)) return 0;
        *(int *)out = m_level;
        return 1;
    }
    if (code == DRVESC_CLASSPAD_SETBRIGHTNESS && m_panel == LCD_PANEL_DCS) {
        int level;
        if (!in || inSize < sizeof(int)) return 0;
        level = *(int *)in;
        if (level < CLASSPAD_BKL_MIN) level = CLASSPAD_BKL_MIN;
        if (level > CLASSPAD_BKL_MAX) level = CLASSPAD_BKL_MAX;
        EnterCriticalSection(&m_csLcd);
        m_level = level;
        m_brightness = g_classpadBklValue[level];
        if (!m_suspended && !m_resumePending) LcdSetBacklight(m_panelNew, m_brightness);
        LeaveCriticalSection(&m_csLcd);
        return 1;
    }
    return GPE::DrvEscape(pso, code, inSize, in, outSize, out);
}

LONG ClasspadGPE::RotateDisplay(int angle)
{
    if (angle != DMDO_0 && angle != DMDO_90 && angle != DMDO_180 && angle != DMDO_270)
        return DISP_CHANGE_BADMODE;
    if (!m_pPrimarySurface || !m_pPrimarySurface->Buffer()) return DISP_CHANGE_FAILED;
    EnterCriticalSection(&m_cs);
    if (angle != m_iRotate) {
        BOOL sideways = angle == DMDO_90 || angle == DMDO_270;
        m_iRotate = angle;
        m_nScreenWidth = sideways ? m_nScreenHeightSave : m_nScreenWidthSave;
        m_nScreenHeight = sideways ? m_nScreenWidthSave : m_nScreenHeightSave;
        m_mode.width = m_nScreenWidth;
        m_mode.height = m_nScreenHeight;
        // Rotation below GPE (classpad_lcdrot.h): the frame stays the
        // panel's bytes, now laid out as an unrotated surface of the turned
        // size (stride = logical width), and PushRows turns it on the way to
        // the panel. GWES and GPE draw exactly as in portrait, so none of
        // GPE's rotated-surface code runs (its generic rotated blit drew
        // icons wrong, 2026-11-09). The old layout means nothing in the new
        // one: clear it, GWES repaints everything after the mode change.
        m_pPrimary->Relayout(m_nScreenWidth, m_nScreenHeight);
        memset(m_pPrimary->Buffer(), 0, m_nScreenWidthSave * 2 * m_nScreenHeightSave);
        m_dirtyLeft = 0;
        m_dirtyRight = m_nScreenWidth;
        m_dirtyTop = 0;
        m_dirtyBottom = m_nScreenHeight;
        SetEvent(m_hEvent);
    }
    LeaveCriticalSection(&m_cs);
    return DISP_CHANGE_SUCCESSFUL;
}

SCODE ClasspadGPE::AllocSurface(GPESurf **ppSurf, int width, int height, EGPEFormat format, int surfaceFlags)
{
    if (surfaceFlags & GPE_REQUIRE_VIDEO_MEMORY) return E_OUTOFMEMORY;    // there is no video memory
    *ppSurf = new GPESurf(width, height, format);
    if (!*ppSurf) return E_OUTOFMEMORY;
    if (!(*ppSurf)->Buffer()) { delete *ppSurf; *ppSurf = NULL; return E_OUTOFMEMORY; }
    return S_OK;
}

// Logical rectangle [left, right) x [top, bottom) changed (in the surface's own coordinates)
void ClasspadGPE::DirtyRect(int left, int top, int right, int bottom)
{
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > m_nScreenWidth) right = m_nScreenWidth;
    if (bottom > m_nScreenHeight) bottom = m_nScreenHeight;
    if (left >= right || top >= bottom) return;
    EnterCriticalSection(&m_cs);
    // One wakeup per pending batch. Additional drawing widens the box
    // already owned by the pending wakeup (or the active push thread).
    BOOL notify = (m_dirtyTop >= m_dirtyBottom || m_dirtyLeft >= m_dirtyRight);
    if (left < m_dirtyLeft) m_dirtyLeft = left;
    if (right > m_dirtyRight) m_dirtyRight = right;
    if (top < m_dirtyTop) m_dirtyTop = top;
    if (bottom > m_dirtyBottom) m_dirtyBottom = bottom;
    LeaveCriticalSection(&m_cs);
    if (notify) SetEvent(m_hEvent);
}

// Logical rows [top, bottom) changed (the surface's own rows, whatever the rotation)
void ClasspadGPE::Dirty(int top, int bottom)
{
    DirtyRect(0, top, m_nScreenWidth, bottom);
}

SCODE ClasspadGPE::Line(GPELineParms *pParms, EGPEPhase phase)
{
    if (phase == gpeSingle || phase == gpePrepare)
        pParms->pLine = (SCODE (GPE::*)(GPELineParms *))&ClasspadGPE::WrappedLine;
    return S_OK;
}

SCODE ClasspadGPE::WrappedLine(GPELineParms *pParms)
{
    // Snapshot geometry before EmulatedLine so our dirty rect covers the full segment.
    int x0 = (int)pParms->xStart;
    int y0 = (int)pParms->yStart;
    int n  = (int)pParms->cPels;
    RECTL clip;
    BOOL hasClip = (pParms->prclClip != NULL);
    if (hasClip) clip = *pParms->prclClip;
    SCODE sc = EmulatedLine(pParms);
    if (pParms->pDst == m_pPrimarySurface) {
        if (n > 0) {
            // Every pixel of a cPels-pixel Bresenham segment stays within +/-cPels
            // of (xStart, yStart) along both axes, independent of octant encoding.
            int left   = x0 - n;
            int right  = x0 + n + 1;
            int top    = y0 - n;
            int bottom = y0 + n + 1;
            if (hasClip) {
                if (clip.left > left)     left = clip.left;
                if (clip.top > top)       top = clip.top;
                if (clip.right < right)   right = clip.right;
                if (clip.bottom < bottom) bottom = clip.bottom;
            }
            DirtyRect(left, top, right, bottom);
        } else if (hasClip) {
            DirtyRect(clip.left, clip.top, clip.right, clip.bottom);
        } else {
            Dirty(0, m_nScreenHeight);
        }
    }
    return sc;
}

// GWES hands a colour converter (an XLATEOBJ) to a blit whose source and
// destination pixel formats differ. GPE's EmulatedBlt then converts pixel by
// pixel through a virtual call inside its generic rop/mask/clip machinery:
// Plotter-Z's back buffer is a 16 bpp BI_RGB DIB (RGB555) blitted onto the
// RGB565 primary after every column of a recalculation, and that path was
// 40% of the recalculation's time under the translator (2026-11-09). Standard
// RGB555 -> RGB565 converters run inline with bitwise ops (2 pixels per
// 32-bit word, zero 128 KiB LUT); any other 16 -> 16 converter falls back to
// a 65536-entry table identified by six probe values.
static const ULONG CVT_PROBE[6] = { 0x7FFF, 0x001F, 0x03E0, 0x7C00, 0x8000, 0x0200 };

SCODE ClasspadGPE::BltPrepare(GPEBltParms *pParms)
{
    pParms->pBlt = (SCODE (GPE::*)(GPEBltParms *))&ClasspadGPE::WrappedBlt;
    if (!pParms->pDst || pParms->pDst->Format() != gpe16Bpp || !pParms->prclDst ||
        pParms->pMask || pParms->pLookup ||
        (pParms->bltFlags & (BLT_STRETCH | BLT_TRANSPARENT | BLT_ALPHABLEND)) ||
        pParms->prclDst->left >= pParms->prclDst->right ||
        pParms->prclDst->top >= pParms->prclDst->bottom)
        return S_OK;

    if (!pParms->pConvert) {
        if ((pParms->rop4 == 0xF0F0 && pParms->solidColor != (COLOR)-1 && !pParms->pBrush) ||
            pParms->rop4 == 0x0000 || pParms->rop4 == 0xFFFF) {
            pParms->pBlt = (SCODE (GPE::*)(GPEBltParms *))&ClasspadGPE::SolidFill16;
            return S_OK;
        }
        if (pParms->rop4 == 0xCCCC && pParms->pSrc && pParms->pSrc->Format() == gpe16Bpp &&
            pParms->prclSrc &&
            pParms->prclSrc->right - pParms->prclSrc->left == pParms->prclDst->right - pParms->prclDst->left &&
            pParms->prclSrc->bottom - pParms->prclSrc->top == pParms->prclDst->bottom - pParms->prclDst->top) {
            pParms->pBlt = (SCODE (GPE::*)(GPEBltParms *))&ClasspadGPE::FastCopyBlt16;
            return S_OK;
        }
    } else if (pParms->rop4 == 0xCCCC && pParms->pSrc && pParms->pColorConverter &&
        pParms->pSrc->Format() == gpe16Bpp && pParms->prclSrc &&
        pParms->prclSrc->right - pParms->prclSrc->left == pParms->prclDst->right - pParms->prclDst->left &&
        pParms->prclSrc->bottom - pParms->prclSrc->top == pParms->prclDst->bottom - pParms->prclDst->top) {
        pParms->pBlt = (SCODE (GPE::*)(GPEBltParms *))&ClasspadGPE::ConvertedCopyBlt16;
    }
    return S_OK;
}

SCODE ClasspadGPE::SolidFill16(GPEBltParms *pParms)
{
    RECTL dst = *pParms->prclDst;
    USHORT col = (pParms->rop4 == 0x0000) ? 0x0000 : (pParms->rop4 == 0xFFFF) ? 0xFFFF : (USHORT)pParms->solidColor;
    int w, h, y, dstride;

    if (pParms->prclClip) {
        RECTL *c = pParms->prclClip;
        if (c->left > dst.left)     dst.left = c->left;
        if (c->top > dst.top)       dst.top = c->top;
        if (c->right < dst.right)   dst.right = c->right;
        if (c->bottom < dst.bottom) dst.bottom = c->bottom;
    }
    if (dst.left < 0) dst.left = 0;
    if (dst.top < 0) dst.top = 0;
    if (dst.right > pParms->pDst->Width())   dst.right = pParms->pDst->Width();
    if (dst.bottom > pParms->pDst->Height()) dst.bottom = pParms->pDst->Height();
    w = dst.right - dst.left;
    h = dst.bottom - dst.top;
    if (w > 0 && h > 0) {
        UINT32 pair = (UINT32)col | ((UINT32)col << 16);
        BYTE *dp;
        dstride = pParms->pDst->Stride();
        dp = (BYTE *)pParms->pDst->Buffer() + dst.top * dstride + dst.left * 2;
        for (y = 0; y < h; y++, dp += dstride) {
            USHORT *d = (USHORT *)dp;
            int n = w;
            UINT32 *d32;
            if (((ULONG)d & 2) && n > 0) { *d++ = col; n--; }
            d32 = (UINT32 *)d;
            while (n >= 8) {
                d32[0] = pair; d32[1] = pair; d32[2] = pair; d32[3] = pair;
                d32 += 4; n -= 8;
            }
            while (n >= 2) { *d32++ = pair; n -= 2; }
            if (n) *(USHORT *)d32 = col;
        }
        if (pParms->pDst == m_pPrimarySurface) DirtyRect(dst.left, dst.top, dst.left + w, dst.top + h);
    }
    return S_OK;
}

SCODE ClasspadGPE::FastCopyBlt16(GPEBltParms *pParms)
{
    RECTL dst = *pParms->prclDst;
    int sx = pParms->prclSrc->left, sy = pParms->prclSrc->top, w, h, y;

    if (pParms->prclClip) {
        RECTL *c = pParms->prclClip;
        if (c->left > dst.left)     { sx += c->left - dst.left; dst.left = c->left; }
        if (c->top > dst.top)       { sy += c->top - dst.top;   dst.top = c->top; }
        if (c->right < dst.right)   dst.right = c->right;
        if (c->bottom < dst.bottom) dst.bottom = c->bottom;
    }
    if (dst.left < 0)   { sx -= dst.left; dst.left = 0; }
    if (dst.top < 0)    { sy -= dst.top;  dst.top = 0; }
    if (sx < 0)         { dst.left -= sx; sx = 0; }
    if (sy < 0)         { dst.top -= sy;  sy = 0; }
    if (dst.right > pParms->pDst->Width())   dst.right = pParms->pDst->Width();
    if (dst.bottom > pParms->pDst->Height()) dst.bottom = pParms->pDst->Height();
    w = dst.right - dst.left;
    h = dst.bottom - dst.top;
    if (sx + w > pParms->pSrc->Width())  w = pParms->pSrc->Width() - sx;
    if (sy + h > pParms->pSrc->Height()) h = pParms->pSrc->Height() - sy;
    if (w > 0 && h > 0) {
        int sstride = pParms->pSrc->Stride(), dstride = pParms->pDst->Stride();
        size_t bytes = (size_t)w * 2;
        BOOL same = (pParms->pSrc == pParms->pDst);
        if (same && dst.top > sy) {
            const BYTE *src = (const BYTE *)pParms->pSrc->Buffer() + (sy + h - 1) * sstride + sx * 2;
            BYTE *dp = (BYTE *)pParms->pDst->Buffer() + (dst.top + h - 1) * dstride + dst.left * 2;
            for (y = 0; y < h; y++, src -= sstride, dp -= dstride) memmove(dp, src, bytes);
        } else if (same && dst.top == sy) {
            const BYTE *src = (const BYTE *)pParms->pSrc->Buffer() + sy * sstride + sx * 2;
            BYTE *dp = (BYTE *)pParms->pDst->Buffer() + dst.top * dstride + dst.left * 2;
            for (y = 0; y < h; y++, src += sstride, dp += dstride) memmove(dp, src, bytes);
        } else {
            const BYTE *src = (const BYTE *)pParms->pSrc->Buffer() + sy * sstride + sx * 2;
            BYTE *dp = (BYTE *)pParms->pDst->Buffer() + dst.top * dstride + dst.left * 2;
            for (y = 0; y < h; y++, src += sstride, dp += dstride) memcpy(dp, src, bytes);
        }
        if (pParms->pDst == m_pPrimarySurface) DirtyRect(dst.left, dst.top, dst.left + w, dst.top + h);
    }
    return S_OK;
}

SCODE ClasspadGPE::ConvertedCopyBlt16(GPEBltParms *pParms)
{
    ColorConverter *cvt = pParms->pColorConverter;
    unsigned long (ColorConverter::*convert)(unsigned long) = pParms->pConvert;
    ULONG probe[6];
    RECTL dst = *pParms->prclDst;
    int sx = pParms->prclSrc->left, sy = pParms->prclSrc->top, x, y, w, h;
    int i, rgb555 = 0;
    for (i = 0; i < 6; i++) probe[i] = (cvt->*convert)(CVT_PROBE[i]);
    if (probe[1] == 0x001F && probe[3] == 0xF800 && probe[4] == 0x0000) {
        if (probe[0] == 0xFFDF && probe[2] == 0x07C0 && probe[5] == 0x0400) rgb555 = 1;       // zero-extended green LSB
        else if (probe[0] == 0xFFFF && probe[2] == 0x07E0 && probe[5] == 0x0420) rgb555 = 2;  // replicated green MSB
    }
    if (!rgb555) {
        if (!m_cvt16) {
            m_cvt16 = (USHORT *)LocalAlloc(LMEM_FIXED, 65536 * sizeof(USHORT));
            if (!m_cvt16) return EmulatedBlt(pParms);
            m_cvtProbe[0] = ~probe[0];                       // force the fill below
        }
        if (memcmp(probe, m_cvtProbe, sizeof probe) != 0) {
            for (i = 0; i < 65536; i++) m_cvt16[i] = (USHORT)(cvt->*convert)((unsigned long)i);
            memcpy(m_cvtProbe, probe, sizeof probe);
        }
    }
    if (pParms->prclClip) {
        RECTL *c = pParms->prclClip;
        if (c->left > dst.left)     { sx += c->left - dst.left; dst.left = c->left; }
        if (c->top > dst.top)       { sy += c->top - dst.top;   dst.top = c->top; }
        if (c->right < dst.right)   dst.right = c->right;
        if (c->bottom < dst.bottom) dst.bottom = c->bottom;
    }
    // stay inside both surfaces (GWES clips, but a table lookup must not read or write past a buffer)
    if (dst.left < 0)   { sx -= dst.left; dst.left = 0; }
    if (dst.top < 0)    { sy -= dst.top;  dst.top = 0; }
    if (sx < 0)         { dst.left -= sx; sx = 0; }
    if (sy < 0)         { dst.top -= sy;  sy = 0; }
    if (dst.right > pParms->pDst->Width())   dst.right = pParms->pDst->Width();
    if (dst.bottom > pParms->pDst->Height()) dst.bottom = pParms->pDst->Height();
    w = dst.right - dst.left; h = dst.bottom - dst.top;
    if (sx + w > pParms->pSrc->Width())  w = pParms->pSrc->Width() - sx;
    if (sy + h > pParms->pSrc->Height()) h = pParms->pSrc->Height() - sy;
    if (w > 0 && h > 0) {
        int sstride = pParms->pSrc->Stride(), dstride = pParms->pDst->Stride();
        const BYTE *src = (const BYTE *)pParms->pSrc->Buffer() + sy * sstride + sx * 2;
        BYTE *dp = (BYTE *)pParms->pDst->Buffer() + dst.top * dstride + dst.left * 2;
        if (rgb555 == 1) {
            for (y = 0; y < h; y++, src += sstride, dp += dstride) {
                const USHORT *s = (const USHORT *)src;
                USHORT *d = (USHORT *)dp;
                int n = w;
                if ((((ULONG)s ^ (ULONG)d) & 2) == 0) {
                    if (((ULONG)d & 2) && n > 0) { USHORT v = *s++; *d++ = (USHORT)(((v & 0x7FE0) << 1) | (v & 0x001F)); n--; }
                    const UINT32 *s32 = (const UINT32 *)s;
                    UINT32 *d32 = (UINT32 *)d;
                    while (n >= 2) { UINT32 v = *s32++; *d32++ = ((v & 0x7FE07FE0u) << 1) | (v & 0x001F001Fu); n -= 2; }
                    s = (const USHORT *)s32; d = (USHORT *)d32;
                }
                while (n-- > 0) { USHORT v = *s++; *d++ = (USHORT)(((v & 0x7FE0) << 1) | (v & 0x001F)); }
            }
        } else if (rgb555 == 2) {
            for (y = 0; y < h; y++, src += sstride, dp += dstride) {
                const USHORT *s = (const USHORT *)src;
                USHORT *d = (USHORT *)dp;
                int n = w;
                if ((((ULONG)s ^ (ULONG)d) & 2) == 0) {
                    if (((ULONG)d & 2) && n > 0) { USHORT v = *s++; *d++ = (USHORT)(((v & 0x7FE0) << 1) | (v & 0x001F) | ((v & 0x0200) >> 4)); n--; }
                    const UINT32 *s32 = (const UINT32 *)s;
                    UINT32 *d32 = (UINT32 *)d;
                    while (n >= 2) { UINT32 v = *s32++; *d32++ = ((v & 0x7FE07FE0u) << 1) | (v & 0x001F001Fu) | ((v & 0x02000200u) >> 4); n -= 2; }
                    s = (const USHORT *)s32; d = (USHORT *)d32;
                }
                while (n-- > 0) { USHORT v = *s++; *d++ = (USHORT)(((v & 0x7FE0) << 1) | (v & 0x001F) | ((v & 0x0200) >> 4)); }
            }
        } else {
            const USHORT *tab = m_cvt16;
            for (y = 0; y < h; y++, src += sstride, dp += dstride) {
                const USHORT *s = (const USHORT *)src;
                USHORT *d = (USHORT *)dp;
                for (x = 0; x < w; x++) d[x] = tab[s[x]];
            }
        }
        if (pParms->pDst == m_pPrimarySurface) DirtyRect(dst.left, dst.top, dst.left + w, dst.top + h);
    }
    return S_OK;
}

SCODE ClasspadGPE::WrappedBlt(GPEBltParms *pParms)
{
    RECTL dst, clip;
    BOOL hasDst = (pParms->prclDst != NULL);
    BOOL hasClip = (pParms->prclClip != NULL);
    if (hasDst) dst = *pParms->prclDst;
    if (hasClip) clip = *pParms->prclClip;
    SCODE sc = EmulatedBlt(pParms);
    if (pParms->pDst == m_pPrimarySurface) {
        if (hasDst) {
            int left = dst.left, top = dst.top;
            int right = dst.right, bottom = dst.bottom;
            if (left > right) { int t = left; left = right; right = t; }
            if (top > bottom) { int t = top; top = bottom; bottom = t; }
            if (hasClip) {
                if (clip.left > left)     left = clip.left;
                if (clip.top > top)       top = clip.top;
                if (clip.right < right)   right = clip.right;
                if (clip.bottom < bottom) bottom = clip.bottom;
            }
            DirtyRect(left, top, right, bottom);
        } else {
            Dirty(0, m_nScreenHeight);
        }
    }
    return sc;
}

// DCS panel: wake it and set 16 bpp. DISPON follows the first push, so the
// panel never shows GRAM the driver has not written (and the emulator stays
// blank until then). The 120 ms is the DCS sleep-out settling time; the
// emulator needs none of this sequence beyond SLPOUT and DISPON.
void ClasspadGPE::PanelInit()
{
    LcdCommand(LCD_DCS_SLPOUT);
    Sleep(120);
    LcdCommand(LCD_DCS_COLMOD);
    LcdParam(LCD_DCS_COLMOD_16BPP);
}

// The panel's write window: physical columns [x0, x1), rows [y0, y1), then
// the pixel stream (DCS: RAMWR; R61524: its address registers and data select).
void ClasspadGPE::SetWindow(unsigned x0, unsigned x1, unsigned y0, unsigned y1)
{
    if (m_panel == LCD_PANEL_DCS) {
        x0 += m_col0; x1 += m_col0;         // the older panel's visible columns start at 40
        LcdCommand(LCD_DCS_CASET);
        LcdParam((UCHAR)(x0 >> 8));         LcdParam((UCHAR)x0);
        LcdParam((UCHAR)((x1 - 1) >> 8));   LcdParam((UCHAR)(x1 - 1));
        LcdCommand(LCD_DCS_PASET);
        LcdParam((UCHAR)(y0 >> 8));         LcdParam((UCHAR)y0);
        LcdParam((UCHAR)((y1 - 1) >> 8));   LcdParam((UCHAR)(y1 - 1));
        LcdCommand(LCD_DCS_RAMWR);
    } else {
        LcdWriteReg(LCD_REG_HSTART, (USHORT)((LCD_CG100_GRAM_COLS - 1) - (LCD_CG100_COL0 + x1 - 1)));
        LcdWriteReg(LCD_REG_HEND,   (USHORT)((LCD_CG100_GRAM_COLS - 1) - (LCD_CG100_COL0 + x0)));
        LcdWriteReg(LCD_REG_VSTART, (USHORT)y0);
        LcdWriteReg(LCD_REG_VEND,   (USHORT)(y1 - 1));
        LcdWriteReg(LCD_REG_HADDR,  0);
        LcdWriteReg(LCD_REG_VADDR,  0);
        LcdSelect(LCD_REG_DATA);
    }
}

// Push the logical rectangle [left, right) x [top, bottom) of the frame to the
// panel. Maps the dirty box to the physical DCS window via ClasspadRotRect,
// passes the P1 cached alias when cache-colour-aligned (skipping CacheRangeFlush),
// and bounds each HC_LCD_PUSH trap by a pixel budget (DISP_CHUNK_PIXELS) rather
// than 4-row strips so full frames need ~11 hypercalls instead of 132.
void ClasspadGPE::PushRect(int left, int top, int right, int bottom)
{
    const BYTE *fb = (const BYTE *)m_pPrimarySurface->Buffer();
    int stride = m_pPrimarySurface->Stride(), width = m_nScreenWidthSave, height = m_nScreenHeightSave, x, y;
    unsigned rot = (unsigned)m_iRotate, x0, x1, y0, y1, pw, chunkRows;
    DWORD fbBase;
    if (!fb) return;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > m_nScreenWidth) right = m_nScreenWidth;
    if (bottom > m_nScreenHeight) bottom = m_nScreenHeight;
    if (left >= right || top >= bottom) return;
    ClasspadRotRect(rot, (unsigned)width, (unsigned)height,
                    (unsigned)left, (unsigned)top, (unsigned)right, (unsigned)bottom,
                    &x0, &x1, &y0, &y1);
    if (m_panel != LCD_PANEL_DCS) { x0 = 0; x1 = (unsigned)width; y1 = (unsigned)height; }
    if (y0 < (unsigned)m_consoleRows) y0 = (unsigned)m_consoleRows;      // the console's physical rows are not ours
    if (y0 >= y1 || x0 >= x1) return;
    pw = x1 - x0;
    chunkRows = DISP_CHUNK_PIXELS / pw;
    if (chunkRows < 4u) chunkRows = 4u;
    fbBase = (m_frameP1Safe ? 0x80000000u : 0xA0000000u) | (m_framePa & 0x1FFFFFFFu);

    if (m_panel == LCD_PANEL_DCS && m_pFrame && m_blit == 1 && ClasspadCanTrap()) {
        if (!m_frameP1Safe) {
            const BYTE *src = fb + top * stride;
            DWORD len = (DWORD)(bottom - top) * stride;
            CacheRangeFlush((LPVOID)src, len, CACHE_SYNC_WRITEBACK);
        }
        if (rot == DMDO_0 && x0 == 0 && x1 == (unsigned)width) {
            for (y = (int)y0; y < (int)y1; y += (int)chunkRows) {
                DWORD rows = (DWORD)(((unsigned)(y1 - y) < chunkRows) ? (y1 - (unsigned)y) : chunkRows);
                ClasspadHcLcdPush(fbBase + (DWORD)y * (DWORD)stride, (DWORD)y, rows, (DWORD)width);
            }
        } else {
            for (y = (int)y0; y < (int)y1; y += (int)chunkRows) {
                unsigned ye = (unsigned)y + chunkRows < y1 ? (unsigned)y + chunkRows : y1;
                ClasspadHcLcdPush(fbBase, (DWORD)y | (ye << 16), x0 | (x1 << 16), CLASSPAD_ROT_PUSH_TAG | rot);
            }
        }
    } else if (rot == DMDO_0 && x0 == 0 && x1 == (unsigned)width) {
        SetWindow(0, (unsigned)width, y0, y1);
        if (m_pFrame && m_blit && ClasspadCanTrap()) {
            if (!m_frameP1Safe) {
                const BYTE *src = fb + y0 * stride;
                DWORD len = (DWORD)(y1 - y0) * stride;
                CacheRangeFlush((LPVOID)src, len, CACHE_SYNC_WRITEBACK);
            }
            ClasspadHcLcdBlit(fbBase + (DWORD)y0 * (DWORD)stride, (DWORD)(y1 - y0) * width, LCD_DATA_PORT);
        } else {
            for (y = (int)y0; y < (int)y1; y++) {
                const USHORT *row = (const USHORT *)(fb + y * stride);
                for (x = 0; x < width; x++) LcdWriteData(row[x]);
            }
        }
    } else {
        SetWindow(x0, x1, y0, y1);
        for (y = (int)y0; y < (int)y1; y++)
            for (x = (int)x0; x < (int)x1; x++)
                LcdWriteData(*(const USHORT *)(fb + ClasspadRotOffset(rot, (unsigned)width, (unsigned)height, (unsigned)x, (unsigned)y)));
    }
    if (m_panel == LCD_PANEL_DCS && !m_displayOn) {
        LcdCommand(LCD_DCS_DISPON);
        m_displayOn = TRUE;
    }
    m_pushes++;
    if (m_pushes <= 3 || (m_pushes % 200) == 0)
        RETAILMSG(1, (TEXT("CLASSPAD DDI: push %d, logical %d,%d..%d,%d, window %u..%u x %u..%u\r\n"), m_pushes, left, top, right, bottom, x0, x1, y0, y1));
}

void ClasspadGPE::PushRows(int top, int bottom)
{
    PushRect(0, top, m_nScreenWidth, bottom);
}

// Power off / on from GWES. Called with the system mostly stopped: register
// writes only, no synchronisation. Off: backlight to 0, blank and sleep the
// DCS panel (GRAM kept). On: wake it and let the push thread, whose wait
// times out every DISP_PUSH_MS, repaint the whole screen and put the
// backlight level back; DISPON follows that push. The R61524 path does
// nothing, as before.
//
// The fx-CP400's backlight follows the panel's brightness register (DCS
// 0x51), not SLPIN: with DISPOFF + SLPIN alone the backlight stays on and
// the normally-white panel shows white for as long as the machine is off
// (user report 2026-11-06). The stock OS powers the panel down with
// 0x51 = 0, DISPOFF, SLPIN (ROM 0x80038a9a, 0x800380ba); it then also puts
// the panel into deep standby (0xB9/0xBD/0xB1, which needs a full re-init)
// and stops a port and a timer channel, which CE does not do: it keeps the
// Casio OS's panel set-up and only needs SLPOUT to come back.
// Busy wait on the RTC's R64CNT (bits 6..0 count at 128 Hz): PowerHandler
// runs with the scheduler stopped, Sleep is not available there. Bounded
// in reads too, in case the RTC is not running.
static void PanelWaitTicks(UINT ticks)
{
    volatile UCHAR *r64 = (volatile UCHAR *)0xA413FEC0;
    UINT last = *r64 & 0x7F, now, elapsed = 0;
    DWORD reads;
    for (reads = 0; elapsed < ticks && reads < 4000000; reads++) {
        now = *r64 & 0x7F;
        elapsed += (now - last) & 0x7F;
        last = now;
    }
}

VOID ClasspadGPE::PowerHandler(BOOL bOff)
{
    if (m_panel != LCD_PANEL_DCS) return;
    if (bOff) {
        m_suspended = TRUE;
        m_resumePending = FALSE;
        // m_brightness is what the wake-up puts back: the level read once at
        // start-up or the last one the backlight applet set. It used to be
        // read from the panel here; on the calculator that read came back
        // 0xFF and every wake-up ended at full brightness (2026-11-10).
        // The OS's timing (ROM 0x80038acc): the backlight dims over a few
        // frames, so DISPOFF only once it is dark (60 ms; DISPOFF right after
        // 0x51 = 0 showed the white panel for a moment), then SLPIN 60 ms
        // after DISPOFF. 8 ticks of R64CNT = 62.5 ms.
        // (The older panel takes the level in register 0xB9; the OS does
        // not zero it there before DISPOFF but goes on to deep standby,
        // which CE does not, so the level is zeroed here as well.)
        LcdSetBacklight(m_panelNew, 0);
        PanelWaitTicks(8);
        LcdCommand(LCD_DCS_DISPOFF);
        PanelWaitTicks(8);
        LcdCommand(LCD_DCS_SLPIN);
        m_displayOn = FALSE;
    } else {
        m_resumePending = TRUE;
        m_suspended = FALSE;
    }
}

DWORD WINAPI ClasspadGPE::PushThread(LPVOID pThis)
{
    ClasspadGPE *p = (ClasspadGPE *)pThis;
    DWORD nextPush = GetTickCount();
    while (p->m_run) {
        int left, top, right, bottom;
        WaitForSingleObject(p->m_hEvent, p->m_suspended ? DISP_PUSH_MS : DISP_IDLE_MS);
        if (!p->m_run) break;
        if (p->m_suspended) continue;
        if (p->m_resumePending) {
            EnterCriticalSection(&p->m_csLcd);
            p->m_resumePending = FALSE;
            LcdCommand(LCD_DCS_SLPOUT);
            Sleep(120); // panel recovery must run in thread context
            if (p->m_suspended) { LeaveCriticalSection(&p->m_csLcd); continue; }
            LcdSetBacklight(p->m_panelNew, p->m_brightness);   // the backlight back (PowerHandler set it to 0)
            LeaveCriticalSection(&p->m_csLcd);
            p->Dirty(0, p->m_nScreenHeight);
        } else if (p->m_dirtyTop >= p->m_dirtyBottom || p->m_dirtyLeft >= p->m_dirtyRight) {
            continue;
        }
        // An event wakes the thread immediately; it is not a rate limit.
        // Keep collecting dirty regions until the next allowed push time.
        {
            LONG wait = (LONG)(nextPush - GetTickCount());
            if (wait > 0) Sleep((DWORD)wait);
        }
        if (!p->m_run) break;
        if (p->m_suspended) continue;
        EnterCriticalSection(&p->m_cs);
        left = p->m_dirtyLeft; top = p->m_dirtyTop;
        right = p->m_dirtyRight; bottom = p->m_dirtyBottom;
        p->m_dirtyLeft = p->m_nScreenWidth; p->m_dirtyRight = 0;
        p->m_dirtyTop = p->m_nScreenHeight; p->m_dirtyBottom = 0;
        LeaveCriticalSection(&p->m_cs);
        if (left < right && top < bottom) {
            EnterCriticalSection(&p->m_csLcd);
            p->PushRect(left, top, right, bottom);
            LeaveCriticalSection(&p->m_csLcd);
            nextPush = GetTickCount() + DISP_PUSH_MS;
        }
    }
    return 0;
}

//------------------------------------------------------------------------------
//  Entry points
//------------------------------------------------------------------------------
static ClasspadGPE *g_pGPE;
static TCHAR g_szInstance[256];

extern "C" BOOL APIENTRY DisplayInit(LPCTSTR pszInstance, DWORD dwNumMonitors)
{
    if (pszInstance) _tcsncpy(g_szInstance, pszInstance, sizeof g_szInstance / sizeof g_szInstance[0] - 1);
    RETAILMSG(1, (TEXT("CLASSPAD DDI: DisplayInit(%s, %d)\r\n"), pszInstance ? pszInstance : TEXT("<null>"), dwNumMonitors));
    return TRUE;
}

extern "C" BOOL APIENTRY DrvEnableDriver(ULONG engineVersion, ULONG cj, DRVENABLEDATA *data, PENGCALLBACKS engineCallbacks)
{
    return GPEEnableDriver(engineVersion, cj, data, engineCallbacks);
}

extern "C" ULONG *APIENTRY DrvGetMasks(DHPDEV)
{
    return g_bitMasks;
}

GPE *GetGPE(void)
{
    if (!g_pGPE) g_pGPE = new ClasspadGPE();
    return g_pGPE;
}

// DllMain comes from gpe.lib (ddi_if.obj).
