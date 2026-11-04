// copytips.cpp
// 极简剪贴板提示器：复制文本 -> 屏幕底部居中弹出圆角胶囊 "✓ Copied"，2 秒后消失。
// 构建（与 README 一致，必须保持可用）：
//   rc copytips.rc
//   cl /EHsc /O2 /MT /DUNICODE /D_UNICODE copytips.cpp copytips.res user32.lib gdi32.lib shell32.lib advapi32.lib
//
// 设计要点：单文件零依赖（仅 user32/gdi32/shell32/advapi32）；DWM 圆角用 LoadLibraryW 动态加载，
// 不新增导入库；弹窗懒创建且全局唯一，重复复制只重置定时器 + 重绘；DIB/DC/字体/托盘图标全部缓存
// 复用；逐像素 alpha（32bpp 顶向下 DIB + UpdateLayeredWindow）实现平滑圆角与柔和投影。
// 触发判定（2.3.0 两种模式）：
//   宽松（默认）：剪贴板出现「非空文本且内容与上次提示过的不完全相同」就提示。内容指纹 + 长度 + 500ms
//                 窗口做去重，因此选中即复制、脚本写入同样会提示——这是 2.3.0 明确采用的产品口径：
//                 与其去猜「用户到底按没按复制键」，不如只认「剪贴板里确实换了一段新文本」这个可观测事实。
//   严格（COPYTIPS_MODE=strict）：2.2.1 的意图驱动。三个旁证信号任一成立才算复制意图：
//                 ① 键盘钩子捕获 Ctrl+C/X/Insert、Shift+Delete；② 鼠标钩子捕获右键抬起（覆盖右键菜单的
//                 「复制」）；③ 40ms 轮询 GetAsyncKeyState（读整个桌面的物理键状态，不受 UIPI 限制，
//                 可兜住管理员窗口里的按键）。再叠加「写入者是前台进程」与「内容非空」两道校验。
// 两种模式的钩子与旁证采样完全相同，strict 只是把判定链换成 2.2.1 那一套，便于 A/B 对照与回退。
// 诊断日志（2.2.1 起）：环境变量 COPYTIPS_DEBUG / 命令行 /debug、--debug / exe 旁放 copytips.debug
// 任一命中即开启，逐事件写入 %TEMP%\copytips.log。日志只记录观测值，绝不参与判定；
// 默认关闭时不采集、不格式化、不写盘（主路径只多一次 ~200 字节的栈清零）。
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

/* 触发判定参数 */
#define INTENT_WINDOW_MS      400   // 复制快捷键按下后，多久内的剪贴板更新算作它的结果（真实复制通常 <50ms）
#define INTENT_WINDOW_RDP_MS  1500  // 远程桌面剪贴板重定向明显更慢，此时放宽窗口
#define MENU_INTENT_WINDOW_MS 2500  // 右键抬起后，多久内的剪贴板更新算作菜单里的「复制」
#define INTENT_POLL_MS        40    // GetAsyncKeyState 轮询间隔（UIPI 兜底，见 PollCopyCombo）
#define CLIP_OPEN_TRIES       5     // 打开剪贴板的重试次数（它常被别的进程短暂占用）
#define CLIP_OPEN_WAIT_MS     10    // 每次重试之间的等待（最多 ~50ms，不能再长：会拖住 UI 线程）
#define TIMER_INTENT_POLL     2     // 主窗口上的意图轮询定时器 id（TIMER_TOAST=1 属于弹窗窗口）

/* 内容指纹去重（2.3.0 宽松模式）：判定「这次是不是一段没提示过的新文本」 */
#define CONTENT_DEDUPE_MS  500        // 同一内容在此窗口内重复写入只提示一次
#define CONTENT_HASH_MAX   (1u << 20) // 参与哈希的最大字符数：超长文本也不让 UI 线程走长循环

/* 诊断日志参数（2.2.1）：只在开关打开时才会用到 */
#define LOG_MAX_LINE       2048                // 单行上限（宽字符）
#define LOG_MAX_FORMATS    8                   // 单个事件最多记录的格式数
#define LOG_HEAD_CHARS     32                  // 文本头截取的字符数
#define LOG_SCAN_MAX_CHARS (1024u * 1024u)     // 文本长度统计上限：超大剪贴板也不让 UI 线程走长循环
#define LOG_ROTATE_BYTES   (2 * 1024 * 1024)   // 超过 2 MB 时把旧日志轮转为 copytips.log.1
#define LOG_FORMAT_RTF     0xC9                // "Rich Text Format"，无标准宏

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

// 复制意图状态：键盘钩子 + 鼠标钩子 + 轮询兜底 + 剪贴板序列号去重
static HHOOK     g_hKeyHook         = nullptr; // nullptr = 键盘钩子未装上
static HHOOK     g_hMouseHook       = nullptr; // nullptr = 鼠标钩子未装上
static DWORD     g_lastIntentMs     = 0;       // 最近一次复制快捷键的 GetTickCount()；0 = 从未
static DWORD     g_lastMenuIntentMs = 0;       // 最近一次右键抬起的 GetTickCount()；0 = 从未
static DWORD     g_lastClipSeq      = 0;       // 已处理过的剪贴板序列号
static bool      g_pollWasCopyCombo = false;   // 轮询用：上一次采样是否处于复制组合键（边沿检测）
static DWORD     g_intentWindowMs   = 0;       // 懒计算：本地 400ms / 远程桌面 1500ms

