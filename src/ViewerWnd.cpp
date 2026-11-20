// ViewerWnd.cpp : the entire ImageBrowser UI.
//
// Design notes (see 项目导航.md):
//  * Rendering is delegated to CJPEGImage::GetDIB(), which resamples with the upstream quality
//    filters and only produces the visible rectangle. We never allocate a full-size DIB.
//  * Rotation is preview-only: the rotation is fed into VerifyRotation() as a parameter, so the
//    pixels are rotated in memory and never written back to disk. Reloading a file resets it.
//  * Configuration is exactly two keys in ImageBrowser.ini, written only on exit.

#include "stdafx.h"
#include "ViewerWnd.h"

#include <commdlg.h>
#include "resource.h"
#include "JPEGProvider.h"
#include "JPEGImage.h"
#include "FileList.h"
#include "DirectoryWatcher.h"
#include "Helpers.h"
#include "MaxImageDef.h"
#include "MessageDef.h"

namespace {

const int ANIMATION_TIMER_ID = 1;
// Upstream refines the resampling only after the user stops interacting (ZOOM_TIMEOUT is
// 200 ms there); the same delay is used here so the two feel alike.
const int HQ_REFINE_TIMER_ID = 2;
const int kHQRefineDelayMs = 200;
// Zoom percentage stays on screen for a second after the last wheel notch (upstream: 1000 ms).
const int ZOOM_LABEL_TIMER_ID = 3;
const int kZoomLabelDelayMs = 1000;
const int kZoomLabelWidth = 70;
const int kZoomLabelHeight = 25;
// The percentage sits in the lower left corner, worn like a caption on the picture: no plate behind
// it, just a 1 px black outline so light digits stay readable on a white photo.
const int kZoomLabelLeftOffset = 32;
const int kZoomLabelBottomOffset = 12;

const TCHAR* const kConfigFileName = _T("ImageBrowser.ini");
const TCHAR* const kKeyMaximized = _T("Maximized");
const TCHAR* const kKeyBackground = _T("BackgroundColor");
const TCHAR* const kKeyBackMode = _T("BackMode");

// ImageBrowser opens with a sane window instead of remembering arbitrary geometry.
const int kDefaultWidth = 1024;
const int kDefaultHeight = 768;

// Zoom limits. Upstream (Helpers.h) sets ZoomMin = DBL_MIN and then floors the result at 0.0001,
// so the wheel really can shrink an image to 0.01%; the old 5% floor made zooming out stop roughly
// ten notches too early. kMaxZoom is only the fallback used when there is no image to measure
// against - a real image is limited by the 65535 px virtual size rule in ZoomAt.
const double kMinZoom = 0.0001;
const double kMaxZoom = 8.0;
const double kWheelStep = 1.25;

CString ExeDirectory() {
	TCHAR path[MAX_PATH];
	::GetModuleFileName(NULL, path, MAX_PATH);
	CString sPath(path);
	int nPos = sPath.ReverseFind(_T('\\'));
	return (nPos > 0) ? sPath.Left(nPos) : sPath;
}

} // namespace

CViewerWnd::CViewerWnd()
	: m_pProvider(NULL)
	, m_pFileList(NULL)
	, m_pWatcher(NULL)
	, m_pImage(NULL)
	, m_bFitToWindow(true)
	, m_dZoom(1.0)
	, m_dRealizedZoom(1.0)
	, m_nRotation(0)
	, m_ptOffset(0, 0)
	, m_bDragging(false)
	// Colour is the default: it is what the empty window shows and what a freshly dropped image
	// keeps, so the two states never disagree. Tab switches to alpha (checkerboard) on demand.
	, m_eBackMode(BACK_Alpha)
	, m_clrBackground(RGB(32, 32, 32))
	, m_hdcBack(NULL)
	, m_hbmBack(NULL)
	, m_hbmBackOld(NULL)
	, m_pBackBits(NULL)
	, m_nBackWidth(0)
	, m_nBackHeight(0)
	// Neutral processing: no contrast, no gamma, no saturation change, no sharpening, no colour correction.
	, m_procParams(0.0, 1.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0)
	, m_procFlags(PFLAG_HighQualityResampling)
	, m_hZoomFont(NULL)
	, m_bZoomLabelVisible(false)
	, m_bLoadError(false)
	, m_hErrorFont(NULL)
	, m_hIconBig(NULL)
	, m_hIconSmall(NULL)
	, m_nAnimationOffset(0)
	, m_nAnimationExpectedTick(0)
	, m_bConfigMaximized(false)
	, m_clrConfigBackground(RGB(32, 32, 32))
	, m_bStartupDone(false)
{
}

CViewerWnd::~CViewerWnd() {
	if (m_hdcBack != NULL) {
		if (m_hbmBackOld != NULL) {
			::SelectObject(m_hdcBack, m_hbmBackOld);
		}
		::DeleteDC(m_hdcBack);
		m_hdcBack = NULL;
	}
	if (m_hbmBack != NULL) {
		::DeleteObject(m_hbmBack);
		m_hbmBack = NULL;
	}
	if (m_hZoomFont != NULL) {
		::DeleteObject(m_hZoomFont);
		m_hZoomFont = NULL;
	}
	if (m_hErrorFont != NULL) {
		::DeleteObject(m_hErrorFont);
		m_hErrorFont = NULL;
	}
	// The window must not keep pointers to icons that are about to be destroyed.
	if (m_hWnd != NULL) {
		SendMessage(WM_SETICON, ICON_BIG, (LPARAM)NULL);
		SendMessage(WM_SETICON, ICON_SMALL, (LPARAM)NULL);
	}
	if (m_hIconBig != NULL) {
		::DestroyIcon(m_hIconBig);
		m_hIconBig = NULL;
	}
	if (m_hIconSmall != NULL) {
		::DestroyIcon(m_hIconSmall);
		m_hIconSmall = NULL;
	}
	ReleaseImage();
	delete m_pProvider;
	delete m_pFileList;
	delete m_pWatcher;
}

CString CViewerWnd::ConfigPath() const {
	return ExeDirectory() + _T("\\") + kConfigFileName;
}

