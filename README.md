# p2ptun-manager (Windows 多端口穿透图形管理器 · C++17)

专为 [`Hana-ame/p2ptun`](https://github.com/Hana-ame/p2ptun) 打造的原生 **C++17 Windows 桌面管理器**。

**只管理、不修改 p2ptun 本体**：管理器以子进程方式拉起 `p2ptun.exe`，全部参数与控制接口均严格对齐上游 v0.5.2 的 flag 定义与 `GET /status` JSON 快照（见上游 `main.go` usage 与 `daemon.go statusSnapshot`）。

在一个原生窗口中**同时管理多个 Expose（暴露本机服务）与多个 Connect（连接远程隧道）实例**：傻瓜式一键预设、`p2ptun://` 配对码复制粘贴、全高级参数透传、内置 COM 串口桥接、看门狗守护重启与实时速率监控。

## ✨ 核心特性

- **原生 C++17 Win32，零依赖**：静态链接 CRT，单文件不足 300KB，任何 Windows 10/11 直接双击运行；不需要 .NET 运行时。
- **多端口并发管理**：任意数量的 Expose + Connect 混合并发；单条启停、双击行快速开关、全部启动 / 全部停止。
- **傻瓜化配置**：
  - 纯数字端口自动补全（`3389` → `127.0.0.1:3389`），全局默认密钥/协议/带宽自动继承。
  - 场景一键预设：RDP `3389`、SSH `22`、SOCKS5 `1080`、Clash `7890`、Web `8080`、Minecraft `25565`、幻兽帕鲁 `8211`。
  - `p2ptun://` 一键配对码：暴露端点击【📋 复制傻瓜配对码】，对端点击【📥 粘贴配对码一键导入并连接】即自动避开系统占用端口（`3389` → `13389`）并立即启动。配对码同时兼容 `p2ptun connect ...` 命令行与 `隧道名:密钥@端口` 极简口令。
- **全高级参数透传**：规则编辑区支持 `key=value` 直接填写并透传 p2ptun 全部 flag —— `broker / stun / turn / proxy / key / keepalive / ice_ports / nat1to1 / idle_timeout / max_peers / force_relay / keep_proxy`；留空即使用 p2ptun 内置默认，不做任何隐藏改写。
- **内置 COM 串口桥接**：选择串口（波特率 `9600`~`921600`，`8N1`/`7E2`/`8O1`）后，管理器自动在本机端口与物理串口间双向透传 —— Expose 模式把本机串口暴露给远端，Connect 模式把远端串口映射到本地串口（Win32 原生 API，零第三方库）。
- **企业级安全与守护**（密钥零命令行泄露 + 僵尸进程零残留 + 看门狗自愈）：
  - 密钥通过 `P2PTUN_SECRET` 环境变量注入子进程，绝不出现在命令行/任务管理器；
  - 所有子进程绑定内核 JobObject（`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`），GUI 被强杀也不残留僵尸进程；
  - 进程异常退出时看门狗自动守护重启（上限 5 次，`auto_restart` 可关）；
  - 控制面板 `POST /reconnect`、`POST /quit` 自动携带同源 `Origin`/`Referer` 头，适配 p2ptun v0.5.2+ 的 CSRF 同源校验；
  - 全局单实例互斥，防止两个管理器同时托管同一批子进程。
- **实时状态面板**：通过本地 `-status-addr` 轮询每条隧道的信令状态、UDP 打洞直连地址、RTT、在线 Peers、上下行速率、累计流量，日志窗口按隧道前缀聚合。

## 🚀 三步上手

1. **准备**：把 Release 的 `p2ptun-gui.exe` 与核心 `p2ptun.exe` 放同一目录，双击运行（首次自动生成 `config.json`）。
2. **暴露端**：模式选 `📤 Expose`，选预设（如 RDP 3389），点【➕ 保存规则并立即启动】，然后【📋 复制傻瓜配对码】。
3. **连接端**：另一台电脑直接点【📥 粘贴配对码一键导入并连接】，状态变 🟢 后点【🚀 打开客户端】自动拉起 mstsc/浏览器/SSH。

## 📝 config.json（支持 `//` 与 `#` 注释）

```json
{
  "p2ptun_path": "p2ptun.exe",
  "default_secret": "my-secret-123456",
  "default_proto": "hy2",
  "default_bandwidth": 100,
  "minimize_to_tray": true,
  "expose": [
    {
      "remark": "本机远程桌面 (RDP)",
      "name": "home-rdp",
      "port": "3389",
      "max_peers": 32,
      "enabled": true
    },
    {
      "remark": "工控机串口透传",
      "name": "plc-serial",
      "port": "9103",
      "serial_port": "COM3",
      "serial_baud": 115200,
      "serial_params": "8N1",
      "enabled": false
    }
  ],
  "connect": [
    {
      "remark": "连接家里远程桌面 (本地13389)",
      "name": "home-rdp",
      "port": "13389",
      "keepalive": "15s",
      "on_demand": true,
      "idle_timeout": "30m",
      "enabled": true
    }
  ]
}
```

高级参数均可选填：`broker / stun / turn / proxy / peer_key / keepalive / ice_ports / nat1to1 / idle_timeout / max_peers / keep_proxy / force_relay / auto_restart`；串口字段：`serial_port / serial_baud / serial_params`。留空字段不会写入 JSON，也不会传给 p2ptun。

## 🛠️ 编译与测试

### GitHub Actions
- `.github/workflows/ci.yml`：每次 Push/PR 在 Ubuntu 上跑 C++17 核心单测（g++ headless），并在 windows-latest 上完成 MSVC x64 构建 + ctest 单测。
- `.github/workflows/release.yml`：推送 `v*` 标签或手动触发时发布 Release（单文件 EXE + 便携包）。

```bash
git tag v1.0.0 && git push origin v1.0.0
```

### 本地编译（Windows MSVC / MinGW）
```powershell
cmake -B build -DCMAKE_BUILD_TYPE=Release -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

### 本地跑核心单测（任意平台，含 Linux CI）
```bash
make test   # 或: g++ -std=c++17 -O2 -Wall -Wextra -pthread src/core.cpp src/process_manager_win32.cpp tests/test_core.cpp -o t && ./t
```

## 🧩 架构

```text
p2ptun-manager/
├── src/
│   ├── core.hpp / core.cpp            # 规则模型、JSON 解析(零依赖)、配对码、flag 构造、串口参数
│   ├── process_manager.hpp             # MultiTunnelManager / SerialTcpBridge 接口
│   ├── process_manager_win32.cpp       # 进程守护、JobObject、看门狗、/status 轮询、串口桥接
│   ├── main_win32.cpp                  # Win32 GUI (列表+编辑+日志+托盘)
│   └── app.rc / app.manifest           # 资源与 PerMonitorV2 DPI 清单
├── tests/test_core.cpp                 # 核心单测 (端口规范化/配对码/JSON/flag/串口/管理器增删)
├── CMakeLists.txt
└── .github/workflows/{ci,release}.yml
```

## 与上游 p2ptun 的对齐关系

| 能力 | p2ptun 侧依据 | 本管理器行为 |
|---|---|---|
| 全 flag 透传 | `main.go` usage 全集 | `BuildSubprocessArgs` 留空不传，传则原样 |
| 密钥注入 | `P2PTUN_SECRET` 环境变量 | 子进程环境块注入，命令行零密钥 |
| 状态轮询 | `daemon.go statusSnapshot` JSON | 逐字段解析（version/pid/udp_endpoint/signaling_up/bytes_tx…） |
| CSRF 适配 | `daemon.go crossSiteRequest` | `/reconnect`、`/quit` 带同源 Origin/Referer |
| 优雅退出 | `POST /quit` | 先 HTTP 优雅退出，超时 600ms 后强杀 |
| 依赖声明 | `p2ptun` v0.5.2 | 不修改、不打包、不内嵌 p2ptun，仅作为外部 exe 管理 |

## 📄 License

MIT License.
