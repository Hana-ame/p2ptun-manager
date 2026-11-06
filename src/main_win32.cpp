// p2ptun-manager — C++17 Win32 GUI for managing multiple p2ptun expose/connect instances.
// High-DPI aware (PerMonitorV2): all layout metrics scale with the window's DPI.
// p2ptun.exe itself is never modified — only spawned with P2PTUN_SECRET env injection.

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
    // ① 全局设置区
    IDC_EDT_P2PTUN_PATH   = 1001,
    IDC_BTN_BROWSE_EXE    = 1002,
    IDC_EDT_DEF_SECRET    = 1003,
    IDC_CMB_DEF_PROTO     = 1004,
    IDC_EDT_DEF_BW        = 1005,
    IDC_CHK_MIN_TRAY      = 1006,
    IDC_BTN_SAVE_GLOBAL   = 1007,
    IDC_BTN_OPEN_CONFIG   = 1008,
    IDC_EDT_ADV_GLOBAL    = 1009,   // 全局高级参数默认值 (k=v 形式)
    IDC_BTN_ADV_HELP      = 1010,
    IDC_BTN_EXPORT_ALL    = 1011,   // 批量导出全部配置

    // ② 规则编辑区 (在 Server/Client Tab 页内)
    IDC_TAB_MODE          = 1020,
    IDC_CMB_PRESET        = 1021,
    IDC_EDT_REMARK        = 1022,
    IDC_EDT_NAME          = 1023,
    IDC_EDT_PORT          = 1024,
    IDC_CMB_PROTO         = 1026,
    IDC_EDT_BW             = 1027,
    IDC_BTN_ADD_START     = 1028,
    IDC_BTN_PASTE_PAIR    = 1029,
    IDC_EDT_ADV_RULE      = 1030,   // 单规则高级参数覆盖 (留空=用全局默认)
    IDC_BTN_MODE_TOGGLE   = 1035,   // 简易/高级模式切换
    IDC_CHK_ON_DEMAND     = 1036,   // Client 按需连接 (高级模式)

    // 列表区
    IDC_LIST_TUNNELS      = 1040,

    // 操作按钮
    IDC_BTN_START_ONE     = 1050,
    IDC_BTN_STOP_ONE      = 1051,
    IDC_BTN_RECONN_ONE    = 1052,
    IDC_BTN_COPY_PAIR     = 1053,
    IDC_BTN_COPY_CLI      = 1054,
    IDC_BTN_LAUNCH_APP    = 1055,
    IDC_BTN_DELETE_ONE    = 1056,
    IDC_BTN_START_ALL     = 1057,
    IDC_BTN_STOP_ALL      = 1058,

    // 日志区
    IDC_EDT_LOGS          = 1070,
    IDC_BTN_CLEAR_LOG     = 1071,

    // 行内启停按钮 (动态创建，每行一个)
    IDC_ROW_BTN_BASE      = 5000,   // +rowIndex

    ID_TRAY_OPEN          = 2001,
    ID_TRAY_START_ALL     = 2002,
    ID_TRAY_STOP_ALL      = 2003,
    ID_TRAY_OPEN_CFG      = 2004,
    ID_TRAY_EXIT          = 2005
};

constexpr UINT WM_APP_TRAY    = WM_APP + 10;
constexpr UINT WM_APP_REFRESH = WM_APP + 11;
constexpr UINT_PTR TIMER_UI   = 1;

// ---------------------------------------------------------------------------
// DPI 缩放：所有布局常量按 96-DPI 基准定义，运行时乘以 dpi_scale。
// ---------------------------------------------------------------------------
int g_dpi = 96;

