// ImageBrowser - ClassRonin
// 极简看图器外壳：只实现“项目设计”里的键位与 ini 两项。
// 解码与图像处理全部复用 JPEGView 内核（JPEGImage / ImageLoadThread / 各格式 Wrapper）。

#include "stdafx.h"

#include "JPEGImage.h"
#include "ImageLoadThread.h"
#include "MessageDef.h"
#include "ProcessParams.h"
#include "Helpers.h"
#include "ColorPicker.h"
#include "JPEGView/ProcessingThreadPool.h"
#include "BasicProcessing.h"
#include "SettingsProvider.h"
#include "JPEGProvider.h"
#include "FileList.h"
#include "DirectoryWatcher.h"

#include <gdiplus.h>
#include <shellapi.h>
#include <shlobj.h>
#include <atlstr.h>


#include <algorithm>
#include <cstdio>
#include <timeapi.h>
#include <cmath>
#include <string>
#include <vector>

#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

static const wchar_t* kAppTitle = L"ImageBrowser - ClassRonin";
static const wchar_t* kIniName = L"ImageBrowser.ini";

static const wchar_t* kExts[] = {
    L".jpg", L".jpeg", L".jpe", L".jfif", L".png", L".gif", L".webp",
    L".bmp", L".tga", L".psd", L".qoi"
};

static uint32_t ClampColor(uint32_t c) { return c & 0xFFFFFFu; }

// ---- 下面四段照 JPEGView 的 HelpersGUI.cpp 原样搬（逻辑未改）：那边的文件还依赖
// resource.h / NLS.h（精简版没编），所以只搬绘制缩放百分比用得到的这几个小函数。
static float g_uiScaling = 1.0f;                       // 原 HelpersGUI::ScreenScaling
static int ScaleToScreen(int value) { return (int)(value * g_uiScaling + 0.5f); }

static HFONT CreateFontFromINIDescription(HDC dc, LPCTSTR sIniFileDescription) {
    CString sIniDesc(sIniFileDescription);
    sIniDesc.TrimLeft(); sIniDesc.TrimRight();

    CString sFontName;
    float fFontSize = 9.0f;
    bool isBold = false;
    if (sIniDesc.CompareNoCase(_T("default")) != 0) {
        if (sIniDesc.Find(_T(' ')) == -1) {
            sIniDesc.TrimLeft(_T('"')); sIniDesc.TrimRight(_T('"'));
            sFontName = sIniDesc;
        } else {
            TCHAR delimiter = (sIniDesc.GetAt(0) == _T('"')) ? _T('"') : _T(' ');
            sIniDesc.TrimLeft(_T('"'));
            if (delimiter != _T('"') && sIniDesc.FindOneOf(_T("0123456789")) == -1) {
                sFontName = sIniDesc;
            } else {
                int indexEndOfFilename = sIniDesc.Find(delimiter);
                if (indexEndOfFilename < 0) {
                    sFontName = sIniDesc;
                } else {
                    sFontName = sIniDesc.Left(indexEndOfFilename);
                    sIniDesc = sIniDesc.Mid(indexEndOfFilename + 1);
                    fFontSize = (float)_ttof(sIniDesc);
                    if (fFontSize <= 0.0) fFontSize = 9.0;
                    fFontSize = max(7.0f, min(30.0f, fFontSize));
                    isBold = sIniDesc.Find(_T("bold")) != -1;
                }
            }
        }
    }
    if (sFontName.IsEmpty()) return (HFONT)NULL;

    int nHeight = -(int)(0.5f + fFontSize * ::GetDeviceCaps(dc, LOGPIXELSY) / 72);
    return ::CreateFont(nHeight, 0, 0, 0, isBold ? FW_BOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                        VARIABLE_PITCH, sFontName);
}

static void SelectDefaultFileNameFont(HDC dc) {
    static HFONT DefaultFileNameFont = NULL;
    if (DefaultFileNameFont == NULL) {
        DefaultFileNameFont = CreateFontFromINIDescription(dc, CSettingsProvider::This().FileNameFont());
        if (DefaultFileNameFont == NULL)
            DefaultFileNameFont = CreateFontFromINIDescription(dc, _T("\"Arial\" 10.0 bold"));
    }
    ::SelectObject(dc, DefaultFileNameFont);
}

static void DrawTextBordered(CDC& dc, LPCTSTR sText, const CRect& rect, UINT nFormat) {
    COLORREF oldColor = dc.SetTextColor(RGB(0, 0, 0));
    CRect textRect = rect;
    textRect.InflateRect(-1, -1);
    textRect.OffsetRect(-1, 0); dc.DrawText(sText, -1, &textRect, nFormat | DT_NOPREFIX);
    textRect.OffsetRect(2, 0);  dc.DrawText(sText, -1, &textRect, nFormat | DT_NOPREFIX);
    textRect.OffsetRect(-1, -1); dc.DrawText(sText, -1, &textRect, nFormat | DT_NOPREFIX);
    textRect.OffsetRect(0, 2);  dc.DrawText(sText, -1, &textRect, nFormat | DT_NOPREFIX);
    textRect.OffsetRect(0, -1);
    dc.SetTextColor(oldColor);
    dc.DrawText(sText, -1, &textRect, nFormat | DT_NOPREFIX);
}

static inline double DMin(double a, double b) { return a < b ? a : b; }
static inline double DMax(double a, double b) { return a > b ? a : b; }
static inline int IMin(int a, int b) { return a < b ? a : b; }
static inline int IMax(int a, int b) { return a > b ? a : b; }

class CImageBrowserApp {
public:
    CImageBrowserApp() {}
    ~CImageBrowserApp() { Shutdown(); }

