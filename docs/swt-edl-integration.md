# EDL 改用 SWT_EDL（C++/Qt6）· 集成方案

> 2026-10-08 制定。**方向由用户确认**：「这个项目所有 EDL 都是可以用的，你别用 Rust，直接用这个」。
> → 本文件取代 `docs/rust-flash-migration-plan.md` 里「用 Rust 重写刷写」的路线。
> Rust 那边已产出的代码**不删、冻结保留**（见 §6）。

---

## 1. SWT_EDL 是什么（实测，不是推测）

| 项 | 实测结果 |
|---|---|
| 工程名 / 版本 | `SWT_EDL v0.11.0`（CMake `project(SWT_EDL VERSION 0.11.0 LANGUAGES CXX)`） |
| 形态 | **Qt6 GUI 程序**：`qt_add_executable(SWT-EDL WIN32 …)`，入口是 `src/ui/MainWindow` |
| 依赖 | Qt6 ≥ 6.5 的 **Core / Widgets / SerialPort / Xml** |
| 规模 | `src/` 下 **30 个 .cpp/.h，合计 8,294 行** |
| 分层 | `core/`（Types、DeviceIdentity、PartitionProtection）、`transport/`（SerialTransport）、`protocol/`（Sahara、Firehose）、`storage/`（GptParser、RawProgramParser/Writer、SparseImage、DynamicSuper、SystemVersionReader）、`oplus/`（**OplusVipSession**）、`app/`（EdlWorker）、`ui/`（MainWindow） |
| 资源 | `resources/{dark,light}.qss`、logo、qrc；`tests/` 有 `GptParserTest`、`MainWindowLogTest` |

### 1.1 厂商覆盖（这一条决定了「所有 EDL 都能用」的含义）

源码里搜关键词的**命中文件数**：

| 关键词 | 命中文件 |
|---|---|
| `oplus` | **13** |
| `vip` | **12** |
| `xiaomi` | 1（仅 `protocol/Sahara.cpp` 里出现） |
| `lenovo` | 1（同上） |
| `redmagic` / `nubia` / `红魔` | **0** |

→ **EDL 协议本身是厂商中立的**（Sahara + Firehose），所以「所有 EDL 都能用」指的是
**通用刷写链路对任何 9008 设备都成立**（只要有对应 loader + XML 包）；
唯一有厂商专属逻辑的是 **欧加的 VIP 鉴权**（`OplusVipSession`）。
这与用户此前口述的「红魔进 9008 和联想一样、刷写包都差不多」**一致** —— 红魔不需要专门适配。

### 1.2 能力面（`src/app/EdlWorker.h`，与我们的页面一一对得上）

```text
connectDevice(port, programmerPath, …)      ← 连接（loader）
disconnectDevice()
readPartitionTable()                        ← 读分区表 → partitionsReady 信号
readSystemVersionInfo()
backupPartition(s)                          ← 备份
flashPartition(s) / flashXml(xmlPaths, …)   ← 刷写；**整包走 XML**（对应 rawprogram/patch）
erasePartition(s)                           ← 擦除
rebootDevice(toEdl)                         ← 重启 / 回 9008
requestStop()                               ← 取消

信号：logMessage / stateChanged / progressChanged / overallProgressChanged
      / deviceInfoChanged / partitionsReady / operationFinished
```

**关键结构事实**：`EdlWorker` 是个纯 `QObject` 工作器，**不依赖 MainWindow**（GUI 只是它的一个消费者）。
这条决定了 §3 的推荐路线可行。

---

## 2. 当前机器**编不出来**（硬阻塞，已取证）

