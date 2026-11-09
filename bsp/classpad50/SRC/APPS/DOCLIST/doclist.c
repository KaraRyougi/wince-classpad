//------------------------------------------------------------------------------
//  doclist.c - DocList.dll for CE 6.0: a stand-in for the Pocket PC shell's
//  document list control, so Pocket PC binaries that import it (MFC CE 3.0's
//  mfcce300.dll, for CCeDocList / CCeDocListDocTemplate) load and work.
//
//  The control is a child window covering its parent's client area: a filter
//  combo box at the top (when there is more than one filter) and a report
//  list view of the files in the folder and its first-level subfolders that
//  match the current filter (Name, Folder, Date, Size). A tap or Enter on an
//  item sends DLN_ITEMACTIVATED with the full path, which is what MFC uses to
//  open the document. Messages and notifications follow the SDK's doclist.h;
//  the shell services the Pocket PC version offered (infrared, e-mail,
//  rename/move dialog, context menu, file change notifications) are not here
//  and answer "not done".
//
//  Children the owner creates inside the control (MFC puts a command bar
//  there) are left alone: the layout keeps the band they cover at the top
//  free.
//------------------------------------------------------------------------------
#include <windows.h>
#include <commctrl.h>
#include <stdlib.h>
#include <string.h>
#include "doclist.h"

#define DL_CLASS            TEXT("DocList")
#define DL_DEFAULT_FOLDER   TEXT("\\My Documents")
#define DL_MAX_FILTERS      16
#define DL_ID_COMBO         100
#define DL_ID_LIST          101
#define WM_SIZEPARENT_MFC   0x0361      // MFC's RepositionBars message (afxpriv.h)
#define DL_PRIV_ACTIVATE    (WM_USER + 700) // wParam = item: activation, posted

enum { COL_NAME, COL_FOLDER, COL_DATE, COL_SIZE, COL_COUNT };

typedef struct {
    TCHAR       path[MAX_PATH];
    int         nameOff;                // file name within path
    int         dirOff;                 // subfolder name within path, -1 = the folder itself
    FILETIME    ft;
    DWORD       size;
} DLITEM;

typedef struct {
    HWND        hwnd, hwndParent, hwndCombo, hwndList;
    WORD        wId;
    DWORD       dwFlags;
    TCHAR       folder[MAX_PATH];
    LPTSTR      filterBuf;              // private copy of pstrFilter
    LPCTSTR     filterDesc[DL_MAX_FILTERS];
    LPCTSTR     filterSpec[DL_MAX_FILTERS];
    int         nFilters;
    int         filterIndex;            // 1-based
    int         sortCol;
    BOOL        sortDesc;
    BOOL        updatesOff, updatePending;
    TCHAR       pendingSel[MAX_PATH];   // DLM_SETSELPATHNAME
} DOCLIST;

static HINSTANCE g_hinst;
static BOOL g_classOk;

//------------------------------------------------------------------------------
// small string helpers (CE 6.0's coredll has no lstrcpyn)

static void CopyStr(LPTSTR dst, LPCTSTR src, int cch)
{
    if (cch <= 0)
        return;
    wcsncpy(dst, src ? src : TEXT(""), cch - 1);
    dst[cch - 1] = 0;
}

static BOOL WildMatch(LPCTSTR pat, int patLen, LPCTSTR s)
{
    // '*' and '?' wildcards, case-insensitive; pat is not NUL-terminated
    int p = 0, star = -1;
    LPCTSTR mark = NULL;
    while (*s) {
        if (p < patLen && (pat[p] == TEXT('?') || towlower(pat[p]) == towlower(*s))) {
            p++; s++;
        } else if (p < patLen && pat[p] == TEXT('*')) {
            star = p++; mark = s;
        } else if (star >= 0) {
            p = star + 1; s = ++mark;
        } else {
            return FALSE;
        }
    }
    while (p < patLen && pat[p] == TEXT('*'))
        p++;
    return p == patLen;
}

