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

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")

namespace {

enum ControlId : int {
    IDC_EDT_P2PTUN_PATH   = 1001,
    IDC_BTN_BROWSE_EXE    = 1002,
    IDC_EDT_DEF_SECRET    = 1003,
    IDC_CMB_DEF_PROTO     = 1004,
    IDC_EDT_DEF_BW        = 1005,
    IDC_CHK_MIN_TRAY      = 1006,
    IDC_BTN_SAVE_GLOBAL   = 1007,
    IDC_BTN_OPEN_CONFIG   = 1008,

    IDC_CMB_MODE          = 1020,
    IDC_CMB_PRESET        = 1021,
    IDC_EDT_REMARK        = 1022,
    IDC_EDT_NAME          = 1023,
    IDC_EDT_PORT          = 1024,
    IDC_EDT_SECRET        = 1025,
    IDC_CMB_PROTO         = 1026,
    IDC_EDT_BW            = 1027,
    IDC_BTN_ADD_START     = 1028,
    IDC_BTN_PASTE_PAIR    = 1029,

    IDC_EDT_ADVANCED      = 1030,   // 高级参数 (k=v 空格分隔，透传 p2ptun flags)
    IDC_CMB_SERIAL_PORT   = 1031,   // 串口桥接: 串口 (留空=禁用)
    IDC_CMB_SERIAL_BAUD   = 1032,   // 串口桥接: 波特率
    IDC_CMB_SERIAL_PARITY = 1033,   // 串口桥接: 数据/校验/停止位
    IDC_BTN_ADV_HELP      = 1034,

    IDC_LIST_TUNNELS      = 1040,

    IDC_BTN_START_ONE     = 1050,
    IDC_BTN_STOP_ONE      = 1051,
    IDC_BTN_RECONN_ONE    = 1052,
    IDC_BTN_COPY_PAIR     = 1053,
    IDC_BTN_COPY_CLI      = 1054,
    IDC_BTN_LAUNCH_APP    = 1055,
    IDC_BTN_DELETE_ONE    = 1056,
    IDC_BTN_START_ALL     = 1057,
    IDC_BTN_STOP_ALL      = 1058,

    IDC_EDT_LOGS          = 1070,
    IDC_BTN_CLEAR_LOG     = 1071,

    ID_TRAY_OPEN          = 2001,
    ID_TRAY_START_ALL     = 2002,
    ID_TRAY_STOP_ALL      = 2003,
    ID_TRAY_OPEN_CFG      = 2004,
    ID_TRAY_EXIT          = 2005
};

constexpr UINT WM_APP_TRAY    = WM_APP + 10;
constexpr UINT WM_APP_REFRESH = WM_APP + 11;
constexpr UINT_PTR TIMER_UI   = 1;

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

bool CopyTextToClipboard(HWND hwnd, const std::string& utf8) {
    if (!OpenClipboard(hwnd)) return false;
    EmptyClipboard();
    std::wstring w = ToW(utf8);
    size_t bytes = (w.size() + 1) * sizeof(wchar_t);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!hMem) {
        CloseClipboard();
        return false;
    }
    void* ptr = GlobalLock(hMem);
    if (ptr) {
        memcpy(ptr, w.c_str(), bytes);
        GlobalUnlock(hMem);
        SetClipboardData(CF_UNICODETEXT, hMem);
    } else {
        GlobalFree(hMem);
    }
    CloseClipboard();
    return true;
}

std::string ReadTextFromClipboard(HWND hwnd) {
    if (!OpenClipboard(hwnd)) return "";
    std::string out;
    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    if (hData) {
        const wchar_t* ptr = static_cast<const wchar_t*>(GlobalLock(hData));
        if (ptr) {
            out = ToU8(std::wstring(ptr));
            GlobalUnlock(hData);
        }
    }
    CloseClipboard();
    return out;
}

std::string DefaultConfigFileNextToExe() {
    wchar_t buf[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, buf, MAX_PATH) > 0) {
        std::wstring path(buf);
        auto pos = path.find_last_of(L"\\/");
        if (pos != std::wstring::npos) {
            return ToU8(path.substr(0, pos + 1) + L"config.json");
        }
    }
    return "config.json";
}

struct GuiState {
    HWND hwnd = nullptr;
    HFONT hFont = nullptr;
    HFONT hFontBold = nullptr;
    HFONT hFontMono = nullptr;

    HWND grpGlobal = nullptr;
    HWND edtExePath = nullptr;
    HWND btnBrowseExe = nullptr;
    HWND edtDefSecret = nullptr;
    HWND cmbDefProto = nullptr;
    HWND edtDefBw = nullptr;
    HWND chkMinTray = nullptr;
    HWND btnSaveGlobal = nullptr;
    HWND btnOpenCfg = nullptr;

    HWND grpAdd = nullptr;
    HWND cmbMode = nullptr;
    HWND cmbPreset = nullptr;
    HWND edtRemark = nullptr;
    HWND edtName = nullptr;
    HWND edtPort = nullptr;
    HWND edtSecret = nullptr;
    HWND cmbProto = nullptr;
    HWND edtBw = nullptr;
    HWND btnAddStart = nullptr;
    HWND btnPastePair = nullptr;
    HWND edtAdvanced = nullptr;
    HWND cmbSerialPort = nullptr;
    HWND cmbSerialBaud = nullptr;
    HWND cmbSerialParity = nullptr;
    HWND btnAdvHelp = nullptr;

    HWND listTunnels = nullptr;

    HWND btnStartOne = nullptr;
    HWND btnStopOne = nullptr;
    HWND btnReconnOne = nullptr;
    HWND btnCopyPair = nullptr;
    HWND btnCopyCli = nullptr;
    HWND btnLaunchApp = nullptr;
    HWND btnDeleteOne = nullptr;
    HWND btnStartAll = nullptr;
    HWND btnStopAll = nullptr;

    HWND grpLogs = nullptr;
    HWND btnClearLog = nullptr;
    HWND edtLogs = nullptr;

    std::unique_ptr<p2ptun::MultiTunnelManager> mgr;
    std::vector<p2ptun::TunnelRowView> cached_rows;
    std::vector<std::vector<std::wstring>> rendered_cells;
    std::string editing_rule_id;
    uint64_t last_log_seq = 0;
    bool tray_added = false;
    bool tray_balloon_shown = false;
    std::atomic<bool> quitting{false};
    std::thread poll_thread;
};

GuiState* g_state = nullptr;

