// 精简构建：JXL / HEIF / AVIF / RAW 四种格式已被移除（不链接其解码库）。
// 这里提供同名空实现，保证 JPEGView 内核的格式分派代码可以无条件编译；
// 这些格式在运行时会正常失败并给出“无法解码”，不会静默出错。

#include "stdafx.h"

#include "JXLWrapper.h"
#include "HEIFWrapper.h"
#include "AVIFWrapper.h"
#include "RAWWrapper.h"
#include "dcraw_mod.h"

// WIC 兜底路径（原来由 WICLoader.dll 提供）在本项目中关闭：
// 精简格式集里没有需要 WIC 的类型，GIF 走 GDI+，所以这两个入口返回失败即可。
typedef unsigned char* WicAllocator(int sizeInBytes);
typedef void WicDeallocator(unsigned char* buffer);

bool __stdcall WICPresent(void) { return false; }

unsigned char* __stdcall LoadImageWithWIC(LPCWSTR, WicAllocator*, WicDeallocator*,
                                          unsigned int* width, unsigned int* height) {
    if (width) *width = 0;
    if (height) *height = 0;
    return nullptr;
}

CJPEGImage* CReaderRAW::ReadRawImage(LPCTSTR, bool& bOutOfMemory) {
    bOutOfMemory = false;
    return nullptr;
}

void* JxlReader::ReadImage(int& width, int& height, int& bpp, bool& has_animation,
                           int& frame_count, int& frame_time, void*& exif,
                           bool& outOfMemory, const void*, int) {
    width = height = 0;
    bpp = 4;
    has_animation = false;
    frame_count = 1;
    frame_time = 0;
    exif = nullptr;
    outOfMemory = false;
    return nullptr;
}

void JxlReader::DeleteCache() {}

void* HeifReader::ReadImage(int& width, int& height, int& bpp, int& frame_count,
                            void*& exif_chunk, bool& outOfMemory, int, const void*, int) {
    width = height = 0;
    bpp = 4;
    frame_count = 1;
    exif_chunk = nullptr;
    outOfMemory = false;
    return nullptr;
}

void* AvifReader::ReadImage(int& width, int& height, int& bpp, bool& has_animation,
                            int, int& frame_count, int& frame_time, void*& exif_chunk,
                            bool& outOfMemory, const void*, int) {
    width = height = 0;
    bpp = 4;
    has_animation = false;
    frame_count = 1;
    frame_time = 0;
    exif_chunk = nullptr;
    outOfMemory = false;
    return nullptr;
}

void AvifReader::DeleteCache() {}

CJPEGImage* RawReader::ReadImage(LPCTSTR, bool& bOutOfMemory, bool) {
    bOutOfMemory = false;
    return nullptr;
}
