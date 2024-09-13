#pragma once

#include "Helpers.h"

enum EProcessingFlags;

namespace HelpersGUI {

	// Errors that occur during loading a file or pasting from clipboard
	enum EFileLoadError {
		FileLoad_Ok = 0,
		FileLoad_PasteFromClipboardFailed = 1,
		FileLoad_LoadError = 2,
		FileLoad_SlideShowListInvalid = 3,
		FileLoad_NoFilesInDirectory = 4,

		// these can be combined with other error codes, so make sure they work with bit arithmetic
		FileLoad_OutOfMemory = 65536,
		FileLoad_ExceptionError = 32768
	};

	// Draws a 32 bit DIB centered in the given target area, filling the remaining area with the given brush
	// The bmInfo struct will be initialized by this method and does not need to be preinitialized.
	// Return value is the top, left coordinate of the painted DIB in the target area
	CPoint DrawDIB32bppWithBlackBorders(CPaintDC& dc, BITMAPINFO& bmInfo, void* pDIBData, HBRUSH backBrush, const CRect& targetArea, CSize dibSize);

	// Draws an error text for the given file loading error (combination of EFileLoadError codes)
	void DrawImageLoadErrorText(CPaintDC& dc, const CRect& clientRect, LPCTSTR sFailedFileName, int nFileLoadError, int nLoadErrorDetail);
}