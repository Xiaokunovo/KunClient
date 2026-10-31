# KunClient 2.0.8 — 发版说明

> 版本号 **2.0.8**（原 2.0.7 基础上升版本）
> 发布日期：2026-10-31

---

## 一、发版产物

| 项 | 值 |
|---|---|
| 线上主包 | `https://kunawa.cn/assets/KunClient-2.0.8.exe` |
| 主包大小 | **8.06 MB**（UPX 压缩，免解压双击即用） |
| 主包 MD5 | `28c717921c2a9d2c456bf408a76f9223` |
| SHA-256 | `8ccdbce6fec3626d9a3215aa9d502fe4778ad1906c2fd0d1ada64f584bfb006c` |
| 7z 分发包 | `https://kunawa.cn/assets/KunClient-2.0.8.7z` · 6.98 MB |
| 构建方式 | 静态单文件（精简 Qt 6.12.0 + strip + UPX） |
| 内嵌版本 | FileVersion / ProductVersion = `2.0.8` |
| 脱环境实测 | 通过（剥离 mingw64 的 PATH 后启动并出图） |

---

## 二、线上状态（已验收）

```
version      = 2.0.8
buildDate    = 2026-10-31
downloadUrl  = /assets/KunClient-2.0.8.exe
zipUrl       = /assets/KunClient-2.0.8.7z
sizeText     = 8.1 MB
sha256       = 8ccdbce6fec3626d...
sig          = MEQCIGwTtUwDOEL4...
forceUpdate  = true
updateEnabled= true
```

- 本地 / 远端 MD5 **完全一致**
- 端到端验签通过（用真实公钥验证线上下发的签名）
- 下载包实测启动正常

---

## 三、代码同步

| 平台 | 状态 | 提交 |
|---|---|---|
| Gitee | 已推送 | `287af1a` |
| GitHub | 已推送 | `287af1a` |

**GitHub 仓库地址修正**：原 remote 指向 `Xiaokunovo/KunClientV1.0.0`（返回 Repository not found），
实际地址是 `Xiaokunovo/KunClient`（会自动重定向自 `kunclient`）。已修正 remote。

**本次提交规模**：103 个文件，+18234 / −555 行。

**安全**：`.keys/`（更新签名私钥）已加入 `.gitignore`，**未进入仓库**。
私钥只存在于构建机本地，服务器上也没有 —— 这是「服务器被拿下也签不出恶意更新包」的前提。

---

## 四、本次内容

### 新增

- **主题包**：5 套内置预设（默认 / 深空 / 暖沙 / 极简 / 浓墨）+ `.kcskin` 导入导出
- **刷机前自动备份**：写入/擦除前自动备 GPT 与关键分区，只留最近 10 次
- **刷机计划预演**：不碰设备先算会写哪些扇区、有无重叠
- **固件包预检真正阻断**：之前只记日志、坏包照样能刷，现在会中止并列明原因
- **重跑上次刷机**：按记录重放，先校验设备身份与包是否存在
- **批量刷机队列**：排队 + 逐个提示换设备
- **分区表对比**：基线快照 + 差异（增/删/改，可区分起始扇区/大小/LUN）
- **镜像格式识别**：按魔数判断 sparse / boot / ext4 / 全 0
- **设备报告导出**：TXT 等宽对齐 / JSON
- **刷机前检查**：电量、磁盘空间、镜像 vs 分区容量
- **自动更新**：握手签名 + SHA-256/ECDSA 双层校验 + 外部脚本替换与回滚

### 修复

- 浅色主题 20 余处对比度不足（最低 1.7:1）→ 全部提到 WCAG AA 以上
- 浅色主题 + 深色壁纸时应用名与图标不可见（1:1）→ 加半透明底
- 动画卡顿：跑马灯滚动拖着整窗重绘（1500 次/秒 → 0）
- 设备主页约 12% 概率崩溃：后台任务在页面销毁后仍访问
- 移除通知中心与 Toast 横幅（按用户要求）

---

## 五、验证

- 全量构建 **0 错误**
- `ctest` **32/32 通过**（本轮新增 6 个测试目标）
- 5 套主题包逐套截图验证外观确实不同
- 线上端到端：哈希一致、签名验证通过、下载包可运行

---

## 六、回滚方式

线上旧包仍在 `assets/` 里不会被删。回滚只需把 `app_settings` 的
`version` / `download_url` 改回旧值，再 `pm2 restart KunClient-server`。

---

## 七、待办提醒

1. **私钥备份**：`E:\flashtool\.keys\update-private.pem` 丢了就再也签不出能通过
   校验的新版本（只能发一个内置新公钥的版本）。建议备份到离线介质。
2. **每次发版都要重新签名**：`tools/sign-update.js <包路径> <版本号>`，
   然后把 `.sig.json` 上传到服务器 `assets/` 同名位置。
   签名覆盖 `version\nfile\nsha256`，**`file` 必须是远端文件名**（不带 `-upx`）。
3. 服务端 `server.js` 本轮已改（新增 `sha256` / `sig` 字段下发），
   备份在服务器 `server.js.bak-20261031-044826`。
