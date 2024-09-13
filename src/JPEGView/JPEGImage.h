#pragma once

#include "ProcessParams.h"

class CHistogram;
class CLocalDensityCorr;
class CEXIFReader;
class CRawMetadata;
enum TJSAMP;

// Represents a rectangle to dim out in the image
struct CDimRect {
	CDimRect() {}
	CDimRect(float fFactor, const CRect& rect) { Factor = fFactor; Rect = rect; }
	float Factor; // between 0.0 and 1.0
	CRect Rect;
};

// Class holding a decoded image (not just JPEG - any supported format) and its meta data (if available).
class CJPEGImage {
public:
	// Ownership of memory in pPixels goes to class, accessing this pointer after the constructor has been called
	// may causes access violations (use OriginalPixels() instead).
	// nChannels can be 1 (greyscale image), 3 (BGR color image) or 4 (BGRA color image, A ignored)
	// pEXIFData can be a pointer to the APP1 block containing the EXIF data. If this pointer is null
	// no EXIF data is available.
	// The nJPEGHash hash value gives a hash over the compressed JPEG pixels that uniquely identifies the
	// JPEG image. It can be zero in which case a pixel based hash value is internally created.
	// The image format is a hint about the original image format this image was created from.
	// The bIsAnimation, nFrameIndex, nNumberOfFrames and nFrameTimeMs are used for multiframe images, e.g. animated GIFs.
	// Frame index is zero based and the frame time is given in milliseconds.
	// The pLDC object is used internally only for thumbnail image creation to avoid duplication. In all other situations,
	// its value must be NULL.
	// If RAW metadata is specified, ownership of this memory is transferred to this class.
	CJPEGImage(int nWidth, int nHeight, void* pPixels, void* pEXIFData, int nChannels, 
		__int64 nJPEGHash, EImageFormat eImageFormat, bool bIsAnimation, int nFrameIndex, int nNumberOfFrames, int nFrameTimeMs,
        CLocalDensityCorr* pLDC = NULL, bool bIsThumbnailImage = false, CRawMetadata* pRawMetadata = NULL);
	~CJPEGImage(void);

	// Gets resampled and processed 32 bpp DIB image (up or downsampled).
	// Parameters:
	// fullTargetSize: Full target size of resized image to render (without any clipping to actual clipping size)
	// clippingSize: Sub-rectangle in fullTargetSize to render (the returned DIB has this size)
	// targetOffset: Offset of the clipping rectangle in full target size
	// imageProcParams: Processing parameters, such as contrast, brightness and sharpen
	// eProcFlags: Processing flags, e.g. apply LDC
	// Return value:
	// The returned DIB is a 32 bpp DIB (BGRA). Note that the returned pointer must not
	// be deleted by the caller and is only valid until another call to GetDIB() is done, the CJPEGImage
	// object is disposed or the Rotate(), Crop() or any other method affecting original pixels is called!
	// Note: The method tries to satisfy consecuting requests as efficient as possible. Calling the GetDIB()
	// method multiple times with the same set of parameters will return the same image starting from the second
	// call without doing image processing anymore.
	void* GetDIB(CSize fullTargetSize, CSize clippingSize, CPoint targetOffset,
		EProcessingFlags eProcFlags) {
		bool bNotUsed;
		return GetDIBInternal(fullTargetSize, clippingSize, targetOffset, eProcFlags, bNotUsed);
		}

	// Gets the hash value of the pixels, for JPEGs the hash is on the compressed pixels
	__int64 GetPixelHash() const { return m_nPixelHash; }

	// Gets the pixel hash over the de-compressed pixels
	__int64 GetUncompressedPixelHash() const;

	// Original image size (of the unprocessed raw image, however the raw image may have been rotated or cropped)
	int OrigWidth() const { return m_nOrigWidth; }
	int OrigHeight() const { return m_nOrigHeight; }
	CSize OrigSize() const { return CSize(m_nOrigWidth, m_nOrigHeight); }

	// Size of DIB - size of resampled section of the original image. If zero, no DIB is currently processed.
	int DIBWidth() const { return m_ClippingSize.cx; }
	int DIBHeight() const { return m_ClippingSize.cy; }

