// ITaskMgr.cpp
//

// ClassPad (Windows CE 5.0 under the translator): changes from upstream 1.0
// are marked "ClassPad"; README.md lists them.

#include "stdafx.h"
#include "ITaskMgr.h"

#include "cpu.h"
#include "process.h"
#include "tasklist.h"

// ClassPad: the dialog is as wide as the 320x528 screen is in portrait and as
// tall as the work area allows, up to MAIN_DLG_CY (upstream: 240 x 240).
#define MAIN_DLG_CX 320
#define MAIN_DLG_CY 300

static BOOL CALLBACK DlgProc(HWND hDlg, UINT Msg, WPARAM wParam, LPARAM lParam);
static BOOL CALLBACK DlgProcHelp(HWND hDlg, UINT Msg, WPARAM wParam, LPARAM lParam);

static BOOL CreateTab(ThreadPack* pTP);
static BOOL CreateIcon(ThreadPack* pTP);
static void PlaceWindow(HWND hDlg);

static void Measure(ThreadPack* pTP);

HINSTANCE	g_hInst;

//-----------------------------------------------------------------------------
// WinMain entry point
//-----------------------------------------------------------------------------
int WINAPI WinMain(	HINSTANCE hInstance,
					HINSTANCE hPrevInstance,
					LPTSTR    lpCmdLine,
					int       nCmdShow)
{
	INITCOMMONCONTROLSEX icex;

	// Ensure that the common control DLL is loaded. 
	icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
	icex.dwICC  = ICC_LISTVIEW_CLASSES|ICC_TAB_CLASSES|ICC_LISTVIEW_CLASSES;
	InitCommonControlsEx(&icex); 

	g_hInst = hInstance;

	//------------ Prevent multiple instance  ------------------
	HANDLE hMutex = CreateMutex(NULL,FALSE,APPNAME);

	if( GetLastError() == ERROR_ALREADY_EXISTS )
	{
		HWND hwndPrev = FindWindow( L"Dialog" , APPNAME );
		if( hwndPrev )
		{
			ShowWindow(hwndPrev, SW_SHOWNORMAL);
			SetForegroundWindow(hwndPrev);
		}
		return 0; 
	}


	// g_hInst = hInstance;
	DialogBox(hInstance, MAKEINTRESOURCE(IDD_MAINDLG), NULL, (DLGPROC)DlgProc);
    return 0;
}

