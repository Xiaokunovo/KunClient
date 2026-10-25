# 刷写全面转 Rust + 三机型适配 · 执行计划

> ## ⛔ 已冻结（2026-10-08 晚）
>
> **用户指示：「这个项目所有 EDL 都是可以用的，你别用 Rust，直接用这个」** —— 指的是一份 Qt6/C++ 的
> EDL 工具 `SWT_EDL v0.11.0`（30 文件 / 8,294 行，Sahara + Firehose + GptParser + SparseImage +
> DynamicSuper + OplusVipSession）。
>
> - **EDL 走 SWT_EDL，不再用 Rust 重写**：见 **`docs/swt-edl-integration.md`**（本文件与它冲突时以它为准）。
> - 本文件**保留不删**：R0 已产出的东西（XML 解析 / CRC32 / JSON 信封 / ELF-MELF loader 解析 /
>   四厂商模型 / Sahara 帧编解码 / 会话日志队列；43+ 测试 + `FFI_PROBE: PASS`）是**已验证的资产**，
>   尤其 §2.1 那份对 `.melf` 的实测结构结论仍然有效。
> - **不要再往下推 R1/R2/R3**（设备枚举 / 连接 / 刷写引擎的 Rust 实现）。
> - 🔴 新增阻塞事实：本机**没有 C++ 工具链也没有 Qt6 开发包**，所以 SWT_EDL 现在也编不出来 ——
>   解阻塞步骤见 `swt-edl-integration.md` §4。

> 2026-10-08 制定。上游方案见 `docs/rust-ffi-migration.md`（2026-09-27，方向已确认）。
> 本文件是它的**执行版**：补上「现状取证 / 删 .NET 的正确顺序 / 米欧加红魔适配矩阵 / 体积账 / 死代码清单」。

---

## 0. 现状取证（先量再改，不是推测）

| 事实 | 证据 |
|---|---|
| Rust 工具链就绪 | `cargo 1.96.0` / `rustc 1.96.0`，已装 target `x86_64-pc-windows-gnu` |
| **Rust 源码已不存在** | `src/rust/` 目录不存在；全仓库 `Cargo.toml` 只有 4 个在 `.workbuddy/tmp/`（enum_probe / rustplan_probe / rustsmoke / rustspike），且它们引用的是 `../../../src/rust/kunflash-core`（**指向不存在的路径**） |
| 旧 FFI 产物只剩 dll | `.workbuddy/tmp/ffi_probe/kunflash_ffi.dll`（2,404,864 B）+ `pe_deps.txt` + `probe.txt`（Sep 27） |
| `package/` 与 `src/rust/kunflash-py` 同样不存在 | 上游方案 §7 说「冻结保留」，实际已丢 |
| 当前 C# 侧核心规模 | `KunClient.Core` = 17,453 行（含 Protocol/Services/Transport/Modules） |

**结论**：阶段 0 的结论（cdylib 可行、无 mingw 运行时依赖、单文件两个开关）仍然有效且被记录在 skill 里；
但**代码要从头重建**，不是「接着写」。这一步的代价必须先说清楚，别按「已完成 60%」排期。

---

## 1. 「删 net」的正确解读与顺序

用户原话：「能不能把 net 删除不需要 net 了，ui 继续使用 C#」。

**唯一自洽的解读**：删的是**C# 侧那套刷写实现**（`KunClient.Core` 的 Protocol / Services / Transport / Modules），
UI 继续用 C#（WPF）。理由：

- WPF **不支持 NativeAOT**（本机也没有 MSVC 链接器），所以「去掉 .NET 运行时」与「UI 用 C# WPF」不可能同时成立。
- 上游方案 §2「边界划分」+ §5 阶段 4 已经写明：进 Rust 的是核心算法，留 C# 的是 `PlatformTools` / `Configuration` / App 侧。
- 因此「不需要 net」= 不需要那份 .NET 写的**核心库**。

🔴 **执行顺序（顺序反了 = 现网功能直接失效）**：

```
① 建 Rust workspace 并让新路径能用（不动 C#）
② 逐条把 UI 调用点从 C# 实现切到 FFI（一条路径切完就删掉对应的 C# 实现，不留双轨）
③ 全部切完 → 删 KunClient.Core 里被取代的文件 / 包引用 → 量体积
```

**绝不先删**。`FlashSession` 目前是 `EdlFlashPage` 唯一依赖（26 个成员），删早了整页直接起不来。

---

## 2. 四机型适配矩阵（EDL / Firehose）

> ⚠️ 表里每一格都要真机过一遍才算数。**「实现状态」直接写进 Rust 代码**（`vendor.rs` 的 `Support`
> 枚举，并经 `kunflash_ffi_vendors()` 交给 UI），免得界面把「只留了接口」的厂商显示成可用。

