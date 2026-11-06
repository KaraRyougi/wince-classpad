#!/usr/bin/env python3
"""Exercise the driver's actual Dirty method, including a consumer interleave."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'bsp/classpad/SRC/DRIVERS/DISPLAY/classpad_ddi.cpp').read_text()
method = source[source.index('void ClasspadGPE::Dirty('):source.index('SCODE ClasspadGPE::Line(')]
pre = r'''
#include <cassert>
typedef bool BOOL;
struct ClasspadGPE {
 int m_cs=0, m_hEvent=1, m_nScreenHeightSave=528;
 int m_dirtyTop=528, m_dirtyBottom=0;
 void Dirty(int,int);
};
ClasspadGPE d;
bool locked=false, event=false, consumeAtUnlock=false;
int wakes=0, consumedTop=528, consumedBottom=0;
void EnterCriticalSection(int*) { assert(!locked); locked=true; }
void consume() {
 assert(!locked); event=false;
 consumedTop=d.m_dirtyTop; consumedBottom=d.m_dirtyBottom;
 d.m_dirtyTop=528; d.m_dirtyBottom=0;
}
void LeaveCriticalSection(int*) {
 assert(locked); locked=false;
 if(consumeAtUnlock) { consumeAtUnlock=false; consume(); }
}
void SetEvent(int) { assert(!locked); ++wakes; event=true; }
'''
post = r'''
int main() {
 d.Dirty(10,20);
 for(int i=0;i<1000;i++) d.Dirty(5,30);
 assert(wakes==1 && event && d.m_dirtyTop==5 && d.m_dirtyBottom==30);
 consume(); assert(consumedTop==5 && consumedBottom==30);
 d.Dirty(100,120); assert(wakes==2 && event);
 consume();
 // The consumer snapshots the first batch before its producer signals.
 consumeAtUnlock=true; d.Dirty(40,50);
 assert(consumedTop==40 && consumedBottom==50 && event);
 d.Dirty(60,70); consume();
 assert(consumedTop==60 && consumedBottom==70);
 // A producer extends a pending batch just before the consumer snapshots it.
 d.Dirty(20,30); int before=wakes;
 consumeAtUnlock=true; d.Dirty(0,60);
 assert(wakes==before && consumedTop==0 && consumedBottom==60);
 // Out-of-range and empty regions must neither wake nor dirty the frame.
 before=wakes; d.Dirty(600,700); d.Dirty(-20,-10); d.Dirty(10,10);
 assert(wakes==before && d.m_dirtyTop>=d.m_dirtyBottom);
 d.Dirty(-10,600); assert(event && d.m_dirtyTop==0 && d.m_dirtyBottom==528);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    cpp, exe = Path(tmp)/'dirty.cpp', Path(tmp)/'dirty'
    cpp.write_text(pre + method + post)
    subprocess.run(['c++', '-std=c++11', str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('PASS: coalesced dirty notifications, consumer interleavings, clipping')