void SetFontRecursive(HWND hwnd, HFONT font) {
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

int GetSelectedRowIndex() {
    if (!g_state || !g_state->listTunnels) return -1;
    return static_cast<int>(SendMessageW(g_state->listTunnels, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_SELECTED));
}

std::string GetSelectedTunnelId() {
    int idx = GetSelectedRowIndex();
    if (idx < 0 || idx >= static_cast<int>(g_state->cached_rows.size())) return "";
    return g_state->cached_rows[static_cast<size_t>(idx)].id;
}

void LoadGlobalControlsFromConfig() {
    if (!g_state || !g_state->mgr) return;
    p2ptun::AppConfig cfg = g_state->mgr->GetConfig();
    SetCtrlTextU8(g_state->edtExePath, cfg.p2ptun_path);
    SetCtrlTextU8(g_state->edtDefSecret, cfg.default_secret);
    SetCtrlTextU8(g_state->edtDefBw, std::to_string(cfg.default_bandwidth));
    SendMessageW(g_state->chkMinTray, BM_SETCHECK, cfg.minimize_to_tray ? BST_CHECKED : BST_UNCHECKED, 0);

    int proto_idx = 0;
    if (cfg.default_proto == "quic") proto_idx = 1;
    else if (cfg.default_proto == "classic") proto_idx = 2;
    SendMessageW(g_state->cmbDefProto, CB_SETCURSEL, static_cast<WPARAM>(proto_idx), 0);
}

void ApplyPresetSelection() {
    if (!g_state) return;
    auto presets = p2ptun::GetScenePresets();
    int p_idx = static_cast<int>(SendMessageW(g_state->cmbPreset, CB_GETCURSEL, 0, 0));
    if (p_idx <= 0 || p_idx >= static_cast<int>(presets.size())) return;

    int mode_idx = static_cast<int>(SendMessageW(g_state->cmbMode, CB_GETCURSEL, 0, 0));
    bool is_connect = (mode_idx == 1);
    const auto& preset = presets[static_cast<size_t>(p_idx)];

    g_state->editing_rule_id.clear();
    SetCtrlTextU8(g_state->edtRemark, is_connect ? preset.remark_connect : preset.remark_expose);
    SetCtrlTextU8(g_state->edtName, preset.default_name);
    SetCtrlTextU8(g_state->edtPort, is_connect ? preset.connect_port : preset.expose_port);
}

void PopulateQuickBarFromRow(int row_idx) {
    if (!g_state || row_idx < 0 || row_idx >= static_cast<int>(g_state->cached_rows.size())) return;
    const auto& row = g_state->cached_rows[static_cast<size_t>(row_idx)];
    g_state->editing_rule_id = row.id;

    SendMessageW(g_state->cmbMode, CB_SETCURSEL, (row.mode == "connect") ? 1 : 0, 0);
    SendMessageW(g_state->cmbPreset, CB_SETCURSEL, 0, 0);
    SetCtrlTextU8(g_state->edtRemark, row.remark);
    SetCtrlTextU8(g_state->edtName, row.name);
    SetCtrlTextU8(g_state->edtPort, row.port_short);
    SetCtrlTextU8(g_state->edtSecret, row.rule.secret);
    SetCtrlTextU8(g_state->edtBw, std::to_string(row.rule.bandwidth_mbps));

    int proto_idx = 0;
    if (row.rule.proto == "quic") proto_idx = 1;
    else if (row.rule.proto == "classic") proto_idx = 2;
    SendMessageW(g_state->cmbProto, CB_SETCURSEL, static_cast<WPARAM>(proto_idx), 0);

    // 高级参数回填: 生成 "k=v k=v" 字符串
    std::string adv;
    auto append_kv = [&adv](const char* k, const std::string& v) {
        if (!p2ptun::Trim(v).empty()) adv += std::string(k) + "=" + v + " ";
    };
    append_kv("broker", row.rule.broker);
    append_kv("stun", row.rule.stun);
    append_kv("turn", row.rule.turn);
    append_kv("proxy", row.rule.proxy);
    append_kv("key", row.rule.peer_key);
    append_kv("keepalive", row.rule.keepalive);
    append_kv("ice_ports", row.rule.ice_ports);
    append_kv("nat1to1", row.rule.nat1to1);
    append_kv("idle_timeout", row.rule.idle_timeout);
    if (row.rule.max_peers > 0) adv += "max_peers=" + std::to_string(row.rule.max_peers) + " ";
    if (row.rule.force_relay) adv += "force_relay=1 ";
    if (row.rule.keep_proxy) adv += "keep_proxy=1 ";
    SetCtrlTextU8(g_state->edtAdvanced, p2ptun::Trim(adv));

    // 串口桥接回填
    if (row.rule.serial_port.empty()) {
        SendMessageW(g_state->cmbSerialPort, CB_SELECTSTRING, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(L"(不启用)"));
        SendMessageW(g_state->cmbSerialPort, CB_SETCURSEL, 0, 0);
    } else {
        SendMessageW(g_state->cmbSerialPort, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(ToW(row.rule.serial_port).c_str()));
    }
    std::wstring baud_w = ToW(std::to_string(row.rule.serial_baud > 0 ? row.rule.serial_baud : 115200));
    int baud_idx = static_cast<int>(SendMessageW(g_state->cmbSerialBaud, CB_SELECTSTRING, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(baud_w.c_str())));
    if (baud_idx == CB_ERR) SendMessageW(g_state->cmbSerialBaud, CB_SETCURSEL, 4, 0);
    SendMessageW(g_state->cmbSerialParity, CB_SELECTSTRING, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(ToW(row.rule.serial_params).c_str()));
    if (SendMessageW(g_state->cmbSerialParity, CB_GETCURSEL, 0, 0) == CB_ERR) SendMessageW(g_state->cmbSerialParity, CB_SETCURSEL, 0, 0);
}

void RefreshTunnelListView() {
    if (!g_state || !g_state->mgr || !g_state->listTunnels) return;

    auto rows = g_state->mgr->GetRowViews();
    g_state->cached_rows = rows;

    HWND hList = g_state->listTunnels;
    bool rebuild = (rows.size() != g_state->rendered_cells.size());
    int prev_sel = GetSelectedRowIndex();

    if (rebuild) {
        SendMessageW(hList, WM_SETREDRAW, FALSE, 0);
        SendMessageW(hList, LVM_DELETEALLITEMS, 0, 0);
        g_state->rendered_cells.clear();
        g_state->rendered_cells.resize(rows.size());
    }

    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& r = rows[i];
        std::vector<std::wstring> cols = {
            ToW(std::to_string(r.index)),
            ToW(r.status_text),
            ToW(r.mode_label),
            ToW(r.remark),
            ToW(r.name),
            ToW(r.port),
            ToW(r.proto_bw_text),
            ToW(r.traffic_text),
            ToW(r.detail_text)
        };

        if (rebuild) {
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = static_cast<int>(i);
            item.iSubItem = 0;
            item.pszText = const_cast<LPWSTR>(cols[0].c_str());
            SendMessageW(hList, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));

            for (size_t c = 1; c < cols.size(); ++c) {
                LVITEMW sub{};
                sub.iSubItem = static_cast<int>(c);
                sub.pszText = const_cast<LPWSTR>(cols[c].c_str());
                SendMessageW(hList, LVM_SETITEMTEXTW, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(&sub));
            }
            g_state->rendered_cells[i] = std::move(cols);
        } else {
            for (size_t c = 0; c < cols.size(); ++c) {
                if (g_state->rendered_cells[i].size() <= c || g_state->rendered_cells[i][c] != cols[c]) {
                    LVITEMW sub{};
                    sub.iSubItem = static_cast<int>(c);
                    sub.pszText = const_cast<LPWSTR>(cols[c].c_str());
                    SendMessageW(hList, LVM_SETITEMTEXTW, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(&sub));
                }
            }
            g_state->rendered_cells[i] = std::move(cols);
        }
    }

    if (rebuild) {
        if (prev_sel >= 0 && prev_sel < static_cast<int>(rows.size())) {
            LVITEMW sel{};
            sel.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
            sel.state = LVIS_SELECTED | LVIS_FOCUSED;
            SendMessageW(hList, LVM_SETITEMSTATE, static_cast<WPARAM>(prev_sel), reinterpret_cast<LPARAM>(&sel));
        }
        SendMessageW(hList, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(hList, nullptr, FALSE);
    }

    uint64_t cur_seq = g_state->mgr->Logs().Seq();
    if (cur_seq != g_state->last_log_seq) {
        g_state->last_log_seq = cur_seq;
        std::string log_text = g_state->mgr->Logs().GetText(250, "\r\n");
        SetCtrlTextU8(g_state->edtLogs, log_text);
        int text_len = GetWindowTextLengthW(g_state->edtLogs);
        SendMessageW(g_state->edtLogs, EM_SETSEL, static_cast<WPARAM>(text_len), static_cast<LPARAM>(text_len));
        SendMessageW(g_state->edtLogs, EM_SCROLLCARET, 0, 0);
    }
}

