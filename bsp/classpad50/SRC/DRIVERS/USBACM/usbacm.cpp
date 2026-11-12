//------------------------------------------------------------------------------
//  usbacm.cpp - USB serial function with CDC ACM descriptors
//
//  CE 5.0's serial function client (serialusbfn.dll) shows the host one
//  vendor-specific interface, which only ActiveSync's Windows driver binds
//  to. A Mac or a Linux PC want the Communications Device Class: this DLL is
//  the same client with CDC ACM descriptors and class requests, so the host
//  loads its stock driver (/dev/cu.usbmodem*, /dev/ttyACM*) and CE gets a
//  COM port for RAS/PPP or a terminal.
//
//  The client itself is Microsoft's: its source is compiled from where
//  Platform Builder keeps it (PUBLIC\COMMON\OAK\DRIVERS\USBFN\CLASS\SERIAL,
//  on the include path, see sources), not copied here. What this file adds
//  around it:
//    - the descriptor tree is rearranged before it is registered: device
//      class 02, interface 0 = Communications/ACM with the header, call
//      management, ACM and union functional descriptors and the interrupt
//      endpoint, interface 1 = CDC Data with the two bulk endpoints. The
//      endpoint descriptors stay the stock arrays, which the stock code
//      reads when it opens its pipes.
//    - SET_LINE_CODING and GET_LINE_CODING (7 bytes on the control pipe;
//      the values are kept and returned, nothing else: there is no UART),
//      SEND_BREAK accepted, SET_CONTROL_LINE_STATE as in the stock client
//      (DTR -> DSR and carrier, RTS -> CTS on the CE side).
//    - the stock client's 2-byte status on the interrupt endpoint is not a
//      CDC notification, so nothing is sent there (SetDTR/SetRTS do nothing;
//      a host that wants carrier detect does not get it: pppd "local").
//
//  Registry: [HKLM\Drivers\USB\FunctionDrivers\CDC_ACM] in platform.reg.
//  Not built or run when written.
//------------------------------------------------------------------------------
#define USE_INTERRUPT_ENDPOINT                      // the stock client registers its three-endpoint tree
#define CreateSerialObject StockCreateSerialObject  // ours makes the subclass
#include <usbfndrv.cpp>
#undef CreateSerialObject

#define CDC_SET_LINE_CODING     0x20
#define CDC_GET_LINE_CODING     0x21
#define CDC_SEND_BREAK          0x23
#define CDC_LINE_CODING_BYTES   7
#define CDC_CLASS_OUT           (USB_REQUEST_CLASS | USB_REQUEST_FOR_INTERFACE | USB_REQUEST_HOST_TO_DEVICE)
#define CDC_CLASS_IN            (USB_REQUEST_CLASS | USB_REQUEST_FOR_INTERFACE | USB_REQUEST_DEVICE_TO_HOST)

// After the communications interface descriptor: header (CDC 1.10), call
// management (none, data interface 1), ACM (line coding and control line
// state), union (master 0, slave 1)
static BYTE g_acmFunctional[] = {
    5, 0x24, 0x00, 0x10, 0x01,
    5, 0x24, 0x01, 0x00, 0x01,
    4, 0x24, 0x02, 0x02,
    5, 0x24, 0x06, 0x00, 0x01,
};
#define ACM_CONFIG_BYTES (sizeof(USB_CONFIGURATION_DESCRIPTOR) + 2 * sizeof(USB_INTERFACE_DESCRIPTOR) + sizeof g_acmFunctional + 3 * sizeof(USB_ENDPOINT_DESCRIPTOR))

static UFN_INTERFACE g_acmHighSpeed[2], g_acmFullSpeed[2];

static void AcmInterfaces(UFN_INTERFACE *out, const UFN_INTERFACE *stock, UFN_ENDPOINT *endpoints, UFN_CONFIGURATION *config)
{
    out[0] = *stock;                    // communications: the interrupt endpoint
    out[0].Descriptor.bInterfaceNumber = 0;
    out[0].Descriptor.bNumEndpoints = 1;
    out[0].Descriptor.bInterfaceClass = 0x02;
    out[0].Descriptor.bInterfaceSubClass = 0x02;
    out[0].Descriptor.bInterfaceProtocol = 0x01;
    out[0].pvExtended = g_acmFunctional;
    out[0].cbExtended = sizeof g_acmFunctional;
    out[0].pEndpoints = &endpoints[INTERRUPT_IN_DESCRIPTOR_INDEX];
    out[1] = *stock;                    // data: bulk in, bulk out
    out[1].Descriptor.bInterfaceNumber = 1;
    out[1].Descriptor.bNumEndpoints = 2;
    out[1].Descriptor.bInterfaceClass = 0x0A;
    out[1].Descriptor.bInterfaceSubClass = 0x00;
    out[1].Descriptor.bInterfaceProtocol = 0x00;
    out[1].pvExtended = NULL;
    out[1].cbExtended = 0;
    out[1].pEndpoints = &endpoints[BULK_IN_DESCRIPTOR_INDEX];
    config->Descriptor.bNumInterfaces = 2;
    config->Descriptor.wTotalLength = (USHORT)ACM_CONFIG_BYTES;
    config->pvExtended = NULL;
    config->cbExtended = 0;
    config->pInterfaces = out;
}

