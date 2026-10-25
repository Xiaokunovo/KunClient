# KunClient C++/Qt6 构建指南

> 2026-10-10 起 KunClient 主工程从 .NET/WPF 迁移到 **C++20 / Qt6 / CMake + Ninja（MSYS2 MinGW64）**。
> 旧 .NET 链（`KunFlashTool.slnx` / `dotnet publish` / `tools/obfuscate-build.js` / Obfuscar）进入退役状态，
> `tests/src/KunClient.*` 仅作移植参考，不再出产物。

## 工具链（本机已就绪）

| 组件 | 位置 | 版本 |
|------|------|------|
| GCC | `<MSYS2>/mingw64/bin/g++.exe` | 16.2.0+ (MinGW64) |
| CMake | `<MSYS2>/mingw64/bin/cmake.exe` | 3.24+ |
| Ninja | `<MSYS2>/mingw64/bin/ninja.exe` | 1.11+ |
| Qt6 | `<MSYS2>/mingw64/`（系统级安装） | 6.5+，含 Core/Gui/Widgets/Network/SerialPort/Xml |

⚠️ 本机 **无 MSVC cl.exe**；`windeployqt6` **必然失败**（内部 spawn `qtpaths` 被拦）——
依赖部署用 build-release.sh 的 ldd 闭包方案，不要用 windeployqt6。

## 目录结构

```
src/cpp/
├── CMakeLists.txt          # 顶层构建（project KunClient）
├── CMakePresets.json        # mingw64-release / mingw64-debug 预设
├── build-release.sh         # Release 全流程：构建+DLL闭包+strip+md5（见下）
├── build-release.cmd        # 同上，cmd 包装
├── tools/set-version.js     # 版本号统一落点更新（见「版本管理」）
├── BUILD.md                 # 本文件
├── core/ transport/ protocol/ storage/ oplus/ app/   # EDL 引擎（swt::edl，零改动）
├── cli/                     # 无头 CLI 模式（--cli，NDJSON stdin/stdout）
├── kc/                      # KunClient 特有代码（kc:: 命名空间）
│   ├── config/    AppStatePaths / ClientConfig / AppSettings
│   ├── net/       HttpClient / ServerGate / ChunkedDownloader
│   ├── auth/      AuthSession / LoginStorage / MembershipService / ForceLogout / Fdl*
│   ├── security/  DpapiBlob / EnvInspector / EnvReportService / StrObf.h
│   ├── update/    UpdateService
│   ├── tools/     PlatformToolsManager
│   ├── devices/   DeviceWatcher / UsbDeviceNotifier
│   └── version.h  版本常量（落点之一）
├── ui/                      # MainShell（侧栏导航外壳）+ pages/ widgets/ theme/
├── resources/               # QSS 深浅主题 + 图标（.qrc）
└── tests/                   # GptParserTest / MainWindowLogTest（ctest）
```

## 日常构建

```bash
# Debug（快速迭代；build/ 目录）
export PATH="/e/msys64/mingw64/bin:$PATH"
cd /e/flashtool/src/cpp
cmake -G Ninja -DCMAKE_BUILD_TYPE=Debug -B build -S .
cmake --build build --parallel
./build/KunClient.exe --screenshot shot.png        # GUI 冒烟
./build/kc-ui-tests.exe && ./build/kc-core-tests.exe

# Release 全流程（构建 + dist/ 部署 + strip + md5，推荐发版用）
./build-release.sh              # 全量
./build-release.sh --no-strip   # 保留符号排查崩溃
./build-release.sh --clean      # 清 build-release/ 重建
```

产物：`dist/KunClient.exe`（~1.6MB，strip 后）+ DLL 闭包 + `platforms/`。
`dist/` 可独立运行（不依赖 PATH 里的 mingw64），发版就是打这个目录。

## 单文件发布包（`build-static.sh`）

发版交付的是**单个 exe**，不是 `dist/` 目录。用 `build-static.sh`：

```bash
bash src/cpp/build-static.sh --7z                 # 自动选 Qt；产出 strip 后 exe + UPX + 7z
bash src/cpp/build-static.sh --min-qt --7z --suffix min   # 精简 Qt + 带后缀（不覆盖已发布底本）
bash src/cpp/build-static.sh --std-qt --no-upx    # 逃生门：退回系统 qt6-static、不压壳
```

### 两套 Qt，体积差 35%

| 模式 | Qt 前缀 | strip 后 exe | 说明 |
|---|---|---|---|
| `--std-qt` | `<MSYS2>/ucrt64/qt6-static` | ~42.4MB | 旧行为 |
| `--min-qt` | `E:/qt6-min/6.12.0` | **~27.7MB** | 自建精简 Qt，同 Qt 6.12.0 |
| 自动（默认） | 探测到 min 前缀就用 min | ~27.7MB | 构建日志会打印 `[Qt] 精简 Qt …` |

精简 Qt 裁掉了 OpenGL / Vulkan / OpenSSL / SQL / PrintSupport / PDF / Movie / QtConcurrent。
**安全性已核**：这些在 `src/cpp` 全量 grep **0 处引用**；GIF/JPEG 解码器仍在（自定义背景不受影响）；
与标准版连拍对比屏蔽标题栏时钟后帧指纹完全一致；`--netcheck` / `--env-check` 均 rc=0。

