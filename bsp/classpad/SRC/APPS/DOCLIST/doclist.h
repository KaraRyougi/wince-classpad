//------------------------------------------------------------------------------
//  doclist.h - the Pocket PC DocList control's interface (DocList.dll), as
//  the Pocket PC 2000 SDK defines it: the creation structure, the DLM_*
//  messages and the DLN_* notifications. CE 6.0 has no DocList, but MFC CE
//  3.0 (mfcce300.dll, CCeDocList) imports DocList_Create by name, so the
//  loader refuses mfcce300.dll without a DocList.dll. Values must stay as
//  the SDK has them: Pocket PC binaries carry them compiled in.
//------------------------------------------------------------------------------
#ifndef CLASSPAD_DOCLIST_H
#define CLASSPAD_DOCLIST_H

#include <windows.h>
#include <commctrl.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tagDLC {
    DWORD   dwStructSize;       // sizeof(DOCLISTCREATE)
    HWND    hwndParent;
    LPCTSTR pszFolder;          // folder to list; NULL or "" = \My Documents
    LPCTSTR pstrFilter;         // "desc\0pattern\0...\0\0", patterns split by ';'
    WORD    wFilterIndex;       // 1-based
    WORD    wId;                // control id (idFrom of the notifications)
    DWORD   dwFlags;            // DLF_*
} DOCLISTCREATE, *PDOCLISTCREATE;

typedef struct tagDLNHDR {
    NMHDR   nmhdr;
    int     iItem;
    LPCTSTR pszPath;
} DLNHDR, *PDLNHDR;

HWND DocList_Create(PDOCLISTCREATE pDLC);
BOOL DocList_GetFirstSelectedWaveFile(HWND hwnd, int *pIndex, LPTSTR szPath, const size_t cchPath);
BOOL DocList_GetNextSelectedWaveFile(HWND hwnd, int *pIndex, LPTSTR szPath, const size_t cchPath);

#define DLF_SHOWEXTENSION   0x0001

// application -> DocList
#define DLM_REFRESH         (WM_USER + 600)
#define DLM_SETFILTERINDEX  (WM_USER + 601)
#define DLM_UPDATE          (WM_USER + 602)
#define DLM_DISABLEUPDATES  (WM_USER + 603)
#define DLM_ENABLEUPDATES   (WM_USER + 604)
#define DLM_SETSORTORDER    (WM_USER + 605)
#define DLM_GETSELPATHNAME  (WM_USER + 606)
#define DLM_SELECTITEM      (WM_USER + 607)
#define DLM_SETSELECT       (WM_USER + 608)
#define DLM_SETONEITEM      (WM_USER + 609)
#define DLM_GETNEXTWAVE     (WM_USER + 610)
#define DLM_GETPREVWAVE     (WM_USER + 611)
#define DLM_SETFOLDER       (WM_USER + 612)
#define DLM_GETSELCOUNT     (WM_USER + 613)
#define DLM_GETFILTERINDEX  (WM_USER + 614)
#define DLM_SETSELPATHNAME  (WM_USER + 615)
#define DLM_RENAMEMOVE      (WM_USER + 616)
#define DLM_SENDIR          (WM_USER + 617)
#define DLM_SENDEMAIL       (WM_USER + 618)
#define DLM_DELETESEL       (WM_USER + 619)
#define DLM_RECEIVEIR       (WM_USER + 620)
#define DLM_GETITEMCOUNT    (WM_USER + 621)
#define DLM_SELECTALL       (WM_USER + 622)
#define DLM_VALIDATEFOLDER  (WM_USER + 623)
#define DLM_GETNEXTITEM     LVM_GETNEXTITEM
#define DLM_SETITEMSTATE    LVM_SETITEMSTATE

// DocList -> parent (WM_NOTIFY, lParam = DLNHDR *)
#define DLN_START               11000
#define DLN_ITEMACTIVATED       (DLN_START + 0)
#define DLN_NOITEMS             (DLN_START + 1)
#define DLN_ESCAPE              (DLN_START + 2)
#define DLN_UP                  (DLN_START + 3)
#define DLN_DOWN                (DLN_START + 4)
#define DLN_PREDELETESEL        (DLN_START + 5)
#define DLN_DELETINGSEL         (DLN_START + 6)
#define DLN_ITEMCHANGING        (DLN_START + 7)
#define DLN_ITEMCHANGED         (DLN_START + 8)
#define DLN_PRINT               (DLN_START + 9)
#define DLN_COPY                (DLN_START + 10)
#define DLN_PROP                (DLN_START + 11)
#define DLN_FOLDER              (DLN_START + 12)
#define DLN_NOHIT               (DLN_START + 13)
#define DLN_PREUPDATE           (DLN_START + 14)
#define DLN_POSTUPDATE          (DLN_START + 15)
#define DLN_FILECHANGEINFO      (DLN_START + 16)

#ifdef __cplusplus
}
#endif

#endif // CLASSPAD_DOCLIST_H
