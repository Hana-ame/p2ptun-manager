#include "process_manager.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")
#endif

namespace p2ptun {

namespace {

std::string CurrentTimeHMS() {
    std::time_t now = std::time(nullptr);
    std::tm tm_val{};
#ifdef _WIN32
    localtime_s(&tm_val, &now);
#else
    localtime_r(&now, &tm_val);
#endif
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm_val.tm_hour, tm_val.tm_min, tm_val.tm_sec);
    return std::string(buf);
}

#ifdef _WIN32
std::wstring Utf8ToWideStr(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return L"";
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0], len);
    return out;
}

std::string WideToUtf8Str(const std::wstring& w) {
    if (w.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &out[0], len, nullptr, nullptr);
    return out;
}

std::wstring QuoteWin32Arg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return arg;
    }
    std::wstring out = L"\"";
    for (auto it = arg.begin();; ++it) {
        size_t num_backslashes = 0;
        while (it != arg.end() && *it == L'\\') {
            ++it;
            ++num_backslashes;
        }
        if (it == arg.end()) {
            out.append(num_backslashes * 2, L'\\');
            break;
        } else if (*it == L'"') {
            out.append(num_backslashes * 2 + 1, L'\\');
            out.push_back(*it);
        } else {
            out.append(num_backslashes, L'\\');
            out.push_back(*it);
        }
    }
    out.push_back(L'"');
    return out;
}

std::vector<wchar_t> BuildChildEnvironmentBlock(const std::string& secret) {
    std::vector<wchar_t> block;
    LPWCH env_strings = GetEnvironmentStringsW();
    if (env_strings) {
        const wchar_t* cur = env_strings;
        while (*cur) {
            size_t len = wcslen(cur);
            std::wstring entry(cur, len);
            if (entry.rfind(L"P2PTUN_SECRET=", 0) != 0 &&
                entry.rfind(L"_P2PTUN_DAEMON_CHILD=", 0) != 0) {
                block.insert(block.end(), entry.begin(), entry.end());
                block.push_back(L'\0');
            }
            cur += len + 1;
        }
        FreeEnvironmentStringsW(env_strings);
    }
    std::wstring sec_entry = L"P2PTUN_SECRET=" + Utf8ToWideStr(secret);
    block.insert(block.end(), sec_entry.begin(), sec_entry.end());
    block.push_back(L'\0');
    block.push_back(L'\0');
    return block;
}

