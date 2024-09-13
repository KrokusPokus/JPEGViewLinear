#include "StdAfx.h"
#include "JPEGImage.h"
#include "BasicProcessing.h"
#include "XMMImage.h"
#include "Helpers.h"
#include "SettingsProvider.h"
//#include "HistogramCorr.h"
//#include "LocalDensityCorr.h"
//#include "ParameterDB.h"
#include "EXIFReader.h"
//#include "RawMetadata.h"
//#include "MaxImageDef.h"
#include "libjpeg-turbo\include\turbojpeg.h"
#include <math.h>
#include <assert.h>

///////////////////////////////////////////////////////////////////////////////////
// Static helpers
///////////////////////////////////////////////////////////////////////////////////

static void RotateInplace(const CSize& imageSize, double& dX, double& dY, double dAngle) {
	dX -= (imageSize.cx - 1) * 0.5;
	dY -= (imageSize.cy - 1) * 0.5;
	double dXr = cos(dAngle) * dX - sin(dAngle) * dY;
	double dYr = sin(dAngle) * dX + cos(dAngle) * dY;
	dX = dXr;
	dY = dYr;
}

static bool SupportsSIMD(Helpers::CPUType cpuType) {
	switch (cpuType)
	{
	case Helpers::CPU_SSE:
	case Helpers::CPU_AVX2:
		return true;
	default:
		return false;
	}
}

static CBasicProcessing::SIMDArchitecture ToSIMDArchitecture(Helpers::CPUType cpuType) {
	switch (cpuType)
	{
	case Helpers::CPU_SSE:
		return CBasicProcessing::SSE;
	case Helpers::CPU_AVX2:
		return CBasicProcessing::AVX2;
	default:
		assert(false);
		return (CBasicProcessing::SIMDArchitecture)(-1);
	}
}

///////////////////////////////////////////////////////////////////////////////////
// Public interface
///////////////////////////////////////////////////////////////////////////////////

