
#pragma once

/// signed 8 bit integer value
typedef signed char int8;
/// unsigned 8 bit integer value
typedef unsigned char uint8;
/// signed 16 bit integer value
typedef signed short int16;
/// unsigned 16 bit integer value
typedef unsigned short uint16;
/// signed 32 bit integer value
typedef signed int int32;
/// unsigned 32 bit integer value
typedef unsigned int uint32;

enum EFilterType {
	Filter_Downsampling_None,
	Filter_Downsampling_Hermite,
	Filter_Downsampling_Mitchell,
	Filter_Downsampling_Catrom,
	Filter_Downsampling_Lanczos2,
	Filter_Upsampling_Bicubic
};

// Image formats (can be other than JPEG...)
enum EImageFormat {
	IF_JPEG,
	IF_WindowsBMP,
	IF_PNG,
	IF_GIF,
	IF_TIFF,
	IF_WEBP,
    IF_WIC,
	IF_CLIPBOARD,
	IF_CameraRAW,
    IF_JPEG_Embedded, // JPEG embedded in another file, e.g. camera raw
	IF_TGA,
	IF_Unknown
};

