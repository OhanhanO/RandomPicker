// ============================================================================
//  RandomPicker —— 随机抽号器（单文件 Win32 / GDI，无第三方依赖）
//
//  主界面：起始号码 / 终止号码 / 抽中号码 / 开始
//  按住 Shift 点「开始」进入权重后台：为范围内每个号码设置 0~10 的权重，
//  权重越大占比越高，0 = 永不抽中，只有一个号码有权重时它就是 100%。
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
#include <vector>

#if defined(_MSC_VER)
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#endif

// ---------------------------------------------------------------- 常量

static const long long   kMaxRange = 1000000;   // 最多一百万个号码
static const unsigned char kDefW   = 5;         // 默认权重

static const int ID_ED_START  = 1001;
static const int ID_ED_END    = 1002;
static const int ID_ED_RESULT = 1003;

static const UINT_PTR kTimerRoll = 1;

static const COLORREF C_BG     = RGB(244, 246, 249);
static const COLORREF C_CARD   = RGB(255, 255, 255);
static const COLORREF C_BORDER = RGB(222, 226, 232);
static const COLORREF C_TEXT   = RGB(24, 27, 33);
static const COLORREF C_MUTED  = RGB(120, 128, 140);
static const COLORREF C_ACCENT = RGB(37, 99, 235);
static const COLORREF C_ACC_DK = RGB(29, 78, 216);
static const COLORREF C_SEL    = RGB(233, 240, 254);
static const COLORREF C_TRACK  = RGB(233, 237, 242);
static const COLORREF C_THUMB  = RGB(186, 193, 203);

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

    // weights[i] 对应号码 startN + i；wStart/wEnd 记录这组权重的号码区间
    long long                wStart = 0;
    long long                wEnd   = -1;
    std::vector<unsigned char> w;

    std::mt19937_64 rng;

    HFONT fTitle = nullptr, fLabel = nullptr, fInput = nullptr, fResult = nullptr;
    HFONT fBtn = nullptr, fRow = nullptr, fSmall = nullptr;
    HBRUSH brCard = nullptr;

    // 主界面布局
    RECT rcTitle{}, rcStartLabel{}, rcEndLabel{}, rcStartEdit{}, rcEndEdit{};
    RECT rcCard{}, rcCardLabel{}, rcResultEdit{}, rcBtnStart{};

    // 后台布局
    RECT rcBkTitle{}, rcAll5{}, rcAll0{}, rcList{}, rcSave{};

    int         scrollRow = 0;
    int         selRow    = 0;   // 0 基，选中行
    bool        editing   = false;
    std::wstring typeBuf;        // 正在输入的权重文本
    int         pressBtn  = 0;   // 1 = 开始, 2 = 保存, 3 = 全5, 4 = 全0

    bool      rolling  = false;
    ULONGLONG rollEnd  = 0;
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
    if (G.fSmall) DeleteObject(G.fSmall);

    const wchar_t* face = L"Microsoft YaHei UI";
    G.fTitle  = MakeFont(16, FW_BOLD,   face);
    G.fLabel  = MakeFont(10, FW_NORMAL, face);
    G.fInput  = MakeFont(14, FW_NORMAL, face);
    G.fResult = MakeFont(34, FW_BOLD,   face);
    G.fBtn    = MakeFont(15, FW_BOLD,   face);
    G.fRow    = MakeFont(12, FW_NORMAL, face);
    G.fSmall  = MakeFont(9,  FW_NORMAL, face);

    if (G.edStart)  SendMessageW(G.edStart,  WM_SETFONT, (WPARAM)G.fInput,  TRUE);
    if (G.edEnd)    SendMessageW(G.edEnd,    WM_SETFONT, (WPARAM)G.fInput,  TRUE);
    if (G.edResult) SendMessageW(G.edResult, WM_SETFONT, (WPARAM)G.fResult, TRUE);
}

static std::wstring PercentText(unsigned long long part, unsigned long long total)
{
    if (total == 0) return L"0%";
    unsigned long long v = (part * 1000ULL + total / 2ULL) / total;   // 0..1000
    std::wstring s = std::to_wstring(v / 10ULL);
    unsigned long long frac = v % 10ULL;
    if (frac != 0ULL) { s.push_back(L'.'); s.push_back((wchar_t)(L'0' + (int)frac)); }
    s.push_back(L'%');
    return s;
}

// ---------------------------------------------------------------- 号码 / 权重

static long long Count()
{
    return G.endN - G.startN + 1;
}

static unsigned long long TotalWeight()
{
    unsigned long long t = 0;
    for (unsigned char v : G.w) t += v;
    return t;
}

