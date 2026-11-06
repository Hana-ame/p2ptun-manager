#pragma once

#include "core.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace p2ptun {

// 线程安全环形日志缓冲器，汇总所有 Expose / Connect 隧道的实时输出
class LogBuffer {
public:
    explicit LogBuffer(size_t max_lines = 500);
    void Append(const std::string& prefix, const std::string& text);
    void Clear();
    uint64_t Seq() const;
    std::string GetText(size_t tail_lines = 250, const std::string& line_sep = "\r\n") const;

private:
    mutable std::mutex mu_;
    std::vector<std::string> lines_;
    size_t max_lines_;
    std::atomic<uint64_t> seq_{0};
};

// GUI 列表单行展示快照
struct TunnelRowView {
    int index = 0;
    std::string id;
    std::string mode;
    std::string mode_label;
    std::string remark;
    std::string name;
    std::string port;
    std::string port_short;
    std::string proto_bw_text;
    bool running = false;
    std::string status_text;
    std::string traffic_text;
    std::string detail_text;
    std::string pairing_uri;
    std::string cli_command;
    TunnelRule rule;
};

// 多端口隧道进程与状态管理器 (同时管理多个 Expose 与多个 Connect 实例)
class MultiTunnelManager {
public:
    explicit MultiTunnelManager(const std::string& config_path);
    ~MultiTunnelManager();

    MultiTunnelManager(const MultiTunnelManager&) = delete;
    MultiTunnelManager& operator=(const MultiTunnelManager&) = delete;

    const std::string& ConfigPath() const { return config_path_; }
    AppConfig GetConfig() const;
    LogBuffer& Logs() { return logs_; }

    // 全局配置更新与持久化
    bool UpdateGlobalSettings(const std::string& p2ptun_path,
                              const std::string& default_secret,
                              const std::string& default_proto,
                              int default_bw,
                              bool minimize_to_tray,
                              std::string& out_err);

    // 重新从磁盘加载 config.json
    bool ReloadConfigFromDisk(std::string& out_err);

    // 更新全局高级参数默认值并持久化 (GUI ①区 "高级默认" 框)
    bool UpdateAdvancedDefaults(const AppConfig& advanced_part, std::string& out_err);

    // 批量导出完整 config.json (全局设置 + 全部规则)
    bool ExportConfig(const std::string& target_path, std::string& out_err) const;

    // 隧道规则增删改
    bool AddOrUpdateRule(const TunnelRule& rule, bool auto_start, std::string& out_id, std::string& out_err);
    bool DeleteRule(const std::string& id, std::string& out_err);
    bool ImportFromPairingCode(const std::string& raw_code, std::string& out_id, std::string& out_err);

    // 单隧道与批量启停控制
    bool StartTunnel(const std::string& id, std::string& out_err);
    bool StopTunnel(const std::string& id, std::string& out_err);
    bool ToggleTunnel(const std::string& id, std::string& out_err);
    bool ReconnectTunnel(const std::string& id, std::string& out_err);

    void StartAutoEnabled();
    void StartAll();
    void StopAll(bool persist_disabled = false);

    // 后台轮询所有运行中隧道的 /status 接口并更新速率
    void PollAllStatuses();

    // 获取用于 GUI 表格渲染的快照列表
    std::vector<TunnelRowView> GetRowViews() const;

    // 根据选中行智能打开对应的客户端 (mstsc 远程桌面 / 浏览器 / SSH 终端)
    bool LaunchClientForTunnel(const std::string& id, std::string& out_err) const;

private:
    struct ManagedInstance {
        mutable std::mutex mu;
        TunnelRule rule;
        int status_port = 0;
        bool running = false;
        uint32_t pid = 0;
        void* process_handle = nullptr; // HANDLE on Windows
        std::thread log_thread;
        std::string last_start_error;
        StatusSnapshot last_snap;
        int watchdog_restarts = 0;                     // 看门狗守护重启计数 (auto_restart)
        uint64_t prev_tx = 0;
        uint64_t prev_rx = 0;
        uint64_t tx_rate_bps = 0;
        uint64_t rx_rate_bps = 0;
        std::chrono::steady_clock::time_point last_rate_time{};
    };

    void RebuildInstancesLocked();
    void SyncConfigFromInstancesLocked();
    bool SaveConfigLocked(std::string& out_err);
    int AllocateStatusPortLocked();
    std::string ResolveP2PTunExecutableLocked() const;

    bool StartInstanceLocked(ManagedInstance& inst, std::string& out_err);
    void StopInstanceInternal(ManagedInstance& inst);

    std::string config_path_;
    mutable std::mutex mu_;
    AppConfig cfg_;
    std::vector<std::shared_ptr<ManagedInstance>> instances_;
    int next_status_port_ = 18300;
    void* job_handle_ = nullptr; // Windows JobObject 句柄 (确保主程序退出时自动清理所有子进程)
    LogBuffer logs_;
    std::vector<std::string> restart_queue_; // 看门狗待重启的隧道 id (由 PollAllStatuses 填充)
};

} // namespace p2ptun