bool HttpLocalRequest(int port, const std::wstring& method, const std::wstring& path, std::string& out_body) {
    out_body.clear();
    if (port <= 0) return false;

    HINTERNET hSession = WinHttpOpen(L"p2ptun-win-gui/1.0",
                                     WINHTTP_ACCESS_TYPE_NO_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;

    WinHttpSetTimeouts(hSession, 400, 400, 500, 500);

    HINTERNET hConnect = WinHttpConnect(hSession, L"127.0.0.1", static_cast<INTERNET_PORT>(port), 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return false;
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, method.c_str(), path.c_str(),
                                            nullptr, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    // p2ptun v0.5.2+ 控制面板启用 CSRF 同源校验 (daemon.go crossSiteRequest):
    // POST /quit 与 POST /reconnect 必须携带同源 Origin/Referer 头，否则返回 403。
    std::wstring origin_headers = L"Origin: http://127.0.0.1:" + std::to_wstring(port) +
                                  L"\r\nReferer: http://127.0.0.1:" + std::to_wstring(port) + L"/\r\n";
    BOOL ok = WinHttpSendRequest(hRequest,
                                 origin_headers.c_str(),
                                 static_cast<DWORD>(-1),
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (ok) {
        ok = WinHttpReceiveResponse(hRequest, nullptr);
    }
    if (ok) {
        DWORD dwSize = 0;
        do {
            dwSize = 0;
            if (!WinHttpQueryDataAvailable(hRequest, &dwSize) || dwSize == 0) break;
            std::string chunk(dwSize, '\0');
            DWORD dwDownloaded = 0;
            if (WinHttpReadData(hRequest, &chunk[0], dwSize, &dwDownloaded) && dwDownloaded > 0) {
                chunk.resize(dwDownloaded);
                out_body.append(chunk);
            } else {
                break;
            }
        } while (dwSize > 0);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return ok == TRUE;
}

#endif

} // namespace

LogBuffer::LogBuffer(size_t max_lines) : max_lines_(max_lines > 0 ? max_lines : 500) {}

void LogBuffer::Append(const std::string& prefix, const std::string& text) {
    std::string cleaned = Trim(text);
    if (cleaned.empty()) return;

    std::string ts = CurrentTimeHMS();
    std::lock_guard<std::mutex> lock(mu_);
    std::istringstream iss(cleaned);
    std::string line;
    while (std::getline(iss, line)) {
        line = Trim(line);
        if (line.empty()) continue;
        std::string formatted = "[" + ts + "]";
        if (!prefix.empty()) {
            formatted += " [" + prefix + "]";
        }
        formatted += " " + line;
        if (lines_.size() >= max_lines_) {
            lines_.erase(lines_.begin());
        }
        lines_.push_back(std::move(formatted));
    }
    seq_.fetch_add(1);
}

void LogBuffer::Clear() {
    std::lock_guard<std::mutex> lock(mu_);
    lines_.clear();
    seq_.fetch_add(1);
}

uint64_t LogBuffer::Seq() const {
    return seq_.load();
}

std::string LogBuffer::GetText(size_t tail_lines, const std::string& line_sep) const {
    std::lock_guard<std::mutex> lock(mu_);
    size_t n = lines_.size();
    size_t start = (tail_lines > 0 && tail_lines < n) ? (n - tail_lines) : 0;
    std::ostringstream oss;
    for (size_t i = start; i < n; ++i) {
        oss << lines_[i];
        if (i + 1 < n) oss << line_sep;
    }
    return oss.str();
}

MultiTunnelManager::MultiTunnelManager(const std::string& config_path)
    : config_path_(config_path) {
#ifdef _WIN32
    HANDLE hJob = CreateJobObjectW(nullptr, nullptr);
    if (hJob) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
        job_handle_ = hJob;
    }
#endif
    std::string err;
    if (!LoadAppConfigFromFile(config_path_, cfg_, err)) {
        logs_.Append("系统", "配置文件读取提示: " + err + "，已启用默认配置");
        cfg_ = CreateDefaultAppConfig();
    }
    std::lock_guard<std::mutex> lock(mu_);
    RebuildInstancesLocked();
    logs_.Append("系统", "p2ptun Windows 多端口穿透管理器已就绪，配置文件: " + config_path_);
}

MultiTunnelManager::~MultiTunnelManager() {
    StopAll(false);
#ifdef _WIN32
    if (job_handle_) {
        CloseHandle(static_cast<HANDLE>(job_handle_));
        job_handle_ = nullptr;
    }
#endif
}

AppConfig MultiTunnelManager::GetConfig() const {
    std::lock_guard<std::mutex> lock(mu_);
    return cfg_;
}

int MultiTunnelManager::AllocateStatusPortLocked() {
    int p = next_status_port_++;
    if (next_status_port_ > 19900) {
        next_status_port_ = 18300;
    }
    return p;
}

std::string MultiTunnelManager::ResolveP2PTunExecutableLocked() const {
    std::string configured = Trim(cfg_.p2ptun_path);
    if (configured.empty()) configured = "p2ptun.exe";
#ifdef _WIN32
    if (configured.find(':') == std::string::npos && configured.find('\\') == std::string::npos && configured.find('/') == std::string::npos) {
        wchar_t exe_buf[MAX_PATH]{};
        if (GetModuleFileNameW(nullptr, exe_buf, MAX_PATH) > 0) {
            std::wstring exe_dir(exe_buf);
            auto slash = exe_dir.find_last_of(L"\\/");
            if (slash != std::wstring::npos) {
                std::wstring candidate = exe_dir.substr(0, slash + 1) + Utf8ToWideStr(configured);
                DWORD attr = GetFileAttributesW(candidate.c_str());
                if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
                    return WideToUtf8Str(candidate);
                }
            }
        }
    }
#endif
    return configured;
}

void MultiTunnelManager::RebuildInstancesLocked() {
    std::vector<std::shared_ptr<ManagedInstance>> next_list;
    auto findExisting = [this](const std::string& id) -> std::shared_ptr<ManagedInstance> {
        for (const auto& inst : instances_) {
            if (inst->rule.id == id) return inst;
        }
        return nullptr;
    };

    auto appendRule = [&](const TunnelRule& r) {
        auto existing = findExisting(r.id);
        if (existing) {
            {
                std::lock_guard<std::mutex> ilock(existing->mu);
                existing->rule = r;
            }
            next_list.push_back(existing);
        } else {
            auto inst = std::make_shared<ManagedInstance>();
            inst->rule = r;
            inst->status_port = AllocateStatusPortLocked();
            next_list.push_back(inst);
        }
    };

    // 统一隧道配置: 每条配置角色无关, rule.mode 表示当前/上次运行角色
    for (const auto& r : cfg_.tunnels) appendRule(r);
    instances_ = std::move(next_list);
}

void MultiTunnelManager::SyncConfigFromInstancesLocked() {
    cfg_.tunnels.clear();
    for (const auto& inst : instances_) {
        std::lock_guard<std::mutex> ilock(inst->mu);
        TunnelRule r = inst->rule;
        if (r.mode != "connect") r.mode = "expose";
        cfg_.tunnels.push_back(r);
    }
}

bool MultiTunnelManager::SaveConfigLocked(std::string& out_err) {
    SyncConfigFromInstancesLocked();
    return SaveAppConfigToFile(config_path_, cfg_, out_err);
}

bool MultiTunnelManager::UpdateGlobalSettings(const std::string& p2ptun_path,
                                              const std::string& default_secret,
                                              const std::string& default_proto,
                                              int default_bw,
                                              bool minimize_to_tray,
                                              std::string& out_err) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!Trim(p2ptun_path).empty()) cfg_.p2ptun_path = Trim(p2ptun_path);
    if (!Trim(default_secret).empty()) cfg_.default_secret = Trim(default_secret);
    if (!Trim(default_proto).empty()) cfg_.default_proto = NormalizeProto(default_proto);
    if (default_bw > 0) cfg_.default_bandwidth = default_bw;
    cfg_.minimize_to_tray = minimize_to_tray;
    bool ok = SaveConfigLocked(out_err);
    if (ok) {
        logs_.Append("设置", "已保存全局配置 (默认协议=" + cfg_.default_proto + ", 默认带宽=" + std::to_string(cfg_.default_bandwidth) + "Mbps)");
    }
    return ok;
}

