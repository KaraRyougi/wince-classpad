#!/usr/bin/env python3
"""Let casio-emu's ClassPad profile (CALCEMU_MODEL=fx-cp400 / fx-cg500) run an
add-in, so the translator image and the CE flash image can be exercised with
the ClassPad's 320x528 panel, its IIC0 touch sensor and INTC priority
arbitration. Three additions, each behind a 'wince-classpad' marker:

1. src/gui/sdl/runtime_paths.c, configureClasspad(): with a .g3a on the
   command line (no HLE_COLDBOOT) the profile no longer refuses ("ClassPad
   requires real-OS cold boot"). It selects the machine and leaves the NOR
   empty (erased, 0xFF); HLE_NORFILE/HLE_OSROM are not set, so memory.c
   places HLE_OSROM at HLE_OSROM_PAD exactly as on the fx-CG100 and loads
   the .g3a into the 2 MiB window behind the 64 MiB NOR (0x84000000, aliased
   at 0x00300000). The cold-boot path is unchanged.
2. src/hardware/display/display.c: HLE_FBDUMP writes the ClassPad frame
   (320x528 RGB565 -> binary PPM, near-uniform frames skipped like the
   fx-CG100 dump) so `make nkshot` works on this profile too.
3. src/hardware/timers/timers.c: HLE_CB_TOUCH="x,y" HLE_CB_TOUCH_AT=<tick>
   HLE_CB_TOUCH_HOLD=<ticks> presses the stylus at (x, y) (x 0..319, y
   0..575, rows 528..575 are the icon strip) from the given updateTimers
   tick for the given number of ticks, through classpadSetTouch like a
   mouse click on the skin: the sensor packet changes, Port R bit 5 drops
   and INTEVT 0x700 is raised on the down edge. Prints "[INJECTTOUCH down]
   x y at tick N" / "[INJECTTOUCH up]".

Idempotent; rerun after updating the emulator, then cmake --build build.

    python3 dbt/emu-patch/add_classpad_addin.py ~/Dev/casio-emu
"""
import os, sys

MARK = 'wince-classpad'

