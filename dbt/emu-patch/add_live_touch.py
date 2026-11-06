#!/usr/bin/env python3
"""Optional file-driven stylus input for reproducing CE UI sequences.

HLE_CB_TOUCH_FILE names a file containing: sequence_id x y hold_ticks.
Replace it atomically for each press. Coordinates are native 320x528 pixels;
4000 ticks holds for about 122 modeled ms. No effect unless opted in.
"""
from pathlib import Path
import sys

path = Path(sys.argv[1]) / 'src/hardware/timers/timers.c'
source = path.read_text()
if 'static void classpadLiveTouch(void)' in source:
    raise SystemExit('Live touch already installed')
code = r'''
static void classpadLiveTouch(void) {
  static const char *path;
  static int initialized, down;
  static unsigned last, release;
  if (!initialized) { path = getenv("HLE_CB_TOUCH_FILE"); initialized = 1; }
  if (!path || !machineIsClasspad()) return;
  if (down && (int)(count - release) >= 0) {
    classpadSetTouch(0, 0, 0); down = 0;
    printf("[LIVETOUCH up] tick=%u\n", count); fflush(stdout);
  }
  if (down || (count & 1023)) return;
  FILE *f = fopen(path, "r");
  if (!f) return;
  unsigned id, hold; int x, y;
  int n = fscanf(f, "%u %d %d %u", &id, &x, &y, &hold);
  fclose(f);
  if (n != 4 || id == last || !id || x < 0 || x >= 320 || y < 0 || y >= 576) return;
  last = id; if (hold < 2) hold = 2; if (hold > 32768) hold = 32768;
  classpadSetTouch(x, y, 1); down = 1; release = count + hold;
  printf("[LIVETOUCH down] id=%u x=%d y=%d tick=%u\n", id, x, y, count); fflush(stdout);
}
'''
anchor = 'void updateTimers(void) {'
assert anchor in source
source = source.replace(anchor, code + '\n' + anchor + '\n  classpadLiveTouch();', 1)
path.write_text(source)