void CViewerWnd::LoadConfig() {
	CString sIni = ConfigPath();
	m_bConfigMaximized = ::GetPrivateProfileInt(_T("ImageBrowser"), kKeyMaximized, 0, sIni) != 0;

	TCHAR buff[32] = { 0 };
	::GetPrivateProfileString(_T("ImageBrowser"), kKeyBackground, _T(""), buff, 31, sIni);
	if (buff[0] != 0) {
		// Stored as 0x00BBGGRR so that a human reading the ini sees a colour, not a number soup.
		m_clrConfigBackground = (COLORREF)_tcstoul(buff, NULL, 16);
	}
	m_clrBackground = m_clrConfigBackground;

	// The background mode is a deliberate choice the user made with Tab, so it survives a restart.
	// First run (no key at all) starts transparent: the image sits on its own alpha, and Tab switches
	// to the flat colour.
	TCHAR sMode[32] = { 0 };
	::GetPrivateProfileString(_T("ImageBrowser"), kKeyBackMode, _T("Alpha"), sMode, 31, sIni);
	m_eBackMode = (_tcsicmp(sMode, _T("Alpha")) == 0) ? BACK_Alpha : BACK_Color;
}

void CViewerWnd::SaveConfig() const {
	CString sIni = ConfigPath();
	TCHAR buff[32];
	_stprintf_s(buff, 32, _T("0x%06X"), (unsigned int)m_clrConfigBackground);
	::WritePrivateProfileString(_T("ImageBrowser"), kKeyMaximized,
		(::IsZoomed((HWND)*this) ? _T("1") : _T("0")), sIni);
	::WritePrivateProfileString(_T("ImageBrowser"), kKeyBackground, buff, sIni);
	::WritePrivateProfileString(_T("ImageBrowser"), kKeyBackMode,
		(m_eBackMode == BACK_Alpha ? _T("Alpha") : _T("Color")), sIni);
}

