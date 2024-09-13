#include "StdAfx.h"
#include "XMMImage.h"
#include "Helpers.h"

CFloatImage::CFloatImage(int nWidth, int nHeight, int padding)
	{
	Init(nWidth, nHeight, false, padding);
	}

CFloatImage::CFloatImage(int nWidth, int nHeight, bool bPadHeight, int padding)
	{
	Init(nWidth, nHeight, bPadHeight, padding);
	}

//GF: version for f32 SSE & AVX2
CFloatImage::CFloatImage(int nWidth, int nHeight, int nFirstX, int nLastX, int nFirstY, int nLastY, const void* pDIB, int nChannels, int padding)
	{
	int nSectionWidth = nLastX - nFirstX + 1;
	int nSectionHeight = nLastY - nFirstY + 1;
	Init(nSectionWidth, nSectionHeight, false, padding);

	if (m_pMemory != NULL) {
		int nSrcLineWidthPadded = Helpers::DoPadding(nWidth * nChannels, 4);
		const uint8* pSrc = (uint8*)pDIB + (long long)nFirstY*(long long)nSrcLineWidthPadded + (long long)nFirstX*(long long)nChannels;

		float* pDst = (float*) m_pMemory;
		for (int j = 0; j < nSectionHeight; j++) {
			if (nChannels == 4) {
				for (int i = 0; i < nSectionWidth; i++) {
					uint32 sourcePixel = ((uint32*)pSrc)[i];
					int d = i;
					uint32 nBlue = sourcePixel & 0xFF;
					uint32 nGreen = (sourcePixel >> 8) & 0xFF;
					uint32 nRed = (sourcePixel >> 16) & 0xFF;

					pDst[d] = ((float)sRGB8_LinRGB12[nBlue]);
					d += m_nPaddedWidth;
					pDst[d] = ((float)sRGB8_LinRGB12[nGreen]);
					d += m_nPaddedWidth;
					pDst[d] = ((float)sRGB8_LinRGB12[nRed]);
				}
			} else {
				for (int i = 0; i < nSectionWidth; i++)	{
					int s = i*3;
					int d = i;
					pDst[d] = ((float)sRGB8_LinRGB12[pSrc[s]]);
					d += m_nPaddedWidth;
					pDst[d] = ((float)sRGB8_LinRGB12[pSrc[s+1]]);
					d += m_nPaddedWidth;
					pDst[d] = ((float)sRGB8_LinRGB12[pSrc[s+2]]);
				}
			}
			pDst += 3*m_nPaddedWidth;
			pSrc += nSrcLineWidthPadded;
		}
	}
}

CFloatImage::~CFloatImage(void) {
	if (m_pMemory != NULL) {
		// free the memory pages
		::VirtualFree(m_pMemory, 0, MEM_RELEASE);
		m_pMemory = NULL;
	}
}

void* CFloatImage::ConvertToDIBRGBA() const {
	if (m_pMemory == NULL)	{
		return NULL;
	}
	uint8* pDIB = new uint8[m_nWidth*4 * m_nHeight];
	
	uint16* pSrc = (uint16*) m_pMemory;
	uint8* pDst = pDIB;
	for (int j = 0; j < m_nHeight; j++) {
		for (int i = 0; i < m_nWidth; i++) {
			int d = i*4;
			int s = i;
			pDst[d] = (uint8)(pSrc[s] >> 6);
			s += m_nPaddedWidth;
			pDst[d+1] = (uint8)(pSrc[s] >> 6);
			s += m_nPaddedWidth;
			pDst[d+2] = (uint8)(pSrc[s] >> 6);
			pDst[d+3] = 0xFF;
		}
		pSrc += 3*m_nPaddedWidth;
		pDst += m_nWidth*4;
	}

	return pDIB;
}

/////////////////////////////////////////////////////////////////////////////////////////
// Private
/////////////////////////////////////////////////////////////////////////////////////////

void CFloatImage::Init(int nWidth, int nHeight, bool bPadHeight, int padding) {
	// pad scanlines
	m_nPaddedWidth = Helpers::DoPadding(nWidth, padding);
	if (bPadHeight) {
		m_nPaddedHeight = Helpers::DoPadding(nHeight, padding);
	} else {
		m_nPaddedHeight = nHeight;
	}
	m_nWidth = nWidth;
	m_nHeight = nHeight;
	// source would have (m_nPaddedWidth * 1((Bytes/ChannelPixel)*3 ChannelPixels)) * 1 (SingleComponentLines/SourceLine)
	//int nMemSize = GetMemSize();	// = (m_nPaddedWidth * 2(Bytes/ChannelPixel)) * (m_nPaddedHeight * 3(SingleComponentLines/SourceLine));
	int nMemSize = GetMemSize();	// = (m_nPaddedWidth * 4(Bytes/ChannelPixel)) * (m_nPaddedHeight * 3(SingleComponentLines/SourceLine));

	// Allocate memory aligned on page boundaries
	m_pMemory = ::VirtualAlloc(
						NULL,	  // let the call determine the start address
						nMemSize, // the size
						MEM_RESERVE | MEM_COMMIT,	// I want that memory, now
						PAGE_READWRITE);			// need both read and write
}
