#pragma once
#include <windows.h>

// 拾色器：返回 true 时 out 为选中的颜色（COLORREF，即 0x00BBGGRR）。
// 纯自绘 Win32 + GDI+，不依赖任何第三方 DLL，也不需要 comdlg32 的 CHOOSECOLOR。
bool PickBackgroundColor(HWND owner, COLORREF current, COLORREF* out);
