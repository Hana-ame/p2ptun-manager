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

// ---------------- 串口桥接 Win32 原生实现 (移植自 C# SerialTcpBridge，零外部依赖) ----------------

struct SerialNative {
    HANDLE com = nullptr;
    OVERLAPPED io{};
};

void LogTo(LogBuffer& logs, const std::string& prefix, const std::string& msg) {
    logs.Append(prefix, msg);
}

bool ParseHostPort(const std::string& ep, std::string& host, int& port) {
    auto pos = ep.rfind(':');
    if (pos == std::string::npos) return false;
    host = p2ptun::Trim(ep.substr(0, pos));
    std::string ps = p2ptun::Trim(ep.substr(pos + 1));
    if (host.empty() || ps.empty()) return false;
    if (host == "localhost") host = "127.0.0.1";
    port = std::atoi(ps.c_str());
    return port > 0 && port <= 65535;
}

SOCKET TcpConnectBlocking(const std::string& host, int port, int timeout_ms) {
    struct sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(static_cast<unsigned short>(port));
    inet_pton(AF_INET, host.c_str(), &sa.sin_addr);
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    // 非阻塞连接 + 超时，避免串口桥接线程卡死
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    connect(s, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa));
    fd_set wset;
    FD_ZERO(&wset);
    FD_SET(s, &wset);
    struct timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (select(0, nullptr, &wset, nullptr, &tv) <= 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);
    return s;
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

    for (const auto& r : cfg_.expose) appendRule(r);
    for (const auto& r : cfg_.connect) appendRule(r);
    instances_ = std::move(next_list);
}

