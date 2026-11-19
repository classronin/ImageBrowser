#include "StdAfx.h"
#include "NLS.h"

// The translation table machinery is gone, together with the thirty strings_*.txt files it used
// to read. What is left is the one helper that builds a language-suffixed file name.

CString CNLS::GetLocalizedFileName(LPCTSTR sPath, LPCTSTR sPrefixName, LPCTSTR sExtension, LPCTSTR sLanguageCode) {
	CString sNLSFile;
	TCHAR buff[16], buff2[16];
	if (_tcscmp(sLanguageCode, _T("auto")) == 0) {
		LCID threadLocale = ::GetThreadLocale();
		::GetLocaleInfo(threadLocale, LOCALE_SISO639LANGNAME, (LPTSTR)&buff, sizeof(buff));
		::GetLocaleInfo(threadLocale, LOCALE_SISO3166CTRYNAME, (LPTSTR)&buff2, sizeof(buff2));
		sLanguageCode = buff;
		sNLSFile = CString(sPath) + sPrefixName + _T("_") + sLanguageCode + _T("-") + buff2 + _T(".") + sExtension;
		if (::GetFileAttributes(sNLSFile) == INVALID_FILE_ATTRIBUTES) {
			sNLSFile.Empty();
		}
	}
	if (sNLSFile.IsEmpty()) {
		sNLSFile = CString(sPath) + sPrefixName + _T("_") + sLanguageCode + _T(".") + sExtension;
	}
	return sNLSFile;
}