    bool Create(HINSTANCE hInst, const std::wstring& startupFile);
    int Run();
    void Shutdown();

private:
    // 窗口
    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);
    void CreateWindowInternal();

    // 配置
    std::wstring IniPath() const;
    void LoadConfig();
    void SaveConfig();

    // 文件与载入：全部用 JPEGView 自己的 CFileList / CDirectoryWatcher / CJPEGProvider。
    // 这三件套就是原版“按住方向键顺滑、CPU 低”的原因：请求是阻塞的、结果有 2 个缓冲的
    // 预读缓存、过期请求会被 RemoveUnusedImages 回收——不是每按一下新起一个解码。
    void ShowImage(CJPEGProvider::EReadAheadDirection dir, int frame = 0, bool newImage = true);
    void OpenPath(const std::wstring& path);
    void StepFile(int delta);
    void DeleteCurrentFile();
    std::wstring CurrentPath() const;

    // 载入
    CProcessParams BuildProcessParams(bool keepView = false);

    // 超大图的“屏幕副本”：原图按 HQ 缩到长边 ≤ 4096，缩放期的 HQ 只在这份副本上做，
    // 168MP 的单次 HQ 从 1.1s CPU 降到几十毫秒；1:1 和超过副本分辨率的放大仍走原图。


    // 绘制
    void Paint();
    void ComputeLayout(CSize& full, CSize& clip, CPoint& targetOffset, CPoint& drawPos);
    HBRUSH BackgroundBrush();
    COLORREF BackgroundColor() const;
    bool EnsureSurface(int w, int h);
    void ReleaseSurface();
    void ClearSurface();
    void BlendToSurface(const BYTE* src, int stride, int sw, int sh, int dx, int dy);
    void FillBackgroundArea(HDC dc, int x, int y, int w, int h);

    // 交互
    void ToggleFit();
    void ToggleBackground();
    void PickBackground();
    void RotateImage(int delta);
    double FitZoom();
    double ZoomMultiplier();
    void ApplyZoom(double newZoom, int mouseX, int mouseY);
    void PerformZoom(double exponent, bool ctrl, int mouseX, int mouseY);
    void PanBy(int dx, int dy);
    void BeginInteraction();
    void Settle();
    double animNextTick_ = 0.0;   // 下一帧的绝对时刻（QPC 秒）
    void RequestAnimFrame();
    void UpdateTitle();

    // 缩放百分比文字：照 CMainDlg::OnPaint 的 m_bInZooming / m_bShowZoomFactor 那两行
    void DrawZoomText(HDC hdc);
    CRect ZoomTextRect();
    void ShowZoomText();

    HINSTANCE hInst_ = nullptr;
    HWND hwnd_ = nullptr;
    CDirectoryWatcher* watcher_ = nullptr;
    CJPEGProvider* provider_ = nullptr;
    CFileList* fileList_ = nullptr;
    CJPEGImage* image_ = nullptr;   // 借自 provider，用完后 NotifyNotUsed，绝不能 delete
    ULONG_PTR gdiplusToken_ = 0;

    double zoom_ = -1.0;         // 绝对缩放（图像像素→屏幕像素），-1 = 适应窗口
    double zoomMult_ = 1.1;      // 滚轮一格的比例（按窗口/图算，让 100% 落在整数步上）
    double zoomStep_ = 1.1;      // ini ZoomStep：普通滚轮一格
    double ctrlZoomStep_ = 2.0;  // ini CtrlZoomStep：Ctrl + 滚轮一格（默认翻倍）
    CPoint offsets_{0, 0};       // 图像相对窗口中心的位移（屏幕像素，右/下为正）
    int rotation_ = 0;           // 当前文件临时旋转

    // 配置（只在关闭时写 ini）
    int cfgMaximized_ = 0;
    uint32_t cfgBackground_ = 0xF2F2F2;
    bool checker_ = true;        // 默认 alpha（棋盘）

    // 拖拽与动画
    bool dragging_ = false;
    POINT dragOrigin_{};
    CPoint dragOffsets_{0, 0};

    // 缩放交互状态：zooming_ 用来做 100% 吸附（原版 m_bZoomMode）
    bool interacting_ = false;
    bool zooming_ = false;

    // 背景画刷（纯色）
    HBRUSH solidBrush_ = nullptr;
    COLORREF solidColor_ = 0xFFFFFFFF;

    // 分层窗口的绘制面：32bpp 自管 DIB，alpha 直接就是窗口的透明度
    HDC memDC_ = nullptr;
    HBITMAP surfaceBmp_ = nullptr;
    HBITMAP oldSurfaceBmp_ = nullptr;
    void* surfaceBits_ = nullptr;
    int surfaceW_ = 0, surfaceH_ = 0;

    static const UINT kSettleTimerId = 2;
    static const UINT kZoomTextTimerId = 3;
    bool showZoom_ = false;       // 原版 m_bShowZoomFactor：缩放后 1 秒内还显示百分比
};

static CImageBrowserApp g_app;

static double NowSec() {
    LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f);
    return (double)c.QuadPart / (double)f.QuadPart;
}

// 全显式中性处理参数：对比 0 / gamma 1 / 饱和 1 / 锐化 0 / 无色彩校正。
// 内核的默认构造函数填的是 -1（表示“去查设置库”），本壳不建设置库，
// -1 会落到未初始化的设置上，导致偏色 + 曲线怪异。
// 参数顺序：Contrast, Gamma, Saturation, Sharpen, ...。
// Sharpen 用原版默认的 0.3（SettingsProvider 里 "Sharpen" 默认值），
// 高质量缩小重采样时内核会按这个值补锐度；填 0 会明显比原版发虚。
static CImageProcessingParams NeutralProcParams() {
    return CImageProcessingParams(0.0, 1.0, 1.0, 0.3, 0.0, 0.0, 0.0, 0.0, 0.0,
                                  0.0, 0.0, 0.0);
}

std::wstring CImageBrowserApp::IniPath() const {
    wchar_t exePath[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring path(exePath);
    size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) path.resize(slash + 1);
    path += kIniName;
    return path;
}