// 诊断日志状态（启动时算一次）与鼠标旁证（只记录，不参与判定）
// 大缓冲故意不写 `= {}`：静态存储期本身就零初始化，放 .bss 才不会让 exe 白胖 16 KB。
static bool      g_logOn        = false;                 // 默认关闭：关闭时不采样、不格式化、不写盘
static wchar_t   g_logPath[MAX_PATH];                    // %TEMP%\copytips.log
static wchar_t   g_logBody[LOG_MAX_LINE];                // 复用缓冲：只在 UI 线程写，无需同步
static wchar_t   g_logLine[LOG_MAX_LINE];
static char      g_logUtf8[LOG_MAX_LINE * 4];
static DWORD     g_lastLUpMs    = 0;                     // 最近一次左键抬起；0 = 从未
static DWORD     g_lastRUpMs    = 0;                     // 最近一次右键抬起；0 = 从未
static bool      g_menuModeSeen = false;                 // 最近一次抬起时前台线程是否处于菜单模式
// 仅日志专用的一对时间戳：判定用的 g_last*Ms 在弹窗时会被清零，那会让
// 「唯一真的弹了的那些行」看不到是哪个信号触发的；这两份永不清零，只被日志读取。
static DWORD     g_logIntentSeenMs = 0;                  // 最近一次复制快捷键的 GetTickCount()；0 = 从未
static DWORD     g_logMenuSeenMs   = 0;                  // 最近一次右键抬起的 GetTickCount()；0 = 从未

// 触发模式（2.3.0）：启动时由环境变量决定一次，运行期不再变化
// 宽松模式的内容去重状态：hash + len 一起比较，避免不同文本撞哈希后互相吞掉提示
static ULONGLONG g_lastTextHash = 0;                     // 上次提示过的文本内容指纹（0 = 从未）
static unsigned  g_lastTextLen  = 0;                     // 与之配套的长度，防哈希碰撞误伤
static DWORD     g_lastTextMs   = 0;                     // 上次提示的时刻（仅宽松模式用于去重窗口）
static bool      g_strictMode   = false;                 // 启动时由 COPYTIPS_MODE=strict 决定；默认宽松

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

/* ---------------- 诊断日志（2.2.1：只观测，不参与判定） ---------------- */
// 目标：先把「微信里仅选中文字也弹窗」确诊为「微信确实写了剪贴板」还是「意图信号误触发」。
// 因此这里只做观测与落盘：所有字段都是当时的实测值，决定弹不弹的逻辑一行都没动。
enum Decision
{
    DECISION_SHOW = 0,
    DECISION_SKIP_SEQ,      // 序列号重复：同一次写入的重复通知
    DECISION_SKIP_NOINTENT, // 没有复制意图（键盘钩子 / 右键窗 / 轮询都没命中）
    DECISION_SKIP_OWNER,    // 写入者不是前台进程
    DECISION_SKIP_EMPTY,    // 无 CF_UNICODETEXT，或文本为空
    DECISION_SKIP_CLIPBUSY, // 5×10ms 都没能打开剪贴板
    DECISION_SKIP_DUPCONTENT // 宽松模式：与上次提示过的内容相同，且落在 CONTENT_DEDUPE_MS 窗口内
};

// 一次剪贴板读取采到的信息（日志关闭时不会填充，也不会多花任何开销）
struct ClipInfo
{
    bool openFailed = false;                    // OpenClipboard 重试耗尽（日志用来区分 SKIP_CLIPBUSY）
    int formatCount = 0;                        // 实际枚举到的格式数（最多 LOG_MAX_FORMATS）
    DWORD formats[LOG_MAX_FORMATS] = {};        // 格式号
    unsigned textLen = 0;                       // 文本字符数（受 LOG_SCAN_MAX_CHARS 限制）
    wchar_t head[LOG_HEAD_CHARS * 2 + 16] = {}; // 已转义的前 LOG_HEAD_CHARS 个字符
};

static const wchar_t* DecisionName(Decision d)
{
    switch (d)
    {
    case DECISION_SHOW:          return L"SHOW";
    case DECISION_SKIP_SEQ:      return L"SKIP_SEQ";
    case DECISION_SKIP_NOINTENT: return L"SKIP_NOINTENT";
    case DECISION_SKIP_OWNER:    return L"SKIP_OWNER";
    case DECISION_SKIP_EMPTY:    return L"SKIP_EMPTY";
    case DECISION_SKIP_CLIPBUSY: return L"SKIP_CLIPBUSY";
    case DECISION_SKIP_DUPCONTENT: return L"SKIP_DUPCONTENT";
    }
    return L"?";
}

// 开关一：环境变量存在且值不为 "0"（含「存在但为空」的情况）
static bool EnvSwitchOn()
{
    wchar_t v[32] = {};
    const DWORD cap = (DWORD)(sizeof(v) / sizeof(v[0]));
    SetLastError(ERROR_SUCCESS);
    const DWORD n = GetEnvironmentVariableW(L"COPYTIPS_DEBUG", v, cap);
    if (n >= cap) return true;                           // 值比缓冲还长：必然不是 "0"
    if (n == 0) return GetLastError() == ERROR_SUCCESS;  // 变量存在但为空 -> 视为开启
    if (n == 1 && v[0] == L'0') return false;            // 显式 "0" -> 关闭
    return true;
}

// 开关二：命令行含 /debug 或 --debug（不区分大小写）。
// 只认「参数起始位置」，否则 exe 放在 .../mydebug/ 这类目录里会被误开。
static bool CmdLineSwitchOn()
{
    const wchar_t* cmd = GetCommandLineW();
    if (!cmd) return false;
    static const wchar_t kSlash[] = L"/debug";
    static const wchar_t kDash[] = L"--debug";
    const size_t slashLen = (sizeof(kSlash) / sizeof(kSlash[0])) - 1;
    const size_t dashLen = (sizeof(kDash) / sizeof(kDash[0])) - 1;
    for (const wchar_t* p = cmd; *p; ++p)
    {
        if (p != cmd && p[-1] != L' ' && p[-1] != L'\t') continue; // 不是参数开头
        if (_wcsnicmp(p, kSlash, slashLen) == 0) return true;
        if (_wcsnicmp(p, kDash, dashLen) == 0) return true;
    }
    return false;
}

