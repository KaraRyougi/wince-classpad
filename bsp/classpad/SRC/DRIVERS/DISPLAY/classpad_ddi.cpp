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
#include "classpad_lcd.h"

#define DISP_PUSH_MS        50
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
    volatile BOOL       m_displayOn;        // DCS: DISPON sent (after the first push)

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

    void            PanelInit();
    SCODE           WrappedLine(GPELineParms *pParms);
    SCODE           WrappedBlt(GPEBltParms *pParms);
    void            Dirty(int top, int bottom);
    void            PushRows(int top);
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

    m_pPrimarySurface = new GPESurf(width, height, gpe16Bpp);
    if (m_pPrimarySurface && m_pPrimarySurface->Buffer())
        memset(m_pPrimarySurface->Buffer(), 0, m_pPrimarySurface->Stride() * height);

    if (m_panel == LCD_PANEL_DCS) PanelInit();

    InitializeCriticalSection(&m_cs);
    m_dirtyTop = 0; m_dirtyBottom = height;             // first push: the whole screen
    m_pushes = 0;
    m_run = TRUE;
    m_hEvent = CreateEvent(NULL, FALSE, TRUE, NULL);
    m_hThread = CreateThread(NULL, 0, PushThread, this, 0, NULL);
    if (m_hThread) CeSetThreadPriority(m_hThread, 250);  // below normal application threads

    RETAILMSG(1, (TEXT("CLASSPAD DDI: %dx%d 16bpp, %s panel, frame buffer %08x, stride %d\r\n"),
                  width, height, m_panel == LCD_PANEL_DCS ? TEXT("DCS fx-CP400") : TEXT("R61524 fx-CG100"),
                  m_pPrimarySurface ? m_pPrimarySurface->Buffer() : NULL,
                  m_pPrimarySurface ? m_pPrimarySurface->Stride() : 0));
}

ClasspadGPE::~ClasspadGPE()
{
    m_run = FALSE;
    if (m_hEvent) SetEvent(m_hEvent);
    if (m_hThread) { WaitForSingleObject(m_hThread, 2000); CloseHandle(m_hThread); }
    if (m_hEvent) CloseHandle(m_hEvent);
    DeleteCriticalSection(&m_cs);
    delete m_pPrimarySurface;
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

SCODE ClasspadGPE::AllocSurface(GPESurf **ppSurf, int width, int height, EGPEFormat format, int surfaceFlags)
{
    if (surfaceFlags & GPE_REQUIRE_VIDEO_MEMORY) return E_OUTOFMEMORY;    // there is no video memory
    *ppSurf = new GPESurf(width, height, format);
    if (!*ppSurf) return E_OUTOFMEMORY;
    if (!(*ppSurf)->Buffer()) { delete *ppSurf; *ppSurf = NULL; return E_OUTOFMEMORY; }
    return S_OK;
}

void ClasspadGPE::Dirty(int top, int bottom)
{
    if (top < 0) top = 0;
    if (bottom > m_nScreenHeight) bottom = m_nScreenHeight;
    if (top >= bottom) return;
    EnterCriticalSection(&m_cs);
    if (top < m_dirtyTop) m_dirtyTop = top;
    if (bottom > m_dirtyBottom) m_dirtyBottom = bottom;
    LeaveCriticalSection(&m_cs);
    SetEvent(m_hEvent);
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

SCODE ClasspadGPE::BltPrepare(GPEBltParms *pParms)
{
    pParms->pBlt = (SCODE (GPE::*)(GPEBltParms *))&ClasspadGPE::WrappedBlt;
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

// Stream rows top..height-1 of the primary surface into the panel window.
void ClasspadGPE::PushRows(int top)
{
    const BYTE *fb = (const BYTE *)m_pPrimarySurface->Buffer();
    int stride = m_pPrimarySurface->Stride(), width = m_nScreenWidth, height = m_nScreenHeight, x, y;
    if (!fb) return;
    if (m_panel == LCD_PANEL_DCS) {
        // window columns 0..width-1, rows top..height-1; RAMWR resets the cursor to its corner
        LcdCommand(LCD_DCS_CASET);
        LcdParam(0);                        LcdParam(0);
        LcdParam((UCHAR)((width - 1) >> 8)); LcdParam((UCHAR)(width - 1));
        LcdCommand(LCD_DCS_PASET);
        LcdParam((UCHAR)(top >> 8));         LcdParam((UCHAR)top);
        LcdParam((UCHAR)((height - 1) >> 8)); LcdParam((UCHAR)(height - 1));
        LcdCommand(LCD_DCS_RAMWR);
    } else {
        LcdWriteReg(LCD_REG_HSTART, (USHORT)((LCD_CG100_GRAM_COLS - 1) - (LCD_CG100_COL0 + width - 1)));
        LcdWriteReg(LCD_REG_HEND,   (USHORT)((LCD_CG100_GRAM_COLS - 1) - LCD_CG100_COL0));
        LcdWriteReg(LCD_REG_VSTART, (USHORT)top);
        LcdWriteReg(LCD_REG_VEND,   (USHORT)(height - 1));
        LcdWriteReg(LCD_REG_HADDR,  0);
        LcdWriteReg(LCD_REG_VADDR,  0);
        LcdSelect(LCD_REG_DATA);
    }
    for (y = top; y < height; y++) {
        const USHORT *row = (const USHORT *)(fb + y * stride);
        for (x = 0; x < width; x++) LcdWriteData(row[x]);
    }
    if (m_panel == LCD_PANEL_DCS && !m_displayOn) {
        LcdCommand(LCD_DCS_DISPON);
        m_displayOn = TRUE;
    }
    m_pushes++;
    if (m_pushes <= 3 || (m_pushes % 200) == 0)
        RETAILMSG(1, (TEXT("CLASSPAD DDI: push %d from row %d\r\n"), m_pushes, top));
}

// Power off / on from GWES. Called with the system mostly stopped: register
// writes only, no synchronisation. Off: blank and sleep the DCS panel (GRAM
// kept). On: wake it and let the push thread, whose wait times out every
// DISP_PUSH_MS, repaint the whole screen; DISPON follows that push. The
// R61524 path does nothing, as before.
VOID ClasspadGPE::PowerHandler(BOOL bOff)
{
    if (m_panel != LCD_PANEL_DCS) return;
    if (bOff) {
        LcdCommand(LCD_DCS_DISPOFF);
        LcdCommand(LCD_DCS_SLPIN);
        m_displayOn = FALSE;
    } else {
        LcdCommand(LCD_DCS_SLPOUT);
        m_dirtyTop = 0; m_dirtyBottom = m_nScreenHeight;
    }
}

DWORD WINAPI ClasspadGPE::PushThread(LPVOID pThis)
{
    ClasspadGPE *p = (ClasspadGPE *)pThis;
    while (p->m_run) {
        int top, bottom;
        WaitForSingleObject(p->m_hEvent, DISP_PUSH_MS);
        EnterCriticalSection(&p->m_cs);
        top = p->m_dirtyTop; bottom = p->m_dirtyBottom;
        p->m_dirtyTop = p->m_nScreenHeight; p->m_dirtyBottom = 0;
        LeaveCriticalSection(&p->m_cs);
        if (top < bottom) p->PushRows(top);
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
