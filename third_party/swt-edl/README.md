# SWT-EDL

高通 EDL（Emergency Download）刷写引擎 —— **C++/Qt6 实现**。

> 本目录是 KunClient 的底层刷写引擎，独立可编译运行。
> 最初作为第三方组件引入，后成为 KunClient C++ 客户端的核心。

---

## 这是什么

一个完整的 Qualcomm 9008（EDL）模式刷写工具，包含：

| 模块 | 能力 |
|---|---|
| `protocol/sahara` | Sahara 协议握手、镜像上传 |
| `protocol/firehose` | Firehose 协议（XML 命令、分区读写、patch、VIP 鉴权） |
| `transport/serial` | 串口传输层（读写、超时、取消） |
| `storage/gpt` | GPT 分区表解析 |
| `storage/rawprogram` | `rawprogram*.xml` / `patch*.xml` 解析与写出 |
| `storage/sparse` | Android sparse image 展开 |
| `storage/dynamic_super` | 动态分区（super / LP metadata）规划 |
| `storage/system_version` | ext4 / erofs 读取系统版本信息 |
| `oplus/vip_session` | 欧加（OPPO/一加）VIP 分区鉴权会话 |
| `core/device_identity` | 设备标识（芯片型号、序列号） |
| `core/partition_protection` | 分区保护策略（防止误刷关键分区） |
| `app/edl_worker` | 刷写任务编排（多分区、进度、取消） |
| `ui/main_window` | Qt Widgets 界面 |
| `cli/headless` | 无头 NDJSON 模式（供自动化调用） |

---

## 构建

### 依赖

- C++20
- Qt 6.5+（Core / Widgets / SerialPort / Xml）
- CMake 3.24+
- MinGW-w64 GCC 13+ 或 MSVC 2022+

### 编译

```bash
cd third_party/swt-edl

cmake -S . -B build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH=<Qt6 安装路径>

ninja -C build
```

产物：`build/SWT-EDL.exe`（GUI）+ `build/swt-edl-cli`（CLI）。

### 运行

```bash
# GUI
./build/SWT-EDL.exe

# 无头模式（NDJSON over stdin/stdout）
./build/swt-edl-cli
```

---

## 测试

```bash
cd third_party/swt-edl
ctest --test-dir build --output-on-failure
```

包含：
- `tests/GptParserTest.cpp` —— GPT 分区表解析（含畸形输入）
- `tests/MainWindowLogTest.cpp` —— 界面日志

---

## 与 KunClient 主工程的关系

KunClient 的 `src/cpp/` 是本引擎的**超集**：
- 协议层 / 存储层代码基本一致（主工程内也有一份，加了更多厂商支持）
- 主工程额外包含 MTK、小米、欧加专用流程、更多 UI 页面

如果你只想用 EDL 刷写能力，本目录可以独立编译；想用完整工具，看 [`../../src/cpp/`](../../src/cpp/)。

---

## 编译产物

`_archive/server/assets/swt-edl.zip`（31 MB）是 **0.11.0 版的编译发行包**，
内含 `SWT-EDL.exe` 与全部 Qt 运行库，解压即用，无需安装 Qt。

> 该文件仅作历史归档，源码以上面目录为准。

---

## 协议

MIT License（同 KunClient 主项目）。

---

## 免责声明

EDL 刷写会**直接读写设备分区**，操作不当可能导致设备变砖。
仅供学习研究与个人设备维护使用，风险自负。
