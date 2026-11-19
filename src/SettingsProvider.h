#pragma once

#include "stdafx.h"
#include "Helpers.h"
#include "ImageProcessingTypes.h"

// ImageBrowser has exactly two settings, and they live in ImageBrowser.ini, owned by ViewerWnd.
//
// Everything else upstream let you tune in a 32 KB INI file is frozen here at the value
// upstream used when the corresponding key was absent. This class keeps its old name so the
// decoding code that asks "how should I resample?" does not need to change, but there is no INI
// parsing, no per-file parameter database and no AppData folder any more.
class CSettingsProvider
{
public:
	static CSettingsProvider& This() {
		static CSettingsProvider s_instance;
		return s_instance;
	}

	// Resampling quality and CPU dispatch: the upstream defaults.
	Helpers::CPUType AlgorithmImplementation() {
		static Helpers::CPUType s_cpu = Helpers::ProbeCPU();
		return s_cpu;
	}
	int NumberOfCoresToUse() { return 0; }   // 0: the thread pool picks
	EFilterType DownsamplingFilter() { return Filter_Downsampling_Best_Quality; }
	int DisplayMonitor() { return -1; }

	// Colour correction is capped at the upstream defaults and only applied if the caller sets
	// PFLAG_ColorCorrection, which the viewer never does.
	float* ColorCorrectionAmounts() {
		static float s_factor[6] = { 0.2f, 0.1f, 0.3f, 0.3f, 0.3f, 0.15f };
		return s_factor;
	}

	// Black is what upstream used as its default background and transparency colour. Transparent
	// pixels end up on ViewerWnd's own checkerboard, so this is only a decoder-internal fallback.
	COLORREF ColorBackground() { return RGB(0, 0, 0); }
	COLORREF ColorTransparency() { return RGB(0, 0, 0); }

	// No GDI+ fallback for the five formats we decode ourselves, and no embedded ICC profiles:
	// colour management was deliberately dropped along with ICCProfileTransform.
	bool ForceGDIPlus() { return false; }
	bool UseEmbeddedColorProfiles() { return false; }

	// Photos from phones and cameras are displayed upright, as every viewer does.
	bool AutoRotateEXIF() { return true; }

	bool AllowFileDeletion() { return true; }

	// No WIC, no RAW: both were removed from the build.
	LPCTSTR FilesProcessedByWIC() { return _T(""); }
	LPCTSTR FileEndingsRAW() { return _T(""); }

	// Slideshow is gone; the value only exists because the file list code still asks for it.
	int MaxSlideShowFileListSize() { return 1; }

	// Per-folder auto-contrast and local density correction used to be selectable. Empty patterns
	// mean Helpers::PatternMatch never matches, so a file never gets those flags.
	LPCTSTR ACCInclude() { return _T(""); }
	LPCTSTR ACCExclude() { return _T(""); }
	LPCTSTR LDCInclude() { return _T(""); }
	LPCTSTR LDCExclude() { return _T(""); }

	// Folder the executable lives in.
	LPCTSTR GetEXEPath() {
		if (m_sEXEPath.IsEmpty()) {
			TCHAR path[MAX_PATH];
			::GetModuleFileName(NULL, path, MAX_PATH);
			CString sPath(path);
			int nPos = sPath.ReverseFind(_T('\\'));
			m_sEXEPath = (nPos >= 0) ? sPath.Left(nPos + 1) : CString(_T(".\\"));
		}
		return m_sEXEPath;
	}

private:
	CSettingsProvider() {}
	CSettingsProvider(const CSettingsProvider&);
	CSettingsProvider& operator=(const CSettingsProvider&);

	CString m_sEXEPath;
};
