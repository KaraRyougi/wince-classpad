# After installing Platform Builder 6.0: step by step

Written 2026-11-02 for the first session in the Windows VM. Each step says
what to run, what to look at, and what to bring back to this repository
(files go under `notes/ce/`, which is git-ignored except for the summaries
you write yourself: the shared-source kernel files are for our own use and
must not be committed).

Prerequisites assumed done: VS2005 + SP1, PB 6.0 with **SH4** and
**Shared Source** selected, SP1, R2, R3, QFEs; a VM snapshot taken after the
last installer; a shared folder between the VM and this Mac.

## Step 1. Check what the install gave you (10 min)

Open a plain command prompt in the VM and run:

```
dir %_WINCEROOT%
dir %_WINCEROOT%\PLATFORM
dir %_WINCEROOT%\PLATFORM\COMMON\SRC
dir %_WINCEROOT%\PRIVATE\WINCEOS\COREOS\NK\KERNEL
dir %_WINCEROOT%\PRIVATE\WINCEOS\COREOS\NK\KERNEL\SH
dir %_WINCEROOT%\SDK\BIN\I386\SH
dir %_WINCEROOT%\PUBLIC\COMMON\OAK\LIB\SH4
```

`_WINCEROOT` is normally `C:\WINCE600`. What matters:

- `SDK\BIN\I386\SH\shcl.exe` and `shlink.exe` exist: the SH4 compiler.
- `PUBLIC\COMMON\OAK\LIB\SH4\...` exists: prebuilt SH4 OS libraries.
- `PRIVATE\WINCEOS\COREOS\NK\KERNEL\SH` exists: the kernel's SH4 sources
  (shared source). If this directory is missing, the Shared Source option
  was not installed; rerun the PB installer and add it before anything else.
- `PLATFORM` lists the sample BSPs. Note which of them is SH4 (look for a
  `\SRC\OAL` folder and a `*.bat` that sets `_TGTCPU=SH4`). If none is, say
  so: the OAL then starts from `PLATFORM\COMMON\SRC\SH4` and the common OAL
  libraries instead of a cloned board.

Bring back: the output of the `dir` commands (save as `notes/ce/install-dirs.txt`).

## Step 2. Build a stock image once (1-2 hours, mostly machine time)

This proves the toolchain before any of our code is involved.

1. Start **Visual Studio 2005**. File > New > Project > Platform Builder for
   CE 6.0 > OS Design. Name it `stock_sh4`.
2. Wizard: BSP = the SH4 BSP found in step 1 (or, if there is none, any
   SH4 choice the wizard offers). Design template: **Custom Device**, then
   remove everything it preselects except the kernel. Finish.
3. Build > Advanced Build Commands > **Sysgen**. Wait (30-60 min the first
   time). Then Build > Build Solution.
4. The image is `NK.bin` in the flat release directory: Build > Open
   Release Directory in Build Window, then `dir NK.bin`. Also note
   `ce.bib`, `config.bib` and `.map` files there.

Bring back: `NK.bin` (copy to the shared folder, then to
`notes/ce/stock-NK.bin`), plus `config.bib` and `ce.bib` from the release
directory. Then, on the Mac:

```bash
python3 tools/nkbin.py notes/ce/stock-NK.bin
```

Expect: CPU 0x01A6 (SH4), a ROM range in the 0x8Cxxxxxx P1 area, a module
list starting with `nk.exe`. If the CPU type is not 0x01A6, the SH4
selection did not take; stop and fix the design settings (`_TGTCPU=SH4`).

If `python3 tools/nkbin.py` is happy, run the image under the translator
once, expecting it to stop early:

```bash
make -C dbt NKBIN=notes/ce/stock-NK.bin run
```

Save the output as `notes/ce/stock-first-run.txt`. Whatever it says
("record outside guest RAM", a halt inside the kernel, a fault) is the
first real data point.

## Step 3. Read the kernel's SH4 sources (1 hour, the important step)

