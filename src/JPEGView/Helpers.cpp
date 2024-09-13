#include "StdAfx.h"
#include "Helpers.h"
#include "MultiMonitorSupport.h"
#include "JPEGImage.h"
#include "FileList.h"
#include "SettingsProvider.h"
#include <thread>
//#include <math.h>
#ifdef _WIN64
#include <intrin.h>
#endif
namespace Helpers {

float ScreenScaling = -1.0f;

TCHAR CReplacePipe::sm_buffer[MAX_SIZE_REPLACE_PIPE];

CReplacePipe::CReplacePipe(LPCTSTR sText)
	{
	_tcsncpy_s(sm_buffer, MAX_SIZE_REPLACE_PIPE, sText, MAX_SIZE_REPLACE_PIPE);
	sm_buffer[MAX_SIZE_REPLACE_PIPE-1] = 0;
	TCHAR* pPtr = sm_buffer;
	while (*pPtr != 0)
		{
		if (*pPtr == _T('|')) *pPtr = 0;
		pPtr++;
		}
	}

static TCHAR buffAppPath[MAX_PATH + 32] = _T("");

LPCTSTR JPEGViewAppDataPath() {
	if (buffAppPath[0] == 0) {
		::SHGetFolderPath(NULL, CSIDL_APPDATA, NULL, SHGFP_TYPE_CURRENT, buffAppPath);
		_tcscat_s(buffAppPath, MAX_PATH + 32, _T("\\JPEGView\\"));
	}
	return buffAppPath;
}

void SetJPEGViewAppDataPath(LPCTSTR sPath) {
	_tcscpy_s(buffAppPath, MAX_PATH + 32, sPath);
}
CPoint LimitOffsets(const CPoint& offsets, const CSize & rectSize, const CSize & outerRect)
	{
	int nMaxOffsetX = (outerRect.cx - rectSize.cx)/2;
	nMaxOffsetX = max(0, nMaxOffsetX);
	int nMaxOffsetY = (outerRect.cy - rectSize.cy)/2;
	nMaxOffsetY = max(0, nMaxOffsetY);
	return CPoint(max(-nMaxOffsetX, min(+nMaxOffsetX, offsets.x)), max(-nMaxOffsetY, min(+nMaxOffsetY, offsets.y)));
	}

#ifdef _WIN64
static CPUType ProbeSSEorAVX2() {
	__try {
		// check if CPU supports AVX and the xgetbv instruction
		int abcd[4];
		__cpuid(abcd, 1);
		if ((abcd[2] & 0x18000000) != 0x18000000) // AVX and OSXSAVE bits
			return CPU_SSE;
		if ((abcd[2] & 0x04000000) == 0) // XSAVE bit, support for xgetbv
			return CPU_SSE;

		// check if operating system supports AVX(2)
		unsigned long long xcr0 = _xgetbv(0);
		if ((xcr0 & 6) != 6)
			return CPU_SSE; // nope, only use SSE

		// check if AVX2 instructions are supported
		const int AVX2BITMASK = 1 << 5;
		__cpuidex(abcd, 7, 0);
		return (abcd[1] & AVX2BITMASK) ? CPU_AVX2 : CPU_SSE;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return CPU_SSE;
	}
}
#endif

CPUType ProbeCPU(void) {
	static CPUType cpuType = CPU_Unknown;
	if (cpuType != CPU_Unknown) {
		return cpuType;
	}

#ifdef _WIN64
	return ProbeSSEorAVX2(); // 64 bit always supports at least SSE
#else
	// Structured exception handling is mantatory, try/catch(...) does not catch such severe stuff.
	cpuType = CPU_Generic;
	__try {
		uint32 FeatureMask;
		_asm {
			mov eax, 1
			cpuid
			mov FeatureMask, edx
		}
		if ((FeatureMask & (1 << 26)) != 0) {
			cpuType = CPU_SSE; // this means SSE2
		}
		
	} __except ( EXCEPTION_EXECUTE_HANDLER ) {
		// even CPUID is not supported, use generic code
		return cpuType;
	}
	return cpuType;
#endif
}

// returns if the CPU supports some form of hardware multiprocessing, e.g. hyperthreading or multicore
static bool CPUSupportsHWMultiprocessing(void) {   
	if (ProbeCPU() >= CPU_SSE) {
		int output[4];
		__cpuid(output, 1);
		return (output[3] & 0x10000000);
	} else {
		return false;
	}  
}

int NumConcurrentThreads(void) {
	if (!CPUSupportsHWMultiprocessing()) {
		return 1;
	}

	//may return 0 when not able to detect
	int processor_count = std::thread::hardware_concurrency();

	if (processor_count<1)
		processor_count = 1;

	return processor_count;
}

// calculate CRT table
void CalcCRCTable(unsigned int crc_table[256])
	{
     for (int n = 0; n < 256; n++)
		{
		unsigned int c = (unsigned int) n;
		for (int k = 0; k < 8; k++)
			{
			if (c & 1)
			c = 0xedb88320L ^ (c >> 1);
			else
			c = c >> 1;
			}
		crc_table[n] = c;
		}
	}

void* FindJPEGMarker(void* pJPEGStream, int nStreamLength, unsigned char nMarker)
	{
	uint8* pStream = (uint8*) pJPEGStream;

	if (pStream == NULL || nStreamLength < 3 || pStream[0] != 0xFF || pStream[1] != 0xD8)
		return NULL; // not a JPEG

	int nIndex = 2;
	do {
		if (pStream[nIndex] == 0xFF)
			{
			// block header found, skip padding bytes
			while (pStream[nIndex] == 0xFF && nIndex < nStreamLength) nIndex++;
			if (pStream[nIndex] == 0 || pStream[nIndex] == nMarker)
				{
				break; // 0xFF 0x00 is part of pixel block, break
				}
			else
				{
				// it's a block marker, read length of block and skip the block
				nIndex++;
				if (nIndex+1 < nStreamLength)
					{
					nIndex += pStream[nIndex]*256 + pStream[nIndex+1];
					}
				else
					{
					nIndex = nStreamLength;
					}
				}
			}
		else
			{
			break; // block with pixel data found, start hashing from here
			}
		} while (nIndex < nStreamLength);

	if (nMarker == 0 || (pStream[nIndex] == nMarker && pStream[nIndex-1] == 0xFF))
		{
		return &(pStream[nIndex-1]); // place on marker start
		}
	else
		{
		return NULL;
		}
	}

void* FindEXIFBlock(void* pJPEGStream, int nStreamLength)
	{
	uint8* pEXIFBlock = (uint8*)Helpers::FindJPEGMarker(pJPEGStream, nStreamLength, 0xE1);
	if (pEXIFBlock != NULL && strncmp((const char*)(pEXIFBlock + 4), "Exif", 4) != 0)
		{
		return NULL;
		}
	return pEXIFBlock;
	}

__int64 CalculateJPEGFileHash(void* pJPEGStream, int nStreamLength)
	{
	uint8* pStream = (uint8*) pJPEGStream;
	void* pPixelStart = FindJPEGMarker(pJPEGStream, nStreamLength, 0);
	if (pPixelStart == NULL)
		{
		return 0;
		}
	int nIndex = (int)((uint8*)pPixelStart - (uint8*)pJPEGStream + 1);
	
	// take whole stream in case of inconsistency or if remaining part is too small
	if (nStreamLength - nIndex < 4)
		{
		nIndex = 0;
		assert(false);
		}

	// now we can calculate the hash over the compressed pixel data
	// do not look at every byte due to performance reasons
	const int nTotalLookups = 10000;
	int nIncrement = (nStreamLength - nIndex)/nTotalLookups;
	nIncrement = max(1, nIncrement);

	unsigned int crc_table[256];
	CalcCRCTable(crc_table);
	uint32 crcValue = 0xffffffff;
	unsigned int sumValue = 0;
	while (nIndex < nStreamLength)
		{
		sumValue += pStream[nIndex];
		crcValue = crc_table[(crcValue ^ pStream[nIndex]) & 0xff] ^ (crcValue >> 8);
		nIndex += nIncrement;
		}

	return ((__int64)crcValue << 32) + sumValue;
	}

CString TryConvertFromUTF8(uint8* pComment, int nLengthInBytes) {
	wchar_t* pCommentUnicode = new wchar_t[nLengthInBytes + 1];
	char* pCommentBack = new char[nLengthInBytes + 1];
	CString result;
	int nCharsConverted = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (LPCSTR)pComment, nLengthInBytes, pCommentUnicode, nLengthInBytes);
	if (nCharsConverted > 0) {
		pCommentUnicode[nCharsConverted] = 0;
		if (::WideCharToMultiByte(CP_UTF8, 0, pCommentUnicode, -1, pCommentBack, nLengthInBytes + 1, NULL , NULL) > 0) {
			if (memcmp(pComment, pCommentBack, nLengthInBytes) == 0) {
				result = CString((LPCWSTR)pCommentUnicode);
			}
		}
	}
	delete[] pCommentUnicode;
	delete[] pCommentBack;
	return result;
}