// 开关三：exe 同目录存在 copytips.debug
static bool DebugFileOn()
{
    wchar_t path[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) return false;
    const wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return false;
    static const wchar_t kName[] = L"copytips.debug";
    const size_t dirLen = (size_t)(slash - path) + 1; // 含结尾反斜杠
    const size_t nameLen = sizeof(kName) / sizeof(kName[0]); // 含结尾 0
    if (dirLen + nameLen > MAX_PATH) return false;
    wmemcpy(path + dirLen, kName, nameLen);
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

// 启动时调用一次：三路开关任一命中即开启，并把日志路径缓存下来
static void InitDebugLog()
{
    g_logOn = EnvSwitchOn() || CmdLineSwitchOn() || DebugFileOn();
    if (!g_logOn) return;
    wchar_t dir[MAX_PATH] = {};
    const DWORD n = GetTempPathW(MAX_PATH, dir); // 末尾自带反斜杠
    static const wchar_t kName[] = L"copytips.log";
    const size_t nameLen = sizeof(kName) / sizeof(kName[0]);
    if (n == 0 || n >= MAX_PATH || (size_t)n + nameLen > MAX_PATH) { g_logOn = false; return; }
    wmemcpy(dir + n, kName, nameLen);
    wmemcpy(g_logPath, dir, (size_t)n + nameLen);
}

// 追加一行：新建文件先写 UTF-8 BOM（记事本里中文才不乱码），超过 2 MB 先把旧文件轮转为 .1。
// 全程 best-effort：任何一步失败都静默返回，绝不抛异常、绝不影响主流程。
static void LogFlush(const wchar_t* line)
{
    if (!g_logOn || !line || !*line) return;
    // 1) 轮转：必须在打开之前判断，否则会把旧内容带进新文件
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (GetFileAttributesExW(g_logPath, GetFileExInfoStandard, &fad)
            && (fad.nFileSizeHigh != 0 || fad.nFileSizeLow > (DWORD)LOG_ROTATE_BYTES))
    {
        wchar_t bak[MAX_PATH] = {};
        const size_t n = wcslen(g_logPath);
        static const wchar_t kSuffix[] = L".1";
        const size_t suffixLen = sizeof(kSuffix) / sizeof(kSuffix[0]);
        if (n + suffixLen <= MAX_PATH)
        {
            wmemcpy(bak, g_logPath, n);
            wmemcpy(bak + n, kSuffix, suffixLen);
            MoveFileExW(g_logPath, bak, MOVEFILE_REPLACE_EXISTING);
        }
    }
    // 2) 追加写入（用完即关：日志量极小，不长期占句柄）
    HANDLE h = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size = {};
    if (GetFileSizeEx(h, &size) && size.QuadPart == 0)
    {
        const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
        DWORD written = 0;
        WriteFile(h, bom, sizeof(bom), &written, nullptr);
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, line, -1, g_logUtf8, (int)sizeof(g_logUtf8),
                                          nullptr, nullptr);
    if (bytes > 1) // 返回值含结尾 0，写入时去掉
    {
        DWORD written = 0;
        WriteFile(h, g_logUtf8, (DWORD)(bytes - 1), &written, nullptr);
        WriteFile(h, "\r\n", 2, &written, nullptr);
    }
    CloseHandle(h);
}

// 极简字符串构造器：本项目不引入 CRT 的 printf 引擎（未知格式串会连带拉进浮点格式化代码，
// exe 会从 118 KB 涨到 157 KB），日志行全部手工拼接，每一步都做上界检查。
struct LogBuf
{
    wchar_t* p = nullptr;
    size_t cap = 0; // 含结尾 0
    size_t len = 0;
};

static void BufInit(LogBuf& b, wchar_t* buf, size_t cch)
{
    b.p = buf; b.cap = cch; b.len = 0;
    if (cch > 0) buf[0] = 0;
}

static void BufCh(LogBuf& b, wchar_t c)
{
    if (!b.p || b.len + 1 >= b.cap) return; // 满了就丢弃剩余内容，绝不越界写
    b.p[b.len++] = c;
    b.p[b.len] = 0;
}

static void BufStr(LogBuf& b, const wchar_t* s)
{
    if (!s) return;
    while (*s) BufCh(b, *s++);
}

// 无符号十进制，不足 minDigits 位左侧补 0
static void BufUint(LogBuf& b, unsigned v, int minDigits)
{
    wchar_t tmp[12] = {};
    int n = 0;
    do { tmp[n++] = (wchar_t)(L'0' + (v % 10u)); v /= 10u; } while (v != 0 && n < 11);
    for (; n < minDigits && n < 11; ++n) tmp[n] = L'0';
    while (n > 0) BufCh(b, tmp[--n]);
}

// 无符号十六进制（0xC9 这类非标准格式号）
static void BufHex(LogBuf& b, unsigned v, int minDigits)
{
    static const wchar_t kDigits[] = L"0123456789ABCDEF";
    wchar_t tmp[12] = {};
    int n = 0;
    do { tmp[n++] = kDigits[v & 0xFu]; v >>= 4; } while (v != 0 && n < 11);
    for (; n < minDigits && n < 11; ++n) tmp[n] = L'0';
    BufCh(b, L'0'); BufCh(b, L'x');
    while (n > 0) BufCh(b, tmp[--n]);
}

// 无符号 64 位十六进制，不足 minDigits 位左侧补 0，**不带 0x 前缀**（日志里按 %016llX 的口径写）
static void BufHex64(LogBuf& b, ULONGLONG v, int minDigits)
{
    static const wchar_t kDigits[] = L"0123456789ABCDEF";
    wchar_t tmp[17] = {};
    int n = 0;
    do { tmp[n++] = kDigits[(unsigned)(v & 0xFull)]; v >>= 4; } while (v != 0 && n < 16);
    for (; n < minDigits && n < 16; ++n) tmp[n] = L'0';
    while (n > 0) BufCh(b, tmp[--n]);
}

// [本地时间.毫秒] + 正文，然后落盘
static void LogWriteBody(const wchar_t* body)
{
    if (!g_logOn || !body) return;
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    LogBuf b;
    BufInit(b, g_logLine, LOG_MAX_LINE);
    BufCh(b, L'[');
    BufUint(b, (unsigned)st.wYear, 4);   BufCh(b, L'-');
    BufUint(b, (unsigned)st.wMonth, 2);  BufCh(b, L'-');
    BufUint(b, (unsigned)st.wDay, 2);    BufCh(b, L' ');
    BufUint(b, (unsigned)st.wHour, 2);   BufCh(b, L':');
    BufUint(b, (unsigned)st.wMinute, 2); BufCh(b, L':');
    BufUint(b, (unsigned)st.wSecond, 2); BufCh(b, L'.');
    BufUint(b, (unsigned)st.wMilliseconds, 3);
    BufStr(b, L"] ");
    BufStr(b, body);
    LogFlush(g_logLine);
}

// 启动行：钩子成败是排查「权限 / UIPI」类问题的第一手材料，所以在两个钩子都装好之后才写
static void LogStartup()
{
    if (!g_logOn) return;
    LogBuf b;
    BufInit(b, g_logBody, LOG_MAX_LINE);
    BufStr(b, L"[startup] ver=" APP_VERSION_WSTR L" mode=");
    BufStr(b, g_strictMode ? L"strict" : L"loose"); // A/B 对照时，日志第一行就要能看出用的是哪套判定
    BufStr(b, L" dpi=");
    BufUint(b, (unsigned)g_dpi, 1);
    BufStr(b, L" rdp=");
    BufUint(b, GetSystemMetrics(SM_REMOTESESSION) ? 1u : 0u, 1);
    BufStr(b, L" keyhook=");
    BufUint(b, g_hKeyHook ? 1u : 0u, 1);
    BufStr(b, L" mousehook=");
    BufUint(b, g_hMouseHook ? 1u : 0u, 1);
    BufStr(b, L" debug=1 log=");
    BufStr(b, g_logPath);
    LogWriteBody(g_logBody);
}

// 距某意图时间戳的毫秒差；该时间戳为 0（从未）时写 -1
static void FmtDelta(wchar_t* out, size_t cch, DWORD ts)
{
    LogBuf b;
    BufInit(b, out, cch);
    if (ts == 0) { BufStr(b, L"-1"); return; }
    BufUint(b, (unsigned)(GetTickCount() - ts), 1); // 无符号差，跨回绕正确
}

// 窗口 -> "类名#pid"；无窗口写 "none#0"
static void FmtWin(wchar_t* out, size_t cch, HWND w)
{
    LogBuf b;
    BufInit(b, out, cch);
    if (!w) { BufStr(b, L"none#0"); return; }
    wchar_t cls[64] = {};
    if (GetClassNameW(w, cls, (int)(sizeof(cls) / sizeof(cls[0]))) <= 0) cls[0] = 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    BufStr(b, cls[0] ? cls : L"?");
    BufCh(b, L'#');
    BufUint(b, (unsigned)pid, 1);
}

// 格式号列表：1:TEXT / 13:UNICODETEXT / 15:HDROP / 16:LOCALE / 0xC9:RTF，其余只写数字
static void FmtFormats(wchar_t* out, size_t cch, const ClipInfo& info)
{
    LogBuf b;
    BufInit(b, out, cch);
    if (info.formatCount <= 0) { BufStr(b, L"-"); return; }
    for (int i = 0; i < info.formatCount; ++i)
    {
        if (i > 0) BufCh(b, L',');
        const DWORD f = info.formats[i];
        if (f == (DWORD)LOG_FORMAT_RTF) BufHex(b, (unsigned)f, 2); // 非标准宏，写十六进制便于对照
        else                            BufUint(b, (unsigned)f, 1);
        if (f == (DWORD)CF_TEXT)             BufStr(b, L":TEXT");
        else if (f == (DWORD)CF_UNICODETEXT) BufStr(b, L":UNICODETEXT");
        else if (f == (DWORD)CF_HDROP)       BufStr(b, L":HDROP");
        else if (f == (DWORD)CF_LOCALE)      BufStr(b, L":LOCALE");
        else if (f == (DWORD)LOG_FORMAT_RTF) BufStr(b, L":RTF");
    }
}

// 文本头：\r \n \t 与双引号转义；超过 32 字符（或被扫描上限截断）补 ...
static void FmtHead(wchar_t* out, size_t cch, const wchar_t* text, size_t len)
{
    if (cch == 0) return;
    size_t o = 0;
    out[0] = 0;
    const size_t take = (len < (size_t)LOG_HEAD_CHARS) ? len : (size_t)LOG_HEAD_CHARS;
    for (size_t i = 0; i < take && o + 3 < cch; ++i)
    {
        switch (text[i])
        {
        case L'\r': out[o++] = L'\\'; out[o++] = L'r'; break;
        case L'\n': out[o++] = L'\\'; out[o++] = L'n'; break;
        case L'\t': out[o++] = L'\\'; out[o++] = L't'; break;
        case L'"':  out[o++] = L'\\'; out[o++] = L'"'; break;
        default:    out[o++] = text[i]; break;
        }
    }
    if (len > take && o + 3 < cch) { out[o++] = L'.'; out[o++] = L'.'; out[o++] = L'.'; }
    out[o] = 0;
}

// 一行事件记录。字段全部是当时的实测值，decision 反映真实分支（不是写死的）。
static void LogClipboardEvent(DWORD seq, const ClipInfo& info, Decision d, ULONGLONG hash)
{
    if (!g_logOn) return;
    wchar_t dtKey[16] = {}, dtMenu[16] = {}, dtLUp[16] = {}, dtRUp[16] = {};
    wchar_t owner[96] = {}, fg[96] = {}, fmts[160] = {};
    // dtKey / dtMenu 读的是「仅日志、永不清零」的那一对副本：判定用的时间戳在弹窗时会被清零，
    // 若直接读它，SHOW 行（唯一真的弹了的行）永远只会显示 -1。
    FmtDelta(dtKey, sizeof(dtKey) / sizeof(dtKey[0]), g_logIntentSeenMs);
    FmtDelta(dtMenu, sizeof(dtMenu) / sizeof(dtMenu[0]), g_logMenuSeenMs);
    FmtDelta(dtLUp, sizeof(dtLUp) / sizeof(dtLUp[0]), g_lastLUpMs);
    FmtDelta(dtRUp, sizeof(dtRUp) / sizeof(dtRUp[0]), g_lastRUpMs);
    const HWND ownerWnd = GetClipboardOwner();
    FmtWin(owner, sizeof(owner) / sizeof(owner[0]), ownerWnd);
    FmtWin(fg, sizeof(fg) / sizeof(fg[0]), GetForegroundWindow());
    FmtFormats(fmts, sizeof(fmts) / sizeof(fmts[0]), info);

    LogBuf b;
    BufInit(b, g_logBody, LOG_MAX_LINE);
    BufStr(b, L"seq=");         BufUint(b, (unsigned)seq, 1);
    BufStr(b, L" dtKey=");      BufStr(b, dtKey);
    BufStr(b, L" dtMenu=");     BufStr(b, dtMenu);
    BufStr(b, L" dtLUp=");      BufStr(b, dtLUp);
    BufStr(b, L" dtRUp=");      BufStr(b, dtRUp);
    BufStr(b, L" mFlag=");      BufStr(b, g_menuModeSeen ? L"menu" : L"nomenu");
    BufStr(b, L" owner=");      BufStr(b, owner);
    BufStr(b, L" ownerVis=");   BufUint(b, IsWindowVisible(ownerWnd) ? 1u : 0u, 1); // 只记录，不参与判定
    BufStr(b, L" fg=");         BufStr(b, fg);
    BufStr(b, L" fmt=");        BufStr(b, fmts);
    BufStr(b, L" hash=");       BufHex64(b, hash, 16); // 内容指纹：与下文 len 一起构成去重键
    BufStr(b, L" len=");        BufUint(b, info.textLen, 1);
    BufStr(b, L" head=\"");     BufStr(b, info.head);
    BufStr(b, L"\" decision="); BufStr(b, DecisionName(d));
    LogWriteBody(g_logBody);
}

/* ---------------- 复制意图判定 ---------------- */
// 为什么需要这一层：WM_CLIPBOARDUPDATE 只能说明「剪贴板被某个进程写入」，并不能说明「用户复制了」。
// 开启「选中即复制」的终端/词典/浏览器扩展、剪贴板管理器与云同步、输入法、脚本 Set-Clipboard
// 都会写入剪贴板；只探测格式是否可用就弹窗（旧实现）必然在这些场景下误弹。
// 现在改为「意图驱动」：钩子只记录复制动作发生的时刻（快捷键 / 右键菜单 / 轮询兜底），
// 剪贴板更新落在时间窗内、写入者又是前台进程、内容确实非空，才认为一次复制动作完成。
static bool IsCopyCombo(const KBDLLHOOKSTRUCT& kb)
{
    // 只比较 DWORD，避免 /W4 下的有符号/无符号比较警告
    static const DWORD kKeyC      = (DWORD)'C';       // Ctrl+C
    static const DWORD kKeyX      = (DWORD)'X';       // Ctrl+X（剪切同样写入剪贴板）
    static const DWORD kKeyInsert = (DWORD)VK_INSERT; // Ctrl+Insert（Unix 习惯，等价于复制）
    static const DWORD kKeyDel    = (DWORD)VK_DELETE; // Shift+Delete（资源管理器剪切）
    const DWORD vk = kb.vkCode;
    // 用 GetAsyncKeyState 现取修饰键状态：不需要自己维护按键状态机，
    // 也不会因为「钩子安装前 Ctrl 就已按下」而漏判。只要 Ctrl 在按，Shift 组合（终端 Ctrl+Shift+C）也算复制。
    if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0)
        return vk == kKeyC || vk == kKeyX || vk == kKeyInsert;
    if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0)
        return vk == kKeyDel;
    return false;
}