void AddOrUpdateTrayIcon(HWND hwnd) {
    if (!g_state) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(nid.szTip, L"p2ptun 多端口穿透管理器 (双击打开窗口)");
    Shell_NotifyIconW(g_state->tray_added ? NIM_MODIFY : NIM_ADD, &nid);
    g_state->tray_added = true;
}

void ShowTrayBalloon(HWND hwnd, const wchar_t* title, const wchar_t* msg) {
    if (!g_state || !g_state->tray_added) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    wcsncpy_s(nid.szInfoTitle, title, _TRUNCATE);
    wcsncpy_s(nid.szInfo, msg, _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void RemoveTrayIcon(HWND hwnd) {
    if (!g_state || !g_state->tray_added) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_state->tray_added = false;
}

void ShowTrayContextMenu(HWND hwnd) {
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_OPEN, L"🗔 打开主窗口");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_START_ALL, L"▶ 全部启动");
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_STOP_ALL, L"⏹ 全部停止");
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_OPEN_CFG, L"📂 打开 config.json");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"❌ 彻底退出并关闭所有隧道");

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(hMenu);
}

void LayoutControls(int width, int height) {
    if (!g_state || width <= 0 || height <= 0) return;

    int pad = 10;
    int content_w = width - pad * 2;
    if (content_w < 760) content_w = 760;

    MoveWindow(g_state->grpGlobal, pad, 6, content_w, 56, TRUE);
    MoveWindow(g_state->grpAdd, pad, 68, content_w, 90, TRUE);

    int log_h = 155;
    int action_h = 34;
    int list_top = 198;
    int list_h = height - list_top - action_h - log_h - 28;
    if (list_h < 140) list_h = 140;

    MoveWindow(g_state->listTunnels, pad, list_top, content_w, list_h, TRUE);

    int btn_y = list_top + list_h + 6;
    int bx = pad;
    auto placeBtn = [&](HWND hBtn, int w) {
        MoveWindow(hBtn, bx, btn_y, w, 30, TRUE);
        bx += w + 6;
    };
    placeBtn(g_state->btnStartOne, 92);
    placeBtn(g_state->btnStopOne, 92);
    placeBtn(g_state->btnReconnOne, 92);
    placeBtn(g_state->btnCopyPair, 146);
    placeBtn(g_state->btnCopyCli, 115);
    placeBtn(g_state->btnLaunchApp, 155);
    placeBtn(g_state->btnDeleteOne, 90);
    placeBtn(g_state->btnStartAll, 92);
    placeBtn(g_state->btnStopAll, 92);

    int log_top = btn_y + action_h + 4;
    int actual_log_h = height - log_top - 8;
    if (actual_log_h < 90) actual_log_h = 90;
    MoveWindow(g_state->grpLogs, pad, log_top, content_w, actual_log_h, TRUE);
    MoveWindow(g_state->btnClearLog, pad + content_w - 92, log_top + 14, 80, 22, TRUE);
    MoveWindow(g_state->edtLogs, pad + 8, log_top + 40, content_w - 16, actual_log_h - 48, TRUE);
}

