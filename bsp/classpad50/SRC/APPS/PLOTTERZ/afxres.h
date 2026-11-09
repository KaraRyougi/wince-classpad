/*
 * afxres.h - stand-in for MFC's header, which the Platform Builder tree does
 * not have: upstream's PlotterZNative.rc (an eVC 3/4 resource script)
 * includes it for the Windows, common-control and IDC_STATIC definitions.
 */
#ifndef PLOTTERZ_AFXRES_H
#define PLOTTERZ_AFXRES_H

#include <windows.h>
#include <commctrl.h>

#ifndef IDC_STATIC
#define IDC_STATIC (-1)
#endif

#endif
