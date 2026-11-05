# bsp/kernel - patches to the CE 6.0 R3 kernel sources

Platform Builder builds kernel.dll from the Shared Source tree
(`%_WINCEROOT%\PRIVATE\WINCEOS\COREOS\NK`). These patches go on top of the
last QFEs (kcalls.c dated 2013-03-29, KB2826824); `patch --dry-run -p1`
against that file applies cleanly.

- `kcalls-waitonemore.patch`: WaitOneMore's exit path unlinks an
  interrupt-event proxy that a timed-out retry left on the event
  (`notes/ce/kernel-sh-notes.md` section 10). Without it a timed
  `WaitForSingleObject` on an interrupt event fails for ever with
  ERROR_INVALID_HANDLE once its timeout expires between the wait's kernel
  calls, which the translator makes routine (the keyboard IST and the touch
  MDD used such waits; both BSP drivers now avoid them, so the patch
  protects whatever else does).

- `loader-sh3-images.patch`: the SH4 kernel's loader accepts executables
  and DLLs whose PE machine type is SH3 (0x1A2) or SH3DSP (0x1A3). Stock
  CE 6 returns ERROR_BAD_EXE_FORMAT for them on SH4 (`loader.c` ~1900,
  the `#ifdef SH3` chain has no SH4 case). SH-3 user-mode code is a subset
  of SH-4 and uses software floating point, so Pocket PC 2000 / CE 3.0 SH3
  binaries (Math Xpander and the like) run as they are; what they import
  (coredll ordinals, aygshell, commctrl, an MFC runtime) is a separate
  question. Applies to `loader.c` dated 2012-01-25 (KB2649922).

## Applying in the VM

In the build window (PBInitEnv.bat environment, `notes/ce/build-commands.txt`):

1. `cd /d %_WINCEROOT%\PRIVATE\WINCEOS\COREOS\NK\KERNEL`, keep
   `copy kcalls.c kcalls.c.orig` (and `loader.c.orig`), then apply the
   hunks: with Git for Windows, `git apply --ignore-whitespace <patch>` from
   `%_WINCEROOT%`, by hand (kcalls.c: 13 added lines at `exitfunc:`, about
   line 2374; loader.c: 6 added lines in the CPU-type chain, about line
   1908; the patches' lines are CRLF like the sources), or by overwriting
   with the patched copies the Mac side keeps in `notes/ce/` (not in git).
2. `build -c` in that directory. It produces `nkmain.lib` (NKNORMAL) and
   the profiling variant under `%_WINCEROOT%\PUBLIC\COMMON\OAK\LIB\SH4\`.
3. kernel.dll is linked from those libraries during the OS design's Sysgen:
   Build > Advanced Build Commands > Sysgen, then Make Run-Time Image (or
   `blddemo -q` style command-line equivalents). Check the timestamp of
   `kernel.dll` in the flat release directory before `makeimg`.
4. Copy NK.bin to `notes/ce/` and run it under the translator.
