#include "StdAfx.h"
#include "ReaderICO.h"
#include "PNGWrapper.h"
#include "MaxImageDef.h"

namespace {

struct SBmpInfoHeader {
	uint32 nSize;
	int32 nWidth;
	int32 nHeight;      // twice the icon height: XOR bitmap plus AND mask
	uint16 nPlanes;
	uint16 nBitCount;
	uint32 nCompression;
	uint32 nSizeImage;
	int32 nXPelsPerMeter;
	int32 nYPelsPerMeter;
	uint32 nClrUsed;
	uint32 nClrImportant;
};

bool HasPngSignature(const uint8* p, uint32 nSize) {
	return nSize >= 8 && p[0] == 0x89 && p[1] == 'P' && p[2] == 'N' && p[3] == 'G' &&
		p[4] == 0x0d && p[5] == 0x0a && p[6] == 0x1a && p[7] == 0x0a;
}

// Converts the classic DIB payload of an icon entry into a 4-byte BGRA, top-down DIB.
// Returns NULL if the payload is not a DIB we understand.
void* ReadClassicDib(const uint8* pData, uint32 nSize, int& nWidth, int& nHeight, bool& bOutOfMemory) {
	if (nSize < sizeof(SBmpInfoHeader)) {
		return NULL;
	}
	SBmpInfoHeader hdr;
	memcpy(&hdr, pData, sizeof(hdr));
	if (hdr.nSize < 40 || hdr.nSize > nSize) {
		return NULL;
	}
	if (hdr.nCompression != 0 || hdr.nPlanes != 1) {
		return NULL;
	}

	nWidth = hdr.nWidth;
	nHeight = abs(hdr.nHeight) / 2;
	if (nWidth <= 0 || nHeight <= 0 || nWidth > MAX_IMAGE_DIMENSION || nHeight > MAX_IMAGE_DIMENSION ||
		(double)nWidth * nHeight > MAX_IMAGE_PIXELS) {
		return NULL;
	}

	uint16 nBPP = hdr.nBitCount;
	if (nBPP != 1 && nBPP != 4 && nBPP != 8 && nBPP != 24 && nBPP != 32) {
		return NULL;
	}

	// Palette (only present for the indexed depths), each entry is BGRx.
	uint32 nColors = (nBPP <= 8) ? (hdr.nClrUsed == 0 ? (1u << nBPP) : hdr.nClrUsed) : 0;
	uint32 nPaletteBytes = nColors * 4;
	unsigned long long nXorOffset = hdr.nSize + nPaletteBytes;
	uint32 nRowStride = ((nWidth * nBPP + 31) / 32) * 4;
	unsigned long long nXorBytes = (unsigned long long)nRowStride * nHeight;
	unsigned long long nMaskOffset = nXorOffset + nXorBytes;
	uint32 nMaskStride = ((nWidth + 31) / 32) * 4;
	unsigned long long nMaskBytes = (unsigned long long)nMaskStride * nHeight;

	if (nXorOffset > nSize || nXorOffset + nXorBytes > nSize) {
		return NULL;
	}
	bool bHasMask = (nMaskOffset + nMaskBytes <= nSize);
	if (bHasMask == false) {
		// Some writers omit the mask entirely when every pixel carries alpha. The alpha
		// channel then has to be trusted; without either we cannot tell, so treat as opaque.
	}

	uint8* pDIB = (uint8*)malloc((size_t)nWidth * nHeight * 4);
	if (pDIB == NULL) {
		bOutOfMemory = true;
		return NULL;
	}

	const uint8* pPalette = pData + hdr.nSize;
	const uint8* pXor = pData + nXorOffset;
	const uint8* pMask = bHasMask ? (pData + nMaskOffset) : NULL;

	// A 32 bpp icon that carries no alpha at all is in practice a 24 bpp icon plus a mask,
	// so the mask decides transparency in that case too.
	bool bUseAlphaChannel = (nBPP == 32);
	if (bUseAlphaChannel) {
		bool bAnyAlpha = false;
		for (int y = 0; y < nHeight && bAnyAlpha == false; y++) {
			const uint8* pRow = pXor + (size_t)(nHeight - 1 - y) * nRowStride;
			for (int x = 0; x < nWidth; x++) {
				if (pRow[x * 4 + 3] != 0) {
					bAnyAlpha = true;
					break;
				}
			}
		}
		bUseAlphaChannel = bAnyAlpha;
	}

	for (int y = 0; y < nHeight; y++) {
		// DIB rows are bottom-up, the DIB we hand out is top-down.
		const uint8* pRow = pXor + (size_t)(nHeight - 1 - y) * nRowStride;
		uint8* pOut = pDIB + (size_t)y * nWidth * 4;
		for (int x = 0; x < nWidth; x++) {
			uint8 bTransparent = 0;
			if (pMask != NULL) {
				const uint8* pMaskRow = pMask + (size_t)(nHeight - 1 - y) * nMaskStride;
				bTransparent = (pMaskRow[x >> 3] & (0x80 >> (x & 7))) ? 1 : 0;
			}

			uint8 bR = 0, bG = 0, bB = 0, bA = 255;
			if (nBPP == 32) {
				bB = pRow[x * 4 + 0];
				bG = pRow[x * 4 + 1];
				bR = pRow[x * 4 + 2];
				bA = bUseAlphaChannel ? pRow[x * 4 + 3] : (bTransparent ? 0 : 255);
			} else if (nBPP == 24) {
				bB = pRow[x * 3 + 0];
				bG = pRow[x * 3 + 1];
				bR = pRow[x * 3 + 2];
				bA = bTransparent ? 0 : 255;
			} else {
				uint32 nIndex;
				if (nBPP == 8) {
					nIndex = pRow[x];
				} else if (nBPP == 4) {
					uint8 bByte = pRow[x >> 1];
					nIndex = (x & 1) ? (bByte & 0x0F) : (bByte >> 4);
				} else {
					nIndex = (pRow[x >> 3] & (0x80 >> (x & 7))) ? 1 : 0;
				}
				if (nIndex >= nColors) {
					nIndex = 0;
				}
				bB = pPalette[nIndex * 4 + 0];
				bG = pPalette[nIndex * 4 + 1];
				bR = pPalette[nIndex * 4 + 2];
				bA = bTransparent ? 0 : 255;
			}

			pOut[x * 4 + 0] = bB;
			pOut[x * 4 + 1] = bG;
			pOut[x * 4 + 2] = bR;
			pOut[x * 4 + 3] = bA;
		}
	}

	return pDIB;
}

} // namespace

