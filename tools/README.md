# tools —— 运维 / 部署脚本

> **软配置说明**：脚本内不硬编码服务器 IP / SSH 凭据 / 端口，统一由 `tools/deploy-config.js` 解析。
> 优先级：环境变量 > `server/config.local.json` → `server/config.json` 的 `deploy` 节 > 内置默认值。
> 可用环境变量：`DEPLOY_HOST` / `DEPLOY_SSH_PORT` / `DEPLOY_USER` / `DEPLOY_PASSWORD` / `DEPLOY_REMOTE_BASE` / `DEPLOY_APP_PORT`，示例见 `server/.env.example`。
>
> 历史的一次性 debug 脚本已移入仓库根目录 `_trash_20260917/`（本仓库没有 git，删除不可恢复）。

## 分发包依赖校验（发版前必跑）

| 脚本 | 作用 |
| --- | --- |
| `check-package-deps.py` | 按 PE 导入表递归求依赖闭包，检查**包内**是否真有每个被依赖的 DLL。支持目录 / `.exe` / `.7z` / `.zip`；缺一个就退出码 1 |

```powershell
# 发版前把要发的东西都过一遍（目录 + 压缩包 + 静态单文件）
python tools/check-package-deps.py `
  src/cpp/dist src/cpp/dist-offline `
  src/cpp/KunClient-2.0.6-offline.7z src/cpp/KunClient-2.0.6-offline.zip `
  "KunClient-2.0.6-功能补充.zip" `
  src/cpp/dist-static/KunClient-2.0.8.exe src/cpp/dist-static/KunClient-2.0.8-upx.exe
```

为什么必须有它：2026-10-31 用户报「由于找不到 liblzma-5.dll，无法继续执行代码」，
根因是打包脚本算 DLL 闭包时按**路径**比对 `ldd` 输出（`/mingw64/bin/xxx` 对
`/e/msys64/mingw64/bin`，永远不匹配），漏拷了 `liblzma-5.dll`。这类缺陷**在开发机上
永远复现不了**（PATH 里有 mingw64，系统直接找到了），只有用户机器才会炸。
所以判据必须是「包内有没有这个文件」，不能是「本机能不能找到」。

两个构建脚本已把闸门接进去了：`build-release.sh`（`dist/` 不完整直接 fail）、
`build-static.sh`（单文件导入表出现任何第三方 DLL 直接 fail）。

> 静态单文件（`dist-static/*.exe`）的判据是另一套：它的承诺是「一个文件就能跑」，
> 所以只要导入表里出现**任何**第三方 DLL 就算不合格（系统 DLL 不算）。

## API 自测（登录服务器）

| 脚本 | 作用 |
| --- | --- |
| `selftest-api.js` | 在临时端口(3123) + 临时数据库上拉起 server.js，验证「用户密钥自助管理 / 密钥登录 / 用户 ID 权限 / 问题反馈」共 28 项断言，结束后自动清理临时文件，不影响正式数据库 |
| `selftest-content.js` | 在临时端口(3124) + 临时数据库上验证「已下架的云端 ROM / 模块接口确实返回 404」+「官网对外口径开关 site_stage」 |
| `selftest-modules.js` | 在临时端口(3125) + 临时数据库上验证增值模块配置：默认归属（无幽灵 key）/ 控制台保存 features / 陌生 key 丢弃 / 跨模块重复先到先得 / 显式空数组 = 空模块 / includedLevels 不受影响 |
| `check-server.js` | 静态检查 server.js：语法、全局函数是否都有定义、关键辅助函数、静态目录是否存在 |
| `check-console.js` | 静态检查控制台（v6 多文件结构）：16 个 ES 模块的语法、import/export 对账、`$('id')` 引用、`data-act` 动作注册、emoji 残留、旧色残留、关键功能标记 |
| `check-modules-drift.js` | 防漂移：server.js 的功能目录 + 默认模块归属 ⇄ website/src/lib/data.ts 的 FEATURE_HINTS + MODULES 回落卡片，逐项比对（改分类后必跑） |

```powershell
cd e:\flashtool\tools; node selftest-api.js; node selftest-content.js; node selftest-modules.js; node check-server.js; node check-console.js; node check-modules-drift.js
```

> 自测会自动把「反馈邮件通知」关闭，不会对外发信；管理员账号取自环境变量
> `SELFTEST_ADMIN_USER` / `SELFTEST_ADMIN_PASS`（默认 `admin` / `selftest_admin_pass`，仅用于临时库）。
> 注入标记为 server.js 中的 `const cfg = require('./config');` 与 `seedSetting('feedback_notify', '1')`，
> 若被改动脚本会报「注入失败」并中止，此时请同步更新 selftest-api.js 里的标记。

