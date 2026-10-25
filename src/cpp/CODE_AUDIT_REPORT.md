# KunClient C++ 代码审计报告

**项目**：`E:\flashtool\src\cpp`（Qt6 / C++20，Windows 刷机工具）
**规模**：297 个源文件 / 63,612 行（不含测试与生成物）；24 个 CMake 目标（1 主程序 + 18 测试）
**日期**：2026-10-24

---

## 一、总体结论

**代码总体质量高于同类工具的平均水平**，但存在 **3 个必须立即修复的严重缺陷**，其中 1 个已用可执行程序实测复现崩溃。

| 维度 | 评价 |
|---|---|
| 编译健康度 | 优秀。全量重编 `-Wall -Wextra -Wpedantic` 仅 10 条警告，无一为真实缺陷（详见附录 A） |
| 测试覆盖 | 良好。18 个测试套件全通过；解析器、协议、密码学均有针对性用例（含 RFC 4231 向量、恶意路径用例） |
| 解析层边界安全 | 中等。GPT / Sparse / OFP / OPS / AesCfb 写得严谨；**Payload / SystemVersionReader / DynamicSuper 有真实溢出与无上限分配** |
| 并发模型 | 方向正确（`moveToThread` + 原子取消标志 + Queued 回投），但**析构契约有两处未兑现，且队列回投有 1 处系统性 UAF** |
| 供应链安全 | **最弱环节**。所有下载产物（更新包 / 工具包 / 云端引导）均无签名或哈希校验 |

**修复优先级**：

1. **P0** — `DialogAsync.h` 的 `&done` 悬垂（实测崩溃，命中登录/更新/下载全部异步链路）
2. **P0** — `MtkFlashPage::m_busy` 计数不配平（功能必现锁死）
3. **P0** — 两处由远端控制的路径穿越写入（`CloudLoaderIndex` / `PayloadParser`）
4. **P1** — 解析层整数溢出与无上限分配（可被恶意固件包触发的 OOM / 越界读）
5. **P1** — 下载管线加签名校验（根治一整类供应链风险）
6. **P2** — 其余 Medium 项

---

## 二、Critical（必须立即修复）

### C-1 `DialogAsync.h:33` — 队列回调按引用捕获外层的 `done`，worker 返回后即悬垂

```cpp
// ui/dialogs/DialogAsync.h:27-37
QThreadPool::globalInstance()->start(
    [guard, work = std::forward<Work>(work),
     done = std::forward<Done>(done)]() mutable {      // ← done 是外层 lambda 的成员
        auto result = work();
        if (guard.isNull()) return;
        QMetaObject::invokeMethod(
            guard, [&done, result = std::move(result)]() mutable {   // ← 按引用捕获成员
                done(std::move(result));
            },
            Qt::QueuedConnection);
    });    // ← 投递后 worker 立即返回，callable 被 QThreadPool 回收 → &done 悬垂
```

**为什么是真问题**：`&done` 引用的是第 29 行 lambda 的**成员副本**。第 32 行投递完，worker 函数返回，`QThreadPool` 随即销毁该 callable；而内层 lambda 要等 GUI 线程事件循环稍后才执行，此时 `done` 已被析构 → **use-after-free**。

**影响面**：`runAsync` 全部 10 个调用点，即**整条登录 / 2FA / 验证码 / 更新 / 下载链路**：
- `LoginDialog.cpp:782, 808, 971, 1014, 1046, 1080, 1133`
- `UpdateDialog.cpp:281, 327`
- `DownloadDialog.cpp:75`

**实测复现**（本次审计构建的等价最小程序，非推测）：

```
--- safe（值捕获）---
done(42)
returned normally
--- unsafe（&捕获）---
[进程异常退出，exit code = 0xC0000005  ACCESS_VIOLATION]
```

复现程序：`build-audit/uaf_repro.cpp`（可保留作回归参照）。

**为什么此前没被发现**：`QThreadPool` 回收内存后，堆块常被复用为等价尺寸的对象，`std::function` 的调用可能在多数情况下"恰好"还能跑通 —— 典型的"偶发崩溃"，复现率随负载变化。

**修复**（一行）：

```cpp
QMetaObject::invokeMethod(
    guard, [done = std::move(done), result = std::move(result)]() mutable {
        done(std::move(result));
    },
    Qt::QueuedConnection);
```

⚠️ 注意：`done = std::move(done)` 会移动掉外层成员，因外层只执行一次，安全。更稳妥的写法是让内层持有 `std::shared_ptr<Done>`。

