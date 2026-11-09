//------------------------------------------------------------------------------
//  kitlstub.c - the KITL-less kernel's Ethernet debug stubs
//
//  The CE 5.0 kernel library expects these from the OAL even when the image
//  has no KITL (PLATFORM\ASPEN\KERNEL\BUILDEXE\KERN\kitlstub.c). Nothing here
//  is ever called: the translator is the debugger.
//------------------------------------------------------------------------------
#include <windows.h>
#include <halether.h>

USHORT wLocalMAC[3];    // saved copy of the MAC address: none

void InitDebugEther(void)
{
}

void CreateDeviceName(char *pMyAddr, char *szBuf)
{
    memcpy(szBuf, "NKNoKitl", 9);
}

BOOL EdbgSendUDP(BYTE *pFrameBuffer, EDBG_ADDR *pDestAddr, USHORT wSrcPort, BYTE *pData, UINT16 cwLength)
{
    return FALSE;
}
