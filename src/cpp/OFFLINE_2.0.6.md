# KunClient 2.0.6 · 停更离线版改造说明

> 背景：作者退圈，**服务器一并关停**。原客户端有登录硬门槛（登录失败直接退出），
> 且登录 / 更新 / 云端引导 / 环境检测上报 / 会员 / 云驱动 / 日志上传 / FDL 在线授权
> 等十余个模块都依赖服务器。服务器一关，用户连工具都进不去。
>
> 因此 2.0.6 被改造成**完全离线版**：删掉全部联网功能，保留全部本地刷机能力。
> 版本号不变（仍是 2.0.6），因为更新邮件与停更通知已经按 2.0.6 发出。

---

## 一、改造结果

| 项目 | 结果 |
|---|---|
| 编译 | **0 error / 0 warning**（`-Wall -Wextra -Wpedantic`） |
| 测试 | **16/16 全部通过**（原 18 个，删掉 2 个联网测试套件） |
| 启动 | **无需登录**，直接进主界面 |
| 产物 | `build-offline/KunClient.exe`（6.3 MB）+ 依赖 DLL + 随包 adb/fastboot |
| 主屏 | 13 个纯本地功能图标（「云端驱动」已移除） |

### 实测截图

- `offline-console.png` —— 主屏，无登录框，13 个图标
- `offline-about.png` —— 关于页，版本 2.0.6，无「检查更新」卡片

---

## 二、删除了什么

### 2.1 整个模块删除（31 个源文件）

| 目录 | 删除内容 | 说明 |
|---|---|---|
| `kc/net/` | HttpClient / RequestSigner / ServerGate / ChunkedDownloader / HttpHeaderUtil | 全部 HTTP 基础设施 |
| `kc/update/` | UpdateService | 自动更新 |
| `kc/cloud/` | CloudLoaderIndex | 云端引导库索引 |
| `kc/auth/` | AuthSession / LoginStorage / ForceLogout / FdlApiClient / FdlKamiStore | 登录态、凭据、强制下线、FDL 在线签名 |
| `kc/security/` | EnvGuard / EnvInspector / EnvReportService / DpapiBlob | 环境检测与上报、凭据加密 |
| `kc/diag/` | LogUploadService | 日志上传（`SessionLog` / `LogCollector` 保留，本地日志仍在用） |

### 2.2 UI 删除（19 个源文件）

- 对话框：LoginDialog / UpdateDialog / StartupPreloadDialog / CloudLoaderPickerDialog /
  AppealDialog / LogUploadDialog / DownloadDialog
- 页面：CloudDriverPage（云端驱动）
- 测试：RequestSignerTest / EnvGuardTest / FdlApiClientTest

### 2.3 功能入口清理

- `main.cpp`：删掉登录闸门、启动更新检查、环境检测启动、预载窗，以及
  `--netcheck` / `--checkupdate` / `--env-check` / `--cloud-loader` / `--preload` /
  `--cloud-panel` / `--fdl-auth` / `--startup-check` / `--update-dialog` 等联网探针
- `MainShell`：删掉 ForceLogout 回调、EnvGuard 封禁提示、30s profile 轮询、启动更新检查
- `MainWindow`：删掉云端引导库的下载 / 填充 / 弹层逻辑
- `SettingsPage`：删掉「安全与隐私」整块、账号卡片、「上传日志」按钮
- `AboutPage`：删掉「检查更新」卡片
- `OnePlusFlashPage`：删掉「云端刷写包下载」整行
- `ScreenMirrorPage`：删掉 scrcpy 服务器下载，改本地查找

---

## 三、保留了什么（重要）

### 3.1 全部本地刷机能力

EDL / Qualcomm、FASTBOOT、Payload、欧加线刷、小米线刷、MTK、可视刷写、
主页、安卓工具、投屏、应用管理、文件传输、命令行、设置、关于。

底层保留：`core/`（EDL 引擎）、`storage/`（GPT / payload / OFP / OPS / sparse / super）、
`protocol/`（Sahara / Firehose）、`transport/`（串口）、`oplus/`（AES-CFB 解包）、
`kc/mtk/`（MTK 协议）、`kc/devices/`、`kc/tools/`（本地 adb 调用）。

### 3.2 三个"看起来像联网、其实是本地"的东西

| 保留项 | 为什么不是联网功能 |
|---|---|
| `kc/auth/SignCodec.*` | FDL 签名的十六进制/Base64 **解析**，纯字符串处理 |
| `ui/dialogs/FdlAuthDialog.*` | blob 只读展示 + sig 手工粘贴框，**本地弹窗**。小米 FDL 流程必需（工具不算签名） |
| `kc/auth/MembershipService.*` | 已精简为**空壳**：`guardAndConsume` / `ensureAllowed` / `consume` 一律 `return true`，无任何网络调用。保留类名只为不动 22+11+9 个调用点 |

> `MembershipService` 从 666 行精简到 40 行头文件。付费体系 2026-10-20 就已下线，
> 所有闸门本来就是放行状态，这里只是把残留的服务端裁决代码彻底删干净。

---

## 四、adb / fastboot 改为随包自带

原实现从服务器 `/assets/tools.zip` 下载。服务器关停后新用户就下载不了，
因此改成**随包自带**：

- 发布目录直接放 `adb.exe` / `fastboot.exe` / `AdbWinApi.dll` / `AdbWinUsbApi.dll`
- `PlatformToolsManager::ensureToolsExist()` 改为从**程序同目录**复制到缓存目录
- 缺失时给出明确提示，不再静默失败

实测：`adb.exe version` → 1.0.41 / 37.0.1；`fastboot --version` → 37.0.1。

> 为什么仍复制到缓存目录而不是直接用程序目录：`AdbGptReader` 等调用方按
> `adbPath()` 取路径，而它固定在缓存目录下 —— 保持单一路径口径。

---

## 五、改动文件清单

### 修改（15 个）

```
main.cpp
ui/MainShell.cpp            ui/MainShell.h
ui/MainWindow.cpp           ui/MainWindow.h
ui/pages/AboutPage.cpp      ui/pages/AboutPage.h
ui/pages/SettingsPage.cpp   ui/pages/SettingsPage.h
ui/pages/OnePlusFlashPage.cpp  ui/pages/OnePlusFlashPage.h
ui/pages/ScreenMirrorPage.cpp
kc/tools/PlatformToolsManager.cpp
kc/auth/MembershipService.cpp  kc/auth/MembershipService.h
CMakeLists.txt
```

### 删除（50 个）

见第二节。核心模块目录 `kc/net/`、`kc/update/`、`kc/cloud/` 已完全清空。

### 新增（2 个）

```
ui/dialogs/FdlAuthDialog.h    ui/dialogs/FdlAuthDialog.cpp   （重建为纯本地版）
```

---

## 六、遗留说明

1. **`MembershipService` 是空壳**：若日后要彻底删干净，把 42 个调用点删掉即可。
   现在保留是为了把改动风险控制在"能编译、能跑、能过测试"。
2. **`--adb-takeover` 探针在无头环境会卡住**（会拉起 adb server 等待），
   这是探针本身的特性，不影响正常使用。
3. **旧构建目录**（`build-verify` / `build-audit` / `build-release` 等）里仍有
   已删文件的 autogen 残留，那是历史产物，不影响新构建。正式出包请用
   `build-offline/`。
4. **`build-offline/` 里混入过旧 exe**：曾因 `cp dist/*` 覆盖导致版本显示 2.0.2，
   已重新链接修复。出包前请确认 `KunClient.exe` 是 6.3 MB 那个（不是 1.4 MB 的旧版）。