//-----------------------------------------------------------------------------
// main dialog
//-----------------------------------------------------------------------------
static BOOL CALLBACK DlgProc(HWND hDlg, UINT Msg, WPARAM wParam, LPARAM lParam)
{
	static ThreadPack* pTP = NULL;
	LPNMHDR lpnmhdr;
	
	switch(Msg)
	{
	// ----------------------------------------------------------
	case WM_INITDIALOG:
	{
		if( pTP == NULL )
		pTP = (ThreadPack*)LocalAlloc(LPTR, sizeof(ThreadPack));
		if( pTP == NULL )
		{
			EndDialog(hDlg, 0);
			return FALSE;
		}
		memset(pTP, 0, sizeof(ThreadPack));

		pTP->hDlg = hDlg;
		pTP->dwInterval = 2000; //sec
		pTP->g_hInst = g_hInst;

		memset(pTP->chPowHistory, -1, HISTORY_MAX);
		pTP->chPowHistory[0] = 0;
		memset(pTP->chMemHistory, -1, HISTORY_MAX);
		pTP->chMemHistory[0] = 0;

		
		PlaceWindow(hDlg);

		if( CreateTab(pTP) == FALSE
			|| CreateIcon(pTP) == FALSE )
		{
			MessageBox(hDlg, _T("Application initialize failed."), APPNAME, MB_ICONERROR);
			EndDialog(hDlg, 0);
			return FALSE;
		}

		pTP->nMode = MODE_CPUPOWER;

		// ClassPad: lay the tab and its pages out now. The WM_SIZE of
		// PlaceWindow came before they existed, and upstream relied on
		// another one arriving when the dialog is shown.
		{
			RECT rcClient;
			GetClientRect(hDlg, &rcClient);
			SendMessage(hDlg, WM_SIZE, 0, MAKELPARAM(rcClient.right, rcClient.bottom));
		}

		
		// make tasktray icons
		pTP->nidTrayIcon.cbSize				= sizeof(NOTIFYICONDATA);
		pTP->nidTrayIcon.hIcon				= pTP->hIcon[0];
		pTP->nidTrayIcon.hWnd				= hDlg;
		pTP->nidTrayIcon.uCallbackMessage	= MY_NOTIFYICON;
		pTP->nidTrayIcon.uFlags				= NIF_MESSAGE | NIF_ICON;
		pTP->nidTrayIcon.uID				= 1;

		Shell_NotifyIcon(NIM_ADD, &pTP->nidTrayIcon);

		Measure(pTP);	// the first sample only sets the reference
		SetTimer(hDlg, 1, pTP->dwInterval, NULL);

		ShowWindow(pTP->hwndCpupower, SW_SHOWNORMAL);

		return TRUE;
	}

	// ----------------------------------------------------------
	case WM_NOTIFY: 

		lpnmhdr = (LPNMHDR)lParam;

		if( (lpnmhdr->hwndFrom == pTP->hwndTab)
			&& (lpnmhdr->idFrom == IDC_TAB)
			&& (lpnmhdr->code == TCN_SELCHANGE))
		{
			pTP->nMode = TabCtrl_GetCurSel(pTP->hwndTab);
			switch( pTP->nMode )
			{
			case MODE_CPUPOWER:
				ShowWindow(pTP->hwndCpupower,		SW_SHOWNORMAL);
				ShowWindow(pTP->hwndProcessList,	SW_HIDE);
				ShowWindow(pTP->hwndTaskList,		SW_HIDE);
				break;
			case MODE_PROCESS:
				ShowWindow(pTP->hwndCpupower,		SW_HIDE);
				ShowWindow(pTP->hwndProcessList,	SW_SHOWNORMAL);
				ShowWindow(pTP->hwndTaskList,		SW_HIDE);
				break;
			case MODE_TASKLIST:
				ShowWindow(pTP->hwndCpupower,		SW_HIDE);
				ShowWindow(pTP->hwndProcessList,	SW_HIDE);
				ShowWindow(pTP->hwndTaskList,		SW_SHOWNORMAL);
				break;

			default:
				break;
			}
		}
		break; 

	// ----------------------------------------------------------
	case WM_SIZE:
	{ 
		HDWP hdwp; 
		RECT rc; 

		SetRect(&rc, 0, 0, LOWORD(lParam), HIWORD(lParam)); 
		TabCtrl_AdjustRect(pTP->hwndTab, FALSE, &rc); 

		// Size the tab control to fit the client area. 
		hdwp = BeginDeferWindowPos(4); 
		
		DeferWindowPos(hdwp, pTP->hwndTab, NULL, 0, 0, LOWORD(lParam), HIWORD(lParam), SWP_NOMOVE | SWP_NOZORDER ); 

		DeferWindowPos(hdwp, pTP->hwndProcessList, HWND_TOP,
			rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, 0 ); 

		DeferWindowPos(hdwp, pTP->hwndCpupower, HWND_TOP,
			rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, 0 ); 

		DeferWindowPos(hdwp, pTP->hwndTaskList, HWND_TOP,
			rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, 0 ); 
		
		EndDeferWindowPos(hdwp);
	}
	break; 

	// ----------------------------------------------------------
	// ClassPad: the screen was rotated or the taskbar moved. Only when the
	// dialog no longer fits: this message comes for other settings too, and
	// the user may have dragged the dialog somewhere.
	case WM_SETTINGCHANGE:
	{
		RECT rcWorkArea, rcWindow;
		if( SystemParametersInfo(SPI_GETWORKAREA, 0, &rcWorkArea, FALSE)
			&& GetWindowRect(hDlg, &rcWindow)
			&& (rcWindow.left < rcWorkArea.left || rcWindow.top < rcWorkArea.top
				|| rcWindow.right > rcWorkArea.right || rcWindow.bottom > rcWorkArea.bottom) )
		{
			PlaceWindow(hDlg);
		}
		break;
	}

	// ----------------------------------------------------------
	case MY_NOTIFYICON:
		switch(lParam)
		{
		case WM_LBUTTONDOWN:
		{
			if(IsWindowVisible(hDlg) )
			{
				ShowWindow(hDlg, SW_HIDE);
			}
			else
			{
				ShowWindow(hDlg, SW_SHOWNORMAL);
				SetForegroundWindow(hDlg);
			}

			break;
		}
		default:
			break;
		}
		break;
	// ----------------------------------------------------------
	case WM_COMMAND:
		switch(LOWORD(wParam))
		{
		case IDOK:
			return TRUE;
		case IDCANCEL:
			if(IsWindowVisible(hDlg) )
			{
				ShowWindow(hDlg, SW_HIDE);
			}
			// EndDialog(hDlg, 0);
			return TRUE;
		}
		break;

	// ----------------------------------------------------------
	case WM_HELP:
		DialogBox(pTP->g_hInst, MAKEINTRESOURCE(IDD_HELP), hDlg, (DLGPROC)DlgProcHelp);
		return TRUE;	// upstream fell through into WM_TIMER



	// ----------------------------------------------------------
	case WM_TIMER:
		Measure(pTP);

		if(!IsWindowVisible(hDlg))
			return 0;

		switch(pTP->nMode)
		{
		case MODE_ICON:
			break;
		case MODE_CPUPOWER:
			PostMessage(pTP->hwndCpupower, WM_TIMER, wParam, lParam);
			break;
		case MODE_PROCESS:
			PostMessage(pTP->hwndProcessList, WM_TIMER, wParam, lParam);
			break;
		case MODE_TASKLIST:
			PostMessage(pTP->hwndTaskList, WM_TIMER, wParam, lParam);
			break;
		default:
			break;
		}
		return 0;

	// ----------------------------------------------------------
	case WM_CLOSE:
		EndDialog(hDlg, 0);
		return TRUE;

	// ----------------------------------------------------------
	case WM_DESTROY:
		KillTimer(hDlg, 1);
		Shell_NotifyIcon(NIM_DELETE, &pTP->nidTrayIcon);
		
		if(pTP)
		{
			LocalFree(pTP);
		}
		return 0;
	}
	
	return FALSE;
}

