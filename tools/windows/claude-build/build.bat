@echo off
rem ===========================================================================
rem  claude-build\build.bat: the ClassPad (ClasspadUI Release) image in one run
rem
rem    build.bat            sync src\ into PLATFORM\CLASSPAD, relink the kernel
rem                         once (if kernel.done is missing), build the BSP,
rem                         copy FILES to the release directory, makeimg
rem    build.bat image      only sync src\FILES and makeimg (bib/reg/config edits)
rem    build.bat kernel     also relink the kernel even if kernel.done exists
rem    build.bat restore    put orig\ back over PLATFORM\CLASSPAD and the
rem                         previous NK.bin/EXT.bin back into the release directory
rem
rem  Arguments combine (build.bat image kernel). Output: NK-claude.bin and
rem  EXT-claude.bin here (config.bib's two XIP regions: NK in the eActivity
rem  area, EXT in the hole of Casio's OS image), result.txt = BUILD_OK /
rem  BUILD_FAILED, logs here. Image mode refuses to run until kernel.done
rem  exists (the kernel step relinks kern.dll with the SH3 loader patch).
rem  No Sysgen of the OS design is needed: the catalog is unchanged.
rem ===========================================================================
setlocal EnableExtensions
set "PKG=%~dp0"
set "PKG=%PKG:~0,-1%"
set "ROOT=C:\WINCE600"
set "PLAT=%ROOT%\PLATFORM\CLASSPAD"
set "LOG=%PKG%\build.log"
set "MAKEIMG=%ROOT%\public\common\oak\bin\i386\makeimg.exe"
set MODE=all
set FORCEKERNEL=0
:args
if "%~1"=="" goto argsdone
if /i "%~1"=="image"   set MODE=image
if /i "%~1"=="kernel"  set FORCEKERNEL=1
if /i "%~1"=="restore" set MODE=restore
shift
goto args
:argsdone

echo BUILD_RUNNING>"%PKG%\result.txt"
echo claude-build %DATE% %TIME% mode=%MODE% kernel=%FORCEKERNEL%>"%LOG%"
call "%ROOT%\OSDesigns\ClasspadUI\ClasspadUI\Wince600\CLASSPAD_SH4\PBInitEnv.bat" >>"%LOG%" 2>&1
if not defined _FLATRELEASEDIR goto noenv
if not exist "%_FLATRELEASEDIR%\ce.bib" goto noenv
if /i "%MODE%"=="restore" goto restore

rem ---------------------------------------------------------------- 1. sync
if /i "%MODE%"=="image" (
  echo [1/5] Sync src\FILES
  xcopy "%PKG%\src\FILES" "%PLAT%\FILES" /e /i /y /q >>"%LOG%" 2>&1
) else (
  echo [1/5] Sync src into PLATFORM\CLASSPAD
  xcopy "%PKG%\src" "%PLAT%" /e /i /y /q >>"%LOG%" 2>&1
)
if errorlevel 2 goto failed
if /i "%MODE%"=="image" if not exist "%PKG%\kernel.done" goto nokernel
if /i "%MODE%"=="image" goto files

rem -------------------------------------------------------------- 2. kernel
rem The SH3 loader patch (MFC CE 3.0 is SH3) and the WaitOneMore patch live
rem in PRIVATE\...\NK\KERNEL; kern.dll only changes when nkmain.lib is
rem rebuilt and the nk sysgen target relinks it. kernel.done skips this later.
if "%FORCEKERNEL%"=="0" if exist "%PKG%\kernel.done" goto bsp
echo [2/5] Kernel: nkmain.lib, then sysgen -p common nk (kern.dll)
findstr /c:"IMAGE_FILE_MACHINE_SH3DSP))" "%ROOT%\PRIVATE\WINCEOS\COREOS\NK\KERNEL\loader.c" >nul
if errorlevel 1 goto nopatch
cd /d "%ROOT%\PRIVATE\WINCEOS\COREOS\NK\KERNEL"
call build -c >>"%LOG%" 2>&1
if errorlevel 1 goto failed
call :builderr || goto failed
rem sysgen.bat leaves a stale errorlevel outside batch mode: judge it by the
rem kern.dll it links instead (deleted first, so it must be a fresh one).
set KERNDLL=%_PROJECTROOT%\cesysgen\oak\target\%_TGTCPU%\%WINCEDEBUG%\kern.dll
if exist "%KERNDLL%" del /f "%KERNDLL%"
call sysgen -p common nk >>"%LOG%" 2>&1
if not exist "%KERNDLL%" (echo sysgen did not link kern.dll>>"%LOG%" & goto failed)
copy /y "%_PROJECTROOT%\cesysgen\oak\target\%_TGTCPU%\%WINCEDEBUG%\kern.*" "%_FLATRELEASEDIR%\" >>"%LOG%" 2>&1
if errorlevel 1 goto failed
echo kern.dll relinked %DATE% %TIME%>"%PKG%\kernel.done"

rem ----------------------------------------------------------------- 3. BSP
:bsp
echo [3/5] BSP: OAL, drivers, apps (build -c, outputs go to the release directory)
cd /d "%PLAT%"
call build -c >>"%LOG%" 2>&1
if errorlevel 1 goto failed
call :builderr || goto failed

rem --------------------------------------------------------------- 4. FILES
rem Direct copy instead of buildrel: faster, and buildrel has been seen to
rem leave an old platform.reg in the release directory.
:files
echo [4/5] FILES to the release directory
rem PostFmergeBib.bat + romexclude.txt: makeimg runs the .bat after fmerge and
rem it drops the network stack and the .wav files from ce.bib (docs/05 6).
for %%F in (config.bib platform.bib platform.reg platform.dat PostFmergeBib.bat romexclude.txt) do (
  copy /y "%PLAT%\FILES\%%F" "%_FLATRELEASEDIR%\%%F" >>"%LOG%" 2>&1
  if errorlevel 1 goto failed
)
for %%F in (oal.exe classpad_ddi.dll classpad_kbd.dll classpad_touch.dll classpad_rotate.exe classpad_rotatekey.exe DocList.dll) do (
  if not exist "%_FLATRELEASEDIR%\%%F" (echo missing %%F>>"%LOG%" & goto failed)
)

rem -------------------------------------------------------------- 5. image
echo [5/5] makeimg
cd /d "%_FLATRELEASEDIR%"
if not exist "%PKG%\NK-before.bin" if exist NK.bin copy /y NK.bin "%PKG%\NK-before.bin" >nul
if not exist "%PKG%\EXT-before.bin" if exist EXT.bin copy /y EXT.bin "%PKG%\EXT-before.bin" >nul
set RESULT=BUILD_OK
call :makeimg
if errorlevel 1 goto failed
rem mfcce300.dll (NK) and MathXpander.exe (EXT) must have reached the merged
rem bib; makeimg writes EXT.bin for config.bib's second RAMIMAGE region.
findstr /i /c:"mfcce300.dll" ce.bib >nul
if errorlevel 1 (echo mfcce300.dll is not in ce.bib>>"%LOG%" & goto failed)
findstr /i /c:"MathXpander.exe" ce.bib >nul
if errorlevel 1 (echo MathXpander.exe is not in ce.bib>>"%LOG%" & goto failed)
if not exist EXT.bin (echo makeimg wrote no EXT.bin: is EXT in config.bib's MEMORY>>"%LOG%" & goto failed)
:imaged
copy /y NK.bin "%PKG%\NK-claude.bin" >nul
if errorlevel 1 goto failed
copy /y EXT.bin "%PKG%\EXT-claude.bin" >nul
if errorlevel 1 goto failed
for %%F in (NK.bin) do echo NK.bin %%~zF bytes (window 7995392)
for %%F in (EXT.bin) do echo EXT.bin %%~zF bytes (window 5242880)
echo %RESULT%>"%PKG%\result.txt"
echo %RESULT%: %PKG%\NK-claude.bin and %PKG%\EXT-claude.bin
exit /b 0

rem ------------------------------------------------------------ helpers
:makeimg
if exist NK.bin del /f /q NK.bin
if exist EXT.bin del /f /q EXT.bin
"%MAKEIMG%" >"%PKG%\makeimg.log" 2>&1
if errorlevel 1 exit /b 1
if not exist NK.bin exit /b 1
findstr /b /c:"Error:" "%PKG%\makeimg.log" >nul
if not errorlevel 1 exit /b 1
exit /b 0

:builderr
if exist build.err for %%F in (build.err) do if %%~zF GTR 0 (type build.err>>"%LOG%" & exit /b 1)
exit /b 0

:restore
echo Restoring orig\ over PLATFORM\CLASSPAD
xcopy "%PKG%\orig" "%PLAT%" /e /i /y /q >>"%LOG%" 2>&1
if errorlevel 2 goto failed
if exist "%PKG%\NK-before.bin" copy /y "%PKG%\NK-before.bin" "%_FLATRELEASEDIR%\NK.bin" >nul
if exist "%PKG%\EXT-before.bin" copy /y "%PKG%\EXT-before.bin" "%_FLATRELEASEDIR%\EXT.bin" >nul
echo RESTORED>"%PKG%\result.txt"
echo RESTORED (files added by src\, such as SRC\APPS\DOCLIST, stay; rebuild to use the old sources)
exit /b 0

:nokernel
echo build.bat image needs the kernel step first (no kernel.done): run build.bat>>"%LOG%"
echo build.bat image needs the kernel step first (no kernel.done): run build.bat
goto failed
:nopatch
echo The SH3 loader patch is not in loader.c: see bsp\kernel\README.md>>"%LOG%"
echo The SH3 loader patch is not in loader.c: see bsp\kernel\README.md
goto failed
:noenv
echo PBInitEnv.bat did not set up the ClasspadUI Release environment>>"%LOG%"
echo PBInitEnv.bat did not set up the ClasspadUI Release environment
:failed
echo BUILD_FAILED>"%PKG%\result.txt"
echo BUILD_FAILED: see %LOG% (and build.err / makeimg.log)
exit /b 1
