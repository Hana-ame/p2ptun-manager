# p2ptun-manager (Windows 多端口穿透图形管理器 · C++17)

专为 [`Hana-ame/p2ptun`](https://github.com/Hana-ame/p2ptun) 打造的原生 **C++17 Windows 桌面管理器**。

**只管理、不修改 p2ptun 本体**：管理器以子进程方式拉起 `p2ptun.exe`，全部参数与控制接口均严格对齐上游 v0.5.2 的 flag 定义与 `GET /status` JSON 快照（见上游 `main.go` usage 与 `daemon.go statusSnapshot`）。本仓库不修改 p2ptun 源码；Release 便携包内的 `p2ptun.exe` 为上游官方二进制原样下载。

一条隧道只配置一次（**id + port**），运行时自己在 **Server（暴露本机端口）/ Client（连接远端隧道）** 之间选角色，同一个 id 两端即可配对。

## ⚠️ 已知问题（当前版本 · 待修）

> 本 GUI 由 AI 合并/生成，存在以下**已知缺陷与未遵从的指示**（欢迎 PR 修复）：

1. **切换模式时花屏**：简易模式 ↔ 高级模式、Server ↔ Client 切换时界面闪烁/残影（未做稳定重排与重绘）。
2. **简易模式未真正精简**：要求简易模式只保留最少的控件，但当前仍强行保留大部分按钮与输入框，未按指示收敛。
3. **莫名其妙的「RDP 模式」**：界面/入口中存在与需求无关的 RDP 相关按钮与文案。
4. **Server ↔ Client 快速切换未实现**：要求对同一条配置一键切换 Server/Client 角色，当前仍需切 Tab / 重填，没有独立的快速切换控件。
5. **列表样式难看**：列表仍为 Win32 ListView 默认样式，未做视觉优化。
6. **列表中插入了莫名其妙的「Server」列**：表格中间多出一列含义不明的「Server」列。

## ✨ 核心特性

- **原生 C++17 Win32，零依赖**：静态链接 CRT，单文件约 600KB，任何 Windows 10/11 直接双击运行；不需要 .NET 运行时。
- **简易模式 / 高级模式一键无痕切换**：
  - **简易模式（默认）**：
    - 全局设置仅需 **`Secret`**（默认全等，所有隧道共用，默认随机生成）；
    - 每个隧道 Item 仅需设定 **`ID`**（默认自动给出随机值）与 **`Port`**；
    - 表格自动隐藏「备注说明」「协议/带宽」等高级列，界面干净清爽。
  - **高级模式**：点击【⚙ 高级模式】展开核心路径、默认协议/带宽、全局高级参数默认、场景预设、备注、单条 Secret/高级参数覆盖、按需连接与粘贴配对码。
- **统一隧道模型 · 共用一条配置**：每条隧道 = `id + port`，Server / Client 角色运行时自选（Tab 切换，或双击某行按上次角色运行）。
  - 切换角色时端口建议**自动反转**：`3389 ↔ 13389`、`22 ↔ 2222`、`7890 ↔ 17890`、`80 ↔ 18080` …（避免本机系统占用端口）。
  - 旧版 `expose` / `connect` 双列表配置自动迁移，无需手工改。
- **每行独立启停**：列表「启停」列每行一个 ▶/⏹ 按钮，单条隧道独立开关；也可双击行切换；另有全部启动 / 全部停止。
- **全高级参数透传（全局默认 + 单条覆盖）**：`secret / broker / stun / turn / proxy / key / keepalive / ice_ports / nat1to1 / idle_timeout / max_peers / force_relay / keep_proxy / on_demand`。
  - 生效顺序：**单条规则 > 全局默认 > p2ptun 内置默认**；留空即不传该 flag，不做任何隐藏改写。
- **高 DPI 自适应**：PerMonitorV2 声明 + 全部布局/字体按窗口 DPI 缩放（96-DPI 基准 × `dpi/96`），**200% 缩放下按钮/表格/文字均为正确大小**；跨屏拖动触发 `WM_DPICHANGED` 自动重建布局。

## 📝 config.json（支持 `//` 与 `#` 注释）

### 简易模式配置（仅需 `secret` + 每个 item 的 `id` 和 `port`）

```jsonc
{
  "secret": "p2p-a1b2c3",
  "tunnels": [
    {
      "id": "tun-7f8a9b",
      "port": "127.0.0.1:3389"
    },
    {
      "id": "tun-3c4d5e",
      "port": "127.0.0.1:22"
    },
    {
      "id": "tun-9e0f1a",
      "port": "127.0.0.1:1080"
    }
  ]
}
```

### 高级模式可选字段

- 顶层可选：`p2ptun_path`、`default_proto`、`default_bandwidth`、`minimize_to_tray`、`advanced`（`broker`、`stun`、`turn`、`proxy`、`key`、`keepalive`、`ice_ports`、`nat1to1`、`idle_timeout`、`max_peers`、`force_relay`、`keep_proxy`）。
- 单条 Item 可选：`mode`（`expose`/`connect`）、`remark`、`secret`（单条覆盖）、`proto`、`bandwidth_mbps`、`on_demand`、`idle_timeout` 等。

## 🛠️ 编译与测试

### 本地编译（Windows MSVC / MinGW）
```powershell
cmake -B build -DCMAKE_BUILD_TYPE=Release -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

### 本地跑核心单测（任意平台，含 Linux CI）
```bash
make test
```

## 📄 License

MIT License.