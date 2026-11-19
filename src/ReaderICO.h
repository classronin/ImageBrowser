#pragma once

// Reads a Windows icon (.ico) or cursor (.cur) container and returns the single best image
// inside it as a 4-byte BGRA, top-down DIB - the same layout PngReader produces.
//
// Windows ships two kinds of icon entries:
//   * a "classic" DIB: BITMAPINFOHEADER + XOR bitmap + 1 bit AND mask
//   * a PNG (Vista and later): the entry payload is simply a PNG file
// Both are handled here. For the DIB case the alpha channel is taken from the 32 bpp pixels
// and, for lower bit depths, reconstructed from the AND mask.
class IcoReader
{
public:
	static void* ReadImage(
		int& width,          // width of the image loaded, 0 on failure
		int& height,         // height of the image loaded, 0 on failure
		bool& outOfMemory,   // true when the failure was caused by allocation
		void* buffer,        // whole .ico file contents
		size_t sizebytes);   // size of those contents
};