bool MultiTunnelManager::ReloadConfigFromDisk(std::string& out_err) {
    AppConfig loaded;
    if (!LoadAppConfigFromFile(config_path_, loaded, out_err)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        cfg_ = std::move(loaded);
        RebuildInstancesLocked();
    }
    logs_.Append("系统", "已从磁盘重新加载配置文件: " + config_path_);
    StartAutoEnabled();
    return true;
}

bool MultiTunnelManager::UpdateAdvancedDefaults(const AppConfig& advanced_part, std::string& out_err) {
    std::lock_guard<std::mutex> lock(mu_);
    cfg_.adv_broker = advanced_part.adv_broker;
    cfg_.adv_stun = advanced_part.adv_stun;
    cfg_.adv_turn = advanced_part.adv_turn;
    cfg_.adv_proxy = advanced_part.adv_proxy;
    cfg_.adv_peer_key = advanced_part.adv_peer_key;
    cfg_.adv_keepalive = advanced_part.adv_keepalive;
    cfg_.adv_ice_ports = advanced_part.adv_ice_ports;
    cfg_.adv_nat1to1 = advanced_part.adv_nat1to1;
    cfg_.adv_idle_timeout = advanced_part.adv_idle_timeout;
    cfg_.adv_max_peers = advanced_part.adv_max_peers;
    cfg_.adv_force_relay = advanced_part.adv_force_relay;
    cfg_.adv_keep_proxy = advanced_part.adv_keep_proxy;
    bool ok = SaveConfigLocked(out_err);
    if (ok) {
        logs_.Append("设置", "已保存全局高级参数默认值");
    }
    return ok;
}

bool MultiTunnelManager::ExportConfig(const std::string& target_path, std::string& out_err) const {
    AppConfig cfg;
    {
        std::lock_guard<std::mutex> lock(mu_);
        cfg = cfg_;
    }
    return SaveAppConfigToFile(target_path, cfg, out_err);
}