	// Convert DIB coordinates into original image coordinates and vice versa
	void DIBToOrig(float & fX, float & fY);
	void OrigToDIB(float & fX, float & fY);

    // Gets or sets the JPEG chromo sampling. See turbojpeg.h for the TJSAMP enumeration.
    TJSAMP GetJPEGChromoSampling() { return m_eJPEGChromoSampling; }
    void SetJPEGChromoSampling(TJSAMP eSampling) { m_eJPEGChromoSampling = eSampling; }

	// Declare the generated DIB as invalid - forcing it to be regenerated on next access
	void SetDIBInvalid() { m_ClippingSize = CSize(0, 0); }

	// Verify that the image is currently rotated by the given angle and rotate the image if not. nRotation must be 0, 90, 180, 270
	bool VerifyRotation(int nRotation);

	// Rotate the image clockwise by 90, 180 or 270 degrees. All other angles are invalid.
	// Applies to original image!
	bool Rotate(int nRotation);

	// Mirrors the image horizontally or vertically.
	// Applies to original image!
	bool Mirror(bool bHorizontally);

	// Returns if this image has been cropped or not
	bool IsCropped() { return m_bCropped; }

	// Returns if this image's original pixels have been processed destructively (e.g. cropped or rotated by non-90 degrees steps)
	bool IsDestructivlyProcessed() { return m_bIsDestructivlyProcessed; }

	// raw access to input pixels - do not delete or store the pointer returned
	void* IJLPixels() { return  m_pOrigPixels; }
	const void* IJLPixels() const { return m_pOrigPixels; }
	// remove IJL pixels form class - will be NULL afterwards
	void DetachIJLPixels() { m_pOrigPixels = NULL; }

	// returns the number of channels in the IJLPixels (3 or 4, corresponding to 24 bpp and 32 bpp)
	int IJLChannels() const { return m_nOriginalChannels; }

	// raw access to DIB pixels with no LUT applied - do not delete or store the pointer returned
	// note that this DIB can be NULL due to optimization if currently only the processed DIB is maintained
	void* DIBPixels() { return m_pDIBPixels; }
	const void* DIBPixels() const { return m_pDIBPixels; }

	// Gets the DIB last processed. If none, the last used parameters are taken to generate the DIB
	void* DIBPixelsLastProcessed(bool bGenerateDIBIfNeeded);

	// Gets the image processing flags as set as default (may varies from file to file)
	EProcessingFlags GetInitialProcessFlags() const { return m_eProcFlagsInitial; }

	// Gets the image processing flags last used to process an image
	EProcessingFlags GetLastProcessFlags() const { return m_eProcFlags; }
	
	// Gets the rotation as set as default (may varies from file to file)
	int GetInitialRotation() const { return m_nInitialRotation; }

	// Gets the zoom as set as default (can vary from file to file)
	double GetInititialZoom() const { return m_dInitialZoom; }

	// Gets the offsets as set as default (can vary from file to file)
	CPoint GetInitialOffsets() const { return m_initialOffsets; }

	// To be called after creation of the object to intialize the initial processing parameters.
	// Input are the global defaults for the processing parameters, output (in pParams) are the
	// processing parameters for this file (maybe different from the global ones)
	void SetFileDependentProcessParams(LPCTSTR sFileName, CProcessParams* pParams);

	// Sets the regions of the returned DIB that are dimmed out (NULL for no dimming)
	void SetDimRects(const CDimRect* dimRects, int nSize);

	// Gets if the image was found in parameter DB
	bool IsInParamDB() const { return m_bInParamDB; }

	// Sets if the image is in the paramter DB (called after the user saves/deletes the image from param DB)
	void SetIsInParamDB(bool bSet) { m_bInParamDB = bSet; }

	// Gets the EXIF data block (including the APP1 marker as first two bytes) of the image if any
	void* GetEXIFData() { return m_pEXIFData; }

	// Gets the size of the EXIF data block in bytes, including the APP1 marker (2 bytes)
	int GetEXIFDataLength() { return m_nEXIFSize; }

	CEXIFReader* GetEXIFReader() { return m_pEXIFReader; }

	// Gets the image format this image was originally created from
	EImageFormat GetImageFormat() const { return m_eImageFormat; }

    // Gets if this image is part of an animation (GIF)
    bool IsAnimation() const { return m_bIsAnimation; }

