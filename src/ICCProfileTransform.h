#pragma once

// ICC color management is intentionally disabled in ImageBrowser (see 项目信息.md / 项目导航.md, decision A1).
// This translation unit exists only to keep call sites source-compatible. All entry points are no-ops:
// CreateTransform() returns NULL, DoTransform() returns false, so every caller falls back to a plain
// pixel copy. Nothing is ever color-converted.
//
// Do not "fix" this by re-adding lcms2: that would require a DLL, which the product forbids.
class ICCProfileTransform
{
public:
	enum PixelFormat {
		FORMAT_BGRA,
		FORMAT_RGBA,
		FORMAT_BGR,
		FORMAT_RGB,
		FORMAT_Lab,
		FORMAT_LabA,
	};

	static void* CreateTransform(const void* /*profile*/, unsigned int /*size*/, PixelFormat /*format*/) {
		return NULL;
	}

	static bool DoTransform(void* /*transform*/, const void* /*inputBuffer*/, void* /*outputBuffer*/,
		unsigned int /*width*/, unsigned int /*height*/, unsigned int /*stride*/ = 0) {
		return false;
	}

	static void DeleteTransform(void* /*transform*/) {
	}

	static void* CreateLabTransform(PixelFormat /*format*/) {
		return NULL;
	}
};