bool MultiTunnelManager::AddOrUpdateRule(const TunnelRule& raw_rule, bool auto_start, std::string& out_id, std::string& out_err) {
    std::string target_id;
    {
        std::lock_guard<std::mutex> lock(mu_);
        TunnelRule nr = NormalizeRule(raw_rule, raw_rule.mode, cfg_.default_proto, cfg_.default_bandwidth);
        if (auto_start) nr.enabled = true;
        target_id = nr.id;
        out_id = target_id;

        std::shared_ptr<ManagedInstance> found;
        for (auto& inst : instances_) {
            if (inst->rule.id == nr.id) {
                found = inst;
                break;
            }
        }
        if (!found) {
            auto inst = std::make_shared<ManagedInstance>();
            inst->rule = nr;
            inst->status_port = AllocateStatusPortLocked();
            instances_.push_back(inst);
        } else {
            StopInstanceInternal(*found);
            {
                std::lock_guard<std::mutex> ilock(found->mu);
                found->rule = nr;
            }
        }
        if (!SaveConfigLocked(out_err)) {
            return false;
        }
        logs_.Append(nr.remark, "已保存规则 (" + nr.mode + " " + nr.name + " <-> " + nr.port + ")");
    }
    if (auto_start) {
        return StartTunnel(target_id, out_err);
    }
    return true;
}

bool MultiTunnelManager::DeleteRule(const std::string& id, std::string& out_err) {
    std::shared_ptr<ManagedInstance> removed;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto it = instances_.begin(); it != instances_.end(); ++it) {
            if ((*it)->rule.id == id) {
                removed = *it;
                instances_.erase(it);
                break;
            }
        }
        if (!removed) {
            out_err = "未找到指定的隧道规则";
            return false;
        }
    }
    StopInstanceInternal(*removed);
    {
        std::lock_guard<std::mutex> lock(mu_);
        SaveConfigLocked(out_err);
    }
    logs_.Append(removed->rule.remark, "已删除隧道规则");
    return true;
}

bool MultiTunnelManager::ImportFromPairingCode(const std::string& raw_code, std::string& out_id, std::string& out_err) {
    TunnelRule r;
    if (!ParsePairingInput(raw_code, r, out_err)) {
        return false;
    }
    return AddOrUpdateRule(r, true, out_id, out_err);
}

