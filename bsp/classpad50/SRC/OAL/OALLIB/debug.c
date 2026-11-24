//------------------------------------------------------------------------------
//  debug.c - debug console through the translator
//
//  With exceptions enabled a byte goes straight out as a hypercall
//  (trapa #0xF3). With SR.BL=1 (SHInit, OEMInit, the kernel sign-on, OEMIdle)
//  a trapa would reset the CPU, so the byte is appended to a ring in the last
//  page of the RAM window; the translator prints the ring whenever it is next
//  entered (any lookup miss or hypercall) and at halt. Both sides use the
//  uncached alias so no cache maintenance is needed.
//------------------------------------------------------------------------------
#include <bsp.h>

#define RING_WR     (((volatile UINT32 *)CLASSPAD_DBG_RING_UA)[0])
#define RING_RD     (((volatile UINT32 *)CLASSPAD_DBG_RING_UA)[1])
#define RING_MAGIC  (((volatile UINT32 *)CLASSPAD_DBG_RING_UA)[2])
#define RING_BYTES  ((volatile UINT8 *)(CLASSPAD_DBG_RING_UA + 16))

static UINT32 g_ringDropped;

//------------------------------------------------------------------------------
void OEMInitDebugSerial()
{
    // The translator initialised the ring before starting the image. If the
    // magic is missing (image run on real hardware without the translator),
    // output is simply dropped.
}

//------------------------------------------------------------------------------
static void RingPut(BYTE ch)
{
    UINT32 wr, rd;
    if (RING_MAGIC != CLASSPAD_DBG_RING_MAGIC) return;
    wr = RING_WR;
    rd = RING_RD;
    if (wr - rd >= CLASSPAD_DBG_RING_BYTES) {   // full and the translator has not run: drop
        g_ringDropped++;
        return;
    }
    RING_BYTES[wr % CLASSPAD_DBG_RING_BYTES] = ch;
    RING_WR = wr + 1;
}

//------------------------------------------------------------------------------
void OEMWriteDebugByte(BYTE ch)
{
    if (ClasspadCanTrap()) {
        ClasspadHcPutc(ch);                 // also drains the ring
    } else {
        RingPut(ch);
    }
}

//------------------------------------------------------------------------------
void OEMWriteDebugString(unsigned short *string)
{
    if (ClasspadCanTrap()) {
        char buf[128];
        int n = 0;
        while (*string != L'\0') {
            buf[n++] = (char)(BYTE)*string++;
            if (n == sizeof(buf) - 1) {
                buf[n] = '\0';
                ClasspadHcPuts(buf);
                n = 0;
            }
        }
        if (n > 0) {
            buf[n] = '\0';
            ClasspadHcPuts(buf);
        }
    } else {
        while (*string != L'\0') {
            RingPut((BYTE)*string++);
        }
    }
}

//------------------------------------------------------------------------------
int OEMReadDebugByte()
{
    return OEM_DEBUG_READ_NODATA;
}

//------------------------------------------------------------------------------
void OEMClearDebugCommError()
{
}
