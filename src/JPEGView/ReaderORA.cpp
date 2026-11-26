#include "StdAfx.h"
#include "ReaderORA.h"

#include <gdiplus.h>
#include <shlwapi.h>
#include <vector>
#include <string>
#include <zlib.h>

#include "JPEGImage.h"
#include "MaxImageDef.h"

using namespace Gdiplus;

namespace {

uint16_t Rd16(const std::vector<uint8_t>& b, size_t p) {
	return (uint16_t)((uint16_t)b[p] | ((uint16_t)b[p + 1] << 8));
}

uint32_t Rd32(const std::vector<uint8_t>& b, size_t p) {
	return (uint32_t)b[p] | ((uint32_t)b[p + 1] << 8) | ((uint32_t)b[p + 2] << 16) | ((uint32_t)b[p + 3] << 24);
}

// ZIP 里 method=8 是裸 deflate（不带 zlib 头），所以用 -15 的窗口
bool InflateRaw(const uint8_t* pSrc, size_t nSrcSize, size_t nOutSize, std::vector<uint8_t>& out) {
	out.clear();
	if (nOutSize == 0) {
		return true;
	}
	out.resize(nOutSize);
	z_stream zs;
	memset(&zs, 0, sizeof(zs));
	zs.next_in = (Bytef*)pSrc;
	zs.avail_in = (uInt)nSrcSize;
	zs.next_out = (Bytef*)out.data();
	zs.avail_out = (uInt)nOutSize;
	if (inflateInit2(&zs, -15) != Z_OK) {
		out.clear();
		return false;
	}
	int nRet = inflate(&zs, Z_FINISH);
	bool bOk = nRet == Z_STREAM_END && zs.total_out == nOutSize;
	inflateEnd(&zs);
	if (!bOk) {
		out.clear();
	}
	return bOk;
}

// 在中央目录里按名字找条目并解出来（method 0=store / 8=deflate，其余不认）
bool ExtractEntry(const std::vector<uint8_t>& b, const char* sName, std::vector<uint8_t>& out) {
	out.clear();
	if (b.size() < 22) {
		return false;
	}
	// EOCD 在文件尾部，注释最长 65535，所以只看尾部这段
	int64_t nMin = (int64_t)b.size() - 22 - 65535;
	if (nMin < 0) {
		nMin = 0;
	}
	int64_t nEocd = -1;
	for (int64_t i = (int64_t)b.size() - 22; i >= nMin; i--) {
		if (Rd32(b, (size_t)i) == 0x06054b50u) {
			nEocd = i;
			break;
		}
	}
	if (nEocd < 0) {
		return false;
	}

	size_t nCount = Rd16(b, (size_t)nEocd + 10);
	size_t nCdOffset = Rd32(b, (size_t)nEocd + 16);
	size_t p = nCdOffset;
	for (size_t i = 0; i < nCount; i++) {
		if (p + 46 > b.size() || Rd32(b, p) != 0x02014b50u) {
			return false;
		}
		uint16_t nMethod = Rd16(b, p + 10);
		uint32_t nCompSize = Rd32(b, p + 20);
		uint32_t nUncompSize = Rd32(b, p + 24);
		uint16_t nNameLen = Rd16(b, p + 28);
		uint16_t nExtraLen = Rd16(b, p + 30);
		uint16_t nCommentLen = Rd16(b, p + 32);
		uint32_t nLocalOffset = Rd32(b, p + 42);
		if (p + 46 + nNameLen > b.size()) {
			return false;
		}
		std::string sEntryName((const char*)b.data() + p + 46, nNameLen);
		p += 46 + nNameLen + nExtraLen + nCommentLen;

		if (sEntryName != sName) {
			continue;
		}
		if ((size_t)nLocalOffset + 30 > b.size() || Rd32(b, (size_t)nLocalOffset) != 0x04034b50u) {
			return false;
		}
		size_t nDataPos = (size_t)nLocalOffset + 30 + Rd16(b, (size_t)nLocalOffset + 26) + Rd16(b, (size_t)nLocalOffset + 28);
		if (nDataPos + nCompSize > b.size()) {
			return false;
		}
		if (nMethod == 0) {
			out.assign(b.begin() + (ptrdiff_t)nDataPos, b.begin() + (ptrdiff_t)(nDataPos + nCompSize));
			return true;
		} else if (nMethod == 8) {
			return InflateRaw(b.data() + nDataPos, nCompSize, nUncompSize, out);
		}
		return false;
	}
	return false;
}

} // namespace

uint32* CReaderORA::DecodePngMemory(const uint8_t* pData, size_t nSize, int& nWidth, int& nHeight) {
	if (pData == NULL || nSize == 0) {
		return NULL;
	}
	IStream* pStream = SHCreateMemStream(pData, (UINT)nSize);
	if (pStream == NULL) {
		return NULL;
	}
	uint32* pPixels = NULL;
	Gdiplus::Bitmap* pBitmap = Gdiplus::Bitmap::FromStream(pStream, FALSE);
	if (pBitmap != NULL && pBitmap->GetLastStatus() == Gdiplus::Ok) {
		nWidth = (int)pBitmap->GetWidth();
		nHeight = (int)pBitmap->GetHeight();
		if (nWidth > 0 && nHeight > 0 && nWidth <= MAX_IMAGE_DIMENSION && nHeight <= MAX_IMAGE_DIMENSION) {
			Gdiplus::Rect rect(0, 0, nWidth, nHeight);
			Gdiplus::BitmapData bmData;
			if (pBitmap->LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bmData) == Gdiplus::Ok) {
				pPixels = new(std::nothrow) uint32[(size_t)nWidth * nHeight];
				if (pPixels != NULL) {
					const uint8_t* pSrc = (const uint8_t*)bmData.Scan0;
					for (int j = 0; j < nHeight; j++) {
						memcpy(pPixels + (size_t)j * nWidth, pSrc + (size_t)j * bmData.Stride, (size_t)nWidth * 4);
					}
				}
				pBitmap->UnlockBits(&bmData);
			}
		}
	}
	if (pBitmap != NULL) {
		delete pBitmap; // FromStream 的位图要在流关闭前释放
	}
	pStream->Release();
	return pPixels;
}

CJPEGImage* CReaderORA::ReadImage(LPCTSTR sFileName, bool& bOutOfMemory) {
	bOutOfMemory = false;

	std::vector<uint8_t> data;
	FILE* fptr = _tfopen(sFileName, _T("rb"));
	if (fptr == NULL) {
		return NULL;
	}
	fseek(fptr, 0, SEEK_END);
	long nFileSize = ftell(fptr);
	fseek(fptr, 0, SEEK_SET);
	if (nFileSize > 0 && (int64_t)nFileSize <= 1024LL * 1024 * 1024) {
		data.resize((size_t)nFileSize);
		if (fread(data.data(), 1, (size_t)nFileSize, fptr) != (size_t)nFileSize) {
			data.clear();
		}
	}
	fclose(fptr);
	if (data.size() < 22) {
		return NULL;
	}

	// 只认编辑器（E:\GitHub\C\ImageEditor）写出的 mergedimage.png：整张画布的合并结果
	std::vector<uint8_t> png;
	if (!ExtractEntry(data, "mergedimage.png", png) || png.empty()) {
		return NULL;
	}

	int nWidth = 0;
	int nHeight = 0;
	uint32* pPixels = DecodePngMemory(png.data(), png.size(), nWidth, nHeight);
	if (pPixels == NULL) {
		return NULL;
	}

	return new CJPEGImage(nWidth, nHeight, pPixels, NULL, 4, 0, IF_ORA, false, 0, 1, 0);
}