| 环节 | 小米 | 欧加 | 联想 | 红魔 |
|---|---|---|---|---|
| 实现状态 | `Ported`（有 C# 可参考） | `Ported`（有 C# 可参考） | **`InterfaceOnly`**（无样本无代码） | `SampleVerified`（有真实 loader） |
| 进 9008 | `getvar token` → 填 sig → `stage` → `oem edl` | fastboot `oem edl` / 直插 9008 | 同红魔（用户口述） | 同联想（用户口述） |
| loader 形态 | 单个 Hexagon ELF | 单个 Hexagon ELF | 单个 Hexagon ELF（预期） | **`.melf` 多镜像容器** |
| 包格式 | 目录 + `rawprogram*/patch*.xml` | `.ofp` / `.ops`（需解密） | XML 目录包（口述） | XML 目录包（口述） |
| 签名 / 鉴权 | FDL Auth 填 sig | ofp/ops 解密 + 厂商认证 | 待验 | 待验 |

### 2.1 红魔 `.melf` 容器：已实测拆清（2026-10-08）

样本 `devprg_NP05J.melf`（1,659,300 B），逐字段量的结果：

| # | 偏移 | 类别 | machine | phnum | 长度 | 判断 |
|---|---|---|---|---|---|---|
| 0 | `0x0` | ELF32 | `0xF3` 高通 | 2 | 2,308 | 外层头 + Hexagon stub（两段正好铺到 `0x904`） |
| 1 | `0x904` | **ELF32** | **`0xF3`** | **12** | 195,696 | ← **firehose 编程器** |
| 2 | `0x30574` | ELF64 | `0xB7` AArch64 | 9 | 961,504 | ARM64 侧镜像 |
| 3 | `0x11B154` | ELF64 | `0xB7` AArch64 | 7 | 303,096 | ARM64 侧镜像 |
| 4 | `0x16514C` | ELF64 | `0x1` | 9 | 196,696 | 未知侧 |

三条硬结论：

1. **容器内没有镜像目录表**：各内嵌偏移与镜像长度在表头区（`0x0..0x904`）里按 32 位小端搜过，
   一次都没命中；全文件也没有 `MELF`/`melf` 字样。→ **只能靠扫 ELF 魔数 + 校验头枚举**，
   所以 `elf::parse_header` 那套「结构自洽才算真头」的判据是必需品（`e_ehsize`/`e_phentsize`/表不越界）。
2. **编程器 = `machine == 0xF3` 且 `phnum >= 4` 的内嵌 ELF**。外层也是 `0xF3` 但只有 2 段，
   所以判据必须带 phnum 门槛。实测编程器 12 个程序头，其中**带数据的 `PT_LOAD` 3 段**
   （`#5/#6/#7`，合计 **177,184 B**），另有一个 `filesz=0 / memsz=24720` 的 `.bss` 段。
3. 机器码是 **`0xF3`(243)**，不是标准 `EM_HEXAGON(164)` —— 改成 164 会把这个文件判成非高通镜像。

### 2.2 红魔这条线**还没验**的（真机才能定，别当已实现）

- 送 Sahara 的段集合：只发 `PT_LOAD`，还是**也要发**那两个 `type=0` 的高通私有段（`filesz=436` / `11376`）。
- 镜像 `#2/#3/#4`（AArch64 侧）在刷写流程里要不要发、在哪个阶段发。
- loader 之后的鉴权/下发细节。

离线自查入口：`cargo run --offline -p kunflash-core --example melf_inspect -- "<path>"`，
拿到新机型的 devprg **先跑它**确认编程器挑对了再上真机。

---

## 3. FFI 面（沿用上游 §3，补 vendor 与 loader）

原清单 A~H 不变，另加：

```
C'. session_connect(handle, { loader_path, digest_path, signature_path, sahara_images, vendor })
      vendor: xiaomi | oplus | lenovo | redmagic
E'. session_flash_image_directory(handle, dir, vendor, pack_format)
      pack_format: xml | ofp | ops | vendor_raw
```

**R0 已实现的导出**（`.workbuddy/tmp/ffi_probe/run.sh` 有断言）：

```
kunflash_ffi_abi_version / _abi_version_number / _crc32 / _string_free
kunflash_ffi_parse_patch_xml / _parse_rawprogram_xml
kunflash_ffi_vendors          → [{id, name, support, expectedLoader}]  ← UI 厂商下拉框的唯一来源
kunflash_ffi_loader_inspect   → 形态(单ELF/melf) + 内嵌镜像表 + 选中的编程器与段清单
```

三条硬约定照搬 skill `rust-csharp-ffi-singlefile`：
统一 JSON 信封 / 每个导出挂 `catch_unwind` / **凡返回 `*mut c_char` 一律由调用方 free（不留例外）**。
日志与进度走**拉取**（`session_poll_logs` / `session_poll_progress`），不用 native 回调 —— 避免
`Dispatcher.Invoke` 死锁（上游 §4.1 已论证）。

---

## 4. 体积账（实测，Release 单文件）

