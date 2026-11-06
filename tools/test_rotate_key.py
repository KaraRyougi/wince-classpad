#!/usr/bin/env python3
"""Host test of the actual touch DDSI contact routing (stub hardware/MDD)."""
from pathlib import Path
import subprocess, tempfile
root = Path(__file__).resolve().parents[1]
s = (root/'bsp/classpad/SRC/DRIVERS/TOUCH/classpad_touch.cpp').read_text()
hit = s[s.index('static BOOL InRotate('):s.index('static INT      g_lastX')]
point = s[s.index('void DdsiTouchPanelGetPoint('):]
pre = r"""
#include <cassert>
#include <cstring>
typedef int BOOL; typedef int INT; typedef unsigned char UINT8;
typedef int *P_TOUCH_PANEL_SAMPLE_FLAGS;
#define FALSE 0
#define TRUE 1
#define RETAILMSG(a,b) ((void)0)
#define TOUCH_PACKET_BYTES 16
#define SYSINTR_NOP 0
#define SYSINTR_UNDEFINED -1
#define TouchSampleIgnore 0
#define TouchSampleValidFlag 1
#define TouchSampleDownFlag 2
int g_lastX, g_lastY, g_samples, g_contacts, gIntrTouch=1;
bool g_prevDown, g_stripContact, g_rotateArmed, g_sampling, g_suspended;
bool g_panel=true, pen, bus=true;
int g_rotateEvent=1, events, rawX, rawY;
void SetEvent(int) { ++events; }
void InterruptDone(int) {}
bool PenDown() { return pen; }
bool TouchReadPacket(UINT8 *p,int) {
 if(!bus)return false;
 int x=rawX<<4,y=rawY<<4;
 p[0]=x>>8;p[1]=x;p[2]=y>>8;p[3]=y;return true;
}
"""
post = r"""
int sample(bool down,int x,int y) {
 pen=down; rawX=0x20B+x*0x9B6/256; rawY=0xF4+y*0x66F/256;
 int flags,a,b; DdsiTouchPanelGetPoint(&flags,&a,&b); return flags;
}
int main() {
 assert(sample(true,251,550)==0);
 for(int i=0;i<100;++i)assert(sample(true,251,550)==0);
 assert(events==0); assert(sample(false,251,550)==0); assert(events==1);
 sample(false,251,550); assert(events==1);
 sample(true,251,550); sample(true,297,550); sample(true,251,550);
 sample(false,251,550); assert(events==1); // drag cancellation stays cancelled
 sample(true,206,550); sample(false,206,550); assert(events==1);
 assert(sample(true,251,527)==3); assert(sample(false,251,527)==1);
 sample(true,251,550); bus=false; sample(true,251,550); assert(events==1);
 bus=true; sample(false,251,550); assert(events==2);
 sample(true,251,550); g_rotateArmed=false; // power handler cancellation
 sample(false,251,550); assert(events==2);
 assert(sample(true,40,40)==3); assert(sample(false,40,40)==1);
}
"""
with tempfile.TemporaryDirectory() as d:
 p=Path(d); (p/'test.cpp').write_text(pre+hit+point+post)
 subprocess.run(['c++','-Wall','-Wextra',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('Rotate touch-strip routing: PASS')
