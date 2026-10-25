// ============================================================================
//  RandomPicker —— 随机抽号器（单文件 Win32 / GDI，无第三方依赖）
//
//  主界面：起始号码 / 终止号码 / 抽中号码 / 开始
//  Shift + 点「开始」进入权重后台：每个号码可直接调到 0% ~ 100%，
//  总和恒为 100%，调高一个，其余按比例让出。
// ============================================================================

#define NOMINMAX
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
// Win7 起步：保证 GetTickCount64 / SetProcessDPIAware 等在 MinGW 下也可见
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
// WM_DPICHANGED 在 winuser.h 里由 WINVER >= 0x0603 控制，这里单独抬高
#ifndef WINVER
#define WINVER 0x0603
#endif

#include <windows.h>
#include <windowsx.h>

// 保险：某些精简 SDK / 老 MinGW 头文件里没有这个宏
#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <random>
#include <string>
#include <utility>
#include <vector>

#if defined(_MSC_VER)
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#endif

// ---------------------------------------------------------------- 常量

static const long long kMaxRange = 1000000;   // 最多一百万个号码

static const int ID_ED_START  = 1001;
static const int ID_ED_END    = 1002;
static const int ID_ED_RESULT = 1003;

static const UINT_PTR kTimerRoll = 1;

static const COLORREF C_BG      = RGB(238, 242, 249);
static const COLORREF C_CARD    = RGB(255, 255, 255);
static const COLORREF C_BORDER  = RGB(229, 233, 241);
static const COLORREF C_TEXT    = RGB(23, 27, 34);
static const COLORREF C_MUTED   = RGB(124, 132, 145);
static const COLORREF C_ACCENT  = RGB(59, 110, 246);
static const COLORREF C_ACC_DK  = RGB(43, 90, 224);
static const COLORREF C_ACC_LT  = RGB(236, 242, 255);
static const COLORREF C_SEL     = RGB(244, 248, 255);
static const COLORREF C_TRACK   = RGB(236, 240, 247);
static const COLORREF C_THUMB   = RGB(196, 205, 219);
static const COLORREF C_SHADOW  = RGB(224, 229, 238);
static const COLORREF C_WELL    = RGB(247, 250, 255);
static const COLORREF C_SEP     = RGB(241, 244, 250);

// ---------------------------------------------------------------- 状态

struct App
{
    HWND      hwnd   = nullptr;
    HINSTANCE hinst  = nullptr;

    int  page = 0;          // 0 = 主界面, 1 = 权重后台
    int  dpi  = 96;

    HWND edStart  = nullptr;
    HWND edEnd    = nullptr;
    HWND edResult = nullptr;

    long long startN = 1;
    long long endN   = 50;

    // pct[i] 对应号码 startN + i，单位是百分数，总和恒为 100
    long long             pStart = 0;
    long long             pEnd   = -1;
    std::vector<double>   pct;

    std::mt19937_64 rng;

    HFONT fTitle = nullptr, fLabel = nullptr, fInput = nullptr, fResult = nullptr;
    HFONT fBtn = nullptr, fRow = nullptr, fPct = nullptr;

    HBRUSH brCard = nullptr;

    // 主界面布局
    RECT rcTitle{}, rcStartLabel{}, rcEndLabel{}, rcStartEdit{}, rcEndEdit{};
    RECT rcCard{}, rcCardLabel{}, rcResultEdit{}, rcBtnStart{};

    // 后台布局
    RECT rcBkTitle{}, rcAll5{}, rcAll0{}, rcList{}, rcSave{};

    int          scrollRow = 0;
    int          selRow    = 0;    // 0 基
    bool         editing   = false;
    std::wstring typeBuf;

    int  pressBtn = 0;   // 1 = 开始, 2 = 保存, 3 = 均分, 4 = 清零
    int  hot      = 0;   // 悬停：同上编号
    bool tracking = false;
    int  dragRow  = -1;  // 正在拖拽拖动条的行

    bool      rolling   = false;
    ULONGLONG rollEnd   = 0;
    long long finalPick = 0;
};

static App G;

// ---------------------------------------------------------------- 小工具

static int S(int v) { return MulDiv(v, G.dpi, 96); }

static RECT Inflate(RECT r, int d)
{
    r.left -= d; r.top -= d; r.right += d; r.bottom += d;
    return r;
}

static void RoundFill(HDC dc, const RECT& r, int rad, COLORREF fill, COLORREF border)
{
    HBRUSH br = CreateSolidBrush(fill);
    HPEN   pn = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(dc, br);
    HGDIOBJ op = SelectObject(dc, pn);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, rad * 2, rad * 2);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(pn);
    DeleteObject(br);
}

// 扁平阴影：卡片下方露一条浅色边，营造层次
static void SoftShadow(HDC dc, const RECT& r, int rad)
{
    RECT s1 = { r.left + S(2), r.top + S(3), r.right - S(2), r.bottom + S(3) };
    RECT s2 = { r.left + S(6), r.top + S(6), r.right - S(6), r.bottom + S(6) };
    RoundFill(dc, s2, rad, RGB(232, 237, 246), RGB(232, 237, 246));
    RoundFill(dc, s1, rad, C_SHADOW, C_SHADOW);
}