| 组件 | 大小 | 转 Rust 后 | 备注 |
|---|---|---|---|
| `SharpCompress.dll` | **2.28 MB** | ✅ 可去 | 欧加 ofp/ops 解密用；Rust 侧用 `zip`/`flate2` |
| `HandyControl.dll` | **1.43 MB** | ✅ 可去（与 Rust 无关） | 全项目 `hc:` 元素数 = **0**，只有 App.xaml 合并了它的主题字典 → **纯死重** |
| `Newtonsoft.Json.dll` | 0.71 MB | ✅ 可去 | App 侧已在用 `System.Text.Json` |
| `Google.Protobuf.dll` | 0.50 MB | ✅ 可去 | payload 元数据解析进 Rust |
| `System.Management.dll` | 0.31 MB | ✅ 可去 | 设备枚举进 Rust（`setupapi`/`cfgmgr32`） |
| `KunClient.Core.dll` | 0.41 MB | ✅ 可去 | 就是被取代的那份 |
| `LibUsbDotNet.dll` + `System.IO.Ports.dll` | 0.17 MB | ✅ 可去 | Rust `nusb` 接管 |
| `KunClient.dll`（UI 本体） | 1.16 MB | 保留 | WPF UI |
| **合计可去** | **≈ 5.8 MB** | | 需新增 `kunflash_ffi.dll` **2.29 MB** |

→ **理论净收益 ≈ −3.5 MB**（7.48 MB → 约 4.0 MB 量级，未计 .NET 宿主自身开销）。
其中 **HandyControl 那 1.43 MB 现在就能拿**（与 Rust 无关），但必须走完整视觉巡检，别只看探针。

---

## 5. 死代码 / 垃圾清单

**已删（2026-10-08）**：

- `tests/src/KunClient.App/KunClient.App_b4vfukgi_wpftmp.csproj`（40 KB，XAML 编译器残留；里面还引用着不存在的 `E:\flashtool\src\KunClient.Core\bin\Release`）
- `tests/src/KunClient.Core/Services/FlashSession.cs.bak.20261007121404`（182 KB，3,600 行备份混在源码树里）
- `tests/src/KunClient.App/Assets/logo-k-bare.png.bak`（103 KB）

**待清（需确认）**：

| 目标 | 证据 | 风险 |
|---|---|---|
| ~~`HandyControl` 包引用 + App.xaml 两处字典合并~~ | **已删（2026-10-08）**：`hc:` 用 0 处 | — |
| ~~`Newtonsoft.Json` 包引用（Core）~~ | **已删（2026-10-08）**：Core/App 用 0 处 | — |
| `src/KunClient.App/obj`（幽灵 `src/` 路径） | 真实工程在 `tests/src/`；slnx 里也没有 `src/` | 低 |
| 仓库根 `old_part.txt` / `run.log` / `dlgharness_trace.log` / `tmp/` | 全是历史产物 | 低（但可能是排障证据，先问） |
| `KunClient.Shell`（不在 slnx） | 加密载荷外壳，属打包链路 | 中：先确认 `obfuscate-build.js` 是否还用它 |

---

## 6. 分阶段（每阶段可独立验收）

| 阶段 | 内容 | 验收 |
|---|---|---|
| **R0 ✅ 已完成（2026-10-08）** | `src/rust/{kunflash-core,kunflash-ffi}` 从零重建；XML 解析 / CRC32 / JSON 信封 / 三条 FFI 硬约定；外加 **ELF+MELF loader 解析**与**四厂商模型** | `FFI_PROBE: PASS`；Rust 43 测试；`clippy -D warnings` 干净；**真实 `devprg_NP05J.melf` 解析通过** |
| **R1** | 设备枚举 + 会话 + 连接 + 读分区表 + 日志轮询（**垂直切片**） | `EdlFlashPage` 的「检测→连接→读分区表」三条走 Rust，UI 行为不变 |
| **R2** | 写 / 擦 / 备份 / 整包刷写 + **四机型 vendor 分支** | 沙盒 `FakeTransport` 双绿；红魔那条要用真机确认 Sahara 段集合（见 §2.2） |
| **R3** | 删除 C# 侧被取代的实现与包引用；量体积 | 单文件 + 体积对比 + 全 UI 巡检 |
| **R4** | 死代码清单收口（`SharpCompress`/`Protobuf`/`System.Management` 等） | 逐页截图复核 |

---

## 7. 硬约束（不要踩）

- 继续用 `x86_64-pc-windows-gnu`；产物**先跑 `pe_deps.py`** 确认没拖 mingw 运行时（否则用户机只报「找不到指定的模块」）。
- 单文件必须同时有 `IncludeNativeLibrariesForSelfExtract` **和** `IncludeAllContentForSelfExtract`（后者才是关键）。
- 跨 FFI 的字符串入参一律 `[MarshalAs(UnmanagedType.LPUTF8Str)]`（`CharSet.Ansi` 会让中文分区名静默变乱码）。
- 小米 stage 产物**只含 sig、不写 blob** —— 这条是已确认的硬约束，别在 Rust 侧「顺手补全」。
