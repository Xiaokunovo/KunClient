const express = require('express');
const cors = require('cors');
const bcrypt = require('bcryptjs');
const jwt = require('jsonwebtoken');
const initSqlJs = require('sql.js');
const path = require('path');
const fs = require('fs');
const crypto = require('crypto');
const net = require('net');
const https = require('https');
const nodemailer = require('nodemailer');
const cfg = require('./config');

const app = express();
// ====== 软配置（config.json > 环境变量 > 默认值），不要在这里写死 ======
const PORT = cfg.port;
const JWT_SECRET = cfg.jwt_secret;
const DB_PATH = cfg.dbFile;

// Email SMTP（config.json → mail 节；mail.enabled=false 时不创建、不发信）
//   authType='password' → 基础认证（user/pass；QQ 邮箱、自建 SMTP）
//   authType='oauth2'   → 现代认证（Outlook.com / Microsoft 365 必需；nodemailer 会用 refreshToken 自动换取 accessToken）
function buildMailAuth(m) {
  if (String(m.authType || 'password').toLowerCase() === 'oauth2') {
    const o = m.oauth2 || {};
    // ⚠️ 必须显式给 accessUrl：nodemailer 的 xoauth2 默认是 Google 的
    //   https://accounts.google.com/o/oauth2/token —— 不覆盖就会去打 Google，
    //   表现为 connect ETIMEDOUT（Google 段 IP）或 invalid_client。
    const tenant = o.tenant || 'consumers'; // 个人 Outlook.com 用 consumers；企业/学校用租户 ID 或 common
    const auth = {
      type: 'OAuth2',
      user: m.user,
      clientId: o.clientId,
      refreshToken: o.refreshToken,
      accessUrl:
        o.accessUrl || `https://login.microsoftonline.com/${tenant}/oauth2/v2.0/token`
    };
    if (o.clientSecret) auth.clientSecret = o.clientSecret;
    if (o.accessToken) auth.accessToken = o.accessToken;
    return auth;
  }
  return { user: m.user, pass: m.pass };
}

// ---- Microsoft Graph 发送通道（mail.mode='graph'）----
// 背景：个人 Outlook.com 账号的 SMTP/IMAP 可能在账号层面被微软拒绝
// （SMTP: 535 5.7.3 Authentication unsuccessful；IMAP: User is authenticated but not connected），
// 与令牌无关。Graph 的 /me/sendMail 是官方推荐路径，只要 refreshToken 带 Graph Mail.Send 授权即可。
function httpsPost(host, path, headers, body) {
  return new Promise((resolve, reject) => {
    const req = https.request(
      { host, path, method: 'POST', headers: Object.assign({ 'Content-Length': Buffer.byteLength(body) }, headers) },
      (res) => {
        let data = '';
        res.on('data', (c) => (data += c));
        res.on('end', () => resolve({ status: res.statusCode, body: data }));
      }
    );
    req.on('error', reject);
    req.write(body);
    req.end();
  });
}

async function graphAccessToken(m) {
  const o = m.oauth2 || {};
  const tenant = o.tenant || 'consumers';
  const body = new URLSearchParams({
    grant_type: 'refresh_token',
    client_id: o.clientId || '',
    refresh_token: o.refreshToken || ''
  }).toString();
  const r = await httpsPost(
    'login.microsoftonline.com',
    `/${tenant}/oauth2/v2.0/token`,
    { 'Content-Type': 'application/x-www-form-urlencoded' },
    body
  );
  let data;
  try { data = JSON.parse(r.body); } catch (e) { data = null; }
  if (!data || !data.access_token) {
    const desc = data ? ((data.error || '') + ' ' + (data.error_description || '')) : r.body.slice(0, 200);
    const err = new Error('Graph access_token 获取失败: ' + desc.trim());
    err.code = 'EGraphToken';
    throw err;
  }
  return data.access_token;
}

function graphAddr(v) {
  // 兼容 "Name <a@b.c>"、"a@b.c"、{address:'a@b.c'} 三种形态
  if (v && typeof v === 'object') v = v.address || v.text || '';
  const m = /<([^>]+)>/.exec(String(v || ''));
  return (m ? m[1] : String(v || '')).trim();
}

async function sendViaGraph(opts) {
  const token = await graphAccessToken(cfg.mail);
  const toList = [].concat(opts.to || []).map((t) => ({ emailAddress: { address: graphAddr(t) } }));
  const message = {
    subject: opts.subject || '(无主题)',
    body: { contentType: opts.html ? 'HTML' : 'Text', content: opts.html || opts.text || '' },
    toRecipients: toList
  };
  const fromAddr = graphAddr(opts.from);
  if (fromAddr) message.from = { emailAddress: { address: fromAddr } };
  const r = await httpsPost(
    'graph.microsoft.com',
    '/v1.0/me/sendMail',
    { Authorization: 'Bearer ' + token, 'Content-Type': 'application/json' },
    JSON.stringify({ message, saveToSentItems: true })
  );
  if (r.status < 200 || r.status >= 300) {
    const err = new Error('Graph sendMail 失败 http=' + r.status + ': ' + r.body.slice(0, 300));
    err.code = 'EGraphSend';
    throw err;
  }
  return {
    messageId: 'graph-' + Date.now(),
    accepted: toList.map((x) => x.emailAddress.address),
    response: '250 via Microsoft Graph'
  };
}

// 统一入口：mode='graph' 走 Graph API，否则走 SMTP（nodemailer）。
// 调用点只认 sendMail(options)，接口保持不变。
const mailTransporter = (() => {
  if (!cfg.mail.enabled) return null;
  const raw = String(cfg.mail.mode || 'smtp').toLowerCase() === 'graph'
    ? { async sendMail(opts) { return sendViaGraph(opts); } }
    : nodemailer.createTransport({
        host: cfg.mail.host,
        port: cfg.mail.port,
        secure: !!cfg.mail.secure,
        auth: buildMailAuth(cfg.mail)
      });
  return { sendMail: (opts) => sendMailWithRetry(raw, opts) };
})();

// ====== 邮件发送重试 ======
// Gmail SMTP 复用长连接，空闲一会儿后对端会直接掐断，表现是
// `Connection closed unexpectedly` / ETIMEDOUT / ENETUNREACH / ECONNRESET。
// 重连后立刻重发基本都能成功，属于「必须重试」的瞬时错误。
// 反之 550/553（收件人不存在 / 地址非法）是永久错误，重试只会白烧发信配额。
const MAIL_TRANSIENT_RE = /Connection closed unexpectedly|ETIMEDOUT|ENETUNREACH|ECONNRESET|ECONNREFUSED|ESOCKET|socket hang up|Greeting never received|Timeout|Temporary|421|450|451|452/i;
const MAIL_PERMANENT_RE = /5\d\d[ -]|not a valid RFC 5321|all recipients were rejected|recipient address rejected|non-existent account/i;
const MAIL_RETRY_TIMES = 3;      // 总尝试次数（首次 + 2 次重试）
const MAIL_RETRY_DELAY = 1500;   // 首次退避毫秒，之后翻倍

/**
 * 极简 HTML → 纯文本（只处理我们自己模板里会出现的标签，够用即可）。
 *
 * 🔴 为什么必须有：**只有 HTML 正文的邮件是垃圾邮件过滤器最经典的减分项**。
 *    2026-10-24 实测：封禁通知发出去 SMTP 回 `250 queued`、accepted 全绿，
 *    用户 QQ 邮箱却收不到 —— 补上纯文本正文是这一环最容易做对的事。
 *    顺手也覆盖验证码 / 登录提醒 / 更新推送（它们全都只传了 html）。
 */
function htmlToText(html) {
  return String(html || '')
    .replace(/<style[\s\S]*?<\/style>/gi, '')
    .replace(/<head[\s\S]*?<\/head>/gi, '')
    .replace(/<br\s*\/?>/gi, '\n')
    .replace(/<\/(p|div|tr|table|h1|h2|h3|li)>/gi, '\n')
    .replace(/<[^>]+>/g, '')
    .replace(/&nbsp;/g, ' ')
    .replace(/&lt;/g, '<')
    .replace(/&gt;/g, '>')
    .replace(/&quot;/g, '"')
    .replace(/&#39;/g, "'")
    .replace(/&amp;/g, '&')
    .replace(/[ \t]+/g, ' ')
    .replace(/\n{3,}/g, '\n\n')
    .trim();
}

async function sendMailWithRetry(raw, opts) {
  // 没给纯文本就自动生成一份：调用方只管写 html，别再逐个记得补 text
  if (opts && opts.html && !opts.text) opts = { ...opts, text: htmlToText(opts.html) };
  let lastErr = null;
  for (let attempt = 1; attempt <= MAIL_RETRY_TIMES; attempt++) {
    try {
      return await raw.sendMail(opts);
    } catch (e) {
      lastErr = e;
      const msg = (e && (e.message || e.response || '')) + '';
      // 永久错误直接放弃，别重试
      if (MAIL_PERMANENT_RE.test(msg) && !MAIL_TRANSIENT_RE.test(msg)) throw e;
      if (attempt >= MAIL_RETRY_TIMES) break;
      const wait = MAIL_RETRY_DELAY * Math.pow(2, attempt - 1);
      console.warn(`[Email] 第 ${attempt} 次发送失败（瞬时错误，${wait}ms 后重试）: ${msg.slice(0, 160)}`);
      await new Promise((r) => setTimeout(r, wait));
    }
  }
  throw lastErr;
}
const MAIL_FROM = cfg.mail.from;

// ── IP 归属地查询（登录通知邮件用）─────────────────────────────────────────
// 用公开的 ip-api.com 免费接口（45 次/分钟，无需 key），只查**公网** IP。
// 内网 / 回环 / 非法 IP 直接跳过，不自造「本机」之类的假地点——宁可留空。
// 结果做 5 分钟内存缓存：同一 IP 短时间内反复登录不必反复外呼。
// ⚠️ 绝不让它拖慢登录：3 秒超时 + 任何异常都返回 null，调用方降级成空串。
const GEO_CACHE = new Map();          // ip -> { text, expireAt }
const GEO_TTL   = 5 * 60 * 1000;
const GEO_TIMEOUT = 3000;

function isPrivateIp(ip) {
  if (!ip) return true;
  const s = String(ip).trim();
  if (s === '127.0.0.1' || s === '::1' || s === 'localhost') return true;
  if (/^10\./.test(s)) return true;
  if (/^192\.168\./.test(s)) return true;
  if (/^172\.(1[6-9]|2\d|3[01])\./.test(s)) return true;
  if (/^169\.254\./.test(s)) return true;
  if (/^f[cd]/i.test(s)) return true;                // IPv6 ULA
  if (!/^[\da-fA-F:.]+$/.test(s)) return true;       // 非 IP 形态（空串/怪值）一律跳过
  return false;
}

/**
 * 邮件里展示用的打码 IP（2026-10-07 用户要求「IP 后面隐藏一下」，随后细化为「打两位」）。
 * 目的是让用户能认出「是不是自己常用的网段」，又不把完整地址写进邮件正文
 * （邮件可能被转发 / 长期留存在第三方邮箱服务器）。
 *   IPv4 → 只留前 2 段：36.21.134.117  ⇒  36.21.*.*
 *   IPv6 → 保留前 2 组，其余用 * 顶掉：2408:8214:1a2b:...  ⇒  2408:8214:*
 * ⚠️ IPv6 按「第 2 个冒号」做字符串截断，**不 split/join 重组** ——
 *    重组会把 `::` 压缩写法里的空组吃掉，`2001:db8::1` 会变成 `2001:db8:1:*`（语义全错）。
 * 解析不出形态的（怪值 / 空）返回空串，交给模板显示占位符，绝不硬凑。
 */
function maskIp(ip) {
  const s = String(ip || '').trim();
  if (!s) return '';
  // IPv4：`a.b.c.d` → `a.b.*.*`。前面已由 canonicalIp 归一，这里只认标准四段式。
  const v4 = s.match(/^(\d{1,3}\.\d{1,3})\.\d{1,3}\.\d{1,3}$/);
  if (v4) return v4[1] + '.*.*';
  // IPv6：数到第 2 个冒号就截断，保留原有写法（含 :: 压缩）。
  if (s.includes(':')) {
    const idx = s.indexOf(':', s.indexOf(':') + 1);
    // 第 2 个冒号不存在（`fe80::1` 这种只有一组前缀）→ 拿不到两个真实组。
    if (idx === -1) return '*';
    const head = s.slice(0, idx);
    // 前缀里必须真含地址字符（排除 `::` 这种纯压缩占位）→ 否则整体打码。
    if (!/[\da-fA-F]/.test(head)) return '*';
    return head + ':*';
  }
  return '';
}

async function ipLocation(ip) {
  if (isPrivateIp(ip)) return '';
  const key = String(ip).trim();
  const hit = GEO_CACHE.get(key);
  if (hit && hit.expireAt > Date.now()) return hit.text;
  try {
    const ctl = new AbortController();
    const timer = setTimeout(() => ctl.abort(), GEO_TIMEOUT);
    const r = await fetch(
      `http://ip-api.com/json/${encodeURIComponent(key)}?lang=zh-CN&fields=status,country,regionName,city`,
      { signal: ctl.signal });
    clearTimeout(timer);
    const j = await r.json();
    let text = '';
    if (j && j.status === 'success') {
      // 只拼非空段，避免出现「中国  undefined 苏州市」这种脏数据
      text = [j.country, j.regionName, j.city].filter(Boolean).join(' ');
    }
    GEO_CACHE.set(key, { text, expireAt: Date.now() + GEO_TTL });
    return text;
  } catch {
    // 查不到就缓存空串，短时间内别对同一个 IP 反复超时等待
    GEO_CACHE.set(key, { text: '', expireAt: Date.now() + GEO_TTL });
    return '';
  }
}

// ── 安全响应头（2026-10-01，免依赖手写；不引 helmet 以避免新增 npm 包）────────
// 不加 CSP：站点/控制台含内联脚本，贸然上 CSP 会把页面打白，收益不抵风险。
app.disable('x-powered-by');
app.use((req, res, next) => {
  res.setHeader('X-Content-Type-Options', 'nosniff');
  res.setHeader('X-Frame-Options', 'SAMEORIGIN');
  res.setHeader('Referrer-Policy', 'strict-origin-when-cross-origin');
  res.setHeader('X-Permitted-Cross-Domain-Policies', 'none');
  res.setHeader('Permissions-Policy', 'geolocation=(), microphone=(), camera=()');
  next();
});

// ── CORS 收敛（2026-10-01）──────────────────────────────────────────────
// 原来 `cors()` = 反射任意来源，等于任何网站都能在浏览器里调用本站 API。
// 现在只放行白名单来源；**无 Origin 的请求照常放行**（桌面客户端 / curl / 同源都不带 Origin）。
const CORS_ORIGINS = new Set([
  'https://kunawa.cn', 'https://www.kunawa.cn',
  'http://kunawa.cn', 'http://www.kunawa.cn',
]);
app.use(cors({
  origin: (origin, cb) => cb(null, !origin || CORS_ORIGINS.has(origin)),
}));

// 客户端日志上传（2026-10-12）：载荷 = 最多 5 个日志文件的 base64（客户端合计裁到 2MB，
// base64 后约 2.7MB），全局 1mb 解析器会先跑并 413 —— 必须按路径在全局解析器**之前**
// 挂大限额（body-parser 看到 req._body 已置就直接跳过，所以路由级 limit 轮不到）。
// verify 钩子照挂：sigGuard 要用 req.rawBody 的原始字节算 sha256 验签
// （该端点已进 SIG_REQUIRED_PATHS）。
// 注：原来的「壁纸上传 40mb」同款注释随云端背景功能一起删了（2026-10-21）。
app.use('/api/v2/client/logs', express.json(Object.assign({ limit: '8mb' }, { verify: (req, res, buf) => { req.rawBody = buf; } })));
// v2 请求签名需要原始字节做 sha256（JSON.stringify 再序列化的字节不一定与请求体一致），
// 所以在全局解析器上挂 verify 钩子把原始缓冲留在 req.rawBody。
app.use(express.json(Object.assign({ limit: '1mb', verify: (req, res, buf) => { req.rawBody = buf; } })));
app.use('/', express.static(path.join(__dirname, 'website')));
// 🔴 控制台 / 管理后台是「源码直出、文件名不带内容哈希」的静态资源，必须每次回源协商：
//    默认的 public,max-age=0 会被 Cloudflare 按边缘 TTL 放大成 4 小时，部署完用户看到的还是旧 JS
//    （2026-10-20 实测：origin 已经是新文件，浏览器里还显示旧的会员页）。
//    no-cache = 可以缓存但每次必须校验，ETag 命中就是 304，代价很小。
// 用 no-store 而不是 no-cache：Cloudflare 的 Browser Cache TTL 会把 no-cache 改写成
// 'max-age=14400, must-revalidate'（实测），只有 no-store 能穿过 CF 不被改写。
const noCacheStatic = (res) => { res.setHeader('Cache-Control', 'no-store, no-cache, must-revalidate'); res.setHeader('Pragma', 'no-cache'); };
app.use('/admin', express.static(path.join(__dirname, 'admin'), { setHeaders: noCacheStatic }));
app.use('/console', express.static(path.join(__dirname, 'console'), { setHeaders: noCacheStatic }));
// 防御纵深（2026-10-12）：客户端日志已移出 assets（见 CLIENT_LOG_DIR），但万一
// 将来有人手滑写回 assets/logs、或线上留有旧目录，这里也一律 404。
// 必须在下面那条 `/assets` static **之前**注册：express.static 放行的请求
// 会绕过其后的所有中间件，鉴权逻辑插不上手 —— 顺序反了就等于没写。
app.use('/assets/logs', (req, res) => res.status(404).end());
// ── 安装包防盗链（2026-10-12「更新鉴权」）──────────────────────────────
// app_settings.download_guard 三档：
//   off    不拦（**默认**）—— 官网静态链接、旧客户端直连、CF 边缘缓存都依赖 /assets 直链；
//          贸然收紧会一次性打断这三条链路，所以默认关，何时收紧由运维拍板。
//   ua     拦「既无有效票据、UA 又不含 KunClient」的直连下载（挡爬虫/镜像站，放行客户端）。
//   strict 只认有效票据（等新版客户端普及 + 官网链接改造后再开）。
// 🔴 必须注册在下面 `/assets` static **之前**：express.static 放行的请求会绕过其后的中间件。
// ⚠️ 挂载在 '/assets' 时 req.path 是**相对路径**（'/KunClient-x.exe'），不是 '/assets/...'。
app.use('/assets', (req, res, next) => {
  // 🔴 内测阶段：客户端包直链一律 403。**只挡客户端包本体**（KunClient*.exe|.7z）——
  //    /assets/tools/adb.exe、fastboot.exe 这些工具二进制是客户端功能依赖，不能一起挡。
  if (isInternalStage() && /^\/KunClient[^\/]*\.(exe|7z)$/i.test(req.path)) {
    res.setHeader('Cache-Control', 'no-store');   // 防 CF 把 403 也缓存住：内测结束后要能立刻恢复
    return res.status(403).json({
      success: false, code: 'INTERNAL_STAGE', message: '内测阶段暂不开放下载。',
    });
  }
  if (!/\.(exe|7z)$/i.test(req.path)) return next();
  const mode = String(getSetting('download_guard', 'off') || 'off').toLowerCase();
  if (mode === 'off') return next();
  const file = decodeURIComponent(String(req.path).replace(/^\/+/, ''));
  if (!downloadAllowlist().includes(file)) return next();   // 非当前版本的历史包不归本闸管
  const r = verifyDownloadTicket(req.query.t);
  if (r && r.file === file) return next();
  if (mode === 'ua' && /KunClient\//i.test(String(req.headers['user-agent'] || ''))) return next();
  return res.status(403).json({
    success: false, code: 'DL_FORBIDDEN',
    message: '请从官网或客户端内下载最新版本。',
  });
});
// E3（2026-10-07）：/assets 显式声明缓存头。express.static 默认 maxAge=0 ——
// 每次都要回源协商（CF 未命中时 TTFB 0.75–1.47s 全花在这里）；etag/lastModified
// 也显式写上，不依赖 express 版本的默认值（"去隐式依赖"）。
app.use('/assets', express.static(path.join(__dirname, 'assets'),
  { maxAge: '4h', etag: true, lastModified: true }));

// ══════════════════════════════════════════════════════════════════════════════
// IP 封禁（2026-09-28，用户要求「增加一个封禁 ip」）
// ══════════════════════════════════════════════════════════════════════════════
// 管理员在控制台录入具体 IP，该来源的**任何 /api 请求**一律拒绝（含登录、注册、发验证码）——
// 所以这一层必须注册在所有 /api 路由之前，包括顶部那两个模块级路由（/api/tools/:filename 等）。
//
// 取 IP：nginx 反代把真实来源放进 X-Forwarded-For（最左是原始客户端）。
// 本项目 app.listen 只绑 127.0.0.1、公网只能经 nginx 进来，所以 XFF 可信；
// 但为了本地直连（curl http://localhost:3000）也能用，回落到 socket 地址。
//
// ══════════════════════════════════════════════════════════════════════════════
// IP 归一化：IPv4 + **IPv6**（2026-09-29 用户要求「封禁 ip 需要支持 IPV6」）
// ══════════════════════════════════════════════════════════════════════════════
// 🔴 归一化必须**双向一致**：管理员录入的 IP 与 clientIpOf() 从请求里取到的 IP
//    必须经过**同一个函数**，否则会出现「明明封了却拦不住」—— 而且不报错，
//    只是静默不匹配（本项目最怕的就是这种静默失效）。
//
// 为什么 IPv6 一定要先规范化再比：同一个地址的合法写法有几十种 ——
//   2001:DB8::1 / 2001:db8:0:0:0:0:0:1 / 2001:0db8:0000::0001 / 2001:db8::0:1 …
//   直接按字符串比对必然对不上。这里统一压成 **RFC 5952** 规范形式：
//   小写、每段去前导零、最长连续零段压成 `::`（并列取最左），最多 8 段。
//
// 还处理两种「同一个来源、两种写法」：
//   · IPv4-mapped IPv6（`::ffff:1.2.3.4`，双栈 socket 常给这个）→ 压成纯 IPv4 `1.2.3.4`，
//     这样封 `1.2.3.4` 能挡住 `::ffff:1.2.3.4` 的请求，反过来也成立；
//   · 方括号写法（`[::1]` / `[::1]:8080`）与 zone id（`fe80::1%eth0`）→ 去掉外壳再解析。
// 解析全靠 Node 内置 `net.isIP()` 做合法性判定（0/4/6），不引第三方依赖。
function expandIpv6(s) {
  // 尾部内嵌 IPv4（如 2001:db8::192.0.2.1）先折成两个 16 位段
  let head = s, tail = null;
  const m = /^(.*:)(\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3})$/.exec(s);
  if (m) {
    const q = m[2].split('.').map(Number);
    if (q.some(n => !Number.isInteger(n) || n < 0 || n > 255)) return null;
    tail = [((q[0] << 8) | q[1]).toString(16), ((q[2] << 8) | q[3]).toString(16)];
    head = m[1];
  }
  const parts = head.split('::');
  if (parts.length > 2) return null;
  // ⚠️ split 出来的空段必须滤掉，否则**尾部内嵌 IPv4 的写法会整条解析失败**：
  //    正则的 `(.*:)` 是贪婪的，head 一定以 `:` 结尾，于是
  //      `::ffff:1.2.3.4`        → head=`::ffff:`  → parts[1]='ffff:'  → split → ['ffff',''] ✗
  //      `0:0:0:0:0:ffff:1.2.3.4`→ head=`0:0:…:ffff:`（无 `::`）→ left 末尾多一个 '' ✗
  //    这两种都会因「空段过不了 1~4 位十六进制校验」而被判非法 —— 连带有两个后果：
  //      · 管理员封不了 `::ffff:1.2.3.4`（双栈 socket 上 Node 常给这个形式）
  //      · `::ffff:1.2.3.4` 收敛不成 `1.2.3.4`，「封 IPv4 挡不住 mapped 请求」
  //    滤空段不会放过非法输入：调用方 canonicalIp 已经先用 `net.isIP()` 把
  //    非法形态全挡在门外了（`1:::2:3.4.5.6` 这类 isIP 直接给 0）。
  const seg = a => a.split(':').filter(x => x !== '');
  const left = parts[0] ? seg(parts[0]) : [];
  const right = parts.length === 2 ? (parts[1] ? seg(parts[1]) : []) : null;
  let groups;
  if (right === null) {
    // 没有 `::`：段数必须正好凑满 8 —— 内嵌 IPv4 折出来的 2 段也要算进去
    // （`0:0:0:0:0:ffff:1.2.3.4` 是 6 段 + 内嵌 IPv4，不是「6 段非法」）
    if (left.length !== 8 - (tail ? 2 : 0)) return null;
    groups = left;
  } else {
    const fill = 8 - left.length - right.length - (tail ? 2 : 0);
    if (fill < 0) return null;
    groups = left.concat(new Array(fill).fill('0'), right);
  }
  if (tail) groups = groups.concat(tail);
  if (groups.length !== 8) return null;
  const nums = [];
  for (const g of groups) {
    if (!/^[0-9a-fA-F]{1,4}$/.test(g)) return null;
    nums.push(parseInt(g, 16));
  }
  return nums;
}

function compressIpv6(groups) {
  const hex = groups.map(n => n.toString(16));
  // 找最长的一段连续 0（RFC 5952：只有 ≥2 段才压缩，长度并列时取最靠左的）
  let start = -1, len = 0;
  for (let i = 0; i < 8;) {
    if (groups[i] !== 0) { i++; continue; }
    let j = i;
    while (j < 8 && groups[j] === 0) j++;
    if (j - i > len) { start = i; len = j - i; }
    i = j;
  }
  if (len < 2) return hex.join(':');
  return hex.slice(0, start).join(':') + '::' + hex.slice(start + len).join(':');
}

/** 任意 IP 文本 → 规范形式（IPv4 去前导零 / IPv6 压成 RFC 5952）；非法返回 ''。 */
function canonicalIp(raw) {
  let s = String(raw == null ? '' : raw).trim();
  if (!s) return '';
  const br = /^\[([^\]]+)\](?::\d+)?$/.exec(s);   // [::1]:8080 → ::1
  if (br) s = br[1];
  s = s.split('%')[0].toLowerCase();               // 去掉 zone id，统一小写（十六进制大小写不影响地址）
  const kind = net.isIP(s);
  if (kind === 4) {
    return s.split('.').map(x => String(Number(x))).join('.');
  }
  if (kind !== 6) return '';
  const g = expandIpv6(s);
  if (!g) return '';
  // IPv4-mapped 的**各种写法**都收敛到纯 IPv4：`::ffff:1.2.3.4` 与 `0:0:0:0:0:ffff:1.2.3.4` 等价。
  // 双栈 socket 上 Node 常给 `::ffff:` 形式，不收敛的话「封 1.2.3.4」挡不住它。
  if (g[0] === 0 && g[1] === 0 && g[2] === 0 && g[3] === 0 && g[4] === 0 && g[5] === 0xffff) {
    return `${g[6] >> 8}.${g[6] & 255}.${g[7] >> 8}.${g[7] & 255}`;
  }
  const c = compressIpv6(g);
  // 回环：双栈下本地直连会被报成 ::1（各种写法都归到这里），与 127.0.0.1 视作同一来源。
  return c === '::1' ? '127.0.0.1' : c;
}

function clientIpOf(req) {
  // 取 IP 顺序（2026-10-01 收紧，修「直连源站可伪造 XFF」）：
  //   ① X-Real-IP —— nginx 由 realip 模块按 CF-Connecting-IP 写成的**真实客户端 IP**，
  //      且 proxy_set_header 会**覆盖**客户端自带的同名头 → 无法伪造。
  //   ② X-Forwarded-For 最左段 —— 仅在①缺失时兜底；直连源站时可被伪造，故不再优先。
  //   ③ socket 地址 —— 本机直连（curl http://localhost:3000）时的最后兜底。
  const xrip = String(req.headers['x-real-ip'] || '').trim();
  const xff = String(req.headers['x-forwarded-for'] || '').split(',')[0].trim();
  const raw = String(xrip || xff || (req.socket && req.socket.remoteAddress) || '').trim();
  // 解析不出来时**原样返回**，别换成空串：这个值还会写进登录日志，
  // 宁可把怪值记下来留线索，也不要丢掉它（匹配不上封禁名单的后果与空串一样）。
  return canonicalIp(raw) || raw;
}

/** 生效中的封禁记录；已过期 / 库还没建表一律返回 null（= 不拦）。 */
function bannedIpRow(ip) {
  if (!ip) return null;
  try {
    const row = rowToObj(db.exec(
      "SELECT ip, reason, expire_at, created_at FROM banned_ips WHERE ip = ?", [ip]));
    if (!row) return null;
    // expire_at 为空 = 永久；有值则判断是否仍在有效期内。
    // ⚠️ 别写成 `expire_at > datetime('now')` 裸比字符串：ISO 的 'T' > ' '，
    //    会把「当天就该到期」的封禁判成「还没到期」（额度链路上踩过同一个坑）。
    if (row.expire_at && !isVipActive(row.expire_at)) return null;
    return row;
  } catch { return null; }
}

function banMessage(row) {
  const until = row.expire_at ? `（封禁至 ${String(row.expire_at).slice(0, 10)}）` : '（永久封禁）';
  return `你的 IP 已被管理员封禁${until}${row.reason ? '：' + row.reason : ''}`;
}

app.use('/api', (req, res, next) => {
  const row = bannedIpRow(clientIpOf(req));
  if (row) return res.status(403).json({ success: false, code: 'IP_BANNED', message: banMessage(row) });
  next();
});

// ── 简易内存限流（2026-10-01，免依赖手写）───────────────────────────────
// 滑动窗口：每个 key 存窗口内的命中时间戳。进程重启即清零 ——
// 与 ENV_INSTANCES / APPEAL_IP_HITS 同一套取舍（不做每请求落库，避免整库重写的写放大）。
function makeLimiter(max, windowMs) {
  const hits = new Map();
  function hit(key) {
    const now = Date.now();
    let arr = hits.get(key);
    if (!arr) { arr = []; hits.set(key, arr); }
    while (arr.length && now - arr[0] > windowMs) arr.shift();
    if (arr.length >= max) return false;
    arr.push(now);
    if (hits.size > 20000) {   // 粗粒度防内存膨胀
      for (const [k, v] of hits) if (!v.length || now - v[v.length - 1] > windowMs) hits.delete(k);
    }
    return true;
  }
  // E4：退额度。登录**成功**这类「合法且低频」的动作不该消耗防撞库额度 ——
  // 否则 NAT 出口（校园网/公司网）下的正常用户攒够次数后会被 429 拦住，
  // 表现就是「登录有时失败，但提示与密码无关」。只退最后一条命中，窗口语义不变。
  hit.refund = (key) => {
    const arr = hits.get(key);
    if (arr && arr.length) arr.pop();
  };
  return hit;
}
const apiLimiter       = makeLimiter(600, 60 * 1000);      // 全局：600 次/分钟/IP（宽松，防刷）
const loginIpLimiter   = makeLimiter(20, 10 * 60 * 1000);  // 登录：20 次/10 分钟/IP
const loginUserLimiter = makeLimiter(8, 10 * 60 * 1000);   // 登录：8 次/10 分钟/(IP+账号)
const registerLimiter  = makeLimiter(5, 60 * 60 * 1000);   // 注册：5 次/小时/IP
const codeLimiter      = makeLimiter(8, 60 * 60 * 1000);   // 发验证码：8 次/小时/IP
// 更新接口：120 次/分钟/IP（2026-10-12「更新鉴权」）—— 本端点刻意不做签名硬校验
// （旧版升级入口，拦了就死锁），于是成了最容易被脚本轮询/爬取下载链接的地方，单独收紧。
// 120 是「远高于任何真实客户端、又足以掐断轮询脚本」的量：真实客户端一次启动只查一次。
const versionLimiter   = makeLimiter(120, 60 * 1000);

// E7：429 必须带 Retry-After（客户端/浏览器可据此提示「x 秒后再试」，而不是干等），
// 登录另有专属文案 —— 泛泛的「操作频繁」用户不知道是登录被限了、更不知道限多久。
const rateLimited = (res, retryAfterSec = 60) => {
  res.setHeader('Retry-After', String(retryAfterSec));
  return res.status(429).json({ success: false, code: 'RATE_LIMITED', message: '操作过于频繁，请稍后再试' });
};
const loginRateLimited = (res) => {
  res.setHeader('Retry-After', String(10 * 60));
  return res.status(429).json({
    success: false, code: 'RATE_LIMITED',
    message: '登录尝试过于频繁：同一 IP 每 10 分钟最多 20 次、同一账号最多 8 次，请稍后再试',
  });
};

app.use('/api', (req, res, next) => {
  if (!apiLimiter(clientIpOf(req))) return rateLimited(res);
  next();
});

// ── API v2（2026-10-11 全量重构）─────────────────────────────────────────
// 路径层：全部业务路由已挂 /api/v2/*。这里把旧路径 /api/*（v1）内部重写到 v2，
// 让已发行的 1.1.4.0 客户端 / 控制台页面在过渡期继续可用；响应带 Deprecation 头。
// 注意挂载顺序：封禁检查 → 全局限流 → 本重写 → 版本闸门 → 签名守卫 → 业务路由。
app.use('/api', (req, res, next) => {
  if (!req.path.startsWith('/v2/') && req.path !== '/v2') {
    res.setHeader('Deprecation', 'v1; sunset="2026-12-31"');
    req.url = '/v2' + req.url;
    // 🔴 标记：这是 v1 旧路径重写进来的请求（1.1.4.0 客户端 / 官网 / 控制台）。
    //    它们不可能带 v2 签名头，sigGuard 必须放行 —— 否则官网登录也被 401 SIG_REQUIRED。
    req.v1Rewritten = true;
  }
  next();
});

// ── 请求签名（防脚本直刷 / 防重放）───────────────────────────────────────
// 模型：客户端先 POST /api/v2/client/handshake 换一次性会话 {sid, secret}（secret 只走
// HTTPS 一次），之后每个受保护请求带四个头：
//   X-Sig-Sid    会话 id
//   X-Sig-Ts     毫秒时间戳（±120s 窗口）
//   X-Sig-Nonce  每请求唯一的随机串（服务端查重，重放直接拒）
//   X-Sig        base64(HMAC_SHA256(secret, ts + '\n' + nonce + '\n' + method + '\n' + path + '\n' + sha256hex(body)))
// 旧版客户端（不带签名头）打到受保护路径 → 401 SIG_REQUIRED，提示升级。
// 会话存内存：进程重启全部失效，客户端重新握手即可（与限流器同一取舍）。
const SIG_TS_WINDOW = 120 * 1000;
const sigSessions = new Map();     // sid -> { secret, expireAt, usedNonces: Map<nonce, expireAt> }
const SIG_SESSION_TTL = 24 * 60 * 60 * 1000;
const SIG_NONCE_TTL = 5 * 60 * 1000;
function sigSessionPrune() {
  const now = Date.now();
  for (const [sid, s] of sigSessions) {
    if (s.expireAt <= now) { sigSessions.delete(sid); continue; }
    for (const [n, exp] of s.usedNonces) if (exp <= now) s.usedNonces.delete(n);
  }
}
function sigIssueSession(ip) {
  sigSessionPrune();
  if (sigSessions.size > 50000) sigSessionPrune();   // 粗粒度防内存膨胀
  const sid = crypto.randomBytes(16).toString('hex');
  const secret = crypto.randomBytes(32).toString('hex');
  sigSessions.set(sid, { secret, expireAt: Date.now() + SIG_SESSION_TTL, usedNonces: new Map() });
  return { sid, secret, expiresIn: SIG_SESSION_TTL / 1000 };
}
function timingSafeEq(a, b) {
  const ba = Buffer.from(String(a)), bb = Buffer.from(String(b));
  if (ba.length !== bb.length) return false;
  return crypto.timingSafeEqual(ba, bb);
}
const sha256hex = (s) => crypto.createHash('sha256').update(s, 'utf8').digest('hex');

/** 受保护路径（v2）：命中即强制签名。浏览器控制台/官网走的端点不在表里。 */
const SIG_REQUIRED_PATHS = [
  '/api/v2/login',
  '/api/v2/register',
  '/api/v2/auth/key-login',
  '/api/v2/auth/code-login',
  '/api/v2/auth/code-register',
  '/api/v2/app/check',
  '/api/v2/app/consume',
  '/api/v2/client/env/challenge',
  '/api/v2/client/env/report',
  '/api/v2/client/env/challenge-anon',
  '/api/v2/client/env/anonymous',
  '/api/v2/usage/log',
  '/api/v2/feedback',
  '/api/v2/client/logs',
];

function sigGuard(req, res, next) {
  // v1 旧路径重写进来的请求（官网 / 控制台 / 1.1.4.0 客户端）不带 v2 签名头，
  // 它们由重写层保证兼容（Deprecation sunset 2026-12-31），不归签名守卫管。
  if (req.v1Rewritten) return next();
  // 2026-10-13：登录态标记（仅签名校验，不含封禁拦截）。更新接口据此判断请求者是否
  // 已登录，从而决定是否签发下载票据（匿名用户拿不到票，无法走鉴权下载端点）。
  req.v2LoggedIn = !!userPayloadOf(req);
  // 不在受保护表里的路径**不拦**，但仍尝试验一次签名：验过就打 req.v2Signed，
  // 让「更新接口」这类不能硬拦的端点能识别出可信客户端（渐进增强，见下方下载票据）。
  const need = SIG_REQUIRED_PATHS.includes(req.path);
  const sid = String(req.headers['x-sig-sid'] || '');
  const ts = String(req.headers['x-sig-ts'] || '');
  const nonce = String(req.headers['x-sig-nonce'] || '');
  const sig = String(req.headers['x-sig'] || '');
  if (!sid || !ts || !nonce || !sig) {
    if (!need) return next();
    return res.status(401).json({ success: false, code: 'SIG_REQUIRED', message: '客户端版本过旧，请升级到最新版本' });
  }
  const session = sigSessions.get(sid);
  if (!session || session.expireAt <= Date.now()) {
    if (!need) return next();
    return res.status(401).json({ success: false, code: 'SIG_SESSION_EXPIRED', message: '签名会话已过期，请重新握手' });
  }
  const tsNum = parseInt(ts, 10);
  if (!Number.isFinite(tsNum) || Math.abs(Date.now() - tsNum) > SIG_TS_WINDOW) {
    if (!need) return next();
    return res.status(401).json({ success: false, code: 'SIG_EXPIRED', message: '请求时间戳超出允许窗口' });
  }
  if (session.usedNonces.has(nonce)) {
    if (!need) return next();
    return res.status(401).json({ success: false, code: 'SIG_REPLAY', message: '检测到重放请求' });
  }
  const bodyHash = sha256hex(req.rawBody ? req.rawBody.toString('utf8') : '');
  const expected = crypto.createHmac('sha256', session.secret)
    .update(ts + '\n' + nonce + '\n' + req.method + '\n' + req.path + '\n' + bodyHash)
    .digest('base64');
  if (!timingSafeEq(sig, expected)) {
    if (!need) return next();
    return res.status(401).json({ success: false, code: 'SIG_INVALID', message: '请求签名校验失败' });
  }
  session.usedNonces.set(nonce, Date.now() + SIG_NONCE_TTL);
  req.v2Signed = true;      // 可信客户端标记：更新接口据此签发下载票据
  next();
}
// ── 下载票据（2026-10-12「更新鉴权」）────────────────────────────────────
// 为什么需要：/api/v2/app/version 与 /assets/*.exe 是升级链路的唯二入口，原本**完全公开**
// —— 任何人可查版本、可直连下载，第三方镜像/爬虫能批量抓包，脚本轮询也拉不住。
//
// 但不能直接给它们上签名守卫：旧版客户端（1.1.4.0 / 2.0.0.x）不带 v2 签名头，
// 一拦就「既用不了、也升不上来」—— 版本接口上方的注释已写明这条硬约束。
//
// 于是用**票据**做渐进增强：版本接口给请求签一张短时效票据，下载端点凭票放行。
// 票据放 URL query，客户端**无需任何改造**：UpdateService::resolveDownloadUrl 会补全
// 站内相对路径，isExeUrl 会先切掉 ?query 再判 .exe 后缀（这两处都已正确处理 query）。
const DL_TICKET_TTL = 10 * 60 * 1000;   // 10 分钟，够一次下载 + 重试
const DL_TICKET_KEY = crypto.createHmac('sha256', String(JWT_SECRET))
  .update('kunclient-download-ticket-v1').digest();   // 域分离：不复用 JWT 原密钥

function signDownloadTicket(file) {
  const payload = `${Date.now() + DL_TICKET_TTL}|${file}`;
  const mac = crypto.createHmac('sha256', DL_TICKET_KEY).update(payload).digest('base64url');
  return Buffer.from(payload, 'utf8').toString('base64url') + '.' + mac;
}

/** 校验票据；通过返回 { file, exp }，失败返回 null（不细分原因，避免给爆破方回执）。 */
function verifyDownloadTicket(ticket) {
  const t = String(ticket || '');
  const dot = t.lastIndexOf('.');
  if (dot <= 0) return null;
  let payload;
  try { payload = Buffer.from(t.slice(0, dot), 'base64url').toString('utf8'); }
  catch { return null; }
  const bar = payload.indexOf('|');
  if (bar <= 0) return null;
  const exp = parseInt(payload.slice(0, bar), 10);
  const file = payload.slice(bar + 1);
  if (!Number.isFinite(exp) || exp < Date.now() || !file) return null;
  const expect = crypto.createHmac('sha256', DL_TICKET_KEY).update(payload).digest('base64url');
  if (!timingSafeEq(t.slice(dot + 1), expect)) return null;
  return { file, exp };
}

/** 允许下载的文件名白名单 —— 只认 app_settings.download_url 指向的 exe 与其同名 7z。
 *  白名单是**防目录穿越**的关键：票据里的 file 必须逐字命中，绝不把用户输入拼进路径。 */
function downloadAllowlist() {
  const dl = String(getSetting('download_url', '') || '');
  const exe = decodeURIComponent(((dl.split('/').pop() || '').split('?')[0] || ''));
  const out = [];
  if (exe) out.push(exe);
  if (/\.exe$/i.test(exe)) out.push(exe.replace(/\.exe$/i, '.7z'));
  return out;
}

/** 内测阶段（控制台「运营 › 官网展示 › 内测阶段」）——**全站取消下载**：
 *  ① 官网 + 控制台收起下载入口与版本号（/api/v2/site/info 的 downloadEnabled → false）；
 *  ② /assets 上的**客户端包**直链一律 403（工具二进制 adb.exe/fastboot.exe 不受影响）；
 *  ③ /api/v2/app/version **不再下发 downloadUrl / zipUrl**。
 *  🔴 ③ 是硬要求，不是修饰：线上 force_update=1，若仍下发地址而文件已 403，存量客户端会
 *     卡在「强制更新 → 下载失败 → QApplication::quit()」被彻底锁死。
 *  与 site_download_enabled 是**两级**开关：这一级是「全站总闸」，那一级是「官网单项」。 */
function isInternalStage() {
  return String(getSetting('internal_stage', '0') || '0') === '1';
}

/** 云端更新（= 客户端自更新通道）总开关（控制台「运营 › 官网展示 › 云端更新」，2026-10-14）。
 *  '1' = 开放客户端自更新（默认）/'0' = 暂时关闭。
 *  🔴 与 internal_stage **正交**，有意拆成两级：
 *     internal_stage 管「能不能**下载**」（官网/控制台入口 + /assets 直链 403）；
 *     cloud_update_enabled 管「客户端要不要**自更新**」。
 *     内测期通常一起关，但恢复时可以分别放行（例如「官网恢复下载、但暂不推送自更新」）。
 *  关闭时的效果：
 *   ① /api/v2/app/version 不下发 downloadUrl / zipUrl / sizeText / downloadTicket，
 *      并回 updateEnabled=false + updateClosedReason —— 客户端据此显示「暂不开放更新」，
 *      而不是误报「已是最新版本」；
 *   ② 带票下载端点 /api/v2/app/download/:file 一律 403（与 /assets 直链同口径）；
 *   ③ **不影响**官网 / 控制台的手动下载（那走 /assets 直链，由 internal_stage + site_download_enabled 管）。 */
function isCloudUpdateEnabled() {
  return String(getSetting('cloud_update_enabled', '1') || '1') === '1';
}
/** 客户端自更新通道当前是否真的开放 = 开关开着 **且** 不在内测阶段。 */
function isCloudUpdateOpen() {
  return isCloudUpdateEnabled() && !isInternalStage();
}

// 握手限流：30 次/小时/IP —— 换会话本身不能成为新的爆破入口。
const handshakeLimiter = makeLimiter(30, 60 * 60 * 1000);
// 卡密登录专属限流：卡密是爆破主向量（直接对着 license_keys 枚举），10 次/10 分钟/IP。
const keyLoginLimiter = makeLimiter(10, 10 * 60 * 1000);

const toolsDir = path.join(__dirname, 'assets', 'tools');
const toolsZip = path.join(__dirname, 'assets', 'tools.zip');

// ====== 用户名规则（注册 / 改名 / 控制台建号改号共用一处）======
// 旧规则 ^[a-zA-Z0-9_]{3,20}$ 只允许纯 ASCII，中文昵称等一律被拒，限制过死。
// 现放宽为 2-24 位，支持 中文 / 字母 / 数字 / 下划线 / 中划线 / 点 / @。
// 仍刻意排除了空白与控制字符，避免出现"看不见的用户名"和前后空格导致的重复账号。
const USERNAME_RE = /^[\u4e00-\u9fffA-Za-z0-9_.\-@]{2,24}$/;
// ⚠️ 措辞必须与另外两处**逐字一致**：客户端 `UsernameRule.CharsetMessage`、
//    控制台 `usernameErr()`。三份是同一句话的三个镜像 ——
//    2026-09-30 订正：这里原来写「2-24 个字符」，另两处写「2-24 位」，
//    用户在客户端看到的提示与 API 直接返回的不一样（连本文件下面的注释都写的是「位」）。
const USERNAME_RULE_MSG = '用户名需 2-24 位，支持中文、字母、数字、下划线、中划线、点和 @';
const USERNAME_ALL_DIGITS_MSG = '用户名不能是纯数字，请混入字母、中文或下划线（例如 kun123）';

/**
 * 纯数字用户名一律拒绝（2026-09-29 用户要求「限制用户名不得是数字」）。
 *
 * 为什么用 Unicode 属性类而不是 `/^\d+$/`：`\d` 只认 ASCII 的 0-9，
 * 于是 `１２３`（全角）、`١٢٣`（阿拉伯-印度数字）、`Ⅻ`（罗马数字）这些**看着就是数字**的
 * 用户名照样建得出来 —— 而用户提这条要求的目的恰恰是"别让账号名看起来像编号"。
 * `\p{Nd}` = 十进制数字（含全角/其它文字的数字），`\p{Nl}` = 罗马数字等字母式数字，
 * `\p{No}` = 上标/分数等其它数字 → 三类一起拦。
 * ⚠️ 属性类必须带 `u` 标志，否则会被当成普通转义序列静默失效（写成没有 `u` 就是不拦）。
 *
 * ⚠️ 只在**创建 / 改名**时判，**登录不判** —— 历史账号里如果已经存在纯数字名，
 * 加一道登录校验等于把人锁在门外（而且他连改名都进不去）。要清理老数据另行处理。
 */
function isAllDigitsUsername(v) {
  const s = String(v == null ? '' : v).trim();
  if (!s) return false;
  return /^[\p{Nd}\p{Nl}\p{No}]+$/u.test(s);
}

/**
 * 用户名校验的**唯一出口**：返回 null = 通过，否则返回给前端直接展示的中文文案。
 * 新增规则只改这里 —— 散在 5 个路由里写就会出现"注册拦住了、管理员建号没拦住"这种漏洞。
 *
 * 🔴 顺序：**先判纯数字**，再判字符集。全角数字（`１２３`）不在字符集白名单里，若字符集优先
 * 它会被报成「用户名需 2-24 位，支持中文、字母、数字…」（用户看着莫名其妙：明明都是数字）。
 * 纯数字优先后，任何纯数字名在任何入口都拿到同一句话。
 * 客户端与控制台两份镜像必须同序。
 * ⚠️ 2026-09-30 起 **5 个入口全部走本函数**（此前 `POST /api/users` 自成一套：
 *    只有 `length < 3` + 纯数字，既不查字符集也不查 {2,24} 的上限 —— 见该路由处的说明）。
 */
function usernameError(v) {
  if (isAllDigitsUsername(v)) return USERNAME_ALL_DIGITS_MSG;
  if (!USERNAME_RE.test(v)) return USERNAME_RULE_MSG;
  return null;
}

/**
 * 大文件 base64 内存缓存（2026-10-02）：/api/packages/tools(~5MB) 与
 * /api/packages/scrcpy(~11MB) 原来每次请求都 readFileSync + toString('base64')，
 * 同步阻塞事件循环上百毫秒，且 base64 结果（+33% 体积）每请求重算一遍。
 * 改为按 mtime 缓存：文件没变就直接复用上次的 base64 串。
 */
const FILE_B64_CACHE = new Map();   // path -> { mtimeMs, b64 }
function cachedFileBase64(p) {
  const st = fs.statSync(p);
  const hit = FILE_B64_CACHE.get(p);
  if (hit && hit.mtimeMs === st.mtimeMs) return hit.b64;
  const b64 = fs.readFileSync(p).toString('base64');
  FILE_B64_CACHE.set(p, { mtimeMs: st.mtimeMs, b64 });
  return b64;
}

/**
 * 邮箱验证码「猜码」防护（2026-10-02）：6 位数字码在 10 分钟有效期内原本可以无限次试错
 * （100 万种组合，配合并发脚本几十秒就能扫完一个码）。现在按「邮箱|type」在内存里记失败
 * 次数：连续错 5 次即视为该验证码作废（ locked ），必须重新获取。只进内存不落库 ——
 * 重启清零无妨，攻击者面对的窗口依然从 10 分钟缩到 5 次尝试。
 */
const CODE_FAILS = new Map();       // "email|type" -> { n, expireAt }
const CODE_FAIL_MAX = 5;
function codeFailState(email, type) {
  const k = String(email) + '|' + String(type);
  const v = CODE_FAILS.get(k);
  if (v && Date.now() > v.expireAt) { CODE_FAILS.delete(k); return null; }
  return { k, v };
}
function codeFailLocked(email, type) {
  const s = codeFailState(email, type);
  return !!(s && s.v && s.v.n >= CODE_FAIL_MAX);
}
function codeFailHit(email, type) {
  const s = codeFailState(email, type);
  const k = s ? s.k : String(email) + '|' + String(type);
  const v = (s && s.v) || { n: 0, expireAt: 0 };
  v.n += 1;
  v.expireAt = Date.now() + 15 * 60 * 1000;   // 失败计数 15 分钟滚动窗口
  CODE_FAILS.set(k, v);
  return v.n;
}
function codeFailClear(email, type) { CODE_FAILS.delete(String(email) + '|' + String(type)); }
const CODE_LOCKED_MSG = '验证码尝试次数过多已作废，请重新获取';

/**
 * 统一的验证码校验出口：锁定判定 + datetime() 归一化比对 + 失败计数一把做掉，
 * 5 个校验点（reset / code-login / register / bind / change_email）不再各写一遍。
 * 返回 { locked: true } | { row: null } | { row }；成功方在消费后自行 codeFailClear。
 */
function fetchValidCode(email, type, code) {
  if (codeFailLocked(email, type)) return { locked: true };
  const row = rowToObj(db.exec(
    "SELECT * FROM verification_codes WHERE email = ? AND code = ? AND type = ? AND datetime(expires_at) > datetime('now') ORDER BY id DESC LIMIT 1",
    [email, code, type]));
  if (!row) { codeFailHit(email, type); return { row: null }; }
  return { row };
}

/**
 * 二进制文件下载统一出口（E1/E2，2026-10-07）：原始字节流 + Range + 显式缓存头。
 *
 * 为什么要它：/api/packages/tools|scrcpy 只有「JSON 塞 base64」一种形态，
 * 比原始 zip 多 33% 字节（tools 5.12→6.83MB、scrcpy 11.3→15.07MB），
 * 而 1.0–1.4 MB/s 的链路上这些字节全是实打实的下载时间。
 *
 * Range（E2）：断点续传的基础 —— 客户端中断后可从断点接着下，不用重下整个包。
 * Cache-Control：与 /assets 静态路由同口径（4 小时），避免每次回源协商。
 */
function sendBinaryFile(req, res, filePath, downloadName) {
  const st = fs.statSync(filePath);
  const size = st.size;
  const etag = `"${size.toString(16)}-${Math.floor(st.mtimeMs).toString(16)}"`;

  res.setHeader('Accept-Ranges', 'bytes');
  res.setHeader('Cache-Control', 'public, max-age=14400');
  res.setHeader('ETag', etag);
  res.setHeader('Last-Modified', st.mtime.toUTCString());
  res.setHeader('Content-Type', 'application/octet-stream');
  res.setHeader('Content-Disposition',
    `attachment; filename="${encodeURIComponent(downloadName || path.basename(filePath))}"`);

  // 条件请求：客户端带着我们的 ETag/日期回来时直接 304，省掉整包字节
  if (req.headers['if-none-match'] === etag) { res.status(304).end(); return; }
  const ims = req.headers['if-modified-since'];
  if (ims && Date.parse(ims) >= Math.floor(st.mtimeMs / 1000) * 1000) { res.status(304).end(); return; }

  const range = req.headers.range;
  if (!range) {
    res.setHeader('Content-Length', size);
    fs.createReadStream(filePath).pipe(res);
    return;
  }

  // bytes=start-end / bytes=start- / bytes=-suffix
  let start;
  let end;
  const m = /^bytes=(\d*)-(\d*)$/.exec(String(range).trim());
  if (m && (m[1] !== '' || m[2] !== '')) {
    if (m[1] === '') {
      const suffix = Number(m[2]);
      start = Number.isFinite(suffix) ? Math.max(0, size - suffix) : 0;
      end = size - 1;
    } else {
      start = Number(m[1]);
      end = m[2] === '' ? size - 1 : Math.min(Number(m[2]), size - 1);
    }
  }
  if (start === undefined || !Number.isFinite(start) || !Number.isFinite(end) || start > end || start >= size) {
    res.setHeader('Content-Range', `bytes */${size}`);
    res.status(416).end();
    return;
  }
  res.status(206);
  res.setHeader('Content-Range', `bytes ${start}-${end}/${size}`);
  res.setHeader('Content-Length', end - start + 1);
  fs.createReadStream(filePath, { start, end }).pipe(res);
}

// E1：二进制端点 —— 客户端 DownloadPackageFromServerAsync 优先打这里（-33% 字节），
// 失败才回落下面的 base64 端点（老客户端继续可用）。
app.get('/api/v2/packages/tools/file', (req, res) => {
  if (!fs.existsSync(toolsZip)) return res.status(404).json({ success: false, message: 'Tools package not found' });
  sendBinaryFile(req, res, toolsZip, 'tools.zip');
});

app.get('/api/v2/packages/tools', (req, res) => {
  if (!fs.existsSync(toolsZip)) return res.status(404).json({ success: false, message: 'Tools package not found' });
  // 2026-10-02：~5MB 的包不再每请求 readFileSync+base64（同步阻塞 + 重复 CPU），
  // 改为按 mtime 缓存 base64 结果（见 cachedFileBase64）。
  const b64 = cachedFileBase64(toolsZip);
  res.json({ success: true, data: b64, size: Math.floor(b64.length * 3 / 4) });
});

app.get('/api/v2/tools/:filename', (req, res) => {
  const allowed = ['adb.exe', 'fastboot.exe', 'AdbWinApi.dll', 'AdbWinUsbApi.dll'];
  if (!allowed.includes(req.params.filename)) return res.status(403).json({ success: false, message: 'Forbidden' });
  const filePath = path.join(toolsDir, req.params.filename);
  if (!fs.existsSync(filePath)) return res.status(404).json({ success: false, message: 'Not found' });
  res.download(filePath, req.params.filename);
});

let db;

// 在线状态（2026-09-28「云端强制下线」配套）：内存里记「最后一次带有效 token 的请求时间」。
// 为什么不落库：每次请求都写 users + saveDb() 会把整个数据库文件重写一遍，写放大不可接受。
// pm2 是单进程 fork 模式，内存足够；进程重启后 Map 清空 → 控制台退回显示 last_login，
// 而不是错误地显示「从没在线过」。
const LAST_SEEN = new Map();
const ONLINE_WINDOW_MS = 90 * 1000;   // 客户端每 30s 轮询一次 /api/profile，90s 无动静视为离线

// ══ 来源 IP 采集（2026-09-29 状态：**恢复采集，仅开发者可见**）══
// 历史沿革，别再照着旧注释改代码：
//   ① 早期：解析 x-forwarded-for / remoteAddress → 落库 → 任何界面都能看。
//   ② 中间某版：按「不留存 IP」整体删掉采集，`ip_address` 列留空；
//      注释里一度写着「服务端不再写入、也不再向任何界面下发」。
//   ③ 现在（本条）：**恢复采集**——登录来源 IP 写进 login_logs.ip_address，
//      但**只对 developer 下发**：两个出口（/api/logs、/api/auth/login-logs）都
//      按角色决定 SELECT 哪些列，非开发者拿到的 JSON 里根本没有 ip_address 这个键。
//      客户端「最近登录记录」也照此渲染（见 console/index.html 的 loadLoginLogs）。
//
// 采集入口是 `clientIpOf()`：取 XFF **最左**（真实来源），再走 `canonicalIp()` 归一
// （IPv4 逐段去前导零 / IPv6 压成 RFC 5952 规范写法 / IPv4-mapped 收敛成纯 IPv4）。
// **归一化失败时原样返回**——宁可把怪值记下来留线索，也别因为解析不了就写了一片空白。
//
// IP 封禁（bannedIpMiddleware）与这里是**同一个** clientIpOf：录入用的
// normalizeIpInput 也是同一个 canonicalIp。两边各写一套归一化 = 静默失效
// （名单里存规范写法，请求来的是另一种等价写法 → 永远匹配不上，而且不报错）。

function saveDb() {
  const data = db.export();
  fs.writeFileSync(DB_PATH, Buffer.from(data));
}

function rowToObj(r) {
  if (!r.length || !r[0].values.length) return null;
  const obj = {};
  r[0].columns.forEach((c, i) => obj[c] = r[0].values[0][i]);
  return obj;
}

function rowsToArr(r) {
  if (!r.length) return [];
  return r[0].values.map(v => {
    const obj = {};
    r[0].columns.forEach((c, i) => obj[c] = v[i]);
    return obj;
  });
}

// ====== 角色层级与鉴权（RBAC） ======
// 三级角色，等级越高权限越大：user(0) < admin(2) < developer(3)（agent 已于 2026-10-21 删除）
// 核心规则（顺序敏感）：
//   1) 任何人都不能管理「等级 >= 自己」的账号 —— 不能动同级，更不能动上级
//      例外：开发者（最高权）可以管理全部账号，包括其他开发者和自己
//   2) 任何人都不能把角色分配成「等级 >= 自己」的角色 —— 管理员造不出第二个管理员
//      例外：开发者可以分配任意角色（含 developer）
//   3) 任何人都不能修改自己的角色 —— 防自提权
//      例外：开发者可以改自己的角色（已是最高权，不存在提权；且 config.json 的 developer 播种
//            会在每次启动时把该账号校正回 developer，误改可自愈）
//   4) 鉴权一律以**数据库里的当前角色**为准，绝不信 JWT 里带的旧 role
//      —— 否则管理员被降权后，旧 token 在 24h 有效期内仍然是管理员
// 2026-10-21：**代理（agent）角色已删除** —— 只剩 普通用户 / 管理员 / 开发者 三级。
// 存量库里如果有 role='agent' 的行，normalizeRole 会把它降成 'user'（见下），不需要迁移脚本。
// 数值保留空洞（0 / 2 / 3）是**故意的**：各处比较都用 ROLE_RANK.admin 这类符号，
// 但日志、审计里出现过的历史数字不该突然换个含义。
const ROLE_RANK = { user: 0, admin: 2, developer: 3 };
const ROLE_LABEL = { user: '普通用户', admin: '管理员', developer: '开发者' };

function normalizeRole(r) {
  const v = String(r || '').trim().toLowerCase();
  return ROLE_RANK[v] === undefined ? 'user' : v;
}
function rankOf(r) { return ROLE_RANK[normalizeRole(r)]; }
// 后台人员：能进入控制台「管理」页的角色（管理员与开发者）
function isStaff(role) { return rankOf(role) >= ROLE_RANK.admin; }
// 开发者是最高权，对所有账号（含同级、含自己）都放行
function isDeveloper(role) { return normalizeRole(role) === 'developer'; }
// actor 能否管理 target：默认必须严格高于对方等级；开发者不受此限
function canManage(actorRole, targetRole) {
  if (isDeveloper(actorRole)) return true;
  return rankOf(actorRole) > rankOf(targetRole);
}
// actor 能否分配某角色：默认只能分配严格低于自己的角色；开发者可分配任意角色
function canAssignRole(actorRole, role) {
  if (isDeveloper(actorRole)) return true;
  return rankOf(actorRole) > rankOf(role);
}

function loadActorById(userId) {
  // session_epoch 必须一起取：authMiddleware 要靠它判断「这份 token 是不是被强制下线之前签发的」。
  // banned_until/ban_reason/ban_level：authMiddleware 的 activeBanOf 靠它们拦「自动时限封禁」——
  //   2026-10-02 修复：原先没 SELECT 这三列，fresh.banned_until 恒为 undefined，
  //   isBanActive() 永远 false，环境检测触发的时限封禁对所有已登录会话完全不生效。
  // mid_strike/mid_strike_day：evaluateEnvSignal 靠它们按自然日累积 MID 级信号 ——
  //   同根因：原先恒为 undefined → strike 永远 = 1，永远达不到阈值，累积机制整个失效。
  try {
    return rowToObj(db.exec(
      "SELECT id, username, role, status, session_epoch, banned_until, ban_reason, ban_level, mid_strike, mid_strike_day FROM users WHERE id = ?",
      [userId]));
  }
  catch { return null; }
}

/**
 * 签发 token（全站唯一出口，2026-09-28 新增）。
 *
 * 🔴 为什么不能让各处直接 jwt.sign：
 *    「云端强制下线」要求把「被踢之前签发的 token」全部作废。最初的做法是拿 JWT 自带的
 *    iat（签发时间）去和 users.session_epoch 比大小，但 **iat 只有秒级精度** ——
 *    管理员踢人 与 该用户的某次登录 落在**同一秒**时 `iat === epoch`，「iat < epoch」不成立，
 *    这份 token 就活了下来（探针里因为登录→踢人全是本机请求、不到 1 秒，必然踩中）。
 *    改成把「签发当时该用户的 session_epoch 快照」写进 token（字段 se）后，判定变成
 *    `token.se < 当前 session_epoch` —— 与时钟精度彻底无关：
 *      · 踢之前签发的：se = 踢前的旧值  < 新值 ⇒ 失效
 *      · 踢之后重新登录：se = 新值，等于当前值 ⇒ 可用（同一秒重登也照样可用）
 *    调用方仍然只传 { userId, username, role }，se 由这里现查现写，
 *    所以 6 处调用点都是同一行机械替换，不用各自去凑字段。
 */
function signToken(payload) {
  let se = 0;
  try {
    const r = rowToObj(db.exec("SELECT session_epoch FROM users WHERE id = ?", [payload.userId]));
    se = Number((r && r.session_epoch) || 0);
  } catch { se = 0; }
  return jwt.sign(Object.assign({}, payload, { se }), JWT_SECRET, { expiresIn: '24h' });
}

function authMiddleware(req, res, next) {
  const h = req.headers.authorization;
  // E8：401 一律带 code —— 客户端（MembershipService 的 B3）据此把 401/403 归 noauth
  // 而不是「无法连接服务器」；没有 code 就只能靠文案猜，改文案即破坏判断。
  if (!h || !h.startsWith('Bearer ')) return res.status(401).json({ success: false, code: 'NO_TOKEN', message: '未登录' });
  let payload;
  try { payload = jwt.verify(h.split(' ')[1], JWT_SECRET); }
  catch { return res.status(401).json({ success: false, code: 'TOKEN_INVALID', message: 'Token无效' }); }

  // 以数据库为准重新取当前身份（token 里的 role/status 可能已过期）
  const fresh = loadActorById(payload.userId);
  if (!fresh) return res.status(401).json({ success: false, code: 'ACCOUNT_GONE', message: '账号不存在或已被删除' });

  // 封禁拦截（2026-09-30：含环境检测触发的**时限封禁**）。
  // 🔴 这里只**拦请求**，绝不去递增 session_epoch —— 用户可能正在刷机，
  //    把客户端强制下线有可能变砖（既有红线：绝不真实踢人）。封禁只在下次登录/请求时生效。
  const banHit = activeBanOf(fresh);
  if (banHit) {
    const ap = appealOf(fresh.id);
    return res.status(403).json({
      success: false, code: 'BANNED',
      message: banMessageOf(banHit, ap),
      until: banHit.until, permanent: banHit.permanent, appeal: ap,
    });
  }
  if (fresh.status && fresh.status !== 'active')
    return res.status(403).json({ success: false, message: '账号已被停用' });

  // 云端强制下线（2026-09-28）：管理员在控制台点「强制下线」会把 session_epoch 递增到一个新值。
  // 判定用的不是时间，而是 token 里那枚「签发时的 session_epoch 快照」（se，见 signToken 注释）：
  // se 比当前值小 ⇒ 这份 token 是「被踢之前」签发的，直接失效。
  //
  // ⚠️ 必须带上 code=KICKED：客户端要据此把「被管理员强制下线」与「登录态自然过期」区分开 ——
  //    前者要清掉本地登录态并弹窗说明原因，后者原本是静默重试。混成一个 401 会误踢用户。
  const curEpoch = Number(fresh.session_epoch || 0);
  if (curEpoch > 0 && Number(payload.se || 0) < curEpoch)
    return res.status(401).json({ success: false, code: 'KICKED', message: '账号已被管理员强制下线，请重新登录' });

  // 在线状态：带有效 token 的请求打一次时间戳（只进内存，不写库）。
  // 放在 KICKED 之后 —— 被踢的请求不该把账号标记成「在线」。
  LAST_SEEN.set(Number(fresh.id), Date.now());

  req.actor = fresh;
  req.actorRole = normalizeRole(fresh.role);
  req.user = Object.assign({}, payload, { role: req.actorRole });
  next();
}

function adminMiddleware(req, res, next) {
  if (!isStaff(req.actorRole)) return res.status(403).json({ success: false, message: '无权限' });
  next();
}

// 仅开发者（最高权）可操作：修改管理员等级、系统级设置等
function developerMiddleware(req, res, next) {
  if (req.actorRole !== 'developer') return res.status(403).json({ success: false, message: '仅开发者可操作' });
  next();
}

// 轻量登录态校验（2026-10-13「更新鉴权·登录鉴权机制」）：
// 仅验 JWT 签名 + 账号存在/未停用/未被强制下线，**不做**封禁拦截（那是 authMiddleware 的事，
// 更新链路不该因封禁直接断了）。返回 payload 或 null。
// 用途：更新接口据此判断「请求是否来自已登录用户」，从而决定是否签发下载票据——
// 匿名/过期/被踢的 token 一律视为未登录，拿不到票。
function userPayloadOf(req) {
  const h = req.headers.authorization;
  if (!h || !h.startsWith('Bearer ')) return null;
  try {
    const payload = jwt.verify(h.split(' ')[1], JWT_SECRET);
    const fresh = loadActorById(payload.userId);
    if (!fresh) return null;
    if (fresh.status && fresh.status !== 'active') return null;
    const curEpoch = Number(fresh.session_epoch || 0);
    if (curEpoch > 0 && Number(payload.se || 0) < curEpoch) return null;  // 被强制下线
    return payload;
  } catch {
    return null;
  }
}

// 后台人员可访问：卡密管理、用户查询（原「代理及以上」，代理角色已删 → 现在只认管理员）
function staffMiddleware(req, res, next) {
  if (rankOf(req.actorRole) < ROLE_RANK.admin) return res.status(403).json({ success: false, message: '无权限' });
  next();
}

// 数据可见范围：能进后台的（管理员/开发者）看全部
function isGlobalStaff(req) { return rankOf(req.actorRole) >= ROLE_RANK.admin; }

// 卡密守卫：非后台人员只能操作自己签发的卡密（owner_id = 自己）
function guardKey(req, res) {
  const key = rowToObj(db.exec("SELECT * FROM license_keys WHERE id = ?", [req.params.id]));
  if (!key) { res.status(404).json({ success: false, message: '卡密不存在' }); return null; }
  if (!isGlobalStaff(req) && key.owner_id !== req.user.userId) {
    res.status(403).json({ success: false, message: '权限不足：只能管理自己签发的卡密' });
    return null;
  }
  return key;
}

// 目标账号守卫：检出目标并校验「等级严格低于自己」。校验失败时会直接应答并返回 null。
function guardTarget(req, res) {
  const id = parseInt(req.params.id, 10);
  if (!Number.isFinite(id)) { res.status(400).json({ success: false, message: '无效的用户 ID' }); return null; }
  const target = rowToObj(db.exec("SELECT id, username, role, status FROM users WHERE id = ?", [id]));
  if (!target) { res.status(404).json({ success: false, message: '用户不存在' }); return null; }
  if (!canManage(req.actorRole, target.role)) {
    res.status(403).json({ success: false, message: '权限不足：不能管理同级或更高等级的账号（仅开发者可管理全部）' });
    return null;
  }
  return target;
}


// 2026-10-02：Math.random() 不是密码学安全的（V8 内部是可预测的 PRNG），
// 验证码是 6 位数字、可被离线预测/碰撞。一律改 crypto.randomBytes。
function generateCode() {
  return String(100000 + (crypto.randomBytes(4).readUInt32BE(0) % 900000));
}

function getSetting(key, fallback = '') {
  try {
    const r = rowToObj(db.exec("SELECT value FROM app_settings WHERE key = ?", [key]));
    return r ? r.value : fallback;
  } catch { return fallback; }
}

function setSetting(key, value) {
  try {
    const r = db.exec("SELECT key FROM app_settings WHERE key = ?", [key]);
    if (r.length && r[0].values.length) db.run("UPDATE app_settings SET value = ? WHERE key = ?", [String(value), key]);
    else db.run("INSERT INTO app_settings (key, value) VALUES (?, ?)", [key, String(value)]);
    saveDb();
  } catch (e) { console.error('[Settings]', e.message); }
}

/**
 * 版本号比较：a 是否比 b 新。按点分段做数字比较（'1.0.10' > '1.0.9'），
 * 非法/缺失段按 0 处理。用于判断"这次是不是真的发版了"。
 */
function isNewerVersion(a, b) {
  const pa = String(a || '').split('.').map(x => parseInt(x, 10) || 0);
  const pb = String(b || '').split('.').map(x => parseInt(x, 10) || 0);
  const n = Math.max(pa.length, pb.length);
  for (let i = 0; i < n; i++) {
    if ((pa[i] || 0) !== (pb[i] || 0)) return (pa[i] || 0) > (pb[i] || 0);
  }
  return false;
}

// ════════════════════════════════════════════════════════════════════════════
// 客户端版本硬闸门（2026-10-09）：**关闭以往版本**
// ════════════════════════════════════════════════════════════════════════════
// 目的：旧版本客户端（含密钥被静态提取过的版本）一律拒绝进入，必须升到
//       min_client_version 及以上。
//
// 版本从哪来：客户端每个业务请求的 User-Agent 形如
//     KunClient/1.1.4.0 (Windows NT 10.0; Win64; x64) KunFlashTool
//   （由客户端 ServerGate.UserAgent 注入；2026-10 起所有业务请求改走
//     ServerGate.CreateClient，保证这一路都带上）。
//   选 UA 而不新造请求头的原因：UA 本来就必须带（空 UA 在网关侧是强机器人信号），
//   复用它不新增协议面，也不必逐个请求点去加字段。
//
// ⚠️ 能力边界：UA 是明文、可任意伪造 —— 它挡的是「正常使用的旧版本客户端」，
//    不是恶意攻击者（对方改个字符串就绕过了）。真正的安全边界在服务端侧的
//    密钥轮换与额度裁决，不在「客户端自证版本」。别拿它去承诺防破解。
//
// 判定：app_settings.min_client_version 为空 ⇒ 闸门关闭（默认，避免误封）；
//      非空时，UA 解析不出版本（= 比底线更老的客户端）或低于底线 ⇒ 426。
const CLIENT_UA_RE = /\bKunClient\/(\d+(?:\.\d+)*)/i;

/** 从 User-Agent 取客户端版本号；取不到返回 ''。 */
function clientVersionOf(req) {
  const m = CLIENT_UA_RE.exec(String((req.headers && req.headers['user-agent']) || ''));
  return m ? m[1] : '';
}

function minClientVersion() {
  return String(getSetting('min_client_version', '') || '').trim();
}

/** 该请求的客户端是否达到版本底线（未配置底线时一律放行）。 */
function clientVersionOk(req) {
  // 🔴 内测阶段：**不执行版本闸门**。内测期下载被全线关掉（/assets 客户端包 403、
  //    更新接口不下发 downloadUrl），此时若仍按下限拦截，低于下限的存量客户端会
  //    「既进不来、又升不了级」—— 直接被锁死。「不开放下载」与「不强制升级」必须成对；
  //    否则一旦 min_client_version 被抬到当前版本（线上现为 2.0.0.4），就是一次全量 lockout。
  if (isInternalStage()) return true;
  const min = minClientVersion();
  if (!min) return true;                       // 未启用闸门
  const v = clientVersionOf(req);
  // 🔴 2026-10-10 修：认不出版本 ≠ 老客户端。控制台 / 官网 / 浏览器都**不带** KunClient UA，
  //    而它们登录要打 /api/login、/api/auth/*、/api/profile（这些都在 CLIENT_GATED_PATHS 里）。
  //    原逻辑「无版本 ⇒ 426」会把控制台 / 官网一起拒掉 —— 用户实测「官网不让登录」。
  //    正确口径：闸门只管**自称是本客户端**的请求（带 KunClient/<ver>）；不带该 UA 的
  //    （浏览器 / 控制台 / 官网 / 脚本）不归版本闸门管，交给它们各自的鉴权。
  if (!v) return true;                         // 非 KunClient 请求 ⇒ 放行
  return !isNewerVersion(min, v);              // 本客户端 v >= min
}

function clientVersionGate(req, res, next) {
  if (clientVersionOk(req)) return next();
  const min = minClientVersion();
  const v = clientVersionOf(req) || '(无版本UA)';
  console.warn(`[VersionGate] 拒绝旧版客户端 ${v} < ${min}  ${req.method} ${req.path}`);
  return res.status(426).json({
    success: false,
    code: 'VERSION_TOO_OLD',
    minVersion: min,
    clientVersion: clientVersionOf(req),
    downloadUrl: resolveDownloadUrl(),
    message: `当前客户端版本过低（要求 ${min} 或更高），请到官网下载最新版本后重试。`,
  });
}

/**
 * 需要闸门的「客户端专用」路径。
 * 🔴 刻意**不含** /api/app/version 与 /api/app/limits：
 *   · version 是升级入口 —— 拦了它，旧版就查不到该升到哪，等于把自救的路也堵死；
 *   · limits 是公开接口，官网（浏览器 UA，认不出 KunClient/）也在读，拦了会误伤官网。
 * 🔴 也**不能**挂到 authMiddleware 上：控制台 / 管理接口共用它，管理员从浏览器访问
 *   不带客户端 UA，全局挂会把管理台一起拒掉（authMiddleware/adminMiddleware 是分开的两层）。
 * 挂载点见下方路由注册前（同一作用域，需 app 在身）。
 */
const CLIENT_GATED_PATHS = [
  '/api/v2/login',
  '/api/v2/register',
  '/api/v2/auth/key-login',
  '/api/v2/auth/code-login',
  '/api/v2/auth/code-register',
  '/api/v2/auth/send-code',
  '/api/v2/auth/reset-password',
  '/api/v2/app/check',
  '/api/v2/app/consume',
  '/api/v2/app/quota',
  '/api/v2/client/env/challenge',
  '/api/v2/client/env/challenge-anon',
  '/api/v2/client/env/report',
  '/api/v2/client/env/anonymous',
  '/api/v2/client/handshake',
  '/api/v2/profile',
  '/api/v2/appeal',
  '/api/v2/client/logs',
];

/**
 * 邮件里要用的下载地址，**必须是绝对 URL**。
 * app_settings.download_url 存的是站内相对路径（如 /assets/KunClient.exe），
 * 相对链接在邮件客户端里是无效的——用户点「立即下载新版本」没有任何反应。
 * 所以这里统一补上 cfg.publicUrl；已经是 http(s) 的原样返回。
 */
function resolveDownloadUrl() {
  const raw = String(getSetting('download_url', '') || '').trim();
  if (/^https?:\/\//i.test(raw)) return raw;
  const rel = raw || '/assets/KunClient.exe';
  const base = String(cfg.publicUrl || '').replace(/\/+$/, '');
  return base + (rel.startsWith('/') ? rel : '/' + rel);
}

function randomIndex(n) {
  // 拒绝采样取均匀随机数：直接对 256 取模会在 n 不整除 256 时有轻微偏斜。
  const limit = Math.floor(256 / n) * n;
  for (;;) {
    const v = crypto.randomBytes(1)[0];
    if (v < limit) return v % n;
  }
}
function genSegment() {
  const chars = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';
  let seg = '';
  for (let i = 0; i < 4; i++) seg += chars[randomIndex(chars.length)];
  return seg;
}
function generateKey() {
  const chars = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789';
  const segments = ['KUN'];
  for (let s = 0; s < 3; s++) {
    let seg = '';
    for (let i = 0; i < 4; i++) seg += chars[randomIndex(chars.length)];
    segments.push(seg);
  }
  return segments.join('-');
}

// ====== 邮件视觉 v4（2026-10-18 重做）======
// 🔴 四类邮件（验证码 / 反馈 / 环境告警 / 版本更新）与三端（客户端 / 控制台 / 官网）
//    共用同一套品牌令牌 —— 邮件是用户唯一从「站外」看到品牌的地方，色板漂了最扎眼。
//    令牌与 website/src/app/globals.css、server/console/index.html 的 :root 同源。
//
// ⚠️ 邮件客户端的硬约束（踩过就知道为什么代码长这样）：
//    · 不认 CSS 变量、不认 <style> 里的类选择器（Gmail 会剥）、不认外部样式表；
//    · 不认 flex / grid / position —— 布局一律 <table role="presentation">；
//    · 按钮不能用 <a> 加 padding 就完事（Outlook 会把 padding 丢掉），必须外包一层
//      带背景色的 <td>，<a> 只负责撑开和上色；
//    · 所以这里是一份**无 var() 的内联副本**，别想着抽公共 CSS。
//
// v4 相对 v3 的改动：
//   · 页头从「20px 深蓝大色块」改成「白底 + 顶部 4px 强调条 + 品牌行」。
//     深色大色块在 Gmail 深色模式下会被整体反色成惨白一块，而且它把标题压得很闷；
//   · 圆角 8 → 14（卡片）/ 8（按钮），描边换成更冷的 #e3e9f2；
//   · 验证码从「小方框里的 28px」改成「虚线圈出的 34px 大字 + 有效期副行」，
//     手机上第一眼就能抄；
//   · 新增 mailButton / mailPanel / mailInfoTable 三个原语，正文不再手搓样式 ——
//     以前每封信都在重复写 padding 和 border，改一处色值要翻四处。
const MAIL_BRAND = {
  accent:     '#7ab8f3',   // 品牌强调色（强调条 / 实心按钮底）
  accentDeep: '#2f7fd0',   // 压在白底上的强调色文字（#7ab8f3 当文字用对比度不够）
  accentInk:  '#10304f',   // 压在强调色底上的文字（= --pri-ink）
  accentWash: '#eef5fe',   // 强调色浅底（验证码块 / 信息面板）
  accentLine: '#a9cdf5',   // 强调色描边（虚线框 / 强描边）
  accentWashLine: '#d8e8fb', // 浅底上的发丝描边（更新内容面板）
  page:       '#eef2f8',   // 页底（卡片之外的那一圈）
  card:       '#ffffff',
  ink:        '#101828',   // 正文
  ink2:       '#4b5a71',   // 次级说明
  ink3:       '#8a97ab',   // 标签 / 页脚
  line:       '#e3e9f2',
  lineSoft:   '#eff3f9',
  danger:     '#c0392b',
  dangerWash: '#fdf0ee',
  dangerLine: '#f0c4bd',
};

const MAIL_FONT = "-apple-system,BlinkMacSystemFont,'Segoe UI','PingFang SC','Microsoft YaHei',sans-serif";
const MAIL_MONO = "'SF Mono',SFMono-Regular,Consolas,'Cascadia Mono','Courier New',monospace";

/** 品牌行：左「logo 图 + KunClient 字标」，右一枚小字 kicker。
 *  2026-10-20：原来是纯文字「K」徽标，用户反馈「邮件里没有 logo」—— 换成真图。
 *  🔴 邮件里只能用**绝对 URL**（相对路径在邮件客户端里根本不发请求），
 *     且必须写死 width/height（Outlook 不认 CSS 宽高），并给 alt：
 *     多数客户端默认拦外部图片，此时 alt 会顶上来，不至于只剩一块空白。 */
function mailBrandRow(kicker) {
  const b = MAIL_BRAND;
  const logoUrl = String(cfg.publicUrl || 'https://kunawa.xyz').replace(/\/+$/, '') + '/logo.png';
  return `<table role="presentation" width="100%" cellpadding="0" cellspacing="0" border="0"><tr>`
    + `<td style="vertical-align:middle">`
    + `<img src="${logoUrl}" width="30" height="30" alt="KunClient" `
    + `style="display:inline-block;vertical-align:middle;width:30px;height:30px;border:0;`
    + `outline:none;text-decoration:none;border-radius:9px">`
    + `<span style="display:inline-block;margin-left:10px;vertical-align:middle;font-size:15px;`
    + `font-weight:700;letter-spacing:.2px;color:${b.ink}">KunClient</span>`
    + `</td>`
    + `<td align="right" style="vertical-align:middle;font-size:11.5px;color:${b.ink3};`
    + `letter-spacing:.4px;white-space:nowrap">${kicker || ''}</td>`
    + `</tr></table>`;
}

/**
 * 整封邮件的骨架。
 * @param {object} o
 * @param {string} o.kicker    品牌行右侧的小字（如「安全验证」）
 * @param {string} o.title     大标题
 * @param {string} o.subtitle  标题下的说明（可空）
 * @param {string} o.body      正文 HTML（用下面的原语拼）
 * @param {string} [o.footNote] 页脚额外一行（可空）
 */
function mailDocument({ kicker, title, subtitle, body, footNote }) {
  const b = MAIL_BRAND;
  return `<div style="margin:0;padding:30px 12px;background:${b.page}">`
    + `<div style="max-width:560px;margin:0 auto;background:${b.card};border:1px solid ${b.line};`
    + `border-radius:14px;overflow:hidden;color:${b.ink};font-family:${MAIL_FONT}">`
    // 顶部 4px 强调条：font-size/line-height 归零，否则 Outlook 会撑出一行字高
    + `<div style="height:4px;background:${b.accent};font-size:0;line-height:0">&nbsp;</div>`
    + `<div style="padding:22px 28px 0">${mailBrandRow(kicker)}</div>`
    + `<div style="padding:20px 28px 0">`
    + `<div style="font-size:21px;font-weight:700;letter-spacing:-.2px;line-height:1.35">${title}</div>`
    + (subtitle
      ? `<div style="margin-top:7px;font-size:13px;line-height:1.7;color:${b.ink2}">${subtitle}</div>`
      : '')
    + `</div>`
    + `<div style="padding:18px 28px 26px;font-size:14px;line-height:1.75;color:${b.ink}">${body}</div>`
    + `<div style="padding:14px 28px 18px;border-top:1px solid ${b.lineSoft};`
    + `font-size:11.5px;line-height:1.7;color:${b.ink3}">`
    + (footNote ? footNote + '<br>' : '')
    + `本邮件由 KunClient 系统自动发送，请勿直接回复。</div>`
    + `</div></div>`;
}

/**
 * 按钮。variant: 'solid'（实心强调色）/ 'ghost'（白底描边）。
 * 🔴 背景色必须挂在 <td> 上：Outlook 会把 <a> 的 padding 丢掉，只剩一行贴边的字。
 */
function mailButton(href, label, variant) {
  const b = MAIL_BRAND;
  const solid = variant !== 'ghost';
  const bg = solid ? b.accent : b.card;
  const fg = solid ? b.accentInk : b.accentDeep;
  const border = solid ? b.accent : b.accentLine;
  return `<table role="presentation" cellpadding="0" cellspacing="0" border="0"`
    + ` style="display:inline-block;margin:0 8px 8px 0"><tr>`
    + `<td style="background:${bg};border:1px solid ${border};border-radius:8px">`
    + `<a href="${href}" style="display:inline-block;padding:11px 24px;font-size:13.5px;`
    + `font-weight:700;color:${fg};text-decoration:none;border-radius:8px;`
    + `font-family:${MAIL_FONT};white-space:nowrap">${label}</a>`
    + `</td></tr></table>`;
}

/** 浅底面板。tone: 'accent'（默认）/ 'danger'（告警）。 */
function mailPanel(inner, tone) {
  const b = MAIL_BRAND;
  const bg = tone === 'danger' ? b.dangerWash : b.accentWash;
  const line = tone === 'danger' ? b.dangerLine : b.accentLine;
  return `<table role="presentation" width="100%" cellpadding="0" cellspacing="0" border="0"`
    + ` style="margin:0 0 16px"><tr>`
    + `<td style="background:${bg};border:1px solid ${line};border-radius:12px;padding:16px 18px">`
    + inner + `</td></tr></table>`;
}

/** 版本元信息三行（版本 / 发布 / 体积），空值自动省略。 */
function mailMetaRows(rows) {
  const b = MAIL_BRAND;
  const live = rows.filter(([, v]) => v);
  if (!live.length) return '';
  return `<table role="presentation" cellpadding="0" cellspacing="0" border="0"`
    + ` style="border-collapse:collapse;margin:0 0 18px">`
    + live.map(([k, v], i) => `<tr>`
      + `<td style="padding:3px 16px 3px 0;font-size:12.5px;color:${b.ink3};white-space:nowrap">${k}</td>`
      + `<td style="padding:3px 0;font-size:12.5px;font-weight:700;`
      + (i === 0 ? `color:${b.accentDeep};` : '') + `">${v}</td>`
      + `</tr>`).join('')
    + `</table>`;
}

/** 「本次更新」条目面板。items 已经剥过行首符号。 */
function mailBulletPanel(items) {
  const b = MAIL_BRAND;
  if (!items || !items.length) return '';
  return `<div style="font-size:12px;font-weight:700;letter-spacing:.4px;color:${b.ink3};`
    + `margin:0 0 8px">本次更新</div>`
    + `<div style="background:${b.accentWash};border:1px solid ${b.accentWashLine};`
    + `border-radius:12px;padding:14px 18px;margin:0 0 18px;font-size:13.5px;line-height:1.9">`
    + items.map(t => `<div style="margin:3px 0;padding-left:14px;text-indent:-14px">`
      + `<span style="color:${b.accentDeep}">&#9642;</span>&nbsp;${t}</div>`).join('')
    + `</div>`;
}

/** 键值信息表。rows = [[标签, 值 HTML], ...]；行间只有发丝分隔线，没有竖线。 */
function mailInfoTable(rows) {
  const b = MAIL_BRAND;
  const body = rows.map(([k, v], i) => `<tr>`
    + `<td style="padding:9px 14px 9px 0;vertical-align:top;width:84px;font-size:12.5px;`
    + `color:${b.ink3};white-space:nowrap;`
    + (i ? `border-top:1px solid ${b.lineSoft};` : '') + `">${k}</td>`
    + `<td style="padding:9px 0;vertical-align:top;font-size:13.5px;line-height:1.7;`
    + (i ? `border-top:1px solid ${b.lineSoft};` : '') + `">${v}</td>`
    + `</tr>`).join('');
  return `<table role="presentation" width="100%" cellpadding="0" cellspacing="0" border="0"`
    + ` style="border-collapse:collapse;margin:0 0 16px">${body}</table>`;
}

/** 字节数 → 人读体积。用于「更新邮件」里的安装包体积。 */
function fmtBytes(n) {
  const b = Number(n) || 0;
  if (!b) return '';
  const u = ['B', 'KB', 'MB', 'GB'];
  let v = b, i = 0;
  while (v >= 1024 && i < u.length - 1) { v /= 1024; i++; }
  return (i >= 2 ? v.toFixed(1) : String(Math.round(v))) + ' ' + u[i];
}

/**
 * 取安装包体积：把站内相对路径还原成磁盘路径后 stat。
 * 拿不到就返回空串（宁可不显示体积，也不要让邮件因为一个 stat 失败而发不出去）。
 */
function packageSizeText(url) {
  try {
    const rel = String(url || '').replace(/^https?:\/\/[^/]+/i, '');
    if (!rel) return '';
    return fmtBytes(fs.statSync(path.join(__dirname, rel.replace(/^\//, ''))).size);
  } catch { return ''; }
}

async function sendEmail(to, code, expireMinutes = 10) {
  if (!mailTransporter) { console.error('[Email] 邮件已禁用(config.json mail.enabled=false)，验证码未发送'); return false; }
  try {
    const b = MAIL_BRAND;
    const html = mailDocument({
      kicker: '安全验证',
      title: '你的验证码',
      subtitle: '请把下面这串数字填回 KunClient，以完成本次身份验证。',
      body:
        // 验证码块：虚线圈 + 34px 大字 + 字号撑开字距 —— 手机上不用放大也能抄准。
        `<table role="presentation" width="100%" cellpadding="0" cellspacing="0" border="0"`
        + ` style="margin:0 0 18px"><tr>`
        + `<td align="center" style="background:${b.accentWash};border:1px dashed ${b.accentLine};`
        + `border-radius:12px;padding:22px 12px 18px">`
        + `<div style="font-family:${MAIL_MONO};font-size:34px;font-weight:700;letter-spacing:9px;`
        + `color:${b.accentInk};line-height:1.15;text-indent:9px">${code}</div>`
        + `<div style="margin-top:10px;font-size:11.5px;letter-spacing:.6px;color:${b.accentDeep}">`
        + `${expireMinutes} 分钟内有效</div>`
        + `</td></tr></table>`
        + `<div style="font-size:13px;line-height:1.8;color:${b.ink2}">`
        + `请勿把验证码告诉任何人 —— KunClient 的客服不会向你索取它。<br>`
        + `如果这不是你本人的操作，直接忽略本邮件即可，账号不会有任何变化。</div>`,
      footNote: `这封邮件发给了 ${to}。`,
    });
    await mailTransporter.sendMail({
      from: MAIL_FROM,
      to,
      subject: `【KunClient】验证码: ${code}`,
      html
    });
    return true;
  } catch (e) {
    console.error('[Email] 发送失败:', e.message);
    return false;
  }
}

// 合规：不再记录 IP，detail 里也不要塞设备标识。
function logUsage(userId, username, feature, detail) {
  db.run("INSERT INTO usage_logs (user_id, username, feature, detail) VALUES (?, ?, ?, ?)",
    [userId, username, feature, detail || '']);
  saveDb();
}

// ====== VIP 时长：给用户叠加会员天数（负数为扣减；从 max(now, 当前到期) 起算） ======
function addVipDays(userId, days) {
  if (!Number.isFinite(days) || days === 0) return null;
  const u = rowToObj(db.exec("SELECT vip_expire_at FROM users WHERE id = ?", [userId]));
  if (!u) return null;
  const now = new Date();
  let base = now;
  if (u.vip_expire_at) {
    const cur = new Date(String(u.vip_expire_at).includes('T') || String(u.vip_expire_at).includes('Z')
      ? u.vip_expire_at : u.vip_expire_at.replace(' ', 'T') + 'Z');
    if (!isNaN(cur) && cur > now) base = cur;
  }
  const next = new Date(base.getTime() + days * 86400000);
  const iso = safeExpireIso(next);
  if (!iso) return null;
  db.run("UPDATE users SET vip_expire_at = ? WHERE id = ?", [iso, userId]);
  return iso;
}

function isVipActive(vipExpireAt) {
  if (!vipExpireAt) return false;
  const d = new Date(String(vipExpireAt).includes('T') || String(vipExpireAt).includes('Z')
    ? vipExpireAt : vipExpireAt.replace(' ', 'T') + 'Z');
  return !isNaN(d) && d > new Date();
}

// ============================================================================
// 环境检测 → 时限封禁（2026-09-30）
//
// 用户要求：客户端做环境检测，检测到问题**自动封禁账户若干天**，按信号强度分级。
//
// 🔴 三条设计底线 —— 缺任何一条，这功能都会反过来伤害正常用户：
//   ① **不主动踢人**：封禁只拦「新的登录与 API」，**绝不**去递增 session_epoch
//      强制下线。用户可能正在刷机，把客户端强制重启有可能变砖（既有红线）。
//   ② **弱信号只记录、不参与自动封禁**：调试器 / 虚拟机 / 抓包与内存工具在刷机用户
//      的机器上极其常见（很多人就是在虚拟机里跑刷机工具）。拿它们自动封号 = 成批封掉
//      正常用户。所以 LOW 级永远只进告警流水。
//   ③ **HMAC 校验失败不能直接自动封**：它也可能是「我们改了密钥而客户端没更新」，
//      表现是**全体用户同时失败**。必须配「同 build 大面积失败」的熔断（见 envBreakerTripped），
//      否则一次密钥失配就是一次全站封号事故。
//
// ⚠️ 关于「客户端上报」的可信度，说清楚别自欺：
//   客户端侧的 HMAC 密钥**必然能被逆向提取**，所以它**不是安全边界**。
//   它的真实作用是：① 挡住随手改包/随手伪造上报；② 检测「客户端与服务端版本失配」。
//   真正的攻击者能绕过它。这功能抓的是「不改客户端的那部分人」，不是逆向者。
// ============================================================================

/** 判时限封禁是否仍在生效 —— 与 isVipActive 同一套写法。
 *  🔴 禁裸比 datetime('now')：库里是 ISO 串，SQLite 的 datetime('now') 是 UTC 且无毫秒，
 *     两者混比会把「当天到期」判成「还没到期」（额度链路上踩过同一个坑）。 */
function isBanActive(bannedUntil) {
  if (!bannedUntil) return false;
  const d = new Date(String(bannedUntil).includes('T') || String(bannedUntil).includes('Z')
    ? bannedUntil : bannedUntil.replace(' ', 'T') + 'Z');
  return !isNaN(d) && d > new Date();
}

function banUntilIso(days) {
  const n = parseInt(days, 10);
  return new Date(Date.now() + (Number.isFinite(n) && n > 0 ? n : 0) * 86400000).toISOString();
}

/** 取账号当前生效的封禁；没有返回 null。人工永久封禁与自动时限封禁都从这里出。 */
function activeBanOf(user) {
  if (!user) return null;
  if (user.status === 'banned')
    return { until: null, reason: user.ban_reason || '管理员封禁', permanent: true, level: 'manual' };
  if (isBanActive(user.banned_until))
    return { until: user.banned_until, reason: user.ban_reason || '环境检测异常', permanent: false,
             level: user.ban_level || '' };
  return null;
}

// ── 分级规则：全部走软配置，误伤暴涨时要能**不开发版**就停掉自动封禁 ──────────
const ENV_SIGNAL_LEVEL = {
  // HIGH：服务端可自证（不需要客户端自述）
  challenge_fail: 'high',   // 防伪校验未通过（客户端不是原版 / 密钥不同步）
  nonce_invalid:  'high',   // nonce 不存在或已被用过（重放）
  multi_instance: 'high',   // 同账号短时内多个不同实例标识（多开共享账号）
  // MID：客户端自述，可信度中等
  bad_hmac:         'mid',  // 上报签名校验失败（伪造？还是版本失配？→ 走熔断）
  integrity_fail:   'mid',  // 客户端报告自身完整性校验失败
  debug_bypassed:   'mid',  // 客户端报告反调试被绕过
  inline_hook:      'mid',  // 客户端报告关键 API 入口被改写（内联 Hook，2026-10-01）
  injected_module:  'mid',  // 客户端报告模块从松散可疑路径加载（2026-10-01）
  // 抓包 / 中间人三项（2026-10-09 新增）。单条是 mid（要累积到阈值才封），
  // 但**两类以上同时命中 → 由 mitmComboHit 组合判据升级为 high，直接封号**。
  // 理由：要抓 HTTPS 必须「跑工具 + 改代理 + 装根证书」三件事都做，做到两件以上
  // 就不再是「机器上恰好装了这个软件」，而是**正在对我们的通信做中间人**。
  intercept_tool:    'mid',  // 抓包/中间人工具进程（Charles / Fiddler / Wireshark / mitmproxy / Reqable…）
  proxy_intercept:   'mid',  // 系统代理被指向环回或中间人常用端口
  root_ca_injected:  'mid',  // 信任库存在中间人根证书（HTTPS 抓包绕不过的一步，最硬的证据）
  // known_tool（2026-10-09 提级）：原先在 LOW，用户要求「更强」→ 提到 MID。
  // 名单里是**明确的反向工程 / 内存 / 沙箱工具**（x64dbg / IDA / dnSpy / Process Hacker…）；
  // 抓包类已拆到 intercept_tool，客户端侧做过去重，同一次采集不会两条齐发。
  // 仍是 MID（累积到阈值才封）而非 HIGH —— 单次命中也可能只是「装了但没在用」。
  // 2026-10-24 再加一道：**只有它一条时不计分**（evaluateEnvSignal 里的
  // MID_NEEDS_COMPANY 守卫）—— 刷机用户机器上开着调试器太常见，单凭这个封 3 天
  // 是最典型的误封来源。它仍然参与组合（工具在跑 + 硬信号 → 照常计数）。
  known_tool:        'mid',
  // LOW：**只记录，永不自动封禁**
  debugger:           'low',
  vm:                 'low',
  suspicious_module:  'low',
  remote_session:     'low',  // 远程桌面/终端服务会话（云主机刷机是正当用法，2026-10-01）
};
const envAutoBanEnabled = () => getSetting('env_auto_ban_enabled', '1') === '1';
const envHighDays  = () => parseInt(getSetting('env_ban_high_days', '7'), 10) || 7;
const envMidDays   = () => parseInt(getSetting('env_ban_mid_days', '3'), 10) || 3;
const envMidThreshold = () => parseInt(getSetting('env_mid_strike_threshold', '3'), 10) || 3;

/** 未登录（没有账号可归因）时按 IP 封禁的小时数。
 *  🔴 刻意比账号封禁短得多：一个出口 IP 背后可能是公司 / 学校 / 手机热点的**很多人**，
 *     账号封禁只影响一个人，IP 封禁会连带整个 NAT 出口。
 *  2026-10-24：默认 24h → **6h**。24 小时对「同办公室的同事」是灾难级误伤，
 *     而真正在抓包的人换个出口就能绕开 —— 长封期惩罚的是邻居，不是当事人。 */
const envBanIpHours = () => parseInt(getSetting('env_ban_ip_hours', '6'), 10) || 6;

/** 中间人特征三项 —— 组合判据的输入集合（与客户端 EnvInspector 上报的三条信号逐字对应）。 */
const MITM_SIGNALS = ['intercept_tool', 'proxy_intercept', 'root_ca_injected'];

/** 「配置类」MID 信号：单独出现只记录，必须配合别的 MID 才计数（见 evaluateEnvSignal）。 */
const MID_NEEDS_COMPANY = ['known_tool', 'proxy_intercept'];

/**
 * 该账号 24 小时内是否出现过**硬 MID 信号**（不含「配置类」那两条）。
 *
 * 只给 MID_NEEDS_COMPANY 的守卫用：known_tool / proxy_intercept 单独出现不计数，
 * 但「工具/代理 + 任意硬信号」要照常计数。
 * 🔴 这里刻意**不把 MID_NEEDS_COMPANY 自己算进去**：否则一个 VPN 用户连报三次
 *    proxy_intercept，第二次就能靠「昨天的自己」凑出『还有别的 MID』—— 守卫形同虚设。
 */
function hasHardMidToday(userId) {
  try {
    const r = db.exec(
      "SELECT COUNT(*) AS c FROM env_reports WHERE user_id=? AND signal IN " +
      "('bad_hmac','integrity_fail','debug_bypassed','inline_hook','injected_module'," +
      "'intercept_tool','root_ca_injected') " +
      "AND datetime(created_at) > datetime('now', '-1 day')", [userId]);
    return (r.length && r[0].values.length ? Number(r[0].values[0][0]) : 0) > 0;
  } catch { return false; }
}

/**
 * 组合判据（2026-10-09）：同账号 24 小时内命中的中间人特征**种类** ≥2 → 升级为 high 直接封。
 *
 * 为什么必须做组合、而不是把单条权重调高：抓包有很长的前置链条，任意**单条**都可能有
 * 正当解释（装 Wireshark 看自己家网络、公司代理恰好用 8080、试用过 Charles 忘了卸）。
 * 但「工具在跑 + 代理被改 + 根证书被信任」同时成立时，解释只剩一个。
 * 这就是「不是只针对一点」的落点 —— 判据从单点变成组合。
 *
 * ⚠️ 计数**含本次**（调用点在此之前已把本次落库），所以阈值写成 >= 2。
 */
function mitmComboHit(userId) {
  try {
    const r = db.exec(
      "SELECT COUNT(DISTINCT signal) AS c FROM env_reports WHERE user_id=? " +
      "AND signal IN ('intercept_tool','proxy_intercept','root_ca_injected') " +
      "AND datetime(created_at) > datetime('now', '-1 day')", [userId]);
    const c = r.length && r[0].values.length ? Number(r[0].values[0][0]) : 0;
    return c >= 2;
  } catch { return false; }
}

/** 按 IP 封禁：写 banned_ips 表（真正的拦截在文件顶部那个 app.use('/api', ...)）。 */
function banIp(ip, hours, reason) {
  const clean = String(ip || '').trim();
  if (!clean || !Number.isFinite(hours) || hours <= 0) return null;
  const expireAt = new Date(Date.now() + hours * 3600000).toISOString();
  try {
    db.run("INSERT OR REPLACE INTO banned_ips (ip, reason, created_by, created_at, expire_at) VALUES (?,?,?,?,?)",
      [clean, String(reason).slice(0, 200), null, new Date().toISOString(), expireAt]);
    saveDb();
    console.log(`[EnvBan] IP ${clean} 自动封禁 ${hours} 小时：${reason}`);
    return expireAt;
  } catch (e) { console.error('[EnvBan] 封 IP 失败:', e.message); return null; }
}

/**
 * HTML 转义（告警邮件用）。detail 是**客户端可控输入**，直接拼进 HTML 等于给收件人
 * 送一个注入点；拼进 subject 还会造成邮件头注入（换行符）。
 * ⚠️ 引号必须写成 \x22 / \x27 —— tools/check-server.js 的简易 lexer 不认正则字面量，
 *    源码里出现裸引号会把它的字符串配对搅乱（已有先例：SQL 表名被误报为未定义函数）。
 */
function htmlEscape(s) {
  return String(s === undefined || s === null ? '' : s)
    .replace(/[\r\n\x00-\x1f\x7f]/g, ' ')
    .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
    .replace(/\x22/g, '&quot;').replace(/\x27/g, '&#39;');
}

/** 告警收件人：优先专用设置 dev_alert_email，未配置时回落管理员邮箱。 */
function devAlertEmail() {
  return String(getSetting('dev_alert_email', '') || '').trim() || getAdminEmail();
}

/**
 * 给开发者发一封环境告警。**任何失败都只记录、不抛** ——
 * 它挂在上报链路末尾，绝不能因为发信失败把上报结果也搞崩。
 */
async function sendDevAlert(subject, lines) {
  if (!mailTransporter) { console.log('[DevAlert] 邮件已禁用，跳过告警'); return false; }
  const to = devAlertEmail();
  if (!to) { console.log('[DevAlert] 未配置告警邮箱（dev_alert_email / admin_email 均为空），跳过'); return false; }
  try {
    const rows = lines.map((l, i) =>
      '<tr><td style="padding:8px 0;font-size:13px;line-height:1.75;'
      + (i ? 'border-top:1px solid #eff3f9;' : '') + '">' + htmlEscape(l) + '</td></tr>').join('');
    const html = mailDocument({
      kicker: '环境告警',
      title: '未登录客户端触发了防护',
      subtitle: '该请求已被拦截并按 IP 处置，以下是上报明细。',
      body:
        mailPanel('<div style="font-size:12.5px;line-height:1.75;color:#c0392b">'
          + '这是<b>开发侧</b>的告警邮件，普通用户看不到。若要放行，到控制台「IP 封禁」里删掉对应条目即可。'
          + '</div>', 'danger')
        + '<table role="presentation" width="100%" cellpadding="0" cellspacing="0" border="0">'
        + rows + '</table>',
    });
    await sendMailWithRetry(mailTransporter, { from: MAIL_FROM, to, subject, html });
    return true;
  } catch (e) {
    console.error('[DevAlert] 发送失败:', e.message);
    return false;
  }
}

/**
 * 受「账号状态邮件」开关控制的类型（封禁 / 申诉）。
 * 密码 / 邮箱 / 2FA 三类**不在里面**：它们是安全警报，必须始终送达。
 */
const GATED_MAIL_KINDS = ['ban', 'unban', 'appeal_ack', 'appeal_ok', 'appeal_reject'];

/**
 * 账号状态邮件（2026-10-24）：封禁 / 解封 / 申诉受理 / 申诉结果，共用一套版式。
 *
 * 为什么要发：账号处置必须**送达理由和期限**。否则用户看到的只是「突然登不上」，
 * 然后来问、来骂、直接卸载 —— 而真正被误封的人根本没有自证的机会。
 * 申诉这条路尤其要有回声：提交完没有回执，用户不知道到底送到没有；
 * 处理完没有结果，用户只能一遍遍重试（还会来骂我们不处理）。
 *
 * kind：
 *   ban           账号被临时限制（自动封禁 / 手动封禁）
 *   unban         限制已解除（管理员手动解封）
 *   appeal_ack    申诉已收到（回执）
 *   appeal_ok     申诉通过（同时已解封）
 *   appeal_reject 申诉驳回（维持原判）
 *
 * 三条纪律：
 *   · 只对**账号**发（IP 封禁没有收件人，走开发者告警）；
 *   · 没有邮箱 / 邮件服务不可用 / 开关关掉 → 静默跳过，**绝不影响业务落库**；
 *   · 调用方不 await：SMTP 可能几秒，接口不能被它拖住（失败只记日志）。
 */
function buildAccountMail(kind, info) {
  const i = info || {};
  const until = i.until ? String(i.until).slice(0, 10) : '';
  const reason = friendlyBanReason(i.reason || '') || '触发环境安全策略';
  const reply = String(i.reply || '').trim();
  const whenText = until ? until + '（到期自动恢复，无需操作）' : '待人工复核';
  const today = new Date().toISOString().slice(0, 10);

  if (kind === 'unban') {
    return {
      subject: '【KunClient】账号限制已解除',
      title: '账号限制已解除',
      subtitle: '你账号上的临时限制已经解除，现在可以正常登录了。',
      tone: '',
      panel:
        '<div style="font-size:13px;line-height:1.95;color:#1f6f43">'
        + '<b>状态：</b>已恢复正常<br>'
        + '<b>解除时间：</b>' + htmlEscape(today)
        + '</div>',
      body:
        '<div style="font-size:13px;line-height:1.95;color:#4a5568">'
        + '如果这次限制是误判，给你添麻烦了，抱歉。<br>'
        + '为了不再触发，启动客户端前建议先关掉抓包 / 逆向工具；使用 VPN 或系统代理本身不会被判定。'
        + '</div>',
      text: [
        '你 KunClient 账号上的临时限制已经解除，现在可以正常登录了。',
        '',
        '解除时间：' + today,
        '',
        '如果这次限制是误判，给你添麻烦了，抱歉。',
        '为了不再触发，启动客户端前建议先关掉抓包 / 逆向工具；使用 VPN 或系统代理本身不会被判定。',
      ],
    };
  }

  if (kind === 'appeal_ack') {
    return {
      subject: '【KunClient】申诉已收到',
      title: '申诉已收到',
      subtitle: '我们已收到你的申诉，会尽快人工复核。',
      tone: '',
      panel:
        '<div style="font-size:13px;line-height:1.95;color:#10304f">'
        + '<b>提交时间：</b>' + htmlEscape(today) + '<br>'
        + '<b>当前状态：</b>待处理'
        + '</div>',
      body:
        '<div style="font-size:13px;line-height:1.95;color:#4a5568">'
        + '复核是人工做的，通常需要 1~2 个工作日。<b>处理结果会再发一封邮件到本邮箱</b>，'
        + '你也可以随时在客户端登录页查看当前状态。<br>'
        + '同一条封禁请不要重复提交申诉 —— 重复提交不会加快处理。'
        + '</div>',
      text: [
        '我们已收到你的 KunClient 账号申诉，会尽快人工复核。',
        '',
        '提交时间：' + today,
        '当前状态：待处理',
        '',
        '复核是人工做的，通常需要 1~2 个工作日。处理结果会再发一封邮件到本邮箱，',
        '你也可以随时在客户端登录页查看当前状态。',
        '同一条封禁请不要重复提交申诉 —— 重复提交不会加快处理。',
      ],
    };
  }

  if (kind === 'appeal_ok') {
    return {
      subject: '【KunClient】申诉已通过',
      title: '申诉已通过',
      subtitle: '经人工复核，我们已解除该账号的限制。',
      tone: '',
      panel:
        '<div style="font-size:13px;line-height:1.95;color:#1f6f43">'
        + '<b>处理结果：</b>申诉通过，限制已解除<br>'
        + '<b>处理时间：</b>' + htmlEscape(today)
        + (reply ? '<br><b>处理回复：</b>' + htmlEscape(reply) : '')
        + '</div>',
      body:
        '<div style="font-size:13px;line-height:1.95;color:#4a5568">'
        + '现在可以正常登录了。给你带来的不便，抱歉。'
        + '</div>',
      text: [
        '你的申诉已通过，账号限制已解除，现在可以正常登录了。',
        '',
        '处理时间：' + today,
        ...(reply ? ['处理回复：' + reply] : []),
        '',
        '给你带来的不便，抱歉。',
      ],
    };
  }

  if (kind === 'appeal_reject') {
    return {
      subject: '【KunClient】申诉未通过',
      title: '申诉未通过',
      subtitle: '经人工复核，本次处置维持原判。',
      tone: 'danger',
      panel:
        '<div style="font-size:13px;line-height:1.95;color:#8a3b32">'
        + '<b>处理结果：</b>申诉未通过，限制继续生效<br>'
        + (until ? '<b>到期：</b>' + htmlEscape(whenText) + '<br>' : '')
        + '<b>原因：</b>' + htmlEscape(reason)
        + (reply ? '<br><b>处理回复：</b>' + htmlEscape(reply) : '')
        + '</div>',
      body:
        '<div style="font-size:13px;line-height:1.95;color:#4a5568">'
        + '如果你有新的证据（例如能说明当时确实在抓包 / 调试的截图），可以在 7 天后再次申诉。<br>'
        + '到期后限制会自动解除，不需要任何操作。'
        + '</div>',
      text: [
        '你的申诉经人工复核后未通过，账号限制继续生效。',
        '',
        ...(until ? ['到期：' + whenText] : []),
        '原因：' + reason,
        ...(reply ? ['处理回复：' + reply] : []),
        '',
        '如果你有新的证据，可以在 7 天后再次申诉。',
        '到期后限制会自动解除，不需要任何操作。',
      ],
    };
  }

  if (kind === 'password_changed') {
    return {
      subject: '【KunClient】密码已重置',
      title: '密码已重置',
      subtitle: '你的账号密码刚刚通过邮箱验证被重置了。',
      tone: '',
      panel:
        '<div style="font-size:13px;line-height:1.95;color:#10304f">'
        + '<b>操作时间：</b>' + htmlEscape(today) + '<br>'
        + '<b>方式：</b>邮箱验证码自助重置'
        + '</div>',
      body:
        '<div style="font-size:13px;line-height:1.95;color:#4a5568">'
        + '这是你本人的操作的话，忽略本邮件即可。所有已登录的设备都已退出，请用新密码重新登录。<br>'
        + '<b>如果这不是你做的</b>，说明有人拿到了你的邮箱验证码 —— 请立即再次重置密码，'
        + '并检查邮箱是否被盗用。'
        + '</div>',
      text: [
        '你的 KunClient 账号密码刚刚通过邮箱验证被重置了。',
        '',
        '操作时间：' + today,
        '方式：邮箱验证码自助重置',
        '',
        '这是你本人的操作的话，忽略本邮件即可。所有已登录的设备都已退出，请用新密码重新登录。',
        '如果这不是你做的，说明有人拿到了你的邮箱验证码 —— 请立即再次重置密码，并检查邮箱是否被盗用。',
      ],
    };
  }

  if (kind === 'password_reset_admin') {
    return {
      subject: '【KunClient】密码已被管理员重置',
      title: '密码已被管理员重置',
      subtitle: '管理员重置了你账号的密码，当前登录状态已全部失效。',
      tone: '',
      panel:
        '<div style="font-size:13px;line-height:1.95;color:#10304f">'
        + '<b>操作时间：</b>' + htmlEscape(today) + '<br>'
        + '<b>方式：</b>管理员后台重置'
        + '</div>',
      body:
        '<div style="font-size:13px;line-height:1.95;color:#4a5568">'
        + '请向管理员索取新密码后重新登录，登录后建议立即在「设置 → 账号安全」里改成自己的密码。<br>'
        + '如果你没有申请过重置，请在客户端「设置 → 问题反馈」里联系我们。'
        + '</div>',
      text: [
        '管理员重置了你 KunClient 账号的密码，当前登录状态已全部失效。',
        '',
        '操作时间：' + today,
        '方式：管理员后台重置',
        '',
        '请向管理员索取新密码后重新登录，登录后建议立即在「设置 → 账号安全」里改成自己的密码。',
        '如果你没有申请过重置，请在客户端「设置 → 问题反馈」里联系我们。',
      ],
    };
  }

  if (kind === 'email_changed') {
    // 这封发给**旧邮箱** —— 换绑时最该收到警报的就是它：
    // 账号被偷的第一步永远是「换掉找回邮箱」，旧邮箱是唯一还能拉响警报的渠道。
    return {
      subject: '【KunClient】账号绑定邮箱已变更',
      title: '账号绑定邮箱已变更',
      subtitle: '你的账号刚刚把绑定邮箱换成了另一个地址。',
      tone: 'danger',
      panel:
        '<div style="font-size:13px;line-height:1.95;color:#8a3b32">'
        + '<b>原邮箱：</b>' + htmlEscape(i.oldEmail || '') + '<br>'
        + '<b>新邮箱：</b>' + htmlEscape(i.newEmail || '') + '<br>'
        + '<b>变更时间：</b>' + htmlEscape(today)
        + '</div>',
      body:
        '<div style="font-size:13px;line-height:1.95;color:#4a5568">'
        + '<b>如果这不是你本人操作</b>，说明有人已经拿到了你的账号 —— 请立即用本邮箱走「忘记密码」'
        + '重置密码，并在客户端「设置 → 问题反馈」里联系我们冻结账号。<br>'
        + '注意：换绑后，本邮箱将不再能用于该账号的登录验证与密码找回。'
        + '</div>',
      text: [
        '你的 KunClient 账号刚刚把绑定邮箱换成了另一个地址。',
        '',
        '原邮箱：' + (i.oldEmail || ''),
        '新邮箱：' + (i.newEmail || ''),
        '变更时间：' + today,
        '',
        '如果这不是你本人操作，说明有人已经拿到了你的账号 —— 请立即用本邮箱走「忘记密码」重置密码，',
        '并在客户端「设置 → 问题反馈」里联系我们冻结账号。',
        '注意：换绑后，本邮箱将不再能用于该账号的登录验证与密码找回。',
      ],
    };
  }

  if (kind === 'email_bound') {
    return {
      subject: '【KunClient】邮箱绑定成功',
      title: '邮箱绑定成功',
      subtitle: '该邮箱已绑定到你的 KunClient 账号。',
      tone: '',
      panel:
        '<div style="font-size:13px;line-height:1.95;color:#10304f">'
        + '<b>绑定邮箱：</b>' + htmlEscape(i.email || '') + '<br>'
        + '<b>绑定时间：</b>' + htmlEscape(today)
        + '</div>',
      body:
        '<div style="font-size:13px;line-height:1.95;color:#4a5568">'
        + '之后登录验证、密码找回、账号通知都会发到这个邮箱，请确保它能正常收信。<br>'
        + '<b>如果这不是你做的</b>，请立即在客户端里解绑并修改密码。'
        + '</div>',
      text: [
        '该邮箱已绑定到你的 KunClient 账号。',
        '',
        '绑定邮箱：' + (i.email || ''),
        '绑定时间：' + today,
        '',
        '之后登录验证、密码找回、账号通知都会发到这个邮箱，请确保它能正常收信。',
        '如果这不是你做的，请立即在客户端里解绑并修改密码。',
      ],
    };
  }

  if (kind === 'twofa') {
    const on = i.enabled === true || i.enabled === 1 || i.enabled === '1';
    return {
      subject: `【KunClient】双重验证已${on ? '开启' : '关闭'}`,
      title: `双重验证已${on ? '开启' : '关闭'}`,
      subtitle: on ? '之后每次登录都需要邮箱验证码。' : '之后登录只需要密码，不再需要邮箱验证码。',
      tone: on ? '' : 'danger',
      panel:
        `<div style="font-size:13px;line-height:1.95;color:${on ? '#1f6f43' : '#8a3b32'}">`
        + `<b>当前状态：</b>${on ? '已开启' : '已关闭'}<br>`
        + '<b>操作时间：</b>' + htmlEscape(today)
        + '</div>',
      body:
        '<div style="font-size:13px;line-height:1.95;color:#4a5568">'
        + (on
          ? '开启后，即使密码泄露，别人也无法在没拿到你邮箱验证码的情况下登录。'
          : '<b>关闭双重验证会明显降低账号安全性</b>：只要密码泄露，别人就能直接登录。')
        + '<br><b>如果这不是你做的</b>，请立即修改密码并重新开启双重验证。'
        + '</div>',
      text: [
        `你的 KunClient 账号双重验证已${on ? '开启' : '关闭'}。`,
        '',
        `当前状态：${on ? '已开启' : '已关闭'}`,
        '操作时间：' + today,
        '',
        on ? '开启后，即使密码泄露，别人也无法在没拿到你邮箱验证码的情况下登录。'
           : '关闭双重验证会明显降低账号安全性：只要密码泄露，别人就能直接登录。',
        '如果这不是你做的，请立即修改密码并重新开启双重验证。',
      ],
    };
  }

  // 默认：ban
  return {
    subject: `【KunClient】账号已被限制${until ? '至 ' + until : ''}`,
    title: '账号已被临时限制',
    subtitle: '我们的环境安全检测在你的客户端上发现了异常，因此对该账号做了临时限制。',
    tone: 'danger',
    panel:
      '<div style="font-size:13px;line-height:1.95;color:#8a3b32">'
      + '<b>原因：</b>' + htmlEscape(reason) + '<br>'
      + '<b>到期：</b>' + htmlEscape(whenText)
      + '</div>',
    body:
      '<div style="font-size:13px;line-height:1.95;color:#4a5568">'
      + '限制期间无法登录客户端，<b>不会影响你的设备与数据</b>。<br>'
      + '如果你认为这是误判（例如电脑上开着抓包 / 逆向工具、使用了 VPN 或系统代理、'
      + '在虚拟机里操作），可以在客户端登录页点「去申诉」提交说明，我们会人工复核。'
      + '</div>',
    text: [
      '你的 KunClient 账号已被临时限制。',
      '',
      '原因：' + reason,
      '到期：' + whenText,
      '',
      '限制期间无法登录客户端，不会影响你的设备与数据。',
      '如果你认为这是误判（例如电脑上开着抓包 / 逆向工具、使用了 VPN 或系统代理、在虚拟机里操作），',
      '可以在客户端登录页点「去申诉」提交说明，我们会人工复核。',
    ],
  };
}

async function sendAccountMail(kind, user, info) {
  if (!mailTransporter) return false;
  const opt = info || {};
  // force = 模板自测：跳过开关，也不看收件人是不是真用户（只发给开发者自己）
  // 开关只关「封禁 / 申诉」这一类（量可能很大，误伤时用户会想关掉）；
  // 密码 / 邮箱 / 2FA 这些**安全类邮件永远发** —— 它们是账号被盗时唯一的警报渠道，
  // 绝不能因为「封禁邮件太吵」被一起关掉。
  if (!opt.force && GATED_MAIL_KINDS.includes(kind) && getSetting('env_ban_mail_enabled', '1') !== '1') return false;
  let to = String((user && user.email) || '').trim();
  if (!to && user && user.id) {
    try {
      const row = rowToObj(db.exec("SELECT email FROM users WHERE id=?", [user.id]));
      to = String((row && row.email) || '').trim();
    } catch (e) { /* 查不到就当没有邮箱 */ }
  }
  if (!to) {
    console.log(`[Mail:${kind}] 账号 #${(user && user.id) || '-'} 没填邮箱，跳过通知`);
    return false;
  }

  const m = buildAccountMail(kind, opt);
  try {
    const html = mailDocument({
      kicker: '账号安全',
      title: m.title,
      subtitle: m.subtitle,
      body: mailPanel(m.panel, m.tone) + m.body,
      footNote: '此邮件由系统自动发送，请勿直接回复。',
    });
    // 必须带纯文本正文：只有 HTML 的邮件是垃圾邮件过滤器最经典的一条减分项
    // （2026-10-24 实测：QQ 邮箱收不到，SMTP 侧明明是 250 queued）。
    // 同时补 Reply-To —— 一个能被回复的地址会显著降低被判垃圾的概率，
    // 而且用户真去回信时不会撞上 no-reply 的退信。
    const text = m.text.concat(['', '此邮件由系统自动发送，请勿直接回复。']).join('\n');
    const replyTo = String(getSetting('admin_email', '') || devAlertEmail() || '').trim();
    const sent = await sendMailWithRetry(mailTransporter, {
      from: MAIL_FROM,
      to,
      subject: m.subject,
      text,
      html,
      ...(replyTo ? { replyTo } : {}),
    });
    // 把 SMTP 的裁决记下来：accepted/rejected 是「阿里云收没收到」，不是「用户收没收到」。
    // 用户说没收到时，先看这一行 —— rejected 非空 = 我们这边的问题；全 accepted = 落在对方的
    // 垃圾箱 / 被对方网关丢弃，得去阿里云控制台看投递与退信记录。
    const acc = (sent && sent.accepted) || [];
    const rej = (sent && sent.rejected) || [];
    console.log(`[Mail:${kind}] ${(user && user.username) || '-'} <${to}> msgId=${(sent && sent.messageId) || '-'} ` +
      `accepted=${acc.length} rejected=${rej.length}${rej.length ? ' → ' + rej.join(',') : ''}`);
    // 回执带上 messageId / accepted：控制台点「发送测试邮件」时能直接看到阿里云的受理号，
    // 用户说没收到时也有据可查（这个号可以去阿里云控制台的投递日志里搜）。
    return { ok: true, messageId: (sent && sent.messageId) || '', accepted: acc.length, rejected: rej.length };
  } catch (e) {
    console.error(`[Mail:${kind}] 发送失败:`, e.message);
    return false;
  }
}

/** 封禁通知（保留这个名字：调用点多，且语义清楚）。 */
function sendBanMail(user, info) {
  return sendAccountMail('ban', user, info);
}

function recordEnvReport(userId, username, level, signal, detail, build, ip) {
  db.run("INSERT INTO env_reports (user_id, username, level, signal, detail, build, ip) VALUES (?,?,?,?,?,?,?)",
    [userId, String(username || ''), level, signal,
     String(detail || '').slice(0, 500), String(build || '').slice(0, 40), String(ip || '')]);
}

/**
 * 🔴 熔断：同一 build 在 1 小时内出现 N 个**不同账号**的同类别信号
 *  → 这几乎不可能是「N 个人同时作弊」，而是**我们自己的密钥/版本/盐值失配**。
 *    该 build 的这类报告一律不再触发自动封禁，只进告警流水。
 *    没有这条保险丝，一次失配 = 一次「全体用户被自动封号」事故。
 *  E9（2026-10-07）：原来只覆盖 bad_hmac；challenge_fail / nonce_invalid 同样是
 *  「服务端可自证」的 HIGH 信号，同样会被版本/盐值失配打成整批误封 7 天 —— 一并纳入。 */
const BREAKER_SIGNALS = ['bad_hmac', 'challenge_fail', 'nonce_invalid'];
function envBreakerTripped(build) {
  const b = String(build || '');
  if (!b) return false;
  const need = parseInt(getSetting('env_breaker_accounts', '5'), 10) || 5;
  // 2026-10-02 修复：原先把 JS 的 ISO 串（含 'T'）当参数去和 datetime('now') 格式的
  // created_at（'YYYY-MM-DD HH:MM:SS'，空格 < 'T'）做字符串比较，同一 UTC 日的行全部
  // 落在窗口外 → 熔断永远数出 0，形同虚设。改用 SQLite 自己的 datetime() 归一化两侧。
  const r = db.exec(
    "SELECT COUNT(DISTINCT user_id) AS c FROM env_reports WHERE signal IN ('bad_hmac','challenge_fail','nonce_invalid') AND build=? AND datetime(created_at) > datetime('now', '-1 hour')",
    [b]);
  const c = r.length && r[0].values.length ? Number(r[0].values[0][0]) : 0;
  if (c >= need) {
    console.warn(`[EnvBreaker] build ${b} 近 1 小时内有 ${c} 个账号触发熔断信号（bad_hmac/challenge_fail/nonce_invalid）—— 疑似密钥/版本失配，自动封禁已熔断`);
    return true;
  }
  return false;
}

/** 该账号近 N 天被自动封过几次（不含本次）。
 *  数据源是 env_bans 流水表 —— 不能读 users.ban_reason（解封会清空）。 */
function autoBanCount(userId, days = 90) {
  if (!userId) return 0;
  try {
    const r = db.exec(
      "SELECT COUNT(*) AS c FROM env_bans WHERE user_id=? AND datetime(created_at) > datetime('now', ?)",
      [userId, `-${days} days`]);
    return r.length && r[0].values.length ? Number(r[0].values[0][0]) : 0;
  } catch { return 0; }
}

/**
 * 落一次时限封禁。**重复违规会升级**（2026-10-24 新增）：
 *   第 1 次 = 基础天数；第 2~3 次 = ×2；第 4 次起 = ×3；封顶 30 天。
 *
 * 为什么必须升级：原来的策略是「3 天到了自动放出来，然后同样的行为再封 3 天」——
 * 对反复用同一套环境的人来说成本恒定，等于没有成本。升级只在**已经被封过**的账号上
 * 生效，不影响任何第一次触发的人，所以不会扩大误伤面。
 */
function applyTimedBan(user, days, reason, level) {
  const base = Math.max(1, parseInt(days, 10) || 1);
  const prior = autoBanCount(user.id);
  const mult = prior >= 3 ? 3 : (prior >= 1 ? 2 : 1);
  const finalDays = Math.min(base * mult, 30);
  const until = banUntilIso(finalDays);
  db.run("UPDATE users SET banned_until=?, ban_reason=?, ban_level=? WHERE id=?",
    [until, String(reason).slice(0, 200), String(level || ''), user.id]);
  try {
    db.run("INSERT INTO env_bans (user_id, level, days, reason, created_at) VALUES (?,?,?,?,?)",
      [user.id, String(level || ''), finalDays, String(reason).slice(0, 200), new Date().toISOString()]);
  } catch (e) { console.error('[EnvBan] 写封禁流水失败:', e.message); }
  saveDb();
  const extra = mult > 1 ? `，90 天内第 ${prior + 1} 次违规 ×${mult}` : '';
  console.log(`[EnvBan] 账号 #${user.id} ${user.username} 自动封禁 ${finalDays} 天（${level}）${extra}：${reason}`);
  // 通知用户（不 await：SMTP 可能几秒，不能拖住上报响应；失败已在 sendBanMail 内吞掉）
  sendBanMail(user, { reason, until, level, days: finalDays }).catch(() => {});
  // 返回**实际**天数（含升级倍数）：调用方会把它写进 API 回执，
  // 回一个「3 天」而实际封了 6 天会让客服/申诉处理看错。
  return { until, days: finalDays };
}

/** 分级处置。**永远先落流水再谈处置** —— 没有流水就没法查证、也没法处理申诉。 */
function evaluateEnvSignal(user, signal, detail, build, ip) {
  const level = ENV_SIGNAL_LEVEL[signal] || 'low';
  recordEnvReport(user.id, user.username, level, signal, detail, build, ip);

  // 工作人员豁免：自动封禁把自己锁在门外是最尴尬的事故
  if (user.role === 'developer' || user.role === 'admin')
    return { level, banned: false, note: '工作人员账号豁免' };
  if (!envAutoBanEnabled())
    return { level, banned: false, note: '自动封禁总开关已关闭' };
  if (level === 'low')
    return { level, banned: false, note: '弱信号只记录，不参与自动封禁' };
  // E9：熔断覆盖到 challenge_fail / nonce_invalid —— 盐值/版本失配时这两类会整批触发，
  // 不熔断就是「成批误封 7 天」。
  if (BREAKER_SIGNALS.includes(signal) && envBreakerTripped(build))
    return { level, banned: false, note: '同类信号批量出现，疑似密钥/版本失配，已熔断' };

  if (level === 'high') {
    // 多开是「账号共享」，与「客户端被改」性质不同：天数按 MID 档（默认 3 天），
    // 但仍**立即封**、不等累积 —— 服务端能自证，没必要攒。
    const days = signal === 'multi_instance' ? envMidDays() : envHighDays();
    const r = applyTimedBan(user, days, `${signal}: ${detail || ''}`, 'high');
    return { level, banned: true, until: r.until, days: r.days };
  }

  // ── 组合判据（2026-10-09）：中间人特征命中 ≥2 种 → 直接按 high 处置 ──────
  // 刻意放在 MID 累积**之前**：组合已经成立时不该再等「攒够 N 次」，
  // 那等于给成套抓包环境留了一段免罚窗口。
  if (MITM_SIGNALS.includes(signal) && mitmComboHit(user.id)) {
    const reason = `${signal} 组合命中（24h 内 ≥2 类中间人特征）: ${detail || ''}`;
    const r = applyTimedBan(user, envHighDays(), reason, 'high');
    return { level: 'high', banned: true, until: r.until, days: r.days, combo: true, source: signal };
  }

  // 「配置类」信号：单独出现不封号，必须配合别的 MID 信号（2026-10-24）。
  //   · known_tool      —— 刷机用户的机器上开着 x64dbg / IDA 太常见；
  //   · proxy_intercept —— 系统代理指向 127.0.0.1 是**所有 VPN 客户端的默认行为**
  //     （线上实测：7 天里 4 个用户因此记 mid，其中 1 个被自动封 3 天，全是 Clash /
  //     v2rayN 这类科学上网工具，不是中间人）。客户端已改成「必须同时有抓包工具进程」
  //     才报这条，服务端再兜一层：单独出现时只记流水。
  // 它们仍然参与组合 —— 24h 内还有任意一条别的 MID，就照常计数。
  if (MID_NEEDS_COMPANY.includes(signal) && !hasHardMidToday(user.id))
    return { level, banned: false, note: '该信号单独出现时只记录，不单独封禁' };

  // MID：按自然日累积，达到阈值才封 —— 单次误报不会直接封号
  const today = new Date().toISOString().slice(0, 10);
  let strike = Number(user.mid_strike || 0);
  strike = user.mid_strike_day === today ? strike + 1 : 1;
  db.run("UPDATE users SET mid_strike=?, mid_strike_day=? WHERE id=?", [strike, today, user.id]);
  saveDb();
  if (strike < envMidThreshold())
    return { level, banned: false, strike, need: envMidThreshold(), note: 'MID 信号累积中，未达阈值' };
  const r = applyTimedBan(user, envMidDays(), `${signal} ×${strike}: ${detail || ''}`, 'mid');
  return { level, banned: true, until: r.until, days: r.days, strike };
}

/**
 * 匿名（未登录）上报的处置 —— **按 IP**。
 *
 * 与 evaluateEnvSignal 的差异，逐条说明白：
 *   · 没有 user：mid_strike 累积字段长在 users 表上，匿名场景用不了，所以这里
 *     **不累积、直接处置**。2026-10-24 起「直接封」收窄成三类硬信号 + 中间人组合，
 *     单条抓包 / 逆向工具只记流水（详见下方 evaluateAnonEnvSignal 里的注释）。
 *   · 只封 IP、不碰账号：账号本来也未知。
 *   · 每次处置都发一封邮件给开发者：这是「未登录」场景下唯一的可见性来源 ——
 *     没有账号面板可看，不发信就等于悄悄封了一个 IP 却没人知道。
 *     2026-10-24 补：该 IP 已在封禁期内时**不重复发信**（客户端 10 分钟复检一次，
 *     否则就是每 10 分钟一封）。
 *   · **熔断照旧生效**：同一 build 短时间内大面积 bad_hmac / challenge_fail 说明是
 *     我们自己的密钥/版本失配，此时绝不去封 IP —— 否则一次发版就会把整批用户的
 *     出口 IP 一起封掉（NAT 出口一封一片）。
 */
/** 未登录时**值得封 IP** 的信号（2026-10-24 收紧）。
 *  只收「服务端能自证」和「客户端被改」两类；抓包三项走组合判据，不单条封。
 *  理由见 evaluateAnonEnvSignal 里的注释 —— 一句话：IP 封禁会连带 NAT 后面的所有人。 */
const ANON_BAN_SIGNALS = new Set([
  'challenge_fail', 'nonce_invalid',                 // 服务端自证：不是原版客户端 / 重放
  'integrity_fail', 'inline_hook', 'injected_module', // 客户端被改 / 被注入
]);

/** 同一 IP 24 小时内命中的中间人特征**种类** ≥2 → 组合成立（匿名版，按 IP 归集）。 */
function mitmComboHitByIp(ip) {
  const clean = String(ip || '').trim();
  if (!clean) return false;
  try {
    const r = db.exec(
      "SELECT COUNT(DISTINCT signal) AS c FROM env_reports WHERE ip=? AND user_id=0 " +
      "AND signal IN ('intercept_tool','proxy_intercept','root_ca_injected') " +
      "AND datetime(created_at) > datetime('now', '-1 day')", [clean]);
    const c = r.length && r[0].values.length ? Number(r[0].values[0][0]) : 0;
    return c >= 2;
  } catch { return false; }
}

function evaluateAnonEnvSignal(ip, signal, detail, build) {
  const level = ENV_SIGNAL_LEVEL[signal] || 'low';
  recordEnvReport(0, '', level, signal, detail, build, ip);   // user_id=0 = 匿名
  if (level === 'low') return { level, banned: false, note: '弱信号只记录，不参与自动封禁' };
  if (!envAutoBanEnabled()) return { level, banned: false, note: '自动封禁总开关已关闭' };
  if (BREAKER_SIGNALS.includes(signal) && envBreakerTripped(build))
    return { level, banned: false, note: '同类信号批量出现，疑似密钥/版本失配，已熔断' };

  // ── 2026-10-24 收紧：未登录按 IP 封禁只在**三种情况**下发生 ──────────────
  //   ① 服务端自证（challenge_fail / nonce_invalid）
  //   ② 客户端被改（integrity_fail / inline_hook / injected_module）
  //   ③ 中间人组合（24h 内 ≥2 类）
  // 其余一律只记流水。原因很具体：IP 封禁拦的是**整个出口**（公司 / 学校 / 热点），
  // 而单条 intercept_tool / proxy_intercept / root_ca_injected 都有正当解释
  // （试用过的抓包软件没卸、公司代理恰好 8080、企业根证书），known_tool 更是
  // 刷机用户的常态。拿这些封 IP，罚的是同办公室的同事，不是真正在抓包的人。
  const combo = MITM_SIGNALS.includes(signal) && mitmComboHitByIp(ip);
  if (!ANON_BAN_SIGNALS.has(signal) && !combo) {
    return {
      level, banned: false, ip: true,
      note: MITM_SIGNALS.includes(signal) ? '未登录：单条中间人特征只记录，组合成立才封 IP'
                                          : '未登录：该信号只记录，不封 IP',
    };
  }

  const hours = envBanIpHours();
  // 已经被封着的出口不再续封、也不再发第二封告警邮件 ——
  // 客户端 10 分钟复检一次，不挡住就是每 10 分钟一封邮件。
  const already = bannedIpRow(ip);
  const until = banIp(ip, hours, `匿名环境检测 ${level}: ${signal} ${detail || ''}${combo ? '（中间人组合）' : ''}`);
  if (until && !already) {
    // 刻意不 await：SMTP 可能几秒，不能拖住上报响应；失败也已在 sendDevAlert 内吞掉。
    sendDevAlert('【KunClient】未登录客户端触发环境封禁', [
      '来源 IP：' + ip,
      '命中信号：' + signal + '（' + level + '）' + (combo ? ' · 中间人组合' : ''),
      '详情：' + (detail || '-'),
      '客户端版本：' + (build || '-'),
      '处置：已封禁该 IP ' + hours + ' 小时',
      '时间：' + new Date().toISOString(),
      '解封方式：控制台 → IP 封禁 → 删除该条目',
    ]);
  }
  return { level, banned: !!until, until, hours, ip: true, combo };
}

/** 上报签名密钥：与客户端 EnvReportService 同源派生。
 *  2027 轮换：改用**乱序片段 + 索引表**（与客户端 SeedStore / SeedOrder 逐字对应）。
 *  为什么换值：2026 版的种子在客户端侧可被静态提取，配合硬闸门一起把旧版作废。
 *  ⚠️ 它不是安全边界（客户端侧的密钥必然可提取），只用于「随手篡改 / 版本失配」的检出。 */
const ENV_REPORT_SEED_STORE = ['t-En', 'ort-', 'KunC', '-Sig', 'lien', '2027', 'nKey', 'vRep'];
const ENV_REPORT_SEED_ORDER = [2, 4, 0, 7, 1, 5, 3, 6];
function envReportKey() {
  const seed = ENV_REPORT_SEED_ORDER.map(i => ENV_REPORT_SEED_STORE[i]).join('');
  return crypto.createHash('sha256').update(Buffer.from(seed, 'utf8')).digest();
}
function envSign(nonce, body) {
  return crypto.createHmac('sha256', envReportKey()).update(nonce + '|' + body).digest('hex');
}

/** 防伪校验答案盐值：与客户端 EnvReportService.PuzzleSalt() 逐字一致（乱序片段 + 索引表）。
 *  🔴 不一致 → 全体用户 challenge_fail（HIGH）→ 会被熔断兜住不自动封号，但功能失效。 */
const ENV_PUZZLE_SALT_STORE = ['-En', 'reh', 'Kun', '-Fi', 'v20', 'ose', '27'];
const ENV_PUZZLE_SALT_ORDER = [2, 3, 1, 5, 0, 4, 6];
const ENV_PUZZLE_SALT = ENV_PUZZLE_SALT_ORDER.map(i => ENV_PUZZLE_SALT_STORE[i]).join('');

/** 该账号最近一条申诉（被封时回传给用户，让他知道自己申诉的进展）。 */
function appealOf(userId) {
  const r = db.exec("SELECT id, status, reply, created_at, handled_at FROM appeals WHERE user_id=? ORDER BY id DESC LIMIT 1",
    [userId]);
  if (!r.length || !r[0].values.length) return null;
  const a = rowToObj(r);
  return { id: a.id, status: a.status, reply: a.reply || '', createdAt: a.created_at || '', handledAt: a.handled_at || '' };
}

/** 环境信号码 → 用户可读说法。技术码（challenge_fail 等）仍留在 ban_reason / 流水里给管理员查，
 *  但**回给用户的话必须是人话** —— 技术码用户只会一头雾水。 */
const ENV_SIGNAL_USER_TEXT = {
  challenge_fail:   '客户端版本不对或已被修改',
  nonce_invalid:    '校验请求重复或已失效',
  multi_instance:   '同一账号在多台设备上同时使用',
  bad_hmac:         '请求签名不对',
  integrity_fail:   '客户端文件被改动',
  debug_bypassed:   '客户端正在被调试',
  inline_hook:      '客户端被人为篡改',
  injected_module:  '客户端被注入了第三方模块',
  intercept_tool:   '电脑上开着抓包工具',
  // 2026-10-24：客户端已改成「必须同时有抓包工具在跑」才报这条，
  // 所以文案也要跟着说清前提 —— 只写「代理被改成抓包」会让用户以为开 VPN 就被判了。
  proxy_intercept:  '系统代理指向本机，且抓包工具正在运行',
  root_ca_injected: '系统装了抓包证书',
  // 2026-10-24 补齐：known_tool 现在会进组合判据（工具在跑 + 别的异常），
  // 它一旦出现在 ban_reason 里，friendlyBanReason 找不到映射就会把技术码原样甩给用户。
  known_tool:        '电脑上开着逆向分析工具',
  debugger:          '有调试器附加',
  vm:                '在虚拟机里运行',
  suspicious_module: '进程内有可疑模块',
  remote_session:    '远程桌面会话',
};

/** 把存库的技术理由（如「challenge_fail: 客户端校验未通过…」）翻成给用户看的人话。 */
function friendlyBanReason(reason) {
  const raw = String(reason || '');
  // 用 startsWith 而不是「按冒号切第一段」—— MID 累积存法是「intercept_tool ×3: 明细」，
  // 组合判据存法是「root_ca_injected 组合命中…」，按冒号切都取不到信号码。
  const key = Object.keys(ENV_SIGNAL_USER_TEXT).find(k => raw.startsWith(k));
  return key ? `环境异常：${ENV_SIGNAL_USER_TEXT[key]}` : raw;
}

/** 封禁提示文案：必须带上**到期时间**与**申诉方式** ——
 *  让用户莫名其妙地「登不上且不知道为什么、不知道什么时候能恢复、不知道能找谁」是最糟的体验。 */
function banMessageOf(ban, appeal) {
  const when = ban.permanent ? '永久封禁' : `封禁至 ${String(ban.until).slice(0, 10)}`;
  let s = `账号已被${when}：${friendlyBanReason(ban.reason)}`;
  if (appeal) {
    const map = { pending: '申诉已提交，等待处理', approved: '申诉已通过', rejected: '申诉已被驳回' };
    s += `（${map[appeal.status] || appeal.status}${appeal.reply ? '：' + appeal.reply : ''}）`;
  } else if (!ban.permanent) {
    s += '。如认为判定有误，可在登录页提交申诉，我们会人工复核。';
  }
  return s;
}

// ====== 新用户试用：注册时按 vip_trial_days 设置赠送会员天数（0 = 不赠送） ======
// 与「付费工具」配套：开启付费模式后，可用该设置为新注册账号赠送体验时长。
function applyTrialDays(user) {
  if (!user || !user.id) return;
  const days = parseInt(getSetting('vip_trial_days', '0'), 10) || 0;
  if (days <= 0) return;
  const iso = addVipDays(user.id, days);
  if (iso) console.log(`[注册] 已为新用户 ${user.username} 赠送 ${days} 天会员试用（到期 ${iso.slice(0, 10)}）`);
}

// ====== VIP 剩余天数（小于 1 天按 0 计；无到期时间返回 0） ======
function daysLeftOf(vipExpireAt) {
  if (!vipExpireAt) return 0;
  const d = new Date(String(vipExpireAt).includes('T') || String(vipExpireAt).includes('Z')
    ? vipExpireAt : vipExpireAt.replace(' ', 'T') + 'Z');
  if (isNaN(d)) return 0;
  return Math.max(0, Math.ceil((d.getTime() - Date.now()) / 86400000));
}

// ====== 永久会员约定 ======
// 「永久」用一个远期到期日表示（客户端/控制台统一按「剩余 ≥ 3650 天」判定为永久），
// 这样无需改动数据库结构，并且所有基于日期比较的逻辑（是否过期、SQL datetime('now')）
// 都天然成立。展示层必须把这种日期渲染成「永久」而不是一个具体年份。
const PERMANENT_VIP_ISO = '9999-12-31T00:00:00.000Z';
const PERMANENT_VIP_MIN_DAYS = 3650;
function isVipForever(vipExpireAt) {
  return daysLeftOf(vipExpireAt) >= PERMANENT_VIP_MIN_DAYS;
}

// 把 JS Date 收成安全的到期时间 ISO 串。
// 在「已经是永久（9999-12-31）」的账号上继续叠加天数，日期会越过 9999 年，
// toISOString() 便输出 ISO 8601「扩展年份」写法（+010099-12-06T00:00:00.000Z）。
// 客户端是 .NET，DateTime 上限正好是 9999 年 → DateTime.TryParse 必然失败，
// 表现为「永久会员既不显示『永久』、还被 MembershipService 当成非会员限次」。
// 所以凡是超出可表示范围的，一律收敛回 PERMANENT_VIP_ISO。
function safeExpireIso(date) {
  if (!(date instanceof Date) || isNaN(date.getTime())) return null;
  return date.getUTCFullYear() > 9999 ? PERMANENT_VIP_ISO : date.toISOString();
}

// ====== 会员分级：普通用户 / 高级会员 / 特权会员 ======
// users.vip_level 存等级，vip_expire_at 存「当前等级」的到期时间。
// 等级过期后不写库、读时判定回落为 normal（无需定时任务，也不会出现脏状态）。
// ⚠️ 内部 key 保持 normal/vip/svip **不变**（数据库存量、卡密前缀 KUN-VIP-/KUN-SVIP-、
//    客户端 MembershipService 都按它走）—— 2026-09-28 只改**展示文案**，不动标识。
const VIP_LEVEL_RANK = { normal: 0, vip: 1, svip: 2 };
const VIP_LEVEL_LABEL = { normal: '普通用户', vip: '高级会员', svip: '特权会员' };

// ====== 法务合规：条款版本 ======
// 用户注册时必须勾选「已阅读并同意《用户协议》《隐私政策》《免责声明》」，
// 服务端把同意时间与当时的条款版本写进 users.terms_accepted_at / legal_version，
// 作为「用户确实同意过」的凭证（将来条款有实质变更就 +1，便于区分同意的是哪一版）。
//
// 🔴 这个版本号有**四份拷贝**，必须完全一致（条款文本是同一套）：
//    ① 本文件 LEGAL_VERSION；② console/index.html 的 LEGAL_VERSION；
//    ③ 客户端 DisclaimerDialog.LegalVersion；④ 客户端 LoginDialog.LegalVersion。
//    2026-09-29 踩过：改了隐私内容只 bump 了 ③，①②④ 还停在 4 ——
//    后果是注册凭证里写的是**旧版本号**，而 users.legal_version **全项目没有任何读取方**，
//    所以这个错**不会自己暴露**，只能靠发版时四处一起 grep。
const LEGAL_VERSION = 6;
const LEGAL_DOCS = '《用户协议》《隐私政策》《免责声明》';

function normalizeVipLevel(v) {
  const s = String(v || '').trim().toLowerCase();
  return VIP_LEVEL_RANK[s] === undefined ? 'normal' : s;
}
// 实际生效的会员等级（已过期 → 回落为 normal）
function effectiveVipLevel(user) {
  if (!isVipActive(user?.vip_expire_at)) return 'normal';
  const lv = normalizeVipLevel(user?.vip_level);
  // 兼容历史数据：只有 vip_expire_at、vip_level 还是默认值的账号按 vip 计
  return lv === 'normal' ? 'vip' : lv;
}
// 等级相减：给卡密「只能升级、不能掉级」用（0 源卡密视为 vip）
function vipLevelRank(v) { return VIP_LEVEL_RANK[normalizeVipLevel(v)]; }

// ====== 功能目录 + 会员额度（2026-09-28 起：不再限次） ======
// 功能目录是客户端放行判定与官网权益表的唯一来源（三端必须一致，别在别处再写一份）。
// group 只用于展示分组（控制台 / 官网），不参与放行判定。
// key 命名规则：`<域>.<动作>`，域 = edl / fastboot / oneplus / mtk / payload / cmd / driver
//   / mirror / app / file / android。
// 新增功能只要往这里加一行，官网「会员权益」表与控制台会自动多出一行；
// 三个等级的 open 都是 ['*']，新功能默认对所有等级开放。
//
// 🔴 2026-10-12 对齐客户端：客户端已新增 MTK 刷写页、命令行页、云端驱动页，而目录里
//    一直只有老八域，导致控制台/官网的权益表把这三块能力漏掉。本次补齐 mtk.flash /
//    mtk.read / cmd.shell / driver.cloud。
//    ⚠️ 「可视刷写」页不单列 key —— 它做的还是 edl.write / edl.erase / edl.gpt / edl.backup
//       这几件事，只是换了操作方式；单列会造成同一个能力重复计数。
//    ⚠️ 客户端侧的调用点：MTK / 命令行 / 云端驱动 / 小米线刷 四个页面目前**还没有**接
//       MembershipService 门禁（源文件里 0 处引用）。所以这几个新 key 属于「目录先行」，
//       控制台能勾、能展示，但要真正产生放行控制，得在对应页面补 ensureAllowed/guardAndConsume。
//    ⚠️ 「读取版本」（EDL 页的 m_readVersionButton）与隐藏页 FDL / 可视刷写 同理不单列：
//       前者是只读信息动作，后两者分别由既有 key 覆盖或不在可见导航里。
//
// 🔴 每日次数限制已按用户要求整体停用（见 getMemberLimits）。现在
//    「功能能不能用」只由增值模块（m1/m2/m3）决定，**不再有次数上限**。
// 🔴 2026-10-13 删除幽灵功能 'edl.oneclick'（EDL 一键刷写）：全仓检索确认
//    C++ 客户端与 .NET 客户端都没有实现过它 —— EDL 页（内嵌 swt-edl MainWindow）
//    只有 读取分区表 / 读取版本 / 读取分区 / 擦除分区 / 写入分区 五个手动步骤，
//    VisualFlashPage 同理。它却一直显示在官网「会员权益」表与控制台模块卡里，
//    属于「官网有、产品没有」的功能。一并从 MODULE_CATALOG m1 移除。
//    （历史 feature_usage 里可能仍有它的计数行：无害，展示只遍历本目录。）
const FEATURE_CATALOG = [
  { key: 'edl.write', label: '写入指定分区', group: '刷写与擦除' },
  { key: 'edl.erase', label: '擦除 / 格式化分区', group: '刷写与擦除' },
  { key: 'edl.gpt', label: '分区表（GPT）写入 / 修复', group: '刷写与擦除' },
  { key: 'fastboot.flash', label: 'Fastboot 刷写', group: '刷写与擦除' },
  { key: 'fastboot.erase', label: 'Fastboot 擦除 / 格式化', group: '刷写与擦除' },
  { key: 'fastboot.bl', label: '引导锁解锁 / 回锁', group: '刷写与擦除' },
  { key: 'oneplus.flash', label: 'OnePlus 一键刷写', group: '刷写与擦除' },
  // 2026-10-13 补：小米线刷页（XiaomiFastbootPage，类 MiFlash：flash_all 三档脚本 +
  // 反回滚检查）是完整实现，但目录里一直没有它 —— 属于「产品有、官网没有」。
  { key: 'xiaomi.flash', label: '小米线刷（Fastboot）', group: '刷写与擦除' },
  { key: 'mtk.flash', label: 'MTK 刷写（BROM / scatter）', group: '刷写与擦除' },

  { key: 'edl.readtable', label: '读取分区表', group: '读取与备份' },
  { key: 'edl.backup', label: '分区备份 / 导出镜像', group: '读取与备份' },
  { key: 'payload.extract', label: 'Payload 分区提取', group: '读取与备份' },
  { key: 'mtk.read', label: 'MTK 分区读回', group: '读取与备份' },

  { key: 'android.tools', label: 'Android 工具（OFP / OPS / Super 处理）', group: '专项与工具' },
  // 2026-10-20 合规下线：「清除 FRP」不再作为功能项存在。
  // 说明：FRP 是厂商防盗机制，把它做成「按次扣额度的会员功能」在性质上等于公开售卖
  // 绕过防盗机制的能力（《刑法》285 条第 3 款的「明知」很难否认）。
  // 注意：这里删掉 key 之后，已发布的客户端调用 /api/app/check|consume 会拿到
  // 「未知功能项」→ 该按钮在旧版本上也会被服务端拒绝（等于远端 kill switch）。
  { key: 'mirror.scrcpy', label: '屏幕镜像', group: '专项与工具' },
  { key: 'app.manage', label: '应用管理（卸载 / 冻结 / 提包）', group: '专项与工具' },
  { key: 'file.transfer', label: '文件传输与批量安装', group: '专项与工具' },
  { key: 'cmd.shell', label: '命令行（ADB / Fastboot 指令台）', group: '专项与工具' },
  { key: 'driver.cloud', label: '云端驱动下载', group: '专项与工具' },
];

// 次数语义：> 0 = 每天最多多少次；0（或缺失）= 不限次。
// open 语义：该等级可用的功能 key 列表；['*'] = 全部开放（含以后新增的功能）。
//
// 🔴 三个等级现在都是「全开放 + 不限次」= 停用限次后的唯一形态（用户要求：把会员额度删除）。
//    limits 留成空对象即可：getMemberLimits() 会把 FEATURE_CATALOG 里每个 key 显式补成 0，
//    三端拿到的仍是一份「每个功能都写了次数」的完整结构，不需要改任何消费方。
const DEFAULT_MEMBER_LIMITS = {
  normal: { open: ['*'], limits: {} },
  vip:    { open: ['*'], limits: {} },
  svip:   { open: ['*'], limits: {} },
};

/**
 * 读出生效的会员额度配置。
 *
 * 🔴 2026-09-28（用户要求「把会员额度删除」）：**每日次数限制已整体停用** ——
 *    这里直接返回「全等级 / 全功能开放 + 不限次」，**不再读 app_settings.member_limits**。
 *
 *    为什么连读都不读：库里的 member_limits 是上一版控制台写进去的历史配置
 *    （例如普通用户 edl.oneclick 每天 2 次）。如果只是删掉控制台那张卡、后端仍旧读库，
 *    老配置会**继续生效** —— 用户以为额度已经取消，实际还在被限次，而且再没有任何入口
 *    能看到或改回来（控制台已经不能编辑它了）。所以这里彻底不读库：历史值即使还躺在库里，
 *    也影响不到放行。
 *
 *    保留了什么：整套额度机制（FEATURE_CATALOG / feature_usage 计数表 / featureQuotaFor /
 *    quotaDecision / /api/app/check|consume|quota）**一行都没删**，`cap = 0`（不限次）
 *    这条现成分支被直接复用，所以 feature_usage 也不会再累积无意义的行。
 *    以后想恢复按等级限次：把本函数改回「SELECT value FROM app_settings WHERE key='member_limits'
 *    + 逐 key 回落 DEFAULT_MEMBER_LIMITS（并把 quotaEnabled 置 true）」，同时把控制台那张卡加回来。
 *    ⚠️ 这里**没有**留着注释掉的旧实现，别在下面找 —— 别照着一句对不上号的注释去改。
 */
function getMemberLimits() {
  const levels = {};
  ['normal', 'vip', 'svip'].forEach(lv => {
    const def = DEFAULT_MEMBER_LIMITS[lv];
    const limits = {};
    FEATURE_CATALOG.forEach(f => { limits[f.key] = 0; });   // 0 = 不限次
    // 2026-10-20 付费下线：三级统一「全开 + 不限次」。
    // 🔴 这一层必须改，不能只改 quotaDecision —— 已发布的客户端会先拿这份 limits
    //    做**本地预判**（isFeatureOpen），预判不通过时根本不会来问服务端。
    levels[lv] = { open: ['*'], limits };
  });

  // quotaEnabled 供三端判断「还要不要显示每日次数」：false 时官网/控制台一律显示「不限次」，
  // 不必各自去猜「全是 0 是不是意味着停用」。
  return { features: FEATURE_CATALOG, levelLabels: VIP_LEVEL_LABEL, levels, quotaEnabled: false };
}

// ====== 增值模块：可按用户单独开通（收费）的功能包（2026-09-28）======
// 需求原文：「把一类的功能变成一个模块 1/2/3，可以给各个用户单独改权限，模块需要另外收费」。
//
// 两层授权，**AND** 关系：
//   ① 会员等级（normal/vip/svip）→ 决定「基础功能能不能用」（每日次数限制已于 2026-09-28 整体停用，
//      现在三级一律不限次，所以这一层实际只影响下面没被模块覆盖的基础功能）；
//   ② 模块（m1/m2/m3）          → 决定「这一类功能到底能不能用」。
// 模块可以「包含在」若干等级里（includedLevels）：被包含的等级无需单独开通；
// 没被包含的等级，必须为**这个用户**单独开通（user_modules 表）才能用。
//
// 🔴 默认值刻意让三个模块都被三个等级包含 = **与改造前行为完全一致**，
//    上线不会一夜之间把现有用户锁死。要开始对某个模块收费，在控制台把该模块
//    从对应等级的「已包含」里取消勾选即可，其他功能完全不受影响。
//
// 🔴 2026-10-13：features（功能归属）从「只读默认值」升级为**可配置** ——
//    控制台「增值模块」卡现在能用单选矩阵把任一功能在三个模块间迁移，
//    或取消归属（选「不归入」= 该功能不再受任何模块授权限制）。
//    存 app_settings.member_modules 的 modules.<id>.features；缺省时回落下面的
//    默认值（老配置不出事），显式空数组 = 空模块（合法配置）。
//    服务端仍做两层防护：陌生 key 一律丢弃；同一功能配到两个模块时先到先得
//    （按 MODULE_CATALOG 顺序），并打警告 —— 控制台的单选矩阵本就堵不住
//    直接调 API 的脏数据。
const MODULE_CATALOG = [
  {
    id: 'm1', name: '模块1', title: '刷写与擦除',
    desc: 'EDL / Fastboot / MTK 刷写、擦除、GPT 与引导锁操作',
    features: ['edl.write', 'edl.erase', 'edl.gpt',
               'fastboot.flash', 'fastboot.erase', 'fastboot.bl', 'oneplus.flash', 'xiaomi.flash', 'mtk.flash'],
  },
  {
    id: 'm2', name: '模块2', title: '读取与备份',
    desc: '读取分区表、分区备份 / 导出镜像、Payload 分区提取、MTK 分区读回',
    features: ['edl.readtable', 'edl.backup', 'payload.extract', 'mtk.read'],
  },
  {
    id: 'm3', name: '模块3', title: '专项与工具',
    desc: 'Android 工具（OFP / OPS / Super）、屏幕镜像、应用管理、文件传输、命令行、云端驱动',
    features: ['android.tools', 'mirror.scrcpy', 'app.manage', 'file.transfer',
               'cmd.shell', 'driver.cloud'],
  },
];

const ALL_LEVELS = ['normal', 'vip', 'svip'];

/** 同一句话只警告一次：getModules() 每请求都会走，脏配置不能把日志刷爆。 */
const _moduleWarned = new Set();
function warnModuleOnce(msg) {
  if (_moduleWarned.has(msg)) return;
  _moduleWarned.add(msg);
  console.warn(msg);
}

/**
 * 读出生效的模块配置。与 getMemberLimits() 同一套「脏数据一律回落默认值」策略，
 * 保证三端拿到的永远是一份完整结构。存 app_settings.member_modules（JSON）。
 *
 * 🔴 2026-10-13：features 可配置（控制台单选矩阵迁移/取消归属）。校验规则：
 *   · 陌生 key（不在 FEATURE_CATALOG）→ 丢弃 + 警告（防手改库写入幽灵功能）；
 *   · 同一功能配进两个模块 → **先到先得**（按 MODULE_CATALOG 顺序）+ 警告；
 *   · 显式空数组 [] → 空模块（合法：该模块下没有任何功能）；
 *   · 字段缺省 → 回落 MODULE_CATALOG 默认归属。
 *   · 显式配置**优先于**别模块的默认值：两遍扫描，先让所有显式数组占位，
 *     再用默认值补没显式配置的模块（避开已被显式占走的功能）——
 *     否则「把功能从 m1 迁到 m2」会被 m1 的默认值反咬一口。
 */
function getModules() {
  let parsed = null;
  const raw = String(getSetting('member_modules', '') || '').trim();
  if (raw) {
    try { parsed = JSON.parse(raw); }
    catch { warnModuleOnce('[模块] member_modules 不是合法 JSON，已回落默认值'); }
  }
  const src = (parsed && typeof parsed === 'object' && parsed.modules && typeof parsed.modules === 'object')
    ? parsed.modules : {};

  const validKeys = new Set(FEATURE_CATALOG.map(f => f.key));
  const claimed = {};   // featureKey -> moduleId

  // 第一遍：显式配置占位（控制台保存的归属从这里来）
  const explicit = {};
  MODULE_CATALOG.forEach(def => {
    const cur = (src[def.id] && typeof src[def.id] === 'object') ? src[def.id] : {};
    if (!Array.isArray(cur.features)) return;
    const list = [];
    cur.features.forEach(v => {
      const key = String(v || '').trim();
      if (!validKeys.has(key)) { warnModuleOnce(`[模块] ${def.id} 的功能 ${key} 不在功能目录里，已忽略`); return; }
      if (claimed[key]) { warnModuleOnce(`[模块] 功能 ${key} 同时配在 ${claimed[key]} 与 ${def.id}，已保留前者`); return; }
      claimed[key] = def.id;
      if (!list.includes(key)) list.push(key);
    });
    explicit[def.id] = list;
  });

  const modules = MODULE_CATALOG.map(def => {
    const cur = (src[def.id] && typeof src[def.id] === 'object') ? src[def.id] : {};
    const pick = (v, fb) => (typeof v === 'string' && v.trim() !== '') ? v.trim() : fb;
    // 🔴 「字段缺省」与「显式空数组」是两种意思，绝不能合并处理：
    //    缺省       → 回落「全部等级包含」（与改造前一致，老配置不出事）
    //    空数组 []  → 就是「所有等级都不包含」= 该模块只能单独开通（收费的极端配置）
    //    之前把两者都当成"没配"，结果「所有等级都不包含」这个选项根本设不出来。
    //   normalizeVipLevel 会把 'VIP' 之类的大小写变体归一化，所以不会出现"值都认不出"的情况。
    const includedLevels = Array.isArray(cur.includedLevels)
      ? cur.includedLevels.map(v => normalizeVipLevel(v)).filter((v, i, a) => a.indexOf(v) === i)
      : ALL_LEVELS.slice();
    // features：显式配置用保存值；缺省用代码默认，但避开已被别模块显式占走的功能
    let features;
    if (explicit[def.id]) {
      features = explicit[def.id];
    } else {
      features = def.features.filter(k => !claimed[k]);
      features.forEach(k => { claimed[k] = def.id; });
    }
    return {
      id: def.id,
      name: pick(cur.name, def.name),
      title: pick(cur.title, def.title),
      desc: pick(cur.desc, def.desc),
      priceNote: pick(cur.priceNote, ''),
      features,
      // 2026-10-20：模块不再按等级售卖 —— 三个等级都视为已包含，等价于全开。
      includedLevels: ALL_LEVELS.slice(),
    };
  });
  return { modules };
}

/**
 * 一致性自检：功能目录与模块归属必须严丝合缝。
 * 这是**启动期就报警**（不是运行期才炸），新加功能忘了归模块会立刻在日志里看到。
 * 查的是**代码默认值**（MODULE_CATALOG）；运行期生效配置的脏数据由 getModules()
 * 内部按 key 校验 + warnModuleOnce 兜底（见上方注释）。
 */
(function assertFeatureModuleCoverage() {
  const owner = {};
  MODULE_CATALOG.forEach(m => m.features.forEach(k => {
    if (owner[k]) console.warn(`[模块] 功能 ${k} 同时属于 ${owner[k]} 与 ${m.id}`);
    owner[k] = m.id;
  }));
  const orphan = FEATURE_CATALOG.filter(f => !owner[f.key]).map(f => f.key);
  const ghost = Object.keys(owner).filter(k => !FEATURE_CATALOG.some(f => f.key === k));
  if (orphan.length) console.warn('[模块] 以下功能未归入任何模块（不受模块授权限制）:', orphan.join(', '));
  if (ghost.length) console.warn('[模块] 以下模块内 key 不在功能目录里（拼写错误？）:', ghost.join(', '));
})();

/** 功能属于哪个模块（返回完整模块对象；未归入 → null = 不受模块授权限制）。
 *  🔴 2026-10-13 起从**生效配置**推导（控制台可迁移/取消归属），不再是启动期快照。 */
function moduleOfFeature(featureKey) {
  const mods = getModules().modules;
  for (const m of mods) if (m.features.includes(featureKey)) return m;
  return null;
}

/** 对外披露的模块信息（只带客户端提示需要的字段，不带 includedLevels 这类配置）。 */
function modulePubInfo(m) {
  return { id: m.id, name: m.name, title: m.title, priceNote: m.priceNote };
}

/**
 * 该用户对某模块的**单独开通**记录。未开通 / 已过期 → null。
 * ⚠️ expire_at 是 ISO 串，判定一律交给 isVipActive()，**不要**和字符串裸比较
 *    （`'T' > ' '` 会让「当天到期」被判成还没过期）。
 */
function userModuleGrant(userId, moduleId) {
  try {
    const row = rowToObj(db.exec(
      "SELECT module_id, expire_at, granted_at, note FROM user_modules WHERE user_id = ? AND module_id = ?",
      [userId, moduleId]));
    if (!row) return null;
    // expire_at 为空 = 永久（跟账号走）；有值且已过期 = 失效。
    if (row.expire_at && !isVipActive(row.expire_at)) return null;
    return row;
  } catch { return null; }
}

// ====== 反破解：功能额度的服务端裁决（2026-09-25）======
// 这里才是「能不能用 / 还能用几次」的唯一权威。客户端 MembershipService 只是
// 拿这里的裁决结果做提示与放行，本地缓存（limits.json / quota.json）一律只作展示。
//
// ⚠️ 为什么必须放在服务端：EDL 刷写这类动作是在**用户自己的机器与设备**上执行的，
// 没有任何服务端资源被消耗 —— 所以客户端无论如何都能被改。能挡住「改文件删文件」这种
// 零技术手段、并把有技术的人的成本抬到不值得，就是这里的目标。

/** 额度结算的自然日（北京时间）。服务端时区通常是 UTC，直接用 toISOString() 会错一天。 */
function quotaDayKey() {
  return new Date(Date.now() + 8 * 3600 * 1000).toISOString().slice(0, 10);
}

/** 取「当前生效等级」需要的会员字段。authMiddleware 的 req.actor 只带了 id/username/role/status。 */
function loadQuotaUser(userId) {
  try {
    return rowToObj(db.exec(
      "SELECT id, username, status, vip_level, vip_expire_at FROM users WHERE id = ?", [userId]));
  } catch { return null; }
}

/**
 * 某用户对某功能：是否开放 + 每日次数（0 = 不限次）。
 * 开放 = 「等级额度里有这个功能」**且**「该功能所属模块对本用户可用」。
 */
function featureQuotaFor(user, featureKey) {
  // 2026-10-20 付费体系整体下线：不再有等级 / 模块 / 每日次数三重闸门。
  // 保留函数与返回结构（调用方 quotaDecision / 控制台仍在读这些字段），
  // 恒返回「开放 + 不限次」—— quotaDecision 因此永远 allowed，等价于全功能免费。
  return { level: 'normal', open: true, openByLevel: true, cap: 0, module: null, moduleOk: true };
}

function usageToday(userId, featureKey, day) {
  const row = rowToObj(db.exec(
    "SELECT count FROM feature_usage WHERE user_id = ? AND feature_key = ? AND day = ?",
    [userId, featureKey, day]));
  return row ? (parseInt(row.count, 10) || 0) : 0;
}

function usageAdd(userId, featureKey, day, delta) {
  // 不用 UPSERT：sql.js 内嵌 SQLite 版本不保证支持 ON CONFLICT DO UPDATE，
  // 「INSERT OR IGNORE 建行 + UPDATE 累加」两步在任何版本上都成立。
  db.run("INSERT OR IGNORE INTO feature_usage (user_id, feature_key, day, count) VALUES (?, ?, ?, 0)",
    [userId, featureKey, day]);
  db.run("UPDATE feature_usage SET count = count + ? WHERE user_id = ? AND feature_key = ? AND day = ?",
    [delta, userId, featureKey, day]);
}

/**
 * 额度裁决。consume=true 时在「允许」的前提下**原子地**扣一次。
 * 返回 remaining：-1 表示不限次（不是「未知」），0 表示今日已用尽。
 */
function quotaDecision(user, featureKey, consume) {
  const q = featureQuotaFor(user, featureKey);
  if (!q.open) {
    // 「等级没开放」与「模块没开通」必须分开报：前者提示升级会员，后者提示单独开通模块。
    // 合成一个 locked 的话，客户端只能笼统说「该功能不可用」，用户不知道该买什么。
    const reason = (q.openByLevel && !q.moduleOk) ? 'module' : 'locked';
    return {
      allowed: false, reason, feature: featureKey, level: q.level, cap: 0, used: 0, remaining: 0,
      module: q.module ? modulePubInfo(q.module) : null,
    };
  }
  const day = quotaDayKey();
  let used = usageToday(user.id, featureKey, day);
  if (q.cap > 0 && used >= q.cap) {
    return { allowed: false, reason: 'exhausted', feature: featureKey, level: q.level, cap: q.cap, used, remaining: 0 };
  }
  if (consume) {
    // 不限次（cap = 0）的功能不落库：否则每次动作用户都会多一行写库，
    // 表会毫无意义地膨胀。用量统计另有 logUsage() 负责。
    if (q.cap > 0) {
      usageAdd(user.id, featureKey, day, 1);
      saveDb();
      used += 1;
    } else {
      used = 0;
    }
  }
  return {
    allowed: true, reason: '', feature: featureKey, level: q.level, cap: q.cap,
    used, remaining: q.cap > 0 ? Math.max(0, q.cap - used) : -1, day,
  };
}

// ====== 卡密核销后的统一收尾：授予会员等级（只升不降） ======
// grantDays > 0 说明这是会员卡密，按其 grant_level 升级用户等级；
// 登录卡密（无天数）只做归属记录，不动等级。
function applyKeyEntitlement(userId, keyRow, grantDays) {
  const user = rowToObj(db.exec("SELECT id, vip_level, vip_expire_at, agent_id FROM users WHERE id = ?", [userId]));
  if (!user) return;

  if (grantDays > 0) {
    const granted = normalizeVipLevel(keyRow.grant_level);
    // 只升不降：SVIP 卡不会把已经是 SVIP 的账号降成 VIP
    if (vipLevelRank(granted) > vipLevelRank(user.vip_level)) {
      db.run("UPDATE users SET vip_level = ? WHERE id = ?", [granted, user.id]);
    }
  }

  // 「归属代理」：代理角色已删除（2026-10-21），这里不再把用户挂到签卡人名下。
  // users.agent_id 列保留（存量数据还在），但不再写入、也不再作为可见性依据。
}
// ====== 付费/授权：返回账号的授权状态（供客户端与控制台判断能否使用工具） ======
function isPaidMode() {
  // 2026-10-20：付费模式下线。控制台里那两个开关（paid_mode / vip_required）保留但不再生效，
  // 免得旧配置把客户端锁在「需要会员」的状态里。
  return false;
}
function isVipRequired() { return isPaidMode() && getSetting('vip_required', '0') === '1'; }

/**
 * 计算授权信息。licensed = 是否允许使用工具功能。
 * - 非付费模式：所有人 licensed = true
 * - 付费模式 + vip_required：需要会员未过期，或账号下存在有效（active/未过期）卡密
 * - 付费模式 + 非 vip_required：登录即可用（仅作提示）
 */
function entitlementOf(user) {
  const vipExpireAt = user?.vip_expire_at || null;
  const isVip = isVipActive(vipExpireAt);
  const vipDaysLeft = daysLeftOf(vipExpireAt);
  const paidMode = isPaidMode();
  const vipRequired = isVipRequired();

  let licensed = true;
  let reason = '';
  if (vipRequired && user?.id) {
    const row = rowToObj(db.exec(
      // ⚠️ 必须写成 datetime(expires_at)：expires_at 存的是 ISO（带 'T'），
      // 直接和 datetime('now') 做字符串比较时，第 11 个字符 'T'(0x54) > ' '(0x20)
      // 会让「当天已过期」的卡密恒判定为未过期。datetime() 会把两种格式都归一化。
      "SELECT * FROM license_keys WHERE user_id = ? AND status = 'active' " +
      "AND (expires_at IS NULL OR datetime(expires_at) > datetime('now')) LIMIT 1", [user.id]));
    const hasKey = !!row;
    licensed = isVip || hasKey;
    if (!licensed) reason = '需要开通会员或使用卡密激活后才能使用工具';
  }
  const toolExpire = getSetting('tool_expire', '');
  // 2026-10-02：tool_expire 是「北京时间 yyyy-mm-dd」——原来拼 'T23:59:59' 后用 new Date()
  // 解析，落到**服务器本地时区**（UTC 服务器上相当于北京早上 7:59 就到期了，用户少用 8 小时）。
  // 显式带 +08:00 偏移，任何时区的服务器上都在北京时间当天最后一刻到期。
  if (toolExpire && new Date(toolExpire + 'T23:59:59+08:00') < new Date()) {
    licensed = false;
    reason = '工具授权已到期，请联系管理员续期';
  }
  const vipLevel = effectiveVipLevel(user);
  return {
    paidMode, vipRequired, licensed, reason, toolExpire,
    isVip: isVip || vipLevel !== 'normal', vipExpireAt, vipDaysLeft,
    vipLevel, vipLevelLabel: VIP_LEVEL_LABEL[vipLevel],
    vipPermanent: isVipForever(vipExpireAt),
    role: normalizeRole(user?.role),
    isStaff: isStaff(user?.role),
    isAdmin: isStaff(user?.role)
  };
}

function getAdminEmail() {
  return getSetting('admin_email', cfg.admin_email) || cfg.admin_email;
}

async function sendFeedbackEmail(fb) {
  if (!mailTransporter) { console.log('[Email] 邮件已禁用(config.json mail.enabled=false)，跳过反馈通知'); return; }
  const category = FEEDBACK_CATEGORIES[fb.category] || fb.category || '其他';
  // 2026-10-02：fb.title / fb.username / fb.contact 都是用户可控输入，直接拼进 HTML
  // 等于给管理员邮箱送一枚 XSS/HTML 注入点；subject 里拼还会造成邮件头注入（换行符）。
  // 统一转义：HTML 转义 + 去掉控制字符（含 CR/LF，subject 防头注入）。
  // ⚠️ 转义正则里的引号必须写成 \x22/\x27：tools/check-server.js 的简易 lexer 不认
  //    正则字面量，源码里出现裸引号会把它的字符串配对搅乱（已踩：SQL 表名被误报为未定义函数）。
  const esc = (s) => String(s ?? '').replace(/[\r\n\x00-\x1f\x7f]/g, ' ')
    .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
    .replace(/\x22/g, '&quot;').replace(/\x27/g, '&#39;');
  const safeTitle = esc(fb.title);
  const safeCategory = esc(category);
  const b = MAIL_BRAND;
  const html = mailDocument({
    kicker: '用户反馈',
    title: `#${fb.id} ${safeTitle}`,
    subtitle: `${safeCategory} · ${new Date().toLocaleString('zh-CN')}`,
    body:
      mailInfoTable([
        ['类型', safeCategory],
        ['用户', `${esc(fb.username) || '-'} <span style="color:${b.ink3}">（ID ${fb.user_id ?? '-'}）</span>`],
        ['邮箱', esc(fb.email) || '-'],
        ['联系方式', esc(fb.contact) || '-'],
      ])
      // 正文单独装：white-space:pre-wrap 保住用户敲的换行，但整块底色要和信息表拉开。
      + `<div style="font-size:12px;font-weight:700;letter-spacing:.4px;color:${b.ink3};`
      + `margin:4px 0 8px">反馈正文</div>`
      + `<div style="padding:14px 16px;background:${b.lineSoft};border-radius:12px;`
      + `font-size:13.5px;line-height:1.8;white-space:pre-wrap">`
      + `${String(fb.content || '').replace(/[<>]/g, c => c === '<' ? '&lt;' : '&gt;')}</div>`,
    footNote: fb.email
      ? '直接回复本邮件即可与用户沟通（回复地址已设为用户绑定邮箱）。'
      : '该用户未绑定邮箱，无法直接回复。',
  });
  const mail = {
    from: MAIL_FROM,
    to: getAdminEmail(),
    subject: `【KunClient 反馈 #${fb.id}】${safeCategory} · ${safeTitle}`,
    html
  };
  if (fb.email && /^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(fb.email)) mail.replyTo = fb.email;
  await mailTransporter.sendMail(mail);
}

async function start() {
  const SQL = await initSqlJs();
  if (fs.existsSync(DB_PATH)) {
    db = new SQL.Database(fs.readFileSync(DB_PATH));
  } else {
    db = new SQL.Database();
  }

  db.run(`CREATE TABLE IF NOT EXISTS users (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    username TEXT UNIQUE NOT NULL,
    password TEXT NOT NULL,
    email TEXT DEFAULT '',
    role TEXT DEFAULT 'user',
    status TEXT DEFAULT 'active',
    signature TEXT DEFAULT '',
    email_login_enabled INTEGER DEFAULT 1,
    login_notify_enabled INTEGER DEFAULT 1,
    created_at TEXT DEFAULT (datetime('now')),
    last_login TEXT,
    vip_level TEXT DEFAULT 'normal',
    agent_id INTEGER DEFAULT NULL,
    terms_accepted_at TEXT,
    legal_version INTEGER DEFAULT 0,
    session_epoch INTEGER DEFAULT 0
  )`);

  // Add missing columns if they don't exist
  try { db.run("ALTER TABLE users ADD COLUMN signature TEXT DEFAULT ''"); } catch(e) {}
  try { db.run("ALTER TABLE users ADD COLUMN email_login_enabled INTEGER DEFAULT 1"); } catch(e) {}
  try { db.run("ALTER TABLE users ADD COLUMN login_notify_enabled INTEGER DEFAULT 1"); } catch(e) {}
  try { db.run("ALTER TABLE users ADD COLUMN avatar TEXT DEFAULT ''"); } catch(e) {}
  try { db.run("ALTER TABLE users ADD COLUMN vip_expire_at TEXT"); } catch(e) {}
  // 会员分级：normal(普通) / vip / svip —— vip_expire_at 表示「当前等级」的到期时间
  try { db.run("ALTER TABLE users ADD COLUMN vip_level TEXT DEFAULT 'normal'"); } catch(e) {}
  // 归属代理：由哪个代理发展的用户（仅 agent 名下的用户可见/可管）
  try { db.run("ALTER TABLE users ADD COLUMN agent_id INTEGER DEFAULT NULL"); } catch(e) {}
  // 法务合规：同意条款的时间 + 当时生效的条款版本（存量账号为空/0 = 历史未留痕）
  try { db.run("ALTER TABLE users ADD COLUMN terms_accepted_at TEXT"); } catch(e) {}
  try { db.run("ALTER TABLE users ADD COLUMN legal_version INTEGER DEFAULT 0"); } catch(e) {}
  // 2026-10-20：个人信息出境（服务器在中国香港）的**单独同意**时间戳。
  // 与 terms_accepted_at 分开存 —— PIPL 第 39 条要求「单独同意」，不能和用户协议捆成一次勾选。
  try { db.run("ALTER TABLE users ADD COLUMN cross_border_consent_at TEXT"); } catch(e) {}
  // 云端强制下线（2026-09-28）：unix 秒，0 = 从未被强制下线。
  // 判定见 authMiddleware —— token 的 iat < session_epoch ⇒ 这份 token 是「被踢之前」签发的，直接失效。
  try { db.run("ALTER TABLE users ADD COLUMN session_epoch INTEGER DEFAULT 0"); } catch(e) {}

  // ====== 环境检测 → 时限封禁（2026-09-30，用户要求「检测到问题自动封禁账户几天」）======
  // 与既有的 `status='banned'`（管理员手工、**永久**、无到期）是**两套并存**的东西：
  //   · status='banned'  → 人工终审，不动
  //   · banned_until     → 自动处置，有到期时间，到点自动恢复
  // 分开的理由：自动判定一定有误伤，必须让它「自己到期消失」，不能和人工封禁混成一个字段，
  // 否则管理员解封时不小心就把自动封禁的证据也抹掉了。
  try { db.run("ALTER TABLE users ADD COLUMN banned_until TEXT"); } catch(e) {}
  try { db.run("ALTER TABLE users ADD COLUMN ban_reason TEXT DEFAULT ''"); } catch(e) {}
  try { db.run("ALTER TABLE users ADD COLUMN ban_level TEXT DEFAULT ''"); } catch(e) {}
  // MID 级信号的累积计数（按自然日分桶，跨天自动归零）
  try { db.run("ALTER TABLE users ADD COLUMN mid_strike INTEGER DEFAULT 0"); } catch(e) {}
  try { db.run("ALTER TABLE users ADD COLUMN mid_strike_day TEXT DEFAULT ''"); } catch(e) {}

  // 环境告警流水：**只追加、不删**。它是申诉时唯一的查证依据 —— 没有流水，
  // 用户申诉「我什么都没干」时管理员无从判断。
  db.run(`CREATE TABLE IF NOT EXISTS env_reports (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id INTEGER NOT NULL,
    username TEXT DEFAULT '',
    level TEXT NOT NULL,
    signal TEXT NOT NULL,
    detail TEXT DEFAULT '',
    build TEXT DEFAULT '',
    ip TEXT DEFAULT '',
    created_at TEXT DEFAULT (datetime('now'))
  )`);
  try { db.run("CREATE INDEX IF NOT EXISTS idx_env_reports_user ON env_reports(user_id, created_at)"); } catch(e) {}
  try { db.run("CREATE INDEX IF NOT EXISTS idx_env_reports_sig ON env_reports(signal, created_at)"); } catch(e) {}

  // 防伪校验（HIGH 级里**唯一服务端可自证**的一项）：服务端下发一次性 nonce，
  // 客户端必须用内嵌密钥算出 HMAC 才能答对 —— 用后即删，重放即判失败。
  db.run(`CREATE TABLE IF NOT EXISTS env_challenges (
    nonce TEXT PRIMARY KEY,
    user_id INTEGER NOT NULL,
    created_at TEXT DEFAULT (datetime('now'))
  )`);

  // 用户申诉（2026-09-30，配套自动封禁）。
  // 🔴 申诉**不自动解封** —— 否则攻击者提交一条申诉就绕过了封禁，那这功能等于没做。
  //    它只是把「我不服」连同证据流水送到管理员面前，由人来决定。
  db.run(`CREATE TABLE IF NOT EXISTS appeals (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id INTEGER NOT NULL,
    username TEXT DEFAULT '',
    ban_reason TEXT DEFAULT '',
    content TEXT NOT NULL,
    contact TEXT DEFAULT '',
    status TEXT DEFAULT 'pending',
    reply TEXT DEFAULT '',
    handled_by INTEGER,
    handled_at TEXT,
    ip TEXT DEFAULT '',
    created_at TEXT DEFAULT (datetime('now'))
  )`);
  try { db.run("CREATE INDEX IF NOT EXISTS idx_appeals_user ON appeals(user_id, created_at)"); } catch(e) {}

  // 反破解（2026-09-25）：功能用量改为**服务端记账**，这是「额度不可绕过」的关键。
  // 之前的实现把「今天用了多少次」只写在客户端 %LOCALAPPDATA%\KunClient\quota.json，
  // 服务端完全不知情 —— 用户删掉那个文件就能无限刷，连反编译都不需要。
  // 现在每次动作前客户端必须来 POST /api/app/check 或 /api/app/consume 取裁决，
  // 计数落在下面这张表里。day 是**北京时间**的自然日（服务端时区可能是 UTC，
  // 直接用 toISOString() 会在早上 8 点前把用户算到前一天）。
  db.run(`CREATE TABLE IF NOT EXISTS feature_usage (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id INTEGER NOT NULL,
    feature_key TEXT NOT NULL,
    day TEXT NOT NULL,
    count INTEGER NOT NULL DEFAULT 0
  )`);
  try { db.run("CREATE UNIQUE INDEX IF NOT EXISTS idx_feature_usage ON feature_usage(user_id, feature_key, day)"); } catch(e) {}

  // 增值模块的**逐用户**开通记录（2026-09-28）：管理员在控制台按用户开通 / 撤销。
  //   expire_at 为 NULL = 永久（跟账号走）；有值 = 该日期前有效。
  //   ⚠️ 复合主键是必须的 ——「开通 / 撤销 / 改到期」三个动作都靠它做 INSERT OR REPLACE / DELETE；
  //      没有主键就会出现「同一用户同一模块多行」，userModuleGrant 读到哪行全看运气。
  //   ⚠️ sql.js 不保证支持 ON CONFLICT DO UPDATE，所以统一用 INSERT OR REPLACE（标准 SQLite 语法）。
  db.run(`CREATE TABLE IF NOT EXISTS user_modules (
    user_id INTEGER NOT NULL,
    module_id TEXT NOT NULL,
    expire_at TEXT,
    granted_by INTEGER,
    granted_at TEXT,
    note TEXT,
    PRIMARY KEY (user_id, module_id)
  )`);

  // IP 封禁（2026-09-28，用户要求「增加一个封禁 ip」）：管理员手动录入的名单。
  //   ip 做主键：同一个 IP 只有一条生效记录（重复录入 = 覆盖，不做「多条累加」）。
  //   expire_at 为 NULL = 永久；有值 = 该时刻前有效（ISO 字符串，用 isVipActive 判定）。
  //   ip 是 TEXT：**IPv4 与 IPv6 共用这一列**，两种都存 `canonicalIp()` 归一后的规范串
  //   （2026-09-29「封禁 ip 需要支持 IPV6」）。列类型无需迁移。
  //
  //   ⚠️ 合规边界（2026-09-29 订正）：这张表里存的**只有管理员主动录入的封禁对象**。
  //      来源 IP 的采集与展示另有其规 —— 见文件顶部 clientIpOf 那一段：
  //      登录来源 IP **恢复采集**（写 login_logs.ip_address），但**仅开发者可见**。
  //      别再照旧注释理解为「访客 IP 一律不落库」。
  db.run(`CREATE TABLE IF NOT EXISTS banned_ips (
    ip TEXT PRIMARY KEY,
    reason TEXT DEFAULT '',
    created_by INTEGER,
    created_at TEXT,
    expire_at TEXT
  )`);
  // 自动封禁流水（2026-10-24）：只为一件事服务 —— **重复违规升级**。
  // 为什么不能直接用 users.ban_reason / ban_level：解封时那两列会被清空
  // （见 /api/v2/admin/users/:id/unban），而「以前被封过几次」必须跨解封存活。
  // 单独一张流水表最省事，也不用去 ALTER users（历史上 ALTER 放错位置踩过坑）。
  db.run(`CREATE TABLE IF NOT EXISTS env_bans (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id INTEGER,
    level TEXT DEFAULT '',
    days INTEGER DEFAULT 0,
    reason TEXT DEFAULT '',
    created_at TEXT
  )`);
  // ⚠️ license_keys 的补列必须放在「CREATE TABLE license_keys」之后：
  // 之前 days 的 ALTER 写在这里（表还不存在），在**全新数据库**上会被空 catch 静默吞掉，
  // 导致 days 列永远建不出来 —— 表现为首次部署时「生成卡密」直接 500。

  // ⚠️ 2026-09-29 订正：`ip_address` **不再是**历史遗留列 —— 登录来源 IP 已恢复采集
  // （见 /api/logs 与 /api/auth/login-logs，仅开发者可见）。历史那批旧值确实在启动时被抹过一次，
  // 但这一列是活的、正在写；写进去的是 `canonicalIp()` 归一化后的规范写法（IPv4 / IPv6 都有）。
  // 别看到「历史遗留」四个字就以为没在写、或去删列。
  db.run(`CREATE TABLE IF NOT EXISTS login_logs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id INTEGER,
    username TEXT,
    login_time TEXT DEFAULT (datetime('now')),
    ip_address TEXT,
    status TEXT
  )`);

  db.run(`CREATE TABLE IF NOT EXISTS license_keys (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    key_code TEXT UNIQUE NOT NULL,
    user_id INTEGER DEFAULT NULL,
    status TEXT DEFAULT 'active',
    max_devices INTEGER DEFAULT 1,
    created_at TEXT DEFAULT (datetime('now')),
    expires_at TEXT DEFAULT NULL,
    days INTEGER DEFAULT 0,
    owner_id INTEGER DEFAULT NULL,
    grant_level TEXT DEFAULT 'vip'
  )`);

  // 卡密补列（必须紧跟 CREATE TABLE 之后，见上方说明）：
  // days = 会员天数；owner_id = 签发者（代理/管理员，用于「代理只能管自己名下的卡密」）；
  // grant_level = 激活后授予的会员等级（normal/vip/svip）
  try { db.run("ALTER TABLE license_keys ADD COLUMN days INTEGER DEFAULT 0"); } catch(e) {}
  try { db.run("ALTER TABLE license_keys ADD COLUMN owner_id INTEGER DEFAULT NULL"); } catch(e) {}
  try { db.run("ALTER TABLE license_keys ADD COLUMN grant_level TEXT DEFAULT 'vip'"); } catch(e) {}

  db.run(`CREATE TABLE IF NOT EXISTS verification_codes (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    email TEXT NOT NULL,
    code TEXT NOT NULL,
    type TEXT NOT NULL,
    expires_at TEXT NOT NULL,
    created_at TEXT DEFAULT (datetime('now'))
  )`);

  // ip_address 与 login_logs 同源（canonicalIp 归一的来源 IP）。目前只建列、不主动写入 ——
  // 需要按 IP 排查「谁在刷接口」时可以从这里扩。别当成废列删掉。
  db.run(`CREATE TABLE IF NOT EXISTS usage_logs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id INTEGER,
    username TEXT,
    feature TEXT NOT NULL,
    detail TEXT DEFAULT '',
    ip_address TEXT DEFAULT '',
    created_at TEXT DEFAULT (datetime('now'))
  )`);

  db.run(`CREATE TABLE IF NOT EXISTS app_settings (
    key TEXT PRIMARY KEY,
    value TEXT DEFAULT ''
  )`);

  // 问题反馈（控制台提交 → 邮件通知管理员）
  db.run(`CREATE TABLE IF NOT EXISTS feedback (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id INTEGER,
    username TEXT DEFAULT '',
    email TEXT DEFAULT '',
    category TEXT DEFAULT 'bug',
    title TEXT NOT NULL,
    content TEXT NOT NULL,
    contact TEXT DEFAULT '',
    status TEXT DEFAULT 'open',
    reply TEXT DEFAULT '',
    created_at TEXT DEFAULT (datetime('now')),
    updated_at TEXT DEFAULT (datetime('now'))
  )`);

  // 客户端日志上传（2026-10-12，用户要求「上传日志只给开发者看，目的是修工具」）。
  // 🔴 仅开发者可见：列表/详情/下载/删除全部走 developerMiddleware，管理员与代理
  //    连接口都进不去 —— 不是前端藏起来的那种「仅开发者可见」。
  // 文件本体落盘 client-logs/（**不在 /assets 下**，避免被静态映射公开下载；见 CLIENT_LOG_DIR）。
  // 为什么不入库：sql.js 每次 saveDb 都重写整个库文件，二进制放大进去会让
  // database.sqlite 无限膨胀），库里只存元数据 + 文件名。
  // status：new（未看）→ viewed（详情被打开过即置）。
  db.run(`CREATE TABLE IF NOT EXISTS client_logs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id INTEGER,
    username TEXT DEFAULT '',
    build TEXT DEFAULT '',
    note TEXT DEFAULT '',
    size INTEGER DEFAULT 0,
    file TEXT DEFAULT '',
    ip TEXT DEFAULT '',
    status TEXT DEFAULT 'new',
    created_at TEXT DEFAULT (datetime('now'))
  )`);
  try { db.run("CREATE INDEX IF NOT EXISTS idx_client_logs_user ON client_logs(user_id, created_at)"); } catch(e) {}

  // Add columns if missing
  try { db.run("ALTER TABLE users ADD COLUMN email TEXT DEFAULT ''"); } catch {}
  try { db.run("ALTER TABLE users ADD COLUMN signature TEXT DEFAULT ''"); } catch {}
  try { db.run("ALTER TABLE login_logs ADD COLUMN device_info TEXT DEFAULT ''"); } catch {}
  // 登录应用来源（2026-10-15 用户要求「官网登陆日志增加客户端登陆日志」）：
  // 客户端与控制台共用同一批登录端点，登录日志里必须能分辨这条记录是
  // KunClient 还是官网控制台产生的。取值 = loginAppNameOf() 的展示名
  // （'KunClient' / '官网控制台'），历史行为空串，前端按 '-' 渲染。
  // 与 device_info 同款「建列暂不主动写」的反面：这列**每登一笔就写**。
  try { db.run("ALTER TABLE login_logs ADD COLUMN login_app TEXT DEFAULT ''"); } catch {}
  // 登录双重验证（2026-10-15 用户要求「增加客户端控制台登陆双重验证功能」）：
  // 0=关闭（缺省，存量账号不受影响） / 1=开启。开启后密码登录与卡密登录
  // 必须先过邮箱验证码第二因子（邮箱验证码登录本身已证明邮箱所有权，豁免）。
  // 开/关都必须走邮箱验证码（见 /api/v2/user/2fa），防止会话被盗后一刀关掉保护。
  try { db.run("ALTER TABLE users ADD COLUMN two_factor_enabled INTEGER DEFAULT 0"); } catch {}

  // Seed default settings
  const seedSetting = (key, value) => {
    const r = db.exec("SELECT key FROM app_settings WHERE key = ?", [key]);
    if (!r.length || !r[0].values.length) db.run("INSERT INTO app_settings (key, value) VALUES (?, ?)", [key, value]);
  };
  seedSetting('login_mail_notify', '1');
  seedSetting('version', '1.0.1');
  seedSetting('download_url', `${cfg.publicUrl}/assets/KunClient.exe`);
  seedSetting('build_date', '2026-09-13');
  seedSetting('changelog', '1. 全新 Fluent UI 界面\n2. EDL 页面重构与性能优化\n3. 用户信息移至设置\n4. 多项问题修复与体验优化');
  seedSetting('force_update', '1');
  // 版本硬闸门底线（空 = 不启用）。置为某个版本号即可**关闭以往版本**：
  // 低于它的客户端所有业务请求一律 426 VERSION_TOO_OLD，旧版彻底进不来。
  // 由控制台「系统设置」维护（键名已进 ADMIN_SETTING_KEYS）。
  seedSetting('min_client_version', '');
  // 安装包防盗链档位（2026-10-12「更新鉴权」）：off | ua | strict，默认 off。
  // off 是有意为之 —— 官网静态链接 / 旧客户端直连 / CF 缓存三条链路都吃 /assets 直链。
  seedSetting('download_guard', 'off');
  // 未登录环境检测的处置参数（2026-10-09）：
  //   env_ban_ip_hours —— 未登录时按 IP 封禁的小时数（有意比账号封禁短，NAT 出口一封一片）
  //   dev_alert_email  —— 告警收件人；空则回落到 admin_email
  seedSetting('env_ban_ip_hours', '24');
  // 客户端上传日志的保留天数（控制台「系统设置」可改）；启动 + 每 6 小时清理一次。
  seedSetting('client_log_retention_days', '30');
  seedSetting('dev_alert_email', '');
  // 环境检测的全部阈值（2026-10-09 暴露到控制台）。
  // 🔴 必须 seed：ADMIN_SETTING_KEYS 读的是 getSetting(k, '')，db 里没有该键时回**空串**，
  //    控制台会显示空白、用户一保存就把 0 写进去（等于把阈值/开关全关掉）。
  //    这些键此前只有代码里的默认值，db 里从来没有 —— 是看不见的坑。
  seedSetting('env_auto_ban_enabled', '1');        // 自动封禁总开关
  seedSetting('env_ban_high_days', '7');           // HIGH / 组合命中封号天数
  seedSetting('env_ban_mid_days', '3');            // MID 累积命中封号天数
  seedSetting('env_mid_strike_threshold', '3');    // MID 同自然日累积阈值
  seedSetting('env_multi_instance_threshold', '3'); // 60 秒内多开实例数阈值
  seedSetting('env_breaker_accounts', '5');        // 熔断：1 小时内同 build 异常账号数
  seedSetting('admin_email', cfg.admin_email);
  seedSetting('feedback_notify', '1');
  seedSetting('announcement', '');
  seedSetting('tool_expire', '');  // 全局工具到期日（空 = 不过期）
  // ====== 付费工具（软开关，默认关闭，保持免费使用） ======
  seedSetting('paid_mode', '0');      // '1' = 付费模式：未开通会员的账号需要卡密/会员才能用工具
  seedSetting('vip_required', '0');   // '1' = 强制会员校验（配合 paid_mode）
  seedSetting('vip_trial_days', '0'); // 新注册用户赠送的会员天数（0 = 不赠送）
  seedSetting('vip_price_note', '');  // 控制台显示的购买说明（填写后展示给用户）
  // 官网对外口径：beta=公测中(免费) / paid=正式收费(会员授权) / free=完全免费
  // 官网首页会实时拉 /api/site/info 渲染这一口径，所以改这里就够了，不用重新构建网站。
  seedSetting('site_stage', 'beta');
  // 官网下载开关：'1' = 开放下载（默认）/'0' = 收起下载区与版本号（内测 / 暂不发版期）。
  // 与 site_stage 一样由 /api/site/info 实时下发，改后官网刷新即生效，不用重新构建。
  seedSetting('site_download_enabled', '1');
  // 内测阶段：'1' = 全站取消下载（官网 + 控制台入口收起、/assets 客户端包 403、
  // 更新接口不下发 downloadUrl）。与 site_download_enabled 是**两级**开关：
  // 这一级是「全站总闸」，site_download_enabled 是「官网单项」。（2026-10-14）
  seedSetting('internal_stage', '0');
  // 云端更新（客户端自更新通道）总开关：'1' = 开放（默认）/'0' = 暂时关闭。
  // 与 internal_stage 正交（那一级管「能不能下载」，这一级管「客户端要不要自更新」）。
  // 关闭后 /api/v2/app/version 不下发下载地址并回 updateEnabled=false。（2026-10-14）
  seedSetting('cloud_update_enabled', '1');

  // Seed admin（软配置：config.json → admin 节；仅首次播种，admin.sync=true 时每次启动强制同步密码）
  const adminUser = String(cfg.admin.username || 'admin').trim() || 'admin';
  const adminPass = String(cfg.admin.password || '');
  const adminRow = db.exec("SELECT id FROM users WHERE username = ?", [adminUser]);
  if (!adminPass) {
    console.warn('[配置] 未配置管理员密码（config.json → admin.password 或环境变量 ADMIN_PASS），跳过管理员账号播种/同步');
  } else if (!adminRow.length || !adminRow[0].values.length) {
    db.run("INSERT INTO users (username, password, role, status) VALUES (?, ?, ?, ?)",
      [adminUser, bcrypt.hashSync(adminPass, 10), 'admin', 'active']);
  } else if (cfg.admin.sync) {
    db.run("UPDATE users SET password = ? WHERE username = ?", [bcrypt.hashSync(adminPass, 10), adminUser]);
  }

  // Seed developer（最高权，全局唯一）
  // config.json → developer 节：{ "username": "小坤ovo", "password": "", "sync": false }
  // 两个作用：
  //   1) 引导入口 —— 新规则下「没有任何角色能创建出开发者」（只能分配严格低于自己的角色），
  //      所以首位开发者必须从配置文件播种，否则系统会锁死在「有管理员但无开发者」。
  //   2) 授权入口 —— 该账号若已存在，会被**直接提升为 developer**，
  //      这正是「把我的小坤ovo账号设为最高权」的落地方式（幂等，每次启动都会校正）。
  const devCfg = cfg.developer || {};
  const devUser = String(devCfg.username || '').trim();
  if (!devUser) {
    console.warn('[配置] 未配置开发者账号（config.json → developer.username），跳过开发者播种');
  } else {
    const devPass = String(devCfg.password || '');
    const devRow = rowToObj(db.exec("SELECT id, role FROM users WHERE username = ?", [devUser]));
    if (!devRow) {
      if (!devPass) {
        console.warn(`[配置] 开发者账号「${devUser}」不存在，且未配置 developer.password，无法创建`);
      } else {
        db.run("INSERT INTO users (username, password, role, status) VALUES (?, ?, 'developer', 'active')",
          [devUser, bcrypt.hashSync(devPass, 10)]);
        console.log(`[配置] 已创建开发者账号「${devUser}」（最高权）`);
      }
    } else {
      if (normalizeRole(devRow.role) !== 'developer') {
        db.run("UPDATE users SET role = 'developer' WHERE id = ?", [devRow.id]);
        console.log(`[配置] 已把账号「${devUser}」提升为开发者（最高权）`);
      }
      if (devPass && devCfg.sync) {
        db.run("UPDATE users SET password = ? WHERE id = ?", [bcrypt.hashSync(devPass, 10), devRow.id]);
      }
    }
  }

  // Seed sample license keys
  const keyCount = db.exec("SELECT COUNT(*) FROM license_keys");
  if (keyCount.length && keyCount[0].values[0][0] === 0) {
    for (let i = 0; i < 5; i++) {
      db.run("INSERT INTO license_keys (key_code, status) VALUES (?, 'active')", [generateKey()]);
    }
  }

  // 兼容历史数据：老账号只有 vip_expire_at、没有 vip_level，
  // ALTER TABLE 会给它们补上 'normal'，这里把「会员仍在有效期内」的账号回填成 vip，
  // 否则升级后老付费用户会被降级。用 JS 判定（与 isVipActive 完全一致），避免日期字符串比较歧义。
  try {
    const legacy = rowsToArr(db.exec("SELECT id, vip_level, vip_expire_at FROM users"));
    let fixed = 0;
    legacy.forEach(u => {
      if (normalizeVipLevel(u.vip_level) === 'normal' && isVipActive(u.vip_expire_at)) {
        db.run("UPDATE users SET vip_level = 'vip' WHERE id = ?", [u.id]);
        fixed++;
      }
    });
    if (fixed) console.log(`[迁移] 已把 ${fixed} 个老会员账号的 vip_level 回填为 vip`);
  } catch (e) { console.warn('[迁移] vip_level 回填跳过:', e.message); }

  // 一次性合规清理（幂等，**已于历史版本执行完毕**）：更早的版本曾把客户端 IP 落库且所有管理员都能看，
  // 当时的处置是「整体移除采集 + 把已存在的值抹掉」。用 app_settings 标记保证只跑一次，
  // 避免每次启动都重写整个数据库文件。
  //
  // ⚠️ 2026-09-29 起语义**已反转**：登录 IP 重新开始采集，但**仅开发者可见**（见 /api/logs 与隐私政策）。
  //    这段清理**保留不动**（标记已是 '1'，不会再跑）—— 它的作用只是确保「历史遗留的、当时无人管控的
  //    那批 IP」已被抹掉；重新采集是之后发生的事，两者不冲突。别看到它就去删，也别把标记重置。
  if (getSetting('privacy_ip_purged', '0') !== '1') {
    try {
      db.run("UPDATE login_logs SET ip_address = '' WHERE ip_address IS NOT NULL AND ip_address <> ''");
      db.run("UPDATE usage_logs SET ip_address = '' WHERE ip_address IS NOT NULL AND ip_address <> ''");
      setSetting('privacy_ip_purged', '1');
      console.log('[Privacy] 已清除历史登录/使用日志中留存的 IP 地址');
    } catch (e) { console.warn('[Privacy] IP 清理跳过:', e.message); }
  }

  // 客户端日志上传（2026-10-12）：文件目录 + 定期清理。
  // 上传目的是修工具，陈账没有保留价值。清理必须**定期跑**（不能只在启动时跑一次）：
  // pm2 的进程可以连着跑几个月不重启，只挂启动钩子等于永远不清理。
  // 保留天数从 app_settings 读（控制台「系统设置」可调，默认 30 天）。
  // 🔴 目录必须放在 **express.static 不可达**的位置（2026-10-12 修）。
  // 一开始写成 assets/logs/ —— 而 `/assets` 是 `express.static` 整目录映射，
  // 日志文件名又是可枚举的 client-log-{id}.json，于是
  // https://kunawa.cn/assets/logs/client-log-1.json 公网任何人都能直接下载。
  // 「仅开发者可见」会被这一行静态映射彻底架空（接口鉴权做再好也没用）。
  // 现与 database.sqlite 同级（__dirname 根下），三个 static 映射（website/admin/console）
  // 与 /assets 都覆盖不到这里。
  const CLIENT_LOG_DIR = path.join(__dirname, 'client-logs');
  function pruneClientLogs(reason) {
    try {
      fs.mkdirSync(CLIENT_LOG_DIR, { recursive: true, mode: 0o700 });
      const days = Math.max(1, parseInt(getSetting('client_log_retention_days', '30'), 10) || 30);
      const cutoff = `-${days} days`;
      // 先删文件再删行：反过来的话中途失败会留下「有行无文件」的孤儿记录，
      // 控制台点开就是 404，反而更难排查。
      let removed = 0;
      for (const row of rowsToArr(db.exec(
        "SELECT id, file FROM client_logs WHERE datetime(created_at) < datetime('now', ?)", [cutoff]))) {
        if (row.file) { try { fs.unlinkSync(path.join(CLIENT_LOG_DIR, row.file)); } catch {} }
        db.run("DELETE FROM client_logs WHERE id = ?", [row.id]);
        removed++;
      }
      // 兜底：目录里没有对应行的孤儿文件（手工删库/中途失败留下）一并清掉，
      // 否则 client-logs/ 会一直涨。只认本服务生成的文件名，不误删别人的东西。
      const known = new Set(rowsToArr(db.exec("SELECT file FROM client_logs"))
        .map(r => String(r.file || '')).filter(Boolean));
      let orphans = 0;
      for (const f of fs.readdirSync(CLIENT_LOG_DIR)) {
        if (!/^client-log-\d+\.json$/.test(f) || known.has(f)) continue;
        const p = path.join(CLIENT_LOG_DIR, f);
        let old = false;
        try { old = fs.statSync(p).mtimeMs < Date.now() - days * 24 * 3600 * 1000; } catch {}
        if (!old) continue;
        try { fs.unlinkSync(p); orphans++; } catch {}
      }
      if (removed || orphans) {
        saveDb();
        console.log(`[ClientLogs] ${reason}：清理过期日志 ${removed} 份、孤儿文件 ${orphans} 个（保留 ${days} 天）`);
      }
    } catch (e) { console.warn('[ClientLogs] 清理跳过:', e.message); }
  }
  pruneClientLogs('启动');
  // 每 6 小时扫一次。unref：定时器不阻止进程退出（pm2 停服务时要能干净退出）。
  setInterval(() => pruneClientLogs('定期'), 6 * 3600 * 1000).unref();

  saveDb();

  // ── 版本硬闸门挂载（必须在所有客户端路由**之前**注册）──────────────────
  // Express 按注册顺序执行中间件：挂到路由后面 = 等于没挂（请求已经进了业务处理器）。
  for (const p of CLIENT_GATED_PATHS) app.use(p, clientVersionGate);

  // ── v2 请求签名守卫：版本闸门之后、业务路由之前（2026-10-11）──────────
  // 🔴 不能写成 app.use('/api/v2', sigGuard)：挂载带前缀时 req.path 是相对路径
  //    （'/login' 而非 '/api/v2/login'），SIG_REQUIRED_PATHS 全量失配 = 守卫形同虚设
  //    （本地实测踩过：/api/v2/login 无签名头照常进入业务处理器）。必须无前缀挂载。
  app.use(sigGuard);

  // ====== v2 签名握手（公开端点，自身在 SIG_REQUIRED_PATHS 之外） ======
  // 限流：30 次/小时/IP（handshakeLimiter）。GET：返回服务器时间帮客户端对表（NTP 不可用时兜底）。
  app.post('/api/v2/client/handshake', (req, res) => {
    if (!handshakeLimiter(clientIpOf(req))) return rateLimited(res, 3600);
    const s = sigIssueSession(clientIpOf(req));
    res.json({ success: true, data: { sid: s.sid, secret: s.secret, expiresIn: s.expires, serverTime: Date.now() } });
  });
  app.get('/api/v2/client/handshake', (req, res) => {
    res.json({ success: true, data: { serverTime: Date.now() } });
  });

  // ====== Auth: Username + Password ======
  app.post('/api/v2/login', async (req, res) => {
    const { username, password } = req.body;
    // E6（2026-10-07）：参数校验上移到限流**之前** —— 缺字段的非法请求不该消耗
    // 防撞库额度，否则脚本刷一批空请求就能把正常用户的登录额度挤掉。
    if (!username || !password) return res.status(400).json({ success: false, message: '请输入用户名和密码' });
    // 防撞库：按 IP 与 (IP+账号) 双维度限流（2026-10-01）
    const ip = clientIpOf(req);
    const loginUserKey = ip + '|' + String(username || '');
    if (!loginIpLimiter(ip)) return loginRateLimited(res);
    if (!loginUserLimiter(loginUserKey)) { loginIpLimiter.refund(ip); return loginRateLimited(res); }

    const r = db.exec("SELECT * FROM users WHERE username = ?", [username]);
    if (!r.length || !r[0].values.length) return res.status(401).json({ success: false, message: '用户名或密码错误' });

    const user = rowToObj(r);
    // 封禁拦截（含自动时限封禁）：必须带上**到期时间**与**申诉进展** ——
    // 让人「登不上、不知道为什么、不知道何时恢复、不知道能找谁」是最糟的体验。
    const ban0 = activeBanOf(user);
    if (ban0) {
      const ap = appealOf(user.id);
      return res.status(403).json({
        success: false, code: 'BANNED',
        message: banMessageOf(ban0, ap),
        until: ban0.until, permanent: ban0.permanent, appeal: ap,
      });
    }
    if (!bcrypt.compareSync(password, user.password)) {
      db.run("INSERT INTO login_logs (user_id, username, status, ip_address, login_app) VALUES (?, ?, 'failed', ?, ?)",
        [user.id, username, clientIpOf(req), loginAppNameOf(req)]);
      saveDb();
      return res.status(401).json({ success: false, message: '用户名或密码错误' });
    }

    // E5：登录**成功**后把这次消耗的额度退回去。合法登录不是威胁行为，计数只该留给
    // 失败尝试（撞库靠失败次数识别）——不退的话，NAT 出口下正常用户 10 分钟 20 次
    // 用完就 429，这正是「登录有时失败」的成因之一。
    // ⚠️ 放在 2FA 分支**之前**：第一因子（密码）已经验证通过，该退的额度与第二因子无关。
    loginIpLimiter.refund(ip);
    loginUserLimiter.refund(loginUserKey);

    // 双重验证（2026-10-15）：开启后密码登录必须先过邮箱验证码第二因子。
    // 挑战阶段不写 success 日志、不动 last_login、不发登录通知 —— 那些都等
    // /auth/2fa/verify 真正签发 token 时才落（见 issueTwoFactorChallenge 注释）。
    if (user.two_factor_enabled === 1) {
      let ch;
      try { ch = await issueTwoFactorChallenge(user, req, req.body && req.body.app, '密码'); }
      catch (e) { console.error('[2FA] 挑战签发失败:', e.message); return res.status(500).json({ success: false, message: '服务器繁忙，请稍后重试' }); }
      if (!ch.ok) return res.status(ch.status || 500).json({ success: false, message: ch.message });
      return res.status(403).json({
        success: false, code: '2FA_REQUIRED',
        message: '账号已开启登录双重验证，验证码已发送至 ' + ch.maskedEmail,
        data: { ticket: ch.ticket, maskedEmail: ch.maskedEmail },
      });
    }

    db.run("UPDATE users SET last_login = datetime('now') WHERE id = ?", [user.id]);
    db.run("INSERT INTO login_logs (user_id, username, status, ip_address, login_app) VALUES (?, ?, 'success', ?, ?)",
      [user.id, username, clientIpOf(req), loginAppNameOf(req)]);
    saveDb();

    // 必须挂 .catch：这是 async 函数，Node 15+ 对未处理的 rejection 默认直接终止进程。
    // 登录通知只是副产品，绝不能因为发信异常把整个服务打挂。
    req._loginVia = '密码';
    req._loginApp = loginAppNameOf(req);
    sendLoginNotification(user, req).catch(e => console.error('[LoginMail] 登录通知异常:', e.message));

    const token = signToken({ userId: user.id, username: user.username, role: user.role });
    res.json({ success: true, message: '登录成功', data: { token, username: user.username, role: user.role, signature: user.signature || '', expiresIn: 86400, entitlement: entitlementOf(user) } });
  });

  // ====== Auth: Register ======
  app.post('/api/v2/register', (req, res) => {
    const { username, password } = req.body;
    if (!registerLimiter(clientIpOf(req))) return rateLimited(res);   // 防批量注册（2026-10-01）
    if (!username || !password) return res.status(400).json({ success: false, message: '请输入用户名和密码' });
    // 法务合规（2026-10-02 与 code-register 对齐）：必须先同意条款，服务端独立拦一次。
    // 兼容老客户端：带 agree 字段就必须为真；完全不带字段（旧版本客户端）放行。
    if (req.body.agree !== undefined || req.body.termsAccepted !== undefined) {
      const agree = req.body.agree === true || req.body.agree === 'true' || req.body.agree === 1
        || req.body.termsAccepted === true;
      if (!agree) return res.status(400).json({ success: false, message: '请先阅读并同意' + LEGAL_DOCS });
    }
    const uErr = usernameError(username);
    if (uErr) return res.status(400).json({ success: false, message: uErr });
    if (password.length < 6) return res.status(400).json({ success: false, message: '密码至少6个字符' });

    try {
      // 同 code-register：存下同意时间与条款版本（用户没显式同意过则记 NULL，由法律口径兜底）
      const cb = req.body.crossBorderConsent;
      db.run("INSERT INTO users (username, password, terms_accepted_at, legal_version, cross_border_consent_at) VALUES (?, ?, ?, ?, ?)",
        [username, bcrypt.hashSync(password, 10),
         (req.body.agree === true || req.body.agree === 'true' || req.body.agree === 1 || req.body.termsAccepted === true)
           ? new Date().toISOString() : null,
         LEGAL_VERSION,
         (cb === true || cb === 'true' || cb === 1) ? new Date().toISOString() : null]);
      saveDb();
      const user = rowToObj(db.exec("SELECT * FROM users WHERE username = ?", [username]));
      applyTrialDays(user);   // 付费模式下的新用户试用天数（vip_trial_days，默认 0 = 不赠送）
      const token = signToken({ userId: user.id, username: user.username, role: user.role });
      res.json({ success: true, message: '注册成功', data: { token, username: user.username, role: user.role, expiresIn: 86400, entitlement: entitlementOf(rowToObj(db.exec("SELECT * FROM users WHERE id = ?", [user.id]))) } });
    } catch (e) {
      if (String(e).includes('UNIQUE')) return res.status(400).json({ success: false, message: '用户名已存在' });
      res.status(500).json({ success: false, message: '注册失败' });
    }
  });

  // ====== Auth: License Key Login ======
  app.post('/api/v2/auth/key-login', async (req, res) => {
    const { key } = req.body;
    if (!keyLoginLimiter(clientIpOf(req))) return loginRateLimited(res);
    if (!key) return res.status(400).json({ success: false, message: '请输入密钥' });

    const normalizedKey = key.trim().toUpperCase();
    const keyRow = rowToObj(db.exec("SELECT * FROM license_keys WHERE key_code = ?", [normalizedKey]));
    // v2 安全口径（防枚举）：无效 / 已使用 统一同一句 401 —— 泄漏「这个密钥存在但已被用」
    // 等于给爆破脚本发确认回执。禁用/过期的 403 同理收敛进来（见下）。
    if (!keyRow) return res.status(401).json({ success: false, code: 'KEY_INVALID', message: '密钥无效或已失效' });
    if (keyRow.status === 'used') return res.status(401).json({ success: false, code: 'KEY_INVALID', message: '密钥无效或已失效' });
    if (keyRow.status !== 'active') return res.status(401).json({ success: false, code: 'KEY_INVALID', message: '密钥无效或已失效' });
    if (keyRow.expires_at && new Date(keyRow.expires_at) < new Date()) {
      return res.status(401).json({ success: false, code: 'KEY_INVALID', message: '密钥无效或已失效' });
    }

    let user;
    if (keyRow.user_id) {
      user = rowToObj(db.exec("SELECT * FROM users WHERE id = ?", [keyRow.user_id]));
    }

    if (!user) {
      // Auto-create user from key
      const autoUsername = 'user_' + normalizedKey.slice(-8).toLowerCase();
      try {
        const autoPassword = crypto.randomBytes(8).toString('hex');
        db.run("INSERT INTO users (username, password) VALUES (?, ?)", [autoUsername, bcrypt.hashSync(autoPassword, 10)]);
        user = rowToObj(db.exec("SELECT * FROM users WHERE username = ?", [autoUsername]));
        db.run("UPDATE license_keys SET user_id = ? WHERE id = ?", [user.id, keyRow.id]);
      } catch {
        // If username exists, find existing
        user = rowToObj(db.exec("SELECT * FROM users WHERE username = ?", [autoUsername]));
        if (user) db.run("UPDATE license_keys SET user_id = ? WHERE id = ?", [user.id, keyRow.id]);
      }
    }

    if (!user) return res.status(500).json({ success: false, message: '密钥登录失败' });
    // 2026-10-02 修复：原先只拦 status==='banned'（人工永久封禁），时限封禁的账号
    // 用卡密就能换到全新 token，绕开 /api/login 的 activeBanOf 口径。现对齐密码登录。
    const banKey = activeBanOf(user);
    if (banKey) {
      const ap = appealOf(user.id);
      return res.status(403).json({
        success: false, code: 'BANNED',
        message: banMessageOf(banKey, ap),
        until: banKey.until, permanent: banKey.permanent, appeal: ap,
      });
    }

    // 双重验证（2026-10-15）：卡密登录同样要过邮箱验证码第二因子。
    // ⚠️ 卡密的消费（标记 used / 加会员天数）刻意**推迟到 2FA 二次校验通过**
    //    （票据带 keyId，消费动作在 /auth/2fa/verify 里）：挑战阶段就消费的话，
    //    验证码邮件迟到 → 用户拿同一把卡密重试 → 命中「密钥无效或已失效」，
    //    一把根本没消费成功的卡密就烧掉了，且没有任何提示说明原因。
    if (user.two_factor_enabled === 1) {
      let ch;
      try { ch = await issueTwoFactorChallenge(user, req, req.body && req.body.app, '卡密', { keyId: keyRow.id }); }
      catch (e) { console.error('[2FA] 挑战签发失败:', e.message); return res.status(500).json({ success: false, message: '服务器繁忙，请稍后重试' }); }
      if (!ch.ok) return res.status(ch.status || 500).json({ success: false, message: ch.message });
      return res.status(403).json({
        success: false, code: '2FA_REQUIRED',
        message: '账号已开启登录双重验证，验证码已发送至 ' + ch.maskedEmail,
        data: { ticket: ch.ticket, maskedEmail: ch.maskedEmail },
      });
    }

    // 消费卡密：标记 used 留档（一次性），带天数时自动为账号叠加会员时长
    const grantDays = parseInt(keyRow.days) || 0;
    db.run("UPDATE license_keys SET status = 'used', user_id = ? WHERE id = ?", [user.id, keyRow.id]);
    if (grantDays > 0) addVipDays(user.id, grantDays);
    applyKeyEntitlement(user.id, keyRow, grantDays);
    db.run("UPDATE users SET last_login = datetime('now') WHERE id = ?", [user.id]);
    db.run("INSERT INTO login_logs (user_id, username, status, ip_address, login_app) VALUES (?, ?, 'success', ?, ?)",
      [user.id, user.username, clientIpOf(req), loginAppNameOf(req)]);
    saveDb();

    const fresh = rowToObj(db.exec("SELECT * FROM users WHERE id = ?", [user.id]));

    // 必须挂 .catch：这是 async 函数，Node 15+ 对未处理的 rejection 默认直接终止进程。
    // 登录通知只是副产品，绝不能因为发信异常把整个服务打挂。
    req._loginVia = '卡密';
    req._loginApp = loginAppNameOf(req);
    sendLoginNotification(user, req).catch(e => console.error('[LoginMail] 登录通知异常:', e.message));

    const token = signToken({ userId: user.id, username: user.username, role: user.role });
    const grantMsg = grantDays > 0 ? `，已自动开通 ${grantDays} 天会员` : '';
    res.json({ success: true, message: `密钥登录成功${grantMsg}`, data: { token, username: user.username, role: user.role, signature: user.signature || '', expiresIn: 86400, grantedDays: grantDays, entitlement: entitlementOf(fresh) } });
  });

  // ====== Auth: 兑换卡密（已登录用户用卡密给自己的账号加会员时长） ======

  // ====== 客户端：授权状态查询（付费模式下客户端据此放行/拦截功能） ======

  // ====== Auth: Generate License Key（2026-10-02 收权）======
  // 原来完全公开：任何人 POST 一下就能铸造卡密并 saveDb()（整库重写），
  // 等于「无限铸币机 + 写放大入口」。实际调用方排查：客户端/控制台都不用它
  // （客户端登录走 /api/user/generate-key，用户中心自带 20 个上限护栏），
  // 仅 tools/selftest-api.js 本地自测在调 —— 改为管理员及以上，不影响任何存量功能。
  app.post('/api/v2/auth/generate-key', authMiddleware, adminMiddleware, (req, res) => {
    try {
      const keyCode = generateKey();
      db.run("INSERT INTO license_keys (key_code, status) VALUES (?, 'active')", [keyCode]);
      saveDb();
      res.json({ success: true, data: { key: keyCode } });
    } catch (e) {
      res.status(500).json({ success: false, message: '生成密钥失败' });
    }
  });

  // ====== User: Generate License Key (Auth, for user center) ======
  // 2026-09-29：用户反馈「官网用户无法生成一次性密钥」。接口一直都在、自测也一直过 ——
  // 真正的原因是**控制台「我的密钥」页没有生成按钮**（只有刷新/删除），空列表还提示
  // 「可在客户端或联系管理员获取」，而客户端根本没有生成入口。按钮补在
  // console/index.html 的 genKey()；这里只补一道上限护栏。
  app.post('/api/v2/user/generate-key', authMiddleware, (req, res) => {
    try {
      // 🔴 上限护栏：密钥是「一次性凭证」，正常用户也就几台设备用得上。加按钮之后任何人
      //    都能在页面上反复点，而每次生成都要 saveDb()（sql.js 把**整个库**重写一遍）——
      //    不设上限就是一个廉价的写放大入口。20 个远高于真实需要；到了上限明确告诉他去清理，
      //    而不是静默失败或报一句看不懂的错。
      const MAX_ACTIVE_KEYS = 20;
      const cnt = rowToObj(db.exec(
        "SELECT COUNT(*) AS n FROM license_keys WHERE user_id = ? AND status = 'active'",
        [req.user.userId]));
      const activeN = Number((cnt && cnt.n) || 0);
      if (activeN >= MAX_ACTIVE_KEYS) {
        return res.status(429).json({
          success: false,
          message: `有效密钥已达上限（${MAX_ACTIVE_KEYS} 个），请先停用或删除不用的密钥再生成`,
        });
      }

      const keyCode = generateKey();
      db.run("INSERT INTO license_keys (key_code, status, user_id) VALUES (?, 'active', ?)", [keyCode, req.user.userId]);
      saveDb();
      res.json({ success: true, data: { key: keyCode } });
    } catch (e) {
      res.status(500).json({ success: false, message: '生成密钥失败' });
    }
  });

  // ====== Auth: Send Verification Code ======
  // ⚠️ 前端历史上混用了两个字段名：客户端传 type，控制台传 scene。
  // 之前服务端只读 type，控制台调用时 type=undefined → 写 verification_codes
  // （type 列是 NOT NULL）直接抛错，接口 500 且邮件根本没发出去。
  // 这里两个字段都收，统一成 type；并对整个流程加 try/catch 与真实发送结果回执。
  app.post('/api/v2/auth/send-code', async (req, res) => {
    try {
      if (!codeLimiter(clientIpOf(req))) return rateLimited(res);   // 防刷验证码（2026-10-01）
      const email = String(req.body.email || '').trim();
      const rawType = String(req.body.type || req.body.scene || '').trim().toLowerCase();
      // login=登录 / register=注册 / reset=忘记密码找回；缺省按注册处理
      const type = ['login', 'register', 'reset'].includes(rawType) ? rawType : 'register';

      if (!email || !/^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(email))
        return res.status(400).json({ success: false, message: '请输入有效的邮箱地址' });

      // Rate limit: max 1 code per email per 60s
      const recent = rowToObj(db.exec(
        "SELECT id FROM verification_codes WHERE email = ? AND created_at > datetime('now', '-60 seconds')",
        [email]));
      if (recent) return res.status(429).json({ success: false, message: '请60秒后再试' });

      // 每邮箱每小时硬上限（2026-10-02）：60s 一条的旧限制仍允许 60 条/小时的邮件轰炸。
      // 10 条/小时远高于真人重试需要，配合 IP 级 codeLimiter 双保险。
      const hourly = rowToObj(db.exec(
        "SELECT COUNT(*) AS n FROM verification_codes WHERE email = ? AND created_at > datetime('now', '-1 hour')",
        [email]));
      if (Number((hourly && hourly.n) || 0) >= 10)
        return res.status(429).json({ success: false, message: '该邮箱验证码发送过于频繁，请1小时后再试' });

      // Check if email already registered
      const exists = rowToObj(db.exec("SELECT id FROM users WHERE email = ?", [email]));
      if (type === 'register' && exists) return res.status(400).json({ success: false, message: '该邮箱已注册' });
      // 登录 / 找回密码都要求邮箱已注册
      if ((type === 'login' || type === 'reset') && !exists) return res.status(400).json({ success: false, message: '该邮箱未注册' });
      if (type === 'login' && exists) {
        const user = rowToObj(db.exec("SELECT email_login_enabled FROM users WHERE email = ?", [email]));
        if (user && user.email_login_enabled === 0) return res.status(403).json({ success: false, message: '该用户已关闭邮箱登录' });
      }

      const code = generateCode();
      const expiresAt = new Date(Date.now() + 10 * 60 * 1000).toISOString(); // 10 minutes

      db.run("DELETE FROM verification_codes WHERE email = ? AND type = ?", [email, type]);
      db.run("INSERT INTO verification_codes (email, code, type, expires_at) VALUES (?, ?, ?, ?)", [email, code, type, expiresAt]);
      saveDb();

      // 邮件发送失败必须如实回执：以前忽略返回值、永远回 success:true，
      // 用户收不到码却看到「已发送」，无法判断问题出在哪。
      // 「邮件服务未启用」单独给 503 + 明确文案：这是 config.json 的配置问题，
      // 和「SMTP 拒绝/网络不通」是两回事，混在一起用户根本无从排查。
      if (!mailTransporter) {
        db.run("DELETE FROM verification_codes WHERE email = ? AND type = ?", [email, type]);
        saveDb();
        return res.status(503).json({
          success: false,
          message: '本站邮件服务未启用，无法发送验证码（请在服务端 config.json 中开启 mail.enabled 并填写 SMTP 账号）'
        });
      }

      const sent = await sendEmail(email, code, 10);
      if (!sent) {
        // 发送失败就把刚写入的验证码删掉：否则 60 秒频率限制会生效，
        // 用户立刻重试只会看到「请60秒后再试」，误以为是自己操作太快。
        db.run("DELETE FROM verification_codes WHERE email = ? AND type = ?", [email, type]);
        saveDb();
        return res.status(502).json({ success: false, message: '验证码发送失败，请稍后重试或联系管理员' });
      }

      res.json({ success: true, message: '验证码已发送，若未收到请检查垃圾邮件', data: { expiresIn: 600 } });
    } catch (e) {
      console.error('[Auth] send-code 失败:', e.message);
      res.status(500).json({ success: false, message: '服务器繁忙，请稍后重试' });
    }
  });

  // ====== Auth: Reset Password（自助找回密码：邮箱验证码 → 设新密码） ======
  app.post('/api/v2/auth/reset-password', (req, res) => {
    try {
      const email = String(req.body.email || '').trim();
      const code = String(req.body.code || '').trim();
      const newPassword = String(req.body.newPassword || '');
      if (!email || !code || !newPassword)
        return res.status(400).json({ success: false, message: '请填写邮箱、验证码和新密码' });
      if (newPassword.length < 6)
        return res.status(400).json({ success: false, message: '密码至少 6 个字符' });

      const user = rowToObj(db.exec("SELECT * FROM users WHERE email = ?", [email]));
      if (!user) return res.status(404).json({ success: false, message: '该邮箱未注册' });
      if (user.status === 'banned') return res.status(403).json({ success: false, message: '账号已被封禁' });

      // 统一用 datetime() 归一化后比较：verification_codes.expires_at 目前存 ISO（带 T），
      // 若哪天写入格式变成 "YYYY-MM-DD HH:mm:ss"，裸字符串比较会因 'T'(0x54) > ' '(0x20)
      // 而恒判定为「未过期」→ 过期验证码可被无限次使用。datetime() 对两种格式都正确。
      const ck = fetchValidCode(email, 'reset', code);
      if (ck.locked) return res.status(429).json({ success: false, message: CODE_LOCKED_MSG });
      if (!ck.row) return res.status(401).json({ success: false, message: '验证码无效或已过期' });
      const codeRow = ck.row;

      codeFailClear(email, 'reset');
      db.run("DELETE FROM verification_codes WHERE email = ? AND type = 'reset'", [email]);
      // 2026-10-02：改密必须递增 session_epoch —— 密码被重置说明旧凭证可能已泄漏，
      // 用旧 token 登录的会话（可能是攻击者）必须在下次请求时被踢下线。
      db.run(
        "UPDATE users SET password = ?, session_epoch = ? WHERE id = ?",
        [bcrypt.hashSync(newPassword, 10),
         Math.max(Math.floor(Date.now() / 1000), Number(user.session_epoch || 0) + 1),
         user.id]);
      saveDb();
      // 安全警报：密码被改必须让账号主人知道（尤其「不是我改的」那种情况）
      sendAccountMail('password_changed', user, {}).catch(() => {});
      res.json({ success: true, message: '密码已重置，请用新密码登录' });
    } catch (e) {
      console.error('[Auth] reset-password 失败:', e.message);
      res.status(500).json({ success: false, message: '服务器繁忙，请稍后重试' });
    }
  });

  // ====== Auth: Verify Code Login ======
  app.post('/api/v2/auth/code-login', (req, res) => {
    const { email, code } = req.body;
    if (!email || !code) return res.status(400).json({ success: false, message: '请输入邮箱和验证码' });

    const ck = fetchValidCode(email, 'login', code);
    if (ck.locked) return res.status(429).json({ success: false, message: CODE_LOCKED_MSG });
    if (!ck.row) return res.status(401).json({ success: false, message: '验证码无效或已过期' });

    const user = rowToObj(db.exec("SELECT * FROM users WHERE email = ?", [email]));
    if (!user) return res.status(401).json({ success: false, message: '用户不存在' });
    // 2026-10-02 修复：与 key-login 同因 —— 只拦 status==='banned' 会放行时限封禁账号，
    // 用邮箱验证码就能换到新 token。对齐 /api/login 的 activeBanOf 口径。
    const banCode = activeBanOf(user);
    if (banCode) {
      const ap = appealOf(user.id);
      return res.status(403).json({
        success: false, code: 'BANNED',
        message: banMessageOf(banCode, ap),
        until: banCode.until, permanent: banCode.permanent, appeal: ap,
      });
    }
    if (user.email_login_enabled === 0) return res.status(403).json({ success: false, message: '该用户已关闭邮箱登录' });

    codeFailClear(email, 'login');
    db.run("DELETE FROM verification_codes WHERE email = ? AND type = 'login'", [email]);
    db.run("UPDATE users SET last_login = datetime('now') WHERE id = ?", [user.id]);
    db.run("INSERT INTO login_logs (user_id, username, status, ip_address, login_app) VALUES (?, ?, 'success', ?, ?)",
      [user.id, user.username, clientIpOf(req), loginAppNameOf(req)]);
    saveDb();

    // 必须挂 .catch：这是 async 函数，Node 15+ 对未处理的 rejection 默认直接终止进程。
    // 登录通知只是副产品，绝不能因为发信异常把整个服务打挂。
    req._loginVia = '邮箱验证码';
    req._loginApp = loginAppNameOf(req);
    sendLoginNotification(user, req).catch(e => console.error('[LoginMail] 登录通知异常:', e.message));

    const token = signToken({ userId: user.id, username: user.username, role: user.role });
    res.json({ success: true, message: '登录成功', data: { token, username: user.username, role: user.role, signature: user.signature || '', expiresIn: 86400, entitlement: entitlementOf(user) } });
  });

  // ====== Auth: Verify Code Register ======
  app.post('/api/v2/auth/code-register', (req, res) => {
    const { email, code, username, password } = req.body;
    if (!email || !code || !username || !password)
      return res.status(400).json({ success: false, message: '请填写完整信息' });
    // 法务合规：必须先在界面勾选同意，服务端再独立拦一次
    // （前端勾选框可被绕过，这条校验才是真正的门槛）。
    const agree = req.body.agree === true || req.body.agree === 'true' || req.body.agree === 1
      || req.body.termsAccepted === true;
    if (!agree)
      return res.status(400).json({ success: false, message: '请先阅读并同意' + LEGAL_DOCS });
    if (!/^[a-zA-Z0-9_]{3,20}$/.test(username))
      return res.status(400).json({ success: false, message: '用户名需3-20个字符' });
    // 与 /api/register 同一套规则：纯数字名不许用。
    // ⚠️ 这里不能直接换成 usernameError()：本接口的正则是「3-20 位纯 ASCII」，
    //    而 USERNAME_RE 是「2-24 位含中文」—— 换了会**顺带放开**中文名，属于计划外的行为变更。
    //    所以只叠加这条新规则，其余保持原样。
    if (isAllDigitsUsername(username))
      return res.status(400).json({ success: false, message: USERNAME_ALL_DIGITS_MSG });
    if (password.length < 6)
      return res.status(400).json({ success: false, message: '密码至少6个字符' });

    const ck = fetchValidCode(email, 'register', code);
    if (ck.locked) return res.status(429).json({ success: false, message: CODE_LOCKED_MSG });
    if (!ck.row) return res.status(401).json({ success: false, message: '验证码无效或已过期' });

    try {
      // 存下同意时间与条款版本；客户端与旧控制台不传 legalVersion 时按当前版本记（服务端为准）。
      const legalVersion = parseInt(req.body.legalVersion, 10);
      // 跨境单独同意：显式 false 直接拒；不传（老客户端）记 NULL，不伪造同意记录。
      const cbRaw = req.body.crossBorderConsent;
      if (cbRaw === false || cbRaw === 'false' || cbRaw === 0)
        return res.status(400).json({ success: false, message: '需单独同意个人信息出境后才能注册' });
      const crossBorderAt = (cbRaw === true || cbRaw === 'true' || cbRaw === 1) ? new Date().toISOString() : null;
      db.run("INSERT INTO users (username, password, email, terms_accepted_at, legal_version, cross_border_consent_at) VALUES (?, ?, ?, ?, ?, ?)",
        [username, bcrypt.hashSync(password, 10), email, new Date().toISOString(),
         Number.isFinite(legalVersion) && legalVersion > 0 ? legalVersion : LEGAL_VERSION, crossBorderAt]);
      codeFailClear(email, 'register');
      db.run("DELETE FROM verification_codes WHERE email = ? AND type = 'register'", [email]);
      saveDb();

      const user = rowToObj(db.exec("SELECT * FROM users WHERE username = ?", [username]));
      const token = signToken({ userId: user.id, username: user.username, role: user.role });
      res.json({ success: true, message: '注册成功', data: { token, username: user.username, role: user.role, signature: '', expiresIn: 86400, entitlement: entitlementOf(user) } });
    } catch (e) {
      if (String(e).includes('UNIQUE')) return res.status(400).json({ success: false, message: '用户名已存在' });
      res.status(500).json({ success: false, message: '注册失败' });
    }
  });

  // ====== Auth: Bind Email ======
  app.post('/api/v2/user/bind-email', authMiddleware, (req, res) => {
    const { email, code } = req.body;
    if (!email || !code) return res.status(400).json({ success: false, message: '请输入邮箱和验证码' });

    const ck = fetchValidCode(email, 'bind', code);
    if (ck.locked) return res.status(429).json({ success: false, message: CODE_LOCKED_MSG });
    if (!ck.row) return res.status(401).json({ success: false, message: '验证码无效或已过期' });
    const codeRow = ck.row;

    const exists = rowToObj(db.exec("SELECT id FROM users WHERE email = ? AND id != ?", [email, req.user.userId]));
    if (exists) return res.status(400).json({ success: false, message: '该邮箱已被其他账号绑定' });

    db.run("UPDATE users SET email = ? WHERE id = ?", [email, req.user.userId]);
    codeFailClear(email, 'bind');
    db.run("DELETE FROM verification_codes WHERE email = ? AND type = 'bind'", [email]);
    saveDb();
    sendAccountMail('email_bound',
      { id: req.user.userId, username: req.user.username, email },
      { email }).catch(() => {});
    res.json({ success: true, message: '邮箱绑定成功' });
  });

  // ⚠️ 必须是 async + await sendEmail，且如实回执发送结果。
  // 原先写成 `sendEmail(...); res.json({success:true,'验证码已发送'})` —— 同步处理器里
  // 不 await，邮件发没发出去都不影响响应，SMTP 挂了用户也只会看到「已发送」，
  // 然后一直等一个永远不来的验证码，无从排查。（与 /api/auth/send-code 的口径保持一致）
  app.post('/api/v2/user/send-bind-code', authMiddleware, async (req, res) => {
    // 2026-10-02 修复：补齐与 /api/auth/send-code 同款的防轰炸双保险（IP 级限流 + 每邮箱
    // 每小时硬上限）—— 原先只有 60 秒一条，理论上一天能对同一邮箱轰 1440 封绑定邮件。
    // 同时整个处理器包上 try/catch：Express 4 不会捕获 async 处理器抛出的异常，
    // 任何一个同步 db.exec 抛错都会变成 unhandled rejection 把整个进程带走。
    try {
      if (!codeLimiter(clientIpOf(req))) return rateLimited(res);
      const { email } = req.body;
      if (!email || !/^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(email))
        return res.status(400).json({ success: false, message: '请输入有效的邮箱地址' });

      const recent = rowToObj(db.exec(
        "SELECT id FROM verification_codes WHERE email = ? AND type = 'bind' AND created_at > datetime('now', '-60 seconds')",
        [email]));
      if (recent) return res.status(429).json({ success: false, message: '请60秒后再试' });

      // 每邮箱每小时硬上限（与 send-code 同口径：10 条/小时）
      const hourly = rowToObj(db.exec(
        "SELECT COUNT(*) AS n FROM verification_codes WHERE email = ? AND created_at > datetime('now', '-1 hour')",
        [email]));
      if (Number((hourly && hourly.n) || 0) >= 10)
        return res.status(429).json({ success: false, message: '该邮箱验证码发送过于频繁，请1小时后再试' });

      const exists = rowToObj(db.exec("SELECT id FROM users WHERE email = ? AND id != ?", [email, req.user.userId]));
      if (exists) return res.status(400).json({ success: false, message: '该邮箱已被其他账号绑定' });

      const code = generateCode();
      const expiresAt = new Date(Date.now() + 10 * 60 * 1000).toISOString();
      db.run("DELETE FROM verification_codes WHERE email = ? AND type = 'bind'", [email]);
      db.run("INSERT INTO verification_codes (email, code, type, expires_at) VALUES (?, ?, 'bind', ?)", [email, code, expiresAt]);
      saveDb();

      if (!mailTransporter) {
        db.run("DELETE FROM verification_codes WHERE email = ? AND type = 'bind'", [email]);
        saveDb();
        return res.status(503).json({ success: false, message: '本站邮件服务未启用，无法发送验证码' });
      }
      const sent = await sendEmail(email, code, 10);
      if (!sent) {
        // 失败就删掉刚写入的码，否则 60 秒频率限制会生效，用户立刻重试只会看到「请60秒后再试」
        db.run("DELETE FROM verification_codes WHERE email = ? AND type = 'bind'", [email]);
        saveDb();
        return res.status(502).json({ success: false, message: '验证码发送失败，请稍后重试或联系管理员' });
      }
      res.json({ success: true, message: '验证码已发送' });
    } catch (e) {
      console.error('[send-bind-code] 异常:', e.message);
      if (!res.headersSent) res.status(500).json({ success: false, message: '服务器内部错误' });
    }
  });

  // ====== Verify ======
  app.post('/api/v2/verify', (req, res) => {
    const { token } = req.body;
    if (!token) return res.status(400).json({ success: false, message: '请提供token' });
    try {
      const d = jwt.verify(token, JWT_SECRET);
      const user = rowToObj(db.exec("SELECT id, username, email, role, signature FROM users WHERE id = ?", [d.userId]));
      res.json({ success: true, data: { userId: d.userId, username: d.username, role: d.role, signature: user?.signature || '' } });
    } catch { res.status(401).json({ success: false, message: 'Token无效' }); }
  });

  // ====== Users (Admin) ======
  app.get('/api/v2/users', authMiddleware, staffMiddleware, (req, res) => {
    // ⚠️ banned_until / ban_reason / ban_level / mid_strike 必须 SELECT 出来：
    //   控制台要给「环境检测封禁」打红标 + 支持按封禁筛选 + 直接解封，
    //   缺了这几列前端就只能看到 status='active'，把被环境检测封掉的账号显示成「正常」。
    const rows = rowsToArr(db.exec("SELECT id, username, email, role, status, created_at, last_login, vip_expire_at, vip_level, agent_id, session_epoch, banned_until, ban_reason, ban_level, mid_strike FROM users"));
    // 代理角色已删：能走到这里的一定是管理员/开发者，直接看全部。
    const users = rows;
    // 单独开通的模块：**一次查出全部再内存归并**，不在循环里逐用户查库（N+1）。
    // 只保留仍在有效期内的（expire_at 为空 = 永久）；已过期的等同于没开通 ——
    // 否则控制台会把「早就过期」显示成「已开通」，管理员据此判定就会错。
    const modNames = {};
    getModules().modules.forEach(m => { modNames[m.id] = m.name; });
    const modByUser = {};
    try {
      rowsToArr(db.exec("SELECT user_id, module_id, expire_at FROM user_modules")).forEach(r => {
        if (r.expire_at && !isVipActive(r.expire_at)) return;
        (modByUser[r.user_id] = modByUser[r.user_id] || []).push(r.module_id);
      });
    } catch (e) { /* 极老库没有这张表：不影响用户列表本身 */ }

    // 在线状态来源是内存里的 LAST_SEEN（见 authMiddleware）。顺手剪掉 1 小时没动静的记录，
    // 免得长跑的服务把 Map 越撑越大（上界本来就只是账号数，剪一下更省心）。
    const nowMs = Date.now();
    LAST_SEEN.forEach((ts, uid) => { if (nowMs - ts > 3600 * 1000) LAST_SEEN.delete(uid); });

    users.forEach(u => {
      u.vipDaysLeft = daysLeftOf(u.vip_expire_at);
      u.vipLevel = effectiveVipLevel(u);
      u.vipLevelLabel = VIP_LEVEL_LABEL[u.vipLevel];
      u.isVip = u.vipLevel !== 'normal';
      u.vipPermanent = isVipForever(u.vip_expire_at);
      const mids = modByUser[u.id] || [];
      u.modules = mids;
      u.modulesLabel = mids.map(id => modNames[id] || id).join('、');   // 控制台列表直接展示
      // 标记当前登录者改得动的账号；改不动的（同级/上级）在控制台里显示为只读
      u.canManage = canManage(req.actorRole, u.role);

      // 云端强制下线相关：online 是内存里的「最后请求时间」，kickedAt 是上次被踢的时刻。
      // online 为空（进程刚重启）时前端退回展示 last_login —— 不要显示成「从没在线」。
      const seen = LAST_SEEN.get(Number(u.id)) || 0;
      u.online = !!seen && (nowMs - seen) < ONLINE_WINDOW_MS;
      u.lastSeenAt = seen ? new Date(seen).toISOString() : null;
      u.kickedAt = Number(u.session_epoch || 0) > 0
        ? new Date(Number(u.session_epoch) * 1000).toISOString() : null;

      // 封禁态（给控制台打标 + 筛选 + 解封用）：
      //   banKind = 'manual'（管理员手工永久封）/ 'env'（环境检测自动时限封）/ ''（无）
      const ban = activeBanOf(u);
      u.banActive = !!ban;
      u.banKind = ban ? ban.level : '';
      u.banUntil = ban ? ban.until : null;
      u.banPermanent = ban ? !!ban.permanent : false;
      u.banReason = ban ? ban.reason : '';
    });
    res.json({ success: true, data: users, actorRole: req.actorRole, actorRank: rankOf(req.actorRole) });
  });

  app.post('/api/v2/users', authMiddleware, adminMiddleware, (req, res) => {
    const { username, password, role } = req.body;
    if (!username || !password) return res.status(400).json({ success: false, message: '用户名和密码不能为空' });
    // 🔴 与 /api/register、/api/auth/code-register、PUT /api/users/:id **共用同一个出口**
    //    （长度 + 字符集 + 纯数字，连判定顺序都一致）。2026-09-30 订正：
    //    这里原来是 `username.length < 3` + 一条单独的纯数字校验，**不查字符集、也不查上限** ——
    //    后果是管理端能建出**用户自己永远改不出来**的名字（改名那条 PUT 走的是完整校验）：
    //    带空格的 `hello world`、带 `<` 的、30 位超长的，都能建出来，而用户想改成别的名时
    //    才第一次撞上规则。同一套规则必须只有一份实现。
    //    ⚠️ 旧行为「2 位名被拒」也一并修正为与全局一致（USERNAME_RE 是 {2,24}）。
    const uErr = usernameError(username);
    if (uErr) return res.status(400).json({ success: false, message: uErr });
    if (password.length < 6) return res.status(400).json({ success: false, message: '密码至少6个字符' });
    const newRole = normalizeRole(role || 'user');
    // 只能创建「等级严格低于自己」的账号：管理员造不出第二个管理员
    if (!canAssignRole(req.actorRole, newRole))
      return res.status(403).json({ success: false, message: `权限不足：不能创建「${ROLE_LABEL[newRole]}」及更高等级的账号` });
    try {
      db.run("INSERT INTO users (username, password, role) VALUES (?, ?, ?)", [username, bcrypt.hashSync(password, 10), newRole]);
      saveDb();
      res.json({ success: true, message: '用户添加成功' });
    } catch (e) {
      if (String(e).includes('UNIQUE')) return res.status(400).json({ success: false, message: '用户名已存在' });
      res.status(500).json({ success: false, message: '添加用户失败' });
    }
  });

  app.put('/api/v2/users/:id', authMiddleware, adminMiddleware, (req, res) => {
    const { username, password, role, status, email } = req.body;
    // 目标账号守卫：管理员不能管理同级或更高等级（管理员之间互不可改）；开发者不受此限
    const target = guardTarget(req, res);
    if (!target) return;
    // 改角色属于提权操作，单独校验：不许改自己（开发者除外）、也不许给出 >= 自己的角色
    if (role !== undefined && role !== '' && normalizeRole(role) !== normalizeRole(target.role)) {
      if (target.id === req.user.userId && !isDeveloper(req.actorRole))
        return res.status(403).json({ success: false, message: '不能修改自己的角色' });
      if (!canAssignRole(req.actorRole, role))
        return res.status(403).json({ success: false, message: `权限不足：不能将账号设置为「${ROLE_LABEL[normalizeRole(role)]}」` });
    }
    const sets = [];
    const vals = [];
    if (username !== undefined && username !== '') {
      const uErr = usernameError(username);
      if (uErr) return res.status(400).json({ success: false, message: uErr });
      const exists = rowToObj(db.exec("SELECT id FROM users WHERE username = ? AND id != ?", [username, req.params.id]));
      if (exists) return res.status(400).json({ success: false, message: '用户名已存在，请换一个' });
      sets.push("username = ?"); vals.push(username);
    }
    if (role) { sets.push("role = ?"); vals.push(normalizeRole(role)); }
    if (status) {
      if (!['active', 'banned'].includes(status))
        return res.status(400).json({ success: false, message: '状态参数不正确' });
      sets.push("status = ?"); vals.push(status);
    }
    if (email !== undefined) {
      if (email !== '' && !/^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(email))
        return res.status(400).json({ success: false, message: '邮箱格式不正确' });
      if (email !== '') {
        const mailTaken = rowToObj(db.exec("SELECT id FROM users WHERE email = ? AND id != ?", [email, req.params.id]));
        if (mailTaken) return res.status(400).json({ success: false, message: '该邮箱已被其他账号绑定' });
      }
      sets.push("email = ?"); vals.push(email);
    }
    if (password) {
      if (password.length < 6) return res.status(400).json({ success: false, message: '密码至少 6 个字符' });
      sets.push("password = ?"); vals.push(bcrypt.hashSync(password, 10));
    }
    if (!sets.length) return res.status(400).json({ success: false, message: '没有需要更新的内容' });
    try {
      vals.push(req.params.id);
      db.run(`UPDATE users SET ${sets.join(', ')} WHERE id = ?`, vals);
      saveDb();
      res.json({ success: true, message: '用户更新成功' });
    } catch (e) {
      if (String(e).includes('UNIQUE')) return res.status(400).json({ success: false, message: '用户名或邮箱已被占用' });
      res.status(500).json({ success: false, message: '更新失败: ' + e.message });
    }
  });

  // 修改用户 ID（管理员，级联更新关联表）
  app.put('/api/v2/users/:id/change-id', authMiddleware, adminMiddleware, (req, res) => {
    const oldId = parseInt(req.params.id);
    const newId = parseInt(req.body.newId);
    if (!Number.isFinite(oldId) || !Number.isFinite(newId) || newId <= 0)
      return res.status(400).json({ success: false, message: '请输入有效的用户 ID（正整数）' });
    if (oldId === newId) return res.json({ success: true, message: '用户 ID 未变化' });

    // 目标账号守卫（不能改同级/上级账号的 ID）
    const target = guardTarget(req, res);
    if (!target) return;
    const taken = rowToObj(db.exec("SELECT id, username FROM users WHERE id = ?", [newId]));
    if (taken) return res.status(400).json({ success: false, message: `ID ${newId} 已被用户 ${taken.username} 占用` });

    try {
      db.run("BEGIN");
      db.run("UPDATE license_keys SET user_id = ? WHERE user_id = ?", [newId, oldId]);
      db.run("UPDATE login_logs SET user_id = ? WHERE user_id = ?", [newId, oldId]);
      db.run("UPDATE usage_logs SET user_id = ? WHERE user_id = ?", [newId, oldId]);
      db.run("UPDATE feedback SET user_id = ? WHERE user_id = ?", [newId, oldId]);
      // 2026-10-02 补迁：原来漏了 5 张表，改完 ID 后这些行会指向一个不存在的用户
      //（功能用量清零、增值模块丢失、环境报告/申诉查不到）。
      db.run("UPDATE feature_usage SET user_id = ? WHERE user_id = ?", [newId, oldId]);
      db.run("UPDATE user_modules SET user_id = ? WHERE user_id = ?", [newId, oldId]);
      db.run("UPDATE env_reports SET user_id = ? WHERE user_id = ?", [newId, oldId]);
      db.run("UPDATE appeals SET user_id = ? WHERE user_id = ?", [newId, oldId]);
      db.run("UPDATE env_challenges SET user_id = ? WHERE user_id = ?", [newId, oldId]);
      // 引用该用户的其他行：名下代理挂靠关系 + 他作为操作人留下的授权/处理记录
      db.run("UPDATE users SET agent_id = ? WHERE agent_id = ?", [newId, oldId]);
      db.run("UPDATE user_modules SET granted_by = ? WHERE granted_by = ?", [newId, oldId]);
      db.run("UPDATE appeals SET handled_by = ? WHERE handled_by = ?", [newId, oldId]);
      db.run("UPDATE users SET id = ? WHERE id = ?", [newId, oldId]);
      db.run("COMMIT");
      saveDb();
      res.json({ success: true, message: `用户 ${target.username} 的 ID 已由 ${oldId} 改为 ${newId}（该用户需重新登录）` });
    } catch (e) {
      try { db.run("ROLLBACK"); } catch { }
      res.status(500).json({ success: false, message: '修改用户 ID 失败: ' + e.message });
    }
  });

  app.delete('/api/v2/users/:id', authMiddleware, adminMiddleware, (req, res) => {
    const target = guardTarget(req, res);
    if (!target) return;
    if (target.id === req.user.userId) return res.status(400).json({ success: false, message: '不能删除自己' });
    // 2026-10-02 级联清理：功能数据必须跟着账号走（否则残留的额度/模块/未答校验题
    // 指向幽灵用户）；审计流水（登录/使用/反馈/卡密）刻意保留，供事后追溯。
    const full = rowToObj(db.exec("SELECT id, username, role, status, email FROM users WHERE id = ?", [target.id]));
    try {
      db.run("BEGIN");
      db.run("DELETE FROM feature_usage WHERE user_id = ?", [target.id]);
      db.run("DELETE FROM user_modules WHERE user_id = ?", [target.id]);
      db.run("DELETE FROM env_challenges WHERE user_id = ?", [target.id]);
      db.run("DELETE FROM env_reports WHERE user_id = ?", [target.id]);
      db.run("DELETE FROM appeals WHERE user_id = ?", [target.id]);
      if (full && full.email) db.run("DELETE FROM verification_codes WHERE email = ?", [full.email]);
      // 名下用户解除挂靠（而不是跟着删：那些是独立账号）
      db.run("UPDATE users SET agent_id = NULL WHERE agent_id = ?", [target.id]);
      // 名下有效卡密停用（防止删号后卡密仍可登录——key-login 对无主卡密会自动建新号）
      db.run("UPDATE license_keys SET status = 'disabled' WHERE user_id = ? AND status = 'active'", [target.id]);
      db.run("DELETE FROM users WHERE id = ?", [target.id]);
      db.run("COMMIT");
      saveDb();
      res.json({ success: true, message: '用户删除成功（名下挂靠已解除，有效卡密已停用，审计流水保留）' });
    } catch (e) {
      try { db.run("ROLLBACK"); } catch { }
      res.status(500).json({ success: false, message: '删除用户失败: ' + e.message });
    }
  });

  app.post('/api/v2/users/:id/reset-password', authMiddleware, adminMiddleware, (req, res) => {
    const target = guardTarget(req, res);
    if (!target) return;
    const { newPassword } = req.body;
    if (!newPassword || newPassword.length < 6) return res.status(400).json({ success: false, message: '密码至少6个字符' });
    // 2026-10-02：管理员重置密码后递增 session_epoch，把目标账号的现有会话全部踢下线
    //（否则旧密码的主人/拿着旧 token 的人还能继续用到 token 过期）。
    const row = rowToObj(db.exec("SELECT session_epoch FROM users WHERE id = ?", [target.id]));
    const epoch = Math.max(Math.floor(Date.now() / 1000), Number((row && row.session_epoch) || 0) + 1);
    db.run("UPDATE users SET password = ?, session_epoch = ? WHERE id = ?", [bcrypt.hashSync(newPassword, 10), epoch, target.id]);
    saveDb();
    LAST_SEEN.delete(Number(target.id));
    // 通知被重置的人：不然他只会觉得「密码突然不对了」，然后来骂
    sendAccountMail('password_reset_admin', target, {}).catch(() => {});
    res.json({ success: true, message: '密码重置成功，该账号已强制重新登录' });
  });

  // ====== Admin: 会员等级（普通 / VIP / SVIP） ======
  // body: { level:'normal'|'vip'|'svip', days? }
  //   level='normal'          → 清空等级与到期时间
  //   level + days>0          → 设定等级，并把到期时间重设为「现在 + days 天」
  //   level 不带 days          → 只改等级、保留原到期时间；若原到期时间已失效（否则等级不生效），默认补 30 天

  // ====== Admin: 单用户权限（会员等级 + 逐模块单独开通）======
  // 这是「为各个用户单独改权限」的落点。等级与模块都挂在同一个用户身上，
  // 所以放同一个接口读写 —— 拆成两个接口的话，控制台弹窗会出现
  // 「等级已存、模块没存」的中间态，用户以为配好了其实没有。

  // body: { level?, days?, modules?: { 'm3': { days?, note? } | null } }
  //   level 非空        → 改会员等级；days>0 同时重设到期时间（缺省且原已失效则补 30 天）
  //   modules.m3 = {…}   → 为 m3 单独开通；days 缺省 / ≤0 = 永久
  //   modules.m3 = null  → 撤销 m3 的单独开通

  // ====== Admin: 云端强制下线（按用户，2026-09-28）======
  // 把 session_epoch 推到一个**新值**，该账号所有已登录客户端会在**下一次请求**时收到
  // 401 + code=KICKED（客户端 30 秒轮询一次 /api/profile；用户点任意功能按钮也会立刻触发），
  // 随即清掉本地登录态并退出登录。
  //
  // ⚠️ 这个值只是「会话代次」，不是严格的时间：authMiddleware 比对的是 token 里的 se 快照
  //    （见 signToken 注释），所以能不能踢掉与时钟精度无关。但仍取 unix 秒有两个好处 ——
  //    控制台能直接展示成「何时踢过」，且用 Math.max(now, prev+1) 保证**严格递增**：
  //    同一秒内连踢两次也会推进，不会出现「第二次踢不动」。
  //
  // ⚠️ 这不是封号：用户重新登录就能继续用。要彻底拦住请配合「账号停用」或「封禁 IP」。
  app.post('/api/v2/users/:id/kick', authMiddleware, adminMiddleware, (req, res) => {
    const guarded = guardTarget(req, res);
    if (!guarded) return;
    const user = rowToObj(db.exec("SELECT id, username, session_epoch FROM users WHERE id = ?", [guarded.id]));
    if (!user) return res.status(404).json({ success: false, message: '账号不存在' });

    const epoch = Math.max(Math.floor(Date.now() / 1000), Number(user.session_epoch || 0) + 1);
    db.run("UPDATE users SET session_epoch = ? WHERE id = ?", [epoch, user.id]);
    saveDb();
    LAST_SEEN.delete(Number(user.id));

    res.json({
      success: true,
      message: `${user.username} 已被强制下线；其客户端将在下次请求（约 30 秒内）退出登录`,
      data: { userId: user.id, kickedAt: new Date(epoch * 1000).toISOString() },
    });
  });

  // ====== Admin: 会员（VIP）时长管理 ======
  // body: { days }              叠加/扣减天数（正数加、负数减，从「当前到期时间」或「现在」较晚者起算）
  // body: { mode:'set', days }  直接设为「现在 + days 天」；days<=0 表示清空会员
  // body: { mode:'set', vip_expire_at:'2026-12-31' }  直接设为指定到期日；空值清空
  // body: { permanent: true }   设为永久会员；{ permanent: false } 取消永久（清空会员）

  // ====== License Keys (Admin) ======
  app.get('/api/v2/keys', authMiddleware, staffMiddleware, (req, res) => {
    const base = "SELECT k.*, u.username as bound_username FROM license_keys k LEFT JOIN users u ON k.user_id = u.id ";
    const keys = isGlobalStaff(req)
      ? rowsToArr(db.exec(base + "ORDER BY k.id DESC"))
      : rowsToArr(db.exec(base + "WHERE k.owner_id = ? ORDER BY k.id DESC", [req.user.userId]));
    res.json({ success: true, data: keys, scope: isGlobalStaff(req) ? 'global' : 'own' });
  });

  app.post('/api/v2/keys', authMiddleware, staffMiddleware, (req, res) => {
    const { count = 1, expires_at, days = 0, level } = req.body;
    const d = Math.max(0, Math.min(36500, parseInt(days) || 0));
    // 卡密等级：管理员/开发者最高可签发 SVIP；代理只能签发到 VIP，不能越级发 SVIP
    const grantLevel = normalizeVipLevel(level || 'vip');
    const canSvip = rankOf(req.actorRole) >= ROLE_RANK.admin;
    if (vipLevelRank(grantLevel) > (canSvip ? VIP_LEVEL_RANK.svip : VIP_LEVEL_RANK.vip))
      return res.status(403).json({ success: false, message: `权限不足：不能签发「${VIP_LEVEL_LABEL[grantLevel]}」等级的卡密` });

    const keys = [];
    for (let i = 0; i < Math.min(count, 100); i++) {
      // 会员卡密（含天数）使用 KUN-VIP-xxxx-xxxx / KUN-SVIP-xxxx-xxxx 格式；登录卡密沿用原格式
      let keyCode;
      if (d > 0) {
        // v2（2026-10-11）：3 段 = 12 字符 × 32 进制 ≈ 2^60 熵（原 2 段仅 2^40，
        // 理论上可被大规模枚举）。校验按前缀查库、不硬编码长度，存量 2 段密钥照常可用。
        keyCode = (grantLevel === 'svip' ? 'KUN-SVIP-' : 'KUN-VIP-') + genSegment() + '-' + genSegment() + '-' + genSegment();
      } else {
        keyCode = generateKey();
      }
      db.run("INSERT INTO license_keys (key_code, expires_at, days, owner_id, grant_level) VALUES (?, ?, ?, ?, ?)",
        [keyCode, expires_at || null, d, req.user.userId, grantLevel]);
      keys.push(keyCode);
    }
    saveDb();
    res.json({ success: true, message: `生成 ${keys.length} 个密钥`, data: keys });
  });

  // 卡密编辑：卡密号/状态/过期/设备数/天数/绑定用户 均可改
  app.put('/api/v2/keys/:id', authMiddleware, staffMiddleware, (req, res) => {
    const { key_code, status, expires_at, max_devices, days, user_id, level } = req.body;
    const key = guardKey(req, res);
    if (!key) return;
    const sets = [];
    const vals = [];
    if (key_code !== undefined && String(key_code).trim() !== '') {
      const kc = String(key_code).trim().toUpperCase();
      if (!/^[A-Z0-9-]{6,64}$/.test(kc)) return res.status(400).json({ success: false, message: '卡密格式不正确（字母/数字/连字符，6-64 位）' });
      const dup = rowToObj(db.exec("SELECT id FROM license_keys WHERE key_code = ? AND id != ?", [kc, key.id]));
      if (dup) return res.status(400).json({ success: false, message: '卡密号已存在' });
      sets.push("key_code = ?"); vals.push(kc);
    }
    if (status !== undefined) {
      if (!['active', 'disabled', 'used'].includes(status)) return res.status(400).json({ success: false, message: '状态不正确' });
      sets.push("status = ?"); vals.push(status);
    }
    if (expires_at !== undefined) {
      if (expires_at === null || expires_at === '') { sets.push("expires_at = NULL"); }
      else {
        const dt = new Date(expires_at);
        if (isNaN(dt)) return res.status(400).json({ success: false, message: '过期时间格式不正确' });
        sets.push("expires_at = ?"); vals.push(dt.toISOString());
      }
    }
    if (max_devices !== undefined) {
      const md = parseInt(max_devices);
      if (!Number.isFinite(md) || md < 1 || md > 999) return res.status(400).json({ success: false, message: '设备数需为 1-999' });
      sets.push("max_devices = ?"); vals.push(md);
    }
    if (days !== undefined) {
      const d = parseInt(days);
      if (!Number.isFinite(d) || d < 0 || d > 36500) return res.status(400).json({ success: false, message: '天数需为 0-36500' });
      sets.push("days = ?"); vals.push(d);
    }
    if (level !== undefined) {
      const lv = normalizeVipLevel(level);
      const canSvip = rankOf(req.actorRole) >= ROLE_RANK.admin;
      if (vipLevelRank(lv) > (canSvip ? VIP_LEVEL_RANK.svip : VIP_LEVEL_RANK.vip))
        return res.status(403).json({ success: false, message: `权限不足：不能把卡密设为「${VIP_LEVEL_LABEL[lv]}」` });
      sets.push("grant_level = ?"); vals.push(lv);
    }
    if (user_id !== undefined) {
      if (user_id === null || user_id === '') { sets.push("user_id = NULL"); }
      else {
        const uid = parseInt(user_id);
        if (!Number.isFinite(uid) || !rowToObj(db.exec("SELECT id FROM users WHERE id = ?", [uid])))
          return res.status(400).json({ success: false, message: '绑定的用户不存在' });
        sets.push("user_id = ?"); vals.push(uid);
      }
    }
    if (!sets.length) return res.status(400).json({ success: false, message: '没有需要修改的内容' });
    vals.push(key.id);
    db.run(`UPDATE license_keys SET ${sets.join(', ')} WHERE id = ?`, vals);
    saveDb();
    res.json({ success: true, message: '卡密已更新' });
  });

  app.delete('/api/v2/keys/:id', authMiddleware, staffMiddleware, (req, res) => {
    const key = guardKey(req, res);
    if (!key) return;
    db.run("DELETE FROM license_keys WHERE id = ?", [key.id]);
    saveDb();
    res.json({ success: true, message: '密钥删除成功' });
  });

  app.post('/api/v2/keys/batch-delete', authMiddleware, staffMiddleware, (req, res) => {
    const { ids } = req.body;
    if (!Array.isArray(ids) || ids.length === 0) return res.status(400).json({ success: false, message: '请选择要删除的密钥' });
    const placeholders = ids.map(() => '?').join(',');
    if (isGlobalStaff(req)) {
      db.run(`DELETE FROM license_keys WHERE id IN (${placeholders})`, ids);
    } else {
      // 代理只能删自己签发的卡密
      db.run(`DELETE FROM license_keys WHERE owner_id = ? AND id IN (${placeholders})`, [req.user.userId, ...ids]);
    }
    saveDb();
    res.json({ success: true, message: `删除 ${ids.length} 个密钥` });
  });

  // ====== User: My Keys ======
  app.get('/api/v2/user/keys', authMiddleware, (req, res) => {
    const keys = rowsToArr(db.exec("SELECT id, key_code, status, max_devices, created_at, expires_at FROM license_keys WHERE user_id = ? ORDER BY id DESC", [req.user.userId]));
    res.json({ success: true, data: keys });
  });

  // 用户自助管理自己的密钥（仅能操作 user_id = 自己 的记录）
  function findOwnKey(req) {
    return rowToObj(db.exec("SELECT * FROM license_keys WHERE id = ? AND user_id = ?", [req.params.id, req.user.userId]));
  }

  app.put('/api/v2/user/keys/:id', authMiddleware, (req, res) => {
    const { status } = req.body;
    if (!['active', 'disabled'].includes(status)) return res.status(400).json({ success: false, message: '状态参数不正确' });
    const key = findOwnKey(req);
    if (!key) return res.status(404).json({ success: false, message: '密钥不存在或不属于当前账号' });
    db.run("UPDATE license_keys SET status = ? WHERE id = ?", [status, key.id]);
    saveDb();
    res.json({ success: true, message: status === 'active' ? '密钥已启用' : '密钥已停用' });
  });

  app.delete('/api/v2/user/keys/:id', authMiddleware, (req, res) => {
    const key = findOwnKey(req);
    if (!key) return res.status(404).json({ success: false, message: '密钥不存在或不属于当前账号' });
    db.run("DELETE FROM license_keys WHERE id = ?", [key.id]);
    saveDb();
    res.json({ success: true, message: '密钥已删除' });
  });

  app.post('/api/v2/user/keys/batch-delete', authMiddleware, (req, res) => {
    const ids = Array.isArray(req.body.ids) ? req.body.ids.map(v => parseInt(v)).filter(v => Number.isFinite(v)) : [];
    if (!ids.length) return res.status(400).json({ success: false, message: '请先勾选要删除的密钥' });
    const ph = ids.map(() => '?').join(',');
    const owned = rowsToArr(db.exec(`SELECT id FROM license_keys WHERE user_id = ? AND id IN (${ph})`, [req.user.userId, ...ids]));
    if (!owned.length) return res.status(404).json({ success: false, message: '没有可删除的密钥' });
    db.run(`DELETE FROM license_keys WHERE user_id = ? AND id IN (${ph})`, [req.user.userId, ...ids]);
    saveDb();
    res.json({ success: true, message: `已删除 ${owned.length} 个密钥` });
  });

  // ====== Profile ======
  app.get('/api/v2/profile', authMiddleware, (req, res) => {
    const user = rowToObj(db.exec("SELECT id, username, email, role, status, signature, avatar, email_login_enabled, login_notify_enabled, two_factor_enabled, created_at, last_login, vip_expire_at, vip_level FROM users WHERE id = ?", [req.user.userId]));
    if (user) {
      user.vipDaysLeft = daysLeftOf(user.vip_expire_at);
      user.isVip = isVipActive(user.vip_expire_at);
      user.vipPermanent = isVipForever(user.vip_expire_at);
      user.vipPriceNote = getSetting('vip_price_note', '');
      // 客户端标题 / 设置页「会员等级 + 到期时间」都靠这三个字段，缺一个就会退化成
      // 「只有到期时间、没有等级」的旧逻辑。等级按过期判定回落，别直接回原始 vip_level。
      user.role = normalizeRole(user.role);
      user.vipLevel = effectiveVipLevel(user);
      user.vipLevelLabel = VIP_LEVEL_LABEL[user.vipLevel];
      // 兼容字段：客户端曾经只认 camelCase 的 vipExpireAt（而本接口回的是表列名
      // vip_expire_at），结果每轮轮询都把会员到期时间读成 null 并清空本地状态 ——
      // 「刚登录显示永久，进主窗口后变成未开通」，还会把永久会员降级成免费用户限次。
      // 老版本客户端已经发出去了，所以这里补一个驼峰别名，让它们也能拿到正确值。
      user.vipExpireAt = user.vip_expire_at || null;
    }
    if (!user) return res.status(404).json({ success: false, message: '用户不存在' });
    res.json({ success: true, data: user });
  });

  // ====== Profile: Avatar（自定义控制台头像，data URL 存储） ======
  app.put('/api/v2/user/avatar', authMiddleware, (req, res) => {
    const av = String(req.body.avatar || '');
    if (!av) {
      db.run("UPDATE users SET avatar = '' WHERE id = ?", [req.user.userId]);
      saveDb();
      return res.json({ success: true, message: '头像已恢复默认' });
    }
    if (!/^data:image\/(png|jpe?g|webp|gif);base64,/.test(av))
      return res.status(400).json({ success: false, message: '仅支持 png/jpg/webp/gif 图片' });
    if (av.length > 300 * 1024)
      return res.status(400).json({ success: false, message: '图片过大，请小于 300KB' });
    db.run("UPDATE users SET avatar = ? WHERE id = ?", [av, req.user.userId]);
    saveDb();
    res.json({ success: true, message: '头像已更新', data: { avatar: av } });
  });

  // ====== User Settings ======
  app.put('/api/v2/user/settings', authMiddleware, (req, res) => {
    const allowed = ['email_login_enabled', 'login_notify_enabled'];
    let n = 0;
    allowed.forEach(k => {
      if (req.body[k] !== undefined) {
        const val = req.body[k] === true || req.body[k] === '1' || req.body[k] === 1 ? 1 : 0;
        db.run(`UPDATE users SET ${k} = ? WHERE id = ?`, [val, req.user.userId]);
        n++;
      }
    });
    if (n > 0) saveDb();
    res.json({ success: true, message: `已保存 ${n} 项设置` });
  });

  app.post('/api/v2/change-password', authMiddleware, (req, res) => {
    const { oldPassword, newPassword } = req.body;
    if (!oldPassword || !newPassword) return res.status(400).json({ success: false, message: '请填写完整信息' });
    if (newPassword.length < 6) return res.status(400).json({ success: false, message: '新密码至少6个字符' });
    const user = rowToObj(db.exec("SELECT * FROM users WHERE id = ?", [req.user.userId]));
    if (!user || !bcrypt.compareSync(oldPassword, user.password)) return res.status(401).json({ success: false, message: '原密码错误' });
    // 2026-10-02：改密后递增 session_epoch，所有旧 token 失效（含当前这个——
    // 控制台改密成功后本来就 doLogout 重新登录，无感；其他设备的旧会话必须踢掉）。
    const epoch = Math.max(Math.floor(Date.now() / 1000), Number(user.session_epoch || 0) + 1);
    db.run("UPDATE users SET password = ?, session_epoch = ? WHERE id = ?", [bcrypt.hashSync(newPassword, 10), epoch, user.id]);
    saveDb();
    res.json({ success: true, message: '密码修改成功，所有会话已下线，请重新登录' });
  });

  // ====== Profile: Change Username ======
  app.put('/api/v2/user/change-username', authMiddleware, (req, res) => {
    const { newUsername, password } = req.body;
    if (!newUsername || !password) return res.status(400).json({ success: false, message: '请输入新用户名和密码' });
    const uErr = usernameError(newUsername);
    if (uErr) return res.status(400).json({ success: false, message: uErr });

    const user = rowToObj(db.exec("SELECT * FROM users WHERE id = ?", [req.user.userId]));
    if (!user || !bcrypt.compareSync(password, user.password))
      return res.status(401).json({ success: false, message: '密码错误' });

    const exists = rowToObj(db.exec("SELECT id FROM users WHERE username = ? AND id != ?", [newUsername, req.user.userId]));
    if (exists) return res.status(400).json({ success: false, message: '用户名已存在' });

    db.run("UPDATE users SET username = ? WHERE id = ?", [newUsername, req.user.userId]);
    saveDb();

    const token = signToken({ userId: user.id, username: newUsername, role: user.role });
    res.json({ success: true, message: '用户名修改成功', data: { token, username: newUsername } });
  });

  // ====== Profile: Change Signature ======
  app.put('/api/v2/user/signature', authMiddleware, (req, res) => {
    // 必须先把非字符串（null / 数字 / 对象）统一成字符串：原先只挡 undefined，
    // 传 {"signature":null} 会在 signature.length 上抛 TypeError → 500。
    const signature = req.body.signature === undefined || req.body.signature === null
      ? null : String(req.body.signature);
    if (signature === null) return res.status(400).json({ success: false, message: '请输入个性签名' });
    if (signature.length > 100) return res.status(400).json({ success: false, message: '签名最多100个字符' });
    db.run("UPDATE users SET signature = ? WHERE id = ?", [signature, req.user.userId]);
    saveDb();
    res.json({ success: true, message: '签名更新成功' });
  });

  // ====== Profile: Get Full Info ======
  app.get('/api/v2/user/full-profile', authMiddleware, (req, res) => {
    const user = rowToObj(db.exec("SELECT id, username, email, role, status, signature, avatar, created_at, last_login FROM users WHERE id = ?", [req.user.userId]));
    if (!user) return res.status(404).json({ success: false, message: '用户不存在' });
    res.json({ success: true, data: user });
  });

  // ====== Profile: Send Change Email Code (to old email) ======
  // ⚠️ 同 send-bind-code：必须 async + await 并如实回执，否则邮件失败也报「已发送」。
  app.post('/api/v2/user/send-change-email-code', authMiddleware, async (req, res) => {
    // 2026-10-02 加固：async 处理器包 try/catch —— Express 4 不捕获 async 异常，
    // 同步 db 抛错会变成 unhandled rejection 直接终止进程（send-bind-code 同款）。
    try {
      const user = rowToObj(db.exec("SELECT email FROM users WHERE id = ?", [req.user.userId]));
      if (!user || !user.email) return res.status(400).json({ success: false, message: '请先绑定邮箱' });

      const recent = rowToObj(db.exec(
        "SELECT id FROM verification_codes WHERE email = ? AND type = 'change_email' AND created_at > datetime('now', '-60 seconds')",
        [user.email]));
      if (recent) return res.status(429).json({ success: false, message: '请60秒后再试' });

      const code = generateCode();
      const expiresAt = new Date(Date.now() + 10 * 60 * 1000).toISOString();
      db.run("DELETE FROM verification_codes WHERE email = ? AND type = 'change_email'", [user.email]);
      db.run("INSERT INTO verification_codes (email, code, type, expires_at) VALUES (?, ?, 'change_email', ?)", [user.email, code, expiresAt]);
      saveDb();

      if (!mailTransporter) {
        db.run("DELETE FROM verification_codes WHERE email = ? AND type = 'change_email'", [user.email]);
        saveDb();
        return res.status(503).json({ success: false, message: '本站邮件服务未启用，无法发送验证码' });
      }
      const sent = await sendEmail(user.email, code, 10);
      if (!sent) {
        db.run("DELETE FROM verification_codes WHERE email = ? AND type = 'change_email'", [user.email]);
        saveDb();
        return res.status(502).json({ success: false, message: '验证码发送失败，请稍后重试或联系管理员' });
      }
      res.json({ success: true, message: '验证码已发送到当前绑定邮箱' });
    } catch (e) {
      console.error('[send-change-email-code] 异常:', e.message);
      if (!res.headersSent) res.status(500).json({ success: false, message: '服务器内部错误' });
    }
  });

  // ====== Profile: Verify Old Email + Bind New Email ======
  app.post('/api/v2/user/change-email', authMiddleware, (req, res) => {
    const { oldCode, newEmail, newCode } = req.body;
    if (!oldCode || !newEmail || !newCode) return res.status(400).json({ success: false, message: '请填写完整信息' });
    if (!/^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(newEmail)) return res.status(400).json({ success: false, message: '请输入有效的邮箱地址' });

    const user = rowToObj(db.exec("SELECT email FROM users WHERE id = ?", [req.user.userId]));
    if (!user || !user.email) return res.status(400).json({ success: false, message: '请先绑定邮箱' });

    const oldCk = fetchValidCode(user.email, 'change_email', oldCode);
    if (oldCk.locked) return res.status(429).json({ success: false, message: CODE_LOCKED_MSG });
    if (!oldCk.row) return res.status(401).json({ success: false, message: '旧邮箱验证码错误' });

    const newCk = fetchValidCode(newEmail, 'bind', newCode);
    if (newCk.locked) return res.status(429).json({ success: false, message: CODE_LOCKED_MSG });
    if (!newCk.row) return res.status(401).json({ success: false, message: '新邮箱验证码错误' });

    const exists = rowToObj(db.exec("SELECT id FROM users WHERE email = ? AND id != ?", [newEmail, req.user.userId]));
    if (exists) return res.status(400).json({ success: false, message: '该邮箱已被其他账号绑定' });

    db.run("UPDATE users SET email = ? WHERE id = ?", [newEmail, req.user.userId]);
    codeFailClear(user.email, 'change_email');
    codeFailClear(newEmail, 'bind');
    db.run("DELETE FROM verification_codes WHERE email = ? AND type = 'change_email'", [user.email]);
    db.run("DELETE FROM verification_codes WHERE email = ? AND type = 'bind'", [newEmail]);
    saveDb();
    // 换绑通知发给**旧邮箱**：账号被偷的第一步就是换掉找回邮箱，旧邮箱是最后一道警报。
    // （新邮箱刚被验证码确认过，再发一封「你绑定了」没有额外信息量。）
    sendAccountMail('email_changed',
      { id: req.user.userId, username: req.user.username, email: user.email },
      { oldEmail: user.email, newEmail }).catch(() => {});
    res.json({ success: true, message: '邮箱换绑成功' });
  });

  // ====== Login Notification (Email) ======
  // 版式对齐 QQ 安全中心那张「账号登录通知」卡片：白底卡片、灰标签 + 黑值的信息行，
  // 底部「若非本人操作，请点击修改密码。」+ 分隔线 + 右侧箭头的「修改密码」行。
  // 2026-10-07：按用户要求恢复「登录地点 / 登录 IP」两行（原注释的「不带 IP」约束作废）。
  // 2026-10-15：按用户要求区分「登录应用」——客户端与控制台共用同一批登录端点，
  //   以前这里恒显 KunClient，从控制台登录也收到「KunClient」通知，无法分辨来源。
  //   现由调用方在登录 body 里带 app 标记（console=官网控制台），经白名单映射成展示名，
  //   缺省按 KunClient 处理（旧版本客户端不发该字段）。
  //   同一映射也服务 login_logs.login_app（登录日志来源列）与 2FA 票据里带的来源。
  const LOGIN_APP_NAMES = { client: 'KunClient', kunclient: 'KunClient', console: '官网控制台' };
  function loginAppNameFromRaw(raw) {
    const k = String(raw || '').trim().toLowerCase();
    return LOGIN_APP_NAMES[k] || 'KunClient';
  }
  function loginAppNameOf(req) {
    return loginAppNameFromRaw(req && req.body && req.body.app);
  }

  async function sendLoginNotification(user, req) {
    // 四个提前 return 都必须留日志：以前完全静默，用户反馈「收不到登录通知」时
    // 无从判断是没配邮箱、关了开关、还是邮件服务不可用。
    if (!mailTransporter) { console.log('[LoginMail] 跳过：邮件服务未启用'); return; }
    if (!user.email) { console.log(`[LoginMail] 跳过：${user.username} 未绑定邮箱`); return; }
    if (getSetting('login_mail_notify', '1') !== '1') { console.log('[LoginMail] 跳过：全局开关 login_mail_notify 已关闭'); return; }
    if (user.login_notify_enabled === 0) { console.log(`[LoginMail] 跳过：${user.username} 个人关闭了登录通知`); return; }

    const time = new Date().toLocaleString('zh-CN', { timeZone: 'Asia/Shanghai', year:'numeric', month:'2-digit', day:'2-digit', hour:'2-digit', minute:'2-digit', second:'2-digit' })
      .replace(/\//g, '-');
    const ip = req ? clientIpOf(req) : '';
    // 归属地查询失败不阻塞：拿不到就留空串，模板里会显示占位符。
    const loc = ip ? await ipLocation(ip) : '';
    // 登录方式：邮箱验证码 / 卡密 / 密码，调用方可通过 req._loginVia 提示。
    const via = (req && req._loginVia) || '密码';
    // 登录应用：KunClient 客户端 / 官网控制台，调用方可通过 req._loginApp 提示。
    // 走白名单映射（loginAppNameOf），不到模板里的值全部来自固定字符串，无注入面。
    const app = (req && req._loginApp) || 'KunClient';

    const b = MAIL_BRAND;
    const html = mailDocument({
      kicker: '登录通知',
      title: '你的账号刚刚登录',
      subtitle: '下面是这次登录的匹配信息，花两秒核对一下。',
      body:
        mailInfoTable([
          ['账号', user.username],
          ['应用', app],
          ['时间', time],
          ['地点', loc],
          ['IP', maskIp(ip)],
          ['方式', via],
        ])
        + `<div style="margin:2px 0 0">`
        + mailButton(`${cfg.publicUrl}/console/`, '去控制台改密码', 'ghost')
        + `</div>`
        + `<div style="margin-top:4px;font-size:12.5px;line-height:1.8;color:${b.ink2}">`
        + `如果这确实是你本人，不用做任何事。</div>`,
      footNote: '如非本人操作，请立即修改密码并检查账号安全。',
    });
    try {
      await mailTransporter.sendMail({
        from: MAIL_FROM,
        to: user.email,
        subject: `账号登录通知 - ${user.username}`,
        html
      });
    } catch (e) {
      console.error('[Email] 登录通知发送失败:', e.message);
    }
  }

  // ====== 登录双重验证（2FA，2026-10-15 用户要求「增加客户端控制台登陆双重验证功能」）====
  // 形态：第一因子（密码 / 卡密）通过后，强制第二因子 = 绑定邮箱的 6 位验证码。
  //   · 邮箱验证码登录**豁免** —— 它本身已要求控制邮箱，与第二因子是同一因子，
  //     再加一道是循环论证（攻击者拿得到邮箱验证码，同样拿得到 2FA 验证码）；
  //   · 开关 users.two_factor_enabled 默认 0，存量账号零影响；
  //   · 开 / 关都必须验证邮箱（/api/v2/user/2fa）：否则会话被盗后一刀就能关掉保护；
  //   · 挑战票据 = 5 分钟有效的 JWT（purpose='2fa'），只带 userId + 来源 + 登录方式，
  //     不碰密码；二次校验时**重新加载账号**复核封禁 / 停用 / 开关状态，
  //     防止两步之间账号被处置或用户从别处关掉 2FA；
  //   · 验证码复用 verification_codes（type='two_factor'）+ CODE_FAILS 防爆破
  //     （错 5 次作废，与其它验证码场景同一把锁）。

  /// 签发 2FA 挑战票据（5 分钟）。app = 调用方的原始 app 标记（'console'/'client'/缺省），
  /// 由消费方（登录日志 / 通知邮件）经 loginAppNameFromRaw 映射，票据里不存展示名。
  /// extra = 附加载荷（卡密登录带 keyId，把「消费卡密」推迟到二次校验通过，见 key-login）。
  function signTwoFactorTicket(user, appRaw, via, extra) {
    return jwt.sign(Object.assign(
      { userId: user.id, username: user.username, purpose: '2fa', app: String(appRaw || ''), via: String(via || '密码'), tf: 1 },
      extra || {}), JWT_SECRET, { expiresIn: '5m' });
  }

  /// 校验票据：签名 + 未过期 + purpose 必须是 '2fa'（防把普通登录 token 拿来当票据用）。
  function verifyTwoFactorTicket(ticket) {
    try {
      const p = jwt.verify(String(ticket || ''), JWT_SECRET);
      if (!p || p.purpose !== '2fa' || !p.userId) return null;
      return p;
    } catch { return null; }
  }

  /// 打码邮箱：登录页提示「验证码已发送至 k***@qq.com」用。不打码等于把完整
  /// 邮箱号写在登录页上 —— 肩窥 / 截图场景里就是白送信息。
  function maskEmail(email) {
    const s = String(email || '');
    const at = s.indexOf('@');
    if (at <= 0) return s ? '***' : '';
    return s.slice(0, 1) + '***' + s.slice(at);
  }

  /// 发送 2FA 验证码（登录挑战自动发 / 登录时重发 / 开关管理共用同一出口）。
  /// 频率控制与 /auth/send-code 逐条对齐：60 秒一条 + 每邮箱每小时 10 条硬上限。
  /// 返回 { ok } | { ok:false, status, message } —— 调用方原样转 HTTP 状态码。
  async function sendTwoFactorCode(email) {
    const recent = rowToObj(db.exec(
      "SELECT id FROM verification_codes WHERE email = ? AND created_at > datetime('now', '-60 seconds')",
      [email]));
    if (recent) return { ok: false, status: 429, message: '请60秒后再试' };

    const hourly = rowToObj(db.exec(
      "SELECT COUNT(*) AS n FROM verification_codes WHERE email = ? AND created_at > datetime('now', '-1 hour')",
      [email]));
    if (Number((hourly && hourly.n) || 0) >= 10)
      return { ok: false, status: 429, message: '该邮箱验证码发送过于频繁，请1小时后再试' };

    const code = generateCode();
    const expiresAt = new Date(Date.now() + 10 * 60 * 1000).toISOString(); // 10 minutes
    db.run("DELETE FROM verification_codes WHERE email = ? AND type = 'two_factor'", [email]);
    db.run("INSERT INTO verification_codes (email, code, type, expires_at) VALUES (?, ?, 'two_factor', ?)",
      [email, code, expiresAt]);
    saveDb();

    if (!mailTransporter) {
      db.run("DELETE FROM verification_codes WHERE email = ? AND type = 'two_factor'", [email]);
      saveDb();
      return { ok: false, status: 503, message: '本站邮件服务未启用，无法发送验证码' };
    }
    const sent = await sendEmail(email, code, 10);
    if (!sent) {
      // 与 /auth/send-code 同口径：发失败就删掉刚写的码，否则 60 秒频控会让
      // 用户立刻重试只看到「请60秒后再试」，误以为是自己手快。
      db.run("DELETE FROM verification_codes WHERE email = ? AND type = 'two_factor'", [email]);
      saveDb();
      return { ok: false, status: 502, message: '验证码发送失败，请稍后重试' };
    }
    return { ok: true, maskedEmail: maskEmail(email) };
  }

  /// 2FA 挑战：密码 / 卡密登录通过后调用。发码 + 落一条 2fa_pending 日志 + 签票据。
  /// extra = 附加票据载荷（卡密登录传 { keyId }，把「消费卡密」推迟到二次校验通过）。
  /// 发码失败**不签票据** —— 让用户整个登录重试，比给一张换不来码的票更直观。
  async function issueTwoFactorChallenge(user, req, appRaw, via, extra) {
    if (!user.email)
      return { ok: false, status: 500, message: '账号安全设置异常（未绑定邮箱却开启了双重验证），请联系管理员' };
    const sent = await sendTwoFactorCode(user.email);
    if (!sent.ok) return sent;
    db.run("INSERT INTO login_logs (user_id, username, status, ip_address, login_app) VALUES (?, ?, '2fa_pending', ?, ?)",
      [user.id, user.username, clientIpOf(req), loginAppNameFromRaw(appRaw)]);
    saveDb();
    return { ok: true, ticket: signTwoFactorTicket(user, appRaw, via, extra), maskedEmail: sent.maskedEmail };
  }

  /// 2FA 二次校验的账号复核（resend / verify 共用）：存在 / 封禁 / 停用 / 开关仍开着。
  /// 返回 { user } | { status, code, message }。
  function twoFactorActorOf(p, res) {
    const user = rowToObj(db.exec("SELECT * FROM users WHERE id = ?", [p.userId]));
    if (!user) return { status: 401, code: 'ACCOUNT_GONE', message: '账号不存在或已被删除' };
    const ban = activeBanOf(user);
    if (ban) {
      return {
        status: 403, code: 'BANNED',
        message: banMessageOf(ban, appealOf(user.id)),
        until: ban.until, permanent: ban.permanent, appeal: appealOf(user.id),
      };
    }
    if (user.status && user.status !== 'active')
      return { status: 403, message: '账号已被停用' };
    // 两步之间用户从别处关掉了 2FA（或账号被改了设置）：票据作废，重新登录。
    if (user.two_factor_enabled !== 1)
      return { status: 401, code: '2FA_CHANGED', message: '安全设置已变化，请重新登录' };
    return { user };
  }

  // ── 登录时重发 2FA 验证码（票据即凭证，未登录态可用）─────────────────────────
  app.post('/api/v2/auth/2fa/resend', async (req, res) => {
    try {
      const p = verifyTwoFactorTicket(req.body.ticket);
      if (!p) return res.status(401).json({ success: false, code: '2FA_TICKET_INVALID', message: '登录验证已过期，请重新登录' });
      const actor = twoFactorActorOf(p);
      if (actor.status) return res.status(actor.status).json({ success: false, code: actor.code, message: actor.message, until: actor.until, permanent: actor.permanent, appeal: actor.appeal });
      const sent = await sendTwoFactorCode(actor.user.email);
      if (!sent.ok) return res.status(sent.status || 500).json({ success: false, message: sent.message });
      res.json({ success: true, message: '验证码已发送，若未收到请检查垃圾邮件', data: { maskedEmail: sent.maskedEmail } });
    } catch (e) {
      console.error('[2FA] resend 异常:', e.message);
      res.status(500).json({ success: false, message: '服务器繁忙，请稍后重试' });
    }
  });

  // ── 2FA 二次校验：票据 + 验证码 → 正式登录 token ─────────────────────────────
  // 这才是登录真正完成的时刻：last_login / success 日志 / 登录通知邮件都落在这里，
  // 挑战阶段只落 2fa_pending（见 issueTwoFactorChallenge）。
  app.post('/api/v2/auth/2fa/verify', async (req, res) => {
    try {
      const p = verifyTwoFactorTicket(req.body.ticket);
      if (!p) return res.status(401).json({ success: false, code: '2FA_TICKET_INVALID', message: '登录验证已过期，请重新登录' });
      const actor = twoFactorActorOf(p);
      if (actor.status) return res.status(actor.status).json({ success: false, code: actor.code, message: actor.message, until: actor.until, permanent: actor.permanent, appeal: actor.appeal });
      const user = actor.user;

      const ck = fetchValidCode(user.email, 'two_factor', String(req.body.code || '').trim());
      if (ck.locked) return res.status(429).json({ success: false, message: CODE_LOCKED_MSG });
      if (!ck.row) return res.status(401).json({ success: false, message: '验证码错误' });

      codeFailClear(user.email, 'two_factor');
      db.run("DELETE FROM verification_codes WHERE email = ? AND type = 'two_factor'", [user.email]);
      db.run("UPDATE users SET last_login = datetime('now') WHERE id = ?", [user.id]);

      // 卡密登录的 2FA：到这里才真正消费卡密（key-login 里推迟消费的原因见那里）。
      // 两步之间卡密被别的会话用掉 / 停用 / 删除 → 拒绝并说明，绝不重复发放。
      let grantDays = 0;
      if (p.keyId) {
        const keyRow = rowToObj(db.exec("SELECT * FROM license_keys WHERE id = ?", [p.keyId]));
        if (!keyRow || keyRow.status !== 'active'
            || (keyRow.expires_at && new Date(keyRow.expires_at) < new Date())) {
          return res.status(401).json({ success: false, code: 'KEY_INVALID', message: '密钥无效或已失效，请重新登录' });
        }
        grantDays = parseInt(keyRow.days) || 0;
        db.run("UPDATE license_keys SET status = 'used', user_id = ? WHERE id = ?", [user.id, keyRow.id]);
        if (grantDays > 0) addVipDays(user.id, grantDays);
        applyKeyEntitlement(user.id, keyRow, grantDays);
      }

      db.run("INSERT INTO login_logs (user_id, username, status, ip_address, login_app) VALUES (?, ?, 'success', ?, ?)",
        [user.id, user.username, clientIpOf(req), loginAppNameFromRaw(p.app)]);
      saveDb();

      // 登录通知邮件：via / app 从票据还原（这一步才是登录真正完成）。
      req._loginVia = p.via || '密码';
      req._loginApp = loginAppNameFromRaw(p.app);
      sendLoginNotification(user, req).catch(e => console.error('[LoginMail] 登录通知异常:', e.message));

      // 会员字段要在卡密消费**之后**重取，否则 entitlement 还是消费前的旧值。
      const fresh = rowToObj(db.exec("SELECT * FROM users WHERE id = ?", [user.id]));
      const token = signToken({ userId: user.id, username: user.username, role: user.role });
      const grantMsg = grantDays > 0 ? `，已自动开通 ${grantDays} 天会员` : '';
      res.json({ success: true, message: `登录成功${grantMsg}`, data: { token, username: user.username, role: user.role, signature: user.signature || '', expiresIn: 86400, grantedDays: grantDays, entitlement: entitlementOf(fresh) } });
    } catch (e) {
      console.error('[2FA] verify 异常:', e.message);
      res.status(500).json({ success: false, message: '服务器繁忙，请稍后重试' });
    }
  });

  // ── 2FA 管理：发送开关验证码（已登录；只发到**本人**绑定邮箱，不接受 body 指定邮箱，
  //    否则登录态即可给任意地址发信，变成邮件轰炸入口）──────────────────────────
  app.post('/api/v2/auth/2fa/send-code', authMiddleware, async (req, res) => {
    try {
      if (!codeLimiter(clientIpOf(req))) return rateLimited(res);
      const user = rowToObj(db.exec("SELECT email FROM users WHERE id = ?", [req.user.userId]));
      if (!user || !user.email) return res.status(400).json({ success: false, message: '请先绑定邮箱再管理双重验证' });
      const sent = await sendTwoFactorCode(user.email);
      if (!sent.ok) return res.status(sent.status || 500).json({ success: false, message: sent.message });
      res.json({ success: true, message: '验证码已发送，若未收到请检查垃圾邮件', data: { maskedEmail: sent.maskedEmail } });
    } catch (e) {
      console.error('[2FA] send-code 异常:', e.message);
      res.status(500).json({ success: false, message: '服务器繁忙，请稍后重试' });
    }
  });

  // ── 2FA 开关：开 / 关都要邮箱验证码 ─────────────────────────────────────────
  // 为什么关也要码：攻击者偷到会话后第一件事就是关 2FA。只给「开」加码等于没防。
  app.post('/api/v2/user/2fa', authMiddleware, (req, res) => {
    try {
      const enabled = req.body.enabled === true || req.body.enabled === '1' || req.body.enabled === 1;
      const user = rowToObj(db.exec("SELECT email FROM users WHERE id = ?", [req.user.userId]));
      if (!user || !user.email) return res.status(400).json({ success: false, message: '请先绑定邮箱再管理双重验证' });

      const ck = fetchValidCode(user.email, 'two_factor', String(req.body.code || '').trim());
      if (ck.locked) return res.status(429).json({ success: false, message: CODE_LOCKED_MSG });
      if (!ck.row) return res.status(401).json({ success: false, message: '验证码错误' });

      codeFailClear(user.email, 'two_factor');
      db.run("DELETE FROM verification_codes WHERE email = ? AND type = 'two_factor'", [user.email]);
      db.run("UPDATE users SET two_factor_enabled = ? WHERE id = ?", [enabled ? 1 : 0, req.user.userId]);
      saveDb();
      console.log(`[2FA] 用户 #${req.user.userId} ${req.user.username} 双重验证已${enabled ? '开启' : '关闭'}`);
      // 关闭 2FA 是账号被接管后攻击者的标准动作，这封邮件往往是唯一的线索
      sendAccountMail('twofa',
        { id: req.user.userId, username: req.user.username, email: user.email },
        { enabled }).catch(() => {});
      res.json({ success: true, message: enabled ? '双重验证已开启' : '双重验证已关闭', data: { twoFactorEnabled: enabled } });
    } catch (e) {
      console.error('[2FA] 开关异常:', e.message);
      res.status(500).json({ success: false, message: '服务器繁忙，请稍后重试' });
    }
  });

  // ====== Feedback（用户提交 → 邮件通知管理员） ======
  app.post('/api/v2/feedback', authMiddleware, async (req, res) => {
    // 2026-10-02 加固：async 处理器包 try/catch，防同步 db 异常变成 unhandled rejection 杀进程。
    try {
      const category = ['bug', 'suggestion', 'account', 'other'].includes(req.body.category) ? req.body.category : 'bug';
      const title = String(req.body.title || '').trim();
      const content = String(req.body.content || '').trim();
      const contact = String(req.body.contact || '').trim().slice(0, 120);

    if (!title) return res.status(400).json({ success: false, message: '请填写反馈标题' });
    if (title.length > 60) return res.status(400).json({ success: false, message: '标题最多 60 个字' });
    if (content.length < 5) return res.status(400).json({ success: false, message: '请把问题描述得再详细一点（至少 5 个字）' });
    if (content.length > 2000) return res.status(400).json({ success: false, message: '描述最多 2000 个字' });

    const recent = rowToObj(db.exec(
      "SELECT id FROM feedback WHERE user_id = ? AND created_at > datetime('now', '-60 seconds') ORDER BY id DESC LIMIT 1",
      [req.user.userId]));
    if (recent) return res.status(429).json({ success: false, message: '提交太频繁了，请 60 秒后再试' });

    const user = rowToObj(db.exec("SELECT username, email FROM users WHERE id = ?", [req.user.userId])) || {};
    db.run("INSERT INTO feedback (user_id, username, email, category, title, content, contact) VALUES (?, ?, ?, ?, ?, ?, ?)",
      [req.user.userId, user.username || req.user.username, user.email || '', category, title, content, contact]);
    const fb = rowToObj(db.exec("SELECT * FROM feedback WHERE id = last_insert_rowid()"));
    saveDb();
    logUsage(req.user.userId, req.user.username, '问题反馈', `#${fb.id} ${title}`);

    let mailed = false;
    if (getSetting('feedback_notify', '1') === '1') {
      try { await sendFeedbackEmail(fb); mailed = true; }
      catch (e) { console.error('[反馈邮件] 发送失败:', e.message); }
    }
    res.json({
      success: true,
      message: mailed ? `反馈已提交（编号 #${fb.id}），已发送到管理员邮箱` : `反馈已提交（编号 #${fb.id}），管理员可在控制台查看`,
      data: { id: fb.id, mailed, adminEmail: getAdminEmail() }
    });
    } catch (e) {
      console.error('[feedback] 异常:', e.message);
      if (!res.headersSent) res.status(500).json({ success: false, message: '服务器内部错误' });
    }
  });

  app.get('/api/v2/feedback/mine', authMiddleware, (req, res) => {
    res.json({ success: true, data: rowsToArr(db.exec(
      "SELECT id, category, title, content, status, reply, created_at FROM feedback WHERE user_id = ? ORDER BY id DESC LIMIT 50",
      [req.user.userId])) });
  });

  // ====== Feedback（管理端） ======
  app.get('/api/v2/feedback', authMiddleware, adminMiddleware, (req, res) => {
    const limit = Math.min(parseInt(req.query.limit) || 100, 300);
    const status = req.query.status;
    const data = status
      ? rowsToArr(db.exec("SELECT * FROM feedback WHERE status = ? ORDER BY id DESC LIMIT ?", [status, limit]))
      : rowsToArr(db.exec("SELECT * FROM feedback ORDER BY id DESC LIMIT ?", [limit]));
    const openCount = rowToObj(db.exec("SELECT COUNT(*) AS cnt FROM feedback WHERE status = 'open'"));
    res.json({ success: true, data, openCount: openCount?.cnt || 0 });
  });

  app.put('/api/v2/feedback/:id', authMiddleware, adminMiddleware, (req, res) => {
    const sets = [];
    const vals = [];
    if (req.body.status !== undefined) { sets.push("status = ?"); vals.push(String(req.body.status)); }
    if (req.body.reply !== undefined) { sets.push("reply = ?"); vals.push(String(req.body.reply).slice(0, 2000)); }
    if (!sets.length) return res.status(400).json({ success: false, message: '没有需要更新的内容' });
    sets.push("updated_at = datetime('now')");
    vals.push(req.params.id);
    db.run(`UPDATE feedback SET ${sets.join(', ')} WHERE id = ?`, vals);
    saveDb();
    res.json({ success: true, message: '反馈已更新' });
  });

  app.delete('/api/v2/feedback/:id', authMiddleware, adminMiddleware, (req, res) => {
    db.run("DELETE FROM feedback WHERE id = ?", [req.params.id]);
    saveDb();
    res.json({ success: true, message: '反馈已删除' });
  });

  // ====== Usage Logs ======
  app.post('/api/v2/usage/log', authMiddleware, (req, res) => {
    const { feature, detail } = req.body;
    if (!feature) return res.status(400).json({ success: false, message: 'feature必填' });
    logUsage(req.user.userId, req.user.username, feature, detail);
    res.json({ success: true });
  });

  app.get('/api/v2/usage/logs', authMiddleware, (req, res) => {
    const limit = parseInt(req.query.limit) || 100;
    const offset = parseInt(req.query.offset) || 0;
    const logs = rowsToArr(db.exec(
      "SELECT * FROM usage_logs WHERE user_id = ? ORDER BY created_at DESC LIMIT ? OFFSET ?",
      [req.user.userId, limit, offset]));
    const total = rowToObj(db.exec("SELECT COUNT(*) as cnt FROM usage_logs WHERE user_id = ?", [req.user.userId]));
    res.json({ success: true, data: { logs, total: total?.cnt || 0 } });
  });

  // ====== User: login history & announcement ======
  app.get('/api/v2/auth/login-logs', authMiddleware, (req, res) => {
    // 登录 IP **仅开发者可见**（2026-09-29）。这里跟 /api/logs 是**同一套做法**：
    // 按角色决定 SELECT 哪些列，非开发者拿到的对象里**根本没有 ip_address 这个键**
    // （抓包 / curl 也拿不到），控制台据「有没有这个键」决定要不要多渲染一列。
    //
    // ⚠️ 为什么这个「本人登录记录」接口也要给开发者开 IP：
    //    控制台有**两张**登录记录卡 —— 账户页「最近登录记录」（走本接口，只看自己）
    //    和管理页「登录日志」（走 /api/logs，看全站）。开发者自查「我最近从哪登的」
    //    时看的是前者，原来那里没有 IP 就等于「开发者看不到登录 IP」。
    const isDev = req.actorRole === 'developer';
    // login_app（登录应用来源，2026-10-15）：客户端登录与官网控制台登录共用端点，
    // 控制台两张登录表都靠这一列把「官网控制台」与「KunClient」的记录区分开。
    // 所有角色都下发 —— 它不是敏感字段（IP 才是），没有按角色裁剪的必要。
    const cols = isDev ? 'login_time, status, ip_address, login_app' : 'login_time, status, login_app';
    // 与本文件 /api/logs 保持一致：login_time 只有秒级精度，必须用 id 兜底排序。
    const logs = rowsToArr(db.exec(
      `SELECT ${cols} FROM login_logs WHERE username = ? ORDER BY login_time DESC, id DESC LIMIT 30`,
      [req.user.username]));
    res.json({ success: true, data: logs });
  });

  app.get('/api/v2/announcement', authMiddleware, (req, res) => {
    res.json({ success: true, data: { content: getSetting('announcement', '') } });
  });

  // ====== Stats & Logs (Admin) ======
  app.get('/api/v2/stats', authMiddleware, adminMiddleware, (req, res) => {
    const count = (sql) => { const r = db.exec(sql); return r.length ? r[0].values[0][0] : 0; };
    res.json({ success: true, data: {
      totalUsers: count("SELECT COUNT(*) FROM users"),
      activeUsers: count("SELECT COUNT(*) FROM users WHERE status = 'active'"),
      totalKeys: count("SELECT COUNT(*) FROM license_keys"),
      activeKeys: count("SELECT COUNT(*) FROM license_keys WHERE status = 'active'"),
      loginsToday: count("SELECT COUNT(*) FROM login_logs WHERE status = 'success' AND login_time > datetime('now', '-24 hours')"),
      failedLogins: count("SELECT COUNT(*) FROM login_logs WHERE status = 'failed' AND login_time > datetime('now', '-24 hours')"),
      totalUsage: count("SELECT COUNT(*) FROM usage_logs")
    }});
  });

  app.get('/api/v2/logs', authMiddleware, adminMiddleware, (req, res) => {
    const limit = parseInt(req.query.limit) || 100;
    // 登录 IP **仅开发者可见**（2026-09-29 用户要求「仅开发者可以看到登陆的 ip 地址」，隐私政策已同步）。
    //
    // 做法是**按角色决定 SELECT 哪些列**，而不是「照常返回、前端藏起来」：
    // 管理员仍然能看登录日志本身（时间 / 用户名 / 状态），但 ip_address 这一列**根本不下发** ——
    // 抓包或直接 curl 这个接口也拿不到。前端隐藏从来不是门槛，这里也一样。
    // 控制台则据「返回的对象里有没有 ip_address 字段」决定要不要多渲染一列，
    // 这样列的存在与否只有一个事实来源（服务端），不会出现前后端判断不一致。
    const isDev = req.actorRole === 'developer';
    // login_app（登录应用来源，2026-10-15）：同上，所有角色都下发。
    const cols = isDev
      ? 'id, user_id, username, login_time, status, ip_address, login_app'
      : 'id, user_id, username, login_time, status, login_app';
    // ⚠️ 排序必须带 id 兜底：login_time 是 DATETIME，**只有秒级精度** ——
    //    同一秒内的多条登录（注册后连点、探针连续登录、脚本批量登录）只按它排是**不稳定**的，
    //    同一份数据两次请求可能给出不同顺序。id 自增，天然是真实的插入顺序。
    res.json({ success: true, data: rowsToArr(db.exec(
      `SELECT ${cols} FROM login_logs ORDER BY login_time DESC, id DESC LIMIT ?`, [limit])) });
  });

  app.get('/api/v2/health', (req, res) => res.json({ status: 'ok' }));

  // ====== 官网对外口径（公开，无需登录）======
  // 官网上「使用许可 / 是否收费」的文案不再写死在前端，而是由控制台一个开关决定：
  // 公测期间显示免费，正式收费后切一下即可，官网无需重新构建、无需重新部署。
  const SITE_STAGES = ['beta', 'paid', 'free'];
  app.get('/api/v2/site/info', (req, res) => {
    // 2026-10-20：付费下线后对外口径固定为「免费」，不再读 site_stage 里的 paid 分支。
    // 保留 stage 字段是为了官网不必重新构建（STAGE_COPY 里有 free 这一档）。
    const stage = 'free';
    res.json({
      success: true,
      data: {
        stage,                                     // 恒为 free
        freeNow: true,
        paidMode: false,
        vipRequired: false,
        trialDays: 0,
        priceNote: '',
        version: getSetting('version', '1.0.1'),
        // 官网下载开关（控制台 运营 › 官网展示）：false = 收起下载区、首屏下载按钮与全部版本号。
        // 🔴 内测阶段优先级更高：开了内测一律 false（全站取消下载），不看单项开关。
        downloadEnabled: !isInternalStage() && getSetting('site_download_enabled', '1') !== '0'
      }
    });
  });

  // Test email endpoint（2026-10-02 收权）
  // 原来完全公开：任何人都能借我们的 SMTP 任意发信（伪造发件人/钓鱼跳板），
  // 且收件人走 query 参数想发给谁发谁。现收为仅开发者可调，收件人固定为
  // 管理员邮箱（query.email 仅在管理员自己的邮箱上做变体时无效——直接忽略外部传入）。
  app.get('/api/v2/test-email', authMiddleware, developerMiddleware, async (req, res) => {
    try {
      const result = await sendEmail(getAdminEmail(), '123456', 10);
      res.json({ success: result, message: result ? '邮件发送成功（收件人=管理员邮箱）' : '邮件发送失败' });
    } catch (err) {
      res.json({ success: false, message: err.message });
    }
  });

  // ====== App Update Check API ======
  // 🔴 本接口**不受**版本闸门限制（CLIENT_GATED_PATHS 里没有它）——
  //    它是旧版的升级入口，被拦掉的话用户既用不了、又查不到该升到哪。
  app.get('/api/v2/app/version', (req, res) => {
    // 「更新鉴权」2026-10-12：本端点不能做签名**硬**校验（上面的硬约束），
    // 但仍单独限流（防轮询），并对可信客户端做**渐进增强** —— 见 verified / downloadTicket。
    if (!versionLimiter(clientIpOf(req))) return rateLimited(res, 60);

    // 双轨分发（2026-10-12）：exe 安装版 + 7z 免安装版。7z 链接由 exe URL 换后缀派生
    // （与官网下载区同规则，见 tools/check-out.js），不额外存一个 app_settings 键 ——
    // 多一个键就多一处「忘了同步」的机会。sizeText 从磁盘 stat 出来，控制台/官网直接显示。
    // 🔴 内测阶段 / 云端更新关闭：**不下发任何下载地址**（downloadUrl/zipUrl 为空）——
    //    客户端 checkForUpdate 见 downloadUrl 为空即 return false（不弹更新窗），
    //    这是避免「force_update=1 + /assets 403 → 存量客户端被锁死」的关键。
    //    两者是**正交**的两级：internal_stage 管下载、cloud_update_enabled 管自更新（见 isCloudUpdateOpen）。
    const internal = isInternalStage();
    const updateOpen = isCloudUpdateOpen();
    const dlUrl = updateOpen ? getSetting('download_url', `${cfg.publicUrl}/assets/KunClient.exe`) : '';
    // sigGuard 对非保护路径也验一次签名 + 查一次登录态（验过即打 req.v2Signed / req.v2LoggedIn）。
    // 仅当「v2 签名**且**已登录」才签发下载票据 —— 2026-10-13：下载要求已登录，
    // 匿名/过期/被踢的 token 一律拿不到票；不带签名 ⇒ 旧客户端，其余字段照旧返回（升级链路不断）。
    const verified = req.v2Signed === true && req.v2LoggedIn === true;
    const dlFile = downloadAllowlist()[0] || '';
    const ticket = (verified && dlFile && updateOpen) ? signDownloadTicket(dlFile) : '';
    res.json({
      success: true,
      data: {
        version: getSetting('version', '1.0.1'),
        forceUpdate: getSetting('force_update', '1') === '1',
        // 硬闸门底线：客户端据此提前提示「必须升级」，不必等撞上 426 才知道。
        minVersion: minClientVersion(),
        clientVersion: clientVersionOf(req),   // 回显服务端识别到的版本，便于排查 UA 未生效
        buildDate: getSetting('build_date', '2026-09-13'),
        // ── 云端更新通道状态（2026-10-14）：客户端据此区分「暂无更新」与「通道被关」──
        //    updateEnabled=false 时客户端应显示「内测中 / 暂不开放更新」，而不是「已是最新版本」。
        updateEnabled: updateOpen,
        updateClosedReason: updateOpen ? '' : (internal ? 'internal_stage' : 'cloud_update_disabled'),
        downloadUrl: dlUrl,
        zipUrl: updateOpen && /\.exe(\?|$)/i.test(dlUrl) ? dlUrl.replace(/\.exe(\?|$)/i, '.7z$1') : '',
        sizeText: updateOpen ? packageSizeText(dlUrl) : '',
        changelog: getSetting('changelog', ''),
        toolExpire: getSetting('tool_expire', ''),
        // ── 更新鉴权（2026-10-12 ~ 10-13）──
        verified: verified,                            // 本请求是否已通过 v2 签名 + 登录
        downloadTicket: ticket,                        // 空串 = 未签名/未登录（拿不到）
        ticketExpiresIn: ticket ? DL_TICKET_TTL / 1000 : 0,
        // 客户端据此知道：不带登录态拿不到票，需登录后再检查更新才能走鉴权下载。
        ticketRequiresLogin: true,
      }
    });
  });

  // ====== 鉴权下载（2026-10-12「更新鉴权」）======
  // 路径 /api/v2/app/download/<文件名>?t=<票据> —— **文件名必须留在路径里**：
  // 客户端 isExeUrl() 靠 ".exe" 后缀决定走「exe 流程」还是「zip 解压流程」，
  // 它已先切掉 ?query 再判后缀，所以这种形态对旧客户端同样安全。
  // 无票据 / 过期 / 文件名不符 → 403；文件名必须逐字命中白名单（防目录穿越）。
  app.get('/api/v2/app/download/:file', (req, res) => {
    // 内测阶段 / 云端更新关闭：带票下载同样一律拒绝（与 /assets 直链一个口径）。
    if (!isCloudUpdateOpen()) {
      res.setHeader('Cache-Control', 'no-store');
      return res.status(403).json({
        success: false,
        code: isInternalStage() ? 'INTERNAL_STAGE' : 'CLOUD_UPDATE_DISABLED',
        message: isInternalStage() ? '内测阶段暂不开放下载。' : '云端更新已暂时关闭，暂不提供自更新下载。',
      });
    }
    const want = decodeURIComponent(String(req.params.file || ''));
    if (!downloadAllowlist().includes(want)) {
      return res.status(403).json({ success: false, code: 'DL_FILE_NOT_ALLOWED', message: '不允许下载该文件。' });
    }
    const r = verifyDownloadTicket(req.query.t);
    if (!r || r.file !== want) {
      return res.status(403).json({
        success: false, code: 'DL_TICKET_INVALID',
        message: '下载票据无效或已过期，请重新检查更新后再下载。',
      });
    }
    res.sendFile(path.join(__dirname, 'assets', want),
      { maxAge: '4h', lastModified: true }, (err) => {
        if (err && !res.headersSent) res.status(404).json({ success: false, message: '安装包不存在。' });
      });
  });

  // ====== 会员额度（公开接口：客户端放行判定与官网权益表都读它） ======
  // 不含任何用户隐私，故无需鉴权；配置本身受 /api/admin/settings 保护。
  app.get('/api/v2/app/limits', (req, res) => {
    // modules 与额度一起下发：客户端要知道「这些功能归属哪个模块、哪些等级已包含」，
    // 才能在锁定时说出「需开通模块3：专项与工具」，而不是笼统的「该功能不可用」。
    // 模块的 name/title/includedLevels 都是配置项、不含隐私，所以与额度一样无需鉴权。
    res.json({ success: true, data: Object.assign({}, getMemberLimits(), getModules()) });
  });

  // ====== 功能额度的服务端裁决（需登录）======
  // 这三个接口是「额度不可绕过」的落点：客户端不再自己算次数，而是每次动作前问这里。
  // 返回一律 200 + {success:true, data:{allowed,...}}（额度用尽不是 HTTP 错误）；
  // 只有「未登录 / 未知功能项」才是 4xx，方便客户端区分「该提示什么」。
  const featureKeyValid = k => FEATURE_CATALOG.some(f => f.key === k);

  // 只校验不扣费：「先检查、用户确认后再扣」的两段式流程用它，避免用户取消也吃掉额度。
  app.post('/api/v2/app/check', authMiddleware, (req, res) => {
    const featureKey = String((req.body && req.body.featureKey) || '').trim();
    if (!featureKeyValid(featureKey)) return res.status(400).json({ success: false, message: '未知功能项' });
    const user = loadQuotaUser(req.user.userId);
    if (!user) return res.status(401).json({ success: false, message: '账号不存在或已被删除' });
    res.json({ success: true, data: quotaDecision(user, featureKey, false) });
  });

  // 校验并原子扣费：按钮点击本身就是确认的场景用它。
  app.post('/api/v2/app/consume', authMiddleware, (req, res) => {
    const featureKey = String((req.body && req.body.featureKey) || '').trim();
    if (!featureKeyValid(featureKey)) return res.status(400).json({ success: false, message: '未知功能项' });
    const user = loadQuotaUser(req.user.userId);
    if (!user) return res.status(401).json({ success: false, message: '账号不存在或已被删除' });
    res.json({ success: true, data: quotaDecision(user, featureKey, true) });
  });

  // 当日各功能的用量快照：客户端用来画「今日剩余 x 次」，纯展示。
  app.get('/api/v2/app/quota', authMiddleware, (req, res) => {
    const user = loadQuotaUser(req.user.userId);
    if (!user) return res.status(401).json({ success: false, message: '账号不存在或已被删除' });
    const day = quotaDayKey();
    const features = {};
    FEATURE_CATALOG.forEach(f => {
      const q = featureQuotaFor(user, f.key);
      const used = usageToday(user.id, f.key, day);
      features[f.key] = {
        open: q.open, openByLevel: q.openByLevel, cap: q.cap, used,
        remaining: q.cap > 0 ? Math.max(0, q.cap - used) : -1,
        // 前端据此把「模块没开通」与「等级不开放」分开显示（一个要买模块，一个要升会员）
        module: q.module ? modulePubInfo(q.module) : null,
        moduleOk: q.moduleOk,
      };
    });
    res.json({ success: true, data: { day, level: effectiveVipLevel(user), features } });
  });

  // ====== 客户端环境检测 → 分级自动封禁（2026-09-30）======
  // 说明见 isBanActive 上方那一大段注释（三条底线：不主动踢人 / 弱信号不封 / HMAC 失败要熔断）。

  // 多开检测用的内存表：userId → { instanceId → 时间戳 }。
  // 只进内存不落库 —— 每请求 saveDb() 是整库重写，写放大不可接受（与 LAST_SEEN 同一套取舍）。
  const ENV_INSTANCES = new Map();

  // 防伪校验题期望答案（2026-10-01）：nonce → { v: 期望答案, t: 时间戳 }。
  // 同样只进内存不落库（避免给 env_challenges 加列做迁移）；nonce 一次性，用后即删。
  const ENV_CHALLENGE_ANSWERS = new Map();
  function pruneChallengeAnswers() {
    const now = Date.now();
    for (const [k, o] of ENV_CHALLENGE_ANSWERS) if (now - o.t > 3600000) ENV_CHALLENGE_ANSWERS.delete(k);
  }

  /** 1) 下发一次性 nonce + **防伪校验题**：客户端必须用内嵌密钥做 HMAC，且把校验题真的算一遍。
   *  puzzle 形如 "a op b"，客户端算出结果拼上 ENV_PUZZLE_SALT 作为答案回传。
   *  这样「抓包回放」拿不到新题答案，门槛从「复制一个包」抬到「跑原版客户端」。 */
  app.post('/api/v2/client/env/challenge', authMiddleware, (req, res) => {
    pruneChallengeAnswers();
    const nonce = crypto.randomBytes(24).toString('hex');
    // 2026-10-02 清理残留：签发后从未回验的 nonce 会一直躺在表里（每行一次 saveDb 写放大）。
    // 下发新 nonce 前顺带清掉：本用户的旧 nonce（一人只保留最新一枚）+ 全表超过 30 分钟的陈货。
    db.run("DELETE FROM env_challenges WHERE user_id = ?", [req.user.userId]);
    // 2026-10-02 修复：created_at 写的是 ISO（含 'T'），与 datetime('now') 的空格格式做
    // 字符串比较时同日行永远更大 → 30 分钟清扫只删得到前一天的情货。datetime() 归一化两侧。
    db.run("DELETE FROM env_challenges WHERE datetime(created_at) < datetime('now', '-30 minutes')");
    db.run("INSERT OR REPLACE INTO env_challenges (nonce, user_id, created_at) VALUES (?, ?, ?)",
      [nonce, req.user.userId, new Date().toISOString()]);
    saveDb();

    const a = 2 + (crypto.randomBytes(2).readUInt16BE(0) % 998);
    const b = 2 + (crypto.randomBytes(2).readUInt16BE(0) % 998);
    const ops = ['+', '-', '*'];
    const op = ops[crypto.randomBytes(1)[0] % ops.length];
    const r = op === '+' ? a + b : (op === '-' ? a - b : a * b);
    const puzzle = `${a} ${op} ${b}`;
    ENV_CHALLENGE_ANSWERS.set(nonce, { v: String(r) + ENV_PUZZLE_SALT, t: Date.now() });

    res.json({ success: true, data: { nonce, puzzle } });
  });

  /**
   * 2) 上报环境问题。
   * body: { nonce, payload, sig, }
   *   payload 是**待签名的原文字符串**（客户端自己拼好 JSON 后把整串发来），
   *   sig = HMAC-SHA256(key, nonce + "|" + payload)
   *
   * 🔴 为什么让客户端把 payload 原文一起发来，而不是服务端自己 JSON.stringify 再验：
   *    键序 / 空格 / 转义任何一处不一致，就会**全体用户**签名校验失败 ——
   *    那正是「我们自己的 bug」而非「用户作弊」。让客户端保留原文，是消除这类系统性失配最省事的办法。
   */
  app.post('/api/v2/client/env/report', authMiddleware, (req, res) => {
    const body = req.body || {};
    const nonce = String(body.nonce || '');
    const payload = String(body.payload || '');
    const sig = String(body.sig || '');
    if (!nonce || !payload || !sig)
      return res.status(400).json({ success: false, message: '缺少 nonce / payload / sig' });
    if (payload.length > 4000)
      return res.status(400).json({ success: false, message: '上报内容过长' });

    const user = loadActorById(req.user.userId);
    if (!user) return res.status(401).json({ success: false, message: '账号不存在' });

    // nonce 一次性：不存在 / 不属于本人 = HIGH（重放或伪造）
    const cr = db.exec("SELECT user_id FROM env_challenges WHERE nonce = ?", [nonce]);
    if (!cr.length || !cr[0].values.length || Number(cr[0].values[0][0]) !== Number(user.id)) {
      const r1 = evaluateEnvSignal(user, 'nonce_invalid', '上报凭据无效（重复或过期）', '', clientIpOf(req));
      saveDb();
      return res.json({ success: true, data: { accepted: false, results: [r1] } });
    }
    db.run("DELETE FROM env_challenges WHERE nonce = ?", [nonce]);   // 用后即删

    let parsed;
    try { parsed = JSON.parse(payload); }
    catch { parsed = null; }
    if (!parsed || typeof parsed !== 'object')
      return res.status(400).json({ success: false, message: 'payload 不是合法 JSON' });

    const results = [];
    const build = String(parsed.build || '').slice(0, 40);
    const ip = clientIpOf(req);

    // 防伪校验：答案必须等于服务端为本题生成的结果。
    // 🔴 空答案**不判** challenge_fail —— 旧版本客户端（1.0.14 及更早）根本不会发 answer，
    //    若把「没答题」也算失败，会成批误封老客户端。只有「答了但答错」才是伪造/篡改特征。
    const exp = ENV_CHALLENGE_ANSWERS.get(nonce);
    ENV_CHALLENGE_ANSWERS.delete(nonce);
    const answer = String(body.answer || '');
    if (exp && answer && answer !== exp.v)
      results.push(evaluateEnvSignal(user, 'challenge_fail', '客户端校验未通过（版本不对或被修改）', build, ip));

    // 签名校验
    // 2026-10-02 修复：原先用「字符数」截断/补零对齐长度，任何含多字节字符的 sig 编码成
    // UTF-8 后超过 want 的字节数，timingSafeEqual 直接抛 RangeError（Express 4 → 500）。
    // 现在先验格式：HMAC-SHA256 的签名必为 64 位小写 hex；不合格式直接按校验失败处理，
    // 合格式时两个 Buffer 才必然等长，timingSafeEqual 不可能抛。
    const want = envSign(nonce, payload);
    const gotSig = String(sig || '').toLowerCase();
    const sigOk = /^[0-9a-f]{64}$/.test(gotSig)
      && crypto.timingSafeEqual(Buffer.from(want, 'utf8'), Buffer.from(gotSig, 'utf8'));
    if (!sigOk) {
      results.push(evaluateEnvSignal(user, 'bad_hmac', '上报数据校验失败（疑似被篡改）', build, ip));
    } else {
      // 多开：同账号 60 秒内出现 ≥ 阈值 个不同实例标识（服务端自判，不靠客户端自述）
      const instId = String(parsed.instanceId || '');
      if (instId) {
        const now = Date.now();
        let m = ENV_INSTANCES.get(Number(user.id));
        if (!m) { m = new Map(); ENV_INSTANCES.set(Number(user.id), m); }
        m.set(instId, now);
        for (const [k, t] of m) if (now - t > 60000) m.delete(k);
        const need = parseInt(getSetting('env_multi_instance_threshold', '3'), 10) || 3;
        if (m.size >= need)
          results.push(evaluateEnvSignal(user, 'multi_instance', `60 秒内 ${m.size} 个实例`, build, ip));
      }
      // 客户端自述的信号（级别由 ENV_SIGNAL_LEVEL 决定，LOW 只记录）
      const list = Array.isArray(parsed.signals) ? parsed.signals.slice(0, 20) : [];
      for (const s of list) {
        const signal = String((s && s.signal) || '');
        if (!signal) continue;
        results.push(evaluateEnvSignal(user, signal,
          String((s && s.detail) || '').slice(0, 200), build, ip));
      }
    }
    saveDb();

    const ban = results.find(r => r.banned) || activeBanOf(loadActorById(user.id));
    res.json({
      success: true,
      data: {
        accepted: true,
        results,
        banned: !!(ban && ban.banned !== false && ban.until),
        until: (ban && ban.until) || null,
      },
    });
  });

  // ══════════════════════════════════════════════════════════════════════════
  // 免登录环境上报（2026-10-09）
  //
  // 为什么需要它：用户要求「工具一打开就开始检测」。客户端在**登录之前**就已经能看出
  // 中间人环境了（抓包进程在跑、系统代理被改、根证书被信任 —— 这三件事与登录无关），
  // 而那时候没有 token。不给一条上报通道，这段检测就只能躺在本地，什么也拦不住。
  //
  // 🔴 与登录版的三个不同，都是刻意的：
  //   ① nonce 按 **IP** 绑定（登录版按 user_id）—— 让「换个账号回放同一份包」失效；
  //   ② 处置按 IP（写 banned_ips + 邮件告警），不走 mid_strike 累积（那字段在 users 表上）；
  //   ③ 走**独立的限流桶** —— 登录版靠 token 天然有门槛，这个接口完全公开，必须自己防刷。
  // ══════════════════════════════════════════════════════════════════════════
  const ANON_ENV_CHALLENGES = new Map();                     // nonce -> { ip, v, t }
  // ⚠️ makeLimiter 返回的是**函数本身**（自带 .refund），不是带 .hit 的对象 ——
  //    调用写成 limiter.hit(ip) 会直接 TypeError → 500（本机实测踩到过）。
  const anonEnvLimiter = makeLimiter(10, 10 * 60 * 1000);    // 同 IP 10 分钟最多 10 次

  app.post('/api/v2/client/env/challenge-anon', (req, res) => {
    const ip = clientIpOf(req) || 'unknown';
    if (!anonEnvLimiter(ip))
      return res.status(429).json({ success: false, message: '请求过于频繁，请稍后再试' });

    const now = Date.now();
    for (const [k, o] of ANON_ENV_CHALLENGES) if (now - o.t > 3600000) ANON_ENV_CHALLENGES.delete(k);

    const nonce = crypto.randomBytes(24).toString('hex');
    const a = 2 + (crypto.randomBytes(2).readUInt16BE(0) % 998);
    const b = 2 + (crypto.randomBytes(2).readUInt16BE(0) % 998);
    const ops = ['+', '-', '*'];
    const op = ops[crypto.randomBytes(1)[0] % ops.length];
    const r = op === '+' ? a + b : (op === '-' ? a - b : a * b);
    ANON_ENV_CHALLENGES.set(nonce, { ip, v: String(r) + ENV_PUZZLE_SALT, t: now });

    res.json({ success: true, data: { nonce, puzzle: `${a} ${op} ${b}` } });
  });

  app.post('/api/v2/client/env/anonymous', (req, res) => {
    const body = req.body || {};
    const nonce = String(body.nonce || '');
    const payload = String(body.payload || '');
    const sig = String(body.sig || '');
    const ip = clientIpOf(req) || 'unknown';
    if (!nonce || !payload || !sig)
      return res.status(400).json({ success: false, message: '缺少 nonce / payload / sig' });
    if (payload.length > 4000)
      return res.status(400).json({ success: false, message: '上报内容过长' });

    const exp = ANON_ENV_CHALLENGES.get(nonce);
    ANON_ENV_CHALLENGES.delete(nonce);   // 一次性，用后即删
    // nonce 必须存在、且必须是**本 IP** 签发的。不满足 = 重放或伪造：
    // 匿名场景下无法归因到人，所以只落流水、不做任何处置（封谁去？）。
    if (!exp || exp.ip !== ip) {
      recordEnvReport(0, '', 'high', 'nonce_invalid', '匿名上报：上报凭据无效（重复或过期）', '', ip);
      saveDb();
      return res.json({ success: true, data: { accepted: false, reason: 'nonce_invalid' } });
    }

    let parsed;
    try { parsed = JSON.parse(payload); }
    catch { parsed = null; }
    if (!parsed || typeof parsed !== 'object')
      return res.status(400).json({ success: false, message: 'payload 不是合法 JSON' });

    const results = [];
    const build = String(parsed.build || '').slice(0, 40);

    // 防伪校验：答错 = 客户端不是原版（与登录版同一口径；**空答案不判**，老客户端不发这个字段）
    const answer = String(body.answer || '');
    if (answer && answer !== exp.v)
      results.push(evaluateAnonEnvSignal(ip, 'challenge_fail', '匿名上报：客户端校验未通过（版本不对或被修改）', build));

    const want = envSign(nonce, payload);
    const gotSig = String(sig || '').toLowerCase();
    const sigOk = /^[0-9a-f]{64}$/.test(gotSig)
      && crypto.timingSafeEqual(Buffer.from(want, 'utf8'), Buffer.from(gotSig, 'utf8'));
    if (!sigOk) {
      results.push(evaluateAnonEnvSignal(ip, 'bad_hmac', '匿名上报：数据校验失败（疑似被篡改）', build));
    } else {
      const list = Array.isArray(parsed.signals) ? parsed.signals.slice(0, 20) : [];
      for (const s of list) {
        const signal = String((s && s.signal) || '');
        if (!signal) continue;
        results.push(evaluateAnonEnvSignal(ip, signal, String((s && s.detail) || '').slice(0, 200), build));
      }
    }
    saveDb();
    res.json({ success: true, data: { accepted: true, results } });
  });

  // ====== 客户端日志上传（2026-10-12） ======
  // 用户在客户端「设置 → 诊断与日志」主动上传。采集内容在客户端的同意弹窗里
  // 逐条列明（最近日志 + 版本/系统环境 + 账号名），用途 = 修工具，
  // **只有开发者能看**（下面的 /admin/client-logs* 全部 developerMiddleware）。
  //
  // 已登录才收（authMiddleware）：按账号归属记录，开发者才能定位与回访。
  // 客户端启动即要求登录，正常路径不会未登录打到这。
  //
  // 体量与客户端同口径：files ≤ 5、单个解 base64 后 ≤ 1MB、合计 ≤ 2MB。
  // 存的是客户端原始请求体（req.rawBody，本身就是自描述 JSON：
  // { build, note, meta, files:[{name,size,content(base64)}] }）——
  // 不重新编码，开发者下载到的就是客户端发出的原包。
  app.post('/api/v2/client/logs', authMiddleware, (req, res) => {
    const body = req.body || {};
    const build = String(body.build || '').slice(0, 40);
    const note = String(body.note || '').slice(0, 500);
    const files = Array.isArray(body.files) ? body.files : [];
    if (!files.length) return res.status(400).json({ success: false, message: '没有要上传的日志文件' });
    if (files.length > 5) return res.status(400).json({ success: false, message: '一次最多上传 5 个日志文件' });

    // 防刷：同账号 1 小时 5 份。正常用户手动点，一天也点不满 5 次。
    const recent = db.exec("SELECT COUNT(*) FROM client_logs WHERE user_id = ? AND datetime(created_at) > datetime('now', '-1 hour')", [req.user.userId]);
    const recentCount = recent.length ? Number(recent[0].values[0][0]) : 0;
    if (recentCount >= 5) return res.status(429).json({ success: false, message: '上传太频繁，请稍后再试（每小时最多 5 份）' });

    // 先校验再落盘：名字只允许 字母数字._-（防路径穿越），base64 逐个解。
    let total = 0;
    for (const f of files) {
      const name = String((f && f.name) || '').slice(0, 64);
      if (!/^[\w.-]{1,64}$/.test(name))
        return res.status(400).json({ success: false, message: '日志文件名不合法' });
      let buf;
      try { buf = Buffer.from(String((f && f.content) || ''), 'base64'); }
      catch { return res.status(400).json({ success: false, message: '日志内容不是合法 base64' }); }
      if (!buf.length) continue;   // 空文件跳过（客户端也可能传了个空日志）
      if (buf.length > 1024 * 1024)
        return res.status(400).json({ success: false, message: '单个日志文件不能超过 1MB' });
      total += buf.length;
      if (total > 2 * 1024 * 1024)
        return res.status(400).json({ success: false, message: '日志总大小不能超过 2MB' });
    }
    if (!req.rawBody || !req.rawBody.length)
      return res.status(400).json({ success: false, message: '上传内容为空' });

    try {
      // 先插行拿自增 id，再用 id 命名文件 —— 文件与行一一对应，删行即删文件。
      db.run("INSERT INTO client_logs (user_id, username, build, note, size, file, ip, status) VALUES (?, ?, ?, ?, 0, '', ?, 'new')",
        [req.user.userId, req.actor.username, build, note, clientIpOf(req)]);
      const idRow = db.exec("SELECT last_insert_rowid()");
      const logId = idRow.length ? Number(idRow[0].values[0][0]) : 0;
      const fileName = `client-log-${logId}.json`;
      fs.writeFileSync(path.join(CLIENT_LOG_DIR, fileName), req.rawBody);
      const size = req.rawBody.length;
      db.run("UPDATE client_logs SET file = ?, size = ? WHERE id = ?", [fileName, size, logId]);
      saveDb();
      console.log(`[ClientLogs] #${logId} 收到 ${req.actor.username} 的日志（${size} B，build ${build || '-'}）`);
      res.json({ success: true, message: '日志已上传，谢谢！', data: { id: logId, size } });
    } catch (e) {
      res.status(500).json({ success: false, message: '保存日志失败：' + e.message });
    }
  });

  // ====== 用户申诉（2026-09-30）======
  // 🔴 **免登录**：被封的人根本登不进去，要求登录的申诉入口等于不存在。
  //    代价是必须自己防刷：① 同一账号已有 pending 申诉 → 拒；② 7 天内已提交过 → 拒；
  //    ③ 同 IP 1 小时最多 5 条（内存计数）。
  // 🔴 **绝不自动解封**：否则攻击者提交一条申诉就绕过封禁，这功能就白做了。
  //    它只是把「我不服」连同证据流水送到管理员面前，由**人**来定。
  const APPEAL_IP_HITS = new Map();
  app.post('/api/v2/appeal', (req, res) => {
    const body = req.body || {};
    const username = String(body.username || '').trim();
    const content = String(body.content || '').trim();
    const contact = String(body.contact || '').trim().slice(0, 100);

    if (!username) return res.status(400).json({ success: false, message: '请填写账号名' });
    if (content.length < 10) return res.status(400).json({ success: false, message: '请说明具体情况（至少 10 个字）' });
    if (content.length > 1000) return res.status(400).json({ success: false, message: '申诉内容最多 1000 字' });

    // 同 IP 限流（内存，进程重启即清零 —— 这里宁可宽松，不能把真想申诉的人挡在门外）
    const ip = clientIpOf(req) || 'unknown';
    const now = Date.now();
    const hits = (APPEAL_IP_HITS.get(ip) || []).filter(t => now - t < 3600000);
    if (hits.length >= 5) { APPEAL_IP_HITS.set(ip, hits); return res.status(429).json({ success: false, message: '提交过于频繁，请稍后再试' }); }
    hits.push(now); APPEAL_IP_HITS.set(ip, hits);

    const r = db.exec("SELECT * FROM users WHERE username = ?", [username]);
    if (!r.length || !r[0].values.length)
      return res.status(404).json({ success: false, message: '账号不存在' });
    const user = rowToObj(r);

    const ban = activeBanOf(user);
    if (!ban)
      return res.status(400).json({ success: false, message: '该账号当前未被封禁，无需申诉' });

    const pend = db.exec("SELECT id FROM appeals WHERE user_id = ? AND status = 'pending' LIMIT 1", [user.id]);
    if (pend.length && pend[0].values.length)
      return res.status(409).json({ success: false, message: '已有一条待处理的申诉，请等待处理结果' });
    const recent = db.exec("SELECT id FROM appeals WHERE user_id = ? AND created_at > datetime('now', '-7 days') LIMIT 1", [user.id]);
    if (recent.length && recent[0].values.length)
      return res.status(409).json({ success: false, message: '7 天内已提交过申诉，不能重复提交' });

    db.run("INSERT INTO appeals (user_id, username, ban_reason, content, contact, ip) VALUES (?,?,?,?,?,?)",
      [user.id, user.username, String(ban.reason || ''), content, contact, ip]);
    saveDb();
    console.log(`[Appeal] 账号 #${user.id} ${user.username} 提交申诉（封禁原因：${ban.reason}）`);
    // 回执：提交完没回声是最糟的体验 —— 用户不知道到底送到没有，只能反复提交。
    sendAccountMail('appeal_ack', user, {}).catch(() => {});
    res.json({ success: true, message: '申诉已提交，我们会人工复核，请留意登录页的处理结果' });
  });

  // ====== Admin: 环境告警 / 申诉处理（2026-09-30）======
  // 仅开发者：这些是「能解封别人」的操作，误操作代价最高，权限收在最高层（与 IP 封禁同一套理由）。
  app.get('/api/v2/admin/env-reports', authMiddleware, developerMiddleware, (req, res) => {
    const limit = Math.min(parseInt(req.query.limit, 10) || 100, 500);
    const rows = rowsToArr(db.exec(
      "SELECT id, user_id, username, level, signal, detail, build, ip, created_at FROM env_reports ORDER BY id DESC LIMIT " + limit));
    res.json({ success: true, data: rows, autoBanEnabled: envAutoBanEnabled() });
  });

  // 自动封禁记录（2026-10-24，**仅开发者**）：env_bans 流水表。
  // 为什么不直接看 users.ban_reason：解封会把 ban_reason / ban_level 清空（见 unban 端点），
  // 历史就没了。这张表专门回答「这个号以前被封过几次、因为什么」，也是升级倍数的依据。
  app.get('/api/v2/admin/env-bans', authMiddleware, developerMiddleware, (req, res) => {
    const limit = Math.min(parseInt(req.query.limit, 10) || 100, 500);
    const rows = rowsToArr(db.exec(
      "SELECT b.id, b.user_id, u.username, b.level, b.days, b.reason, b.created_at " +
      "FROM env_bans b LEFT JOIN users u ON u.id = b.user_id ORDER BY b.id DESC LIMIT " + limit));
    // 近 90 天每账号次数：控制台用它标「第 N 次」（N≥2 就是升级过的）
    const counts90d = {};
    try {
      const r = db.exec("SELECT user_id, COUNT(*) FROM env_bans " +
        "WHERE datetime(created_at) > datetime('now', '-90 days') GROUP BY user_id");
      if (r.length) r[0].values.forEach(v => { counts90d[String(v[0])] = Number(v[1]); });
    } catch (e) { /* 表刚建时可能为空，忽略 */ }
    res.json({ success: true, data: rows, counts90d });
  });

  // ====== Admin: 封禁中心（2026-10-24，**仅开发者**）======
  // 把散在「安全」页（IP / 阈值）和「用户」页（解封）里的封禁操作收成一处：
  // 谁被封着、封到什么时候、手动封 / 解封、封禁邮件开关、最近自动封禁。
  // 🔴 永久禁用（status='banned'）仍然只在用户管理里做：那是「账号停用」，
  //    与「临时限制」语义不同，混在一起迟早有人点错。
  app.get('/api/v2/admin/bans/summary', authMiddleware, developerMiddleware, (req, res) => {
    const one = (sql) => {
      try {
        const r = db.exec(sql);
        return r.length && r[0].values.length ? Number(r[0].values[0][0]) : 0;
      } catch (e) { return 0; }
    };
    res.json({
      success: true,
      data: {
        timed: one("SELECT COUNT(*) FROM users WHERE banned_until IS NOT NULL AND datetime(banned_until) > datetime('now')"),
        permanent: one("SELECT COUNT(*) FROM users WHERE status='banned'"),
        ips: one("SELECT COUNT(*) FROM banned_ips WHERE expire_at IS NULL OR datetime(expire_at) > datetime('now')"),
        auto7d: one("SELECT COUNT(*) FROM env_bans WHERE datetime(created_at) > datetime('now', '-7 days')"),
        appeals: one("SELECT COUNT(*) FROM appeals WHERE status='pending'"),
        mailEnabled: getSetting('env_ban_mail_enabled', '1') === '1',
        ipHours: envBanIpHours(),
        autoBanEnabled: envAutoBanEnabled(),
      },
    });
  });

  app.get('/api/v2/admin/bans/accounts', authMiddleware, developerMiddleware, (req, res) => {
    const limit = Math.min(parseInt(req.query.limit, 10) || 200, 500);
    const rows = rowsToArr(db.exec(
      "SELECT u.id, u.username, u.email, u.status, u.role, u.banned_until, u.ban_reason, u.ban_level, " +
      "  (SELECT COUNT(*) FROM env_bans b WHERE b.user_id = u.id " +
      "     AND datetime(b.created_at) > datetime('now', '-90 days')) AS auto90 " +
      "FROM users u " +
      "WHERE u.status='banned' OR (u.banned_until IS NOT NULL AND datetime(u.banned_until) > datetime('now')) " +
      "ORDER BY (u.status='banned') DESC, u.banned_until ASC LIMIT " + limit));
    res.json({ success: true, data: rows });
  });

  // 手动封禁（有时限）。与自动封禁走同一条「到期自动恢复」链路，
  // 区别只在 ban_level='manual'（控制台的封禁类型筛选靠它区分）。
  app.post('/api/v2/admin/bans/account', authMiddleware, developerMiddleware, (req, res) => {
    const body = req.body || {};
    const key = String(body.user || '').trim();
    const days = parseInt(body.days, 10);
    if (!key) return res.status(400).json({ success: false, message: '请填账号名或用户 ID' });
    if (!Number.isFinite(days) || days < 1 || days > 36500)
      return res.status(400).json({ success: false, message: '天数需在 1~36500 之间（永久封禁请到「用户管理」里禁用）' });

    let user = null;
    try {
      user = /^\d+$/.test(key)
        ? loadActorById(parseInt(key, 10))
        : rowToObj(db.exec("SELECT * FROM users WHERE username=?", [key]));
    } catch (e) { user = null; }
    if (!user) return res.status(404).json({ success: false, message: '找不到该账号' });
    if (user.id === req.user.id)
      return res.status(403).json({ success: false, message: '不能封禁自己' });
    if (user.role === 'developer' || user.role === 'admin')
      return res.status(403).json({ success: false, message: '工作人员账号不参与封禁，请先降权' });

    const reason = (String(body.reason || '').trim() || '管理员手动封禁').slice(0, 200);
    const until = banUntilIso(days);
    db.run("UPDATE users SET banned_until=?, ban_reason=?, ban_level='manual' WHERE id=?",
      [until, reason, user.id]);
    // 立刻踢下线：不 +1 的话，已登录的会话要等 token 自然过期才失效
    db.run("UPDATE users SET session_epoch = COALESCE(session_epoch, 0) + 1 WHERE id=?", [user.id]);
    saveDb();
    console.log(`[BanCenter] ${req.user.username} 手动封禁 #${user.id} ${user.username} ${days} 天：${reason}`);
    // 通知用户（不 await：SMTP 可能几秒）
    sendBanMail(user, { reason, until, level: 'manual', days }).catch(() => {});
    res.json({ success: true, message: `已封禁 ${user.username} ${days} 天`, until });
  });

  // 封禁邮件模板自测（2026-10-24）：只发给**开发者自己的**告警邮箱，绝不发给用户。
  // 改完邮件模板点一下这里，比拿真实用户试错强一万倍。
  app.post('/api/v2/admin/bans/test-mail', authMiddleware, developerMiddleware, async (req, res) => {
    const to = devAlertEmail();
    if (!to) return res.status(400).json({ success: false, message: '未配置告警邮箱（dev_alert_email / admin_email）' });
    // kind：五种模板都能单独预览（封禁 / 解封 / 申诉回执 / 申诉通过 / 申诉驳回）。
    // 改完文案先在这里看一眼，别拿真实用户当试验品。
    const kind = String((req.body || {}).kind || 'ban');
    const ALL_KINDS = ['ban', 'unban', 'appeal_ack', 'appeal_ok', 'appeal_reject',
      'password_changed', 'password_reset_admin', 'email_changed', 'email_bound', 'twofa'];
    if (!ALL_KINDS.includes(kind))
      return res.status(400).json({ success: false, message: 'kind 只能是 ' + ALL_KINDS.join(' / ') });
    try {
      const r = await sendAccountMail(kind, { id: 0, username: '模板自测', email: to }, {
        reason: 'intercept_tool: 这是一封模板自测邮件，可忽略',
        until: banUntilIso(3),
        reply: '（这是测试用的处理回复，正式邮件里是人工填写的内容）',
        force: true,
      });
      if (!r || !r.ok) return res.json({ success: false, message: '发送失败，请看服务端日志' });
      res.json({
        success: true,
        message: `已提交到 ${to}（受理号 ${String(r.messageId).replace(/[<>]/g, '').slice(0, 18)}…）` +
          '。收不到先翻垃圾箱；仍没有就去阿里云「邮件推送 → 投递日志」用这个受理号查',
        messageId: r.messageId,
      });
    } catch (e) {
      res.status(500).json({ success: false, message: '发送异常：' + e.message });
    }
  });

  // ====== Admin: 客户端上传的日志（2026-10-12，**仅开发者**）======
  // 🔴 四个端点全是 developerMiddleware：管理员/代理看不到列表，也拿不到内容与文件。
  //    「仅开发者可见」是靠服务端鉴权保证的，不是把按钮藏起来。
  // IP 与登录日志同口径（仅开发者可见），所以这里可以放心回传。
  app.get('/api/v2/admin/client-logs', authMiddleware, developerMiddleware, (req, res) => {
    const limit = Math.min(parseInt(req.query.limit, 10) || 200, 500);
    const rows = rowsToArr(db.exec(
      "SELECT id, user_id, username, build, note, size, status, ip, created_at FROM client_logs ORDER BY id DESC LIMIT " + limit));
    const retentionDays = Math.max(1, parseInt(getSetting('client_log_retention_days', '30'), 10) || 30);
    res.json({ success: true, data: rows, retentionDays, newCount: rows.filter(r => r.status === 'new').length });
  });

  // 详情：回传 meta + 逐文件解码后的文本（预览用，>512KB 或解不出 UTF-8 的标记为 binary，
  // 完整内容走下载）。打开一次即把 new 置成 viewed —— 省掉一个「标记已读」按钮。
  app.get('/api/v2/admin/client-logs/:id', authMiddleware, developerMiddleware, (req, res) => {
    const id = parseInt(req.params.id, 10);
    if (!Number.isFinite(id)) return res.status(400).json({ success: false, message: '无效的日志编号' });
    const row = rowToObj(db.exec("SELECT * FROM client_logs WHERE id = ?", [id]));
    if (!row) return res.status(404).json({ success: false, message: '日志不存在或已删除' });
    if (!row.file) return res.status(404).json({ success: false, message: '日志文件缺失（可能已被清理）' });
    const file = path.join(CLIENT_LOG_DIR, path.basename(String(row.file)));
    if (!fs.existsSync(file)) return res.status(404).json({ success: false, message: '日志文件缺失（可能已被清理）' });

    let envelope = null;
    try { envelope = JSON.parse(fs.readFileSync(file, 'utf8')); } catch { envelope = null; }
    if (!envelope) return res.status(500).json({ success: false, message: '日志文件损坏，无法解析' });

    const files = (Array.isArray(envelope.files) ? envelope.files : []).map(f => {
      const name = String((f && f.name) || '');
      const size = Number((f && f.size) || 0);
      let text = '', binary = false;
      try {
        const buf = Buffer.from(String((f && f.content) || ''), 'base64');
        if (buf.length > 512 * 1024) { binary = true; }
        else {
          text = buf.toString('utf8');
          if (text.indexOf('\uFFFD') >= 0) { binary = true; text = ''; }   // 解出替换符 = 不是文本
        }
      } catch { binary = true; }
      return { name, size, text, binary };
    });

    if (row.status === 'new') { db.run("UPDATE client_logs SET status = 'viewed' WHERE id = ?", [id]); saveDb(); }
    res.json({ success: true, data: {
      id: row.id, username: row.username, build: row.build, note: row.note,
      ip: row.ip, size: row.size, createdAt: row.created_at,
      meta: (envelope.meta && typeof envelope.meta === 'object') ? envelope.meta : {},
      files,
    }});
  });

  // 下载：原样回传客户端发出的请求体（自描述 JSON）。文件名带编号便于归档。
  app.get('/api/v2/admin/client-logs/:id/download', authMiddleware, developerMiddleware, (req, res) => {
    const id = parseInt(req.params.id, 10);
    if (!Number.isFinite(id)) return res.status(400).json({ success: false, message: '无效的日志编号' });
    const row = rowToObj(db.exec("SELECT file FROM client_logs WHERE id = ?", [id]));
    if (!row || !row.file) return res.status(404).json({ success: false, message: '日志不存在或已删除' });
    const file = path.join(CLIENT_LOG_DIR, path.basename(String(row.file)));
    if (!fs.existsSync(file)) return res.status(404).json({ success: false, message: '日志文件缺失（可能已被清理）' });
    res.download(file, `kunclient-log-${id}.json`);
  });

  // 删除：文件与元数据一起删（留下孤儿的文件定期清理也会兜住，但不留垃圾更干净）。
  app.delete('/api/v2/admin/client-logs/:id', authMiddleware, developerMiddleware, (req, res) => {
    const id = parseInt(req.params.id, 10);
    if (!Number.isFinite(id)) return res.status(400).json({ success: false, message: '无效的日志编号' });
    const row = rowToObj(db.exec("SELECT file FROM client_logs WHERE id = ?", [id]));
    if (!row) return res.status(404).json({ success: false, message: '日志不存在或已删除' });
    if (row.file) { try { fs.unlinkSync(path.join(CLIENT_LOG_DIR, path.basename(String(row.file)))); } catch {} }
    db.run("DELETE FROM client_logs WHERE id = ?", [id]);
    saveDb();
    res.json({ success: true, message: '已删除' });
  });

  app.get('/api/v2/admin/appeals', authMiddleware, developerMiddleware, (req, res) => {
    const rows = rowsToArr(db.exec(
      "SELECT id, user_id, username, ban_reason, content, contact, status, reply, ip, created_at, handled_at FROM appeals ORDER BY id DESC LIMIT 200"));
    const data = rows.map(a => Object.assign({}, a, { pending: a.status === 'pending' }));
    res.json({ success: true, data, pendingCount: data.filter(a => a.pending).length });
  });

  // body: { action: 'approve' | 'reject', reply? }
  app.post('/api/v2/admin/appeals/:id/resolve', authMiddleware, developerMiddleware, (req, res) => {
    const id = parseInt(req.params.id, 10);
    const action = String((req.body || {}).action || '');
    const reply = String((req.body || {}).reply || '').slice(0, 500);
    if (!['approve', 'reject'].includes(action))
      return res.status(400).json({ success: false, message: 'action 只能是 approve 或 reject' });

    const r = db.exec("SELECT * FROM appeals WHERE id = ?", [id]);
    if (!r.length || !r[0].values.length) return res.status(404).json({ success: false, message: '申诉不存在' });
    const ap = rowToObj(r);

    // 申诉对应的账号：通过与驳回都要用它发结果邮件，所以提到 if 外面
    const u = loadActorById(ap.user_id);

    if (action === 'approve') {
      // 🔴 管理员手工的永久封禁**不由申诉通道解封** —— 那是人工终审的结果，
      //    让申诉把 status='banned' 清掉等于绕过管理员。
      if (u && u.status === 'banned')
        return res.status(403).json({ success: false, message: '该账号为管理员手工永久封禁，申诉通道不处理，请直接由管理员在用户管理里改状态' });
      db.run("UPDATE users SET banned_until=NULL, ban_reason='', ban_level='', mid_strike=0, mid_strike_day='' WHERE id=?",
        [ap.user_id]);
    }
    db.run("UPDATE appeals SET status=?, reply=?, handled_by=?, handled_at=? WHERE id=?",
      [action === 'approve' ? 'approved' : 'rejected', reply, req.user.userId, new Date().toISOString(), id]);
    saveDb();
    const msg = action === 'approve' ? '已通过申诉并解除封禁' : '已驳回申诉';
    console.log(`[Appeal] #${id} ${msg}（处理人 ${req.user.username}）`);
    // 结果邮件：通过和驳回都要发 —— 用户等的就是这个结论，只写进控制台等于没告诉他。
    // 注意「通过」这一封里已经写了「已解封」，所以这里**不再**额外发解封通知（避免两封重复）。
    if (u) {
      sendAccountMail(action === 'approve' ? 'appeal_ok' : 'appeal_reject', u, {
        reply,
        reason: ap.ban_reason,
        until: action === 'reject' ? u.banned_until : '',
      }).catch(() => {});
    }
    res.json({ success: true, message: msg });
  });

  // 删除申诉记录（仅开发者）：把申诉流水从控制台列表里抹掉。
  // ⚠️ 只删 appeals 表的这一行，**不动被封账号本身** ——
  //    要解封走 resolve(approve) 或用户管理的「解封」，删除不等于放行。
  app.delete('/api/v2/admin/appeals/:id', authMiddleware, developerMiddleware, (req, res) => {
    const id = parseInt(req.params.id, 10);
    if (!Number.isFinite(id)) return res.status(400).json({ success: false, message: 'id 不合法' });
    const r = db.exec("SELECT id, username, status FROM appeals WHERE id = ?", [id]);
    if (!r.length || !r[0].values.length) return res.status(404).json({ success: false, message: '申诉不存在' });
    const ap = rowToObj(r);
    db.run("DELETE FROM appeals WHERE id = ?", [id]);
    saveDb();
    console.log(`[Appeal] #${id} 申诉记录已删除（${ap.username || '-'}，原状态 ${ap.status}；操作人 ${req.user.username}）`);
    res.json({ success: true, message: '已删除该申诉记录' });
  });

  // 🔴 环境检测的设置**不并入** /api/admin/settings：那是 admin 级，
  //    而这组里有一个「自动封禁总开关」——误伤暴涨时靠它止血，**不能让管理员重开**。
  //    所以单开一组，权限收在 developer。
  const ENV_SETTING_KEYS = ['env_auto_ban_enabled', 'env_ban_high_days', 'env_ban_mid_days',
    'env_mid_strike_threshold', 'env_multi_instance_threshold', 'env_breaker_accounts',
    'env_ban_mail_enabled'];
  const ENV_SETTING_DEFAULT = { env_auto_ban_enabled: '1', env_ban_high_days: '7', env_ban_mid_days: '3',
    env_mid_strike_threshold: '3', env_multi_instance_threshold: '3', env_breaker_accounts: '5',
    env_ban_mail_enabled: '1' };

  app.get('/api/v2/admin/env-settings', authMiddleware, developerMiddleware, (req, res) => {
    const data = {};
    ENV_SETTING_KEYS.forEach(k => data[k] = getSetting(k, ENV_SETTING_DEFAULT[k]));
    res.json({ success: true, data, defaults: ENV_SETTING_DEFAULT });
  });

  app.put('/api/v2/admin/env-settings', authMiddleware, developerMiddleware, (req, res) => {
    const body = req.body || {};
    let n = 0, bad = null;
    ENV_SETTING_KEYS.forEach(k => {
      if (body[k] === undefined) return;
      const v = String(body[k]);
      if (k === 'env_auto_ban_enabled' || k === 'env_ban_mail_enabled') {
        if (!['0', '1'].includes(v)) { bad = bad || `${k} 只能是 0 或 1`; return; }
      } else {
        const num = parseInt(v, 10);
        // 天数 1~30、阈值 1~20：给个硬边界，防止手滑填成 99999 天
        const max = k.includes('days') ? 30 : 20;
        if (!Number.isFinite(num) || num < 1 || num > max) { bad = bad || `${k} 需在 1~${max} 之间`; return; }
      }
      setSetting(k, v); n++;
    });
    if (bad) return res.status(400).json({ success: false, message: bad });
    saveDb();
    console.log(`[EnvSettings] ${req.user.username} 修改了 ${n} 项环境检测设置`);
    res.json({ success: true, message: `已保存 ${n} 项设置` });
  });

  // 管理员直接解封（不经过申诉）：同样**只清时限封禁**，不动手工永久封禁
  app.post('/api/v2/admin/users/:id/unban', authMiddleware, developerMiddleware, (req, res) => {
    const id = parseInt(req.params.id, 10);
    const u = loadActorById(id);
    if (!u) return res.status(404).json({ success: false, message: '用户不存在' });
    if (u.status === 'banned')
      return res.status(403).json({ success: false, message: '该账号是管理员手工永久封禁，请到用户管理里改状态' });
    if (!isBanActive(u.banned_until))
      return res.status(400).json({ success: false, message: '该账号当前没有生效的时限封禁' });
    db.run("UPDATE users SET banned_until=NULL, ban_reason='', ban_level='', mid_strike=0, mid_strike_day='' WHERE id=?", [id]);
    saveDb();
    console.log(`[Unban] 账号 #${id} ${u.username} 的时限封禁已解除（操作人 ${req.user.username}）`);
    // 解封也要通知：封禁时发了信，解封不发就等于让用户继续以为被封着（甚至一直不来用）。
    sendAccountMail('unban', u, {}).catch(() => {});
    res.json({ success: true, message: '已解除时限封禁' });
  });

  // ====== Admin: IP 封禁（2026-09-28）======
  // 名单存在 banned_ips 表；真正的拦截在文件顶部那个 `app.use('/api', ...)`，
  // 它对**所有** /api 请求生效（含登录、注册、发验证码）。下面这三个接口只负责管名单。

  /**
   * 归一化管理员输入的 IP（IPv4 或 IPv6），返回规范串；不合法返回 ''。
   * 直接复用 clientIpOf 用的那一个 `canonicalIp()` —— **录入与匹配必须是同一个函数**：
   * 两处各写一套的话，`2001:DB8::1` 录进去、请求来的是 `2001:db8::1`，就永远匹配不上，
   * 而且不报错（静默失效），管理员以为封上了其实什么都没封。
   * 🔴 IPv4 逐段 0~255 的校验也不能省（`1.2.3.999` 这种「看着像 IP 却永不匹配」最危险）——
   *    这一步现在由 `net.isIP()` + `canonicalIp` 一起保证：net.isIP 只认合法地址。
   */
  function normalizeIpInput(v) {
    return canonicalIp(v);
  }

  // 仅开发者（2026-09-29 用户要求「IP 封禁只有开发者能封禁」）。
  // ⚠️ 这里用 developerMiddleware 而不是 adminMiddleware：封禁是**按来源 IP**、会一次挡住
  //    同一出口（NAT / 公司网 / 手机热点）下的所有人，误封代价远高于其它后台操作，所以权限收在最高层。
  //    控制台那张卡同时改成 data-min-rank="3" 隐藏 —— 但**前端隐藏不是门槛**：直接打这个 API 一样要被拒。
  app.get('/api/v2/admin/banned-ips', authMiddleware, developerMiddleware, (req, res) => {
    const rows = rowsToArr(db.exec(
      "SELECT ip, reason, created_by, created_at, expire_at FROM banned_ips ORDER BY created_at DESC"));
    const data = rows.map(r => ({
      ip: r.ip,
      reason: r.reason || '',
      createdBy: r.created_by,
      createdAt: r.created_at || '',
      expireAt: r.expire_at || null,
      permanent: !r.expire_at,
      // 已过期但没删掉的记录仍然返回（让管理员看得见），只是标记为已失效
      expired: !!(r.expire_at && !isVipActive(r.expire_at)),
    }));
    // selfIp 只回给管理员本人，用于前端在「封的正好是自己」时弹二次确认。
    res.json({ success: true, data, selfIp: clientIpOf(req) });
  });

  // body: { ip, reason?, days?, force? }   days 缺省 / <= 0 = 永久
  // 仅开发者，理由同上（GET 那条注释）。
  app.post('/api/v2/admin/banned-ips', authMiddleware, developerMiddleware, (req, res) => {
    const body = req.body || {};
    const ip = normalizeIpInput(body.ip);
    if (!ip) return res.status(400).json({ success: false, message: '请输入合法的 IPv4 或 IPv6 地址（例如 1.2.3.4 或 240e:390:1:2::1）' });

    // 最容易被自己坑到的一步：封的正好是当前来源 IP → 这条请求之后立刻失联（连解封都点不了）。
    // 前端拿到 code=SELF_BAN 后弹二次确认，用户确认了再带 force=1 重来。
    const selfIp = clientIpOf(req);
    if (ip === selfIp && String(body.force || '') !== '1') {
      return res.status(409).json({
        success: false, code: 'SELF_BAN',
        message: `这个 IP（${ip}）正是你当前访问控制台的来源，封禁后你会立刻失去对控制台的访问，且无法在此处解封。`,
      });
    }

    const d = parseInt(body.days, 10);
    // days 缺省 / <=0 = 永久，用 NULL 表示（与 user_modules.expire_at 同一套语义，别写成 9999-12-31）
    const expireAt = (Number.isFinite(d) && d > 0) ? new Date(Date.now() + d * 86400000).toISOString() : null;
    const reason = String(body.reason || '').slice(0, 200);

    // 同一 IP 只保留一条（ip 是主键）：再次录入 = 覆盖（改原因 / 改时长都要能生效）。
    // 注意 created_at 也会跟着刷新成这次的时间 —— 这是有意的「重新封禁」语义。
    db.run("INSERT OR REPLACE INTO banned_ips (ip, reason, created_by, created_at, expire_at) VALUES (?, ?, ?, ?, ?)",
      [ip, reason, req.user.userId, new Date().toISOString(), expireAt]);
    saveDb();
    res.json({
      success: true,
      message: `${ip} 已封禁${expireAt ? `（至 ${expireAt.slice(0, 10)}）` : '（永久）'}`,
      data: { ip, expireAt },
    });
  });

  // 仅开发者，理由同上（GET 那条注释）。
  app.delete('/api/v2/admin/banned-ips/:ip', authMiddleware, developerMiddleware, (req, res) => {
    // IPv6 里冒号是合法字符，前端会 encodeURIComponent 后再拼进路径；
    // Express 已经解过一次码，这里再解一次要防「已解过的值里带 % 」（如 zone id 的 %25）抛 URIError。
    let raw = req.params.ip || '';
    try { raw = decodeURIComponent(raw); } catch { /* 已是解码后的值，原样用 */ }
    const ip = normalizeIpInput(raw);
    if (!ip) return res.status(400).json({ success: false, message: 'IP 不合法' });
    db.run("DELETE FROM banned_ips WHERE ip = ?", [ip]);
    saveDb();
    res.json({ success: true, message: `${ip} 已解封` });
  });

  // ====== Admin Settings (toggles / version info) ======
  // ⚠️ member_limits 已从名单移除（2026-09-28）：不再按等级限次，
  //    且 getMemberLimits() 已经完全不读它 —— 留在这里只会让控制台
  //    显示一个「看着能配、其实不起作用」的项。
  const ADMIN_SETTING_KEYS = ['login_mail_notify', 'version', 'download_url', 'build_date', 'changelog', 'force_update', 'min_client_version', 'env_auto_ban_enabled', 'env_ban_high_days', 'env_ban_mid_days', 'env_mid_strike_threshold', 'env_multi_instance_threshold', 'env_breaker_accounts', 'env_ban_ip_hours', 'env_ban_mail_enabled', 'dev_alert_email', 'client_log_retention_days', 'console_bg_color', 'admin_email', 'feedback_notify', 'paid_mode', 'vip_required', 'vip_trial_days', 'vip_price_note', 'site_stage', 'site_download_enabled', 'internal_stage', 'cloud_update_enabled', 'announcement', 'tool_expire', 'member_modules'];

  app.get('/api/v2/admin/settings', authMiddleware, adminMiddleware, (req, res) => {
    const data = {};
    ADMIN_SETTING_KEYS.forEach(k => data[k] = getSetting(k, ''));
    res.json({ success: true, data });
  });

  app.put('/api/v2/admin/settings', authMiddleware, adminMiddleware, (req, res) => {
    if (req.body.admin_email !== undefined && req.body.admin_email !== '' &&
        !/^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(req.body.admin_email))
      return res.status(400).json({ success: false, message: '管理员邮箱格式不正确' });
    let n = 0;
    ADMIN_SETTING_KEYS.forEach(k => {
      if (req.body[k] !== undefined) { setSetting(k, req.body[k]); n++; }
    });
    res.json({ success: true, message: `已保存 ${n} 项设置` });
  });

  // ====== Admin: Push version update email to all users ======
  // 手动群发，保留为「补发 / 重发」的逃生通道。正常发版会自动群发，见 maybeNotifyNewVersion。
  app.post('/api/v2/admin/push-update', authMiddleware, adminMiddleware, async (req, res) => {
    // 2026-10-02 加固：async 处理器包 try/catch，防同步 db 异常变成 unhandled rejection 杀进程。
    try {
      const version = req.body.version || getSetting('version', '1.0.1');
      const changelog = req.body.changelog || getSetting('changelog', '');
      const downloadUrl = req.body.downloadUrl || resolveDownloadUrl();
      const r = await pushUpdateEmailToAll(version, changelog, downloadUrl);
      if (r.total === 0) return res.json({ success: true, message: '没有可推送的用户（均未绑定邮箱）', data: r });
      res.json({ success: true, message: `推送完成：成功 ${r.sent} 个，失败 ${r.failed} 个`, data: r });
    } catch (e) {
      console.error('[push-update] 异常:', e.message);
      if (!res.headersSent) res.status(500).json({ success: false, message: '服务器内部错误' });
    }
  });

  // ====== Version update email template ======
  /**
   * 版本更新邮件（2026-10-12 重做：品牌视觉 + 双轨下载 + 版本元信息）。
   *
   * 入参保持 (user, version, changelog, downloadUrl) 不变 —— 手动补发端点 /api/v2/admin/push-update
   * 会传自己的 downloadUrl 覆盖值，所以体积与 7z 链接一律**从这个入参派生**，不去读 app_settings，
   * 否则手动端点指定的包和邮件里显示的信息会不一致。
   *   7z：由 exe URL 直接换后缀（与官网下载区的派生规则一致，见 tools/check-out.js）。
   *   体积：把站内相对路径还原成磁盘路径后 stat；拿不到就整行不显示。
   */
  async function sendUpdateEmail(user, version, changelog, downloadUrl) {
    if (!mailTransporter) return Promise.resolve();
    const b = MAIL_BRAND;
    const exeUrl = String(downloadUrl || '');
    const zipUrl = /\.exe(\?|$)/i.test(exeUrl) ? exeUrl.replace(/\.exe(\?|$)/i, '.7z$1') : '';
    const sizeText = packageSizeText(exeUrl);
    const buildDate = getSetting('build_date', '');

    // changelog 条目化：入库时一行一条、行首可能带「·」或「1.」，统一剥掉再渲染成列表。
    const items = String(changelog || '').split('\n')
      .map(l => l.trim().replace(/^[·•\-*]\s*/, '').replace(/^\d+[.、)]\s*/, ''))
      .filter(l => l && !/^本次更新/.test(l));

    // 更新内容 / 元信息都走模块级原语，预览脚本（tools/preview-mail.js）调的是同一份，
    // 不会再出现「预览好看、真发出去少条描边」的漂移。
    const changesHtml = mailBulletPanel(items);
    const meta = mailMetaRows([['版本', 'v' + version], ['发布', buildDate], ['体积', sizeText]]);

    const btns = mailButton(exeUrl, '下载安装版 (.exe)', 'solid')
      + (zipUrl ? mailButton(zipUrl, '免安装版 (.7z)', 'ghost') : '');

    const html = mailDocument({
      kicker: '版本更新',
      title: `v${version} 已发布`,
      subtitle: `${user.username}，您好 —— 建议更新到最新版本，以获得这一轮的改进与修复。`,
      body: meta + changesHtml
        + `<div style="margin:2px 0 0">${btns}</div>`
        + `<div style="margin-top:6px;font-size:12px;line-height:1.8;color:${b.ink2}">`
        + `.exe 为安装版；.7z 为免安装绿色包，解压即用，适合没有安装权限或想随身携带的场景。`
        + `两条链接任一可用即可。</div>`,
      footNote: '你收到这封邮件，是因为该邮箱绑定在 KunClient 账号上。',
    });

    return mailTransporter.sendMail({
      from: MAIL_FROM,
      to: user.email,
      subject: `【版本更新】KunClient v${version} 已发布`,
      html
    });
  }

  /**
   * 给「所有绑定了邮箱且未被封禁」的用户群发版本更新邮件。
   * 手动端点与自动发版通知共用这一份实现，避免两处逻辑漂移。
   * @returns {Promise<{sent:number, failed:number, total:number}>}
   */
  async function pushUpdateEmailToAll(version, changelog, downloadUrl) {
    const users = rowsToArr(db.exec("SELECT id, username, email FROM users WHERE email IS NOT NULL AND email != '' AND status != 'banned'"));
    if (users.length === 0) return { sent: 0, failed: 0, total: 0 };
    let sent = 0, failed = 0;
    for (const u of users) {
      try {
        await sendUpdateEmail(u, version, changelog, downloadUrl);
        sent++;
      } catch (e) {
        failed++;
        console.error(`[Email] 版本推送失败 ${u.email}:`, e.message);
      }
    }
    return { sent, failed, total: users.length };
  }

  /**
   * 发版即自动群发更新邮件。
   *
   * 「发版」的判定：app_settings.version 比 app_settings.last_notified_version 更高。
   * 这样无论版本号是被管理接口改的、还是被部署脚本直接改库的，重启后都能被识别到。
   *
   * 关键约束：
   *  - 同一版本只发一次（重启、改别的设置都不会重复发）；
   *  - **首次启用**（没有 last_notified_version）只对齐标记、不补发 —— 否则功能一上线
   *    就会对全量用户群发一次；
   *  - 版本号没升高（降级 / 误改）不发；
   *  - mail.enabled=false（mailTransporter 为 null）直接跳过，不发空邮件；
   *  - 无论成败都推进标记：宁可个别漏发，也不要每次重启给已收到的人重复发。
   *    失败明细写日志并落到 last_notify_result；需要补发时用 /api/admin/push-update 手动端点。
   */
  async function maybeNotifyNewVersion(reason) {
    if (!mailTransporter) return;
    const current = String(getSetting('version', '') || '').trim();
    if (!current) return;

    const notified = String(getSetting('last_notified_version', '') || '').trim();
    if (!notified) {
      setSetting('last_notified_version', current);
      console.log(`[版本通知] 首次启用，标记当前版本为 ${current}（不补发历史版本）`);
      return;
    }
    if (!isNewerVersion(current, notified)) return;

    const changelog = getSetting('changelog', '');
    const downloadUrl = resolveDownloadUrl();
    console.log(`[版本通知] 检测到发版 ${notified} → ${current}（触发点：${reason}），开始群发更新邮件…`);
    try {
      const r = await pushUpdateEmailToAll(current, changelog, downloadUrl);
      setSetting('last_notified_version', current);
      setSetting('last_notify_result',
        `${new Date().toISOString()} ${notified}->${current} 成功${r.sent}/失败${r.failed}/共${r.total}`);
      console.log(`[版本通知] 群发完成：成功 ${r.sent}，失败 ${r.failed}，共 ${r.total}`);
    } catch (e) {
      console.error('[版本通知] 群发异常:', e.message);
    }
  }

  // EDL Loader download API
  app.get('/api/v2/loaders/:type', (req, res) => {
    const loaderType = req.params.type;
    const allowed = ['firehose', 'sahara'];
    if (!allowed.includes(loaderType)) {
      return res.status(400).json({ success: false, message: 'Invalid loader type' });
    }
    
    const loaderDir = path.join(__dirname, 'assets', 'loaders');
    const loaderPath = path.join(loaderDir, `${loaderType}_loader.mbn`);
    
    if (!fs.existsSync(loaderPath)) {
      // Return empty base64 if no loader available yet
      return res.json({ 
        success: true, 
        data: '', 
        message: `No ${loaderType} loader available, please select local file`,
        size: 0 
      });
    }
    
    const data = fs.readFileSync(loaderPath);
    res.json({ success: true, data: data.toString('base64'), size: data.length });
  });

  // ══════════ EDL 云端引导库 —— 2026-10-20 合规下线 ══════════
  // 原实现把 65 个厂商 firehose loader（56 MB，高通 / 各品牌的专有二进制）放在
  // assets/loaders/firehose 下直出下载。兼容性反向工程本身可以做，**分发提取出来的
  // 二进制不行**（《计算机软件保护条例》第 17 条只豁免反向工程，不豁免复制与发行）。
  // 现在两个接口只回「已下线」的说明：客户端引导库选择器会展示这句提示，
  // 取 loader 的正道是从用户自己的官方固件包里提取（固件解析页已支持）。
  const FIREHOSE_OFFLINE_NOTE =
    '云端引导库已按合规要求下线：请从你手机对应版本的官方固件包中提取 loader ' +
    '（本工具的固件解析可直接识别），不要使用来源不明的引导文件。';

  app.get('/api/v2/loaders/firehose/index', (req, res) => {
    res.json({ success: true, data: [], count: 0, message: FIREHOSE_OFFLINE_NOTE });
  });

  app.get('/api/v2/loaders/firehose/file', (req, res) => {
    res.status(410).json({ success: false, message: FIREHOSE_OFFLINE_NOTE });
  });

  // scrcpy download API
  app.get('/api/v2/packages/scrcpy', (req, res) => {
    const scrcpyPath = path.join(__dirname, 'assets', 'scrcpy.zip');
    
    if (!fs.existsSync(scrcpyPath)) {
      return res.json({ 
        success: true, 
        data: '', 
        message: 'scrcpy not available on server',
        size: 0 
      });
    }
    
    // 2026-10-02：~11MB 的 scrcpy.zip 同 tools —— 按 mtime 缓存 base64，不再每请求重读重编。
    const b64 = cachedFileBase64(scrcpyPath);
    res.json({ success: true, data: b64, size: Math.floor(b64.length * 3 / 4) });
  });

  // E1：scrcpy 二进制端点（原始 zip，比 base64 JSON 少 33% 字节）
  app.get('/api/v2/packages/scrcpy/file', (req, res) => {
    const scrcpyPath = path.join(__dirname, 'assets', 'scrcpy.zip');
    if (!fs.existsSync(scrcpyPath)) {
      return res.status(404).json({ success: false, message: 'scrcpy not found' });
    }
    sendBinaryFile(req, res, scrcpyPath, 'scrcpy.zip');
  });

  // ====== 云端驱动（客户端「云端驱动」页） ======
  // 目录即清单：把驱动包丢进 server/assets/drivers/ 就会自动出现在客户端列表里，
  // 不需要改数据库、也不需要重启（每次请求都现读目录）。
  // 下载走已有的 `app.use('/assets', express.static(...))`，所以这里只返回相对 URL。
  // ⚠️ 目录不存在 / 为空是**正常的空状态**，不是错误 —— 客户端据此显示「暂无云端驱动」。
  // 2026-10-21：**云端背景功能整体下线**（用户口径「删除云端背景」）。
  // 原来这里有三条路由（列表 / 分片上传 / 删除）+ assets/wallpapers 目录，
  // 传输慢（跨境上行 ~1MB/s）、客户端每次启动还要重下，收益不抵成本，整块删掉。
  // 目录 assets/wallpapers 已清空，可以随时删除。

  const DRIVER_EXT = ['.zip', '.7z', '.exe', '.msi', '.inf'];
  app.get('/api/v2/packages/drivers', (req, res) => {
    const dir = path.join(__dirname, 'assets', 'drivers');
    let items = [];
    try {
      if (fs.existsSync(dir)) {
        items = fs.readdirSync(dir)
          .filter((f) => DRIVER_EXT.includes(path.extname(f).toLowerCase()))
          .map((f) => {
            const st = fs.statSync(path.join(dir, f));
            return { name: f, size: st.size, updatedAt: st.mtime.toISOString(), url: '/assets/drivers/' + encodeURIComponent(f) };
          })
          .sort((a, b) => a.name.localeCompare(b.name, 'zh'));
      }
    } catch (e) {
      return res.json({ success: false, message: '读取驱动目录失败：' + e.message, data: [] });
    }
    res.json({ success: true, data: items, count: items.length });
  });

  // v2 未匹配路径统一 404（必须挂所有业务路由之后）。v1 路径已被顶部重写进 v2，同样落到这里。
  app.use('/api', (req, res) => {
    console.warn(`[404] ${req.method} ${req.originalUrl}`);
    res.status(404).json({ success: false, code: 'NOT_FOUND', message: '接口不存在' });
  });

  // ⚠️ 必须显式绑回环地址（2026-09-26 修）。
  //
  // 原来写的是 `app.listen(PORT, cb)` —— 不传 host 时 Node 绑 **0.0.0.0（所有网卡）**，
  // 于是公网可以直接访问 `http://<公网IP>:3000/...`，**绕过 nginx 的全部安全规则
  // （敏感文件拦截、.bak/.log/.sql 404 规则）以及 Cloudflare 的速率限制与挑战**。
  // 实测 `http://your-server.example.com:3000/assets/KunClient.exe` 返回 200 —— 静态目录暴露无遗。
  //
  // nginx 反代走的是 127.0.0.1:3000（site conf 里 proxy_pass http://127.0.0.1:3000），
  // 所以绑回环不影响线上；本地开发用 http://localhost:3000 照常可用。
  app.listen(PORT, '127.0.0.1', () => {
    console.log(`[启动] KunClient Server 运行于 127.0.0.1:${PORT}（仅回环，仅经 nginx 对外）`);
    console.log(`[启动] 管理后台: http://localhost:${PORT}/admin  用户控制台: http://localhost:${PORT}/console`);
    console.log(`[启动] 邮件: ${cfg.mail.enabled ? cfg.mail.host + ':' + cfg.mail.port + '（' + (String(cfg.mail.authType || 'password').toLowerCase() === 'oauth2' ? 'OAuth2' : '基础认证') + '）' : '已禁用(mail.enabled=false)'}  反馈/通知邮箱: ${getAdminEmail()}`);
    // 发版即自动群发更新邮件。异步执行、不阻塞启动；函数内部已有 try/catch，
    // 这里再兜一层，确保任何异常都不会拖垮服务本身。
    maybeNotifyNewVersion('startup').catch((e) => console.error('[版本通知] 启动检查异常:', e.message));
  });
}

start().catch(err => { console.error('Failed to start server:', err); process.exit(1); });
