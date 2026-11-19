#pragma once

#include "stdafx.h"
#include "ProcessParams.h"
#include "MessageDef.h"
#include <atlwin.h>

class CJPEGImage;
class CJPEGProvider;
class CFileList;
class CDirectoryWatcher;

// The whole UI of ImageBrowser.
//
// This window replaces upstream's CMainDlg entirely. It has no menus, no toolbars, no panels,
// no editor, no slideshow: it only shows one image at a time and offers the seven gestures
// listed in 项目信息.md. Everything else was deleted, not disabled.
//
// Rendering goes through CJPEGImage::GetDIB(), which does the high quality resampling and only
// renders the visible rectangle, so a 100 MP image never allocates 100 MP of DIB.
class CViewerWnd : public CWindowImpl<CViewerWnd>
{
public:
	CViewerWnd();
	~CViewerWnd();

	DECLARE_WND_CLASS_EX(_T("ImageBrowserWnd"), 0, -1)

	// Creates the window, restores the maximized state from ImageBrowser.ini and shows it.
	// Returns false if window creation failed.
	bool CreateMain();

	// Blocks in the message loop until the window is closed.
	void RunMessageLoop();

	// Opens the given path (file or folder). Empty string means "show the open dialog".
	void OpenPath(LPCTSTR sPath);

	BEGIN_MSG_MAP(CViewerWnd)
		MESSAGE_HANDLER(WM_CREATE, OnCreate)
		MESSAGE_HANDLER(WM_PAINT, OnPaint)
		MESSAGE_HANDLER(WM_ERASEBKGND, OnEraseBkgnd)
		MESSAGE_HANDLER(WM_SIZE, OnSize)
		MESSAGE_HANDLER(WM_GETMINMAXINFO, OnGetMinMaxInfo)
		MESSAGE_HANDLER(WM_LBUTTONDOWN, OnLButtonDown)
		MESSAGE_HANDLER(WM_LBUTTONUP, OnLButtonUp)
		MESSAGE_HANDLER(WM_MOUSEMOVE, OnMouseMove)
		MESSAGE_HANDLER(WM_MOUSEWHEEL, OnMouseWheel)
		MESSAGE_HANDLER(WM_RBUTTONUP, OnRButtonUp)
		MESSAGE_HANDLER(WM_KEYDOWN, OnKeyDown)
		MESSAGE_HANDLER(WM_TIMER, OnTimer)
		MESSAGE_HANDLER(WM_DROPFILES, OnDropFiles)
		MESSAGE_HANDLER(WM_IMAGE_LOAD_COMPLETED, OnImageLoadCompleted)
		MESSAGE_HANDLER(WM_CLOSE, OnClose)
	END_MSG_MAP()

private:
	// What fills the area around the image. Tab cycles between the two.
	enum EBackMode {
		BACK_Alpha,   // checkerboard, image alpha is honoured
		BACK_Color    // solid colour taken from ImageBrowser.ini
	};

	LRESULT OnCreate(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
	LRESULT OnPaint(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
	LRESULT OnEraseBkgnd(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
	LRESULT OnSize(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
	LRESULT OnGetMinMaxInfo(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
	LRESULT OnLButtonDown(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
	LRESULT OnLButtonUp(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
	LRESULT OnMouseMove(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
	LRESULT OnMouseWheel(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
	LRESULT OnRButtonUp(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
	LRESULT OnKeyDown(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
	LRESULT OnTimer(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
	LRESULT OnDropFiles(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
	LRESULT OnImageLoadCompleted(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
	LRESULT OnClose(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);

	// --- image pipeline ---------------------------------------------------
	void UpdateTitle();
	void RequestCurrentImage();
	void RequestFrame(int nFrameIndex);
	void ReleaseImage();
	CProcessParams MakeProcessParams() const;

	// geometry helpers
	CSize ComputeVirtualSize(const CRect& clientRect) const;

	// --- painting ---------------------------------------------------------
	void EnsureBackBuffer(int nWidth, int nHeight);
	void FillBackground(uint32* pBits, int nStride, int nWidth, int nHeight);
	void FillArea(uint32* pBits, int nStride, int nLeft, int nTop, int nWidth, int nHeight);
	bool DrawImage(uint32* pBackBits, int nBackStride, const CRect& clientRect);
	void DrawZoomLabel(const CRect& clientRect);
	void DrawLoadError(const CRect& clientRect);

	// --- state transitions -------------------------------------------------
	void ToggleViewMode();
	void ToggleBackMode();
	void PickBackgroundColor();
	void RotateBy(int nDegrees);
	void ZoomAt(const CPoint& ptClient, double dFactor);
	double WheelZoomMultiplier(const CSize& clientSize) const;
	void BeginInteractivePaint();

	// --- file navigation ---------------------------------------------------
	void StepFile(bool bForward);
	void GoToPath(LPCTSTR sPath);

	// --- config -----------------------------------------------------------
	void LoadConfig();
	void SaveConfig() const;
	CString ConfigPath() const;

	CJPEGProvider* m_pProvider;
	CFileList* m_pFileList;
	CDirectoryWatcher* m_pWatcher;
	CJPEGImage* m_pImage;

	// View state. AutoZoom means "fit to window"; 1.0 means "actual pixels".
	bool m_bFitToWindow;
	double m_dZoom;          // valid when !m_bFitToWindow
	double m_dRealizedZoom;  // what was actually painted last time, used as the wheel base
	int m_nRotation;         // 0 / 90 / 180 / 270, preview only, reset when the file changes
	CPoint m_ptOffset;       // pan offset in screen pixels, relative to centred position

	bool m_bDragging;
	CPoint m_ptDragOrigin;

	EBackMode m_eBackMode;
	COLORREF m_clrBackground;

	// Back buffer: a 32bpp DIB section we own, so a frame is painted with plain pixel writes and
	// a single BitBlt. Recreated only when the client size changes.
	HDC m_hdcBack;
	HBITMAP m_hbmBack;
	HBITMAP m_hbmBackOld;
	uint32* m_pBackBits;
	int m_nBackWidth;
	int m_nBackHeight;

	CImageProcessingParams m_procParams;
	EProcessingFlags m_procFlags;

	// Zoom percentage label, same place upstream puts it (top right, fades after a second).
	HFONT m_hZoomFont;
	bool m_bZoomLabelVisible;

	// Set when a file could not be opened at all (unknown format or a failed decode). The window then
	// says so in place instead of throwing a message box at the user.
	bool m_bLoadError;
	HFONT m_hErrorFont;

	// Caption / taskbar / Alt+Tab icons, taken from the embedded IDR_MAINFRAME resource.
	HICON m_hIconBig;
	HICON m_hIconSmall;

	bool m_bConfigMaximized;
	COLORREF m_clrConfigBackground;

	bool m_bStartupDone;
};