In `%_WINCEROOT%\PRIVATE\WINCEOS\COREOS\NK\KERNEL\SH\` open every `.src`
and `.c` file; the names vary between QFE levels, but you are looking for:

| What | Likely files | What to note |
|---|---|---|
| Exception vectors and entry | `exvector.src`, `excall.src` / `except.src` | At the general-exception and TLB-miss entries: which registers are saved first and how (`stc r0_bank`? direct use of r0-r7 with RB=1?). Whether the handler lowers RB or BL before calling C. Copy the first ~80 lines of each entry. |
| API call convention | `kcall.src`, `apicall.*`, headers with `FIRST_METHOD` | The address range of API traps (expected 0xF0010000 downward) and which exception they expect (address error 0x0E0 or TLB miss). |
| Address-space switch | search for `PTEH`, `ASID`, `SetCPUASID`, `SwitchAddressSpace`, `MMUCR` | Where PTEH is written with a new ASID, and the instruction sequence after it (is there an `rte` or `ldc sr` before user code runs?). This is where `trapa #0xFA` goes. |
| TLB invalidation | search for `ldtlb`, `0xF6000000`, `MMUCR`, `InvalidateTLB`, `FlushTLB` | How entries are removed: array writes, MMUCR.TI, or `ldtlb` with V=0. This is where `trapa #0xF9` goes. |
| Cache flush | search for `icbi`, `ocbwb`, `ocbp`, `OEMCacheRangeFlush` | Confirms the loader path that will carry `trapa #0xF2`. |
| rte sites | search for `rte` | What is in each delay slot (must never touch r0-r7). |
| FPU state | search for `fpscr`, `FPSCR`, `fmov` | How thread FPU state is saved; the FPSCR value the kernel installs. |

Bring back: the matching excerpts in `notes/ce/kernel-sh-notes.md`
(file name, line numbers, the lines). The whole `SH` directory can go to
`notes/ce/kernel-sh/` on the Mac for me to read; it stays uncommitted.

## Step 4. Find the OAL pieces to replace (30 min)

In the SH4 BSP's `SRC\OAL` (or `PLATFORM\COMMON\SRC\SH4`), list the files
and find:

- `startup.src` or `startup.s`: the image entry (`StartUp`), what it does
  before `KernelStart`.
- The timer: files mentioning `TMU`, `TCOR`, `TSTR` (SH7750 addresses
  `0xFFD8xxxx`).
- Interrupts: `IPR`, `INTEVT`, `OEMInterruptEnable`, the ISR table.
- Debug serial: `OEMInitDebugSerial`, `OEMWriteDebugByte`.
- `OEMInit`, `OEMIdle`, `OEMPowerOff`, `OEMIoControl`, `OEMCacheRangeFlush`.

Bring back: `notes/ce/oal-files.txt` with the file list and, per item above,
the file and function names. With that list I write `PLATFORM\CLASSPAD`
(docs/05 section 3 has the SH7305 register map).

## Step 5. Set up the transfer loop (15 min)

For the rest of the work the cycle is: edit OAL sources on the Mac
(committed here under `bsp/`), copy them into the VM's
`PLATFORM\CLASSPAD`, build, copy `NK.bin` back, run in casio-emu. Make the
shared folder hold both directions and keep a `build.bat` in the VM that
runs the build from a command prompt (`blddemo` / `build -c` in the BSP
directory, then `makeimg`) so no clicking is needed. Note the exact commands
that worked in `notes/ce/build-commands.txt`.

## What I do with the results

1. Step 2's run output tells whether the loader, ROMHDR and entry are
   right for a real image (likely fixes: image placed outside the guest
   window, `config.bib` addresses).
2. Step 3's notes settle the bank-1 assumption and locate the three
   hypercall sites (docs/04 "Address spaces", docs/05 section 3).
3. Step 4's list is the skeleton of `bsp/classpad/`, which then goes into
   the VM as `PLATFORM\CLASSPAD`.
