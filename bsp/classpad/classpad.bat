@REM classpad.bat - BSP environment for PLATFORM\CLASSPAD (Casio SH7305 under the translator)
@ECHO OFF
set SH_PLATFORM=CLASSPAD
REM No display, keyboard, serial or network driver yet: headless kernel boot first.
set BSP_NODISPLAY=1
set BSP_NOKEYBD=1
set BSP_NOMOUSE=1
set BSP_NOSERIAL=1
set BSP_NOAUDIO=1
set BSP_NOPCCARD=1
set BSP_NOUSB=1
REM The translator is the debugger: no KITL, no kernel debugger in the image.
set IMGNOKITL=1
set IMGNODEBUGGER=1
