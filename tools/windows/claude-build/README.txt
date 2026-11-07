claude-build: everything for the ClassPad image in one folder
=============================================================

Kept in the repository as tools/windows/claude-build (build.bat, this file);
tools/windows/claude-build/stage.py fills a package folder (src, orig, hhk,
EXT.bin, VERSION.txt) for copying to C:\WINCE600\claude-build.

Source: branch claude/ce-5-memory-pressure-7462c3 (commit in VERSION.txt):
Codex's integration work (tickless timer, rotation, timer recovery)
+ review fixes, the R-standby OAL (claude/device-profiler-tickless, incl. the KEYSC two-write
fix e54c409), DocList.dll, mfcce300.dll (MFC CE 3.0 only, always in the image), object store 3.1% (FSRAMPERCENT=08080808), and no
LCD DMA presenter, no hello window. Two XIP regions (below): NK and EXT with
Math Xpander.

Since 2026-11-06 also staged from claude/rstandby-diag, which adds: R-standby
suspend/resume that works on the board (Clear/ON, or SHIFT then Clear/ON), no
white flash at power-off, the image filter (FILES\PostFmergeBib.bat +
romexclude.txt, copied to the release directory in step 4: makeimg drops the
network stack and the .wav files, makeimg.log says "PostFmergeBib: N ce.bib
entries dropped") and CE RAM 0x8C000000-0x8C7FFFFF (8 MiB; the translator has
the other 8 MiB with a 4 MiB code cache). Boot only with this package's hhk\.

Folder
  build.bat   the build (arguments below)
  src\        the complete bsp\classpad tree, copied over
              C:\WINCE600\PLATFORM\CLASSPAD by build.bat, plus Math Xpander's
              files in FILES\xpander (not in the repository: stage.py copies
              them from the unpacked cab)
  orig\       the VM's PLATFORM\CLASSPAD versions of those files before this
              package (build.bat restore puts them back)
  hhk\        the matching programs for the eActivity window (NK_WINDOW=eact):
              wince-eact.hh3        the launcher + translator; flashes NK.bin
                                    and EXT.bin from \fls0\ when they differ
              nkflash-eact.hh3      the same flasher as a tool of its own

Build (in the VM)
  Open any command prompt (build.bat calls PBInitEnv.bat itself) and run
      C:\WINCE600\claude-build\build.bat
  It does, and stops at the first failure:
    1. xcopy src\ over PLATFORM\CLASSPAD (no deletions)
    2. once only (kernel.done marks it): build -c in PRIVATE\...\NK\KERNEL
       (the SH3 loader patch for mfcce300.dll is in loader.c but not yet in
       kern.dll: nkmain.lib dates from before the patch), sysgen -p common nk,
       copy kern.* to the release directory. No full Sysgen.
    3. build -c of the whole BSP (OAL, drivers, apps incl. DocList)
    4. copy FILES\config.bib, platform.bib, platform.reg, platform.dat to
       the release directory (instead of buildrel)
    5. makeimg (NK.bin and EXT.bin, one per region), then check that
       mfcce300.dll and MathXpander.exe are in the merged ce.bib
  Result: NK-claude.bin and EXT-claude.bin here, result.txt, build.log,
  makeimg.log.

  build.bat image     only FILES + makeimg (after editing bib/reg/config.bib)
  build.bat kernel    relink the kernel again
  build.bat restore   orig\ back over PLATFORM\CLASSPAD, NK-before.bin back

Plotter-Z (SRC\APPS\PLOTTERZ, README.md there) is part of step 3 and lands in
NK as \Windows\PlotterZ.exe with a Start menu link (Programs > Plotter-Z);
step 5 fails if it did not reach ce.bib (it needs commctrl and commdlg,
both in ClasspadUI).

If step 2 fails on "sysgen -p common nk", do it from the IDE instead: build -c
in PRIVATE\WINCEOS\COREOS\NK\KERNEL, then Build > Advanced Build Commands >
Sysgen (full, slow), then create kernel.done here and run build.bat again.

Two XIP regions (config.bib, docs/03 "The EXT region")
  NK   83860000, 7.625 MiB, the eActivity area: kernel, shell, drivers,
       mfcce300.dll (SH3, as an XIP module realigned to 4 KiB sections by
       stage.py, tools/pe4k.py) and DocList.dll. Casio's OS
       reformats the area after a Casio session, so wince.hhk writes NK.bin
       again then.
  EXT  80EE0000, 5 MiB, the erased hole inside Casio's OS image: Math Xpander's
       23 SH3 DLLs and MathXpander.exe as XIP modules (flag P: no CPU type
       check; realigned to 4 KiB sections by stage.py, the /FIXED EXE after
       its relocations are rebuilt), its help pages, keypad bitmaps and
       Start menu link as files. Casio's boots leave
       it alone (emulator: a cold boot neither checks the OS checksum that
       covers it nor writes it), so wince.hhk writes EXT.bin once and finds it
       identical afterwards; an OS update erases it.
  The OAL chains EXT to the kernel in OEMInit (classpad_extrom.h), so its
  modules run in place and its files appear in \Windows; without a valid
  EXT region it says so and CE boots without Math Xpander. config.bib lists
  EXT before NK (address order) with AUTOSIZE=OFF: AUTOSIZE=ON moved RAM
  into EXT ("Ram start overlaps rom binary").

Device (fx-CP400): copy NK-claude.bin as \fls0\NK.bin and EXT-claude.bin as
\fls0\EXT.bin, start hhk\wince-eact.hh3 (it flashes what differs, then
boots). Things to look at:
  - it boots to the desktop with the object store at 3.1%: open Control
    Panel, change a setting, reboot-free check that the registry writes work
  - Explorer, rotation (Rotate icon), Clear/ON suspend and resume (R-standby)
  - mfcce300.dll is SH3: it loads only with step 2's kernel. The IDE build of
    2026-11-06 (NK.bin 5,795,023 bytes) had neither mfcce300 nor that kernel
    (its kern.dll came from an nkmain.lib older than the patched loader.c).
    An IDE Sysgen alone never rebuilds nkmain.lib: run build.bat at least once.
  - the OAL's line "CLASSPAD OAL: EXT ROM region 80ee0000..., N files"
    (console builds / debug ring), then Start > Programs > Math Xpander
    (\Windows\MathXpander.exe, run in place from the hole like its DLLs).
    The Keyboard key shows its math keyboard (MathKeyPad.dll, a SIP input
    method registered in platform.reg); the strip icons act as HP Xpander's
    keys: Menu = HOME (Start menu), Main = MATH (Math Xpander), Settings =
    MENU (the program's menu bar), Esc = ESC.
