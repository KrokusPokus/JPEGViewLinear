#pragma once

#include "ProcessParams.h"

class CJPEGImage;

// Allows to copy an image to the clipboard
class CClipboard {
public:
	// Copy processed full size image to clipboard
	static void CopyFullImageToClipboard(HWND hWnd, CJPEGImage * pImage, EProcessingFlags eFlags);

	// Copy section of full size image to clipboard
	static void CopyFullImageToClipboard(HWND hWnd, CJPEGImage * pImage, EProcessingFlags eFlags, CRect clipRect);

private:
	CClipboard(void);

	static void DoCopy(HWND hWnd, int nWidth, int nHeight, const void* pSourceImageDIB32);
};
