# PLOTTERZ: Plotter-Z in the image

Plotter-Z by anderain (Kuki Himekawa) is a 3D function plotter for early
handhelds. The image carries its Windows CE front end as
`\Windows\PlotterZ.exe`, with a Start menu link, Programs > Plotter-Z
(`FILES\plotterz.lnk`, `platform.dat`).

## Sources

`upstream\` holds upstream's files verbatim, in their own layout:

| Repository | Commit |
|---|---|
| github.com/anderain/Plotter-Z | 3dbf8c180e994ca34039b4c2cfb69f44e3e82be7 (2026-07-06) |
| github.com/anderain/salvia89 (`upstream\deps\salvia89`) | 19667944ee1eded67d2f846c879ead28e6f10287 |
| github.com/anderain/pine89 (`upstream\deps\pine89`) | bb03c0b2db7a4a8af2fb5f56e46ae48b28c3dde9 |

Only what the CE program needs was copied: `common`, `formula-z`,
`evaluator-z`, `renderer-z`, `plotter-z\utils` (font, samples),
`plotter-z\win32-native` (the CE/Win32 front end, its resources and icon)
and the two libraries upstream's `fetch-deps.sh` clones. None of the three
repositories states a licence.

**Local changes to `plotter-z-win32-native.c`** (each marked `ClassPad
(2026-11-09)`; a recalculation of the Saddle Surface sample took 160 s under
the translator and almost none of it was arithmetic, docs/08 section 10):

- the 16 bpp back buffer is a `BI_BITFIELDS` RGB565 DIB, the screen's
  format, so the blit to the window is a plain copy (`g_bDib565`,
  `RGB888_TO_WORD_RGB565`); it was BI_RGB, i.e. RGB555, converted pixel by
  pixel by the display driver;
- `PaintCanvas16BppRect`: the 2 bpp canvas is converted row-wise, four
  pixels per byte, and only for the rectangle being repainted; the window's
  WM_PAINT converts and copies just `ps.rcPaint` at 16 bpp;
- `DrawRecalcProgress` invalidates the progress bar's rectangle instead of
  the whole window (which repainted the command bar's text and the whole
  canvas after every column).

Second round, same day (docs/08 section 8, "Plotter-Z: drag and first
draw"): dragging redrew at about 37M emulated instructions a frame, 81% of
it the canvas clear, and painted nothing until the pen came up.

- `FillRectCanvas` and `InvertRectCanvas` clip once and work row by row,
  whole bytes (four pixels) at a time; `DrawLineCanvas` and
  `Draw1bppCanvas` write their pixels in line (`CANVAS_PUT`) instead of
  calling `SetPixelCanvas` per pixel. Same pixels as before: checked on the
  Mac against the old functions with 200000 random rectangles, lines and
  glyphs.
- `WM_MOUSEMOVE` calls `UpdateWindow` after its `InvalidateRect`, so each
  redraw of a drag is painted at once; WM_PAINT alone waits for an empty
  message queue, which a moving stylus never leaves.
- `RefreshCameraTrigBuf` takes sin/cos of the camera's whole-degree angles
  from `..\..\..\pz_trig_table.h` (ours, included by `plotterz.c`; without it
  the file compiles as upstream's). The table holds the values of
  upstream's own expressions, computed on the build host.
- `upstream\evaluator-z\eval.c`, `EZOP_POW`: a whole exponent from -8 to 8
  is evaluated by multiplication (`x^2` is the common case; `pow` in double
  precision on the emulated FPU was most of a recalculation's arithmetic).
  Exponents -1 to 2 give the same bits as `pow`, the others differ by a few
  units in the last place of a float.

Third round (2026-11-14): `X_GRID_MAX`, `Y_GRID_MAX`, `U_GRID_MAX` and
`V_GRID_MAX` are all 90 (upstream: 30, 30, 20, 20), the upper bound the
Window dialog clamps the four Grid fields to. `g_fVertexBuf` grows to
90 x 90 x 3 fixed-point values (about 95 KB), and a recalculation at the
maximum evaluates nine times (Cartesian) or about twenty times (parametric)
as many points as upstream's maximum. `LoadSession` clamps the four grids
it reads to the same range (5 to the limit; upstream took them unchecked, and
a saved value above the limit overran the buffer) and stores `ugrid` and
`vgrid` in `uGrid` and `vGrid` (upstream overwrote `xGrid` and `yGrid`
with them); choosing a sample sets `vGrid` (upstream set `uGrid` twice).

When replacing upstream's files with a newer checkout, reapply them (or
better, send them upstream).

The CE 6.0 glue is in this directory, so upstream's files can be replaced
by a newer checkout as they are:

- `plotterz.c` compiles everything as one unit (upstream's relative
  `#include "../../x.h"` paths keep working, and `sources` needs no paths
  outside this directory). It also calls `InitCommonControlsEx` before
  upstream's `WinMain`: the CE 1.0/2.x shells did that for every process,
  CE 6.0 does not, and without it the trackbar of File > Refresh threshold
  cannot be created.
- `afxres.h` stands in for MFC's header, which upstream's eVC resource
  script includes and Platform Builder does not have.
- `plotterz.rc` includes upstream's resource script; the icon is found
  through `INCLUDES`.
- `sources`: `WARNISERROR` off (third-party C89 code, `strcpy` etc.),
  linked against the OS design's coredll, commctrl and commdlg.

Fixed-point rendering (`USE_FIXED_POINT`, upstream's default for this front
end) keeps the per-vertex projection off the FPU, which the translator
emulates.

## Checked on the Mac (no Platform Builder build yet)

- `plotterz.c` compiles with clang against the VM's
  `PUBLIC\COMMON\SDK\INC` headers (SH4 defines), no errors.
- Every external it uses (88 names: GDI/USER, CommandBar_*,
  GetOpenFileNameW/GetSaveFileNameW, the C math and string functions) is
  exported by the ClasspadUI OS design's coredll.lib, commctrl.lib or
  commdlg.lib.
- `plotterz.rc` preprocesses with every symbol resolved.

## Using it on the ClassPad

Upstream's README describes the menus (File > Samples, Edit > Cartesian /
Parametric / Window Editor, View modes, Display toggles). Drag on the plot
with the stylus to rotate, pan or zoom depending on the View mode; the EXE
key (VK_RETURN) toggles full screen (hides the taskbar and the command bar).
An optional `\plotter-z.ini` (upstream's `plotter-z.example.ini` format:
`[graphics] force_bpp=`, `[expression] expr=`) is read at start; sessions
are saved where File > Save Session's dialog puts them.
