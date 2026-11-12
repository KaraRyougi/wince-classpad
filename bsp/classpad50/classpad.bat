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
REM USB function: the controller driver's MDD and the client library
REM (SRC\DRIVERS\USBFN, SRC\DRIVERS\USBACM) and the mass storage function
REM (usbmsfn.dll) that hands Casio's storage memory to a PC. BSP_NOUSB above
REM only keeps the USB host drivers out. Sysgen again after adding this.
set SYSGEN_USBFN=1
set SYSGEN_USBFN_STORAGE=1
REM Networking over the USB serial port (SRC\DRIVERS\USBACM): PPP on a direct
REM serial connection. SYSGEN_MODEM brings Unimodem and AsyncMac, and through
REM winceos.bat TAPI, PPP, TCP/IP (tcpstk, with NDIS and Winsock) and
REM iphlpapi; SYSGEN_CONNMC the Network and Dial-up Connections folder
REM (connmc, rnaapp, netui); SYSGEN_NETUTILS ping, ipconfig, route, tracert,
REM netstat. FILES\romexclude.txt no longer drops the stack and
REM FILES\extmove.txt sends these modules to the EXT region (the serial
REM function client itself, classpad_usbacm.dll, stays in NK). Sysgen again
REM after adding this.
set SYSGEN_MODEM=1
set SYSGEN_CONNMC=1
set SYSGEN_NETUTILS=1
REM A command line reached from the computer: the Telnet server (telnetd,
REM under services.exe) hands out cmd.exe; winceos.bat and servers.bat add
REM SYSGEN_CMD, SYSGEN_STDIO, SYSGEN_SERVICES and SYSGEN_FSPASSWORD for it.
REM With PPP up (USB Network): telnet 192.168.55.2. No console window on the
REM device (SYSGEN_CONSOLE needs a Courier font the flash has no room for).
REM FILES\extmove.txt sends the three modules to the EXT region. Sysgen again
REM after adding this.
set SYSGEN_TELNETD=1
set IMGNOKITL=1
set IMGNODEBUGGER=1
set IMGNOSHAREETH=1
set KERNELNOSHAREETH=1
