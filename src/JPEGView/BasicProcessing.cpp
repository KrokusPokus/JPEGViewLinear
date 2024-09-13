#include "StdAfx.h"
#include "BasicProcessing.h"
#include "ResizeFilter.h"
#include "XMMImage.h"
#include "Helpers.h"
#include "WorkThread.h"
#include "ProcessingThreadPool.h"
#ifdef _WIN64
#include "ApplyFilterAVX.h"
#endif
#include "stdint.h"
#include <math.h>
#include <emmintrin.h>
#include <smmintrin.h>

// This macro allows for aligned definition of a 16 byte value with initialization of the 8 components
// to a single value
#define DECLARE_ALIGNED_DQWORD(name, initializer) \
	int16 _tempVal##name[16]; \
	int16* name = (int16*)((((PTR_INTEGRAL_TYPE)&(_tempVal##name) + 15) & ~15)); \
	name[0] = name[1] = name[2] = name[3] = name[4] = name[5] = name[6] = name[7] = initializer;

#define ALPHA_OPAQUE 0xFF000000

// holds last resize timing info
static TCHAR s_TimingInfo[256];

/////////////////////////////////////////////////////////////////////////////////////////////
// Processing images stripwise on thread pool
/////////////////////////////////////////////////////////////////////////////////////////////

// Used in ProcessStrip()
static void* SampleDown_SSE_Core_f32(CSize fullTargetSize, CPoint fullTargetOffset, CSize clippedTargetSize, CSize sourceSize, const void* pIJLPixels, int nChannels, EFilterType eFilter, uint8* pTarget);
static void* SampleDown_AVX_Core_f32(CSize fullTargetSize, CPoint fullTargetOffset, CSize clippedTargetSize, CSize sourceSize, const void* pIJLPixels, int nChannels, EFilterType eFilter, uint8* pTarget);
static void* SampleUp_SSE_Core_f32(CSize fullTargetSize, CPoint fullTargetOffset, CSize clippedTargetSize, CSize sourceSize, const void* pIJLPixels, int nChannels, uint8* pTarget);
static void* SampleUp_AVX_Core_f32(CSize fullTargetSize, CPoint fullTargetOffset, CSize clippedTargetSize, CSize sourceSize, const void* pIJLPixels, int nChannels, uint8* pTarget);

//---------------------------------------------------------------------------------------------

// Request for upsampling or downsampling
class CRequestUpDownSampling : public CProcessingRequest {
public:
	CRequestUpDownSampling(const void* pSourcePixels, CSize sourceSize, void* pTargetPixels,
		CSize fullTargetSize, CPoint fullTargetOffset, CSize clippedTargetSize,
		int nChannels, EFilterType eFilter, CBasicProcessing::SIMDArchitecture simd)
		: CProcessingRequest(pSourcePixels, sourceSize, pTargetPixels, fullTargetSize, fullTargetOffset, clippedTargetSize) {
		Channels = nChannels;
		Filter = eFilter;
		SIMD = simd;
		//StripPadding = (simd == CBasicProcessing::AVX2) ? 16 : 8; // important to set for AVX
		StripPadding = (simd == CBasicProcessing::AVX2) ? 8 : 4; // All slices must have a height dividable by 'StripPadding', except the last one
	}

	virtual bool ProcessStrip(int offsetY, int sizeY)
		{
		if (Filter == Filter_Upsampling_Bicubic)
			{
			if (SIMD == CBasicProcessing::AVX2)
				return NULL != SampleUp_AVX_Core_f32(FullTargetSize, CPoint(FullTargetOffset.x, FullTargetOffset.y + offsetY), CSize(ClippedTargetSize.cx, sizeY), SourceSize, SourcePixels, Channels, (uint8*)TargetPixels + ClippedTargetSize.cx * 4 * offsetY);
			else
				return NULL != SampleUp_SSE_Core_f32(FullTargetSize, CPoint(FullTargetOffset.x, FullTargetOffset.y + offsetY), CSize(ClippedTargetSize.cx, sizeY), SourceSize, SourcePixels, Channels, (uint8*)TargetPixels + ClippedTargetSize.cx * 4 * offsetY);
			}
		else
			{
			if (SIMD == CBasicProcessing::AVX2)
				return NULL != SampleDown_AVX_Core_f32(FullTargetSize, CPoint(FullTargetOffset.x, FullTargetOffset.y + offsetY), CSize(ClippedTargetSize.cx, sizeY), SourceSize, SourcePixels, Channels, Filter, (uint8*)TargetPixels + ClippedTargetSize.cx * 4 * offsetY);
			else
				return NULL != SampleDown_SSE_Core_f32(FullTargetSize, CPoint(FullTargetOffset.x, FullTargetOffset.y + offsetY), CSize(ClippedTargetSize.cx, sizeY), SourceSize, SourcePixels, Channels, Filter, (uint8*)TargetPixels + ClippedTargetSize.cx * 4 * offsetY);
			}
		}

	int Channels;
	EFilterType Filter;
	CBasicProcessing::SIMDArchitecture SIMD;
};

/////////////////////////////////////////////////////////////////////////////////////////////
// Conversion and rotation methods
/////////////////////////////////////////////////////////////////////////////////////////////

// Rotate a block of a 32 bit DIB from source to target by 90 or 270 degrees
static void RotateBlock32bpp(const uint32* pSrc, uint32* pTgt, int nWidth, int nHeight,
							 int nXStart, int nYStart, int nBlockWidth, int nBlockHeight, bool bCW) {
	int nIncTargetLine = nHeight;
	int nIncSource = nWidth - nBlockWidth;
	const uint32* pSource = pSrc + nWidth * nYStart + nXStart;
	uint32* pTarget = bCW ? pTgt + nIncTargetLine * nXStart + (nHeight - 1 - nYStart) :
		pTgt + nIncTargetLine * (nWidth - 1 - nXStart) + nYStart;
	uint32* pStartYPtr = pTarget;
	if (!bCW) nIncTargetLine = -nIncTargetLine;
	int nIncStartYPtr = bCW ? -1 : +1;

	for (uint32 i = 0; i < nBlockHeight; i++) {
		for (uint32 j = 0; j < nBlockWidth; j++) {
			*pTarget = *pSource;
			pTarget += nIncTargetLine;
			pSource++;
		}
		pStartYPtr += nIncStartYPtr;
		pTarget = pStartYPtr;
		pSource += nIncSource;
	}
}

// 180 degrees rotation
static void* Rotate32bpp180(int nWidth, int nHeight, const void* pDIBPixels) {
	uint32* pTarget = new(std::nothrow) uint32[nWidth * nHeight];
	if (pTarget == NULL) return NULL;
	const uint32* pSource = (uint32*)pDIBPixels;
	for (uint32 i = 0; i < nHeight; i++) {
		uint32* pTgt = pTarget + nWidth*(nHeight - 1 - i) + (nWidth - 1);
		const uint32* pSrc = pSource + nWidth*i;
		for (uint32 j = 0; j < nWidth; j++) {
			*pTgt = *pSrc;
			pTgt -= 1;
			pSrc += 1;
		}
	}
	return pTarget;
}

void* CBasicProcessing::Rotate32bpp(int nWidth, int nHeight, const void* pDIBPixels, int nRotationAngleCW) {
	if (pDIBPixels == NULL) {
		return NULL;
	}
	if (nRotationAngleCW != 90 && nRotationAngleCW != 180 && nRotationAngleCW != 270) {
		return NULL; // not allowed
	} else if (nRotationAngleCW == 180) {
		return Rotate32bpp180(nWidth, nHeight, pDIBPixels);
	}

	uint32* pTarget = new(std::nothrow) uint32[nHeight * nWidth];
	if (pTarget == NULL) return NULL;
	const uint32* pSource = (uint32*)pDIBPixels;

	const int cnBlockSize = 32;
	int nX = 0, nY = 0;
	while (nY < nHeight) {
		nX = 0;
		while (nX < nWidth) {
			RotateBlock32bpp(pSource, pTarget, nWidth, nHeight,
				nX, nY,
				min(cnBlockSize, nWidth - nX),
				min(cnBlockSize, nHeight - nY), nRotationAngleCW == 90);
			nX += cnBlockSize;
		}
		nY += cnBlockSize;
	}

	return pTarget;
}

void* CBasicProcessing::MirrorH32bpp(int nWidth, int nHeight, const void* pDIBPixels) {
	uint32* pTarget = new(std::nothrow) uint32[nWidth * nHeight];
	if (pTarget == NULL) return NULL;
	uint32* pTgt = pTarget;
	for (int j = 0; j < nHeight; j++) {
		const uint32* pSource = (uint32*)pDIBPixels + (j + 1) * nWidth - 1;
		for (int i = 0; i < nWidth; i++) {
			*pTgt = *pSource;
			pTgt++; pSource--;
		}
	}
	return pTarget;
}

void* CBasicProcessing::MirrorV32bpp(int nWidth, int nHeight, const void* pDIBPixels) {
	uint32* pTarget = new(std::nothrow) uint32[nWidth * nHeight];
	if (pTarget == NULL) return NULL;
	uint32* pTgt = pTarget;
	for (int j = 0; j < nHeight; j++) {
		const uint32* pSource = (uint32*)pDIBPixels + (nHeight - 1 - j) * nWidth;
		for (int i = 0; i < nWidth; i++) {
			*pTgt = *pSource;
			pTgt++; pSource++;
		}
	}
	return pTarget;
}

void CBasicProcessing::MirrorVInplace(int nWidth, int nHeight, int nStride, void* pDIBPixels) {
	for (int j = 0; j < (nHeight >> 1); j++) {
		if ((nStride & 3) == 0) {
			uint32 nPixelsPerStride = nStride >> 2;
			uint32* pSource = (uint32*)pDIBPixels + (nHeight - 1 - j) * nPixelsPerStride;
			uint32* pTgt = (uint32*)pDIBPixels + j * nPixelsPerStride;
			for (int i = 0; i < nPixelsPerStride; i++) {
				uint32 t = *pTgt;
				*pTgt = *pSource;
				*pSource = t;
				pTgt++; pSource++;
			}
		} else {
			uint8* pSource = (uint8*)pDIBPixels + (nHeight - 1 - j) * nStride;
			uint8* pTgt = (uint8*)pDIBPixels + j * nStride;
			for (int i = 0; i < nStride; i++) {
				uint8 t = *pTgt;
				*pTgt = *pSource;
				*pSource = t;
				pTgt++; pSource++;
			}
		}
	}
}

void* CBasicProcessing::Convert8bppTo32bppDIB(int nWidth, int nHeight, const void* pDIBPixels, const uint8* pPalette) {
	if (pDIBPixels == NULL || pPalette == NULL) {
		return NULL;
	}
	int nPaddedWidthS = Helpers::DoPadding(nWidth, 4);
	int nPadS = nPaddedWidthS - nWidth;
	uint32* pNewDIB = new(std::nothrow) uint32[nWidth * nHeight];
	if (pNewDIB == NULL) return NULL;
	uint32* pTargetDIB = pNewDIB;
	const uint8* pSourceDIB = (uint8*)pDIBPixels;
	for (int j = 0; j < nHeight; j++) {
		for (int i = 0; i < nWidth; i++) {
			*pTargetDIB++ = pPalette[4 * *pSourceDIB] + pPalette[4 * *pSourceDIB + 1] * 256 + pPalette[4 * *pSourceDIB + 2] * 65536 + ALPHA_OPAQUE;
			pSourceDIB ++;
		}
		pSourceDIB += nPadS;
	}
	return pNewDIB;
}

void CBasicProcessing::Convert32bppTo24bppDIB(int nWidth, int nHeight, void* pDIBTarget, 
											  const void* pDIBSource, bool bFlip) {
	if (pDIBTarget == NULL || pDIBSource == NULL) {
		return;
	}
	int nPaddedWidth = Helpers::DoPadding(nWidth*3, 4);
	int nPad = nPaddedWidth - nWidth*3;
	uint8* pTargetDIB = (uint8*)pDIBTarget;
	for (int j = 0; j < nHeight; j++) {
		const uint8* pSourceDIB;
		if (bFlip) {
			pSourceDIB = (uint8*)pDIBSource + (nHeight - 1 - j)*nWidth*4;
		} else {
			pSourceDIB = (uint8*)pDIBSource + j*nWidth*4;
		}
		for (int i = 0; i < nWidth; i++) {
			*pTargetDIB++ = pSourceDIB[0];
			*pTargetDIB++ = pSourceDIB[1];
			*pTargetDIB++ = pSourceDIB[2];
			pSourceDIB += 4;
		}
		pTargetDIB += nPad;
	}
}

void* CBasicProcessing::Convert1To4Channels(int nWidth, int nHeight, const void* pPixels) {
	if (pPixels == NULL) {
		return NULL;
	}
	int nPadSrc = Helpers::DoPadding(nWidth, 4) - nWidth;
	uint32* pNewDIB = new(std::nothrow) uint32[nWidth * nHeight];
	if (pNewDIB == NULL) return NULL;
	uint32* pTarget = pNewDIB;
	const uint8* pSource = (uint8*)pPixels;
	for (int j = 0; j < nHeight; j++) {
		for (int i = 0; i < nWidth; i++) {
			*pTarget++ = *pSource + *pSource * 256 + *pSource * 65536 + ALPHA_OPAQUE;
			pSource++;
		}
		pSource += nPadSrc;
	}
	return pNewDIB;
}

void* CBasicProcessing::Convert16bppGrayTo32bppDIB(int nWidth, int nHeight, const int16* pPixels) {
	uint32* pNewDIB = new(std::nothrow) uint32[nWidth * nHeight];
	if (pNewDIB == NULL) return NULL;
	uint32* pTarget = pNewDIB;
	const int16* pSource = pPixels;
	for (int j = 0; j < nHeight; j++) {
		for (int i = 0; i < nWidth; i++) {
			int nSourceValue = *pSource++ >> 6; // from 14 to 8 bits
			*pTarget++ = nSourceValue + nSourceValue * 256 + nSourceValue * 65536 + ALPHA_OPAQUE;
		}
	}
	return pNewDIB;
}


void* CBasicProcessing::Convert3To4Channels(int nWidth, int nHeight, const void* pIJLPixels) {
	if (pIJLPixels == NULL) {
		return NULL;
	}
	int nPadSrc = Helpers::DoPadding(nWidth*3, 4) - nWidth*3;
	uint32* pNewDIB = new(std::nothrow) uint32[nWidth * nHeight];
	if (pNewDIB == NULL) return NULL;
	uint32* pTarget = pNewDIB;
	const uint8* pSource = (uint8*)pIJLPixels;
	for (int j = 0; j < nHeight; j++) {
		for (int i = 0; i < nWidth; i++) {
			*pTarget++ = pSource[0] + pSource[1] * 256 + pSource[2] * 65536 + ALPHA_OPAQUE;
			pSource += 3;
		}
		pSource += nPadSrc;
	}
	return pNewDIB;
}

void* CBasicProcessing::ConvertGdiplus32bppRGB(int nWidth, int nHeight, int nStride, const void* pGdiplusPixels) {
	if (pGdiplusPixels == NULL || nWidth*4 > abs(nStride)) {
		return NULL;
	}
	uint32* pNewDIB = new(std::nothrow) uint32[nWidth * nHeight];
	if (pNewDIB == NULL) return NULL;
	uint32* pTgt = pNewDIB;
	const uint8* pSrc = (const uint8*)pGdiplusPixels;
	for (int j = 0; j < nHeight; j++) {
		for (int i = 0; i < nWidth; i++)
			pTgt[i] = ((uint32*)pSrc)[i] | ALPHA_OPAQUE;
		pTgt += nWidth;
		pSrc += nStride;
	}
	return pNewDIB;
}

void* CBasicProcessing::CopyRect32bpp(void* pTarget, const void* pSource,  CSize targetSize, CRect targetRect,
									  CSize sourceSize, CRect sourceRect) {
	if (pSource == NULL || sourceRect.Size() != targetRect.Size() || 
		sourceRect.left < 0 || sourceRect.right > sourceSize.cx ||
		sourceRect.top < 0 || sourceRect.bottom > sourceSize.cy ||
		targetRect.left < 0 || targetRect.right > targetSize.cx ||
		targetRect.top < 0 || targetRect.bottom > targetSize.cy) {
		return NULL;
	}
	uint32* pSourceDIB = (uint32*)pSource;
	uint32* pTargetDIB = (uint32*)pTarget;
	if (pTargetDIB == NULL) {
		pTargetDIB = new(std::nothrow) uint32[targetSize.cx * targetSize.cy];
		if (pTargetDIB == NULL) return NULL;
		pTarget = pTargetDIB;
	}

	pTargetDIB += (targetRect.top * targetSize.cx) + targetRect.left;
	pSourceDIB += (sourceRect.top * sourceSize.cx) + sourceRect.left;
	for (int y = 0; y < sourceRect.Height(); y++) {
		memcpy(pTargetDIB, pSourceDIB, sourceRect.Width()*sizeof(uint32));
		pTargetDIB += targetSize.cx;
		pSourceDIB += sourceSize.cx;
	}

	return pTarget;
}

/////////////////////////////////////////////////////////////////////////////////////////////
// Simple point sampling resize and rotation methods
/////////////////////////////////////////////////////////////////////////////////////////////

void* CBasicProcessing::PointSample(CSize fullTargetSize, CPoint fullTargetOffset, CSize clippedTargetSize, 
	CSize sourceSize, const void* pPixels, int nChannels) {
	if (fullTargetSize.cx < 1 || fullTargetSize.cy < 1 ||
		clippedTargetSize.cx < 1 || clippedTargetSize.cy < 1 ||
		fullTargetOffset.x < 0 || fullTargetOffset.x < 0 ||
		clippedTargetSize.cx + fullTargetOffset.x > fullTargetSize.cx ||
		clippedTargetSize.cy + fullTargetOffset.y > fullTargetSize.cy ||
		pPixels == NULL || (nChannels != 3 && nChannels != 4)) {
		return NULL;
	}

	uint8* pDIB = new(std::nothrow) uint8[clippedTargetSize.cx*4 * clippedTargetSize.cy];
	if (pDIB == NULL) return NULL;

	uint32 nIncrementX, nIncrementY;
	if (fullTargetSize.cx <= sourceSize.cx) {
		// Downsampling
		nIncrementX = (uint32)(sourceSize.cx << 16)/fullTargetSize.cx + 1;
		nIncrementY = (uint32)(sourceSize.cy << 16)/fullTargetSize.cy + 1;
	} else {
		// Upsampling
		nIncrementX = (fullTargetSize.cx == 1) ? 0 : (uint32)((65536*(uint32)(sourceSize.cx - 1) + 65535)/(fullTargetSize.cx - 1));
		nIncrementY = (fullTargetSize.cy == 1) ? 0 : (uint32)((65536*(uint32)(sourceSize.cy - 1) + 65535)/(fullTargetSize.cy - 1));
	}

	int nPaddedSourceWidth = Helpers::DoPadding(sourceSize.cx * nChannels, 4);
	const uint8* pSrc = NULL;
	uint8* pDst = pDIB;
	uint32 nCurY = fullTargetOffset.y*nIncrementY;
	uint32 nStartX = fullTargetOffset.x*nIncrementX;
	for (int j = 0; j < clippedTargetSize.cy; j++) {
		pSrc = (uint8*)pPixels + nPaddedSourceWidth * (nCurY >> 16);
		uint32 nCurX = nStartX;
		if (nChannels == 3) {
			for (int i = 0; i < clippedTargetSize.cx; i++) {
				uint32 sx = nCurX >> 16; 
				uint32 s = sx*3;
				uint32 d = i*4;
				pDst[d] = pSrc[s];
				pDst[d+1] = pSrc[s+1];
				pDst[d+2] = pSrc[s+2];
				pDst[d+3] = 0xFF;
				nCurX += nIncrementX;
			}
		} else {
			for (int i = 0; i < clippedTargetSize.cx; i++) {
				uint32 sx = nCurX >> 16; 
				((uint32*)pDst)[i] = ((uint32*)pSrc)[sx];
				nCurX += nIncrementX;
			}
		}
		pDst += clippedTargetSize.cx*4;
		nCurY += nIncrementY;
	}
	return pDIB;
}

static void RotateInplace(double& dX, double& dY, double dAngle) {
	double dXr = cos(dAngle) * dX - sin(dAngle) * dY;
	double dYr = sin(dAngle) * dX + cos(dAngle) * dY;
	dX = dXr;
	dY = dYr;
}

/////////////////////////////////////////////////////////////////////////////////////////////
// Helper methods for high quality resizing (C++ implementation)
/////////////////////////////////////////////////////////////////////////////////////////////

// Apply filtering in x-direction and rotate
// nSourceWidth: Width of the image in pSource in pixels
// nTargetWidth: Target width of the image after filtering.
// nHeight: Height of the target image in pixels
// nSourceBytesPerPixel: 3 or 4, the target will always have 4 bytes per pixel
// nStartX_FP: 16.16 fixed point number, start of filtering in source image
// nStartY: First row to filter in source image (Offset, not in FP format)
// nIncrementX_FP: 16.16 fixed point number, increment in x-direction in source image
// filter: Filter to apply (in x direction)
// nFilterOffset: Offset into filter (to filter.Indices array)
// pSource: Source image
// Returns the filtered image of size(nHeight, nTargetWidth)
static uint8* ApplyFilter(int nSourceWidth, int nTargetWidth, int nHeight,
						  int nSourceBytesPerPixel,
						  int nStartX_FP, int nStartY, int nIncrementX_FP,
						  const FilterKernelBlock& filter,
						  int nFilterOffset,
						  const uint8* pSource) {

	uint8* pTarget = new(std::nothrow) uint8[nTargetWidth*4*nHeight];
	if (pTarget == NULL) return NULL;

	// width of new image is (after rotation) : nHeight
	// height of new image is (after rotation) : nTargetWidth
	
	int nPaddedSourceWidth = Helpers::DoPadding(nSourceWidth * nSourceBytesPerPixel, 4);
	const uint8* pSourcePixelLine = NULL;
	uint8* pTargetPixelLine = NULL;
	const int FP_05 = 255; // rounding correction because in filter 1.0 is 16383 but we shift by 14 what is a division by 16384
	for (int j = 0; j < nHeight; j++) {
		pSourcePixelLine = ((uint8*) pSource) + nPaddedSourceWidth * (j + nStartY);
		pTargetPixelLine = pTarget + 4*j;
		uint8* pTargetPixel = pTargetPixelLine;
		uint32 nX = nStartX_FP;
		for (int i = 0; i < nTargetWidth; i++) {
			uint32 nXSourceInt = nX >> 16;
			FilterKernel* pKernel = filter.Indices[i + nFilterOffset];
			const uint8* pSourcePixel = pSourcePixelLine + nSourceBytesPerPixel*(nXSourceInt - pKernel->FilterOffset);
			int nPixelValue1 = 0;
			int nPixelValue2 = 0;
			int nPixelValue3 = 0;
			for (int n = 0; n < pKernel->FilterLen; n++) {
				nPixelValue1 += pKernel->Kernel[n] * pSourcePixel[0];
				nPixelValue2 += pKernel->Kernel[n] * pSourcePixel[1];
				nPixelValue3 += pKernel->Kernel[n] * pSourcePixel[2];
				pSourcePixel += nSourceBytesPerPixel;
			}
			nPixelValue1 = (nPixelValue1 + FP_05) >> 14;
			nPixelValue2 = (nPixelValue2 + FP_05) >> 14;
			nPixelValue3 = (nPixelValue3 + FP_05) >> 14;

			*pTargetPixel++ = (uint8)max(0, min(255, nPixelValue1));
			*pTargetPixel++ = (uint8)max(0, min(255, nPixelValue2));
			*pTargetPixel++ = (uint8)max(0, min(255, nPixelValue3));
			*pTargetPixel++ = 0xFF;
			// rotate: go to next row in target - width of target is nHeight
			pTargetPixel = pTargetPixel - 4 + nHeight*4;
			nX += nIncrementX_FP;
		}
	}

	return pTarget;
}

/////////////////////////////////////////////////////////////////////////////////////////////
// High quality upsampling (C++ implementation)
// Used with generic CPUs where no SIMD extensions are available (SSE and AVX2).
// This will NOT use linear color space! (it would need INT64 or FLOAT!)
// This WILL still use a Cubic kernel.
/////////////////////////////////////////////////////////////////////////////////////////////

void* CBasicProcessing::SampleUp(CSize fullTargetSize, CPoint fullTargetOffset, CSize clippedTargetSize,
	CSize sourceSize, const void* pPixels, int nChannels) {

	// Resizing consists of resize in x direction followed by resize in y direction.
	// To simplify implementation, the method performs a 90 degree rotation/flip while resizing,
	// thus enabling to use the same loop on the rows for both resize directions.
	// The image is correctly orientated again after two rotation/flip operations!

	if (pPixels == NULL || fullTargetSize.cx < 2 || fullTargetSize.cy < 2 || clippedTargetSize.cx <= 0 || clippedTargetSize.cy <= 0) {
		return NULL;
	}
	int nTargetWidth = clippedTargetSize.cx;
	int nTargetHeight = clippedTargetSize.cy;
	int nSourceWidth = sourceSize.cx;
	int nSourceHeight = sourceSize.cy;

	uint32 nIncrementX = (uint32)(65536*(uint32)(nSourceWidth - 1)/(fullTargetSize.cx - 1));
	uint32 nIncrementY = (uint32)(65536*(uint32)(nSourceHeight - 1)/(fullTargetSize.cy - 1));

	// Caution: This code assumes a upsampling filter kernel of length 4, with a filter offset of 1
	int nFirstY = max(0, int((uint32)(nIncrementY*fullTargetOffset.y) >> 16) - 1);
	int nLastY = min(sourceSize.cy - 1, int(((uint32)(nIncrementY*(fullTargetOffset.y + nTargetHeight - 1)) >> 16) + 2));
	int nTempTargetWidth = nLastY - nFirstY + 1;
	int nTempTargetHeight = nTargetWidth;
	int nFilterOffsetX = fullTargetOffset.x;
	int nFilterOffsetY = fullTargetOffset.y;
	int nStartX = nIncrementX*fullTargetOffset.x;
	int nStartY = nIncrementY*fullTargetOffset.y - 65536*nFirstY;

	CResizeFilter filterX(nSourceWidth, fullTargetSize.cx, Filter_Upsampling_Bicubic, FilterSIMDType_None);
	const FilterKernelBlock& kernelsX = filterX.GetFilterKernels();

	uint8* pTemp = ApplyFilter(nSourceWidth, nTempTargetHeight, nTempTargetWidth,
		nChannels, nStartX, nFirstY, nIncrementX,
		kernelsX, nFilterOffsetX, (const uint8*)pPixels);
	if (pTemp == NULL) return NULL;

	CResizeFilter filterY(nSourceHeight, fullTargetSize.cy, Filter_Upsampling_Bicubic, FilterSIMDType_None);
	const FilterKernelBlock& kernelsY = filterY.GetFilterKernels();

	uint8* pDIB = ApplyFilter(nTempTargetWidth, nTargetHeight, nTargetWidth,
			4, nStartY, 0, nIncrementY,
			kernelsY, nFilterOffsetY, pTemp);

	delete[] pTemp;

	return pDIB;
}


/////////////////////////////////////////////////////////////////////////////////////////////////////////
// High quality downsampling (C++ implementation)
// Used with generic CPUs where no SIMD extensions are available (SSE and AVX2).
// This will NOT use linear color space! (it would need INT64 or FLOAT!)
// This WILL still use Hermite/Mitchell/Catrom/Lanczos2 kernel as specified.
/////////////////////////////////////////////////////////////////////////////////////////////////////////

void* CBasicProcessing::SampleDown(CSize fullTargetSize, CPoint fullTargetOffset, CSize clippedTargetSize,
	CSize sourceSize, const void* pPixels, int nChannels, EFilterType eFilter) {
	// Resizing consists of resize in x direction followed by resize in y direction.
	// To simplify implementation, the method performs a 90 degree rotation/flip while resizing,
	// thus enabling to use the same loop on the rows for both resize directions.
	// The image is correctly orientated again after two rotation/flip operations!

	if (pPixels == NULL || clippedTargetSize.cx <= 0 || clippedTargetSize.cy <= 0) {
		return NULL;
	}
	CResizeFilter filterX(sourceSize.cx, fullTargetSize.cx, eFilter, FilterSIMDType_None);
	const FilterKernelBlock& kernelsX = filterX.GetFilterKernels();
	CResizeFilter filterY(sourceSize.cy, fullTargetSize.cy, eFilter, FilterSIMDType_None);
	const FilterKernelBlock& kernelsY = filterY.GetFilterKernels();

	uint32 nIncrementX = (uint32)(sourceSize.cx << 16)/fullTargetSize.cx + 1;
	uint32 nIncrementY = (uint32)(sourceSize.cy << 16)/fullTargetSize.cy + 1;

	int nIncOffsetX = (nIncrementX - 65536) >> 1;
	int nIncOffsetY = (nIncrementY - 65536) >> 1;
	int nFirstY = (uint32)(nIncOffsetY + nIncrementY*fullTargetOffset.y) >> 16;
	nFirstY = max(0, nFirstY - kernelsY.Indices[fullTargetOffset.y]->FilterOffset);
	int nLastY  = (uint32)(nIncOffsetY + nIncrementY*(fullTargetOffset.y + clippedTargetSize.cy - 1)) >> 16;
	FilterKernel* pLastYFilter = kernelsY.Indices[fullTargetOffset.y + clippedTargetSize.cy - 1];
	nLastY  = min(sourceSize.cy - 1, nLastY - pLastYFilter->FilterOffset + pLastYFilter->FilterLen - 1);
	int nTempTargetWidth = nLastY - nFirstY + 1;
	int nTempTargetHeight = clippedTargetSize.cx;
	int nFilterOffsetX = fullTargetOffset.x;
	int nFilterOffsetY = fullTargetOffset.y;
	int nStartX = nIncOffsetX + nIncrementX*fullTargetOffset.x;
	int nStartY = nIncOffsetY + nIncrementY*fullTargetOffset.y - 65536*nFirstY;

	uint8* pTemp = ApplyFilter(sourceSize.cx, nTempTargetHeight, nTempTargetWidth, nChannels, nStartX, nFirstY, nIncrementX, kernelsX, nFilterOffsetX, (const uint8*)pPixels);

	if (pTemp == NULL)
		return NULL;

	uint8* pDIB = ApplyFilter(nTempTargetWidth, clippedTargetSize.cy, clippedTargetSize.cx, 4, nStartY, 0, nIncrementY, kernelsY, nFilterOffsetY, pTemp);

	delete[] pTemp;

	return pDIB;
}

/////////////////////////////////////////////////////////////////////////////////////////////
// High quality downsampling (Helpers for SSE implementation)
/////////////////////////////////////////////////////////////////////////////////////////////

// Rotates a line of 'simdPixelsPerRegister' pixels from source to targt
inline static const int16* RotateLine(const int16* pSource, int16* pTarget, int nIncTargetLine, int simdPixelsPerRegister) {
	for (int i = 0; i < simdPixelsPerRegister - 1; i++)
	{
		*pTarget = *pSource++; pTarget += nIncTargetLine;
	}
	*pTarget = *pSource++;

	return pSource;
}

inline static const int16* RotateLineToDIB_1(const int16* pSource, uint8* pTarget, int nIncTargetLine, int simdPixelsPerRegister) {
	const int FP_05 = 42; // 0.5 (actually a bit more) in fixed point, improves rounding
	for (int i = 0; i < simdPixelsPerRegister - 1; i++)
	{
		*((uint32*)pTarget) = ALPHA_OPAQUE | ((*pSource + FP_05) >> 6); pSource++;	pTarget += nIncTargetLine;
	}
	*((uint32*)pTarget) = ALPHA_OPAQUE | ((*pSource + FP_05) >> 6); pSource++;

	return pSource;
}

inline static const int16* RotateLineToDIB(const int16* pSource, uint8* pTarget, int nIncTargetLine, int simdPixelsPerRegister) {
	const int FP_05 = 42;
	for (int i = 0; i < simdPixelsPerRegister - 1; i++)
	{
		*pTarget = (*pSource++ + FP_05) >> 6; pTarget += nIncTargetLine;
	}
	*pTarget = (*pSource++ + FP_05) >> 6;

	return pSource;
}

// Rotate a block in a CFloatImage. Blockwise rotation is needed because with normal
// rotation, trashing occurs, making rotation a very slow operation.
// The input format is what the ResizeYCore() method outputs:
// RRRRRRRRGGGGGGGGBBBBBBBB... (blocks of 'simdPixelsPerRegister' pixels of a channel).
// After rotation, the format is line interleaved again:
// RRRRRRRRRRR...
// GGGGGGGGGGG...
// BBBBBBBBBBB...
static void RotateBlock(const int16* pSrc, int16* pTgt, int nWidth, int nHeight,
						int nXStart, int nYStart, int nBlockWidth, int nBlockHeight,
						int simdPixelsPerRegister) {
	int nPaddedWidth = Helpers::DoPadding(nWidth, simdPixelsPerRegister);
	int nPaddedHeight = Helpers::DoPadding(nHeight, simdPixelsPerRegister);
	int nIncTargetChannel = nPaddedHeight;
	int nIncTargetLine = nIncTargetChannel * 3;
	int nIncSource = nPaddedWidth * 3 - nBlockWidth * 3;
	const int16* pSource = pSrc + nPaddedWidth * 3 * nYStart + nXStart * 3;
	int16* pTarget = pTgt + nPaddedHeight * 3 * nXStart + nYStart;
	int16* pStartYPtr = pTarget;
	int nLoopX = Helpers::DoPadding(nBlockWidth, simdPixelsPerRegister) / simdPixelsPerRegister;
	int nTargetIncrement = ((simdPixelsPerRegister - 1) * nIncTargetLine) + nIncTargetChannel;

	for (int i = 0; i < nBlockHeight; i++) {
		for (int j = 0; j < nLoopX; j++) {
			pSource = RotateLine(pSource, pTarget, nIncTargetLine, simdPixelsPerRegister);
			pTarget += nIncTargetChannel;
			pSource =  RotateLine(pSource, pTarget, nIncTargetLine, simdPixelsPerRegister);
			pTarget += nIncTargetChannel;
			pSource =  RotateLine(pSource, pTarget, nIncTargetLine, simdPixelsPerRegister);
			pTarget += nTargetIncrement;
		}
		pStartYPtr++;
		pTarget = pStartYPtr;
		pSource += nIncSource;
	}
}

// Same as above, directly rotates into a 32 bpp DIB
static void RotateBlockToDIB(const int16* pSrc, uint8* pTgt, int nWidth, int nHeight,
							 int nXStart, int nYStart, int nBlockWidth, int nBlockHeight,
							 int simdPixelsPerRegister) {
	int nPaddedWidth = Helpers::DoPadding(nWidth, simdPixelsPerRegister);
	int nPaddedHeight = Helpers::DoPadding(nHeight, simdPixelsPerRegister);
	int nIncTargetLine = nHeight * 4;
	int nIncSource = nPaddedWidth * 3 - nBlockWidth * 3;
	const int16* pSource = pSrc + nPaddedWidth * 3 * nYStart + nXStart * 3;
	uint8* pTarget = pTgt + nHeight * 4 * nXStart + nYStart * 4;
	uint8* pStartYPtr = pTarget;
	int nLoopX = Helpers::DoPadding(nBlockWidth, simdPixelsPerRegister) / simdPixelsPerRegister;
	int nTargetIncrement = simdPixelsPerRegister * nIncTargetLine - 2;

	for (int i = 0; i < nBlockHeight; i++) {
		for (int j = 0; j < nLoopX; j++) {
			pSource = RotateLineToDIB_1(pSource, pTarget, nIncTargetLine, simdPixelsPerRegister);
			pTarget++;
			pSource = RotateLineToDIB(pSource, pTarget, nIncTargetLine, simdPixelsPerRegister);
			pTarget++;
			pSource = RotateLineToDIB(pSource, pTarget, nIncTargetLine, simdPixelsPerRegister);
			pTarget += nTargetIncrement;
		}
		pStartYPtr += 4;
		pTarget = pStartYPtr;
		pSource += nIncSource;
	}
}

// RotateFlip the source image by 90 deg and return rotated image
// RotateFlip is invertible: img = RotateFlip(RotateFlip(img))
static CFloatImage* Rotate(const CFloatImage* pSourceImg, int simdPixelsPerRegister) {
	CFloatImage* targetImage = new CFloatImage(pSourceImg->GetHeight(), pSourceImg->GetWidth(), true, simdPixelsPerRegister);
	if (targetImage->AlignedPtr() == NULL) {
		delete targetImage;
		return NULL;
	}
	const int16* pSource = (const int16*) pSourceImg->AlignedPtr();
	int16* pTarget = (int16*) targetImage->AlignedPtr();

	const int cnBlockSize = 32;
	int nX = 0, nY = 0;
	while (nY < pSourceImg->GetHeight()) {
		nX = 0;
		while (nX < pSourceImg->GetWidth()) {
			RotateBlock(pSource, pTarget, pSourceImg->GetWidth(), pSourceImg->GetHeight(),
				nX, nY, 
				min(cnBlockSize, pSourceImg->GetPaddedWidth() - nX), // !! here we need to use the padded width
				min(cnBlockSize, pSourceImg->GetHeight() - nY),
				simdPixelsPerRegister);
			nX += cnBlockSize;
		}
		nY += cnBlockSize;
	}

	return targetImage;
}

// RotateFlip the source image by 90 deg and return rotated image as 32 bpp DIB
static void* RotateToDIB(const CFloatImage* pSourceImg, int simdPixelsPerRegister, uint8* pTarget = NULL) {

	const int16* pSource = (const int16*) pSourceImg->AlignedPtr();
	if (pTarget == NULL) {
		pTarget = new(std::nothrow) uint8[pSourceImg->GetHeight() * 4 * Helpers::DoPadding(pSourceImg->GetWidth(), simdPixelsPerRegister)];
		if (pTarget == NULL) return NULL;
	}

	const int cnBlockSize = 32;
	int nX = 0, nY = 0;
	while (nY < pSourceImg->GetHeight()) {
		nX = 0;
		while (nX < pSourceImg->GetWidth()) {
			RotateBlockToDIB(pSource, pTarget, pSourceImg->GetWidth(), pSourceImg->GetHeight(),
				nX, nY, 
				min(cnBlockSize, pSourceImg->GetPaddedWidth() - nX),  // !! here we need to use the padded width
				min(cnBlockSize, pSourceImg->GetHeight() - nY),
				simdPixelsPerRegister);

			nX += cnBlockSize;
		}
		nY += cnBlockSize;
	}

	return pTarget;
}

/////////////////////////////////////////////////////////////////////////////////////////////
// High quality filtering (SSE implementation)
/////////////////////////////////////////////////////////////////////////////////////////////

#ifdef _WIN64
// Apply filter in y direction in SSE
// No inline assembly is supported in 64 bit mode, thus intrinsics are used instead.
// nSourceHeight: Height of source image, only here to match interface of C++ implementation
// nTargetHeight: Height of target image after resampling
// nWidth: Width of source image
// nStartY_FP: 16.16 fixed point number, denoting the y-start subpixel coordinate
// nStartX: Start of filtering in x-direction (not an FP number)
// nIncrementY_FP: 16.16 fixed point number, denoting the increment for the y coordinates
// filter: filter to apply
// nFilterOffset: Offset into filter (to filter.Indices array)
// pSourceImg: Source image
// The filter is applied to 'nTargetHeight' rows of the input image at positions
// nStartY_FP, nStartY_FP+nIncrementY_FP, ... 
// In X direction, the filter is applied starting from column /nStartX\ to (including) \nStartX+nWidth-1/
// where the /\ symbol denotes padding to lower 8 pixel boundary and \/ padding to the upper 8 pixel
// boundary.
static CFloatImage* ApplyFilter_SSE(int nSourceHeight, int nTargetHeight, int nWidth,
	int nStartY_FP, int nStartX, int nIncrementY_FP,
	const SSEFilterKernelBlock& filter,
	int nFilterOffset, const CFloatImage* pSourceImg) {

	int nStartXAligned = nStartX & ~7;
	int nEndXAligned = (nStartX + nWidth + 7) & ~7;
	CFloatImage* tempImage = new CFloatImage(nEndXAligned - nStartXAligned, nTargetHeight, 8);
	if (tempImage->AlignedPtr() == NULL) {
		delete tempImage;
		return NULL;
	}

	int nCurY = nStartY_FP;
	int nChannelLenBytes = pSourceImg->GetPaddedWidth() * sizeof(short);
	int nRowLenBytes = nChannelLenBytes * 3;
	int nNumberOfBlocksX = (nEndXAligned - nStartXAligned) >> 3;
	const uint8* pSourceStart = (const uint8*)pSourceImg->AlignedPtr() + nStartXAligned * sizeof(short);
	SSEFilterKernel** pKernelIndexStart = filter.Indices;

	DECLARE_ALIGNED_DQWORD(ONE_XMM, 16383 - 42); // 1.0 in fixed point notation, minus rounding correction

	__m128i xmm0 = *((__m128i*)ONE_XMM);
	__m128i xmm1 = _mm_setzero_si128();
	__m128i xmm2;
	__m128i xmm3;
	__m128i xmm4 = _mm_setzero_si128();
	__m128i xmm5 = _mm_setzero_si128();
	__m128i xmm6 = _mm_setzero_si128();
	__m128i xmm7;

	__m128i* pDestination = (__m128i*)tempImage->AlignedPtr();

	for (int y = 0; y < nTargetHeight; y++) {
		uint32 nCurYInt = (uint32)nCurY >> 16; // integer part of Y
		int filterIndex = y + nFilterOffset;
		SSEFilterKernel* pKernel = pKernelIndexStart[filterIndex];
		int filterLen = pKernel->FilterLen;
		int filterOffset = pKernel->FilterOffset;
		const __m128i* pFilterStart = (__m128i*)&(pKernel->Kernel);
		const __m128i* pSourceRow = (const __m128i*)(pSourceStart + ((int)nCurYInt - filterOffset) * nRowLenBytes);

		for (int x = 0; x < nNumberOfBlocksX; x++) {
			const __m128i* pSource = pSourceRow;
			const __m128i* pFilter = pFilterStart;
			xmm4 = _mm_setzero_si128();
			xmm5 = _mm_setzero_si128();
			xmm6 = _mm_setzero_si128();
			for (int i = 0; i < filterLen; i++) {
				xmm7 = *pFilter;

				// the pixel data RED channel
				xmm2 = *pSource;
				xmm2 = _mm_add_epi16(xmm2, xmm2);
				xmm2 = _mm_mulhi_epi16(xmm2, xmm7);
				xmm2 = _mm_add_epi16(xmm2, xmm2);
				xmm4 = _mm_adds_epi16(xmm4, xmm2);
				pSource = (__m128i*)((uint8*)pSource + nChannelLenBytes);

				// the pixel data GREEN channel
				xmm3 = *pSource;
				xmm3 = _mm_add_epi16(xmm3, xmm3);
				xmm3 = _mm_mulhi_epi16(xmm3, xmm7);
				xmm3 = _mm_add_epi16(xmm3, xmm3);
				xmm5 = _mm_adds_epi16(xmm5, xmm3);
				pSource = (__m128i*)((uint8*)pSource + nChannelLenBytes);

				// the pixel data BLUE channel
				xmm2 = *pSource;
				xmm2 = _mm_add_epi16(xmm2, xmm2);
				xmm2 = _mm_mulhi_epi16(xmm2, xmm7);
				xmm2 = _mm_add_epi16(xmm2, xmm2);
				xmm6 = _mm_adds_epi16(xmm6, xmm2);
				pSource = (__m128i*)((uint8*)pSource + nChannelLenBytes);

				pFilter++;
			}

			// limit to range 0 (in xmm1), 16383-42 (in xmm0)
			xmm4 = _mm_min_epi16(xmm4, xmm0);
			xmm5 = _mm_min_epi16(xmm5, xmm0);
			xmm6 = _mm_min_epi16(xmm6, xmm0);

			xmm1 = _mm_setzero_si128();

			xmm4 = _mm_max_epi16(xmm4, xmm1);
			xmm5 = _mm_max_epi16(xmm5, xmm1);
			xmm6 = _mm_max_epi16(xmm6, xmm1);

			// store result in blocks
			*pDestination++ = xmm4;
			*pDestination++ = xmm5;
			*pDestination++ = xmm6;

			pSourceRow++;
		};

		nCurY += nIncrementY_FP;
	};

	return tempImage;
}
#endif

void* CBasicProcessing::SampleDown_SIMD(CSize fullTargetSize, CPoint fullTargetOffset, CSize clippedTargetSize,
	CSize sourceSize, const void* pPixels, int nChannels,
	EFilterType eFilter, SIMDArchitecture simd) {	
	if (pPixels == NULL || clippedTargetSize.cx <= 0 || clippedTargetSize.cy <= 0) {
		return NULL;
	}
	int padding = (simd == AVX2) ? 8 : 4;
	uint8* pTarget = new(std::nothrow) uint8[clippedTargetSize.cx * 4 * Helpers::DoPadding(clippedTargetSize.cy, padding)];
	if (pTarget == NULL) return NULL;
	CProcessingThreadPool& threadPool = CProcessingThreadPool::This();
	CRequestUpDownSampling request(pPixels, sourceSize,
		pTarget, fullTargetSize, fullTargetOffset, clippedTargetSize,
		nChannels, eFilter, simd);
	bool bSuccess = threadPool.Process(&request);

	return bSuccess ? pTarget : NULL;
	}

void* CBasicProcessing::SampleUp_SIMD(CSize fullTargetSize, CPoint fullTargetOffset, CSize clippedTargetSize,
	CSize sourceSize, const void* pPixels, int nChannels, SIMDArchitecture simd) {
	if (pPixels == NULL || fullTargetSize.cx < 2 || fullTargetSize.cy < 2 || clippedTargetSize.cx <= 0 || clippedTargetSize.cy <= 0) {
		return NULL;
	}
	int padding = (simd == AVX2) ? 8 : 4;
	uint8* pTarget = new(std::nothrow) uint8[clippedTargetSize.cx * 4 * Helpers::DoPadding(clippedTargetSize.cy, padding)];
	if (pTarget == NULL) return NULL;
	CProcessingThreadPool& threadPool = CProcessingThreadPool::This();
	CRequestUpDownSampling request(pPixels, sourceSize,
		pTarget, fullTargetSize, fullTargetOffset, clippedTargetSize,
		nChannels, Filter_Upsampling_Bicubic, simd);
	bool bSuccess = threadPool.Process(&request);

	return bSuccess ? pTarget : NULL;
	}

LPCTSTR CBasicProcessing::TimingInfo() {
	return s_TimingInfo;
}





//#################################################################################################################################################
// Sampling in linear space
//#################################################################################################################################################

// SSE f32 Implementation

// Rotates a line of 'simdPixelsPerRegister' pixels from source to target
inline static const float* RotateLine_f32(const float* pSource, float* pTarget, int nIncTargetLine, int simdPixelsPerRegister) {
	for (int i = 0; i < simdPixelsPerRegister - 1; i++)
	{
		*pTarget = *pSource++; pTarget += nIncTargetLine;
	}
	*pTarget = *pSource++;

	return pSource;
}

inline static const float* RotateLineToDIB_1_f32(const float* pSource, uint8* pTarget, int nIncTargetLine, int simdPixelsPerRegister) {

	for (int i = 0; i < simdPixelsPerRegister - 1; i++)
	{
		//*((uint32*)pTarget) = ALPHA_OPAQUE | (*pSource / 64);		// ALPHA_OPAQUE 0xFF000000
		*((uint8*)pTarget) = LinRGB12_sRGB8[(INT)(*pSource)]; pSource++; pTarget += nIncTargetLine;
	}

	//*((uint32*)pTarget) = ALPHA_OPAQUE | (*pSource / 64);
	*((uint8*)pTarget) = LinRGB12_sRGB8[(INT)(*pSource)]; pSource++;

	return pSource;
	}

inline static const float* RotateLineToDIB_f32(const float* pSource, uint8* pTarget, int nIncTargetLine, int simdPixelsPerRegister) {

	for (int i = 0; i < simdPixelsPerRegister - 1; i++)
	{
		*pTarget = LinRGB12_sRGB8[(INT)(*pSource++)]; pTarget += nIncTargetLine;
	}
	*pTarget = LinRGB12_sRGB8[(INT)(*pSource++)];

	return pSource;
	}

// Rotate a block in a CFloatImage. Blockwise rotation is needed because with normal
// rotation, trashing occurs, making rotation a very slow operation.
// The input format is what the ResizeYCore() method outputs:
// RRRRRRRRGGGGGGGGBBBBBBBB... (blocks of 'simdPixelsPerRegister' pixels of a channel).
// After rotation, the format is line interleaved again:
// RRRRRRRRRRR...
// GGGGGGGGGGG...
// BBBBBBBBBBB...
static void RotateBlock_f32(const float* pSrc, float* pTgt, int nWidth, int nHeight,
						int nXStart, int nYStart, int nBlockWidth, int nBlockHeight,
						int simdPixelsPerRegister) {
	int nPaddedWidth = Helpers::DoPadding(nWidth, simdPixelsPerRegister);
	int nPaddedHeight = Helpers::DoPadding(nHeight, simdPixelsPerRegister);
	int nIncTargetChannel = nPaddedHeight;
	int nIncTargetLine = nIncTargetChannel * 3;
	int nIncSource = nPaddedWidth * 3 - nBlockWidth * 3;
	const float* pSource = pSrc + nPaddedWidth * 3 * nYStart + nXStart * 3;
	float* pTarget = pTgt + nPaddedHeight * 3 * nXStart + nYStart;
	float* pStartYPtr = pTarget;
	int nLoopX = Helpers::DoPadding(nBlockWidth, simdPixelsPerRegister) / simdPixelsPerRegister;
	int nTargetIncrement = ((simdPixelsPerRegister - 1) * nIncTargetLine) + nIncTargetChannel;

	for (int i = 0; i < nBlockHeight; i++) {
		for (int j = 0; j < nLoopX; j++) {
			pSource = RotateLine_f32(pSource, pTarget, nIncTargetLine, simdPixelsPerRegister);
			pTarget += nIncTargetChannel;
			pSource =  RotateLine_f32(pSource, pTarget, nIncTargetLine, simdPixelsPerRegister);
			pTarget += nIncTargetChannel;
			pSource =  RotateLine_f32(pSource, pTarget, nIncTargetLine, simdPixelsPerRegister);
			pTarget += nTargetIncrement;
		}
		pStartYPtr++;
		pTarget = pStartYPtr;
		pSource += nIncSource;
	}
}

// Same as above, directly rotates into a 32 bpp DIB
static void RotateBlockToDIB_f32(const float* pSrc, uint8* pTgt, int nWidth, int nHeight,
							 int nXStart, int nYStart, int nBlockWidth, int nBlockHeight,
							 int simdPixelsPerRegister) {
	int nPaddedWidth = Helpers::DoPadding(nWidth, simdPixelsPerRegister);
	int nPaddedHeight = Helpers::DoPadding(nHeight, simdPixelsPerRegister);
	int nIncTargetLine = nHeight * 4;
	int nIncSource = nPaddedWidth * 3 - nBlockWidth * 3;
	const float* pSource = pSrc + nPaddedWidth * 3 * nYStart + nXStart * 3;
	uint8* pTarget = pTgt + nHeight * 4 * nXStart + nYStart * 4;
	uint8* pStartYPtr = pTarget;
	int nLoopX = Helpers::DoPadding(nBlockWidth, simdPixelsPerRegister) / simdPixelsPerRegister;
	int nTargetIncrement = simdPixelsPerRegister * nIncTargetLine - 2;

	for (int i = 0; i < nBlockHeight; i++) {
		for (int j = 0; j < nLoopX; j++) {
			pSource = RotateLineToDIB_1_f32(pSource, pTarget, nIncTargetLine, simdPixelsPerRegister);
			pTarget++;
			pSource = RotateLineToDIB_f32(pSource, pTarget, nIncTargetLine, simdPixelsPerRegister);
			pTarget++;
			pSource = RotateLineToDIB_f32(pSource, pTarget, nIncTargetLine, simdPixelsPerRegister);
			pTarget += nTargetIncrement;
		}
		pStartYPtr += 4;
		pTarget = pStartYPtr;
		pSource += nIncSource;
	}
}

// RotateFlip the source image by 90 deg and return rotated image
// RotateFlip is invertible: img = RotateFlip(RotateFlip(img))
static CFloatImage* Rotate_f32(const CFloatImage* pSourceImg, int simdPixelsPerRegister) {
	CFloatImage* targetImage = new CFloatImage(pSourceImg->GetHeight(), pSourceImg->GetWidth(), true, simdPixelsPerRegister);
	if (targetImage->AlignedPtr() == NULL) {
		delete targetImage;
		return NULL;
	}
	const float* pSource = (const float*) pSourceImg->AlignedPtr();
	float* pTarget = (float*) targetImage->AlignedPtr();

	const int cnBlockSize = 32;
	int nX = 0, nY = 0;
	while (nY < pSourceImg->GetHeight()) {
		nX = 0;
		while (nX < pSourceImg->GetWidth()) {
			RotateBlock_f32(pSource, pTarget, pSourceImg->GetWidth(), pSourceImg->GetHeight(),
				nX, nY, 
				min(cnBlockSize, pSourceImg->GetPaddedWidth() - nX), // !! here we need to use the padded width
				min(cnBlockSize, pSourceImg->GetHeight() - nY),
				simdPixelsPerRegister);
			nX += cnBlockSize;
		}
		nY += cnBlockSize;
	}

	return targetImage;
}

// RotateFlip the source image by 90 deg and return rotated image as 32 bpp DIB
static void* RotateToDIB_f32(const CFloatImage* pSourceImg, int simdPixelsPerRegister, uint8* pTarget = NULL) {

	const float* pSource = (const float*) pSourceImg->AlignedPtr();
	if (pTarget == NULL) {
		pTarget = new(std::nothrow) uint8[pSourceImg->GetHeight() * 4 * Helpers::DoPadding(pSourceImg->GetWidth(), simdPixelsPerRegister)];
		if (pTarget == NULL) return NULL;
	}

	const int cnBlockSize = 32;
	int nX = 0, nY = 0;
	while (nY < pSourceImg->GetHeight()) {
		nX = 0;
		while (nX < pSourceImg->GetWidth()) {
			RotateBlockToDIB_f32(pSource, pTarget, pSourceImg->GetWidth(), pSourceImg->GetHeight(),
				nX, nY, 
				min(cnBlockSize, pSourceImg->GetPaddedWidth() - nX),  // !! here we need to use the padded width
				min(cnBlockSize, pSourceImg->GetHeight() - nY),
				simdPixelsPerRegister);

			nX += cnBlockSize;
		}
		nY += cnBlockSize;
	}

	return pTarget;
}

static CFloatImage* ApplyFilter_SSE_f32(int nSourceHeight, int nTargetHeight, int nWidth,
	int nStartY_FP, int nStartX, int nIncrementY_FP,
	const SSEFilterKernelBlock& filter,
	int nFilterOffset, const CFloatImage* pSourceImg, bool bRoundResult) {

	int nStartXAligned = nStartX & ~3;
	int nEndXAligned = (nStartX + nWidth + 3) & ~3;
	CFloatImage* tempImage = new CFloatImage(nEndXAligned - nStartXAligned, nTargetHeight, 4);
	if (tempImage->AlignedPtr() == NULL) {
		delete tempImage;
		return NULL;
	}

	int nCurY = nStartY_FP;
	int nChannelLenBytes = pSourceImg->GetPaddedWidth() * sizeof(float);
	int nRowLenBytes = nChannelLenBytes * 3;
	int nNumberOfBlocksX = (nEndXAligned - nStartXAligned) >> 2;

	const uint8* pSourceStart = (const uint8*)pSourceImg->AlignedPtr() + nStartXAligned * sizeof(float);
	SSEFilterKernel** pKernelIndexStart = filter.Indices;

	_MM_ALIGN16 float XMM255[4] = {4095.0,  4095.0, 4095.0, 4095.0};

	__m128 xmm0 = _mm_setzero_ps();
	__m128 xmm1 = *((__m128*)XMM255);
	__m128 xmm2;
	__m128 xmm3;
	__m128 xmm4 = _mm_setzero_ps();
	__m128 xmm5 = _mm_setzero_ps();
	__m128 xmm6 = _mm_setzero_ps();
	__m128 xmm7;

	__m128* pDestination = (__m128*)tempImage->AlignedPtr();

	for (int y = 0; y < nTargetHeight; y++) {
		uint32 nCurYInt = (uint32)nCurY >> 16; // integer part of Y
		int filterIndex = y + nFilterOffset;
		SSEFilterKernel* pKernel = pKernelIndexStart[filterIndex];
		int filterLen = pKernel->FilterLen;
		int filterOffset = pKernel->FilterOffset;
		const __m128* pFilterStart = (__m128*)&(pKernel->Kernel);
		const __m128* pSourceRow = (const __m128*)(pSourceStart + ((int)nCurYInt - filterOffset) * nRowLenBytes);

		for (int x = 0; x < nNumberOfBlocksX; x++) {
			const __m128* pSource = pSourceRow;
			const __m128* pFilter = pFilterStart;
			xmm4 = _mm_setzero_ps();
			xmm5 = _mm_setzero_ps();
			xmm6 = _mm_setzero_ps();
			for (int i = 0; i < filterLen; i++) {
				xmm7 = *pFilter;		// filter range was adjusted to output maximum of 1.0]

				// the pixel data RED channel
				xmm2 = *pSource;
				xmm2 = _mm_mul_ps(xmm2, xmm7);
				xmm4 = _mm_add_ps(xmm4, xmm2);
				pSource = (__m128*)((uint8*)pSource + nChannelLenBytes);

				// the pixel data GREEN channel
				xmm3 = *pSource;
				xmm3 = _mm_mul_ps(xmm3, xmm7);
				xmm5 = _mm_add_ps(xmm5, xmm3);
				pSource = (__m128*)((uint8*)pSource + nChannelLenBytes);

				// the pixel data BLUE channel
				xmm2 = *pSource;
				xmm2 = _mm_mul_ps(xmm2, xmm7);
				xmm6 = _mm_add_ps(xmm6, xmm2);
				pSource = (__m128*)((uint8*)pSource + nChannelLenBytes);

				pFilter++;
				}

			if (bRoundResult == true) {
				// limit to range <=255 (in xmm1)
				xmm4 = _mm_min_ps(xmm4, xmm1);
				xmm5 = _mm_min_ps(xmm5, xmm1);
				xmm6 = _mm_min_ps(xmm6, xmm1);

				// limit to range >=0 (in xmm0)
				xmm4 = _mm_max_ps(xmm4, xmm0);
				xmm5 = _mm_max_ps(xmm5, xmm0);
				xmm6 = _mm_max_ps(xmm6, xmm0);

				// round to nearest integer
				xmm4 = _mm_round_ps(xmm4, _MM_FROUND_TO_NEAREST_INT);
				xmm5 = _mm_round_ps(xmm5, _MM_FROUND_TO_NEAREST_INT);
				xmm6 = _mm_round_ps(xmm6, _MM_FROUND_TO_NEAREST_INT);
				}

			// store result in blocks
			*pDestination++ = xmm4;
			*pDestination++ = xmm5;
			*pDestination++ = xmm6;

			pSourceRow++;
		};

		nCurY += nIncrementY_FP;
	};

	return tempImage;
}

// Used in ProcessStrip()
void* SampleDown_SSE_Core_f32(CSize fullTargetSize, CPoint fullTargetOffset, CSize clippedTargetSize,
	CSize sourceSize, const void* pPixels, int nChannels,
	EFilterType eFilter, uint8* pTarget) {

//*GF*/	TCHAR debugtext[512];
//*GF*/	swprintf(debugtext,255,TEXT("SampleDown_SSE_Core_f32()->filterY() sourceSize.cy %d fullTargetSize.cy %d"), sourceSize.cy, fullTargetSize.cy);
//*GF*/	::OutputDebugStringW(debugtext);

 	CAutoSSEFilter filterY(sourceSize.cy, fullTargetSize.cy, eFilter);
	const SSEFilterKernelBlock& kernelsY = filterY.Kernels();

//*GF*/	swprintf(debugtext,255,TEXT("SampleDown_SSE_Core_f32()->filterX() sourceSize.cx %d fullTargetSize.cx %d"), sourceSize.cx, fullTargetSize.cx);
//*GF*/	::OutputDebugStringW(debugtext);

	CAutoSSEFilter filterX(sourceSize.cx, fullTargetSize.cx, eFilter);
	const SSEFilterKernelBlock& kernelsX = filterX.Kernels();

	uint32 nIncrementX = (uint32)(sourceSize.cx << 16)/fullTargetSize.cx + 1;
	uint32 nIncrementY = (uint32)(sourceSize.cy << 16)/fullTargetSize.cy + 1;

	int nIncOffsetX = (nIncrementX - 65536) >> 1;
	int nIncOffsetY = (nIncrementY - 65536) >> 1;
	int nFirstX = (uint32)(nIncOffsetX + nIncrementX*fullTargetOffset.x) >> 16;
	nFirstX = max(0, nFirstX - kernelsX.Indices[fullTargetOffset.x]->FilterOffset);
	int nLastX  = (uint32)(nIncOffsetX + nIncrementX*(fullTargetOffset.x + clippedTargetSize.cx - 1)) >> 16;
	SSEFilterKernel* pLastXFilter = kernelsX.Indices[fullTargetOffset.x + clippedTargetSize.cx - 1];
	nLastX  = min(sourceSize.cx - 1, nLastX - pLastXFilter->FilterOffset + pLastXFilter->FilterLen - 1);
	int nFirstY = (uint32)(nIncOffsetY + nIncrementY*fullTargetOffset.y) >> 16;
	nFirstY = max(0, nFirstY - kernelsY.Indices[fullTargetOffset.y]->FilterOffset);
	int nLastY  = (uint32)(nIncOffsetY + nIncrementY*(fullTargetOffset.y + clippedTargetSize.cy - 1)) >> 16;
	SSEFilterKernel* pLastYFilter = kernelsY.Indices[fullTargetOffset.y + clippedTargetSize.cy - 1];
	nLastY  = min(sourceSize.cy - 1, nLastY - pLastYFilter->FilterOffset + pLastYFilter->FilterLen - 1);
	int nFilterOffsetX = fullTargetOffset.x;
	int nFilterOffsetY = fullTargetOffset.y;
	int nStartX = nIncOffsetX + nIncrementX*fullTargetOffset.x - 65536*nFirstX;
	int nStartY = nIncOffsetY + nIncrementY*fullTargetOffset.y - 65536*nFirstY;

	// Resize Y
	double t1 = Helpers::GetExactTickCount();
	CFloatImage* pImage1 = new CFloatImage(sourceSize.cx, sourceSize.cy, nFirstX, nLastX, nFirstY, nLastY, pPixels, nChannels, 8);
	if (pImage1->AlignedPtr() == NULL) {
		delete pImage1;
		return NULL;
	}
	double t2 = Helpers::GetExactTickCount();
	CFloatImage* pImage2 = ApplyFilter_SSE_f32(pImage1->GetHeight(), clippedTargetSize.cy, pImage1->GetWidth(), nStartY, 0, nIncrementY, kernelsY, nFilterOffsetY, pImage1, false);
	delete pImage1;
	if (pImage2 == NULL) return NULL;
	double t3 = Helpers::GetExactTickCount();
	// Rotate
	CFloatImage* pImage3 = Rotate_f32(pImage2, 4);
	delete pImage2;
	if (pImage3 == NULL) return NULL;
	double t4 = Helpers::GetExactTickCount();
	// Resize Y again
	CFloatImage* pImage4 = ApplyFilter_SSE_f32(pImage3->GetHeight(), clippedTargetSize.cx, clippedTargetSize.cy, nStartX, 0, nIncrementX, kernelsX, nFilterOffsetX, pImage3, true);
	delete pImage3;
	if (pImage4 == NULL) return NULL;
	double t5 = Helpers::GetExactTickCount();
	// Rotate back
	void* pTargetDIB = RotateToDIB_f32(pImage4, 4, pTarget);
	double t6 = Helpers::GetExactTickCount();

	delete pImage4;

	_stprintf_s(s_TimingInfo, 256, _T("Create: %.2f, Filter1: %.2f, Rotate: %.2f, Filter2: %.2f, Rotate: %.2f"), t2 - t1, t3 - t2, t4 - t3, t5 - t4, t6 - t5);
	
	return pTargetDIB;
	}

// Used in ProcessStrip()
void* SampleDown_AVX_Core_f32(CSize fullTargetSize, CPoint fullTargetOffset, CSize clippedTargetSize,
	CSize sourceSize, const void* pPixels, int nChannels,
	EFilterType eFilter, uint8* pTarget) {

	CAutoAVXFilter filterY(sourceSize.cy, fullTargetSize.cy, eFilter);
	const AVXFilterKernelBlock& kernelsY = filterY.Kernels();

	CAutoAVXFilter filterX(sourceSize.cx, fullTargetSize.cx, eFilter);
	const AVXFilterKernelBlock& kernelsX = filterX.Kernels();

	uint32 nIncrementX = (uint32)(sourceSize.cx << 16) / fullTargetSize.cx + 1;
	uint32 nIncrementY = (uint32)(sourceSize.cy << 16) / fullTargetSize.cy + 1;

	int nIncOffsetX = (nIncrementX - 65536) >> 1;
	int nIncOffsetY = (nIncrementY - 65536) >> 1;
	int nFirstX = (uint32)(nIncOffsetX + nIncrementX*fullTargetOffset.x) >> 16;
	nFirstX = max(0, nFirstX - kernelsX.Indices[fullTargetOffset.x]->FilterOffset);
	int nLastX = (uint32)(nIncOffsetX + nIncrementX*(fullTargetOffset.x + clippedTargetSize.cx - 1)) >> 16;
	AVXFilterKernel* pLastXFilter = kernelsX.Indices[fullTargetOffset.x + clippedTargetSize.cx - 1];
	nLastX = min(sourceSize.cx - 1, nLastX - pLastXFilter->FilterOffset + pLastXFilter->FilterLen - 1);
	int nFirstY = (uint32)(nIncOffsetY + nIncrementY*fullTargetOffset.y) >> 16;
	nFirstY = max(0, nFirstY - kernelsY.Indices[fullTargetOffset.y]->FilterOffset);
	int nLastY = (uint32)(nIncOffsetY + nIncrementY*(fullTargetOffset.y + clippedTargetSize.cy - 1)) >> 16;
	AVXFilterKernel* pLastYFilter = kernelsY.Indices[fullTargetOffset.y + clippedTargetSize.cy - 1];
	nLastY = min(sourceSize.cy - 1, nLastY - pLastYFilter->FilterOffset + pLastYFilter->FilterLen - 1);
	int nFilterOffsetX = fullTargetOffset.x;
	int nFilterOffsetY = fullTargetOffset.y;
	int nStartX = nIncOffsetX + nIncrementX*fullTargetOffset.x - 65536 * nFirstX;
	int nStartY = nIncOffsetY + nIncrementY*fullTargetOffset.y - 65536 * nFirstY;

	// Resize Y
	double t1 = Helpers::GetExactTickCount();
	CFloatImage* pImage1 = new CFloatImage(sourceSize.cx, sourceSize.cy, nFirstX, nLastX, nFirstY, nLastY, pPixels, nChannels, 16);
	if (pImage1->AlignedPtr() == NULL) {
		delete pImage1;
		return NULL;
	}
	double t2 = Helpers::GetExactTickCount();
	CFloatImage* pImage2 = ApplyFilter_AVX_f32(pImage1->GetHeight(), clippedTargetSize.cy, pImage1->GetWidth(), nStartY, 0, nIncrementY, kernelsY, nFilterOffsetY, pImage1, false);
	delete pImage1;
	if (pImage2 == NULL) return NULL;
	double t3 = Helpers::GetExactTickCount();
	// Rotate
	CFloatImage* pImage3 = Rotate_f32(pImage2, 8);
	delete pImage2;
	if (pImage3 == NULL) return NULL;
	double t4 = Helpers::GetExactTickCount();
	// Resize Y again
	CFloatImage* pImage4 = ApplyFilter_AVX_f32(pImage3->GetHeight(), clippedTargetSize.cx, clippedTargetSize.cy, nStartX, 0, nIncrementX, kernelsX, nFilterOffsetX, pImage3, true);
	delete pImage3;
	if (pImage4 == NULL) return NULL;
	double t5 = Helpers::GetExactTickCount();
	// Rotate back
	void* pTargetDIB = RotateToDIB_f32(pImage4, 8, pTarget);
	double t6 = Helpers::GetExactTickCount();

	delete pImage4;

	_stprintf_s(s_TimingInfo, 256, _T("Create: %.2f, Filter1: %.2f, Rotate: %.2f, Filter2: %.2f, Rotate: %.2f"), t2 - t1, t3 - t2, t4 - t3, t5 - t4, t6 - t5);

	return pTargetDIB;
}

// Used in ProcessStrip()
void* SampleUp_SSE_Core_f32(CSize fullTargetSize, CPoint fullTargetOffset, CSize clippedTargetSize,
	CSize sourceSize, const void* pPixels, int nChannels, uint8* pTarget) {
	int nTargetWidth = clippedTargetSize.cx;
	int nTargetHeight = clippedTargetSize.cy;
	int nSourceWidth = sourceSize.cx;
	int nSourceHeight = sourceSize.cy;

	uint32 nIncrementX = (uint32)(65536*(uint32)(nSourceWidth - 1)/(fullTargetSize.cx - 1));
	uint32 nIncrementY = (uint32)(65536*(uint32)(nSourceHeight - 1)/(fullTargetSize.cy - 1));

	int nFirstX = max(0, int((uint32)(nIncrementX*fullTargetOffset.x) >> 16) - 1);
	int nLastX = min(sourceSize.cx - 1, int(((uint32)(nIncrementX*(fullTargetOffset.x + nTargetWidth - 1)) >> 16) + 2));
	int nFirstY = max(0, int((uint32)(nIncrementY*fullTargetOffset.y) >> 16) - 1);
	int nLastY = min(sourceSize.cy - 1, int(((uint32)(nIncrementY*(fullTargetOffset.y + nTargetHeight - 1)) >> 16) + 2));
	int nFirstTargetWidth = nLastX - nFirstX + 1;
	int nFirstTargetHeight = nTargetHeight;
	int nFilterOffsetX = fullTargetOffset.x;
	int nFilterOffsetY = fullTargetOffset.y;
	int nStartX = nIncrementX*fullTargetOffset.x - 65536*nFirstX;
	int nStartY = nIncrementY*fullTargetOffset.y - 65536*nFirstY;

	CAutoSSEFilter filterY(nSourceHeight, fullTargetSize.cy, Filter_Upsampling_Bicubic);
	const SSEFilterKernelBlock& kernelsY = filterY.Kernels();

	CAutoSSEFilter filterX(nSourceWidth, fullTargetSize.cx, Filter_Upsampling_Bicubic);
	const SSEFilterKernelBlock& kernelsX = filterX.Kernels();

	// Resize Y
	CFloatImage* pImage1 = new CFloatImage(nSourceWidth, nSourceHeight, nFirstX, nLastX, nFirstY, nLastY, pPixels, nChannels, 8);
	if (pImage1->AlignedPtr() == NULL) {
		delete pImage1;
		return NULL;
	}
	CFloatImage* pImage2 = ApplyFilter_SSE_f32(pImage1->GetHeight(), nTargetHeight, pImage1->GetWidth(), nStartY, 0, nIncrementY, kernelsY, nFilterOffsetY, pImage1, false);
	delete pImage1;
	if (pImage2 == NULL) return NULL;
	CFloatImage* pImage3 = Rotate_f32(pImage2, 4);
	delete pImage2;
	if (pImage3 == NULL) return NULL;
	CFloatImage* pImage4 = ApplyFilter_SSE_f32(pImage3->GetHeight(), nTargetWidth, nTargetHeight, nStartX, 0, nIncrementX, kernelsX, nFilterOffsetX, pImage3, true);
	delete pImage3;
	if (pImage4 == NULL) return NULL;
	void* pTargetDIB = RotateToDIB_f32(pImage4, 4, pTarget);
	delete pImage4;

	return pTargetDIB;
	}

// Used in ProcessStrip()
void* SampleUp_AVX_Core_f32(CSize fullTargetSize, CPoint fullTargetOffset, CSize clippedTargetSize,
	CSize sourceSize, const void* pPixels, int nChannels, uint8* pTarget) {

	int nTargetWidth = clippedTargetSize.cx;
	int nTargetHeight = clippedTargetSize.cy;
	int nSourceWidth = sourceSize.cx;
	int nSourceHeight = sourceSize.cy;

	uint32 nIncrementX = (uint32)(65536 * (uint32)(nSourceWidth - 1) / (fullTargetSize.cx - 1));
	uint32 nIncrementY = (uint32)(65536 * (uint32)(nSourceHeight - 1) / (fullTargetSize.cy - 1));

	int nFirstX = max(0, int((uint32)(nIncrementX*fullTargetOffset.x) >> 16) - 1);
	int nLastX = min(sourceSize.cx - 1, int(((uint32)(nIncrementX*(fullTargetOffset.x + nTargetWidth - 1)) >> 16) + 2));
	int nFirstY = max(0, int((uint32)(nIncrementY*fullTargetOffset.y) >> 16) - 1);
	int nLastY = min(sourceSize.cy - 1, int(((uint32)(nIncrementY*(fullTargetOffset.y + nTargetHeight - 1)) >> 16) + 2));
	int nFirstTargetWidth = nLastX - nFirstX + 1;
	int nFirstTargetHeight = nTargetHeight;
	int nFilterOffsetX = fullTargetOffset.x;
	int nFilterOffsetY = fullTargetOffset.y;
	int nStartX = nIncrementX*fullTargetOffset.x - 65536 * nFirstX;
	int nStartY = nIncrementY*fullTargetOffset.y - 65536 * nFirstY;

	CAutoAVXFilter filterY(nSourceHeight, fullTargetSize.cy, Filter_Upsampling_Bicubic);
	const AVXFilterKernelBlock& kernelsY = filterY.Kernels();

	CAutoAVXFilter filterX(nSourceWidth, fullTargetSize.cx, Filter_Upsampling_Bicubic);
	const AVXFilterKernelBlock& kernelsX = filterX.Kernels();

	// Resize Y
	CFloatImage* pImage1 = new CFloatImage(nSourceWidth, nSourceHeight, nFirstX, nLastX, nFirstY, nLastY, pPixels, nChannels, 16);
	if (pImage1->AlignedPtr() == NULL) {
		delete pImage1;
		return NULL;
	}
	CFloatImage* pImage2 = ApplyFilter_AVX_f32(pImage1->GetHeight(), nTargetHeight, pImage1->GetWidth(), nStartY, 0, nIncrementY, kernelsY, nFilterOffsetY, pImage1, false);
	delete pImage1;
	if (pImage2 == NULL) return NULL;
	CFloatImage* pImage3 = Rotate_f32(pImage2, 8);
	delete pImage2;
	if (pImage3 == NULL) return NULL;
	CFloatImage* pImage4 = ApplyFilter_AVX_f32(pImage3->GetHeight(), nTargetWidth, nTargetHeight, nStartX, 0, nIncrementX, kernelsX, nFilterOffsetX, pImage3, true);
	delete pImage3;
	if (pImage4 == NULL) return NULL;
	void* pTargetDIB = RotateToDIB_f32(pImage4, 8, pTarget);
	delete pImage4;

	return pTargetDIB;
}