---

### C-2 `MtkFlashPage.cpp:569` — 连接成功后 `m_busy` 永不归零，整页此后完全锁死

```cpp
// ui/pages/MtkFlashPage.cpp:567-573
void MtkFlashPage::connectDevice()
{
    if (m_busy.fetchAndAddAcquire(1) != 0) {   // ← 先 +1
        m_busy.fetchAndSubRelease(1);
        return;
    }
    setBusy(true);
```

`m_busy` 的**全部**增减点（已 grep 穷举）：

| 行号 | 操作 | 出现场景 |
|---|---|---|
| 569 | `+1` | 点「连接设备」 |
| 570 | `-1` | 连接请求被拒（已有操作在跑） |
| 593 | `-1` | 扫描设备**失败**分支 |
| 522 | `-1` | `jobsFinished`（刷写/读回/擦除结束） |
| 738/796/874 | `storeRelease(1)` | 三个操作开始前直接置 1 |

**缺口**：连接**成功**路径（`connectionResult` 492-508、`daResult` 510-518）**从不减 1**。

**后果（必现，非偶发）**：
1. 点「连接设备」→ 握手成功 → `m_busy` 恒为 1；
2. 此后点「下载选中」(`:688`)、「读回选中」(`:760`)、「擦除选中」(`:820`) 全部命中 `if (m_busy.loadRelaxed() != 0)` → 一律弹「有操作正在进行中」并 `return`；
3. 再点「连接设备」也在 569 直接返回。

**MtkFlashPage 一旦连上设备就永久变砖，只能重启程序。** 这是本次审计中**唯一 100% 必现的功能性缺陷**。

**修复**：在 `connectionResult` / `daResult` 的收尾 `updateActionButtons()` 之前补 `m_busy.fetchAndSubRelease(1);`；更根本的做法是把忙碌状态改成 RAII token（构造 +1、析构 -1），避免 6 处手工配平。

---

### C-3 远端下发的名字直接拼成落盘路径 —— 任意文件写（两个独立入口）

#### 入口 1：`CloudLoaderIndex.cpp:98` → `MainWindow.cpp:1782`

```cpp
// kc/cloud/CloudLoaderIndex.cpp:93-101
const QString name = el.value(QStringLiteral("name")).toString();   // ← 服务端下发，零清洗
...
item.name = name;

// ui/MainWindow.cpp:1781-1783
const QString cacheRoot = kc::AppStatePaths::Cache(QStringLiteral("loaders"));
const QString dest = QDir::toNativeSeparators(cacheRoot + QStringLiteral("/") + name);
QDir().mkpath(QFileInfo(dest).absolutePath());        // ← 连父目录一起建出来
```

写入原语 `ChunkedDownloader.cpp:236-237` 是 `QIODevice::ReadWrite | QIODevice::Truncate` —— 目标文件会被**先截断**。

同一条 `cacheRoot + "/" + rel` 拼接在 `MainWindow.cpp:1826`（`digest` / `signature` 伴生文件）与 `CloudLoaderPickerDialog.cpp:314-316, 370-371` 重复出现。

攻击场景：索引接口返回 `{"name":"../../../../AppData/Roaming/Microsoft/Windows/Start Menu/Programs/Startup/evil.exe"}` → 落进用户启动目录 → 登录即执行。

> 现状备注：`downloadCloudLoader()` 当前**不可达**（`fillCloudLoaderItems` 无调用点，云端库已下线）。修 `CloudLoaderPickerDialog` 那条同样不可达。但只要该功能恢复，缺陷立即生效 —— 属**潜伏的 P0**。

#### 入口 2：`PayloadParser.cpp:518` — 同一工程内实现不一致

```cpp
// storage/PayloadParser.cpp:517-521
for (const ParsedPartition& partition : targets) {
    const QString outputPath =
        QDir(outputDirectory).filePath(partition.name + QStringLiteral(".img"));   // ← 未净化
    QFile output(outputPath);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
```

`partition.name` 直接来自 `payload.bin` 的 protobuf manifest（`:189` `QString::fromUtf8`），全程无字符或路径校验。`QDir::filePath()` 对绝对路径原样返回、对 `..` 原样保留。

**这条是可达的**：用户从任意来路下载 OTA 包 → 选「提取全部分区」→ `partition_name = "../../evil"` 即写到输出目录之外。

**关键对照** —— 同一工程的 OFP 解包器**有**防护，还有单测：