CString GetJPEGComment(void* pJPEGStream, int nStreamLength)
	{
	uint8* pCommentSeg = (uint8*)FindJPEGMarker(pJPEGStream, nStreamLength, 0xFE);
	if (pCommentSeg == NULL)
		return CString("");

	pCommentSeg += 2;
	int nCommentLen = pCommentSeg[0]*256 + pCommentSeg[1] - 2;
	if (nCommentLen <= 0)
		return CString("");

	uint8* pComment = &(pCommentSeg[2]);
	if (nCommentLen > 2 && (nCommentLen & 1) == 0)
		{
		if (pComment[0] == 0xFF && pComment[1] == 0xFE)
			{
			// UTF16 little endian
			return CString((LPCWSTR)&(pComment[2]), (nCommentLen - 2) / 2);
			}
		else if (pComment[0] == 0xFE && pComment[1] == 0xFF)
			{
			// UTF16 big endian -> cannot read
			return CString("");
			}
		}

	// check if this is a reasonable string - it must contain enough characters between a and z and A and Z
	if (nCommentLen < 7)
		return CString(""); // cannot check for such short strings, do not use

	int nGoodChars = 0;
	for (int i = 0; i < nCommentLen; i++)
		{
		uint8 ch = pComment[i];
		if (ch >= 'a' && ch <= 'z' ||  ch >= 'A' && ch <= 'Z' || ch == ' ' || ch == ',' || ch == '.' || ch >= '0' && ch <= '9')
			{
			nGoodChars++;
			}
		}

	// The Intel lib puts this useless comment into each JPEG it writes - filter this out as nobody is interested in that...
	if (nCommentLen > 20 && strstr((char*)pComment, "Intel(R) JPEG Library") != NULL)
		return CString("");

	return (nGoodChars > nCommentLen * 8 / 10) ? CString((LPCSTR)pComment, nCommentLen) : CString("");
	}