| 需要的东西 | 本机实测 |
|---|---|
| C++ 编译器 | **没有**。`g++/gcc --version` → `command not found`；在 `C:\Users\Administrator\AppData`、`E:\`、`D:\`、`C:\Program Files*`、`E:\soft`、`E:\work` 里按 `g++.exe` 搜过，**无命中** |
| CMake | **没有**（同上，`cmake.exe` 无命中） |
| Qt6 开发包 | **没有**。`Qt6Core.dll` 唯一命中在 `C:\Program Files\obs-studio\bin\64bit\` —— 那是 OBS 自带的**运行时**，没有头文件也没有 `Qt6Config.cmake`，编不了 |
| 预编译的 SWT-EDL.exe | **没有**（`Downloads`、`E:\flashtool`、`D:\` 里搜 `*SWT*EDL*` 无命中） |

> ⚠️ 这台机器历史上是「Rust 走 MSYS2、无 MSVC cl.exe」的环境；但**当前 PATH 与常见安装位置都没有 MSYS2**，
> 所以我既不能编 SWT_EDL，也不能跑它的 `tests/`。

---

## 3. 三条集成路线（推荐 B）

| 路线 | 做法 | 代价 | 能否自动化 |
|---|---|---|---|
| **A. 并列交付** | 把 SWT-EDL.exe 作为**独立工具**随包分发，用户在它自己的界面里刷 | 最小；零改代码 | ❌ 两套 UI 并存，KunClient 的 EDL 页变成「打开 SWT-EDL」 |
| **B. 给它加 headless CLI（推荐）** | 在 SWT_EDL 里加一个 `--cli`/`--json` 模式：**复用 `EdlWorker`**，不起 MainWindow，把 log/state/progress 以 JSON 行打到 stdout；KunClient 以子进程方式驱动 | 需要改 SWT_EDL（但 `EdlWorker` 已与 GUI 解耦，改动集中在新增一个入口 + 一个 headless 控制器） | ✅ 完整自动化，100% 复用它的 EDL 逻辑 |
| **C. 源码内联成库** | 把 SWT_EDL 的 `core/transport/protocol/storage/oplus` 抽成静态库，加 C ABI 给 C# 调 | 最重；且照样得有 Qt6 才能编 | ✅ 但要重做一遍 FFI，等于把 Rust 那条路换语言再走一遍 |

**推荐 B**，理由：`EdlWorker` 的能力面已经覆盖我们 EDL 页需要的一切（连接/读分区/备份/刷写/擦除/重启/取消 + 5 条进度与状态信号），
而它与 GUI 解耦，新增一个 headless 入口的改动量小、风险低、可回归。

### 3.1 路线 B **已实现**（2026-10-08）

源码已收进仓库：**`third_party/swt-edl/`**（含我加的 CLI；原 Downloads 副本保持不动）。
新增/改动文件：

| 文件 | 作用 |
|---|---|
| `src/cli/HeadlessController.h/.cpp` | `--cli` 的控制器：把 `EdlWorker` 的能力暴露成 NDJSON 协议；含协议文档（事件/指令/退出码） |
| `src/cli/StdinReader.h/.cpp` | **独立线程**里阻塞读 stdin（主线程必须留给事件循环，否则事件一条都发不出去） |
| `src/cli/HeadlessMain.cpp` | `--cli` 入口：用 `QCoreApplication`（不建窗口系统、不建 MainWindow） |
| `src/main.cpp` | 在**构造 QApplication 之前**判 `--cli` 并分叉 |
| `src/core/Types.h` | 新增 `stateKey()`：状态的**稳定机器标识**（`stateText` 是中文给人看的，两者刻意分开） |
| `CMakeLists.txt` | 把上面 5 个源文件加进 `qt_add_executable(SWT-EDL …)` |

配套（都在 `E:\flashtool\.workbuddy\tmp\`）：

- `build-swt-edl.sh` —— 一键构建 + 跑它自带的 ctest（`--deploy` 会额外 `windeployqt6`）
- `swt_edl_cli_probe.py` —— CLI 验收探针，**8 条判据全不需要真机**（ready/ping-pong/错误路径/坏 JSON/连接失败不崩/NDJSON 合法性/quit 退出码）

### 3.2 协议（与实现一致）

```text
SWT-EDL.exe --cli                       # 进入 headless；stdin 收指令、stdout 出 NDJSON