int Scale(int v) {
    return MulDiv(v, g_dpi, 96);
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

    // ① 全局设置区
    HWND grpGlobal = nullptr;
    HWND edtExePath = nullptr;
    HWND btnBrowseExe = nullptr;
    HWND edtDefSecret = nullptr;
    HWND cmbDefProto = nullptr;
    HWND edtDefBw = nullptr;
    HWND chkMinTray = nullptr;
    HWND btnSaveGlobal = nullptr;
    HWND btnOpenCfg = nullptr;
    HWND edtAdvGlobal = nullptr;
    HWND btnAdvHelp = nullptr;
    HWND btnExportAll = nullptr;
    HWND btnModeToggle = nullptr;

    // ② 规则编辑区 (Tab 双页)
    HWND tabMode = nullptr;
    HWND cmbPreset = nullptr;
    HWND edtRemark = nullptr;
    HWND edtName = nullptr;
    HWND edtPort = nullptr;
    HWND cmbProto = nullptr;
    HWND edtBw = nullptr;
    HWND btnAddStart = nullptr;
    HWND btnPastePair = nullptr;
    HWND edtAdvRule = nullptr;
    HWND lblPort = nullptr;          // "暴露端口"/"本地监听" 标签随 Tab 切换

    // Tab 页内的静态标签 (随 Tab 显示/隐藏 + DPI 移动)
    HWND lblPreset = nullptr;
    HWND lblRemark = nullptr;
    HWND lblName = nullptr;
    HWND lblProtoBw = nullptr;
    HWND lblAdvRule = nullptr;
    HWND chkOnDemand = nullptr;      // Client 按需连接 (仅高级模式显示)

    // 列表 + 行内启停
    HWND listTunnels = nullptr;
    std::vector<HWND> row_buttons;   // 与列表行一一对应的启/停按钮
    std::vector<std::string> row_button_ids;
    bool suppress_row_btn_rebuild = false;

    // 操作按钮
    HWND btnStartOne = nullptr;
    HWND btnStopOne = nullptr;
    HWND btnReconnOne = nullptr;
    HWND btnCopyPair = nullptr;
    HWND btnCopyCli = nullptr;
    HWND btnLaunchApp = nullptr;
    HWND btnDeleteOne = nullptr;
    HWND btnStartAll = nullptr;
    HWND btnStopAll = nullptr;

    // 日志区
    HWND grpLogs = nullptr;
    HWND btnClearLog = nullptr;
    HWND edtLogs = nullptr;

    bool advanced_mode = false;      // 默认简易模式: 只显示 id + ip:port
    bool syncing_tab = false;        // 程序性切换 Tab (回填选中行) 时不反转端口

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

// 当前 Tab 页对应的模式: 0 = Server (expose), 1 = Client (connect)
std::string CurrentTabMode() {
    if (!g_state || !g_state->tabMode) return "expose";
    int sel = static_cast<int>(SendMessageW(g_state->tabMode, TCM_GETCURSEL, 0, 0));
    return sel == 1 ? "connect" : "expose";
}

// ---------------------------------------------------------------------------
// 高级参数 <-> "k=v k=v" 字符串转换
// ---------------------------------------------------------------------------
std::string AdvFieldsToString(const p2ptun::TunnelRule* rule, const p2ptun::AppConfig* global_cfg) {
    std::string adv;
    auto append_kv = [&adv](const char* k, const std::string& v) {
        std::string tv = p2ptun::Trim(v);
        if (!tv.empty()) adv += std::string(k) + "=" + tv + " ";
    };
    if (rule) {
        append_kv("broker", rule->broker);
        append_kv("stun", rule->stun);
        append_kv("turn", rule->turn);
        append_kv("proxy", rule->proxy);
        append_kv("key", rule->peer_key);
        append_kv("keepalive", rule->keepalive);
        append_kv("ice_ports", rule->ice_ports);
        append_kv("nat1to1", rule->nat1to1);
        append_kv("idle_timeout", rule->idle_timeout);
        if (rule->max_peers > 0) adv += "max_peers=" + std::to_string(rule->max_peers) + " ";
        if (rule->force_relay) adv += "force_relay=1 ";
        if (rule->keep_proxy) adv += "keep_proxy=1 ";
    } else if (global_cfg) {
        append_kv("broker", global_cfg->adv_broker);
        append_kv("stun", global_cfg->adv_stun);
        append_kv("turn", global_cfg->adv_turn);
        append_kv("proxy", global_cfg->adv_proxy);
        append_kv("key", global_cfg->adv_peer_key);
        append_kv("keepalive", global_cfg->adv_keepalive);
        append_kv("ice_ports", global_cfg->adv_ice_ports);
        append_kv("nat1to1", global_cfg->adv_nat1to1);
        append_kv("idle_timeout", global_cfg->adv_idle_timeout);
        if (global_cfg->adv_max_peers > 0) adv += "max_peers=" + std::to_string(global_cfg->adv_max_peers) + " ";
        if (global_cfg->adv_force_relay) adv += "force_relay=1 ";
        if (global_cfg->adv_keep_proxy) adv += "keep_proxy=1 ";
    }
    return p2ptun::Trim(adv);
}

// 解析 "k=v k=v" 到规则覆盖字段
void ApplyAdvStringToRule(const std::string& adv_raw, p2ptun::TunnelRule& r) {
    std::istringstream iss(p2ptun::Trim(adv_raw));
    std::string tok;
    while (iss >> tok) {
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
        else if (key == "nat1to1") r.nat1to1 = val;
        else if (key == "idle_timeout" || key == "idle-timeout") r.idle_timeout = val;
        else if (key == "max_peers" || key == "max-peers") r.max_peers = (std::max)(0, std::atoi(val.c_str()));
        else if (key == "force_relay" || key == "force-relay") r.force_relay = (val == "1" || p2ptun::ToLower(val) == "true");
        else if (key == "keep_proxy" || key == "keep-proxy") r.keep_proxy = (val == "1" || p2ptun::ToLower(val) == "true");
        else if (key == "on_demand" || key == "on-demand") r.on_demand = (val == "1" || p2ptun::ToLower(val) == "true");
    }
}

// 解析 "k=v k=v" 到全局默认字段
void ApplyAdvStringToGlobal(const std::string& adv_raw, p2ptun::AppConfig& cfg) {
    std::istringstream iss(p2ptun::Trim(adv_raw));
    std::string tok;
    while (iss >> tok) {
        auto eq = tok.find('=');
        if (eq == std::string::npos) continue;
        std::string key = p2ptun::ToLower(tok.substr(0, eq));
        std::string val = tok.substr(eq + 1);
        if (key == "broker") cfg.adv_broker = val;
        else if (key == "stun") cfg.adv_stun = val;
        else if (key == "turn") cfg.adv_turn = val;
        else if (key == "proxy") cfg.adv_proxy = val;
        else if (key == "key" || key == "peer_key") cfg.adv_peer_key = val;
        else if (key == "keepalive") cfg.adv_keepalive = val;
        else if (key == "ice_ports" || key == "ice-ports") cfg.adv_ice_ports = val;
        else if (key == "nat1to1") cfg.adv_nat1to1 = val;
        else if (key == "idle_timeout" || key == "idle-timeout") cfg.adv_idle_timeout = val;
        else if (key == "max_peers" || key == "max-peers") cfg.adv_max_peers = (std::max)(0, std::atoi(val.c_str()));
        else if (key == "force_relay" || key == "force-relay") cfg.adv_force_relay = (val == "1" || p2ptun::ToLower(val) == "true");
        else if (key == "keep_proxy" || key == "keep-proxy") cfg.adv_keep_proxy = (val == "1" || p2ptun::ToLower(val) == "true");
    }
}

void LoadGlobalControlsFromConfig() {
    if (!g_state || !g_state->mgr) return;
    p2ptun::AppConfig cfg = g_state->mgr->GetConfig();
    SetCtrlTextU8(g_state->edtExePath, cfg.p2ptun_path);
    SetCtrlTextU8(g_state->edtDefSecret, cfg.default_secret);
    SetCtrlTextU8(g_state->edtDefBw, std::to_string(cfg.default_bandwidth));
    SendMessageW(g_state->chkMinTray, BM_SETCHECK, cfg.minimize_to_tray ? BST_CHECKED : BST_UNCHECKED, 0);
    SetCtrlTextU8(g_state->edtAdvGlobal, AdvFieldsToString(nullptr, &cfg));

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

    bool is_connect = (CurrentTabMode() == "connect");
    const auto& preset = presets[static_cast<size_t>(p_idx)];

    g_state->editing_rule_id.clear();
    SetCtrlTextU8(g_state->edtRemark, is_connect ? preset.remark_connect : preset.remark_expose);
    SetCtrlTextU8(g_state->edtName, preset.default_name);
    SetCtrlTextU8(g_state->edtPort, is_connect ? preset.connect_port : preset.expose_port);
}

// 把选中行回填到编辑区 (按行的 mode 切换 Tab)
void PopulateQuickBarFromRow(int row_idx) {
    if (!g_state || row_idx < 0 || row_idx >= static_cast<int>(g_state->cached_rows.size())) return;
    const auto& row = g_state->cached_rows[static_cast<size_t>(row_idx)];
    g_state->editing_rule_id = row.id;

    // 行模式驱动 Tab 切换 (角色即"这条配置这次以哪个角色运行")
    int tab_idx = (row.mode == "connect") ? 1 : 0;
    g_state->syncing_tab = true;
    SendMessageW(g_state->tabMode, TCM_SETCURSEL, tab_idx, 0);
    UpdateTabDependentLabels();   // 仅更新标签，不反转端口
    g_state->syncing_tab = false;

    SendMessageW(g_state->cmbPreset, CB_SETCURSEL, 0, 0);
    SetCtrlTextU8(g_state->edtRemark, row.remark);
    SetCtrlTextU8(g_state->edtName, row.name);
    SetCtrlTextU8(g_state->edtPort, row.port_short);
    SetCtrlTextU8(g_state->edtBw, std::to_string(row.rule.bandwidth_mbps));
    SetCtrlTextU8(g_state->edtAdvRule, AdvFieldsToString(&row.rule, nullptr));

    int proto_idx = 0;
    if (row.rule.proto == "quic") proto_idx = 1;
    else if (row.rule.proto == "classic") proto_idx = 2;
    SendMessageW(g_state->cmbProto, CB_SETCURSEL, static_cast<WPARAM>(proto_idx), 0);
    SendMessageW(g_state->chkOnDemand, BM_SETCHECK, row.rule.on_demand ? BST_CHECKED : BST_UNCHECKED, 0);
}

// ---------------------------------------------------------------------------
// 刷新列表 + 行内启停按钮
// ---------------------------------------------------------------------------
void SyncRowButtons() {
    if (!g_state || !g_state->listTunnels) return;
    auto& rows = g_state->cached_rows;
    auto& btns = g_state->row_buttons;

    // 数量不匹配时重建
    if (btns.size() != rows.size()) {
        for (HWND b : btns) {
            if (b) DestroyWindow(b);
        }
        btns.clear();
        g_state->row_button_ids.clear();
        HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(g_state->hwnd, GWLP_HINSTANCE));
        for (size_t i = 0; i < rows.size(); ++i) {
            HWND b = CreateWindowExW(0, L"BUTTON", L"▶",
                                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                     0, 0, Scale(44), Scale(24), g_state->hwnd,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_ROW_BTN_BASE + static_cast<int>(i))),
                                     hInst, nullptr);
            SetFontRecursive(b, g_state->hFont);
            btns.push_back(b);
            g_state->row_button_ids.push_back(rows[i].id);
        }
    } else {
        // 数量一致时仅更新 id 映射与文本
        for (size_t i = 0; i < rows.size(); ++i) {
            g_state->row_button_ids[i] = rows[i].id;
            SetWindowTextW(btns[i], rows[i].running ? L"⏹" : L"▶");
        }
    }
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

    SyncRowButtons();

    // 行内按钮定位到每行第一列 (#序号) 下方区域 —— 放在状态列左侧
    for (size_t i = 0; i < g_state->row_buttons.size() && i < rows.size(); ++i) {
        RECT rc{};
        ListView_GetItemRect(hList, static_cast<int>(i), &rc, LVIR_BOUNDS);
        MapWindowPoints(hList, g_state->hwnd, reinterpret_cast<POINT*>(&rc), 2);
        // 按钮放在 # 列宽内、行高垂直居中
        int btn_w = Scale(44);
        int btn_h = Scale(24);
        int bx = rc.left + Scale(2);
        int by = rc.top + ((rc.bottom - rc.top) - btn_h) / 2;
        MoveWindow(g_state->row_buttons[i], bx, by, btn_w, btn_h, TRUE);
        SetWindowTextW(g_state->row_buttons[i], rows[i].running ? L"⏹" : L"▶");
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

// Tab 标签随当前页切换："暴露端口" / "本地监听"，并同步端口预设方向
void UpdateTabDependentLabels() {
    if (!g_state) return;
    static std::string last_mode = "expose";
    std::string cur_mode = CurrentTabMode();
    bool is_connect = (cur_mode == "connect");
    SetWindowTextW(g_state->lblPort, is_connect ? L"本地监听:" : L"暴露端口:");

    if (!g_state->syncing_tab && cur_mode != last_mode) {
        // 用户切换角色: 同一配置两端共用，端口建议自动反转 (3389 <-> 13389, 22 <-> 2222 ...)
        std::string cur_port = GetCtrlTextU8(g_state->edtPort);
        std::string p = p2ptun::ExtractPortOnly(cur_port);
        std::string flipped = is_connect
                                  ? p2ptun::SuggestConnectPort("127.0.0.1:" + p)
                                  : p2ptun::SuggestExposePort("127.0.0.1:" + p);
        if (!flipped.empty()) {
            SetCtrlTextU8(g_state->edtPort, flipped);
        }
        SendMessageW(g_state->cmbPreset, CB_SETCURSEL, 0, 0); // 预设清位避免错位
    }
    last_mode = cur_mode;
}

// ---------------------------------------------------------------------------
// 布局：所有数值均为 96-DPI 逻辑像素，经 Scale() 缩放
// ---------------------------------------------------------------------------
void LayoutControls(int width, int height) {
    if (!g_state || width <= 0 || height <= 0) return;

    int pad = Scale(10);
    int content_w = width - pad * 2;
    if (content_w < Scale(760)) content_w = Scale(760);

    int y = Scale(6);

    // ① 全局区
    int g1_h = Scale(86);   // 两行: 主设置行 + 高级默认行
    MoveWindow(g_state->grpGlobal, pad, y, content_w, g1_h, TRUE);
    y += g1_h + Scale(6);

    // ② Tab 编辑区 (简易模式一行矮些; 高级模式三行)
    int tab_h = g_state->advanced_mode ? Scale(120) : Scale(64);
    MoveWindow(g_state->tabMode, pad, y, content_w, tab_h, TRUE);
    y += tab_h + Scale(6);

    // 列表
    int action_h = Scale(34);
    int log_h = Scale(155);
    int list_top = y;
    int list_h = height - y - action_h - log_h - Scale(28);
    if (list_h < Scale(140)) list_h = Scale(140);
    MoveWindow(g_state->listTunnels, pad, list_top, content_w, list_h, TRUE);

    int btn_y = list_top + list_h + Scale(6);
    int bx = pad;
    auto placeBtn = [&](HWND hBtn, int w) {
        MoveWindow(hBtn, bx, btn_y, w, Scale(30), TRUE);
        bx += w + Scale(6);
    };
    placeBtn(g_state->btnStartOne, Scale(92));
    placeBtn(g_state->btnStopOne, Scale(92));
    placeBtn(g_state->btnReconnOne, Scale(92));
    placeBtn(g_state->btnCopyPair, Scale(146));
    placeBtn(g_state->btnCopyCli, Scale(115));
    placeBtn(g_state->btnLaunchApp, Scale(155));
    placeBtn(g_state->btnDeleteOne, Scale(90));
    placeBtn(g_state->btnStartAll, Scale(92));
    placeBtn(g_state->btnStopAll, Scale(92));

    int log_top = btn_y + action_h + Scale(4);
    int actual_log_h = height - log_top - Scale(8);
    if (actual_log_h < Scale(90)) actual_log_h = Scale(90);
    MoveWindow(g_state->grpLogs, pad, log_top, content_w, actual_log_h, TRUE);
    MoveWindow(g_state->btnClearLog, pad + content_w - Scale(92), log_top + Scale(14), Scale(80), Scale(22), TRUE);
    MoveWindow(g_state->edtLogs, pad + Scale(8), log_top + Scale(40), content_w - Scale(16), actual_log_h - Scale(48), TRUE);

    // 行内按钮随列表刷新 (列表行矩形依赖列表客户区)
    SyncRowButtons();
    for (size_t i = 0; i < g_state->row_buttons.size(); ++i) {
        RECT rc{};
        if (ListView_GetItemRect(g_state->listTunnels, static_cast<int>(i), &rc, LVIR_BOUNDS)) {
            MapWindowPoints(g_state->listTunnels, g_state->hwnd, reinterpret_cast<POINT*>(&rc), 2);
            int btn_w = Scale(44), btn_h = Scale(24);
            MoveWindow(g_state->row_buttons[i], rc.left + Scale(2),
                       rc.top + ((rc.bottom - rc.top) - btn_h) / 2, btn_w, btn_h, TRUE);
        }
    }
}

// Tab 显示区内的控件位置（相对主窗口客户区，位于 tab 控件内部）
// 简易模式: 一行 (隧道名称 + 端口 + 保存启动)
// 高级模式: 三行 (预设/备注/粘贴配对码；名称/端口/协议/带宽/保存；高级参数覆盖 + 按需连接)
void LayoutTabChildren() {
    if (!g_state || !g_state->tabMode) return;

    RECT tab_rc{};
    GetClientRect(g_state->tabMode, &tab_rc);
    SendMessageW(g_state->tabMode, TCM_ADJUSTRECT, FALSE, reinterpret_cast<LPARAM>(&tab_rc));
    MapWindowPoints(g_state->tabMode, g_state->hwnd, reinterpret_cast<POINT*>(&tab_rc), 2);

    int x0 = tab_rc.left + Scale(12);

    // 高级专属控件显隐
    BOOL adv = g_state->advanced_mode ? TRUE : FALSE;
    auto show_adv = [&](HWND h) { ShowWindow(h, adv ? SW_SHOW : SW_HIDE); };
    show_adv(g_state->lblPreset);   show_adv(g_state->cmbPreset);
    show_adv(g_state->lblRemark);   show_adv(g_state->edtRemark);
    show_adv(g_state->btnPastePair);
    show_adv(g_state->lblProtoBw); show_adv(g_state->cmbProto);
    show_adv(g_state->edtBw);
    show_adv(g_state->lblAdvRule);  show_adv(g_state->edtAdvRule);
    show_adv(g_state->chkOnDemand);

    if (!g_state->advanced_mode) {
        // ===== 简易模式: 唯一一行 =====
        int row_y = tab_rc.top + Scale(14);
        MoveWindow(g_state->lblName, x0, row_y + Scale(4), Scale(62), Scale(22), TRUE);
        MoveWindow(g_state->edtName, x0 + Scale(66), row_y, Scale(160), Scale(24), TRUE);
        MoveWindow(g_state->lblPort, x0 + Scale(250), row_y + Scale(4), Scale(62), Scale(22), TRUE);
        MoveWindow(g_state->edtPort, x0 + Scale(316), row_y, Scale(130), Scale(24), TRUE);
        MoveWindow(g_state->btnAddStart, x0 + Scale(470), row_y - Scale(2), Scale(220), Scale(28), TRUE);
    } else {
        // ===== 高级模式: 三行 =====
        int row1_y = tab_rc.top + Scale(8);
        int row2_y = row1_y + Scale(32);
        int row3_y = row2_y + Scale(32);

        MoveWindow(g_state->lblPreset, x0, row1_y + Scale(4), Scale(62), Scale(22), TRUE);
        MoveWindow(g_state->cmbPreset, x0 + Scale(66), row1_y, Scale(185), Scale(220), TRUE);
        MoveWindow(g_state->lblRemark, x0 + Scale(270), row1_y + Scale(4), Scale(62), Scale(22), TRUE);
        MoveWindow(g_state->edtRemark, x0 + Scale(334), row1_y, Scale(140), Scale(24), TRUE);
        MoveWindow(g_state->btnPastePair, x0 + Scale(500), row1_y - Scale(2), Scale(298), Scale(28), TRUE);

        // 行2: 名称 -> 端口
        MoveWindow(g_state->lblName, x0, row2_y + Scale(4), Scale(62), Scale(22), TRUE);
        MoveWindow(g_state->edtName, x0 + Scale(66), row2_y, Scale(140), Scale(24), TRUE);
        MoveWindow(g_state->lblPort, x0 + Scale(230), row2_y + Scale(4), Scale(62), Scale(22), TRUE);
        MoveWindow(g_state->edtPort, x0 + Scale(296), row2_y, Scale(110), Scale(24), TRUE);
        MoveWindow(g_state->lblProtoBw, x0 + Scale(430), row2_y + Scale(4), Scale(62), Scale(22), TRUE);
        MoveWindow(g_state->cmbProto, x0 + Scale(494), row2_y, Scale(80), Scale(120), TRUE);
        MoveWindow(g_state->edtBw, x0 + Scale(590), row2_y, Scale(42), Scale(24), TRUE);
        MoveWindow(g_state->btnAddStart, x0 + Scale(660), row2_y - Scale(2), Scale(250), Scale(28), TRUE);

        // 行3: 高级参数覆盖 + Client 按需
        MoveWindow(g_state->lblAdvRule, x0, row3_y + Scale(4), Scale(92), Scale(22), TRUE);
        MoveWindow(g_state->edtAdvRule, x0 + Scale(96), row3_y, Scale(430), Scale(24), TRUE);
        MoveWindow(g_state->chkOnDemand, x0 + Scale(560), row3_y + Scale(3), Scale(280), Scale(22), TRUE);
    }
}

// ---------------------------------------------------------------------------
// 控件创建
// ---------------------------------------------------------------------------
void CreateAllControls(HWND hwnd) {
    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));

    UpdateDpiFromWindow(hwnd);

    // 字体随 DPI 缩放：96DPI 基准 -14 → 200% 时 -28
    int font_h = -MulDiv(9, g_dpi, 72);   // 9pt 基准
    g_state->hFont = CreateFontW(font_h, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    g_state->hFontBold = CreateFontW(font_h, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                     CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    g_state->hFontMono = CreateFontW(-MulDiv(8, g_dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
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

    // ============ ① 全局设置区 (两行) ============
    g_state->grpGlobal = CreateWindowExW(0, L"BUTTON",
                                         L"① 全局设置 (密钥/协议/带宽全局共享，密钥只在这里配)",
                                         WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                         Scale(10), Scale(6), Scale(980), Scale(86), hwnd, nullptr, hInst, nullptr);
    SetFontRecursive(g_state->grpGlobal, g_state->hFontBold);

    // 行1: 核心路径 / 密钥 / 协议 / 带宽 / 托盘 / 保存 / 导出
    int r1y = Scale(30);
    makeLabel(L"核心路径:", Scale(20), r1y + Scale(4), Scale(62), Scale(22));
    g_state->edtExePath = makeEdit(IDC_EDT_P2PTUN_PATH, L"p2ptun.exe", Scale(84), r1y, Scale(115), Scale(24));
    g_state->btnBrowseExe = makeBtn(IDC_BTN_BROWSE_EXE, L"浏览...", Scale(202), r1y - Scale(1), Scale(56), Scale(26));

    makeLabel(L"默认密钥:", Scale(268), r1y + Scale(4), Scale(62), Scale(22));
    g_state->edtDefSecret = makeEdit(IDC_EDT_DEF_SECRET, L"", Scale(332), r1y, Scale(115), Scale(24));

    makeLabel(L"默认协议:", Scale(456), r1y + Scale(4), Scale(62), Scale(22));
    g_state->cmbDefProto = CreateWindowExW(0, L"COMBOBOX", L"",
                                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                                           Scale(520), r1y - Scale(1), Scale(100), Scale(120), hwnd,
                                           reinterpret_cast<HMENU>(IDC_CMB_DEF_PROTO), hInst, nullptr);
    SetFontRecursive(g_state->cmbDefProto, g_state->hFont);
    SendMessageW(g_state->cmbDefProto, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"hy2 (极速)"));
    SendMessageW(g_state->cmbDefProto, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"quic"));
    SendMessageW(g_state->cmbDefProto, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"classic"));
    SendMessageW(g_state->cmbDefProto, CB_SETCURSEL, 0, 0);

    makeLabel(L"带宽(M):", Scale(628), r1y + Scale(4), Scale(54), Scale(22));
    g_state->edtDefBw = makeEdit(IDC_EDT_DEF_BW, L"100", Scale(682), r1y, Scale(46), Scale(24), ES_NUMBER);

    g_state->chkMinTray = CreateWindowExW(0, L"BUTTON", L"关闭最小化到托盘",
                                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                          Scale(736), r1y + Scale(2), Scale(128), Scale(22), hwnd,
                                          reinterpret_cast<HMENU>(IDC_CHK_MIN_TRAY), hInst, nullptr);
    SetFontRecursive(g_state->chkMinTray, g_state->hFont);

    g_state->btnSaveGlobal = makeBtn(IDC_BTN_SAVE_GLOBAL, L"💾 保存设置", Scale(868), r1y - Scale(1), Scale(86), Scale(27));
    g_state->btnOpenCfg = makeBtn(IDC_BTN_OPEN_CONFIG, L"📂 配置文件", Scale(958), r1y - Scale(1), Scale(86), Scale(27));

    // 行2: 全局高级参数默认值 + 帮助 + 批量导出
    int r2y = Scale(62);
    makeLabel(L"高级默认:", Scale(20), r2y + Scale(4), Scale(62), Scale(22));
    g_state->edtAdvGlobal = makeEdit(IDC_EDT_ADV_GLOBAL, L"", Scale(84), r2y, Scale(560), Scale(24));
    SendMessageW(g_state->edtAdvGlobal, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(
        L"全局生效: broker=wss://0.peerjs.com keepalive=15s max_peers=32 ... (留空=p2ptun 内置默认)"));
    g_state->btnAdvHelp = makeBtn(IDC_BTN_ADV_HELP, L"❓", Scale(648), r2y - Scale(1), Scale(34), Scale(26));
    g_state->btnExportAll = makeBtn(IDC_BTN_EXPORT_ALL, L"📤 批量导出全部配置", Scale(686), r2y - Scale(1), Scale(180), Scale(27));
    g_state->btnModeToggle = makeBtn(IDC_BTN_MODE_TOGGLE, L"⚙ 高级模式", Scale(872), r2y - Scale(1), Scale(100), Scale(27));

    // ============ ② Server / Client 双 Tab 编辑区 ============
    g_state->tabMode = CreateWindowExW(0, WC_TABCONTROLW, L"",
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | TCS_TABS,
                                       Scale(10), Scale(98), Scale(980), Scale(120), hwnd,
                                       reinterpret_cast<HMENU>(IDC_TAB_MODE), hInst, nullptr);
    SetFontRecursive(g_state->tabMode, g_state->hFont);

    TCITEMW ti{};
    ti.mask = TCIF_TEXT;
    ti.pszText = const_cast<LPWSTR>(L"📤 Server (暴露本机端口)");
    SendMessageW(g_state->tabMode, TCM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&ti));
    ti.pszText = const_cast<LPWSTR>(L"📥 Client (连接远程端口)");
    SendMessageW(g_state->tabMode, TCM_INSERTITEMW, 1, reinterpret_cast<LPARAM>(&ti));
    SendMessageW(g_state->tabMode, TCM_SETCURSEL, 0, 0);

    // Tab 内控件 (父窗口仍为主窗口，位置由 LayoutTabChildren 摆放)
    g_state->lblPreset = makeLabel(L"常用预设:", 0, 0, Scale(62), Scale(22));
    g_state->cmbPreset = CreateWindowExW(0, L"COMBOBOX", L"",
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                                         0, 0, Scale(185), Scale(220), hwnd,
                                         reinterpret_cast<HMENU>(IDC_CMB_PRESET), hInst, nullptr);
    SetFontRecursive(g_state->cmbPreset, g_state->hFont);
    for (const auto& p : p2ptun::GetScenePresets()) {
        std::wstring wl = ToW(p.label);
        SendMessageW(g_state->cmbPreset, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wl.c_str()));
    }
    SendMessageW(g_state->cmbPreset, CB_SETCURSEL, 0, 0);

    g_state->lblRemark = makeLabel(L"备注说明:", 0, 0, Scale(62), Scale(22));
    g_state->edtRemark = makeEdit(IDC_EDT_REMARK, L"本机远程桌面", 0, 0, Scale(140), Scale(24));
    g_state->btnPastePair = makeBtn(IDC_BTN_PASTE_PAIR, L"📥 粘贴配对码一键导入并连接", 0, 0, Scale(298), Scale(28), true);

    g_state->lblName = makeLabel(L"隧道名称:", 0, 0, Scale(62), Scale(22));
    g_state->edtName = makeEdit(IDC_EDT_NAME, L"home-rdp", 0, 0, Scale(140), Scale(24));
    g_state->lblPort = makeLabel(L"暴露端口:", 0, 0, Scale(62), Scale(22));
    g_state->edtPort = makeEdit(IDC_EDT_PORT, L"3389", 0, 0, Scale(110), Scale(24));
    g_state->lblProtoBw = makeLabel(L"协议/带宽:", 0, 0, Scale(62), Scale(22));
    g_state->cmbProto = CreateWindowExW(0, L"COMBOBOX", L"",
                                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                                        0, 0, Scale(80), Scale(120), hwnd,
                                        reinterpret_cast<HMENU>(IDC_CMB_PROTO), hInst, nullptr);
    SetFontRecursive(g_state->cmbProto, g_state->hFont);
    SendMessageW(g_state->cmbProto, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"hy2"));
    SendMessageW(g_state->cmbProto, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"quic"));
    SendMessageW(g_state->cmbProto, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"classic"));
    SendMessageW(g_state->cmbProto, CB_SETCURSEL, 0, 0);
    g_state->edtBw = makeEdit(IDC_EDT_BW, L"100", 0, 0, Scale(42), Scale(24), ES_NUMBER);
    g_state->btnAddStart = makeBtn(IDC_BTN_ADD_START, L"➕ 保存规则并立即启动", 0, 0, Scale(250), Scale(28), true);

    g_state->lblAdvRule = makeLabel(L"高级参数覆盖:", 0, 0, Scale(92), Scale(22));
    g_state->edtAdvRule = makeEdit(IDC_EDT_ADV_RULE, L"", 0, 0, Scale(500), Scale(24));
    g_state->chkOnDemand = CreateWindowExW(0, L"BUTTON", L"Client 按需连接 (空闲自动休眠)",
                                            WS_CHILD | BS_AUTOCHECKBOX,
                                            0, 0, Scale(260), Scale(22), hwnd,
                                            reinterpret_cast<HMENU>(IDC_CHK_ON_DEMAND), hInst, nullptr);
    SetFontRecursive(g_state->chkOnDemand, g_state->hFont);
    SendMessageW(g_state->edtAdvRule, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(
        L"留空=用上方全局高级默认; 填了则只覆盖本条: keepalive=15s ice_ports=40000-40100 ..."));

    // ============ 列表 ============
    g_state->listTunnels = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                           Scale(10), Scale(224), Scale(980), Scale(260), hwnd,
                                           reinterpret_cast<HMENU>(IDC_LIST_TUNNELS), hInst, nullptr);
    SetFontRecursive(g_state->listTunnels, g_state->hFont);
    DWORD ex_style = LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER;
    SendMessageW(g_state->listTunnels, LVM_SETEXTENDEDLISTVIEWSTYLE, ex_style, ex_style);

    struct ColDef { const wchar_t* title; int width; };
    const ColDef cols[] = {
        {L"启停", Scale(52)},
        {L"#", Scale(36)},
        {L"运行状态", Scale(125)},
        {L"模式 (Server/Client)", Scale(148)},
        {L"备注说明", Scale(135)},
        {L"隧道名称 (Name)", Scale(125)},
        {L"端口地址", Scale(120)},
        {L"协议/带宽", Scale(88)},
        {L"实时速率 / 累计流量", Scale(160)},
        {L"延迟 / 直连地址 / 状态详情", Scale(180)}
    };
    for (int i = 0; i < static_cast<int>(sizeof(cols) / sizeof(cols[0])); ++i) {
        LVCOLUMNW lvc{};
        lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        lvc.cx = cols[i].width;
        lvc.pszText = const_cast<LPWSTR>(cols[i].title);
        lvc.iSubItem = i;
        SendMessageW(g_state->listTunnels, LVM_INSERTCOLUMNW, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(&lvc));
    }

    // ============ 操作按钮 ============
    g_state->btnStartOne  = makeBtn(IDC_BTN_START_ONE,  L"▶ 启动选中", Scale(10), Scale(490), Scale(92), Scale(30), true);
    g_state->btnStopOne   = makeBtn(IDC_BTN_STOP_ONE,   L"⏹ 停止选中", Scale(108), Scale(490), Scale(92), Scale(30));
    g_state->btnReconnOne = makeBtn(IDC_BTN_RECONN_ONE, L"🔄 重连选中", Scale(206), Scale(490), Scale(92), Scale(30));
    g_state->btnCopyPair  = makeBtn(IDC_BTN_COPY_PAIR,  L"📋 复制傻瓜配对码", Scale(304), Scale(490), Scale(146), Scale(30), true);
    g_state->btnCopyCli   = makeBtn(IDC_BTN_COPY_CLI,   L"💻 复制命令行", Scale(456), Scale(490), Scale(115), Scale(30));
    g_state->btnLaunchApp = makeBtn(IDC_BTN_LAUNCH_APP, L"🚀 打开客户端(RDP/Web)", Scale(577), Scale(490), Scale(155), Scale(30));
    g_state->btnDeleteOne = makeBtn(IDC_BTN_DELETE_ONE, L"🗑 删除选中", Scale(738), Scale(490), Scale(90), Scale(30));
    g_state->btnStartAll  = makeBtn(IDC_BTN_START_ALL,  L"▶ 全部启动", Scale(834), Scale(490), Scale(92), Scale(30));
    g_state->btnStopAll   = makeBtn(IDC_BTN_STOP_ALL,   L"⏹ 全部停止", Scale(932), Scale(490), Scale(92), Scale(30));

    // ============ 日志区 ============
    g_state->grpLogs = CreateWindowExW(0, L"BUTTON",
                                       L"③ 多隧道实时运行日志 (每行左侧 ▶/⏹ 按钮可独立启停；双击行也可切换)",
                                       WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                       Scale(10), Scale(528), Scale(980), Scale(160), hwnd, nullptr, hInst, nullptr);
    SetFontRecursive(g_state->grpLogs, g_state->hFontBold);

    g_state->btnClearLog = makeBtn(IDC_BTN_CLEAR_LOG, L"清空日志", Scale(900), Scale(542), Scale(80), Scale(22));
    g_state->edtLogs = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                       WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
                                       Scale(18), Scale(568), Scale(964), Scale(112), hwnd,
                                       reinterpret_cast<HMENU>(IDC_EDT_LOGS), hInst, nullptr);
    SetFontRecursive(g_state->edtLogs, g_state->hFontMono);

    LayoutTabChildren();
}