```cpp
// oplus/OfpExtractor.cpp:127-132
const QString base = QDir::cleanPath(QDir(root).absolutePath());
const QString destination = QDir::cleanPath(base + QLatin1Char('/') + normalized);
if (!destination.startsWith(base + QLatin1Char('/'), Qt::CaseInsensitive)) {
    if (error) *error = QStringLiteral("固件表里有不安全的输出路径: %1").arg(relative);
    return false;
}
```

`OpsExtractor.cpp:222-223` 同款。**唯独 PayloadParser 漏了** —— 明确是复制粘贴时漏掉，而非设计取舍。

**修复**：把 `resolveOutputPath` 提取成公共工具，三处共用；额外拒绝空名、Windows 保留名（`CON`/`PRN`/`AUX`/`NUL`/`COM1..9`/`LPT1..9`）、以及 `cleanPath` 后仍含 `..` 的输入。

---

## 三、High

### H-1 所有下载产物均无哈希/签名校验（供应链 RCE 面）

| 产物 | 唯一"校验" | 后果 |
|---|---|---|
| 更新包 | `UpdateService.cpp:71-79`：只检查头两字节是不是 `MZ` | 覆盖 `KunClient.exe` 并重启 |
| 平台工具包 | `PlatformToolsManager.cpp:276-290`：解压后直接执行 | 生成 `adb.exe` / `fastboot.exe` 并运行 |
| 云端引导 | 无 | 直接喂给 EDL Firehose |

`ChunkedDownloader::Download`（`:159`）签名里**根本没有 expected-hash 参数**，唯一的完整性检查是 `:311-316` 的字节数比对。

而 `CloudLoaderIndex` 已经解析了 `digest` / `signature` 字段（`:100-101`），但全项目 grep 显示这两个字段**从未参与过任何验算** —— 只被当成"再去下载两个文件"的路径名。这是"设施已就位、开关没接上"的典型。

**修复**：`ChunkedDownloader::Download` 增加 `expectedSha256` 参数，边下边算（复用 `kc::crypto::Digest` 的流式会话，它已走 CNG）；更新包叠加 `WinVerifyTrust` 校验 Authenticode 签名主体。

---

### H-2 更新通道接受任意 scheme 的绝对 URL

```cpp
// kc/update/UpdateService.cpp:63-69
QString resolveDownloadUrl(const QString& url) {
    if (url.trimmed().isEmpty()) return QString();
    const QUrl u(url);
    if (u.isValid() && !u.scheme().isEmpty()) return u.toString();   // ← http:// 也照收
    return ClientConfig::ServerUrl() + "/" + ...;
}
```

`ClientConfig` 对**服务器地址**做了 https-only + 域名白名单（`ClientConfig.cpp:168-183`，这块实现正确），但**更新包下载地址完全绕开这套策略**。服务端（或被控的响应）回 `"downloadUrl":"http://attacker/k.exe"` 即明文下载并替换自身二进制。

zip 分支更差：`UpdateService.cpp:346-348` 用 `QDirIterator(..., Subdirectories)` 递归找任意位置的 `KunClient.exe`。

**修复**：只允许相对路径，或"https 且 host 通过 `IsTrustedHost`"的绝对 URL；解压后只在固定一级目录内找 exe，并拒绝 `..` / 绝对路径条目（zip-slip）。

---

### H-3 `PayloadParser` 的 REPLACE / ZERO 分支缺少长度上限

压缩分支都有保护：

```cpp
// :601  REPLACE_BZ / REPLACE_XZ / REPLACE_ZSTD
if (op.dataLength == 0 || op.dataLength > kMaxOperationBytes) {   // 512 MB
```

但**未压缩的 REPLACE 和 ZERO 都没有**：

```cpp
// :588  REPLACE —— targetBytes 完全由 extent 数量决定
const QByteArray data = file.read(static_cast<qint64>(targetBytes));

// :560  ZERO —— 无溢出检查、无上限、无 cancel 检查
quint64 remaining = extent.second * BlockSize;
while (remaining > 0) { ... output.write(zeros.constData(), chunk); ... }
```

`num_blocks` 是 manifest 里的任意 varint。8 GiB 的 extent → 一次性 `read(8 GiB)` 分配；4 TiB 的 ZERO 段 → 把用户输出盘写满（`QFile::write` 越过 EOF 会继续扩展文件）。

**修复**：两个分支都加 `> kMaxOperationBytes` 拒绝；`extent.second * BlockSize` 前做溢出判断；并强制 `(extent.first + extent.second) * BlockSize <= partitionSize`。

