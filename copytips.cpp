// copytips.cpp
// 极简剪贴板提示器：复制文本 -> 屏幕底部居中弹出圆角胶囊 "✓ Copied"，2 秒后消失。
// 构建（与 README 一致，必须保持可用）：
//   rc copytips.rc
//   cl /EHsc /O2 /MT /DUNICODE /D_UNICODE copytips.cpp copytips.res user32.lib gdi32.lib shell32.lib advapi32.lib
//
// 设计要点：单文件零依赖（仅 user32/gdi32/shell32/advapi32）；DWM 圆角用 LoadLibraryW 动态加载，
// 不新增导入库；弹窗懒创建且全局唯一，重复复制只重置定时器 + 重绘；DIB/DC/字体/托盘图标全部缓存
// 复用；逐像素 alpha（32bpp 顶向下 DIB + UpdateLayeredWindow）实现平滑圆角与柔和投影。
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601 // AddClipboardFormatListener 等需要 Vista+
#include <windows.h>
#include <shellapi.h>
#include <wchar.h>
#include <string.h>
#include <math.h>

#include "version.h"   // 版本号唯一来源
#ifdef _MSC_VER
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33 // Win11 SDK 才有，缺失时自行定义
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif
#define WM_TRAY        (WM_USER + 1)
#define ID_EXIT        1000
#define ID_SETTINGS    1001
#define ID_AUTORUN     1002
#define IDD_SETTINGS   100
#define IDC_CHK_RUN    101
#define TIMER_TOAST    1
#define TOAST_MS       2000
#define TRAY_ICON_ID   1
#define TRAY_ICON_SIZE 32

/* 视觉规格：96 DPI 下的逻辑值，运行时按 DPI 缩放 */
#define L_PILL_H      32  // 胶囊高度
#define L_RADIUS      10  // 圆角半径
#define L_PAD_L       12  // 左内边距
#define L_PAD_R       14  // 右内边距
#define L_ICON_W      12  // 图标宽度
#define L_GAP         7   // 图文间距
#define L_FONT_PX     13  // 正文字号
#define L_SHADOW_M    7   // 投影留白
#define L_SHADOW_DY   2   // 投影垂直偏移
#define L_SHADOW_BLUR 5   // 投影模糊半径
#define L_BOTTOM_GAP  16  // 距任务栏的最小间距
#define L_MIN_PILL_W  84  // 胶囊最小宽度
#define L_TRAY_RADIUS 26  // 托盘图标圆角（独立于胶囊，避免缩小胶囊时图标变方）

/* 颜色与不透明度 */
#define C_BG     RGB(32, 33, 40)    // 胶囊主体
#define C_TEXT   RGB(245, 245, 247) // 正文
#define C_ICON   RGB(74, 222, 128)  // 图标
#define A_BG     (168.0f / 255.0f)  // 主体 ~66%（半透明）
#define A_BORDER (34.0f / 255.0f)   // 描边 ~13%
#define A_SHADOW (58.0f / 255.0f)   // 投影（随主体变淡而减淡）

static const wchar_t kWndMain[]    = L"CopytipsMain";
static const wchar_t kWndToast[]   = L"CopytipsToast";
#define kAppName L"Copytips"  // 字面量而非数组：便于与版本号拼成相邻宽字符串常量
static const wchar_t kRegPath[]    = L"Software\\Copytips";
static const wchar_t kRegRun[]     = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t kRegVal[]     = L"AutoRun";
static const wchar_t kText[]       = L"Copied";
static const wchar_t kGlyphCheck[] = L"\x2713"; // ✓

static HINSTANCE g_hInst     = nullptr;
static HWND      g_hwndMain  = nullptr; // 隐藏的 message-only 窗口
static HICON     g_hIconTray = nullptr; // 托盘图标
static bool      g_iconOwned = false;   // 仅自绘图标才可 DestroyIcon（系统共享图标不可销毁）
static int       g_dpi       = 96;      // 系统 DPI，声明 DPI 感知后即固定
static NOTIFYICONDATAW g_nid;           // 托盘图标数据

