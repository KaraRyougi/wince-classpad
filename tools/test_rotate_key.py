#!/usr/bin/env python3
"""Host test of the actual touch DDSI contact routing (stub hardware/MDD):
the icon strip (Settings, Menu, Main, Resize, Swap, Rotate, Esc) and normal
desktop contacts."""
from pathlib import Path
import subprocess, tempfile
root = Path(__file__).resolve().parents[1]
s = (root/'bsp/classpad/SRC/DRIVERS/TOUCH/classpad_touch.cpp').read_text()
hit = s[s.index('#define STRIP_X'):s.index('static void IconEventsOpen')]
point = s[s.index('void DdsiTouchPanelGetPoint('):]
pre = r"""
#include <cassert>
#include <cstring>
typedef int BOOL; typedef int INT; typedef unsigned char UINT8;
typedef int *P_TOUCH_PANEL_SAMPLE_FLAGS;
typedef int HANDLE; typedef const char *LPCTSTR;
#define TEXT(x) x
#define FALSE 0
#define TRUE 1
#define RETAILMSG(a,b) ((void)g_iconName)
#define TOUCH_PACKET_BYTES 16
#define SYSINTR_NOP 0
#define SYSINTR_UNDEFINED -1
#define TouchSampleIgnore 0
#define TouchSampleValidFlag 1
#define TouchSampleDownFlag 2
int g_lastX, g_lastY, g_samples, g_contacts, gIntrTouch=1;
bool g_prevDown, g_stripContact, g_sampling, g_suspended;
int g_iconArmed=-1;
bool g_panel=true, pen, bus=true;
int fired[8], rawX, rawY;
void SetEvent(int h) { ++fired[h]; }
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
 for(int i=0;i<7;++i) g_iconEvent[i]=g_iconEventName[i]?i+1:0;   // handle = icon + 1
 const int ROT=6, ESC=7, SET=1, MENU=2, MAIN=3;
#define events fired[ROT]
 assert(sample(true,251,550)==0);
 for(int i=0;i<100;++i)assert(sample(true,251,550)==0);
 assert(events==0); assert(sample(false,251,550)==0); assert(events==1);
 sample(false,251,550); assert(events==1);
 sample(true,251,550); sample(true,297,550); sample(true,251,550);
 sample(false,251,550); assert(events==1); assert(fired[ESC]==0); // drag cancellation stays cancelled
 sample(true,206,550); sample(false,206,550); assert(events==1);    // Swap: no event
 sample(true,160,550); sample(false,160,550);                       // Resize: no event
 for(int k=0;k<8;++k) if(k!=ROT) assert(fired[k]==0);
 assert(sample(true,251,527)==3); assert(sample(false,251,527)==1);
 sample(true,251,550); bus=false; sample(true,251,550); assert(events==1);
 bus=true; sample(false,251,550); assert(events==2);
 sample(true,251,550); g_iconArmed=-1; // power handler cancellation
 sample(false,251,550); assert(events==2);
 assert(sample(true,40,40)==3); assert(sample(false,40,40)==1);
 // the other icons: centres and edges (x=0 and x=319), one event each
 sample(true,23,550); sample(false,23,550); assert(fired[SET]==1);
 sample(true,0,575); sample(false,0,575); assert(fired[SET]==2);
 sample(true,69,550); sample(false,69,550); assert(fired[MENU]==1);
 sample(true,114,550); sample(false,114,550); assert(fired[MAIN]==1);
 sample(true,297,550); sample(false,297,550); assert(fired[ESC]==1);
 sample(true,319,560); sample(false,319,560); assert(fired[ESC]==2);
 sample(true,69,550); sample(true,114,550); sample(false,114,550);  // Menu slid onto Main: nothing
 assert(fired[MENU]==1 && fired[MAIN]==1);
 assert(events==2);
}
"""
with tempfile.TemporaryDirectory() as d:
 p=Path(d); (p/'test.cpp').write_text(pre+hit+point+post)
 subprocess.run(['c++','-Wall','-Wextra',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('Touch-strip icon routing: PASS')