//-----------------------------------------------------------------------------
// create tab control
//-----------------------------------------------------------------------------
static BOOL CreateTab(ThreadPack* pTP)
{
	RECT rcDlg;
	HWND hwndProcessList;
	HWND hwndCpupower;
	HWND hwndTaskList;

	HWND hwndTab = GetDlgItem(pTP->hDlg, IDC_TAB);
	pTP->hwndTab = hwndTab;

	if( hwndTab == NULL )
		return FALSE;

    GetClientRect(pTP->hDlg, &rcDlg);

	TCITEM tie; 

	tie.mask = TCIF_TEXT | TCIF_IMAGE; 
	tie.iImage = -1; 
	
	// ---------------------------------------------------- CPUPOWER
	tie.pszText = _T("CPU");
	
	if(TabCtrl_InsertItem(hwndTab, MODE_CPUPOWER, &tie) == -1)
		return FALSE;

	hwndCpupower = CreateDialog( pTP->g_hInst, MAKEINTRESOURCE(IDD_CPU), hwndTab, DlgProcCpu );

	if( hwndCpupower == NULL )
		return FALSE;
	
	SendMessage(hwndCpupower, WM_THREADPACK_POINTER, (WPARAM)pTP, 0);
	ShowWindow(pTP->hwndCpupower, SW_SHOWNORMAL);

	// ---------------------------------------------------- PROCESSLIST
	tie.pszText = _T("Process");

	if(TabCtrl_InsertItem(hwndTab, MODE_PROCESS, &tie) == -1)
		return FALSE;

	hwndProcessList = CreateDialog( pTP->g_hInst, MAKEINTRESOURCE(IDD_PROCESS_LIST), hwndTab, DlgProcProcess );

	if( hwndProcessList == NULL )
		return FALSE;
	
	SendMessage(hwndProcessList, WM_THREADPACK_POINTER, (WPARAM)pTP, 0);
	ShowWindow(pTP->hwndProcessList, SW_SHOWNORMAL);


	// ---------------------------------------------------- TASKLIST
	tie.pszText = _T("Task");
	if(TabCtrl_InsertItem(hwndTab, MODE_TASKLIST, &tie) == -1)
		return FALSE;

	hwndTaskList = CreateDialog( pTP->g_hInst, MAKEINTRESOURCE(IDD_TASK_LIST), hwndTab, DlgProcTask );

	if( hwndTaskList == NULL )
		return FALSE;
	
	SendMessage(hwndTaskList, WM_THREADPACK_POINTER, (WPARAM)pTP, 0);
	ShowWindow(pTP->hwndTaskList, SW_SHOWNORMAL);
	
	// ---------------------------------------------------- ADD

	pTP->hwndProcessList = hwndProcessList;
	pTP->hwndCpupower = hwndCpupower;
	pTP->hwndTaskList = hwndTaskList;

	return TRUE;

}

