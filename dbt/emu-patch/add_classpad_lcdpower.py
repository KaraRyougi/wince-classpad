#!/usr/bin/env python3
"""Make casio-emu's ClassPad LCD model notice the panel's deep standby, so the
CE suspend path that copies Casio's power-off (OAL power.c, docs/08) can be
checked: apply after add_classpad_oldpanel.py.

  - Command 0xB1 with an odd first parameter (0xB1 0x51 on the newer panel,
    0xB1 0x01 on the older one: the last step of the OS's LCD power-off
    0x80038A9A) puts the model into deep standby: the frame is blank and
    pixel writes are dropped, as on a panel that has lost its set-up.
  - It leaves deep standby only on SLPOUT (0x11) after a register set-up
    command of the cold power-on has been seen (0xE0, the gamma table of the
    newer panel, or 0xC8 of the older one). SLPOUT without it prints
    "[classpad-lcd] SLPOUT in deep standby without the power-on set-up" and
    the panel stays dead.
  - HLE_CP_LCD_LOG=1 prints every command and parameter except pixel data
    (the first 6000 lines).
  - 0xA44C0006 (written by the OS's LCD interface set-up 0x8008138C, byte)
    becomes a plain register.

Idempotent; rerun after updating the emulator, then cmake --build build.

    python3 dbt/emu-patch/add_classpad_lcdpower.py ~/Dev/casio-emu
"""
import os, sys

MARK = 'wince-classpad lcdpower'

def rep(s, old, new):
    assert s.count(old) == 1, 'classpad.c changed: %r' % old[:60]
    return s.replace(old, new, 1)

def main(root):
    p = os.path.join(root, 'src/hardware/classpad/classpad.c')
    s = open(p).read()
    if MARK in s:
        print('already patched', p); return 0
    assert 'wince-classpad oldpanel' in s, 'apply add_classpad_oldpanel.py first'
    s = rep(s, 'static u8 b9[4]={0x01,0x80,0x02,0x03};\n',
            'static u8 b9[4]={0x01,0x80,0x02,0x03};\n'
            '/* %s: deep standby, command log, 0xA44C0006 */\n'
            'static int deepStandby,setupSeen,lcdLog;\n'
            'static unsigned lcdLogLines;\n'
            'static u8 lcdifCr6;\n' % MARK)
    s = rep(s, '    command=value & 255; parameterCount=0; readIndex=0;\n',
            '    command=value & 255; parameterCount=0; readIndex=0;\n'
            '    if(lcdLog && lcdLogLines<6000 && command!=0x2c && command!=0x2a && command!=0x2b && command!=0xda){printf("[classpad-lcd] cmd %02x\\n",command);lcdLogLines++;}\n'
            '    if(deepStandby && (command==0xe0 || command==0xc8))setupSeen=1;\n'
            '    if(deepStandby && command==0x11){\n'
            '      if(setupSeen){deepStandby=0;printf("[classpad-lcd] out of deep standby\\n");}\n'
            '      else printf("[classpad-lcd] SLPOUT in deep standby without the power-on set-up\\n");\n'
            '    }\n')
    s = rep(s, '  if(command==0xb9 && panelCol0) {\n',
            '  if(lcdLog && lcdLogLines<6000 && command!=0x2c && command!=0x2a && command!=0x2b){printf("[classpad-lcd]   %02x\\n",value&255);lcdLogLines++;}\n'
            '  if(command==0xb1 && parameterCount==0 && (value&1) && !deepStandby){\n'
            '    deepStandby=1;setupSeen=0;parameterCount=1;printf("[classpad-lcd] deep standby\\n");return;\n'
            '  }\n'
            '  if(command==0xb9 && panelCol0) {\n')
    s = rep(s, '    if(x>=panelCol0 && x-panelCol0<CLASSPAD_WIDTH && y<CLASSPAD_HEIGHT)frame[y*CLASSPAD_WIDTH+x-panelCol0]=value;\n',
            '    if(!deepStandby && x>=panelCol0 && x-panelCol0<CLASSPAD_WIDTH && y<CLASSPAD_HEIGHT)frame[y*CLASSPAD_WIDTH+x-panelCol0]=value;\n')
    s = rep(s, 'u16* classpadFrame(void) { return displayEnabled && !panelSleeping ? frame : blankFrame; }\n',
            'u16* classpadFrame(void) { return displayEnabled && !panelSleeping && !deepStandby ? frame : blankFrame; }\n')
    s = rep(s, '    panelCol0=(panelId==0x16||panelId==0x52)?0:40; readIndex=b9Logs=0;\n',
            '    panelCol0=(panelId==0x16||panelId==0x52)?0:40; readIndex=b9Logs=0;\n'
            '    deepStandby=setupSeen=0;lcdLogLines=0;lcdLog=getenv("HLE_CP_LCD_LOG")!=NULL;lcdifCr6=0;\n')
    s = rep(s, '  defineRegCBUnsized("ClassPad LCD",lcdRead,lcdWrite,0xb4000000);\n',
            '  defineRegCBUnsized("ClassPad LCD",lcdRead,lcdWrite,0xb4000000);\n'
            '  defineReg("ClassPad LCD interface +6",lcdifCr6,0xa44c0006);\n')
    open(p, 'w').write(s); print('patched', p)
    return 0

if __name__ == '__main__':
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser('~/Dev/casio-emu')))