// 弹窗状态：窗口 + DIB 缓存 + 字体缓存，全部懒创建并复用
struct ToastState
{
    HWND hwnd = nullptr;
    HDC memDC = nullptr;
    HBITMAP bmp = nullptr, bmpOld = nullptr;
    BYTE* bits = nullptr;
    int w = 0, h = 0;         // 当前 DIB 尺寸
    int pillW = 0, pillH = 0; // 当前胶囊尺寸
    int dpi = 0;              // 当前尺寸/字体对应的 DPI
    HFONT fText = nullptr, fIcon = nullptr;
};
static ToastState g_toast;

/* ---------------- DPI ---------------- */
static void RefreshDpi()
{
    HDC hdc = GetDC(nullptr);
    if (!hdc) return;
    const int dpi = GetDeviceCaps(hdc, LOGPIXELSY);
    ReleaseDC(nullptr, hdc);
    if (dpi >= 48 && dpi <= 480) g_dpi = dpi;
}
static int Dpi(int v) { return MulDiv(v, g_dpi, 96); } // 逻辑值 -> 物理像素

/* ---------------- 注册表：开机自启 ---------------- */
static bool GetAutoRun()
{
    bool on = false;
    HKEY h = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegPath, 0, KEY_READ, &h) == ERROR_SUCCESS)
    {
        DWORD val = 0, sz = sizeof(val), type = 0;
        if (RegQueryValueExW(h, kRegVal, nullptr, &type, reinterpret_cast<BYTE*>(&val), &sz)
                == ERROR_SUCCESS && type == REG_DWORD)
            on = (val != 0);
        RegCloseKey(h);
    }
    return on;
}

static void SetAutoRun(bool on)
{
    HKEY h = nullptr;
    // 1) 记录状态，供托盘菜单勾选态使用
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegPath, 0, nullptr, REG_OPTION_NON_VOLATILE,
                        KEY_WRITE, nullptr, &h, nullptr) == ERROR_SUCCESS)
    {
        const DWORD val = on ? 1u : 0u;
        RegSetValueExW(h, kRegVal, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&val), sizeof(val));
        RegCloseKey(h);
    }
    // 2) 写入 / 删除 Run 项
    h = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegRun, 0, nullptr, REG_OPTION_NON_VOLATILE,
                        KEY_SET_VALUE, nullptr, &h, nullptr) != ERROR_SUCCESS)
        return;
    wchar_t exe[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (!on)
    {
        RegDeleteValueW(h, kAppName);
    }
    else if (n > 0 && n < MAX_PATH)
    {
        // 路径含空格时必须加引号，否则系统无法定位可执行文件
        const size_t len = wcslen(exe);
        wchar_t quoted[MAX_PATH + 2];
        quoted[0] = L'"'; wmemcpy(quoted + 1, exe, len); quoted[len + 1] = L'"';
        quoted[len + 2] = 0;
        RegSetValueExW(h, kAppName, 0, REG_SZ, reinterpret_cast<const BYTE*>(quoted),
                       static_cast<DWORD>((len + 2) * sizeof(wchar_t)));
    }
    RegCloseKey(h);
}