def main(root):
    # 1. add-in mode for the ClassPad profile
    p = os.path.join(root, 'src/gui/sdl/runtime_paths.c')
    anchor = '  if(!coldBoot)return showFirmwareError("ClassPad requires real-OS cold boot.");\n'
    text = ('  /* add-in mode (wince-classpad): a .g3a on the command line runs at\n'
            '   * 0x00300000 on this profile too, with an erased NOR; HLE_OSROM and\n'
            '   * HLE_OSROM_PAD place a flash image into it (memory.c), as on the CG. */\n'
            '  if(!coldBoot){\n'
            '    if(!machineSelectExperimental(profile->key))return -1;\n'
            '    g_currentModel=profile->key;\n'
            '    setRuntimeDefault("CALCEMU_MODEL_NAME",profile->name);\n'
            '    if(runtimeAssetPath("assets/fx-cp400.bmp",asset,sizeof asset))setRuntimeDefault("HLE_FACEPLATE",asset);\n'
            '    if(joinPath(g_exportPath,sizeof g_exportPath,g_prefPath,"exports"))makeDirectory(g_exportPath);\n'
            '    printf("[startup] ClassPad model=%s add-in mode (erased NOR%s)\\n",profile->key,\n'
            '           configuredOs&&configuredOs[0]?", HLE_OSROM placed by memory.c":"");\n'
            '    return 0;\n'
            '  }\n')
    # the declaration of asset[] must come first: move the refusal below it
    s = open(p).read()
    if MARK not in s:
        decl = '  char userNor[PATH_BUFFER_SIZE],asset[PATH_BUFFER_SIZE];\n'
        assert anchor in s and decl in s, 'configureClasspad changed'
        s = s.replace(anchor, '', 1).replace(decl, decl + text, 1)
        open(p, 'w').write(s); print('patched', p)
    else:
        print('already patched', p)

    # 2. HLE_FBDUMP for the ClassPad frame
    p = os.path.join(root, 'src/hardware/display/display.c')
    anchor = '  if (machineIsClasspad()) { updateDisplay(classpadFrame()); return; }\n'
    text = ('  if (machineIsClasspad()) { classpadFrameDump(classpadFrame()); updateDisplay(classpadFrame()); return; }\n')
    func_anchor = 'void updateDisplayFromFramebuffer(void) {\n'
    func = ('// HLE_FBDUMP for the ClassPad panel (wince-classpad): the 320x528 RGB565 frame\n'
            '// as a binary PPM, near-uniform frames skipped so the file keeps the last\n'
            '// meaningful screen (the fx-CG100 dump below does the same).\n'
            'static void classpadFrameDump(const u16* frame) {\n'
            '  const char* fp = getenv("HLE_FBDUMP");\n'
            '  if (!fp) return;\n'
            '  const int w = CLASSPAD_WIDTH, h = CLASSPAD_HEIGHT;\n'
            '  u16 p0 = frame[0]; int varied = 0;\n'
            '  for (int i = 0; i < w * h && varied < 500; i++) if (frame[i] != p0) varied++;\n'
            '  if (varied < 500) return;\n'
            '  char tmp[1024]; snprintf(tmp, sizeof tmp, "%s.tmp", fp);\n'
            '  FILE* f = fopen(tmp, "wb");\n'
            '  if (!f) return;\n'
            '  fprintf(f, "P6\\n%d %d\\n255\\n", w, h);\n'
            '  for (int i = 0; i < w * h; i++) {\n'
            '    u16 p = frame[i];\n'
            '    unsigned char rgb[3] = {\n'
            '      (unsigned char)(((p >> 11) & 0x1f) * 255 / 31),\n'
            '      (unsigned char)(((p >> 5) & 0x3f) * 255 / 63),\n'
            '      (unsigned char)((p & 0x1f) * 255 / 31) };\n'
            '    fwrite(rgb, 1, 3, f);\n'
            '  }\n'
            '  fclose(f); rename(tmp, fp);\n'
            '}\n\n')
    s = open(p).read()
    if MARK not in s:
        assert anchor in s and func_anchor in s, 'display.c changed'
        s = s.replace(anchor, text, 1).replace(func_anchor, func + func_anchor, 1)
        open(p, 'w').write(s); print('patched', p)
    else:
        print('already patched', p)

    # 3. headless touch injection
    p = os.path.join(root, 'src/hardware/timers/timers.c')
    anchor = 'void updateTimers(void) {\n  classpadIicPoll();\n'
    func = ('// HLE_CB_TOUCH="x,y" HLE_CB_TOUCH_AT=<tick> HLE_CB_TOUCH_HOLD=<ticks> (wince-classpad):\n'
            '// press the ClassPad stylus at (x, y) from the given updateTimers tick for the given\n'
            '// number of ticks, as a click on the skin would (classpadSetTouch: sensor packet,\n'
            '// Port R bit 5, INTEVT 0x700 on the down edge). ClassPad profile only.\n'
            'static void classpadInjectTouch(void) {\n'
            '  static int en = -1, tx = 0, ty = 0, at = 0, hold = 0, state = 0;\n'
            '  if (en < 0) {\n'
            '    const char* t = getenv("HLE_CB_TOUCH");\n'
            '    en = t && machineIsClasspad() ? 1 : 0;\n'
            '    if (en) { tx = (int) strtol(t, (char**)&t, 0); if (*t == \',\') t++; ty = (int) strtol(t, NULL, 0); }\n'
            '    at = getenv("HLE_CB_TOUCH_AT") ? atoi(getenv("HLE_CB_TOUCH_AT")) : 4000;\n'
            '    hold = getenv("HLE_CB_TOUCH_HOLD") ? atoi(getenv("HLE_CB_TOUCH_HOLD")) : 4000;\n'
            '    if (hold < 2) hold = 2;\n'
            '  }\n'
            '  if (!en || state == 2) return;\n'
            '  if (state == 0 && count >= (u32) at) {\n'
            '    printf("[INJECTTOUCH down] %d %d at tick %u\\n", tx, ty, count); fflush(stdout);\n'
            '    classpadSetTouch(tx, ty, 1); state = 1;\n'
            '  } else if (state == 1 && count >= (u32) (at + hold)) {\n'
            '    printf("[INJECTTOUCH up] at tick %u\\n", count); fflush(stdout);\n'
            '    classpadSetTouch(tx, ty, 0); state = 2;\n'
            '  }\n'
            '}\n\n')
    text = anchor + '  classpadInjectTouch();\n'
    s = open(p).read()
    if MARK not in s:
        assert anchor in s, 'updateTimers changed'
        s = s.replace(anchor, func + text, 1)
        open(p, 'w').write(s); print('patched', p)
    else:
        print('already patched', p)
    return 0

if __name__ == '__main__':
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser('~/Dev/casio-emu')
    sys.exit(main(root))