→ {"event":"ready","version":"0.11.0","protocol":"ndjson","commands":[…]}
← {"cmd":"ping","seq":1}
→ {"event":"pong","seq":1}
← {"cmd":"connect","port":"COM4","programmer":"…melf","storage":"ufs","vip":{…}}
→ {"event":"state","value":"saharaHandshake","state":2,"text":"Sahara 握手","detail":""}
→ {"event":"log","level":"info","text":"…"}
→ {"event":"device","port":"COM4","storage":"ufs","serial":"…", …}
← {"cmd":"readPartitions"}
→ {"event":"partitions","count":42,"items":[{"name":"super","lun":0,"firstLba":…,"lastLba":…,"sectors":…}]}
← {"cmd":"flashXml","paths":["…\\rawprogram0.xml"],"allowProtected":false}
→ {"event":"progress","completed":123,"total":456}
→ {"event":"finished","success":true,"message":"…"}
← {"cmd":"cancel"} / {"cmd":"reboot","toEdl":true} / {"cmd":"quit"}
→ {"event":"bye"}   （随后进程正常退出，退出码 0）
```

指令全集：`ping / connect / disconnect / readPartitions / readVersionInfo / backup / flash / flashXml / erase / reboot / cancel / quit`。
`flash|erase|backup` 支持两种写法：单件（`name` + `lun`）或批量（`names`）；批量 `flash` 的 `images` 个数必须与分区数一致
（不一致直接 `error`，不会猜）。分区名靠 `readPartitions` 的快照解析成 LUN/LBA，所以**必须先读一次分区表**。

要点（都是从我们这边踩过的坑里来的）：

- **日志与进度是事件流**，不是「只回结果」—— 否则长任务期间界面只能干等。
- 每行一个 JSON（NDJSON），**不做多行美化**：跨进程按行解析最稳，也不怕日志正文里的换行（会被转义）。
- **每行 flush**；否则父进程长时间收不到事件，表现成「界面卡住」。
- 显式 **UTF-8**（Windows 控制台默认 GBK，中文分区名会整成乱码**且不报错**）。
- 退出码只表达 CLI 自身：`0` 正常、`2` 参数/启动问题；**刷机失败走 `finished.success=false`**（刷机失败不是 CLI 崩溃）。
- 自定义类型跨线程投递前必须 `qRegisterMetaType`（`StorageType/PartitionInfo/DeviceInfo/OplusVipPackage` 等）。
- 本目标是 WIN32（GUI 子系统）可执行文件：**用管道重定向时 stdio 照常可用**，但直接在 cmd 里跑看不到输出
  （要 `SWT-EDL.exe --cli < in.txt > out.txt`）。


---

## 4. 解阻塞：工具链怎么装（二选一）

### 4.1 MSYS2 + Qt6（本机既有路线，推荐）

```bash
# 装 MSYS2 后，在 MINGW64 shell 里：
pacman -S --needed mingw-w64-x86_64-qt6-base mingw-w64-x86_64-qt6-serialport \
                   mingw-w64-x86_64-qt6-tools mingw-w64-x86_64-cmake \
                   mingw-w64-x86_64-ninja mingw-w64-x86_64-gcc
