// p2ptun-manager — 极简卡片式 Win32 GUI（C++17，零依赖）
//
// 设计目标：一条隧道 = 一张卡片 = 本地端口 + ID + 三个按钮。
// 没有列表选中态、没有 Tab、没有预设下拉、没有高级参数面板。
// 全局只有一处 Secret。
// p2ptun.exe 本体永不修改，仅以子进程方式拉起。

#include "core.hpp"
#include "process_manager.hpp"

#ifdef _WIN32
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")

namespace {

// ---------------------------------------------------------------------------
// 控件 ID
// ---------------------------------------------------------------------------
constexpr int IDC_EDT_SECRET    = 1001;
constexpr int IDC_BTN_RAND_SEC  = 1002;
constexpr int IDC_BTN_ADD       = 1003;
constexpr int IDC_BTN_LOG       = 1004;
constexpr int IDC_EDT_LOG       = 1005;
constexpr int IDC_BTN_CLEAR_LOG = 1006;

constexpr int CARD_BASE   = 3000;
constexpr int CARD_STRIDE = 8;

enum CardSlot : int {
    CS_ID = 0,
    CS_PORT,
    CS_SERVER,
    CS_CLIENT,
    CS_STOP,
    CS_DEL
};

constexpr UINT WM_APP_REFRESH = WM_APP + 11;

// ---------------------------------------------------------------------------
// 配色（浅色扁平）
// ---------------------------------------------------------------------------
const COLORREF CLR_BG        = RGB(0xF4, 0xF5, 0xF7);
const COLORREF CLR_CARD      = RGB(0xFF, 0xFF, 0xFF);
const COLORREF CLR_CARD_LINE = RGB(0xE3, 0xE6, 0xEA);
const COLORREF CLR_FIELD     = RGB(0xF1, 0xF3, 0xF5);
const COLORREF CLR_TEXT      = RGB(0x11, 0x18, 0x27);
const COLORREF CLR_TEXT_DIM  = RGB(0x6B, 0x72, 0x80);
const COLORREF CLR_TEXT_FAINT= RGB(0x9C, 0xA3, 0xAF);
const COLORREF CLR_PRIMARY   = RGB(0x25, 0x63, 0xEB);
const COLORREF CLR_PRIMARY_D = RGB(0x1D, 0x4E, 0xD8);
const COLORREF CLR_GREEN     = RGB(0x16, 0xA3, 0x4A);
const COLORREF CLR_GREEN_D   = RGB(0x11, 0x8B, 0x3E);
const COLORREF CLR_RED       = RGB(0xDC, 0x26, 0x26);
const COLORREF CLR_RED_D     = RGB(0xB9, 0x1C, 0x1C);
const COLORREF CLR_GRAY_BTN  = RGB(0xF3, 0xF4, 0xF6);
const COLORREF CLR_GRAY_LINE = RGB(0xD5, 0xD9, 0xDE);

// ---------------------------------------------------------------------------
// 布局常量（96-DPI 基准）
// ---------------------------------------------------------------------------
constexpr int L_PADX      = 16;
constexpr int L_HDR_H     = 88;
constexpr int L_CARD_H    = 112;
constexpr int L_CARD_GAP  = 10;
constexpr int L_LOG_H     = 132;
constexpr int L_SCROLL_W  = 16;

// ---------------------------------------------------------------------------
// 全局状态
// ---------------------------------------------------------------------------
int g_dpi = 96;

HWND g_hwnd        = nullptr;
HWND g_edt_secret  = nullptr;
HWND g_btn_rand    = nullptr;
HWND g_btn_add     = nullptr;
HWND g_btn_log     = nullptr;
HWND g_edt_log     = nullptr;
HWND g_btn_clear   = nullptr;

HFONT g_font       = nullptr;
HFONT g_font_bold  = nullptr;
HFONT g_font_small = nullptr;

HBRUSH g_brush_bg      = nullptr;
HBRUSH g_brush_card    = nullptr;
HBRUSH g_brush_field   = nullptr;
HBRUSH g_brush_white   = nullptr;

p2ptun::MultiTunnelManager* g_mgr = nullptr;

struct Card {
    std::string id;                 // 绑定的配置 id
    std::string mode = "expose";
    bool running = false;
    bool failed  = false;
    std::string status;
    std::string traffic;
    HWND h_id     = nullptr;
    HWND h_port   = nullptr;
    HWND h_server = nullptr;
    HWND h_client = nullptr;
    HWND h_stop   = nullptr;
    HWND h_del    = nullptr;
    RECT rc{};                      // 客户区坐标（已含滚动偏移）
};

std::vector<Card> g_cards;
int  g_scroll     = 0;
bool g_show_log   = false;
int  g_card_area_h = 0;
int  g_card_top    = 0;
int  g_card_bottom = 0;
int  g_card_view_w = 0;
int  g_client_w    = 0;
int  g_client_h    = 0;
std::atomic<bool> g_stopping{false};
std::thread g_poll_thread;

// ---------------------------------------------------------------------------
// 基础工具
// ---------------------------------------------------------------------------
int Scale(int v) { return MulDiv(v, g_dpi, 96); }

std::wstring ToW(const std::string& utf8) {
    if (utf8.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (n <= 0) return L"";
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), &w[0], n);
    return w;
}

std::string ToU8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return "";
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

