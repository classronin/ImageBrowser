
#include "StdAfx.h"
#include "ImageLoadThread.h"
#include <gdiplus.h>
#include <shlwapi.h>
#include <vector>
#include "JPEGImage.h"
#include "MessageDef.h"
#include "Helpers.h"
#include "SettingsProvider.h"
#include "ReaderBMP.h"
#include "ReaderTGA.h"
#include "ReaderORA.h"
#include "BasicProcessing.h"
#include "dcraw_mod.h"
#include "TJPEGWrapper.h"
#include "PNGWrapper.h"
#ifndef WINXP
#include "JXLWrapper.h"
#include "HEIFWrapper.h"
#include "AVIFWrapper.h"
#include "RAWWrapper.h"
#endif
#include "WEBPWrapper.h"
#include "QOIWrapper.h"
#include "PSDWrapper.h"
#include "MaxImageDef.h"


using namespace Gdiplus;

// static initializers
volatile int CImageLoadThread::m_curHandle = 0;

/////////////////////////////////////////////////////////////////////////////////////////////
// static helpers
/////////////////////////////////////////////////////////////////////////////////////////////

// find image format of this image by reading some header bytes
static EImageFormat GetImageFormat(LPCTSTR sFileName) {
	FILE *fptr;
	if ((fptr = _tfopen(sFileName, _T("rb"))) == NULL) {
		return IF_Unknown;
	}
	unsigned char header[16];
	int nSize = (int)fread((void*)header, 1, 16, fptr);
	fclose(fptr);
	if (nSize < 2) {
		return IF_Unknown;
	}

	if (header[0] == 0x42 && header[1] == 0x4d) {
		return IF_WindowsBMP;
	} else if (header[0] == 0xff && header[1] == 0xd8) {
		return IF_JPEG;
	} else if (header[0] == 0x89 && header[1] == 'P' && header[2] == 'N' && header[3] == 'G' &&
		header[4] == 0x0d && header[5] == 0x0a && header[6] == 0x1a && header[7] == 0x0a) {
		return IF_PNG;
	} else if (header[0] == 'G' && header[1] == 'I' && header[2] == 'F' && header[3] == '8' &&
		(header[4] == '7' || header[4] == '9') && header[5] == 'a') {
		return IF_GIF;
	} else if (header[0] == 'R' && header[1] == 'I' && header[2] == 'F' && header[3] == 'F' &&
		header[8] == 'W' && header[9] == 'E' && header[10] == 'B' && header[11] == 'P') {
		return IF_WEBP;
	} else if ((header[0] == 0xff && header[1] == 0x0a) ||
		memcmp(header, "\x00\x00\x00\x0cJXL\x20\x0d\x0a\x87\x0a", 12) == 0) {
		return IF_JXL;
	} else if (!memcmp(header+4, "ftyp", 4)) {
		// https://github.com/strukturag/libheif/issues/83
		// https://github.com/strukturag/libheif/blob/ce1e4586b6222588c5afcd60c7ba9caa86bcc58c/libheif/heif.h#L602-L805

		// AV1: avif, avis
		if (!memcmp(header+8, "avi", 3))
			return IF_AVIF;
		// H265: heic, heix, hevc, hevx, heim, heis, hevm, hevs
		if (!memcmp(header+8, "hei", 3) || !memcmp(header+8, "hev", 3))
			return IF_HEIF;
		// Canon CR3
		if (!memcmp(header+8, "crx ", 4))
			return IF_CameraRAW;
	} else if (header[0] == 'q' && header[1] == 'o' && header[2] == 'i' && header[3] == 'f') {
		return IF_QOI;
	} else if (header[0] == '8' && header[1] == 'B' && header[2] == 'P' && header[3] == 'S') {
		return IF_PSD;
	} else if (header[0] == 0 && header[1] == 0 && (header[2] == 1 || header[2] == 2) && header[3] == 0) {
		// ICO / CUR: 目录头 = reserved(2)=0, type(2)=1|2, count(2)
		return IF_ICO;
	}

	// default fallback if no matches based on magic bytes
	EImageFormat eImageFormat = Helpers::GetImageFormat(sFileName);

	if (eImageFormat != IF_Unknown) {
		return eImageFormat;
	} else if (!memcmp(header+4, "ftyp", 4)) {
		// Unspecified encoding (possibly AVIF or HEIF): mif1, mif2, msf1, miaf, 1pic
		return IF_AVIF;
	} else if (!memcmp(header, "II*\0", 4) || !memcmp(header, "MM\0*", 4)) {
		// Must be checked after file extension to avoid classifying RAW as TIFF
		// A few RAW image formats use TIFF as the container
		// ex: CR2 - http://lclevy.free.fr/cr2/#key_info
		// ex: DNG - https://www.adobe.com/creativecloud/file-types/image/raw/dng-file.html#dng
		return IF_TIFF;
	}
	return IF_Unknown;
}

static EImageFormat GetBitmapFormat(Gdiplus::Bitmap * pBitmap) {
	GUID guid{ 0 };
	pBitmap->GetRawFormat(&guid);
	if (guid == Gdiplus::ImageFormatBMP) {
		return IF_WindowsBMP;
	} else if (guid == Gdiplus::ImageFormatPNG) {
		return IF_PNG;
	} else if (guid == Gdiplus::ImageFormatGIF) {
		return IF_GIF;
	} else if (guid == Gdiplus::ImageFormatTIFF) {
		return IF_TIFF;
	} else if (guid == Gdiplus::ImageFormatJPEG || guid == Gdiplus::ImageFormatEXIF) {
		return IF_JPEG;
	} else {
		return IF_Unknown;
	}
}

