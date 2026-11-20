#include "stdafx.h"
#include "ColorPicker.h"
#include <gdiplus.h>
#include <cmath>

using namespace Gdiplus;

namespace {

const int kWinW = 460;
const int kWinH = 332;
const int kSvX = 16, kSvY = 16, kSvW = 320, kSvH = 220;
const int kHueX = 352, kHueY = 16, kHueW = 26, kHueH = 220;
const int kPrevX = 16, kPrevY = 250, kPrevW = 92, kPrevH = 32;
const int kOkX = 356, kOkY = 248, kOkW = 84, kOkH = 30;
const int kCancelX = 264, kCancelY = 248, kCancelW = 84, kCancelH = 30;

struct PickerState {
    HWND hwnd = nullptr;
    double h = 0.0;   // 0..360
    double s = 0.0;   // 0..1
    double v = 0.0;   // 0..1
    int drag = 0;     // 1 = SV, 2 = 色相条
    bool ok = false;
};

void HsvToRgb(double h, double s, double v, int& r, int& g, int& b) {
    if (s < 0) s = 0;
    if (s > 1) s = 1;
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    double c = v * s;
    double hp = h / 60.0;
    double x = c * (1 - std::fabs(std::fmod(hp, 2.0) - 1));
    double m = v - c;
    double r1 = 0, g1 = 0, b1 = 0;
    int seg = (int)std::floor(hp) % 6;
    switch (seg) {
        case 0: r1 = c; g1 = x; b1 = 0; break;
        case 1: r1 = x; g1 = c; b1 = 0; break;
        case 2: r1 = 0; g1 = c; b1 = x; break;
        case 3: r1 = 0; g1 = x; b1 = c; break;
        case 4: r1 = x; g1 = 0; b1 = c; break;
        default: r1 = c; g1 = 0; b1 = x; break;
    }
    r = (int)((r1 + m) * 255.0 + 0.5);
    g = (int)((g1 + m) * 255.0 + 0.5);
    b = (int)((b1 + m) * 255.0 + 0.5);
}

void RgbToHsv(int r, int g, int b, double& h, double& s, double& v) {
    double rd = r / 255.0, gd = g / 255.0, bd = b / 255.0;
    double mx = max(rd, max(gd, bd)), mn = min(rd, min(gd, bd));
    double d = mx - mn;
    v = mx;
    s = (mx <= 0.0) ? 0.0 : d / mx;
    if (d <= 0.0) {
        h = 0.0;
    } else if (mx == rd) {
        h = 60.0 * std::fmod((gd - bd) / d, 6.0);
    } else if (mx == gd) {
        h = 60.0 * ((bd - rd) / d + 2.0);
    } else {
        h = 60.0 * ((rd - gd) / d + 4.0);
    }
    if (h < 0) h += 360.0;
}

void DrawPicker(PickerState* st) {
    HDC hdcWin = GetDC(st->hwnd);
    RECT rc;
    GetClientRect(st->hwnd, &rc);
    Bitmap buffer(rc.right, rc.bottom, PixelFormat32bppPARGB);
    Graphics g(&buffer);
    g.SetSmoothingMode(SmoothingModeAntiAlias);

    SolidBrush bg(Color(255, 245, 245, 245));
    g.FillRectangle(&bg, 0, 0, rc.right, rc.bottom);

    // SV 方块：底色 = 当前色相的纯色，叠白（横向）与黑（纵向）
    int hc = (int)st->h % 360;
    if (hc < 0) hc += 360;
    int r, gr, b;
    HsvToRgb((double)hc, 1.0, 1.0, r, gr, b);
    SolidBrush base(Color(255, r, gr, b));
    g.FillRectangle(&base, kSvX, kSvY, kSvW, kSvH);
    {
        LinearGradientBrush whiteG(Point(kSvX, kSvY), Point(kSvX + kSvW, kSvY),
                                   Color(255, 255, 255, 255), Color(0, 255, 255, 255));
        g.FillRectangle(&whiteG, kSvX, kSvY, kSvW, kSvH);
        LinearGradientBrush blackG(Point(kSvX, kSvY), Point(kSvX, kSvY + kSvH),
                                   Color(0, 0, 0, 0), Color(255, 0, 0, 0));
        g.FillRectangle(&blackG, kSvX, kSvY, kSvW, kSvH);
    }
    Pen frame(Color(255, 180, 180, 180));
    g.DrawRectangle(&frame, kSvX - 1, kSvY - 1, kSvW + 1, kSvH + 1);

    // 色相条（六段线性渐变，避免依赖 ColorBlend）
    {
        static const Color stops[7] = {
            Color(255, 255, 0, 0), Color(255, 255, 255, 0), Color(255, 0, 255, 0),
            Color(255, 0, 255, 255), Color(255, 0, 0, 255), Color(255, 255, 0, 255),
            Color(255, 255, 0, 0)
        };
        for (int i = 0; i < 6; ++i) {
            REAL y0 = (REAL)(kHueY + kHueH * i / 6.0);
            REAL y1 = (REAL)(kHueY + kHueH * (i + 1) / 6.0);
            LinearGradientBrush seg(PointF((REAL)kHueX, y0), PointF((REAL)kHueX, y1),
                                    stops[i], stops[i + 1]);
            g.FillRectangle(&seg, (REAL)kHueX, y0, (REAL)kHueW, y1 - y0);
        }
        g.DrawRectangle(&frame, kHueX - 1, kHueY - 1, kHueW + 1, kHueH + 1);
    }

    // 光标
    {
        GraphicsPath path;
        int px = kSvX + (int)(st->s * kSvW);
        int py = kSvY + (int)((1.0 - st->v) * kSvH);
        path.AddEllipse(px - 7, py - 7, 14, 14);
        Pen p(Color(255, 255, 255, 255), 2.0f);
        g.DrawPath(&p, &path);
        Pen p2(Color(255, 30, 30, 30));
        GraphicsPath in;
        in.AddEllipse(px - 4, py - 4, 8, 8);
        g.DrawPath(&p2, &in);
    }
    {
        int hy = kHueY + (int)((st->h / 360.0) * kHueH);
        GraphicsPath path;
        path.AddRectangle(Rect(kHueX, hy - 2, kHueW, 4));
        SolidBrush wb(Color(255, 255, 255, 255));
        g.FillPath(&wb, &path);
        Pen p(Color(255, 30, 30, 30));
        g.DrawPath(&p, &path);
    }

    // 预览与读数
    int r2, g2, b2;
    HsvToRgb(st->h, st->s, st->v, r2, g2, b2);
    SolidBrush prev(Color(255, r2, g2, b2));
    g.FillRectangle(&prev, kPrevX, kPrevY, kPrevW, kPrevH);
    g.DrawRectangle(&frame, kPrevX - 1, kPrevY - 1, kPrevW + 1, kPrevH + 1);

    Font font(L"Consolas", 13);
    Font fontUi(L"Microsoft YaHei UI", 12);
    SolidBrush text(Color(255, 40, 40, 40));
    wchar_t buf[64];
    swprintf_s(buf, L"#%02X%02X%02X", r2, g2, b2);
    g.DrawString(buf, -1, &font, PointF(124.0f, 256.0f), &text);
    swprintf_s(buf, L"R%d G%d B%d", r2, g2, b2);
    g.DrawString(buf, -1, &fontUi, PointF(128.0f, 276.0f), &text);

    // 按钮
    auto drawButton = [&](int x, int y, int w, int h, const wchar_t* label) {
        SolidBrush fill(Color(255, 236, 236, 236));
        g.FillRectangle(&fill, x, y, w, h);
        Pen line(Color(255, 175, 175, 175));
        g.DrawRectangle(&line, x, y, w - 1, h - 1);
        StringFormat sf;
        sf.SetAlignment(StringAlignmentCenter);
        sf.SetLineAlignment(StringAlignmentCenter);
        g.DrawString(label, -1, &fontUi, RectF((REAL)x, (REAL)y, (REAL)w, (REAL)h), &sf, &text);
    };
    drawButton(kOkX, kOkY, kOkW, kOkH, L"确定");
    drawButton(kCancelX, kCancelY, kCancelW, kCancelH, L"取消");

    Graphics screen(hdcWin);
    screen.DrawImage(&buffer, 0, 0);
    ReleaseDC(st->hwnd, hdcWin);
}

inline bool InRect(int x, int y, int rx, int ry, int rw, int rh) {
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

void UpdateFromMouse(PickerState* st, int x, int y) {
    if (st->drag == 1) {
        double s = (x - kSvX) / (double)kSvW;
        double v = 1.0 - (y - kSvY) / (double)kSvH;
        st->s = min(1.0, max(0.0, s));
        st->v = min(1.0, max(0.0, v));
        InvalidateRect(st->hwnd, nullptr, FALSE);
    } else if (st->drag == 2) {
        double h = (y - kHueY) / (double)kHueH * 360.0;
        st->h = min(359.9, max(0.0, h));
        InvalidateRect(st->hwnd, nullptr, FALSE);
    }
}

LRESULT CALLBACK PickerProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    PickerState* st = (PickerState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_NCCREATE) {
        st = (PickerState*)((CREATESTRUCTW*)lp)->lpCreateParams;
        st->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);
    }
    if (!st) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            DrawPicker(st);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_LBUTTONDOWN: {
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            if (InRect(x, y, kSvX, kSvY, kSvW, kSvH)) {
                st->drag = 1;
                SetCapture(hwnd);
                UpdateFromMouse(st, x, y);
            } else if (InRect(x, y, kHueX, kHueY, kHueW, kHueH)) {
                st->drag = 2;
                SetCapture(hwnd);
                UpdateFromMouse(st, x, y);
            } else if (InRect(x, y, kOkX, kOkY, kOkW, kOkH)) {
                st->ok = true;
                DestroyWindow(hwnd);
            } else if (InRect(x, y, kCancelX, kCancelY, kCancelW, kCancelH)) {
                DestroyWindow(hwnd);
            }
            return 0;
        }
        case WM_MOUSEMOVE:
            if (st->drag) UpdateFromMouse(st, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;
        case WM_LBUTTONUP:
            if (st->drag) {
                st->drag = 0;
                ReleaseCapture();
            }
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) DestroyWindow(hwnd);
            else if (wp == VK_RETURN) { st->ok = true; DestroyWindow(hwnd); }
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

bool PickBackgroundColor(HWND owner, COLORREF current, COLORREF* out) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &PickerProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"ImageBrowserColorPicker";
        if (!RegisterClassExW(&wc)) return false;
        registered = true;
    }

    PickerState st;
    RgbToHsv(GetRValue(current), GetGValue(current), GetBValue(current), st.h, st.s, st.v);

    RECT ownerRect{};
    int ox = 200, oy = 200;
    // 客户区按 kWinW/kWinH 设计，窗口尺寸要含标题栏
    RECT winRect{0, 0, kWinW, kWinH};
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    DWORD exStyle = WS_EX_DLGMODALFRAME | WS_EX_TOPMOST;
    AdjustWindowRectEx(&winRect, style, FALSE, exStyle);
    int winW = winRect.right - winRect.left;
    int winH = winRect.bottom - winRect.top;
    if (owner && GetWindowRect(owner, &ownerRect)) {
        ox = ownerRect.left + ((ownerRect.right - ownerRect.left) - winW) / 2;
        oy = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - winH) / 2;
    }

    HWND hwnd = CreateWindowExW(exStyle, L"ImageBrowserColorPicker",
                                L"拾色器", style,
                                ox, oy, winW, winH, owner, nullptr,
                                GetModuleHandleW(nullptr), &st);
    if (!hwnd) return false;

    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (owner) {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }
    if (!st.ok) return false;

    int r, g, b;
    HsvToRgb(st.h, st.s, st.v, r, g, b);
    *out = RGB(r, g, b);
    return true;
}
