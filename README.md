# JPEGViewLinear

This is a heavily customized version of the original JPEGView by David Kleiner (dkleiner).
It also incorporates some of the fixes from the more recent fork by Kevin M (sylikc).

## Differences to the original version

* Abiliy to up- and downsamples images in linear space when using SSE2.
* The available downsampling filters have been changed to offer 'Hermite', 'Mitchell', 'Catrom' and 'Lanczos2'.
* Any image editing functionality has been removed. JPEGViewLinear is a pure viewer, focusing on speed and maximum image quality.

## Why?

While many of the big image editing tools like Photoshop and GIMP have been using linear light scaling for years, there seem to currently exist no image viewer giving correct output.
More information: http://www.ericbrasseur.org/gamma.html

## Formats Supported

JPEG, PNG, WEBP, GIF, BMP, TIFF.

## System Requirements

64-bit Windows OS
CPU that supports SSE2. (Basically any CPU since 2000 should do.)

## Note

If anybody finds a way to get the AVX2 path working without colour issues, let me know.