static CJPEGImage* ConvertGDIPlusBitmapToJPEGImage(Gdiplus::Bitmap* pBitmap, int nFrameIndex, void* pEXIFData, 
	__int64 nJPEGHash, bool &isOutOfMemory, bool &isAnimatedGIF) {

	isOutOfMemory = false;
	isAnimatedGIF = false;
	Gdiplus::Status lastStatus = pBitmap->GetLastStatus();
	if (lastStatus != Gdiplus::Ok) {
		isOutOfMemory = lastStatus == Gdiplus::OutOfMemory;
		return NULL;
	}

	if (pBitmap->GetWidth() > MAX_IMAGE_DIMENSION || pBitmap->GetHeight() > MAX_IMAGE_DIMENSION)
	{
		return NULL;
	}
	if ((double)pBitmap->GetWidth() * pBitmap->GetHeight() > MAX_IMAGE_PIXELS)
	{
		isOutOfMemory = true;
		return NULL;
	}

	EImageFormat eImageFormat = GetBitmapFormat(pBitmap);

	// Handle multiframe images.
	// Note that only the first frame dimension is looked at, it is unclear if GDI+ anyway supports image formats with more dimensions
	UINT nDimensions = pBitmap->GetFrameDimensionsCount();
	GUID* pDimensionIDs = new GUID[nDimensions];
	pBitmap->GetFrameDimensionsList(pDimensionIDs, nDimensions);
	int nFrameCount = (nDimensions == 0) ? 1 : pBitmap->GetFrameCount(&pDimensionIDs[0]);
	nFrameIndex = max(0, min(nFrameCount - 1, nFrameIndex));
	int nFrameTimeMs = 100;
	if (nFrameCount > 1) {
		isAnimatedGIF = eImageFormat == IF_GIF;
		int nTagFrameDelaySize = pBitmap->GetPropertyItemSize(PropertyTagFrameDelay);
		if (nTagFrameDelaySize > 0) {
			PropertyItem* pPropertyItem = (PropertyItem*)new char[nTagFrameDelaySize];
			if (pBitmap->GetPropertyItem(PropertyTagFrameDelay, nTagFrameDelaySize, pPropertyItem) == Gdiplus::Ok) {
				nFrameTimeMs = ((long*)pPropertyItem->value)[nFrameIndex] * 10;
			}
			delete[] pPropertyItem;
		}
		GUID pageGuid = (eImageFormat == IF_TIFF) ? FrameDimensionPage : FrameDimensionTime;
		pBitmap->SelectActiveFrame(&pageGuid, nFrameIndex);
	}
	delete[] pDimensionIDs;

	// ImageBrowser 改动：原版在这里把带 alpha 的图先 DrawImage 到 32bppRGB 离屏位图上，
	// 底下填 ColorTransparency() 的实色，alpha 就此被烘掉——GIF 因此永远不透明。
	// 改成统一转到 PixelFormat32bppARGB 并逐行原样拷贝，alpha 交给外壳按 Tab（棋盘/底色）合成。
	// GIF 的透明来自调色板透明索引，GDI+ 未必在 PixelFormat 里报 Alpha 位，所以 GIF 一律走 ARGB。
	CJPEGImage* pJPEGImage = NULL;
	Gdiplus::PixelFormat pixelFormat = pBitmap->GetPixelFormat();
	bool bHasAlphaChannel = (pixelFormat & (PixelFormatAlpha | PixelFormatPAlpha)) != 0;
	bool bKeepAlpha = bHasAlphaChannel || eImageFormat == IF_GIF;
	int nWidth = (int)pBitmap->GetWidth();
	int nHeight = (int)pBitmap->GetHeight();

	Gdiplus::Bitmap* pArgbBitmap = NULL;
	Gdiplus::Bitmap* pBitmapToUse = pBitmap;
	if (bKeepAlpha) {
		pArgbBitmap = new Gdiplus::Bitmap(nWidth, nHeight, PixelFormat32bppARGB);
		Gdiplus::Graphics argbGraphics(pArgbBitmap);
		argbGraphics.DrawImage(pBitmap, 0, 0, nWidth, nHeight);
		if (argbGraphics.GetLastStatus() == Gdiplus::OutOfMemory) {
			isOutOfMemory = true;
			delete pArgbBitmap;
			return NULL;
		}
		pBitmapToUse = pArgbBitmap;
	}

	Gdiplus::Rect bmRect(0, 0, nWidth, nHeight);
	Gdiplus::BitmapData bmData;
	lastStatus = pBitmapToUse->LockBits(&bmRect, Gdiplus::ImageLockModeRead,
		bKeepAlpha ? PixelFormat32bppARGB : PixelFormat32bppRGB, &bmData);
	if (lastStatus == Gdiplus::Ok) {
		void* pDIB = NULL;
		if (bKeepAlpha && bmData.PixelFormat == PixelFormat32bppARGB) {
			uint32* pNewDIB = new(std::nothrow) uint32[(size_t)nWidth * nHeight];
			if (pNewDIB != NULL) {
				const uint8* pSrc = (const uint8*)bmData.Scan0;
				for (int j = 0; j < nHeight; j++) {
					memcpy(pNewDIB + (size_t)j * nWidth, pSrc + (size_t)j * bmData.Stride, (size_t)nWidth * 4);
				}
				pDIB = pNewDIB;
			}
		} else {
			assert(bmData.PixelFormat == PixelFormat32bppRGB);
			pDIB = CBasicProcessing::ConvertGdiplus32bppRGB(bmRect.Width, bmRect.Height, bmData.Stride, bmData.Scan0);
		}
		if (pDIB != NULL) {
			pJPEGImage = new CJPEGImage(nWidth, nHeight, pDIB, pEXIFData, 4, nJPEGHash, eImageFormat,
				eImageFormat == IF_GIF && nFrameCount > 1, nFrameIndex, nFrameCount, nFrameTimeMs);
		}
		pBitmapToUse->UnlockBits(&bmData);
	} else if (lastStatus == Gdiplus::ValueOverflow) {
		isOutOfMemory = true;
	}

	if (pArgbBitmap != NULL) {
		delete pArgbBitmap;
	}

	pBitmap->GetLastStatus(); // reset status

	return pJPEGImage;
}

/////////////////////////////////////////////////////////////////////////////////////////////
// ICO / CUR
//
// 一个 .ico 里有多种尺寸（256/128/64/48/32/16 …）。GDI+ 的 Bitmap(file) 对 ICO 自己挑一张，
// 不保证是最大的，所以这里自己读目录项：按像素面积选最大那张作为"原图"，缩放交给内核，
// 与 PNG 走同一条路径，窗口尺寸变化时不换图。alpha 原样保留（由外壳按 Tab 合成）。
/////////////////////////////////////////////////////////////////////////////////////////////////////////////

struct SIconDirEntry {
	BYTE bWidth, bHeight, bColorCount, bReserved;
	WORD wPlanes, wBitCount;
	DWORD dwBytesInRes, dwImageOffset;
};
static_assert(sizeof(SIconDirEntry) == 16, "ICONDIRENTRY must be 16 bytes");

static int IconEntryWidth(const SIconDirEntry& entry) { return entry.bWidth == 0 ? 256 : entry.bWidth; }
static int IconEntryHeight(const SIconDirEntry& entry) { return entry.bHeight == 0 ? 256 : entry.bHeight; }

// 大尺寸 ICO 条目是 PNG 载荷，直接借 GDI+ 解 PNG，再拷成 BGRA（alpha 保留）。
// 解码实现与 ORA 共用一份：CReaderORA::DecodePngMemory
static uint32* DecodeIconPng(const BYTE* pPng, size_t nPngSize, int& nWidth, int& nHeight) {
	return CReaderORA::DecodePngMemory(pPng, nPngSize, nWidth, nHeight);
}

