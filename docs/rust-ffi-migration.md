# Rust 核心 + C# 前端 迁移方案

> 2026-09-27 制定。方向由用户确认：**放弃 Python UI；Rust 全量接管核心；改造现有 `KunClient.App`；最终收口单个 exe。**

---

## 1. 已实证的前提（不是推测）

| 结论 | 证据 |
|---|---|
| Rust `cdylib` 能被 C# 单文件加载 | `FfiProbe.exe` 2,580,512 B，复制到空目录仍跑通 |
| 无 mingw 运行时依赖，不必额外带 DLL | `kunflash_ffi.dll` 导入表 16 项全为系统 DLL |
| 单文件开关是**两个** | `IncludeAllContentForSelfExtract=true` 缺了就变成「exe+dll」 |
| 体积收益 | Python 46.1 MB → 纯 C# 6.17 MB → **C#+Rust ≈ 8.5 MB** |
| 门禁 | core 272 测试 / ffi 13 测试 / clippy `-D warnings` 干净 |

一键回归：`bash .workbuddy/tmp/ffi_probe/run.sh` → `FFI_PROBE: PASS`。

---

## 2. 边界划分

### 进 Rust（`kunflash-core` → `kunflash-ffi`）

`KunClient.Core` 17,453 行里，除下面「留 C#」两块之外的全部：

- `Protocol/`（Gpt / Sahara / Firehose）
- `Services/FlashSession`、`RawProgramProcessor`、`OfpDecryptor`、`OpsDecryptor`
- `Transport/`（WinUsb / QcUsbSerial）
- `Modules/Common`、`DynamicSuper`、`FileSystems`、`Images`

### 留 C#

| 模块 | 理由 |
|---|---|
| `PlatformTools/PlatformToolsManager` | 只是「启动 adb.exe/fastboot.exe + 走自家 API 下载解压 zip」，8 个页面在用。移植到 Rust 反而要把子进程输出跨 FFI 搬回来，纯增复杂度。**这是 UI 基础设施，不是核心算法。** |
| `Configuration/ClientConfig` | 软配置链涉及**程序集内嵌资源**（`KunClient.App` 里那份），跨 FFI 反而绕远 |
| App 侧全部（`MembershipService` / `UpdateService` / `AccountText` / 主题 / 动效 / 壁纸 …） | 本来就是 UI 层 |

---

## 3. FFI 面清单（按 UI 真实调用点反推）

统计口径：`grep` 出 `KunClient.App` 每个文件对 `KunFlashTool.Core.*` 的实际调用。

- **8 个页面**只用了 `PlatformTools`（留 C#）
- **`FlashSession` 只出现在 `EdlFlashPage.xaml.cs` 一个文件**，共 **26 个成员**
- **`EdlDeviceScanner.ScanAll`** 5 处
- **`QfilXmlParser.ParseRawProgram / ParsePatch`** 已在 FFI 里

### 3.1 要新增的导出（分组）

```
A. 设备枚举
   scan_devices()                    → [{path, display_name, kind}]

B. 会话生命周期（opaque handle）
   session_new(kind, port_path)      → handle
   session_free(handle)
   session_cancel(handle)
   session_state(handle)             → {is_connecting, is_connected, state, fault_reason}

C. 连接
   session_connect(handle, {loader_path, digest_path, signature_path, sahara_images})

D. 读取 / 信息
   session_read_partitions(handle)   → 分区列表（含每个 LUN）
   session_device_info(handle)       → {version, memory_name, sector_size, ...}
   session_version_info_text(handle)
   session_storage_brand_text(handle)

E. 写 / 擦 / 备份
   session_write_partition / write_gpt_image / erase_partition / erase_partitions
   session_erase_lun / fix_gpt
   session_backup_partitions / save_partition_to_file
   session_flash_image_directory    ← 整包刷写的重头

F. 其它操作
   session_wipe_data / unlock_oem / set_active_slot
   session_reboot(handle, mode)     ← mode: system | recovery | fastboot | fastbootd | 9008

G. 日志 / 进度（**拉取式**，见 §4）
   session_poll_logs(handle)         → [LogItem]，取走并清空
   session_poll_progress(handle)     → FlashProgress 快照

H. 解析（已完成）
   parse_patch_xml / parse_rawprogram_xml
```

---

## 4. 三个关键设计决策

### 4.1 日志与进度走**拉取（poll）**，不走回调 —— 这是必须的

现状是：