CJPEGImage::CJPEGImage(int nWidth, int nHeight, void* pPixels, void* pEXIFData, int nChannels, __int64 nJPEGHash, 
					   EImageFormat eImageFormat, bool bIsAnimation, int nFrameIndex, int nNumberOfFrames, int nFrameTimeMs,
					   CLocalDensityCorr* pLDC, bool bIsThumbnailImage, CRawMetadata* pRawMetadata) {
	if (nChannels == 3 || nChannels == 4) {
		m_pOrigPixels = pPixels;
		m_nOriginalChannels = nChannels;
	} else if (nChannels == 1) {
		m_pOrigPixels = CBasicProcessing::Convert1To4Channels(nWidth, nHeight, pPixels);
		delete[] pPixels;
		m_nOriginalChannels = 4;
	} else {
		assert(false);
		m_pOrigPixels = NULL;
		m_nOriginalChannels = 0;
	}

	if (pEXIFData != NULL) {
		unsigned char * pEXIF = (unsigned char *)pEXIFData;
		m_nEXIFSize = pEXIF[2]*256 + pEXIF[3] + 2;
		m_pEXIFData = new char[m_nEXIFSize];
		memcpy(m_pEXIFData, pEXIFData, m_nEXIFSize);
		m_pEXIFReader = new CEXIFReader(m_pEXIFData);
	} else {
		m_nEXIFSize = 0;
		m_pEXIFData = NULL;
		m_pEXIFReader = NULL;
	}

	m_pRawMetadata = pRawMetadata;

/*GF*/	TCHAR debugtext[512];
/*GF*/	swprintf(debugtext,255,TEXT("bIsAnimation: %d"), bIsAnimation);
/*GF*/	::OutputDebugStringW(debugtext);

	m_nPixelHash = nJPEGHash;
	m_eImageFormat = eImageFormat;
	m_bIsAnimation = bIsAnimation;
	m_nFrameIndex = nFrameIndex;
	m_nNumberOfFrames = nNumberOfFrames;
	m_nFrameTimeMs = nFrameTimeMs;
	m_eJPEGChromoSampling = TJSAMP_420;

	m_nOrigWidth = m_nInitOrigWidth = nWidth;
	m_nOrigHeight = m_nInitOrigHeight = nHeight;
	m_pDIBPixels = NULL;
	m_pDIBPixelsLUTProcessed = NULL;
	m_pLastDIB = NULL;
//	m_pThumbnail = NULL;
//	m_pHistogramThumbnail = NULL;
//	m_pGrayImage = NULL;
//	m_pSmoothGrayImage = NULL;

//	m_pLUTAllChannels = NULL;
//	m_pLUTRGB = NULL;
//	m_pSaturationLUTs = NULL;
	m_eProcFlags = PFLAG_None;
	m_eProcFlagsInitial = PFLAG_None;
	m_nInitialRotation = 0;
	m_dInitialZoom = -1;
	m_initialOffsets = CPoint(0, 0);
//	m_pDimRects = 0;
	m_nNumDimRects = 0;
//	m_bEnableDimming = true;
//	m_bShowGrid = false;
	m_bInParamDB = false;
//	m_bHasZoomStoredInParamDB = false;
//	m_bUnsharpMaskParamsValid = false;
	m_bIsThumbnailImage = bIsThumbnailImage;
//	m_pCachedProcessedHistogram = NULL;

	m_bCropped = false;
	m_bIsDestructivlyProcessed = false;
	m_nRotation = 0;
	m_bRotationByEXIF = false;
	m_bFirstReprocessing = true;
	m_dLastOpTickCount = 0;
	m_dLoadTickCount = 0;
//	m_dUnsharpMaskTickCount = 0;
	m_FullTargetSize = CSize(0, 0);
	m_ClippingSize = CSize(0, 0);
	m_TargetOffset = CPoint(0, 0);
	m_dRotationLQ = 0.0;
//	m_bTrapezoidValid = false;
/*
	// Create the LDC object on the image
	m_pLDC = (pLDC == NULL) ? (new CLocalDensityCorr(*this, true)) : pLDC;
	m_bLDCOwned = pLDC == NULL;
	if (nJPEGHash == 0) {
		// Use the decompressed pixel hash in this case
		m_nPixelHash = m_pLDC->GetPixelHash();
	}
	m_fLightenShadowFactor = (1.0f - m_pLDC->GetHistogram()->IsNightShot())*(1.0f - m_pLDC->IsSunset());

	// Initialize to INI value, may be overriden later by parameter DB
	memcpy(m_fColorCorrectionFactors, CSettingsProvider::This().ColorCorrectionAmounts(), sizeof(m_fColorCorrectionFactors));
	memset(m_fColorCorrectionFactorsNull, 0, sizeof(m_fColorCorrectionFactorsNull));
*/
}

CJPEGImage::~CJPEGImage(void) {
	delete[] m_pOrigPixels;
	m_pOrigPixels = NULL;
	delete[] m_pDIBPixels;
	m_pDIBPixels = NULL;
	delete[] m_pDIBPixelsLUTProcessed;
	m_pDIBPixelsLUTProcessed = NULL;
//	delete[] m_pGrayImage;
//	m_pGrayImage = NULL;
//	delete[] m_pSmoothGrayImage;
//	m_pSmoothGrayImage = NULL;
//	delete[] m_pLUTAllChannels;
//	m_pLUTAllChannels = NULL;
//	delete[] m_pLUTRGB;
//	m_pLUTRGB = NULL;
//	delete[] m_pSaturationLUTs;
//	m_pSaturationLUTs = NULL;
//	if (m_bLDCOwned) delete m_pLDC;
//	m_pLDC = NULL;
	m_pLastDIB = NULL;
	delete[] m_pEXIFData;
	m_pEXIFData = NULL;
	delete m_pEXIFReader;
	m_pEXIFReader = NULL;
//	delete[] m_pDimRects;
//	m_pDimRects = NULL;
//	delete m_pThumbnail;
//	m_pThumbnail = NULL;
//	delete m_pHistogramThumbnail;
//	m_pHistogramThumbnail = NULL;
//	delete m_pCachedProcessedHistogram;
//	m_pCachedProcessedHistogram = NULL;
//	delete m_pRawMetadata;
//	m_pRawMetadata = NULL;
}

