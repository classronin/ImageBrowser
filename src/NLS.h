#pragma once

// ImageBrowser is English only: no string tables, no language files, no localisation.
//
// CNLS remains as the single place the rest of the code fetches UI text from, so the decoder and
// helpers keep their call sites untouched, but every lookup simply returns the English original
// that is written in the source.
class CNLS
{
public:
	// Resolves a language-suffixed file name in the EXE folder, e.g. "readme_ru.html".
	// Kept because the settings code still asks for a template name.
	static CString GetLocalizedFileName(LPCTSTR sPath, LPCTSTR sPrefixName, LPCTSTR sExtension, LPCTSTR sLanguageCode);

	// No-op: there are no string tables to read.
	static void ReadStringTable(LPCTSTR /*sFileName*/) {}

	// Returns the English string unchanged.
	static LPCTSTR GetString(LPCTSTR sString) { return sString; }
};