bool CViewerWnd::CreateMain() {
	LoadConfig();

	CRect rcWindow;
	int nScrW = ::GetSystemMetrics(SM_CXSCREEN);
	int nScrH = ::GetSystemMetrics(SM_CYSCREEN);
	int nW = min(kDefaultWidth, nScrW), nH = min(kDefaultHeight, nScrH);
	rcWindow.SetRect((nScrW - nW) / 2, (nScrH - nH) / 2, (nScrW - nW) / 2 + nW, (nScrH - nH) / 2 + nH);

	if (Create(NULL, rcWindow, _T(ImageBrowser_TITLE),
			WS_OVERLAPPEDWINDOW, WS_EX_ACCEPTFILES) == NULL) {
		return false;
	}

	ShowWindow(m_bConfigMaximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
	UpdateWindow();
	return true;
}

void CViewerWnd::RunMessageLoop() {
	MSG msg;
	while (::GetMessage(&msg, NULL, 0, 0) != 0) {
		::TranslateMessage(&msg);
		::DispatchMessage(&msg);
	}
}

/////////////////////////////////////////////////////////////////////////////
// Messages
/////////////////////////////////////////////////////////////////////////////

LRESULT CViewerWnd::OnCreate(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/) {
	m_pWatcher = new CDirectoryWatcher(m_hWnd);
	m_pProvider = new CJPEGProvider(m_hWnd, 1, 2);
	::DragAcceptFiles(m_hWnd, TRUE);

	// The window class carries no icon, so the caption, the taskbar button and Alt+Tab would fall
	// back to the generic application icon. Both sizes come from the same embedded resource.
	HINSTANCE hInstance = ::GetModuleHandle(NULL);
	m_hIconBig = (HICON)::LoadImage(hInstance, MAKEINTRESOURCE(IDR_MAINFRAME), IMAGE_ICON,
		::GetSystemMetrics(SM_CXICON), ::GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
	m_hIconSmall = (HICON)::LoadImage(hInstance, MAKEINTRESOURCE(IDR_MAINFRAME), IMAGE_ICON,
		::GetSystemMetrics(SM_CXSMICON), ::GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
	if (m_hIconBig != NULL) {
		SendMessage(WM_SETICON, ICON_BIG, (LPARAM)m_hIconBig);
	}
	if (m_hIconSmall != NULL) {
		SendMessage(WM_SETICON, ICON_SMALL, (LPARAM)m_hIconSmall);
	}

	SetTimer(0, 0, NULL); // placeholder, real timers are the animation timer only
	KillTimer(0);
	return 0;
}

LRESULT CViewerWnd::OnEraseBkgnd(UINT, WPARAM, LPARAM, BOOL& bHandled) {
	// Everything is painted in OnPaint into a back buffer; erasing here would only flicker.
	bHandled = TRUE;
	return 1;
}

void CViewerWnd::EnsureBackBuffer(int nWidth, int nHeight) {
	if (nWidth < 1) nWidth = 1;
	if (nHeight < 1) nHeight = 1;
	if (m_pBackBits != NULL && m_nBackWidth == nWidth && m_nBackHeight == nHeight) {
		return;
	}

	if (m_hdcBack == NULL) {
		m_hdcBack = ::CreateCompatibleDC(NULL);
		if (m_hdcBack == NULL) return;
	}
	if (m_hbmBack != NULL) {
		if (m_hbmBackOld != NULL) {
			::SelectObject(m_hdcBack, m_hbmBackOld);
			m_hbmBackOld = NULL;
		}
		::DeleteObject(m_hbmBack);
		m_hbmBack = NULL;
		m_pBackBits = NULL;
	}

	BITMAPINFO bmi;
	memset(&bmi, 0, sizeof(BITMAPINFO));
	bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth = nWidth;
	bmi.bmiHeader.biHeight = -nHeight; // top-down: rows run in screen order
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 32;
	bmi.bmiHeader.biCompression = BI_RGB;
	m_hbmBack = ::CreateDIBSection(m_hdcBack, &bmi, DIB_RGB_COLORS, (void**)&m_pBackBits, NULL, 0);
	if (m_hbmBack == NULL || m_pBackBits == NULL) {
		m_pBackBits = NULL;
		return;
	}
	m_hbmBackOld = (HBITMAP)::SelectObject(m_hdcBack, m_hbmBack);
	m_nBackWidth = nWidth;
	m_nBackHeight = nHeight;
}

// Fills one rectangle of the back buffer with the current background. Only the area the image
// does not cover is filled: painting pixels that the image overwrites next would be wasted work,
// and while panning or zooming a frame normally covers the whole window.
void CViewerWnd::FillBackground(uint32* pBits, int nStride, int nWidth, int nHeight) {
	FillArea(pBits, nStride, 0, 0, nWidth, nHeight);
}

void CViewerWnd::FillArea(uint32* pBits, int nStride, int nLeft, int nTop, int nWidth, int nHeight) {
	if (pBits == NULL || nWidth <= 0 || nHeight <= 0) return;
	if (m_eBackMode == BACK_Alpha) {
		// Same 8 pixel checkerboard the window used as a pattern brush: gray where the cell parity
		// is even, white otherwise, anchored at the client origin. One colour decision per 8 pixel run.
		const uint32 nGray = 0xFFC8C8C8;
		const uint32 nWhite = 0xFFFFFFFF;
		for (int y = 0; y < nHeight; y++) {
			uint32* pLine = pBits + (size_t)(nTop + y) * nStride + nLeft;
			const bool bOddRow = (((nTop + y) >> 3) & 1) != 0;
			int x = 0;
			while (x < nWidth) {
				const uint32 nColor = (((((nLeft + x) >> 3) & 1) != 0) == bOddRow) ? nWhite : nGray;
				const int nRunEnd = min(nWidth, (x + 8) - ((nLeft + x) & 7));
				for (; x < nRunEnd; x++) {
					pLine[x] = nColor;
				}
			}
		}
	} else {
		const uint32 nColor = 0xFF000000 | ((uint32)GetRValue(m_clrBackground) << 16) |
			((uint32)GetGValue(m_clrBackground) << 8) | (uint32)GetBValue(m_clrBackground);
		for (int y = 0; y < nHeight; y++) {
			uint32* pLine = pBits + (size_t)(nTop + y) * nStride + nLeft;
			for (int x = 0; x < nWidth; x++) {
				pLine[x] = nColor;
			}
		}
	}
}

CSize CViewerWnd::ComputeVirtualSize(const CRect& clientRect) const {
	if (m_pImage == NULL) {
		return CSize(0, 0);
	}
	double dZoom = m_bFitToWindow ? -1.0 : m_dZoom;
	Helpers::EAutoZoomMode eMode = Helpers::ZM_FitToScreenNoZoom;
	CSize size = Helpers::GetVirtualImageSize(m_pImage->OrigSize(), clientRect.Size(), eMode, dZoom);
	return size;
}

bool CViewerWnd::DrawImage(uint32* pBackBits, int nBackStride, const CRect& clientRect) {
	if (m_pImage == NULL || pBackBits == NULL) {
		return false;
	}

	// Rotation is applied to the in-memory pixels; the file on disk is untouched.
	m_pImage->VerifyRotation(CRotationParams(m_pImage->GetRotationParams(), m_nRotation));

	CSize newSize = ComputeVirtualSize(clientRect);
	if (newSize.cx <= 0 || newSize.cy <= 0) {
		return false;
	}
	m_dRealizedZoom = (double)newSize.cx / (double)m_pImage->OrigSize().cx;

	CSize clippedSize(min(clientRect.Width(), newSize.cx), min(clientRect.Height(), newSize.cy));
	// Upstream keeps m_offsets permanently inside the allowed range: the clamp happens here, in the
	// paint pass, and the clamped value is what the next zoom anchors on. Overshooting the drag is
	// therefore discarded instead of sliding the picture and exposing background (upstream only
	// shifts the DIB by the excess in zoom-navigator mode, which this app does not have).
	m_ptOffset = Helpers::LimitOffsets(m_ptOffset, clientRect.Size(), newSize);
	CPoint limitedOffset = m_ptOffset;
	CPoint drawOffset(0, 0);
	CPoint offsetInImage = CJPEGImage::ConvertOffset(newSize, clippedSize, limitedOffset);

	void* pDIB = m_pImage->GetDIB(newSize, clippedSize, offsetInImage, m_procParams, m_procFlags);
	if (pDIB == NULL) {
		return false;
	}

	int xDest = (clientRect.Width() - clippedSize.cx) / 2 + drawOffset.x;
	int yDest = (clientRect.Height() - clippedSize.cy) / 2 + drawOffset.y;

	// The image can hang off the left/top edge while panning, so clip instead of writing outside
	// the back buffer.
	int nCopyX = xDest, nCopyY = yDest;
	int nCopyW = clippedSize.cx, nCopyH = clippedSize.cy;
	int nSkipX = 0, nSkipY = 0;
	if (nCopyX < 0) { nSkipX = -nCopyX; nCopyW += nCopyX; nCopyX = 0; }
	if (nCopyY < 0) { nSkipY = -nCopyY; nCopyH += nCopyY; nCopyY = 0; }
	nCopyW = min(nCopyW, clientRect.Width() - nCopyX);
	nCopyH = min(nCopyH, clientRect.Height() - nCopyY);
	if (nCopyW <= 0 || nCopyH <= 0) {
		return false;
	}

	// Background only where the image does not reach; the pixels underneath it are overwritten anyway.
	int nClientW = clientRect.Width();
	int nClientH = clientRect.Height();
	FillArea(pBackBits, nBackStride, 0, 0, nClientW, nCopyY);
	FillArea(pBackBits, nBackStride, 0, nCopyY + nCopyH, nClientW, nClientH - (nCopyY + nCopyH));
	FillArea(pBackBits, nBackStride, 0, nCopyY, nCopyX, nCopyH);
	FillArea(pBackBits, nBackStride, nCopyX + nCopyW, nCopyY, nClientW - (nCopyX + nCopyW), nCopyH);

	const uint8* pSrcBase = (const uint8*)pDIB;
	const bool bTransparent = m_pImage->HasTransparency();
	if (!bTransparent) {
		// Opaque image: straight row copy, no per pixel work at all.
		for (int y = 0; y < nCopyH; y++) {
			uint32* pDst = pBackBits + (size_t)(nCopyY + y) * nBackStride + nCopyX;
			const uint8* pSrc = pSrcBase + ((size_t)(nSkipY + y) * clippedSize.cx + nSkipX) * 4;
			memcpy(pDst, pSrc, (size_t)nCopyW * 4);
		}
		return true;
	}

	// Transparency: the pixels are stored premultiplied, so dst = src + bg * (255 - a) / 255 gives
	// proper partial transparency with clean edges. The background for each pixel is computed on the
	// spot instead of being painted first, so the image area is written exactly once. The scaling
	// itself comes from a 256x256 table: an integer division per channel per pixel was the single
	// most expensive thing left in the paint path.
	struct CBlendTable {
		uint8 mA[256 * 256];
		CBlendTable() {
			for (int a = 0; a < 256; a++) {
				for (int b = 0; b < 256; b++) {
					mA[a * 256 + b] = (uint8)((a * b) / 255);
				}
			}
		}
	};
	static const CBlendTable s_blend;

	const bool bChecker = (m_eBackMode == BACK_Alpha);
	const uint32 nSolid = 0xFF000000 | ((uint32)GetRValue(m_clrBackground) << 16) |
		((uint32)GetGValue(m_clrBackground) << 8) | (uint32)GetBValue(m_clrBackground);
	const uint32 nGray = 0xFFC8C8C8;
	const uint32 nWhite = 0xFFFFFFFF;
	for (int y = 0; y < nCopyH; y++) {
		uint32* pDstRow = pBackBits + (size_t)(nCopyY + y) * nBackStride + nCopyX;
		const uint8* pSrc = pSrcBase + ((size_t)(nSkipY + y) * clippedSize.cx + nSkipX) * 4;
		const int nRowY = nCopyY + y;
		const bool bOddRow = ((nRowY >> 3) & 1) != 0;
		if (!bChecker) {
			const uint32 nBgB = nSolid & 0xFF, nBgG = (nSolid >> 8) & 0xFF, nBgR = (nSolid >> 16) & 0xFF;
			for (int x = 0; x < nCopyW; x++, pSrc += 4) {
				const uint32 nAlpha = pSrc[3];
				if (nAlpha == 0) {
					pDstRow[x] = nSolid;
					continue;
				}
				if (nAlpha == 255) {
					pDstRow[x] = 0xFF000000 | ((uint32)pSrc[2] << 16) | ((uint32)pSrc[1] << 8) | (uint32)pSrc[0];
					continue;
				}
				const uint32 nInv = 255 - nAlpha;
				const uint32 nBlue = min(255, (uint32)pSrc[0] + s_blend.mA[nInv * 256 + nBgB]);
				const uint32 nGreen = min(255, (uint32)pSrc[1] + s_blend.mA[nInv * 256 + nBgG]);
				const uint32 nRed = min(255, (uint32)pSrc[2] + s_blend.mA[nInv * 256 + nBgR]);
				pDstRow[x] = 0xFF000000 | (nRed << 16) | (nGreen << 8) | nBlue;
			}
			continue;
		}
		// Checkerboard: the cell colour only changes every 8 pixels, so it is picked once per run.
		int x = 0;
		while (x < nCopyW) {
			const uint32 nBg = (((((nCopyX + x) >> 3) & 1) != 0) == bOddRow) ? nWhite : nGray;
			const uint32 nBgB = nBg & 0xFF, nBgG = (nBg >> 8) & 0xFF, nBgR = (nBg >> 16) & 0xFF;
			const int nRunEnd = min(nCopyW, (x + 8) - ((nCopyX + x) & 7));
			for (; x < nRunEnd; x++, pSrc += 4) {
				const uint32 nAlpha = pSrc[3];
				if (nAlpha == 0) {
					pDstRow[x] = nBg;
					continue;
				}
				if (nAlpha == 255) {
					pDstRow[x] = 0xFF000000 | ((uint32)pSrc[2] << 16) | ((uint32)pSrc[1] << 8) | (uint32)pSrc[0];
					continue;
				}
				const uint32 nInv = 255 - nAlpha;
				const uint32 nBlue = min(255, (uint32)pSrc[0] + s_blend.mA[nInv * 256 + nBgB]);
				const uint32 nGreen = min(255, (uint32)pSrc[1] + s_blend.mA[nInv * 256 + nBgG]);
				const uint32 nRed = min(255, (uint32)pSrc[2] + s_blend.mA[nInv * 256 + nBgR]);
				pDstRow[x] = 0xFF000000 | (nRed << 16) | (nGreen << 8) | nBlue;
			}
		}
	}
	return true;
}

void CViewerWnd::DrawZoomLabel(const CRect& clientRect) {
	// Lower left corner, no plate: the digits are drawn in light grey and outlined 1 px in black,
	// which keeps them readable on a dark photo and on a white one alike.
	if (m_hZoomFont == NULL) {
		m_hZoomFont = ::CreateFont(-16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
			OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, _T("Segoe UI"));
	}
	if (m_hZoomFont == NULL) {
		return;
	}
	CString sZoom;
	sZoom.Format(_T("%d%%"), (int)(m_dRealizedZoom * 100.0 + 0.5));
	CRect rcLabel(kZoomLabelLeftOffset, clientRect.Height() - kZoomLabelBottomOffset - kZoomLabelHeight,
		kZoomLabelLeftOffset + kZoomLabelWidth, clientRect.Height() - kZoomLabelBottomOffset);

	HGDIOBJ hOldFont = ::SelectObject(m_hdcBack, m_hZoomFont);
	::SetBkMode(m_hdcBack, TRANSPARENT);

	// Outline first: the same string blitted at the eight neighbours of every glyph, then the glyph
	// itself on top. One pixel of black separates the text from whatever colour it lands on.
	::SetTextColor(m_hdcBack, RGB(0, 0, 0));
	for (int dy = -1; dy <= 1; dy++) {
		for (int dx = -1; dx <= 1; dx++) {
			if (dx == 0 && dy == 0) {
				continue;
			}
			CRect rcOutline(rcLabel);
			rcOutline.OffsetRect(dx, dy);
			::DrawText(m_hdcBack, sZoom, -1, &rcOutline, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
		}
	}

	::SetTextColor(m_hdcBack, RGB(232, 232, 232));
	::DrawText(m_hdcBack, sZoom, -1, &rcLabel, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
	::SelectObject(m_hdcBack, hOldFont);
}

void CViewerWnd::DrawLoadError(const CRect& clientRect) {
	// "无法读取文件" in white with a 1 px black outline, centred. No message box: a file that cannot
	// be decoded leaves the window saying so and nothing else.
	if (m_hErrorFont == NULL) {
		m_hErrorFont = ::CreateFont(-28, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
			OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, _T("Microsoft YaHei"));
	}
	if (m_hErrorFont == NULL) {
		return;
	}

	const TCHAR* sMessage = _T("无法读取文件");
	HGDIOBJ hOldFont = ::SelectObject(m_hdcBack, m_hErrorFont);
	::SetBkMode(m_hdcBack, TRANSPARENT);

	CRect rcText(clientRect);
	::SetTextColor(m_hdcBack, RGB(0, 0, 0));
	for (int dy = -1; dy <= 1; dy++) {
		for (int dx = -1; dx <= 1; dx++) {
			if (dx == 0 && dy == 0) {
				continue;
			}
			CRect rcOutline(rcText);
			rcOutline.OffsetRect(dx, dy);
			::DrawText(m_hdcBack, sMessage, -1, &rcOutline, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
		}
	}

	::SetTextColor(m_hdcBack, RGB(255, 255, 255));
	::DrawText(m_hdcBack, sMessage, -1, &rcText, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
	::SelectObject(m_hdcBack, hOldFont);
}

LRESULT CViewerWnd::OnPaint(UINT, WPARAM, LPARAM, BOOL& /*bHandled*/) {
	PAINTSTRUCT ps;
	HDC hdc = ::BeginPaint(m_hWnd, &ps);

	CRect rcClient;
	GetClientRect(&rcClient);

	// One owned 32bpp DIB as back buffer, kept across frames: the background and the image are
	// both written straight into its pixels, then a single BitBlt puts the frame on screen.
	EnsureBackBuffer(max(1, rcClient.Width()), max(1, rcClient.Height()));
	if (m_pBackBits != NULL) {
		// An image paints its own background, only where it does not cover the client; with no
		// image the whole window needs one.
		if (m_pImage == NULL || !DrawImage(m_pBackBits, m_nBackWidth, rcClient)) {
			FillBackground(m_pBackBits, m_nBackWidth, m_nBackWidth, m_nBackHeight);
		}
		if (m_bZoomLabelVisible && m_pImage != NULL) {
			DrawZoomLabel(rcClient);
		}
		if (m_bLoadError && m_pImage == NULL) {
			DrawLoadError(rcClient);
		}
		::BitBlt(hdc, 0, 0, rcClient.Width(), rcClient.Height(), m_hdcBack, 0, 0, SRCCOPY);
	}

	::EndPaint(m_hWnd, &ps);
	return 0;
}

LRESULT CViewerWnd::OnSize(UINT, WPARAM, LPARAM, BOOL& /*bHandled*/) {
	BeginInteractivePaint();
	return 0;
}

LRESULT CViewerWnd::OnGetMinMaxInfo(UINT, WPARAM, LPARAM lParam, BOOL& /*bHandled*/) {
	MINMAXINFO* pInfo = (MINMAXINFO*)lParam;
	pInfo->ptMinTrackSize.x = 240;
	pInfo->ptMinTrackSize.y = 180;
	return 0;
}

LRESULT CViewerWnd::OnLButtonDown(UINT, WPARAM, LPARAM lParam, BOOL& /*bHandled*/) {
	m_bDragging = true;
	m_ptDragOrigin = CPoint(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
	SetCapture();
	SetCursor(::LoadCursor(NULL, IDC_HAND));
	return 0;
}

LRESULT CViewerWnd::OnLButtonUp(UINT, WPARAM, LPARAM, BOOL& /*bHandled*/) {
	if (m_bDragging) {
		m_bDragging = false;
		ReleaseCapture();
		SetCursor(::LoadCursor(NULL, IDC_ARROW));
	}
	return 0;
}

LRESULT CViewerWnd::OnMouseMove(UINT, WPARAM, LPARAM lParam, BOOL& /*bHandled*/) {
	if (m_bDragging && m_pImage != NULL) {
		CPoint pt(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
		CPoint delta(pt.x - m_ptDragOrigin.x, pt.y - m_ptDragOrigin.y);
		m_ptDragOrigin = pt;
		CRect rcClient;
		GetClientRect(&rcClient);
		CSize newSize = ComputeVirtualSize(rcClient);
		// Nothing to pan while the picture fits, and the offset is clamped right here so state can
		// never drift past the edge the way it did before.
		if (newSize.cx > 0 && newSize.cy > 0 &&
			(newSize.cx > rcClient.Width() || newSize.cy > rcClient.Height())) {
			CPoint ptNew(m_ptOffset.x + delta.x, m_ptOffset.y + delta.y);
			CPoint ptLimited = Helpers::LimitOffsets(ptNew, rcClient.Size(), newSize);
			if (ptLimited != m_ptOffset) {
				m_ptOffset = ptLimited;
				BeginInteractivePaint();
			}
		}
	}
	return 0;
}

void CViewerWnd::ZoomAt(const CPoint& ptClient, double dFactor) {
	if (m_pImage == NULL) {
		return;
	}
	CRect rcClient;
	GetClientRect(&rcClient);

	double dOldZoom = (m_bFitToWindow) ? m_dRealizedZoom : m_dZoom;
	if (dOldZoom <= 0.0) {
		dOldZoom = 1.0;
	}
	double dNewZoom = dOldZoom * dFactor;
	// Exactly 100% is a state the eye can judge, so both ends of the wheel stop there: snap when the
	// result is within 1% of it, and never skip over it in one notch. Same rule as upstream
	// (PerformZoom's "always try to snap to 100%" and its ZoomPausePercent stop).
	if (fabs(dNewZoom - 1.0) < 0.01) {
		dNewZoom = 1.0;
	} else if ((dOldZoom - 1.0) * (dNewZoom - 1.0) < 0.0) {
		dNewZoom = 1.0;
	}
	// Upstream never lets the virtual image grow past 65535 px in either direction, so the reachable
	// magnification follows the image size: a 1024 px wide photo tops out at 6400%, a 2000 px one at
	// 3276%. A fixed 800% cap was the only reason this viewer felt "shorter" than upstream.
	double dMaxZoom = kMaxZoom;
	if (m_pImage->OrigSize().cx > 0 && m_pImage->OrigSize().cy > 0) {
		dMaxZoom = 65535.0 / (double)max(m_pImage->OrigSize().cx, m_pImage->OrigSize().cy);
	}
	dNewZoom = max(kMinZoom, min(dMaxZoom, dNewZoom));

	// Keep the pixel under the cursor where it is: convert the cursor into image space with the
	// old zoom, then move the offset so that the same image point lands under it again. This is
	// algebraically the same expression upstream uses in PerformZoom(bZoomToMouse = true).
	CPoint ptCenter(rcClient.Width() / 2, rcClient.Height() / 2);
	// Upstream anchors on the clamped offset, because the previous paint already clamped it. Without
	// this, an overshot drag would leave the anchor outside the legal range and the picture would land
	// somewhere upstream never puts it.
	CSize szOld = ComputeVirtualSize(rcClient);
	if (szOld.cx > 0 && szOld.cy > 0) {
		m_ptOffset = Helpers::LimitOffsets(m_ptOffset, rcClient.Size(), szOld);
	}
	double dImgX = (double)(ptClient.x - ptCenter.x - m_ptOffset.x) / dOldZoom;
	double dImgY = (double)(ptClient.y - ptCenter.y - m_ptOffset.y) / dOldZoom;

	m_bFitToWindow = false;
	m_dZoom = dNewZoom;
	m_ptOffset.x = (int)(ptClient.x - ptCenter.x - dImgX * dNewZoom);
	m_ptOffset.y = (int)(ptClient.y - ptCenter.y - dImgY * dNewZoom);

	// Zooming in can also overshoot the new limit; clamp immediately so the drawn frame and the stored
	// state agree instead of waiting for the next paint pass.
	CSize szNew = ComputeVirtualSize(rcClient);
	if (szNew.cx > 0 && szNew.cy > 0) {
		m_ptOffset = Helpers::LimitOffsets(m_ptOffset, rcClient.Size(), szNew);
	}

	m_bZoomLabelVisible = true;
	SetTimer(ZOOM_LABEL_TIMER_ID, kZoomLabelDelayMs, NULL);
	BeginInteractivePaint();
}

double CViewerWnd::WheelZoomMultiplier(const CSize& clientSize) const {
	if (m_pImage == NULL || m_pImage->OrigSize().cx <= 0) {
		return kWheelStep;
	}
	double dFitZoom = -1.0;
	double dZoomToFit = (double)Helpers::GetVirtualImageSize(m_pImage->OrigSize(), clientSize,
		Helpers::ZM_FitToScreenNoZoom, dFitZoom).cx / (double)m_pImage->OrigSize().cx;
	if (dZoomToFit <= 0.0) {
		return kWheelStep;
	}
	// Upstream: the step should feel like 1.1 but must land exactly on 100% after a whole number of
	// notches, so the step is solved from the fit ratio instead of being fixed.
	int n = (int)(log(1.0 / dZoomToFit) / log(1.1) + 0.5);
	return (n == 0) ? 1.1 : exp(log(1.0 / dZoomToFit) / n);
}

LRESULT CViewerWnd::OnMouseWheel(UINT, WPARAM wParam, LPARAM lParam, BOOL& /*bHandled*/) {
	int nDelta = GET_WHEEL_DELTA_WPARAM(wParam);
	if (nDelta == 0) {
		// Synthetic or malformed wheel message: nothing to scale by.
		return 0;
	}
	POINT ptScreen = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
	ScreenToClient(&ptScreen);
	CRect rcClient;
	GetClientRect(&rcClient);
	double dStep = WheelZoomMultiplier(rcClient.Size());
	ZoomAt(CPoint(ptScreen.x, ptScreen.y), nDelta > 0 ? dStep : 1.0 / dStep);
	return 0;
}

void CViewerWnd::ToggleViewMode() {
	m_bFitToWindow = !m_bFitToWindow;
	if (!m_bFitToWindow) {
		m_dZoom = 1.0;
	}
	// Both directions recentre. A pan offset left over while the image fits the window would
	// draw it off centre, which is the artefact this reset exists to prevent.
	m_ptOffset = CPoint(0, 0);
	m_bZoomLabelVisible = true;
	SetTimer(ZOOM_LABEL_TIMER_ID, kZoomLabelDelayMs, NULL);
	BeginInteractivePaint();
}

void CViewerWnd::ToggleBackMode() {
	m_eBackMode = (m_eBackMode == BACK_Alpha) ? BACK_Color : BACK_Alpha;
	Invalidate(FALSE);
}

void CViewerWnd::PickBackgroundColor() {
	CHOOSECOLOR cc;
	ZeroMemory(&cc, sizeof(cc));
	COLORREF acrCust[16];
	for (int i = 0; i < 16; i++) {
		acrCust[i] = RGB(255, 255, 255);
	}
	cc.lStructSize = sizeof(CHOOSECOLOR);
	cc.hwndOwner = m_hWnd;
	cc.lpCustColors = acrCust;
	cc.rgbResult = m_clrConfigBackground;
	cc.Flags = CC_FULLOPEN | CC_RGBINIT;
	if (::ChooseColor(&cc)) {
		m_clrConfigBackground = cc.rgbResult;
		m_clrBackground = m_clrConfigBackground;
		// Switching to the colour background makes the picked colour visible right away; otherwise
		// F1 would look like it did nothing while the window sits in alpha mode.
		m_eBackMode = BACK_Color;
		Invalidate(FALSE);
	}
}

void CViewerWnd::RotateBy(int nDegrees) {
	if (m_pImage == NULL) {
		return;
	}
	if (m_pImage->IsAnimation()) {
		// Spec: rotations are silently ignored for animations, no beep, no error dialog.
		return;
	}
	m_nRotation = (m_nRotation + nDegrees + 360) % 360;
	BeginInteractivePaint();
}

LRESULT CViewerWnd::OnRButtonUp(UINT, WPARAM, LPARAM, BOOL& /*bHandled*/) {
	// No context menu in ImageBrowser. Right click is half of the view toggle.
	ToggleViewMode();
	return 0;
}

LRESULT CViewerWnd::OnKeyDown(UINT, WPARAM wParam, LPARAM, BOOL& bHandled) {
	switch (wParam) {
		case VK_TAB:
			ToggleBackMode();
			break;
		case VK_SPACE:
			ToggleViewMode();
			break;
		case VK_LEFT:
			StepFile(false);
			break;
		case VK_RIGHT:
			StepFile(true);
			break;
		case VK_UP:
			RotateBy(270);
			break;
		case VK_DOWN:
			RotateBy(90);
			break;
		case VK_F1:
			PickBackgroundColor();
			break;
		case VK_ESCAPE:
			PostMessage(WM_CLOSE);
			break;
		case VK_PRIOR:
			StepFile(false);
			break;
		case VK_NEXT:
			StepFile(true);
			break;
		default:
			bHandled = FALSE;
			break;
	}
	return 0;
}

LRESULT CViewerWnd::OnTimer(UINT, WPARAM wParam, LPARAM, BOOL& /*bHandled*/) {
	if ((int)wParam == ANIMATION_TIMER_ID && m_pImage != NULL && m_pImage->IsAnimation()) {
		int nFrames = m_pImage->NumberOfFrames();
		if (nFrames > 1) {
			int nNext = (m_pImage->FrameIndex() + 1) % nFrames;
			RequestFrame(nNext);
			// Upstream paints the new frame before re-arming, so the frame's own display time starts
			// when the pixels are actually on screen. Every frame carries its own delay (GIF per-frame
			// GCE, WebP per-frame duration, APNG fcTL), so the wait is recomputed from that frame.
			UpdateWindow();
			AdjustAnimationTimer();
		}
	}
	if ((int)wParam == HQ_REFINE_TIMER_ID) {
		KillTimer(HQ_REFINE_TIMER_ID);
		m_procFlags = (EProcessingFlags)(m_procFlags | PFLAG_HighQualityResampling);
		// Magnification is point sampled at every quality level, so the idle refine frame would
		// repaint exactly the same pixels. Only shrink-to-fit gains anything from it.
		if (m_pImage != NULL && m_dZoom < 1.0) {
			Invalidate(FALSE);
		}
	}
	if ((int)wParam == ZOOM_LABEL_TIMER_ID) {
		KillTimer(ZOOM_LABEL_TIMER_ID);
		m_bZoomLabelVisible = false;
		Invalidate(FALSE);
	}
	return 0;
}

void CViewerWnd::BeginInteractivePaint() {
	// Two-stage quality, exactly the upstream trade: while the user is zooming or panning the
	// resampling runs with the fast filter so every frame stays cheap, then one idle repaint puts
	// the high quality result on screen. The timer is restarted by every new interaction step.
	m_procFlags = (EProcessingFlags)(m_procFlags & ~PFLAG_HighQualityResampling);
	if (m_pImage != NULL) {
		SetTimer(HQ_REFINE_TIMER_ID, kHQRefineDelayMs, NULL);
	}
	Invalidate(FALSE);
}

LRESULT CViewerWnd::OnDropFiles(UINT, WPARAM wParam, LPARAM, BOOL& /*bHandled*/) {
	HDROP hDrop = (HDROP)wParam;
	if (hDrop != NULL) {
		TCHAR buff[MAX_PATH] = { 0 };
		if (::DragQueryFile(hDrop, 0, buff, MAX_PATH - 1) > 0) {
			::DragFinish(hDrop);
			GoToPath(buff);
			return 0;
		}
		::DragFinish(hDrop);
	}
	return 0;
}

LRESULT CViewerWnd::OnImageLoadCompleted(UINT, WPARAM wParam, LPARAM, BOOL& /*bHandled*/) {
	if (m_pProvider != NULL) {
		m_pProvider->OnImageLoadCompleted((int)wParam);
	}
	return 0;
}

LRESULT CViewerWnd::OnClose(UINT, WPARAM, LPARAM, BOOL& /*bHandled*/) {
	SaveConfig();
	StopAnimationTimer();
	DestroyWindow();
	::PostQuitMessage(0);
	return 0;
}

/////////////////////////////////////////////////////////////////////////////
// Image pipeline
/////////////////////////////////////////////////////////////////////////////

CProcessParams CViewerWnd::MakeProcessParams() const {
	CRect rcClient(0, 0, kDefaultWidth, kDefaultHeight);
	if ((HWND)*this != NULL) {
		::GetClientRect(m_hWnd, &rcClient);
	}
	double dZoom = m_bFitToWindow ? -1.0 : m_dZoom;
	return CProcessParams(max(1, rcClient.Width()), max(1, rcClient.Height()),
		CSize(::GetSystemMetrics(SM_CXSCREEN), ::GetSystemMetrics(SM_CYSCREEN)),
		CRotationParams(0),
		m_nRotation,
		dZoom,
		Helpers::ZM_FitToScreen,
		m_ptOffset,
		m_procParams,
		m_procFlags);
}

void CViewerWnd::ReleaseImage() {
	if (m_pProvider != NULL && m_pImage != NULL) {
		m_pProvider->NotifyNotUsed(m_pImage);
	}
	m_pImage = NULL;
}

void CViewerWnd::UpdateTitle() {
	CString sTitle;
	if (m_pFileList != NULL && m_pFileList->CurrentFileTitle() != NULL) {
		sTitle = m_pFileList->CurrentFileTitle();
		sTitle += _T(" - ");
	}
	sTitle += _T(ImageBrowser_TITLE);
	SetWindowText(sTitle);
}

void CViewerWnd::RequestCurrentImage() {
	if (m_pProvider == NULL || m_pFileList == NULL || m_pFileList->Current() == NULL) {
		return;
	}
	bool bOutOfMemory = false, bException = false;
	CJPEGImage* pImage = m_pProvider->RequestImage(m_pFileList, CJPEGProvider::FORWARD,
		m_pFileList->Current(), 0, MakeProcessParams(), bOutOfMemory, bException);
	m_pImage = pImage;
	m_bLoadError = (pImage == NULL);
	UpdateTitle();

	StopAnimationTimer();
	if (m_pImage != NULL) {
		StartAnimationTimer();
	}
	Invalidate(FALSE);
}

void CViewerWnd::RequestFrame(int nFrameIndex) {
	if (m_pProvider == NULL || m_pFileList == NULL || m_pFileList->Current() == NULL) {
		return;
	}
	bool bOutOfMemory = false, bException = false;
	CJPEGImage* pImage = m_pProvider->RequestImage(m_pFileList, CJPEGProvider::NONE,
		m_pFileList->Current(), nFrameIndex, MakeProcessParams(), bOutOfMemory, bException);
	if (pImage != NULL) {
		m_pImage = pImage;
		Invalidate(FALSE);
	}
}

void CViewerWnd::StartAnimationTimer() {
	if (m_pImage == NULL || !m_pImage->IsAnimation() || m_pImage->NumberOfFrames() <= 1) {
		StopAnimationTimer();
		return;
	}
	int nFrameTime = max(10, m_pImage->FrameTimeMs());
	SetTimer(ANIMATION_TIMER_ID, (UINT)nFrameTime, NULL);
	m_nAnimationOffset = 0;
	m_nAnimationExpectedTick = ::GetTickCount() + nFrameTime;
}

void CViewerWnd::AdjustAnimationTimer() {
	// Scheduling model copied from upstream: the next frame is due at an absolute tick, and whatever
	// the timer system was late by is subtracted from the following wait (never below 10 ms), so the
	// 15.6 ms WM_TIMER quantisation cannot accumulate into a permanent slowdown.
	if (m_pImage == NULL || !m_pImage->IsAnimation() || m_pImage->NumberOfFrames() <= 1) {
		StopAnimationTimer();
		return;
	}
	KillTimer(ANIMATION_TIMER_ID);
	m_nAnimationOffset += (int)(::GetTickCount() - m_nAnimationExpectedTick);
	m_nAnimationOffset = min(m_nAnimationOffset, max(100, m_pImage->FrameTimeMs()));
	int nFrameTime = max(10, m_pImage->FrameTimeMs() - max(0, m_nAnimationOffset));
	m_nAnimationExpectedTick = ::GetTickCount() + max(10, m_pImage->FrameTimeMs());
	SetTimer(ANIMATION_TIMER_ID, (UINT)nFrameTime, NULL);
}

void CViewerWnd::StopAnimationTimer() {
	KillTimer(ANIMATION_TIMER_ID);
	m_nAnimationOffset = 0;
	m_nAnimationExpectedTick = 0;
}

void CViewerWnd::GoToPath(LPCTSTR sPath) {
	if (sPath == NULL || *sPath == 0) {
		return;
	}

	// Format gate: same rule as the command line and the open dialog.
	DWORD dwAttrs = ::GetFileAttributes(sPath);
	bool bIsDirectory = (dwAttrs != INVALID_FILE_ATTRIBUTES) && (dwAttrs & FILE_ATTRIBUTE_DIRECTORY);
	if (!bIsDirectory && !CFileList::IsSupportedFile(sPath)) {
		// No message box: the window itself states that the file cannot be read.
		ReleaseImage();
		StopAnimationTimer();
		SetWindowText(CString(_T(ImageBrowser_TITLE)));
		m_bLoadError = true;
		Invalidate(FALSE);
		return;
	}
	if (dwAttrs == INVALID_FILE_ATTRIBUTES) {
		return;
	}

	ReleaseImage();
	StopAnimationTimer();
	delete m_pFileList;
	m_pFileList = new CFileList(CString(sPath), *m_pWatcher, Helpers::FS_FileName, true, false);
	if (m_pFileList == NULL || m_pFileList->Size() == 0) {
		return;
	}

	// New document: view state goes back to defaults (fit, no zoom, no pan, no rotation).
	m_bFitToWindow = true;
	m_dZoom = 1.0;
	m_ptOffset = CPoint(0, 0);
	m_nRotation = 0;

	RequestCurrentImage();
}

void CViewerWnd::StepFile(bool bForward) {
	if (m_pFileList == NULL || m_pFileList->Size() == 0) {
		return;
	}
	// No wrap-around: the spec says arrows walk the current folder, so we stop at the edges.
	int nIndex = m_pFileList->CurrentIndex();
	if (bForward && nIndex >= m_pFileList->Size() - 1) {
		return;
	}
	if (!bForward && nIndex <= 0) {
		return;
	}

	ReleaseImage();
	StopAnimationTimer();
	if (bForward) {
		m_pFileList->Next();
	} else {
		m_pFileList->Prev();
	}

	m_bFitToWindow = true;
	m_dZoom = 1.0;
	m_ptOffset = CPoint(0, 0);
	m_nRotation = 0;
	RequestCurrentImage();
}

void CViewerWnd::OpenPath(LPCTSTR sPath) {
	// Starting with no argument opens an empty window, not a file dialog: ImageBrowser has no
	// "open" command at all. A file arrives either as the command line argument (file manager
	// association) or by being dropped onto the window.
	if (sPath != NULL && *sPath != 0) {
		GoToPath(sPath);
	}
}