void CJPEGImage::ResampleWithPan(void* & pDIBPixels, void* & pDIBPixelsLUTProcessed, CSize fullTargetSize, 
								 CSize clippingSize, CPoint targetOffset, CRect oldClippingRect,
								 EProcessingFlags eProcFlags, EResizeType eResizeType) {
	CPoint oldOffset = oldClippingRect.TopLeft();
	CSize oldSize = oldClippingRect.Size();
	CRect newClippingRect = CRect(targetOffset, clippingSize);
	CRect sourceRect;
	if (sourceRect.IntersectRect(oldClippingRect, newClippingRect)) {
		// there is an intersection, reuse the non LUT processed DIB
		sourceRect.OffsetRect(-oldOffset.x, -oldOffset.y);
		CRect targetRect = CRect(CPoint(max(0, oldOffset.x - newClippingRect.left), max(0, oldOffset.y - newClippingRect.top)), 
			CSize(sourceRect.Width(), sourceRect.Height()));

		bool bCanUseLUTProcDIB = ApplyCorrectionLUTandLDC(eProcFlags, pDIBPixelsLUTProcessed, fullTargetSize,
			targetOffset, pDIBPixels, clippingSize, false, true, false) != NULL;

		// the LUT processed pixels cannot be used and the original pixels are not available -
		// full recreation of DIBs is needed
		if (!bCanUseLUTProcDIB && pDIBPixels == NULL) {
			delete[] pDIBPixelsLUTProcessed; pDIBPixelsLUTProcessed = NULL;
			return;
		}

		// Copy the reusable part of original DIB pixels
		void* pPannedPixels = (bCanUseLUTProcDIB == false) ? 
			CBasicProcessing::CopyRect32bpp(NULL, pDIBPixels, clippingSize, targetRect, oldSize, sourceRect) :
			NULL;

		// get rid of original DIB, will we recreated automatically when needed
		delete[] pDIBPixels; pDIBPixels = NULL;

		// Copy the reusable part of processed DIB pixels
		void* pPannedPixelsLUTProcessed = bCanUseLUTProcDIB ? 
			CBasicProcessing::CopyRect32bpp(NULL, pDIBPixelsLUTProcessed, clippingSize, targetRect, oldSize, sourceRect) :
			NULL;

		// Delete old LUT processed DIB, we copied the part that can be reused to a new DIB (pPannedPixelsLUTProcessed)
		delete[] pDIBPixelsLUTProcessed; pDIBPixelsLUTProcessed = NULL;

		if (targetRect.top > 0) {
			CSize clipSize(clippingSize.cx, targetRect.top);
			void* pTop = Resample(fullTargetSize, clipSize, targetOffset, eProcFlags, eResizeType);
			
			if (!bCanUseLUTProcDIB) {
				CBasicProcessing::CopyRect32bpp(pPannedPixels, pTop,
					clippingSize, CRect(CPoint(0, 0), clipSize),
					clipSize, CRect(CPoint(0, 0), clipSize));
			} else {
				void* pTopProc = NULL;
				ApplyCorrectionLUTandLDC(eProcFlags, pTopProc, fullTargetSize, targetOffset, pTop, clipSize, false, false, false);
				CBasicProcessing::CopyRect32bpp(pPannedPixelsLUTProcessed, pTopProc,
					clippingSize, CRect(CPoint(0, 0), clipSize),
					clipSize, CRect(CPoint(0, 0), clipSize));
				delete[] pTopProc;
			}

			delete[] pTop;
		}
		if (targetRect.bottom < clippingSize.cy)
			{
			CSize clipSize(clippingSize.cx, clippingSize.cy -  targetRect.bottom);
			CPoint offset(targetOffset.x, targetOffset.y + targetRect.bottom);
			void* pBottom = Resample(fullTargetSize, clipSize, offset, eProcFlags, eResizeType);
			
			if (!bCanUseLUTProcDIB) {
				CBasicProcessing::CopyRect32bpp(pPannedPixels, pBottom,
					clippingSize, CRect(CPoint(0, targetRect.bottom), clipSize),
					clipSize, CRect(CPoint(0, 0), clipSize));
			} else {
				void* pBottomProc = NULL;
				ApplyCorrectionLUTandLDC(eProcFlags, pBottomProc, fullTargetSize, offset, pBottom, clipSize, false, false, false);
				CBasicProcessing::CopyRect32bpp(pPannedPixelsLUTProcessed, pBottomProc,
					clippingSize, CRect(CPoint(0, targetRect.bottom), clipSize),
					clipSize, CRect(CPoint(0, 0), clipSize));
				delete[] pBottomProc;
			}

			delete[] pBottom;
		}
		if (targetRect.left > 0) {
			CSize clipSize(targetRect.left, clippingSize.cy);
			void* pLeft = Resample(fullTargetSize, clipSize, targetOffset, eProcFlags, eResizeType);
			
			if (!bCanUseLUTProcDIB) {
				CBasicProcessing::CopyRect32bpp(pPannedPixels, pLeft,
					clippingSize, CRect(CPoint(0, 0), clipSize),
					clipSize, CRect(CPoint(0, 0), clipSize));
			} else {
				void* pLeftProc = NULL;
				ApplyCorrectionLUTandLDC(eProcFlags, pLeftProc, fullTargetSize, targetOffset, pLeft, clipSize, false, false, false);
				CBasicProcessing::CopyRect32bpp(pPannedPixelsLUTProcessed, pLeftProc,
					clippingSize, CRect(CPoint(0, 0), clipSize),
					clipSize, CRect(CPoint(0, 0), clipSize));
				delete[] pLeftProc;
			}

			delete[] pLeft;
		}
		if (targetRect.right < clippingSize.cx) {
			CSize clipSize(clippingSize.cx -  targetRect.right, clippingSize.cy);
			CPoint offset(targetOffset.x + targetRect.right, targetOffset.y);
			void* pRight = Resample(fullTargetSize, clipSize, offset, eProcFlags, eResizeType);
			
			if (!bCanUseLUTProcDIB) {
				CBasicProcessing::CopyRect32bpp(pPannedPixels, pRight,
					clippingSize, CRect(CPoint(targetRect.right, 0), clipSize),
					clipSize, CRect(CPoint(0, 0), clipSize));
			} else {
				void* pRigthProc = NULL;
				ApplyCorrectionLUTandLDC(eProcFlags, pRigthProc, fullTargetSize, offset, pRight, clipSize, false, false, false);
				CBasicProcessing::CopyRect32bpp(pPannedPixelsLUTProcessed, pRigthProc,
					clippingSize, CRect(CPoint(targetRect.right, 0), clipSize),
					clipSize, CRect(CPoint(0, 0), clipSize));
				delete[] pRigthProc;
			}

			delete[] pRight;
		}
		pDIBPixels = pPannedPixels;
		pDIBPixelsLUTProcessed = pPannedPixelsLUTProcessed;
		return;
	}

	delete[] pDIBPixels; pDIBPixels = NULL;
	delete[] pDIBPixelsLUTProcessed; pDIBPixelsLUTProcessed = NULL;
}