/* ---------------- 圆角矩形 SDF / 覆盖率 ---------------- */
// 点到圆角矩形的有符号距离：<0 在内，>0 在外
static float RoundRectSdf(float px, float py, float l, float t, float r, float b, float rad)
{
    const float cx = (l + r) * 0.5f, cy = (t + b) * 0.5f;
    const float hx = (r - l) * 0.5f, hy = (b - t) * 0.5f;
    const float qx = fabsf(px - cx) - (hx - rad);
    const float qy = fabsf(py - cy) - (hy - rad);
    const float mx = qx > 0.0f ? qx : 0.0f, my = qy > 0.0f ? qy : 0.0f;
    return sqrtf(mx * mx + my * my) + fminf(fmaxf(qx, qy), 0.0f) - rad;
}
// 覆盖率：1px 抗锯齿过渡带
static float RoundRectCov(float px, float py, float l, float t, float r, float b, float rad)
{
    const float c = 0.5f - RoundRectSdf(px, py, l, t, r, b, rad);
    return c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c);
}
static float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
// 0..1 的预乘分量 -> 0..255
static BYTE ToByte(float v)
{
    const float c = v * 255.0f + 0.5f;
    return c <= 0.0f ? (BYTE)0 : (c >= 255.0f ? (BYTE)255 : (BYTE)c);
}
// 预乘 alpha 的 "over" 合成：把 (r,g,b,a) 叠到 (R,G,B,A) 之上
static void Over(float& R, float& G, float& B, float& A,
                 float r, float g, float b, float a)
{
    R = r * a + R * (1.0f - a);
    G = g * a + G * (1.0f - a);
    B = b * a + B * (1.0f - a);
    A = a + A * (1.0f - a);
}

/* ---------------- 字体 ---------------- */
// 必须用 ANTIALIASED_QUALITY：ClearType 会在分层窗口上产生彩边并污染 alpha 通道
static HFONT MakeFont(const wchar_t* face, int pixelHeight, int weight)
{
    return CreateFontW(-pixelHeight, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, face);
}

// 仅在首次渲染或 DPI 变化时重建字体并重算胶囊尺寸
static void EnsureMetrics()
{
    if (g_toast.dpi == g_dpi && g_toast.fText && g_toast.fIcon && g_toast.pillW > 0) return;
    if (g_toast.fText) { DeleteObject(g_toast.fText); g_toast.fText = nullptr; }
    if (g_toast.fIcon) { DeleteObject(g_toast.fIcon); g_toast.fIcon = nullptr; }
    g_toast.fText = MakeFont(L"Segoe UI", Dpi(L_FONT_PX), FW_SEMIBOLD);
    g_toast.fIcon = MakeFont(L"Segoe UI Symbol", Dpi(L_FONT_PX), FW_SEMIBOLD);
    g_toast.pillH = Dpi(L_PILL_H);
    int textW = 0; // 文本宽度只需量一次（内容恒定）
    HDC hdc = GetDC(nullptr);
    if (hdc)
    {
        HGDIOBJ old = SelectObject(hdc, g_toast.fText);
        SIZE sz = { 0, 0 };
        if (GetTextExtentPoint32W(hdc, kText, (int)wcslen(kText), &sz)) textW = sz.cx;
        SelectObject(hdc, old);
        ReleaseDC(nullptr, hdc);
    }
    const int minW = Dpi(L_MIN_PILL_W);
    const int pillW = Dpi(L_PAD_L) + Dpi(L_ICON_W) + Dpi(L_GAP) + textW + Dpi(L_PAD_R);
    g_toast.pillW = pillW < minW ? minW : pillW;
    g_toast.dpi = g_dpi;
}

