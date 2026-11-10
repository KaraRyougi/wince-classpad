@echo off
rem ===========================================================================
rem  PostFmergeBib.bat - makeimg runs this from the release directory after
rem  fmerge has written ce.bib and before romimage reads it. Two jobs:
rem
rem  1. Drop from the image the modules and files that romexclude.txt names:
rem     one findstr regex per line, matched against the start of a ce.bib
rem     entry [no comments in that file: every line is a pattern]. The
rem     unfiltered bib is kept as ce.bib.unfiltered.
rem     What is dropped [2026-11-06, docs/05-ce-image-and-oal.md "Image size"]:
rem      - the network stack [tcpstk, ndis, afd, dhcp, netbios, cxport, ws2k,
rem        ws2serv, ws2instl, winsock, wspm, nspm, uiproxy, serial]: the
rem        Standard Shell's catalog item pulls it in, but no shell module
rem        imports it; servicesd.exe imports ws2.dll, which stays.
rem      - the standard .wav files: the Control Panel item forces them in and
rem        the image has no audio driver.
rem     To keep everything: delete romexclude.txt from the release directory.
rem
rem  2. Take the EXT region's entries [platform.bib lines with EXT in the
rem     region column] out of ce.bib into ext.modules.bib [.dll/.exe] and
rem     ext.files.bib [the rest]: CE 5.0's romimage cannot build two regions
rem     that are not contiguous [config.bib], so romimage builds NK from
rem     ce.bib alone and PostRomImage.bat builds EXT from ext.bib.hdr plus
rem     these two files in a second run.
rem
rem  Written without parenthesised blocks: a ")" inside an echo text ends a
rem  cmd block, which is how the first version of this file died silently.
rem ===========================================================================
setlocal
set "REL=%_FLATRELEASEDIR%"
if "%REL%"=="" set "REL=%~dp0."

rem ------------------------------------------------------------- 1. exclusions
if not exist "%REL%\romexclude.txt" goto noexclude
findstr /v /r /g:"%REL%\romexclude.txt" "%REL%\ce.bib" > "%REL%\ce.bib.filtered"
if errorlevel 2 goto excludefailed
for /f %%N in ('find /c /v "" ^< "%REL%\ce.bib"') do set BEFORE=%%N
for /f %%N in ('find /c /v "" ^< "%REL%\ce.bib.filtered"') do set AFTER=%%N
copy /y "%REL%\ce.bib" "%REL%\ce.bib.unfiltered" >nul
move /y "%REL%\ce.bib.filtered" "%REL%\ce.bib" >nul
set /a DROPPED=BEFORE-AFTER
echo PostFmergeBib: %DROPPED% ce.bib entries dropped by romexclude.txt, unfiltered copy in ce.bib.unfiltered
goto ext
:excludefailed
echo PostFmergeBib: findstr failed, ce.bib unchanged
del /q "%REL%\ce.bib.filtered" 2>nul
goto ext
:noexclude
echo PostFmergeBib: no romexclude.txt, nothing excluded

rem --------------------------------------------- 1b. NK entries sent to EXT
rem extmove.txt: one findstr regex per line, like romexclude.txt, naming
rem ce.bib entries of Microsoft's bib files [region NK] that belong in the
rem EXT region: WordPad and the card games since 2026-11-09, so that NK
rem keeps room in the 5 MiB hole. Each matching entry is rewritten as
rem "name path EXT flags" [four blank-separated columns: no path here has a
rem space] and step 2 then takes it out with the platform.bib EXT entries.
:ext
if not exist "%REL%\extmove.txt" goto ext2
findstr /i /r /g:"%REL%\extmove.txt" "%REL%\ce.bib" > "%REL%\ext.moved.bib"
if errorlevel 2 goto ext2
findstr /v /i /r /g:"%REL%\extmove.txt" "%REL%\ce.bib" > "%REL%\ce.bib.kept"
if errorlevel 2 goto ext2
for /f "usebackq tokens=1,2,3,*" %%a in ("%REL%\ext.moved.bib") do echo     %%a  %%b  EXT  %%d>> "%REL%\ce.bib.kept"
for /f %%N in ('find /c /v "" ^< "%REL%\ext.moved.bib"') do set NMOVED=%%N
move /y "%REL%\ce.bib.kept" "%REL%\ce.bib" >nul
echo PostFmergeBib: %NMOVED% NK entries of extmove.txt sent to the EXT region

rem ------------------------------------------------------- 2. the EXT region
rem An entry is "name path REGION flags"; the region column EXT, then a space.
:ext2
set "EXTLINE=^ *[^ ;][^ ]* *[^ ]* *EXT "
del /q "%REL%\ext.modules.bib" "%REL%\ext.files.bib" 2>nul
findstr /r /c:"%EXTLINE%" "%REL%\ce.bib" > "%REL%\ext.all.bib"
if errorlevel 2 goto extfailed
for /f %%N in ('find /c /v "" ^< "%REL%\ext.all.bib"') do set NEXT=%%N
if "%NEXT%"=="0" goto noext
findstr /i /r /c:"^ *[^ ;]*\.dll " /c:"^ *[^ ;]*\.exe " "%REL%\ext.all.bib" > "%REL%\ext.modules.bib"
findstr /v /i /r /c:"^ *[^ ;]*\.dll " /c:"^ *[^ ;]*\.exe " "%REL%\ext.all.bib" > "%REL%\ext.files.bib"
findstr /v /r /c:"%EXTLINE%" "%REL%\ce.bib" > "%REL%\ce.bib.nkonly"
if errorlevel 2 goto extfailed
move /y "%REL%\ce.bib.nkonly" "%REL%\ce.bib" >nul
echo PostFmergeBib: %NEXT% EXT entries moved out of ce.bib into ext.modules.bib and ext.files.bib; PostRomImage.bat builds EXT.bin
exit /b 0
:noext
echo PostFmergeBib: no EXT entries in ce.bib
del /q "%REL%\ext.all.bib" 2>nul
exit /b 0
:extfailed
echo PostFmergeBib: findstr failed, EXT entries left in ce.bib
del /q "%REL%\ext.all.bib" "%REL%\ce.bib.nkonly" 2>nul
exit /b 0
