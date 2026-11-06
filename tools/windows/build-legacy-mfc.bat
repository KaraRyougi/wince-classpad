@echo off
setlocal
rem Run from the repository in a ClasspadUI Release PB build window.
if not defined _WINCEROOT goto noenv
if not defined _FLATRELEASEDIR goto noenv
set "SRC=%~dp0..\..\bsp\classpad\FILES\legacy-mfc"
set "DEST=%_WINCEROOT%\PLATFORM\CLASSPAD\FILES"
for %%F in (mfcce300.dll olece300.dll mfcce211.dll olece211.dll legacy-mfc.bib) do if not exist "%SRC%\%%F" goto missing
if not exist "%DEST%\platform.bib" goto missing
rem The SH3 files require the project's loader patch and a rebuilt kernel.
findstr /c:"IMAGE_FILE_MACHINE_SH3DSP))" "%_WINCEROOT%\PRIVATE\WINCEOS\COREOS\NK\KERNEL\loader.c" >nul
if errorlevel 1 goto loader
if not exist "%DEST%\legacy-mfc" mkdir "%DEST%\legacy-mfc"
if errorlevel 1 exit /b 1
for %%F in (mfcce300.dll olece300.dll mfcce211.dll olece211.dll) do (
  copy /y "%SRC%\%%F" "%DEST%\legacy-mfc\%%F" >nul
  if errorlevel 1 exit /b 1
)
rem Preserve the VM's additional applications and its existing ROM layout.
findstr /c:"IF BSP_LEGACY_MFC" "%DEST%\platform.bib" >nul
if not errorlevel 1 goto configured
if exist "%DEST%\platform.bib.before-legacy-mfc" goto backup_exists
copy "%DEST%\platform.bib" "%DEST%\platform.bib.before-legacy-mfc" >nul
if errorlevel 1 exit /b 1
echo.>>"%DEST%\platform.bib"
type "%SRC%\legacy-mfc.bib" >>"%DEST%\platform.bib"
if errorlevel 1 exit /b 1
:configured
set BSP_LEGACY_MFC=1
cd /d "%_FLATRELEASEDIR%"
if errorlevel 1 exit /b 1
call buildrel
if errorlevel 1 exit /b 1
call makeimg
if errorlevel 1 exit /b 1
echo Image built. MFCCE300 still requires a compatible doclist.dll.
echo Keep BSP_LEGACY_MFC=1 in OS Design environment settings for future builds.
exit /b 0
:noenv
echo Open the ClasspadUI Release Platform Builder build window first.
exit /b 1
:missing
echo Missing runtime payload or BSP files. See legacy-mfc README.md.
exit /b 1
:loader
echo Apply loader-sh3-images.patch and rebuild the kernel before building this image.
exit /b 1
:backup_exists
echo Existing backup found without the integration marker. Inspect platform.bib before retrying.
exit /b 1