// 传统条目是 DIB：BITMAPINFOHEADER + 调色板 + XOR 位图 + AND 掩码（高度是两段之和）
static uint32* DecodeIconDib(const BYTE* pData, size_t nDataSize, int& nWidth, int& nHeight) {
	if (nDataSize < sizeof(BITMAPINFOHEADER)) {
		return NULL;
	}
	const BITMAPINFOHEADER* pBih = (const BITMAPINFOHEADER*)pData;
	if (pBih->biCompression != BI_RGB) {
		return NULL;
	}
	int w = (int)pBih->biWidth;
	int nDibHeight = (int)pBih->biHeight;
	int h = (nDibHeight < 0 ? -nDibHeight : nDibHeight) / 2;
	int nBpp = (int)pBih->biBitCount;
	if (w <= 0 || h <= 0 || w > MAX_IMAGE_DIMENSION || h > MAX_IMAGE_DIMENSION) {
		return NULL;
	}
	if (nBpp != 1 && nBpp != 4 && nBpp != 8 && nBpp != 24 && nBpp != 32) {
		return NULL;
	}

	size_t nPaletteColors = (nBpp <= 8) ? (size_t)(pBih->biClrUsed ? pBih->biClrUsed : (1 << nBpp)) : 0;
	const BYTE* pPalette = pData + sizeof(BITMAPINFOHEADER);
	const BYTE* pXor = pPalette + nPaletteColors * 4;
	int nXorStride = ((w * nBpp + 31) / 32) * 4;
	int nAndStride = ((w + 31) / 32) * 4;
	const BYTE* pAnd = pXor + (size_t)nXorStride * h;
	size_t nNeeded = (size_t)(pAnd - pData) + (size_t)nAndStride * h;
	if (nNeeded > nDataSize) {
		return NULL;
	}

	uint32* pPixels = new(std::nothrow) uint32[(size_t)w * h];
	if (pPixels == NULL) {
		return NULL;
	}

	bool bAnyAlpha = false;
	for (int y = 0; y < h; y++) {
		const BYTE* pSrcRow = pXor + (size_t)(h - 1 - y) * nXorStride;
		uint32* pDstRow = pPixels + (size_t)y * w;
		for (int x = 0; x < w; x++) {
			uint32 pixel;
			if (nBpp == 32) {
				memcpy(&pixel, pSrcRow + (size_t)x * 4, 4);
				if ((pixel & 0xFF000000u) != 0) {
					bAnyAlpha = true;
				}
			} else if (nBpp == 24) {
				pixel = 0xFF000000u | ((uint32)pSrcRow[x * 3 + 2] << 16) | ((uint32)pSrcRow[x * 3 + 1] << 8) | pSrcRow[x * 3];
			} else {
				int nIndex;
				if (nBpp == 8) {
					nIndex = pSrcRow[x];
				} else if (nBpp == 4) {
					nIndex = (pSrcRow[x / 2] >> ((x & 1) ? 0 : 4)) & 0x0F;
				} else {
					nIndex = (pSrcRow[x / 8] >> (7 - (x & 7))) & 0x01;
				}
				const BYTE* pEntry = pPalette + (size_t)nIndex * 4;
				pixel = 0xFF000000u | ((uint32)pEntry[2] << 16) | ((uint32)pEntry[1] << 8) | pEntry[0];
			}
			pDstRow[x] = pixel;
		}
	}

	// 透明来自 AND 掩码（1 = 透明）。32bpp 自带 alpha 时按 alpha 走，全 0 才退回掩码
	if (!(nBpp == 32 && bAnyAlpha)) {
		for (int y = 0; y < h; y++) {
			const BYTE* pMaskRow = pAnd + (size_t)(h - 1 - y) * nAndStride;
			uint32* pDstRow = pPixels + (size_t)y * w;
			for (int x = 0; x < w; x++) {
				bool bTransparent = ((pMaskRow[x / 8] >> (7 - (x & 7))) & 0x01) != 0;
				pDstRow[x] = bTransparent ? (pDstRow[x] & 0x00FFFFFFu) : (pDstRow[x] | 0xFF000000u);
			}
		}
	}

	nWidth = w;
	nHeight = h;
	return pPixels;
}

/////////////////////////////////////////////////////////////////////////////////////////////
// Public
/////////////////////////////////////////////////////////////////////////////////////////////

CImageLoadThread::CImageLoadThread(void) : CWorkThread(true) {
	m_pLastBitmap = NULL;
}

CImageLoadThread::~CImageLoadThread(void) {
	DeleteCachedGDIBitmap();
	DeleteCachedWebpDecoder();
	DeleteCachedPngDecoder();
	DeleteCachedJxlDecoder();
	DeleteCachedAvifDecoder();
}

int CImageLoadThread::AsyncLoad(LPCTSTR strFileName, int nFrameIndex, const CProcessParams & processParams, HWND targetWnd, HANDLE eventFinished) {
	CRequest* pRequest = new CRequest(strFileName, nFrameIndex, targetWnd, processParams, eventFinished);

	ProcessAsync(pRequest);

	return pRequest->RequestHandle;
}

CImageData CImageLoadThread::GetLoadedImage(int nHandle) {
	Helpers::CAutoCriticalSection criticalSection(m_csList);
	CJPEGImage* imageFound = NULL;
	bool bFailedMemory = false;
	bool bFailedException = false;
	std::list<CRequestBase*>::iterator iter;
	for (iter = m_requestList.begin( ); iter != m_requestList.end( ); iter++ ) {
		CRequest* pRequest = (CRequest*)(*iter);
		if (pRequest->Processed && pRequest->Deleted == false && pRequest->RequestHandle == nHandle) {
			imageFound = pRequest->Image;
			bFailedMemory = pRequest->OutOfMemory;
			bFailedException = pRequest->ExceptionError;
			// only mark as deleted
			pRequest->Deleted = true;
			break;
		}
	}
	return CImageData(imageFound, bFailedMemory, bFailedException);
}

void CImageLoadThread::ReleaseFile(LPCTSTR strFileName) {
	CReleaseFileRequest* pRequest = new CReleaseFileRequest(strFileName);
	ProcessAndWait(pRequest);
}

/////////////////////////////////////////////////////////////////////////////////////////////
// Protected
/////////////////////////////////////////////////////////////////////////////////////////////