void* CJPEGImage::Resample(CSize fullTargetSize, CSize clippingSize, CPoint targetOffset, EProcessingFlags eProcFlags, EResizeType eResizeType)
	{
	Helpers::CPUType cpu = CSettingsProvider::This().AlgorithmImplementation();
	EFilterType filter = CSettingsProvider::This().DownsamplingFilter();

	if (fullTargetSize.cx > 65535 || fullTargetSize.cy > 65535)
		return NULL;

	/*GF*/	TCHAR debugtext[512];

	/*GF*/	swprintf(debugtext,255,TEXT("eResizeType: %d",eResizeType));
	/*GF*/	::OutputDebugStringW(debugtext);
				
	if (GetProcessingFlag(eProcFlags, PFLAG_HighQualityResampling)
		&& !(eResizeType == NoResize)
		&& (filter>0))
		{
		if (SupportsSIMD(cpu))
			{
			if (eResizeType == UpSample)
				{
				/*GF*/	swprintf(debugtext,255,TEXT("Resample()->SampleUp_SIMD()"));
				/*GF*/	::OutputDebugStringW(debugtext);
				return CBasicProcessing::SampleUp_SIMD(fullTargetSize, targetOffset, clippingSize, CSize(m_nOrigWidth, m_nOrigHeight), m_pOrigPixels, m_nOriginalChannels, ToSIMDArchitecture(cpu));
				}
			else
				{
				/*GF*/	swprintf(debugtext,255,TEXT("Resample()->SampleDown_SIMD()"));
				/*GF*/	::OutputDebugStringW(debugtext);
				return CBasicProcessing::SampleDown_SIMD(fullTargetSize, targetOffset, clippingSize, CSize(m_nOrigWidth, m_nOrigHeight), m_pOrigPixels, m_nOriginalChannels, filter, ToSIMDArchitecture(cpu));
				}
		} else {
			if (eResizeType == UpSample) {
				/*GF*/	swprintf(debugtext,255,TEXT("Resample()->SampleUp()"));
				/*GF*/	::OutputDebugStringW(debugtext);
				return CBasicProcessing::SampleUp(fullTargetSize, targetOffset, clippingSize, CSize(m_nOrigWidth, m_nOrigHeight), m_pOrigPixels, m_nOriginalChannels);
			} else
				{
				/*GF*/	swprintf(debugtext,255,TEXT("Resample()->SampleDown()"));
				/*GF*/	::OutputDebugStringW(debugtext);
				return CBasicProcessing::SampleDown(fullTargetSize, targetOffset, clippingSize, CSize(m_nOrigWidth, m_nOrigHeight), m_pOrigPixels, m_nOriginalChannels, filter);
				}
			}
		}
	else
		{
		/*GF*/	swprintf(debugtext,255,TEXT("Resample()->PointSample()"));
		/*GF*/	::OutputDebugStringW(debugtext);
		return CBasicProcessing::PointSample(fullTargetSize, targetOffset, clippingSize, CSize(m_nOrigWidth, m_nOrigHeight), m_pOrigPixels, m_nOriginalChannels);
		}
	}