class AcmSerialFn : public USBSerialFn {
public:
    AcmSerialFn(LPTSTR lpActivePath, PVOID pMdd, PHWOBJ pHwObj);
    ~AcmSerialFn();
    virtual BOOL Init();
    virtual CONTROL_RESPONSE HandleClassRequest(USB_DEVICE_REQUEST udr);
    virtual void SetDTR(BOOL) { }
    virtual void SetRTS(BOOL) { }
private:
    BOOL ControlData(DWORD dwFlags, PVOID pvBuffer, DWORD cbBuffer);
    void CloseControl();
    static DWORD WINAPI ControlDoneStub(PVOID pvThis);
    BYTE m_lineCoding[CDC_LINE_CODING_BYTES];      // dwDTERate, bCharFormat, bParityType, bDataBits
    BYTE m_incoming[CDC_LINE_CODING_BYTES];
    UFN_TRANSFER m_hControl;
    BOOL m_fSetting;
};

AcmSerialFn::AcmSerialFn(LPTSTR lpActivePath, PVOID pMdd, PHWOBJ pHwObj)
:   USBSerialFn(lpActivePath, pMdd, pHwObj)
,   m_hControl(NULL)
,   m_fSetting(FALSE)
{
    static const BYTE c_default[CDC_LINE_CODING_BYTES] = { 0x00, 0xC2, 0x01, 0x00, 0, 0, 8 };  // 115200 8N1
    memcpy(m_lineCoding, c_default, sizeof m_lineCoding);
}

AcmSerialFn::~AcmSerialFn()
{
    CloseControl();
}

BOOL AcmSerialFn::Init()
{
    g_HighSpeedDeviceDesc.bDeviceClass = 0x02;
    g_FullSpeedDeviceDesc.bDeviceClass = 0x02;
    AcmInterfaces(g_acmHighSpeed, &g_HighSpeedInterface1, g_HighSpeedEndpoints, &g_HighSpeedConfig1);
    AcmInterfaces(g_acmFullSpeed, &g_FullSpeedInterface1, g_FullSpeedEndpoints, &g_FullSpeedConfig1);
    return USBSerialFn::Init();
}

void AcmSerialFn::CloseControl()
{
    DWORD cb, err;

    if (!m_hControl || !m_pUfnFuncs || !m_hDevice) return;
    if (m_pUfnFuncs->lpGetTransferStatus(m_hDevice, m_hControl, &cb, &err) == ERROR_SUCCESS && err == UFN_NOT_COMPLETE_ERROR) {
        m_pUfnFuncs->lpAbortTransfer(m_hDevice, m_hControl);
    }
    m_pUfnFuncs->lpCloseTransfer(m_hDevice, m_hControl);
    m_hControl = NULL;
}

// The data stage of a class request; the status handshake follows when it is done
BOOL AcmSerialFn::ControlData(DWORD dwFlags, PVOID pvBuffer, DWORD cbBuffer)
{
    CloseControl();                     // the one of the previous request
    return m_pUfnFuncs->lpIssueTransfer(m_hDevice, m_hDefaultPipe, ControlDoneStub, this, dwFlags,
                                        cbBuffer, pvBuffer, 0, NULL, &m_hControl) == ERROR_SUCCESS;
}

DWORD WINAPI AcmSerialFn::ControlDoneStub(PVOID pvThis)
{
    AcmSerialFn *pThis = (AcmSerialFn *)pvThis;
    DWORD cb = 0, err = UFN_NOT_COMPLETE_ERROR;

    if (pThis->m_fSetting && pThis->m_hControl &&
        pThis->m_pUfnFuncs->lpGetTransferStatus(pThis->m_hDevice, pThis->m_hControl, &cb, &err) == ERROR_SUCCESS &&
        err == UFN_NO_ERROR && cb == CDC_LINE_CODING_BYTES) {
        memcpy(pThis->m_lineCoding, pThis->m_incoming, CDC_LINE_CODING_BYTES);
    }
    pThis->m_fSetting = FALSE;
    pThis->m_pUfnFuncs->lpSendControlStatusHandshake(pThis->m_hDevice);
    return ERROR_SUCCESS;
}

CONTROL_RESPONSE AcmSerialFn::HandleClassRequest(USB_DEVICE_REQUEST udr)
{
    if (udr.bmRequestType == CDC_CLASS_OUT) {
        switch (udr.bRequest) {
        case SET_CONTROL_LINE_STATE:
            return USBSerialFn::HandleClassRequest(udr);
        case CDC_SET_LINE_CODING:
            if (udr.wLength != CDC_LINE_CODING_BYTES) break;
            m_fSetting = TRUE;
            if (ControlData(USB_OUT_TRANSFER, m_incoming, CDC_LINE_CODING_BYTES)) return CR_SUCCESS;
            m_fSetting = FALSE;
            break;
        case CDC_SEND_BREAK:
            return CR_SUCCESS_SEND_CONTROL_HANDSHAKE;
        }
    } else if (udr.bmRequestType == CDC_CLASS_IN && udr.bRequest == CDC_GET_LINE_CODING && udr.wLength >= CDC_LINE_CODING_BYTES) {
        if (ControlData(USB_IN_TRANSFER, m_lineCoding, CDC_LINE_CODING_BYTES)) return CR_SUCCESS;
    }
    return CR_STALL_DEFAULT_PIPE;
}

// What the serial MDD (com_mdd2) calls for its PDD
CSerialPDD * CreateSerialObject(LPTSTR lpActivePath, PVOID pMdd, PHWOBJ pHwObj, DWORD)
{
    CSerialPDD *pSerialPDD = new AcmSerialFn(lpActivePath, pMdd, pHwObj);
    if (pSerialPDD && !pSerialPDD->Init()) {
        delete pSerialPDD;
        pSerialPDD = NULL;
    }
    return pSerialPDD;
}
