#!/usr/bin/env python3
"""Let casio-emu's ClassPad profile cold-boot a NOR whose OS bytes were
patched (HLE_CLASSPAD_NOVERIFY=1 skips the OS SHA-256 check in
src/gui/sdl/runtime_paths.c), for experiments on Casio's OS such as the
eActivity volume-size patch in docs/03. Development knob only: the model
records at 0x300/0x14FFEB0 are still checked.

Idempotent; rerun after updating the emulator, then cmake --build build.

    python3 dbt/emu-patch/add_classpad_noverify.py ~/Dev/casio-emu
"""
import os, sys

MARK = 'wince-classpad noverify'

def main(root):
    p = os.path.join(root, 'src/gui/sdl/runtime_paths.c')
    s = open(p).read()
    if MARK in s:
        print('already patched', p); return 0
    old = '  int valid=classpadValidateImage(image,n,profile->key);SDL_free(image);\n'
    new = ('  /* HLE_CLASSPAD_NOVERIFY (wince-classpad noverify): accept a patched OS */\n'
           '  int valid=getenv("HLE_CLASSPAD_NOVERIFY")?(image&&n==CLASSPAD_NOR_SIZE):classpadValidateImage(image,n,profile->key);SDL_free(image);\n')
    assert s.count(old) == 1, 'configureClasspad changed'
    s = s.replace(old, new, 1)
    old2 = '    int valid=classpadValidateImage(bytes,n,g_currentModel);SDL_free(bytes);\n'
    if old2 in s:
        s = s.replace(old2, '    int valid=getenv("HLE_CLASSPAD_NOVERIFY")?(bytes&&n==CLASSPAD_NOR_SIZE):classpadValidateImage(bytes,n,g_currentModel);SDL_free(bytes);\n', 1)
    open(p, 'w').write(s); print('patched', p)
    return 0

if __name__ == '__main__':
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser('~/Dev/casio-emu')))
