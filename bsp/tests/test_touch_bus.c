/* Scripted bus contract, not a sensor emulator: check register ordering,
 * full packet length, last-byte NACK, bounded failure and next-read recovery. */
#include <assert.h>
#include <stdio.h>
#include "../classpad/SRC/INC/bsp_base_regs.h"
#include "../classpad/SRC/INC/classpad_touch.h"
typedef struct { unsigned address, value; int write; } Op;
static Op ops[8192];
static unsigned count, cursor, sleeps;
static void expect(int write, unsigned a, unsigned v) { assert(count < 8192); ops[count++] = (Op){a,v,write}; }
static unsigned read8(unsigned a) { assert(cursor<count && !ops[cursor].write && ops[cursor].address==a); return ops[cursor++].value; }
static void write8(unsigned a,unsigned v) { assert(cursor<count && ops[cursor].write && ops[cursor].address==a && ops[cursor].value==(v&255)); cursor++; }
static void Sleep(unsigned ms) { assert(ms==1); sleeps++; }
#define MMIO_R8 read8
#define MMIO_W8 write8
#include "../classpad/SRC/INC/classpad_touch_bus.h"
#define W(a,v) expect(1,a,v)
#define R(a,v) expect(0,a,v)
#define SR(v) R(IIC0_ICSR,v)
static void reset_script(void) { count=cursor=sleeps=0; }
static void reset_bus(void) {
 W(IIC0_ICCR,0); W(IIC0_ICCR,0x80); W(IIC0_ICCL,0x29); W(IIC0_ICCH,0x22);
 W(IIC0_ICSR,0); W(IIC0_ICIC,0x0e);
}
static void release_wait(void) { SR(0x12); W(IIC0_ICSR,0x10); }
static void packet_script(unsigned n) {
 unsigned i;
 reset_bus(); SR(0); W(IIC0_ICCR,0x94); SR(1); W(IIC0_ICDR,0x80); SR(0x12);
 W(IIC0_ICDR,0x84); release_wait(); SR(0x12);
 W(IIC0_ICCR,0x94); release_wait(); SR(0x11); W(IIC0_ICDR,0x81); SR(0x12);
 W(IIC0_ICCR,0x81); release_wait();
 for(i=0;i<n;i++) {
  SR(0x12); if(i==n-1) W(IIC0_ICCR,0xc0);
  release_wait(); SR(0x11); R(IIC0_ICDR,i+1);
 }
 SR(0); W(IIC0_ICCR,0);
}
int main(void) {
 unsigned char packet[16]; unsigned i;
 reset_script(); packet_script(16); assert(TouchBusRead(packet,16)); assert(cursor==count);
 for(i=0;i<16;i++) assert(packet[i]==i+1);
 reset_script(); packet_script(1); assert(TouchBusRead(packet,1)); assert(cursor==count);
 for(i=0;i<2;i++) {
  reset_script(); reset_bus(); SR(0); W(IIC0_ICCR,0x94); SR(1);
  W(IIC0_ICDR,0x80); SR(i?ICSR_AL:ICSR_TACK); W(IIC0_ICCR,0);
  assert(!TouchBusRead(packet,16)); assert(cursor==count && g_touchBusStage==3);
 }
 reset_script(); reset_bus(); for(i=0;i<20*64;i++) SR(ICSR_BUSY); W(IIC0_ICCR,0);
 assert(!TouchBusRead(packet,16)); assert(cursor==count && sleeps==20 && g_touchBusStage==1);
 reset_script(); packet_script(16); assert(TouchBusRead(packet,16)); assert(cursor==count);
 reset_script(); assert(!TouchBusRead(packet,0)); assert(!TouchBusRead(NULL,16)); assert(!cursor);
 puts("touch bus: packet, NACK, arbitration, timeout and recovery PASS");
}
