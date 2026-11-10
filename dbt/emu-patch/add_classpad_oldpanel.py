#!/usr/bin/env python3
"""Let casio-emu's ClassPad LCD model stand in for the fx-CP400's older panel.

    HLE_CP_LCD_ID=<hex>   the ID the panel answers to RDID 0xDA (default 16)

Casio's OS (02.01.7003) treats ID 0x16 and 0x52 as the panel whose first
visible column is GRAM column 0; for every other ID LCD_SetDrawingBounds
(0x80038A10) adds 40 to the column window, and the backlight level is the
second of the four parameters of register 0xB9 (0x80038550) instead of
WRDISBV 0x51. With another ID the model shows GRAM columns 40..359, keeps
the four 0xB9 parameters (a dummy read, then the four) and prints every
write to them as "[classpad-lcd] B9 ..." (the first 64), and prints a 0x51
sent to it, which that panel does not have. Default behaviour is unchanged.

Idempotent; rerun after updating the emulator, then cmake --build build.

    python3 dbt/emu-patch/add_classpad_oldpanel.py ~/Dev/casio-emu
"""
import os, sys

MARK = 'wince-classpad oldpanel'

def rep(s, old, new):
    assert s.count(old) == 1, 'classpad.c changed: %r' % old[:50]
    return s.replace(old, new, 1)

def main(root):
    p = os.path.join(root, 'src/hardware/classpad/classpad.c')
    s = open(p).read()
    if MARK in s:
        print('already patched', p); return 0
    s = rep(s, '#include <stdio.h>\n',
            '#include <stdio.h>\n#include <stdlib.h>\n'
            '/* %s: HLE_CP_LCD_ID selects the panel variant */\n'
            'static unsigned panelId=0x16,panelCol0,readIndex,b9Logs;\n'
            'static u8 b9[4]={0x01,0x80,0x02,0x03};\n' % MARK)
    s = rep(s, '  if (command==0xda) return 0x16;\n',
            '  if (command==0xda) return panelId;\n'
            '  if (command==0xb9 && panelCol0) { unsigned i=readIndex++; return (i>=1 && i<=4)?b9[i-1]:0; }\n')
    s = rep(s, '    command=value & 255; parameterCount=0;\n',
            '    command=value & 255; parameterCount=0; readIndex=0;\n'
            '    if(panelCol0 && command==0x51 && b9Logs<64){printf("[classpad-lcd] 0x51 sent to the older panel\\n");b9Logs++;}\n')
    s = rep(s, '  if(command==0x2a || command==0x2b) {\n',
            '  if(command==0xb9 && panelCol0) {\n'
            '    if(parameterCount<4){b9[parameterCount++]=value&255;\n'
            '      if(parameterCount==4 && b9Logs<64){printf("[classpad-lcd] B9 %02x %02x %02x %02x\\n",b9[0],b9[1],b9[2],b9[3]);b9Logs++;}}\n'
            '    return;\n'
            '  }\n'
            '  if(command==0x2a || command==0x2b) {\n')
    s = rep(s, '    if(x<CLASSPAD_WIDTH && y<CLASSPAD_HEIGHT)frame[y*CLASSPAD_WIDTH+x]=value;\n',
            '    if(x>=panelCol0 && x-panelCol0<CLASSPAD_WIDTH && y<CLASSPAD_HEIGHT)frame[y*CLASSPAD_WIDTH+x-panelCol0]=value;\n')
    s = rep(s, '  touchDown=0;\n  displayEnabled=0;panelSleeping=1;\n',
            '  touchDown=0;\n  displayEnabled=0;panelSleeping=1;\n'
            '  { const char *e=getenv("HLE_CP_LCD_ID"); panelId=e?(unsigned)strtoul(e,NULL,16):0x16;\n'
            '    panelCol0=(panelId==0x16||panelId==0x52)?0:40; readIndex=b9Logs=0;\n'
            '    if(e)printf("[classpad-lcd] panel ID %02x, first visible column %u\\n",panelId,panelCol0); }\n')
    open(p, 'w').write(s); print('patched', p)
    return 0

if __name__ == '__main__':
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser('~/Dev/casio-emu')))