// 低层键盘钩子回调必须极轻：只做位判断与整数比较，绝不 I/O、不加锁、不创建窗口。
// 任何阻塞都会拖慢全系统的键盘输入（系统对超时的钩子会直接摘掉）。
static LRESULT CALLBACK KeyHookProc(int code, WPARAM w, LPARAM l)
{
    if (code == HC_ACTION && (w == WM_KEYDOWN || w == WM_SYSKEYDOWN))
    {
        const KBDLLHOOKSTRUCT* kb = reinterpret_cast<const KBDLLHOOKSTRUCT*>(l);
        if (kb && IsCopyCombo(*kb))
        {
            g_lastIntentMs = GetTickCount();
            g_logIntentSeenMs = g_lastIntentMs; // 仅日志，永不清零
        }
    }
    return CallNextHookEx(g_hKeyHook, code, w, l);
}

// 鼠标钩子：右键抬起只记一个时间戳，不去猜菜单窗口类名
//（Win11/Office/Chrome 的自绘菜单都不是传统 #32768，按类名判断会漏；按动作时间判断则全都覆盖）
static LRESULT CALLBACK MouseHookProc(int code, WPARAM w, LPARAM l)
{
    if (code == HC_ACTION && (w == WM_RBUTTONUP || w == WM_LBUTTONUP))
    {
        const DWORD now = GetTickCount(); // 判定用的时间戳：与 2.2.0 完全一致（仍在抬起瞬间取值）
        if (w == WM_RBUTTONUP)
        {
            g_lastMenuIntentMs = now; g_lastRUpMs = now;
            g_logMenuSeenMs = g_lastMenuIntentMs; // 仅日志，永不清零
        }
        else { g_lastLUpMs = now; }
        // 诊断旁证（只记录，不参与判定）：抬起瞬间前台线程是否处于某种菜单模式。
        // 日志关闭时连这次跨线程查询都不做，保持 2.2.0 的开销。
        if (g_logOn)
        {
            bool menu = false;
            const HWND fg = GetForegroundWindow();
            const DWORD tid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
            GUITHREADINFO gti = {};
            gti.cbSize = sizeof(gti);
            if (tid != 0 && GetGUIThreadInfo(tid, &gti))
                menu = (gti.flags & (GUI_INMENUMODE | GUI_SYSTEMMENUMODE | GUI_POPUPMENUMODE)) != 0;
            g_menuModeSeen = menu; // 取不到就保持 nomenu
        }
    }
    return CallNextHookEx(g_hMouseHook, code, w, l);
}