// 按号码保留旧权重，重新铺开 weights 数组
static void RebuildWeights()
{
    std::vector<unsigned char> nw;
    long long n = Count();
    if (n > 0 && n <= kMaxRange)
    {
        nw.assign((size_t)n, kDefW);
        if (!G.w.empty() && G.wStart <= G.wEnd)
        {
            long long a = std::max(G.startN, G.wStart);
            long long b = std::min(G.endN, G.wEnd);
            // 用计数循环，避免 v == LLONG_MAX 时 ++v 溢出成死循环
            long long span = b - a + 1;
            for (long long k = 0; k < span; ++k)
            {
                long long v  = a + k;
                long long oi = v - G.wStart;
                if (oi >= 0 && oi < (long long)G.w.size())
                    nw[(size_t)(v - G.startN)] = G.w[(size_t)oi];
            }
        }
    }
    G.w.swap(nw);
    G.wStart = G.startN;
    G.wEnd   = G.endN;
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
    long long v = wcstoll(s.c_str(), &end, 10);
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

    unsigned long long span = (unsigned long long)b - (unsigned long long)a;   // b >= a，精确
    if (span >= (unsigned long long)kMaxRange)
    {
        msg = L"号码范围最多 1000000 个";
        return false;
    }

    G.startN = a;
    G.endN   = b;
    return true;
}

static bool PickOne(long long& out)
{
    unsigned long long total = TotalWeight();
    if (total == 0) return false;

    std::uniform_int_distribution<unsigned long long> dist(0, total - 1);
    unsigned long long r = dist(G.rng);
    for (size_t i = 0; i < G.w.size(); ++i)
    {
        unsigned long long wi = G.w[i];
        if (r < wi) { out = G.startN + (long long)i; return true; }
        r -= wi;
    }
    for (size_t i = G.w.size(); i-- > 0;)
        if (G.w[i] != 0) { out = G.startN + (long long)i; return true; }
    return false;
}

// ---------------------------------------------------------------- 布局