CPoint CJPEGImage::ConvertOffset(CSize fullTargetSize, CSize clippingSize, CPoint targetOffset) {
	int nStartX = (fullTargetSize.cx - clippingSize.cx)/2 - targetOffset.x;
	int nStartY = (fullTargetSize.cy - clippingSize.cy)/2 - targetOffset.y;
	return CSize(nStartX, nStartY);
}

bool CJPEGImage::VerifyRotation(int nRotation) {
	// First integer rotation (fast)
	int nDiff = ((nRotation - m_nRotation) + 360) % 360;
	if (nDiff != 0) {
		return Rotate(nDiff);
	}
/*
	if (fabs(rotationParams.FreeRotation - m_rotationParams.FreeRotation) >= 0.009)
	{
		return RotateOriginalPixels(2 * 3.141592653  * rotationParams.FreeRotation / 360, 
			GetRotationFlag(rotationParams.Flags, RFLAG_AutoCrop), GetRotationFlag(rotationParams.Flags, RFLAG_KeepAspectRatio));
	}
*/
	return true;
	}

bool CJPEGImage::Rotate(int nRotation) {
	double dStartTickCount = Helpers::GetExactTickCount();

	// Rotation can only be done in 32 bpp
	if (!ConvertSrcTo4Channels()) {
		return false;
	}

	InvalidateAllCachedPixelData();
	void* pNewOriginalPixels = CBasicProcessing::Rotate32bpp(m_nOrigWidth, m_nOrigHeight, m_pOrigPixels, nRotation);
	if (pNewOriginalPixels == NULL) return false;
	delete[] m_pOrigPixels;
	m_pOrigPixels = pNewOriginalPixels;
	if (nRotation != 180) {
		// swap width and height
		int nTemp = m_nOrigWidth;
		m_nOrigWidth = m_nOrigHeight;
		m_nOrigHeight = nTemp;
	}
	m_nRotation = (m_nRotation + nRotation) % 360;

	m_dLastOpTickCount = Helpers::GetExactTickCount() - dStartTickCount;
	return true;
}

