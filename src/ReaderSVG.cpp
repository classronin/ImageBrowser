#include "StdAfx.h"
#include "ReaderSVG.h"
#include <string>
#include <cstdlib>
#include <d2d1_3.h>
#include <d3d11.h>
#include <shlwapi.h>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "shlwapi.lib")

// A vector document is rasterised once, on load. 2048 px on the long side is enough for the 6400 %
// ceiling this viewer allows on a 1080p window, and it keeps the memory of one SVG at about 16 MB.
static const int SVG_TARGET_LONG_SIDE = 2048;
static const int SVG_MAX_LONG_SIDE = 16384;
static const long long SVG_MAX_FILE_SIZE = 32 * 1024 * 1024;

static bool ReadAllBytes(LPCTSTR fileName, std::string& data) {
	HANDLE hFile = ::CreateFile(fileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		return false;
	}
	long long nFileSize = 0;
	LARGE_INTEGER liSize;
	if (::GetFileSizeEx(hFile, &liSize)) {
		nFileSize = liSize.QuadPart;
	}
	if (nFileSize <= 0 || nFileSize > SVG_MAX_FILE_SIZE) {
		::CloseHandle(hFile);
		return false;
	}
	data.resize((size_t)nFileSize);
	DWORD nRead = 0;
	bool bOk = ::ReadFile(hFile, &data[0], (DWORD)nFileSize, &nRead, NULL) && (long long)nRead == nFileSize;
	::CloseHandle(hFile);
	return bOk;
}

// Reads the first number of the attribute value, ignoring a trailing unit such as "px".
static bool ParseAttributeNumber(const std::string& tag, const char* name, double& value) {
	std::string key(name);
	size_t pos = tag.find(key);
	if (pos == std::string::npos) {
		return false;
	}
	pos += key.length();
	while (pos < tag.length() && (tag[pos] == ' ' || tag[pos] == '\t' || tag[pos] == '=' || tag[pos] == '"' || tag[pos] == '\'')) {
		pos++;
	}
	const char* pStart = tag.c_str() + pos;
	char* pEnd = NULL;
	double d = strtod(pStart, &pEnd);
	if (pEnd == pStart || d <= 0.0) {
		return false;
	}
	// A percentage is relative to a viewport that is not known here, so it is not usable.
	if (*pEnd == '%') {
		return false;
	}
	value = d;
	return true;
}

// User-unit size of the document: the root width/height when they are absolute, otherwise the viewBox,
// otherwise the 300 x 150 default the SVG specification defines for a missing viewport.
static void ParseIntrinsicSize(const std::string& text, double& dWidth, double& dHeight) {
	dWidth = 0.0;
	dHeight = 0.0;

	size_t nRootStart = text.find("<svg");
	size_t nRootEnd = (nRootStart == std::string::npos) ? std::string::npos : text.find('>', nRootStart);
	std::string rootTag = (nRootEnd == std::string::npos) ? std::string() : text.substr(nRootStart, nRootEnd - nRootStart + 1);

	if (!rootTag.empty()) {
		ParseAttributeNumber(rootTag, "width", dWidth);
		ParseAttributeNumber(rootTag, "height", dHeight);
	}

	if (dWidth <= 0.0 || dHeight <= 0.0) {
		size_t nViewBox = text.find("viewBox");
		if (nViewBox == std::string::npos) {
			nViewBox = text.find("viewbox");
		}
		if (nViewBox != std::string::npos) {
			size_t nQuote = text.find_first_of("\"'", nViewBox);
			size_t nQuoteEnd = (nQuote == std::string::npos) ? std::string::npos : text.find(text[nQuote], nQuote + 1);
			if (nQuoteEnd != std::string::npos) {
				std::string sValues = text.substr(nQuote + 1, nQuoteEnd - nQuote - 1);
				double dValues[4] = { 0.0, 0.0, 0.0, 0.0 };
				int nFound = 0;
				const char* p = sValues.c_str();
				while (nFound < 4) {
					char* pEnd = NULL;
					double d = strtod(p, &pEnd);
					if (pEnd == p) {
						break;
					}
					dValues[nFound++] = d;
					p = pEnd;
				}
				if (nFound == 4 && dValues[2] > 0.0 && dValues[3] > 0.0) {
					dWidth = dValues[2];
					dHeight = dValues[3];
				}
			}
		}
	}

	if (dWidth <= 0.0 || dHeight <= 0.0) {
		dWidth = 300.0;
		dHeight = 150.0;
	}
}