double GetExactTickCount()
	{
	static __int64 nFrequency = -1;
	if (nFrequency == -1)
		{
		if (!::QueryPerformanceFrequency((LARGE_INTEGER*)&nFrequency))
			{
			nFrequency = 0;
			}
		}
	if (nFrequency == 0)
		{
		return ::GetTickCount();
		}
	else
		{
		__int64 nCounter;
		::QueryPerformanceCounter((LARGE_INTEGER*)&nCounter);
		return (1000*(double)nCounter)/nFrequency;
		}
	}

CSize GetTotalBorderSize() {
	const int SM_CXP_ADDEDBORDER = 92;
	int nBorderWidth = (::GetSystemMetrics(SM_CXSIZEFRAME) + ::GetSystemMetrics(SM_CXP_ADDEDBORDER)) * 2;
	int nBorderHeight = (::GetSystemMetrics(SM_CYSIZEFRAME) + ::GetSystemMetrics(SM_CXP_ADDEDBORDER)) * 2 + ::GetSystemMetrics(SM_CYCAPTION);
	return CSize(nBorderWidth, nBorderHeight);
}

EImageFormat GetImageFormat(LPCTSTR sFileName) {
	LPCTSTR sEnding = _tcsrchr(sFileName, _T('.'));
	if (sEnding != NULL) {
		sEnding += 1;
		if (_tcsicmp(sEnding, _T("JPG")) == 0 || _tcsicmp(sEnding, _T("JPEG")) == 0) {
			return IF_JPEG;
		} else if (_tcsicmp(sEnding, _T("BMP")) == 0) {
			return IF_WindowsBMP;
		} else if (_tcsicmp(sEnding, _T("PNG")) == 0) {
			return IF_PNG;
		} else if (_tcsicmp(sEnding, _T("TIF")) == 0 || _tcsicmp(sEnding, _T("TIFF")) == 0) {
			return IF_TIFF;
		} else if (_tcsicmp(sEnding, _T("WEBP")) == 0) {
			return IF_WEBP;
		}  else if (_tcsicmp(sEnding, _T("TGA")) == 0) {
			return IF_TGA;
/*
		} else if (IsInFileEndingList(CSettingsProvider::This().FilesProcessedByWIC(), sEnding)) {
			return IF_WIC;
		} else if (IsInFileEndingList(CSettingsProvider::This().FileEndingsRAW(), sEnding)) {
			return IF_CameraRAW;
*/
		}
	}
	return IF_Unknown;
}

