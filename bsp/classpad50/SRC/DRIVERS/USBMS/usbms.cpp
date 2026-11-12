//------------------------------------------------------------------------------
//  usbms.cpp - the USB mass storage function with a name
//
//  CE 5.0's mass storage function client (usbmsfn.dll) answers the SCSI
//  INQUIRY with 36 bytes of zeros and the removable bit: no vendor, no
//  product, no version, so a host shows the drive without a name. This DLL
//  (classpad_usbms.dll) is the same client with the standard inquiry data
//  filled in:
//      vendor   "CASIO   "             (8 characters)
//      product  "ClassPad WinCE  "     (16)
//      revision "1.00"                 (4)
//  and the fields in front of them as a SCSI-2 direct access device gives
//  them (ANSI version 2, response data format 2, additional length 31).
//  It also lets the host eject the drive: after START STOP UNIT (eject) the
//  unit reports "no medium" until it is enumerated again (the stock client
//  stays ready, so the volume came back a moment after it was ejected).
//
//  The client itself is Microsoft's. Its transport (bulk-only, bot.cpp) is
//  linked as the library Platform Builder ships (usbmsfn_lib.lib); its SCSI
//  emulation (block.cpp) is compiled from where Platform Builder keeps it
//  (PUBLIC\COMMON\OAK\DRIVERS\USBFN\CLASS\STORAGE, on the include path, see
//  sources), not copied here, with the one function the transport calls for
//  every command wrapped below.
//
//  Registry: [HKLM\Drivers\USB\FunctionDrivers\Mass_Storage_Class] "Dll" in
//  platform.reg; the stock usbmsfn.dll is left out of the image
//  (romexclude.txt).
//------------------------------------------------------------------------------
#define STORE_ExecuteCommand StockStoreExecuteCommand      // the emulation's own; ours wraps it
#include <block.cpp>
#undef STORE_ExecuteCommand

#define INQUIRY_BYTES   36

// Ejected by the host (START STOP UNIT with LoEj and without Start): from
// then on the unit has no medium, which is what a host asks about every
// second or so (TEST UNIT READY) to decide whether to mount it. The stock
// emulation answers "ready" for ever, so a volume ejected on the computer
// came straight back. The medium returns with the next INQUIRY, which a
// host sends when it enumerates the device (cable out and in again, or the
// function started again), or when the host loads it (LoEj with Start).
static BOOL g_fEjected;

DWORD STORE_ExecuteCommand(PTRANSPORT_COMMAND ptcCommand, PTRANSPORT_DATA ptdData)
{
    static const char c_name[] = "CASIO   " "ClassPad WinCE  " "1.00";
    PBYTE pbCommand = (ptcCommand && ptcCommand->CommandBlock) ? (PBYTE)ptcCommand->CommandBlock : NULL;
    BYTE bOpCode = pbCommand ? pbCommand[0] : 0xFF;
    DWORD dwResult;
    PBYTE pbData;

    if (bOpCode == SCSI_INQUIRY) g_fEjected = FALSE;
    if (bOpCode == SCSI_START_STOP && (pbCommand[4] & 0x02)) g_fEjected = (pbCommand[4] & 0x01) ? FALSE : TRUE;

    if (g_fEjected) {
        switch (bOpCode) {
        case SCSI_TEST_UNIT_READY:
        case SCSI_READ10:
        case SCSI_WRITE10:
        case SCSI_READ_CAPACITY:
        case SCSI_VERIFY:
            return EXECUTE_FAIL;                // the host asks why: REQUEST SENSE below
        case SCSI_REQUEST_SENSE:
            dwResult = StockStoreExecuteCommand(ptcCommand, ptdData);
            if (dwResult == EXECUTE_PASS && ptdData && ptdData->DataBlock && ptdData->TransferLength >= sizeof(SENSE_DATA)) {
                PSENSE_DATA pSense = (PSENSE_DATA)ptdData->DataBlock;
                pSense->SenseKey = 0x02;                    // NOT READY
                pSense->AdditionalSenseCode = 0x3A;         // MEDIUM NOT PRESENT
                pSense->AdditionalSenseCodeQualifier = 0;
            }
            return dwResult;
        }
    }

    dwResult = StockStoreExecuteCommand(ptcCommand, ptdData);
    if (dwResult == EXECUTE_PASS && bOpCode == SCSI_INQUIRY && ptdData && ptdData->DataBlock && ptdData->TransferLength >= INQUIRY_BYTES) {
        pbData = (PBYTE)ptdData->DataBlock;
        pbData[2] = 0x02;                       // ANSI version: SCSI-2
        pbData[3] = 0x02;                       // response data format
        pbData[4] = INQUIRY_BYTES - 5;          // additional length
        memcpy(pbData + 8, c_name, 28);         // vendor, product, revision
    }
    return dwResult;
}