// UIPI 兜底：低层键盘钩子收不到发往更高完整性级别（管理员）窗口的按键，
// 而 GetAsyncKeyState 读的是整个桌面的物理键状态，不受 UIPI 限制。
// 只认「按下沿」：长按 Ctrl+C 不会把时间戳一直往后推。
static void PollCopyCombo()
{
    const int vkC = 'C', vkX = 'X';
    const bool ctrl  = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool hit = (ctrl && ((GetAsyncKeyState(vkC) & 0x8000) != 0 ||
                               (GetAsyncKeyState(vkX) & 0x8000) != 0 ||
                               (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0))
                     || (shift && (GetAsyncKeyState(VK_DELETE) & 0x8000) != 0);
    if (hit && !g_pollWasCopyCombo)
    {
        g_lastIntentMs = GetTickCount();
        g_logIntentSeenMs = g_lastIntentMs; // 仅日志，永不清零
    }
    g_pollWasCopyCombo = hit;
}

// 时间窗：远程桌面下剪贴板重定向慢，窗口放宽（只在首次调用时探测一次）
static DWORD IntentWindowMs()
{
    if (g_intentWindowMs == 0)
        g_intentWindowMs = GetSystemMetrics(SM_REMOTESESSION) ? INTENT_WINDOW_RDP_MS : INTENT_WINDOW_MS;
    return g_intentWindowMs;
}

// 时间窗判定：GetTickCount 约 49.7 天回绕一次，无符号减法天然跨回绕正确
static bool HasRecentCopyIntent()
{
    if (!g_hKeyHook && !g_hMouseHook) return true; // 两个钩子都没装上（极少见）：退化为宽松判定，保证工具仍可用
    const DWORD now = GetTickCount();
    if (g_lastIntentMs != 0 && (DWORD)(now - g_lastIntentMs) <= IntentWindowMs()) return true;
    if (g_lastMenuIntentMs != 0 && (DWORD)(now - g_lastMenuIntentMs) <= (DWORD)MENU_INTENT_WINDOW_MS) return true;
    return false;
}

// 剪贴板里是否真有非空文本：真正打开并读取，而不是只探测格式。
// info == nullptr  -> 与 2.2.0 完全相同的浅读：只看首字符，O(1)，不枚举格式、不读文本头。
// info != nullptr  -> 日志开启：在同一次 OpenClipboard 里顺带采集格式列表与文本头。
// 所有路径（成功 / 失败 / 早退）都必须 GlobalUnlock（仅在 GlobalLock 成功时）并 CloseClipboard，
// 否则会锁死全系统的剪贴板。
static bool ClipboardHasText(ClipInfo* info)
{
    for (int i = 0; i < CLIP_OPEN_TRIES; ++i)
    {
        if (!OpenClipboard(nullptr)) { Sleep(CLIP_OPEN_WAIT_MS); continue; } // 被占用：小步退避
        bool hasText = false;
        if (info)
        {
            // 1) 格式列表：EnumClipboardFormats 从 0 开始迭代，最多记 LOG_MAX_FORMATS 个
            UINT fmt = 0;
            while (info->formatCount < LOG_MAX_FORMATS && (fmt = EnumClipboardFormats(fmt)) != 0)
                info->formats[info->formatCount++] = fmt;
            // 2) 文本：非空判定与浅读同义（wcsnlen 停在 0，等价于「首字符 != 0」），
            //    只是额外记下长度与前 32 字符；扫描长度受缓冲区大小与 LOG_SCAN_MAX_CHARS 双重限制。
            if (IsClipboardFormatAvailable(CF_UNICODETEXT))
            {
                HANDLE data = GetClipboardData(CF_UNICODETEXT);
                const wchar_t* text = data ? static_cast<const wchar_t*>(GlobalLock(data)) : nullptr;
                if (text)
                {
                    SIZE_T maxChars = GlobalSize(data) / sizeof(wchar_t); // 缓冲区上界，绝不越界读
                    if (maxChars == 0) maxChars = 1;
                    if (maxChars > (SIZE_T)LOG_SCAN_MAX_CHARS) maxChars = (SIZE_T)LOG_SCAN_MAX_CHARS;
                    const size_t n = wcsnlen(text, maxChars);
                    hasText = (n > 0);
                    info->textLen = (unsigned)n;
                    FmtHead(info->head, sizeof(info->head) / sizeof(info->head[0]), text, n);
                    GlobalUnlock(data); // 拿到就一定要解锁，否则句柄被永久锁住
                }
            }
        }
        else if (IsClipboardFormatAvailable(CF_UNICODETEXT))
        {
            HANDLE data = GetClipboardData(CF_UNICODETEXT);
            const wchar_t* text = data ? static_cast<const wchar_t*>(GlobalLock(data)) : nullptr;
            if (text)
            {
                hasText = (text[0] != L'\0'); // 只看首字符判空：再大的文本也是 O(1)，不复制内容
                GlobalUnlock(data);           // 拿到就一定要解锁，否则句柄被永久锁住
            }
        }
        CloseClipboard(); // 无论读取成功与否都必须关闭，否则整个系统的剪贴板会被锁死
        return hasText;
    }
    if (info) info->openFailed = true; // 5×10ms 仍被占用：本次不提示（宁可漏报，也绝不误报）
    return false;
}

// 宽松模式的判定读取：在一次 OpenClipboard 内完成「格式列表（仅日志）+ 非空文本 + 内容指纹」。
// 与 ClipboardHasText 的差别只在「要不要算指纹」，重试节奏、扫描上限、句柄纪律完全一致。
// info == nullptr 表示日志关闭：只算指纹，不枚举格式、不读文本头（不产生任何日志相关的开销）。
// 所有路径（成功 / 失败 / 空文本）都恰好 GlobalUnlock 一次（仅在 GlobalLock 成功时）并 CloseClipboard 一次。
static bool ReadClipboardText(ULONGLONG* hash, unsigned* len, ClipInfo* info)
{
    if (hash) *hash = 0;
    if (len)  *len = 0;
    for (int i = 0; i < CLIP_OPEN_TRIES; ++i)
    {
        if (!OpenClipboard(nullptr)) { Sleep(CLIP_OPEN_WAIT_MS); continue; } // 被占用：小步退避
        bool hasText = false;
        if (info)
        {
            // 1) 格式列表：与 2.2.1 的采集口径完全一致（EnumClipboardFormats 从 0 开始迭代）
            UINT fmt = 0;
            while (info->formatCount < LOG_MAX_FORMATS && (fmt = EnumClipboardFormats(fmt)) != 0)
                info->formats[info->formatCount++] = fmt;
        }
        // 2) 文本：长度 / 头 32 字符 / 内容指纹，全部在同一次打开里取完
        if (IsClipboardFormatAvailable(CF_UNICODETEXT))
        {
            HANDLE data = GetClipboardData(CF_UNICODETEXT);
            const wchar_t* text = data ? static_cast<const wchar_t*>(GlobalLock(data)) : nullptr;
            if (text)
            {
                SIZE_T maxChars = GlobalSize(data) / sizeof(wchar_t); // 缓冲区上界，绝不越界读
                if (maxChars == 0) maxChars = 1;
                if (maxChars > (SIZE_T)LOG_SCAN_MAX_CHARS) maxChars = (SIZE_T)LOG_SCAN_MAX_CHARS;
                const size_t n = wcsnlen(text, maxChars);
                if (n > 0)
                {
                    // FNV-1a 64：按 UTF-16 code unit 逐 2 字节（低字节在前）累积，最多 CONTENT_HASH_MAX 个字符。
                    // 超长文本只有头部参与指纹：两段百万字符以上的文本若仅尾部不同会被判为同一内容。
                    // 这个取舍是刻意的——剪贴板事件跑在 UI 线程上，绝不能在这里走长循环。
                    const size_t hmax = (n < (size_t)CONTENT_HASH_MAX) ? n : (size_t)CONTENT_HASH_MAX;
                    ULONGLONG h = 14695981039346656037ULL; // FNV-1a 64 offset basis
                    for (size_t k = 0; k < hmax; ++k)
                    {
                        h ^= (ULONGLONG)((unsigned)text[k] & 0xFFu);        h *= 1099511628211ULL; // prime
                        h ^= (ULONGLONG)(((unsigned)text[k] >> 8) & 0xFFu); h *= 1099511628211ULL;
                    }
                    if (hash) *hash = h;
                    if (len)  *len  = (unsigned)n;
                    if (info)
                    {
                        info->textLen = (unsigned)n;
                        FmtHead(info->head, sizeof(info->head) / sizeof(info->head[0]), text, n);
                    }
                    hasText = true;
                }
                GlobalUnlock(data); // 拿到就一定要解锁，否则句柄被永久锁住
            }
        }
        CloseClipboard(); // 无论读取成功与否都必须关闭，否则整个系统的剪贴板会被锁死
        return hasText;
    }
    if (info) info->openFailed = true; // 5×10ms 仍被占用：本次不提示（宁可漏报，也绝不误报）
    return false;
}

// 写入者必须是前台进程：云剪贴板同步、输入法、剪贴板管理器的程序化写入，owner 都不是前台进程。
// 取不到 owner（NULL）或前台窗口时不做否决——宁可放行，也不误杀正常复制。
static bool ClipboardOwnerIsForeground()
{
    const HWND owner = GetClipboardOwner();
    if (!owner) return true;
    const HWND fg = GetForegroundWindow();
    if (!fg) return true;
    DWORD pidOwner = 0, pidFg = 0;
    GetWindowThreadProcessId(owner, &pidOwner);
    GetWindowThreadProcessId(fg, &pidFg);
    if (pidOwner == 0 || pidFg == 0) return true; // 查不到 PID：不做否决
    return pidOwner == pidFg;
}

// 触发模式：只有 COPYTIPS_MODE=strict 才是严格模式（2.2.1 的意图驱动），其它任何值 / 未设置都走宽松。
// 只读 16 个字符就够区分 "strict"，不会被异常长的环境变量拖住启动。
static void InitPromptMode()
{
    wchar_t buf[16] = {};
    const DWORD n = GetEnvironmentVariableW(L"COPYTIPS_MODE", buf, 16);
    g_strictMode = (n == 6 && _wcsicmp(buf, L"strict") == 0); // 其它任何值/未设置 = 宽松
}

// WM_CLIPBOARDUPDATE 的统一入口。两种模式共用同一段骨架：序列号去重 -> 读内容 -> 各自的否决条件 -> 提示。
//   宽松（默认）：内容非空、且与上次提示过的内容不同（或已过 CONTENT_DEDUPE_MS）就提示。
//   严格（strict）：去重 -> 意图 -> 写入者 -> 内容，与 2.2.1 逐条一致。
// 日志仍然只是「算出 decision 后写一行」，关闭时这一步整体不存在。
static void OnClipboardUpdate()
{
    const DWORD seq = GetClipboardSequenceNumber(); // 每次写入 +1，用于丢弃重复通知（0 = 取不到，不参与比较）
    const bool dup = (seq != 0 && seq == g_lastClipSeq);
    if (!dup) g_lastClipSeq = seq; // 与 2.2.0 相同：只在非重复时更新（重复值本来就相等）

    Decision d = DECISION_SHOW;
    ClipInfo info;         // 只在日志开启时被填充
    bool clipRead = false; // 本次事件是否已经读过剪贴板（保证每个事件最多 OpenClipboard 一次）
    ULONGLONG h = 0;       // 内容指纹（宽松模式判重用；提到函数作用域，SHOW 时要用它更新去重状态）
    unsigned len = 0;      // 与指纹配套的长度，防不同文本撞哈希

    if (dup) d = DECISION_SKIP_SEQ;
    else
    {
        clipRead = true;
        if (!ReadClipboardText(&h, &len, g_logOn ? &info : nullptr))
            d = (g_logOn && info.openFailed) ? DECISION_SKIP_CLIPBUSY : DECISION_SKIP_EMPTY;
        else if (g_strictMode && !HasRecentCopyIntent())        d = DECISION_SKIP_NOINTENT;
        else if (g_strictMode && !ClipboardOwnerIsForeground()) d = DECISION_SKIP_OWNER;
        else if (!g_strictMode && g_lastTextMs != 0 && len == g_lastTextLen && h == g_lastTextHash &&
                 (DWORD)(GetTickCount() - g_lastTextMs) <= (DWORD)CONTENT_DEDUPE_MS)
            d = DECISION_SKIP_DUPCONTENT; // 同一内容在去重窗口内被重复写入：只提示一次
    }

    if (d == DECISION_SHOW)
    {
        // 宽松模式去重：记住「这次提示的是什么内容、什么时候提示的」；严格模式下这三行只是无害的旁路记录
        g_lastTextHash = h; g_lastTextLen = len; g_lastTextMs = GetTickCount();
        g_lastIntentMs = 0;     // 消费掉这次意图：同一窗口内的后续写入不再二次弹窗
        g_lastMenuIntentMs = 0;
    }
    if (g_logOn)
    {
        // 早退路径（重复通知）在判定链里根本不碰剪贴板；这里为定位问题补读一次，
        // 只读、立即关闭，结果不参与任何判定，且每个事件仍只 OpenClipboard 一次。
        // 注意：日志取的是 g_log*SeenMs（永不清零），所以放在清零之后也不会丢时间差。
        if (!clipRead) ClipboardHasText(&info);
        LogClipboardEvent(seq, info, d, h);
    }
    if (d == DECISION_SHOW) ShowToast();
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
        // 剪贴板被写入 ≠ 用户复制了：交给 OnClipboardUpdate 做意图与内容校验
        OnClipboardUpdate();
        return 0;
    case WM_TIMER:
        if (w == TIMER_INTENT_POLL) { PollCopyCombo(); return 0; }
        break;
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
    InitDebugLog(); // 诊断日志开关只在启动时算一次并缓存；关闭时不采样、不格式化、不写盘，只剩一次 ~200 B 栈清零
    InitPromptMode(); // 触发模式同理：只在启动时读一次 COPYTIPS_MODE，运行期不再变化
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
    // 复制意图的三个信号源：键盘钩子（快捷键）、鼠标钩子（右键菜单）、定时轮询（UIPI 兜底）。
    // 低层钩子无 DLL 注入，要求安装线程带消息循环（下面即是）。安装失败不致命：
    // 全部失败时 HasRecentCopyIntent() 会退化为宽松判定，工具仍可用。
    g_hKeyHook   = SetWindowsHookExW(WH_KEYBOARD_LL, KeyHookProc, g_hInst, 0);
    g_hMouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc, g_hInst, 0);
    SetTimer(g_hwndMain, TIMER_INTENT_POLL, INTENT_POLL_MS, nullptr); // 定时器随主窗口销毁自动清除
    LogStartup(); // 诊断日志打开时写一行启动信息（关闭时该函数立刻返回）
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
    // 统一释放：钩子 -> 窗口 -> DIB/DC -> 字体 -> 图标（轮询定时器已随主窗口销毁自动清除）
    if (g_hKeyHook) { UnhookWindowsHookEx(g_hKeyHook); g_hKeyHook = nullptr; }
    if (g_hMouseHook) { UnhookWindowsHookEx(g_hMouseHook); g_hMouseHook = nullptr; }
    if (g_toast.hwnd) { DestroyWindow(g_toast.hwnd); g_toast.hwnd = nullptr; }
    ReleaseDib();
    if (g_toast.fText) { DeleteObject(g_toast.fText); g_toast.fText = nullptr; }
    if (g_toast.fIcon) { DeleteObject(g_toast.fIcon); g_toast.fIcon = nullptr; }
    if (g_hIconTray && g_iconOwned) DestroyIcon(g_hIconTray); // 仅销毁自绘图标
    g_hIconTray = nullptr;
    if (mutex) CloseHandle(mutex);
    return 0;
}
