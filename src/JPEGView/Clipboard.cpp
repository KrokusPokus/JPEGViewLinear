#include "StdAfx.h"
#include "Clipboard.h"
#include "JPEGImage.h"
#include "Helpers.h"
#include "BasicProcessing.h"
#include <gdiplus.h>

void CClipboard::CopyFullImageToClipboard(HWND hWnd, CJPEGImage * pImage, EProcessingFlags eFlags)
	{
	CopyFullImageToClipboard(hWnd, pImage, eFlags, CRect(0, 0, pImage->OrigWidth(), pImage->OrigHeight()));
	}

void CClipboard::CopyFullImageToClipboard(HWND hWnd, CJPEGImage * pImage, EProcessingFlags eFlags, CRect clipRect)
	{
	if (pImage == NULL)
		{
		return;
		}

	clipRect.left = max(0, clipRect.left);
	clipRect.top = max(0, clipRect.top);
	clipRect.right = min(pImage->OrigWidth(), clipRect.right);
	clipRect.bottom = min(pImage->OrigHeight(), clipRect.bottom);

	void* pDIB = pImage->GetDIB(pImage->OrigSize(), clipRect.Size(), clipRect.TopLeft(), eFlags);
	DoCopy(hWnd, clipRect.Width(), clipRect.Height(), pDIB);
	}

void CClipboard::DoCopy(HWND hWnd, int nWidth, int nHeight, const void* pSourceImageDIB32) {
	if (!::OpenClipboard(hWnd)) {
        return;
	}
	::EmptyClipboard();

	// get needed size of memory block
	uint32 nSizeLinePadded = Helpers::DoPadding(nWidth*3, 4);
	uint32 nSizeBytes = sizeof(BITMAPINFO) + nSizeLinePadded * nHeight;
	
	// Allocate memory
	HGLOBAL hMem = ::GlobalAlloc(GMEM_MOVEABLE, nSizeBytes);
	if (hMem == NULL) {
		::CloseClipboard();
		return;
	}
	void* pMemory = ::GlobalLock(hMem); 

	BITMAPINFO* pBMInfo = (BITMAPINFO*) pMemory;
	memset(pBMInfo, 0, sizeof(BITMAPINFO));

	pBMInfo->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	pBMInfo->bmiHeader.biWidth = nWidth;
	pBMInfo->bmiHeader.biHeight = nHeight;
	pBMInfo->bmiHeader.biPlanes = 1;
	pBMInfo->bmiHeader.biBitCount = 24;
	pBMInfo->bmiHeader.biCompression = BI_RGB;
	pBMInfo->bmiHeader.biXPelsPerMeter = 10000;
	pBMInfo->bmiHeader.biYPelsPerMeter = 10000;
	pBMInfo->bmiHeader.biClrUsed = 0;

	uint8* pDIBPixelsTarget = (uint8*)pMemory + sizeof(BITMAPINFO) - sizeof(RGBQUAD);
	CBasicProcessing::Convert32bppTo24bppDIB(nWidth, nHeight, pDIBPixelsTarget, pSourceImageDIB32, true);

	::GlobalUnlock(hMem); 
	::SetClipboardData(CF_DIB, hMem); 

	::CloseClipboard();
}