/* ---------------- 32bpp 顶向下 DIB 段 ---------------- */
// 创建 DIB 段 + 兼容内存 DC；调用方负责 DeleteObject / DeleteDC
static bool NewSection(int w, int h, HBITMAP* bmp, HDC* mem, BYTE** px)
{
    // 32bpp、负数高度 = 顶向下（行号与 y 一致）
    BITMAPINFO bi = {};
    BITMAPINFOHEADER& bh = bi.bmiHeader;
    bh.biSize = sizeof(BITMAPINFOHEADER); bh.biWidth = w; bh.biHeight = -h;
    bh.biPlanes = 1; bh.biBitCount = 32; bh.biCompression = BI_RGB;
    HDC screen = GetDC(nullptr);
    if (!screen) return false;
    void* p = nullptr;
    *bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &p, nullptr, 0);
    *mem = (*bmp && p) ? CreateCompatibleDC(screen) : nullptr;
    ReleaseDC(nullptr, screen);
    if (*bmp && p && *mem) { *px = static_cast<BYTE*>(p); return true; }
    if (*mem) DeleteDC(*mem);
    if (*bmp) DeleteObject(*bmp);
    *mem = nullptr; *bmp = nullptr;
    return false;
}
static void ReleaseDib()
{
    if (g_toast.memDC)
    {
        if (g_toast.bmpOld) SelectObject(g_toast.memDC, g_toast.bmpOld);
        DeleteDC(g_toast.memDC);
        g_toast.memDC = nullptr;
    }
    g_toast.bmpOld = nullptr;
    if (g_toast.bmp) { DeleteObject(g_toast.bmp); g_toast.bmp = nullptr; }
    g_toast.bits = nullptr;
    g_toast.w = g_toast.h = 0;
}
static bool CreateDib(int w, int h)
{
    if (!NewSection(w, h, &g_toast.bmp, &g_toast.memDC, &g_toast.bits)) return false;
    g_toast.bmpOld = (HBITMAP)SelectObject(g_toast.memDC, g_toast.bmp);
    g_toast.w = w;
    g_toast.h = h;
    return true;
}

/* ---------------- 弹窗渲染 ---------------- */
// 把 "✓ Copied" 以「黑底白字」画进 DIB：白色像素的灰度正好等于该像素的覆盖率。
// GDI 不写 alpha 通道，只有这样才能拿到平滑的抗锯齿边缘。
static void DrawContentToDib(int M, int pillW, int pillH)
{
    HDC mem = g_toast.memDC;
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, RGB(255, 255, 255));
    const int iconLeft = M + Dpi(L_PAD_L);
    const int textLeft = iconLeft + Dpi(L_ICON_W) + Dpi(L_GAP);
    HGDIOBJ oldFont = SelectObject(mem, g_toast.fIcon);
    // 左右各留 3px：不同 DPI / 字体回退下 ✓ 的字形可能略超图标格，避免被裁掉
    RECT rcIcon = { iconLeft - 3, M, iconLeft + Dpi(L_ICON_W) + 3, M + pillH };
    DrawTextW(mem, kGlyphCheck, -1, &rcIcon, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(mem, g_toast.fText);
    RECT rcText = { textLeft, M, M + pillW - Dpi(L_PAD_R), M + pillH };
    DrawTextW(mem, kText, -1, &rcText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(mem, oldFont);
}

// 逐像素 alpha 合成：投影 -> 主体 -> 1px 描边 -> 内容，自下而上按预乘 "over" 累加。
static void CompositeToPremultipliedBgra(int winW, int winH, int M, int pillW, int pillH)
{
    const COLORREF bg = C_BG, tc = C_TEXT, ic = C_ICON;
    // 颜色分量归一化到 0..1，与 alpha 同量纲，才能按预乘 alpha 正确累加
    const float k = 1.0f / 255.0f;
    const float bgR = GetRValue(bg) * k, bgG = GetGValue(bg) * k, bgB = GetBValue(bg) * k;
    const float tcR = GetRValue(tc) * k, tcG = GetGValue(tc) * k, tcB = GetBValue(tc) * k;
    const float icR = GetRValue(ic) * k, icG = GetGValue(ic) * k, icB = GetBValue(ic) * k;
    const float fM = (float)M, fW = (float)pillW, fH = (float)pillH;
    const float fRadius = (float)Dpi(L_RADIUS), fBlur = (float)Dpi(L_SHADOW_BLUR);
    const float fDy = (float)Dpi(L_SHADOW_DY);
    // 图标 / 正文的着色分界；两者之间的空白像素覆盖率恒为 0，分界点不敏感
    const int iconSplit = M + Dpi(L_PAD_L) + Dpi(L_ICON_W) + Dpi(L_GAP);
    BYTE* bits = g_toast.bits;
    for (int y = 0; y < winH; ++y)
    {
        const float fy = (float)y + 0.5f;
        for (int x = 0; x < winW; ++x)
        {
            const float fx = (float)x + 0.5f;
            const size_t i = (size_t)y * (size_t)winW + (size_t)x;
            // 内容覆盖率：白字黑底；取三通道均值以兼容灰度/子像素两种文字平滑
            const float cov = (bits[i * 4] + bits[i * 4 + 1] + bits[i * 4 + 2]) / 765.0f;
            const float pc = RoundRectCov(fx, fy, fM, fM, fM + fW, fM + fH, fRadius);
            // 内描边：内缩 1.5px。实测最外侧那颗像素被外部 AA 与内环分掉，
            // 内缩 0.5px 只有半强度(+12)、1.0px 满强度但只占 1px(+23)，
            // 1.5px 既保满强度(+23)又向内延伸一格(+11)，放大后是一条清晰的高光细线。
            const float inner = RoundRectCov(fx, fy, fM + 1.5f, fM + 1.5f,
                                             fM + fW - 1.5f, fM + fH - 1.5f, fRadius - 1.5f);
            // 投影：形状外扩 1px 并下移 dy，再用 blur 柔化
            const float dShadow = RoundRectSdf(fx, fy - fDy, fM - 1.0f, fM - 1.0f,
                                               fM + fW + 1.0f, fM + fH + 1.0f, fRadius + 1.0f);
            const float sc = Clamp01(0.5f - dShadow / fBlur);
            const bool isIcon = (x < iconSplit);
            float A = 0.0f, R = 0.0f, G = 0.0f, B = 0.0f;
            Over(R, G, B, A, 0.0f, 0.0f, 0.0f, A_SHADOW * sc);              // 1) 投影
            Over(R, G, B, A, bgR, bgG, bgB, A_BG * pc);                     // 2) 主体
            Over(R, G, B, A, 1.0f, 1.0f, 1.0f,                           // 3) 内描边
                 A_BORDER * fmaxf(0.0f, pc - inner));
            Over(R, G, B, A, isIcon ? icR : tcR, isIcon ? icG : tcG,        // 4) 内容
                 isIcon ? icB : tcB, cov);
            bits[i * 4 + 0] = ToByte(B); // 小端 BGRA，且已是预乘值
            bits[i * 4 + 1] = ToByte(G);
            bits[i * 4 + 2] = ToByte(R);
            bits[i * 4 + 3] = ToByte(A);
        }
    }
}

// 弹窗窗口只创建一次，之后永远复用（不反复建窗、不重复注册窗口类）
static void EnsureToastWindow()
{
    if (g_toast.hwnd) return;
    g_toast.hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
        WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, // TRANSPARENT：鼠标点击穿透
        kWndToast, nullptr, WS_POPUP, 0, 0, 0, 0, // 不带 WS_VISIBLE，显示用 SW_SHOWNOACTIVATE
        nullptr, nullptr, g_hInst, nullptr);
}