### 体积分层（实测）

```
59.6MB  未 strip（带全部调试符号）
42.4MB  strip 后（标准 Qt）
27.7MB  strip 后（精简 Qt）              −35%
13.1MB  标准 Qt + UPX --best --lzma      −69%
 8.5MB  精简 Qt + UPX --best --lzma      −80%   ← 双击即用，代价见下
 7.3MB  精简 Qt + 7z -mx=9               −83%   ← 体积最小，需解压一次
```

**UPX 的代价**：解压耗时约 926ms（每次启动），启动总时长中位 5.9s → 7.5s；
且 UPX 壳在 360 / 火绒等杀软上有**误报风险**。要排查崩溃请用 `--no-upx` 构建
（压缩后栈回溯看到的是 UPX 解压桩，不是真实地址）。

**7z / SFX**：官方 `7z.sfx` 不读 SFX 配置（`Directory` / `RunProgram` 被忽略），
只能做「双击解出自解压包、用户再点一次」，做不到解压即启动。

### 可复现构建

脚本默认 `SOURCE_DATE_EPOCH=0`：同一份源码 → 同一个 md5（代价是 PE 时间戳变成 1970；
版本资源、功能均不受影响）。`KC_REPRODUCIBLE=0` 可关闭。

两个必要条件（缺一不可，2026-10-24 实测）：
1. **strip 到副本**，不原地 strip —— strip 不幂等，而 ninja 增量「no work to do」时
   不会重出未 strip 的 exe，原地 strip 会让连跑两次 md5 不同；
2. **固定 `SOURCE_DATE_EPOCH`** —— strip 默认把 PE TimeDateStamp 写成当前时间。

7z 还要加 `-mtm=off -mtc=off -mta=off`，否则归档头带文件时间戳，exe 相同而 7z 每次不同。

### 红线

- **带 `--suffix` 时绝不覆盖** `KunClient-<ver>.exe` / `.7z` / `-upx.exe` ——
  那是线上已发布、可回滚的底本。
- UPX / 7z 一律**先压到临时文件** → 校验（>1MB 且 `upx -t` / `7z t` 通过）→ 再 `mv` 进
  `dist-static/`。UPX 是直接在目标路径上写的，中途被杀会留下几百字节的半成品；
  这种残骸一旦被发版脚本选中，用户下到的就是坏 exe。
- 构建前**必须查源码是否被并发改动**——本仓库 `git` 不跟踪 `src/cpp`
  （`.gitignore` 白名单只放 `tests/`），`git status` 永远干净：

```powershell
Get-ChildItem src\cpp -Recurse -Include *.cpp,*.h |
  Where-Object { $_.LastWriteTime -gt $基线时间 }
```

### 运行参数（探针/CI 用）

| 参数 | 作用 |
|------|------|
| `--screenshot <path>` | 启动 500ms 后截图退出 |
| `--page <key>` | 直落指定页（如 `EdlFlash` / `DeviceHome` / `Settings`） |
| `--light` / `--dark` | 主题覆盖（写 QSettings） |
| `KC_NOSTYLE=1` | 环境变量：跳过 QSS（隔离样式问题的诊断开关） |
| `--cli` | 无头 NDJSON 模式（供外部程序以子进程驱动） |

## 测试

```bash
cd build && ctest --output-on-failure     # kc-core-tests + kc-ui-tests
```

判据红线（与 .NET 时代一致）：从规范推导（UEFI GPT 布局 / libsparse / rawprogram 语义），
**不回抄实现输出快照**——否则实现改错测试也跟着绿。

## 版本管理

版本号 5 处落点（红线：一个版本号只承载一个产物）：

| # | 落点 | 说明 |
|---|------|------|
| 1 | `src/cpp/CMakeLists.txt` | `project(KunClient VERSION x.y.z)` |
| 2 | `src/cpp/kc/version.h` | kVersion* 四常量 + kVersionString + kUserAgent |
| 3 | `website/src/lib/data.ts` | `FALLBACK_VERSION.version`（官网兜底展示） |
| 4 | 线上 `app_settings.version` | 运行时库，上线时用 `.workbuddy/tmp/set_setting.js` |
| 5 | UA 头 | 派生自 version.h（HttpClient 统一注入），不单独改 |

```bash
node src/cpp/tools/set-version.js 2.1.0.3               # 三处全改
node src/cpp/tools/set-version.js 2.1.0.3 --no-website  # 迁移期：官网 FALLBACK 跟线上已发布版本走
node src/cpp/tools/set-version.js 2.1.0.3 --dry         # 预演
```

⚠️ **迁移期约定**：C++ 客户端（2.0.x）未发布前，官网 FALLBACK_VERSION 保持线上
.NET 客户端的版本（1.1.4.x），两者允许暂时不同步；C++ 正式发版时再用全量模式统一。

## 字符串保护（StrObf）

Obfuscar HideStrings 的 C++ 替代，见 `kc/security/StrObf.h`：