bool CJPEGImage::Mirror(bool bHorizontally) {
	double dStartTickCount = Helpers::GetExactTickCount();

	// Rotation can only be done in 32 bpp
	if (!ConvertSrcTo4Channels()) {
		return false;
	}

	InvalidateAllCachedPixelData();
	void* pNewOriginalPixels = bHorizontally ? CBasicProcessing::MirrorH32bpp(m_nOrigWidth, m_nOrigHeight, m_pOrigPixels) :
		CBasicProcessing::MirrorV32bpp(m_nOrigWidth, m_nOrigHeight, m_pOrigPixels);
	if (pNewOriginalPixels == NULL) return false;
	delete[] m_pOrigPixels;
	m_pOrigPixels = pNewOriginalPixels;
//	MarkAsDestructivelyProcessed();
//	m_bIsProcessedNoParamDB = true;

	m_dLastOpTickCount = Helpers::GetExactTickCount() - dStartTickCount;
	return true;
	}

void* CJPEGImage::DIBPixelsLastProcessed(bool bGenerateDIBIfNeeded) {
	if (bGenerateDIBIfNeeded && m_pLastDIB == NULL) {
		m_pLastDIB = GetDIB(m_FullTargetSize, m_ClippingSize, m_TargetOffset, m_eProcFlags);
	}
	return m_pLastDIB;
}

void CJPEGImage::SetFileDependentProcessParams(LPCTSTR sFileName, CProcessParams* pParams) {
	pParams->Rotation = GetRotationFromEXIF(pParams->Rotation);
	m_nInitialRotation = pParams->Rotation;
	m_dInitialZoom = pParams->Zoom;
	m_initialOffsets = pParams->Offsets;
	m_eProcFlagsInitial = pParams->ProcFlags;
//	m_imageProcParamsInitial = pParams->ImageProcParams;
}

void CJPEGImage::DIBToOrig(float & fX, float & fY) {
	float fXo = m_TargetOffset.x + fX;
	float fYo = m_TargetOffset.y + fY;
	fX = fXo/m_FullTargetSize.cx*m_nOrigWidth;
	fY = fYo/m_FullTargetSize.cy*m_nOrigHeight;
}

void CJPEGImage::OrigToDIB(float & fX, float & fY) {
	float fXo = fX/m_nOrigWidth*m_FullTargetSize.cx;
	float fYo = fY/m_nOrigHeight*m_FullTargetSize.cy;
	fX = fXo - m_TargetOffset.x;
	fY = fYo - m_TargetOffset.y;
}
/*
__int64 CJPEGImage::GetUncompressedPixelHash() const { 
	return (m_pLDC == NULL) ? 0 : m_pLDC->GetPixelHash(); 
}
*/
///////////////////////////////////////////////////////////////////////////////////
// Private
///////////////////////////////////////////////////////////////////////////////////