bool MultiTunnelManager::StartInstanceLocked(ManagedInstance& inst, std::string& out_err) {
    std::lock_guard<std::mutex> ilock(inst.mu);
    if (inst.running) return true;

    inst.rule.enabled = true;
    inst.last_start_error.clear();
    inst.last_snap = StatusSnapshot{};
    inst.prev_tx = 0;
    inst.prev_rx = 0;
    inst.tx_rate_bps = 0;
    inst.rx_rate_bps = 0;
    inst.last_rate_time = std::chrono::steady_clock::now();

    std::string secret = EffectiveSecret(cfg_);
    if (secret.empty()) {
        out_err = "配对密钥不能为空，请在全局设置或当前规则中填写密钥";
        inst.last_start_error = out_err;
        return false;
    }

#ifdef _WIN32
    std::string exe_path = ResolveP2PTunExecutableLocked();
    auto args = BuildSubprocessArgs(inst.rule, cfg_, inst.status_port);

    std::wstring cmdline = QuoteWin32Arg(Utf8ToWideStr(exe_path));
    for (const auto& a : args) {
        cmdline += L" " + QuoteWin32Arg(Utf8ToWideStr(a));
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE hReadPipe = nullptr;
    HANDLE hWritePipe = nullptr;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
        out_err = "创建日志管道失败";
        inst.last_start_error = out_err;
        return false;
    }
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> env_block = BuildChildEnvironmentBlock(secret);
    std::vector<wchar_t> cmd_buf(cmdline.begin(), cmdline.end());
    cmd_buf.push_back(L'\0');

    BOOL created = CreateProcessW(
        nullptr,
        cmd_buf.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
        env_block.data(),
        nullptr,
        &si,
        &pi);

    CloseHandle(hWritePipe);

    if (!created) {
        CloseHandle(hReadPipe);
        DWORD err_code = GetLastError();
        std::ostringstream oss;
        oss << "无法启动核心程序 (" << exe_path << ", 错误码=" << err_code
            << ")。请确认 p2ptun.exe 已放在本程序同级目录！";
        out_err = oss.str();
        inst.last_start_error = out_err;
        logs_.Append(inst.rule.remark, "❌ " + out_err);
        return false;
    }

    if (job_handle_) {
        AssignProcessToJobObject(static_cast<HANDLE>(job_handle_), pi.hProcess);
    }
    CloseHandle(pi.hThread);

    inst.process_handle = pi.hProcess;
    inst.pid = pi.dwProcessId;
    inst.running = true;
    inst.rule.last_role = inst.rule.mode; // 记住成功角色, 重启后恢复
    inst.last_start_error.clear();

    if (inst.log_thread.joinable()) {
        inst.log_thread.detach();
    }
    std::string prefix = inst.rule.remark.empty() ? inst.rule.name : inst.rule.remark;
    inst.log_thread = std::thread([this, hReadPipe, prefix]() {
        char buf[1024];
        std::string pending;
        DWORD bytes_read = 0;
        while (ReadFile(hReadPipe, buf, sizeof(buf) - 1, &bytes_read, nullptr) && bytes_read > 0) {
            buf[bytes_read] = '\0';
            pending.append(buf, bytes_read);
            size_t pos = 0;
            while ((pos = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, pos);
                pending.erase(0, pos + 1);
                logs_.Append(prefix, line);
            }
        }
        if (!pending.empty()) {
            logs_.Append(prefix, pending);
        }
        CloseHandle(hReadPipe);
    });

    inst.watchdog_restarts = 0;

    logs_.Append(prefix, "▶ 隧道已启动 (PID=" + std::to_string(inst.pid) + ", 方向=" + inst.rule.mode +
                         ", 隧道名=" + inst.rule.name + ", 端口=" + inst.rule.port + ")");
    return true;
#else
    inst.running = true;
    inst.pid = 9999;
    logs_.Append(inst.rule.remark, "▶ 隧道已启动 (模拟模式)");
    return true;
#endif
}

void MultiTunnelManager::StopInstanceInternal(ManagedInstance& inst) {
    int status_port = 0;
    void* proc_handle = nullptr;
    std::string remark;
    {
        std::lock_guard<std::mutex> ilock(inst.mu);
        if (!inst.running && !inst.process_handle) {
            return;
        }
        status_port = inst.status_port;
        proc_handle = inst.process_handle;
        inst.process_handle = nullptr;
        inst.running = false;
        inst.pid = 0;
        inst.tx_rate_bps = 0;
        inst.rx_rate_bps = 0;
        inst.last_snap = StatusSnapshot{};
        remark = inst.rule.remark;
        inst.watchdog_restarts = 0;
    }

#ifdef _WIN32
    if (status_port > 0) {
        std::string resp;
        HttpLocalRequest(status_port, L"POST", L"/quit", resp);
    }
    if (proc_handle) {
        HANDLE hProc = static_cast<HANDLE>(proc_handle);
        if (WaitForSingleObject(hProc, 600) != WAIT_OBJECT_0) {
            TerminateProcess(hProc, 0);
            WaitForSingleObject(hProc, 400);
        }
        CloseHandle(hProc);
    }
#else
    (void)status_port;
    (void)proc_handle;
#endif
    if (inst.log_thread.joinable()) {
        inst.log_thread.detach();
    }
    logs_.Append(remark, "⏹ 隧道已停止");
}

bool MultiTunnelManager::StartTunnel(const std::string& id, std::string& out_err) {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& inst : instances_) {
        if (inst->rule.id == id) {
            bool ok = StartInstanceLocked(*inst, out_err);
            std::string save_err;
            SaveConfigLocked(save_err);
            return ok;
        }
    }
    out_err = "未找到隧道: " + id;
    return false;
}