// strstr() ignoring case
LPTSTR stristr(LPCTSTR szStringToBeSearched, LPCTSTR szSubstringToSearchFor) {
	LPCTSTR pPos = NULL;
	LPCTSTR szCopy1 = NULL;
	LPCTSTR szCopy2 = NULL;
	 
	// verify parameters
	if ( szStringToBeSearched == NULL || szSubstringToSearchFor == NULL ) {
		return (LPTSTR)szStringToBeSearched;
	}
	 
	// empty substring - return input (consistent with strstr)
	if ( _tcslen(szSubstringToSearchFor) == 0 ) {
		return (LPTSTR)szStringToBeSearched;
	}
	 
	szCopy1 = _tcslwr(_tcsdup(szStringToBeSearched));
	szCopy2 = _tcslwr(_tcsdup(szSubstringToSearchFor));
	 
	if ( szCopy1 == NULL || szCopy2 == NULL ) {
		// another option is to raise an exception here
		free((void*)szCopy1);
		free((void*)szCopy2);
		return NULL;
	}
	 
	pPos = _tcsstr(szCopy1, szCopy2);
	 
	if ( pPos != NULL ) {
		// map to the original string
		pPos = szStringToBeSearched + (pPos - szCopy1);
	}
	 
	free((void*)szCopy1);
	free((void*)szCopy2);
	 
	return (LPTSTR)pPos;
 } // stristr(...)
 
__int64 GetFileSize(LPCTSTR sPath)
	{
	HANDLE hFile = ::CreateFile(sPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (hFile == INVALID_HANDLE_VALUE)
		{
		return 0;
		}
	__int64 fileSize = 0;
	::GetFileSizeEx(hFile, (PLARGE_INTEGER)&fileSize);
	::CloseHandle(hFile);
	return fileSize;
	}

// Gets the frame index of the next frame, depending on the index of the last image (relevant if the image is a multiframe image)
int GetFrameIndex(CJPEGImage* pImage, bool bNext, bool bPlayAnimation, bool & switchImage)
	{
    bool isMultiFrame = pImage != NULL && pImage->NumberOfFrames() > 1;
    bool isAnimation = pImage != NULL && pImage->IsAnimation();
    int nFrameIndex = 0;
    switchImage = true;
    if (isMultiFrame)
		{
        switchImage = false;
        nFrameIndex = pImage->FrameIndex() + (bNext ? 1 : -1);
        if (isAnimation) {
            if (bPlayAnimation)
				{
                if (nFrameIndex < 0)
					{
                    nFrameIndex = pImage->NumberOfFrames() - 1;
					}
				else if (nFrameIndex > pImage->NumberOfFrames() - 1)
					{
                    nFrameIndex = 0;
					}
				}
			else
				{
                switchImage = true;
				nFrameIndex = 0;
				}
			}
		else
			{
            if (nFrameIndex < 0 || nFrameIndex >= pImage->NumberOfFrames())
				{
                nFrameIndex = 0;
                switchImage = true;
				}
			}
		}
    if (bPlayAnimation && pImage == NULL)
		{
        switchImage = false; // never switch image when error during animation playing
		}

    return nFrameIndex;
	}

// Gets an index string of the form [a/b] for multiframe images, empty string for single frame images
CString GetMultiframeIndex(CJPEGImage* pImage)
	{
    bool isMultiFrame = pImage != NULL && pImage->NumberOfFrames() > 1;
    if (isMultiFrame && !pImage->IsAnimation())
		{
        CString s;
        s.Format(_T(" [%d/%d]"), pImage->FrameIndex() + 1, pImage->NumberOfFrames());
        return s;
		}
    return CString(_T(""));
	}

}