static void Layout()
{
    RECT rc = {};
    GetClientRect(G.hwnd, &rc);
    const int W   = rc.right;
    const int pad = S(28);
    const int cw  = W - pad * 2;

    G.rcTitle = { pad, S(20), pad + S(300), S(56) };

    if (G.page == 0)
    {
        int gap  = S(20);
        int colW = (cw - gap) / 2;

        G.rcStartLabel = { pad, S(68), pad + colW, S(90) };
        G.rcEndLabel   = { pad + colW + gap, S(68), W - pad, S(90) };
        G.rcStartEdit  = { pad, S(94), pad + colW, S(144) };
        G.rcEndEdit    = { pad + colW + gap, S(94), W - pad, S(144) };

        G.rcCard       = { pad, S(168), W - pad, S(338) };
        G.rcCardLabel  = { G.rcCard.left + S(18), G.rcCard.top + S(12),
                           G.rcCard.right - S(18), G.rcCard.top + S(36) };
        G.rcResultEdit = { G.rcCard.left + S(14), G.rcCard.top + S(44),
                           G.rcCard.right - S(14), G.rcCard.bottom - S(16) };

        G.rcBtnStart = { pad, S(360), W - pad, S(428) };
    }
    else
    {
        G.rcBkTitle = { pad, S(20), pad + S(300), S(56) };
        G.rcAll5    = { W - pad - S(96) * 2 - S(10), S(26), W - pad - S(96) - S(10), S(58) };
        G.rcAll0    = { W - pad - S(96), S(26), W - pad, S(58) };
        G.rcList    = { pad, S(76), W - pad, S(358) };
        G.rcSave    = { pad, S(382), W - pad, S(442) };
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

    RoundFill(dc, Inflate(G.rcStartEdit, S(2)), S(10), C_CARD,
              fStart ? C_ACCENT : C_BORDER);
    RoundFill(dc, Inflate(G.rcEndEdit, S(2)), S(10), C_CARD,
              fEnd ? C_ACCENT : C_BORDER);

    RoundFill(dc, G.rcCard, S(12), C_CARD, C_BORDER);
    TextOut2(dc, L"抽中号码", G.rcCardLabel, G.fLabel, C_MUTED,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RoundFill(dc, Inflate(G.rcResultEdit, S(2)), S(10), C_CARD,
              GetFocus() == G.edResult ? C_ACCENT : C_BORDER);

    COLORREF btn = (G.pressBtn == 1) ? C_ACC_DK : C_ACCENT;
    RoundFill(dc, G.rcBtnStart, S(12), btn, btn);
    TextOut2(dc, L"开  始", G.rcBtnStart, G.fBtn, RGB(255, 255, 255),
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static int RowHeight() { return S(38); }

static int VisibleRows()
{
    int v = (G.rcList.bottom - G.rcList.top - S(8)) / RowHeight();
    return v < 1 ? 1 : v;
}

static RECT PillRect(int rowTop)
{
    int   innerRight = G.rcList.right - S(12);
    int   pw = S(64), ph = S(26);
    RECT  r  = {};
    r.right  = innerRight - S(84);
    r.left   = r.right - pw;
    r.top    = rowTop + (RowHeight() - ph) / 2;
    r.bottom = r.top + ph;
    return r;
}

// 权重格左边界（用于点击/滚轮命中判断）
static int PillLeftEdge()
{
    return G.rcList.right - S(12) - S(84) - S(64);
}

static int RowFromY(int y)
{
    int rel = y - (G.rcList.top + S(4));
    if (rel < 0) return -1;
    int r = rel / RowHeight();
    if (r < 0 || r >= VisibleRows()) return -1;
    int abs = G.scrollRow + r;
    if (abs < 0 || abs >= (int)G.w.size()) return -1;
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

    RoundFill(dc, G.rcAll5, S(8), (G.pressBtn == 3) ? C_TRACK : C_CARD, C_BORDER);
    RoundFill(dc, G.rcAll0, S(8), (G.pressBtn == 4) ? C_TRACK : C_CARD, C_BORDER);
    TextOut2(dc, L"全部设为 5", G.rcAll5, G.fSmall, C_TEXT,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    TextOut2(dc, L"全部设为 0", G.rcAll0, G.fSmall, C_TEXT,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    RoundFill(dc, G.rcList, S(12), C_CARD, C_BORDER);

    unsigned long long total = TotalWeight();
    const int count = (int)G.w.size();
    const int rowH  = RowHeight();
    const int maxScroll = std::max(0, count - VisibleRows());

    if (G.scrollRow > maxScroll) G.scrollRow = maxScroll;
    if (G.scrollRow < 0) G.scrollRow = 0;

    int saveDC = SaveDC(dc);
    IntersectClipRect(dc, G.rcList.left + 1, G.rcList.top + 1,
                      G.rcList.right - 1, G.rcList.bottom - 1);

    int y = G.rcList.top + S(4);
    for (int i = G.scrollRow; i < count && i < G.scrollRow + VisibleRows(); ++i)
    {
        RECT row = { G.rcList.left + 6, y, G.rcList.right - 6, y + rowH - 2 };

        if (i == G.selRow)
            RoundFill(dc, row, S(8), C_SEL, C_SEL);

        std::wstring num = std::to_wstring(G.startN + (long long)i);
        RECT numRc = { G.rcList.left + S(18), y, G.rcList.left + S(300), y + rowH };
        TextOut2(dc, num, numRc, G.fRow, C_TEXT,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        RECT pill = PillRect(y);
        bool editingThis = (i == G.selRow && G.editing);

        RoundFill(dc, pill, S(7), editingThis ? RGB(226, 236, 255) : RGB(244, 246, 249),
                  editingThis ? C_ACCENT : RGB(232, 236, 241));

        std::wstring wtext;
        if (editingThis)
            wtext = G.typeBuf;
        else
            wtext = std::to_wstring((int)G.w[i]);

        TextOut2(dc, wtext, pill, G.fRow, editingThis ? C_ACCENT : C_TEXT,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        if (editingThis)
        {
            SIZE sz = {};
            HGDIOBJ oldFont = SelectObject(dc, G.fRow);
            GetTextExtentPoint32W(dc, wtext.c_str(), (int)wtext.size(), &sz);
            SelectObject(dc, oldFont);

            int cx = (pill.left + pill.right) / 2 + sz.cx / 2 + S(2);
            HPEN pn = CreatePen(PS_SOLID, S(2) > 0 ? S(2) : 1, C_ACCENT);
            HGDIOBJ op = SelectObject(dc, pn);
            MoveToEx(dc, cx, pill.top + S(5), nullptr);
            LineTo(dc, cx, pill.bottom - S(5));
            SelectObject(dc, op);
            DeleteObject(pn);
        }

        std::wstring pct = PercentText((unsigned long long)G.w[i], total);
        RECT pctRc = { pill.right + S(6), y, G.rcList.right - S(14), y + rowH };
        TextOut2(dc, pct, pctRc, G.fSmall, C_MUTED,
                 DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

        y += rowH;
    }

    // 滚动条
    if (count > VisibleRows())
    {
        int trackTop = G.rcList.top + S(8);
        int trackBot = G.rcList.bottom - S(8);
        int trackH   = trackBot - trackTop;
        int thumbH   = std::max(S(28), (int)((long long)trackH * VisibleRows() / count));
        if (thumbH > trackH) thumbH = trackH;
        int maxS = std::max(1, count - VisibleRows());
        int pos  = trackTop + (int)((long long)(trackH - thumbH) * G.scrollRow / maxS);

        RECT t = { G.rcList.right - S(11), trackTop, G.rcList.right - S(5), trackBot };
        RECT h = { G.rcList.right - S(11), pos, G.rcList.right - S(5), pos + thumbH };
        RoundFill(dc, t, S(3), C_TRACK, C_TRACK);
        RoundFill(dc, h, S(3), C_THUMB, C_THUMB);
    }

    RestoreDC(dc, saveDC);

    COLORREF save = (G.pressBtn == 2) ? C_ACC_DK : C_ACCENT;
    RoundFill(dc, G.rcSave, S(12), save, save);
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

    RebuildWeights();
    if (TotalWeight() == 0)
    {
        ShowError(L"范围内所有权重都是 0，没有可抽的号码");
        return;
    }

    long long pick = 0;
    if (!PickOne(pick))
    {
        ShowError(L"抽取失败：请检查号码范围与权重");
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
    if (!G.typeBuf.empty() && G.selRow >= 0 && G.selRow < (int)G.w.size())
    {
        int v = _wtoi(G.typeBuf.c_str());
        if (v < 0)  v = 0;
        if (v > 10) v = 10;
        G.w[(size_t)G.selRow] = (unsigned char)v;
    }
    G.editing = false;
    G.typeBuf.clear();
}

static void EnsureVisible()
{
    int vis = VisibleRows();
    if (G.selRow < G.scrollRow) G.scrollRow = G.selRow;
    if (G.selRow >= G.scrollRow + vis) G.scrollRow = G.selRow - vis + 1;
    int maxScroll = (int)G.w.size() - vis;
    if (maxScroll < 0) maxScroll = 0;
    if (G.scrollRow > maxScroll) G.scrollRow = maxScroll;
    if (G.scrollRow < 0) G.scrollRow = 0;
}

static void EnterBackend()
{
    std::wstring msg;
    if (!ValidateRange(msg)) { ShowError(msg); return; }

    RebuildWeights();
    G.page     = 1;
    G.editing  = false;
    G.typeBuf.clear();
    G.scrollRow = 0;
    G.selRow    = 0;
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
    Layout();
    PlaceEdits();
    InvalidateRect(G.hwnd, nullptr, TRUE);
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

    std::fprintf(f, "v1\n");
    std::fprintf(f, "start %lld\n", G.startN);
    std::fprintf(f, "end %lld\n", G.endN);

    long long n = 0;
    for (size_t i = 0; i < G.w.size(); ++i)
    {
        if (G.w[i] == kDefW) continue;
        if (++n > 200000) break;
        std::fprintf(f, "w %lld %d\n", G.startN + (long long)i, (int)G.w[i]);
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
    std::vector<std::pair<long long, int>> pairs;

    while (std::fgets(line, sizeof(line), f))
    {
        long long a = 0;
        int       b = 0;
        if (std::sscanf(line, "start %lld", &a) == 1)      s = a;
        else if (std::sscanf(line, "end %lld", &a) == 1)   e = a;
        else if (std::sscanf(line, "w %lld %d", &a, &b) == 2) pairs.push_back({ a, b });
    }
    std::fclose(f);

    if (s <= e && ((unsigned long long)e - (unsigned long long)s) < (unsigned long long)kMaxRange)
    {
        G.startN = s;
        G.endN   = e;
    }

    RebuildWeights();
    for (auto& kv : pairs)
    {
        long long idx = kv.first - G.startN;
        if (idx >= 0 && idx < (long long)G.w.size())
        {
            int v = kv.second;
            if (v < 0) v = 0;
            if (v > 10) v = 10;
            G.w[(size_t)idx] = (unsigned char)v;
        }
    }
}

// ---------------------------------------------------------------- 窗口过程

static void OnSize()
{
    Layout();
    PlaceEdits();
    InvalidateRect(G.hwnd, nullptr, TRUE);
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
        if (row >= 0)
        {
            CommitEdit();
            G.selRow = row;
            RECT pill = PillRect(G.rcList.top + S(4) + (row - G.scrollRow) * RowHeight());
            G.editing = (x >= pill.left - S(18) && x <= G.rcList.right - S(10));
            G.typeBuf.clear();
            InvalidateRect(G.hwnd, nullptr, FALSE);
        }
    }
}

static void OnLButtonUp(int x, int y)
{
    POINT pt = { x, y };
    int pressed = G.pressBtn;
    G.pressBtn = 0;

    if (pressed == 0) return;

    if (G.page == 0)
    {
        InvalidateRect(G.hwnd, &G.rcBtnStart, FALSE);
        if (PtInRect(&G.rcBtnStart, pt))
        {
            bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (shift) EnterBackend();
            else       DoDraw();
        }
        return;
    }

    InvalidateRect(G.hwnd, nullptr, FALSE);
    if (pressed == 2 && PtInRect(&G.rcSave, pt)) ExitBackend(true);
    if (pressed == 3 && PtInRect(&G.rcAll5, pt))
    {
        CommitEdit();
        for (auto& v : G.w) v = kDefW;
        InvalidateRect(G.hwnd, nullptr, FALSE);
    }
    if (pressed == 4 && PtInRect(&G.rcAll0, pt))
    {
        CommitEdit();
        for (auto& v : G.w) v = 0;
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
    if (row >= 0 && pt.x >= PillLeftEdge() - S(18))
    {
        // 在权重格上滚动：微调
        int v = (int)G.w[(size_t)row] + (delta > 0 ? 1 : -1);
        if (v < 0)  v = 0;
        if (v > 10) v = 10;
        G.w[(size_t)row] = (unsigned char)v;
        InvalidateRect(G.hwnd, nullptr, FALSE);
        return;
    }

    G.scrollRow -= (delta > 0 ? 3 : -3);
    if (G.scrollRow < 0) G.scrollRow = 0;
    int maxScroll = std::max(0, (int)G.w.size() - VisibleRows());
    if (G.scrollRow > maxScroll) G.scrollRow = maxScroll;
    InvalidateRect(G.hwnd, nullptr, FALSE);
}

static void OnChar(wchar_t c)
{
    if (G.page != 1) return;

    if (c >= L'0' && c <= L'9')
    {
        if (!G.editing) { G.editing = true; G.typeBuf.clear(); }
        if (G.typeBuf.size() < 2u) G.typeBuf.push_back(c);
        InvalidateRect(G.hwnd, nullptr, FALSE);
        return;
    }
    if (c == 8)   // backspace
    {
        if (G.editing && !G.typeBuf.empty()) G.typeBuf.pop_back();
        InvalidateRect(G.hwnd, nullptr, FALSE);
        return;
    }
    if (c == 13)  // enter
    {
        CommitEdit();
        InvalidateRect(G.hwnd, nullptr, FALSE);
        return;
    }
    if (c == 27)  // esc
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

    int n = (int)G.w.size();
    if (n <= 0) return;

    if (key == VK_UP || key == VK_DOWN || key == VK_PRIOR || key == VK_NEXT ||
        key == VK_HOME || key == VK_END)
    {
        CommitEdit();
        switch (key)
        {
        case VK_UP:    G.selRow -= 1;       break;
        case VK_DOWN:  G.selRow += 1;       break;
        case VK_PRIOR: G.selRow -= VisibleRows(); break;
        case VK_NEXT:  G.selRow += VisibleRows(); break;
        case VK_HOME:  G.selRow = 0;        break;
        case VK_END:   G.selRow = n - 1;    break;
        default: break;
        }
        if (G.selRow < 0)     G.selRow = 0;
        if (G.selRow > n - 1) G.selRow = n - 1;
        EnsureVisible();
        InvalidateRect(G.hwnd, nullptr, FALSE);
    }
}

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
        {
            SendMessageW(G.edStart, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                         MAKELPARAM(S(14), S(14)));
        }
        if (G.edEnd)
        {
            SendMessageW(G.edEnd, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                         MAKELPARAM(S(14), S(14)));
        }

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
                if (ValidateRange(msg)) RebuildWeights();
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
        if (G.fSmall)  DeleteObject(G.fSmall);
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
    // 尽量开启 Per-Monitor DPI 感知（老系统上退化为系统 DPI）
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

    RECT wr = { 0, 0, S(680), S(460) };
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

    // 创建之后再取一次 DPI，保证缩放准确
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
                RECT cr = { 0, 0, S(680), S(460) };
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
    SetFocus(G.edStart);

    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        // 主界面下，回车等价于点「开始」（在输入框里按也一样）；Shift+回车进后台
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN && G.page == 0)
        {
            if (GetKeyState(VK_SHIFT) & 0x8000) EnterBackend();
            else                                DoDraw();
            continue;
        }
        // Tab 在三个输入框之间轮换（普通窗口里 EDIT 自己不会切焦点）
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