//-----------------------------------------------------------------------------
//	load icon image from resource block
//-----------------------------------------------------------------------------
static BOOL CreateIcon(ThreadPack* pTP)
{
	HICON* pIcon = pTP->hIcon;
	for( int ii = 0; ii < 12; ii++, pIcon++ )
	{
		*pIcon = (HICON)LoadImage( pTP->g_hInst,
				MAKEINTRESOURCE(IDI_CPU_00 + ii), IMAGE_ICON, 16,16,0);
		if( pIcon == NULL )
			return FALSE;
	}
	return TRUE;
}

//-----------------------------------------------------------------------------
// measure cpu power and ...
//
// ClassPad: upstream measured the processor's load with a thread of idle
// priority that never stops running and took what that thread did not get.
// Here that thread would keep the processor from ever sleeping (OEMIdle, the
// tickless timer) and the translator busy all the time, so the load is taken
// from the kernel's own idle counter instead: GetIdleTime() is the time spent
// in OEMIdle in milliseconds (the OAL's curridlelow/high, SRC\OAL\OALLIB\timer.c).
//-----------------------------------------------------------------------------
static void Measure(ThreadPack* pTP)
{
	static DWORD	dwLastIdleTime = 0;
	static DWORD	dwLastTickTime = 0;
	static BOOL		bHaveLast = FALSE;
	static int		nLastIconIndex = -1;

	DWORD dwCurrentTickTime = GetTickCount();
	DWORD dwCurrentIdleTime = GetIdleTime();
	DWORD dwCpuPower = 0;

	MEMORYSTATUS ms;
	ms.dwLength = sizeof(MEMORYSTATUS);

	// calculate cpu power
	if( bHaveLast && dwCurrentTickTime != dwLastTickTime )
	{
		DWORD dwTick = dwCurrentTickTime - dwLastTickTime;
		DWORD dwIdle = dwCurrentIdleTime - dwLastIdleTime;
		if( dwIdle > dwTick )
			dwIdle = dwTick;
		dwCpuPower = 100 - (dwIdle * 100) / dwTick;
	}

	// Draw tray icon (only when it changes: every update is a message to the shell)
	int nIconIndex = (dwCpuPower)*11/100;
	if( nIconIndex > 11 || nIconIndex < 0 )
		nIconIndex = 0;

	if( nIconIndex != nLastIconIndex )
	{
		pTP->nidTrayIcon.hIcon = pTP->hIcon[nIconIndex];
		Shell_NotifyIcon(NIM_MODIFY, &pTP->nidTrayIcon);
		nLastIconIndex = nIconIndex;
	}

	// memory history
	GlobalMemoryStatus(&ms);

	// Shift history
	memmove(pTP->chPowHistory+1, pTP->chPowHistory, HISTORY_MAX-1);
	memmove(pTP->chMemHistory+1, pTP->chMemHistory, HISTORY_MAX-1);

	// save history
	*pTP->chPowHistory = (char)dwCpuPower;
	*pTP->chMemHistory = (char)ms.dwMemoryLoad;

	// save status
	dwLastTickTime = dwCurrentTickTime;
	dwLastIdleTime = dwCurrentIdleTime;
	bHaveLast = TRUE;
}

