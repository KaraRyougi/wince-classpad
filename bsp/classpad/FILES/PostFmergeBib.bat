@echo off
rem ===========================================================================
rem  PostFmergeBib.bat - makeimg runs this from the release directory after
rem  fmerge has written ce.bib and before romimage reads it. It drops from the
rem  image the modules and files that romexclude.txt names: one findstr regex
rem  per line, matched against the start of a ce.bib entry (no comments in
rem  that file: every line is a pattern). The unfiltered bib is kept as
rem  ce.bib.unfiltered.
rem
rem  What is dropped (2026-11-06, docs/05-ce-image-and-oal.md "Image size"):
rem   - the network stack (tcpstk, ndis, afd, dhcp, netbios, cxport, ws2k,
rem     ws2serv, ws2instl, winsock, wspm, nspm, uiproxy, serial): the Standard
rem     Shell's catalog item pulls it in, but no shell module imports it
rem     (explorer, ceshell, shdocvw, mfcce300 checked); servicesd.exe imports
rem     ws2.dll, which stays. The fx-CP400 has no network hardware.
rem   - the standard .wav files: the Control Panel item forces them in
rem     (SYSGEN_AUDIO_STDWAVEFILES) and the image has no audio driver.
rem  To keep everything: delete romexclude.txt from the release directory.
rem ===========================================================================
setlocal
set "REL=%_FLATRELEASEDIR%"
if "%REL%"=="" set "REL=%~dp0."
if not exist "%REL%\romexclude.txt" (
  echo PostFmergeBib: no romexclude.txt, ce.bib unchanged
  exit /b 0
)
findstr /v /r /g:"%REL%\romexclude.txt" "%REL%\ce.bib" > "%REL%\ce.bib.filtered"
if errorlevel 2 (
  echo PostFmergeBib: findstr failed, ce.bib unchanged
  del /q "%REL%\ce.bib.filtered" 2>nul
  exit /b 0
)
for /f %%N in ('find /c /v "" ^< "%REL%\ce.bib"') do set BEFORE=%%N
for /f %%N in ('find /c /v "" ^< "%REL%\ce.bib.filtered"') do set AFTER=%%N
copy /y "%REL%\ce.bib" "%REL%\ce.bib.unfiltered" >nul
move /y "%REL%\ce.bib.filtered" "%REL%\ce.bib" >nul
set /a DROPPED=BEFORE-AFTER
echo PostFmergeBib: %DROPPED% ce.bib entries dropped (romexclude.txt), unfiltered copy in ce.bib.unfiltered
exit /b 0
