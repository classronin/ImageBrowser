#pragma once

#include "stdafx.h"

class CJPEGImage;

// ORA（OpenRaster）：本质是 ZIP，内含 stack.xml + 若干图层 PNG + mergedimage.png。
// 本项目只读不写：把编辑器写好的 mergedimage.png（整张画布的合并结果）当作"图像"显示，
// alpha 原样保留，与 PNG 同一条外壳合成路径。图层合并不在这里做（那是编辑器的事）。
namespace CReaderORA {
	CJPEGImage* ReadImage(LPCTSTR sFileName, bool& bOutOfMemory);

	// 把内存里的 PNG 解成 32bpp BGRA（alpha 保留）。ICO 的 PNG 载荷也走这里。
	uint32* DecodePngMemory(const uint8_t* pData, size_t nSize, int& nWidth, int& nHeight);
}