// Called on the processing thread
void CImageLoadThread::ProcessRequest(CRequestBase& request) {
	if (request.Type == CReleaseFileRequest::ReleaseFileRequest) {
		CReleaseFileRequest& rq = (CReleaseFileRequest&)request;
		if (rq.FileName == m_sLastFileName) {
			DeleteCachedGDIBitmap();
		}
		if (rq.FileName == m_sLastWebpFileName) {
			DeleteCachedWebpDecoder();
		}
		if (rq.FileName == m_sLastPngFileName) {
			DeleteCachedPngDecoder();
		}
		if (rq.FileName == m_sLastJxlFileName) {
			DeleteCachedJxlDecoder();
		}
		if (rq.FileName == m_sLastAvifFileName) {
			DeleteCachedAvifDecoder();
		}
		return;
	}

	CRequest& rq = (CRequest&)request;
	double dStartTime = Helpers::GetExactTickCount(); 
	// Get image format and read the image
	switch (GetImageFormat(rq.FileName)) {
		case IF_JPEG :
			DeleteCachedGDIBitmap();
			DeleteCachedWebpDecoder();
			DeleteCachedPngDecoder();
			DeleteCachedJxlDecoder();
			DeleteCachedAvifDecoder();
			ProcessReadJPEGRequest(&rq);
			break;
		case IF_WindowsBMP :
			DeleteCachedGDIBitmap();
			DeleteCachedWebpDecoder();
			DeleteCachedPngDecoder();
			DeleteCachedJxlDecoder();
			DeleteCachedAvifDecoder();
			ProcessReadBMPRequest(&rq);
			break;
		case IF_TGA :
			DeleteCachedGDIBitmap();
			DeleteCachedWebpDecoder();
			DeleteCachedPngDecoder();
			DeleteCachedJxlDecoder();
			DeleteCachedAvifDecoder();
			ProcessReadTGARequest(&rq);
			break;
		case IF_ICO:
			DeleteCachedGDIBitmap();
			DeleteCachedWebpDecoder();
			DeleteCachedPngDecoder();
			DeleteCachedJxlDecoder();
			DeleteCachedAvifDecoder();
			ProcessReadICORequest(&rq);
			break;
		case IF_ORA:
			DeleteCachedGDIBitmap();
			DeleteCachedWebpDecoder();
			DeleteCachedPngDecoder();
			DeleteCachedJxlDecoder();
			DeleteCachedAvifDecoder();
			ProcessReadORARequest(&rq);
			break;
		case IF_WEBP:
			DeleteCachedGDIBitmap();
			DeleteCachedPngDecoder();
			DeleteCachedJxlDecoder();
			DeleteCachedAvifDecoder();
			ProcessReadWEBPRequest(&rq);
			break;
		case IF_PNG:
			DeleteCachedGDIBitmap();
			DeleteCachedWebpDecoder();
			DeleteCachedJxlDecoder();
			DeleteCachedAvifDecoder();
			ProcessReadPNGRequest(&rq);
			break;
#ifndef WINXP
		case IF_JXL:
			DeleteCachedGDIBitmap();
			DeleteCachedWebpDecoder();
			DeleteCachedPngDecoder();
			DeleteCachedAvifDecoder();
			ProcessReadJXLRequest(&rq);
			break;
		case IF_AVIF:
			DeleteCachedGDIBitmap();
			DeleteCachedWebpDecoder();
			DeleteCachedPngDecoder();
			DeleteCachedJxlDecoder();
			ProcessReadAVIFRequest(&rq);
			break;
		case IF_HEIF:
			DeleteCachedGDIBitmap();
			DeleteCachedWebpDecoder();
			DeleteCachedPngDecoder();
			DeleteCachedJxlDecoder();
			DeleteCachedAvifDecoder();
			ProcessReadHEIFRequest(&rq);
			break;
		case IF_PSD:
			DeleteCachedGDIBitmap();
			DeleteCachedWebpDecoder();
			DeleteCachedPngDecoder();
			DeleteCachedJxlDecoder();
			DeleteCachedAvifDecoder();
			ProcessReadPSDRequest(&rq);
			break;
		case IF_CameraRAW:
			DeleteCachedGDIBitmap();
			DeleteCachedWebpDecoder();
			DeleteCachedPngDecoder();
			DeleteCachedJxlDecoder();
			DeleteCachedAvifDecoder();
			ProcessReadRAWRequest(&rq);
			break;
#endif
		case IF_QOI:
			DeleteCachedGDIBitmap();
			DeleteCachedWebpDecoder();
			DeleteCachedPngDecoder();
			DeleteCachedJxlDecoder();
			DeleteCachedAvifDecoder();
			ProcessReadQOIRequest(&rq);
			break;
		case IF_WIC:
			DeleteCachedGDIBitmap();
			DeleteCachedWebpDecoder();
			DeleteCachedPngDecoder();
			DeleteCachedJxlDecoder();
			DeleteCachedAvifDecoder();
			ProcessReadWICRequest(&rq);
			break;
		default:
			// try with GDI+
			DeleteCachedWebpDecoder();
			DeleteCachedPngDecoder();
			DeleteCachedJxlDecoder();
			DeleteCachedAvifDecoder();
			ProcessReadGDIPlusRequest(&rq);
			break;
	}
	// then process the image if read was successful
	if (rq.Image != NULL) {
		rq.Image->SetLoadTickCount(Helpers::GetExactTickCount() - dStartTime); 
		if (!ProcessImageAfterLoad(&rq)) {
			delete rq.Image;
			rq.Image = NULL;
			rq.OutOfMemory = true;
		}
	}
}

// Called on the processing thread
void CImageLoadThread::AfterFinishProcess(CRequestBase& request) {
	if (request.Type == CReleaseFileRequest::ReleaseFileRequest) {
		return;
	}

	CRequest& rq = (CRequest&)request;
	if (rq.TargetWnd != NULL) {
		// post message to window that request has been processed
		::PostMessage(rq.TargetWnd, WM_IMAGE_LOAD_COMPLETED, 0, rq.RequestHandle);
	}
}

/////////////////////////////////////////////////////////////////////////////////////////////
// Private
/////////////////////////////////////////////////////////////////////////////////////////////

static void LimitOffsets(CPoint& offsets, CSize clippingSize, const CSize & imageSize) {
	int nMaxOffsetX = (imageSize.cx - clippingSize.cx)/2;
	nMaxOffsetX = max(0, nMaxOffsetX);
	int nMaxOffsetY = (imageSize.cy - clippingSize.cy)/2;
	nMaxOffsetY = max(0, nMaxOffsetY);
	offsets.x = max(-nMaxOffsetX, min(+nMaxOffsetX, offsets.x));
	offsets.y = max(-nMaxOffsetY, min(+nMaxOffsetY, offsets.y));
}

void CImageLoadThread::DeleteCachedGDIBitmap() {
	if (m_pLastBitmap != NULL) {
		delete m_pLastBitmap;
	}
	m_pLastBitmap = NULL;
	m_sLastFileName.Empty();
}

void CImageLoadThread::DeleteCachedWebpDecoder() {
	WebpReaderWriter::DeleteCache();
	m_sLastWebpFileName.Empty();
}

void CImageLoadThread::DeleteCachedPngDecoder() {
#ifndef WINXP
	PngReader::DeleteCache();
	m_sLastPngFileName.Empty();
#endif
}

void CImageLoadThread::DeleteCachedJxlDecoder() {
#ifndef WINXP
	JxlReader::DeleteCache();
	m_sLastJxlFileName.Empty();
#endif
}

void CImageLoadThread::DeleteCachedAvifDecoder() {
#ifndef WINXP
	// prevent crashing when libavif/dav1d fail or missing
	UINT nPrevErrorMode = SetErrorMode(SEM_FAILCRITICALERRORS);
	try {
		AvifReader::DeleteCache();
	} catch (...) {}
	SetErrorMode(nPrevErrorMode);
	m_sLastAvifFileName.Empty();
#endif
}

