#pragma once

// Rasterises a static SVG document into 4-byte BGRA pixels with straight (not premultiplied) alpha.
// The renderer is Direct2D's own SVG parser, which Windows ships inside d2d1.dll - nothing has to be
// deployed next to the executable. Direct2D can only draw SVG into a D3D-backed device context, so a
// WARP software device is created for the single frame.
class SvgReader
{
public:
	// Returns a buffer of width * height * 4 bytes (BGRA), or NULL when the document cannot be
	// rendered. outOfMemory is set only when the failure was caused by memory.
	static void* ReadImage(LPCTSTR fileName, int& width, int& height, bool& outOfMemory);

	// Frees a buffer returned by ReadImage.
	static void FreeMemory(void* pointer);
};
