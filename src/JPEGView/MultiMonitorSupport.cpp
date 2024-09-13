#include "StdAfx.h"
#include "MultiMonitorSupport.h"
#include "SettingsProvider.h"
#include <math.h>

struct EnumMonitorParams {
	EnumMonitorParams(int nIndexMonitor) {
		IndexMonitor = nIndexMonitor;
		NumPixels = 0;
		Iterations = 0;
		rectMonitor = CRect(0, 0, 0, 0);
	}

	int Iterations;
	int IndexMonitor;
	int NumPixels;
	CRect rectMonitor;
};

bool CMultiMonitorSupport::IsMultiMonitorSystem() {
	return ::GetSystemMetrics(SM_CMONITORS) > 1;
}

CRect CMultiMonitorSupport::GetVirtualDesktop() {
	return CRect(CPoint(::GetSystemMetrics(SM_XVIRTUALSCREEN), ::GetSystemMetrics(SM_YVIRTUALSCREEN)),
		CSize(::GetSystemMetrics(SM_CXVIRTUALSCREEN), ::GetSystemMetrics(SM_CYVIRTUALSCREEN)));
}

// Callback called during enumaration of monitors
static BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData) {
	MONITORINFO monitorInfo;
	monitorInfo.cbSize = sizeof(MONITORINFO);
	EnumMonitorParams* pParams = (EnumMonitorParams*) dwData;
	if (pParams->IndexMonitor == -1) {
		// Use the monitor with largest number of pixels
		int nNumPixels = (lprcMonitor->right - lprcMonitor->left)*(lprcMonitor->bottom - lprcMonitor->top);
		if (nNumPixels > pParams->NumPixels) {
			pParams->NumPixels = nNumPixels;
			pParams->rectMonitor = CRect(lprcMonitor);
		} else if (nNumPixels == pParams->NumPixels) {
			// if same size take primary
			::GetMonitorInfo(hMonitor, &monitorInfo);
			if (monitorInfo.dwFlags & MONITORINFOF_PRIMARY) {
				pParams->rectMonitor = CRect(lprcMonitor);
			}
		}
	} else {
		::GetMonitorInfo(hMonitor, &monitorInfo);
		if (pParams->IndexMonitor == 0) {
			// take primary monitor
			if (monitorInfo.dwFlags & MONITORINFOF_PRIMARY) {
				pParams->rectMonitor = CRect(lprcMonitor);
				return FALSE;
			}
		} else {
			// take the i-th non primary monitor
			if (!(monitorInfo.dwFlags & MONITORINFOF_PRIMARY)) {
				pParams->Iterations += 1;
				if (pParams->IndexMonitor == pParams->Iterations) {
					pParams->rectMonitor = CRect(lprcMonitor);
					return FALSE;
				}
			}
		}
	}
	return TRUE;
}

CRect CMultiMonitorSupport::GetMonitorRect(int nIndex) {
	if (!CMultiMonitorSupport::IsMultiMonitorSystem()) {
		return CRect(0, 0, ::GetSystemMetrics(SM_CXSCREEN), ::GetSystemMetrics(SM_CYSCREEN));
	}
	EnumMonitorParams params(nIndex);
	::EnumDisplayMonitors(NULL, NULL, &MonitorEnumProc, (LPARAM) &params);
	if (params.rectMonitor.Width() == 0) {
		return CRect(0, 0, ::GetSystemMetrics(SM_CXSCREEN), ::GetSystemMetrics(SM_CYSCREEN));
	} else {
		return params.rectMonitor;
	}
}

CRect CMultiMonitorSupport::GetWorkingRect(HWND hWnd) {
	HMONITOR hMonitor = ::MonitorFromWindow(hWnd, MONITOR_DEFAULTTOPRIMARY);
	MONITORINFO monitorInfo;
	monitorInfo.cbSize = sizeof(MONITORINFO);
	::GetMonitorInfo(hMonitor, &monitorInfo);
	return CRect(monitorInfo.rcWork);
}

CRect CMultiMonitorSupport::GetDefaultWindowRect()
	{
	CRect windowRect;
	RECT workAreaRect;
	INT iBorderPosR = 0;
	
	HDC hdc = ::GetDC(NULL);
	int ScreenDPI = GetDeviceCaps(hdc, LOGPIXELSX);
	::ReleaseDC(NULL, hdc);

	::SystemParametersInfo(SPI_GETWORKAREA, 0, &workAreaRect, 0);

	iBorderPosR = (int(floor(((workAreaRect.right - floor(((256.0*(workAreaRect.right-workAreaRect.left))/1920) + 0.5))/32.0)+0.5))) * 32;

	// left, top, right, bottom
	windowRect = CRect((workAreaRect.right-iBorderPosR),workAreaRect.top,iBorderPosR,workAreaRect.bottom);

	CRect rectAllScreens = CMultiMonitorSupport::GetVirtualDesktop();
	if (windowRect.IsRectEmpty() || !rectAllScreens.IntersectRect(&rectAllScreens, &windowRect))
		{
		CRect monitorRect = CMultiMonitorSupport::GetMonitorRect(-1);
		int nDesiredWidth = monitorRect.Width()*2/3;
		int nDesiredHeight = nDesiredWidth*3/4;
		CSize borderSize = Helpers::GetTotalBorderSize();
		nDesiredWidth += borderSize.cx;
		nDesiredHeight += borderSize.cy;
		windowRect = CRect(CPoint(monitorRect.left + (monitorRect.Width() - nDesiredWidth) / 2, monitorRect.top + (monitorRect.Height() - nDesiredHeight) / 2), CSize(nDesiredWidth, nDesiredHeight));
		}
	return windowRect;
	}

CRect CMultiMonitorSupport::GetDefaultClientRectInWindowMode()
	{
	CRect wndRect = CMultiMonitorSupport::GetDefaultWindowRect();
	CSize borderSize = Helpers::GetTotalBorderSize();
	return CRect(0, 0, wndRect.Width() - borderSize.cx, wndRect.Height() - borderSize.cy);
	}