---

### H-4 `SystemVersionReader.cpp:467` — u32 加法回绕导致 `QVector::at()` 越界

```cpp
// :467
if(at+52>meta.size()||!le(meta,at+40,&first)||!le(meta,at+44,&count)||
   first+count>static_cast<quint32>(all.size())) return {};
for(quint32 x=0;x<count;++x){ part.extents.push_back(all.at(first+x)); ... }
```

`first`、`count` 均为 `quint32`，`first + count` **按 32 位无符号回绕**后再比较。

取 `all.size() == 1`、`first = 0xFFFFFFFF`、`count = 2`：`0xFFFFFFFF + 2 == 1`，`1 > 1` 为假 → **通过校验**，随后 `all.at(0xFFFFFFFF)` 越界。Qt6 的 `QVector::at()` 在 Release 下只有 `Q_ASSERT`（被编译掉），是真正的堆越界读，读到的值还会被当作 `sectors` 继续参与设备读取偏移计算。

**触发**：`EdlWorker::readSystemVersionInfo` 对设备 `super` 分区调用 `parseLp` —— 刷入恶意镜像后点「读取系统版本」即可。

**修复**：用 `quint64(first) + count` 计算，并分别校验 `count <= all.size()`、`first <= all.size()`。

---

### H-5 `SystemVersionReader.cpp:464` — `reserve(en)` 由不可信字段直接控制

```cpp
QVector<LpExtent> all; all.reserve(en);    // en 来自 metadata 偏移 96，无任何关联校验
```

`en = 0xFFFFFFFF` → 尝试分配约 **103 GB**。Qt 容器不做上限截断，失败抛 `std::bad_alloc`，在 Qt 事件循环里未捕获 → `std::terminate`。

注意：紧随其后的 `for` 循环本身是安全的（`le()` 会失败返回），**唯一的伤害就是这行 reserve** —— 说明它是纯多余分配。

**修复**：`en` 先与 `(meta.size() - header - eo) / es` 比较，超界直接返回空。

---

### H-6 `DynamicSuper.cpp:218` — `metadataMaxSize` 无上限 → 128 GiB 分配意图

```cpp
// :218-223 校验缺上界
if (metadata->metadataMaxSize < 128 || metadata->metadataMaxSize % 512 != 0 || ...)

// :313-320
QByteArray padded(static_cast<qsizetype>(metadata.metadataMaxSize), '\0');
for (quint32 index = 0; index < metadata.metadataSlotCount * 2U; ++index) {
    result.append(padded);      // slotCount 最大 16 → 32 次
}
```

取 `metadataMaxSize = 0xFFFFFE00`（4 GiB）× 32 次 → **128 GiB**。geometry 有 SHA-256 保护，但攻击者构造包时当然能算出正确摘要。

**触发**：`DynamicSuperPlanner::create` ← `RawProgramParser::parse`。**只需打开恶意包，不需要连设备。**

**修复**：给 `metadataMaxSize` 加上界（真实 LP 元数据是 512 KB ~ 2 MB 量级，取 4 MiB 足够），并校验 `slotCount * metadataMaxSize <= blockDeviceSize`。

---

### H-7 `SystemVersionReader.cpp:126` — ext4 extent 树递归无深度上限 → 栈溢出

```cpp
// :126-153
bool appendExtentNode(const QByteArray& node, quint64 maximum, QByteArray* out)
{
    quint16 magic = 0, entries = 0, depth = 0;
    ...
    } else {
        if (!appendExtentNode(m_reader.read(leaf * m_blockSize, m_blockSize), maximum, out)) return false;
    }
```

`depth` 被读出来了（第 130 行），但**从未用于限制递归** —— 每层都从当前节点重新读 `depth`，既不递减也不与层数比较。构造一个 `ei_leaf` 指向自身的节点即是无限递归，每层帧还带一个 `QByteArray`（4 KB），几千层足够打爆线程栈。

**修复**：递归时传 `expectedDepth`（子节点必须 `parent.depth - 1`），并在 `depth > 5`（ext4 实际上限）时拒绝。

---

### H-8 `Firehose.cpp:411` — XML 帧无累计上限

```cpp
// :411-425
for (int frameIndex = 0; frameIndex < 1024; ++frameIndex) {
    if (!m_transport.readUntil(..., 2 * 1024 * 1024, remaining, error)) ...
    response->xml += current.xml;            // ← 单帧 2MB × 1024 帧 = 最坏 2GB
    response->messages += current.messages;
```