    // Gets the frame index if this is a multiframe image, 0 otherwise
    int FrameIndex() const { return m_nFrameIndex; }

    // Gets the number of frames for multiframe images, 1 otherwise
    int NumberOfFrames() const { return m_nNumberOfFrames; }

	// Defaults to 100ms for frame times <=10, to match behavior of web browsers
	int FrameTimeMs() const { return m_nFrameTimeMs <= 10 ? 100 : m_nFrameTimeMs; }

	// Gets if this image was created from the clipboard
	bool IsClipboardImage() const { return m_eImageFormat == IF_CLIPBOARD; }

	// Gets if the rotation is given by EXIF
	bool IsRotationByEXIF() const { return m_bRotationByEXIF; }

	// Debug: Ticks (millseconds) of the last operation
	double LastOpTickCount() const { return m_dLastOpTickCount; }

	// Debug: Loading time of image in ms
	void SetLoadTickCount(double tc) { m_dLoadTickCount = tc; }
	double GetLoadTickCount() { return m_dLoadTickCount; }

	// Sets and gets the JPEG comment of this image (COM marker)
	void SetJPEGComment(LPCTSTR sComment) { m_sJPEGComment = CString(sComment); }
	LPCTSTR GetJPEGComment() { return m_sJPEGComment; }

	// Gets the metadata for RAW camera images, NULL if none
	CRawMetadata* GetRawMetadata() { return m_pRawMetadata; }

	// Converts the target offset from 'center of image' based format to pixel coordinate format 
	static CPoint ConvertOffset(CSize fullTargetSize, CSize clippingSize, CPoint targetOffset);

private:

	// used internally for re-sampling type
	enum EResizeType {
		NoResize,
		DownSample,
		UpSample
	};

	// Original pixel data - only rotations and crop are done directly on this data because this is non-destructive
	// The data is not modified in all other cases
	void* m_pOrigPixels;
	void* m_pEXIFData;
	CRawMetadata* m_pRawMetadata;
	int m_nEXIFSize;
	CEXIFReader* m_pEXIFReader;
	CString m_sJPEGComment;
	int m_nOrigWidth, m_nOrigHeight; // these may changes by rotation
	int m_nInitOrigWidth, m_nInitOrigHeight; // original width of image when constructed (before any rotation and crop)
	int m_nOriginalChannels;
	__int64 m_nPixelHash;
	EImageFormat m_eImageFormat;
    TJSAMP m_eJPEGChromoSampling;

    // multiframe and GIF animation related data
    bool m_bIsAnimation;
    int m_nFrameIndex;
    int m_nNumberOfFrames;
    int m_nFrameTimeMs;

	// thumbnail image for histogram of the processed image
	// this thumbnail is needed because not the whole image is processed, only the visible section,
	// however the histogram must be calculated over the whole processed image
	CJPEGImage* m_pHistogramThumbnail;

	// Thumbnail related stuff
	bool m_bIsThumbnailImage;

	// Processed data of size m_ClippingSize, with LUT/LDC applied and without
	// The version without LUT/LDC is used to efficiently reapply a different LUT/LDC
	// Size of the DIBs is m_ClippingSize
	void* m_pDIBPixelsLUTProcessed;
	void* m_pDIBPixels;
	void* m_pLastDIB; // one of the pointers above

	// Image processing parameters and flags during last call to GetDIB()
	EProcessingFlags m_eProcFlags;

	// Initial image processing parameters and flags, as set with SetFileDependentProcessParams()
	EProcessingFlags m_eProcFlagsInitial;
	int m_nInitialRotation;
	double m_dInitialZoom;
	CPoint m_initialOffsets;

	bool m_bCropped; // Image has been cropped
	bool m_bIsDestructivlyProcessed; // Original image pixels destructively processed (i.e. cropped or size changed)
	uint32 m_nRotation; // current rotation angle
	bool m_bRotationByEXIF; // is the rotation given by EXIF

	// This is the geometry that was requested during last GetDIB() call
	CSize m_FullTargetSize; 
	CSize m_ClippingSize; // this is the size of the DIB
	CPoint m_TargetOffset;
	double m_dRotationLQ; // low quality rotation angle

