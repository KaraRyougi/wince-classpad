@echo off
setlocal
set REVIEW=C:\WINCE600\codex-office-review
echo BUILD_RUNNING>"%REVIEW%\result.txt"
call C:\WINCE600\OSDesigns\ClasspadUI\ClasspadUI\Wince600\CLASSPAD_SH4\PBInitEnv.bat
if not defined _FLATRELEASEDIR goto failed
cd /d C:\WINCE600\PLATFORM\CLASSPAD\SRC\APPS\OFFICETEST
call build -c >"%REVIEW%\build.log" 2>&1
if errorlevel 1 goto failed
if exist build.err for %%F in (build.err) do if %%~zF GTR 0 goto failed
cd /d "%_FLATRELEASEDIR%"
copy /y platform.bib "%REVIEW%\platform.bib.saved" >nul
if errorlevel 1 goto failed
copy /y platform.reg "%REVIEW%\platform.reg.saved" >nul
if errorlevel 1 goto failed
copy /y NK.bin "%REVIEW%\NK-normal.saved" >nul
if errorlevel 1 goto failed
copy /y "%REVIEW%\payload\*" . >nul
if errorlevel 1 goto restorefailed
echo.>>platform.bib
echo MODULES>>platform.bib
echo classpad_officetest.exe $(_FLATRELEASEDIR)\classpad_officetest.exe NK S>>platform.bib
echo FILES>>platform.bib
for %%F in (ppviewer.exe ppv_res.dll overview.ppv ppt.htp) do echo %%F $(_FLATRELEASEDIR)\%%F NK U>>platform.bib
echo.>>platform.reg
echo [HKEY_LOCAL_MACHINE\init]>>platform.reg
echo "Launch96"="classpad_officetest.exe">>platform.reg
echo "Depend96"=hex:14,00,1e,00,32,00>>platform.reg
echo [HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Power\Timeouts]>>platform.reg
for %%T in (ACUserIdle ACSystemIdle ACSuspend BattUserIdle BattSystemIdle BattSuspend) do echo "%%T"=dword:0>>platform.reg
C:\WINCE600\public\common\oak\bin\i386\makeimg.exe >"%REVIEW%\makeimg.log" 2>&1
if errorlevel 1 goto restorefailed
copy /y NK.bin "%REVIEW%\NK-office-test.bin" >nul
if errorlevel 1 goto restorefailed
call :restore
if errorlevel 1 goto failed
echo BUILD_OK>"%REVIEW%\result.txt"
echo BUILD_OK - emulator-only NK-office-test.bin; normal image restored
exit /b 0
:restore
copy /y "%REVIEW%\platform.bib.saved" platform.bib >nul
if errorlevel 1 exit /b 1
copy /y "%REVIEW%\platform.reg.saved" platform.reg >nul
if errorlevel 1 exit /b 1
copy /y "%REVIEW%\NK-normal.saved" NK.bin >nul
exit /b %errorlevel%
:restorefailed
call :restore
:failed
echo BUILD_FAILED>"%REVIEW%\result.txt"
echo BUILD_FAILED - see logs in %REVIEW%
exit /b 1
