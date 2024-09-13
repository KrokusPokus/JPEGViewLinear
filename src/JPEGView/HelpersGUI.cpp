#include "StdAfx.h"
#include "HelpersGUI.h"
#include "ProcessParams.h"

namespace HelpersGUI {

CPoint DrawDIB32bppWithBlackBorders(CPaintDC& dc, BITMAPINFO& bmInfo, void* pDIBData, HBRUSH backBrush, const CRect& targetArea, CSize dibSize)
	{
	memset(&bmInfo, 0, sizeof(BITMAPINFO));
	bmInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmInfo.bmiHeader.biWidth = dibSize.cx;
	bmInfo.bmiHeader.biHeight = -dibSize.cy;
	bmInfo.bmiHeader.biPlanes = 1;
	bmInfo.bmiHeader.biBitCount = 32;
	bmInfo.bmiHeader.biCompression = BI_RGB;
	int xDest = (targetArea.Width() - dibSize.cx) / 2;
	int yDest = (targetArea.Height() - dibSize.cy) / 2;
	
	dc.SetDIBitsToDevice(xDest, yDest, dibSize.cx, dibSize.cy, 0, 0, 0, dibSize.cy, pDIBData, &bmInfo, DIB_RGB_COLORS);

	// remaining client area is painted black
	if (dibSize.cx < targetArea.Width())
		{
		CRect r(0, 0, xDest, targetArea.Height());
		dc.FillRect(&r, backBrush);
		CRect rr(xDest + dibSize.cx, 0, targetArea.Width(), targetArea.Height());
		dc.FillRect(&rr, backBrush);
		}
	if (dibSize.cy < targetArea.Height())
		{
		CRect r(0, 0, targetArea.Width(), yDest);
		dc.FillRect(&r, backBrush);
		CRect rr(0, yDest + dibSize.cy, targetArea.Width(), targetArea.Height());
		dc.FillRect(&rr, backBrush);
		}

	return CPoint(xDest, yDest);
	}

void DrawImageLoadErrorText(CPaintDC& dc, const CRect& clientRect, LPCTSTR sFailedFileName, int nFileLoadError, int nLoadErrorDetail)
	{
	bool bOutOfMemory = nLoadErrorDetail & FileLoad_OutOfMemory;
	bool bExceptionError = nLoadErrorDetail & FileLoad_ExceptionError;

	const int BUF_LEN = 512;
	TCHAR buff[BUF_LEN];
	buff[0] = 0;
	CRect rectText(0, clientRect.Height()/2 - 40, clientRect.Width(), clientRect.Height());
	switch (nFileLoadError)
		{
		case FileLoad_PasteFromClipboardFailed:
			_tcsncpy_s(buff, BUF_LEN, _T("Pasting image from clipboard failed!"), BUF_LEN);
			break;
		case FileLoad_LoadError:
			_stprintf_s(buff, BUF_LEN, _T("The file '%s' could not be read!"), sFailedFileName);
			break;
		case FileLoad_SlideShowListInvalid:
			_stprintf_s(buff, BUF_LEN, _T("The file '%s' does not contain a list of file names!"), sFailedFileName);
			break;
		case FileLoad_NoFilesInDirectory:
			_stprintf_s(buff, BUF_LEN, CString(_T("The directory '%s' does not contain any image files!")), sFailedFileName);
			break;
		default:
			return;
		}
	if (bOutOfMemory)
		{
		_tcscat_s(buff, BUF_LEN, _T("\n"));
		_tcscat_s(buff, BUF_LEN, _T("Reason: Not enough memory available"));
		}
	dc.DrawText(buff, -1, &rectText, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
	}

}