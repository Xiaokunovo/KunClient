# 安全说明

## 项目状态

本项目**已停止维护**（最后一个版本 v2.0.6，2026-10-25）。
不会有安全更新，也不接受漏洞修复。请自行评估使用风险。

---

## 凭据与敏感信息

### 仓库中不含任何真实凭据

开源前已做完整脱敏，以下内容**全部**已从仓库移除或替换为占位符：

| 类型 | 处理 |
|---|---|
| JWT secret | 清空 / 占位符 |
| SMTP 密码（阿里云 DirectMail、QQ 邮箱、Foxmail） | 清空 |
| OAuth2 refreshToken / clientSecret（Gmail、Outlook） | 清空 |
| 服务器 root 密码 | 清空 |
| 服务器 IP / 部署地址 | 替换为 `your-server.example.com` |
| 管理员账号密码 | 清空 |
| 数据库（含真实用户数据） | 排除（`*.sqlite`） |

### 以下目录被 `.gitignore` 永久排除

```
.workbuddy/        # 开发记录，含大量真实密钥
.workbuddy-ai/
.runtime/          # OAuth 令牌缓存
tmp/               # 临时脚本
_archive/server/config.local.json
_archive/server/database.sqlite
qqbot-dafeiyu/config.json
qqbot-dafeiyu/data/
qqbot-dafeiyu/napcat/
```

### 如果你要部署

1. 复制 `config.example.json` 为 `config.local.json`，填入**你自己的**凭据；
2. **不要**把 `config.local.json` / `.env` / `*.sqlite` 提交到任何仓库；
3. 生成新的 JWT secret（不要复用示例值）；
4. SMTP 建议用独立账号 + 应用专用密码，不要用主邮箱密码。

---

## 已知安全问题（未修复）

由于项目已停止维护，以下是审计中发现但**未修复**的问题，仅供参考：

### 严重

- **更新包无签名校验**：`UpdateService` 只检查文件头是否为 `MZ`，未做 Authenticode 验证。
  v2.0.6 已移除自动更新功能，此风险随之消除。
- **远端可控的路径拼接**：`CloudLoaderIndex` 的 `name` 字段直接拼入落盘路径。
  v2.0.6 已移除云端引导库。

### 中等

- **`Expand-Archive` 单引号转义错误**：`'` 被替换成 `2` 而非双写 `''`。
- **`adb devices` 表头误判**：`checkAdbDevice()` 对 `"List of devices attached"` 恒返回 true。
- **线程析构未检查 `wait()` 返回值**：超时后仍 `delete` 可能导致崩溃。

完整审计报告见 [`src/cpp/CODE_AUDIT_REPORT.md`](src/cpp/CODE_AUDIT_REPORT.md)（43 项发现）。

> ⚠️ 这些是**历史代码**的问题记录，不是对本项目的安全承诺。

---

## 报告安全问题

项目已归档，**不再接受**安全报告。若你 fork 后继续维护，请自行建立安全响应流程。
