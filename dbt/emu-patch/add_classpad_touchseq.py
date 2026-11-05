#!/usr/bin/env python3
"""Headless stylus sequences for casio-emu's ClassPad profile (after
add_classpad_addin.py): HLE_CB_TOUCHSEQ="x,y,at,hold;x,y,at,hold;..." presses
the stylus at (x, y) from updateTimers tick `at` for `hold` ticks, one entry
after the other (ticks are absolute). Used to drive Casio's OS through its
dialogs and apps in cold-boot runs (docs/03, the eActivity-area experiment).
Prints "[INJECTTOUCHSEQ down/up]" like the single-press HLE_CB_TOUCH.

Idempotent; rerun after updating the emulator, then cmake --build build.

    python3 dbt/emu-patch/add_classpad_touchseq.py ~/Dev/casio-emu
"""
import os, sys

MARK = 'wince-classpad touchseq'

def main(root):
    p = os.path.join(root, 'src/hardware/timers/timers.c')
    s = open(p).read()
    if MARK in s:
        print('already patched', p); return 0
    anchor = 'void updateTimers(void) {\n  classpadIicPoll();\n  classpadInjectTouch();\n'
    assert anchor in s, 'updateTimers changed (apply add_classpad_addin.py first)'
    func = ('// HLE_CB_TOUCHSEQ="x,y,at,hold;..." (wince-classpad touchseq): a list of stylus\n'
            '// presses, each at an absolute updateTimers tick for a number of ticks, through\n'
            '// classpadSetTouch like HLE_CB_TOUCH above. ClassPad profile only.\n'
            'static void classpadInjectTouchSeq(void) {\n'
            '  static int en = -1, n = 0, i = 0, state = 0;\n'
            '  static int tx[64], ty[64], at[64], hold[64];\n'
            '  if (en < 0) {\n'
            '    const char* t = getenv("HLE_CB_TOUCHSEQ");\n'
            '    en = t && machineIsClasspad() ? 1 : 0;\n'
            '    while (en && *t && n < 64) {\n'
            '      tx[n] = (int) strtol(t, (char**)&t, 0); if (*t == \',\') t++;\n'
            '      ty[n] = (int) strtol(t, (char**)&t, 0); if (*t == \',\') t++;\n'
            '      at[n] = (int) strtol(t, (char**)&t, 0); if (*t == \',\') t++;\n'
            '      hold[n] = (int) strtol(t, (char**)&t, 0); if (hold[n] < 2) hold[n] = 2;\n'
            '      n++;\n'
            '      while (*t == \';\' || *t == \' \') t++;\n'
            '    }\n'
            '  }\n'
            '  if (!en || i >= n) return;\n'
            '  if (state == 0 && count >= (u32) at[i]) {\n'
            '    printf("[INJECTTOUCHSEQ down] %d %d at tick %u (%d/%d)\\n", tx[i], ty[i], count, i + 1, n); fflush(stdout);\n'
            '    classpadSetTouch(tx[i], ty[i], 1); state = 1;\n'
            '  } else if (state == 1 && count >= (u32) (at[i] + hold[i])) {\n'
            '    printf("[INJECTTOUCHSEQ up] at tick %u\\n", count); fflush(stdout);\n'
            '    classpadSetTouch(tx[i], ty[i], 0); state = 0; i++;\n'
            '  }\n'
            '}\n\n')
    s = s.replace(anchor, func + anchor + '  classpadInjectTouchSeq();\n', 1)
    open(p, 'w').write(s); print('patched', p)
    return 0

if __name__ == '__main__':
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser('~/Dev/casio-emu')))
