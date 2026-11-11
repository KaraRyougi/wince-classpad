@REM classpad.bat - BSP environment for PLATFORM\CLASSPAD (Casio SH7305 under the translator), Windows CE 5.0
@ECHO OFF
set SH_PLATFORM=CLASSPAD
REM The stock drivers common.bib would pull in are not for this board; the
REM ClassPad display, keyboard and touch drivers are listed in platform.bib.
set BSP_NODISPLAY=1
set BSP_NOKEYBD=1
set BSP_NOMOUSE=1
set BSP_NOSERIAL=1
set BSP_NOAUDIO=1
set BSP_NOPCCARD=1
set BSP_NOUSB=1
set BSP_NOETHER=1
REM The translator is the debugger: no KITL, no kernel debugger in the image,
REM so common.bib picks kern.exe (SRC\OAL\KERN) as nk.exe.
REM The battery driver (SRC\DRIVERS\BATTDRVR needs the public MDD library and
REM coredll's battery API; the catalog item is SYSGEN_BATTERY, set here so the
REM OS design does not have to carry it). Sysgen again after adding this.
set SYSGEN_BATTERY=1
REM Casio's storage memory as a folder (SRC\DRIVERS\FUGUEDSK): FATFS, which
REM brings the Storage Manager (fsdmgr), MSPART, fatutil and diskcache. Sysgen
REM again after adding this.
set SYSGEN_FATFS=1
set IMGNOKITL=1
set IMGNODEBUGGER=1
set IMGNOSHAREETH=1
set KERNELNOSHAREETH=1