void CImageBrowserApp::LoadConfig() {
    std::wstring ini = IniPath();
    cfgMaximized_ = GetPrivateProfileIntW(L"ImageBrowser", L"Maximized", 0, ini.c_str());
    wchar_t buf[64] = {0};
    GetPrivateProfileStringW(L"ImageBrowser", L"Background", L"f2f2f2", buf, 64, ini.c_str());
    cfgBackground_ = ClampColor(wcstoul(buf, nullptr, 16));

    // 背景模式：alpha（棋盘）/ color（底色）。没写过就当 alpha。
    wchar_t mode[32] = {0};
    GetPrivateProfileStringW(L"ImageBrowser", L"BackgroundMode", L"alpha", mode, 32, ini.c_str());
    checker_ = (_wcsicmp(mode, L"color") != 0);

    // 缩放倍率：普通滚轮一格 / Ctrl + 滚轮一格。下限 1.01（再小就几乎不动），
    // ZoomStep 上限 2.0（超过 Ctrl 档），CtrlZoomStep 上限 8.0。
    wchar_t num[32] = {0};
    GetPrivateProfileStringW(L"ImageBrowser", L"ZoomStep", L"1.1", num, 32, ini.c_str());
    zoomStep_ = _wtof(num);
    if (zoomStep_ < 1.01) zoomStep_ = 1.01;
    if (zoomStep_ > 2.0) zoomStep_ = 2.0;

    GetPrivateProfileStringW(L"ImageBrowser", L"CtrlZoomStep", L"2.0", num, 32, ini.c_str());
    ctrlZoomStep_ = _wtof(num);
    if (ctrlZoomStep_ < 1.01) ctrlZoomStep_ = 1.01;
    if (ctrlZoomStep_ > 8.0) ctrlZoomStep_ = 8.0;
}

void CImageBrowserApp::SaveConfig() {
    std::wstring ini = IniPath();
    wchar_t buf[64];
    swprintf_s(buf, L"%d", cfgMaximized_);
    WritePrivateProfileStringW(L"ImageBrowser", L"Maximized", buf, ini.c_str());
    swprintf_s(buf, L"%06x", cfgBackground_);
    WritePrivateProfileStringW(L"ImageBrowser", L"Background", buf, ini.c_str());
    WritePrivateProfileStringW(L"ImageBrowser", L"BackgroundMode", checker_ ? L"alpha" : L"color", ini.c_str());
    swprintf_s(buf, L"%.3f", zoomStep_);
    WritePrivateProfileStringW(L"ImageBrowser", L"ZoomStep", buf, ini.c_str());
    swprintf_s(buf, L"%.3f", ctrlZoomStep_);
    WritePrivateProfileStringW(L"ImageBrowser", L"CtrlZoomStep", buf, ini.c_str());
}

bool CImageBrowserApp::Create(HINSTANCE hInst, const std::wstring& startupFile) {
    hInst_ = hInst;
    LoadConfig();

    // 和 JPEGView 原版一样，先把图像处理线程池建起来；不建的话每次重采样只有单线程，
    // 大图缩放就会比原版慢好几倍。
    CProcessingThreadPool::This().CreateThreadPoolThreads();
    GdiplusStartupInput gdiplusStartupInput;
    GdiplusStartup(&gdiplusToken_, &gdiplusStartupInput, nullptr);



    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = &CImageBrowserApp::WndProcThunk;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
    wc.hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                  GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED);
    wc.lpszClassName = L"ImageBrowserClass";
    if (!RegisterClassExW(&wc)) return false;

    RECT workArea{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    int w = 800, h = 600;
    int x = (workArea.left + workArea.right - w) / 2;
    int y = (workArea.top + workArea.bottom - h) / 2;

    hwnd_ = CreateWindowExW(0, wc.lpszClassName, kAppTitle, WS_OVERLAPPEDWINDOW,
                            x, y, w, h, nullptr, nullptr, hInst, this);
    if (!hwnd_) return false;

    // 任务栏与 Alt+Tab 用大图标，标题栏用小图标，均取资源 ID 1
    SendMessageW(hwnd_, WM_SETICON, ICON_BIG,
                 (LPARAM)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                    GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0));
    SendMessageW(hwnd_, WM_SETICON, ICON_SMALL,
                 (LPARAM)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                    GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));

    if (cfgMaximized_) ShowWindow(hwnd_, SW_MAXIMIZE);

    watcher_ = new CDirectoryWatcher(hwnd_);
    provider_ = new CJPEGProvider(hwnd_, 1, 2);   // NUM_THREADS=1 / READ_AHEAD_BUFFERS=2，与原版一致

    // 原版在 CMainDlg 初始化里做这一句，ScaleToScreen 靠它；不设就是 -1，
    // 缩放文字的矩形会算成负数。
    {
        HDC dc = GetDC(hwnd_);
        g_uiScaling = ::GetDeviceCaps(dc, LOGPIXELSX) / 96.0f;
        ReleaseDC(hwnd_, dc);
    }

    if (!startupFile.empty()) OpenPath(startupFile);
    if (!fileList_) UpdateTitle();
    DragAcceptFiles(hwnd_, TRUE);   // 允许把文件/文件夹拖进窗口打开
    return true;
}

// 打开一个文件或目录（命令行参数、拖放共用）。和 IDM_MOVE_TO_RECYCLE_BIN 一样，
// 先把自己手里的图交还给 provider，再清空请求、换掉 CFileList，避免旧请求继续写内存。
void CImageBrowserApp::OpenPath(const std::wstring& path) {
    if (path.empty() || !provider_) return;

    CJPEGImage* old = image_;
    image_ = nullptr;
    if (old) provider_->ClearRequest(old);
    provider_->ClearAllRequests();
    if (fileList_) {
        delete fileList_;
        fileList_ = nullptr;
    }

    wchar_t full[MAX_PATH] = {0};
    if (!GetFullPathNameW(path.c_str(), MAX_PATH, full, nullptr)) return;
    CString p(full);
    DWORD attr = GetFileAttributesW(full);
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) p += _T("\\");
    // 字母序（FS_FileName）+ 升序，与“←→ 按字母序切图”一致
    fileList_ = new CFileList(p, *watcher_, Helpers::FS_FileName, true, true);
    fileList_->SetNavigationMode(Helpers::NM_LoopDirectory);
    ShowImage(CJPEGProvider::FORWARD);
}