```csharp
_flashSession = new FlashSession(transport) {
    Log = msg => Dispatcher.Invoke(() => HandleEdlLog(msg)),      // 同步阻塞
    Progress = new Progress<FlashProgressEventArgs>(...)
};
```

`Dispatcher.Invoke` 是**同步**的 —— 它阻塞当前线程直到 UI 线程处理完。

如果照搬成「Rust 通过 C 函数指针回调 C# 委托」，就变成：

```
C# 刷写线程 → Rust 刷写逻辑 → 回调 C# 委托 → Dispatcher.Invoke → 等 UI 线程
                                                                      ↑
                                              UI 线程可能正在 Task.Run(刷写).Wait()
```

**这是一个教科书级的死锁。** 而且异常穿越 FFI 边界是 UB。

拉取式没有这些问题：方向永远是「C# 主动调 Rust」，无重入、无栈穿越、无跨边界异常。
C# 侧用 `DispatcherTimer`（或刷写线程内轮询）取日志，**跑在 UI 线程上，直接 `HandleEdlLog`，
连 `Dispatcher.Invoke` 都不需要了** —— 反而比现在更简单。

⚠️ **队列必须有上限**（建议 5000 条），超出丢最老的并补一条「日志过多已截断」。
否则长时间刷写（整包几十分钟）会把内存吃光。

### 4.2 取消通过 handle 显式传递

不把 C# 的 `CancellationToken` 传过 FFI。改为 `session_cancel(handle)` 置 Rust 侧的
`CancelToken`（内部是 `Arc<AtomicBool>`，core 里已有）。C# 侧把取消按钮直接接到这个调用。

### 4.3 每个会话一个 opaque handle，所有权规则同 §「统一规则」

`session_new` 返回 `Box::into_raw(Box::new(Session))`，`session_free` 负责 `drop`。
和 `string_free` 一样：**凡拿到的指针，都由配对的 free 函数释放，没有例外。**

---

## 5. 分阶段计划

| 阶段 | 内容 | 验收 |
|---|---|---|
| **0 ✅ 已完成** | FFI 骨架 + 单文件可行性 + 探针 | `FFI_PROBE: PASS` |
| **1** | A + B + C + D + G（设备枚举 / 会话 / 连接 / 读分区表 / 日志轮询）—— **垂直切片** | `EdlFlashPage` 的「检测设备 → 连接 → 读分区表」三条走 Rust，UI 行为不变 |
| **2** | E 组（写 / 擦 / 备份 / 整包刷写） | 沙盒（`FakeTransport`）+ 真实包探针双绿 |
| **3** | F 组 + 收尾 | 全 UI 巡检 `combo_probe2` 四条件仍「点不到 0、被裁 0」 |
| **4** | 删除 C# Core 中被取代的文件，只留 `PlatformTools` + `Configuration` | `dotnet publish` 单文件 + 体积对比 |

**阶段 1 之所以先做，是因为它同时验证了三件最容易翻车的事**：
handle 生命周期、日志轮询、以及「C# UI 能否在不知道 native 存在的情况下正常工作」。

---

## 6. 风险

| 风险 | 说明 | 对策 |
|---|---|---|
| **杀软拦截自解压** | 单文件会把 native dll 解到 `%TEMP%\.net\<名>\<哈希>\` 再加载。本机已出现过「杀软锁住刚写出的 exe」（`os error 5`） | **阶段 1 结束时必须在干净环境实测**；若被拦，退路是把 dll 作为内嵌资源 + `NativeLibrary.SetDllImportResolver` 自己解到可控目录 |
| `%TEMP%` 不可写 | 自解压失败 → 启动即崩 | 同上，退路方案可控目录 |
| 过渡期双实现并存 | C# 与 Rust 各有一份 `FlashSession` | 阶段 1 起，被 Rust 接管的路径**立刻删掉 C# 侧对应调用**，不留双轨 |
| 混淆闸门 | `deploy.py exe` 会检查明文 `/api/app/*` | FFI 是 native 调用，不受 WPF 混淆影响；发版前重跑闸门 |
| 首次启动解压开销 | 每次升级后首次运行要解 2.4 MB | 实测 < 100 ms 量级，可接受；若否可用 `SetDllImportResolver` |

---

## 7. 明确不做的事

- **不删** `package/python/` 与 `src/rust/kunflash-py`（冻结保留，可随时回查）
- **不移植** `PlatformToolsManager`（见 §2）
- **不改** 域名/会员/合规/主题等 App 侧逻辑
- **不用** native 回调（见 §4.1）