void* CJPEGImage::GetDIBInternal(CSize fullTargetSize, CSize clippingSize, CPoint targetOffset,
								 EProcessingFlags eProcFlags,
								 bool &bParametersChanged) {

 	// Check if resampling due to bHighQualityResampling parameter change is needed
	bool bMustResampleQuality = GetProcessingFlag(eProcFlags, PFLAG_HighQualityResampling) != GetProcessingFlag(m_eProcFlags, PFLAG_HighQualityResampling);
	bool bTargetSizeChanged = fullTargetSize != m_FullTargetSize;

	// Check if resampling due to change of geometric parameters is needed
	bool bMustResampleGeometry = bTargetSizeChanged || clippingSize != m_ClippingSize || targetOffset != m_TargetOffset;

	EResizeType eResizeType = GetResizeType(fullTargetSize, CSize(m_nOrigWidth, m_nOrigHeight));

	// the geometrical parameters must be set before calling ApplyCorrectionLUT()
	CRect oldClippingRect = CRect(m_TargetOffset, m_ClippingSize);
	m_FullTargetSize = fullTargetSize;
	m_ClippingSize = clippingSize;
	m_TargetOffset = targetOffset;

	double dStartTickCount = Helpers::GetExactTickCount();

	// Check if only the LUT must be reapplied but no resampling (resampling is much slower than the LUTs)
	void * pDIB = NULL;

	if (!bMustResampleQuality && !bMustResampleGeometry) {
		// no resizing needed (maybe even nothing must be done)
		pDIB = ApplyCorrectionLUTandLDC(eProcFlags, m_pDIBPixelsLUTProcessed, fullTargetSize, targetOffset, m_pDIBPixels, clippingSize, bMustResampleGeometry, false, false, bParametersChanged);
		}

	// ApplyCorrectionLUTandLDC() could have failed, then recreate the DIBs
	if (pDIB == NULL)
		{
		// if the image is reprocessed more than once, it is worth to convert the original to 4 channels
		// as this is faster for further processing
		if (!m_bFirstReprocessing)
			ConvertSrcTo4Channels();

		bParametersChanged = true;

		// If we only pan, we can resample far more efficiently by only calculating the newly visible areas
		bool bPanningOnly = !m_bFirstReprocessing && !bTargetSizeChanged && !bMustResampleQuality;
		m_bFirstReprocessing = false;
		if (bPanningOnly)
			ResampleWithPan(m_pDIBPixels, m_pDIBPixelsLUTProcessed, fullTargetSize, clippingSize, targetOffset, oldClippingRect, eProcFlags, eResizeType);
		else
			{
			delete[] m_pDIBPixelsLUTProcessed; m_pDIBPixelsLUTProcessed = NULL;
			delete[] m_pDIBPixels; m_pDIBPixels = NULL;
			}

		// both DIBs are NULL, do normal resampling
		if (m_pDIBPixels == NULL && m_pDIBPixelsLUTProcessed == NULL)
			m_pDIBPixels = Resample(fullTargetSize, clippingSize, targetOffset, eProcFlags, eResizeType);

		// if ResampleWithPan() has preseved this DIB, we can reuse it
		if (m_pDIBPixelsLUTProcessed == NULL)
			pDIB = ApplyCorrectionLUTandLDC(eProcFlags, m_pDIBPixelsLUTProcessed, fullTargetSize, targetOffset, m_pDIBPixels, clippingSize, bMustResampleGeometry, false, false);
		else
			pDIB = m_pDIBPixelsLUTProcessed;
		}

	m_dLastOpTickCount = Helpers::GetExactTickCount() - dStartTickCount; 

	// set these parameters after ApplyCorrectionLUT() - else it cannot be detected that the parameters changed
	m_eProcFlags = eProcFlags;
	m_pLastDIB = pDIB;

	return pDIB;
	}

void* CJPEGImage::ApplyCorrectionLUTandLDC(EProcessingFlags eProcFlags,
										   void * & pCachedTargetDIB, CSize fullTargetSize, CPoint targetOffset, 
										   void * pSourceDIB, CSize dibSize,
										   bool bGeometryChanged, bool bOnlyCheck, bool bCanTakeOwnershipOfSourceDIB, bool &bParametersChanged)
	{
	bParametersChanged = false;

	if (pCachedTargetDIB != NULL)
		return pCachedTargetDIB;

	// If it shall only be checked if this method would be able to reuse the existing pCachedTargetDIB, return
	if (bOnlyCheck)
		return NULL;

	if (pSourceDIB == NULL)
		return NULL;

	delete[] pCachedTargetDIB;
	pCachedTargetDIB = NULL;
	
	if (bCanTakeOwnershipOfSourceDIB)
		pCachedTargetDIB = pSourceDIB;

	return pSourceDIB;
	}

bool CJPEGImage::ConvertSrcTo4Channels() {
	if (m_nOriginalChannels == 3) {
		void* pNewOriginalPixels = CBasicProcessing::Convert3To4Channels(m_nOrigWidth, m_nOrigHeight, m_pOrigPixels);
		if (pNewOriginalPixels != NULL) {
			delete[] m_pOrigPixels;
			m_pOrigPixels = pNewOriginalPixels;
			m_nOriginalChannels = 4;
		}
		return pNewOriginalPixels != NULL;
	}
	return true;
}

