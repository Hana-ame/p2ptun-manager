#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace p2ptun {

// 单条隧道配置规则 (同时支持 Expose 暴露本机端口 与 Connect 连接远程隧道到本地端口)
// 设计原则：极致傻瓜化。用户只需关心 name (隧道名) + port (本地端口)，其余自动补全或继承全局配置。
struct TunnelRule {
    std::string id;                 // 规则唯一标识 (为空时自动生成)
    std::string mode = "expose";    // 当前运行角色: "expose" (Server) 或 "connect" (Client)
    std::string last_role = "";     // 上次启动成功的角色 (持久化，重启后恢复；空=从未运行)
    std::string remark;             // 备注说明，如 "家里远程桌面 RDP"
    std::string name;               // 隧道名称 (Expose 与 Connect 填相同的名字即可配对)
    std::string port;               // 端口或地址，支持直接填 "3389"、":3389" 或 "127.0.0.1:3389"
    std::string proto = "hy2";      // 传输协议: "hy2" (默认极速 Brutal CC), "quic", "classic"
    int bandwidth_mbps = 100;       // Hysteria 2 Brutal CC 带宽上限 (Mbps)
    bool on_demand = false;         // Connect 按需连接模式
    bool enabled = false;           // 是否在启动时自动运行

    // ---- 高级穿透参数 (合并自 C# 全参数版；留空/0 = 不传该 flag，由 p2ptun 使用内置默认) ----
    std::string broker;             // -broker    PeerJS 信令服务器列表 (逗号分隔)
    std::string stun;               // -stun      STUN 列表 (逗号分隔)
    std::string turn;               // -turn      TURN 中继 (user:pass@host:3478?transport=udp)
    std::string proxy;              // -proxy     信令 WebSocket HTTP(S) 代理地址
    std::string peer_key;           // -key       PeerJS API Key (默认 peerjs)
    std::string keepalive;          // -keepalive 隧道心跳间隔 (如 "15s")
    std::string ice_ports;          // -ice-ports ICE UDP 端口范围 (如 "40000-40100")
    std::string nat1to1;            // -nat1to1   1:1 NAT 外部公网 IP
    std::string idle_timeout;      // -idle-timeout Connect 按需模式空闲休眠时间 (如 "10m")
    bool keep_proxy = false;        // -keep-proxy 保留系统环境代理供信令使用
    bool force_relay = false;       // -force-relay 强制 TURN 中继模式
    int max_peers = 0;              // -max-peers Expose 最大并发客户端 (0 = p2ptun 默认 16)
    bool auto_restart = true;      // 进程异常退出时守护重启
};

// 全局配置文件结构 (对应 exe 同级目录的 config.json)
struct AppConfig {
    std::string p2ptun_path = "p2ptun.exe"; // p2ptun 核心可执行文件路径
    std::string default_secret = "p2p-123456";
    std::string default_proto = "hy2";
    int default_bandwidth = 100;
    bool minimize_to_tray = true;           // 点击关闭按钮时最小化到系统托盘
    std::vector<TunnelRule> expose;         // 兼容旧格式: Server 角色规则
    std::vector<TunnelRule> connect;        // 兼容旧格式: Client 角色规则
    std::vector<TunnelRule> tunnels;        // 新格式: 角色无关的统一隧道配置 (运行时自选 expose/connect)

    // ---- 全局高级参数默认值 (单规则同名字段留空时生效) ----
    std::string adv_broker;                // -broker
    std::string adv_stun;                  // -stun
    std::string adv_turn;                   // -turn
    std::string adv_proxy;                 // -proxy
    std::string adv_peer_key;              // -key
    std::string adv_keepalive;             // -keepalive
    std::string adv_ice_ports;             // -ice-ports
    std::string adv_nat1to1;               // -nat1to1
    std::string adv_idle_timeout;          // -idle-timeout
    int adv_max_peers = 0;                 // -max-peers (0 = p2ptun 默认 16)
    bool adv_force_relay = false;          // -force-relay
    bool adv_keep_proxy = false;           // -keep-proxy
};