bool MultiTunnelManager::StopTunnel(const std::string& id, std::string& out_err) {
    std::shared_ptr<ManagedInstance> target;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& inst : instances_) {
            if (inst->rule.id == id) {
                target = inst;
                break;
            }
        }
    }
    if (!target) {
        out_err = "未找到隧道: " + id;
        return false;
    }
    StopInstanceInternal(*target);
    {
        std::lock_guard<std::mutex> lock(mu_);
        {
            std::lock_guard<std::mutex> ilock(target->mu);
            target->rule.enabled = false;
            target->last_start_error.clear();
        }
        SaveConfigLocked(out_err);
    }
    return true;
}

bool MultiTunnelManager::ToggleTunnel(const std::string& id, std::string& out_err) {
    bool is_running = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& inst : instances_) {
            if (inst->rule.id == id) {
                std::lock_guard<std::mutex> ilock(inst->mu);
                is_running = inst->running;
                break;
            }
        }
    }
    if (is_running) {
        return StopTunnel(id, out_err);
    }
    return StartTunnel(id, out_err);
}

bool MultiTunnelManager::ReconnectTunnel(const std::string& id, std::string& out_err) {
    int status_port = 0;
    bool running = false;
    std::string remark;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& inst : instances_) {
            if (inst->rule.id == id) {
                std::lock_guard<std::mutex> ilock(inst->mu);
                running = inst->running;
                status_port = inst->status_port;
                remark = inst->rule.remark;
                break;
            }
        }
    }
    if (!running) {
        return StartTunnel(id, out_err);
    }
#ifdef _WIN32
    std::string body;
    if (HttpLocalRequest(status_port, L"POST", L"/reconnect", body)) {
        logs_.Append(remark, "🔄 已触发快速重连");
        return true;
    }
#else
    (void)status_port;
#endif
    out_err = "触发重连失败，正在重启该隧道...";
    StopTunnel(id, out_err);
    return StartTunnel(id, out_err);
}

void MultiTunnelManager::StartAutoEnabled() {
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (const auto& inst : instances_) {
            std::lock_guard<std::mutex> ilock(inst->mu);
            if (inst->rule.enabled && !inst->running) {
                ids.push_back(inst->rule.id);
            }
        }
    }
    for (const auto& id : ids) {
        std::string err;
        StartTunnel(id, err);
    }
}

void MultiTunnelManager::StartAll() {
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (const auto& inst : instances_) {
            ids.push_back(inst->rule.id);
        }
    }
    for (const auto& id : ids) {
        std::string err;
        StartTunnel(id, err);
    }
}

void MultiTunnelManager::StopAll(bool persist_disabled) {
    std::vector<std::shared_ptr<ManagedInstance>> copy;
    {
        std::lock_guard<std::mutex> lock(mu_);
        copy = instances_;
    }
    for (auto& inst : copy) {
        StopInstanceInternal(*inst);
        if (persist_disabled) {
            std::lock_guard<std::mutex> ilock(inst->mu);
            inst->rule.enabled = false;
        }
    }
    if (persist_disabled) {
        std::lock_guard<std::mutex> lock(mu_);
        std::string err;
        SaveConfigLocked(err);
    }
}