void* SvgReader::ReadImage(LPCTSTR fileName, int& width, int& height, bool& outOfMemory) {
	width = 0;
	height = 0;
	outOfMemory = false;

	std::string text;
	if (!ReadAllBytes(fileName, text)) {
		return NULL;
	}

	double dUserWidth = 0.0, dUserHeight = 0.0;
	ParseIntrinsicSize(text, dUserWidth, dUserHeight);

	double dScale = SVG_TARGET_LONG_SIDE / max(dUserWidth, dUserHeight);
	if (dScale < 1.0) {
		dScale = 1.0;
	}
	if (dScale > 8.0) {
		dScale = 8.0;
	}
	int nPixelWidth = (int)(dUserWidth * dScale + 0.5);
	int nPixelHeight = (int)(dUserHeight * dScale + 0.5);
	if (nPixelWidth <= 0 || nPixelHeight <= 0) {
		return NULL;
	}
	if (nPixelWidth > SVG_MAX_LONG_SIDE || nPixelHeight > SVG_MAX_LONG_SIDE) {
		return NULL;
	}

	CComPtr<ID2D1Factory3> pFactory;
	if (FAILED(::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory3), NULL, (void**)&pFactory))) {
		return NULL;
	}

	// Direct2D's SVG document lives on a device context, which needs a Direct3D device behind it.
	// WARP keeps that entirely in software: no display, no driver requirement.
	CComPtr<ID3D11Device> pD3DDevice;
	CComPtr<ID3D11DeviceContext> pD3DContext;
	D3D_FEATURE_LEVEL eFeatureLevel;
	if (FAILED(::D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
		NULL, 0, D3D11_SDK_VERSION, &pD3DDevice, &eFeatureLevel, &pD3DContext))) {
		return NULL;
	}

	CComPtr<IDXGIDevice> pDxgiDevice;
	if (FAILED(pD3DDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&pDxgiDevice))) {
		return NULL;
	}

	CComPtr<ID2D1Device> pD2DDevice;
	if (FAILED(pFactory->CreateDevice(pDxgiDevice, &pD2DDevice))) {
		return NULL;
	}

	CComPtr<ID2D1DeviceContext> pDeviceContext;
	if (FAILED(pD2DDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &pDeviceContext))) {
		return NULL;
	}

	CComPtr<ID2D1DeviceContext5> pContext5;
	if (FAILED(pDeviceContext->QueryInterface(__uuidof(ID2D1DeviceContext5), (void**)&pContext5))) {
		return NULL;
	}

	CComPtr<IStream> pStream;
	pStream.Attach(::SHCreateMemStream((const BYTE*)text.data(), (UINT)text.size()));
	if (pStream == NULL) {
		return NULL;
	}

	CComPtr<ID2D1SvgDocument> pDocument;
	if (FAILED(pContext5->CreateSvgDocument(pStream, D2D1::SizeF((float)dUserWidth, (float)dUserHeight), &pDocument))) {
		return NULL;
	}

	// Direct2D cannot make a bitmap that is both a render target and CPU readable (E_INVALIDARG), so
	// the frame is drawn into a D3D render-target texture and read back through a staging copy.
	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = (UINT)nPixelWidth;
	desc.Height = (UINT)nPixelHeight;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

	CComPtr<ID3D11Texture2D> pTexture;
	if (FAILED(pD3DDevice->CreateTexture2D(&desc, NULL, &pTexture))) {
		return NULL;
	}

	CComPtr<IDXGISurface> pSurface;
	if (FAILED(pTexture->QueryInterface(__uuidof(IDXGISurface), (void**)&pSurface))) {
		return NULL;
	}

	D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
		D2D1_BITMAP_OPTIONS_TARGET,
		D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);

	CComPtr<ID2D1Bitmap1> pBitmap;
	if (FAILED(pContext5->CreateBitmapFromDxgiSurface(pSurface, &props, &pBitmap))) {
		return NULL;
	}

	uint8* pPixels = new(std::nothrow) uint8[(size_t)nPixelWidth * (size_t)nPixelHeight * 4];
	if (pPixels == NULL) {
		outOfMemory = true;
		return NULL;
	}

	pContext5->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
	pContext5->SetTarget(pBitmap);
	pContext5->SetTransform(D2D1::Matrix3x2F::Scale((float)dScale, (float)dScale));
	pContext5->BeginDraw();
	pContext5->Clear(D2D1::ColorF(0.0f, 0.0f));

	// DrawSvgDocument takes the document, so the root element is rendered with the transform above.
	pContext5->DrawSvgDocument(pDocument);

	HRESULT hrEnd = pContext5->EndDraw();
	if (FAILED(hrEnd)) {
		delete[] pPixels;
		return NULL;
	}

	D3D11_TEXTURE2D_DESC stagingDesc = desc;
	stagingDesc.Usage = D3D11_USAGE_STAGING;
	stagingDesc.BindFlags = 0;
	stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

	CComPtr<ID3D11Texture2D> pStaging;
	if (FAILED(pD3DDevice->CreateTexture2D(&stagingDesc, NULL, &pStaging))) {
		delete[] pPixels;
		return NULL;
	}

	pD3DContext->CopyResource(pStaging, pTexture);
	D3D11_MAPPED_SUBRESOURCE mapped;
	if (FAILED(pD3DContext->Map(pStaging, 0, D3D11_MAP_READ, 0, &mapped))) {
		delete[] pPixels;
		return NULL;
	}

	// Direct2D hands out premultiplied BGRA; the image pipeline wants straight alpha and does the
	// premultiply itself, so every pixel is divided back out here.
	for (int y = 0; y < nPixelHeight; y++) {
		const uint8* pSrc = (const uint8*)mapped.pData + (size_t)y * mapped.RowPitch;
		uint8* pDst = pPixels + (size_t)y * (size_t)nPixelWidth * 4;
		for (int x = 0; x < nPixelWidth; x++) {
			uint8 a = pSrc[3];
			if (a == 0) {
				pDst[0] = pDst[1] = pDst[2] = pDst[3] = 0;
			} else if (a == 255) {
				pDst[0] = pSrc[0];
				pDst[1] = pSrc[1];
				pDst[2] = pSrc[2];
				pDst[3] = 255;
			} else {
				pDst[0] = (uint8)min(255, (pSrc[0] * 255 + a / 2) / a);
				pDst[1] = (uint8)min(255, (pSrc[1] * 255 + a / 2) / a);
				pDst[2] = (uint8)min(255, (pSrc[2] * 255 + a / 2) / a);
				pDst[3] = a;
			}
			pSrc += 4;
			pDst += 4;
		}
	}
	pD3DContext->Unmap(pStaging, 0);

	width = nPixelWidth;
	height = nPixelHeight;
	return pPixels;
}

void SvgReader::FreeMemory(void* pointer) {
	delete[] (uint8*)pointer;
}
