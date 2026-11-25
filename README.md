# p2ptun-manager (Windows 多端口穿透图形管理器 · C++17)

专为 [`Hana-ame/p2ptun`](https://github.com/Hana-ame/p2ptun) 打造的原生 **C++17 Windows 桌面管理器**。

**只管理、不修改 p2ptun 本体**：管理器以子进程方式拉起 `p2ptun.exe`，全部参数与控制接口均严格对齐上游 v0.5.2 的 flag 定义与 `GET /status` JSON 快照。Release 便携包内的 `p2ptun.exe` 为上游官方二进制原样下载。

## 界面：一条隧道一张卡片

打开就是全部功能，没有第二层。

```
┌────────────────────────────────────────────┐
│ [+ 新增隧道]                    [日志]     │
├────────────────────────────────────────────┤
│ ┌────────────────────────────────────────┐ │
│ │ ID [tun-7f8a9b]  端口 [3389]        ×  │ │
│ │ Secret [p2p-a1b2c3.........] [随机]    │ │
│ │ ● 服务端 · 待命（已注册信令）          │ │
│ │ [服务端] [客户端] [停止]               │ │
│ └────────────────────────────────────────┘ │
└────────────────────────────────────────────┘
```

一张卡片里只有：

- **`ID`** — 两端填相同值即可配对（新增时自动生成随机值）
- **`端口`** — 本地端口，填 `3389` 或 `127.0.0.1:3389` 都行
- **`Secret`** — **每条隧道各自独立**，两端填相同即可（新增时自动生成随机值，点「随机」可重掷）
- **三个按钮** — `服务端`（暴露本机端口）/ `客户端`（把远端隧道接到本地）/ `停止`

顶部就两个按钮：`+ 新增隧道`、`日志`。没有全局 Secret——每条隧道用自己的。

没有列表选中、没有 Tab、没有预设下拉、没有模式切换、没有高级参数面板、没有 RDP 入口。一个只有三个字段的工具不该需要学会一套控件。

## 状态怎么看

卡片边框和状态点就是状态：

| 颜色 | 含义 |
|---|---|
| 灰色 | 未运行 |
| 绿色 | 在线（服务端显示连入数，客户端显示连接状态） |
| 红色 | 异常退出 |

运行中的卡片会在状态行显示角色与累计流量，右侧实时 `↑ 上行 / ↓ 下行`。正在运行的那一侧按钮会变成绿色。

## 高 DPI

PerMonitorV2 声明，全部布局与字体按窗口 DPI 缩放（96-DPI 基准 × `dpi/96`），跨屏拖动触发 `WM_DPICHANGED` 重建布局。200% 缩放下按钮、输入框、文字均为正确大小。

## config.json（支持 `//` 与 `#` 注释）

```jsonc
{
  "tunnels": [
    { "id": "tun-7f8a9b", "port": "127.0.0.1:3389", "secret": "p2p-a1b2c3" },
    { "id": "tun-3c4d5e", "port": "127.0.0.1:22",   "secret": "p2p-9d8e7f" }
  ]
}
```

每条隧道三样东西：`id` / `port` / `secret`。顶层 `secret` 是可选的兜底值，只在某条隧道没写自己的 `secret` 时才生效。

`proto`、`bandwidth_mbps`、`broker`、`stun`、`turn`、`keepalive`、`ice_ports`、`max_peers`、`on_demand` 等 p2ptun 原生参数仍然**可写在配置里并被原样透传**，只是界面不再展示——手写配置文件即可生效，留空则不传该 flag。

## 编译与测试

### Windows MSVC / MinGW

```powershell
cmake -B build -DCMAKE_BUILD_TYPE=Release -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

MSVC 静态链接 CRT，产物 `p2ptun-gui.exe` 约 600KB，任何 Windows 10/11 双击即用，不需要运行库。

### 本地跑核心单测（任意平台）

```bash
make test
```

## 目录

```
src/core.cpp                 配置解析 / 端口规范化 / 配对码 / 状态 JSON
src/process_manager_win32.cpp 多实例进程与状态管理
src/main_win32.cpp           卡片式 GUI（自绘按钮 + 双缓冲）
tests/test_core.cpp          核心单元测试
```

## License

MIT License.