void CreateAllControls(HWND hwnd) {
    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));

    g_state->hFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    g_state->hFontBold = CreateFontW(-14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                     CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    g_state->hFontMono = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                     CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");

    auto makeLabel = [&](const wchar_t* text, int x, int y, int w, int h) -> HWND {
        HWND hCtrl = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE, x, y, w, h, hwnd, nullptr, hInst, nullptr);
        SetFontRecursive(hCtrl, g_state->hFont);
        return hCtrl;
    };

    auto makeEdit = [&](int id, const wchar_t* text, int x, int y, int w, int h, DWORD extra_style = 0) -> HWND {
        HWND hCtrl = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text,
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | extra_style,
                                     x, y, w, h, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst, nullptr);
        SetFontRecursive(hCtrl, g_state->hFont);
        return hCtrl;
    };

    auto makeBtn = [&](int id, const wchar_t* text, int x, int y, int w, int h, bool bold = false) -> HWND {
        HWND hCtrl = CreateWindowExW(0, L"BUTTON", text,
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                     x, y, w, h, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst, nullptr);
        SetFontRecursive(hCtrl, bold ? g_state->hFontBold : g_state->hFont);
        return hCtrl;
    };

    g_state->grpGlobal = CreateWindowExW(0, L"BUTTON", L"① 全局傻瓜设置 (所有端口自动共享默认密钥与极速协议，无需逐个重复配置)",
                                         WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                         10, 6, 980, 56, hwnd, nullptr, hInst, nullptr);
    SetFontRecursive(g_state->grpGlobal, g_state->hFontBold);

    makeLabel(L"核心路径:", 20, 30, 62, 22);
    g_state->edtExePath = makeEdit(IDC_EDT_P2PTUN_PATH, L"p2ptun.exe", 84, 27, 115, 24);
    g_state->btnBrowseExe = makeBtn(IDC_BTN_BROWSE_EXE, L"浏览...", 202, 26, 56, 26);

    makeLabel(L"默认密钥:", 268, 30, 62, 22);
    g_state->edtDefSecret = makeEdit(IDC_EDT_DEF_SECRET, L"", 332, 27, 115, 24);

    makeLabel(L"默认协议:", 456, 30, 62, 22);
    g_state->cmbDefProto = CreateWindowExW(0, L"COMBOBOX", L"",
                                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                                           520, 26, 100, 120, hwnd, reinterpret_cast<HMENU>(IDC_CMB_DEF_PROTO), hInst, nullptr);
    SetFontRecursive(g_state->cmbDefProto, g_state->hFont);
    SendMessageW(g_state->cmbDefProto, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"hy2 (极速)"));
    SendMessageW(g_state->cmbDefProto, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"quic"));
    SendMessageW(g_state->cmbDefProto, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"classic"));
    SendMessageW(g_state->cmbDefProto, CB_SETCURSEL, 0, 0);

    makeLabel(L"带宽(M):", 628, 30, 54, 22);
    g_state->edtDefBw = makeEdit(IDC_EDT_DEF_BW, L"100", 682, 27, 46, 24, ES_NUMBER);

    g_state->chkMinTray = CreateWindowExW(0, L"BUTTON", L"关闭最小化到托盘",
                                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                          736, 28, 128, 22, hwnd, reinterpret_cast<HMENU>(IDC_CHK_MIN_TRAY), hInst, nullptr);
    SetFontRecursive(g_state->chkMinTray, g_state->hFont);

    g_state->btnSaveGlobal = makeBtn(IDC_BTN_SAVE_GLOBAL, L"💾 保存设置", 868, 25, 86, 27);
    g_state->btnOpenCfg = makeBtn(IDC_BTN_OPEN_CONFIG, L"📂 配置文件", 958, 25, 86, 27);

    g_state->grpAdd = CreateWindowExW(0, L"BUTTON", L"② 一键添加 / 修改端口隧道 (支持多端口 Expose 暴露与多端口 Connect 连接，端口直接填数字即可)",
                                      WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                      10, 68, 980, 122, hwnd, nullptr, hInst, nullptr);
    SetFontRecursive(g_state->grpAdd, g_state->hFontBold);

    makeLabel(L"模式方向:", 20, 94, 62, 22);
    g_state->cmbMode = CreateWindowExW(0, L"COMBOBOX", L"",
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                                       84, 90, 175, 120, hwnd, reinterpret_cast<HMENU>(IDC_CMB_MODE), hInst, nullptr);
    SetFontRecursive(g_state->cmbMode, g_state->hFont);
    SendMessageW(g_state->cmbMode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"📤 Expose (暴露本机端口)"));
    SendMessageW(g_state->cmbMode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"📥 Connect (连接远程端口)"));
    SendMessageW(g_state->cmbMode, CB_SETCURSEL, 0, 0);

    makeLabel(L"常用预设:", 270, 94, 62, 22);
    g_state->cmbPreset = CreateWindowExW(0, L"COMBOBOX", L"",
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                                         334, 90, 185, 220, hwnd, reinterpret_cast<HMENU>(IDC_CMB_PRESET), hInst, nullptr);
    SetFontRecursive(g_state->cmbPreset, g_state->hFont);
    for (const auto& p : p2ptun::GetScenePresets()) {
        std::wstring wl = ToW(p.label);
        SendMessageW(g_state->cmbPreset, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wl.c_str()));
    }
    SendMessageW(g_state->cmbPreset, CB_SETCURSEL, 0, 0);

    makeLabel(L"备注说明:", 530, 94, 62, 22);
    g_state->edtRemark = makeEdit(IDC_EDT_REMARK, L"本机远程桌面", 594, 91, 140, 24);

    g_state->btnPastePair = makeBtn(IDC_BTN_PASTE_PAIR, L"📥 粘贴配对码一键导入并连接", 746, 89, 298, 28, true);

    makeLabel(L"隧道名称:", 20, 126, 62, 22);
    g_state->edtName = makeEdit(IDC_EDT_NAME, L"home-rdp", 84, 123, 140, 24);

    makeLabel(L"本地端口:", 234, 126, 62, 22);
    g_state->edtPort = makeEdit(IDC_EDT_PORT, L"3389", 298, 123, 110, 24);

    makeLabel(L"独立密钥(可空):", 418, 126, 95, 22);
    g_state->edtSecret = makeEdit(IDC_EDT_SECRET, L"", 515, 123, 95, 24);

    makeLabel(L"协议:", 618, 126, 36, 22);
    g_state->cmbProto = CreateWindowExW(0, L"COMBOBOX", L"",
                                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                                        656, 122, 80, 120, hwnd, reinterpret_cast<HMENU>(IDC_CMB_PROTO), hInst, nullptr);
    SetFontRecursive(g_state->cmbProto, g_state->hFont);
    SendMessageW(g_state->cmbProto, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"hy2"));
    SendMessageW(g_state->cmbProto, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"quic"));
    SendMessageW(g_state->cmbProto, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"classic"));
    SendMessageW(g_state->cmbProto, CB_SETCURSEL, 0, 0);

    g_state->edtBw = makeEdit(IDC_EDT_BW, L"100", 742, 123, 42, 24, ES_NUMBER);
    g_state->btnAddStart = makeBtn(IDC_BTN_ADD_START, L"➕ 保存规则并立即启动", 794, 121, 250, 28, true);

    // 第三行：高级参数 (k=v 直接透传 p2ptun flags) + 串口桥接 (合并自 C# 管理器)
    makeLabel(L"高级参数:", 20, 158, 62, 22);
    g_state->edtAdvanced = makeEdit(IDC_EDT_ADVANCED, L"", 84, 155, 420, 24);
    SendMessageW(g_state->edtAdvanced, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(
        L"如: keepalive=15s ice_ports=40000-40100 nat1to1=1.2.3.4 broker=wss://0.peerjs.com"));
    g_state->btnAdvHelp = makeBtn(IDC_BTN_ADV_HELP, L"❓", 508, 154, 34, 26);

    makeLabel(L"串口桥接:", 556, 158, 58, 22);
    g_state->cmbSerialPort = CreateWindowExW(0, L"COMBOBOX", L"",
                                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWN,
                                             616, 154, 78, 200, hwnd, reinterpret_cast<HMENU>(IDC_CMB_SERIAL_PORT), hInst, nullptr);
    SetFontRecursive(g_state->cmbSerialPort, g_state->hFont);
    SendMessageW(g_state->cmbSerialPort, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"(不启用)"));
    for (const wchar_t* p : {L"COM1", L"COM2", L"COM3", L"COM4", L"COM5", L"COM6", L"COM7", L"COM8"}) {
        SendMessageW(g_state->cmbSerialPort, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(p));
    }
    SendMessageW(g_state->cmbSerialPort, CB_SETCURSEL, 0, 0);

    g_state->cmbSerialBaud = CreateWindowExW(0, L"COMBOBOX", L"",
                                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                                             700, 154, 82, 200, hwnd, reinterpret_cast<HMENU>(IDC_CMB_SERIAL_BAUD), hInst, nullptr);
    SetFontRecursive(g_state->cmbSerialBaud, g_state->hFont);
    for (const wchar_t* b : {L"9600", L"19200", L"38400", L"57600", L"115200", L"230400", L"460800", L"921600"}) {
        SendMessageW(g_state->cmbSerialBaud, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(b));
    }
    SendMessageW(g_state->cmbSerialBaud, CB_SETCURSEL, 4, 0); // 115200

    g_state->cmbSerialParity = CreateWindowExW(0, L"COMBOBOX", L"",
                                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                                               790, 154, 70, 200, hwnd, reinterpret_cast<HMENU>(IDC_CMB_SERIAL_PARITY), hInst, nullptr);
    SetFontRecursive(g_state->cmbSerialParity, g_state->hFont);
    for (const wchar_t* s : {L"8N1", L"7E2", L"8O1"}) {
        SendMessageW(g_state->cmbSerialParity, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
    }
    SendMessageW(g_state->cmbSerialParity, CB_SETCURSEL, 0, 0);

    g_state->listTunnels = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                           10, 166, 980, 260, hwnd, reinterpret_cast<HMENU>(IDC_LIST_TUNNELS), hInst, nullptr);
    SetFontRecursive(g_state->listTunnels, g_state->hFont);
    DWORD ex_style = LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER;
    SendMessageW(g_state->listTunnels, LVM_SETEXTENDEDLISTVIEWSTYLE, ex_style, ex_style);

    struct ColDef { const wchar_t* title; int width; };
    const ColDef cols[] = {
        {L"#", 36},
        {L"运行状态", 125},
        {L"模式 (Expose/Connect)", 148},
        {L"备注说明", 135},
        {L"隧道名称 (Name)", 125},
        {L"本地端口地址", 120},
        {L"协议/带宽", 88},
        {L"实时速率 / 累计流量", 160},
        {L"延迟 / 直连地址 / 状态详情", 180}
    };
    for (int i = 0; i < static_cast<int>(sizeof(cols) / sizeof(cols[0])); ++i) {
        LVCOLUMNW lvc{};
        lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        lvc.cx = cols[i].width;
        lvc.pszText = const_cast<LPWSTR>(cols[i].title);
        lvc.iSubItem = i;
        SendMessageW(g_state->listTunnels, LVM_INSERTCOLUMNW, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(&lvc));
    }

    g_state->btnStartOne  = makeBtn(IDC_BTN_START_ONE,  L"▶ 启动选中", 10, 432, 92, 30, true);
    g_state->btnStopOne   = makeBtn(IDC_BTN_STOP_ONE,   L"⏹ 停止选中", 108, 432, 92, 30);
    g_state->btnReconnOne = makeBtn(IDC_BTN_RECONN_ONE, L"🔄 重连选中", 206, 432, 92, 30);
    g_state->btnCopyPair  = makeBtn(IDC_BTN_COPY_PAIR,  L"📋 复制傻瓜配对码", 304, 432, 146, 30, true);
    g_state->btnCopyCli   = makeBtn(IDC_BTN_COPY_CLI,   L"💻 复制命令行", 456, 432, 115, 30);
    g_state->btnLaunchApp = makeBtn(IDC_BTN_LAUNCH_APP, L"🚀 打开客户端(RDP/Web)", 577, 432, 155, 30);
    g_state->btnDeleteOne = makeBtn(IDC_BTN_DELETE_ONE, L"🗑 删除选中", 738, 432, 90, 30);
    g_state->btnStartAll  = makeBtn(IDC_BTN_START_ALL,  L"▶ 全部启动", 834, 432, 92, 30);
    g_state->btnStopAll   = makeBtn(IDC_BTN_STOP_ALL,   L"⏹ 全部停止", 932, 432, 92, 30);

    g_state->grpLogs = CreateWindowExW(0, L"BUTTON", L"③ 多隧道实时运行日志 (双击上方表格任意行可快速切换启停)",
                                       WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                       10, 470, 980, 160, hwnd, nullptr, hInst, nullptr);
    SetFontRecursive(g_state->grpLogs, g_state->hFontBold);

    g_state->btnClearLog = makeBtn(IDC_BTN_CLEAR_LOG, L"清空日志", 900, 484, 80, 22);
    g_state->edtLogs = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                       WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
                                       18, 510, 964, 112, hwnd, reinterpret_cast<HMENU>(IDC_EDT_LOGS), hInst, nullptr);
    SetFontRecursive(g_state->edtLogs, g_state->hFontMono);
}