void CImageLoadThread::ProcessReadJPEGRequest(CRequest * request) {
	HANDLE hFile = ::CreateFile(request->FileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		return;
	}

	HGLOBAL hFileBuffer = NULL;
	void* pBuffer = NULL;
	try {
		// Don't read too huge files
		long long nFileSize = Helpers::GetFileSize(hFile);
		if (nFileSize > MAX_JPEG_FILE_SIZE) {
			request->OutOfMemory = true;
			::CloseHandle(hFile);
			return;
		}
		hFileBuffer  = ::GlobalAlloc(GMEM_MOVEABLE, nFileSize);
		pBuffer = (hFileBuffer == NULL) ? NULL : ::GlobalLock(hFileBuffer);
		if (pBuffer == NULL) {
			if (hFileBuffer) ::GlobalFree(hFileBuffer);
			request->OutOfMemory = true;
			::CloseHandle(hFile);
			return;
		}
		unsigned int nNumBytesRead;
		if (::ReadFile(hFile, pBuffer, nFileSize, (LPDWORD) &nNumBytesRead, NULL) && nNumBytesRead == nFileSize) {
			bool bUseGDIPlus = CSettingsProvider::This().ForceGDIPlus() || CSettingsProvider::This().UseEmbeddedColorProfiles();
			if (bUseGDIPlus) {
				IStream* pStream = NULL;
				if (::CreateStreamOnHGlobal(hFileBuffer, FALSE, &pStream) == S_OK) {
					Gdiplus::Bitmap* pBitmap = Gdiplus::Bitmap::FromStream(pStream, CSettingsProvider::This().UseEmbeddedColorProfiles());
					bool isOutOfMemory, isAnimatedGIF;
					request->Image = ConvertGDIPlusBitmapToJPEGImage(pBitmap, 0, Helpers::FindEXIFBlock(pBuffer, nFileSize),
						Helpers::CalculateJPEGFileHash(pBuffer, nFileSize), isOutOfMemory, isAnimatedGIF);
					request->OutOfMemory = request->Image == NULL && isOutOfMemory;
					if (request->Image != NULL) {
						request->Image->SetJPEGComment(Helpers::GetJPEGComment(pBuffer, nFileSize));
					}
					pStream->Release();
					delete pBitmap;
				} else {
					request->OutOfMemory = true;
				}
			}
			if (!bUseGDIPlus || request->OutOfMemory) {
				int nWidth, nHeight, nBPP;
				TJSAMP eChromoSubSampling;
				bool bOutOfMemory;
				// int nTicks = ::GetTickCount();

				void* pPixelData = TurboJpeg::ReadImage(nWidth, nHeight, nBPP, eChromoSubSampling, bOutOfMemory, pBuffer, nFileSize);
				
				/*
				TCHAR buffer[20];
				_stprintf_s(buffer, 20, _T("%d"), ::GetTickCount() - nTicks);
				::MessageBox(NULL, CString(_T("Elapsed ticks: ")) + buffer, _T("Time"), MB_OK);
				*/

				// Color and b/w JPEG is supported
				if (pPixelData != NULL && (nBPP == 3 || nBPP == 1)) {
					request->Image = new CJPEGImage(nWidth, nHeight, pPixelData, 
						Helpers::FindEXIFBlock(pBuffer, nFileSize), nBPP, 
						Helpers::CalculateJPEGFileHash(pBuffer, nFileSize), IF_JPEG, false, 0, 1, 0);
					request->Image->SetJPEGComment(Helpers::GetJPEGComment(pBuffer, nFileSize));
					request->Image->SetJPEGChromoSampling(eChromoSubSampling);
				} else if (bOutOfMemory) {
					request->OutOfMemory = true;
				} else {
					// failed, try GDI+
					delete[] pPixelData;
					ProcessReadGDIPlusRequest(request);
				}
			}
		}
	} catch (...) {
		delete request->Image;
		request->Image = NULL;
		request->ExceptionError = true;
	}
	::CloseHandle(hFile);
	if (pBuffer) ::GlobalUnlock(hFileBuffer);
	if (hFileBuffer) ::GlobalFree(hFileBuffer);
}


void CImageLoadThread::ProcessReadBMPRequest(CRequest * request) {
	bool bOutOfMemory;
	request->Image = CReaderBMP::ReadBmpImage(request->FileName, bOutOfMemory);
	if (bOutOfMemory) {
		request->OutOfMemory = true;
	} else if (request->Image == NULL) {
		// probably one of the bitmap formats that can not be read directly, try with GDI+
		ProcessReadGDIPlusRequest(request);
	}
}

void CImageLoadThread::ProcessReadTGARequest(CRequest * request) {
	bool bOutOfMemory;
	request->Image = CReaderTGA::ReadTgaImage(request->FileName, CSettingsProvider::This().ColorTransparency(), bOutOfMemory);
	if (bOutOfMemory) {
		request->OutOfMemory = true;
	}
}