## 客户端图标（`src/KunClient.App/Assets/logo.ico`）

应用图标必须**由脚本生成**，不要拿图片编辑器另存 ICO。

| 脚本 | 作用 |
| --- | --- |
| `make-app-icon.py` | 从 `Assets/logo.png` 重新生成多尺寸 `Assets/logo.ico`（16/24/32/48/64/128/256；≤128 写 BMP/DIB 帧，256 写 PNG 帧），生成后自动做结构校验 |
| `make-app-icon.py --check` | 只校验现有 ico 是否目录自洽，不改文件（改过图标素材后先跑它） |

```powershell
python tools/make-app-icon.py --check   # 校验
python tools/make-app-icon.py           # 重新生成
```

需要这样一份脚本的原因：历史 `logo.ico` 的 ICONDIR 声明有 6 张图，但只有第 0 项有效
（宽高/长度/偏移全 0），真实存在的 32/48… 帧又没有任何目录项指向它们。Windows 解析失败后
会退回默认占位图标，表现为任务栏、标题栏、资源管理器里「logo 没了」。

两个容易踩的点：

1. **图标是编译期打进 exe 的**。重新生成 ico 后必须重新发布
   （`dotnet publish ... -o publish`），否则 `publish/` 里还是旧图标。
   校验是否真的换掉了：确认新 ico 的各帧字节能在 exe 里找到（旧帧找不到）。
2. **窗口图标有两处来源**：`KunClient.App.csproj` 的 `<ApplicationIcon>`（exe 自身图标）
   与 `<Window Icon>` / `App.xaml` 里的隐式 `Window` 样式（窗口左上角与任务栏）。
   只改一处会出现「exe 有图标、窗口没有」或反之。`dotnet KunClient.dll` 方式启动时，
   窗口图标取的是 dotnet 的图标——所以窗口级 Icon 也要设。

## 网站（Next.js 静态导出 → /www/wwwroot/KunClient-server/website）

| 脚本 | 作用 |
| --- | --- |
| `check-out.js` | 校验 `website/out` 产物：必备标记存在、禁用文案不存在 |
| `serve-check.js` | 本地静态托管 `out/` 并校验全部 chunk / 资源返回 200、锚点存在（可用 `SERVE_ROOT` / `SERVE_PORT` 覆盖根目录与端口） |
| `deploy-website.js` | 原子部署：打包上传到 `/tmp` → 解到 `website.new` → `mv` 切换，旧版留 `website.old` |
| `verify-live.js` | 线上验收（**打公网 `https://kunawa.xyz`**，`LIVE_BASE` 可覆盖）：文案/旧版残留/chunk/接口/下载开关/控制台/404 共 40+ 项 |
| `deploy-console.js` | 控制台原子部署：先跑 `check-console.js`，不过不上传；再打包 → `console.new` → `mv` 切换，旧版留 `console.old` |

```powershell
cd e:\flashtool\website; npm run build
cd e:\flashtool\tools; node check-out.js; node serve-check.js; node deploy-website.js; node verify-live.js
```

回滚：`mv website website.fail && mv website.old website`

## 登录服务器（Node/Express → /www/wwwroot/KunClient-server/）

| 脚本 | 作用 |
| --- | --- |
| `deploy-all.js` | 一键全量部署：上传 `server/` 与 `server/website/` 全部文件并重启服务 |
| `deploy-server.js` | 上传 `server.js` + 网站/控制台等静态文件并重启服务 |
| `restart-server.js` | 只重启：`pm2 restart KunClient-server`（pm2 无此条目时退回「按端口找 pid 杀掉 → nohup」）→ 探测 3000 |
| `update-version.js` | 更新线上版本号 / 强制更新标记 / 更新日志（库路径：命令行参数 > `REMOTE_DB_PATH` > 由 `remoteBase` 派生） |
| `deploy-config.js` | 部署配置解析模块（被上述脚本引用，非独立执行入口） |

