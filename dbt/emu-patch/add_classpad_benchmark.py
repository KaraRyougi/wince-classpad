#!/usr/bin/env python3
"""Add optional frame matching to a casio-emu checkout with ClassPad support.
Usage: add_classpad_benchmark.py /path/to/casio-emu
HLE_FBDUMP_ALL preserves black suspend frames. HLE_FB_EXPECT points to a
320x528 P6 reference: exact rows 50..199 terminate with the simulator tick.
This diagnoses the fixed My Device benchmark, not arbitrary application load.
"""
import pathlib, sys
p = pathlib.Path(sys.argv[1]) / "src/hardware/display/display.c"
s = p.read_text()
start = s.index("static void classpadFrameDump(")
end = s.index("\nvoid updateDisplayFromFramebuffer", start)
replacement = r'''
static void classpadFrameDump(const u16* frame) {
  const char* fp = getenv("HLE_FBDUMP");
  if (!fp) return;
  const int w = CLASSPAD_WIDTH, h = CLASSPAD_HEIGHT;
  u16 p0 = frame[0]; int varied = 0;
  for (int i = 0; i < w * h && varied < 500; i++) if (frame[i] != p0) varied++;
  if (varied < 500 && !getenv("HLE_FBDUMP_ALL")) return;
  char tmp[1024]; snprintf(tmp, sizeof tmp, "%s.tmp", fp);
  FILE* f = fopen(tmp, "wb");
  if (!f) return;
  fprintf(f, "P6\n%d %d\n255\n", w, h);
  for (int i = 0; i < w * h; i++) {
    u16 p = frame[i];
    unsigned char rgb[3] = {
      (unsigned char)(((p >> 11) & 0x1f) * 255 / 31),
      (unsigned char)(((p >> 5) & 0x3f) * 255 / 63),
      (unsigned char)((p & 0x1f) * 255 / 31) };
    fwrite(rgb, 1, 3, f);
  }
  fclose(f); rename(tmp, fp);
  /* Local benchmark: exact final directory region, excluding the clock. */
  const char *expected = getenv("HLE_FB_EXPECT");
  if (expected) {
    static unsigned char rgb[320*528*3]; static int ready;
    if (!ready) {
      FILE *e = fopen(expected, "rb");
      int ew, eh, max; if (!e || fscanf(e, "P6\n%d %d\n%d", &ew, &eh, &max) != 3 || ew != 320 || eh != 528 || max != 255) exit(3);
      fgetc(e); if (fread(rgb, 1, sizeof rgb, e) != sizeof rgb) exit(3); fclose(e); ready=1;
    }
    int match=1;
    for (int i=50*320; i<200*320 && match; ++i) {
      u16 p=frame[i];
      if (rgb[i*3] != ((p>>11)&31)*255/31 || rgb[i*3+1] != ((p>>5)&63)*255/63 || rgb[i*3+2] != (p&31)*255/31) match=0;
    }
    if (match) { extern u32 count; printf("[BENCH loaded] tick=%u\n",count); fflush(stdout); exit(0); }
  }
}
'''
p.write_text(s[:start] + replacement.lstrip('\n') + s[end:])
