/*
 * plotterz.c - Plotter-Z (github.com/anderain/Plotter-Z, win32-native front
 * end) for the ClassPad CE 6.0 image.
 *
 * One translation unit: the upstream sources stay verbatim in upstream\ in
 * their own layout (their #include "../../x.h" paths are relative to each
 * file) and are compiled from here, so the BSP build needs no SOURCES entries
 * outside this directory. The only CE 6.0 addition is the common-controls
 * registration upstream leaves out (the Windows CE 1.0/2.x shells did it for
 * every process): without it the trackbar in File > Refresh threshold cannot
 * be created and that dialog never opens.
 */
#include <windows.h>
#include <commctrl.h>

/* the portable core: parser, evaluator, renderer and their two libraries */
#include "upstream/common/vlist.c"
#include "upstream/common/utils.c"
#include "upstream/common/constants.c"
#include "upstream/deps/salvia89/salvia.c"
#include "upstream/deps/pine89/pine-ini.c"
#include "upstream/formula-z/lexer.c"
#include "upstream/formula-z/parser.c"
#include "upstream/evaluator-z/emitter.c"
#include "upstream/evaluator-z/eval.c"
#include "upstream/evaluator-z/interval.c"
#include "upstream/renderer-z/transform.c"
#include "upstream/renderer-z/render.c"

/* the Windows CE front end, its WinMain renamed so ours runs first */
#define WinMain PlotterZ_WinMain
#include "upstream/plotter-z/win32-native/plotter-z-win32-native.c"
#undef WinMain

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPWSTR lpCmdLine, int nCmdShow)
{
    INITCOMMONCONTROLSEX icc;

    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_BAR_CLASSES;   /* command bar, trackbar */
    InitCommonControlsEx(&icc);
    return PlotterZ_WinMain(hInstance, hPrevInstance, lpCmdLine, nCmdShow);
}
