@echo off
setlocal
set REVIEW=C:\WINCE600\codex-tickless-review
if not exist "%REVIEW%" mkdir "%REVIEW%"
echo BUILD_RUNNING>"%REVIEW%\result.txt"
call C:\WINCE600\OSDesigns\ClasspadUI\ClasspadUI\Wince600\CLASSPAD_SH4\PBInitEnv.bat
if not defined _FLATRELEASEDIR goto failed
if not exist "%_FLATRELEASEDIR%\platform.bib" goto failed
if /i "%~1"=="image" goto image
for %%D in (OAL\OALLIB OAL\OALEXE APPS\DBTSTAT) do (
 echo Building %%D...
 cd /d C:\WINCE600\PLATFORM\CLASSPAD\SRC\%%D
 call build -c >>"%REVIEW%\build.log" 2>&1
 if errorlevel 1 goto failed
 if exist build.err for %%F in (build.err) do if %%~zF GTR 0 goto failed
)
:image
if not exist "%_FLATRELEASEDIR%\classpad_dbtstat.exe" goto failed
cd /d "%_FLATRELEASEDIR%"
if not exist "%REVIEW%\NK-before.bin" copy /y NK.bin "%REVIEW%\NK-before.bin"
if not exist "%REVIEW%\platform.bib.before" copy /y platform.bib "%REVIEW%\platform.bib.before"
findstr /i /c:"classpad_dbtstat.exe" platform.bib >nul
if not errorlevel 1 goto bibready
echo.>>platform.bib
echo MODULES>>platform.bib
echo classpad_dbtstat.exe $(_FLATRELEASEDIR)\classpad_dbtstat.exe NK S>>platform.bib
:bibready
echo Making tickless image...
C:\WINCE600\public\common\oak\bin\i386\makeimg.exe >>"%REVIEW%\makeimg.log" 2>&1
if errorlevel 1 goto failed
copy /y NK.bin "%REVIEW%\NK-tickless.bin"
if errorlevel 1 goto failed
echo BUILD_OK>"%REVIEW%\result.txt"
echo BUILD_OK: %REVIEW%\NK-tickless.bin
exit /b 0
:failed
echo BUILD_FAILED>"%REVIEW%\result.txt"
echo BUILD_FAILED: see %REVIEW%\build.log and makeimg.log
exit /b 1
