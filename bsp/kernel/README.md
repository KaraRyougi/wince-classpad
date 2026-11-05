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
  calls, which the translator makes routine for the keyboard IST and the
  touch MDD.

## Applying in the VM

In the build window (PBInitEnv.bat environment, `notes/ce/build-commands.txt`):

1. `cd /d %_WINCEROOT%\PRIVATE\WINCEOS\COREOS\NK\KERNEL`, keep
   `copy kcalls.c kcalls.c.orig`, then apply the hunk: with Git for Windows,
   `git apply --ignore-whitespace <path to the patch>` from `%_WINCEROOT%`,
   or by hand (one hunk of 13 added lines at `exitfunc:`, about line 2374;
   the patch's lines are CRLF like the source).
2. `build -c` in that directory. It produces `nkmain.lib` (NKNORMAL) and
   the profiling variant under `%_WINCEROOT%\PUBLIC\COMMON\OAK\LIB\SH4\`.
3. kernel.dll is linked from those libraries during the OS design's Sysgen:
   Build > Advanced Build Commands > Sysgen, then Make Run-Time Image (or
   `blddemo -q` style command-line equivalents). Check the timestamp of
   `kernel.dll` in the flat release directory before `makeimg`.
4. Copy NK.bin to `notes/ce/` and run it under the translator.
