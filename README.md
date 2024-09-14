# JPEGViewLinear

This is a heavily customized version of the original JPEGView by David Kleiner.
It also incorporates some of the fixes from the more recent fork by Kevin M at https://github.com/sylikc/jpegview.

## Differences to the original version

* Abiliy to up- and downsample images in linear space when using SSE2.
* The available downsampling filters have been changed to offer the well known 'Hermite', 'Mitchell', 'Catrom' and 'Lanczos2' and give reference output identical to ImageMagick.
* Any image editing functionality has been removed. JPEGViewLinear is a pure viewer, focusing on speed and maximum image quality.

## Why?

While many of the big image editing tools like Photoshop and GIMP have been using linear light scaling for years, there currently seems to exist no plain image viewer giving correct output.
More information: http://www.ericbrasseur.org/gamma.html

## Formats Supported

JPEG, PNG, WEBP, GIF, BMP, TIFF.

## System Requirements

* 64-bit Windows OS
* CPU with SSE2
