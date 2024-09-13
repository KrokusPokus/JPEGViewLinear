#include "StdAfx.h"
#include "XMMImage.h"
#include "ResizeFilter.h"
#include "ApplyFilterAVX.h"

#ifdef _WIN64

CFloatImage* ApplyFilter_AVX_f32(int nSourceHeight, int nTargetHeight, int nWidth,
	int nStartY_FP, int nStartX, int nIncrementY_FP,
	const AVXFilterKernelBlock& filter,
	int nFilterOffset, const CFloatImage* pSourceImg, bool bRoundResult) {

	int nStartXAligned = nStartX & ~7;
	int nEndXAligned = (nStartX + nWidth + 7) & ~7;


	CFloatImage* tempImage = new CFloatImage(nEndXAligned - nStartXAligned, nTargetHeight, 8);
	if (tempImage->AlignedPtr() == NULL) {
		delete tempImage;
		return NULL;
	}

	int nCurY = nStartY_FP;
	int nChannelLenBytes = pSourceImg->GetPaddedWidth() * sizeof(float);
	int nRowLenBytes = nChannelLenBytes * 3;
	int nNumberOfBlocksX = (nEndXAligned - nStartXAligned) >> 3;

	const uint8* pSourceStart = (const uint8*)pSourceImg->AlignedPtr() + nStartXAligned * sizeof(float);
	AVXFilterKernel** pKernelIndexStart = filter.Indices;

	_MM_ALIGN16 float YMM255[8] = {4095.0, 4095.0, 4095.0, 4095.0, 4095.0, 4095.0, 4095.0, 4095.0};

	__m256 ymm0 = _mm256_setzero_ps();
	__m256 ymm1 = *((__m256*)YMM255);
	__m256 ymm2;
	__m256 ymm3;
	__m256 ymm4 = _mm256_setzero_ps();
	__m256 ymm5 = _mm256_setzero_ps();
	__m256 ymm6 = _mm256_setzero_ps();
	__m256 ymm7;

	__m256* pDestination = (__m256*)tempImage->AlignedPtr();

	for (int y = 0; y < nTargetHeight; y++) {
		uint32 nCurYInt = (uint32)nCurY >> 16; // integer part of Y
		int filterIndex = y + nFilterOffset;
		AVXFilterKernel* pKernel = pKernelIndexStart[filterIndex];
		int filterLen = pKernel->FilterLen;
		int filterOffset = pKernel->FilterOffset;
		const __m256* pFilterStart = (__m256*)&(pKernel->Kernel);
		const __m256* pSourceRow = (const __m256*)(pSourceStart + ((int)nCurYInt - filterOffset) * nRowLenBytes);

		for (int x = 0; x < nNumberOfBlocksX; x++) {
			const __m256* pSource = pSourceRow;
			const __m256* pFilter = pFilterStart;
			ymm4 = _mm256_setzero_ps();
			ymm5 = _mm256_setzero_ps();
			ymm6 = _mm256_setzero_ps();
			for (int i = 0; i < filterLen; i++) {
				ymm7 = *pFilter;

				// the pixel data RED channel
				ymm2 = *pSource;
				ymm2 = _mm256_mul_ps(ymm2, ymm7);
				ymm4 = _mm256_add_ps(ymm4, ymm2);
				pSource = (__m256*)((uint8*)pSource + nChannelLenBytes);

				// the pixel data GREEN channel
				ymm3 = *pSource;
				ymm3 = _mm256_mul_ps(ymm2, ymm7);
				ymm5 = _mm256_add_ps(ymm5, ymm3);
				pSource = (__m256*)((uint8*)pSource + nChannelLenBytes);

				// the pixel data BLUE channel
				ymm2 = *pSource;
				ymm2 = _mm256_mul_ps(ymm2, ymm7);
				ymm6 = _mm256_add_ps(ymm6, ymm2);
				pSource = (__m256*)((uint8*)pSource + nChannelLenBytes);

				pFilter++;
			}

			if (bRoundResult == true) {
				// limit to range <=255 (in ymm1)
				ymm4 = _mm256_min_ps(ymm4, ymm1);
				ymm5 = _mm256_min_ps(ymm5, ymm1);
				ymm6 = _mm256_min_ps(ymm6, ymm1);

				// limit to range >=0 (in ymm0)
				ymm4 = _mm256_max_ps(ymm4, ymm0);
				ymm5 = _mm256_max_ps(ymm5, ymm0);
				ymm6 = _mm256_max_ps(ymm6, ymm0);

				// round to nearest integer
				ymm4 = _mm256_round_ps(ymm4, _MM_FROUND_TO_NEAREST_INT);
				ymm5 = _mm256_round_ps(ymm5, _MM_FROUND_TO_NEAREST_INT);
				ymm6 = _mm256_round_ps(ymm6, _MM_FROUND_TO_NEAREST_INT);
				}

			// store result in blocks
			*pDestination++ = ymm4;
			*pDestination++ = ymm5;
			*pDestination++ = ymm6;

			pSourceRow++;
		};

		nCurY += nIncrementY_FP;
	};

	return tempImage;
}

#endif