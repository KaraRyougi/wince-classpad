@echo off
rem ===========================================================================
rem  PostRomImage.bat - makeimg runs this from the release directory after
rem  romimage has written NK.bin. It builds the EXT region (config.bib: CE
rem  5.0's romimage cannot lay out two regions that are not contiguous, so
rem  EXT gets its own run): ext.bib = ext.bib.hdr (the region's MEMORY and
rem  CONFIG) + MODULES from ext.modules.bib + FILES from ext.files.bib, the
rem  entries PostFmergeBib.bat took out of ce.bib; then "romimage ext.bib"
rem  writes EXT.bin. Flash it into the eActivity area with wince.hh3,
rem  or give it to the emulator with EXTBIN=.
rem ===========================================================================
setlocal
set "REL=%_FLATRELEASEDIR%"
if "%REL%"=="" set "REL=%~dp0."
if not exist "%REL%\ext.modules.bib" (
  echo PostRomImage: no ext.modules.bib, no EXT region to build
  exit /b 0
)
if not exist "%REL%\ext.bib.hdr" (
  echo PostRomImage: ext.bib.hdr missing in the release directory, EXT.bin not built
  exit /b 0
)
(
  type "%REL%\ext.bib.hdr"
  echo MODULES
  type "%REL%\ext.modules.bib"
  echo.
  echo FILES
  if exist "%REL%\ext.files.bib" type "%REL%\ext.files.bib"
  echo.
) > "%REL%\ext.bib"
echo PostRomImage: romimage ext.bib (the EXT region, 83860000)
del /q "%REL%\EXT.bin" "%REL%\EXT.nb0" 2>nul
romimage "%REL%\ext.bib"
if errorlevel 1 (
  echo PostRomImage: romimage ext.bib failed, EXT.bin not built
  exit /b 1
)
rem romimage writes the region's EXT.bin and EXT.nb0 into the directory of
rem the FIRST module listed (NK.bin lands in the release directory because
rem nk.exe is there). Since 2026-11-09 the first EXT module is mfcce300.dll
rem in PLATFORM\CLASSPAD\FILES\legacy-mfc, so fetch them from there.
set "FIRSTMOD="
for /f "tokens=2" %%P in ('findstr /r /c:"^ *[^ ;]" "%REL%\ext.modules.bib"') do if not defined FIRSTMOD set "FIRSTMOD=%%P"
for %%F in ("%FIRSTMOD%") do set "OUTDIR=%%~dpF"
if exist "%REL%\EXT.bin" goto extdone
if not exist "%OUTDIR%EXT.bin" goto extmissing
echo PostRomImage: moving EXT.bin and EXT.nb0 from %OUTDIR%
move /y "%OUTDIR%EXT.bin" "%REL%\EXT.bin" >nul
move /y "%OUTDIR%EXT.nb0" "%REL%\EXT.nb0" >nul
goto extdone
:extmissing
echo PostRomImage: romimage ran but wrote no EXT.bin (not in %REL% nor %OUTDIR%)
exit /b 1
:extdone
for %%F in ("%REL%\EXT.bin") do echo PostRomImage: EXT.bin %%~zF bytes
exit /b 0
