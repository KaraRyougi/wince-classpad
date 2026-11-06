@echo off
setlocal
set "REVIEW=C:\WINCE600\codex-boot-review"
if not exist "%REVIEW%" mkdir "%REVIEW%"
echo BUILD_RUNNING>"%REVIEW%\result.txt"
set "STAGE=initialize environment"
echo [1/6] Initializing CE environment...
call C:\WINCE600\OSDesigns\ClasspadUI\ClasspadUI\Wince600\CLASSPAD_SH4\PBInitEnv.bat
rem PBInitEnv does not define a reliable exit code; validate its outputs.
if not defined _FLATRELEASEDIR goto failed
if not exist "C:\WINCE600\public\common\oak\bin\i386\makeimg.exe" goto failed
echo Release directory: %_FLATRELEASEDIR%
set "STAGE=validate release files"
cmd /c exit 0
cd /d "%_FLATRELEASEDIR%"
if errorlevel 1 goto failed
if not exist platform.reg goto failed
if not exist NK.bin goto failed
set "STAGE=backup original files"
echo [2/6] Backing up original registry and NK.bin...
copy /y platform.reg "%REVIEW%\platform.reg.saved" >nul
if errorlevel 1 goto failed
copy /y NK.bin "%REVIEW%\NK-original.bin" >nul
if errorlevel 1 goto failed
rem Both diagnostic images disable automatic power timeouts. No test app.
echo.>>platform.reg
echo [HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Power\Timeouts]>>platform.reg
for %%T in (ACUserIdle ACSystemIdle ACSuspend BattUserIdle BattSystemIdle BattSuspend) do echo "%%T"=dword:0>>platform.reg
set "STAGE=build awake image"
echo [3/6] Building NK-awake.bin; log: %REVIEW%\awake.log
cmd /c exit 0
call C:\WINCE600\public\common\oak\bin\i386\makeimg.exe >"%REVIEW%\awake.log" 2>&1
if errorlevel 1 goto restorefailed
copy /y NK.bin "%REVIEW%\NK-awake.bin" >nul
if errorlevel 1 goto restorefailed
rem Isolate touch initialization independently from display/power policy.
echo.>>platform.reg
echo [HKEY_LOCAL_MACHINE\HARDWARE\DEVICEMAP\TOUCH]>>platform.reg
echo "Panel"=dword:0>>platform.reg
set "STAGE=build awake no-touch image"
echo [4/6] Building NK-awake-notouch.bin; log: %REVIEW%\notouch.log
cmd /c exit 0
call C:\WINCE600\public\common\oak\bin\i386\makeimg.exe >"%REVIEW%\notouch.log" 2>&1
if errorlevel 1 goto restorefailed
copy /y NK.bin "%REVIEW%\NK-awake-notouch.bin" >nul
if errorlevel 1 goto restorefailed
copy /y "%REVIEW%\platform.reg.saved" platform.reg >nul
if errorlevel 1 goto failed
rem Restore normal release artifacts as well as registry source.
set "STAGE=restore normal release image"
echo [5/6] Restoring normal release image; log: %REVIEW%\restore.log
cmd /c exit 0
call C:\WINCE600\public\common\oak\bin\i386\makeimg.exe >"%REVIEW%\restore.log" 2>&1
if errorlevel 1 goto failed
echo [6/6] BUILD_OK - diagnostic images are in %REVIEW%
echo BUILD_OK>"%REVIEW%\result.txt"
exit /b 0
:restorefailed
copy /y "%REVIEW%\platform.reg.saved" platform.reg >nul
copy /y "%REVIEW%\NK-original.bin" NK.bin >nul
:failed
echo BUILD_FAILED at: %STAGE%
echo %STAGE%>"%REVIEW%\failed-stage.txt"
echo BUILD_FAILED>"%REVIEW%\result.txt"
exit /b 1