void MultiTunnelManager::SyncConfigFromInstancesLocked() {
    cfg_.expose.clear();
    cfg_.connect.clear();
    for (const auto& inst : instances_) {
        std::lock_guard<std::mutex> ilock(inst->mu);
        if (inst->rule.mode == "connect") {
            cfg_.connect.push_back(inst->rule);
        } else {
            cfg_.expose.push_back(inst->rule);
        }
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

    std::string secret = EffectiveSecret(inst.rule, cfg_);
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

    // 串口桥接：进程启动成功后打开 (合并自 C# SerialTcpBridge 的管理器托管模式)
    if (!Trim(inst.rule.serial_port).empty()) {
        inst.serial_bridge = std::make_shared<SerialTcpBridge>(inst.rule, logs_);
        std::string s_err;
        if (inst.serial_bridge->Start(s_err)) {
            // 桥接器持有串口; 正常运行
        } else {
            logs_.Append(prefix, "⚠ 串口桥接启动失败: " + s_err);
            inst.serial_bridge.reset();
        }
    }
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
    std::shared_ptr<SerialTcpBridge> bridge;
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
        bridge = inst.serial_bridge;
        inst.serial_bridge.reset();
        inst.watchdog_restarts = 0;
    }
    if (bridge) {
        bridge->Stop(); // 串口句柄与桥接线程在此回收
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
                std::shared_ptr<SerialTcpBridge> dead_bridge;
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
                    dead_bridge = inst->serial_bridge;
                    inst->serial_bridge.reset();
                }
                if (dead_bridge) {
                    dead_bridge->Stop();
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


// ============================================================================
// SerialTcpBridge — COM 串口 <-> TCP 双向透传桥接 (合并自 C# SerialTcpBridge)
// ============================================================================

SerialTcpBridge::SerialTcpBridge(const TunnelRule& rule, LogBuffer& logs)
    : rule_(rule), logs_(logs) {}

SerialTcpBridge::~SerialTcpBridge() {
    Stop();
}

bool SerialTcpBridge::ComPortExists(const std::string& com_name) {
#ifdef _WIN32
    std::string device = "\\\\.\\" + p2ptun::Trim(com_name);
    HANDLE h = CreateFileA(device.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    CloseHandle(h);
    return true;
#else
    (void)com_name;
    return false;
#endif
}

bool SerialTcpBridge::OpenComPort(std::string& out_err) {
#ifdef _WIN32
    std::string device = "\\\\.\\" + p2ptun::Trim(rule_.serial_port);
    com_handle_ = CreateFileA(device.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING, 0, nullptr);
    if (com_handle_ == INVALID_HANDLE_VALUE || com_handle_ == nullptr) {
        out_err = "无法打开串口 " + rule_.serial_port + " (错误码 " + std::to_string(GetLastError()) + ")";
        com_handle_ = nullptr;
        return false;
    }

    int databits = 8; char parity = 'N'; int stopbits = 1;
    p2ptun::ParseSerialParams(rule_.serial_params, databits, parity, stopbits);

    DCB dcb{};
    dcb.DCBlength = sizeof(DCB);
    if (!GetCommState(static_cast<HANDLE>(com_handle_), &dcb)) {
        out_err = "读取串口状态失败 (GetCommState)";
        CloseComPort();
        return false;
    }
    dcb.BaudRate = static_cast<DWORD>(rule_.serial_baud > 0 ? rule_.serial_baud : 115200);
    dcb.ByteSize = static_cast<BYTE>(databits);
    dcb.Parity = (parity == 'O') ? ODDPARITY : ((parity == 'E') ? EVENPARITY : NOPARITY);
    dcb.StopBits = (stopbits == 2) ? TWOSTOPBITS : ONESTOPBIT;
    if (!SetCommState(static_cast<HANDLE>(com_handle_), &dcb)) {
        out_err = "设置串口参数失败 (SetCommState)";
        CloseComPort();
        return false;
    }

    COMMTIMEOUTS timeouts{};
    timeouts.ReadIntervalTimeout = 20;
    timeouts.ReadTotalTimeoutMultiplier = 1;
    timeouts.ReadTotalTimeoutConstant = 50;
    timeouts.WriteTotalTimeoutMultiplier = 1;
    timeouts.WriteTotalTimeoutConstant = 50;
    SetCommTimeouts(static_cast<HANDLE>(com_handle_), &timeouts);
    return true;
#else
    (void)out_err;
    return false;
#endif
}

void SerialTcpBridge::CloseComPort() {
#ifdef _WIN32
    if (com_handle_) {
        CloseHandle(static_cast<HANDLE>(com_handle_));
        com_handle_ = nullptr;
    }
#endif
}

int SerialTcpBridge::ReadCom(char* buf, int len) {
#ifdef _WIN32
    if (!com_handle_) return 0;
    DWORD read_n = 0;
    if (!ReadFile(static_cast<HANDLE>(com_handle_), buf, static_cast<DWORD>(len), &read_n, nullptr)) {
        return -1;
    }
    return static_cast<int>(read_n);
#else
    (void)buf; (void)len;
    return 0;
#endif
}

bool SerialTcpBridge::WriteCom(const char* buf, int len) {
#ifdef _WIN32
    if (!com_handle_) return false;
    DWORD written = 0;
    return WriteFile(static_cast<HANDLE>(com_handle_), buf, static_cast<DWORD>(len), &written, nullptr) == TRUE;
#else
    (void)buf; (void)len;
    return false;
#endif
}

void SerialTcpBridge::ServerLoop(std::string host, int port) {
#ifdef _WIN32
    struct sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(static_cast<unsigned short>(port));
    inet_pton(AF_INET, host.c_str(), &sa.sin_addr);
    SOCKET lis = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (lis == INVALID_SOCKET) {
        LogTo(logs_, rule_.remark, "串口桥接: 创建监听 socket 失败");
        return;
    }
    int reuse = 1;
    setsockopt(lis, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    if (bind(lis, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) == SOCKET_ERROR ||
        listen(lis, 1) == SOCKET_ERROR) {
        LogTo(logs_, rule_.remark, "串口桥接: 监听 " + host + ":" + std::to_string(port) + " 失败 (端口被占用?)");
        closesocket(lis);
        return;
    }
    listen_sock_ = reinterpret_cast<void*>(lis);
    LogTo(logs_, rule_.remark, "串口桥接: Expose 模式监听 TCP " + host + ":" + std::to_string(port) +
                                   " <-> 串口 " + rule_.serial_port);

    while (!stopping_.load()) {
        SOCKET client = accept(lis, nullptr, nullptr);
        if (client == INVALID_SOCKET) break;
        LogTo(logs_, rule_.remark, "串口桥接: 收到 p2ptun 隧道连接");
        PumpBidirectional(reinterpret_cast<void*>(client));
    }
    closesocket(lis);
    listen_sock_ = nullptr;
#else
    (void)host; (void)port;
#endif
}

void SerialTcpBridge::ClientLoop(std::string host, int port) {
#ifdef _WIN32
    LogTo(logs_, rule_.remark, "串口桥接: Connect 模式尝试连入本地 p2ptun 监听端口 " + host + ":" + std::to_string(port));
    // 等待 p2ptun connect 进程先就绪
    for (int i = 0; i < 15 && !stopping_.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    while (!stopping_.load()) {
        SOCKET s = TcpConnectBlocking(host, port, 2000);
        if (s == INVALID_SOCKET) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            continue;
        }
        LogTo(logs_, rule_.remark, "串口桥接: 已桥接 " + rule_.serial_port + " <-> 本地端口 " + host + ":" + std::to_string(port));
        PumpBidirectional(reinterpret_cast<void*>(s));
        if (!stopping_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
    }
#else
    (void)host; (void)port;
#endif
}

void SerialTcpBridge::PumpBidirectional(void* sock_handle) {
#ifdef _WIN32
    SOCKET s = static_cast<SOCKET>(reinterpret_cast<uintptr_t>(sock_handle));
    char buf[2048];
    while (!stopping_.load()) {
        // TCP -> COM
        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(s, &rset);
        struct timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 100000; // 100ms
        if (select(0, &rset, nullptr, nullptr, &tv) > 0 && FD_ISSET(s, &rset)) {
            int n = recv(s, buf, sizeof(buf), 0);
            if (n <= 0) break;
            if (!WriteCom(buf, n)) break;
        }
        // COM -> TCP (ReadCom 受 COMMTIMEOUTS 约束最多阻塞 ~70ms)
        int m = ReadCom(buf, sizeof(buf));
        if (m > 0) {
            if (send(s, buf, m, 0) == SOCKET_ERROR) break;
        } else if (m < 0) {
            break;
        }
    }
    closesocket(s);
    LogTo(logs_, rule_.remark, "串口桥接: 桥接连接已断开");
#else
    (void)sock_handle;
#endif
}

bool SerialTcpBridge::Start(std::string& out_err) {
    if (p2ptun::Trim(rule_.serial_port).empty()) {
        out_err = "未配置串口号";
        return false;
    }
#ifdef _WIN32
    if (!OpenComPort(out_err)) {
        return false;
    }
    LogTo(logs_, rule_.remark, "串口桥接: 已打开串口 " + rule_.serial_port + " (" +
                                   std::to_string(rule_.serial_baud) + " bps, " + rule_.serial_params + ")");
    std::string ep = p2ptun::NormalizeEndpoint(rule_.port, "1080");
    std::string host;
    int port = 0;
    if (!ParseHostPort(ep, host, port)) {
        out_err = "串口桥接: 端口地址格式无效 " + ep;
        Stop();
        return false;
    }
    stopping_.store(false);
    pump_thread_ = std::thread([this, host, port]() {
        if (rule_.mode == "expose") {
            ServerLoop(host, port);
        } else {
            ClientLoop(host, port);
        }
    });
    return true;
#else
    out_err = "串口桥接仅支持 Windows";
    return false;
#endif
}

void SerialTcpBridge::Stop() {
    stopping_.store(true);
#ifdef _WIN32
    if (listen_sock_) {
        closesocket(static_cast<SOCKET>(reinterpret_cast<uintptr_t>(listen_sock_)));
        listen_sock_ = nullptr;
    }
    CloseComPort();
#endif
    if (pump_thread_.joinable()) {
        pump_thread_.join();
    }
}
} // namespace p2ptun
