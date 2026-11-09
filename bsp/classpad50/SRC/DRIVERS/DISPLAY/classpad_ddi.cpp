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
#include <windows.h>
#include <types.h>
#include <winddi.h>
#include <gpe.h>
#include <pwingdi.h>
#include "classpad_lcd.h"
#include "classpad_hc.h"
#include "classpad_lcdrot.h"

#define DISP_PUSH_MS        50
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
    int                 m_dirtyTop;
    int                 m_dirtyBottom;
    DWORD               m_pushes;
    int                 m_panel;            // LCD_PANEL_DCS / LCD_PANEL_R61524
    volatile BOOL       m_suspended, m_resumePending;
    volatile BOOL       m_displayOn;        // DCS: DISPON sent (after the first push)
    UCHAR               m_brightness;       // DCS: the backlight level saved at power-off
    BYTE               *m_pFrame;           // the primary surface, physically contiguous (AllocPhysMem)
    ULONG               m_framePa;          // its physical address
    int                 m_blit;             // registry "Blit": 1 = HC_LCD_PUSH (default), 2 = HC_LCD_BLIT with our window commands, 0 = translated stores
    int                 m_consoleRows;      // top rows left to the translator's console (registry "Console"), else 0
    ClasspadSurf       *m_pPrimary;         // m_pPrimarySurface, with Relayout
    USHORT             *m_cvt16;            // 16 -> 16 bpp colour conversion table for ConvertedCopyBlt16 (65536 entries)
    ULONG               m_cvtProbe[4];      // what the table's converter made of CVT_PROBE: identifies it

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
    SCODE           ConvertedCopyBlt16(GPEBltParms *pParms);
    void            SetWindow(unsigned x0, unsigned x1, unsigned y0, unsigned y1);
    void            Dirty(int top, int bottom);
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

    m_iRotate = DMDO_0;
    m_cvt16 = NULL;
    m_cvtProbe[0] = m_cvtProbe[1] = m_cvtProbe[2] = m_cvtProbe[3] = 0;
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
    // its rows to the translator by physical address (PushRows). "Blit"=0
    // in the registry keeps the translated store loop instead, 2 the
    // pixels-only hypercall with the window commands from here.
    m_blit    = (int)RegDword(TEXT("Blit"), 1);
    m_framePa = 0;
    m_pFrame  = (BYTE *)AllocPhysMem(width * 2 * height, PAGE_READWRITE, 0, 0, &m_framePa);
    if (m_pFrame) m_pPrimary = new ClasspadSurf(width, height, m_pFrame, width * 2, gpe16Bpp);
    else          m_pPrimary = new ClasspadSurf(width, height, gpe16Bpp);
    m_pPrimarySurface = m_pPrimary;
    if (m_pPrimarySurface && m_pPrimarySurface->Buffer()) {
        m_pPrimarySurface->SetRotation(width, height, DMDO_0);
        memset(m_pPrimarySurface->Buffer(), 0, m_pPrimarySurface->Stride() * height);
    }

    if (m_panel == LCD_PANEL_DCS) PanelInit();

    InitializeCriticalSection(&m_cs);
    m_dirtyTop = 0; m_dirtyBottom = height;             // first push: the whole screen
    m_pushes = 0;
    m_run = TRUE;
    m_hEvent = CreateEvent(NULL, FALSE, TRUE, NULL);
    m_hThread = CreateThread(NULL, 0, PushThread, this, 0, NULL);
    if (m_hThread) CeSetThreadPriority(m_hThread, 250);  // below normal application threads

    RETAILMSG(1, (TEXT("CLASSPAD DDI: %dx%d 16bpp, %s panel, frame buffer %08x (phys %08x), stride %d, push %s\r\n"),
                  width, height, m_panel == LCD_PANEL_DCS ? TEXT("DCS fx-CP400") : TEXT("R61524 fx-CG100"),
                  m_pPrimarySurface ? m_pPrimarySurface->Buffer() : NULL, m_framePa,
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

// Logical rows [top, bottom) changed (the surface's own rows, whatever the rotation)
void ClasspadGPE::Dirty(int top, int bottom)
{
    if (top < 0) top = 0;
    if (bottom > m_nScreenHeight) bottom = m_nScreenHeight;
    if (top >= bottom) return;
    EnterCriticalSection(&m_cs);
    // One wakeup per pending batch. Additional drawing widens the range
    // already owned by the pending wakeup (or the active push thread).
    BOOL notify = m_dirtyTop >= m_dirtyBottom;
    if (top < m_dirtyTop) m_dirtyTop = top;
    if (bottom > m_dirtyBottom) m_dirtyBottom = bottom;
    LeaveCriticalSection(&m_cs);
    if (notify) SetEvent(m_hEvent);
}

SCODE ClasspadGPE::Line(GPELineParms *pParms, EGPEPhase phase)
{
    if (phase == gpeSingle || phase == gpePrepare)
        pParms->pLine = (SCODE (GPE::*)(GPELineParms *))&ClasspadGPE::WrappedLine;
    return S_OK;
}

SCODE ClasspadGPE::WrappedLine(GPELineParms *pParms)
{
    SCODE sc = EmulatedLine(pParms);
    if (pParms->pDst == m_pPrimarySurface) {
        if (pParms->prclClip) Dirty(pParms->prclClip->top, pParms->prclClip->bottom);
        else Dirty(0, m_nScreenHeight);
    }
    return sc;
}

// GWES hands a colour converter (an XLATEOBJ) to a blit whose source and
// destination pixel formats differ. GPE's EmulatedBlt then converts pixel by
// pixel through a virtual call inside its generic rop/mask/clip machinery:
// Plotter-Z's back buffer is a 16 bpp BI_RGB DIB (RGB555) blitted onto the
// RGB565 primary after every column of a recalculation, and that path was
// 40% of the recalculation's time under the translator (2026-11-09). A plain
// copy with a converter (SRCCOPY, no mask, no stretch, no alpha, same size,
// 16 bpp both sides) goes through a 65536-entry table instead. The table is
// filled from the converter itself (one virtual call per entry, once) and
// identified by what the converter makes of four probe values, since GWES
// may hand out a new converter object for the same translation.
static const ULONG CVT_PROBE[4] = { 0x7FFF, 0x001F, 0x03E0, 0x7C00 };

SCODE ClasspadGPE::BltPrepare(GPEBltParms *pParms)
{
    pParms->pBlt = (SCODE (GPE::*)(GPEBltParms *))&ClasspadGPE::WrappedBlt;
    if (pParms->rop4 == 0xCCCC && pParms->pSrc && pParms->pDst && !pParms->pMask &&
        pParms->pConvert && pParms->pColorConverter && !pParms->pLookup &&
        !(pParms->bltFlags & (BLT_STRETCH | BLT_TRANSPARENT | BLT_ALPHABLEND)) &&
        pParms->pSrc->Format() == gpe16Bpp && pParms->pDst->Format() == gpe16Bpp &&
        pParms->prclSrc && pParms->prclDst &&
        pParms->prclSrc->right - pParms->prclSrc->left == pParms->prclDst->right - pParms->prclDst->left &&
        pParms->prclSrc->bottom - pParms->prclSrc->top == pParms->prclDst->bottom - pParms->prclDst->top)
        pParms->pBlt = (SCODE (GPE::*)(GPEBltParms *))&ClasspadGPE::ConvertedCopyBlt16;
    return S_OK;
}

SCODE ClasspadGPE::ConvertedCopyBlt16(GPEBltParms *pParms)
{
    ColorConverter *cvt = pParms->pColorConverter;
    unsigned long (ColorConverter::*convert)(unsigned long) = pParms->pConvert;
    ULONG probe[4];
    RECTL dst = *pParms->prclDst;
    int sx = pParms->prclSrc->left, sy = pParms->prclSrc->top, x, y, w, h;
    int i;
    for (i = 0; i < 4; i++) probe[i] = (cvt->*convert)(CVT_PROBE[i]);
    if (!m_cvt16) {
        m_cvt16 = (USHORT *)LocalAlloc(LMEM_FIXED, 65536 * sizeof(USHORT));
        if (!m_cvt16) return EmulatedBlt(pParms);
        m_cvtProbe[0] = ~probe[0];                       // force the fill below
    }
    if (probe[0] != m_cvtProbe[0] || probe[1] != m_cvtProbe[1] || probe[2] != m_cvtProbe[2] || probe[3] != m_cvtProbe[3]) {
        for (i = 0; i < 65536; i++) m_cvt16[i] = (USHORT)(cvt->*convert)((unsigned long)i);
        for (i = 0; i < 4; i++) m_cvtProbe[i] = probe[i];
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
        const USHORT *tab = m_cvt16;
        int sstride = pParms->pSrc->Stride(), dstride = pParms->pDst->Stride();
        const BYTE *src = (const BYTE *)pParms->pSrc->Buffer() + sy * sstride + sx * 2;
        BYTE *dp = (BYTE *)pParms->pDst->Buffer() + dst.top * dstride + dst.left * 2;
        for (y = 0; y < h; y++, src += sstride, dp += dstride) {
            const USHORT *s = (const USHORT *)src;
            USHORT *d = (USHORT *)dp;
            for (x = 0; x < w; x++) d[x] = tab[s[x]];
        }
        if (pParms->pDst == m_pPrimarySurface) Dirty(dst.top, dst.top + h);
    }
    return S_OK;
}

SCODE ClasspadGPE::WrappedBlt(GPEBltParms *pParms)
{
    static int logged;
    SCODE sc = EmulatedBlt(pParms);
    if (pParms->pDst == m_pPrimarySurface) {
        if (pParms->prclDst) Dirty(pParms->prclDst->top, pParms->prclDst->bottom);
        else Dirty(0, m_nScreenHeight);
    }
    // Diagnostic (2026-11-04): exact-colour fills and lines reach the frame
    // buffer, blits with a dithered brush or a glyph mask seem not to. Log
    // the first blits: what GWES asks for and what EmulatedBlt says.
    if (logged < 48) {
        logged++;
        RETAILMSG(1, (TEXT("CLASSPAD DDI: blt rop4=%04X dst=%d,%d-%d,%d %s src=%d mask=%d brush=%d flags=%08X color=%08X -> %08X\r\n"),
                      (UINT)pParms->rop4,
                      pParms->prclDst ? pParms->prclDst->left : -1, pParms->prclDst ? pParms->prclDst->top : -1,
                      pParms->prclDst ? pParms->prclDst->right : -1, pParms->prclDst ? pParms->prclDst->bottom : -1,
                      pParms->pDst == m_pPrimarySurface ? TEXT("screen") : TEXT("offscreen"),
                      pParms->pSrc ? (int)pParms->pSrc->Format() : -1,
                      pParms->pMask ? (int)pParms->pMask->Format() : -1,
                      pParms->pBrush ? (int)pParms->pBrush->Format() : -1,
                      (UINT)pParms->bltFlags, (UINT)pParms->solidColor, (UINT)sc));
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

// Push the logical rows [top, bottom) of the frame to the panel. Portrait:
// the rows as they are, by the translator (HC_LCD_PUSH, "Blit"=1), its
// pixel-only hypercall after our window commands ("Blit"=2) or translated
// stores. Rotated: the logical buffer is the panel turned by the angle
// (classpad_lcdrot.h), so the rows are a physical window (columns for 90 and
// 270) streamed in panel order from transposed reads, by the translator's
// rotated form of HC_LCD_PUSH or, without it, here. R61524 (fx-CG100):
// portrait only, with its bottom-of-frame presentation rule kept.
void ClasspadGPE::PushRows(int top, int bottom)
{
    const BYTE *fb = (const BYTE *)m_pPrimarySurface->Buffer();
    int stride = m_pPrimarySurface->Stride(), width = m_nScreenWidthSave, height = m_nScreenHeightSave, x, y;
    unsigned rot = (unsigned)m_iRotate, x0, x1, y0, y1;
    if (!fb) return;
    if (top < 0) top = 0;
    if (bottom > m_nScreenHeight) bottom = m_nScreenHeight;
    if (top >= bottom) return;
    ClasspadRotWindow(rot, (unsigned)width, (unsigned)height, (unsigned)top, (unsigned)bottom, &x0, &x1, &y0, &y1);
    if (m_panel != LCD_PANEL_DCS) y1 = (unsigned)height;
    if (y0 < (unsigned)m_consoleRows) y0 = (unsigned)m_consoleRows;      // the console's physical rows are not ours
    if (y0 >= y1 || x0 >= x1) return;
    if (rot == DMDO_0) {
        const BYTE *src = fb + y0 * stride;
        DWORD len = (DWORD)(y1 - y0) * stride;
        if (m_panel == LCD_PANEL_DCS && m_pFrame && m_blit == 1 && ClasspadCanTrap()) {
            // Native push by the translator with the window set there too
            // (HC_LCD_PUSH, docs/04): write the rows back from the cache, then
            // hand it the P2 alias of their physical address. One hypercall
            // replaces width*rows translated stores, each of which cost a swap
            // sequence, and nothing the translator prints (its panel console)
            // can land between the window commands and the pixels.
            CacheRangeFlush((LPVOID)src, len, CACHE_SYNC_WRITEBACK);
            // Each trap runs with BL=1. Bound it to four rows and return to
            // guest code between strips so pending input/timer IRQs can run.
            for (y = (int)y0; y < (int)y1; y += 4) {
                DWORD rows = (DWORD)(((int)y1 - y < 4) ? (int)y1 - y : 4);
                ClasspadHcLcdPush(0xA0000000 | ((m_framePa + y * stride) & 0x1FFFFFFF), (DWORD)y, rows, (DWORD)width);
            }
        } else {
            SetWindow(0, (unsigned)width, y0, y1);
            if (m_pFrame && m_blit && ClasspadCanTrap()) {
                // pixels by the translator (HC_LCD_BLIT), the window by us: "Blit"=2, or the R61524
                CacheRangeFlush((LPVOID)src, len, CACHE_SYNC_WRITEBACK);
                ClasspadHcLcdBlit(0xA0000000 | ((m_framePa + y0 * stride) & 0x1FFFFFFF), (DWORD)(y1 - y0) * width, LCD_DATA_PORT);
            } else {
                for (y = (int)y0; y < (int)y1; y++) {
                    const USHORT *row = (const USHORT *)(fb + y * stride);
                    for (x = 0; x < width; x++) LcdWriteData(row[x]);
                }
            }
        }
    } else {
        // The logical rows [top, bottom) hold every pixel of the window, contiguous in the buffer
        const BYTE *src = fb + top * stride;
        DWORD len = (DWORD)(bottom - top) * stride;
        if (m_panel == LCD_PANEL_DCS && m_pFrame && m_blit == 1 && ClasspadCanTrap()) {
            CacheRangeFlush((LPVOID)src, len, CACHE_SYNC_WRITEBACK);
            for (y = (int)y0; y < (int)y1; y += 4) {
                unsigned ye = (unsigned)y + 4 < y1 ? (unsigned)y + 4 : y1;
                ClasspadHcLcdPush(0xA0000000 | (m_framePa & 0x1FFFFFFF), (DWORD)y | (ye << 16), x0 | (x1 << 16), CLASSPAD_ROT_PUSH_TAG | rot);
            }
        } else {
            SetWindow(x0, x1, y0, y1);
            for (y = (int)y0; y < (int)y1; y++)
                for (x = (int)x0; x < (int)x1; x++)
                    LcdWriteData(*(const USHORT *)(fb + ClasspadRotOffset(rot, (unsigned)width, (unsigned)height, (unsigned)x, (unsigned)y)));
        }
    }
    if (m_panel == LCD_PANEL_DCS && !m_displayOn) {
        LcdCommand(LCD_DCS_DISPON);
        m_displayOn = TRUE;
    }
    m_pushes++;
    if (m_pushes <= 3 || (m_pushes % 200) == 0)
        RETAILMSG(1, (TEXT("CLASSPAD DDI: push %d, logical rows %d..%d, window %u..%u x %u..%u\r\n"), m_pushes, top, bottom, x0, x1, y0, y1));
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
        UCHAR level;
        m_suspended = TRUE;
        m_resumePending = FALSE;
        LcdCommand(LCD_DCS_RDDISBV);
        (void)LcdReadParam();                   // dummy
        level = LcdReadParam();
        // 0 is what the emulator reads (no RDDISBV there), and a level this
        // low would not have been the user's: come back at full brightness
        m_brightness = level >= 0x10 ? level : LCD_BRIGHTNESS_DEFAULT;
        // The OS's timing (ROM 0x80038acc): the backlight dims over a few
        // frames, so DISPOFF only once it is dark (60 ms; DISPOFF right after
        // 0x51 = 0 showed the white panel for a moment), then SLPIN 60 ms
        // after DISPOFF. 8 ticks of R64CNT = 62.5 ms.
        LcdCommand(LCD_DCS_WRDISBV);
        LcdParam(0);
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
        int top, bottom;
        WaitForSingleObject(p->m_hEvent, DISP_PUSH_MS);
        // An event wakes the thread immediately; it is not a rate limit.
        // Keep collecting dirty rows until the next allowed push time.
        LONG wait = (LONG)(nextPush - GetTickCount());
        if (wait > 0) Sleep((DWORD)wait);
        if (!p->m_run) break;
        if (p->m_suspended) continue;
        if (p->m_resumePending) {
            p->m_resumePending = FALSE;
            LcdCommand(LCD_DCS_SLPOUT);
            Sleep(120); // panel recovery must run in thread context
            if (p->m_suspended) continue;
            LcdCommand(LCD_DCS_WRDISBV);         // the backlight back (PowerHandler set it to 0)
            LcdParam(p->m_brightness);
            p->Dirty(0, p->m_nScreenHeight);
        }
        EnterCriticalSection(&p->m_cs);
        top = p->m_dirtyTop; bottom = p->m_dirtyBottom;
        p->m_dirtyTop = p->m_nScreenHeight; p->m_dirtyBottom = 0;
        LeaveCriticalSection(&p->m_cs);
        if (top < bottom) {
            p->PushRows(top, bottom);
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
