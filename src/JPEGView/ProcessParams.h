#pragma once

#include "Helpers.h"

// Processing flags for image processing
enum EProcessingFlags
	{
	PFLAG_None = 0,
	PFLAG_HighQualityResampling = 1
	};

static inline bool GetProcessingFlag(EProcessingFlags eFlags, EProcessingFlags eFlagToGet)
	{
	return (eFlags & eFlagToGet) != 0;
	};

// Parameters used to process an image, including geometry
// only used within CJPEGProvider::RequestImage() which is used in
//			CJPEGProvider::StartNewRequest(),
//			CJPEGProvider::StartNewRequestBundle() which are used in
//				CImageLoadThread::AsyncLoad() which is used in
//					CImageLoadThread::CRequest()
class CProcessParams
	{
	public:
		// TargetWidth,TargetHeight is the dimension of the target output screen, the image is fit into this rectangle
		// nRotation must be 0, 90, 270 degrees
		// dZoom is the zoom factor compared to intial image size (1.0 means no zoom)
		// offsets are relative to center of image and refer to original image size (not zoomed)
		CProcessParams(int nTargetWidth, int nTargetHeight, int nRotation, double dZoom, CPoint offsets, EProcessingFlags eProcFlags)
			{
			TargetWidth = nTargetWidth;
			TargetHeight = nTargetHeight;
			Rotation = nRotation;
			Zoom = dZoom;
			Offsets = offsets;
			ProcFlags = eProcFlags;
			}

		int TargetWidth;
		int TargetHeight;
		int Rotation;
		double Zoom;
		CPoint Offsets;
		EProcessingFlags ProcFlags;
	};