void HandleCommand(HWND hwnd, int id, int notify_code) {
    if (!g_state || !g_state->mgr) return;

    if ((id == IDC_CMB_PRESET || id == IDC_CMB_MODE) && notify_code == CBN_SELCHANGE) {
        if (id == IDC_CMB_PRESET) {
            ApplyPresetSelection();
        } else {
            int p_idx = static_cast<int>(SendMessageW(g_state->cmbPreset, CB_GETCURSEL, 0, 0));
            if (p_idx > 0) ApplyPresetSelection();
        }
        return;
    }

    switch (id) {
        case IDC_BTN_BROWSE_EXE: {
            wchar_t file_buf[MAX_PATH]{};
            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = L"p2ptun 可执行文件 (*.exe)\0*.exe\0所有文件 (*.*)\0*.*\0";
            ofn.lpstrFile = file_buf;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (GetOpenFileNameW(&ofn)) {
                SetWindowTextW(g_state->edtExePath, file_buf);
            }
            break;
        }

        case IDC_BTN_SAVE_GLOBAL: {
            std::string exe_path = GetCtrlTextU8(g_state->edtExePath);
            std::string def_sec = GetCtrlTextU8(g_state->edtDefSecret);
            int p_idx = static_cast<int>(SendMessageW(g_state->cmbDefProto, CB_GETCURSEL, 0, 0));
            std::string def_proto = (p_idx == 1) ? "quic" : (p_idx == 2 ? "classic" : "hy2");
            int def_bw = std::atoi(GetCtrlTextU8(g_state->edtDefBw).c_str());
            bool min_tray = (SendMessageW(g_state->chkMinTray, BM_GETCHECK, 0, 0) == BST_CHECKED);

            std::string err;
            if (!g_state->mgr->UpdateGlobalSettings(exe_path, def_sec, def_proto, def_bw, min_tray, err)) {
                MessageBoxW(hwnd, ToW(err).c_str(), L"保存失败", MB_ICONERROR);
            } else {
                RefreshTunnelListView();
                MessageBoxW(hwnd, L"全局设置已保存到 config.json！", L"保存成功", MB_ICONINFORMATION);
            }
            break;
        }

        case IDC_BTN_OPEN_CONFIG: {
            std::wstring cfg_w = ToW(g_state->mgr->ConfigPath());
            ShellExecuteW(hwnd, L"open", L"notepad.exe", cfg_w.c_str(), nullptr, SW_SHOWNORMAL);
            break;
        }

        case IDC_BTN_ADD_START: {
            p2ptun::TunnelRule r;
            int mode_idx = static_cast<int>(SendMessageW(g_state->cmbMode, CB_GETCURSEL, 0, 0));
            r.mode = (mode_idx == 1) ? "connect" : "expose";
            r.remark = GetCtrlTextU8(g_state->edtRemark);
            r.name = GetCtrlTextU8(g_state->edtName);
            r.port = GetCtrlTextU8(g_state->edtPort);
            r.secret = GetCtrlTextU8(g_state->edtSecret);
            int pr_idx = static_cast<int>(SendMessageW(g_state->cmbProto, CB_GETCURSEL, 0, 0));
            r.proto = (pr_idx == 1) ? "quic" : (pr_idx == 2 ? "classic" : "hy2");
            r.bandwidth_mbps = std::atoi(GetCtrlTextU8(g_state->edtBw).c_str());
            r.enabled = true;

            // 高级参数解析: "k=v k=v" 形式，支持全 flag 名称 (broker/stun/turn/proxy/key/keepalive/
            // ice_ports/nat1to1/idle_timeout/max_peers/force_relay/keep_proxy)
            {
                std::string adv_raw = GetCtrlTextU8(g_state->edtAdvanced);
                std::istringstream adv_iss(adv_raw);
                std::string tok;
                while (adv_iss >> tok) {
                    auto eq = tok.find('=');
                    if (eq == std::string::npos) continue;
                    std::string key = p2ptun::ToLower(tok.substr(0, eq));
                    std::string val = tok.substr(eq + 1);
                    if (key == "broker") r.broker = val;
                    else if (key == "stun") r.stun = val;
                    else if (key == "turn") r.turn = val;
                    else if (key == "proxy") r.proxy = val;
                    else if (key == "key" || key == "peer_key") r.peer_key = val;
                    else if (key == "keepalive") r.keepalive = val;
                    else if (key == "ice_ports" || key == "ice-ports") r.ice_ports = val;
                    else if (key == "nat1to1" || key == "nat1t") r.nat1to1 = val;
                    else if (key == "idle_timeout" || key == "idle-timeout") r.idle_timeout = val;
                    else if (key == "max_peers" || key == "max-peers") r.max_peers = (std::max)(0, std::atoi(val.c_str()));
                    else if (key == "force_relay" || key == "force-relay") r.force_relay = (val == "1" || p2ptun::ToLower(val) == "true");
                    else if (key == "keep_proxy" || key == "keep-proxy") r.keep_proxy = (val == "1" || p2ptun::ToLower(val) == "true");
                }
            }

            // 串口桥接参数
            {
                wchar_t serial_buf[64]{};
                SendMessageW(g_state->cmbSerialPort, WM_GETTEXT, 64, reinterpret_cast<LPARAM>(serial_buf));
                std::string serial_sel = ToU8(serial_buf);
                if (serial_sel != "(不启用)" && !p2ptun::Trim(serial_sel).empty()) {
                    r.serial_port = p2ptun::Trim(serial_sel);
                    r.serial_baud = std::atoi(GetCtrlTextU8(g_state->cmbSerialBaud).c_str());
                    if (r.serial_baud <= 0) r.serial_baud = 115200;
                    wchar_t par_buf[16]{};
                    SendMessageW(g_state->cmbSerialParity, WM_GETTEXT, 16, reinterpret_cast<LPARAM>(par_buf));
                    r.serial_params = ToU8(par_buf);
                    // 非法参数格式由 NormalizeRule 统一回退为 8N1
                }
            }

            if (r.name.empty()) {
                MessageBoxW(hwnd, L"请填写【隧道名称】（Expose 与 Connect 两端填相同的名称即可配对）！", L"提示", MB_ICONWARNING);
                SetFocus(g_state->edtName);
                return;
            }
            if (r.port.empty()) {
                MessageBoxW(hwnd, L"请填写【本地端口】（直接填数字如 3389 或 1080 即可）！", L"提示", MB_ICONWARNING);
                SetFocus(g_state->edtPort);
                return;
            }

            if (!g_state->editing_rule_id.empty()) {
                for (const auto& existing : g_state->cached_rows) {
                    if (existing.id == g_state->editing_rule_id && existing.name == r.name && existing.mode == r.mode) {
                        r.id = existing.id;
                        break;
                    }
                }
            }

            std::string out_id, err;
            bool ok = g_state->mgr->AddOrUpdateRule(r, true, out_id, err);
            g_state->editing_rule_id = out_id;
            RefreshTunnelListView();
            if (!ok && !err.empty()) {
                MessageBoxW(hwnd, ToW(err).c_str(), L"启动提示", MB_ICONWARNING);
            }
            break;
        }

        case IDC_BTN_PASTE_PAIR: {
            std::string clip = ReadTextFromClipboard(hwnd);
            std::string out_id, err;
            if (!g_state->mgr->ImportFromPairingCode(clip, out_id, err)) {
                MessageBoxW(hwnd,
                            ToW("导入配对码失败: " + err + "\n\n请先在对端点击【📋 复制傻瓜配对码】，再在此处点击粘贴导入！").c_str(),
                            L"粘贴配对码提示", MB_ICONWARNING);
            } else {
                RefreshTunnelListView();
                MessageBoxW(hwnd, L"已成功从剪贴板导入配对隧道并自动启动！", L"一键配对成功", MB_ICONINFORMATION);
            }
            break;
        }

        case IDC_BTN_START_ONE: {
            std::string tid = GetSelectedTunnelId();
            if (tid.empty()) {
                MessageBoxW(hwnd, L"请先在表格中选中一条隧道！", L"提示", MB_ICONINFORMATION);
                return;
            }
            std::string err;
            if (!g_state->mgr->StartTunnel(tid, err) && !err.empty()) {
                MessageBoxW(hwnd, ToW(err).c_str(), L"启动失败", MB_ICONWARNING);
            }
            RefreshTunnelListView();
            break;
        }

        case IDC_BTN_STOP_ONE: {
            std::string tid = GetSelectedTunnelId();
            if (tid.empty()) {
                MessageBoxW(hwnd, L"请先在表格中选中一条隧道！", L"提示", MB_ICONINFORMATION);
                return;
            }
            std::string err;
            g_state->mgr->StopTunnel(tid, err);
            RefreshTunnelListView();
            break;
        }

        case IDC_BTN_RECONN_ONE: {
            std::string tid = GetSelectedTunnelId();
            if (tid.empty()) {
                MessageBoxW(hwnd, L"请先在表格中选中一条隧道！", L"提示", MB_ICONINFORMATION);
                return;
            }
            std::string err;
            g_state->mgr->ReconnectTunnel(tid, err);
            RefreshTunnelListView();
            break;
        }

        case IDC_BTN_COPY_PAIR: {
            int idx = GetSelectedRowIndex();
            if (idx < 0 || idx >= static_cast<int>(g_state->cached_rows.size())) {
                MessageBoxW(hwnd, L"请先在表格中选中一条要分享的隧道！", L"提示", MB_ICONINFORMATION);
                return;
            }
            const auto& row = g_state->cached_rows[static_cast<size_t>(idx)];
            CopyTextToClipboard(hwnd, row.pairing_uri);
            std::string msg = "已复制傻瓜配对码到剪贴板：\n\n" + row.pairing_uri +
                              "\n\n发给另一台电脑后，对方只需点击【📥 粘贴配对码一键导入并连接】即可自动填好所有参数并连通！";
            MessageBoxW(hwnd, ToW(msg).c_str(), L"配对码已复制", MB_ICONINFORMATION);
            break;
        }

        case IDC_BTN_COPY_CLI: {
            int idx = GetSelectedRowIndex();
            if (idx < 0 || idx >= static_cast<int>(g_state->cached_rows.size())) {
                MessageBoxW(hwnd, L"请先在表格中选中一条隧道！", L"提示", MB_ICONINFORMATION);
                return;
            }
            const auto& row = g_state->cached_rows[static_cast<size_t>(idx)];
            CopyTextToClipboard(hwnd, row.cli_command);
            std::string msg = "已复制命令行到剪贴板：\n\n" + row.cli_command;
            MessageBoxW(hwnd, ToW(msg).c_str(), L"命令行已复制", MB_ICONINFORMATION);
            break;
        }

        case IDC_BTN_LAUNCH_APP: {
            std::string tid = GetSelectedTunnelId();
            if (tid.empty()) {
                MessageBoxW(hwnd, L"请先在表格中选中一条隧道！", L"提示", MB_ICONINFORMATION);
                return;
            }
            std::string err;
            g_state->mgr->LaunchClientForTunnel(tid, err);
            break;
        }

        case IDC_BTN_DELETE_ONE: {
            std::string tid = GetSelectedTunnelId();
            if (tid.empty()) {
                MessageBoxW(hwnd, L"请先在表格中选中一条要删除的隧道！", L"提示", MB_ICONINFORMATION);
                return;
            }
            if (MessageBoxW(hwnd, L"确定要停止并删除选中的隧道规则吗？", L"确认删除", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                std::string err;
                g_state->mgr->DeleteRule(tid, err);
                g_state->editing_rule_id.clear();
                RefreshTunnelListView();
            }
            break;
        }

        case IDC_BTN_START_ALL:
        case ID_TRAY_START_ALL: {
            g_state->mgr->StartAll();
            RefreshTunnelListView();
            break;
        }

        case IDC_BTN_STOP_ALL:
        case ID_TRAY_STOP_ALL: {
            g_state->mgr->StopAll(true);
            RefreshTunnelListView();
            break;
        }

        case IDC_BTN_ADV_HELP: {
            MessageBoxW(hwnd,
                ToW("高级参数以 key=value 形式填写，空格分隔，直接透传给 p2ptun.exe (留空即使用 p2ptun 内置默认)：\n\n"
                    "  broker=wss://0.peerjs.com     信令服务器列表(逗号分隔)\n"
                    "  stun=stun:stun.example.com    STUN 列表\n"
                    "  turn=turn:user:pass@host:3478  TURN 中继\n"
                    "  proxy=http://127.0.0.1:7890    信令代理\n"
                    "  key=peerjs                     PeerJS API Key\n"
                    "  keepalive=15s                  隧道心跳间隔\n"
                    "  ice_ports=40000-40100          ICE UDP 端口范围\n"
                    "  nat1to1=203.0.113.10           1:1 NAT 外部 IP\n"
                    "  idle_timeout=30m               按需模式空闲休眠(需勾选按需)\n"
                    "  max_peers=32                   Expose 最大并发客户端\n"
                    "  force_relay=1                  强制 TURN 中继\n"
                    "  keep_proxy=1                   保留系统环境代理\n\n"
                    "串口桥接：选择 COM 口后保存并启动，管理器自动在本地端口与串口之间做双向透传\n"
                    "(Expose 模式暴露串口给远端；Connect 模式把远端串口映射到本地串口)。").c_str(),
                L"高级参数与串口桥接说明", MB_ICONINFORMATION);
            break;
        }

        case IDC_BTN_CLEAR_LOG: {
            g_state->mgr->Logs().Clear();
            RefreshTunnelListView();
            break;
        }

        case ID_TRAY_OPEN: {
            ShowWindow(hwnd, SW_SHOW);
            ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
            break;
        }

        case ID_TRAY_OPEN_CFG: {
            std::wstring cfg_w = ToW(g_state->mgr->ConfigPath());
            ShellExecuteW(hwnd, L"open", L"notepad.exe", cfg_w.c_str(), nullptr, SW_SHOWNORMAL);
            break;
        }

        case ID_TRAY_EXIT: {
            g_state->quitting.store(true);
            DestroyWindow(hwnd);
            break;
        }
    }
}

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            CreateAllControls(hwnd);
            LoadGlobalControlsFromConfig();
            AddOrUpdateTrayIcon(hwnd);
            RefreshTunnelListView();
            SetTimer(hwnd, TIMER_UI, 1000, nullptr);
            return 0;
        }

        case WM_SIZE: {
            if (wParam != SIZE_MINIMIZED) {
                LayoutControls(LOWORD(lParam), HIWORD(lParam));
            }
            return 0;
        }

        case WM_GETMINMAXINFO: {
            MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = 1040;
            mmi->ptMinTrackSize.y = 660;
            return 0;
        }

        case WM_COMMAND: {
            HandleCommand(hwnd, LOWORD(wParam), HIWORD(wParam));
            return 0;
        }

        case WM_NOTIFY: {
            NMHDR* hdr = reinterpret_cast<NMHDR*>(lParam);
            if (hdr && hdr->idFrom == IDC_LIST_TUNNELS) {
                if (hdr->code == LVN_ITEMCHANGED) {
                    NMLISTVIEW* nmlv = reinterpret_cast<NMLISTVIEW*>(lParam);
                    if ((nmlv->uChanged & LVIF_STATE) && (nmlv->uNewState & LVIS_SELECTED)) {
                        PopulateQuickBarFromRow(nmlv->iItem);
                    }
                } else if (hdr->code == NM_DBLCLK) {
                    NMITEMACTIVATE* nmia = reinterpret_cast<NMITEMACTIVATE*>(lParam);
                    if (nmia->iItem >= 0 && nmia->iItem < static_cast<int>(g_state->cached_rows.size())) {
                        std::string tid = g_state->cached_rows[static_cast<size_t>(nmia->iItem)].id;
                        std::string err;
                        g_state->mgr->ToggleTunnel(tid, err);
                        RefreshTunnelListView();
                    }
                }
            }
            return 0;
        }

        case WM_TIMER: {
            if (wParam == TIMER_UI) {
                RefreshTunnelListView();
            }
            return 0;
        }

        case WM_APP_REFRESH: {
            RefreshTunnelListView();
            return 0;
        }

        case WM_APP_TRAY: {
            if (lParam == WM_LBUTTONDBLCLK) {
                ShowWindow(hwnd, SW_SHOW);
                ShowWindow(hwnd, SW_RESTORE);
                SetForegroundWindow(hwnd);
            } else if (lParam == WM_RBUTTONUP) {
                ShowTrayContextMenu(hwnd);
            }
            return 0;
        }

        case WM_CLOSE: {
            bool min_tray = (SendMessageW(g_state->chkMinTray, BM_GETCHECK, 0, 0) == BST_CHECKED);
            if (min_tray && !g_state->quitting.load()) {
                ShowWindow(hwnd, SW_HIDE);
                if (!g_state->tray_balloon_shown) {
                    g_state->tray_balloon_shown = true;
                    ShowTrayBalloon(hwnd, L"p2ptun 仍在后台运行",
                                    L"所有已启动的穿透隧道继续保持在线。双击右下角托盘图标可重新打开主窗口。");
                }
                return 0;
            }
            DestroyWindow(hwnd);
            return 0;
        }

        case WM_DESTROY: {
            g_state->quitting.store(true);
            KillTimer(hwnd, TIMER_UI);
            RemoveTrayIcon(hwnd);
            PostQuitMessage(0);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int RunGuiMain(HINSTANCE hInstance, int nCmdShow) {
    // 单实例守护 (合并自 C# 管理器): 已有实例运行时弹出提示并退出，
    // 避免两个 GUI 同时管理同一批 p2ptun.exe 子进程造成 JobObject 竞态。
    HANDLE single_mutex = CreateMutexW(nullptr, TRUE, L"Global\P2PTunManager_SingleInstance_Mutex");
    if (single_mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"p2ptun 管理器已在运行中 (请检查任务栏或系统托盘)。",
                    L"p2ptun Manager", MB_OK | MB_ICONINFORMATION);
        if (single_mutex) ReleaseMutex(single_mutex);
        return 0;
    }

    SetProcessDPIAware();

    INITCOMMONCONTROLSEX icex{};
    icex.dwSize = sizeof(icex);
    icex.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icex);

    GuiState state;
    g_state = &state;
    state.mgr = std::make_unique<p2ptun::MultiTunnelManager>(DefaultConfigFileNextToExe());
    state.mgr->StartAutoEnabled();

    const wchar_t* kClassName = L"P2PTunWinMultiManagerClass";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = kClassName;
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        0,
        kClassName,
        L"p2ptun Windows 多端口穿透管理器 (同时管理多个 Expose 暴露与多个 Connect 连接)",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1060, 700,
        nullptr, nullptr, hInstance, nullptr);

    if (!hwnd) {
        return 1;
    }
    state.hwnd = hwnd;

    state.poll_thread = std::thread([&state, hwnd]() {
        while (!state.quitting.load()) {
            state.mgr->PollAllStatuses();
            PostMessageW(hwnd, WM_APP_REFRESH, 0, 0);
            for (int i = 0; i < 10 && !state.quitting.load(); ++i) {
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
            DispatchMessage(&msg);
        }
    }

    state.quitting.store(true);
    if (state.poll_thread.joinable()) {
        state.poll_thread.join();
    }
    state.mgr->StopAll(false);

    if (state.hFont) DeleteObject(state.hFont);
    if (state.hFontBold) DeleteObject(state.hFontBold);
    if (state.hFontMono) DeleteObject(state.hFontMono);
    g_state = nullptr;
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
    std::cout << "p2ptun-win manager initialized (" << mgr.GetRowViews().size() << " rules loaded).\n";
    return 0;
}
#endif