CProcessParams CImageBrowserApp::BuildProcessParams(bool keepView) {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    CSize client(IMax(1, (int)(rc.right - rc.left)), IMax(1, (int)(rc.bottom - rc.top)));
    CSize screen(GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    CImageProcessingParams procParams = NeutralProcParams();
    // 和 JPEGView 原版一致的加载参数：zoom=-1（自动适应窗口），让内核在加载线程里
    // 把窗口尺寸那一帧重采样好并缓存。native 像素仍保留（内核不销毁它），
    // 所以之后 1:1 / 放大时的 GetDIB 是"从 native 取可见区域"，清晰且只算一块。
    // 和参考版 JPEGView.ini 一致：HighQualityResampling=false。
    // 内核 HQ 缩小的成本 ∝ 源图像素（168MP 一次 1~1.3s CPU），关掉才是同一量级。
    //
    // keepView：动图推进帧用。原版 StartAnimation 会先 IDM_KEEP_PARAMETERS，于是
    // GotoImage(POS_NextAnimation) 的 CreateProcessParams 走 "kept" 分支——把当前的
    // m_dZoom / m_offsets 一起发给加载线程，并带 PFLAG_KeepParams。这样新的一帧是按
    // 你当前的缩放预处理出来的，不会每帧都按"适应窗口"重生一次。
    double zoom = keepView ? zoom_ : -1.0;
    CPoint offset = keepView ? offsets_ : CPoint(0, 0);
    EProcessingFlags flags = keepView ? EProcessingFlags(PFLAG_KeepParams)
                                      : EProcessingFlags(PFLAG_None);
    return CProcessParams(client.cx, client.cy, screen, CRotationParams(0), 0, zoom,
                          Helpers::ZM_FitToScreenNoZoom, offset, procParams, flags);
}

std::wstring CImageBrowserApp::CurrentPath() const {
    if (!fileList_ || fileList_->Current() == nullptr) return std::wstring();
    return std::wstring(fileList_->Current());
}

// 与原版 CMainDlg::GotoImage 的载入部分同一套：
// RequestImage 阻塞到这一张就绪（命中预读缓存时立即返回），provider 自己发下一张预读。
void CImageBrowserApp::ShowImage(CJPEGProvider::EReadAheadDirection dir, int frame, bool newImage) {
    if (!fileList_ || !provider_ || fileList_->Current() == nullptr) {
        UpdateTitle();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    // 只在新图时复位视图。动图推进帧时 frame 会从 0 开始轮转，第 0 帧也是“同一张图的
    // 第 0 帧”，不能拿 frame == 0 当“换了图”，否则动画每循环一圈缩放就被打回适应窗口。
    if (newImage) {
        rotation_ = 0;
        zoom_ = -1.0;             // 切图回到适应窗口（原版行为）
        offsets_ = CPoint(0, 0);
        zoomMult_ = zoomStep_;
        interacting_ = false;
        zooming_ = false;
        animNextTick_ = 0.0;
        KillTimer(hwnd_, kSettleTimerId);
        showZoom_ = false;
        KillTimer(hwnd_, kZoomTextTimerId);
    }

    bool oom = false, excErr = false;
    CJPEGImage* loaded = provider_->RequestImage(fileList_, dir, fileList_->Current(), frame,
                                                 BuildProcessParams(!newImage), oom, excErr);
    CJPEGImage* old = image_;
    image_ = loaded;
    if (old) provider_->NotifyNotUsed(old);   // 交由 provider 决定缓存还是释放

    if (image_ && image_->IsAnimation()) {
        // 绝对节拍：按上一帧的计划时刻递增，落后太多才重置，避免误差累积
        double frameSec = image_->FrameTimeMs() / 1000.0;
        if (frameSec <= 0.0) frameSec = 0.05;
        if (animNextTick_ <= 0.0 || NowSec() - animNextTick_ > frameSec * 3.0) {
            animNextTick_ = NowSec() + frameSec;
        } else {
            animNextTick_ += frameSec;
        }
    }
    UpdateTitle();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void CImageBrowserApp::UpdateTitle() {
    SetWindowTextW(hwnd_, kAppTitle);
}

void CImageBrowserApp::StepFile(int delta) {
    if (!fileList_) return;
    CFileList* next = (delta >= 0) ? fileList_->Next() : fileList_->Prev();
    if (!next) return;
    fileList_ = next;             // Next/Prev 可能跨到另一个目录节点
    ShowImage((delta >= 0) ? CJPEGProvider::FORWARD : CJPEGProvider::BACKWARD);
}

void CImageBrowserApp::DeleteCurrentFile() {
    std::wstring path = CurrentPath();
    if (path.empty() || !fileList_) return;

    // 与原件 IDM_MOVE_TO_RECYCLE_BIN 相同：先离开当前图（不请求），删进回收站，再刷新列表
    CJPEGImage* old = image_;
    image_ = nullptr;
    if (old) provider_->ClearRequest(old);

    if (fileList_->DeleteFile(path.c_str())) {
        fileList_->Reload(nullptr, false);
        fileList_->DeleteHistory(true);
        if (fileList_->Current() == nullptr) {
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        ShowImage(CJPEGProvider::NONE);
    } else {
        ShowImage(CJPEGProvider::NONE);
    }
}

void CImageBrowserApp::RequestAnimFrame() {
    if (!image_ || !image_->IsAnimation()) return;
    int frames = image_->NumberOfFrames();
    if (frames <= 1) return;
    int next = (image_->FrameIndex() + 1) % frames;
    ShowImage(CJPEGProvider::NONE, next, false);
}


void CImageBrowserApp::BeginInteraction() {
    interacting_ = true;
    KillTimer(hwnd_, kSettleTimerId);
    SetTimer(hwnd_, kSettleTimerId, 200, nullptr);
}

void CImageBrowserApp::Settle() {
    KillTimer(hwnd_, kSettleTimerId);
    interacting_ = false;
    zooming_ = false;
}

// 照 CMainDlg::GetZoomTextRect（本壳没有处理面板，去掉那两处面板修正）：
// 宽 70、高 25、右/下各离边 35（窗口宽 < ScaleToScreen(800) 时下边距用 25）
CRect CImageBrowserApp::ZoomTextRect() {
    CRect area;
    GetClientRect(hwnd_, &area);
    int bottomOffset = (area.Width() < ScaleToScreen(800)) ? 25 : 35;
    int startX = area.right - ScaleToScreen(70 + 35);
    int endX = startX + ScaleToScreen(70);
    return CRect(startX, area.bottom - ScaleToScreen(25 + bottomOffset),
                 endX, area.bottom - ScaleToScreen(bottomOffset));
}

// 照 CMainDlg::OnPaint 里的 "Show current zoom factor" 段：字号用文件名默认字体，颜色用 ColorGUI，
// 文字带黑色描边（DrawTextBordered），右对齐。
void CImageBrowserApp::DrawZoomText(HDC hdc) {
    if (!image_) return;
    double z = (zoom_ > 0.0) ? zoom_ : FitZoom();
    wchar_t buff[32];
    swprintf_s(buff, 32, L"%d %%", (int)(z * 100.0 + 0.5));

    CDC dc;
    dc.Attach(hdc);
    // 原版 DisplayFileName 开头就是这么设的：透明底 + 黑色底色调，描边的黑字才是"描边"，
    // 不设透明底的话 DrawTextBordered 的 5 遍都会各刷一块底色，字周围就是脏方块。
    dc.SetBkMode(TRANSPARENT);
    dc.SetBkColor(RGB(0, 0, 0));
    dc.SetTextColor(CSettingsProvider::This().ColorGUI());
    SelectDefaultFileNameFont(dc);
    DrawTextBordered(dc, buff, ZoomTextRect(), DT_RIGHT);
    dc.Detach();
}

void CImageBrowserApp::ShowZoomText() {
    showZoom_ = true;
    KillTimer(hwnd_, kZoomTextTimerId);
    SetTimer(hwnd_, kZoomTextTimerId, 1000, nullptr);   // 原版 ZOOM_TEXT_TIMEOUT = 1000
    InvalidateRect(hwnd_, ZoomTextRect(), FALSE);
}

void CImageBrowserApp::ToggleFit() {
    if (!image_) return;
    if (zoom_ > 0.0) {
        // 不是适应状态 → 回适应窗口（原版 ResetZoomToFitScreen）
        zoom_ = -1.0;
        offsets_ = CPoint(0, 0);
    } else {
        // 适应状态 → 1:1，以光标为锚（原版 ResetZoomTo100Percents）
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(hwnd_, &pt);
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        if (pt.x < 0 || pt.y < 0 || pt.x >= (int)(rc.right - rc.left) || pt.y >= (int)(rc.bottom - rc.top)) {
            pt.x = (int)((rc.right - rc.left) / 2);
            pt.y = (int)((rc.bottom - rc.top) / 2);
        }
        ApplyZoom(1.0, pt.x, pt.y);
    }
    BeginInteraction();
    InvalidateRect(hwnd_, nullptr, FALSE);
    ShowZoomText();       // 空格/R 键也在改缩放，同样显示百分比
}

void CImageBrowserApp::ToggleBackground() {
    checker_ = !checker_;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void CImageBrowserApp::PickBackground() {
    COLORREF chosen = RGB((cfgBackground_ >> 16) & 0xFF, (cfgBackground_ >> 8) & 0xFF, cfgBackground_ & 0xFF);
    if (PickBackgroundColor(hwnd_, chosen, &chosen)) {
        uint32_t rgb = ((uint32_t)GetRValue(chosen) << 16) |
                       ((uint32_t)GetGValue(chosen) << 8) |
                       (uint32_t)GetBValue(chosen);
        cfgBackground_ = ClampColor(rgb);
        checker_ = false;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void CImageBrowserApp::RotateImage(int delta) {
    if (!image_ || image_->IsAnimation()) return;  // 动画静默无反应
    int step = delta > 0 ? 90 : 270;
    if (!image_->Rotate(step)) return;
    rotation_ = (rotation_ + step) % 360;
    offsets_ = CPoint(0, 0);

    InvalidateRect(hwnd_, nullptr, FALSE);
}

void CImageBrowserApp::ComputeLayout(CSize& full, CSize& clip, CPoint& targetOffset, CPoint& drawPos) {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    int cw = (int)(rc.right - rc.left);
    int ch = (int)(rc.bottom - rc.top);
    full = CSize(cw, ch);
    clip = CSize(cw, ch);
    targetOffset = CPoint(0, 0);
    drawPos = CPoint(0, 0);
    if (!image_) return;

    double sw = image_->OrigWidth();
    double sh = image_->OrigHeight();
    if (sw <= 0 || sh <= 0) return;

    // 和原版一致：zoom_ 是绝对比例，-1 表示适应窗口（不放大）
    double z = (zoom_ > 0.0) ? zoom_ : FitZoom();
    full = CSize(IMax(1, (int)(sw * z + 0.5)), IMax(1, (int)(sh * z + 0.5)));
    clip = CSize(IMin(full.cx, cw), IMin(full.cy, ch));

    // 照 Helpers::LimitOffsets：位移只能走图像比窗口多出来的那一段
    int maxOffX = IMax(0, (full.cx - cw) / 2);
    int maxOffY = IMax(0, (full.cy - ch) / 2);
    int ox = IMax(-maxOffX, IMin(maxOffX, offsets_.x));
    int oy = IMax(-maxOffY, IMin(maxOffY, offsets_.y));

    // 照 CJPEGImage::ConvertOffset：GetDIB 要的是"可见窗口在目标图里的左上角"
    if (clip.cx >= cw) {
        targetOffset.x = (full.cx - clip.cx) / 2 - ox;
        drawPos.x = 0;
    } else {
        targetOffset.x = 0;
        drawPos.x = (cw - clip.cx) / 2;
    }
    if (clip.cy >= ch) {
        targetOffset.y = (full.cy - clip.cy) / 2 - oy;
        drawPos.y = 0;
    } else {
        targetOffset.y = 0;
        drawPos.y = (ch - clip.cy) / 2;
    }
}

// 背景：alpha 模式铺纯浅灰，底色模式铺 ini 里的颜色，都走 solid brush。
// 原来 alpha 模式铺棋盘花纹，图像透明的那圈边缘会被花纹衬成一块“填充色块”，
// 看着像把透明边填成了实体，所以取消花纹。
HBRUSH CImageBrowserApp::BackgroundBrush() {
    COLORREF c = checker_ ? RGB(0xF2, 0xF2, 0xF2)
        : RGB((cfgBackground_ >> 16) & 0xFF, (cfgBackground_ >> 8) & 0xFF, cfgBackground_ & 0xFF);
    if (!solidBrush_ || solidColor_ != c) {
        if (solidBrush_) DeleteObject(solidBrush_);
        solidBrush_ = CreateSolidBrush(c);
        solidColor_ = c;
    }
    return solidBrush_;
}

// 只刷一块矩形：图像盖住的区域不刷。整窗刷棋盘（pattern ROP）在 800×600 上
// 每帧要好几毫秒，这正是缩放时 CPU 偏高的主因。
void CImageBrowserApp::FillBackgroundArea(HDC dc, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    RECT r{ x, y, x + w, y + h };
    FillRect(dc, &r, BackgroundBrush());
}

COLORREF CImageBrowserApp::BackgroundColor() const {
    if (checker_) return RGB(0xF2, 0xF2, 0xF2);
    return RGB((cfgBackground_ >> 16) & 0xFF, (cfgBackground_ >> 8) & 0xFF, cfgBackground_ & 0xFF);
}

bool CImageBrowserApp::EnsureSurface(int w, int h) {
    if (memDC_ && surfaceW_ == w && surfaceH_ == h) return true;
    ReleaseSurface();
    memDC_ = CreateCompatibleDC(nullptr);
    if (!memDC_) return false;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;      // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    surfaceBmp_ = CreateDIBSection(memDC_, &bi, DIB_RGB_COLORS, &surfaceBits_, nullptr, 0);
    if (!surfaceBmp_) {
        ReleaseSurface();
        return false;
    }
    oldSurfaceBmp_ = (HBITMAP)SelectObject(memDC_, surfaceBmp_);
    surfaceW_ = w;
    surfaceH_ = h;
    return true;
}

void CImageBrowserApp::ReleaseSurface() {
    if (oldSurfaceBmp_) {
        SelectObject(memDC_, oldSurfaceBmp_);
        oldSurfaceBmp_ = nullptr;
    }
    if (surfaceBmp_) { DeleteObject(surfaceBmp_); surfaceBmp_ = nullptr; }
    if (memDC_) { DeleteDC(memDC_); memDC_ = nullptr; }
    surfaceBits_ = nullptr;
    surfaceW_ = surfaceH_ = 0;
}

// 合成到已铺好的底色上：透明像素保留底色，半透明正常混合，输出 alpha=255
void CImageBrowserApp::BlendToSurface(const BYTE* src, int stride, int sw, int sh, int dx, int dy) {
    if (!surfaceBits_ || stride <= 0 || sw <= 0 || sh <= 0) return;
    int x0 = 0, y0 = 0, w = sw, h = sh;
    if (dy < 0) { y0 = -dy; h += dy; dy = 0; }
    if (dx < 0) { x0 = -dx; w += dx; dx = 0; }
    if (dx + w > surfaceW_) w = surfaceW_ - dx;
    if (dy + h > surfaceH_) h = surfaceH_ - dy;
    if (w <= 0 || h <= 0) return;
    BYTE* dstBase = (BYTE*)surfaceBits_ + (size_t)dy * surfaceW_ * 4 + (size_t)dx * 4;
    for (int y = 0; y < h; y++) {
        const BYTE* s = src + (size_t)(y0 + y) * (size_t)stride * 4 + (size_t)x0 * 4;
        BYTE* d = dstBase + (size_t)y * surfaceW_ * 4;
        for (int x = 0; x < w; x++) {
            int a = s[x * 4 + 3];
            if (a == 255) {
                d[x * 4 + 0] = s[x * 4 + 0];
                d[x * 4 + 1] = s[x * 4 + 1];
                d[x * 4 + 2] = s[x * 4 + 2];
            } else if (a != 0) {
                d[x * 4 + 0] = (BYTE)((s[x * 4 + 0] * a + d[x * 4 + 0] * (255 - a)) / 255);
                d[x * 4 + 1] = (BYTE)((s[x * 4 + 1] * a + d[x * 4 + 1] * (255 - a)) / 255);
                d[x * 4 + 2] = (BYTE)((s[x * 4 + 2] * a + d[x * 4 + 2] * (255 - a)) / 255);
            }
            d[x * 4 + 3] = 255;
        }
    }
}


void CImageBrowserApp::Paint() {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    int cw = (int)(rc.right - rc.left);
    int ch = (int)(rc.bottom - rc.top);
    if (cw <= 0 || ch <= 0) return;
    if (!EnsureSurface(cw, ch)) return;

    // 底：alpha 模式铺棋盘（透明标识），底色模式铺纯色
    DWORD* p = (DWORD*)surfaceBits_;
    int n = surfaceW_ * surfaceH_;
    if (checker_) {
        const int cell = 8;
        const DWORD c1 = 0xFFFFFFFFu;      // 白
        const DWORD c2 = 0xFFE6E6E6u;      // 浅灰格
        for (int yy = 0; yy < surfaceH_; yy++) {
            DWORD* row = p + (size_t)yy * surfaceW_;
            int cy = (yy / cell) & 1;
            for (int xx = 0; xx < surfaceW_; xx++) {
                row[xx] = (((xx / cell) & 1) ^ cy) ? c2 : c1;
            }
        }
    } else {
        COLORREF c = BackgroundColor();
        DWORD px = 0xFF000000u | ((DWORD)GetRValue(c) << 16) | ((DWORD)GetGValue(c) << 8) | (DWORD)GetBValue(c);
        for (int i = 0; i < n; i++) p[i] = px;
    }

    if (image_) {
        CSize full(0, 0), clip(0, 0);
        CPoint targetOffset(0, 0), drawPos(0, 0);
        ComputeLayout(full, clip, targetOffset, drawPos);

        // 与参考版一致：不开 HQ（PFLAG_HighQualityResampling）。内核无 HQ 标志时走
        // CBasicProcessing::PointSample，只按可见区域取样，1.68 亿像素也是几毫秒。
        void* pDIB = image_->GetDIB(full, clip, targetOffset, NeutralProcParams(), EProcessingFlags(PFLAG_None));
        int dibW = image_->DIBWidth();
        int dibH = image_->DIBHeight();
        if (pDIB && dibW > 0 && dibH > 0) {
            int w = IMin(clip.cx, dibW);
            int h = IMin(clip.cy, dibH);
            if (w > 0 && h > 0) {
                // 带 alpha 的图（PNG/GIF/WebP）按 alpha 合成到窗口底色上：透明像素保留底色，
                // 半透明像素正常混合；不带 alpha 的图（JPEG/BMP）alpha 视作 255，原样贴。
                BlendToSurface((const BYTE*)pDIB, clip.cx, w, h, drawPos.x, drawPos.y);
            }
        }
    }

    if (showZoom_) DrawZoomText(memDC_);

    // 普通窗口：整窗一次 BitBlt
    HDC hdc = GetDC(hwnd_);
    BitBlt(hdc, 0, 0, cw, ch, memDC_, 0, 0, SRCCOPY);
    ReleaseDC(hwnd_, hdc);
}
double CImageBrowserApp::FitZoom() {
    if (!image_) return 1.0;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    double cw = IMax(1, (int)(rc.right - rc.left));
    double ch = IMax(1, (int)(rc.bottom - rc.top));
    double ow = image_->OrigWidth(), oh = image_->OrigHeight();
    if (ow <= 0 || oh <= 0) return 1.0;
    return DMin(1.0, DMin(cw / ow, ch / oh));
}

// 照 CMainDlg::GetZoomMultiplier：每格是 ini 里的 ZoomStep（默认 1.1），且整数步之后正好落在 100%
double CImageBrowserApp::ZoomMultiplier() {
    double fit = FitZoom();
    double step = (zoomStep_ > 1.0) ? zoomStep_ : 1.1;
    if (fit >= 1.0) return step;
    double n = floor(log(1.0 / fit) / log(step) + 0.5);
    if (n < 1.0) return step;
    return exp(log(1.0 / fit) / n);
}

// 照 Helpers::LimitOffsets：位移限制在图像比窗口多出来的那一段
void CImageBrowserApp::PanBy(int dx, int dy) {
    if (!image_) return;
    offsets_.x += dx;
    offsets_.y += dy;
    double z = (zoom_ > 0.0) ? zoom_ : FitZoom();
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    int cw = IMax(1, (int)(rc.right - rc.left)), ch = IMax(1, (int)(rc.bottom - rc.top));
    int fullW = IMax(1, (int)(image_->OrigWidth() * z + 0.5));
    int fullH = IMax(1, (int)(image_->OrigHeight() * z + 0.5));
    int maxX = IMax(0, (fullW - cw) / 2);
    int maxY = IMax(0, (fullH - ch) / 2);
    offsets_.x = IMax(-maxX, IMin(maxX, offsets_.x));
    offsets_.y = IMax(-maxY, IMin(maxY, offsets_.y));
}

// 照 CMainDlg::PerformZoom：绝对比例 + 以光标为锚 + 100% 吸附 + 65535 上限
void CImageBrowserApp::ApplyZoom(double newZoom, int mouseX, int mouseY) {
    if (!image_) return;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    int cw = IMax(1, (int)(rc.right - rc.left)), ch = IMax(1, (int)(rc.bottom - rc.top));
    double ow = image_->OrigWidth(), oh = image_->OrigHeight();
    if (ow <= 0 || oh <= 0) return;

    double oldZ = (zoom_ > 0.0) ? zoom_ : FitZoom();
    // 原版 ZoomMin = DBL_MIN / ZoomMax = DBL_MAX，真正生效的是这两道：下限 0.0001，
    // 以及"输出不超过 65535 像素"（CBasicProcessing 撑不住更大）。
    newZoom = DMax(0.0001, newZoom);
    if (fabs(newZoom - 1.0) < 0.01) newZoom = 1.0;
    // 连续缩放跨过 100% 时吸住一次，停手后再继续（原版 ZoomPausePercent 默认 100）
    if (zooming_ && (oldZ - 1.0) * (newZoom - 1.0) <= 0.0) newZoom = 1.0;

    int oldW = IMax(1, (int)(ow * oldZ + 0.5)), oldH = IMax(1, (int)(oh * oldZ + 0.5));
    int newW = IMax(1, (int)(ow * newZoom + 0.5)), newH = IMax(1, (int)(oh * newZoom + 0.5));
    if (newW > 65535 || newH > 65535) {
        double f = 65535.0 / DMax(newW, newH);
        newZoom *= f;
        newW = IMax(1, (int)(ow * newZoom + 0.5));
        newH = IMax(1, (int)(oh * newZoom + 0.5));
    }
    if (newW == oldW && newH == oldH) {
        // 尺寸没变就不动位移；缩小时还原比例，避免卡在同一个像素尺寸上
        zoom_ = (newZoom < oldZ) ? oldZ : newZoom;
        return;
    }

    // 光标下的那个点，缩放前后停在原处
    double fac = newZoom / oldZ;
    int oldX = oldW / 2 - cw / 2 + mouseX - offsets_.x;
    int oldY = oldH / 2 - ch / 2 + mouseY - offsets_.y;
    offsets_.x = (int)floor(newW / 2.0 - cw / 2.0 + mouseX - oldX * fac + 0.5);
    offsets_.y = (int)floor(newH / 2.0 - ch / 2.0 + mouseY - oldY * fac + 0.5);

    zoom_ = newZoom;
    PanBy(0, 0);   // 立刻夹回边界
}

void CImageBrowserApp::PerformZoom(double exponent, bool ctrl, int mouseX, int mouseY) {
    if (!image_) return;
    // 普通滚轮用 ZoomMultiplier()（带"整数步落到 100%"的自适应）；
    // Ctrl + 滚轮直接用 ini 里的 CtrlZoomStep（默认 2.0，一格翻倍），不做自适应。
    zoomMult_ = ctrl ? ctrlZoomStep_ : ZoomMultiplier();
    double oldZ = (zoom_ > 0.0) ? zoom_ : FitZoom();
    ApplyZoom(oldZ * pow(zoomMult_, exponent), mouseX, mouseY);
    zooming_ = true;
    BeginInteraction();
    InvalidateRect(hwnd_, nullptr, FALSE);
    ShowZoomText();
}
LRESULT CALLBACK CImageBrowserApp::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    CImageBrowserApp* self = nullptr;
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        self = (CImageBrowserApp*)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
        if (self) self->hwnd_ = hwnd;
    } else {
        self = (CImageBrowserApp*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    }
    if (self) return self->WndProc(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CImageBrowserApp::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            Paint();
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;

        case WM_IMAGE_LOAD_COMPLETED:
            // 与原版 CMainDlg::OnImageLoadCompleted 一样，交给 provider 收结果
            if (provider_) provider_->OnImageLoadCompleted((int)lp);
            return 0;



        case WM_TIMER:
            if (wp == kSettleTimerId) {
                Settle();
            } else if (wp == kZoomTextTimerId) {
                // 原版 ZOOM_TEXT_TIMER：到点把百分比文字撤掉
                KillTimer(hwnd, kZoomTextTimerId);
                showZoom_ = false;
                InvalidateRect(hwnd, ZoomTextRect(), FALSE);
            }
            return 0;

        case WM_DROPFILES: {
            // 从资源管理器拖进来的文件/文件夹：取第 1 个打开，其余忽略
            HDROP drop = (HDROP)wp;
            wchar_t file[MAX_PATH] = {0};
            if (DragQueryFileW(drop, 0, file, MAX_PATH) > 0) OpenPath(file);
            DragFinish(drop);
            return 0;
        }

        case WM_KEYDOWN:
            switch (wp) {
                case VK_TAB:
                    ToggleBackground();
                    return 0;
                case VK_F1:
                    PickBackground();
                    return 0;
                case VK_SPACE:
                    ToggleFit();
                    return 0;
                case VK_LEFT:
                    StepFile(-1);
                    return 0;
                case VK_RIGHT:
                    StepFile(1);
                    return 0;
                case VK_UP:
                    RotateImage(1);
                    return 0;
                case VK_DOWN:
                    RotateImage(-1);
                    return 0;
                case VK_DELETE:
                    DeleteCurrentFile();
                    return 0;
                case VK_ESCAPE:
                    // 关闭程序：走 WM_CLOSE，保证 ini 只在关闭时写
                    PostMessageW(hwnd, WM_CLOSE, 0, 0);
                    return 0;
                case 'W':
                    if (GetKeyState(VK_CONTROL) & 0x8000) {
                        PostMessageW(hwnd, WM_CLOSE, 0, 0);
                        return 0;
                    }
                    return 0;
                case VK_RETURN:
                case VK_F11:
                    // 窗口最大化 / 还原
                    if (IsZoomed(hwnd)) {
                        ShowWindow(hwnd, SW_RESTORE);
                        cfgMaximized_ = 0;
                    } else {
                        ShowWindow(hwnd, SW_MAXIMIZE);
                        cfgMaximized_ = 1;
                    }
                    return 0;
            }
            return 0;

        case WM_LBUTTONDOWN:
            if (image_) {
                dragging_ = true;
                interacting_ = true;
                KillTimer(hwnd_, kSettleTimerId);
                dragOrigin_ = POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                dragOffsets_ = offsets_;
                SetCapture(hwnd);
            }
            return 0;

        case WM_MOUSEMOVE:
            if (dragging_ && image_) {
                int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
                CPoint want(dragOffsets_.x + (mx - dragOrigin_.x),
                            dragOffsets_.y + (my - dragOrigin_.y));
                offsets_ = want;
                PanBy(0, 0);
                // 撞到边缘被截断时把锚点挪到当前点，否则往回拖会有一段"死区"
                if (offsets_ != want) {
                    dragOrigin_ = POINT{mx, my};
                    dragOffsets_ = offsets_;
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_LBUTTONUP:
            if (dragging_) {
                dragging_ = false;
                ReleaseCapture();
                BeginInteraction();           // 松手 200ms 后补高质量帧
            }
            return 0;

        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            // 原版 MouseWheelZoomSpeed 默认 1.0：一格 = 一个缩放步（约 1.1 倍）
            PerformZoom((double)delta / (double)WHEEL_DELTA, (GetKeyState(VK_CONTROL) & 0x8000) != 0, pt.x, pt.y);
            return 0;
        }

        case WM_RBUTTONUP:
            ToggleFit();
            return 0;

        case WM_SIZE:
            if (image_) {
                PanBy(0, 0);
                BeginInteraction();
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_MOVE:
            if (IsZoomed(hwnd)) cfgMaximized_ = 1;
            return 0;

        case WM_CLOSE:
            cfgMaximized_ = IsZoomed(hwnd) ? 1 : 0;
            SaveConfig();
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            if (watcher_) watcher_->Terminate();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int CImageBrowserApp::Run() {
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    MSG msg{};
    for (;;) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return 0;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        // 动画不等 SetTimer：WM_TIMER 的粒度会让 17ms 的帧拖成 31ms。
        // 用 MsgWaitForMultipleObjectsEx 精确等到下一帧时刻，再请求解码。
        DWORD waitMs = INFINITE;
        bool animating = image_ && image_->IsAnimation();
        if (animating) {
            double left = animNextTick_ - NowSec();
            waitMs = left > 0.0 ? (DWORD)(left * 1000.0 + 0.5) : 0;
        }
        MsgWaitForMultipleObjectsEx(0, nullptr, waitMs, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (animating && NowSec() >= animNextTick_) {
            RequestAnimFrame();
        }
    }
}

void CImageBrowserApp::Shutdown() {

    CProcessingThreadPool::This().StopAllThreads();
    // 顺序：provider 持有所有 CJPEGImage（含预读缓存），先放它，再放文件列表与监视器
    if (provider_) {
        provider_->ClearAllRequests();
        delete provider_;
        provider_ = nullptr;
    }
    image_ = nullptr;
    if (fileList_) {
        delete fileList_;
        fileList_ = nullptr;
    }
    if (watcher_) {
        watcher_->Abort();
        delete watcher_;
        watcher_ = nullptr;
    }
    if (gdiplusToken_) {
        GdiplusShutdown(gdiplusToken_);
        gdiplusToken_ = 0;
    }
}


CAppModule _Module;

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR lpCmdLine, int) {
    _Module.Init(nullptr, hInst);
    // 默认系统定时器精度 15.6ms，会把 17ms 的动画帧拖到 ~32ms；动画要准，开 1ms。
    timeBeginPeriod(1);

    std::wstring startup;
    if (lpCmdLine && *lpCmdLine) {
        std::wstring raw(lpCmdLine);
        if (raw.size() >= 2 && raw.front() == L'"' && raw.back() == L'"') {
            raw = raw.substr(1, raw.size() - 2);
        }
        while (!raw.empty() && raw.front() == L' ') raw.erase(raw.begin());
        if (!raw.empty() && raw.front() == L'"') {
            size_t q = raw.find(L'"', 1);
            if (q != std::wstring::npos) raw = raw.substr(1, q - 1);
        } else {
            size_t sp = raw.find(L' ');
            if (sp != std::wstring::npos) raw = raw.substr(0, sp);
        }
        startup = raw;
    }

    if (!g_app.Create(hInst, startup)) {
        timeEndPeriod(1);
        return 1;
    }
    int rc = g_app.Run();
    timeEndPeriod(1);
    _Module.Term();
    return rc;
}























































































