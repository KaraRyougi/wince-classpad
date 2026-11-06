@echo off
setlocal
set REVIEW=C:\WINCE600\codex-display-speed-review
if not exist "%REVIEW%" mkdir "%REVIEW%"
echo BUILD_RUNNING>"%REVIEW%\result.txt"
call C:\WINCE600\OSDesigns\ClasspadUI\ClasspadUI\Wince600\CLASSPAD_SH4\PBInitEnv.bat
if not defined _FLATRELEASEDIR goto failed
if not exist "%_FLATRELEASEDIR%\platform.bib" goto failed
if /i "%~1"=="image" goto image
set BUILDDIRS=DRIVERS\DISPLAY
for %%D in (%BUILDDIRS%) do (
 echo Building %%D...
 cd /d C:\WINCE600\PLATFORM\CLASSPAD\SRC\%%D
 call build -c >>"%REVIEW%\build.log" 2>&1
 if errorlevel 1 goto failed
 if exist build.err for %%F in (build.err) do if %%~zF GTR 0 goto failed
)
:image
if not exist "%_FLATRELEASEDIR%\classpad_rotate.exe" goto failed
if not exist "%_FLATRELEASEDIR%\classpad_rotatekey.exe" goto failed
cd /d "%_FLATRELEASEDIR%"
if not exist "%REVIEW%\NK-before.bin" copy /y NK.bin "%REVIEW%\NK-before.bin"
if not exist "%REVIEW%\platform.bib.before" copy /y platform.bib "%REVIEW%\platform.bib.before"
findstr /i /c:"classpad_rotate.exe" platform.bib >nul
if not errorlevel 1 goto bibready
echo.>>platform.bib
echo MODULES>>platform.bib
echo classpad_rotate.exe $(_FLATRELEASEDIR)\classpad_rotate.exe NK S>>platform.bib
:bibready
findstr /i /c:"classpad_rotatekey.exe" platform.bib >nul
if not errorlevel 1 goto keybibready
echo.>>platform.bib
echo MODULES>>platform.bib
echo classpad_rotatekey.exe $(_FLATRELEASEDIR)\classpad_rotatekey.exe NK S>>platform.bib
:keybibready
findstr /i /c:"classpad_rotatekey.exe" platform.reg >nul
if not errorlevel 1 goto keyregready
echo.>>platform.reg
echo [HKEY_LOCAL_MACHINE\init]>>platform.reg
echo "Launch94"="classpad_rotatekey.exe">>platform.reg
echo "Depend94"=hex:14,00,1e,00,32,00>>platform.reg
:keyregready
echo Making normal display batching image...
C:\WINCE600\public\common\oak\bin\i386\makeimg.exe >>"%REVIEW%\makeimg.log" 2>&1
if errorlevel 1 goto failed
copy /y NK.bin "%REVIEW%\NK-display-speed.bin"
if errorlevel 1 goto failed
copy /y platform.reg "%REVIEW%\platform.reg.saved"
if errorlevel 1 goto failed
echo.>>platform.reg
echo [HKEY_LOCAL_MACHINE\init]>>platform.reg
echo "Launch95"="classpad_rotate.exe">>platform.reg
echo "Depend95"=hex:14,00,1e,00,32,00>>platform.reg
echo [HKEY_LOCAL_MACHINE\Software\ClassPad\RotationTest]>>platform.reg
echo "Enabled"=dword:1>>platform.reg
echo [HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Power\Timeouts]>>platform.reg
for %%T in (ACUserIdle ACSystemIdle ACSuspend BattUserIdle BattSystemIdle BattSuspend) do echo "%%T"=dword:0>>platform.reg
echo Making emulator-only rotation regression image...
C:\WINCE600\public\common\oak\bin\i386\makeimg.exe >>"%REVIEW%\makeimg-test.log" 2>&1
if errorlevel 1 goto restorefailed
copy /y NK.bin "%REVIEW%\NK-display-speed-test.bin"
if errorlevel 1 goto restorefailed
copy /y "%REVIEW%\platform.reg.saved" platform.reg
if errorlevel 1 goto failed
copy /y "%REVIEW%\NK-display-speed.bin" NK.bin
if errorlevel 1 goto failed
echo BUILD_OK>"%REVIEW%\result.txt"
echo BUILD_OK: NK-display-speed.bin, emulator NK-display-speed-test.bin
exit /b 0
:restorefailed
copy /y "%REVIEW%\platform.reg.saved" platform.reg
copy /y "%REVIEW%\NK-display-speed.bin" NK.bin
:failed
echo BUILD_FAILED>"%REVIEW%\result.txt"
echo BUILD_FAILED: see %REVIEW% logs
exit /b 1