单帧有 2 MB 上限、帧数有 1024 上限，但**总量没有**。设备连续吐 `<log value="..."/>` 而不回 ACK/NAK 即可 OOM。`waitForVipPartitionRequest`、`receiveVipPartitionResult` 同结构同问题。

**为什么算真问题**：本工具的核心场景就是给**来路不明**的设备刷机，协议层不该假设设备友善。（实际触发前会先撞上 `timeoutMs` 总超时，故实际危害受超时值约束 —— 但 `timeoutMs` 在长刷写路径上设得很宽松。）

**修复**：加累计字节上限（8 MB / 4 MB messages），超限即失败。

---

### H-9 线程析构契约未兑现：`wait()` 超时后仍无条件 delete

```cpp
// ui/pages/MtkFlashPage.cpp:538-548
if (m_worker) m_worker->requestCancel();
if (m_workerThread->isRunning() && m_worker) {
    QMetaObject::invokeMethod(m_worker, "closeSession", Qt::BlockingQueuedConnection);
    m_workerThread->quit();
    m_workerThread->wait(5000);        // ← 返回值被忽略
}
delete m_worker;                       // ← 线程可能仍在跑
delete m_workerThread;                 // ← QThread 仍在 running → qFatal → abort
```

worker 正卡在 `writeFromDevice` / `readToDevice` 的长循环（每 512 KB 才看一次 cancel）时，`wait(5000)` 很可能超时；随后 `delete m_worker`（其 `QSerialPort` 仍在另一活线程上）是 UB。典型现象：**刷写中途关窗/切页崩溃**。

`MainWindow.cpp:717-721` 同款（`:720` 忽略返回值，`~QThread` 在 running 状态会 `qFatal`）。

`VideoBackground.cpp:498-517` 更危险 —— `terminate()` 后立即 `delete`：

```cpp
if (!m_thread->wait(5000)) {
    m_thread->terminate();
    m_thread->wait(1000);      // ← 也可能超时，且 terminate 本身不保证释放锁
}
delete m_worker;               // ← "线程已停，此刻删是安全的" —— 这句注释在超时路径上不成立
```

**修复**：检查 `wait()` 返回值；超时时不要删（泄漏优于 abort），改为 `m_workerThread->setParent(nullptr)` 并放弃；正常路径用 `connect(thread, &QThread::finished, worker, &QObject::deleteLater)` 让线程自己收尾。

---

## 四、Medium（摘要，均已定位核实）