	bool m_bInParamDB; // true if image found in param DB
	bool m_bFirstReprocessing; // true if never reprocessed before, some optimizations may be not done initially
	int m_nNumDimRects;

	double m_dLastOpTickCount;
	double m_dLoadTickCount;
/*
	// stuff needed to perform LUT and LDC processing
	uint8* m_pLUTAllChannels; // for global contrast and brightness correction
	uint8* m_pLUTRGB; // B,G,R three channel LUT
	int32* m_pSaturationLUTs; // Saturation LUTs
	CLocalDensityCorr* m_pLDC;
	bool m_bLDCOwned;
	float m_fColorCorrectionFactors[6];
	float m_fColorCorrectionFactorsNull[6];
*/
	// Internal GetDIB() implementation combining unsharp mask and (low quality= rotation with GetDIB().
	// bUsingOriginalDIB is output parameter and contains if the cached DIB could be used and no processing has been done
	// When dRotation is not 0.0, PFLAG_HighQualityResampling must not be set
	void* GetDIBInternal(CSize fullTargetSize,
						 CSize clippingSize,
						 CPoint targetOffset,
						 EProcessingFlags eProcFlags,
						 bool& bParametersChanged);

	// Resample when panning was done, using existing data in DIBs. Old clipping rectangle is given in oldClippingRect
	void ResampleWithPan(void* & pDIBPixels, void* & pDIBPixelsLUTProcessed, CSize fullTargetSize, 
		CSize clippingSize, CPoint targetOffset, CRect oldClippingRect,
		EProcessingFlags eProcFlags, EResizeType eResizeType);

	// Resample to given target size. Returns resampled DIB
	void* Resample(CSize fullTargetSize, CSize clippingSize, CPoint targetOffset, EProcessingFlags eProcFlags, EResizeType eResizeType);

	// pCachedTargetDIB is a pointer at the caller side holding the old processed DIB.
	// Returns a pointer to DIB to be used (either pCachedTargetDIB or pSourceDIB)
	// If bOnlyCheck is set to true, the method does nothing but only checks if the existing processed DIB
	// can be used (return != NULL) or not (return == NULL)
	// The out parameter bParametersChanged returns if one of the parameters relevant for image processing has been changed since the last call
	void* ApplyCorrectionLUTandLDC(EProcessingFlags eProcFlags,
		void * & pCachedTargetDIB, CSize fullTargetSize, CPoint targetOffset, 
		void * pSourceDIB, CSize dibSize, bool bGeometryChanged, bool bOnlyCheck, bool bCanTakeOwnershipOfSourceDIB, bool &bParametersChanged);

	void* ApplyCorrectionLUTandLDC(EProcessingFlags eProcFlags,
		void * & pCachedTargetDIB, CSize fullTargetSize, CPoint targetOffset, 
		void * pSourceDIB, CSize dibSize, bool bGeometryChanged, bool bOnlyCheck, bool bCanTakeOwnershipOfSourceDIB) {
		bool bNotUsed;
		return ApplyCorrectionLUTandLDC(eProcFlags, pCachedTargetDIB, fullTargetSize, targetOffset, 
			pSourceDIB, dibSize, bGeometryChanged, bOnlyCheck, bCanTakeOwnershipOfSourceDIB, bNotUsed);
	}

	// makes sure that the input image (m_pOrigPixels) is a 4 channel BGRA image (converts if necessary)
	bool ConvertSrcTo4Channels();

	// Gets the processing flags according to the inclusion/exclusion list in INI file
	EProcessingFlags GetProcFlagsIncludeExcludeFolders(LPCTSTR sFileName, EProcessingFlags procFlags) const;

	// Return size of original image if the image would be rotated the given amount
	CSize SizeAfterRotation(int nRotation);

	// Get if from source to target size it is down or upsampling
	EResizeType GetResizeType(CSize targetSize, CSize sourceSize);

	// Gets the rotation from EXIF if available
	int GetRotationFromEXIF(int nOrigRotation);

	// Sets the m_bIsDestructivlyProcessed flag to true and resets rotation
	void MarkAsDestructivelyProcessed();

	// Called when the original pixels have changed (rotate, crop, unsharp mask), all cached pixel data gets invalid
	void InvalidateAllCachedPixelData();
};