// 取「可用区域底边」。任务栏自动隐藏（ABS_AUTOHIDE）时 SPI_GETWORKAREA 不预留空间、
// 直接返回整屏，会让胶囊压到任务栏上；此时改用 Shell_TrayWnd 的真实矩形。
// 注意：自动隐藏的任务栏会整体滑出屏幕（本机隐藏时 rect 为 0,1078-1920,1126），
// 其 top 不能直接用，要用「屏幕底 - 任务栏厚度」还原它弹出后占据的顶边。
// 另：SHAppBarMessage(ABM_GETTASKBARPOS) 在本机返回无效数据，故不采用。
static int BottomSafeLine()
{
    const int screenW = GetSystemMetrics(SM_CXSCREEN);
    const int screenH = GetSystemMetrics(SM_CYSCREEN);
    int safe = screenH;
    RECT wa = {};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0) && wa.bottom > 0 && wa.bottom < safe)
        safe = wa.bottom; // 任务栏未自动隐藏：工作区底边本身就是安全线
    RECT tr = {};
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!tray || !GetWindowRect(tray, &tr)) return safe;
    const int thick = (int)(tr.bottom - tr.top); // 任务栏厚度：隐藏/弹出两种状态下不变
    // 必须同时满足：左右贴满、厚度合理、且纵向位于屏幕下半区
    // —— 顶部停靠的任务栏横向也贴满（0,0-1920,48），只能用纵向位置排除
    const bool bottomDocked = tr.left <= 0 && tr.right >= screenW
                              && thick > 0 && thick < screenH / 2
                              && tr.top > screenH / 2;
    const int revealedTop = screenH - thick;     // 它弹出后占据的顶边
    if (bottomDocked && revealedTop > 0 && revealedTop < safe) safe = revealedTop;
    return safe;
}