| # | 位置 | 问题 | 修复 |
|---|---|---|---|
| M-1 | `VideoBackground.cpp:465-486` | `sizeOut`/`errorOut` 是**栈变量**，传给 worker；5 秒超时路径 `close()` 可能等不到线程结束，之后 worker 仍写这两个变量 → UAF | 改 `shared_ptr`；`announce()` 前先查 `stop` |
| M-2 | `AdbGptReader.cpp:388-391` | `remote` 名清洗了，但 `sources` 里的 `partition` 没洗，直接拼进 `su -c "..."` → 设备端命令注入 | 统一白名单 `[A-Za-z0-9_.-]`，`su -c` 单引号包裹并转义 |
| M-3 | `PlatformToolsManager.cpp:67-71`<br>`FastbootSizeFallback.cpp:111-112` | 从 `PATH` 收集目录去找 `AdbWinApi.dll` 并拷进 adb 同目录；从 `PATH` 找 `fastboot.exe` 拿来刷机 | 只从 `System32` + 程序自身目录取，且校验签名 |
| M-4 | `PlatformToolsManager.cpp:821-830` | `checkAdbDevice()` 恒为真：`adb devices` 表头 `"List of devices attached"` 就含 `"device"` | 逐行匹配 `^\S+\s+(device\|fastboot)$`，排除 `unauthorized\|offline` |
| M-5 | `EnvReportService.cpp:17-31` | 环境上报的 HMAC 密钥**硬编码在客户端**（乱序拆分 + 索引表就在旁边）→ 可伪造"环境干净"上报绕过封禁 | 客户端只持公钥；或把 key 绑定到会话 `secret` 做一次性派生 |
| M-6 | `LoginStorage.cpp:22-32` | DPAPI 熵是硬编码 `"KunClient2024"`；且明文密码随 JSON 一起加密存盘 → token 生命周期被拉长成密码泄露 | 熵改为每用户随机；不存密码只存 token |
| M-7 | `ChunkedDownloader.cpp:269-275` | 分片请求不校验 `Content-Range` 起始偏移，只按长度截 → 静默写坏固件（长度对、内容错） | 断言 `S == pos && E == pos + size - 1` |
| M-8 | `MembershipService.cpp:480-508` | GUI 线程同步 HTTP（最长 12 s），且 `g_limits` 在锁外写 | 投 `QThreadPool`；读写统一走 `g_gate` |
| M-9 | `UpdateService.cpp:246` | 固定临时目录 `%TEMP%\KunClient_Update` + 固定 `apply_update.bat` → 同机低权限进程可预置 junction 或替换 bat | 用 `QTemporaryDir`（随机名） |
| M-10 | `PlatformToolsManager.cpp:93-103` 等 5 处 | PowerShell `Expand-Archive` 的单引号"转义"是把 `'` 换成 `2`（正确做法是双写 `''`）→ 静默改错路径；虽是拼接但当前不可直接利用 | 改用参数数组，或统一走 `tar.exe`（`extractTarGz` 的写法是对的） |
| M-11 | `SerialTransport.cpp:172-175` | `fillBuffer` 内层 `while(waitForReadyRead(2))` 无上限，可能把整段传输吸进 buffer | 加 `m_buffer.size() < 上限` |
| M-12 | `OfpExtractor.cpp:224,226,236`<br>`OpsExtractor.cpp:309` | `value * pageSize` 有符号溢出 UB（`toLongLong` 可给 `INT64_MAX`） | 乘前判 `> INT64_MAX / pageSize` |
| M-13 | `EdlWorker.cpp:763-769` | GPT `backupLba` 直接当 LUN 容量，未与 `getstorageinfo` 交叉校验 → 越界擦除校验失效 | 取交集/取小，差距过大 fail-closed |
| M-14 | `Firehose.cpp:624`<br>`OcdtGenerator.cpp:404` | 先 `readAll()` 后检查大小，上限形同虚设 | 先 `file.size()` 再读 |
| M-15 | `PayloadParser.cpp:346-378` | `extent.second * BlockSize` 裸 u64 乘法回绕 → "数据比目标区间短"检查失效，**解包出的镜像静默错位** | 统一走显式溢出检查 |
| M-16 | `Sahara.cpp:571-627` | 上传循环无总时限，设备反复 `READ_DATA offset=0` 即可让主机无限转 | 加累计期限 + 重复 offset 计数判死 |
| M-17 | `MtkFlashPage.cpp:541,623`<br>`MainWindow.cpp:718` | UI 线程 `BlockingQueuedConnection` 等 worker 长任务 → 界面"未响应" | 先 cancel，改用 `QueuedConnection` + 回投信号 |
| M-18 | `AppManagerPage.cpp:229`<br>`BasePage.cpp:47-51` | 线程池任务用裸 `this` 回投 UI，页面销毁后仍 `invokeMethod` → UAF（同项目 `CloudDriverPage.cpp:281` 已有正确的 `QPointer` 范式） | 捕获 `QPointer`，投递前判空 |
| M-19 | `MainWindow.cpp:2096-2104` | GUI 线程递归扫描包目录（`discoverPackage` 默认 `recursive=true`），且 `setChecked(true)` 无 `QSignalBlocker` 再触发两次扫描 | 显式传 `false`；加 `QSignalBlocker` |
| M-20 | `MainShell.cpp:601-622` | 析构未清 `s_instance`，全局单例悬空（头文件注释明确写了"析构里清"但没实现） | 析构首行 `s_instance = nullptr` |
| M-21 | `LogView.cpp:413` + `SessionLog.cpp:111-112` | 每条日志在 UI 线程做一次 `write + flush` | 定时批量 flush 或挪到写线程 |
| M-22 | `MainShell.cpp:2492-2521` | detach 的 `std::thread` 在 `qApp` 析构后仍 `invokeMethod` | 改 `QThreadPool` + `QPointer` |

---

## 五、Low（摘要）

