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
    IDC_BTN_RAND_SECRET   = 1012,   // 随机生成全局 Secret

    // ② 规则编辑区 (Server/Client 角色 + ID + Port)
    IDC_TAB_MODE          = 1020,
    IDC_CMB_PRESET        = 1021,
    IDC_EDT_REMARK        = 1022,
    IDC_EDT_NAME          = 1023,   // 隧道 ID (默认随机值)
    IDC_EDT_PORT          = 1024,   // 端口 Port
    IDC_CMB_PROTO         = 1026,
    IDC_EDT_BW            = 1027,
    IDC_BTN_ADD_START     = 1028,
    IDC_BTN_PASTE_PAIR    = 1029,
    IDC_EDT_ADV_RULE      = 1030,   // 单规则高级参数覆盖 (留空=用全局默认)
    IDC_BTN_RAND_ID       = 1031,   // 随机生成隧道 ID
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
    HWND lblExePath = nullptr;
    HWND edtExePath = nullptr;
    HWND btnBrowseExe = nullptr;
    HWND lblDefSecret = nullptr;
    HWND edtDefSecret = nullptr;
    HWND btnRandSecret = nullptr;
    HWND lblDefProto = nullptr;
    HWND cmbDefProto = nullptr;
    HWND lblDefBw = nullptr;
    HWND edtDefBw = nullptr;
    HWND chkMinTray = nullptr;
    HWND btnSaveGlobal = nullptr;
    HWND btnOpenCfg = nullptr;
    HWND lblAdvGlobal = nullptr;
    HWND edtAdvGlobal = nullptr;
    HWND btnAdvHelp = nullptr;
    HWND btnExportAll = nullptr;
    HWND btnModeToggle = nullptr;

    // ② 隧道 Item 编辑区 (GroupBox + 独立 Tab 条，绝不与输入控件重叠)
    HWND grpRule = nullptr;
    HWND tabMode = nullptr;
    HWND lblPreset = nullptr;
    HWND cmbPreset = nullptr;
    HWND lblRemark = nullptr;
    HWND edtRemark = nullptr;
    HWND btnPastePair = nullptr;
    HWND lblName = nullptr;
    HWND edtName = nullptr;
    HWND btnRandId = nullptr;
    HWND lblPort = nullptr;
    HWND edtPort = nullptr;
    HWND lblProtoBw = nullptr;
    HWND cmbProto = nullptr;
    HWND edtBw = nullptr;
    HWND btnAddStart = nullptr;
    HWND lblAdvRule = nullptr;
    HWND edtAdvRule = nullptr;
    HWND chkOnDemand = nullptr;

    // 列表 + 行内启停
    HWND listTunnels = nullptr;
    std::vector<HWND> row_buttons;
    std::vector<std::string> row_button_ids;

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

    bool advanced_mode = false;      // 默认简易模式: 全局仅设 Secret，每个 Item 仅设 ID + Port
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
    if (hwnd) {
        SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
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
        append_kv("secret", rule->secret);
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
    r.secret.clear();
    r.broker.clear();
    r.stun.clear();
    r.turn.clear();
    r.proxy.clear();
    r.peer_key.clear();
    r.keepalive.clear();
    r.ice_ports.clear();
    r.nat1to1.clear();
    r.idle_timeout.clear();
    r.max_peers = 0;
    r.force_relay = false;
    r.keep_proxy = false;

    std::istringstream iss(p2ptun::Trim(adv_raw));
    std::string tok;
    while (iss >> tok) {
        auto eq = tok.find('=');
        if (eq == std::string::npos) continue;
        std::string key = p2ptun::ToLower(tok.substr(0, eq));
        std::string val = tok.substr(eq + 1);
        if (key == "secret") r.secret = val;
        else if (key == "broker") r.broker = val;
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
    cfg.adv_broker.clear();
    cfg.adv_stun.clear();
    cfg.adv_turn.clear();
    cfg.adv_proxy.clear();
    cfg.adv_peer_key.clear();
    cfg.adv_keepalive.clear();
    cfg.adv_ice_ports.clear();
    cfg.adv_nat1to1.clear();
    cfg.adv_idle_timeout.clear();
    cfg.adv_max_peers = 0;
    cfg.adv_force_relay = false;
    cfg.adv_keep_proxy = false;

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
    if (GetCtrlTextU8(g_state->edtName).empty()) {
        SetCtrlTextU8(g_state->edtName, p2ptun::GenerateRandomTunnelId());
    }
    SetCtrlTextU8(g_state->edtPort, is_connect ? preset.connect_port : preset.expose_port);
}

void UpdateTabDependentLabels();

// 把选中行回填到编辑区 (按行的 mode 切换 Tab)
void PopulateQuickBarFromRow(int row_idx) {
    if (!g_state || row_idx < 0 || row_idx >= static_cast<int>(g_state->cached_rows.size())) return;
    const auto& row = g_state->cached_rows[static_cast<size_t>(row_idx)];
    g_state->editing_rule_id = row.id;

    int tab_idx = (row.mode == "connect") ? 1 : 0;
    g_state->syncing_tab = true;
    SendMessageW(g_state->tabMode, TCM_SETCURSEL, tab_idx, 0);
    UpdateTabDependentLabels();
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
        // 与 10 列严格对齐: 0=启停按钮占位, 1=#, 2=运行状态, 3=模式, 4=ID, 5=Port, 6=备注, 7=协议/带宽, 8=速率/流量, 9=详情
        std::vector<std::wstring> cols = {
            L"",
            ToW(std::to_string(r.index)),
            ToW(r.status_text),
            ToW(r.mode_label),
            ToW(r.name),
            ToW(r.port),
            ToW(r.remark),
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

    for (size_t i = 0; i < g_state->row_buttons.size() && i < rows.size(); ++i) {
        RECT rc{};
        ListView_GetItemRect(hList, static_cast<int>(i), &rc, LVIR_BOUNDS);
        MapWindowPoints(hList, g_state->hwnd, reinterpret_cast<POINT*>(&rc), 2);
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

// Tab 标签随当前页切换："Port(暴露):" / "Port(本地):"，并同步端口预设方向
void UpdateTabDependentLabels() {
    if (!g_state) return;
    static std::string last_mode = "expose";
    std::string cur_mode = CurrentTabMode();
    bool is_connect = (cur_mode == "connect");
    SetWindowTextW(g_state->lblPort, is_connect ? L"Port(本地):" : L"Port(暴露):");

    if (!g_state->syncing_tab && cur_mode != last_mode) {
        std::string cur_port = GetCtrlTextU8(g_state->edtPort);
        std::string p = p2ptun::ExtractPortOnly(cur_port);
        std::string flipped = is_connect
                                  ? p2ptun::SuggestConnectPort("127.0.0.1:" + p)
                                  : p2ptun::SuggestExposePort("127.0.0.1:" + p);
        if (!flipped.empty()) {
            SetCtrlTextU8(g_state->edtPort, flipped);
        }
        SendMessageW(g_state->cmbPreset, CB_SETCURSEL, 0, 0);
    }
    last_mode = cur_mode;
}

// ---------------------------------------------------------------------------
// 统一布局：简易模式 vs 高级模式
//   简易模式：
//     ① 全局设置仅显示 Secret (默认全等) + 随机 + 保存 + 切换高级模式
//     ② 每个 Item 仅显示 Server/Client 角色 + ID (默认随机值) + Port + 保存启动
//     列表自动收起「备注说明」「协议/带宽」高级列
//   高级模式：
//     展开全部全局设置、预设、备注、协议/带宽、单条参数覆盖与全部表格列
// ---------------------------------------------------------------------------
void LayoutControls(int width, int height) {
    if (!g_state || width <= 0 || height <= 0) return;

    int pad = Scale(10);
    int content_w = width - pad * 2;
    if (content_w < Scale(780)) content_w = Scale(780);

    BOOL adv = g_state->advanced_mode ? TRUE : FALSE;
    auto showAdv = [&](HWND h) {
        if (h) ShowWindow(h, adv ? SW_SHOW : SW_HIDE);
    };

    // ① 全局设置区显隐与排版
    showAdv(g_state->lblExePath);
    showAdv(g_state->edtExePath);
    showAdv(g_state->btnBrowseExe);
    showAdv(g_state->lblDefProto);
    showAdv(g_state->cmbDefProto);
    showAdv(g_state->lblDefBw);
    showAdv(g_state->edtDefBw);
    showAdv(g_state->chkMinTray);
    showAdv(g_state->btnOpenCfg);
    showAdv(g_state->lblAdvGlobal);
    showAdv(g_state->edtAdvGlobal);
    showAdv(g_state->btnAdvHelp);
    showAdv(g_state->btnExportAll);

    int y = Scale(6);
    int g1_h = adv ? Scale(92) : Scale(56);
    MoveWindow(g_state->grpGlobal, pad, y, content_w, g1_h, TRUE);
    SetWindowTextW(g_state->grpGlobal,
                   adv ? L"① 全局设置 (高级模式：核心路径 / 全局 Secret / 默认协议带宽 / 全局高级参数)"
                       : L"① 全局设置 (简易模式：仅需设定全局 Secret，所有隧道默认全等共用)");

    int x0 = pad + Scale(12);
    if (!adv) {
        // 简易模式 ① 区：仅一行 Secret + 随机 + 保存 + 切换高级模式
        int r1y = y + Scale(22);
        SetWindowTextW(g_state->lblDefSecret, L"Secret (默认全等):");
        SetWindowTextW(g_state->btnSaveGlobal, L"💾 保存 Secret");
        MoveWindow(g_state->lblDefSecret, x0, r1y + Scale(4), Scale(118), Scale(22), TRUE);
        MoveWindow(g_state->edtDefSecret, x0 + Scale(122), r1y, Scale(200), Scale(24), TRUE);
        MoveWindow(g_state->btnRandSecret, x0 + Scale(328), r1y - Scale(1), Scale(72), Scale(26), TRUE);
        MoveWindow(g_state->btnSaveGlobal, x0 + Scale(408), r1y - Scale(1), Scale(116), Scale(26), TRUE);
        MoveWindow(g_state->btnModeToggle, pad + content_w - Scale(124), r1y - Scale(1), Scale(112), Scale(26), TRUE);
    } else {
        // 高级模式 ① 区：两行完整全局配置
        int r1y = y + Scale(24);
        int r2y = y + Scale(56);
        SetWindowTextW(g_state->lblDefSecret, L"Secret:");
        SetWindowTextW(g_state->btnSaveGlobal, L"💾 保存设置");

        MoveWindow(g_state->lblExePath, x0, r1y + Scale(4), Scale(60), Scale(22), TRUE);
        MoveWindow(g_state->edtExePath, x0 + Scale(62), r1y, Scale(105), Scale(24), TRUE);
        MoveWindow(g_state->btnBrowseExe, x0 + Scale(170), r1y - Scale(1), Scale(54), Scale(26), TRUE);

        MoveWindow(g_state->lblDefSecret, x0 + Scale(232), r1y + Scale(4), Scale(48), Scale(22), TRUE);
        MoveWindow(g_state->edtDefSecret, x0 + Scale(282), r1y, Scale(120), Scale(24), TRUE);
        MoveWindow(g_state->btnRandSecret, x0 + Scale(405), r1y - Scale(1), Scale(58), Scale(26), TRUE);

        MoveWindow(g_state->lblDefProto, x0 + Scale(471), r1y + Scale(4), Scale(38), Scale(22), TRUE);
        MoveWindow(g_state->cmbDefProto, x0 + Scale(511), r1y - Scale(1), Scale(92), Scale(120), TRUE);

        MoveWindow(g_state->lblDefBw, x0 + Scale(611), r1y + Scale(4), Scale(52), Scale(22), TRUE);
        MoveWindow(g_state->edtDefBw, x0 + Scale(665), r1y, Scale(44), Scale(24), TRUE);

        MoveWindow(g_state->chkMinTray, x0 + Scale(717), r1y + Scale(2), Scale(110), Scale(22), TRUE);
        MoveWindow(g_state->btnSaveGlobal, x0 + Scale(831), r1y - Scale(1), Scale(82), Scale(26), TRUE);
        MoveWindow(g_state->btnOpenCfg, x0 + Scale(917), r1y - Scale(1), Scale(78), Scale(26), TRUE);

        MoveWindow(g_state->lblAdvGlobal, x0, r2y + Scale(4), Scale(60), Scale(22), TRUE);
        int adv_edt_w = content_w - Scale(390);
        if (adv_edt_w < Scale(320)) adv_edt_w = Scale(320);
        MoveWindow(g_state->edtAdvGlobal, x0 + Scale(62), r2y, adv_edt_w, Scale(24), TRUE);
        int ax = x0 + Scale(68) + adv_edt_w;
        MoveWindow(g_state->btnAdvHelp, ax, r2y - Scale(1), Scale(32), Scale(26), TRUE);
        MoveWindow(g_state->btnExportAll, ax + Scale(38), r2y - Scale(1), Scale(148), Scale(26), TRUE);
        MoveWindow(g_state->btnModeToggle, pad + content_w - Scale(124), r2y - Scale(1), Scale(112), Scale(26), TRUE);
    }

    y += g1_h + Scale(6);

    // ② 隧道规则编辑区显隐与排版
    showAdv(g_state->lblPreset);
    showAdv(g_state->cmbPreset);
    showAdv(g_state->lblRemark);
    showAdv(g_state->edtRemark);
    showAdv(g_state->btnPastePair);
    showAdv(g_state->lblProtoBw);
    showAdv(g_state->cmbProto);
    showAdv(g_state->edtBw);
    showAdv(g_state->lblAdvRule);
    showAdv(g_state->edtAdvRule);
    showAdv(g_state->chkOnDemand);

    int g2_h = adv ? Scale(126) : Scale(62);
    MoveWindow(g_state->grpRule, pad, y, content_w, g2_h, TRUE);
    SetWindowTextW(g_state->grpRule,
                   adv ? L"② 隧道设置 (高级模式：支持预设、备注、协议带宽、按需连接与单条参数覆盖)"
                       : L"② 隧道设置 (简易模式：每个 Item 仅需设定 ID 与 Port，ID 默认给出随机值)");

    if (!adv) {
        // 简易模式 ② 区：单行 (左侧 Server/Client 角色切换 Tab 条 + ID + 随机 + Port + 保存并启动)
        int ry = y + Scale(24);
        MoveWindow(g_state->tabMode, x0, ry - Scale(2), Scale(256), Scale(28), TRUE);
        MoveWindow(g_state->lblName, x0 + Scale(272), ry + Scale(4), Scale(26), Scale(22), TRUE);
        MoveWindow(g_state->edtName, x0 + Scale(300), ry, Scale(150), Scale(24), TRUE);
        MoveWindow(g_state->btnRandId, x0 + Scale(454), ry - Scale(1), Scale(68), Scale(26), TRUE);
        MoveWindow(g_state->lblPort, x0 + Scale(536), ry + Scale(4), Scale(72), Scale(22), TRUE);
        MoveWindow(g_state->edtPort, x0 + Scale(610), ry, Scale(120), Scale(24), TRUE);
        MoveWindow(g_state->btnAddStart, x0 + Scale(744), ry - Scale(1), Scale(190), Scale(26), TRUE);
    } else {
        // 高级模式 ② 区：三行无重叠布局
        int row1_y = y + Scale(24);
        int row2_y = row1_y + Scale(33);
        int row3_y = row2_y + Scale(33);

        MoveWindow(g_state->tabMode, x0, row1_y - Scale(2), Scale(256), Scale(28), TRUE);
        MoveWindow(g_state->lblPreset, x0 + Scale(270), row1_y + Scale(4), Scale(60), Scale(22), TRUE);
        MoveWindow(g_state->cmbPreset, x0 + Scale(332), row1_y, Scale(178), Scale(220), TRUE);
        MoveWindow(g_state->lblRemark, x0 + Scale(522), row1_y + Scale(4), Scale(60), Scale(22), TRUE);
        MoveWindow(g_state->edtRemark, x0 + Scale(584), row1_y, Scale(140), Scale(24), TRUE);
        MoveWindow(g_state->btnPastePair, x0 + Scale(736), row1_y - Scale(1), Scale(220), Scale(26), TRUE);

        MoveWindow(g_state->lblName, x0, row2_y + Scale(4), Scale(26), Scale(22), TRUE);
        MoveWindow(g_state->edtName, x0 + Scale(28), row2_y, Scale(140), Scale(24), TRUE);
        MoveWindow(g_state->btnRandId, x0 + Scale(172), row2_y - Scale(1), Scale(64), Scale(26), TRUE);
        MoveWindow(g_state->lblPort, x0 + Scale(248), row2_y + Scale(4), Scale(72), Scale(22), TRUE);
        MoveWindow(g_state->edtPort, x0 + Scale(322), row2_y, Scale(110), Scale(24), TRUE);
        MoveWindow(g_state->lblProtoBw, x0 + Scale(444), row2_y + Scale(4), Scale(62), Scale(22), TRUE);
        MoveWindow(g_state->cmbProto, x0 + Scale(508), row2_y, Scale(78), Scale(120), TRUE);
        MoveWindow(g_state->edtBw, x0 + Scale(590), row2_y, Scale(44), Scale(24), TRUE);
        MoveWindow(g_state->chkOnDemand, x0 + Scale(644), row2_y + Scale(2), Scale(116), Scale(22), TRUE);
        MoveWindow(g_state->btnAddStart, x0 + Scale(766), row2_y - Scale(1), Scale(190), Scale(26), TRUE);

        MoveWindow(g_state->lblAdvRule, x0, row3_y + Scale(4), Scale(88), Scale(22), TRUE);
        MoveWindow(g_state->edtAdvRule, x0 + Scale(92), row3_y, content_w - Scale(116), Scale(24), TRUE);
    }

    y += g2_h + Scale(6);

    // 列表列宽随简易/高级模式动态切换：简易模式隐藏「备注说明」(col 6) 与「协议/带宽」(col 7)
    if (g_state->listTunnels) {
        ListView_SetColumnWidth(g_state->listTunnels, 4, adv ? Scale(135) : Scale(180)); // ID 列
        ListView_SetColumnWidth(g_state->listTunnels, 5, adv ? Scale(120) : Scale(150)); // Port 列
        ListView_SetColumnWidth(g_state->listTunnels, 6, adv ? Scale(130) : 0);          // 备注说明 (仅高级模式)
        ListView_SetColumnWidth(g_state->listTunnels, 7, adv ? Scale(88) : 0);           // 协议/带宽 (仅高级模式)
    }

    // 列表与底部区域
    int action_h = Scale(34);
    int log_h = Scale(150);
    int list_top = y;
    int list_h = height - y - action_h - log_h - Scale(24);
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

// ---------------------------------------------------------------------------
// 控件创建
// ---------------------------------------------------------------------------
void CreateAllControls(HWND hwnd) {
    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));

    UpdateDpiFromWindow(hwnd);

    int font_h = -MulDiv(9, g_dpi, 72);
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

    // ============ ① 全局设置区 ============
    g_state->grpGlobal = CreateWindowExW(0, L"BUTTON",
                                         L"① 全局设置 (简易模式：仅需设定全局 Secret，所有隧道默认全等共用)",
                                         WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                         0, 0, Scale(980), Scale(56), hwnd, nullptr, hInst, nullptr);
    SetFontRecursive(g_state->grpGlobal, g_state->hFontBold);

    g_state->lblExePath = makeLabel(L"核心路径:", 0, 0, Scale(60), Scale(22));
    g_state->edtExePath = makeEdit(IDC_EDT_P2PTUN_PATH, L"p2ptun.exe", 0, 0, Scale(110), Scale(24));
    g_state->btnBrowseExe = makeBtn(IDC_BTN_BROWSE_EXE, L"浏览...", 0, 0, Scale(54), Scale(26));

    g_state->lblDefSecret = makeLabel(L"Secret (默认全等):", 0, 0, Scale(118), Scale(22));
    g_state->edtDefSecret = makeEdit(IDC_EDT_DEF_SECRET, L"", 0, 0, Scale(180), Scale(24));
    g_state->btnRandSecret = makeBtn(IDC_BTN_RAND_SECRET, L"🎲 随机", 0, 0, Scale(68), Scale(26));

    g_state->lblDefProto = makeLabel(L"协议:", 0, 0, Scale(40), Scale(22));
    g_state->cmbDefProto = CreateWindowExW(0, L"COMBOBOX", L"",
                                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                                           0, 0, Scale(92), Scale(120), hwnd,
                                           reinterpret_cast<HMENU>(IDC_CMB_DEF_PROTO), hInst, nullptr);
    SetFontRecursive(g_state->cmbDefProto, g_state->hFont);
    SendMessageW(g_state->cmbDefProto, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"hy2 (极速)"));
    SendMessageW(g_state->cmbDefProto, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"quic"));
    SendMessageW(g_state->cmbDefProto, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"classic"));
    SendMessageW(g_state->cmbDefProto, CB_SETCURSEL, 0, 0);

    g_state->lblDefBw = makeLabel(L"带宽(M):", 0, 0, Scale(52), Scale(22));
    g_state->edtDefBw = makeEdit(IDC_EDT_DEF_BW, L"100", 0, 0, Scale(44), Scale(24), ES_NUMBER);

    g_state->chkMinTray = CreateWindowExW(0, L"BUTTON", L"最小化到托盘",
                                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                          0, 0, Scale(110), Scale(22), hwnd,
                                          reinterpret_cast<HMENU>(IDC_CHK_MIN_TRAY), hInst, nullptr);
    SetFontRecursive(g_state->chkMinTray, g_state->hFont);

    g_state->btnSaveGlobal = makeBtn(IDC_BTN_SAVE_GLOBAL, L"💾 保存 Secret", 0, 0, Scale(110), Scale(26));
    g_state->btnOpenCfg = makeBtn(IDC_BTN_OPEN_CONFIG, L"📂 配置文件", 0, 0, Scale(78), Scale(26));

    g_state->lblAdvGlobal = makeLabel(L"高级默认:", 0, 0, Scale(60), Scale(22));
    g_state->edtAdvGlobal = makeEdit(IDC_EDT_ADV_GLOBAL, L"", 0, 0, Scale(500), Scale(24));
    SendMessageW(g_state->edtAdvGlobal, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(
        L"全局生效: broker=wss://0.peerjs.com keepalive=15s max_peers=32 ... (留空=p2ptun 内置默认)"));
    g_state->btnAdvHelp = makeBtn(IDC_BTN_ADV_HELP, L"❓", 0, 0, Scale(32), Scale(26));
    g_state->btnExportAll = makeBtn(IDC_BTN_EXPORT_ALL, L"📤 批量导出配置", 0, 0, Scale(148), Scale(26));
    g_state->btnModeToggle = makeBtn(IDC_BTN_MODE_TOGGLE, L"⚙ 高级模式", 0, 0, Scale(112), Scale(26), true);

    // ============ ② 隧道设置区 (每个 Item 简易模式只需 ID + Port) ============
    g_state->grpRule = CreateWindowExW(0, L"BUTTON",
                                       L"② 隧道设置 (简易模式：每个 Item 仅需设定 ID 与 Port，ID 默认给出随机值)",
                                       WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                       0, 0, Scale(980), Scale(62), hwnd, nullptr, hInst, nullptr);
    SetFontRecursive(g_state->grpRule, g_state->hFontBold);

    g_state->tabMode = CreateWindowExW(0, WC_TABCONTROLW, L"",
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | TCS_TABS,
                                       0, 0, Scale(256), Scale(28), hwnd,
                                       reinterpret_cast<HMENU>(IDC_TAB_MODE), hInst, nullptr);
    SetFontRecursive(g_state->tabMode, g_state->hFont);

    TCITEMW ti{};
    ti.mask = TCIF_TEXT;
    ti.pszText = const_cast<LPWSTR>(L"📤 Server (暴露)");
    SendMessageW(g_state->tabMode, TCM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&ti));
    ti.pszText = const_cast<LPWSTR>(L"📥 Client (连接)");
    SendMessageW(g_state->tabMode, TCM_INSERTITEMW, 1, reinterpret_cast<LPARAM>(&ti));
    SendMessageW(g_state->tabMode, TCM_SETCURSEL, 0, 0);

    g_state->lblPreset = makeLabel(L"常用预设:", 0, 0, Scale(60), Scale(22));
    g_state->cmbPreset = CreateWindowExW(0, L"COMBOBOX", L"",
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                                         0, 0, Scale(178), Scale(220), hwnd,
                                         reinterpret_cast<HMENU>(IDC_CMB_PRESET), hInst, nullptr);
    SetFontRecursive(g_state->cmbPreset, g_state->hFont);
    for (const auto& p : p2ptun::GetScenePresets()) {
        std::wstring wl = ToW(p.label);
        SendMessageW(g_state->cmbPreset, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wl.c_str()));
    }
    SendMessageW(g_state->cmbPreset, CB_SETCURSEL, 0, 0);

    g_state->lblRemark = makeLabel(L"备注说明:", 0, 0, Scale(60), Scale(22));
    g_state->edtRemark = makeEdit(IDC_EDT_REMARK, L"", 0, 0, Scale(140), Scale(24));
    g_state->btnPastePair = makeBtn(IDC_BTN_PASTE_PAIR, L"📥 粘贴配对码一键导入", 0, 0, Scale(220), Scale(26), true);

    // ID 默认给出随机值
    std::wstring init_rand_id = ToW(p2ptun::GenerateRandomTunnelId());
    g_state->lblName = makeLabel(L"ID:", 0, 0, Scale(26), Scale(22));
    g_state->edtName = makeEdit(IDC_EDT_NAME, init_rand_id.c_str(), 0, 0, Scale(150), Scale(24));
    g_state->btnRandId = makeBtn(IDC_BTN_RAND_ID, L"🎲 随机", 0, 0, Scale(68), Scale(26));

    g_state->lblPort = makeLabel(L"Port(暴露):", 0, 0, Scale(72), Scale(22));
    g_state->edtPort = makeEdit(IDC_EDT_PORT, L"3389", 0, 0, Scale(120), Scale(24));

    g_state->lblProtoBw = makeLabel(L"协议/带宽:", 0, 0, Scale(62), Scale(22));
    g_state->cmbProto = CreateWindowExW(0, L"COMBOBOX", L"",
                                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                                        0, 0, Scale(78), Scale(120), hwnd,
                                        reinterpret_cast<HMENU>(IDC_CMB_PROTO), hInst, nullptr);
    SetFontRecursive(g_state->cmbProto, g_state->hFont);
    SendMessageW(g_state->cmbProto, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"hy2"));
    SendMessageW(g_state->cmbProto, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"quic"));
    SendMessageW(g_state->cmbProto, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"classic"));
    SendMessageW(g_state->cmbProto, CB_SETCURSEL, 0, 0);
    g_state->edtBw = makeEdit(IDC_EDT_BW, L"100", 0, 0, Scale(44), Scale(24), ES_NUMBER);

    g_state->chkOnDemand = CreateWindowExW(0, L"BUTTON", L"Client 按需连接",
                                            WS_CHILD | BS_AUTOCHECKBOX,
                                            0, 0, Scale(116), Scale(22), hwnd,
                                            reinterpret_cast<HMENU>(IDC_CHK_ON_DEMAND), hInst, nullptr);
    SetFontRecursive(g_state->chkOnDemand, g_state->hFont);

    g_state->btnAddStart = makeBtn(IDC_BTN_ADD_START, L"➕ 保存并立即启动", 0, 0, Scale(190), Scale(26), true);

    g_state->lblAdvRule = makeLabel(L"高级参数覆盖:", 0, 0, Scale(88), Scale(22));
    g_state->edtAdvRule = makeEdit(IDC_EDT_ADV_RULE, L"", 0, 0, Scale(500), Scale(24));
    SendMessageW(g_state->edtAdvRule, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(
        L"留空=用上方全局默认; 支持单条覆盖: secret=xxx keepalive=15s ice_ports=40000-40100 ..."));

    // ============ 列表 ============
    g_state->listTunnels = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                           Scale(10), Scale(140), Scale(980), Scale(320), hwnd,
                                           reinterpret_cast<HMENU>(IDC_LIST_TUNNELS), hInst, nullptr);
    SetFontRecursive(g_state->listTunnels, g_state->hFont);
    DWORD ex_style = LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER;
    SendMessageW(g_state->listTunnels, LVM_SETEXTENDEDLISTVIEWSTYLE, ex_style, ex_style);

    struct ColDef { const wchar_t* title; int width; };
    const ColDef cols[] = {
        {L"启停", Scale(52)},
        {L"#", Scale(36)},
        {L"运行状态", Scale(130)},
        {L"角色 (Server/Client)", Scale(145)},
        {L"ID", Scale(160)},
        {L"Port (端口)", Scale(135)},
        {L"备注说明", 0},            // 简易模式默认隐藏，高级模式展开
        {L"协议/带宽", 0},           // 简易模式默认隐藏，高级模式展开
        {L"实时速率 / 累计流量", Scale(165)},
        {L"延迟 / 直连地址 / 状态详情", Scale(200)}
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
    g_state->btnStartOne  = makeBtn(IDC_BTN_START_ONE,  L"▶ 启动选中", 0, 0, Scale(92), Scale(30), true);
    g_state->btnStopOne   = makeBtn(IDC_BTN_STOP_ONE,   L"⏹ 停止选中", 0, 0, Scale(92), Scale(30));
    g_state->btnReconnOne = makeBtn(IDC_BTN_RECONN_ONE, L"🔄 重连选中", 0, 0, Scale(92), Scale(30));
    g_state->btnCopyPair  = makeBtn(IDC_BTN_COPY_PAIR,  L"📋 复制傻瓜配对码", 0, 0, Scale(146), Scale(30), true);
    g_state->btnCopyCli   = makeBtn(IDC_BTN_COPY_CLI,   L"💻 复制命令行", 0, 0, Scale(115), Scale(30));
    g_state->btnLaunchApp = makeBtn(IDC_BTN_LAUNCH_APP, L"🚀 打开客户端(RDP/Web)", 0, 0, Scale(155), Scale(30));
    g_state->btnDeleteOne = makeBtn(IDC_BTN_DELETE_ONE, L"🗑 删除选中", 0, 0, Scale(90), Scale(30));
    g_state->btnStartAll  = makeBtn(IDC_BTN_START_ALL,  L"▶ 全部启动", 0, 0, Scale(92), Scale(30));
    g_state->btnStopAll   = makeBtn(IDC_BTN_STOP_ALL,   L"⏹ 全部停止", 0, 0, Scale(92), Scale(30));

    // ============ 日志区 ============
    g_state->grpLogs = CreateWindowExW(0, L"BUTTON",
                                       L"③ 多隧道实时运行日志 (每行左侧 ▶/⏹ 按钮可独立启停；双击行也可切换)",
                                       WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                       0, 0, Scale(980), Scale(160), hwnd, nullptr, hInst, nullptr);
    SetFontRecursive(g_state->grpLogs, g_state->hFontBold);

    g_state->btnClearLog = makeBtn(IDC_BTN_CLEAR_LOG, L"清空日志", 0, 0, Scale(80), Scale(22));
    g_state->edtLogs = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                       WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
                                       0, 0, Scale(964), Scale(112), hwnd,
                                       reinterpret_cast<HMENU>(IDC_EDT_LOGS), hInst, nullptr);
    SetFontRecursive(g_state->edtLogs, g_state->hFontMono);

    RECT rc{};
    GetClientRect(hwnd, &rc);
    LayoutControls(rc.right, rc.bottom);
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
        case IDC_BTN_RAND_SECRET: {
            SetCtrlTextU8(g_state->edtDefSecret, p2ptun::GenerateRandomSecret());
            break;
        }

        case IDC_BTN_RAND_ID: {
            g_state->editing_rule_id.clear();
            SetCtrlTextU8(g_state->edtName, p2ptun::GenerateRandomTunnelId());
            break;
        }

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
            if (def_sec.empty()) {
                def_sec = p2ptun::GenerateRandomSecret();
                SetCtrlTextU8(g_state->edtDefSecret, def_sec);
            }
            int p_idx = static_cast<int>(SendMessageW(g_state->cmbDefProto, CB_GETCURSEL, 0, 0));
            std::string def_proto = (p_idx == 1) ? "quic" : (p_idx == 2 ? "classic" : "hy2");
            int def_bw = std::atoi(GetCtrlTextU8(g_state->edtDefBw).c_str());
            bool min_tray = (SendMessageW(g_state->chkMinTray, BM_GETCHECK, 0, 0) == BST_CHECKED);

            std::string err;
            if (!g_state->mgr->UpdateGlobalSettings(exe_path, def_sec, def_proto, def_bw, min_tray, err)) {
                MessageBoxW(hwnd, ToW(err).c_str(), L"保存失败", MB_ICONERROR);
            } else {
                if (g_state->advanced_mode) {
                    p2ptun::AppConfig cfg2 = g_state->mgr->GetConfig();
                    ApplyAdvStringToGlobal(GetCtrlTextU8(g_state->edtAdvGlobal), cfg2);
                    std::string adv_err;
                    if (!g_state->mgr->UpdateAdvancedDefaults(cfg2, adv_err)) {
                        MessageBoxW(hwnd, ToW(adv_err).c_str(), L"高级默认保存失败", MB_ICONERROR);
                    }
                }
                RefreshTunnelListView();
                MessageBoxW(hwnd, L"全局设置已保存到 config.json！", L"保存成功", MB_ICONINFORMATION);
            }
            break;
        }

        case IDC_BTN_EXPORT_ALL: {
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
            // 若用户修改了上方 Secret 但还没点保存，自动同步全局 Secret
            std::string cur_sec = GetCtrlTextU8(g_state->edtDefSecret);
            if (cur_sec.empty()) {
                cur_sec = p2ptun::GenerateRandomSecret();
                SetCtrlTextU8(g_state->edtDefSecret, cur_sec);
            }
            p2ptun::AppConfig cur_cfg = g_state->mgr->GetConfig();
            if (cur_sec != cur_cfg.default_secret) {
                std::string sec_err;
                g_state->mgr->UpdateGlobalSettings(cur_cfg.p2ptun_path, cur_sec, cur_cfg.default_proto,
                                                   cur_cfg.default_bandwidth, cur_cfg.minimize_to_tray, sec_err);
            }

            p2ptun::TunnelRule r;
            r.mode = CurrentTabMode();
            r.name = GetCtrlTextU8(g_state->edtName);
            if (r.name.empty()) {
                // ID 为空时自动生成随机值
                r.name = p2ptun::GenerateRandomTunnelId();
                SetCtrlTextU8(g_state->edtName, r.name);
            }
            r.id = r.name;
            r.port = GetCtrlTextU8(g_state->edtPort);
            if (r.port.empty()) {
                MessageBoxW(hwnd, L"请填写端口 Port (直接填数字如 3389 或 1080 即可)！", L"提示", MB_ICONWARNING);
                SetFocus(g_state->edtPort);
                return;
            }

            // 检查是否正在更新已有项
            bool updating_existing = false;
            for (const auto& existing : g_state->cached_rows) {
                if (existing.id == r.id || (!g_state->editing_rule_id.empty() && existing.id == g_state->editing_rule_id && existing.name == r.name)) {
                    r = existing.rule;
                    r.mode = CurrentTabMode();
                    r.id = GetCtrlTextU8(g_state->edtName);
                    r.name = r.id;
                    r.port = GetCtrlTextU8(g_state->edtPort);
                    updating_existing = true;
                    break;
                }
            }

            if (g_state->advanced_mode) {
                r.remark = GetCtrlTextU8(g_state->edtRemark);
                int pr_idx = static_cast<int>(SendMessageW(g_state->cmbProto, CB_GETCURSEL, 0, 0));
                r.proto = (pr_idx == 1) ? "quic" : (pr_idx == 2 ? "classic" : "hy2");
                r.bandwidth_mbps = std::atoi(GetCtrlTextU8(g_state->edtBw).c_str());
                if (r.mode == "connect") {
                    r.on_demand = (SendMessageW(g_state->chkOnDemand, BM_GETCHECK, 0, 0) == BST_CHECKED);
                }
                ApplyAdvStringToRule(GetCtrlTextU8(g_state->edtAdvRule), r);
            }
            r.enabled = true;

            std::string out_id, err;
            bool ok = g_state->mgr->AddOrUpdateRule(r, true, out_id, err);
            if (!updating_existing) {
                // 新增项保存后，为下一次新增预填一个新的随机 ID
                g_state->editing_rule_id.clear();
                SetCtrlTextU8(g_state->edtName, p2ptun::GenerateRandomTunnelId());
            } else {
                g_state->editing_rule_id = out_id;
            }
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
                              "\n\n发给另一台电脑后，对方只需填写相同 ID（或在高级模式粘贴配对码）即可连通！";
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
                SetCtrlTextU8(g_state->edtName, p2ptun::GenerateRandomTunnelId());
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
            RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
            break;
        }

        case IDC_BTN_ADV_HELP: {
            MessageBoxW(hwnd,
                ToW("高级参数以 key=value 形式填写，空格分隔，直接透传给 p2ptun.exe：\n\n"
                    "■ 全局默认框: 对所有规则生效 (规则未覆盖时回退到这里)\n"
                    "■ 规则覆盖框: 只对当前编辑的规则生效，覆盖同名的全局默认\n\n"
                    "支持的 key:\n"
                    "  secret=my-custom-sec           单条隧道独立密钥覆盖(留空=用全局Secret)\n"
                    "  broker=wss://0.peerjs.com      信令服务器列表(逗号分隔)\n"
                    "  stun=stun:stun.example.com     STUN 列表\n"
                    "  turn=turn:user:pass@host:3478   TURN 中继\n"
                    "  proxy=http://127.0.0.1:7890     信令代理\n"
                    "  key=peerjs                      PeerJS API Key\n"
                    "  keepalive=15s                   隧道心跳间隔\n"
                    "  ice_ports=40000-40100           ICE UDP 端口范围\n"
                    "  nat1to1=203.0.113.10            1:1 NAT 外部 IP\n"
                    "  idle_timeout=30m                Client 按需模式空闲休眠\n"
                    "  max_peers=32                    Server 最大并发客户端\n"
                    "  on_demand=1                     Client 按需连接模式\n"
                    "  force_relay=1                   强制 TURN 中继\n"
                    "  keep_proxy=1                    保留系统环境代理\n\n"
                    "简易模式下只需要配置全局 Secret（默认全等）以及每条隧道的 ID（默认随机）和 Port。").c_str(),
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
            UpdateDpiFromWindow(hwnd);
            RECT* suggested = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            RECT rc{};
            GetClientRect(hwnd, &rc);
            LayoutControls(rc.right, rc.bottom);
            RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
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
            mmi->ptMinTrackSize.x = Scale(1040);
            mmi->ptMinTrackSize.y = Scale(640);
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

    UpdateDpiFromWindow(GetDesktopWindow());
    HWND hwnd = CreateWindowExW(
        0,
        kClassName,
        L"p2ptun Windows 多端口穿透管理器 (简易模式 / 高级模式 · 高 DPI 自适应)",
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
