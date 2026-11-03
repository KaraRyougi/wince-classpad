//------------------------------------------------------------------------------
//  power.c - suspend/resume (not supported yet)
//------------------------------------------------------------------------------
#include <bsp.h>

VOID OEMPowerOff(void)
{
    // No wake sources are wired up: sleep once and resume immediately.
    INTERRUPTS_ON();
    __asm("sleep");
    INTERRUPTS_OFF();
}