void* IcoReader::ReadImage(int& width, int& height, bool& outOfMemory, void* buffer, size_t sizebytes) {
	width = 0;
	height = 0;
	outOfMemory = false;

	if (buffer == NULL || sizebytes < 6 + 16) {
		return NULL;
	}
	const uint8* pData = (const uint8*)buffer;

	uint16 nType = *(const uint16*)(pData + 2);
	uint16 nCount = *(const uint16*)(pData + 4);
	if ((nType != 1 && nType != 2) || nCount == 0 || nCount > 512) {
		return NULL;
	}

	// Pick the entry with the largest pixel count, preferring 32 bpp on a tie. Icons often
	// ship a whole family (16, 32, 48, 256); the window shows one image, so take the biggest.
	int nBest = -1;
	uint32 nBestArea = 0;
	uint16 nBestBPP = 0;
	for (int i = 0; i < nCount; i++) {
		const uint8* pEntry = pData + 6 + i * 16;
		uint32 nW = (pEntry[0] == 0) ? 256 : pEntry[0];
		uint32 nH = (pEntry[1] == 0) ? 256 : pEntry[1];
		uint16 nBPP = *(const uint16*)(pEntry + 6);
		uint32 nOffset = *(const uint32*)(pEntry + 12);
		uint32 nBytes = *(const uint32*)(pEntry + 8);
		if (nOffset > sizebytes || nBytes == 0 || (unsigned long long)nOffset + nBytes > sizebytes) {
			continue;
		}
		uint32 nArea = nW * nH;
		if (nArea > nBestArea || (nArea == nBestArea && nBPP > nBestBPP)) {
			nBest = i;
			nBestArea = nArea;
			nBestBPP = nBPP;
		}
	}
	if (nBest < 0) {
		return NULL;
	}

	const uint8* pEntry = pData + 6 + nBest * 16;
	uint32 nOffset = *(const uint32*)(pEntry + 12);
	uint32 nBytes = *(const uint32*)(pEntry + 8);
	const uint8* pPayload = pData + nOffset;

	if (HasPngSignature(pPayload, nBytes)) {
		int nBPP = 0, nFrameCount = 1, nFrameTime = 0;
		bool bHasAnimation = false, bPngOOM = false;
		void* pEXIF = NULL;
		void* pPixels = PngReader::ReadImage(width, height, nBPP, bHasAnimation, nFrameCount, nFrameTime,
			pEXIF, bPngOOM, (void*)pPayload, nBytes);
		free(pEXIF);
		outOfMemory = (pPixels == NULL && bPngOOM);
		return pPixels;
	}

	return ReadClassicDib(pPayload, nBytes, width, height, outOfMemory);
}

