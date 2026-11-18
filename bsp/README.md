# bsp/ - Platform Builder BSP sources

- `classpad50/`: `PLATFORM\CLASSPAD` for Windows CE 5.0, the current target:
  the SH7305 OAL, the display, keyboard, touch and battery drivers, the
  applications and the image files. Needs Windows with Platform Builder 5.0
  and its SH4 support; see `classpad50/README.md` and `docs/08-ce50-port.md`.
- `classpad/`: the earlier BSP for CE 6.0 R3 (Visual Studio 2005, Platform
  Builder 6.0 R3 with SH4 support and Shared Source). It reached the desktop
  and is kept for reference; see `classpad/README.md`.
- `kernel/`: patches to the CE kernel sources (`kernel/README.md`).
- `tests/`: host tests for the parts of both BSPs that compile off the
  target (`make test` at the top).

Copy a BSP into the Platform Builder VM with `tools/stage_bsp50.py` (CRLF
conversion; `tools/windows/claude-build/stage.py` prepares the Pocket PC
binaries). Everything SH7305-specific lives in
the OAL and the drivers; the kernel is Platform Builder's own build plus the
patches in `kernel/`.
