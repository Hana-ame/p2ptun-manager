# p2ptun-manager (Windows 多端口穿透图形管理器 · C++17)

专为 [`Hana-ame/p2ptun`](https://github.com/Hana-ame/p2ptun) 打造的原生 **C++17 Windows 桌面管理器**。

**只管理、不修改 p2ptun 本体**：管理器以子进程方式拉起 `p2ptun.exe`，全部参数与控制接口均严格对齐上游 v0.5.2 的 flag 定义与 `GET /status` JSON 快照（见上游 `main.go` usage 与 `daemon.go statusSnapshot`）。本仓库不修改 p2ptun 源码；Release 便携包内的 `p2ptun.exe` 为上游官方二进制原样下载。

一条隧道只配置一次（**id + ip:port**），运行时自己在 **Server（暴露本机端口）/ Client（连接远端隧道）** 之间选角色，同一个 id 两端即可配对。

## ✨ 核心特性

- **原生 C++17 Win32，零依赖**：静态链接 CRT，单文件约 600KB，任何 Windows 10/11 直接双击运行；不需要 .NET 运行时。
- **统一隧道模型 · 共用一条配置**：每条隧道 = `id + ip:port`，Server / Client 角色运行时自选（Tab 切换，或双击某行按上次角色运行）。
  - 切换角色时端口建议**自动反转**：`3389 ↔ 13389`、`22 ↔ 2222`、`7890 ↔ 17890`、`80 ↔ 18080` …（避免本机系统占用端口）。
  - 旧版 `expose` / `connect` 双列表配置自动迁移，无需手工改。
- **每行独立启停**：列表「启停」列每行一个 ▶/⏹ 按钮，单条隧道独立开关；也可双击行切换；另有全部启动 / 全部停止。
- **简易模式 / 高级模式一键切换**：默认简易模式只有 **id + ip:port** 两个输入框；点【⚙ 高级模式】才展开备注、场景预设、协议、带宽、按需连接、单条高级参数覆盖与粘贴配对码。
- **全高级参数透传（全局默认 + 单条覆盖）**：`broker / stun / turn / proxy / key / keepalive / ice_ports / nat1to1 / idle_timeout / max_peers / force_relay / keep_proxy / on_demand`。
  - 生效顺序：**单条规则 > 全局「高级默认」> p2ptun 内置默认**；留空即不传该 flag，不做任何隐藏改写。
- **高 DPI 自适应**：PerMonitorV2 声明 + 全部布局/字体按窗口 DPI 缩放（96-DPI 基准 × `dpi/96`），**200% 缩放下按钮/表格/文字均为正确大小**；跨屏拖动触发 `WM_DPICHANGED` 自动重建布局。
- **傻瓜化辅助**：
  - 纯数字端口自动补全（`3389` → `127.0.0.1:3389`），全局默认密钥/协议/带宽自动继承。
  - 场景一键预设：RDP `3389`、SSH `22`、SOCKS5 `1080`、Clash `7890`、Web `8080`、Minecraft `25565`、幻兽帕鲁 `8211`。
  - `p2ptun://` 一键配对码：一端【📋 复制傻瓜配对码】，对端【📥 粘贴配对码一键导入并连接】；兼容 `p2ptun connect ...` 命令行与 `隧道名:密钥@端口` 极简口令。
- **批量导出**：一键把完整配置（全局设置 + 全部隧道）导出为 JSON 备份。
- **企业级安全与守护**：
  - 密钥通过 `P2PTUN_SECRET` 环境变量注入子进程，**绝不出现在命令行/任务管理器**（密钥全局配置一处，所有隧道共用）；
  - 所有子进程绑定内核 JobObject（`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`），GUI 被强杀也不残留僵尸进程；
  - 进程异常退出时看门狗自动守护重启（上限 5 次，`auto_restart` 可关）；
  - 控制面板 `POST /reconnect`、`POST /quit` 自动携带同源 `Origin`/`Referer` 头，适配 p2ptun v0.5.2+ 的 CSRF 同源校验；
  - 全局单实例互斥，防止两个管理器同时托管同一批子进程。
- **实时状态面板**：通过本地 `-status-addr` 轮询每条隧道的信令状态、UDP 打洞直连地址、RTT、在线 Peers、上下行速率、累计流量；日志窗口按隧道前缀聚合。

## 🚀 三步上手

1. **准备**：下载 Release 的 `p2ptun-win-x64-portable.zip` 解压（内含管理器 `p2ptun-gui.exe` + 官方 `p2ptun.exe`），双击 `p2ptun-gui.exe`；首次运行自动生成 `config.json`。
2. **暴露端**：在 `📤 Server` 页填 `id` + `ip:port`（如 `my-rdp` + `3389`），【➕ 保存并立即启动】，选中该行点【📋 复制傻瓜配对码】。
3. **连接端**：另一台电脑切到 `📥 Client` 页，直接【📥 粘贴配对码一键导入并连接】；状态变 🟢 后点【🚀 打开客户端】自动拉起 mstsc / 浏览器 / SSH。

> 同一条隧道可在 Server / Client 之间切换角色：切 Tab 时端口自动反转（`3389` → `13389`），配置不用重填。

## 📝 config.json（支持 `//` 与 `#` 注释）

```jsonc
{
  "p2ptun_path": "p2ptun.exe",
  "default_secret": "my-secret-123456",
  "default_proto": "hy2",
  "default_bandwidth": 100,
  "minimize_to_tray": true,

  // 全局高级参数默认值（所有隧道共享；单条隧道可覆盖）
  "advanced": {
    "keepalive": "15s",
    "max_peers": 16
  },

  // 统一隧道列表：id + ip:port；mode/last_role 记录当前/上次运行角色
  "tunnels": [
    {
      "id": "tunnel-rdp",
      "remark": "远程桌面 (RDP)",
      "name": "my-rdp",
      "port": "127.0.0.1:3389",
      "mode": "expose",
      "last_role": "expose",
      "enabled": false
    },
    {
      "id": "tunnel-proxy",
      "remark": "代理 (SOCKS5/HTTP)",
      "name": "my-proxy",
      "port": "127.0.0.1:1080",
      "mode": "connect",
      "last_role": "connect",
      "on_demand": true,
      "idle_timeout": "30m",
      "enabled": false
    }
  ]
}
```

说明：
- `tunnels` 是**统一隧道列表**：每条配置同时能作为 Server 或 Client；`mode` 为当前角色、`last_role` 为上次成功角色，重启后恢复。
- 全局 `advanced` 对所有隧道生效；单条隧道里的同名字段覆盖全局值。
- 旧版 `expose` / `connect` 双列表配置会自动迁移合并。
- 密钥只在全局 `default_secret` 配置一处，所有隧道共用（通过 `P2PTUN_SECRET` 环境变量注入子进程）。
- `enabled`: 管理器启动时是否自动拉起该隧道。

## 🛠️ 编译与测试

### GitHub Actions
- **`.github/workflows/ci.yml`**：每次 Push/PR —— Ubuntu 上跑 C++17 核心单测（g++ headless），windows-latest 上 MSVC x64 构建 + ctest 单测。
- **`.github/workflows/release.yml`**：推送 `v*` 标签或手动触发 —— 构建 `p2ptun-gui.exe`，**自动从上游下载官方 `p2ptun_windows_amd64.exe`（不修改）**，与配置示例/README/LICENSE 一起打成便携包，发布 GitHub Release。

```bash
git tag v1.1.1 && git push origin v1.1.1
```

Release 产物：
- `p2ptun-gui.exe` —— 单独的管理器（自备 `p2ptun.exe` 使用）
- `p2ptun-win-x64-portable.zip` —— 便携包：`p2ptun-gui.exe` + 官方 `p2ptun.exe` + `config.json` + README + LICENSE，解压即用
- `config.example.json` —— 配置示例

### 本地编译（Windows MSVC / MinGW）
```powershell
cmake -B build -DCMAKE_BUILD_TYPE=Release -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

### 本地跑核心单测（任意平台，含 Linux CI）
```bash
make test
# 等价于:
g++ -std=c++17 -O2 -Wall -Wextra -pthread \
  src/core.cpp src/process_manager_win32.cpp tests/test_core.cpp -o t && ./t
```

## 🧩 架构

```text
p2ptun-manager/
├── src/
│   ├── core.hpp / core.cpp            # 统一隧道模型、零依赖 JSON、配对码、flag 构造、端口反转
│   ├── process_manager.hpp             # MultiTunnelManager / 行视图模型
│   ├── process_manager_win32.cpp       # 进程守护、JobObject、看门狗、/status 轮询、导出
│   ├── main_win32.cpp                  # Win32 GUI（Server/Client Tab、简易/高级模式、行内启停、托盘、DPI 缩放）
│   └── app.rc / app.manifest           # 资源与 PerMonitorV2 DPI 清单
├── tests/test_core.cpp                 # 核心单测（端口规范化/配对码/JSON/flag/迁移/管理器增删）
├── config.example.json
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
| 本体零修改 | 上游 `v0.5.2` | 不修改源码；便携包内的 `p2ptun.exe` 为官方 Release 原样下载 |

## 📄 License

MIT License.