void MultiTunnelManager::PollAllStatuses() {
    std::vector<std::shared_ptr<ManagedInstance>> copy;
    {
        std::lock_guard<std::mutex> lock(mu_);
        copy = instances_;
    }

    auto now = std::chrono::steady_clock::now();
    for (auto& inst : copy) {
        int port = 0;
        bool running = false;
        void* proc_handle = nullptr;
        {
            std::lock_guard<std::mutex> ilock(inst->mu);
            running = inst->running;
            port = inst->status_port;
            proc_handle = inst->process_handle;
        }
        if (!running) continue;

#ifdef _WIN32
        bool exited = false;
        DWORD exit_code = STILL_ACTIVE;
        bool want_restart = false;
        std::string rid;
        std::string rremark;
        if (proc_handle) {
            if (GetExitCodeProcess(static_cast<HANDLE>(proc_handle), &exit_code) && exit_code != STILL_ACTIVE) {
                {
                    std::lock_guard<std::mutex> ilock(inst->mu);
                    CloseHandle(static_cast<HANDLE>(inst->process_handle));
                    inst->process_handle = nullptr;
                    inst->running = false;
                    inst->pid = 0;
                    inst->tx_rate_bps = 0;
                    inst->rx_rate_bps = 0;
                    exited = true;
                    want_restart = inst->rule.auto_restart;
                    rid = inst->rule.id;
                    rremark = inst->rule.remark;
                }
                if (want_restart && inst->watchdog_restarts < 5) {
                    inst->watchdog_restarts++;
                    logs_.Append(rremark, "🔄 看门狗: 进程异常退出 (退出码 " + std::to_string(exit_code) +
                                              ")，2 秒后自动重启 (第 " + std::to_string(inst->watchdog_restarts) + " 次)");
                    {
                        std::lock_guard<std::mutex> ilock(inst->mu);
                        inst->last_start_error = "看门狗守护重启中 (第 " + std::to_string(inst->watchdog_restarts) + " 次)";
                    }
                    restart_queue_.push_back(rid);
                } else if (!want_restart) {
                    std::lock_guard<std::mutex> ilock(inst->mu);
                    inst->last_start_error = "进程已退出 (退出码 " + std::to_string(exit_code) + ")，请查看底部日志";
                } else {
                    std::lock_guard<std::mutex> ilock(inst->mu);
                    inst->last_start_error = "连续退出超过 5 次，看门狗已停止重启，请检查配置或网络";
                }
                continue;
            }
        }
        (void)exited;

        std::string body;
        if (HttpLocalRequest(port, L"GET", L"/status", body)) {
            StatusSnapshot snap;
            if (ParseStatusSnapshotJson(body, snap)) {
                std::lock_guard<std::mutex> ilock(inst->mu);
                double elapsed = std::chrono::duration<double>(now - inst->last_rate_time).count();
                if (elapsed >= 0.4) {
                    inst->tx_rate_bps = (snap.bytes_tx >= inst->prev_tx)
                                            ? static_cast<uint64_t>((snap.bytes_tx - inst->prev_tx) / elapsed)
                                            : 0;
                    inst->rx_rate_bps = (snap.bytes_rx >= inst->prev_rx)
                                            ? static_cast<uint64_t>((snap.bytes_rx - inst->prev_rx) / elapsed)
                                            : 0;
                    inst->prev_tx = snap.bytes_tx;
                    inst->prev_rx = snap.bytes_rx;
                    inst->last_rate_time = now;
                }
                inst->last_snap = std::move(snap);
            }
        }
#else
        (void)port;
        (void)proc_handle;
        (void)now;
#endif
    }

    // 看门狗延迟重启 (等待 2 秒让端口释放，避免立即重启撞上 TIME_WAIT/占用)
    if (!restart_queue_.empty()) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        std::vector<std::string> ids;
        ids.swap(restart_queue_);
        for (const auto& rid : ids) {
            std::string err;
            StartTunnel(rid, err); // rule.auto_restart 已校验过；此调用会重新保存 enabled 状态
        }
    }
}

