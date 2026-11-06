claude-build: everything for the ClassPad image in one folder
=============================================================

Kept in the repository as tools/windows/claude-build (build.bat, this file);
tools/windows/claude-build/stage.py fills a package folder (src, orig, hhk,
EXT.bin, VERSION.txt) for copying to C:\WINCE600\claude-build.

Source: branch claude/ce-5-memory-pressure-7462c3 (commit in VERSION.txt):
Codex's integration work (tickless timer, rotation, timer recovery)
+ review fixes, the R-standby OAL (claude/device-profiler-tickless, incl. the KEYSC two-write
fix e54c409), DocList.dll, mfcce300.dll (MFC CE 3.0 only, always in the image), object store 3.1% (FSRAMPERCENT=08080808), and no
LCD DMA presenter. Plus the EXT region (below): Math Xpander in the NOR hole.

Folder
  build.bat   the build (arguments below)
  src\        the complete bsp\classpad tree, copied over
              C:\WINCE600\PLATFORM\CLASSPAD by build.bat. Only difference from
              the repository: FILES\config.bib uses the eActivity window
              (ROMSTART 83860000, ROMSIZE 007A0000 = 7.625 MiB), like the VM's.
  orig\       the VM's PLATFORM\CLASSPAD versions of those files before this
              package (build.bat restore puts them back)
  hhk\        the matching programs for the eActivity window (NK_WINDOW=eact):
              wince-eact.hh3        the launcher + translator; flashes NK.bin
                                    and EXT.bin from \fls0\ when they differ
              nkflash-eact.hh3      the same flasher as a tool of its own
              EXT.bin               the EXT region (Math Xpander), built on the Mac

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
    5. makeimg, then check that mfcce300.dll is in the merged ce.bib
  Result: NK-claude.bin here, result.txt, build.log, makeimg.log.

  build.bat image     only FILES + makeimg (after editing bib/reg/config.bib)
  build.bat kernel    relink the kernel again
  build.bat restore   orig\ back over PLATFORM\CLASSPAD, NK-before.bin back

If step 2 fails on "sysgen -p common nk", do it from the IDE instead: build -c
in PRIVATE\WINCEOS\COREOS\NK\KERNEL, then Build > Advanced Build Commands >
Sysgen (full, slow), then create kernel.done here and run build.bat again.

The EXT region (docs/03)
  The 5 MiB erased hole inside Casio's OS image (chip 0x0EE0000-0x13E0000,
  P1 80EE0000) holds a second, files-only ROM region that the OAL chains to
  the kernel (OEMInit, classpad_extrom.h): its files appear in \Windows like
  the NK image's. It is not made by makeimg: on the Mac,
      make -C dbt NK_WINDOW=eact XPANDER=~/Downloads/Xpander_SH3/files extrom
  builds dbt/build/EXT.bin from bsp/classpad/EXT/xpander.txt (Math Xpander:
  24 SH3 binaries, help, keypad bitmaps, the Start menu link; 3.8 of 5 MiB).
  Casio's boots leave the hole alone (emulator: a cold boot neither checks
  the OS checksum that covers it nor writes it), so wince.hhk writes EXT.bin
  once and later finds it identical; an OS update erases it. With no EXT
  region the OAL says so and CE boots without those files.

Device (fx-CP400): copy NK-claude.bin as \fls0\NK.bin and hhk\EXT.bin as
\fls0\EXT.bin, start hhk\wince-eact.hh3 (it flashes what differs, then
boots). Things to look at:
  - it boots to the desktop with the object store at 3.1%: open Control
    Panel, change a setting, reboot-free check that the registry writes work
  - Explorer, rotation (Rotate icon), Clear/ON suspend and resume (R-standby)
  - mfcce300.dll is SH3: it loads only with step 2's kernel. The IDE build of
    2026-11-06 (NK.bin 5,795,023 bytes) had neither mfcce300 nor that kernel
    (its kern.dll came from an nkmain.lib older than the patched loader.c).
    An IDE Sysgen alone never rebuilds nkmain.lib: run build.bat at least once.
  - the OAL's line "CLASSPAD OAL: EXT ROM region 80ee0000..812..., 38 files"
    (console builds / debug ring), then Start > Programs > Math Xpander
    (\Windows\MathXpander.exe: SH3, needs mfcce300.dll and the kernel above).
    Its keypad input method (MathKeyPad.dll) is in the region but not
    registered (the cab ran regsvrce.exe; COM self-registration is not done).