std::string GetCtrlTextU8(HWND hCtrl) {
    if (!hCtrl) return "";
    int len = GetWindowTextLengthW(hCtrl);
    if (len <= 0) return "";
    std::wstring buf(static_cast<size_t>(len + 1), L'\0');
    GetWindowTextW(hCtrl, &buf[0], len + 1);
    buf.resize(static_cast<size_t>(len));
    return p2ptun::Trim(ToU8(buf));
}

void SetCtrlTextU8(HWND hCtrl, const std::string& text) {
    if (!hCtrl) return;
    std::wstring w = ToW(text);
    SetWindowTextW(hCtrl, w.c_str());
}

void UpdateDpiFromWindow(HWND hwnd) {
    UINT dpi = 96;
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        using GetDpiForWindow_t = UINT (WINAPI*)(HWND);
        if (auto fn = reinterpret_cast<GetDpiForWindow_t>(reinterpret_cast<void*>(GetProcAddress(user32, "GetDpiForWindow")))) {
            dpi = fn(hwnd);
        }
    }
    g_dpi = dpi > 0 ? dpi : 96;
}

// 去掉 UTF-8 里的 emoji / 符号，只留文字（GDI 画不出彩色 emoji）
std::string StripEmoji(const std::string& s) {
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if ((c & 0xF8) == 0xF0) {           // 4 字节（emoji 主体）
            i += 4;
            continue;
        }
        if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {   // 3 字节
            unsigned cp = (static_cast<unsigned>(c & 0x0F) << 12) |
                          (static_cast<unsigned>(s[i + 1] & 0x3F) << 6) |
                          static_cast<unsigned>(s[i + 2] & 0x3F);
            if (cp >= 0x2000 && cp <= 0x2BFF) {         // 各类符号/箭头/几何形状
                i += 3;
                continue;
            }
            out.push_back(s[i]); out.push_back(s[i + 1]); out.push_back(s[i + 2]);
            i += 3;
            continue;
        }
        out.push_back(s[i]);
        ++i;
    }
    return p2ptun::Trim(out);
}

