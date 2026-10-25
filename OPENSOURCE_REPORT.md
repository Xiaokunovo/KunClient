# 开源完成报告

> 日期：2026-10-25
> 提交：`a435fe6` — 649 files changed, 147,338 insertions(+), 163 deletions(-)

---

## 一、已完成

### 1.1 仓库定位调整

原仓库是「只公开 C# 客户端」的白名单模式，`.gitignore` 用 `/*` 排除一切再放行少数。
本次改为**全项目开源**，`.gitignore` 重写为排除式：源码全进，只挡凭据/产物/临时文件。

### 1.2 脱敏（核心工作）

**全部真实凭据已清除，零泄漏**（提交后 `git grep` 复验）：

| 凭据 | 原值 | 处理 |
|---|---|---|
| 服务器 root 密码 | `8Bnz****` | 清除 |
| JWT secret | `3b53****` | 清除 |
| 阿里云 SMTP 密码 | `Km7q****` | 清除 |
| Gmail OAuth clientSecret | `GOCSPX-****` | 清除 |
| Gmail OAuth refreshToken | `1//0eFk****` | 清除 |
| Outlook OAuth refreshToken | `M.C531****` | 清除 |
| Foxmail SMTP 密码 | `pskn****` | 清除 |
| 管理员密码 | `xiaokun****` | 清除 |
| 服务器 IP | `206.187.215.152` / `38.22.235.198` | → `your-server.example.com` |
| 客户端默认域名 | `kunawa.cn` | → `your-domain.example.com` |

### 1.3 排除的敏感目录

以下目录含大量真实密钥与用户数据，**永久排除**：

```
.workbuddy/                 开发记录（1955 文件，含全部密钥原文）
.workbuddy-ai/
.runtime/                   OAuth 令牌缓存
tmp/                        临时脚本
_archive/server/config.local.json    真实配置
_archive/server/database.sqlite      用户数据库
qqbot-dafeiyu/config.json            机器人配置
qqbot-dafeiyu/data/                  机器人数据
qqbot-dafeiyu/napcat/                NapCat 运行时（大量 DLL）
```

### 1.4 文档重写

| 文件 | 说明 |
|---|---|
| `README.md` | 重写为全项目开源版：项目状态、仓库结构、技术栈、构建步骤、配置说明、协议、免责声明 |
| `SECURITY.md` | 新建：凭据处理说明、已排除目录清单、已知安全问题（引用审计报告） |
| `.gitignore` | 重写：排除式规则，7 大类 |
| `src/cpp/BUILD.md` | 本机绝对路径（`E:\msys64`）改为通用写法 |

---

## 二、提交内容

### 649 个文件，按模块分布

| 模块 | 内容 |
|---|---|
| `src/cpp/` | C++20/Qt6 客户端（149 cpp + 133 h） |
| `src/rust/` | Rust 刷写栈（已冻结） |
| `tests/src/` | C#/WPF 版客户端（v1.x，已退役） |
| `_archive/server/` | Node.js 服务端（Express + sql.js） |
| `_archive/website/` | Next.js 官网 |
| `_archive/qqbot/` | QQ 机器人（早期版本） |
| `qqbot-dafeiyu/` | QQ 机器人（Python + NapCat） |
| `docs/` | 设计文档（4 篇） |
| `tools/` | 构建 / 部署 / 校验脚本 |
| `third_party/swt-edl/` | EDL 引擎第三方组件 |

---

## 三、你需要做的（push）

### 3.1 检查提交内容

```bash
cd E:\flashtool

# 看这次提交改了什么
git show --stat HEAD

# 确认没有敏感文件被误提交（应该没有任何输出）
git ls-files | grep -E "config\.local\.json|\.env$|\.sqlite$|workbuddy|\.runtime"
```

### 3.2 推送到 GitHub

```bash
# 当前 remote：https://github.com/Xiaokunovo/KunClientV1.0.0.git
git push origin main
```

如果 GitHub 上还是旧仓库（只含 C# 源码），这次 push 会是**大量新增**，
不会删除旧文件（旧的 C# 源码仍在 `tests/src/` 里，只是多了其它模块）。

### 3.3 推送前最后确认

```bash
# 逐条确认这些不该出现
git ls-files | grep -c ""                    # 应为 842（含历史）
git grep -l "8BnzEKbT5J7b" HEAD              # 应为空
git grep -l "206.187.215.152" HEAD           # 应为空
```

---

## 四、注意事项

### 4.1 仓库体积

649 个文件、约 147K 行，含 46 个 PNG（logo 设计稿）。推送后仓库约 **30-50 MB**。

如果嫌大，可以把 `logo-design_assets/`（12 个设计稿 PNG）和 `prototype/` 移除，
不影响源码完整性。

### 4.2 服务端代码是归档状态

`_archive/server/` 的代码**能跑但跑不通线上功能**：
- 依赖的域名、证书已失效
- 支付 / 短信 / 邮件通道需要自备
- 数据库结构完整，但数据已排除

想复用需要自己接后端。

### 4.3 历史提交里有旧内容

`b7a5e90` 和 `c59342e` 是之前的脱敏提交，内容是「移除鸣谢区块」「API 统一 v1」。
这些提交里**不含**本次发现的真实密钥（那些一直在 `.workbuddy/` 里，从未入库）。

但如果你担心历史提交有残留，可以考虑：
```bash
# 方案 A：保留历史（推荐，简单）
git push origin main

# 方案 B：压成一个全新提交（清空历史，需谨慎）
git checkout --orphan clean-main
git add -A && git commit -m "开源：完整项目源码"
git branch -D main && git branch -m main
git push -f origin main
```

---

## 五、验证记录

```
待纳入文件:  649
密钥扫描:    10 类全部 0 命中
敏感目录:    7 个全部确认排除
提交后复验:  git grep 零命中
工作区:      clean
```