static void TextOut2(HDC dc, const std::wstring& s, const RECT& r, HFONT f,
                     COLORREF c, UINT fmt)
{
    if (!f || s.empty()) return;
    HGDIOBJ    oldFont = SelectObject(dc, f);
    int        oldMode = SetBkMode(dc, TRANSPARENT);
    COLORREF   oldTxt  = SetTextColor(dc, c);
    RECT       rr      = r;
    DrawTextW(dc, s.c_str(), (int)s.size(), &rr, fmt | DT_NOPREFIX);
    SetTextColor(dc, oldTxt);
    SetBkMode(dc, oldMode);
    SelectObject(dc, oldFont);
}

static HFONT MakeFont(int pt, int weight, const wchar_t* face)
{
    int h = -MulDiv(pt, G.dpi, 72);
    return CreateFontW(h, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
}

static void MakeFonts()
{
    if (G.fTitle) DeleteObject(G.fTitle);
    if (G.fLabel) DeleteObject(G.fLabel);
    if (G.fInput) DeleteObject(G.fInput);
    if (G.fResult) DeleteObject(G.fResult);
    if (G.fBtn) DeleteObject(G.fBtn);
    if (G.fRow) DeleteObject(G.fRow);
    if (G.fPct) DeleteObject(G.fPct);

    const wchar_t* face = L"Microsoft YaHei UI";
    G.fTitle  = MakeFont(20, FW_BOLD,   face);
    G.fLabel  = MakeFont(11, FW_NORMAL, face);
    G.fInput  = MakeFont(18, FW_NORMAL, face);
    G.fResult = MakeFont(54, FW_BOLD,   face);
    G.fBtn    = MakeFont(19, FW_BOLD,   face);
    G.fRow    = MakeFont(15, FW_NORMAL, face);
    G.fPct    = MakeFont(13, FW_NORMAL, face);

    if (G.edStart)  SendMessageW(G.edStart,  WM_SETFONT, (WPARAM)G.fInput,  TRUE);
    if (G.edEnd)    SendMessageW(G.edEnd,    WM_SETFONT, (WPARAM)G.fInput,  TRUE);
    if (G.edResult) SendMessageW(G.edResult, WM_SETFONT, (WPARAM)G.fResult, TRUE);
}

// 百分比文本：大值取整，小值保留一位，极小显示 <0.1%
static std::wstring PctText(double v)
{
    if (v <= 0.0004) return L"0%";
    if (v < 0.05)    return L"<0.1%";

    if (v >= 10.0)
    {
        long long w = (long long)(v + 0.5);
        if (w >= 100) return L"100%";
        return std::to_wstring(w) + L"%";
    }

    long long whole = (long long)v;
    long long dec   = (long long)((v - (double)whole) * 10.0 + 0.5);
    if (dec >= 10) { dec = 0; whole += 1; }
    std::wstring s = std::to_wstring(whole);
    s.push_back(L'.');
    s.push_back((wchar_t)(L'0' + (int)dec));
    s.push_back(L'%');
    return s;
}

// ---------------------------------------------------------------- 号码 / 概率

static long long Count()
{
    return G.endN - G.startN + 1;
}

static void NormalizePct()
{
    double s = 0.0;
    for (double v : G.pct) s += v;
    if (s <= 1e-12) return;
    if (s > 99.98 && s < 100.02) return;
    double k = 100.0 / s;
    for (double& v : G.pct) v *= k;
}

// 按号码保留旧概率，重新铺开；新号码给平均值
static void RebuildPct()
{
    std::vector<double> nw;
    long long n = Count();
    if (n > 0 && n <= kMaxRange)
    {
        double avg = (double)100.0 / (double)n;
        if (!G.pct.empty())
        {
            double s = 0.0;
            for (double v : G.pct) s += v;
            if (s > 1e-9 && G.pct.size() > 0)
                avg = s / (double)G.pct.size();
        }

        nw.assign((size_t)n, avg);

        if (!G.pct.empty() && G.pStart <= G.pEnd)
        {
            long long a = std::max(G.startN, G.pStart);
            long long b = std::min(G.endN, G.pEnd);
            long long span = b - a + 1;
            for (long long k = 0; k < span; ++k)
            {
                long long v  = a + k;
                long long oi = v - G.pStart;
                if (oi >= 0 && oi < (long long)G.pct.size())
                    nw[(size_t)(v - G.startN)] = G.pct[(size_t)oi];
            }
        }
    }
    G.pct.swap(nw);
    G.pStart = G.startN;
    G.pEnd   = G.endN;
    NormalizePct();
}

// 把某一行设成 target%，其余按比例让出剩余份额
static void SetPercent(int row, double target)
{
    int n = (int)G.pct.size();
    if (row < 0 || row >= n) return;
    if (target < 0.0)   target = 0.0;
    if (target > 100.0) target = 100.0;

    if (n == 1) { G.pct[0] = 100.0; return; }

    double rest = 100.0 - target;
    double others = 0.0;
    for (int i = 0; i < n; ++i)
        if (i != row) others += G.pct[i];

    if (others <= 1e-9)
    {
        double share = rest / (double)(n - 1);
        for (int i = 0; i < n; ++i)
            if (i != row) G.pct[i] = share;
    }
    else
    {
        double k = rest / others;
        for (int i = 0; i < n; ++i)
            if (i != row) G.pct[i] *= k;
    }
    G.pct[row] = target;
}

static bool PickOne(long long& out)
{
    double total = 0.0;
    for (double v : G.pct) total += v;
    if (total <= 1e-9) return false;

    std::uniform_real_distribution<double> dist(0.0, total);
    double r = dist(G.rng);
    for (size_t i = 0; i < G.pct.size(); ++i)
    {
        if (r < G.pct[i]) { out = G.startN + (long long)i; return true; }
        r -= G.pct[i];
    }
    for (size_t i = G.pct.size(); i-- > 0;)
        if (G.pct[i] > 0.0) { out = G.startN + (long long)i; return true; }
    return false;
}

static bool ReadInt64(HWND ed, long long& out)
{
    wchar_t buf[64] = {};
    int n = GetWindowTextW(ed, buf, 64);
    if (n <= 0) return false;
    std::wstring s(buf, (size_t)n);
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return false;
    size_t b = s.find_last_not_of(L" \t\r\n");
    s = s.substr(a, b - a + 1);

    wchar_t* end = nullptr;
    long long v = std::wcstoll(s.c_str(), &end, 10);
    if (end == s.c_str() || *end != 0) return false;
    out = v;
    return true;
}

static bool ValidateRange(std::wstring& msg)
{
    long long a = 0, b = 0;
    if (!ReadInt64(G.edStart, a)) { msg = L"起始号码请填写整数"; return false; }
    if (!ReadInt64(G.edEnd, b))   { msg = L"终止号码请填写整数"; return false; }
    if (a > b)                    { msg = L"起始号码不能大于终止号码"; return false; }

    unsigned long long span = (unsigned long long)b - (unsigned long long)a;
    if (span >= (unsigned long long)kMaxRange)
    {
        msg = L"号码范围最多 1000000 个";
        return false;
    }

    G.startN = a;
    G.endN   = b;
    return true;
}

// ---------------------------------------------------------------- 布局

static void Layout()
{
    RECT rc = {};
    GetClientRect(G.hwnd, &rc);
    const int W   = rc.right;
    const int pad = S(36);
    const int cw  = W - pad * 2;

    G.rcTitle = { pad, S(26), pad + S(320), S(70) };

    if (G.page == 0)
    {
        int gap  = S(22);
        int colW = (cw - gap) / 2;

        G.rcStartLabel = { pad, S(98),  pad + colW, S(120) };
        G.rcEndLabel   = { pad + colW + gap, S(98), W - pad, S(120) };
        G.rcStartEdit  = { pad, S(124), pad + colW, S(186) };
        G.rcEndEdit    = { pad + colW + gap, S(124), W - pad, S(186) };

        G.rcCard       = { pad, S(214), W - pad, S(408) };
        G.rcCardLabel  = { G.rcCard.left + S(22), G.rcCard.top + S(16),
                           G.rcCard.right - S(22), G.rcCard.top + S(42) };
        G.rcResultEdit = { G.rcCard.left + S(14), G.rcCard.top + S(52),
                           G.rcCard.right - S(14), G.rcCard.bottom - S(16) };

        G.rcBtnStart = { pad, S(434), W - pad, S(502) };
    }
    else
    {
        G.rcBkTitle = { pad, S(26), pad + S(320), S(70) };
        G.rcAll5    = { W - pad - S(112) * 2 - S(12), S(32), W - pad - S(112) - S(12), S(68) };
        G.rcAll0    = { W - pad - S(112), S(32), W - pad, S(68) };
        G.rcList    = { pad, S(96), W - pad, S(452) };
        G.rcSave    = { pad, S(468), W - pad, S(524) };
    }
}

static void PlaceEdits()
{
    if (!G.edStart) return;

    auto place = [](HWND h, const RECT& r) {
        SetWindowPos(h, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    };
    place(G.edStart,  G.rcStartEdit);
    place(G.edEnd,    G.rcEndEdit);
    place(G.edResult, G.rcResultEdit);

    int show = (G.page == 0) ? SW_SHOW : SW_HIDE;
    ShowWindow(G.edStart,  show);
    ShowWindow(G.edEnd,    show);
    ShowWindow(G.edResult, show);

    if (G.page != 0) SetFocus(G.hwnd);
}

// ---------------------------------------------------------------- 绘制

static void PaintMain(HDC dc)
{
    HBRUSH bg = CreateSolidBrush(C_BG);
    RECT   full = {};
    GetClientRect(G.hwnd, &full);
    FillRect(dc, &full, bg);
    DeleteObject(bg);

    TextOut2(dc, L"随机抽号器", G.rcTitle, G.fTitle, C_TEXT,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    TextOut2(dc, L"起始号码", G.rcStartLabel, G.fLabel, C_MUTED,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    TextOut2(dc, L"终止号码", G.rcEndLabel, G.fLabel, C_MUTED,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    bool fStart = (GetFocus() == G.edStart);
    bool fEnd   = (GetFocus() == G.edEnd);

    RECT r1 = Inflate(G.rcStartEdit, S(2));
    RECT r2 = Inflate(G.rcEndEdit, S(2));
    RoundFill(dc, r1, S(14), C_CARD, fStart ? C_ACCENT : C_BORDER);
    RoundFill(dc, r2, S(14), C_CARD, fEnd   ? C_ACCENT : C_BORDER);

    SoftShadow(dc, G.rcCard, S(20));
    RoundFill(dc, G.rcCard, S(20), C_CARD, C_BORDER);
    TextOut2(dc, L"抽中号码", G.rcCardLabel, G.fLabel, C_MUTED,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    RECT well = Inflate(G.rcResultEdit, S(2));
    RoundFill(dc, well, S(14), C_WELL,
              GetFocus() == G.edResult ? C_ACCENT : C_BORDER);

    COLORREF btn = (G.pressBtn == 1 || G.hot == 1) ? C_ACC_DK : C_ACCENT;
    RoundFill(dc, G.rcBtnStart, S(18), btn, btn);
    TextOut2(dc, L"开 始", G.rcBtnStart, G.fBtn, RGB(255, 255, 255),
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static int RowHeight() { return S(48); }

static int VisibleRows()
{
    int v = (G.rcList.bottom - G.rcList.top - S(12)) / RowHeight();
    return v < 1 ? 1 : v;
}

static int BarLeft()  { return G.rcList.left + S(152); }
static int BarWidth() { return (G.rcList.right - S(132)) - BarLeft(); }

static RECT RowRect(int index)
{
    int top = G.rcList.top + S(6) + (index - G.scrollRow) * RowHeight();
    RECT r = { G.rcList.left + S(8), top, G.rcList.right - S(8), top + RowHeight() };
    return r;
}

static RECT BarRect(int index)
{
    RECT row = RowRect(index);
    int  h   = S(10);
    int  y   = row.top + (RowHeight() - h) / 2;
    RECT r = { BarLeft(), y, BarLeft() + BarWidth(), y + h };
    return r;
}

static int RowFromY(int y)
{
    int rel = y - (G.rcList.top + S(6));
    if (rel < 0) return -1;
    int r = rel / RowHeight();
    if (r < 0 || r >= VisibleRows()) return -1;
    int abs = G.scrollRow + r;
    if (abs < 0 || abs >= (int)G.pct.size()) return -1;
    return abs;
}

static void PaintBack(HDC dc)
{
    HBRUSH bg = CreateSolidBrush(C_BG);
    RECT   full = {};
    GetClientRect(G.hwnd, &full);
    FillRect(dc, &full, bg);
    DeleteObject(bg);

    TextOut2(dc, L"号码权重", G.rcBkTitle, G.fTitle, C_TEXT,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    auto chip = [&](const RECT& r, bool hot, const std::wstring& text)
    {
        RoundFill(dc, r, S(10), hot ? C_ACC_LT : C_CARD, hot ? C_ACCENT : C_BORDER);
        TextOut2(dc, text, r, G.fLabel, hot ? C_ACCENT : C_TEXT,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    };
    chip(G.rcAll5, G.hot == 3, L"平均分配");
    chip(G.rcAll0, G.hot == 4, L"全部清零");

    SoftShadow(dc, G.rcList, S(20));
    RoundFill(dc, G.rcList, S(20), C_CARD, C_BORDER);

    const int count = (int)G.pct.size();
    const int maxScroll = std::max(0, count - VisibleRows());
    if (G.scrollRow > maxScroll) G.scrollRow = maxScroll;
    if (G.scrollRow < 0) G.scrollRow = 0;

    int saveDC = SaveDC(dc);
    IntersectClipRect(dc, G.rcList.left + S(2), G.rcList.top + S(2),
                      G.rcList.right - S(2), G.rcList.bottom - S(2));

    for (int i = G.scrollRow; i < count && i < G.scrollRow + VisibleRows(); ++i)
    {
        RECT row = RowRect(i);
        bool sel = (i == G.selRow);

        if (sel)
            RoundFill(dc, Inflate(row, -S(2)), S(12), C_SEL, C_SEL);

        // 号码
        std::wstring num = std::to_wstring(G.startN + (long long)i);
        RECT numRc = { row.left + S(18), row.top, G.rcList.left + S(150), row.bottom };
        TextOut2(dc, num, numRc, G.fRow, C_TEXT,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // 拖动条
        RECT bar = BarRect(i);
        RoundFill(dc, bar, S(5), C_TRACK, C_TRACK);

        double v = G.pct[(size_t)i];
        if (v > 0.0)
        {
            int w = (int)((double)BarWidth() * v / 100.0 + 0.5);
            if (w < S(10)) w = S(10);
            if (w > BarWidth()) w = BarWidth();
            RECT f = { bar.left, bar.top, bar.left + w, bar.bottom };
            RoundFill(dc, f, S(5), C_ACCENT, C_ACCENT);
        }

        // 百分比
        std::wstring txt = (sel && G.editing) ? G.typeBuf : PctText(v);
        RECT pctRc = { G.rcList.right - S(120), row.top, G.rcList.right - S(22), row.bottom };
        TextOut2(dc, txt, pctRc, G.fPct,
                 (sel && G.editing) ? C_ACCENT : (v <= 0.0 ? C_MUTED : C_TEXT),
                 DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

        if (i != G.scrollRow + VisibleRows() - 1 && i != count - 1)
        {
            RECT sep = { row.left + S(18), row.bottom - 1, row.right - S(18), row.bottom };
            HBRUSH b = CreateSolidBrush(C_SEP);
            FillRect(dc, &sep, b);
            DeleteObject(b);
        }
    }

    if (count > VisibleRows())
    {
        int trackTop = G.rcList.top + S(14);
        int trackBot = G.rcList.bottom - S(14);
        int trackH   = std::max(trackBot - trackTop, 0);
        int thumbH   = std::max(S(30), (int)((long long)trackH * VisibleRows() / count));
        if (thumbH > trackH) thumbH = trackH;
        int maxS = std::max(1, count - VisibleRows());
        int pos  = trackTop + (int)((long long)(trackH - thumbH) * G.scrollRow / maxS);

        RECT t = { G.rcList.right - S(13), trackTop, G.rcList.right - S(7), trackBot };
        RECT h = { G.rcList.right - S(13), pos, G.rcList.right - S(7), pos + thumbH };
        RoundFill(dc, t, S(3), C_TRACK, C_TRACK);
        RoundFill(dc, h, S(3), C_THUMB, C_THUMB);
    }

    RestoreDC(dc, saveDC);

    COLORREF saveCol = (G.pressBtn == 2) ? C_ACC_DK : (G.hot == 2 ? C_ACC_DK : C_ACCENT);
    RoundFill(dc, G.rcSave, S(18), saveCol, saveCol);
    TextOut2(dc, L"保存并关闭", G.rcSave, G.fBtn, RGB(255, 255, 255),
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void Render()
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(G.hwnd, &ps);

    RECT rc = {};
    GetClientRect(G.hwnd, &rc);

    HDC     mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    if (mem && bmp)
    {
        HGDIOBJ ob = SelectObject(mem, bmp);
        if (G.page == 0) PaintMain(mem); else PaintBack(mem);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, ob);
    }
    if (bmp) DeleteObject(bmp);
    if (mem) DeleteDC(mem);

    EndPaint(G.hwnd, &ps);
}

// ---------------------------------------------------------------- 行为

static void SaveConfig();   // 定义见下方「配置」

static void SetResultText(long long v)
{
    SetWindowTextW(G.edResult, std::to_wstring(v).c_str());
}

static void ShowError(const std::wstring& msg)
{
    MessageBoxW(G.hwnd, msg.c_str(), L"随机抽号器", MB_OK | MB_ICONWARNING);
}

static void DoDraw()
{
    if (G.rolling) return;

    std::wstring msg;
    if (!ValidateRange(msg)) { ShowError(msg); return; }

    RebuildPct();
    double total = 0.0;
    for (double v : G.pct) total += v;
    if (total <= 1e-9)
    {
        ShowError(L"范围内所有号码都是 0%");
        return;
    }

    long long pick = 0;
    if (!PickOne(pick))
    {
        ShowError(L"抽取失败：请检查号码范围与概率");
        return;
    }

    G.finalPick = pick;
    G.rolling   = true;
    G.rollEnd   = GetTickCount64() + 420;
    SetResultText(pick);
    SetTimer(G.hwnd, kTimerRoll, 40, nullptr);
}

static void CommitEdit()
{
    if (!G.editing) return;
    if (!G.typeBuf.empty() && G.selRow >= 0 && G.selRow < (int)G.pct.size())
    {
        int v = _wtoi(G.typeBuf.c_str());
        if (v < 0)   v = 0;
        if (v > 100) v = 100;
        SetPercent(G.selRow, (double)v);
    }
    G.editing = false;
    G.typeBuf.clear();
}

static void EnsureVisible()
{
    int vis = VisibleRows();
    if (G.selRow < G.scrollRow) G.scrollRow = G.selRow;
    if (G.selRow >= G.scrollRow + vis) G.scrollRow = G.selRow - vis + 1;
    int maxScroll = (int)G.pct.size() - vis;
    if (maxScroll < 0) maxScroll = 0;
    if (G.scrollRow > maxScroll) G.scrollRow = maxScroll;
    if (G.scrollRow < 0) G.scrollRow = 0;
}

static void EnterBackend()
{
    std::wstring msg;
    if (!ValidateRange(msg)) { ShowError(msg); return; }

    RebuildPct();
    G.page      = 1;
    G.editing   = false;
    G.typeBuf.clear();
    G.scrollRow = 0;
    G.selRow    = 0;
    G.dragRow   = -1;
    Layout();
    PlaceEdits();
    InvalidateRect(G.hwnd, nullptr, TRUE);
}

static void ExitBackend(bool save)
{
    CommitEdit();
    if (save) SaveConfig();
    G.page    = 0;
    G.editing = false;
    G.typeBuf.clear();
    G.dragRow = -1;
    Layout();
    PlaceEdits();
    InvalidateRect(G.hwnd, nullptr, TRUE);
}

static void ApplyDrag(int x)
{
    if (G.dragRow < 0) return;
    int bw = BarWidth();
    if (bw <= 0) return;
    double t = (double)(x - BarLeft()) * 100.0 / (double)bw;
    if (t < 0.0)   t = 0.0;
    if (t > 100.0) t = 100.0;
    t = (double)((int)(t * 10.0 + 0.5)) / 10.0;     // 吸附到 0.1%
    SetPercent(G.dragRow, t);
}

// ---------------------------------------------------------------- 配置

static std::wstring ConfigPath()
{
    wchar_t p[MAX_PATH] = {};
    DWORD   n = GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s(p, n);
    size_t k = s.find_last_of(L'\\');
    if (k != std::wstring::npos) s.resize(k);
    return s + L"\\RandomPicker.cfg";
}

static void SaveConfig()
{
    std::wstring path = ConfigPath();
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return;

    std::fprintf(f, "v2\n");
    std::fprintf(f, "start %lld\n", G.startN);
    std::fprintf(f, "end %lld\n", G.endN);

    long long n = 0;
    for (size_t i = 0; i < G.pct.size(); ++i)
    {
        if (++n > 200000) break;
        std::fprintf(f, "p %lld %.3f\n", G.startN + (long long)i, G.pct[i]);
    }
    std::fclose(f);
}

static void LoadConfig()
{
    std::wstring path = ConfigPath();
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return;

    char line[256] = {};
    long long s = 1, e = 50;
    std::vector<std::pair<long long, double>> pairs;

    while (std::fgets(line, sizeof(line), f))
    {
        long long a = 0;
        double    b = 0.0;
        if (std::sscanf(line, "start %lld", &a) == 1)      s = a;
        else if (std::sscanf(line, "end %lld", &a) == 1)   e = a;
        else if (std::sscanf(line, "p %lld %lf", &a, &b) == 2) pairs.push_back({ a, b });
    }
    std::fclose(f);

    if (s <= e && ((unsigned long long)e - (unsigned long long)s) < (unsigned long long)kMaxRange)
    {
        G.startN = s;
        G.endN   = e;
    }

    RebuildPct();
    for (auto& kv : pairs)
    {
        long long idx = kv.first - G.startN;
        if (idx >= 0 && idx < (long long)G.pct.size())
        {
            double v = kv.second;
            if (v < 0.0)   v = 0.0;
            if (v > 100.0) v = 100.0;
            G.pct[(size_t)idx] = v;
        }
    }
    NormalizePct();
}

// ---------------------------------------------------------------- 鼠标 / 键盘

static void OnSize()
{
    Layout();
    PlaceEdits();
    InvalidateRect(G.hwnd, nullptr, TRUE);
}

static void UpdateHot(int x, int y)
{
    POINT pt = { x, y };
    int h = 0;

    if (G.page == 0)
    {
        if (PtInRect(&G.rcBtnStart, pt)) h = 1;
    }
    else
    {
        if (PtInRect(&G.rcSave, pt)) h = 2;
        else if (PtInRect(&G.rcAll5, pt)) h = 3;
        else if (PtInRect(&G.rcAll0, pt)) h = 4;
    }

    if (h != G.hot) { G.hot = h; InvalidateRect(G.hwnd, nullptr, FALSE); }
}

static void OnMouseMove(int x, int y)
{
    if (!G.tracking)
    {
        TRACKMOUSEEVENT tme = {};
        tme.cbSize    = sizeof(tme);
        tme.dwFlags   = TME_LEAVE;
        tme.hwndTrack = G.hwnd;
        TrackMouseEvent(&tme);
        G.tracking = true;
    }

    if (G.dragRow >= 0)
    {
        ApplyDrag(x);
        InvalidateRect(G.hwnd, &G.rcList, FALSE);
        return;
    }

    UpdateHot(x, y);
}

static void OnLButtonDown(int x, int y)
{
    POINT pt = { x, y };

    if (G.page == 0)
    {
        if (PtInRect(&G.rcBtnStart, pt))
        {
            G.pressBtn = 1;
            InvalidateRect(G.hwnd, &G.rcBtnStart, FALSE);
        }
        return;
    }

    if (PtInRect(&G.rcSave, pt)) { G.pressBtn = 2; InvalidateRect(G.hwnd, nullptr, FALSE); return; }
    if (PtInRect(&G.rcAll5, pt)) { G.pressBtn = 3; InvalidateRect(G.hwnd, nullptr, FALSE); return; }
    if (PtInRect(&G.rcAll0, pt)) { G.pressBtn = 4; InvalidateRect(G.hwnd, nullptr, FALSE); return; }

    if (PtInRect(&G.rcList, pt))
    {
        SetFocus(G.hwnd);
        int row = RowFromY(y);
        if (row < 0) return;

        CommitEdit();
        G.selRow = row;

        RECT bar = BarRect(row);
        RECT hit = Inflate(bar, S(12));
        if (PtInRect(&hit, pt))
        {
            G.dragRow = row;
            SetCapture(G.hwnd);
            ApplyDrag(x);
        }
        InvalidateRect(G.hwnd, nullptr, FALSE);
    }
}

static void OnLButtonUp(int x, int y)
{
    if (G.dragRow >= 0)
    {
        G.dragRow = -1;
        ReleaseCapture();
        InvalidateRect(G.hwnd, &G.rcList, FALSE);
        return;
    }

    POINT pt = { x, y };
    int pressed = G.pressBtn;
    G.pressBtn = 0;

    if (pressed == 0) return;

    if (G.page == 0)
    {
        InvalidateRect(G.hwnd, &G.rcBtnStart, FALSE);
        if (PtInRect(&G.rcBtnStart, pt))
        {
            if (GetKeyState(VK_SHIFT) & 0x8000) EnterBackend();
            else                                DoDraw();
        }
        return;
    }

    InvalidateRect(G.hwnd, nullptr, FALSE);
    if (pressed == 2 && PtInRect(&G.rcSave, pt)) ExitBackend(true);
    if (pressed == 3 && PtInRect(&G.rcAll5, pt))
    {
        CommitEdit();
        int n = (int)G.pct.size();
        if (n > 0)
        {
            double each = 100.0 / (double)n;
            for (double& v : G.pct) v = each;
        }
        InvalidateRect(G.hwnd, nullptr, FALSE);
    }
    if (pressed == 4 && PtInRect(&G.rcAll0, pt))
    {
        CommitEdit();
        for (double& v : G.pct) v = 0.0;
        InvalidateRect(G.hwnd, nullptr, FALSE);
    }
}

static void OnWheel(int delta, int sx, int sy)
{
    if (G.page != 1) return;
    if (delta == 0) return;

    POINT pt = { sx, sy };
    ScreenToClient(G.hwnd, &pt);

    int row = RowFromY(pt.y);
    if (row >= 0 && pt.x >= BarLeft() - S(20) && pt.x <= G.rcList.right - S(110))
    {
        CommitEdit();
        double v = G.pct[(size_t)row] + (delta > 0 ? 1.0 : -1.0);
        SetPercent(row, v);
        InvalidateRect(G.hwnd, nullptr, FALSE);
        return;
    }

    G.scrollRow -= (delta > 0 ? 3 : -3);
    if (G.scrollRow < 0) G.scrollRow = 0;
    int maxScroll = std::max(0, (int)G.pct.size() - VisibleRows());
    if (G.scrollRow > maxScroll) G.scrollRow = maxScroll;
    InvalidateRect(G.hwnd, nullptr, FALSE);
}

static void OnChar(wchar_t c)
{
    if (G.page != 1) return;

    if (c >= L'0' && c <= L'9')
    {
        if (!G.editing) { G.editing = true; G.typeBuf.clear(); }
        if (G.typeBuf.size() < 3u) G.typeBuf.push_back(c);
        InvalidateRect(G.hwnd, nullptr, FALSE);
        return;
    }
    if (c == 8)
    {
        if (G.editing && !G.typeBuf.empty()) G.typeBuf.pop_back();
        InvalidateRect(G.hwnd, nullptr, FALSE);
        return;
    }
    if (c == 13)
    {
        CommitEdit();
        InvalidateRect(G.hwnd, nullptr, FALSE);
        return;
    }
    if (c == 27)
    {
        if (G.editing)
        {
            G.editing = false;
            G.typeBuf.clear();
            InvalidateRect(G.hwnd, nullptr, FALSE);
        }
        else
        {
            ExitBackend(true);
        }
    }
}

static void OnKeyDown(WPARAM key)
{
    if (G.page != 1) return;

    int n = (int)G.pct.size();
    if (n <= 0) return;
    if (G.selRow < 0)     G.selRow = 0;
    if (G.selRow > n - 1) G.selRow = n - 1;

    if (key == VK_UP || key == VK_DOWN || key == VK_PRIOR || key == VK_NEXT ||
        key == VK_HOME || key == VK_END)
    {
        CommitEdit();
        switch (key)
        {
        case VK_UP:    G.selRow -= 1;             break;
        case VK_DOWN:  G.selRow += 1;             break;
        case VK_PRIOR: G.selRow -= VisibleRows(); break;
        case VK_NEXT:  G.selRow += VisibleRows(); break;
        case VK_HOME:  G.selRow = 0;              break;
        case VK_END:   G.selRow = n - 1;          break;
        default: break;
        }
        if (G.selRow < 0)     G.selRow = 0;
        if (G.selRow > n - 1) G.selRow = n - 1;
        EnsureVisible();
        InvalidateRect(G.hwnd, nullptr, FALSE);
        return;
    }

    if (key == VK_LEFT || key == VK_RIGHT)
    {
        CommitEdit();
        double d = (key == VK_RIGHT) ? 1.0 : -1.0;
        if (GetKeyState(VK_SHIFT) & 0x8000) d *= 10.0;
        SetPercent(G.selRow, G.pct[(size_t)G.selRow] + d);
        InvalidateRect(G.hwnd, nullptr, FALSE);
    }
}

// ---------------------------------------------------------------- 窗口过程

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        G.hwnd  = hwnd;
        G.hinst = (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE);
        G.brCard = CreateSolidBrush(C_CARD);

        G.edStart = CreateWindowExW(0, L"EDIT", L"1",
            WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL,
            0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)ID_ED_START, G.hinst, nullptr);
        G.edEnd = CreateWindowExW(0, L"EDIT", L"50",
            WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL,
            0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)ID_ED_END, G.hinst, nullptr);
        G.edResult = CreateWindowExW(0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_CENTER | ES_AUTOHSCROLL,
            0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)ID_ED_RESULT, G.hinst, nullptr);

        if (G.edStart)
            SendMessageW(G.edStart, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                         MAKELPARAM(S(18), S(18)));
        if (G.edEnd)
            SendMessageW(G.edEnd, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                         MAKELPARAM(S(18), S(18)));

        MakeFonts();
        LoadConfig();
        SetWindowTextW(G.edStart, std::to_wstring(G.startN).c_str());
        SetWindowTextW(G.edEnd, std::to_wstring(G.endN).c_str());
        Layout();
        PlaceEdits();
        return 0;
    }

    case WM_SIZE:
        OnSize();
        return 0;

    case WM_PAINT:
        Render();
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_CTLCOLOREDIT:
    {
        HDC dc = (HDC)wp;
        SetTextColor(dc, C_TEXT);
        SetBkColor(dc, C_CARD);
        return (LRESULT)G.brCard;
    }

    case WM_MOUSEMOVE:
        OnMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;

    case WM_MOUSELEAVE:
        G.tracking = false;
        if (G.hot != 0) { G.hot = 0; InvalidateRect(hwnd, nullptr, FALSE); }
        return 0;

    case WM_CAPTURECHANGED:
        // 拖动中失去捕获（Alt+Tab 等）时收尾，避免卡在拖拽状态
        if (G.dragRow >= 0)
        {
            G.dragRow = -1;
            InvalidateRect(hwnd, &G.rcList, FALSE);
        }
        return 0;

    case WM_LBUTTONDOWN:
        OnLButtonDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;

    case WM_LBUTTONUP:
        OnLButtonUp(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;

    case WM_MOUSEWHEEL:
        OnWheel(GET_WHEEL_DELTA_WPARAM(wp), GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;

    case WM_CHAR:
        OnChar((wchar_t)wp);
        return 0;

    case WM_KEYDOWN:
        OnKeyDown(wp);
        return 0;

    case WM_TIMER:
        if (wp == kTimerRoll)
        {
            if (!G.rolling) { KillTimer(hwnd, kTimerRoll); return 0; }
            if (GetTickCount64() >= G.rollEnd)
            {
                KillTimer(hwnd, kTimerRoll);
                G.rolling = false;
                SetResultText(G.finalPick);
            }
            else
            {
                long long n = Count();
                if (n > 0) SetResultText(G.startN + (long long)(G.rng() % (unsigned long long)n));
            }
        }
        return 0;

    case WM_COMMAND:
    {
        int id   = LOWORD(wp);
        int code = HIWORD(wp);

        if (id == ID_ED_START || id == ID_ED_END)
        {
            if (code == EN_SETFOCUS || code == EN_KILLFOCUS)
                InvalidateRect(hwnd, nullptr, FALSE);
            if (code == EN_KILLFOCUS)
            {
                std::wstring msg;
                if (ValidateRange(msg)) RebuildPct();
            }
        }
        if (id == ID_ED_RESULT)
        {
            if (code == EN_SETFOCUS || code == EN_KILLFOCUS)
                InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_DPICHANGED:
    {
        G.dpi = (int)HIWORD(wp);
        if (G.dpi < 72) G.dpi = 96;
        MakeFonts();
        RECT* pr = (RECT*)lp;
        SetWindowPos(hwnd, nullptr, pr->left, pr->top,
                     pr->right - pr->left, pr->bottom - pr->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        Layout();
        PlaceEdits();
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }

    case WM_CLOSE:
        if (G.page == 1) CommitEdit();
        SaveConfig();
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, kTimerRoll);
        if (G.fTitle)  DeleteObject(G.fTitle);
        if (G.fLabel)  DeleteObject(G.fLabel);
        if (G.fInput)  DeleteObject(G.fInput);
        if (G.fResult) DeleteObject(G.fResult);
        if (G.fBtn)    DeleteObject(G.fBtn);
        if (G.fRow)    DeleteObject(G.fRow);
        if (G.fPct)    DeleteObject(G.fPct);
        if (G.brCard)  DeleteObject(G.brCard);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- 入口

typedef UINT(WINAPI* GetDpiForWindowFn)(HWND);
typedef BOOL(WINAPI* SetDpiCtxFn)(void*);

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int)
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32)
    {
        auto setCtx = (SetDpiCtxFn)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (setCtx)
        {
            INT_PTR ctxVal = -4;                    // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
            setCtx((void*)ctxVal);
        }
        else
        {
            SetProcessDPIAware();
        }
    }

    G.hinst = hInst;

    std::random_device rd;
    unsigned long long seed = ((unsigned long long)rd() << 32) ^ (unsigned long long)rd();
    seed ^= (unsigned long long)GetTickCount64() * 0x9E3779B97F4A7C15ULL;
    seed ^= ((unsigned long long)GetCurrentProcessId() << 16) ^
            (unsigned long long)(INT_PTR)&seed;
    G.rng.seed(seed);

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(C_BG);
    wc.lpszClassName = L"RandomPickerClass";
    wc.hIcon         = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm       = LoadIconW(nullptr, IDI_APPLICATION);
    if (!RegisterClassExW(&wc)) return 1;

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;

    RECT wr = { 0, 0, S(720), S(540) };
    AdjustWindowRectEx(&wr, style, FALSE, 0);
    int ww = wr.right - wr.left;
    int wh = wr.bottom - wr.top;

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int px = (sw - ww) / 2;
    int py = (sh - wh) / 2;
    if (px < 0) px = 0;
    if (py < 0) py = 0;

    HWND hwnd = CreateWindowExW(0, L"RandomPickerClass", L"随机抽号器",
                                style, px, py, ww, wh,
                                nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;

    if (user32)
    {
        auto getDpi = (GetDpiForWindowFn)GetProcAddress(user32, "GetDpiForWindow");
        if (getDpi)
        {
            UINT d = getDpi(hwnd);
            if (d >= 72 && d != (UINT)G.dpi)
            {
                G.dpi = (int)d;
                MakeFonts();
                RECT cr = { 0, 0, S(720), S(540) };
                AdjustWindowRectEx(&cr, style, FALSE, 0);
                SetWindowPos(hwnd, nullptr, px, py, cr.right - cr.left, cr.bottom - cr.top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                Layout();
                PlaceEdits();
            }
        }
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    if (G.edStart) SetFocus(G.edStart);

    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN && G.page == 0)
        {
            if (GetKeyState(VK_SHIFT) & 0x8000) EnterBackend();
            else                                DoDraw();
            continue;
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_TAB && G.page == 0)
        {
            HWND cur  = GetFocus();
            HWND next = (cur == G.edStart) ? G.edEnd
                      : (cur == G.edEnd)   ? G.edResult
                                           : G.edStart;
            if (next)
            {
                SetFocus(next);
                SendMessageW(next, EM_SETSEL, 0, -1);
            }
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