- `MainWindow.cpp:1870-1893` — `shared_ptr` 自引用环（捕获持有自己的 `shared_ptr`），确定性泄漏；仅 `--cloud-panel-popup` 探针路径。
- `EnvInspector.cpp:749-756` — `KUN_ALLOW_DEBUG` 反调试后门保留在 release 构建（好在会自报 `debug_bypassed`，属"留痕后门"）。
- `HttpClient.cpp:106-111` — `KC_TIMING` 下把含 `?t=<ticket>` 的完整 URL 打到 stderr。
- `ChunkedDownloader.cpp:70-73` — 下载失败无条件删调用方传入的路径（`Truncate` 已先清空过）。
- `Sha256.cpp:85-91` — `BCryptHashData` 返回值被丢弃，失败会得到"算了一半"的合法长度摘要。
- `FirmwareChecksum.cpp:44-45` vs `AndroidToolsPage.cpp:1686-1687` — 接受 40 位 SHA-1 记录，但算法反推只按 `32→MD5，否则 SHA-256`，40 位必判"不一致"。
- `DeviceWatcher.cpp:243-249` — 对 `const` 容器元素 `const_cast` 写入，UB。
- `Firehose.cpp:1234-1239` — `patch` 的 `start_sector` / `value` 未转义拼进 XML 属性。
- `SettingsPage.cpp:104` — `levelLabel()` 定义未使用（编译器已警告）。

---

## 六、已核实为「无问题」的部分（负向结论）

避免报告只列问题造成误读，以下经逐行核对**确认实现正确**：

1. **密码学**：`Sha256.cpp` / `Digest.cpp` **不是**自实现算法，而是 Windows CNG（`BCrypt*`）封装。非 HMAC 路径 `pbSecret=nullptr, cbSecret=0`、HMAC 路径空密钥显式补 64 字节零，符合 CNG 与 RFC 2104 语义；`m_object` 生命周期正确不短于 `m_handle`。仓库内 `RequestSignerTest.cpp:32-59` 已用 RFC 4231 Case 1/2 + FIPS 180-4 三个向量钉死。
2. **随机数**：全项目无 `qrand` / `srand` / `rand()` / 时间种子。`RequestSigner::nonceHex()` 用 `QRandomGenerator::system()`（OS CSPRNG）。
3. **TLS**：全项目 grep 无 `setPeerVerifyMode(VerifyNone)`、无 `ignoreSslErrors`、无 `QSslConfiguration` 覆写 → QNAM 默认严格校验；Qt6 默认 `NoLessSafeRedirectPolicy`，HTTPS→HTTP 降级会被拒。
4. **主机白名单**：`ClientConfig::IsTrustedHost` 用精确匹配或 `.` 前缀（**不是**裸 `endsWith`），挡住 `kunawa.cn.evil.com`。
5. **DPAPI 调用本身**：无 `CRYPTPROTECT_LOCAL_MACHINE`（正确保持用户 scope）、`CRYPTPROTECT_UI_FORBIDDEN`、`LocalFree` 正确。
6. **QProcess 参数**：`adb` / `fastboot` 全部走 `setProgram` + `setNativeArguments`，**不经 cmd.exe**，无 shell 解释；`tar.exe` 同理用参数数组。仅 PowerShell 回退分支是字符串拼接（见 M-10）。
7. **`GptParser.cpp`**：所有读取先校验 `data.size()`；`entryLba * sectorSize`、`entryCount * entrySize` 都有溢出检查。是全项目解析器里写得最好的一份。
8. **`SparseImage.cpp`**：`cursor + totalSize`、RAW chunk 长度、`outputBlock + chunkBlocks > totalBlocks` 校验完整。
9. **`AesCfb.cpp`**：S 盒索引与块内索引都有界，密钥/IV 强制 16 字节。
10. **`MtkWorker` 线程模型**：`moveToThread` + 全 slot 在 worker 线程执行 + `QAtomicInt` 取消标志，方向正确；`writeJobs` 的"整批镜像先打开并保持 + 写入前二次 `size()` 校验 + 稀疏镜像拒绝 + `QSaveFile` 原子提交"，是防"检查后被替换"的正确做法。
11. **UI 层 `singleShot`**：全部带 context 对象或 `QPointer` 防护；`connect(..., [this]{})` 无 context 对象的写法**零命中**。
12. **`RequestSigner`**：`QMutex` 保护、按 `scheme://host:port` 绑定服务端、5 分钟提前过期、串行 nonce、401 仅重放一次。

---

## 七、附录 A：编译器警告（全量重编，10 条，均为误报或无害）

```
storage/DynamicSuper.cpp:36:16  -Warray-bounds         QByteArray 空对象常量折叠，误报
main.cpp:563:21                 -Wunused-result        QFile::open 返回值未用（探针日志文件）
ui/pages/SettingsPage.cpp:104   -Wunused-function      levelLabel() 未使用（见 Low）
tests/MtkProtocolTest.cpp:235   -Wswitch               测试代码枚举未覆盖
```