std::vector<TunnelRowView> MultiTunnelManager::GetRowViews() const {
    std::vector<std::shared_ptr<ManagedInstance>> copy;
    std::string def_secret;
    {
        std::lock_guard<std::mutex> lock(mu_);
        copy = instances_;
        def_secret = cfg_.default_secret;
    }

    std::vector<TunnelRowView> rows;
    rows.reserve(copy.size());

    int idx = 1;
    for (const auto& inst : copy) {
        std::lock_guard<std::mutex> ilock(inst->mu);
        const TunnelRule& r = inst->rule;
        const StatusSnapshot& snap = inst->last_snap;

        TunnelRowView v;
        v.index = idx++;
        v.id = r.id;
        v.mode = r.mode;
        v.mode_label = (r.mode == "connect") ? "📥 Connect (连接远程)" : "📤 Expose (暴露本机)";
        v.remark = r.remark;
        v.name = r.name;
        v.port = r.port;
        v.port_short = ExtractPortOnly(r.port);
        v.proto_bw_text = ToLower(r.proto) + " (" + std::to_string(r.bandwidth_mbps) + "M)";
        v.running = inst->running;
        v.pairing_uri = BuildPairingUri(r, def_secret);
        v.cli_command = BuildCliCommand(r, def_secret);
        v.rule = r;

        if (!inst->running) {
            if (!inst->last_start_error.empty()) {
                v.status_text = "🔴 异常退出";
                v.detail_text = inst->last_start_error;
            } else {
                v.status_text = "⚪ 已停止";
                v.detail_text = "双击此行或点击 [▶ 启动选中] 开启";
            }
            v.traffic_text = "↑ 0 B | ↓ 0 B";
        } else {
            if (!snap.reachable) {
                v.status_text = "🟡 正在初始化...";
                v.detail_text = "正在启动 p2ptun 核心并探测 DNS...";
            } else if (r.mode == "expose") {
                if (snap.active_peers > 0) {
                    v.status_text = "🟢 在线 (" + std::to_string(snap.active_peers) + "连入)";
                } else if (snap.signaling_up) {
                    v.status_text = "🟢 待命 (已注册信令)";
                } else {
                    v.status_text = "🟡 连接信令中...";
                }
            } else {
                if (snap.active_peers > 0) {
                    v.status_text = "🟢 已直连 (P2P)";
                } else if (r.on_demand && !snap.signaling_up) {
                    v.status_text = "🔵 按需监听中";
                } else if (snap.signaling_up) {
                    v.status_text = "🟡 正在打洞握手...";
                } else {
                    v.status_text = "🟡 连接信令中...";
                }
            }

            if (inst->tx_rate_bps > 0 || inst->rx_rate_bps > 0) {
                v.traffic_text = "↑" + FormatBytes(inst->tx_rate_bps) + "/s ↓" + FormatBytes(inst->rx_rate_bps) +
                                 "/s (总" + FormatBytes(snap.bytes_tx + snap.bytes_rx) + ")";
            } else {
                v.traffic_text = "↑" + FormatBytes(snap.bytes_tx) + " ↓" + FormatBytes(snap.bytes_rx);
            }

            std::vector<std::string> details;
            if (snap.last_rtt_ms > 0) {
                details.push_back(std::to_string(snap.last_rtt_ms) + "ms");
            }
            if (!snap.udp_endpoint.empty()) {
                details.push_back("UDP:" + snap.udp_endpoint);
            }
            if (snap.active_streams > 0) {
                details.push_back(std::to_string(snap.active_streams) + "并发流");
            }
            if (!snap.last_error.empty() && !snap.signaling_up && snap.active_peers == 0) {
                details.push_back(snap.last_error);
            }
            if (details.empty() && snap.reachable) {
                details.push_back("PID:" + std::to_string(snap.pid) + " | 在线:" + snap.uptime);
            }
            std::ostringstream dss;
            for (size_t i = 0; i < details.size(); ++i) {
                if (i > 0) dss << " | ";
                dss << details[i];
            }
            if (!dss.str().empty()) {
                v.detail_text = dss.str();
            }
        }

        rows.push_back(std::move(v));
    }
    return rows;
}

bool MultiTunnelManager::LaunchClientForTunnel(const std::string& id, std::string& out_err) const {
    TunnelRule rule;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (const auto& inst : instances_) {
            std::lock_guard<std::mutex> ilock(inst->mu);
            if (inst->rule.id == id) {
                rule = inst->rule;
                found = true;
                break;
            }
        }
    }
    if (!found) {
        out_err = "请先在列表中选中一条隧道";
        return false;
    }

    std::string ep = NormalizeEndpoint(rule.port, "1080");
    std::string port = ExtractPortOnly(ep);
    std::string hint = ToLower(rule.remark + " " + rule.name);

#ifdef _WIN32
    if (port == "3389" || port == "13389" || hint.find("rdp") != std::string::npos || rule.remark.find("桌面") != std::string::npos) {
        std::wstring arg = L"/v:" + Utf8ToWideStr(ep);
        ShellExecuteW(nullptr, L"open", L"mstsc.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
        return true;
    }
    if (port == "22" || port == "2222" || hint.find("ssh") != std::string::npos) {
        std::wstring arg = L"/k ssh -p " + Utf8ToWideStr(port) + L" 127.0.0.1";
        ShellExecuteW(nullptr, L"open", L"cmd.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
        return true;
    }
    std::wstring url = L"http://" + Utf8ToWideStr(ep);
    ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return true;
#else
    (void)ep;
    (void)port;
    (void)hint;
    return true;
#endif
}


} // namespace p2ptun