EProcessingFlags CJPEGImage::GetProcFlagsIncludeExcludeFolders(LPCTSTR sFileName, EProcessingFlags procFlags) const
	{
	return procFlags;
	}

CSize CJPEGImage::SizeAfterRotation(int nRotation) {
	int nDiff = ((nRotation - m_nRotation) + 360) % 360;
	if (nDiff == 90 || nDiff == 270) {
		return CSize(m_nOrigHeight, m_nOrigWidth);
	} else {
		return CSize(m_nOrigWidth, m_nOrigHeight);
	}
}

CJPEGImage::EResizeType CJPEGImage::GetResizeType(CSize targetSize, CSize sourceSize) {
	if (targetSize.cx == sourceSize.cx && targetSize.cy == sourceSize.cy) {
		return NoResize;
	} else if (targetSize.cx <= sourceSize.cx && targetSize.cy <= sourceSize.cy) {
		return DownSample;
	} else {
		return UpSample;
	}
}

int CJPEGImage::GetRotationFromEXIF(int nOrigRotation)
	{
/*GF*/	TCHAR debugtext[512];

	if (m_pEXIFReader != NULL && m_pEXIFReader->ImageOrientationPresent() && CSettingsProvider::This().AutoRotateEXIF())
		{
		// Some tools rotate the pixel data but do not reset the EXIF orientation flag.
		// In this case the EXIF thumbnail is normally also not rotated.
		// So check if the thumbnail orientation is the same as the image orientation.
		// If not, it can be assumed that someone touched the pixels and we ignore the EXIF
		// orientation.
		if (m_pEXIFReader->GetThumbnailWidth() > 0 && m_pEXIFReader->GetThumbnailHeight() > 0)
			{
			bool bWHOrig = m_nInitOrigWidth > m_nInitOrigHeight;
			bool bWHThumb = m_pEXIFReader->GetThumbnailWidth() > m_pEXIFReader->GetThumbnailHeight();
			if (bWHOrig != bWHThumb)
				{
/*GF*/	swprintf(debugtext,255,TEXT("GetRotationFromEXIF(int nOrigRotation=%d) Note: EXIF thumbnail (%dx%d) has different ratio than image (%dx%d)"), nOrigRotation, m_pEXIFReader->GetThumbnailWidth(), m_pEXIFReader->GetThumbnailHeight(), m_nInitOrigWidth, m_nInitOrigHeight);
/*GF*/	::OutputDebugStringW(debugtext);

//GF			return nOrigRotation;
				}
			}

/*GF*/	swprintf(debugtext,255,TEXT("GetRotationFromEXIF(int nOrigRotation=%d) EXIF rotation found: %d  (values: 0=0° / 3=180° / 6=90° / 8=270°)"), nOrigRotation, m_pEXIFReader->GetImageOrientation());
/*GF*/	::OutputDebugStringW(debugtext);

		switch (m_pEXIFReader->GetImageOrientation())
			{
			case 1:
				m_bRotationByEXIF = true;
				return 0;
			case 3:
				m_bRotationByEXIF = true;
				return 180;
			case 6:
				m_bRotationByEXIF = true;
				return 90;
			case 8:
				m_bRotationByEXIF = true;
				return 270;
			}
		}
	else
		{
		/*GF*/	swprintf(debugtext,255,TEXT("GetRotationFromEXIF(int nOrigRotation=%d) No EXIF rotation found"), nOrigRotation);
		/*GF*/	::OutputDebugStringW(debugtext);
		}

	return nOrigRotation;
	}

void CJPEGImage::MarkAsDestructivelyProcessed() {
	m_bIsDestructivlyProcessed = true;
	//m_rotationParams.FreeRotation = 0.0;
	//m_rotationParams.Flags = RFLAG_None;
}

void CJPEGImage::InvalidateAllCachedPixelData() {
	m_pLastDIB = NULL;
	delete[] m_pDIBPixels; 
	m_pDIBPixels = NULL;
	delete[] m_pDIBPixelsLUTProcessed; 
	m_pDIBPixelsLUTProcessed = NULL;
	m_ClippingSize = CSize(0, 0);
}