static BOOL SpecMatches(LPCTSTR spec, LPCTSTR name)
{
    // "*.mwb;*.txt"; "*.*" also matches names without a dot
    while (spec && *spec) {
        LPCTSTR end = spec;
        int len;
        while (*end && *end != TEXT(';'))
            end++;
        len = (int)(end - spec);
        while (len > 0 && *spec == TEXT(' ')) { spec++; len--; }
        while (len > 0 && spec[len - 1] == TEXT(' ')) len--;
        if (len == 3 && wcsncmp(spec, TEXT("*.*"), 3) == 0)
            return TRUE;
        if (len > 0 && WildMatch(spec, len, name))
            return TRUE;
        spec = *end ? end + 1 : end;
    }
    return FALSE;
}

static BOOL IsWave(const DLITEM *it)
{
    LPCTSTR dot = wcsrchr(it->path + it->nameOff, TEXT('.'));
    return dot && _wcsicmp(dot, TEXT(".wav")) == 0;
}

//------------------------------------------------------------------------------
// notifications to the parent

static LRESULT Notify(DOCLIST *dl, UINT code, int iItem, LPCTSTR path)
{
    DLNHDR h;
    h.nmhdr.hwndFrom = dl->hwnd;
    h.nmhdr.idFrom = dl->wId;
    h.nmhdr.code = code;
    h.iItem = iItem;
    h.pszPath = path;
    return SendMessage(dl->hwndParent, WM_NOTIFY, dl->wId, (LPARAM)&h);
}

//------------------------------------------------------------------------------
// items

static DLITEM *ItemAt(DOCLIST *dl, int i)
{
    LVITEM lvi;
    if (i < 0)
        return NULL;
    memset(&lvi, 0, sizeof(lvi));
    lvi.mask = LVIF_PARAM;
    lvi.iItem = i;
    if (!ListView_GetItem(dl->hwndList, &lvi))
        return NULL;
    return (DLITEM *)lvi.lParam;
}

static int FindPath(DOCLIST *dl, LPCTSTR path)
{
    int i, n = ListView_GetItemCount(dl->hwndList);
    for (i = 0; i < n; i++) {
        DLITEM *it = ItemAt(dl, i);
        if (it && _wcsicmp(it->path, path) == 0)
            return i;
    }
    return -1;
}

