#pragma once

#include "ProcessParams.h"

// Upstream remembered the processing parameters of every image it had ever seen, keyed by a hash of
// the pixels, in a database under AppData. This viewer deliberately has no memory: nothing you do
// to an image is remembered, and switching files always starts from a neutral state.
//
// The class stays so the decoder's "is there a remembered entry for this file?" question still
// compiles, but the answer is always no, which sends the decoder down its default path.

class CParameterDBEntry {
public:
	void WriteToProcessParams(CImageProcessingParams& /*processParams*/, EProcessingFlags& /*eFlags*/,
		CRotationParams& /*rotationParams*/) const {}
	void WriteToGeometricParams(double& /*fZoom*/, CPoint& /*offsets*/, CSize /*sourceSize*/, CSize /*targetSize*/) {}
	void GetColorCorrectionAmounts(float /*corrections*/[6]) const {}
	bool HasZoomOffsetStored() const { return false; }
	bool IsStoredRelativeToScreenSize() const { return false; }
};

class CParameterDB {
public:
	static CParameterDB& This() {
		static CParameterDB s_instance;
		return s_instance;
	}

	// Always "no entry": ImageBrowser remembers nothing between sessions.
	CParameterDBEntry* FindEntry(__int64 /*nHash*/) { return NULL; }
};
