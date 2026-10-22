# loader/ (deferred)

The original plan was a gint/hollyhock add-in that reads NK.bin from storage,
copies the records into RAM and jumps to the kernel entry. That add-in would
run under Casio's big-endian OS on a big-endian core, and a little-endian CE
kernel cannot execute there (see docs/00-plan.md). The directory stays as the
place for whichever boot path the chosen route needs:

- route A: nothing here; casio-emu loads the image directly.
- route B: a little-endian NOR boot sector, plus a big-endian add-in that
  programs it into flash before the endian strap is flipped.
