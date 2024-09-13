#pragma once

class CFloatImage;
struct AVXFilterKernelBlock;

// Used by BasicProcessing.cpp: Applies a filter using AVX. Own compilation unit to be able to compile this with AVX compiler flag.

CFloatImage* ApplyFilter_AVX_f32(int nSourceHeight, int nTargetHeight, int nWidth,
	int nStartY_FP, int nStartX, int nIncrementY_FP,
	const AVXFilterKernelBlock& filter,
	int nFilterOffset, const CFloatImage* pSourceImg, bool bRoundResult);