// 对应 p2ptun `GET /status` 返回的运行期状态快照
struct StatusSnapshot {
    bool reachable = false;
    std::string version;
    int pid = 0;
    std::string mode;
    std::string peer_id;
    std::string remote_peer;
    std::string endpoint;
    std::string proto;
    std::string udp_endpoint;
    std::string broker;
    int64_t uptime_sec = 0;
    std::string uptime;
    bool signaling_up = false;
    int64_t active_peers = 0;
    int64_t active_streams = 0;
    uint64_t reconnects = 0;
    uint64_t bytes_tx = 0;
    uint64_t bytes_rx = 0;
    int64_t last_rtt_ms = 0;
    std::string last_error;
};

// 常用场景傻瓜预设模板
struct ScenePreset {
    std::string label;
    std::string remark_expose;
    std::string remark_connect;
    std::string default_name;
    std::string expose_port;
    std::string connect_port;
};

// 获取内置常用端口预设列表
std::vector<ScenePreset> GetScenePresets();

// 字符串与端口规范化工具
std::string Trim(const std::string& s);
std::string ToLower(const std::string& s);
std::string NormalizeProto(const std::string& raw);

// 将用户输入的纯端口 (如 "3389")、":3389" 或 "127.0.0.1:3389" 统一规范化为 "host:port"
std::string NormalizeEndpoint(const std::string& raw, const std::string& fallback_port = "3389");

// 提取 "127.0.0.1:3389" 中的纯端口号 "3389"
std::string ExtractPortOnly(const std::string& endpoint);

// 当复制 Expose 规则给另一台 Windows 电脑 Connect 时，自动避开系统保留端口 (如 3389 -> 13389, 22 -> 2222)
std::string SuggestConnectPort(const std::string& expose_endpoint);

// SuggestConnectPort 的反向映射 (Client -> Server 切换时把 13389 还原回 3389)
std::string SuggestExposePort(const std::string& connect_endpoint);

// 规范化单条规则与整体配置
TunnelRule NormalizeRule(TunnelRule rule, const std::string& forced_mode, const std::string& def_proto, int def_bw);
void NormalizeAppConfig(AppConfig& cfg);
AppConfig CreateDefaultAppConfig();

// 配置文件 JSON 序列化与反序列化 (零外部依赖)
std::string SerializeAppConfigToJson(const AppConfig& cfg);
bool ParseAppConfigFromJson(const std::string& json_text, AppConfig& out_cfg, std::string& out_err);
bool LoadAppConfigFromFile(const std::string& path, AppConfig& out_cfg, std::string& out_err);
bool SaveAppConfigToFile(const std::string& path, const AppConfig& cfg, std::string& out_err);

// 解析 p2ptun /status 接口返回的 JSON
bool ParseStatusSnapshotJson(const std::string& json_text, StatusSnapshot& out_snap);

// 一键配对码 (p2ptun://) 与 CLI 命令生成和智能解析
std::string BuildPairingUri(const TunnelRule& rule, const std::string& default_secret);
std::string BuildCliCommand(const TunnelRule& rule, const std::string& default_secret);
bool ParsePairingInput(const std::string& raw_input, TunnelRule& out_rule, std::string& out_err);

// 构造启动子进程 p2ptun.exe 所需的命令行参数列表 (不含明文密钥，密钥通过 P2PTUN_SECRET 环境变量注入)
std::vector<std::string> BuildSubprocessArgs(const TunnelRule& rule, const AppConfig& global_cfg, int status_port);
std::string EffectiveSecret(const AppConfig& global_cfg);

// 流量字节数可读格式化 (如 "12.5 MB", "850.0 KB/s")
std::string FormatBytes(uint64_t bytes);

} // namespace p2ptun
