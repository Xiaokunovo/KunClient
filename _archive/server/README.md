# KunClient 登录服务器

## 安装步骤

### 1. 安装依赖
```bash
cd server
npm install
```

### 2. 启动服务器
```bash
# 开发模式（自动重启）
npm run dev

# 生产模式
npm start
```

### 3. 管理员账号
管理员账号由配置提供，代码中不内置任何默认密码：
- 配置文件：`server/config.json` → `admin.username` / `admin.password`（模板见 `server/config.example.json`）
- 或环境变量：`ADMIN_USER` / `ADMIN_PASS`
- `admin.password` 留空时跳过管理员播种；已有账号需同步密码时设 `admin.sync = true`

## 配置说明（全部软配置，改配置即可，无需改代码）

优先级：**环境变量 > `server/config.local.json` > `server/config.json` > 内置默认值**

| 配置项 | config.json 字段 | 环境变量 | 说明 |
| --- | --- | --- | --- |
| 端口 | `port` | `PORT` | 默认 3000 |
| JWT 密钥 | `jwt_secret` | `JWT_SECRET` / `JWT_SECRET_FILE` | 留空自动生成并持久化到 `server/.runtime/jwt.secret` |
| 对外地址 | `public_url` | `PUBLIC_URL` | 版本接口下载地址、邮件链接使用；留空回退 `http://localhost:<port>` |
| 数据库 | `db_path` | `DB_PATH` | 留空使用 `server/database.sqlite` |
| 管理员 | `admin.username` / `admin.password` / `admin.sync` | `ADMIN_USER` / `ADMIN_PASS` | 密码留空跳过播种 |
| 通知邮箱 | `admin_email` | `ADMIN_EMAIL` | 接收验证码/反馈通知 |
| 邮件 | `mail.*` | `MAIL_HOST` / `MAIL_PORT` / `MAIL_SECURE` / `MAIL_USER` / `MAIL_PASS` / `MAIL_FROM` / `MAIL_DISABLE` | `enabled=false` 或 `MAIL_DISABLE=1` 即关闭发信 |
| 部署 | `deploy.*` | `DEPLOY_HOST` / `DEPLOY_SSH_PORT` / `DEPLOY_USER` / `DEPLOY_PASSWORD` / `DEPLOY_REMOTE_BASE` / `DEPLOY_APP_PORT` | 供 `tools/*.js` 读取 |

使用方式：
1. 公共默认值写 `server/config.json`；
2. 本机/生产私有值（含密码、授权码）写 `server/config.local.json`（已被根目录 `.gitignore` 忽略）；
3. 一次性覆盖或容器化部署用环境变量，示例见 `server/.env.example`。

## API 接口

### 登录
- **URL**: `POST /api/login`
- **Body**: `{ "username": "admin", "password": "123456" }`
- **Response**: `{ "success": true, "data": { "token": "...", "username": "admin" } }`

### 验证Token
- **URL**: `POST /api/verify`
- **Body**: `{ "token": "jwt_token_here" }`

### 修改密码
- **URL**: `POST /api/change-password`
- **Body**: `{ "token": "jwt_token", "oldPassword": "123456", "newPassword": "newpass" }`

## 宝塔面板部署

1. 在宝塔面板安装 Node.js 项目管理器
2. 创建项目，选择 `/server` 目录
3. 启动项目

或使用 PM2:
```bash
pm2 start server.js --name kunclient-server
```

## 配置服务器地址

在 C# 客户端中修改 `LoginDialog.xaml.cs` 中的 `serverUrl`:
```csharp
private string serverUrl = "http://your-server-ip:3000";
```