std::string DefaultConfigFileNextToExe() {
    wchar_t path[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return "config.json";
    std::wstring p(path, n);
    size_t slash = p.find_last_of(L"\\/");
    if (slash != std::wstring::npos) p = p.substr(0, slash + 1);
    p += L"config.json";
    return ToU8(p);
}

// ---------------------------------------------------------------------------
// 字体 / 画刷
// ---------------------------------------------------------------------------
void CreateUiResources() {
    g_font = CreateFontW(-Scale(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    g_font_bold = CreateFontW(-Scale(14), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    g_font_small = CreateFontW(-Scale(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");

    g_brush_bg    = CreateSolidBrush(CLR_BG);
    g_brush_card  = CreateSolidBrush(CLR_CARD);
    g_brush_field = CreateSolidBrush(CLR_FIELD);
    g_brush_white = CreateSolidBrush(CLR_CARD);
}

void DestroyUiResources() {
    if (g_font)       { DeleteObject(g_font);       g_font = nullptr; }
    if (g_font_bold)  { DeleteObject(g_font_bold);  g_font_bold = nullptr; }
    if (g_font_small) { DeleteObject(g_font_small); g_font_small = nullptr; }
    if (g_brush_bg)    { DeleteObject(g_brush_bg);    g_brush_bg = nullptr; }
    if (g_brush_card)  { DeleteObject(g_brush_card);  g_brush_card = nullptr; }
    if (g_brush_field) { DeleteObject(g_brush_field); g_brush_field = nullptr; }
    if (g_brush_white) { DeleteObject(g_brush_white); g_brush_white = nullptr; }
}

// ---------------------------------------------------------------------------
// 控件工厂
// ---------------------------------------------------------------------------
HWND MakeEdit(HWND parent, int id, const std::string& text, DWORD extra_style = 0) {
    HWND h = CreateWindowExW(
        0, L"EDIT", ToW(text).c_str(),
        WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL | extra_style,
        0, 0, 10, 10, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr), nullptr);
    if (h) {
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
        SendMessageW(h, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELPARAM(Scale(8), Scale(8)));
    }
    return h;
}

HWND MakeButton(HWND parent, int id, const std::wstring& text) {
    HWND h = CreateWindowExW(
        0, L"BUTTON", text.c_str(),
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
        0, 0, 10, 10, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr), nullptr);
    if (h) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    return h;
}

// ---------------------------------------------------------------------------
// 卡片构建
// ---------------------------------------------------------------------------
void DestroyCards() {
    for (auto& c : g_cards) {
        HWND hs[6] = {c.h_id, c.h_port, c.h_server, c.h_client, c.h_stop, c.h_del};
        for (HWND h : hs) {
            if (h) DestroyWindow(h);
        }
    }
    g_cards.clear();
}

void LayoutAll(int width, int height);

void BuildCards() {
    DestroyCards();
    if (!g_mgr) return;
    p2ptun::AppConfig cfg = g_mgr->GetConfig();

    int index = 0;
    for (const auto& r : cfg.tunnels) {
        Card c;
        c.id = r.id;
        c.mode = r.mode.empty() ? "expose" : r.mode;

        int base_id = CARD_BASE + index * CARD_STRIDE;
        c.h_id     = MakeEdit(g_hwnd, base_id + CS_ID, r.id);
        c.h_port   = MakeEdit(g_hwnd, base_id + CS_PORT, p2ptun::ExtractPortOnly(r.port));
        c.h_server = MakeButton(g_hwnd, base_id + CS_SERVER, L"服务端");
        c.h_client = MakeButton(g_hwnd, base_id + CS_CLIENT, L"客户端");
        c.h_stop   = MakeButton(g_hwnd, base_id + CS_STOP, L"停止");
        c.h_del    = MakeButton(g_hwnd, base_id + CS_DEL, L"×");
        EnableWindow(c.h_stop, FALSE);

        g_cards.push_back(c);
        ++index;
    }

    g_scroll = 0;
    RECT rc{};
    GetClientRect(g_hwnd, &rc);
    LayoutAll(rc.right, rc.bottom);
    InvalidateRect(g_hwnd, nullptr, TRUE);
}

// ---------------------------------------------------------------------------
// 布局
// ---------------------------------------------------------------------------
void MoveCtrl(HWND h, int x, int y, int w, int hgt, bool visible) {
    if (!h) return;
    if (!visible) {
        ShowWindow(h, SW_HIDE);
        return;
    }
    ShowWindow(h, SW_SHOWNOACTIVATE);
    SetWindowPos(h, nullptr, x, y, w, hgt, SWP_NOZORDER | SWP_NOACTIVATE);
}

void LayoutAll(int width, int height) {
    if (!g_hwnd) return;
    if (width <= 0 || height <= 0) {
        RECT rc{};
        GetClientRect(g_hwnd, &rc);
        width = rc.right;
        height = rc.bottom;
    }
    g_client_w = width;
    g_client_h = height;

    const int pad = Scale(L_PADX);

    // ---- 顶部 ----
    int row1_y = Scale(12);
    int row1_h = Scale(30);
    int label_w = Scale(56);
    int rand_w  = Scale(88);
    int secret_x = pad + label_w;
    int secret_w = width - secret_x - pad - rand_w - Scale(8);
    if (secret_w < Scale(80)) secret_w = Scale(80);
    MoveCtrl(g_edt_secret, secret_x, row1_y, secret_w, row1_h, true);
    MoveCtrl(g_btn_rand, secret_x + secret_w + Scale(8), row1_y, rand_w, row1_h, true);

    int row2_y = Scale(50);
    int row2_h = Scale(28);
    MoveCtrl(g_btn_add, pad, row2_y, Scale(108), row2_h, true);
    MoveCtrl(g_btn_log, width - pad - Scale(72), row2_y, Scale(72), row2_h, true);
    MoveCtrl(g_btn_clear, width - pad - Scale(72) - Scale(8) - Scale(56),
             row2_y, Scale(56), row2_h, g_show_log);

    // ---- 日志区 ----
    int log_h = g_show_log ? Scale(L_LOG_H) : 0;
    int log_y = height - Scale(10) - log_h;
    MoveCtrl(g_edt_log, pad, log_y, width - pad * 2 - Scale(L_SCROLL_W), log_h, g_show_log);

    // ---- 卡片区 ----
    int card_top = Scale(L_HDR_H) + Scale(8);
    int card_bottom = g_show_log ? (log_y - Scale(8)) : (height - Scale(8));
    g_card_top = card_top;
    g_card_bottom = card_bottom;
    g_card_area_h = card_bottom - card_top;
    if (g_card_area_h < Scale(40)) g_card_area_h = Scale(40);

    int card_w = width - pad * 2 - Scale(L_SCROLL_W);
    if (card_w < Scale(240)) card_w = Scale(240);
    g_card_view_w = card_w;

    const int card_h = Scale(L_CARD_H);
    const int gap    = Scale(L_CARD_GAP);

    int total = static_cast<int>(g_cards.size()) * (card_h + gap);
    int max_scroll = total - g_card_area_h;
    if (max_scroll < 0) max_scroll = 0;
    if (g_scroll > max_scroll) g_scroll = max_scroll;
    if (g_scroll < 0) g_scroll = 0;

    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask  = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin   = 0;
    si.nMax   = total > 0 ? total - gap : 0;
    si.nPage  = static_cast<UINT>(g_card_area_h);
    si.nPos   = g_scroll;
    SetScrollInfo(g_hwnd, SB_VERT, &si, TRUE);

    const int inner = Scale(12);
    const int field_h = Scale(28);
    const int btn_h = Scale(30);

    for (size_t i = 0; i < g_cards.size(); ++i) {
        Card& c = g_cards[i];
        int y = card_top + static_cast<int>(i) * (card_h + gap) - g_scroll;
        c.rc.left   = pad;
        c.rc.right  = pad + card_w;
        c.rc.top    = y;
        c.rc.bottom = y + card_h;

        bool visible = (c.rc.bottom > card_top) && (c.rc.top < card_bottom);

        int cy = y + Scale(14);
        int id_label_w = Scale(22);
        int id_w = Scale(150);
        int port_label_w = Scale(34);
        int port_w = Scale(88);

        int x = pad + inner;
        MoveCtrl(c.h_id, x + id_label_w, cy, id_w, field_h, visible);
        int x2 = x + id_label_w + id_w + Scale(20);
        MoveCtrl(c.h_port, x2 + port_label_w, cy, port_w, field_h, visible);

        MoveCtrl(c.h_del, pad + card_w - Scale(34), y + Scale(10), Scale(24), Scale(24), visible);

        int by = y + card_h - Scale(12) - btn_h;
        int bw1 = Scale(92);
        int bw2 = Scale(92);
        int bw3 = Scale(72);
        int bx = pad + inner;
        MoveCtrl(c.h_server, bx, by, bw1, btn_h, visible);
        MoveCtrl(c.h_client, bx + bw1 + Scale(8), by, bw2, btn_h, visible);
        MoveCtrl(c.h_stop,   bx + bw1 + bw2 + Scale(16), by, bw3, btn_h, visible);
    }

    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// 绘制
// ---------------------------------------------------------------------------
void FillRoundRect(HDC hdc, const RECT& rc, int radius, COLORREF fill, COLORREF border, int border_w) {
    HBRUSH br = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, border_w, border);
    HGDIOBJ old_br = SelectObject(hdc, br);
    HGDIOBJ old_pen = SelectObject(hdc, pen);
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
    SelectObject(hdc, old_br);
    SelectObject(hdc, old_pen);
    DeleteObject(br);
    DeleteObject(pen);
}

void DrawTextIn(HDC hdc, const std::wstring& text, RECT rc, COLORREF color, HFONT font, UINT fmt) {
    HGDIOBJ old_font = SelectObject(hdc, font);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, color);
    DrawTextW(hdc, text.c_str(), -1, &rc, fmt);
    SelectObject(hdc, old_font);
}

void DrawStatusDot(HDC hdc, int cx, int cy, int r, COLORREF color) {
    HBRUSH br = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ ob = SelectObject(hdc, br);
    HGDIOBJ op = SelectObject(hdc, pen);
    Ellipse(hdc, cx - r, cy - r, cx + r, cy + r);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(br);
    DeleteObject(pen);
}

void PaintAll(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT client{};
    GetClientRect(hwnd, &client);
    int w = client.right;
    int h = client.bottom;
    if (w <= 0 || h <= 0) {
        EndPaint(hwnd, &ps);
        return;
    }
    if (g_card_bottom <= g_card_top) {
        g_card_top = Scale(L_HDR_H);
        g_card_bottom = h;
    }

    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ old_bmp = SelectObject(mem, bmp);

    // 背景
    FillRect(mem, &client, g_brush_bg);

    const int pad = Scale(L_PADX);

    // 顶部白底 + 分隔线
    RECT hdr{0, 0, w, Scale(L_HDR_H)};
    FillRect(mem, &hdr, g_brush_card);
    RECT hdr_line{0, Scale(L_HDR_H) - 1, w, Scale(L_HDR_H)};
    FillRect(mem, &hdr_line, g_brush_field);

    // 「Secret」标签
    RECT lbl{pad, Scale(12), pad + Scale(56), Scale(42)};
    DrawTextIn(mem, L"Secret", lbl, CLR_TEXT_DIM, g_font,
               DT_SINGLELINE | DT_VCENTER | DT_LEFT);

    // 卡片（裁剪到卡片区，避免滚动时压到顶部/日志区）
    const int radius = Scale(10);
    int saved_dc = SaveDC(mem);
    IntersectClipRect(mem, 0, g_card_top, w, g_card_bottom);
    for (size_t i = 0; i < g_cards.size(); ++i) {
        const Card& c = g_cards[i];
        if (c.rc.bottom <= g_card_top) continue;
        if (c.rc.top >= g_card_bottom) continue;

        COLORREF border = CLR_CARD_LINE;
        int border_w = 1;
        if (c.running) {
            border = c.failed ? CLR_RED : CLR_GREEN;
            border_w = 2;
        }
        FillRoundRect(mem, c.rc, radius, CLR_CARD, border, border_w);

        int cy = c.rc.top + Scale(14);
        int x = c.rc.left + Scale(12);

        // ID / 端口 标签
        RECT idl{x, cy, x + Scale(22), cy + Scale(28)};
        DrawTextIn(mem, L"ID", idl, CLR_TEXT_FAINT, g_font_small,
                   DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        int x2 = x + Scale(22) + Scale(150) + Scale(20);
        RECT pl{x2, cy, x2 + Scale(34), cy + Scale(28)};
        DrawTextIn(mem, L"端口", pl, CLR_TEXT_FAINT, g_font_small,
                   DT_SINGLELINE | DT_VCENTER | DT_LEFT);

        // 状态行
        int sy = c.rc.top + Scale(52);
        COLORREF dot = CLR_TEXT_FAINT;
        if (c.running) dot = c.failed ? CLR_RED : CLR_GREEN;

        DrawStatusDot(mem, x + Scale(4), sy + Scale(9), Scale(4), dot);

        std::string status = c.status;
        if (status.empty()) status = "未运行";
        if (c.running && !c.mode.empty()) {
            status = (c.mode == "connect" ? "客户端 · " : "服务端 · ") + status;
        }
        RECT st{x + Scale(16), sy, c.rc.right - Scale(150), sy + Scale(18)};
        DrawTextIn(mem, ToW(status), st,
                   c.running ? CLR_TEXT : CLR_TEXT_DIM, g_font,
                   DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);

        if (!c.traffic.empty()) {
            RECT tr{c.rc.right - Scale(160), sy, c.rc.right - Scale(44), sy + Scale(18)};
            DrawTextIn(mem, ToW(c.traffic), tr, CLR_TEXT_FAINT, g_font_small,
                       DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
        }
    }

    // 空状态
    RestoreDC(mem, saved_dc);
    if (g_cards.empty()) {
        RECT empty{pad, Scale(L_HDR_H) + Scale(40), w - pad, Scale(L_HDR_H) + Scale(80)};
        DrawTextIn(mem, L"还没有隧道。点上方「+ 新增隧道」开始。", empty, CLR_TEXT_FAINT,
                   g_font, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    }

    BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old_bmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

// ---------------------------------------------------------------------------
// 按钮自绘
// ---------------------------------------------------------------------------
void DrawOwnButton(const DRAWITEMSTRUCT* dis) {
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;
    int id = static_cast<int>(dis->CtlID);
    int card_index = -1;
    int slot = -1;
    if (id >= CARD_BASE) {
        int off = id - CARD_BASE;
        card_index = off / CARD_STRIDE;
        slot = off % CARD_STRIDE;
    }

    bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    bool pressed  = (dis->itemState & ODS_SELECTED) != 0;

    const Card* card = nullptr;
    if (card_index >= 0 && card_index < static_cast<int>(g_cards.size())) {
        card = &g_cards[static_cast<size_t>(card_index)];
    }

    COLORREF bg = CLR_GRAY_BTN;
    COLORREF fg = CLR_TEXT;
    COLORREF border = CLR_GRAY_LINE;
    int border_w = 1;
    bool fill_background_transparent = false;
    std::wstring text;

    switch (slot) {
        case CS_SERVER: {
            bool active = card && card->running && card->mode == "expose";
            text = L"服务端";
            if (active) {
                bg = pressed ? CLR_GREEN_D : CLR_GREEN;
                fg = RGB(255, 255, 255);
                border = bg;
            } else {
                bg = pressed ? CLR_PRIMARY_D : CLR_PRIMARY;
                fg = RGB(255, 255, 255);
                border = bg;
            }
            break;
        }
        case CS_CLIENT: {
            bool active = card && card->running && card->mode == "connect";
            text = L"客户端";
            if (active) {
                bg = pressed ? CLR_GREEN_D : CLR_GREEN;
                fg = RGB(255, 255, 255);
                border = bg;
            } else {
                bg = pressed ? RGB(0xEC, 0xEE, 0xF1) : RGB(255, 255, 255);
                fg = CLR_TEXT;
                border = CLR_GRAY_LINE;
            }
            break;
        }
        case CS_STOP: {
            text = L"停止";
            if (!card || !card->running) {
                bg = CLR_GRAY_BTN;
                fg = CLR_TEXT_FAINT;
                border = CLR_GRAY_BTN;
                disabled = true;
            } else {
                bg = pressed ? CLR_RED_D : CLR_RED;
                fg = RGB(255, 255, 255);
                border = bg;
            }
            break;
        }
        case CS_DEL: {
            text = L"×";
            fill_background_transparent = true;
            bg = CLR_CARD;
            fg = CLR_TEXT_FAINT;
            border = bg;
            break;
        }
        default: {
            // 顶部按钮
            if (id == IDC_BTN_ADD) {
                text = L"+ 新增隧道";
                bg = pressed ? CLR_PRIMARY_D : CLR_PRIMARY;
                fg = RGB(255, 255, 255);
                border = bg;
            } else {
                text = L"";
                bg = pressed ? RGB(0xEC, 0xEE, 0xF1) : RGB(255, 255, 255);
                fg = CLR_TEXT_DIM;
                border = CLR_GRAY_LINE;
            }
            break;
        }
    }

    // 顶部次级按钮文字（随机 / 日志 / 清空）
    if (slot < 0) {
        if (id == IDC_BTN_RAND_SEC) text = L"随机";
        else if (id == IDC_BTN_LOG) text = g_show_log ? L"收起日志" : L"日志";
        else if (id == IDC_BTN_CLEAR_LOG) text = L"清空";
    }

    if (disabled) {
        bg = CLR_GRAY_BTN;
        fg = CLR_TEXT_FAINT;
        border = CLR_GRAY_BTN;
        fill_background_transparent = false;
    }

    int radius = Scale(6);

    if (fill_background_transparent) {
        // 透明按钮：用父窗口底色填充
        FillRect(hdc, &rc, g_brush_card);
        // 悬停/按下时给一点背景
        if (pressed) {
            FillRoundRect(hdc, rc, radius, RGB(0xFE, 0xE2, 0xE2), RGB(0xFE, 0xE2, 0xE2), 1);
            fg = CLR_RED;
        }
        SetBkMode(hdc, TRANSPARENT);
        HGDIOBJ old_font = SelectObject(hdc, g_font_bold);
        SetTextColor(hdc, fg);
        DrawTextW(hdc, text.c_str(), -1, &rc, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        SelectObject(hdc, old_font);
        return;
    }

    FillRoundRect(hdc, rc, radius, bg, border, border_w);

    SetBkMode(hdc, TRANSPARENT);
    HGDIOBJ old_font = SelectObject(hdc, g_font);
    SetTextColor(hdc, fg);
    RECT tr = rc;
    if (pressed) OffsetRect(&tr, 0, 1);
    DrawTextW(hdc, text.c_str(), -1, &tr,
              DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS);
    SelectObject(hdc, old_font);
}

// ---------------------------------------------------------------------------
// 配置操作
// ---------------------------------------------------------------------------
std::string CurrentSecret() {
    return GetCtrlTextU8(g_edt_secret);
}

void SaveSecretToConfig() {
    if (!g_mgr) return;
    std::string secret = CurrentSecret();
    if (secret.empty()) return;
    p2ptun::AppConfig cfg = g_mgr->GetConfig();
    if (cfg.default_secret == secret) return;
    std::string err;
    g_mgr->UpdateGlobalSettings(cfg.p2ptun_path, secret, cfg.default_proto,
                                cfg.default_bandwidth, cfg.minimize_to_tray, err);
}

void RefreshStatuses() {
    if (!g_mgr || !g_hwnd) return;
    std::vector<p2ptun::TunnelRowView> rows = g_mgr->GetRowViews();

    if (rows.size() != g_cards.size()) {
        BuildCards();
        return;
    }

    bool changed = false;
    for (size_t i = 0; i < rows.size(); ++i) {
        Card& c = g_cards[i];
        bool run = rows[i].running;
        bool failed = (rows[i].status_text.find("异常") != std::string::npos);
        std::string st = StripEmoji(rows[i].status_text);
        if (st.empty()) st = run ? "运行中" : "未运行";
        if (st == "已停止") st = "未运行";
        std::string tr = rows[i].traffic_text;
        std::string md = rows[i].mode;

        if (c.running != run || c.failed != failed || c.status != st ||
            c.traffic != tr || c.mode != md) {
            c.running = run;
            c.failed = failed;
            c.status = st;
            c.traffic = tr;
            c.mode = md;
            EnableWindow(c.h_stop, run ? TRUE : FALSE);
            InvalidateRect(c.h_server, nullptr, TRUE);
            InvalidateRect(c.h_client, nullptr, TRUE);
            InvalidateRect(c.h_stop, nullptr, TRUE);
            changed = true;
        }
    }
    if (changed) {
        RECT rc{};
        GetClientRect(g_hwnd, &rc);
        InvalidateRect(g_hwnd, &rc, FALSE);
    }
}

// 把某张卡片的编辑框写回配置（不启动）
bool ApplyCardToConfig(size_t index, p2ptun::TunnelRule& out_rule, std::string& out_err) {
    if (!g_mgr || index >= g_cards.size()) {
        out_err = "无效的隧道索引";
        return false;
    }
    Card& c = g_cards[index];
    std::string new_id = GetCtrlTextU8(c.h_id);
    std::string port   = GetCtrlTextU8(c.h_port);

    if (new_id.empty()) {
        out_err = "隧道 ID 不能为空";
        return false;
    }
    if (port.empty()) {
        out_err = "端口不能为空";
        return false;
    }

    p2ptun::AppConfig cfg = g_mgr->GetConfig();
    p2ptun::TunnelRule rule;
    bool found = false;
    for (const auto& r : cfg.tunnels) {
        if (r.id == c.id) { rule = r; found = true; break; }
    }
    if (!found) rule = p2ptun::TunnelRule{};

    std::string old_id = rule.id.empty() ? c.id : rule.id;
    rule.id   = new_id;
    rule.name = new_id;
    rule.port = port;

    std::string out_id;
    if (!g_mgr->AddOrUpdateRule(rule, false, out_id, out_err)) {
        return false;
    }
    if (!old_id.empty() && old_id != out_id) {
        std::string derr;
        g_mgr->DeleteRule(old_id, derr);
    }
    c.id = out_id;
    out_rule = rule;
    out_rule.id = out_id;
    out_rule.name = out_id;
    return true;
}

void StartCard(size_t index, const char* mode) {
    if (!g_mgr || index >= g_cards.size()) return;
    SaveSecretToConfig();

    p2ptun::TunnelRule rule;
    std::string err;
    if (!ApplyCardToConfig(index, rule, err)) {
        MessageBoxW(g_hwnd, ToW(err).c_str(), L"无法启动", MB_OK | MB_ICONWARNING);
        return;
    }

    rule.mode = mode;
    std::string out_id;
    if (!g_mgr->AddOrUpdateRule(rule, true, out_id, err)) {
        MessageBoxW(g_hwnd, ToW(err).c_str(), L"启动失败", MB_OK | MB_ICONERROR);
    }
    RefreshStatuses();
}

void StopCard(size_t index) {
    if (!g_mgr || index >= g_cards.size()) return;
    std::string err;
    if (!g_mgr->StopTunnel(g_cards[index].id, err)) {
        MessageBoxW(g_hwnd, ToW(err).c_str(), L"停止失败", MB_OK | MB_ICONERROR);
    }
    RefreshStatuses();
}

void DeleteCard(size_t index) {
    if (!g_mgr || index >= g_cards.size()) return;
    std::string id = g_cards[index].id;
    std::wstring msg = L"删除隧道 " + ToW(id) + L" ?";
    if (MessageBoxW(g_hwnd, msg.c_str(), L"确认删除",
                    MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
        return;
    }
    std::string err;
    g_mgr->DeleteRule(id, err);
    BuildCards();
}

void AddCard() {
    if (!g_mgr) return;
    p2ptun::TunnelRule rule;
    rule.id   = p2ptun::GenerateRandomTunnelId();
    rule.name = rule.id;
    rule.port = "3389";
    rule.mode = "expose";

    std::string out_id, err;
    if (!g_mgr->AddOrUpdateRule(rule, false, out_id, err)) {
        MessageBoxW(g_hwnd, ToW(err).c_str(), L"新增失败", MB_OK | MB_ICONERROR);
        return;
    }
    BuildCards();

    // 滚到最底部并聚焦新卡片的端口框
    if (!g_cards.empty()) {
        int card_h = Scale(L_CARD_H) + Scale(L_CARD_GAP);
        int total = static_cast<int>(g_cards.size()) * card_h;
        g_scroll = std::max(0, total - g_card_area_h);
        RECT rc{};
        GetClientRect(g_hwnd, &rc);
        LayoutAll(rc.right, rc.bottom);
        HWND target = g_cards.back().h_port;
        if (target) {
            SetFocus(target);
            SendMessageW(target, EM_SETSEL, 0, -1);
        }
    }
}

void ToggleLog() {
    g_show_log = !g_show_log;
    RECT rc{};
    GetClientRect(g_hwnd, &rc);
    LayoutAll(rc.right, rc.bottom);
    if (g_hwnd) InvalidateRect(g_hwnd, nullptr, TRUE);
}

void SyncLogText() {
    if (!g_mgr || !g_edt_log || !g_show_log) return;
    std::string text = g_mgr->Logs().GetText(200, "\r\n");
    std::wstring w = ToW(text);
    int len = GetWindowTextLengthW(g_edt_log);
    std::wstring cur(static_cast<size_t>(len + 1), L'\0');
    if (len > 0) GetWindowTextW(g_edt_log, &cur[0], len + 1);
    cur.resize(static_cast<size_t>(len));
    if (cur != w) {
        SetWindowTextW(g_edt_log, w.c_str());
        SendMessageW(g_edt_log, EM_SELENDOK, 0, 0);
        SendMessageW(g_edt_log, WM_VSCROLL, SB_BOTTOM, 0);
    }
}

// ---------------------------------------------------------------------------
// 命令处理
// ---------------------------------------------------------------------------
void HandleCommand(HWND hwnd, int id, int notify_code) {
    if (id >= CARD_BASE) {
        int off = id - CARD_BASE;
        int index = off / CARD_STRIDE;
        int slot = off % CARD_STRIDE;
        if (index < 0 || index >= static_cast<int>(g_cards.size())) return;
        if (notify_code != BN_CLICKED) return;

        switch (slot) {
            case CS_SERVER: StartCard(static_cast<size_t>(index), "expose");  break;
            case CS_CLIENT: StartCard(static_cast<size_t>(index), "connect"); break;
            case CS_STOP:   StopCard(static_cast<size_t>(index));             break;
            case CS_DEL:    DeleteCard(static_cast<size_t>(index));           break;
            default: break;
        }
        return;
    }

    switch (id) {
        case IDC_BTN_ADD:
            if (notify_code == BN_CLICKED) AddCard();
            break;
        case IDC_BTN_RAND_SEC:
            if (notify_code == BN_CLICKED) {
                SetCtrlTextU8(g_edt_secret, p2ptun::GenerateRandomSecret());
                SaveSecretToConfig();
            }
            break;
        case IDC_BTN_LOG:
            if (notify_code == BN_CLICKED) ToggleLog();
            break;
        case IDC_BTN_CLEAR_LOG:
            if (notify_code == BN_CLICKED && g_mgr) {
                g_mgr->Logs().Clear();
                if (g_edt_log) SetWindowTextW(g_edt_log, L"");
            }
            break;
        case IDC_EDT_SECRET:
            if (notify_code == EN_KILLFOCUS) SaveSecretToConfig();
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// 主窗口过程
// ---------------------------------------------------------------------------
LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            g_hwnd = hwnd;
            UpdateDpiFromWindow(hwnd);
            CreateUiResources();

            g_edt_secret = MakeEdit(hwnd, IDC_EDT_SECRET, "");
            g_btn_rand   = MakeButton(hwnd, IDC_BTN_RAND_SEC, L"随机");
            g_btn_add    = MakeButton(hwnd, IDC_BTN_ADD, L"+ 新增隧道");
            g_btn_log    = MakeButton(hwnd, IDC_BTN_LOG, L"日志");
            g_edt_log    = CreateWindowExW(
                0, L"EDIT", L"",
                WS_CHILD | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
                0, 0, 10, 10, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EDT_LOG)),
                GetModuleHandleW(nullptr), nullptr);
            if (g_edt_log) {
                SendMessageW(g_edt_log, WM_SETFONT, reinterpret_cast<WPARAM>(g_font_small), TRUE);
            }
            g_btn_clear = MakeButton(hwnd, IDC_BTN_CLEAR_LOG, L"清空");

            if (g_mgr) {
                SetCtrlTextU8(g_edt_secret, g_mgr->GetConfig().default_secret);
            }
            BuildCards();
            return 0;
        }

        case WM_DPICHANGED: {
            UpdateDpiFromWindow(hwnd);
            DestroyUiResources();
            CreateUiResources();
            // 重新给所有控件换字体
            for (auto& c : g_cards) {
                HWND hs[6] = {c.h_id, c.h_port, c.h_server, c.h_client, c.h_stop, c.h_del};
                for (HWND h : hs) {
                    if (h) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
                }
            }
            HWND tops[4] = {g_edt_secret, g_btn_rand, g_btn_add, g_btn_log};
            for (HWND h : tops) {
                if (h) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
            }
            if (g_edt_log) SendMessageW(g_edt_log, WM_SETFONT, reinterpret_cast<WPARAM>(g_font_small), TRUE);

            RECT* suggested = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            RECT rc{};
            GetClientRect(hwnd, &rc);
            LayoutAll(rc.right, rc.bottom);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }

        case WM_SIZE: {
            if (wParam != SIZE_MINIMIZED) {
                LayoutAll(LOWORD(lParam), HIWORD(lParam));
            }
            return 0;
        }

        case WM_GETMINMAXINFO: {
            MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = Scale(470);
            mmi->ptMinTrackSize.y = Scale(380);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
            PaintAll(hwnd);
            return 0;

        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT* dis = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            if (dis) DrawOwnButton(dis);
            return TRUE;
        }

        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            HWND ctrl = reinterpret_cast<HWND>(lParam);
            if (ctrl == g_edt_log) {
                SetTextColor(hdc, CLR_TEXT_DIM);
                SetBkColor(hdc, CLR_CARD);
                return reinterpret_cast<LRESULT>(g_brush_white);
            }
            SetTextColor(hdc, CLR_TEXT);
            SetBkColor(hdc, CLR_FIELD);
            return reinterpret_cast<LRESULT>(g_brush_field);
        }

        case WM_VSCROLL: {
            SCROLLINFO si{};
            si.cbSize = sizeof(si);
            si.fMask = SIF_ALL;
            GetScrollInfo(hwnd, SB_VERT, &si);
            int pos = si.nPos;
            switch (LOWORD(wParam)) {
                case SB_LINEUP:   pos -= Scale(30); break;
                case SB_LINEDOWN: pos += Scale(30); break;
                case SB_PAGEUP:   pos -= static_cast<int>(si.nPage); break;
                case SB_PAGEDOWN: pos += static_cast<int>(si.nPage); break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION: pos = si.nTrackPos; break;
                case SB_TOP:      pos = si.nMin; break;
                case SB_BOTTOM:   pos = si.nMax; break;
                default: return 0;
            }
            int max_pos = si.nMax - static_cast<int>(si.nPage) + 1;
            if (pos > max_pos) pos = max_pos;
            if (pos < si.nMin) pos = si.nMin;
            if (pos != g_scroll) {
                g_scroll = pos;
                SetScrollPos(hwnd, SB_VERT, g_scroll, TRUE);
                RECT rc{};
                GetClientRect(hwnd, &rc);
                LayoutAll(rc.right, rc.bottom);
            }
            return 0;
        }

        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            SCROLLINFO si{};
            si.cbSize = sizeof(si);
            si.fMask = SIF_ALL;
            GetScrollInfo(hwnd, SB_VERT, &si);
            int pos = g_scroll - MulDiv(delta, Scale(60), WHEEL_DELTA);
            int max_pos = si.nMax - static_cast<int>(si.nPage) + 1;
            if (pos > max_pos) pos = max_pos;
            if (pos < si.nMin) pos = si.nMin;
            if (pos != g_scroll) {
                g_scroll = pos;
                SetScrollPos(hwnd, SB_VERT, g_scroll, TRUE);
                RECT rc{};
                GetClientRect(hwnd, &rc);
                LayoutAll(rc.right, rc.bottom);
            }
            return 0;
        }

        case WM_COMMAND:
            HandleCommand(hwnd, LOWORD(wParam), HIWORD(wParam));
            return 0;

        case WM_APP_REFRESH:
            RefreshStatuses();
            SyncLogText();
            return 0;

        case WM_CLOSE: {
            if (g_mgr) {
                // 把编辑框里的内容落盘（不启动），再停掉所有隧道
                for (size_t i = 0; i < g_cards.size(); ++i) {
                    p2ptun::TunnelRule tmp;
                    std::string err;
                    ApplyCardToConfig(i, tmp, err);
                }
                SaveSecretToConfig();
                g_mgr->StopAll(false);
            }
            DestroyWindow(hwnd);
            return 0;
        }

        case WM_DESTROY:
            g_stopping.store(true);
            g_hwnd = nullptr;
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// 入口
// ---------------------------------------------------------------------------
int RunGuiMain(HINSTANCE hInstance, int nCmdShow) {
    HANDLE single_mutex = CreateMutexW(nullptr, TRUE, L"Global\\P2PTunManager_SingleInstance_Mutex");
    if (single_mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"p2ptun 管理器已在运行中。", L"p2ptun", MB_OK | MB_ICONINFORMATION);
        ReleaseMutex(single_mutex);
        return 0;
    }

    SetProcessDPIAware();

    INITCOMMONCONTROLSEX icex{};
    icex.dwSize = sizeof(icex);
    icex.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icex);

    auto mgr = std::make_unique<p2ptun::MultiTunnelManager>(DefaultConfigFileNextToExe());
    g_mgr = mgr.get();
    mgr->StartAutoEnabled();

    const wchar_t* kClassName = L"P2PTunMinimalCardClass";
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = MainWndProc;
    wc.hInstance     = hInstance;
    wc.hIcon         = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClassName;
    wc.hIconSm       = wc.hIcon;
    RegisterClassExW(&wc);

    UpdateDpiFromWindow(GetDesktopWindow());
    HWND hwnd = CreateWindowExW(
        0, kClassName, L"p2ptun 穿透管理器",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_VSCROLL,
        CW_USEDEFAULT, CW_USEDEFAULT, Scale(600), Scale(720),
        nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) return 1;

    std::atomic<bool>* stop_flag = &g_stopping;
    g_poll_thread = std::thread([hwnd, mgr_ptr = mgr.get(), stop_flag]() {
        while (!stop_flag->load()) {
            mgr_ptr->PollAllStatuses();
            if (!IsWindow(hwnd)) break;
            PostMessageW(hwnd, WM_APP_REFRESH, 0, 0);
            for (int i = 0; i < 8 && !stop_flag->load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    });

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    g_stopping.store(true);
    if (g_poll_thread.joinable()) g_poll_thread.join();
    mgr->StopAll(false);
    g_mgr = nullptr;

    DestroyUiResources();

    if (single_mutex) {
        ReleaseMutex(single_mutex);
        CloseHandle(single_mutex);
    }
    return static_cast<int>(msg.wParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    return RunGuiMain(hInstance, nCmdShow);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    return RunGuiMain(hInstance, nCmdShow);
}

int main() {
    return RunGuiMain(GetModuleHandleW(nullptr), SW_SHOWNORMAL);
}

#else
#include <iostream>
int main() {
    p2ptun::MultiTunnelManager mgr("config.json");
    std::cout << "p2ptun-manager core initialized (" << mgr.GetRowViews().size() << " rules).\n";
    return 0;
}
#endif
