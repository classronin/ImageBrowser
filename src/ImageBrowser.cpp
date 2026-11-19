// ImageBrowser.cpp : entry point for ImageBrowser - ClassRonin
//
// This file used to be the upstream launcher: it parsed /slideshow, /fullscreen, /order and friends,
// hunted down a second instance and forwarded the path over WM_COPYDATA, then opened the big
// CMainDlg. None of that survives. ImageBrowser takes one optional path argument and nothing else.

#include "stdafx.h"
#include <locale.h>
#include <gdiplus.h>
#include "ViewerWnd.h"

CAppModule _Module;

// Distinct from the upstream mutex on purpose: ImageBrowser may run alongside the original.
static CString ParseCommandLineForStartupFile(LPCTSTR sCommandLine) {
	CString sStartupFile = sCommandLine;
	if (sStartupFile.GetLength() == 0) {
		return CString(_T(""));
	}
	if (sCommandLine[0] == _T('/') || sCommandLine[0] == _T('-')) {
		// No command line switches exist any more; treat anything switch-like as "no file".
		return CString(_T(""));
	}

	// The shell quotes paths that contain spaces; take the first quoted run, otherwise the
	// first whitespace-delimited token.
	int nQuote = sStartupFile.Find(_T('"'));
	if (nQuote == -1) {
		int nSpace = sStartupFile.Find(_T(' '));
		if (nSpace > 0) {
			sStartupFile = sStartupFile.Left(nSpace);
		}
	} else {
		int nQuoteNext = sStartupFile.Find(_T('"'), nQuote + 1);
		if (nQuoteNext > nQuote) {
			sStartupFile = sStartupFile.Mid(nQuote + 1, nQuoteNext - nQuote - 1);
		}
	}

	if (sStartupFile.GetLength() > 0 && sStartupFile.Find(_T('\\')) == -1) {
		return _T(".\\") + sStartupFile;
	}
	return sStartupFile;
}

int WINAPI _tWinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/, LPTSTR lpstrCmdLine, int /*nCmdShow*/)
{
	HRESULT hRes = ::CoInitialize(NULL);

	// Numbers always read and written in the C locale, so the hand-written INI parsing cannot trip
	// over a regional decimal separator.
	_tsetlocale(LC_ALL, _T(""));
	_tsetlocale(LC_NUMERIC, _T("C"));

	srand(::GetTickCount());

	// Resolves the ATL window thunking problem when the Microsoft Layer for Unicode is used.
	::DefWindowProc(NULL, 0, 0, 0L);

	AtlInitCommonControls(ICC_BAR_CLASSES);

	hRes = _Module.Init(NULL, hInstance);
	ATLASSERT(SUCCEEDED(hRes));

	// GIF decoding still rides on GDI+ (upstream never ported it), so GDI+ is not optional.
	Gdiplus::GdiplusStartupInput gdiplusStartupInput;
	ULONG_PTR gdiplusToken = 0;
	Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

	CString sStartupFile = ParseCommandLineForStartupFile(lpstrCmdLine);

	CViewerWnd wnd;
	if (wnd.CreateMain()) {
		wnd.OpenPath(sStartupFile);  // empty path means: show the open dialog
		wnd.RunMessageLoop();
	}

	Gdiplus::GdiplusShutdown(gdiplusToken);

	_Module.Term();
	::CoUninitialize();

	return 0;
}