# 构建：
cmake -S <SWT_EDL 目录> -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
# 部署 Qt 运行库（否则 exe 在别的机器上缺 Qt6Core.dll 起不来）：
windeployqt6 build/SWT-EDL.exe
```

### 4.2 Qt 官方在线安装器

装 Qt 6.5+ 的 `MSVC 2022 64-bit` 或 `MinGW 64-bit` 套件（含 Qt Serial Port、Qt Xml）+ CMake + Ninja。
⚠️ 若走 MSVC 套件，本机**没有 MSVC 编译器**（历史上确认过），得同时装 VS Build Tools —— 所以 4.1 更省事。

---

## 5. 验收标准（改完必须过）

| 项 | 判据 |
|---|---|
| 能编 | `cmake --build build` 零错误；`tests/`（GptParserTest、MainWindowLogTest）通过 |
| CLI 可用 | `--cli --json` 下 `{"cmd":"connect"}` → `{"event":"state","value":"connected"}`；日志是 NDJSON |
| 行为一致 | 用同一份 XML 包 + 同一个 loader，SWT_EDL 与现有 C# EDL 对**同一台设备**的分区表读数一致（分区名/起始/大小逐项对齐） |
| 无回归 | KunClient 关掉 EDL 走 SWT_EDL 之后，combo_probe2 三档仍全绿 |
| 体积 | 记录 SWT-EDL.exe + Qt 运行库的总体积；若过大，讨论是否只保留用到的 Qt 模块 |

---

## 6. Rust 那条线的处置（**冻结，不删**）

已产出并已有门禁覆盖的部分（`src/rust/{kunflash-core,kunflash-ffi}`：XML 解析、CRC32、JSON 信封、
ELF/MELF loader 解析、四厂商模型、Sahara 帧编解码、会话日志队列，43+ 测试 / `FFI_PROBE: PASS`）
**按用户指示不再继续投入**，但**不删除**：

- 它是**已验证可用的资产**，删掉等于把已经量过的结论（尤其 §2.2 的 melf 实测结构）一起丢掉；
- `docs/rust-flash-migration-plan.md` 顶部已加冻结说明，指向本文件。

⚠️ **本文件与那份文档冲突时，以本文件为准**（EDL 走 SWT_EDL）。

---

## 7. 落地形态改为「路线 A」（2026-10-09，用户拍板）

用户指令：「**使用这个项目所有功能来替换现在工具的功能**」，并在三选一里选定
**「直接改用 SWT-EDL 自带界面」** —— 即

> 把 SWT-EDL 作为独立刷机工具分发，用户直接用它自己的 Qt 界面刷机；
> KunClient 里的 EDL 页退役，只留一个「打开 SWT-EDL」入口。

所以 §3 的「路线 B（headless CLI 子进程驱动）」**不再作为主线**（CLI 仍保留在源码里，
也已修好并全绿，见 §7.4 —— 它是「将来想再切回内嵌驱动」的现成资产）。

### 7.1 构建工具链（§2「编不出来」已解阻塞）

| 项 | 现状 |
|---|---|
| MSYS2 | `E:\msys64`（**一直存在**，§2 当时搜漏了）—— 脚本 `E:\msys64\install_qt6.sh` |
| Qt6 | `mingw-w64-x86_64-qt6-{base,serialport,tools} 6.11.2`（首次装到一半中断，见 §7.2） |
| 编译器 | `g++ 16.2.0` / `cmake 4.4.4` / `ninja` |
| 构建脚本 | `.workbuddy/tmp/build-swt-edl.sh`（`--deploy` 额外打包；**windeployqt6 在本机不可用**，见 §7.3） |

### 7.2 踩过的两个坑（都已修，别重踩）

1. **pacman 数据库锁 + 包只解了一半**。装 Qt6 时被中断，留下 `var/lib/pacman/db.lck`，
   且 `mingw-w64-x86_64-cmake` 的 local DB 条目只剩 `mtree`（文件只解了一半）。
   症状：`cmake` 报 **`System is unknown to cmake`** + 缺 `Modules/Internal/HeaderpadWorkaround.cmake`。
   修法：删 `db.lck`；`rm -rf /var/lib/pacman/local/mingw-w64-x86_64-cmake-*`；
   再 `pacman -U --noconfirm --overwrite '*' /var/cache/pacman/pkg/mingw-w64-x86_64-cmake-*.pkg.tar.zst`。
   修复判据：`Modules/` 文件数 **786 → 1686**，`Modules/Platform/Windows.cmake` 就位。
   （脚本：`.workbuddy/tmp/fix_cmake.sh`）

2. **`windeployqt6` 在本机必然失败**：`Unable to query qtpaths: Error running binary qtpaths: pipe:`。
   而 `qtpaths6.exe --qt-version` 单独跑是好的（6.11.2）—— 是它 QProcess 起子进程那一步被拦，
   与「无控制台 + 杀软」有关，不是 Qt 装坏了。
   → 改用 **ldd 求依赖闭包的手工部署**：`.workbuddy/tmp/deploy_manual.sh`
   （27 个依赖 DLL + `platforms/qwindows.dll` + `styles/qmodernwindowsstyle.dll`）。

### 7.3 修掉的两个 SWT-EDL 真 bug（都只在 CLI 路径，GUI 不受影响）

| 文件 | 问题 | 判据 |
|---|---|---|
| `src/cli/StdinReader.cpp` | `std::fread(buf, 1, 65536, stdin)` 的契约是**读满 65536 字节或遇 EOF 才返回** → 父进程保持管道打开时一条指令都不处理（`printf … \| exe` 因为 printf 立刻退出=EOF，看起来"正常"，把 bug 掩盖了）。改成 `_read()`/`read()` 原始 fd 读 | CLI 探针 6 PASS/7 FAIL → 全绿 |
| `src/cli/HeadlessMain.cpp` | `stdinThread.quit()+wait(2000)` 叫不醒阻塞在 `_read` 的线程 → `~QThread` 判定"线程仍在运行"→ `qFatal` → **abort，退出码 `0xC0000602`（STATUS_FAIL_FAST_EXCEPTION）**，父进程会当成"CLI 崩了"。改成 device 侧收尾后直接 `std::_Exit(rc)` | 退出码 `3221227010` → `0` |

### 7.4 验收记录（全部实测，2026-10-09）

| 项 | 结果 |
|---|---|
| 编译 | `cmake --build` 零错误；产物 `SWT-EDL.exe` **1,197,543 B** |
| SWT-EDL 自带 ctest | `gpt-parser` / `ui-log` **2/2 Passed** |
| CLI 探针 `swt_edl_cli_probe.py` | **PASS=13 / FAIL=0**（`ready`/NDJSON 纪律/ping-pong/未知指令/坏 JSON/缺 cmd/连接不存在串口不崩/UTF-8/quit 退出码 0） |
| GUI | `--screenshot` 抓到完整界面（分区表 / 日志 / 设备信息 / 双进度 / 按钮带），深色主题正常 |
| 分发包自洽 | 解压到 `E:\flashtool\.workbuddy\tmp\swt_verify`，**PATH 里剥掉 `/mingw64/bin`** 仍能启动（rc=0 + 截图） |
| 体积 | 解压后 **76 MB / 30 文件**（`libicudt78.dll` 31.6MB 最大）；zip **30.47 MB** |

### 7.5 客户端侧改动（路线 A）

| 文件 | 改动 |
|---|---|
| `tests/src/KunClient.App/Services/SwtEdlService.cs` | **新增**：`Dir`（走 `AppStatePaths.ResolveCache("swt-edl", …)`，**跨会话保留**、退出清理不动）、`FindExe()`（**递归找**，因为 zip 带一层 `SWT-EDL-0.11.0/` 顶目录）、`EnsureAsync()`（`ChunkedDownloader` 分片下载 + 解压）、`Launch()` |
| `tests/src/KunClient.App/Pages/SwtEdlPage.xaml(.cs)` | **新增**：入口页（左列「引擎状态」+「日志」，右列「打开 SWT-EDL」/「重新下载引擎」/「打开安装目录」/「清空日志」） |
| `tests/src/KunClient.App/MainWindow.xaml.cs` | `Groups["edl"]` 里 `PageDef("EdlFlash","Qualcomm")` → **`PageDef("SwtEdl","SWT-EDL")`** |

🔴 **故意保留的东西**（别当垃圾删）：
- `Pages/EdlFlashPage.xaml(.cs)` **文件不删**（还有探针与历史取证引用它，且删了要动 1000 行），只是**不再挂到导航上**。
- 其余刷机页全部保留 —— SWT-EDL **不会**做下列客户端独有能力，删了是净损失：
  `XiaomiEdlPage`（sign.bin → stage → oem edl 的进 EDL 流程 + FDL Auth）、
  `OnePlusFlashPage`（.ofp/.ops 解密 + Payload）、`FastbootFlashPage` / `XiaomiFastbootPage`（fastboot 协议）、
  `PayloadPage`（payload.bin 解析）。**SWT-EDL 只覆盖 Qualcomm 9008 这条链路。**

### 7.6 分发包落点（与 scrcpy 同一条链路）

本地已产出 **`server/assets/swt-edl.zip`（30.47 MB，30 条目，sha256 `7cf153f8…7210`）**，
顶层目录 `SWT-EDL-0.11.0/`，客户端按 `{ServerUrl}/assets/swt-edl.zip` 取。
打包脚本：`.workbuddy/tmp/pack_swt_edl.py`（含"解压到异地 + 干净 PATH 启动"的脱环境验收）。

⏳ **尚未上线**：`server/assets/swt-edl.zip` 还需上传到生产（`deploy.py` 目前没有 assets 根的通用模式，
只有 `drivers`；上线时要么加一个模式，要么沿用 drivers 的写法）。**未上传前客户端点「打开 SWT-EDL」会下载失败。**

### 7.7 客户端验收

| 探针 | 结果 |
|---|---|
| `combo_probe2` × 4 档（1180×760 / 1000×640 × light/dark） | **ALL_PASS，UI 问题 0 条**；`NavSwtEdl` 点不到 0 / 被裁 0 |
| `dotnet test` | **249 通过 / 0 失败** |
| `dlg_probe` | **PASS** |
| `view_probe` | **FAIL=0**（SKIP=43 / OK=36；`SwtEdlPage` 第 3 段真实构造 OK） |

🔴 **探针自身的三个历史断链，本次一并修好**（都不改产品代码）：
1. `dlg_probe` / `view_probe` 的 csproj 还指着 `..\..\..\src\KunClient.App`（真实工程早迁到 `tests\src\`）→ `BG1002` 编不出。
2. `dlg_probe` / `dlgharness` 还在合并**已被删除**的 HandyControl 字典 →
   `PackWebResponse.get_ContentType()` 直接崩在 `Main`。
3. `view_probe` 第 2 段（`Application.LoadComponent` 逐个加载 BAML）把「带 `x:Class` 的 BAML 二次注册
   x:Name / 资源键」当成 FAIL —— 这是**机制性假阳性**（构造函数里的 `InitializeComponent`
   已经把这份 BAML 应用过一遍）。已改判 SKIP。
   **取证**：同一份探针跑**改动前**的 `KunClient.dll`（2026-10-08 版）同样报 27 条同类 FAIL，
   改后 28 条 —— 差值 1 = 只多了新页 `pages/swtedlpage.xaml` 一个，与本次改动无关。

---

## 8. 最终落地：**严格逐文件移植**（2026-10-09 晚，用户改口径）

用户澄清：「**把他 edl 所有功能移植到现在工具，edl 的所有功能替换掉，UI 也改成一样的**」，
并在「移植口径」三选一里选了 **「严格逐文件重写」** —— 即把 SWT-EDL 的 C++ **逐文件翻成 C#**
放进 KunClient，再把现有的 C# EDL 栈删掉。

→ **§7 的「路线 A（外挂独立 exe）」作废**：`Pages/SwtEdlPage.*`、`Services/SwtEdlService.cs`
和 `server/assets/swt-edl.zip` 都已删除（那套外挂入口与本口径冲突）。

### 8.1 移植产物：`tests/src/KunClient.Core/SwtEdl/`（**7,061 行**，一次编译通过）

| C# 文件 | 行 | 对应 C++ |
|---|---|---|
| `Types.cs` | 161 | `core/Types.h` |
| `DeviceIdentity.cs` | 159 | `core/DeviceIdentity.*` |
| `PartitionProtection.cs` | 54 | `core/PartitionProtection.*` |
| `Transport/SerialTransport.cs` | 385 | `transport/SerialTransport.*` |
| `Protocol/SaharaClient.cs` | 695 | `protocol/Sahara.*` |
| `Protocol/FirehoseClient.cs` | 1225 | `protocol/Firehose.*` |
| `Storage/GptParser.cs` | 120 | `storage/GptParser.*` |
| `Storage/SparseImage.cs` | 205 | `storage/SparseImage.*` |
| `Storage/RawProgramParser.cs` | 350 | `storage/RawProgramParser.*` |
| `Storage/RawProgramWriter.cs` | 124 | `storage/RawProgramWriter.*` |
| `Storage/DynamicSuperPlanner.cs` | 573 | `storage/DynamicSuper.*` |
| `Storage/SystemVersionReader.cs` | 817 | `storage/SystemVersionReader.*` |
| `Oplus/OplusVipSession.cs` | 204 | `oplus/OplusVipSession.*` |
| `App/EdlWorker.cs` | **1989** | `app/EdlWorker.*`（Qt 信号 → C# `event`，同步 invoke） |

固定约定：`<bool>(..., QString* error)` → `bool Xxx(..., out string error)`（不用异常替代返回值）；
`std::atomic_bool` → 新增的 `EdlCancel`（**不能用 `CancellationToken`：那个不可重置**，而每个新操作都要清上一轮的取消态）；
`QFile/QIODevice` → `FileStream/Stream`；中文文案**一字未改**。

### 8.2 界面：`Pages/EdlFlashPage.xaml(.cs)` 按 SWT-EDL 的版式重做

来源 `ui/MainWindow.cpp` 的 `buildLeftCard` / `buildRightCard`，两列卡片：
左 = 串口/自动扫描/存储 + 分区表（□｜分区名｜文件名｜大小｜状态）+ rawprogram XML + firehose/模式 +
芯片/查找分区 + 5 个选项 + 5 个主操作按钮；
右 = 操作日志（含 Debug 开关）+ 设备信息 3×4 读数 + 状态横幅 + 进度区（当前任务/总体进度/速率/重启/停止）。
代码后置 **1,727 行**（整份替换旧的 3,501 行），只依赖 `Core.SwtEdl.*`。

🔴 **与源界面的三处有意取舍**（都不是功能缺失，已在 XAML 注释里写明）：
1. 源程序顶部的「快速操作 / 高级功能 / 辅助功能」与卡片内「刷机界面 / 其他界面」四个按钮里，
   后三个是 `setEnabled(false)` 的死占位 → 不再重复摆（外壳菜单栏 + 页签区已承担导航）。
2. 「操作日志」旁那个 checkable 按钮 → 用**复选框**表达（WPF 里 Button 样式套不到 ToggleButton 上）。
3. 表头「全选」复选框、`QSettings` 持久化（自动扫描/自动重启/保护分区/读取信息/生成XML）、
   页脚「协议通信状态」——分别由「外壳 `ShellStatus` 承担」「页面内字段（未落盘）」「本轮未做」处理。
   ⚠️ 进度/状态**同时**上报 `ShellStatus`，所以外壳的两条通栏进度条也会跟着走。

### 8.3 删掉的现有 C# EDL 栈（**46 个文件**，已备份可还原）

备份目录：**`.workbuddy/tmp/deleted-old-edl-stack/`**（825 KB，什么都没真删）。
- Core **26 个**：`Protocol/{Sahara×5, Firehose/{FirehoseClient,FirehoseOptions,FirehoseSpoof}, Gpt×3, ProtocolExceptions}`、
  `Transport/*`（6：EdlDeviceScanner/EdlPortInfo/IEdlTransport/LibUsb/WinUsb/QcUsbSerial）、
  `Services/{EdlManager,FlashSession,FlashProgressEventArgs,RawProgramProcessor}`、
  `DynamicSuper/{DynamicSuperPlanner,SuperDefParser}`、`Images/AndroidSparseImage`、`Utils/SpanReader`。
- 测试 **18 个**（全部在测被删引擎）+ 真机回环 exe `tests/src/KunClient.Test`（2 文件，已从 `KunFlashTool.slnx` 摘除）。

⭐**判据**：删完 `KunClient.Core` 与 `KunClient.App` 都是 **0 错误** —— 说明这 26 个文件在 Core/App 里
**早已是死重**（唯一的外部引用全在注释里，或由测试项目/真机 exe 持有）。
保留（**不是 EDL 引擎，删了是净损失**）：`Services/{OfpDecryptor,OpsDecryptor,SuperMergeService,LpMetadataParser,
PayloadBinParser,XiaomiEdlAuthFile,UsernameRule,FastbootOutput}`、`Protocol/Firehose/QfilXmlParser`、
`DynamicSuper/LpMake/*`、`FileSystems/*`、`PlatformTools/*`、`Net/*`、`Configuration/*`。

### 8.4 验收（2026-10-09 20:0x）

| 项 | 结果 |
|---|---|
| `KunClient.Core` / `KunClient.App` / `KunClient.Diag` / `KunClient.Lsusb` 编译 | **全部 0 错误** |
| `combo_probe2` × 4 档（1180×760 / 1000×640 × light/dark） | **ALL_PASS，UI 问题 0 条**；`NavEdl` 点不到 0 / 被裁 0，可交互控件 24 个 |
| 界面渲染 | 浅/深色各一张截图，两列卡片与 SWT-EDL 界面一致 |

⏳→✅ **两件未完成已收口（2026-10-09 夜 · 三）**：

1. `dotnet test` 基线**已恢复绿灯**。复查时 `AuthSessionTests.cs` 里已经是 `persistLogin:`，
   与并发改动后的 `LoginStorage.SaveCredentials(username, token, expiry, persistLogin = true)` 对得上 ——
   两边已被同一次改动对齐，**不需要我再动**。实跑：**102 通过 / 0 失败**
   （原基线 249，差值 147 = 随旧引擎一起删掉的 18 个测试文件的用例数）。

2. 新引擎单测**已补齐**：新增 6 个文件 / **113 条用例**，全量 **215 通过 / 0 失败**。
   明细见 §8.5。

---

### 8.5 新引擎单测（2026-10-09 夜 · 三）

放在 `tests/KunFlashTool.Core.Tests/`，全部**离线可跑**（无串口、无真机、无管理员权限）：

| 文件 | 覆盖 | 用例数 |
|---|---|---|
| `SwtEdlGptParserTests.cs` | 合成 GPT（512/4096 扇区）：分区项解析、**GUID 混合端序**、UTF-16LE 分区名、LUN 透传、5 条错误分支 | 11 |
| `SwtEdlSparseImageTests.cs` | 按 libsparse 规范手拼文件：RAW/FILL/DONT_CARE 三种 extent、**CRC chunk 不占输出块**、游标还原、6 条错误分支 | 11 |
| `SwtEdlRawProgramWriterTests.cs` | 备份 XML 的 LUN 分组与**升序生成**、属性顺序/数值换算、**CRLF**、XML 转义、原子写入无 `.new` 残留 | 6 |
| `SwtEdlRawProgramParserTests.cs` | 缺失镜像/零扇区跳过、**OCDT `TDCO` 魔数+68 字节校验**、十六进制与十进制混写、动态扇区延迟到设备端、super 拆分判据、去重 | 14 |
| `SwtEdlCorePrimitivesTests.cs` | 分区名归一化与 A/B 后缀、保护名单**自洽+去重**、高通型号识别、**MSM_ID 低 24 位掩码**、并列时保留先出现者、stateKey 完备性 | 66 |
| `SwtEdlSystemVersionReaderTests.cs` | 走 RAW 回退通道喂 build.prop：字段映射与 **key 优先级回落**、空值跳过、注释/无 `=` 行忽略 | 5 |
| | | **合计 113** |

判据一律**从规范推导**（UEFI GPT 布局 / libsparse 格式 / rawprogram 语义），
不从当前实现的输出快照回抄 —— 否则实现改错了测试也跟着绿。

#### 🔴 单测撞出的**一个移植真 bug**（已修）

`SwtEdl/Storage/GptParser.cs` 的 `GptGuid()` 把 C++ 的三次 `std::reverse` 照抄过来，
然后喂给 .NET 的 `new Guid(span)` —— **反转了两次**：

- C++ 侧：`std::reverse(...) → QUuid::fromRfc4122(rfc)`，`fromRfc4122` 吃的是**大端** RFC 序 → 反转是**对的**。
- .NET 侧：`new Guid(ReadOnlySpan<byte>)` 用的是 `ToByteArray()` 序，即前 3 段**小端**，
  **正好等于 GPT 的磁盘序** → 再反转一次就错了。

实测现象：Linux 基本数据分区 `EBD0A0A2-B9E5-4433-…` 被解析成 `A2A0D0EB-E5B9-3344-…`。
修法：去掉三次反转，直接 `new Guid(bytes)`（`bytes` 就是 GPT 磁盘序）。

> 影响面：全仓只在 `Types.cs` 声明了 `TypeGuid` / `UniqueGuid`，**没有被任何逻辑判定使用**，
> 所以是「显示错」不是「刷错分区」。但这类**跨语言端序差异**正是逐文件移植最容易静默跑偏的地方，
> 之后凡是 C++ 侧出现 `qFromLittleEndian` / `std::reverse` / `QUuid::fromRfc4122`，
> 都要先确认 .NET 对应 API 的端序口径再照抄。
