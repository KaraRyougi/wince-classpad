# bsp/ - Platform Builder BSP sources

- `classpad/`: `PLATFORM\CLASSPAD`, the SH7305 OAL for CE 6.0 R3. Copy it
  into the Platform Builder VM; see `classpad/README.md`.

Needs Windows with Visual Studio 2005 and Platform Builder 6.0 R3 with the
SH4 CPU support and Shared Source installed (SH4 is dropped in Windows
Embedded Compact 7). The kernel (kernel.dll) is Platform Builder's own build of the
Shared Source tree plus the patches in `kernel/` (one so far, to a wait
path in kcalls.c; see `kernel/README.md`); everything SH7305-specific
lives in the OAL.

Later additions: `drivers/display` (R61524 frame buffer, protocol in
`dbt/guest/lcd.c`), `drivers/keybd` (KEYSC), `drivers/touch` (ADC).