// 渲染 + 定位 + 投放；尺寸与 DPI 不变时完全复用已有 DIB 与字体
static bool RenderToast()
{
    EnsureMetrics();
    const int M = Dpi(L_SHADOW_M);
    const int pillW = g_toast.pillW, pillH = g_toast.pillH;
    const int winW = pillW + 2 * M, winH = pillH + 2 * M;
    if (!g_toast.bmp || g_toast.w != winW || g_toast.h != winH)
    {
        ReleaseDib();
        if (!CreateDib(winW, winH)) return false;
    }
    memset(g_toast.bits, 0, (size_t)winW * (size_t)winH * 4); // 全黑，alpha = 0
    DrawContentToDib(M, pillW, pillH);
    CompositeToPremultipliedBgra(winW, winH, M, pillW, pillH);
    // 位置：可用区域底边上方 L_BOTTOM_GAP，水平居中（水平方向仍用工作区，竖屏/侧边栏任务栏下也正确）
    RECT wa = {};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) // 任务栏（工作区）
    {
        wa.right = GetSystemMetrics(SM_CXSCREEN); wa.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    const int safeBottom = BottomSafeLine();
    POINT dst = { wa.left + (wa.right - wa.left - pillW) / 2 - M,
                  safeBottom - Dpi(L_BOTTOM_GAP) - pillH - M };
    SIZE size = { winW, winH };
    POINT src = { 0, 0 };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    return UpdateLayeredWindow(g_toast.hwnd, nullptr, &dst, &size, g_toast.memDC,
                               &src, 0, &blend, ULW_ALPHA) != FALSE;
}
static LRESULT CALLBACK ToastProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m)
    {
    case WM_ERASEBKGND: return 1; // 画面完全由分层位图提供，不需要擦除背景
    case WM_NCHITTEST: return HTTRANSPARENT; // 点击穿透的补强
    case WM_TIMER:
        if (w == TIMER_TOAST) { KillTimer(h, TIMER_TOAST); ShowWindow(h, SW_HIDE); } // 隐藏而非销毁
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
static void ShowToast()
{
    EnsureToastWindow();
    if (!g_toast.hwnd) return;
    if (!RenderToast()) return; // 渲染失败静默跳过，绝不影响复制本身
    // 复位定时器：快速连按 Ctrl+C 只重置倒计时，不会堆叠出多个窗口/定时器
    if (!SetTimer(g_toast.hwnd, TIMER_TOAST, TOAST_MS, nullptr))
    {
        ShowWindow(g_toast.hwnd, SW_HIDE); // 定时器建不出来宁可不显示，也不能永久卡在屏幕上
        return;
    }
    ShowWindow(g_toast.hwnd, SW_SHOWNOACTIVATE);
}

/* ---------------- DWM 圆角（动态加载，不新增导入库） ---------------- */
static void ApplyRoundedCorners(HWND hwnd)
{
    typedef HRESULT(WINAPI * PfnSetAttr)(HWND, DWORD, LPCVOID, DWORD);
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll"); // Win11 才支持；Win10 上静默跳过
    if (!dwm) return;
    PfnSetAttr setAttr = (PfnSetAttr)(void*)GetProcAddress(dwm, "DwmSetWindowAttribute");
    if (setAttr) { const int pref = DWMWCP_ROUND; setAttr(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref)); }
    FreeLibrary(dwm);
}

