# Platform Builder Windows VM Workflow

Windows CE images (`NK.bin` and `EXT.bin`) are built in a Windows VM running **Platform Builder 5.0** (active target: `bsp/classpad50/`). *(The archived CE 6.0 R3 BSP in `bsp/classpad/` uses Visual Studio 2005 + Platform Builder 6.0 R3.)*

## 1. VM Prerequisites

- **OS:** Windows XP SP3 or Windows 7 32-bit VM (40 GB disk, 2–4 GB RAM) with a shared folder or SMB mount accessible from macOS/Linux (e.g. `/Volumes/WINCE500`).
- **Platform Builder 5.0** installed with:
  - **SH4** CPU support (`%_WINCEROOT%\SDK\BIN\I386\SH\shcl.exe`, `shlink.exe`, `PUBLIC\COMMON\OAK\LIB\SH4\...`).
  - **Shared Source** (`%_WINCEROOT%\PRIVATE\WINCEOS\COREOS\NK\KERNEL\SH\...`) — required for the SH-3 PE loader patch (`bsp/kernel/`).
  - Recommended: take a VM snapshot right after installation and QFEs.

## 2. Staging the BSP from macOS / Linux

Platform Builder's `build.exe` requires **CRLF** line endings in `dirs`, `sources`, `.bib`, `.reg`, and batch files. Always stage `bsp/classpad50/` with `tools/stage_bsp50.py`:

```sh
python3 tools/stage_bsp50.py /Volumes/WINCE500/PLATFORM/CLASSPAD
```

What `stage_bsp50.py` does:
- Converts all text files to CRLF and copies them into `PLATFORM\CLASSPAD`.
- Preserves pre-staged third-party binaries already in the destination (such as realigned `mfcce300.dll`, Math Xpander files in `FILES\xpander\`, and SpreadCE in `FILES\spreadce\`) unless `--force-binaries` is passed.

See [bsp/classpad50/README.md](../bsp/classpad50/README.md) for preparing optional third-party Pocket PC binaries (`tools/pe4k.py`, `tools/sh3prep.py`, `tools/sh3imports.py`).

## 3. Building in Platform Builder 5.0

1. **Full build (first time or after OS Design / catalog changes):**
   - Open the `CLASSPAD` workspace (`PBWorkspaces\CLASSPAD\`) in Platform Builder 5.0.
   - Select **CLASSPAD: SH4 Release**.
   - Run **Build OS → Sysgen** (builds OS libraries + BSP + `Make Run-Time Image`).
2. **Incremental BSP / driver rebuild (typical edit loop):**
   - Stage updated source files with `tools/stage_bsp50.py`.
   - In Platform Builder, either right-click the modified directory (e.g. `DRIVERS\DISPLAY` or `OAL\OALLIB`) → **Build Current Project**, or run **Build OS → Build Current BSP and Subprojects**.
   - Run **Build OS → Copy Files to Release Directory** and **Build OS → Make Run-Time Image**.
   - `PostFmergeBib.bat` and `PostRomImage.bat` automatically split `EXT` entries out of `ce.bib` and run `romimage` twice, producing both `NK.bin` and `EXT.bin` in `PBWorkspaces\CLASSPAD\RelDir\CLASSPAD_SH4_Release\`.

## 4. Post-Build Verification & Packaging (macOS / Linux)

After Platform Builder finishes:

```sh
REL="/Volumes/WINCE500/PBWorkspaces/CLASSPAD/RelDir/CLASSPAD_SH4_Release"

# 1. Inspect the built NK.bin (verify CPU 0x01A6 SH4 and ROM bounds)
python3 tools/nkbin.py "$REL/NK.bin"

# 2. If nk.exe (OAL/kernel) changed, check if kdev signatures changed
python3 tools/kernel_devfuncs.py "$REL/NK.bin" "$REL/kern.map" --table kdev_funcs50 > /tmp/kdev50.h
diff -u dbt/rt/kernel_dev50.h /tmp/kdev50.h
# (If kdev50.h changed, update dbt/rt/kernel_dev50.h and rebuild wince.hh3!)

# 3. Pack compressed NK.bin / EXT.bin for the calculator (~50% smaller)
python3 tools/cepack.py -o dbt/build/cez "$REL/NK.bin" "$REL/EXT.bin"
dbt/build/ceztest "$REL/NK.bin" dbt/build/cez/NK.bin
dbt/build/ceztest "$REL/EXT.bin" dbt/build/cez/EXT.bin

# 4. Smoke-test in casio-emu before flashing to real hardware
make -C dbt NKBIN="$REL/NK.bin" EXTBIN="$REL/EXT.bin" RUN_SECS=300 run
```

> **Tip:** Never commit Microsoft Shared Source files (`PRIVATE\...`) or proprietary Pocket PC binaries into git. Use `notes/ce/` (git-ignored except `.md`/`.txt` summaries) for any temporary exports from the VM.
