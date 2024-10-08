#pragma once

// Sizes are in bytes

#ifdef _WIN64
const unsigned int MAX_JPEG_FILE_SIZE = 1024 * 1024 * 300;
#else
const unsigned int MAX_JPEG_FILE_SIZE = 1024 * 1024 * 50;
#endif

#ifdef _WIN64
const unsigned int MAX_PNG_FILE_SIZE = 1024 * 1024 * 300;
#else
const unsigned int MAX_PNG_FILE_SIZE = 1024 * 1024 * 50;
#endif

#ifdef _WIN64
const unsigned int MAX_WEBP_FILE_SIZE = 1024 * 1024 * 300;
#else
const unsigned int MAX_WEBP_FILE_SIZE = 1024 * 1024 * 50;
#endif

#ifdef _WIN64
const unsigned int MAX_BMP_FILE_SIZE = 1024 * 1024 * 500;
#else
const unsigned int MAX_BMP_FILE_SIZE = 1024 * 1024 * 100;
#endif

#ifdef _WIN64
const unsigned int MAX_IMAGE_PIXELS = 4294836225;	// 65535 * 65535
#else
const unsigned int MAX_IMAGE_PIXELS = 10240 * 10240;
#endif

// That must not be bigger than 65535 due to internal limitations
const unsigned int MAX_IMAGE_DIMENSION = 65535;