/* ---------------- 设置对话框 ---------------- */
static INT_PTR CALLBACK SettingsProc(HWND h, UINT m, WPARAM w, LPARAM)
{
    if (m == WM_INITDIALOG) // 勾选态同步 + Win11 圆角
    {
        SetWindowTextW(h, L"Copytips " APP_VERSION_WSTR); // 标题带版本，方便确认跑的是哪一版
        CheckDlgButton(h, IDC_CHK_RUN, GetAutoRun() ? BST_CHECKED : BST_UNCHECKED);
        ApplyRoundedCorners(h);
        return TRUE;
    }
    if (m != WM_COMMAND) return FALSE;
    if (LOWORD(w) == IDOK)
    {
        SetAutoRun(IsDlgButtonChecked(h, IDC_CHK_RUN) == BST_CHECKED);
        EndDialog(h, IDOK);
        return TRUE;
    }
    if (LOWORD(w) == IDCANCEL) { EndDialog(h, IDCANCEL); return TRUE; }
    return FALSE;
}

/* ---------------- 托盘菜单 ---------------- */
static void ShowTrayMenu(HWND hwnd)
{
    const bool autoRun = GetAutoRun();
    HMENU m = CreatePopupMenu();
    if (!m) return;
    AppendMenuW(m, MF_STRING, ID_SETTINGS, L"Settings...");
    AppendMenuW(m, MF_STRING | (autoRun ? MF_CHECKED : 0), ID_AUTORUN, L"Auto run");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Exit");
    POINT pt = {};
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    const UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(m);
    if (cmd == ID_EXIT)          PostMessageW(hwnd, WM_CLOSE, 0, 0);
    else if (cmd == ID_SETTINGS) DialogBoxW(g_hInst, MAKEINTRESOURCEW(IDD_SETTINGS), hwnd, SettingsProc);
    else if (cmd == ID_AUTORUN)  SetAutoRun(!autoRun);
}

/* ---------------- 自绘托盘图标：圆角深色方块 + 翠绿对勾 ---------------- */
static HICON CreateTrayIcon(int size)
{
    if (size <= 0 || size > TRAY_ICON_SIZE) size = TRAY_ICON_SIZE;
    HBITMAP bmp = nullptr; HDC mem = nullptr; BYTE* px = nullptr;
    if (!NewSection(size, size, &bmp, &mem, &px)) return nullptr; // 失败由调用方回退
    memset(px, 0, (size_t)size * (size_t)size * 4);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    // 深色圆角方块（笔与刷同色，避免边缘出现一圈透明细线）
    const COLORREF bg = C_BG;
    const int rad = MulDiv(size, L_TRAY_RADIUS, 96);
    HBRUSH br = CreateSolidBrush(bg);
    HPEN pen = CreatePen(PS_SOLID, 1, bg);
    HGDIOBJ oldBr = SelectObject(mem, br), oldPen = SelectObject(mem, pen);
    RoundRect(mem, 0, 0, size + 1, size + 1, rad * 2, rad * 2);
    HFONT f = MakeFont(L"Segoe UI Symbol", MulDiv(size, 22, TRAY_ICON_SIZE), FW_SEMIBOLD);
    HGDIOBJ oldFont = SelectObject(mem, f);
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, C_ICON);
    RECT rc = { 0, 0, size, size };
    DrawTextW(mem, kGlyphCheck, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    // GDI 不写 alpha：用圆角 SDF 覆盖率补齐（内部 = 255，边缘抗锯齿）
    const float fRad = (float)rad, fSize = (float)size;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
            px[((size_t)y * (size_t)size + (size_t)x) * 4 + 3] =
                ToByte(RoundRectCov((float)x + 0.5f, (float)y + 0.5f, 0.0f, 0.0f, fSize, fSize, fRad));
    SelectObject(mem, oldFont); SelectObject(mem, oldPen);
    SelectObject(mem, oldBr); SelectObject(mem, oldBmp);
    DeleteObject(f); DeleteObject(pen); DeleteObject(br);
    DeleteDC(mem);
    // 掩码全 0 -> 完全不遮罩，透明度由颜色位图的 alpha 决定
    BYTE maskBits[TRAY_ICON_SIZE * 4] = {};
    HBITMAP mask = CreateBitmap(size, size, 1, 1, maskBits);
    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmColor = bmp;
    ii.hbmMask = mask;
    HICON icon = CreateIconIndirect(&ii);
    if (mask) DeleteObject(mask);
    DeleteObject(bmp);
    return icon;
}