void HandleCommand(HWND hwnd, int id, int notify_code) {
    if (!g_state || !g_state->mgr) return;

    if ((id == IDC_CMB_PRESET) && notify_code == CBN_SELCHANGE) {
        ApplyPresetSelection();
        return;
    }

    // 行内启停按钮
    if (id >= IDC_ROW_BTN_BASE && id < IDC_ROW_BTN_BASE + 1000) {
        size_t row_idx = static_cast<size_t>(id - IDC_ROW_BTN_BASE);
        if (row_idx < g_state->row_button_ids.size()) {
            std::string tid = g_state->row_button_ids[row_idx];
            std::string err;
            g_state->mgr->ToggleTunnel(tid, err);
            if (!err.empty() && err.find("启动失败") != std::string::npos) {
                MessageBoxW(hwnd, ToW(err).c_str(), L"启停提示", MB_ICONWARNING);
            }
            RefreshTunnelListView();
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
                // 全局高级默认值一并保存
                p2ptun::AppConfig cfg = g_state->mgr->GetConfig();
                ApplyAdvStringToGlobal(GetCtrlTextU8(g_state->edtAdvGlobal), cfg);
                g_state->mgr->UpdateAdvancedDefaults(cfg);
                RefreshTunnelListView();
                MessageBoxW(hwnd, L"全局设置已保存到 config.json！", L"保存成功", MB_ICONINFORMATION);
            }
            break;
        }

        case IDC_BTN_EXPORT_ALL: {
            // 批量导出：完整 config.json (含全部规则 + 全局设置)
            wchar_t file_buf[MAX_PATH]{};
            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = L"JSON 配置 (*.json)\0*.json\0所有文件 (*.*)\0*.*\0";
            ofn.lpstrFile = file_buf;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_OVERWRITEPROMPT;
            wcscpy_s(file_buf, L"p2ptun-config-backup.json");
            if (GetSaveFileNameW(&ofn)) {
                std::string err;
                if (g_state->mgr->ExportConfig(ToU8(file_buf), err)) {
                    MessageBoxW(hwnd, ToW("全部配置已导出到:\n" + ToU8(file_buf)).c_str(),
                               L"批量导出成功", MB_ICONINFORMATION);
                } else {
                    MessageBoxW(hwnd, ToW(err).c_str(), L"导出失败", MB_ICONERROR);
                }
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
            r.mode = CurrentTabMode();   // Tab 页决定 Server/Client
            r.remark = GetCtrlTextU8(g_state->edtRemark);
            r.name = GetCtrlTextU8(g_state->edtName);
            r.port = GetCtrlTextU8(g_state->edtPort);
            int pr_idx = static_cast<int>(SendMessageW(g_state->cmbProto, CB_GETCURSEL, 0, 0));
            r.proto = (pr_idx == 1) ? "quic" : (pr_idx == 2 ? "classic" : "hy2");
            r.bandwidth_mbps = std::atoi(GetCtrlTextU8(g_state->edtBw).c_str());
            r.enabled = true;
            if (r.mode == "connect" && g_state->advanced_mode) {
                r.on_demand = (SendMessageW(g_state->chkOnDemand, BM_GETCHECK, 0, 0) == BST_CHECKED);
            }

            // 单规则高级参数覆盖 (留空字段在 BuildSubprocessArgs 里回退全局默认)
            ApplyAdvStringToRule(GetCtrlTextU8(g_state->edtAdvRule), r);

            if (r.name.empty()) {
                MessageBoxW(hwnd, L"请填写【隧道名称】(Server 与 Client 两端填相同名称即可配对)！", L"提示", MB_ICONWARNING);
                SetFocus(g_state->edtName);
                return;
            }
            if (r.port.empty()) {
                MessageBoxW(hwnd, L"请填写端口 (直接填数字如 3389 或 1080 即可)！", L"提示", MB_ICONWARNING);
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

        case IDC_BTN_MODE_TOGGLE: {
            g_state->advanced_mode = !g_state->advanced_mode;
            SetWindowTextW(g_state->btnModeToggle, g_state->advanced_mode ? L"✨ 简易模式" : L"⚙ 高级模式");
            RECT rc{};
            GetClientRect(hwnd, &rc);
            LayoutControls(rc.right, rc.bottom);
            LayoutTabChildren();
            break;
        }

        case IDC_BTN_ADV_HELP: {
            MessageBoxW(hwnd,
                ToW("高级参数以 key=value 形式填写，空格分隔，直接透传给 p2ptun.exe：\n\n"
                    "■ 全局默认框: 对所有规则生效 (规则未覆盖时回退到这里)\n"
                    "■ 规则覆盖框: 只对当前编辑的规则生效，覆盖同名的全局默认\n\n"
                    "支持的 key:\n"
                    "  broker=wss://0.peerjs.com     信令服务器列表(逗号分隔)\n"
                    "  stun=stun:stun.example.com    STUN 列表\n"
                    "  turn=turn:user:pass@host:3478  TURN 中继\n"
                    "  proxy=http://127.0.0.1:7890    信令代理\n"
                    "  key=peerjs                     PeerJS API Key\n"
                    "  keepalive=15s                  隧道心跳间隔\n"
                    "  ice_ports=40000-40100          ICE UDP 端口范围\n"
                    "  nat1to1=203.0.113.10           1:1 NAT 外部 IP\n"
                    "  idle_timeout=30m               Client 按需模式空闲休眠\n"
                    "  max_peers=32                   Server 最大并发客户端\n"
                    "  on_demand=1                    Client 按需连接模式\n"
                    "  force_relay=1                  强制 TURN 中继\n"
                    "  keep_proxy=1                   保留系统环境代理\n\n"
                    "所有 key 留空 = 使用 p2ptun 内置默认值。密钥不需要在这里填——\n"
                    "统一在上方【默认密钥】配置，只此一处。").c_str(),
                L"高级参数说明", MB_ICONINFORMATION);
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

        case WM_DPICHANGED: {
            // 跨屏拖动时更新缩放并按建议矩形重设窗口大小
            UpdateDpiFromWindow(hwnd);
            RECT* suggested = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            RECT rc{};
            GetClientRect(hwnd, &rc);
            LayoutControls(rc.right, rc.bottom);
            LayoutTabChildren();
            return 0;
        }

        case WM_SIZE: {
            if (wParam != SIZE_MINIMIZED) {
                LayoutControls(LOWORD(lParam), HIWORD(lParam));
                LayoutTabChildren();
            }
            return 0;
        }

        case WM_GETMINMAXINFO: {
            MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = Scale(1040);
            mmi->ptMinTrackSize.y = Scale(660);
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
            } else if (hdr && hdr->idFrom == IDC_TAB_MODE && hdr->code == TCN_SELCHANGE) {
                UpdateTabDependentLabels();
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
            for (HWND b : g_state->row_buttons) {
                if (b) DestroyWindow(b);
            }
            g_state->row_buttons.clear();
            KillTimer(hwnd, TIMER_UI);
            RemoveTrayIcon(hwnd);
            PostQuitMessage(0);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int RunGuiMain(HINSTANCE hInstance, int nCmdShow) {
    // 单实例守护: 已有实例运行时弹出提示并退出，
    // 避免两个 GUI 同时管理同一批 p2ptun.exe 子进程造成 JobObject 竞态。
    HANDLE single_mutex = CreateMutexW(nullptr, TRUE, L"Global\\P2PTunManager_SingleInstance_Mutex");
    if (single_mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"p2ptun 管理器已在运行中 (请检查任务栏或系统托盘)。",
                    L"p2ptun Manager", MB_OK | MB_ICONINFORMATION);
        if (single_mutex) ReleaseMutex(single_mutex);
        return 0;
    }

    SetProcessDPIAware();

    INITCOMMONCONTROLSEX icex{};
    icex.dwSize = sizeof(icex);
    icex.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_TAB_CLASSES;
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

    // 窗口初始尺寸按 DPI 缩放 (96-DPI 基准 1060x700)
    UpdateDpiFromWindow(GetDesktopWindow());
    HWND hwnd = CreateWindowExW(
        0,
        kClassName,
        L"p2ptun Windows 多端口穿透管理器 (Server / Client 双页 · 高 DPI 自适应)",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, Scale(1060), Scale(700),
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
            DispatchMessageW(&msg);
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