//-----------------------------------------------------------------------------
// ClassPad: put the dialog at the top of the work area, centred, no larger
// than the work area (at start and after a rotation)
//-----------------------------------------------------------------------------
static void PlaceWindow(HWND hDlg)
{
	RECT rcWorkArea;
	if( SystemParametersInfo(SPI_GETWORKAREA, 0, &rcWorkArea, FALSE) )
	{
		int cxWork = rcWorkArea.right - rcWorkArea.left;
		int cyWork = rcWorkArea.bottom - rcWorkArea.top;
		int cx = (cxWork > MAIN_DLG_CX) ? MAIN_DLG_CX : cxWork;
		int cy = (cyWork > MAIN_DLG_CY) ? MAIN_DLG_CY : cyWork;

		MoveWindow(hDlg
			, rcWorkArea.left + (cxWork - cx)/2
			, rcWorkArea.top
			, cx
			, cy
			, TRUE );
	}
}

//-----------------------------------------------------------------------------
// about dlg proc
//-----------------------------------------------------------------------------
static BOOL CALLBACK DlgProcHelp(HWND hDlg, UINT Msg, WPARAM wParam, LPARAM lParam)
{
	switch(Msg)
	{
	case WM_CLOSE:
		EndDialog(hDlg, 0);
		return TRUE;
	}
	return FALSE;
}

//-----------------------------------------------------------------------------
// HelperFunction
//-----------------------------------------------------------------------------
LPARAM GetSelectedItemLParam(HWND hwndLView)
{
	int nIndex = -1;
	LVITEM lvItem;
	memset(&lvItem, 0, sizeof(LVITEM));
	lvItem.mask = LVIF_PARAM;
	
	nIndex = ListView_GetNextItem(hwndLView, nIndex, 0);

	while( nIndex != -1 )
	{
		lvItem.iItem = nIndex;
		if( ListView_GetItemState(hwndLView, nIndex, LVIS_SELECTED) )
		{
			ListView_GetItem(hwndLView, &lvItem);
			return lvItem.lParam;
		}
		nIndex = ListView_GetNextItem(hwndLView, nIndex, 0);
	}
	return 0;
}

//-----------------------------------------------------------------------------
// select item by lparam
//-----------------------------------------------------------------------------
void SelectItemLParam(HWND hwndLView, LPARAM lParam)
{
	int nIndex = -1;
	LVITEM lvItem;
	memset(&lvItem, 0, sizeof(LVITEM));
	lvItem.mask = LVIF_PARAM;
	
	nIndex = ListView_GetNextItem(hwndLView, nIndex, 0);

	while( nIndex != -1 )
	{
		lvItem.iItem = nIndex;

		ListView_GetItem(hwndLView, &lvItem);

		if( lvItem.lParam == lParam )
		{
			ListView_SetItemState(hwndLView, nIndex, LVIS_SELECTED, LVIS_SELECTED);
		}

		nIndex = ListView_GetNextItem(hwndLView, nIndex, 0);
	}
}