`-fanalyzer` 额外报 10 条，经逐条核实**全部是 Qt `QHash`/`QByteArray` 内部实现的误报**（`LpPartition` 成员均有 `= 0` 默认初始化）。

## 八、附录 B：构建与测试基线

- 工具链：MSYS2 MinGW64 GCC **16.2.0** + Qt6（`E:\msys64\mingw64`），Ninja，C++20，`-Wall -Wextra -Wpedantic`
- **全量重编：成功，0 error，10 warning**
- **单元测试：18/18 全部通过**（总耗时 14.68 s）
  ```
  core-tests / dynsuper-tests / maintenance-tests / luncapacity-tests
  storagebrand-tests / ocdt-tests / usbsticky-tests / oplus-tests
  payload-tests / edlproto-tests / mtk-tests / mtkproto-tests
  requestsigner-tests / diag-tests / ui-tests / maintenance-ui-tests
  checksum-tests / env-tests
  ```
- 审计构建目录：`src/cpp/build-audit/`（含 `compile_commands.json`、`warnings.log`、`analyzer.log`、`uaf_repro.cpp`）

> 注：`src/cpp` 未被 git 跟踪（`.gitignore` 第 17 行 `/*` 白名单模式将其排除），因此本次审计基于工作区实际文件，无版本历史可对比。

## 九、附录 C：修复代码（已通过编译验证）

两处 P0 的最小补丁（本次审计已验证可编译，但**未写入源码**，留给你决定）。

### C-1 `ui/dialogs/DialogAsync.h:33`

```diff
             QMetaObject::invokeMethod(
-                guard, [&done, result = std::move(result)]() mutable {
+                guard, [done = std::move(done), result = std::move(result)]() mutable {
                     done(std::move(result));
                 },
                 Qt::QueuedConnection);
```

验证：用 MinGW GCC 16.2 + Qt6 头文件做 `-fsyntax-only` 编译，**通过**。

### C-2 `ui/pages/MtkFlashPage.cpp` — 补两处减 1

```diff
 // :507  connectionResult 收尾
                 }
+                m_busy.fetchAndSubRelease(1);
                 updateActionButtons();
             });
 
 // :517  daResult 收尾
         } else {
             error(message.isEmpty() ? QStringLiteral("DA 引导失败") : message);
         }
+        m_busy.fetchAndSubRelease(1);
         updateActionButtons();
     });
```

⚠️ 注意：`daResult` 与 `connectionResult` 是**两个独立信号**，都会在连接流程中触发。若两者都减 1 而只加过 1 次，`m_busy` 会变成负数（`QAtomicInt` 是有符号的），之后 `!= 0` 判断依旧成立 —— **等于没修**。

更安全的做法（推荐）：把 +1/-1 收敛到 `connectDevice()` 一处，用作用域守卫：

```cpp
struct BusyGuard {
    QAtomicInt& f;
    bool owned = false;
    explicit BusyGuard(QAtomicInt& v) : f(v) { owned = (f.fetchAndAddAcquire(1) == 0); }
    ~BusyGuard() { if (owned) f.fetchAndSubRelease(1); }
    explicit operator bool() const { return owned; }
};
```

把 `m_busy` 的生命周期绑定到"连接流程这一整个异步事务"，流程结束（成功或失败）时自动归零，就不会再出现"6 处手工配平、漏一处就锁死"的问题。

---

## 十、附录 D：未覆盖范围


受时间预算限制，以下**未逐行审查**：

- `ui/pages/`：`VisualFlashPage`、`ScreenMirrorPage`、`FileTransferPage`、`OnePlusFlashPage`、`XiaomiEdlPage`、`ConsolePage`、`CommandConsolePage`、`DeviceHomePage`、`CloudDriverPage` 仅读了异步骨架；`SettingsPage` 只读约 60%；`AndroidToolsPage` 140-440 / 900-1200 未读；`FastbootFlashPage` 160-290 / 580-960 未读。
- `ui/dialogs/`：`WelcomeDialog`、`AppealDialog`、`PackageImageListDialog`、`WallpaperEnginePickerDialog`、`LogUploadDialog`、`LoaderPickerDialog` 只读了异步片段。
- `ui/widgets/`：`GlassSurface`、`LiquidGlass`、`SvgPath`、`IosSpinner`、`MarqueeLabel`、`NotificationBar` 未审；`DynamicIsland`(868 行)、`LogView`(669)、`PageTransition`(634) 只读部分。
- `tools/`、`third_party/` 不在本次范围。