> ⚠️ **重启方式（2026-10-20 实测更正）**：线上进程由 **pm2** 托管，应用名是 **`KunClient-server`**
> （2026-09-21 记的 `kunserver` 已不存在），以 **`www`** 用户运行，只监听 `127.0.0.1:3000`，
> 公网入口是 nginx 的 `kunawa.xyz` / `kunawa.cn`。
> （`pm2 describe KunClient-server` 的 script path / exec cwd 都正确指向 `/www/wwwroot/KunClient-server`）。
> 所以正确做法是 **`pm2 restart KunClient-server`**。
>
> 旧文档说的「pm2 里的 `kunflashtool-server` 指向不存在的目录，不要用 pm2」已不成立——
> 那条僵尸条目现在不存在了，`KunClient-server` 是有效条目。同时 `pkill -9 -f "node.*server.js"` 这条路有坑：
>
> 1. `pkill -f` 的模式会匹配到**执行这条命令的 shell 自己**（它的命令行里就含 `node server.js`），
>    结果 shell 在跑到 `nohup` 之前就被自己杀掉——本次部署就踩了这个，表现为「重启命令无任何输出」；
> 2. 即便没自杀，pm2 也会立刻按自己的记录把进程拉起来，和 `nohup` 拉起的那一份抢 3000 端口
>    （EADDRINUSE），表现为 pm2 里 `unstable restarts` 计数上涨。
>
> 结论：一律 `pm2 restart KunClient-server`。`restart-server.js` 已按此改写（保留「非 pm2 托管」时的兜底）。

## 改完东西要不要重启？——一条判断规则

| 改的是什么 | 需要重启吗 | 为什么 |
| --- | --- | --- |
| `console/index.html`、`website/**`、`admin/**`、`assets/**` 等静态文件 | **不需要** | `express.static` 每个请求都从磁盘重读，上传即生效 |
| `server/config.json`、`server/config.local.json` | **必须重启** | `config.js` 在 `require()` 时读一次并缓存，进程内不会重新加载 |
| `server.js` 本身 | **必须重启** | 代码改动只在进程启动时加载 |

判断方法：改动只落在 `express.static` 挂载的目录里 → 传完直接访问即可；否则用
`restart-server.js` 重启。**重启前后都跑一遍 `verify-live.js`**（30 项线上验收）确认。

## 官网「公测免费 / 正式收费」怎么切

对外口径不写死在官网代码里，而是由控制台 → **管理 → 官网展示** 的 `site_stage` 决定：

| `site_stage` | 官网显示 | 含义 |
| --- | --- | --- |
| `beta`（默认） | 公测期免费使用 | 公测中，客户端不拦截 |
| `paid` | 正式版 · 会员授权 | 配合 `paid_mode` / `vip_required` 拦截未授权账号 |
| `free` | 免费使用 | 长期免费 |

官网首页运行时拉 `/api/site/info`（公开接口）渲染 Hero 徽标、技术规格「使用许可」、数据带与收费 FAQ，
所以**切换后访客刷新即生效，不需要 `npm run build` 也不需要重新部署官网**。
文案集中定义在 `website/src/lib/data.ts` 的 `STAGE_COPY`，改文案只改那一处。

> **已下架（2026-09-21）**：云端 ROM 与模块功能已**整体删除** —— 服务端 `/api/roms`、`/api/modules`、
> `/api/rom-proxy/*`、`/api/admin/content` 全部移除，`server/assets/roms`、`server/assets/modules` 目录删除，
> 客户端「模块专区」页面与导航项删除，控制台不再有「云资源管理」卡片。
> `selftest-content.js` 保留了「这些接口必须 404」的回归断言：既挡「有人把接口加回来」，
> 也挡「部署时漏传 server.js 导致线上还是旧版本」（旧版本会返回 200/401 而不是 404）。
>
> 注意 **不要误删/误改**：`/api/loaders/firehose/*`（EDL 云端引导库）、`/api/packages/tools`、
> `/api/packages/scrcpy` 都是**仍在用**的功能。

## 部署 `config.local.json` 的两个注意点

1. **省略 `jwt_secret`**。`config.js` 的 `ensureJwtSecret()` 在 `jwt_secret` 缺省时会复用
   `.runtime/jwt.secret`，因此省略它可让线上已签发的 token 继续有效——否则上传新
   `jwt_secret` 会让所有用户当场掉登录态。想确认是否复用了，检查启动日志里的
   `jwtReusedFromRuntimeFile: true` 与 `jwtLen: 64`。
2. **省略 `deploy` 节**。该节只给本地部署脚本解析用，服务器上不需要（也不应该）存 root 密码。

> 邮件配置可以用 `nodemailer.createTransport({...}).verify()` 做**只验证不发信**的连通性检查，
> 避免为了测配置给真实用户发邮件。

## 静态文件的原子替换

单个静态文件的热更新走「备份 → 上传 `.new` → `mv -f`」，避免上传到一半被请求读到半截文件：

```bash
cp -a <file> <file>.bak_$(date +%Y%m%d%H%M%S)
# 上传到 <file>.new
mv -f <file>.new <file>
```

回滚：`mv -f <file>.bak_<时间戳> <file>`（无需重启，静态文件立即生效）。
