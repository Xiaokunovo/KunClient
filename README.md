# KunClient

基于 **.NET 10 + WPF** 的安卓设备刷机工具箱客户端。

本仓库是 KunClient 的 **C# 客户端源码** 开源仓库。

> **开源范围**：本仓库只包含 C# 客户端代码。
> EDL 刷写引擎的 **C++/Qt6 版本**（`third_party/swt-edl`、`src/cpp`）属于**闭源**部分，
> 不包含在本仓库内；服务端、官网、机器人及构建部署脚本同样未开源。
> 详见下方「[开源范围](#开源范围)」。

> **⚠️ 本仓库是脱敏后的开源版**
> - 所有实际部署域名已替换为占位符：主服务端 `your-domain.com`、FDL 计算接口 `fdl-endpoint.example.com`
> - API 版本统一为 **v1**（路径形如 `/api/v1/...`）
>
> 代码可以直接编译，但**在线功能（登录、会员、云驱动、自动更新）连不上任何后端** ——
> 需要你把域名换成自己的部署，并把服务端接口对齐到代码中的路径。
> 本地刷写类功能（EDL / Fastboot / 镜像与分区表处理）不依赖后端，不受影响。

---

## 功能一览

| 分类 | 页面 |
| --- | --- |
| 设备 | 设备首页、文件传输、投屏 |
| 刷机 | EDL 刷写、Fastboot 刷写、可视化刷写、一加刷写、小米 EDL、小米 Fastboot |
| 镜像 | Payload 提取、云端驱动 |
| 工具 | 应用管理、安卓工具箱、命令控制台 |
| 其它 | 设置、关于 |

核心库（`KunClient.Core`）提供的底层能力：

- **EDL 协议栈（C# 实现）** —— Sahara / Firehose 握手与读写、欧加 VIP 鉴权会话、
  GPT 分区表解析、`rawprogram` / `patch` XML 解析与写出、sparse 镜像、动态分区（super）规划
- **镜像与文件系统** —— ext4 / erofs 读取、`build.prop` 解析、payload.bin 解析、lp 元数据解析
- **刷机包解密** —— OPPO/一加 OFP 解密、OPS 解密、小米 EDL 鉴权文件
- **下载与网络** —— 分片并行下载器、请求签名
- **设备与服务** —— USB/9008 端口监听、平台工具（adb / fastboot）管理、账号与会员校验

---

## 开源范围

### 包含（本仓库）

- `tests/src/KunClient.App` —— WPF 客户端（窗口、页面、控件、样式、主题）
- `tests/src/KunClient.Core` —— 核心库（协议、文件系统、服务、下载、配置）
- `tests/src/KunClient.Shell` / `KunClient.Diag` / `KunClient.Lsusb` —— 辅助入口
- `tests/KunFlashTool.Core.Tests` —— 单元测试

### 不包含（闭源 / 未开源）

| 目录 | 说明 |
| --- | --- |
| `third_party/swt-edl/` | **SWT_EDL（C++/Qt6）EDL 刷写引擎**，第三方组件，不开源 |
| `src/cpp/` | KunClient 的 C++/Qt6 实现（含 EDL 分层），不开源 |
| `src/rust/` | Rust 刷写栈（已冻结保留），不开源 |
| `server/` | 后端服务（账号、会员、云驱动、更新） |
| `website/` | 官网 |
| `qqbot/` | 机器人 |
| `tools/`、`*.ps1`、`*.bat` | 构建与部署脚本 |
| `docs/`、`prototype/` | 设计文档与 UI 原型 |

> 由于 C# 客户端中的 EDL 协议栈是**独立实现**，本仓库可以脱离 C++ 版引擎单独编译与使用；
> 但依赖后端的在线功能（登录、会员、云驱动、自动更新）需要自行提供 `serverUrl` 才能使用。

---

## 目录结构

```
tests/
├── KunFlashTool.Core.Tests/        # 单元测试（含 EDL 协议、分区表、解密器等用例）
└── src/
    ├── KunClient.App/              # WPF 客户端
    │   ├── Controls/  Converters/  Dialogs/  Localization/
    │   ├── Pages/                  # 15 个功能页面
    │   ├── Services/               # 会话、更新、下载、主题、设备状态等
    │   ├── Assets/                 # 图标
    │   ├── DesignTokens.xaml       # 设计令牌
    │   └── KunClient.config.example.json
    ├── KunClient.Core/             # 核心库
    │   ├── Configuration/  DynamicSuper/  FileSystems/
    │   ├── Modules/  Net/  PlatformTools/  Protocol/
    │   ├── Services/               # OFP/OPS 解密、payload 解析、super 合并等
    │   └── SwtEdl/                 # EDL 协议栈（C# 实现）
    ├── KunClient.Diag/             # 诊断入口
    ├── KunClient.Lsusb/            # USB 设备枚举
    └── KunClient.Shell/            # 外壳入口
KunFlashTool.slnx                   # 解决方案文件
LICENSE
```

---

## 构建

**环境要求**

- Windows 10 / 11（x64）
- [.NET 10 SDK](https://dotnet.microsoft.com/download)

**构建客户端**

```bash
dotnet build tests/src/KunClient.App/KunClient.App.csproj -c Release -r win-x64
```

**发布单文件**

```bash
dotnet publish tests/src/KunClient.App/KunClient.App.csproj \
  -c Release -r win-x64 --no-build -p:PublishSingleFile=true
```

**运行单元测试**

```bash
dotnet test tests/KunFlashTool.Core.Tests/KunFlashTool.Core.Tests.csproj
```

> `KunClient.App.csproj` 中的 `RuntimeIdentifiers` 必须为 `win-x64`。
> `publish` 会自行触发一次隐式还原，缺少 RID 目标会直接以 `NETSDK1047` 失败。

---

## 平台工具（adb / fastboot）

`PlatformTools` 目录**不随仓库分发**（第三方二进制，约 11 MB）。
运行时由 `PlatformToolsManager` 自动下载并解压；也可手动放置以下文件：

```
tests/src/KunClient.Core/PlatformTools/
├── adb.exe
├── fastboot.exe
├── AdbWinApi.dll
└── AdbWinUsbApi.dll
```

来源：<https://dl.google.com/android/repository/platform-tools-latest-windows.zip>
（Android Platform Tools，Apache License 2.0）

---

## 配置

服务器地址按以下优先级解析，首个非空值生效：

1. 环境变量 `KUNCLIENT_SERVER_URL`
2. `%LOCALAPPDATA%\KunClient\KunClient.config.json`（登录框「服务器设置」写入）
3. exe 同目录 `KunClient.config.local.json`
4. exe 同目录 `KunClient.config.json`
5. 源码内置默认值（`ClientConfig.DefaultServerUrl`，**开源版为占位符** `https://your-domain.com`）

字段模板见 `tests/src/KunClient.App/KunClient.config.example.json`。
`KunClient.config.local.json` 为本机私有覆盖，**不入库**。

---

## 第三方组件

| 组件 | 许可证 |
| --- | --- |
| [CommunityToolkit.Mvvm](https://github.com/CommunityToolkit/dotnet) | MIT |
| [LibUsbDotNet](https://github.com/LibUsbDotNet/LibUsbDotNet) | LGPL-3.0 |
| [SharpCompress](https://github.com/adamhathcock/sharpcompress) | MIT |
| [Google.Protobuf](https://github.com/protocolbuffers/protobuf) | BSD-3-Clause |
| [System.Management](https://github.com/dotnet/runtime) | MIT |
| Android Platform Tools | Apache-2.0 |

---

## 许可证

本项目采用 [MIT License](LICENSE) 开源。