/* ---------------- 主窗口 ---------------- */
static LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m)
    {
    case WM_CREATE: AddClipboardFormatListener(h); return 0;
    case WM_CLIPBOARDUPDATE:
        // 只查询格式是否可用，不 Open/Close 剪贴板：少一次系统调用，也不会阻塞其他程序
        if (IsClipboardFormatAvailable(CF_UNICODETEXT)) ShowToast();
        return 0;
    case WM_TRAY: if (l == WM_RBUTTONUP) ShowTrayMenu(h); return 0;
    case WM_COMMAND: if (LOWORD(w) == ID_EXIT) DestroyWindow(h); return 0;
    case WM_DESTROY:
        RemoveClipboardFormatListener(h); Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* ---------------- 入口 ---------------- */
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int)
{
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"CopytipsSingleInstance"); // 单实例
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(mutex); return 0; }
    // 系统级 DPI 感知：避免系统对整窗做位图拉伸（省内存，文字更清晰）。
    // 故意不用 per-monitor v2：资源对话框在 PMv2 下不会自动缩放，会带来额外复杂度。
    SetProcessDPIAware();
    RefreshDpi(); g_hInst = hInst;
    // 两个窗口类各注册一次（旧实现在每次复制时都重复注册弹窗类）
    WNDCLASSEXW wc = { sizeof(wc), 0, MainProc, 0, 0, hInst,
                       nullptr, nullptr, nullptr, nullptr, kWndMain, nullptr };
    if (!RegisterClassExW(&wc)) return 0;
    wc.lpfnWndProc = ToastProc;
    wc.lpszClassName = kWndToast;
    if (!RegisterClassExW(&wc)) return 0;
    // 主窗口用 message-only：不进任务栏/Alt+Tab，也不参与前台激活，是最省资源的选择。
    g_hwndMain = CreateWindowExW(0, kWndMain, L"", 0, 0, 0, 0, 0,
                                 HWND_MESSAGE, nullptr, hInst, nullptr);
    if (!g_hwndMain) return 0;
    g_hIconTray = CreateTrayIcon(GetSystemMetrics(SM_CXSMICON)); // 优先自绘，失败回退系统图标
    if (g_hIconTray) g_iconOwned = true;
    else g_hIconTray = LoadIconW(nullptr, IDI_APPLICATION);        // 共享图标：不可 DestroyIcon
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwndMain; g_nid.uID = TRAY_ICON_ID;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY; g_nid.hIcon = g_hIconTray;
    lstrcpynW(g_nid.szTip, kAppName L" " APP_VERSION_WSTR, (int)(sizeof(g_nid.szTip) / sizeof(g_nid.szTip[0])));
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    // 统一释放：窗口 -> DIB/DC -> 字体 -> 图标
    if (g_toast.hwnd) { DestroyWindow(g_toast.hwnd); g_toast.hwnd = nullptr; }
    ReleaseDib();
    if (g_toast.fText) { DeleteObject(g_toast.fText); g_toast.fText = nullptr; }
    if (g_toast.fIcon) { DeleteObject(g_toast.fIcon); g_toast.fIcon = nullptr; }
    if (g_hIconTray && g_iconOwned) DestroyIcon(g_hIconTray); // 仅销毁自绘图标
    g_hIconTray = nullptr;
    if (mutex) CloseHandle(mutex);
    return 0;
}