void CImageLoadThread::ProcessReadWEBPRequest(CRequest * request) {
	bool bUseCachedDecoder = false;
	const wchar_t* sFileName;
	sFileName = (const wchar_t*)request->FileName;
	if (sFileName != m_sLastWebpFileName) {
		DeleteCachedWebpDecoder();
	}
	else {
		bUseCachedDecoder = true;
	}

	HANDLE hFile;
	if (!bUseCachedDecoder) {
		hFile = ::CreateFile(request->FileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
		if (hFile == INVALID_HANDLE_VALUE) {
			return;
		}
	}
	char* pBuffer = NULL;
	try {
		long long nFileSize = 0;
		unsigned int nNumBytesRead;
		if (!bUseCachedDecoder) {
			// Don't read too huge files
			nFileSize = Helpers::GetFileSize(hFile);
			if (nFileSize > MAX_WEBP_FILE_SIZE) {
				request->OutOfMemory = true;
				::CloseHandle(hFile);
				return;
			}

			pBuffer = new(std::nothrow) char[nFileSize];
			if (pBuffer == NULL) {
				request->OutOfMemory = true;
				::CloseHandle(hFile);
				return;
			}
		}
		if (bUseCachedDecoder || (::ReadFile(hFile, pBuffer, nFileSize, (LPDWORD)&nNumBytesRead, NULL) && nNumBytesRead == nFileSize)) {
			int nWidth, nHeight;
			bool bHasAnimation = bUseCachedDecoder;
			int nFrameCount = 1;
			int nFrameTimeMs = 0;
			int nBPP;
			void* pEXIFData;
			uint8* pPixelData = (uint8*)WebpReaderWriter::ReadImage(nWidth, nHeight, nBPP, bHasAnimation, nFrameCount, nFrameTimeMs, pEXIFData, request->OutOfMemory, pBuffer, nFileSize);
			if (pPixelData && nBPP == 4) {
				// Multiply alpha value into each AABBGGRR pixel
				uint32* pImage32 = (uint32*)pPixelData;
				for (int i = 0; i < nWidth * nHeight; i++)
					*pImage32++ = Helpers::AlphaBlendBackground(*pImage32, CSettingsProvider::This().ColorTransparency());

				if (bHasAnimation) {
					m_sLastWebpFileName = sFileName;
				}
				request->Image = new CJPEGImage(nWidth, nHeight, pPixelData, pEXIFData, nBPP, 0, IF_WEBP, bHasAnimation, request->FrameIndex, nFrameCount, nFrameTimeMs);
				free(pEXIFData);
			}
			else {
				delete[] pPixelData;
				DeleteCachedWebpDecoder();
			}
		}
	} catch (...) {
		delete request->Image;
		request->Image = NULL;
	}
	if (!bUseCachedDecoder) {
		::CloseHandle(hFile);
		delete[] pBuffer;
	}
}

#ifndef WINXP
void CImageLoadThread::ProcessReadPNGRequest(CRequest* request) {
	bool bUseCachedDecoder = false;
	const wchar_t* sFileName;
	sFileName = (const wchar_t*)request->FileName;
	if (sFileName != m_sLastPngFileName) {
		DeleteCachedPngDecoder();
	}
	else {
		bUseCachedDecoder = true;
	}

	HANDLE hFile;
	if (!bUseCachedDecoder) {
		hFile = ::CreateFile(request->FileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
		if (hFile == INVALID_HANDLE_VALUE) {
			return;
		}
	}
	HGLOBAL hFileBuffer = NULL;
	void* pBuffer = NULL;
	try {
		long long nFileSize;
		unsigned int nNumBytesRead;
		if (!bUseCachedDecoder) {
			// Don't read too huge files
			nFileSize = Helpers::GetFileSize(hFile);
			if (nFileSize > MAX_PNG_FILE_SIZE) {
				request->OutOfMemory = true;
				::CloseHandle(hFile);
				return;
			}
			hFileBuffer = ::GlobalAlloc(GMEM_MOVEABLE, nFileSize);
			pBuffer = (hFileBuffer == NULL) ? NULL : ::GlobalLock(hFileBuffer);
			if (pBuffer == NULL) {
				if (hFileBuffer) ::GlobalFree(hFileBuffer);
				request->OutOfMemory = true;
				::CloseHandle(hFile);
				return;
			}
		} else {
			nFileSize = 0; // to avoid compiler warnings, not used
		}
		if (bUseCachedDecoder || (::ReadFile(hFile, pBuffer, nFileSize, (LPDWORD)&nNumBytesRead, NULL) && nNumBytesRead == nFileSize)) {
			int nWidth, nHeight, nBPP, nFrameCount, nFrameTimeMs;
			bool bHasAnimation;
			uint8* pPixelData = NULL;
			void* pEXIFData = NULL;

#ifndef WINXP
			// If UseEmbeddedColorProfiles is true and the image isn't animated, we should use GDI+ for better color management
			bool bUseGDIPlus = CSettingsProvider::This().ForceGDIPlus() || CSettingsProvider::This().UseEmbeddedColorProfiles();
			if (bUseCachedDecoder || !bUseGDIPlus || PngReader::MustUseLibpng(pBuffer, nFileSize))
				pPixelData = (uint8*)PngReader::ReadImage(nWidth, nHeight, nBPP, bHasAnimation, nFrameCount, nFrameTimeMs, pEXIFData, request->OutOfMemory, pBuffer, nFileSize);
#endif

			if (pPixelData != NULL) {
				if (bHasAnimation)
					m_sLastPngFileName = sFileName;
				// Multiply alpha value into each AABBGGRR pixel
				uint32* pImage32 = (uint32*)pPixelData;
				for (int i = 0; i < nWidth * nHeight; i++)
					*pImage32++ = Helpers::AlphaBlendBackground(*pImage32, CSettingsProvider::This().ColorTransparency());

				request->Image = new CJPEGImage(nWidth, nHeight, pPixelData, pEXIFData, 4, 0, IF_PNG, bHasAnimation, request->FrameIndex, nFrameCount, nFrameTimeMs);
			} else {
				DeleteCachedPngDecoder();
				
				IStream* pStream = NULL;
				if (::CreateStreamOnHGlobal(hFileBuffer, FALSE, &pStream) == S_OK) {
					Gdiplus::Bitmap* pBitmap = Gdiplus::Bitmap::FromStream(pStream, CSettingsProvider::This().UseEmbeddedColorProfiles());
					bool isOutOfMemory, isAnimatedGIF;
					pEXIFData = PngReader::GetEXIFBlock(pBuffer, nFileSize);
					request->Image = ConvertGDIPlusBitmapToJPEGImage(pBitmap, 0, pEXIFData, 0, isOutOfMemory, isAnimatedGIF);
					request->OutOfMemory = request->Image == NULL && isOutOfMemory;
					pStream->Release();
					delete pBitmap;
				} else {
					request->OutOfMemory = true;
				}
			}
			free(pEXIFData);
		}
	}
	catch (...) {
		delete request->Image;
		request->Image = NULL;
		request->ExceptionError = true;
	}
	if (!bUseCachedDecoder) {
		::CloseHandle(hFile);
		if (pBuffer) ::GlobalUnlock(hFileBuffer);
		if (hFileBuffer) ::GlobalFree(hFileBuffer);
	}
}
#endif

#ifndef WINXP
void CImageLoadThread::ProcessReadJXLRequest(CRequest* request) {
	bool bUseCachedDecoder = false;
	const wchar_t* sFileName;
	sFileName = (const wchar_t*)request->FileName;
	if (sFileName != m_sLastJxlFileName) {
		DeleteCachedJxlDecoder();
	} else {
		bUseCachedDecoder = true;
	}

	HANDLE hFile;
	if (!bUseCachedDecoder) {
		hFile = ::CreateFile(request->FileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
		if (hFile == INVALID_HANDLE_VALUE) {
			return;
		}
	}
	char* pBuffer = NULL;
	UINT nPrevErrorMode = SetErrorMode(SEM_FAILCRITICALERRORS);
	try {
		long long nFileSize = 0;
		unsigned int nNumBytesRead;
		if (!bUseCachedDecoder) {
			// Don't read too huge files
			nFileSize = Helpers::GetFileSize(hFile);
			if (nFileSize > MAX_JXL_FILE_SIZE) {
				request->OutOfMemory = true;
				::CloseHandle(hFile);
				return;
			}

			pBuffer = new(std::nothrow) char[nFileSize];
			if (pBuffer == NULL) {
				request->OutOfMemory = true;
				::CloseHandle(hFile);
				return;
			}
		}
		if (bUseCachedDecoder || (::ReadFile(hFile, pBuffer, nFileSize, (LPDWORD)&nNumBytesRead, NULL) && nNumBytesRead == nFileSize)) {
			int nWidth, nHeight, nBPP, nFrameCount, nFrameTimeMs;
			bool bHasAnimation;
			void* pEXIFData;
			uint8* pPixelData = (uint8*)JxlReader::ReadImage(nWidth, nHeight, nBPP, bHasAnimation, nFrameCount, nFrameTimeMs, pEXIFData, request->OutOfMemory, pBuffer, nFileSize);
			if (pPixelData != NULL) {
				if (bHasAnimation)
					m_sLastJxlFileName = sFileName;
				// Multiply alpha value into each AABBGGRR pixel
				uint32* pImage32 = (uint32*)pPixelData;
				for (int i = 0; i < nWidth * nHeight; i++)
					*pImage32++ = Helpers::AlphaBlendBackground(*pImage32, CSettingsProvider::This().ColorTransparency());

				request->Image = new CJPEGImage(nWidth, nHeight, pPixelData, pEXIFData, 4, 0, IF_JXL, bHasAnimation, request->FrameIndex, nFrameCount, nFrameTimeMs);
				free(pEXIFData);
			} else {
				DeleteCachedJxlDecoder();
			}
		}
	}
	catch (...) {
		delete request->Image;
		request->Image = NULL;
		request->ExceptionError = true;
	}
	SetErrorMode(nPrevErrorMode);
	if (!bUseCachedDecoder) {
		::CloseHandle(hFile);
		// delete[] pBuffer;
	}
}
#endif

#ifndef WINXP
void CImageLoadThread::ProcessReadAVIFRequest(CRequest* request) {
	bool bSuccess = false;
	bool bUseCachedDecoder = false;
	const wchar_t* sFileName;
	sFileName = (const wchar_t*)request->FileName;
	if (sFileName != m_sLastAvifFileName) {
		DeleteCachedAvifDecoder();
	} else {
		bUseCachedDecoder = true;
	}

	HANDLE hFile;
	if (!bUseCachedDecoder) {
		hFile = ::CreateFile(request->FileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
		if (hFile == INVALID_HANDLE_VALUE) {
			return;
		}
	}
	char* pBuffer = NULL;
	UINT nPrevErrorMode = SetErrorMode(SEM_FAILCRITICALERRORS);
	try {
		long long nFileSize = 0;
		unsigned int nNumBytesRead;
		if (!bUseCachedDecoder) {
			// Don't read too huge files
			nFileSize = Helpers::GetFileSize(hFile);
			if (nFileSize > MAX_HEIF_FILE_SIZE) {
				request->OutOfMemory = true;
				::CloseHandle(hFile);
				return;
			}

			pBuffer = new(std::nothrow) char[nFileSize];
			if (pBuffer == NULL) {
				request->OutOfMemory = true;
				::CloseHandle(hFile);
				return;
			}
		}
		if (bUseCachedDecoder || (::ReadFile(hFile, pBuffer, nFileSize, (LPDWORD)&nNumBytesRead, NULL) && nNumBytesRead == nFileSize)) {
			int nWidth, nHeight, nBPP, nFrameCount, nFrameTimeMs;
			bool bHasAnimation;
			void* pEXIFData;
			uint8* pPixelData = (uint8*)AvifReader::ReadImage(nWidth, nHeight, nBPP, bHasAnimation, request->FrameIndex, 
				nFrameCount, nFrameTimeMs, pEXIFData, request->OutOfMemory, pBuffer, nFileSize);
			if (pPixelData != NULL) {
				if (bHasAnimation)
					m_sLastAvifFileName = sFileName;
				// Multiply alpha value into each AABBGGRR pixel
				uint32* pImage32 = (uint32*)pPixelData;
				for (int i = 0; i < nWidth * nHeight; i++)
					*pImage32++ = Helpers::AlphaBlendBackground(*pImage32, CSettingsProvider::This().ColorTransparency());

				request->Image = new CJPEGImage(nWidth, nHeight, pPixelData, pEXIFData, 4, 0, IF_AVIF, bHasAnimation, request->FrameIndex, nFrameCount, nFrameTimeMs);
				free(pEXIFData);
				bSuccess = true;
			} else {
				DeleteCachedAvifDecoder();
			}
		}
	}
	catch (...) {
		delete request->Image;
		request->Image = NULL;
		request->ExceptionError = true;
	}
	SetErrorMode(nPrevErrorMode);
	if (!bUseCachedDecoder) {
		::CloseHandle(hFile);
		delete[] pBuffer;
	}
	if (!bSuccess)
		return ProcessReadHEIFRequest(request);
}
#endif

#ifndef WINXP
void CImageLoadThread::ProcessReadHEIFRequest(CRequest* request) {
	HANDLE hFile;
	hFile = ::CreateFile(request->FileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		return;
	}
	char* pBuffer = NULL;
	UINT nPrevErrorMode = SetErrorMode(SEM_FAILCRITICALERRORS);
	try {
		unsigned int nNumBytesRead;
		// Don't read too huge files
		long long nFileSize = Helpers::GetFileSize(hFile);
		if (nFileSize > MAX_HEIF_FILE_SIZE) {
			request->OutOfMemory = true;
			::CloseHandle(hFile);
			return;
		}

		pBuffer = new(std::nothrow) char[nFileSize];
		if (pBuffer == NULL) {
			request->OutOfMemory = true;
			::CloseHandle(hFile);
			return;
		}
		if (::ReadFile(hFile, pBuffer, nFileSize, (LPDWORD)&nNumBytesRead, NULL) && nNumBytesRead == nFileSize) {
			int nWidth, nHeight, nBPP, nFrameCount, nFrameTimeMs;
			nFrameCount = 1;
			nFrameTimeMs = 0;
			void* pEXIFData;
			uint8* pPixelData = (uint8*)HeifReader::ReadImage(nWidth, nHeight, nBPP, nFrameCount, pEXIFData, request->OutOfMemory, request->FrameIndex, pBuffer, nFileSize);
			if (pPixelData != NULL) {
				// Multiply alpha value into each AABBGGRR pixel
				uint32* pImage32 = (uint32*)pPixelData;
				for (int i = 0; i < nWidth * nHeight; i++)
					*pImage32++ = Helpers::AlphaBlendBackground(*pImage32, CSettingsProvider::This().ColorTransparency());

				request->Image = new CJPEGImage(nWidth, nHeight, pPixelData, pEXIFData, nBPP, 0, IF_HEIF, false, request->FrameIndex, nFrameCount, nFrameTimeMs);
				free(pEXIFData);
			}
		}
	} catch(heif::Error he) {
		// invalid image
		delete request->Image;
		request->Image = NULL;
	} catch (...) {
		delete request->Image;
		request->Image = NULL;
		request->ExceptionError = true;
	}
	SetErrorMode(nPrevErrorMode);
	::CloseHandle(hFile);
	delete[] pBuffer;
}

void CImageLoadThread::ProcessReadPSDRequest(CRequest* request) {
	request->Image = PsdReader::ReadImage(request->FileName, request->OutOfMemory);
	if (request->Image == NULL && !request->OutOfMemory) {
		request->Image = PsdReader::ReadThumb(request->FileName, request->OutOfMemory);
	}
}

#endif

void CImageLoadThread::ProcessReadQOIRequest(CRequest* request) {
	HANDLE hFile;
	hFile = ::CreateFile(request->FileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		return;
	}
	char* pBuffer = NULL;
	try {
		unsigned int nNumBytesRead;
		// Don't read too huge files
		long long nFileSize = Helpers::GetFileSize(hFile);
		if (nFileSize > MAX_PNG_FILE_SIZE) {
			request->OutOfMemory = true;
			::CloseHandle(hFile);
			return;
		}

		pBuffer = new(std::nothrow) char[nFileSize];
		if (pBuffer == NULL) {
			request->OutOfMemory = true;
			::CloseHandle(hFile);
			return;
		}
		if (::ReadFile(hFile, pBuffer, nFileSize, (LPDWORD)&nNumBytesRead, NULL) && nNumBytesRead == nFileSize) {
			int nWidth, nHeight, nBPP;
			void* pPixelData = QoiReaderWriter::ReadImage(nWidth, nHeight, nBPP, request->OutOfMemory, pBuffer, nFileSize);
			if (pPixelData != NULL) {
				if (nBPP == 4) {
					// Multiply alpha value into each AABBGGRR pixel
					uint32* pImage32 = (uint32*)pPixelData;
					for (int i = 0; i < nWidth * nHeight; i++)
						*pImage32++ = Helpers::AlphaBlendBackground(*pImage32, CSettingsProvider::This().ColorTransparency());
				}
				request->Image = new CJPEGImage(nWidth, nHeight, pPixelData, NULL, nBPP, 0, IF_QOI, false, 0, 1, 0);
			}
		}
	} catch (...) {
		delete request->Image;
		request->Image = NULL;
		request->ExceptionError = true;
	}
	::CloseHandle(hFile);
	delete[] pBuffer;
}

void CImageLoadThread::ProcessReadRAWRequest(CRequest * request) {
	bool bOutOfMemory = false;
	try {
		int fullsize = CSettingsProvider::This().DisplayFullSizeRAW();

#ifndef WINXP
		// Try with libraw
		UINT nPrevErrorMode = SetErrorMode(SEM_FAILCRITICALERRORS);
		try {
			if (fullsize == 2 || fullsize == 3) {
				request->Image = RawReader::ReadImage(request->FileName, bOutOfMemory, fullsize == 2);
			}
			if (request->Image == NULL && fullsize == 2) {
				request->Image = CReaderRAW::ReadRawImage(request->FileName, bOutOfMemory);
			}
			if (request->Image == NULL) {
				request->Image = RawReader::ReadImage(request->FileName, bOutOfMemory, fullsize == 0 || fullsize == 3);
			}
		} catch (...) {
			// libraw.dll not found or VC++ Runtime not installed
		}
		SetErrorMode(nPrevErrorMode);
#else
		fullsize = fullsize == 1;
#endif

		// Try with dcraw_mod
		if (request->Image == NULL && fullsize != 1 && fullsize != 2) {
			request->Image = CReaderRAW::ReadRawImage(request->FileName, bOutOfMemory);
		}
	} catch (...) {
		delete request->Image;
		request->Image = NULL;
	}
	if (bOutOfMemory) {
		request->OutOfMemory = true;
	}
}

void CImageLoadThread::ProcessReadICORequest(CRequest * request) {
	std::vector<BYTE> data;
	FILE* fptr = _tfopen(request->FileName, _T("rb"));
	if (fptr == NULL) {
		return;
	}
	fseek(fptr, 0, SEEK_END);
	long nFileSize = ftell(fptr);
	fseek(fptr, 0, SEEK_SET);
	if (nFileSize > 0 && nFileSize <= 64L * 1024 * 1024) {
		data.resize((size_t)nFileSize);
		if (fread(data.data(), 1, (size_t)nFileSize, fptr) != (size_t)nFileSize) {
			data.clear();
		}
	}
	fclose(fptr);

	if (data.size() < 6 || (data[2] != 1 && data[2] != 2) || data[3] != 0) { // type: 1=ICO, 2=CUR
		return;
	}
	int nCount = data[4] | (data[5] << 8);
	if (nCount <= 0 || data.size() < (size_t)(6 + 16 * nCount)) {
		return;
	}

	// 选像素面积最大的一张；面积相同取位深更高的
	int nBest = -1;
	for (int i = 0; i < nCount; i++) {
		const SIconDirEntry& entry = *(const SIconDirEntry*)(data.data() + 6 + 16 * i);
		if ((size_t)entry.dwImageOffset + entry.dwBytesInRes > data.size()) {
			continue;
		}
		if (nBest < 0) {
			nBest = i;
			continue;
		}
		const SIconDirEntry& best = *(const SIconDirEntry*)(data.data() + 6 + 16 * nBest);
		int nArea = IconEntryWidth(entry) * IconEntryHeight(entry);
		int nBestArea = IconEntryWidth(best) * IconEntryHeight(best);
		if (nArea > nBestArea || (nArea == nBestArea && entry.wBitCount > best.wBitCount)) {
			nBest = i;
		}
	}
	if (nBest < 0) {
		return;
	}

	const SIconDirEntry& entry = *(const SIconDirEntry*)(data.data() + 6 + 16 * nBest);
	const BYTE* pPayload = data.data() + entry.dwImageOffset;
	size_t nPayloadSize = entry.dwBytesInRes;

	int nWidth = 0;
	int nHeight = 0;
	uint32* pPixels = NULL;
	// 大尺寸条目是 PNG 载荷（\x89PNG），其余是 DIB
	if (nPayloadSize > 8 && pPayload[0] == 0x89 && pPayload[1] == 'P' && pPayload[2] == 'N' && pPayload[3] == 'G') {
		pPixels = DecodeIconPng(pPayload, nPayloadSize, nWidth, nHeight);
	} else {
		pPixels = DecodeIconDib(pPayload, nPayloadSize, nWidth, nHeight);
	}
	if (pPixels == NULL) {
		return;
	}

	request->Image = new CJPEGImage(nWidth, nHeight, pPixels, NULL, 4, 0, IF_ICO, false, 0, 1, 0);
}

void CImageLoadThread::ProcessReadORARequest(CRequest * request) {
	bool bOutOfMemory = false;
	request->Image = CReaderORA::ReadImage(request->FileName, bOutOfMemory);
	request->OutOfMemory = request->Image == NULL && bOutOfMemory;
}

void CImageLoadThread::ProcessReadGDIPlusRequest(CRequest * request) {
	const wchar_t* sFileName;
	sFileName = (const wchar_t*)request->FileName;

	Gdiplus::Bitmap* pBitmap = NULL;
	if (sFileName == m_sLastFileName) {
		pBitmap = m_pLastBitmap;
	} else {
		DeleteCachedGDIBitmap();
		m_pLastBitmap = pBitmap = new Gdiplus::Bitmap(sFileName, CSettingsProvider::This().UseEmbeddedColorProfiles());
		m_sLastFileName = sFileName;
	}
	bool isOutOfMemory, isAnimatedGIF;
	request->Image = ConvertGDIPlusBitmapToJPEGImage(pBitmap, request->FrameIndex, NULL, 0, isOutOfMemory, isAnimatedGIF);
	request->OutOfMemory = request->Image == NULL && isOutOfMemory;
	if (!isAnimatedGIF) {
		DeleteCachedGDIBitmap();
	}
}

static unsigned char* alloc(int sizeInBytes) {
	return new(std::nothrow) unsigned char[sizeInBytes];
}

static void dealloc(unsigned char* buffer) {
	delete[] buffer;
}

typedef unsigned char* Allocator(int sizeInBytes);
typedef void Deallocator(unsigned char* buffer);

unsigned char* __stdcall LoadImageWithWIC(LPCWSTR fileName, Allocator* allocator, Deallocator* deallocator,
	unsigned int* width, unsigned int* height);

void CImageLoadThread::ProcessReadWICRequest(CRequest* request) {
	const wchar_t* sFileName;
	sFileName = (const wchar_t*)request->FileName;

	try {
		uint32 nWidth, nHeight;
		unsigned char* pDIB = LoadImageWithWIC(sFileName, &alloc, &dealloc, &nWidth, &nHeight);
		if (pDIB != NULL) {
			request->Image = new CJPEGImage(nWidth, nHeight, pDIB, NULL, 4, 0, IF_WIC, false, 0, 1, 0);
		}
	} catch (...) {
		// fatal error in WIC
	}
}

bool CImageLoadThread::ProcessImageAfterLoad(CRequest * request) {
	// set process parameters depending on filename
	request->Image->SetFileDependentProcessParams(request->FileName, &(request->ProcessParams));

	// First do rotation, this maybe modifies the width and height
	if (!request->Image->VerifyRotation(CRotationParams(request->ProcessParams.RotationParams, request->ProcessParams.RotationParams.Rotation + request->ProcessParams.UserRotation))) {
		return false;
	}

	// Do nothing (except rotation) if processing after load turned off
	if (GetProcessingFlag(request->ProcessParams.ProcFlags, PFLAG_NoProcessingAfterLoad)) {
		return true;
	}

	int nWidth = request->Image->OrigWidth();
	int nHeight = request->Image->OrigHeight();

	double dZoom = request->ProcessParams.Zoom;
	CSize newSize;
	if (dZoom < 0.0) {
		newSize = Helpers::GetImageRect(nWidth, nHeight, 
			request->ProcessParams.TargetWidth, request->ProcessParams.TargetHeight, request->ProcessParams.AutoZoomMode, dZoom);
	} else {
		newSize = CSize((int)(nWidth*dZoom + 0.5), (int)(nHeight*dZoom + 0.5));
	}

	newSize.cx = max(1, min(65535, newSize.cx));
	newSize.cy = max(1, min(65535, newSize.cy)); // max size must not be bigger than this after zoom

	// clip to target rectangle
	CSize clippedSize(min(request->ProcessParams.TargetWidth, newSize.cx), 
		min(request->ProcessParams.TargetHeight, newSize.cy));

	LimitOffsets(request->ProcessParams.Offsets, CSize(request->ProcessParams.TargetWidth, request->ProcessParams.TargetHeight), newSize);

	// this will process the image and cache the processed DIB in the CJPEGImage instance
	CPoint offsetInImage = request->Image->ConvertOffset(newSize, clippedSize, request->ProcessParams.Offsets);
	return NULL != request->Image->GetDIB(newSize, clippedSize, offsetInImage,
		request->ProcessParams.ImageProcParams, request->ProcessParams.ProcFlags);
}