static void SelectOnly(DOCLIST *dl, int i, BOOL visible)
{
    ListView_SetItemState(dl->hwndList, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    if (i >= 0) {
        ListView_SetItemState(dl->hwndList, i, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
        if (visible)
            ListView_EnsureVisible(dl->hwndList, i, FALSE);
    }
}

static int CALLBACK CompareItems(LPARAM a, LPARAM b, LPARAM ctx)
{
    const DLITEM *x = (const DLITEM *)a, *y = (const DLITEM *)b;
    const DOCLIST *dl = (const DOCLIST *)ctx;
    int r = 0;
    switch (dl->sortCol) {
    case COL_FOLDER:
        r = _wcsicmp(x->dirOff >= 0 ? x->path + x->dirOff : TEXT(""),
                     y->dirOff >= 0 ? y->path + y->dirOff : TEXT(""));
        break;
    case COL_DATE:
        r = CompareFileTime(&x->ft, &y->ft);
        break;
    case COL_SIZE:
        r = x->size < y->size ? -1 : x->size > y->size;
        break;
    }
    if (r == 0)
        r = _wcsicmp(x->path + x->nameOff, y->path + y->nameOff);
    return dl->sortDesc ? -r : r;
}

static void AddItem(DOCLIST *dl, LPCTSTR dir, LPCTSTR subdir, const WIN32_FIND_DATA *fd)
{
    DLITEM *it;
    TCHAR text[MAX_PATH], *dot;
    LVITEM lvi;
    SYSTEMTIME st;
    FILETIME lft;
    int n, len = (int)wcslen(dir);

    if (len + 1 + (int)wcslen(fd->cFileName) >= MAX_PATH)
        return;
    it = (DLITEM *)LocalAlloc(LPTR, sizeof(DLITEM));
    if (!it)
        return;
    wcscpy(it->path, dir);
    if (len == 0 || it->path[len - 1] != TEXT('\\'))
        it->path[len++] = TEXT('\\');
    it->nameOff = len;
    wcscpy(it->path + len, fd->cFileName);
    it->dirOff = -1;
    if (subdir) {
        // dir ends with "\subdir"
        int sl = (int)wcslen(subdir);
        if (sl < len)
            it->dirOff = len - 1 - sl;
    }
    it->ft = fd->ftLastWriteTime;
    it->size = fd->nFileSizeLow;

    CopyStr(text, fd->cFileName, MAX_PATH);
    if (!(dl->dwFlags & DLF_SHOWEXTENSION) && (dot = wcsrchr(text, TEXT('.'))) != NULL && dot != text)
        *dot = 0;
    memset(&lvi, 0, sizeof(lvi));
    lvi.mask = LVIF_TEXT | LVIF_PARAM;
    lvi.iItem = ListView_GetItemCount(dl->hwndList);
    lvi.pszText = text;
    lvi.lParam = (LPARAM)it;
    n = ListView_InsertItem(dl->hwndList, &lvi);
    if (n < 0) {
        LocalFree(it);
        return;
    }
    if (it->dirOff >= 0) {
        CopyStr(text, it->path + it->dirOff, MAX_PATH);
        text[it->nameOff - 1 - it->dirOff] = 0;
        ListView_SetItemText(dl->hwndList, n, COL_FOLDER, text);
    }
    if (FileTimeToLocalFileTime(&it->ft, &lft) && FileTimeToSystemTime(&lft, &st)) {
        wsprintf(text, TEXT("%04u-%02u-%02u %02u:%02u"),
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
        ListView_SetItemText(dl->hwndList, n, COL_DATE, text);
    }
    if (it->size < 1024)
        wsprintf(text, TEXT("%u B"), it->size);
    else
        wsprintf(text, TEXT("%u K"), (it->size + 1023) / 1024);
    ListView_SetItemText(dl->hwndList, n, COL_SIZE, text);
}

static void ScanDir(DOCLIST *dl, LPCTSTR dir, LPCTSTR subdir, LPCTSTR spec, BOOL recurse)
{
    TCHAR pattern[MAX_PATH], sub[MAX_PATH];
    WIN32_FIND_DATA fd;
    HANDLE h;
    int len = (int)wcslen(dir);

    if (len + 5 >= MAX_PATH)
        return;
    wcscpy(pattern, dir);
    if (len == 0 || pattern[len - 1] != TEXT('\\'))
        pattern[len++] = TEXT('\\');
    wcscpy(pattern + len, TEXT("*"));
    h = FindFirstFile(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!recurse || fd.cFileName[0] == TEXT('.'))
                continue;
            if (len + (int)wcslen(fd.cFileName) >= MAX_PATH)
                continue;
            CopyStr(sub, pattern, MAX_PATH);
            wcscpy(sub + len, fd.cFileName);
            ScanDir(dl, sub, sub + len, spec, FALSE);
        } else if (SpecMatches(spec, fd.cFileName)) {
            AddItem(dl, dir, subdir, &fd);
        }
    } while (FindNextFile(h, &fd));
    FindClose(h);
}

static void Populate(DOCLIST *dl, BOOL keepSel)
{
    TCHAR sel[MAX_PATH];
    LPCTSTR spec = TEXT("*.*");
    int i;

    if (dl->updatesOff) {
        dl->updatePending = TRUE;
        return;
    }
    dl->updatePending = FALSE;
    if (Notify(dl, DLN_PREUPDATE, -1, NULL))
        return;

    sel[0] = 0;
    if (dl->pendingSel[0]) {
        CopyStr(sel, dl->pendingSel, MAX_PATH);
        dl->pendingSel[0] = 0;
    } else if (keepSel) {
        DLITEM *it = ItemAt(dl, ListView_GetNextItem(dl->hwndList, -1, LVNI_SELECTED));
        if (it)
            CopyStr(sel, it->path, MAX_PATH);
    }

    if (dl->nFilters > 0 && dl->filterIndex >= 1 && dl->filterIndex <= dl->nFilters)
        spec = dl->filterSpec[dl->filterIndex - 1];

    SendMessage(dl->hwndList, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(dl->hwndList);
    ScanDir(dl, dl->folder, NULL, spec, TRUE);
    ListView_SortItems(dl->hwndList, CompareItems, (LPARAM)dl);
    SendMessage(dl->hwndList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(dl->hwndList, NULL, TRUE);

    i = sel[0] ? FindPath(dl, sel) : -1;
    if (i >= 0) {
        SelectOnly(dl, i, TRUE);
    } else if (ListView_GetItemCount(dl->hwndList) > 0) {
        // ListView_SetItemState is a { } block in commctrl.h: keep it braced
        ListView_SetItemState(dl->hwndList, 0, LVIS_FOCUSED, LVIS_FOCUSED);
    }

    Notify(dl, DLN_POSTUPDATE, -1, NULL);
    if (ListView_GetItemCount(dl->hwndList) == 0)
        Notify(dl, DLN_NOITEMS, -1, NULL);
}

//------------------------------------------------------------------------------
// filters and layout

static void ParseFilters(DOCLIST *dl, LPCTSTR filter)
{
    // pairs of NUL-terminated strings ending with an empty string
    LPCTSTR p;
    int total;

    dl->nFilters = 0;
    if (!filter || !*filter)
        return;
    for (p = filter; *p && (p - filter) < 4096; p += wcslen(p) + 1)
        ;
    total = (int)(p - filter) + 1;
    dl->filterBuf = (LPTSTR)LocalAlloc(LPTR, total * sizeof(TCHAR));
    if (!dl->filterBuf)
        return;
    memcpy(dl->filterBuf, filter, total * sizeof(TCHAR));
    dl->filterBuf[total - 1] = 0;
    for (p = dl->filterBuf; *p && dl->nFilters < DL_MAX_FILTERS; ) {
        LPCTSTR desc = p, spec;
        p += wcslen(p) + 1;
        if (!*p)
            break;
        spec = p;
        p += wcslen(p) + 1;
        dl->filterDesc[dl->nFilters] = desc;
        dl->filterSpec[dl->nFilters] = spec;
        dl->nFilters++;
    }
}

static void Layout(DOCLIST *dl)
{
    RECT rc, rw;
    HWND child;
    int top = 0, w, h, cy, sb, nameW;
    LVCOLUMN col;

    if (!dl->hwndList)
        return;         // WM_SIZE during creation
    GetClientRect(dl->hwnd, &rc);
    w = rc.right;
    h = rc.bottom;

    // leave room for children the owner put at the top (MFC's command bar)
    for (child = GetWindow(dl->hwnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        if (child == dl->hwndCombo || child == dl->hwndList || !IsWindowVisible(child))
            continue;
        GetWindowRect(child, &rw);
        MapWindowPoints(NULL, dl->hwnd, (LPPOINT)&rw, 2);
        if (rw.top <= 0 && rw.bottom > top && rw.bottom < h / 2)
            top = rw.bottom;
    }

    if (dl->hwndCombo) {
        if (dl->nFilters > 1) {
            MoveWindow(dl->hwndCombo, 0, top, w, 160, TRUE);
            GetWindowRect(dl->hwndCombo, &rw);
            cy = rw.bottom - rw.top;
            if (cy > 40)    // the rect included the drop-down part
                cy = (int)SendMessage(dl->hwndCombo, CB_GETITEMHEIGHT, (WPARAM)-1, 0) + 6;
            top += cy;
            ShowWindow(dl->hwndCombo, SW_SHOW);
        } else {
            ShowWindow(dl->hwndCombo, SW_HIDE);
        }
    }
    MoveWindow(dl->hwndList, 0, top, w, h > top ? h - top : 0, TRUE);

    sb = GetSystemMetrics(SM_CXVSCROLL);
    w -= sb + 2;
    if (w < 120)
        w = 120;
    nameW = w - (w * 18) / 100 - (w * 30) / 100 - (w * 14) / 100;
    memset(&col, 0, sizeof(col));
    col.mask = LVCF_WIDTH;
    col.cx = nameW;                 ListView_SetColumn(dl->hwndList, COL_NAME, &col);
    col.cx = (w * 18) / 100;        ListView_SetColumn(dl->hwndList, COL_FOLDER, &col);
    col.cx = (w * 30) / 100;        ListView_SetColumn(dl->hwndList, COL_DATE, &col);
    col.cx = (w * 14) / 100;        ListView_SetColumn(dl->hwndList, COL_SIZE, &col);
}

static void FitParent(DOCLIST *dl)
{
    RECT rp, rc;
    GetClientRect(dl->hwndParent, &rp);
    GetWindowRect(dl->hwnd, &rc);
    MapWindowPoints(NULL, dl->hwndParent, (LPPOINT)&rc, 2);
    if (!EqualRect(&rp, &rc))
        MoveWindow(dl->hwnd, rp.left, rp.top, rp.right - rp.left, rp.bottom - rp.top, TRUE);
    else
        Layout(dl);
}

static void SetFilterIndex(DOCLIST *dl, int index)
{
    dl->filterIndex = index;
    if (dl->hwndCombo && dl->nFilters > 0)
        SendMessage(dl->hwndCombo, CB_SETCURSEL, index - 1, 0);
}

//------------------------------------------------------------------------------
// actions

static void Activate(DOCLIST *dl, int i)
{
    // MFC's handler opens the document and then destroys this window, so
    // nothing of dl may be touched after the notification returns, and it
    // runs from a posted message, not inside the list view's own notification
    TCHAR path[MAX_PATH];
    DLITEM *it = ItemAt(dl, i);
    if (!it)
        return;
    CopyStr(path, it->path, MAX_PATH);
    Notify(dl, DLN_ITEMACTIVATED, i, path);
}

static LRESULT DeleteSelection(DOCLIST *dl)
{
    TCHAR path[MAX_PATH];
    int i, n = ListView_GetSelectedCount(dl->hwndList);

    if (n == 0)
        return S_FALSE;
    if (MessageBox(dl->hwnd, n == 1 ? TEXT("Delete the selected item?") : TEXT("Delete the selected items?"),
                   TEXT("Delete"), MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
        return S_FALSE;
    if (Notify(dl, DLN_PREDELETESEL, -1, NULL))
        return S_FALSE;
    for (i = ListView_GetNextItem(dl->hwndList, -1, LVNI_SELECTED); i >= 0;
         i = ListView_GetNextItem(dl->hwndList, i, LVNI_SELECTED)) {
        DLITEM *it = ItemAt(dl, i);
        if (!it)
            continue;
        CopyStr(path, it->path, MAX_PATH);
        Notify(dl, DLN_DELETINGSEL, i, path);
        DeleteFile(path);
    }
    Populate(dl, FALSE);
    return S_OK;
}

static BOOL NextWave(DOCLIST *dl, int *pItem, int step)
{
    int n = ListView_GetItemCount(dl->hwndList), i;
    if (!pItem)
        return FALSE;
    i = *pItem;
    if (i < 0)
        i = step > 0 ? -1 : n;
    for (i += step; i >= 0 && i < n; i += step) {
        DLITEM *it = ItemAt(dl, i);
        if (it && IsWave(it)) {
            *pItem = i;
            SelectOnly(dl, i, TRUE);
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL SelectedWave(HWND hwnd, int *pIndex, LPTSTR szPath, size_t cchPath, BOOL first)
{
    DOCLIST *dl = hwnd ? (DOCLIST *)GetWindowLong(hwnd, 0) : NULL;
    int i;
    if (!dl || !pIndex)
        return FALSE;
    for (i = ListView_GetNextItem(dl->hwndList, first ? -1 : *pIndex, LVNI_SELECTED); i >= 0;
         i = ListView_GetNextItem(dl->hwndList, i, LVNI_SELECTED)) {
        DLITEM *it = ItemAt(dl, i);
        if (it && IsWave(it)) {
            *pIndex = i;
            if (szPath)
                CopyStr(szPath, it->path, (int)cchPath);
            return TRUE;
        }
    }
    return FALSE;
}

//------------------------------------------------------------------------------
// window

static LRESULT OnListNotify(DOCLIST *dl, NMHDR *nm)
{
    switch (nm->code) {
    case NM_CLICK: {
        LVHITTESTINFO ht;
        DWORD pos = GetMessagePos();
        memset(&ht, 0, sizeof(ht));
        ht.pt.x = (short)LOWORD(pos);
        ht.pt.y = (short)HIWORD(pos);
        ScreenToClient(dl->hwndList, &ht.pt);
        if (ListView_HitTest(dl->hwndList, &ht) >= 0 && (ht.flags & LVHT_ONITEM))
            PostMessage(dl->hwnd, DL_PRIV_ACTIVATE, ht.iItem, 0);
        else
            Notify(dl, DLN_NOHIT, -1, NULL);
        return 0;
    }
    case LVN_KEYDOWN: {
        NMLVKEYDOWN *kd = (NMLVKEYDOWN *)nm;
        if (kd->wVKey == VK_RETURN)
            PostMessage(dl->hwnd, DL_PRIV_ACTIVATE,
                        ListView_GetNextItem(dl->hwndList, -1, LVNI_FOCUSED), 0);
        else if (kd->wVKey == VK_ESCAPE)
            Notify(dl, DLN_ESCAPE, -1, NULL);
        return 0;
    }
    case LVN_ITEMCHANGED: {
        NMLISTVIEW *lv = (NMLISTVIEW *)nm;
        if ((lv->uChanged & LVIF_STATE) && ((lv->uNewState ^ lv->uOldState) & LVIS_SELECTED)
            && (lv->uNewState & LVIS_SELECTED)) {
            DLITEM *it = (DLITEM *)lv->lParam;
            Notify(dl, DLN_ITEMCHANGED, lv->iItem, it ? it->path : NULL);
        }
        return 0;
    }
    case LVN_COLUMNCLICK: {
        NMLISTVIEW *lv = (NMLISTVIEW *)nm;
        if (lv->iSubItem == dl->sortCol)
            dl->sortDesc = !dl->sortDesc;
        else {
            dl->sortCol = lv->iSubItem;
            dl->sortDesc = lv->iSubItem == COL_DATE;    // newest first
        }
        ListView_SortItems(dl->hwndList, CompareItems, (LPARAM)dl);
        return 0;
    }
    case LVN_DELETEITEM: {
        NMLISTVIEW *lv = (NMLISTVIEW *)nm;
        if (lv->lParam)
            LocalFree((HLOCAL)lv->lParam);
        return 0;
    }
    case LVN_DELETEALLITEMS:
        return FALSE;       // keep the per-item LVN_DELETEITEM (frees the items)
    }
    return 0;
}

static LRESULT CALLBACK DocListWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    DOCLIST *dl = (DOCLIST *)GetWindowLong(hwnd, 0);

    if (msg == WM_CREATE) {
        CREATESTRUCT *cs = (CREATESTRUCT *)lParam;
        dl = (DOCLIST *)cs->lpCreateParams;
        dl->hwnd = hwnd;
        SetWindowLong(hwnd, 0, (LONG)dl);
        return 0;
    }
    if (!dl)
        return DefWindowProc(hwnd, msg, wParam, lParam);

    // the list view messages the SDK maps (DLM_GETNEXTITEM, DLM_SETITEMSTATE) and the rest
    if (msg >= LVM_FIRST && msg < LVM_FIRST + 0x100)
        return SendMessage(dl->hwndList, msg, wParam, lParam);

    switch (msg) {
    case WM_SIZE:
        Layout(dl);
        return 0;
    case WM_SIZEPARENT_MFC:
        FitParent(dl);
        return 0;
    case WM_SETFOCUS:
        SetFocus(dl->hwndList);
        return 0;
    case WM_NOTIFY:
        if (((NMHDR *)lParam)->hwndFrom == dl->hwndList)
            return OnListNotify(dl, (NMHDR *)lParam);
        break;
    case WM_COMMAND:
        if (LOWORD(wParam) == DL_ID_COMBO && HIWORD(wParam) == CBN_SELCHANGE) {
            int sel = (int)SendMessage(dl->hwndCombo, CB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel + 1 != dl->filterIndex) {
                dl->filterIndex = sel + 1;
                Populate(dl, TRUE);
            }
            return 0;
        }
        if ((HWND)lParam == dl->hwndCombo || (HWND)lParam == dl->hwndList)
            return 0;
        // commands from the owner's children (MFC's command bar) go to the owner
        return SendMessage(dl->hwndParent, msg, wParam, lParam);
    case DL_PRIV_ACTIVATE:
        Activate(dl, (int)wParam);
        return 0;
    case WM_DESTROY:
        ListView_DeleteAllItems(dl->hwndList);
        SetWindowLong(hwnd, 0, 0);
        if (dl->filterBuf)
            LocalFree(dl->filterBuf);
        LocalFree(dl);
        return 0;

    case DLM_REFRESH:
        Layout(dl);
        Populate(dl, TRUE);
        return S_OK;
    case DLM_UPDATE:
        Layout(dl);
        Populate(dl, FALSE);
        return S_OK;
    case DLM_SETFILTERINDEX:
        if (dl->nFilters > 0 && ((int)wParam < 1 || (int)wParam > dl->nFilters))
            return E_INVALIDARG;
        SetFilterIndex(dl, (int)wParam);
        Layout(dl);
        Populate(dl, TRUE);
        return S_OK;
    case DLM_GETFILTERINDEX:
        return dl->filterIndex;
    case DLM_DISABLEUPDATES:
        dl->updatesOff = TRUE;
        return 0;
    case DLM_ENABLEUPDATES:
        dl->updatesOff = FALSE;
        if (dl->updatePending)
            Populate(dl, TRUE);
        return 0;
    case DLM_SETSORTORDER:
        return ListView_SortItems(dl->hwndList, CompareItems, (LPARAM)dl);
    case DLM_GETSELPATHNAME: {
        DLITEM *it = ItemAt(dl, ListView_GetNextItem(dl->hwndList, -1, LVNI_SELECTED));
        if (!it || !lParam)
            return FALSE;
        CopyStr((LPTSTR)lParam, it->path, (int)wParam);
        return TRUE;
    }
    case DLM_SETSELPATHNAME:
        CopyStr(dl->pendingSel, (LPCTSTR)lParam, MAX_PATH);
        return 0;
    case DLM_SELECTITEM: {
        int i = lParam ? FindPath(dl, (LPCTSTR)lParam) : -1;
        if (i < 0)
            return E_FAIL;
        SelectOnly(dl, i, (BOOL)wParam);
        return S_OK;
    }
    case DLM_SETSELECT:
        SelectOnly(dl, (int)wParam, TRUE);
        return 0;
    case DLM_SETONEITEM:
        return E_NOTIMPL;
    case DLM_GETNEXTWAVE:
        return NextWave(dl, (int *)lParam, 1);
    case DLM_GETPREVWAVE:
        return NextWave(dl, (int *)lParam, -1);
    case DLM_SETFOLDER:
        CopyStr(dl->folder, lParam && *(LPCTSTR)lParam ? (LPCTSTR)lParam : DL_DEFAULT_FOLDER, MAX_PATH);
        Populate(dl, FALSE);
        return 0;
    case DLM_GETSELCOUNT:
        return ListView_GetSelectedCount(dl->hwndList);
    case DLM_GETITEMCOUNT:
        return ListView_GetItemCount(dl->hwndList);
    case DLM_SELECTALL:
        ListView_SetItemState(dl->hwndList, -1, LVIS_SELECTED, LVIS_SELECTED);
        return 0;
    case DLM_DELETESEL:
        return DeleteSelection(dl);
    case DLM_VALIDATEFOLDER: {
        DWORD a = GetFileAttributes(dl->folder);
        if (a != 0xFFFFFFFF && (a & FILE_ATTRIBUTE_DIRECTORY))
            return TRUE;
        CopyStr(dl->folder, DL_DEFAULT_FOLDER, MAX_PATH);
        Populate(dl, FALSE);
        return FALSE;
    }
    case DLM_RENAMEMOVE:            // no rename/move dialog
        return FALSE;
    case DLM_SENDIR:                // no infrared or mail here
    case DLM_RECEIVEIR:
    case DLM_SENDEMAIL:
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

static BOOL RegisterDocListClass(void)
{
    WNDCLASS wc;
    if (g_classOk)
        return TRUE;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = DocListWndProc;
    wc.cbWndExtra = sizeof(LONG);
    wc.hInstance = g_hinst;
    wc.hCursor = NULL;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = DL_CLASS;
    if (!RegisterClass(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return FALSE;
    g_classOk = TRUE;
    return TRUE;
}

//------------------------------------------------------------------------------
// exports

HWND DocList_Create(PDOCLISTCREATE pDLC)
{
    INITCOMMONCONTROLSEX icc;
    DOCLIST *dl;
    RECT rc;
    HWND hwnd;
    LVCOLUMN col;
    int i;
    static const LPCTSTR titles[COL_COUNT] = {
        TEXT("Name"), TEXT("Folder"), TEXT("Date"), TEXT("Size")
    };

    if (!pDLC || pDLC->dwStructSize < sizeof(DOCLISTCREATE) || !IsWindow(pDLC->hwndParent)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);
    if (!RegisterDocListClass())
        return NULL;

    dl = (DOCLIST *)LocalAlloc(LPTR, sizeof(DOCLIST));
    if (!dl) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    dl->hwndParent = pDLC->hwndParent;
    dl->wId = pDLC->wId;
    dl->dwFlags = pDLC->dwFlags;
    CopyStr(dl->folder, pDLC->pszFolder && *pDLC->pszFolder ? pDLC->pszFolder : DL_DEFAULT_FOLDER, MAX_PATH);
    ParseFilters(dl, pDLC->pstrFilter);
    dl->filterIndex = pDLC->wFilterIndex;
    if (dl->nFilters > 0 && (dl->filterIndex < 1 || dl->filterIndex > dl->nFilters))
        dl->filterIndex = 1;
    dl->sortCol = COL_NAME;

    GetClientRect(pDLC->hwndParent, &rc);
    hwnd = CreateWindowEx(0, DL_CLASS, TEXT(""), WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                          0, 0, rc.right, rc.bottom, pDLC->hwndParent,
                          (HMENU)(UINT)pDLC->wId, g_hinst, dl);
    if (!hwnd) {
        if (dl->filterBuf)
            LocalFree(dl->filterBuf);
        LocalFree(dl);
        return NULL;
    }
    // from here on WM_DESTROY frees dl

    dl->hwndCombo = CreateWindowEx(0, TEXT("COMBOBOX"), TEXT(""),
                                   WS_CHILD | WS_VSCROLL | WS_TABSTOP | CBS_DROPDOWNLIST,
                                   0, 0, rc.right, 160, hwnd, (HMENU)DL_ID_COMBO, g_hinst, NULL);
    for (i = 0; dl->hwndCombo && i < dl->nFilters; i++)
        SendMessage(dl->hwndCombo, CB_ADDSTRING, 0, (LPARAM)dl->filterDesc[i]);

    dl->hwndList = CreateWindowEx(0, WC_LISTVIEW, TEXT(""),
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS,
                                  0, 0, rc.right, rc.bottom, hwnd, (HMENU)DL_ID_LIST, g_hinst, NULL);
    if (!dl->hwndList) {
        DestroyWindow(hwnd);
        return NULL;
    }
    ListView_SetExtendedListViewStyle(dl->hwndList, LVS_EX_FULLROWSELECT);
    memset(&col, 0, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT | LVCF_SUBITEM;
    for (i = 0; i < COL_COUNT; i++) {
        col.fmt = i == COL_SIZE ? LVCFMT_RIGHT : LVCFMT_LEFT;
        col.cx = 60;
        col.pszText = (LPTSTR)titles[i];
        col.iSubItem = i;
        ListView_InsertColumn(dl->hwndList, i, &col);
    }

    SetFilterIndex(dl, dl->filterIndex);
    SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    Layout(dl);
    Populate(dl, FALSE);
    return hwnd;
}

BOOL DocList_GetFirstSelectedWaveFile(HWND hwnd, int *pIndex, LPTSTR szPath, const size_t cchPath)
{
    return SelectedWave(hwnd, pIndex, szPath, cchPath, TRUE);
}

BOOL DocList_GetNextSelectedWaveFile(HWND hwnd, int *pIndex, LPTSTR szPath, const size_t cchPath)
{
    return SelectedWave(hwnd, pIndex, szPath, cchPath, FALSE);
}

BOOL WINAPI DllMain(HANDLE hinst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_hinst = (HINSTANCE)hinst;
        DisableThreadLibraryCalls((HMODULE)hinst);
    } else if (reason == DLL_PROCESS_DETACH && g_classOk) {
        UnregisterClass(DL_CLASS, g_hinst);
        g_classOk = FALSE;
    }
    return TRUE;
}