```cpp
#include "kc/security/StrObf.h"
const QString url = KC_STR("https://kunawa.cn", 0x5A);   // 每个调用点换 key
```

- 编译期 constexpr 逐字节 XOR（key + 位置系数 31），**二进制里只有密文**；
- 适用对象：域名、端点路径、UA 前缀等敏感字面量；
- 已实测（2026-10-11）：中英文 roundtrip ✓、自体 grep 明文 0 命中 ✓；
- 这是提高门槛不是加密学安全（与 Obfuscar 同级）；
- 🔴 每个敏感字面量用**独立 key**（0~255），同 key 同模式会被批量还原。

反调试/环境检测在 `kc/security/`：Win32 API 直调（`IsDebuggerPresent` /
`CheckRemoteDebuggerPresent` / `NtQueryInformationProcess`），不需要 .NET 时代的 P/Invoke 封装层。
旧壳（AES 内存加载 / KUNCLIENT_DEV 后门）是 .NET 装载模型专属，C++ 工程不适用；C++ 侧
基础保护 = strip + StrObf + 原生反调试。商用壳已决策落地 **VMProtect**（"壳使用 vmp"），
由 `build-release.sh` 在 strip 之后自动调用 `tools/vmp/protect-vmp.sh` 加壳；详见下方
「商业壳（VMProtect）」一节与 `tools/vmp/README.md`。

## 商业壳（VMProtect）

> 决策：壳使用 VMProtect。保护在 strip 之后、冒烟/ md5 之前执行，故发布的 md5 即加壳后产物。

- **接线位置**：`build-release.sh` 第 `[5/6]` 步调用 `tools/vmp/protect-vmp.sh`。
- **启用条件**（两者齐全才激活，否则静默跳过、构建与原先一致）：
  1. `tools/vmp/VMProtect_Con.exe`（合法授权的控制台版）—— 本机当前**未安装**。
  2. `tools/vmp/KunClient.vmp`（在 VMProtect GUI 中生成，Input=`dist/KunClient.exe`、
     Output=`dist/KunClient.vmp.exe`）。
- **失败策略**：两者都在但 VMProtect 报错 → 构建**中止**，不静默发未保护成品。
- **准备步骤**：见 `tools/vmp/README.md`（GUI 打开 `dist/KunClient.exe` → 设 Mode/压缩/水印
  /许可 → 保存为 `KunClient.vmp` → 拷贝 `VMProtect_Con.exe` → 重跑 `build-release.sh`）。
- **兼容性**：VMProtect 与 MinGW64/SEH 兼容；strip 后无符号名，按地址整文件保护即可。
- **红线**：发版三方一致校验以加壳后的 `dist/KunClient.exe` 为准；激进虚拟化若致运行期异常，
  先 `build-release.sh --no-strip` 构建并在 GUI 单独用 `--screenshot` 探针验证。

## 已知坑（2026-10-10/11 实测沉淀）

1. **`QStringLiteral()` 不能包 constexpr 变量**——宏要求编译期字面量。用
   `QString::fromLatin1(kc::kVersionString)`。
2. **自定义控件 setStyleSheet + changeEvent 回灌 = 无限递归（栈溢出 0xC00000FD）**。
   `applyTheme()` 里 setStyleSheet 会同步投递 StyleChange → changeEvent → applyTheme…
   必须加重入守卫（NotificationBar / PageHeaderBar 已修，新控件照做）。
3. **Qt DLL 必须同源**：build/ 里混用「SWT-EDL 旧部署 DLL」+「mingw64 新 DLL」
   会加载失败（exit 127 / STATUS_入口点未找到）。一律整组从 mingw64/bin 复制。
4. **EdlWorker 依赖 QSerialPort**：kc-core-tests 只链 Qt6::Core，不要把
   transport/protocol/app 的源喂给它（CMakeLists 里 EDL_CORE_SOURCES 与
   EDL_FULL_SOURCES 两层就是为此拆的）。
5. **并发跑两个 ninja 会撞锁**（failed recompaction: Permission denied）——
   build-release.sh 用独立 build-release/ 目录规避；手动构建时别同时开两个。
6. **EmbeddedResource / const string 泄漏判据只适用 .NET**；C++ 侧等价判据是
   `strings`/grep 产物 0 命中（StrObf 验证脚本的口径）。

## 迁移状态（2026-10-11）

| 层 | 状态 |
|----|------|
| EDL 引擎（Sahara/Firehose/GPT/Sparse/DynamicSuper/VIP） | ✅ 原样复用（swt::edl 零改动） |
| 无头 CLI（NDJSON） | ✅ 原样复用 |
| config / net / auth / security / update / tools / devices | ✅ 已移植（kc::） |
| UI 框架（MainShell + 主题/动效/日志/通知/进度） | ✅ 已移植 |
| 页面：DeviceHome / EdlFlash（嵌 SWT-EDL 全功能） | ✅ 可用 |
| 其余 14 页 + 12 对话框 | ⏳ 占位（PlaceholderPage），逐页移植中 |
| C# 工程（tests/src/KunClient.*） | 待退役，仅作移植参考 |
