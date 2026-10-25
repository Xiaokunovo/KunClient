# KunClient

安卓设备刷机工具箱 —— **完整开源版**（客户端 + 服务端 + 官网 + 机器人）。

> **项目已停止维护。** 2026-10-25 发布最后一个版本 **v2.0.6**，作者退圈，服务器同步关停。
> 本仓库是完整源码的公开归档，供学习、参考与二次开发。

---

## 关于这次开源

作者（小坤ovo）已退出，项目不再更新。原本闭源的 C++/Qt6 客户端、服务端、
官网与机器人，这次一并公开。

**需要注意的几点：**

- 仓库里的服务端代码是**归档状态**，依赖的线上域名、证书、支付与短信通道均已失效；
- 客户端 v2.0.6 已改为**完全离线**（详见 [`src/cpp/OFFLINE_2.0.6.md`](src/cpp/OFFLINE_2.0.6.md)），
  登录 / 云端更新 / 云端引导库 / 环境检测等联网功能已全部移除；
- 所有真实凭据（JWT secret、SMTP 密码、OAuth refreshToken、服务器密码等）
  已从仓库中移除，示例配置里的字段一律为**空值或占位符**；
- 若你想基于它继续开发，需要自备后端、自行配置 `config.local.json`。

---

## 仓库结构

```
KunClient/
├── src/
│   ├── cpp/                    ★ 主力实现：C++20 / Qt6 桌面客户端
│   │   ├── app/                EDL 工作线程（刷写编排）
│   │   ├── core/               设备标识、EDL 维护、分区保护
│   │   ├── storage/            GPT / payload / OFP / OPS / sparse / super 解析
│   │   ├── protocol/           Sahara / Firehose 协议栈
│   │   ├── transport/          串口传输层
│   │   ├── oplus/              欧加（OPPO/一加）固件解包
│   │   ├── kc/                 业务层：配置、设备、MTK、工具、安全、日志
│   │   ├── ui/                 Qt Widgets 界面（页面 / 对话框 / 控件 / 主题）
│   │   ├── cli/                无头 NDJSON 模式
│   │   ├── tests/              单元测试
│   │   ├── third_party/        swt-edl 引擎
│   │   └── CMakeLists.txt
│   └── rust/                   Rust 刷写栈（已冻结，历史保留）
├── tests/
│   ├── src/                    C# / WPF 版客户端（v1.x，已退役）
│   └── KunFlashTool.Core.Tests/
├── _archive/
│   ├── server/                 Node.js 服务端（Express + sql.js）
│   ├── website/                Next.js 官网
│   └── qqbot/                  QQ 机器人（早期版本）
├── qqbot-dafeiyu/              QQ 机器人（Python + NapCat）
├── docs/                       设计文档
├── prototype/                  UI 原型
└── tools/                      构建 / 部署 / 校验脚本
```

---

## 技术栈

| 组件 | 技术 |
|---|---|
| 桌面客户端 | C++20 / Qt 6（Widgets、SerialPort、Network、Xml）+ CMake + Ninja |
| 编译工具链 | MSYS2 MinGW-w64 GCC 16.x |
| 旧版客户端 | C# / .NET 10 / WPF（v1.x，已退役） |
| 服务端 | Node.js / Express / sql.js（SQLite） |
| 官网 | Next.js 16 / React 19 / Tailwind 4 |
| 机器人 | Python + NapCat（OneBot） |

---

## 功能一览（v2.0.6 离线版）

| 分类 | 功能 |
|---|---|
| 高通 EDL | Qualcomm EDL 刷写、分区表读写、备份还原、Firehose 引导 |
| Fastboot | 常规刷写、可视刷写、分区擦除 / 格式化 |
| MTK | 联发科 BROM 握手、scatter 解析、DA 直写直读 |
| 厂商 | 欧加（OPPO/一加）线刷、小米线刷 / FDL |
| 镜像 | payload.bin 提取、OFP / OPS 解包、sparse / super 处理 |
| 工具 | 安卓工具、应用管理、文件传输、投屏、命令行 |

> v2.0.6 起为纯离线版本：所有刷机、分区、备份、解包功能不依赖任何服务器。

---

## 构建

### C++/Qt6 客户端（主力）

前置：MSYS2 + MinGW-w64 + Qt 6.5+

```bash
# 安装依赖（MSYS2）
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja \
          mingw-w64-x86_64-qt6-base mingw-w64-x86_64-qt6-serialport \
          mingw-w64-x86_64-qt6-tools

# 构建
cd src/cpp
cmake -S . -B build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_COMPILER=E:/msys64/mingw64/bin/g++.exe \
      -DCMAKE_PREFIX_PATH=E:/msys64/mingw64
ninja -C build KunClient

# 测试
ctest --test-dir build --output-on-failure
```

详细说明见 [`src/cpp/BUILD.md`](src/cpp/BUILD.md)。

### 服务端

```bash
cd _archive/server
npm install
cp config.example.json config.local.json   # 填入你自己的配置
node server.js
```

### 官网

```bash
cd _archive/website
npm install
npm run dev
```

---

## 配置

所有真实凭据都**不在仓库里**。需要自备的配置项（见各目录的 `config.example.json` / `.env.example`）：

| 文件 | 需要的字段 |
|---|---|
| `_archive/server/config.local.json` | `jwt_secret`、`admin_email`、`mail.*`（SMTP / OAuth2） |
| `_archive/server/.env` | 数据库路径、端口等 |
| `_archive/website/.env.local` | `NEXT_PUBLIC_API_BASE`、`NEXT_PUBLIC_ADMIN_EMAIL` |

> 🔴 `*.local.json`、`.env`、`*.sqlite` 均已在 `.gitignore` 中排除，不会被误提交。

---

## 开源协议

[MIT License](LICENSE) —— 可自由使用、修改、分发，保留版权声明即可。

---

## 致谢

见 [`src/cpp/ui/pages/AboutPage.cpp`](src/cpp/ui/pages/AboutPage.cpp) 中的鸣谢名单。

---

## 免责声明

本项目仅供**学习研究与个人设备维护**使用。刷机有风险，操作不当可能导致设备变砖、
数据丢失或保修失效。使用本项目即表示你理解并自行承担全部后果。

请勿将本项目用于任何违法用途。
